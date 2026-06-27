#ifndef WORKER_ATOMIC_SEND_RECV_H
#define WORKER_ATOMIC_SEND_RECV_H

#ifdef __cplusplus
 #define atomic_int int
 #define atomic_bool bool
#else
 #include <stdatomic.h>
#endif
#include <sys/socket.h>

/**
 * State machine of `atomic_send` and `atomic_recv`. If the process crashes
 * during the function calls, the progress can be restored as long as the
 * `struct atomic_ring_buffer` can be recovered.
 */
enum {
    /**
     * Initialize the ring buffer for the syscall. (All initialization is
     * relocatable.)
     */
    ATOMIC_INIT = 0,
    /** Perform the syscall next. */
    ATOMIC_DO_SYSCALL = 1,
    /**
     * The syscall was performed but the ring buffer ranges were not yet
     * updated. (The currently active range is encoded in `(1 << 7)`.)
     */
    ATOMIC_SWAP = 0x7F,
};

/**
 * Range of used bytes in `atomic_ring_buffer::buf`.
 */
struct ring_buffer_range {
    /** Offset in `atomic_ring_buffer::buf`. */
    size_t start;
    /** Number of bytes used in `atomic_ring_buffer::buf`. */
    size_t len;
};

/**
 * Number of bytes in `atomic_ring_buffer::buf`.
 */
#define RING_BUFFER_SIZE 8192

/**
 * Ring buffer that can appended to and trimmed atomically. Each buffer has two
 * `ring_buffer_range`. Operations take place on the inactive range and then
 * `active` is set atomically to switch the active range.
 */
struct atomic_ring_buffer {
    /**
     * State machine around `sendmmsg(2)` and `recvmmsg(2)`.
     */
    atomic_int state;
    /**
     * Struct `sendmmsg(2)` and `recvmmsg(2)` operate on.
     */
    struct mmsghdr mm;
    /**
     * Index of `atomic_ring_buffer::ranges`.
     */
    atomic_bool active;
    /**
     * Operation take place on the same buffer but on the inactive range. Only
     * after the operation completes the active and inactive ranges are swapped.
     */
    struct ring_buffer_range ranges[2];
    /**
     * Buffer shared by the two `atomic_ring_buffer::ranges`.
     */
    char buf[RING_BUFFER_SIZE];
};

/**
 * Initialization value of `struct atomic_ring_buffer`.
 */
#define ATOMIC_RING_BUFFER_INIT \
    ((const struct atomic_ring_buffer){ \
        .state = ATOMIC_INIT, \
        .active = 0, \
        .ranges = { \
            { .start = 0, .len = 0 }, \
            { .start = 0, .len = 0 }, \
        }, \
    })

/**
 * Return the currently active `struct ring_buffer_range` of the buffer.
 * \param b `struct atomic_ring_buffer *`
 * \return `struct ring_buffer_range *`
 */
#define ACTIVE_RANGE(b) (&(b)->ranges[!!(b)->active])

/**
 * Append `size` bytes from `tail` to `buf`. If there are less than `size` free
 * bytes in `buf`, `tail` is only copied partially.
 */
size_t ring_buffer_append(
    char buf[RING_BUFFER_SIZE],
    struct ring_buffer_range *range,
    const char *tail,
    size_t size
);

/**
 * Drop the first `size` bytes from the range.
 */
void ring_buffer_ltrim(struct ring_buffer_range *range, size_t size);

/**
 * Send as much data out of `fd` as possible without blocking and trim sent data
 * from `buf`.
 * \return -1 on error
 * \return >0 the number of bytes sent and trimmed from `buf`
 */
int atomic_send(int fd, struct atomic_ring_buffer *buf);

/**
 * Receive as much data from `fd` as possible without blocking and append to
 * `buf`.
 * \return -1 on error
 * \return 0 on EOF
 * \return >0 the number of bytes received and appended to `buf`
 */
int atomic_recv(int fd, struct atomic_ring_buffer *buf);

#endif
