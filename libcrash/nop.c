#include "libcrash.h"

void libcrash_accept(int fd, const struct sockaddr *addr, socklen_t len) {
    (void)fd, (void)addr, (void)len;
}

void libcrash_atomic_send_prepare(int fd, struct atomic_ring_buffer *buf) {
    (void)fd, (void)buf;
}

void libcrash_atomic_send_sendmmsg_pre(int fd, struct atomic_ring_buffer *buf) {
    (void)fd, (void)buf;
}

void libcrash_atomic_send_sendmmsg_post(int fd, struct atomic_ring_buffer *buf, int *rc) {
    (void)fd, (void)buf, (void)rc;
}

void libcrash_atomic_ring_buffer_ltrim(struct atomic_ring_buffer *buf, size_t active) {
    (void)buf, (void)active;
}

void libcrash_atomic_recv_prepare(int fd, struct atomic_ring_buffer *buf) {
    (void)fd, (void)buf;
}

void libcrash_atomic_recv_recvmmsg_pre(int fd, struct atomic_ring_buffer *buf) {
    (void)fd, (void)buf;
}

void libcrash_atomic_recv_recvmmsg_post(int fd, struct atomic_ring_buffer *buf, int *rc) {
    (void)fd, (void)buf, (void)rc;
}

void libcrash_atomic_ring_buffer_append(struct atomic_ring_buffer *buf, size_t active) {
    (void)buf, (void)active;
}
