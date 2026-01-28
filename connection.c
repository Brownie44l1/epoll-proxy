#define _POSIX_C_SOURCE 200809L
#include "connection.h"
#include "buffer.h"
#include "epoll.h"
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>

/* ============================================================================
 * CONNECTION POOL MANAGEMENT
 * ============================================================================
 */

void connection_pool_init(proxy_config_t *config) {
    /* Initialize all connections to CONN_CLOSED state.
     * This marks them as available for allocation.
     */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connection_t *conn = &config->connections[i];
        conn->fd = -1;
        conn->peer = NULL;
        conn->state = CONN_CLOSED;
        conn->is_client = 0;
        conn->last_active = 0;
        
        /* Initialize buffers */
        buffer_init(&conn->read_buf);
        buffer_init(&conn->write_buf);
        
        /* Add to free list.
         * We build the free list in reverse order (MAX_CONNECTIONS-1 down to 0)
         * so that popping gives us indices in forward order.
         * This doesn't matter functionally, but it makes debugging easier
         * (connection[0] is allocated first).
         */
        config->free_list[i] = MAX_CONNECTIONS - 1 - i;
    }
    
    /* All connections are free initially */
    config->free_count = MAX_CONNECTIONS;
    
    /* Initialize statistics */
    memset(&config->stats, 0, sizeof(config->stats));
}

connection_t* connection_alloc(proxy_config_t *config) {
    /* Check if we have free connections available.
     * If free_count == 0, we've hit our MAX_CONNECTIONS limit.
     */
    if (config->free_count == 0) {
        /* Pool exhausted. This is a resource limit, not an error.
         * 
         * Options for handling this:
         * 1. Reject new connections (what we do here)
         * 2. Close oldest idle connection (LRU eviction)
         * 3. Increase MAX_CONNECTIONS and rebuild
         * 
         * For a proxy, option 1 is safest - we don't want to randomly
         * disconnect existing clients just to accept new ones.
         */
        fprintf(stderr, "Connection pool exhausted (%d connections active)\n",
                MAX_CONNECTIONS);
        return NULL;
    }
    
    /* Pop from free list (stack operation).
     * free_count is both the count AND the index of the next free slot.
     * 
     * Example:
     *   free_list = [5, 3, 7, ...], free_count = 3
     *   Pop: index = free_list[2] = 7, free_count = 2
     */
    config->free_count--;
    int index = config->free_list[config->free_count];
    
    connection_t *conn = &config->connections[index];
    
    /* Sanity check: connection should be in CLOSED state.
     * If not, we have a bug in our free list management.
     */
    if (conn->state != CONN_CLOSED) {
        fprintf(stderr, "BUG: Allocated connection %d in state %d\n",
                index, conn->state);
        /* Try to recover by forcing it to closed state */
        conn->state = CONN_CLOSED;
    }
    
    /* Update statistics */
    config->stats.total_connections++;
    config->stats.active_connections++;
    
    return conn;
}

void connection_free(proxy_config_t *config, connection_t *conn) {
    /* Sanity checks */
    if (conn == NULL) {
        return;
    }
    
    /* Calculate index of this connection in the pool.
     * Pointer arithmetic: conn is in connections array, so
     * index = (conn - &connections[0])
     */
    int index = conn - config->connections;
    
    /* Validate index is in range */
    if (index < 0 || index >= MAX_CONNECTIONS) {
        fprintf(stderr, "BUG: Freeing connection outside pool range\n");
        return;
    }
    
    /* Mark as closed */
    conn->state = CONN_CLOSED;
    conn->fd = -1;
    conn->peer = NULL;
    
    /* Clear buffers (fast - just resets pointers) */
    buffer_clear(&conn->read_buf);
    buffer_clear(&conn->write_buf);
    
    /* Push back onto free list.
     * free_count is the next available slot.
     */
    if (config->free_count >= MAX_CONNECTIONS) {
        fprintf(stderr, "BUG: Free list overflow\n");
        return;
    }
    
    config->free_list[config->free_count] = index;
    config->free_count++;
    
    /* Update statistics */
    config->stats.active_connections--;
}

/* ============================================================================
 * CONNECTION LIFECYCLE
 * ============================================================================
 */

void connection_init(connection_t *conn, int fd, int is_client,
                    conn_state_t state) {
    conn->fd = fd;
    conn->is_client = is_client;
    conn->state = state;
    conn->peer = NULL;
    conn->last_active = get_timestamp_ms();
    
    /* Clear buffers */
    buffer_clear(&conn->read_buf);
    buffer_clear(&conn->write_buf);
}

void connection_pair(connection_t *client, connection_t *backend) {
    /* Create bidirectional link.
     * This is the magic that makes forwarding trivial:
     *   read(client) → write(client->peer)
     *   read(backend) → write(backend->peer)
     */
    client->peer = backend;
    backend->peer = client;
}

void connection_unpair(connection_t *conn) {
    if (conn == NULL) {
        return;
    }
    
    /* Break the bidirectional link.
     * If conn->peer exists, break its link back to us too.
     */
    if (conn->peer != NULL) {
        conn->peer->peer = NULL;
        conn->peer = NULL;
    }
}

void connection_close(proxy_config_t *config, connection_t *conn) {
    if (conn == NULL || conn->state == CONN_CLOSED) {
        return;
    }
    
    /* Remove from epoll interest list.
     * We do this before close() to avoid race conditions.
     * 
     * Note: Linux automatically removes fds from epoll on close(),
     * but explicit removal is clearer and portable.
     */
    if (conn->fd >= 0) {
        epoll_del(config->epoll_fd, conn->fd);
        close(conn->fd);
    }
    
    /* Unpair from peer.
     * This prevents the peer from trying to forward data to us after we're freed.
     * Important: This doesn't close the peer - caller must decide that.
     */
    connection_unpair(conn);
    
    /* Return to free list */
    connection_free(config, conn);
}

void connection_close_pair(proxy_config_t *config, connection_t *conn) {
    if (conn == NULL) {
        return;
    }
    
    /* Save peer pointer before closing, since connection_close() unpairs */
    connection_t *peer = conn->peer;
    
    /* Close this connection */
    connection_close(config, conn);
    
    /* Close peer if it exists.
     * connection_close() already unparied, so peer->peer is NULL now.
     */
    if (peer != NULL) {
        connection_close(config, peer);
    }
}

void connection_set_state(connection_t *conn, conn_state_t state) {
    if (conn == NULL) {
        return;
    }
    
    /* In a production system, you might log state transitions here:
     * printf("conn[%d]: %s -> %s\n", conn->fd, 
     *        state_name(conn->state), state_name(state));
     * 
     * This is invaluable for debugging complex state machine issues.
     */
    conn->state = state;
}

int connection_is_valid(const connection_t *conn) {
    return conn != NULL && 
           conn->state != CONN_CLOSED && 
           conn->fd >= 0;
}

void connection_update_activity(connection_t *conn) {
    if (conn != NULL) {
        conn->last_active = get_timestamp_ms();
    }
}

uint64_t get_timestamp_ms(void) {
    struct timespec ts;
    
    /* CLOCK_MONOTONIC is unaffected by system time changes (NTP, DST, etc.).
     * CLOCK_REALTIME would jump if admin changes system time.
     * 
     * For timeout tracking, monotonic is critical - we don't want timeouts
     * to fire early/late because someone adjusted the clock.
     */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        perror("clock_gettime");
        return 0;
    }
    
    /* Convert to milliseconds.
     * ts.tv_sec is in seconds, ts.tv_nsec is in nanoseconds.
     * 1 second = 1000 milliseconds
     * 1 nanosecond = 0.000001 milliseconds
     */
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ============================================================================
 * STATE MACHINE HELPERS
 * ============================================================================
 * These functions encode the state machine logic.
 * They answer: "In this state, should I do X?"
 */

int connection_can_read(const connection_t *conn) {
    if (!connection_is_valid(conn)) {
        return 0;
    }
    
    /* Can't read if we're not in a readable state.
     * CONN_CONNECTING: waiting for connect() to complete
     * CONN_CLOSING: shutting down
     * CONN_CLOSED: already closed
     */
    if (conn->state != CONN_CONNECTED && 
        conn->state != CONN_READING) {
        return 0;
    }
    
    /* Can't read if peer doesn't exist.
     * Where would we forward the data?
     */
    if (conn->peer == NULL) {
        return 0;
    }
    
    /* Can't read if peer's write buffer is full.
     * This is BACKPRESSURE: the peer is slow, so we stop reading
     * from the fast side to avoid buffering infinite data.
     * 
     * TCP flow control will handle this at the kernel level:
     * - We stop reading → our recv buffer fills
     * - Kernel stops ACKing → sender's send buffer fills
     * - Sender slows down
     * 
     * This is how TCP naturally handles speed mismatches.
     */
    if (buffer_is_full(&conn->peer->write_buf)) {
        return 0;
    }
    
    return 1;
}

int connection_can_write(const connection_t *conn) {
    if (!connection_is_valid(conn)) {
        return 0;
    }
    
    /* Can't write if we're not in a writable state */
    if (conn->state != CONN_CONNECTED && 
        conn->state != CONN_WRITING) {
        return 0;
    }
    
    /* Only write if we have data.
     * Otherwise we'd get EAGAIN immediately - waste of a syscall.
     */
    if (buffer_is_empty(&conn->write_buf)) {
        return 0;
    }
    
    return 1;
}

int connection_wants_read(const connection_t *conn) {
    /* Want to read if we can read.
     * This determines if we register for EPOLLIN.
     * 
     * If we don't want to read (e.g., peer buffer full), we deregister
     * from EPOLLIN to avoid wakeups we can't handle.
     */
    return connection_can_read(conn);
}

int connection_wants_write(const connection_t *conn) {
    if (!connection_is_valid(conn)) {
        return 0;
    }
    
    /* Want to write if:
     * 1. We're waiting for async connect to complete (CONN_CONNECTING)
     * 2. We have data to write
     */
    if (conn->state == CONN_CONNECTING) {
        /* Async connect in progress.
         * When socket becomes writable, connect is complete.
         * We need EPOLLOUT to detect this.
         */
        return 1;
    }
    
    /* Want to write if we have buffered data */
    if (!buffer_is_empty(&conn->write_buf)) {
        return 1;
    }
    
    /* Otherwise, don't register for EPOLLOUT.
     * EPOLLOUT fires whenever socket is writable (almost always),
     * so registering when we have nothing to write causes busy-waiting.
     * 
     * This is a common epoll mistake:
     *   BAD:  Always register for EPOLLOUT
     *   GOOD: Only register when you have data to send
     */
    return 0;
}