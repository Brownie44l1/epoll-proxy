#ifndef PROXY_H
#define PROXY_H

#include "config.h"
#include <sys/epoll.h>

/* ============================================================================
 * PROXY CORE LOGIC
 * ============================================================================
 * This is where all the pieces come together:
 *   - Accept client connections
 *   - Create backend connections
 *   - Forward data bidirectionally
 *   - Handle all epoll events
 *   - Drive the event loop
 */

/* Initialize the proxy.
 * 
 * Parameters:
 *   config: proxy configuration (must be allocated by caller)
 *   listen_addr: IP address to listen on (e.g., "0.0.0.0")
 *   listen_port: port to listen on
 *   backend_addr: backend server IP
 *   backend_port: backend server port
 * 
 * Returns: 0 on success, -1 on error
 * 
 * This function:
 *   1. Initializes connection pool
 *   2. Creates epoll instance
 *   3. Creates and binds listening socket
 *   4. Adds listening socket to epoll
 * 
 * After this succeeds, proxy is ready to call proxy_run().
 */
int proxy_init(proxy_config_t *config,
               const char *listen_addr, uint16_t listen_port,
               const char *backend_addr, uint16_t backend_port);

/* Run the proxy event loop.
 * This is the main loop - it never returns (until interrupted).
 * 
 * The loop:
 *   while (1) {
 *       events = epoll_wait()
 *       for each event:
 *           handle_event()
 *   }
 * 
 * Returns: 0 on graceful shutdown, -1 on error
 */
int proxy_run(proxy_config_t *config);

/* Cleanup and shutdown the proxy.
 * Closes all connections, listening socket, and epoll instance.
 * 
 * Call this before process exit or when reloading configuration.
 */
void proxy_cleanup(proxy_config_t *config);

/* ============================================================================
 * EVENT HANDLERS
 * ============================================================================
 * These functions handle specific epoll events.
 * They're called from the main event loop.
 */

/* Handle event on listening socket (new client connection).
 * 
 * This:
 *   1. accept() the new connection
 *   2. Set it to non-blocking
 *   3. Allocate a connection struct
 *   4. Create backend connection
 *   5. Pair client and backend
 *   6. Add both to epoll
 * 
 * With edge-triggered epoll, we must accept() in a loop until EAGAIN.
 * Multiple connections might be pending in the listen queue.
 */
void handle_accept(proxy_config_t *config);

/* Handle read event on a connection.
 * 
 * This:
 *   1. Read data from socket into read_buf
 *   2. Copy data to peer's write_buf
 *   3. Update epoll registrations
 * 
 * With edge-triggered epoll, we must read() in a loop until EAGAIN.
 * 
 * Special cases:
 *   - read() returns 0: peer closed (EOF)
 *   - read() returns -1 with EAGAIN: drained socket (expected)
 *   - read() returns -1 with other error: connection failure
 */
void handle_read(proxy_config_t *config, connection_t *conn);

/* Handle write event on a connection.
 * 
 * This:
 *   1. Write data from write_buf to socket
 *   2. Update buffer position
 *   3. If buffer empty, deregister from EPOLLOUT
 *   4. If buffer still has data, keep EPOLLOUT registered
 * 
 * With edge-triggered epoll, we must write() in a loop until EAGAIN.
 * 
 * Special cases:
 *   - write() returns -1 with EAGAIN: socket buffer full (expected)
 *   - write() returns -1 with EPIPE/ECONNRESET: peer closed
 */
void handle_write(proxy_config_t *config, connection_t *conn);

/* Handle backend connection completion.
 * 
 * When we create_backend_connection(), connect() returns EINPROGRESS.
 * We add the socket to epoll with EPOLLOUT.
 * When the socket becomes writable, connection is complete.
 * 
 * This function:
 *   1. Checks SO_ERROR to see if connection succeeded
 *   2. If success: transition to CONN_CONNECTED
 *   3. If failure: close both client and backend
 */
void handle_connect(proxy_config_t *config, connection_t *conn);

/* Handle error/hangup events.
 * 
 * These events indicate connection failure:
 *   EPOLLERR: Error condition (check SO_ERROR)
 *   EPOLLHUP: Peer closed connection
 *   EPOLLRDHUP: Peer closed write side (half-close)
 * 
 * In all cases, we close both sides of the connection pair.
 */
void handle_error(proxy_config_t *config, connection_t *conn);

/* ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================
 */

/* Forward data from source connection to peer.
 * 
 * This is the heart of the proxy:
 *   1. Copy data from src->read_buf to dst->write_buf
 *   2. Clear src->read_buf
 *   3. Update epoll registrations
 * 
 * Returns: number of bytes forwarded, or -1 on error
 */
ssize_t forward_data(connection_t *src, connection_t *dst);

/* Update epoll registration based on connection state.
 * 
 * This determines what events we're interested in:
 *   - Want to read? Register EPOLLIN
 *   - Want to write? Register EPOLLOUT
 *   - Want both? Register EPOLLIN | EPOLLOUT
 *   - Want neither? This shouldn't happen (means idle connection)
 * 
 * We call this after every state change:
 *   - After reading (might need to write now)
 *   - After writing (might be done writing)
 *   - After forwarding data (buffers changed)
 */
int update_epoll_events(proxy_config_t *config, connection_t *conn);

/* Print connection statistics.
 * Useful for debugging and monitoring.
 * 
 * Shows:
 *   - Total connections accepted
 *   - Currently active connections
 *   - Bytes received/sent
 *   - Error count
 */
void print_stats(const proxy_config_t *config);

#endif /* PROXY_H */