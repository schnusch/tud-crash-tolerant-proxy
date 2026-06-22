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

struct double_buffer {
    /**
     * State machine around `sendmmsg(2)` and `recvmmsg(2)`.
     */
    atomic_int state;
    struct mmsghdr mm;
    /**
     * Index of `double_buffer::buffers`.
     */
    atomic_bool active;
    /**
     * Every operation takes places on the inactive buffer and after it
     * completes the active and inactive buffer are swapped.
     */
    struct {
        size_t used;
        char buf[8192];
    } buffers[2];
};

#define DOUBLE_BUFFER_INIT \
    ((const struct double_buffer){ \
        .state = ATOMIC_INIT, \
        .active = 0, \
        .buffers = { \
            { .used = 0 }, \
            { .used = 0 }, \
        }, \
    })

#define ACTIVE_BUFFER(b) (&(b)->buffers[!!(b)->active])

void double_buffer_append(struct double_buffer *buf, const char *tail, size_t size);

void double_buffer_lshift(struct double_buffer *buf, size_t size);

int atomic_send(int fd, struct double_buffer *buf);

/**
 * \return -1 on error
 * \return 0 on EOF
 * \return 1 regardless of how much data was received
 */
int atomic_recv(int fd, struct double_buffer *buf);

#endif
