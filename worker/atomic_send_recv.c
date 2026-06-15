#define _GNU_SOURCE // struct mmsghdr
#include <errno.h>
#include <string.h>

#include "atomic_send_recv.h"
#include "../common/util.h"
#include "../libcrash/libcrash.h"

void double_buffer_append(struct double_buffer *buf, const char *tail, size_t size) {
    if(size > sizeof(ACTIVE_BUFFER(buf)->buf) - ACTIVE_BUFFER(buf)->used) {
        size = sizeof(ACTIVE_BUFFER(buf)->buf) - ACTIVE_BUFFER(buf)->used;
    }
    memcpy(
        buf->buffers[!buf->active].buf,
        ACTIVE_BUFFER(buf)->buf,
        ACTIVE_BUFFER(buf)->used
    );
    memcpy(
        buf->buffers[!buf->active].buf + ACTIVE_BUFFER(buf)->used,
        tail,
        size
    );
    buf->buffers[!buf->active].used = ACTIVE_BUFFER(buf)->used + size;
    LIBCRASH_HOOK(double_buffer_append(buf, !!buf->active));
    buf->active = !buf->active;
}

void double_buffer_lshift(struct double_buffer *buf, size_t size) {
    // If the data was shifted inside a single buffer and the memmove would
    // be interupted, that data would be corrupted. Instead the A/B buffers
    // are used and the data is copied from the active to to the inactive
    // buffer and only after that completed is the active buffer swapped.
    if(size >= ACTIVE_BUFFER(buf)->used) {
        buf->buffers[!buf->active].used = 0;
    } else {
        memcpy(
            buf->buffers[!buf->active].buf,
            ACTIVE_BUFFER(buf)->buf + size,
            ACTIVE_BUFFER(buf)->used - size
        );
        buf->buffers[!buf->active].used = ACTIVE_BUFFER(buf)->used - size;
    }
    LIBCRASH_HOOK(double_buffer_lshift(buf, !!buf->active));
    buf->active = !buf->active;
}

int atomic_send(int fd, struct double_buffer *buf) {
    struct iovec io;
    switch(buf->state) {
    default:
    case ATOMIC_INIT:
        // The following value will be overwritten once sendmmsg returned.
        // So as long as `state == SEND_SEND && mm.msg_len == -1` sendmmsg did
        // not complete.
        _Static_assert(
            (unsigned int)-1 > sizeof(ACTIVE_BUFFER(buf)->buf),
            "sentinel value cannot be returned by sendmmsg"
        );
        buf->mm.msg_len = -1;
        LIBCRASH_HOOK(atomic_send_prepare(fd, buf));
        buf->state = ATOMIC_DO_SYSCALL;
        __attribute__((fallthrough));
    case ATOMIC_DO_SYSCALL:
        // Pointers might be inconsistent.
        buf->mm.msg_hdr = (struct msghdr){
            .msg_iov = &io,
            .msg_iovlen = 1,
        };
        io = (struct iovec){
            .iov_base = ACTIVE_BUFFER(buf)->buf,
            .iov_len = ACTIVE_BUFFER(buf)->used,
        };
        if(buf->mm.msg_len == (unsigned int)-1) {
            LIBCRASH_HOOK(atomic_send_sendmmsg_pre(fd, buf));
            int rc = sendmmsg(fd, &buf->mm, 1, MSG_DONTWAIT);
            LIBCRASH_HOOK(atomic_send_sendmmsg_post(fd, buf, rc));
            if(rc < 0) {
                return -1;
            }
        }
        // The next part gets quite ugly, just remember `case` is just a `goto`.
        // So the if-else is only evaluated when coming from `ATOMIC_DO_SYSCALL`.
        // Only `buf->active = ...` is re-used by `ATOMIC_MEMCPY_*` and is
        // necessary if the active might have been swapped after memcpy
        // completed, but the state was not changed, which would not be
        // detected and the buffers would be swapped back on the next call.
        if(buf->active) {
            buf->state = ATOMIC_MEMCPY_1to0;
            __attribute__((fallthrough));
    case ATOMIC_MEMCPY_1to0:
            buf->active = 1;
        } else {
            buf->state = ATOMIC_MEMCPY_0to1;
            __attribute__((fallthrough));
    case ATOMIC_MEMCPY_0to1:
            buf->active = 0;
        }
        double_buffer_lshift(buf, buf->mm.msg_len);
        buf->state = ATOMIC_INIT;
        return 0;
    }
}

int atomic_recv(int fd, struct double_buffer *buf) {
    struct iovec io;
    switch(buf->state) {
    default:
    case ATOMIC_INIT:
        if(ACTIVE_BUFFER(buf)->used == sizeof(ACTIVE_BUFFER(buf)->buf)) {
            return 0;
        }
        // The following value will be overwritten once sendmmsg returned.
        // So as long as `state == RECV_RECV && mm.msg_len == -1` sendmmsg did
        // not complete.
        _Static_assert(
            (unsigned int)-1 > sizeof(ACTIVE_BUFFER(buf)->buf),
            "sentinel value cannot be returned by recvmmsg"
        );
        buf->mm.msg_len = -1;
        LIBCRASH_HOOK(atomic_recv_prepare(fd, buf));
        buf->state = ATOMIC_DO_SYSCALL;
        __attribute__((fallthrough));
    case ATOMIC_DO_SYSCALL:
        if(sizeof(ACTIVE_BUFFER(buf)->buf) < ACTIVE_BUFFER(buf)->used) {
            errno = EOVERFLOW;
            return -1;
        }
        // Pointers might be inconsistent.
        buf->mm.msg_hdr = (struct msghdr){
            .msg_iov = &io,
            .msg_iovlen = 1,
        };
        io = (struct iovec){
            .iov_base = ACTIVE_BUFFER(buf)->buf + ACTIVE_BUFFER(buf)->used,
            .iov_len = sizeof(ACTIVE_BUFFER(buf)->buf) - ACTIVE_BUFFER(buf)->used,
        };
        if(buf->mm.msg_len == (unsigned int)-1) {
            LIBCRASH_HOOK(atomic_recv_recvmmsg_pre(fd, buf));
            int rc = recvmmsg(fd, &buf->mm, 1, MSG_DONTWAIT, NULL);
            LIBCRASH_HOOK(atomic_recv_recvmmsg_post(fd, buf, rc));
            if(rc < 0) {
                return -1;
            }
        }
        // The next part gets quite ugly, just remember `case` is just a `goto`.
        // So the if-else is only evaluated when coming from `ATOMIC_DO_SYSCALL`.
        // Only `buf->active = ...` is re-used by `ATOMIC_MEMCPY_*` and is
        // necessary if the active might have been swapped after memcpy
        // completed, but the state was not changed yet. This would not be
        // detected and the buffers would be swapped back on the next call.
        if(buf->mm.msg_len == 0) {
            buf->state = ATOMIC_RECV_EOF;
            __attribute__((fallthrough));
    case ATOMIC_RECV_EOF:
            return 0;
        } else  if(buf->active) {
            buf->state = ATOMIC_MEMCPY_1to0;
            __attribute__((fallthrough));
    case ATOMIC_MEMCPY_1to0:
            buf->active = 1;
        } else {
            buf->state = ATOMIC_MEMCPY_0to1;
            __attribute__((fallthrough));
    case ATOMIC_MEMCPY_0to1:
            buf->active = 0;
        }
        double_buffer_append(
            buf,
            ACTIVE_BUFFER(buf)->buf + ACTIVE_BUFFER(buf)->used,
            buf->mm.msg_len
        );
        buf->state = ATOMIC_INIT;
        return 1;
    }
}
