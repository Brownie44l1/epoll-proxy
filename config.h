#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * CONFIGURATION CONSTANTS
 * ============================================================================
 * These are tuned for a balance between memory usage and performance.
 * You'll benchmark and adjust these later.
 */

/* Maximum number of simultaneous connections the proxy can handle.
 * This limits memory usage: ~16KB per connection = ~16MB for 1000 connections.
 * With epoll, the cost is primarily memory, not CPU.
 */
#define MAX_CONNECTIONS 1024

/* Maximum number of events to retrieve in a single epoll_wait() call.
 * Higher = fewer syscalls but more latency per batch.
 * Lower = more syscalls but lower latency.
 * 128 is a good sweet spot for most workloads.
 */
#define MAX_EVENTS 128

/* Buffer size for read/write operations (8KB = typical TCP window size).
 * Why 8KB? It's a common TCP receive window size, reducing the need for
 * multiple read() calls for typical requests.
 * Too small = many syscalls. Too large = wasted memory.
 */
#define BUFFER_SIZE 8192

/* Listen backlog - how many pending connections can wait in kernel queue.
 * This is the SYN queue depth. Under heavy load, you want this higher.
 * 128 is conservative; production systems often use 511 or 1024.
 */
#define LISTEN_BACKLOG 128

/* Connection timeout in seconds.
 * If a backend connection doesn't complete within this time, close it.
 * Prevents resource exhaustion from slow/dead backends.
 */
#define CONNECT_TIMEOUT 5

/* ============================================================================
 * CONNECTION STATE MACHINE
 * ============================================================================
 * Every connection transitions through these states.
 * Understanding these states is critical to the proxy logic.
 */
typedef enum {
    /* Initial state after accept() or before connect() completes.
     * Backend connections start here during async connect.
     */
    CONN_CONNECTING,
    
    /* Connection established and ready for I/O.
     * Most time is spent in this state.
     */
    CONN_CONNECTED,
    
    /* We have data to read from this fd.
     * Registered for EPOLLIN events.
     */
    CONN_READING,
    
    /* We have data to write to this fd.
     * Registered for EPOLLOUT events.
     */
    CONN_WRITING,
    
    /* Graceful shutdown in progress.
     * Waiting to drain buffers before close().
     */
    CONN_CLOSING,
    
    /* Connection closed, resources freed.
     * This is a tombstone state before struct reuse.
     */
    CONN_CLOSED
} conn_state_t;

/* ============================================================================
 * BUFFER STRUCTURE
 * ============================================================================
 * A ring buffer would be more efficient, but a simple linear buffer is easier
 * to reason about and debug. Since we're edge-triggered, we drain buffers
 * completely anyway, so ring buffer benefits are minimal.
 */
typedef struct {
    char data[BUFFER_SIZE];  /* Actual buffer memory */
    size_t len;              /* Number of bytes currently in buffer */
    size_t pos;              /* Read position (for partial writes) */
} buffer_t;

/* ============================================================================
 * CONNECTION STRUCTURE
 * ============================================================================
 * This is the heart of the proxy. Each socket (client or backend) gets one.
 * The peer pointer creates the client<->backend relationship.
 */
typedef struct connection {
    /* File descriptor for this socket */
    int fd;
    
    /* Pointer to the paired connection (client->backend or backend->client).
     * This is how we know where to forward data.
     * NULL means unpaired (shouldn't happen in steady state).
     */
    struct connection *peer;
    
    /* Current state in the connection lifecycle */
    conn_state_t state;
    
    /* Read buffer: data read FROM this socket (to be written to peer) */
    buffer_t read_buf;
    
    /* Write buffer: data to be written TO this socket (read from peer) */
    buffer_t write_buf;
    
    /* Is this a client-facing connection or backend connection?
     * Needed for logging and some error handling decisions.
     */
    int is_client;
    
    /* Timestamp of last activity (for timeout detection).
     * We'll use this to close idle connections.
     */
    uint64_t last_active;
} connection_t;

/* ============================================================================
 * PROXY CONFIGURATION
 * ============================================================================
 * Runtime configuration loaded from command-line args or config file.
 */
typedef struct {
    /* Address and port the proxy listens on for client connections */
    const char *listen_addr;
    uint16_t listen_port;
    
    /* Backend server address and port to forward requests to */
    const char *backend_addr;
    uint16_t backend_port;
    
    /* Epoll file descriptor (created once at startup) */
    int epoll_fd;
    
    /* Listening socket file descriptor */
    int listen_fd;
    
    /* Connection pool - preallocated array of connection structs.
     * We use an array instead of malloc for each connection because:
     * 1. Predictable memory layout (cache-friendly)
     * 2. No malloc/free in hot path
     * 3. Easy to iterate for debugging/stats
     */
    connection_t connections[MAX_CONNECTIONS];
    
    /* Free list of connection indices.
     * Instead of searching for free connections, we maintain a stack.
     * free_count tells us how many are available.
     */
    int free_list[MAX_CONNECTIONS];
    int free_count;
    
    /* Statistics for monitoring */
    struct {
        uint64_t total_connections;
        uint64_t active_connections;
        uint64_t bytes_received;
        uint64_t bytes_sent;
        uint64_t errors;
    } stats;
} proxy_config_t;

/* ============================================================================
 * HELPER MACROS
 * ============================================================================
 */

/* Get connection pointer from file descriptor.
 * We store the array index in epoll_event.data.fd, so we can quickly
 * map fd -> connection without a hash table or linear search.
 */
#define FD_TO_CONN(config, fd) (&(config)->connections[fd])

/* Check if a connection is valid and active */
#define CONN_IS_VALID(conn) ((conn) != NULL && (conn)->state != CONN_CLOSED)

/* Check if buffer has data to write */
#define BUFFER_HAS_DATA(buf) ((buf)->len > (buf)->pos)

/* Remaining data in buffer */
#define BUFFER_REMAINING(buf) ((buf)->len - (buf)->pos)

/* Reset buffer to empty state */
#define BUFFER_RESET(buf) do { (buf)->len = 0; (buf)->pos = 0; } while(0)

#endif /* CONFIG_H */