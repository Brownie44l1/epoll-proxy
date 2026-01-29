#ifndef BUFFER_H
#define BUFFER_H

#include "config.h"
#include <sys/types.h>

/* ============================================================================
 * BUFFER OPERATIONS
 * ============================================================================
 * These functions abstract buffer management. They handle the tedious details
 * of tracking positions, lengths, and edge cases.
 * 
 * Design principle: Keep it simple. No fancy algorithms. The buffer is just
 * a sliding window over an array.
 */

/* Initialize a buffer to empty state.
 * Call this once when allocating a new connection.
 */
void buffer_init(buffer_t *buf);

/* Reset buffer to empty without freeing (we reuse the struct).
 * Call this when a connection is recycled from the free list.
 */
void buffer_clear(buffer_t *buf);

/* Read from fd into buffer.
 * Returns:
 *   > 0: number of bytes read
 *   0: EOF (peer closed connection gracefully)
 *   -1: error (check errno - EAGAIN is expected with non-blocking)
 * 
 * This handles partial reads automatically. With edge-triggered epoll,
 * we must read until EAGAIN, so this will be called in a loop.
 */
ssize_t buffer_read_fd(buffer_t *buf, int fd);

/* Write from buffer to fd.
 * Returns:
 *   > 0: number of bytes written
 *   0: nothing written (shouldn't happen with non-blocking sockets)
 *   -1: error (check errno - EAGAIN means socket buffer full)
 * 
 * Handles partial writes. Updates buf->pos to track where we left off.
 * Call this in a loop until buffer is empty or EAGAIN.
 */
ssize_t buffer_write_fd(buffer_t *buf, int fd);

/* Check if buffer is full (no room for more reads).
 * If true, we have a problem: peer is sending faster than we can forward.
 * Options: close connection, apply backpressure, or increase buffer size.
 */
int buffer_is_full(const buffer_t *buf);

/* Check if buffer is empty (nothing to write).
 * If true, we can deregister from EPOLLOUT to avoid busy-waiting.
 */
int buffer_is_empty(const buffer_t *buf);

/* Get number of bytes available for reading from buffer.
 * This is the amount of data waiting to be written to peer.
 */
size_t buffer_readable_bytes(const buffer_t *buf);

/* Get free space in buffer.
 * This is how much more data we can read from socket.
 */
size_t buffer_writable_bytes(const buffer_t *buf);

/* Compact buffer by moving unread data to the beginning.
 * Called after partial write to reclaim space.
 * 
 * Example:
 *   Buffer has: [xxxx____] where x = data, _ = empty
 *   After partial write of 2 bytes: [__xx____] (pos=2, len=4)
 *   After compact: [xx______] (pos=0, len=2)
 * 
 * This is a memmove(), which is fast for small buffers but could be
 * optimized with a ring buffer for large ones. Trade-off: simplicity wins.
 */
void buffer_compact(buffer_t *buf);

#endif /* BUFFER_H */