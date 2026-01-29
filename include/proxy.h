#ifndef PROXY_H
#define PROXY_H

#include "config.h"
#include <sys/epoll.h>

/* ============================================================================
 * PROXY INITIALIZATION
 * ============================================================================
 */

/* Initialize proxy in HTTP mode (new)
 * This mode makes the proxy HTTP-aware for better performance:
 * - Parses HTTP requests
 * - Handles keep-alive properly
 * - Can route based on Host header or path
 * - Validates requests before forwarding
 */
int proxy_init_http(proxy_config_t *config,
                    const char *listen_addr, uint16_t listen_port,
                    const char *backend_addr, uint16_t backend_port);

/* Initialize proxy in TCP mode (original behavior) */
int proxy_init(proxy_config_t *config,
               const char *listen_addr, uint16_t listen_port,
               const char *backend_addr, uint16_t backend_port);

/* Run the proxy event loop */
int proxy_run(proxy_config_t *config);

/* Cleanup and shutdown */
void proxy_cleanup(proxy_config_t *config);

/* ============================================================================
 * EVENT HANDLERS
 * ============================================================================
 */

/* Handle new client connection */
void handle_accept(proxy_config_t *config);

/* Handle read event - now HTTP-aware in HTTP mode */
void handle_read(proxy_config_t *config, connection_t *conn);

/* Handle write event */
void handle_write(proxy_config_t *config, connection_t *conn);

/* Handle backend connection completion */
void handle_connect(proxy_config_t *config, connection_t *conn);

/* Handle error/hangup */
void handle_error(proxy_config_t *config, connection_t *conn);

/* ============================================================================
 * HTTP-SPECIFIC HANDLERS
 * ============================================================================
 */

/* Handle complete HTTP request (called when we have full request parsed) */
void handle_http_request(proxy_config_t *config, connection_t *client);

/* Send HTTP error response to client */
void send_http_error(connection_t *client, int status_code, const char *message);

/* ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================
 */

/* Forward data from source to peer */
ssize_t forward_data(connection_t *src, connection_t *dst);

/* Update epoll registration for connection */
int update_epoll_events(proxy_config_t *config, connection_t *conn);

/* Print statistics */
void print_stats(const proxy_config_t *config);

#endif /* PROXY_H */