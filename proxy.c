#include "proxy.h"
#include "connection.h"
#include "buffer.h"
#include "epoll.h"
#include "http_request.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>

/* Global flag for graceful shutdown */
static volatile int running = 1;

/* Forward declarations */
static void handle_read_tcp(proxy_config_t *config, connection_t *conn);
static void handle_read_http_client(proxy_config_t *config, connection_t *client);

/* Signal handler */
static void signal_handler(int signum) {
    (void)signum;
    running = 0;
}

/* ============================================================================
 * INITIALIZATION - HTTP MODE
 * ============================================================================
 */

int proxy_init_http(proxy_config_t *config,
                    const char *listen_addr, uint16_t listen_port,
                    const char *backend_addr, uint16_t backend_port) {
    
    /* Store configuration */
    config->listen_addr = listen_addr;
    config->listen_port = listen_port;
    config->backend_addr = backend_addr;
    config->backend_port = backend_port;
    config->mode = PROXY_MODE_HTTP;  /* HTTP mode */
    
    /* Initialize connection pool */
    connection_pool_init(config);
    
    /* Create epoll instance */
    config->epoll_fd = epoll_init();
    if (config->epoll_fd == -1) {
        return -1;
    }
    
    /* Create listening socket with optimizations */
    config->listen_fd = create_listen_socket(listen_addr, listen_port);
    if (config->listen_fd == -1) {
        close(config->epoll_fd);
        return -1;
    }
    
    /* Set socket options for performance */
    int on = 1;
    
    /* SO_REUSEADDR - allow rapid restart */
    if (setsockopt(config->listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        perror("setsockopt SO_REUSEADDR");
    }
    
    /* SO_REUSEPORT - allow multiple processes to bind (for scale-out) */
    /* Note: Not available on all systems, so we check if it's defined */
#ifdef SO_REUSEPORT
    if (setsockopt(config->listen_fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) < 0) {
        perror("setsockopt SO_REUSEPORT");
    }
#endif
    
    /* TCP_DEFER_ACCEPT - only wake up when data arrives (reduces wakeups) */
#ifdef TCP_DEFER_ACCEPT
    int timeout = 1;  /* 1 second */
    if (setsockopt(config->listen_fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, 
                   &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt TCP_DEFER_ACCEPT");
    }
#endif
    
    /* Add listening socket to epoll */
    if (epoll_add(config->epoll_fd, config->listen_fd, EPOLLIN, NULL) == -1) {
        close(config->listen_fd);
        close(config->epoll_fd);
        return -1;
    }
    
    printf("HTTP Proxy listening on %s:%d, forwarding to %s:%d\n",
           listen_addr, listen_port, backend_addr, backend_port);
    
    return 0;
}

/* TCP mode initialization (original) */
int proxy_init(proxy_config_t *config,
               const char *listen_addr, uint16_t listen_port,
               const char *backend_addr, uint16_t backend_port) {
    
    config->mode = PROXY_MODE_TCP;  /* TCP mode */
    
    /* Same as HTTP init but without HTTP-specific opts */
    return proxy_init_http(config, listen_addr, listen_port, 
                          backend_addr, backend_port);
}

void proxy_cleanup(proxy_config_t *config) {
    /* Close all active connections */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connection_t *conn = &config->connections[i];
        if (conn->state != CONN_CLOSED) {
            /* Free HTTP request if allocated */
            if (conn->http_req) {
                free(conn->http_req);
                conn->http_req = NULL;
            }
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
    
    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);  /* Ignore SIGPIPE - we handle EPIPE in write */
    
    const char *mode = (config->mode == PROXY_MODE_HTTP) ? "HTTP" : "TCP";
    printf("%s Proxy running (Ctrl-C to stop)...\n", mode);
    
    while (running) {
        /* Wait for events with timeout for periodic tasks */
        int nfds = epoll_wait_events(config->epoll_fd, events, MAX_EVENTS, 1000);
        
        if (nfds == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            return -1;
        }
        
        /* Process each ready file descriptor */
        for (int i = 0; i < nfds; i++) {
            struct epoll_event *ev = &events[i];
            connection_t *conn = (connection_t*)ev->data.ptr;
            
            /* Handle listening socket */
            if (conn == NULL) {
                handle_accept(config);
                continue;
            }
            
            /* Handle error conditions */
            if (ev->events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                handle_error(config, conn);
                continue;
            }
            
            /* Handle backend connection completion */
            if (conn->state == CONN_CONNECTING && (ev->events & EPOLLOUT)) {
                handle_connect(config, conn);
                if (conn->state == CONN_CONNECTED && (ev->events & EPOLLOUT)) {
                    handle_write(config, conn);
                }
                continue;
            }
            
            /* Handle write events (process before reads for flow control) */
            if (ev->events & EPOLLOUT) {
                handle_write(config, conn);
            }
            
            /* Handle read events */
            if (ev->events & EPOLLIN) {
                handle_read(config, conn);
            }
        }
        
        /* Periodic tasks every second */
        static uint64_t last_maintenance = 0;
        uint64_t now = get_timestamp_ms();
        if (now - last_maintenance > 1000) {
            last_maintenance = now;
            
            /* TODO: Close idle connections
             * for each connection:
             *   if (now - conn->last_active > IDLE_TIMEOUT * 1000)
             *       connection_close(conn);
             */
        }
    }
    
    printf("\nShutting down...\n");
    return 0;
}

/* ============================================================================
 * ACCEPT HANDLER
 * ============================================================================
 */

void handle_accept(proxy_config_t *config) {
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(config->listen_fd, 
                              (struct sockaddr*)&client_addr, 
                              &client_len);
        
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            perror("accept");
            break;
        }
        
        /* Set non-blocking */
        if (set_nonblocking(client_fd) == -1) {
            close(client_fd);
            continue;
        }
        
        /* Set socket options */
        if (set_socket_options(client_fd) == -1) {
            close(client_fd);
            continue;
        }
        
        /* Allocate connection */
        connection_t *client = connection_alloc(config);
        if (client == NULL) {
            fprintf(stderr, "Connection pool exhausted\n");
            close(client_fd);
            continue;
        }
        
        /* Initialize client connection */
        connection_init(client, client_fd, 1, CONN_CONNECTED);
        
        /* In HTTP mode, allocate HTTP request structure */
        if (config->mode == PROXY_MODE_HTTP) {
            client->http_req = calloc(1, sizeof(http_request_t));
            if (client->http_req == NULL) {
                fprintf(stderr, "Failed to allocate HTTP request\n");
                connection_close(config, client);
                close(client_fd);
                continue;
            }
            http_request_init((http_request_t*)client->http_req);
            client->state = CONN_READING_REQUEST;
        }
        
        /* Add to epoll */
        if (epoll_add(config->epoll_fd, client_fd, EPOLLIN, client) == -1) {
            if (client->http_req) free(client->http_req);
            connection_close(config, client);
            continue;
        }
    }
}

/* ============================================================================
 * READ HANDLER - HTTP AWARE
 * ============================================================================
 */

void handle_read(proxy_config_t *config, connection_t *conn) {
    if (!connection_is_valid(conn)) {
        return;
    }
    
    /* HTTP mode: Handle client reads specially */
    if (config->mode == PROXY_MODE_HTTP && conn->is_client) {
        handle_read_http_client(config, conn);
        return;
    }
    
    /* TCP mode or backend reads: Original behavior */
    handle_read_tcp(config, conn);
}

/* TCP read handler (original logic) */
static void handle_read_tcp(proxy_config_t *config, connection_t *conn) {
    if (!connection_can_read(conn)) {
        return;
    }
    
    while (1) {
        ssize_t n = buffer_read_fd(&conn->read_buf, conn->fd);
        
        if (n > 0) {
            connection_update_activity(conn);
            config->stats.bytes_received += n;
            
            if (forward_data(conn, conn->peer) == -1) {
                handle_error(config, conn);
                return;
            }
            continue;
        } else if (n == 0) {
            /* EOF */
            connection_close_pair(config, conn);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno != ECONNRESET) {
                perror("read");
            }
            config->stats.errors++;
            connection_close_pair(config, conn);
            return;
        }
    }
    
    update_epoll_events(config, conn);
    if (conn->peer) {
        update_epoll_events(config, conn->peer);
    }
}

/* HTTP client read handler */
static void handle_read_http_client(proxy_config_t *config, connection_t *client) {
    /* Read data into buffer */
    while (1) {
        ssize_t n = buffer_read_fd(&client->read_buf, client->fd);
        
        if (n > 0) {
            connection_update_activity(client);
            config->stats.bytes_received += n;
            
            /* Try to parse HTTP request */
            int parse_result = http_request_parse(
                (http_request_t*)client->http_req,
                client->read_buf.data,
                client->read_buf.len
            );
            
            if (parse_result == 1) {
                /* Request complete! */
                client->state = CONN_REQUEST_COMPLETE;
                
                /* Validate request */
                if (!http_request_is_valid((const http_request_t*)client->http_req)) {
                    config->stats.requests_error++;
                    send_http_error(client, 400, "Bad Request");
                    return;
                }
                
                /* Update stats */
                config->stats.requests_total++;
                http_request_t *req = (http_request_t*)client->http_req;
                if (req->method == HTTP_METHOD_GET) {
                    config->stats.requests_get++;
                } else if (req->method == HTTP_METHOD_POST) {
                    config->stats.requests_post++;
                }
                
                /* Handle the request */
                handle_http_request(config, client);
                return;
                
            } else if (parse_result == -1) {
                /* Parse error */
                config->stats.requests_error++;
                send_http_error(client, 400, "Malformed Request");
                return;
            }
            
            /* Need more data - continue reading */
            continue;
            
        } else if (n == 0) {
            /* Client closed connection */
            connection_close(config, client);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Drained socket - wait for more data */
                break;
            }
            /* Read error */
            config->stats.errors++;
            connection_close(config, client);
            return;
        }
    }
    
    /* Check if request is getting too large */
    if (client->read_buf.len > MAX_REQUEST_SIZE) {
        config->stats.requests_error++;
        send_http_error(client, 413, "Request Too Large");
        return;
    }
}

/* ============================================================================
 * WRITE HANDLER
 * ============================================================================
 */

void handle_write(proxy_config_t *config, connection_t *conn) {
    if (!connection_can_write(conn)) {
        return;
    }
    
    while (1) {
        ssize_t n = buffer_write_fd(&conn->write_buf, conn->fd);
        
        if (n > 0) {
            connection_update_activity(conn);
            config->stats.bytes_sent += n;
            
            if (buffer_is_empty(&conn->write_buf)) {
                break;
            }
            continue;
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno != EPIPE && errno != ECONNRESET) {
                perror("write");
            }
            config->stats.errors++;
            
            /* In HTTP mode, client writes are special */
            if (config->mode == PROXY_MODE_HTTP && conn->is_client) {
                /* Response write failed - close connection */
                connection_close(config, conn);
            } else {
                /* TCP mode or backend write - close both */
                connection_close_pair(config, conn);
            }
            return;
        }
    }
    
    /* If we finished writing response and it's not keep-alive, close */
    if (config->mode == PROXY_MODE_HTTP && conn->is_client && 
        buffer_is_empty(&conn->write_buf) && !conn->keep_alive) {
        connection_close(config, conn);
        return;
    }
    
    /* If we finished writing response and it IS keep-alive, prepare for next request */
    if (config->mode == PROXY_MODE_HTTP && conn->is_client && 
        buffer_is_empty(&conn->write_buf) && conn->keep_alive) {
        
        /* Reset for next request */
        buffer_clear(&conn->read_buf);
        buffer_clear(&conn->write_buf);
        http_request_init((http_request_t*)conn->http_req);
        conn->state = CONN_READING_REQUEST;
        conn->requests_handled++;
        
        /* Check max requests limit */
        if (conn->requests_handled >= MAX_REQUESTS_PER_CONN) {
            connection_close(config, conn);
            return;
        }
        
        config->stats.keep_alive_reused++;
    }
    
    update_epoll_events(config, conn);
    if (conn->peer) {
        update_epoll_events(config, conn->peer);
    }
}

/* ============================================================================
 * HTTP REQUEST HANDLER
 * ============================================================================
 */

void handle_http_request(proxy_config_t *config, connection_t *client) {
    /* We have a complete, valid HTTP request in client->read_buf
     * Now we need to forward it to backend
     */
    
    /* Create backend connection */
    int backend_fd = create_backend_connection(config->backend_addr,
                                               config->backend_port);
    if (backend_fd == -1) {
        fprintf(stderr, "Failed to connect to backend\n");
        send_http_error(client, 502, "Bad Gateway");
        return;
    }
    
    /* Allocate connection for backend */
    connection_t *backend = connection_alloc(config);
    if (backend == NULL) {
        fprintf(stderr, "Connection pool exhausted for backend\n");
        close(backend_fd);
        send_http_error(client, 503, "Service Unavailable");
        return;
    }
    
    /* Initialize backend connection */
    connection_init(backend, backend_fd, 0, CONN_CONNECTING);
    
    /* Pair client and backend */
    connection_pair(client, backend);
    
    /* Copy request data to backend write buffer */
    http_request_t *req = (http_request_t*)client->http_req;
    size_t request_len = req->total_length;
    if (request_len > BUFFER_SIZE) {
        /* Request too large for our buffer - shouldn't happen if we validated */
        fprintf(stderr, "Request too large: %zu bytes\n", request_len);
        connection_close(config, backend);
        send_http_error(client, 413, "Request Entity Too Large");
        return;
    }
    
    memcpy(backend->write_buf.data, client->read_buf.data, request_len);
    backend->write_buf.len = request_len;
    backend->write_buf.pos = 0;
    
    /* Clear client read buffer */
    buffer_clear(&client->read_buf);
    
    /* Save keep-alive preference */
    client->keep_alive = req->keep_alive;
    
    /* Add backend to epoll */
    if (epoll_add(config->epoll_fd, backend_fd, EPOLLOUT, backend) == -1) {
        connection_close_pair(config, client);
        return;
    }
    
    /* Update client state */
    client->state = CONN_WRITING_RESPONSE;
    
    /* Update epoll for client (we want to write response to it later) */
    update_epoll_events(config, client);
}

/* ============================================================================
 * ERROR RESPONSE
 * ============================================================================
 */

void send_http_error(connection_t *client, int status_code, const char *message) {
    const char *status_line = http_get_status_line(status_code);
    
    char response[1024];
    int len = snprintf(response, sizeof(response),
        "%s"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s\n",
        status_line,
        strlen(message) + 1,
        message
    );
    
    /* Write error response to client buffer */
    if (len > 0 && (size_t)len < sizeof(response)) {
        size_t copy_len = (size_t)len < BUFFER_SIZE ? len : BUFFER_SIZE;
        memcpy(client->write_buf.data, response, copy_len);
        client->write_buf.len = copy_len;
        client->write_buf.pos = 0;
        client->keep_alive = 0;  /* Close after error */
    }
}

/* ============================================================================
 * CONNECT HANDLER
 * ============================================================================
 */

void handle_connect(proxy_config_t *config, connection_t *conn) {
    int error = 0;
    socklen_t len = sizeof(error);
    
    if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &error, &len) == -1) {
        perror("getsockopt SO_ERROR");
        
        /* In HTTP mode, send error to client */
        if (config->mode == PROXY_MODE_HTTP && conn->peer && conn->peer->is_client) {
            send_http_error(conn->peer, 502, "Bad Gateway");
        } else {
            connection_close_pair(config, conn);
        }
        return;
    }
    
    if (error != 0) {
        errno = error;
        perror("backend connect");
        config->stats.errors++;
        
        /* In HTTP mode, send error to client */
        if (config->mode == PROXY_MODE_HTTP && conn->peer && conn->peer->is_client) {
            send_http_error(conn->peer, 502, "Bad Gateway");
        } else {
            connection_close_pair(config, conn);
        }
        return;
    }
    
    /* Connection succeeded */
    connection_set_state(conn, CONN_CONNECTED);
    update_epoll_events(config, conn);
}

/* ============================================================================
 * ERROR HANDLER
 * ============================================================================
 */

void handle_error(proxy_config_t *config, connection_t *conn) {
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
    
    /* In HTTP mode, client errors don't close backend */
    if (config->mode == PROXY_MODE_HTTP && conn->is_client) {
        connection_close(config, conn);
    } else {
        connection_close_pair(config, conn);
    }
}

/* ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================
 */

ssize_t forward_data(connection_t *src, connection_t *dst) {
    if (src == NULL || dst == NULL) {
        return -1;
    }
    
    size_t available = buffer_readable_bytes(&src->read_buf);
    if (available == 0) {
        return 0;
    }
    
    size_t space = buffer_writable_bytes(&dst->write_buf);
    if (space == 0) {
        return 0;
    }
    
    size_t to_copy = available < space ? available : space;
    
    memcpy(dst->write_buf.data + dst->write_buf.len,
           src->read_buf.data + src->read_buf.pos,
           to_copy);
    
    dst->write_buf.len += to_copy;
    src->read_buf.pos += to_copy;
    
    if (src->read_buf.pos >= src->read_buf.len) {
        buffer_clear(&src->read_buf);
    }
    
    if (dst->write_buf.pos > 0 && buffer_writable_bytes(&dst->write_buf) < 1024) {
        buffer_compact(&dst->write_buf);
    }
    
    return to_copy;
}

int update_epoll_events(proxy_config_t *config, connection_t *conn) {
    if (!connection_is_valid(conn)) {
        return -1;
    }
    
    uint32_t events = 0;
    
    if (connection_wants_read(conn)) {
        events |= EPOLLIN;
    }
    
    if (connection_wants_write(conn)) {
        events |= EPOLLOUT;
    }
    
    if (events == 0) {
        events = EPOLLIN;  /* Keep minimal registration */
    }
    
    return epoll_mod(config->epoll_fd, conn->fd, events, conn);
}

void print_stats(const proxy_config_t *config) {
    printf("\n=== Proxy Statistics ===\n");
    printf("Mode:               %s\n", 
           config->mode == PROXY_MODE_HTTP ? "HTTP" : "TCP");
    printf("Total connections:  %lu\n", config->stats.total_connections);
    printf("Active connections: %lu\n", config->stats.active_connections);
    printf("Bytes received:     %lu\n", config->stats.bytes_received);
    printf("Bytes sent:         %lu\n", config->stats.bytes_sent);
    printf("Errors:             %lu\n", config->stats.errors);
    
    if (config->mode == PROXY_MODE_HTTP) {
        printf("\n--- HTTP Stats ---\n");
        printf("Requests total:     %lu\n", config->stats.requests_total);
        printf("Requests GET:       %lu\n", config->stats.requests_get);
        printf("Requests POST:      %lu\n", config->stats.requests_post);
        printf("Requests error:     %lu\n", config->stats.requests_error);
        printf("Keep-alive reused:  %lu\n", config->stats.keep_alive_reused);
    }
    
    printf("========================\n");
}