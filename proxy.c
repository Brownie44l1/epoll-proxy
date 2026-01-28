#include "proxy.h"
#include "connection.h"
#include "buffer.h"
#include "epoll.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

/* Global flag for graceful shutdown */
static volatile int running = 1;

/* Signal handler for SIGINT/SIGTERM */
static void signal_handler(int signum) {
    (void)signum;  /* Unused */
    running = 0;
}

/* ============================================================================
 * INITIALIZATION AND CLEANUP
 * ============================================================================
 */

int proxy_init(proxy_config_t *config,
               const char *listen_addr, uint16_t listen_port,
               const char *backend_addr, uint16_t backend_port) {
    
    /* Store configuration */
    config->listen_addr = listen_addr;
    config->listen_port = listen_port;
    config->backend_addr = backend_addr;
    config->backend_port = backend_port;
    
    /* Initialize connection pool */
    connection_pool_init(config);
    
    /* Create epoll instance */
    config->epoll_fd = epoll_init();
    if (config->epoll_fd == -1) {
        return -1;
    }
    
    /* Create listening socket */
    config->listen_fd = create_listen_socket(listen_addr, listen_port);
    if (config->listen_fd == -1) {
        close(config->epoll_fd);
        return -1;
    }
    
    /* Add listening socket to epoll.
     * We pass NULL for the connection pointer because the listening socket
     * is special - it's not a regular connection.
     * We'll check for this in the event loop.
     */
    if (epoll_add(config->epoll_fd, config->listen_fd, EPOLLIN, NULL) == -1) {
        close(config->listen_fd);
        close(config->epoll_fd);
        return -1;
    }
    
    printf("Proxy listening on %s:%d, forwarding to %s:%d\n",
           listen_addr, listen_port, backend_addr, backend_port);
    
    return 0;
}

void proxy_cleanup(proxy_config_t *config) {
    /* Close all active connections */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connection_t *conn = &config->connections[i];
        if (conn->state != CONN_CLOSED) {
            connection_close(config, conn);
        }
    }
    
    /* Close listening socket */
    if (config->listen_fd >= 0) {
        epoll_del(config->epoll_fd, config->listen_fd);
        close(config->listen_fd);
    }
    
    /* Close epoll instance */
    if (config->epoll_fd >= 0) {
        close(config->epoll_fd);
    }
    
    print_stats(config);
}

/* ============================================================================
 * MAIN EVENT LOOP
 * ============================================================================
 */

int proxy_run(proxy_config_t *config) {
    struct epoll_event events[MAX_EVENTS];
    
    /* Setup signal handlers for graceful shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("Proxy running (Ctrl-C to stop)...\n");
    
    while (running) {
        /* Wait for events.
         * Timeout of -1 means block indefinitely.
         * 
         * In production, you'd use a timeout (e.g., 1000ms) to periodically:
         *   - Check for idle connections to close
         *   - Update statistics
         *   - Check configuration changes
         */
        int nfds = epoll_wait_events(config->epoll_fd, events, MAX_EVENTS, -1);
        
        if (nfds == -1) {
            if (errno == EINTR) {
                /* Interrupted by signal - check running flag */
                continue;
            }
            perror("epoll_wait");
            return -1;
        }
        
        /* Process each ready file descriptor */
        for (int i = 0; i < nfds; i++) {
            struct epoll_event *ev = &events[i];
            
            /* Get connection pointer from event data.
             * Special case: listening socket has NULL connection.
             */
            connection_t *conn = (connection_t*)ev->data.ptr;
            
            /* Handle listening socket separately */
            if (conn == NULL) {
                /* This is the listening socket - new client connection */
                handle_accept(config);
                continue;
            }
            
            /* Check for error conditions first.
             * EPOLLERR, EPOLLHUP, EPOLLRDHUP indicate connection issues.
             */
            if (ev->events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                handle_error(config, conn);
                continue;
            }
            
            /* Handle backend connection completion.
             * When async connect() completes, socket becomes writable.
             */
            if (conn->state == CONN_CONNECTING && (ev->events & EPOLLOUT)) {
                handle_connect(config, conn);
                /* After connect completes, conn might have data to write */
                if (conn->state == CONN_CONNECTED && (ev->events & EPOLLOUT)) {
                    handle_write(config, conn);
                }
                continue;
            }
            
            /* Handle write events.
             * Process writes before reads to drain buffers faster.
             * This reduces memory usage and improves flow control.
             */
            if (ev->events & EPOLLOUT) {
                handle_write(config, conn);
            }
            
            /* Handle read events */
            if (ev->events & EPOLLIN) {
                handle_read(config, conn);
            }
        }
    }
    
    printf("\nShutting down...\n");
    return 0;
}

/* ============================================================================
 * EVENT HANDLERS
 * ============================================================================
 */

void handle_accept(proxy_config_t *config) {
    /* Edge-triggered epoll requires us to accept() in a loop until EAGAIN.
     * Multiple connections might be pending.
     */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        /* Accept new connection */
        int client_fd = accept(config->listen_fd, 
                              (struct sockaddr*)&client_addr, 
                              &client_len);
        
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* No more pending connections - this is normal */
                break;
            }
            /* Real error */
            perror("accept");
            break;
        }
        
        /* Set non-blocking mode.
         * Critical: must be done before any I/O operations.
         */
        if (set_nonblocking(client_fd) == -1) {
            close(client_fd);
            continue;
        }
        
        /* Set socket options */
        if (set_socket_options(client_fd) == -1) {
            close(client_fd);
            continue;
        }
        
        /* Allocate connection for client */
        connection_t *client = connection_alloc(config);
        if (client == NULL) {
            /* Connection pool exhausted - reject connection */
            fprintf(stderr, "Connection pool exhausted, rejecting client\n");
            close(client_fd);
            continue;
        }
        
        /* Initialize client connection */
        connection_init(client, client_fd, 1, CONN_CONNECTED);
        
        /* Create backend connection */
        int backend_fd = create_backend_connection(config->backend_addr,
                                                   config->backend_port);
        if (backend_fd == -1) {
            fprintf(stderr, "Failed to connect to backend\n");
            connection_close(config, client);
            continue;
        }
        
        /* Allocate connection for backend */
        connection_t *backend = connection_alloc(config);
        if (backend == NULL) {
            fprintf(stderr, "Connection pool exhausted, no backend connection\n");
            close(backend_fd);
            connection_close(config, client);
            continue;
        }
        
        /* Initialize backend connection.
         * State is CONN_CONNECTING because connect() returned EINPROGRESS.
         */
        connection_init(backend, backend_fd, 0, CONN_CONNECTING);
        
        /* Pair client and backend */
        connection_pair(client, backend);
        
        /* Add client to epoll (register for EPOLLIN - ready to read) */
        if (epoll_add(config->epoll_fd, client_fd, EPOLLIN, client) == -1) {
            connection_close_pair(config, client);
            continue;
        }
        
        /* Add backend to epoll (register for EPOLLOUT - wait for connect) */
        if (epoll_add(config->epoll_fd, backend_fd, EPOLLOUT, backend) == -1) {
            connection_close_pair(config, client);
            continue;
        }
        
        printf("Accepted connection from client (fd=%d), connecting to backend (fd=%d)\n",
               client_fd, backend_fd);
    }
}

void handle_read(proxy_config_t *config, connection_t *conn) {
    if (!connection_can_read(conn)) {
        return;
    }
    
    /* Edge-triggered epoll requires us to read until EAGAIN.
     * Partial reads are common with non-blocking I/O.
     */
    while (1) {
        ssize_t n = buffer_read_fd(&conn->read_buf, conn->fd);
        
        if (n > 0) {
            /* Successfully read n bytes */
            connection_update_activity(conn);
            config->stats.bytes_received += n;
            
            /* Forward data to peer.
             * We read into conn->read_buf, now copy to peer->write_buf.
             */
            if (forward_data(conn, conn->peer) == -1) {
                handle_error(config, conn);
                return;
            }
            
            /* Continue reading until EAGAIN */
            continue;
            
        } else if (n == 0) {
            /* EOF - peer closed connection gracefully */
            printf("Connection closed by %s (fd=%d)\n",
                   conn->is_client ? "client" : "backend", conn->fd);
            connection_close_pair(config, conn);
            return;
            
        } else {
            /* n == -1: error occurred */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Drained socket - this is expected with edge-triggered epoll */
                break;
            }
            
            /* Real error */
            if (errno != ECONNRESET) {
                /* ECONNRESET is common (client aborted), don't spam logs */
                perror("read");
            }
            config->stats.errors++;
            connection_close_pair(config, conn);
            return;
        }
    }
    
    /* Update epoll registration based on new buffer state */
    update_epoll_events(config, conn);
    if (conn->peer) {
        update_epoll_events(config, conn->peer);
    }
}

void handle_write(proxy_config_t *config, connection_t *conn) {
    if (!connection_can_write(conn)) {
        return;
    }
    
    /* Edge-triggered epoll requires us to write until EAGAIN.
     * Partial writes are common when socket buffer is small.
     */
    while (1) {
        ssize_t n = buffer_write_fd(&conn->write_buf, conn->fd);
        
        if (n > 0) {
            /* Successfully wrote n bytes */
            connection_update_activity(conn);
            config->stats.bytes_sent += n;
            
            /* If buffer is now empty, we might need to compact */
            if (buffer_is_empty(&conn->write_buf)) {
                /* Buffer drained - deregister from EPOLLOUT to avoid busy-wait */
                break;
            }
            
            /* Continue writing until EAGAIN or buffer empty */
            continue;
            
        } else if (n == 0) {
            /* write() returned 0 - unusual, treat as EAGAIN */
            break;
            
        } else {
            /* n == -1: error occurred */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Socket buffer full - stop writing, wait for next EPOLLOUT */
                break;
            }
            
            /* Real error */
            if (errno != EPIPE && errno != ECONNRESET) {
                /* EPIPE/ECONNRESET are common (peer closed), don't spam logs */
                perror("write");
            }
            config->stats.errors++;
            connection_close_pair(config, conn);
            return;
        }
    }
    
    /* Update epoll registration.
     * If write buffer is empty, we'll deregister from EPOLLOUT.
     * If peer's read buffer has space, we'll register peer for EPOLLIN.
     */
    update_epoll_events(config, conn);
    if (conn->peer) {
        update_epoll_events(config, conn->peer);
    }
}

void handle_connect(proxy_config_t *config, connection_t *conn) {
    /* Backend async connect completed (socket became writable).
     * Check if connection succeeded or failed.
     */
    int error = 0;
    socklen_t len = sizeof(error);
    
    /* SO_ERROR contains the connection result.
     * 0 = success, other = error code (ECONNREFUSED, ETIMEDOUT, etc.)
     */
    if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &error, &len) == -1) {
        perror("getsockopt SO_ERROR");
        connection_close_pair(config, conn);
        return;
    }
    
    if (error != 0) {
        /* Connection failed */
        errno = error;
        perror("backend connect");
        config->stats.errors++;
        connection_close_pair(config, conn);
        return;
    }
    
    /* Connection succeeded! */
    printf("Backend connection established (fd=%d)\n", conn->fd);
    connection_set_state(conn, CONN_CONNECTED);
    
    /* Update epoll registration.
     * We were registered for EPOLLOUT (waiting for connect).
     * Now we want EPOLLIN (ready to read from backend).
     */
    update_epoll_events(config, conn);
}

void handle_error(proxy_config_t *config, connection_t *conn) {
    /* Get error code if available */
    int error = 0;
    socklen_t len = sizeof(error);
    
    if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error != 0) {
        errno = error;
        if (errno != ECONNRESET && errno != EPIPE) {
            fprintf(stderr, "Connection error on fd=%d: %s\n", 
                   conn->fd, strerror(errno));
        }
    }
    
    config->stats.errors++;
    connection_close_pair(config, conn);
}

/* ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================
 */

ssize_t forward_data(connection_t *src, connection_t *dst) {
    if (src == NULL || dst == NULL) {
        return -1;
    }
    
    /* How much data do we have to forward? */
    size_t available = buffer_readable_bytes(&src->read_buf);
    if (available == 0) {
        return 0;
    }
    
    /* How much space does peer have? */
    size_t space = buffer_writable_bytes(&dst->write_buf);
    if (space == 0) {
        /* Peer's buffer full - apply backpressure.
         * We'll stop reading from src until peer drains its buffer.
         */
        return 0;
    }
    
    /* Forward as much as we can (limited by available data and space) */
    size_t to_copy = available < space ? available : space;
    
    /* Copy data from src's read buffer to dst's write buffer.
     * 
     * src->read_buf contains data read FROM src's socket.
     * dst->write_buf will be written TO dst's socket.
     * 
     * Note: We copy starting from read_buf.pos (where we left off reading)
     * and append to write_buf at write_buf.len (end of existing data).
     */
    memcpy(dst->write_buf.data + dst->write_buf.len,
           src->read_buf.data + src->read_buf.pos,
           to_copy);
    
    /* Update destination write buffer length */
    dst->write_buf.len += to_copy;
    
    /* Update source read buffer position */
    src->read_buf.pos += to_copy;
    
    /* If we've consumed all data from read buffer, reset it */
    if (src->read_buf.pos >= src->read_buf.len) {
        buffer_clear(&src->read_buf);
    }
    
    /* If dst's write buffer is getting fragmented, compact it */
    if (dst->write_buf.pos > 0 && buffer_writable_bytes(&dst->write_buf) < 1024) {
        buffer_compact(&dst->write_buf);
    }
    
    return to_copy;
}

int update_epoll_events(proxy_config_t *config, connection_t *conn) {
    if (!connection_is_valid(conn)) {
        return -1;
    }
    
    /* Determine what events we're interested in */
    uint32_t events = 0;
    
    if (connection_wants_read(conn)) {
        events |= EPOLLIN;
    }
    
    if (connection_wants_write(conn)) {
        events |= EPOLLOUT;
    }
    
    /* If we want no events, something is wrong (idle connection?).
     * But don't remove from epoll - just register for nothing.
     * This can happen briefly during state transitions.
     */
    if (events == 0) {
        /* Keep a minimal registration to detect errors */
        events = EPOLLIN;
    }
    
    /* Modify epoll registration.
     * We use EPOLL_CTL_MOD instead of DEL+ADD because the fd is already
     * registered (from accept or initial setup).
     */
    return epoll_mod(config->epoll_fd, conn->fd, events, conn);
}

void print_stats(const proxy_config_t *config) {
    printf("\n=== Proxy Statistics ===\n");
    printf("Total connections:  %lu\n", config->stats.total_connections);
    printf("Active connections: %lu\n", config->stats.active_connections);
    printf("Bytes received:     %lu\n", config->stats.bytes_received);
    printf("Bytes sent:         %lu\n", config->stats.bytes_sent);
    printf("Errors:             %lu\n", config->stats.errors);
    printf("========================\n");
}