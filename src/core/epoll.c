#include "epoll.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* ============================================================================
 * EPOLL OPERATIONS
 * ============================================================================
 */

int epoll_init(void) {
    /* epoll_create1() is the modern interface (vs old epoll_create(size)).
     * The size parameter in old API was a hint and is now ignored.
     * 
     * EPOLL_CLOEXEC: Close this fd if we exec() another program.
     * This prevents fd leaks to child processes.
     */
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    
    if (epoll_fd == -1) {
        perror("epoll_create1");
        return -1;
    }
    
    return epoll_fd;
}

int epoll_add(int epoll_fd, int fd, uint32_t events, void *conn) {
    struct epoll_event ev;
    
    /* We always use EPOLLET (edge-triggered mode).
     * 
     * Edge-triggered vs Level-triggered:
     * 
     * Level-triggered (default):
     *   - epoll_wait() returns as long as data is available
     *   - If you read 100 bytes but 200 are available, epoll_wait() 
     *     returns again immediately
     *   - Easy to use but can cause busy-looping
     * 
     * Edge-triggered (EPOLLET):
     *   - epoll_wait() returns only on state CHANGE
     *   - If you don't read all data, epoll_wait() won't notify you again
     *     until NEW data arrives
     *   - Forces you to drain socket (read until EAGAIN)
     *   - More efficient: fewer syscalls, no busy loops
     * 
     * For high performance, edge-triggered is the right choice.
     * It requires discipline: always read/write until EAGAIN.
     */
    ev.events = events | EPOLLET;
    
    /* We also add these events to catch error conditions:
     * 
     * EPOLLRDHUP: Peer closed connection (TCP FIN received)
     *   This lets us detect half-closed connections gracefully
     * 
     * EPOLLHUP: Hang up (socket closed)
     *   This is set automatically, but being explicit is clearer
     * 
     * EPOLLERR: Error condition
     *   Also automatic, but we check it anyway
     */
    ev.events |= EPOLLRDHUP | EPOLLHUP | EPOLLERR;
    
    /* Store the connection pointer in the event data.
     * When epoll_wait() returns, we can get our connection back:
     *   connection_t *conn = (connection_t*)event.data.ptr;
     * 
     * Alternative: store fd in event.data.fd and lookup connection
     * in a hash table. But direct pointer is faster (no lookup).
     */
    ev.data.ptr = conn;
    
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        /* Common errors:
         * EEXIST: fd already registered (programmer error)
         * ENOSPC: Out of memory or hit kernel limit
         * EBADF: Invalid fd
         */
        perror("epoll_ctl ADD");
        return -1;
    }
    
    return 0;
}

int epoll_mod(int epoll_fd, int fd, uint32_t events, void *conn) {
    struct epoll_event ev;
    
    /* Same event configuration as epoll_add */
    ev.events = events | EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
    ev.data.ptr = conn;
    
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == -1) {
        /* ENOENT: fd not registered (programmer error)
         * Usually means we tried to modify before adding
         */
        perror("epoll_ctl MOD");
        return -1;
    }
    
    return 0;
}

int epoll_del(int epoll_fd, int fd) {
    /* In Linux 2.6.9+, the event pointer can be NULL for EPOLL_CTL_DEL.
     * We don't need to pass event details when removing.
     */
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        /* Errors here are usually benign:
         * ENOENT: fd wasn't registered (maybe already removed)
         * EBADF: fd already closed
         * 
         * We can safely ignore these - the fd is gone either way.
         */
        if (errno != ENOENT && errno != EBADF) {
            perror("epoll_ctl DEL");
        }
        return -1;
    }
    
    return 0;
}

int epoll_wait_events(int epoll_fd, struct epoll_event *events,
                      int max_events, int timeout_ms) {
    /* epoll_wait() blocks until:
     * 1. One or more fds are ready
     * 2. Timeout expires
     * 3. Signal is delivered (returns EINTR)
     * 
     * Returns: number of ready events, 0 on timeout, -1 on error
     */
    int n = epoll_wait(epoll_fd, events, max_events, timeout_ms);
    
    if (n == -1) {
        /* EINTR: Interrupted by signal (e.g., SIGTERM, SIGINT)
         * This is expected and should be handled by caller.
         * Don't print error for EINTR - it clutters logs.
         */
        if (errno != EINTR) {
            perror("epoll_wait");
        }
        return -1;
    }
    
    return n;
}

/* ============================================================================
 * SOCKET UTILITIES
 * ============================================================================
 */

int set_nonblocking(int fd) {
    /* fcntl() is the POSIX way to manipulate file descriptor flags.
     * F_GETFL: get current flags
     * F_SETFL: set flags
     * O_NONBLOCK: non-blocking I/O flag
     * 
     * Two-step process:
     * 1. Get current flags (preserves other flags like O_APPEND)
     * 2. Set new flags with O_NONBLOCK added
     */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return -1;
    }
    
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");
        return -1;
    }
    
    return 0;
}

int set_socket_options(int fd) {
    int optval;
    
    /* SO_REUSEADDR: Allow binding to an address that's in TIME_WAIT state.
     * 
     * Without this, if you restart the proxy within ~60 seconds, bind()
     * fails with EADDRINUSE because the old socket is in TIME_WAIT.
     * 
     * TIME_WAIT is a TCP state that lasts 2*MSL (maximum segment lifetime)
     * to ensure old packets don't confuse new connections.
     * 
     * SO_REUSEADDR is safe for servers that bind to specific addresses.
     */
    optval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        perror("setsockopt SO_REUSEADDR");
        return -1;
    }
    
    /* SO_KEEPALIVE: Enable TCP keepalive packets.
     * 
     * If a connection is idle for a long time, TCP sends keepalive probes
     * to detect if the peer is still alive. This catches scenarios like:
     * - Client machine crashes without sending FIN
     * - Network cable unplugged
     * - Firewall silently drops connection
     * 
     * Default Linux keepalive: 2 hours idle, then probes every 75 seconds.
     * You can tune this with /proc/sys/net/ipv4/tcp_keepalive_* sysctls.
     */
    optval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) == -1) {
        perror("setsockopt SO_KEEPALIVE");
        /* Non-fatal - keepalive is nice to have but not required */
    }
    
    /* TCP_NODELAY: Disable Nagle's algorithm.
     * 
     * Nagle's algorithm batches small packets to reduce network overhead:
     * - If you write() 10 bytes, TCP waits up to 200ms to see if you'll
     *   write more, then sends one larger packet
     * - Great for throughput (fewer packets)
     * - Terrible for latency (200ms delay per small write)
     * 
     * For a proxy, we care about latency. Every millisecond of delay
     * compounds: client -> proxy -> backend -> proxy -> client.
     * 
     * Disabling Nagle means small packets are sent immediately.
     * 
     * Trade-off:
     *   Pro: Lower latency (critical for interactive apps)
     *   Con: More packets (40-byte TCP header for each small write)
     * 
     * For HTTP, most requests/responses are > 1 MTU anyway, so the
     * overhead is minimal. Latency wins.
     */
    optval = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) == -1) {
        perror("setsockopt TCP_NODELAY");
        /* Non-fatal - proxy works without it, just slower */
    }
    
    return 0;
}

int create_listen_socket(const char *addr, uint16_t port) {
    int listen_fd;
    struct sockaddr_in server_addr;
    
    /* Create TCP socket.
     * AF_INET: IPv4 (use AF_INET6 for IPv6)
     * SOCK_STREAM: TCP (vs SOCK_DGRAM for UDP)
     * 0: Protocol (0 means "default for this socket type" = TCP)
     */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        return -1;
    }
    
    /* Set socket options before bind() */
    if (set_socket_options(listen_fd) == -1) {
        close(listen_fd);
        return -1;
    }
    
    /* Set non-blocking mode.
     * This makes accept() non-blocking, which is required for edge-triggered
     * epoll. If multiple connections are pending, we must accept() in a loop
     * until EAGAIN.
     */
    if (set_nonblocking(listen_fd) == -1) {
        close(listen_fd);
        return -1;
    }
    
    /* Configure server address structure */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);  /* Convert to network byte order */
    
    /* Convert IP address string to binary form.
     * inet_pton() handles both IPv4 and IPv6.
     * "0.0.0.0" means bind to all interfaces.
     */
    if (inet_pton(AF_INET, addr, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(listen_fd);
        return -1;
    }
    
    /* Bind socket to address and port.
     * This reserves the port and IP for this process.
     */
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(listen_fd);
        return -1;
    }
    
    /* Start listening for connections.
     * LISTEN_BACKLOG is the queue size for pending connections.
     * 
     * When SYN arrives:
     * 1. Kernel sends SYN-ACK
     * 2. Connection enters SYN_RCVD state
     * 3. When ACK arrives, connection enters ESTABLISHED and waits in queue
     * 4. accept() removes connection from queue
     * 
     * If queue is full, kernel drops SYN packets (client sees timeout).
     * 
     * Typical values: 128 (conservative), 511 (common), 1024 (high traffic)
     */
    if (listen(listen_fd, LISTEN_BACKLOG) == -1) {
        perror("listen");
        close(listen_fd);
        return -1;
    }
    
    return listen_fd;
}

int create_backend_connection(const char *addr, uint16_t port) {
    int backend_fd;
    struct sockaddr_in backend_addr;
    
    /* Create TCP socket for backend connection */
    backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend_fd == -1) {
        perror("socket");
        return -1;
    }
    
    /* Set non-blocking BEFORE connect().
     * This makes connect() non-blocking, which is critical for async I/O.
     */
    if (set_nonblocking(backend_fd) == -1) {
        close(backend_fd);
        return -1;
    }
    
    /* Set socket options */
    if (set_socket_options(backend_fd) == -1) {
        close(backend_fd);
        return -1;
    }
    
    /* Configure backend address */
    memset(&backend_addr, 0, sizeof(backend_addr));
    backend_addr.sin_family = AF_INET;
    backend_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, addr, &backend_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(backend_fd);
        return -1;
    }
    
    /* Initiate connection to backend.
     * 
     * With non-blocking socket, connect() returns immediately:
     * - Success: returns 0 (connection completed immediately, rare)
     * - In progress: returns -1 with errno = EINPROGRESS (normal case)
     * - Failure: returns -1 with other errno (ECONNREFUSED, etc.)
     * 
     * For EINPROGRESS, we add socket to epoll with EPOLLOUT.
     * When socket becomes writable, connection is complete.
     * We then check getsockopt(SO_ERROR) to see if connection succeeded.
     */
    int ret = connect(backend_fd, (struct sockaddr*)&backend_addr, 
                     sizeof(backend_addr));
    
    if (ret == -1) {
        if (errno == EINPROGRESS) {
            /* Normal case - connection in progress.
             * Caller should add to epoll with EPOLLOUT and wait.
             */
            return backend_fd;
        } else {
            /* Real error - connection failed immediately */
            perror("connect");
            close(backend_fd);
            return -1;
        }
    }
    
    /* Connection completed immediately (unlikely but possible for localhost) */
    return backend_fd;
}