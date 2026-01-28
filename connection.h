#ifndef CONNECTION_H
#define CONNECTION_H

#include "config.h"

/* ============================================================================
 * CONNECTION MANAGEMENT
 * ============================================================================
 * These functions manage the connection lifecycle:
 *   1. Allocation from free list
 *   2. Initialization
 *   3. State transitions
 *   4. Pairing (client <-> backend)
 *   5. Cleanup and recycling
 * 
 * The connection state machine is the heart of the proxy logic.
 */

/* Initialize the connection pool and free list.
 * Call this once at startup before accepting any connections.
 * 
 * Sets up:
 *   - All connections in CONN_CLOSED state
 *   - Free list populated with all indices (0 to MAX_CONNECTIONS-1)
 *   - Buffers initialized
 */
void connection_pool_init(proxy_config_t *config);

/* Allocate a connection from the free list.
 * Returns: pointer to connection, or NULL if pool exhausted
 * 
 * This pops from the free_list stack. O(1) operation.
 * The returned connection is in CONN_CLOSED state - caller must initialize it.
 * 
 * If this returns NULL, we've hit MAX_CONNECTIONS limit.
 * Options: close old connections, increase limit, or reject new clients.
 */
connection_t* connection_alloc(proxy_config_t *config);

/* Return a connection to the free list.
 * The connection is reset to CONN_CLOSED state and can be reused.
 * 
 * This pushes back onto the free_list stack. O(1) operation.
 * 
 * Important: Caller must close the fd and remove from epoll BEFORE calling this.
 */
void connection_free(proxy_config_t *config, connection_t *conn);

/* Initialize a connection after allocation.
 * 
 * Parameters:
 *   conn: connection to initialize (from connection_alloc)
 *   fd: socket file descriptor
 *   is_client: 1 if client-facing, 0 if backend
 *   state: initial state (usually CONN_CONNECTED or CONN_CONNECTING)
 * 
 * This:
 *   - Sets fd, is_client, state
 *   - Clears buffers
 *   - Nulls peer pointer
 *   - Records timestamp
 */
void connection_init(connection_t *conn, int fd, int is_client, 
                    conn_state_t state);

/* Pair two connections (client <-> backend).
 * Each connection's peer pointer points to the other.
 * 
 * This creates the forwarding relationship:
 *   client->peer = backend
 *   backend->peer = client
 * 
 * Now when we read from client, we know to write to client->peer (backend).
 * And vice versa.
 * 
 * Important: Both connections must be allocated before calling this.
 */
void connection_pair(connection_t *client, connection_t *backend);

/* Unpair connections.
 * Sets both peer pointers to NULL.
 * 
 * Call this during cleanup when one side of a connection pair fails.
 * This prevents use-after-free if we try to forward data to a freed peer.
 */
void connection_unpair(connection_t *conn);

/* Close and cleanup a connection.
 * 
 * This:
 *   1. Removes fd from epoll
 *   2. Closes the socket
 *   3. Unpairs from peer (if paired)
 *   4. Returns connection to free list
 * 
 * If peer exists, caller should decide whether to close peer too.
 * Common pattern:
 *   connection_close(config, client);
 *   if (client->peer) connection_close(config, client->peer);
 * 
 * This is THE cleanup function - call it whenever a connection needs to end.
 */
void connection_close(proxy_config_t *config, connection_t *conn);

/* Close a connection pair (both client and backend).
 * Convenience function that closes both sides of a paired connection.
 * 
 * This is what you call when either side fails:
 *   - Client disconnects → close both
 *   - Backend connection fails → close both
 *   - Read/write error → close both
 */
void connection_close_pair(proxy_config_t *config, connection_t *conn);

/* Update connection state.
 * This is a simple setter, but it's a function so we can add logging,
 * metrics, or state validation later.
 * 
 * Example state transitions:
 *   CONN_CONNECTING → CONN_CONNECTED (backend connect completes)
 *   CONN_CONNECTED → CONN_READING (data available)
 *   CONN_READING → CONN_WRITING (forwarding data)
 *   CONN_WRITING → CONN_CONNECTED (write complete)
 *   CONN_CONNECTED → CONN_CLOSING (graceful shutdown)
 *   CONN_CLOSING → CONN_CLOSED (cleanup complete)
 */
void connection_set_state(connection_t *conn, conn_state_t state);

/* Check if connection is still valid and active.
 * Returns 1 if connection can be used, 0 otherwise.
 * 
 * A valid connection:
 *   - Is not NULL
 *   - State is not CONN_CLOSED
 *   - fd is valid (>= 0)
 */
int connection_is_valid(const connection_t *conn);

/* Update last activity timestamp.
 * Call this whenever we successfully read/write on the connection.
 * Used for idle timeout detection.
 * 
 * In a production system, you'd have a timer that periodically scans
 * connections and closes those where (now - last_active) > timeout.
 */
void connection_update_activity(connection_t *conn);

/* Get current timestamp in milliseconds.
 * Helper function for timeout tracking.
 * Uses CLOCK_MONOTONIC to avoid issues with system time changes.
 */
uint64_t get_timestamp_ms(void);

/* ============================================================================
 * STATE MACHINE HELPERS
 * ============================================================================
 * These help reason about what operations are valid in each state.
 */

/* Can we read from this connection in its current state?
 * Returns 1 if reading is appropriate, 0 otherwise.
 * 
 * Reading is OK when:
 *   - State is CONN_CONNECTED, CONN_READING
 *   - Peer exists and has buffer space
 */
int connection_can_read(const connection_t *conn);

/* Can we write to this connection in its current state?
 * Returns 1 if writing is appropriate, 0 otherwise.
 * 
 * Writing is OK when:
 *   - State is CONN_CONNECTED, CONN_WRITING
 *   - Write buffer has data
 */
int connection_can_write(const connection_t *conn);

/* Should we register for EPOLLIN events?
 * Returns 1 if we want to be notified of incoming data.
 * 
 * Register for EPOLLIN when:
 *   - Connection is connected
 *   - Peer's write buffer has space (backpressure control)
 */
int connection_wants_read(const connection_t *conn);

/* Should we register for EPOLLOUT events?
 * Returns 1 if we want to be notified when socket is writable.
 * 
 * Register for EPOLLOUT when:
 *   - We have data in write buffer
 *   - We're waiting for async connect to complete
 */
int connection_wants_write(const connection_t *conn);

#endif /* CONNECTION_H */