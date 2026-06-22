#ifndef WORKER_ATOMIC_SEND_RECV_H
#define WORKER_ATOMIC_SEND_RECV_H

#ifdef __cplusplus
 #define atomic_int int
 #define atomic_bool bool
#else
 #include <stdatomic.h>
#endif
#include <sys/socket.h>

enum {
    ATOMIC_INIT = 0,
    ATOMIC_DO_SYSCALL,
    ATOMIC_SWAP_0to1,
    ATOMIC_SWAP_1to0,
    ATOMIC_RECV_EOF,
};

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
    struct {
        size_t start;
        size_t len;
    } ranges[2];
    /**
     * Buffer shared by the two `atomic_ring_buffer::ranges`.
     */
    char buf[8192];
};

#define ATOMIC_RING_BUFFER_INIT \
    ((const struct atomic_ring_buffer){ \
        .state = ATOMIC_INIT, \
        .active = 0, \
        .ranges = { \
            { .start = 0, .len = 0 }, \
            { .start = 0, .len = 0 }, \
        }, \
    })

#define ACTIVE_RANGE(b) (&(b)->ranges[!!(b)->active])

/**
 * Append `size` bytes from `tail` to `buf`. If there are less than `size` free
 * bytes in `buf`, `tail` is only copied partially.
 */
void atomic_ring_buffer_append(struct atomic_ring_buffer *buf, const char *tail, size_t size);

/**
 * Drop the first `size` bytes from the ring buffer.
 */
void atomic_ring_buffer_ltrim(struct atomic_ring_buffer *buf, size_t size);

/**
 * \return -1 on error
 * \return 0 on success
 */
int atomic_send(int fd, struct atomic_ring_buffer *buf);

/**
 * \return -1 on error
 * \return 0 on EOF
 * \return 1 regardless of how much data was received
 */
int atomic_recv(int fd, struct atomic_ring_buffer *buf);

#endif
