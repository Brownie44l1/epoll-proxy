#ifndef EPOLL_H
#define EPOLL_H

#include "config.h"
#include <sys/epoll.h>

/* ============================================================================
 * EPOLL WRAPPER FUNCTIONS
 * ============================================================================
 * These functions wrap the low-level epoll syscalls with error handling,
 * logging, and our specific configuration choices.
 * 
 * Design philosophy: Hide epoll's complexity behind a clean interface.
 * The rest of the codebase shouldn't need to know about EPOLL_CTL_ADD vs
 * EPOLL_CTL_MOD or the exact event flags.
 */

/* Create and configure the epoll instance.
 * Returns: epoll file descriptor on success, -1 on error.
 * 
 * This is called once at startup. The epoll fd is stored in proxy_config
 * and used for the lifetime of the process.
 * 
 * We use epoll_create1(EPOLL_CLOEXEC) to ensure the fd is closed on exec(),
 * which is good practice for any fd that shouldn't leak to child processes.
 */
int epoll_init(void);

/* Add a file descriptor to the epoll interest list.
 * 
 * Parameters:
 *   epoll_fd: the epoll instance
 *   fd: file descriptor to monitor
 *   events: event mask (EPOLLIN, EPOLLOUT, etc.)
 *   conn: pointer to connection struct (stored in event.data.ptr)
 * 
 * Returns: 0 on success, -1 on error
 * 
 * We use EPOLLET (edge-triggered) by default. This is critical for performance
 * but requires careful handling - you MUST read/write until EAGAIN.
 */
int epoll_add(int epoll_fd, int fd, uint32_t events, void *conn);

/* Modify events for an already-registered file descriptor.
 * 
 * Use this to switch between EPOLLIN and EPOLLOUT as needed:
 *   - Have data to send? Add EPOLLOUT
 *   - Done sending? Remove EPOLLOUT (to avoid busy-wait)
 *   - Want to read? Add EPOLLIN
 * 
 * Returns: 0 on success, -1 on error
 */
int epoll_mod(int epoll_fd, int fd, uint32_t events, void *conn);

/* Remove a file descriptor from epoll interest list.
 * 
 * Call this before close()ing a socket. While Linux automatically removes
 * fds from epoll on close(), explicitly removing is clearer and portable.
 * 
 * Returns: 0 on success, -1 on error (which we can ignore)
 */
int epoll_del(int epoll_fd, int fd);

/* Wait for events on registered file descriptors.
 * 
 * Parameters:
 *   epoll_fd: the epoll instance
 *   events: array to store returned events
 *   max_events: size of events array
 *   timeout_ms: timeout in milliseconds (-1 = block forever)
 * 
 * Returns: number of ready file descriptors, -1 on error
 * 
 * This is the heart of the event loop. We'll call this in a loop:
 *   while (1) {
 *       int n = epoll_wait_events(...);
 *       for (int i = 0; i < n; i++) {
 *           handle_event(&events[i]);
 *       }
 *   }
 */
int epoll_wait_events(int epoll_fd, struct epoll_event *events, 
                      int max_events, int timeout_ms);

/* ============================================================================
 * SOCKET UTILITIES
 * ============================================================================
 * These aren't strictly epoll functions, but they're closely related.
 * Non-blocking mode is required for edge-triggered epoll to work correctly.
 */

/* Set a socket to non-blocking mode.
 * 
 * Why non-blocking?
 *   With blocking sockets, read() would hang waiting for data, freezing
 *   the entire event loop. Non-blocking returns immediately with EAGAIN.
 * 
 * We set this on:
 *   - The listening socket (accept() should never block)
 *   - Every accepted client socket
 *   - Every backend connection socket
 * 
 * Returns: 0 on success, -1 on error
 */
int set_nonblocking(int fd);

/* Set common socket options for optimal performance.
 * 
 * We set:
 *   - SO_REUSEADDR: Allow binding to recently-used addresses (important for
 *     quick restarts during development)
 *   - SO_KEEPALIVE: Send TCP keepalive packets to detect dead connections
 *   - TCP_NODELAY: Disable Nagle's algorithm for low latency
 * 
 * TCP_NODELAY tradeoff:
 *   Pro: Lower latency (no 200ms delay waiting to batch small packets)
 *   Con: More packets on the wire (less efficient for bulk data)
 *   For a proxy forwarding HTTP requests, low latency wins.
 * 
 * Returns: 0 on success, -1 on error
 */
int set_socket_options(int fd);

/* Create and bind a listening socket.
 * 
 * Parameters:
 *   addr: IP address to bind to (e.g., "0.0.0.0" for all interfaces)
 *   port: port number
 * 
 * Returns: socket fd on success, -1 on error
 * 
 * This creates a TCP socket, binds it, and calls listen().
 * The socket is set to non-blocking and SO_REUSEADDR is enabled.
 */
int create_listen_socket(const char *addr, uint16_t port);

/* Create a non-blocking connection to backend.
 * 
 * Parameters:
 *   addr: backend IP address
 *   port: backend port
 * 
 * Returns: socket fd on success (may be in EINPROGRESS state), -1 on error
 * 
 * With non-blocking sockets, connect() returns immediately with EINPROGRESS.
 * We add the socket to epoll with EPOLLOUT, and when it becomes writable,
 * the connection is complete. We then check SO_ERROR to see if it succeeded.
 * 
 * This is how we handle async connects without blocking the event loop.
 */
int create_backend_connection(const char *addr, uint16_t port);

#endif /* EPOLL_H */