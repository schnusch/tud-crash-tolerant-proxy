#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "libcrash.h"

static sigset_t set;
static int where = 0;

int libcrash_init(int signal) {
    sigemptyset(&set);
    sigaddset(&set, signal);
    where = 0;

    if(sigprocmask(SIG_BLOCK, &set, NULL) < 0) {
        perror("sigprocmask(SIG_BLOCK)");
        return -1;
    }
    return 0;
}

static int pending_signal(void) {
    siginfo_t info;
    int sig;
    do {
        sig = sigtimedwait(
            &set,
            &info,
            &(const struct timespec){
                .tv_sec = 0,
                .tv_nsec = 0,
            }
        );
    } while(sig < 0 && errno == EINTR);
    if(sig < 0) {
        if(errno != EAGAIN) {
            perror("sigtimedwait");
        }
    } else if(info.si_code == SI_QUEUE) {
        return info.si_value.sival_int;
    }
    return 0;
}

static void exit_if(enum libcrash_injected_error flag) {
    where |= pending_signal();
    if(where & flag) {
        _exit(3);
    }
}

void libcrash_accept_post(int fd, const struct sockaddr *addr, socklen_t len) {
    (void)fd, (void)addr, (void)len;
    exit_if(CRASH_ACCEPT_POST);
}

void libcrash_connect_post(int fd, const struct sockaddr *addr, socklen_t len) {
    (void)fd, (void)addr, (void)len;
    exit_if(CRASH_CONNECT_POST);
}

void libcrash_atomic_send_prepare(int fd, struct atomic_ring_buffer *buf) {
    (void)fd, (void)buf;
    exit_if(CRASH_ATOMIC_SEND_PREPARE);
}

void libcrash_atomic_send_sendmmsg_pre(int fd, struct atomic_ring_buffer *buf) {
    (void)fd, (void)buf;
    exit_if(CRASH_ATOMIC_SEND_SENDMMSG_PRE);
}

void libcrash_atomic_send_sendmmsg_post(int fd, struct atomic_ring_buffer *buf, int *rc) {
    (void)fd, (void)buf, (void)rc;
    exit_if(CRASH_ATOMIC_SEND_SENDMMSG_POST);
}

void libcrash_atomic_ring_buffer_ltrim(struct atomic_ring_buffer *buf, size_t active) {
    (void)buf, (void)active;
    exit_if(CRASH_ATOMIC_RING_BUFFER_LTRIM);
}

void libcrash_atomic_recv_prepare(int fd, struct atomic_ring_buffer *buf) {
    (void)fd, (void)buf;
    exit_if(CRASH_ATOMIC_RECV_PREPARE);
}

void libcrash_atomic_recv_recvmmsg_pre(int fd, struct atomic_ring_buffer *buf) {
    (void)fd, (void)buf;
    exit_if(CRASH_ATOMIC_RECV_RECVMMSG_PRE);
}

void libcrash_atomic_recv_recvmmsg_post(int fd, struct atomic_ring_buffer *buf, int *rc) {
    (void)fd, (void)buf, (void)rc;
    exit_if(CRASH_ATOMIC_RECV_RECVMMSG_POST);
}

void libcrash_atomic_ring_buffer_append(struct atomic_ring_buffer *buf, size_t active) {
    (void)buf, (void)active;
    exit_if(CRASH_ATOMIC_RING_BUFFER_APPEND);
}
