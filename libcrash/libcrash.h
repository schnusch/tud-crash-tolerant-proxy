#ifndef LIBCRASH_H
#define LIBCRASH_H

#ifdef USE_LIBCRASH
#define LIBCRASH(x) libcrash_##x
#else
#define LIBCRASH(x)
#endif

#include "../worker/atomic_send_recv.h"

/**
 * Bit flags that describe which error injection is active.
 */
enum libcrash_injected_error {
    // Listener
    CRASH_ACCEPT_POST = 1 << 0,
    CRASH_CONNECT_POST = 1 << 1,
    // Worker.
    CRASH_ATOMIC_RECV_PREPARE = 1 << 2,
    CRASH_ATOMIC_RECV_RECVMMSG_PRE = 1 << 3,
    CRASH_ATOMIC_RECV_RECVMMSG_POST = 1 << 4,
    CRASH_ATOMIC_RING_BUFFER_LTRIM = 1 << 5,
    CRASH_ATOMIC_RING_BUFFER_APPEND = 1 << 6,
    CRASH_ATOMIC_SEND_PREPARE = 1 << 7,
    CRASH_ATOMIC_SEND_SENDMMSG_PRE = 1 << 8,
    CRASH_ATOMIC_SEND_SENDMMSG_POST = 1 << 9,
};

int libcrash_init(int signal);


void libcrash_accept_post(int fd, const struct sockaddr *addr, socklen_t len);

void libcrash_connect_post(int fd, const struct sockaddr *addr, socklen_t len);


void libcrash_atomic_send_prepare(int fd, struct atomic_ring_buffer *buf);

void libcrash_atomic_send_sendmmsg_pre(int fd, struct atomic_ring_buffer *buf);

void libcrash_atomic_send_sendmmsg_post(int fd, struct atomic_ring_buffer *buf, int *rc);

void libcrash_atomic_ring_buffer_ltrim(struct atomic_ring_buffer *buf, size_t active);


void libcrash_atomic_recv_prepare(int fd, struct atomic_ring_buffer *buf);

void libcrash_atomic_recv_recvmmsg_pre(int fd, struct atomic_ring_buffer *buf);

void libcrash_atomic_recv_recvmmsg_post(int fd, struct atomic_ring_buffer *buf, int *rc);

void libcrash_atomic_ring_buffer_append(struct atomic_ring_buffer *buf, size_t active);

#endif
