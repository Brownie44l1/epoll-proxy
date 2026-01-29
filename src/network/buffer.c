#include "buffer.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

/* ============================================================================
 * BUFFER IMPLEMENTATION
 * ============================================================================
 */

void buffer_init(buffer_t *buf) {
    /* Zero everything. Technically only need to set len=0 and pos=0,
     * but zeroing the data array helps with debugging (valgrind, gdb).
     * The performance cost is negligible during connection setup.
     */
    memset(buf, 0, sizeof(buffer_t));
}

void buffer_clear(buffer_t *buf) {
    /* Just reset the pointers. Don't zero the data array - that's wasted
     * cycles since we'll overwrite it on next read anyway.
     * This is called frequently (every connection reuse), so speed matters.
     */
    buf->len = 0;
    buf->pos = 0;
}

ssize_t buffer_read_fd(buffer_t *buf, int fd) {
    /* Sanity check: if buffer is full, we can't read more.
     * This shouldn't happen in normal operation because we apply backpressure
     * (stop reading from fast side), but defensive programming prevents buffer
     * overflows.
     */
    if (buf->len >= BUFFER_SIZE) {
        errno = ENOBUFS;  /* No buffer space available */
        return -1;
    }
    
    /* Read into the buffer starting at buf->len position.
     * We read as much as will fit: BUFFER_SIZE - buf->len bytes.
     * 
     * Why read at buf->len instead of buf->pos?
     *   - buf->pos is for reading OUT of the buffer (writes to peer)
     *   - buf->len is where new data gets appended
     *   - This is like a queue: write at tail (len), read from head (pos)
     */
    ssize_t n = read(fd, buf->data + buf->len, BUFFER_SIZE - buf->len);
    
    if (n > 0) {
        /* Successfully read n bytes. Update len to reflect new data. */
        buf->len += n;
    } else if (n == 0) {
        /* EOF: peer closed the connection gracefully.
         * This is normal and expected. The caller will handle cleanup.
         */
        return 0;
    } else {
        /* n == -1: error occurred.
         * With non-blocking sockets, EAGAIN/EWOULDBLOCK are EXPECTED.
         * They mean "no data available right now, try again later."
         * Edge-triggered epoll requires us to read until EAGAIN.
         * 
         * Other errors (ECONNRESET, EPIPE, etc.) are real failures.
         */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* This is normal - we've drained the socket buffer.
             * Return -1 with errno set so caller knows to stop reading.
             */
            return -1;
        }
        /* Real error - caller should close connection */
        return -1;
    }
    
    return n;
}

ssize_t buffer_write_fd(buffer_t *buf, int fd) {
    /* Nothing to write? Don't even make the syscall.
     * This saves CPU when called in a loop.
     */
    if (buf->pos >= buf->len) {
        return 0;
    }
    
    /* Write starting from buf->pos (where we left off last time).
     * Write up to buf->len (total data in buffer).
     * 
     * Why buf->pos instead of always starting at 0?
     *   Partial writes! write() might only accept 2KB of our 8KB buffer
     *   if the socket buffer is nearly full. We track position so we
     *   don't re-send the same data.
     */
    ssize_t n = write(fd, buf->data + buf->pos, buf->len - buf->pos);
    
    if (n > 0) {
        /* Successfully wrote n bytes. Advance position. */
        buf->pos += n;
        
        /* Optimization: if we wrote everything, reset to beginning.
         * This avoids needing to compact the buffer later.
         * Condition: buf->pos == buf->len means buffer is empty.
         */
        if (buf->pos >= buf->len) {
            buf->pos = 0;
            buf->len = 0;
        }
    } else if (n == 0) {
        /* write() returned 0 - this is unusual for sockets.
         * According to POSIX, this shouldn't happen with non-blocking sockets.
         * If it does, treat it as "try again later".
         */
        errno = EAGAIN;
        return -1;
    } else {
        /* n == -1: error occurred.
         * EAGAIN/EWOULDBLOCK mean socket send buffer is full.
         * This is expected with non-blocking I/O.
         */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Socket buffer full - stop writing, wait for EPOLLOUT. */
            return -1;
        }
        /* Real error (EPIPE, ECONNRESET, etc.) - caller should close */
        return -1;
    }
    
    return n;
}

int buffer_is_full(const buffer_t *buf) {
    /* Buffer is full when len reaches BUFFER_SIZE.
     * We can't append more data without overflowing.
     */
    return buf->len >= BUFFER_SIZE;
}

int buffer_is_empty(const buffer_t *buf) {
    /* Buffer is empty when pos catches up to len.
     * All data has been written out.
     */
    return buf->pos >= buf->len;
}

size_t buffer_readable_bytes(const buffer_t *buf) {
    /* How much data is waiting to be written out?
     * This is the delta between what we've buffered and what we've sent.
     */
    return buf->len - buf->pos;
}

size_t buffer_writable_bytes(const buffer_t *buf) {
    /* How much free space is left for reading new data?
     * Once len hits BUFFER_SIZE, we must stop reading or compact.
     */
    return BUFFER_SIZE - buf->len;
}

void buffer_compact(buffer_t *buf) {
    /* If buffer is already at the beginning, nothing to do.
     * This check avoids unnecessary memmove when pos == 0.
     */
    if (buf->pos == 0) {
        return;
    }
    
    /* If buffer is empty, just reset pointers.
     * No need to move zero bytes.
     */
    if (buf->pos >= buf->len) {
        buf->pos = 0;
        buf->len = 0;
        return;
    }
    
    /* Move unwritten data to the beginning of the buffer.
     * Example:
     *   Before: [____xxxx] pos=4, len=8 (4 bytes written, 4 remaining)
     *   After:  [xxxx____] pos=0, len=4
     * 
     * This reclaims the space used by already-written data.
     * 
     * memmove() is used instead of memcpy() because source and destination
     * overlap. memmove() handles this correctly.
     * 
     * Performance note: memmove() is optimized in glibc and is very fast
     * for small buffers (our typical case). For 8KB buffers, this is ~100ns.
     */
    size_t remaining = buf->len - buf->pos;
    memmove(buf->data, buf->data + buf->pos, remaining);
    buf->pos = 0;
    buf->len = remaining;
}