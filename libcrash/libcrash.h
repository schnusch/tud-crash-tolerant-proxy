#ifndef LIBCRASH_H
#define LIBCRASH_H

#ifdef USE_LIBCRASH
#define LIBCRASH_HOOK(x) libcrash_##x
#else
#define LIBCRASH_HOOK(x)
#endif

#include <sys/socket.h>

void libcrash_atomic_send_prepare(int fd, struct double_buffer *buf);

void libcrash_atomic_send_sendmmsg_pre(int fd, struct double_buffer *buf);

void libcrash_atomic_send_sendmmsg_post(int fd, struct double_buffer *buf, int rc);

void libcrash_double_buffer_lshift(struct double_buffer *buf, size_t active);


void libcrash_atomic_recv_prepare(int fd, struct double_buffer *buf);

void libcrash_atomic_recv_recvmmsg_pre(int fd, struct double_buffer *buf);

void libcrash_atomic_recv_recvmmsg_post(int fd, struct double_buffer *buf, int rc);

void libcrash_double_buffer_append(struct double_buffer *buf, size_t active);

#endif
