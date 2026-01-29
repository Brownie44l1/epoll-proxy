#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * CONFIGURATION CONSTANTS
 * ============================================================================
 */

/* Maximum number of simultaneous connections */
#define MAX_CONNECTIONS 10000  /* Increased from 1024 for high concurrency */

/* Maximum events per epoll_wait() */
#define MAX_EVENTS 256  /* Increased from 128 for better batching */

/* Buffer size - increased for HTTP */
#define BUFFER_SIZE 16384  /* 16KB - holds most HTTP requests + small body */

/* Listen backlog */
#define LISTEN_BACKLOG 511  /* Increased from 128 for high concurrency */

/* Connection timeout */
#define CONNECT_TIMEOUT 5

/* HTTP-specific limits */
#define MAX_REQUEST_SIZE (10 * 1024 * 1024)  /* 10MB max request */
#define IDLE_TIMEOUT 60  /* Close idle connections after 60s */
#define MAX_REQUESTS_PER_CONN 1000  /* Limit keep-alive reuse */

/* ============================================================================
 * CONNECTION STATE MACHINE
 * ============================================================================
 */
typedef enum {
    CONN_CONNECTING,
    CONN_CONNECTED,
    CONN_READING_REQUEST,   /* NEW: Reading HTTP request */
    CONN_REQUEST_COMPLETE,  /* NEW: Have complete HTTP request */
    CONN_WRITING_RESPONSE,  /* NEW: Writing HTTP response */
    CONN_CLOSING,
    CONN_CLOSED
} conn_state_t;

/* ============================================================================
 * BUFFER STRUCTURE
 * ============================================================================
 */
typedef struct {
    char data[BUFFER_SIZE];
    size_t len;
    size_t pos;
} buffer_t;

/* Forward declare http_request_t */
struct http_request;

/* ============================================================================
 * CONNECTION STRUCTURE
 * ============================================================================
 */
typedef struct connection {
    int fd;
    struct connection *peer;
    conn_state_t state;
    buffer_t read_buf;
    buffer_t write_buf;
    int is_client;
    uint64_t last_active;
    
    /* HTTP-specific fields */
    struct http_request *http_req;  /* Parsed HTTP request (client connections only) */
    int requests_handled;           /* Number of requests on this connection */
    int keep_alive;                 /* Should we keep connection open? */
} connection_t;

/* ============================================================================
 * PROXY MODE
 * ============================================================================
 */
typedef enum {
    PROXY_MODE_TCP,   /* Original TCP proxy mode */
    PROXY_MODE_HTTP   /* NEW: HTTP-aware proxy mode */
} proxy_mode_t;

/* ============================================================================
 * PROXY CONFIGURATION
 * ============================================================================
 */
typedef struct {
    /* Network config */
    const char *listen_addr;
    uint16_t listen_port;
    const char *backend_addr;
    uint16_t backend_port;
    
    /* Operating mode */
    proxy_mode_t mode;  /* NEW: TCP or HTTP mode */
    
    /* File descriptors */
    int epoll_fd;
    int listen_fd;
    
    /* Connection pool */
    connection_t connections[MAX_CONNECTIONS];
    int free_list[MAX_CONNECTIONS];
    int free_count;
    
    /* Statistics */
    struct {
        uint64_t total_connections;
        uint64_t active_connections;
        uint64_t bytes_received;
        uint64_t bytes_sent;
        uint64_t errors;
        
        /* HTTP-specific stats */
        uint64_t requests_total;
        uint64_t requests_get;
        uint64_t requests_post;
        uint64_t requests_error;  /* Malformed requests */
        uint64_t keep_alive_reused;
    } stats;
} proxy_config_t;

/* ============================================================================
 * HELPER MACROS
 * ============================================================================
 */

#define FD_TO_CONN(config, fd) (&(config)->connections[fd])
#define CONN_IS_VALID(conn) ((conn) != NULL && (conn)->state != CONN_CLOSED)
#define BUFFER_HAS_DATA(buf) ((buf)->len > (buf)->pos)
#define BUFFER_REMAINING(buf) ((buf)->len - (buf)->pos)
#define BUFFER_RESET(buf) do { (buf)->len = 0; (buf)->pos = 0; } while(0)

#endif /* CONFIG_H */