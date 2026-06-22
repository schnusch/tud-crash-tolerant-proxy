#define _GNU_SOURCE // struct mmsghdr
#include <errno.h>
#include <string.h>

#include "atomic_send_recv.h"
#include "../common/util.h"
#include "../libcrash/libcrash.h"

void atomic_ring_buffer_append(struct atomic_ring_buffer *buf, const char *tail, size_t size) {
    if(size > sizeof(buf->buf) - ACTIVE_RANGE(buf)->len) {
        size = sizeof(buf->buf) - ACTIVE_RANGE(buf)->len;
    }
    const size_t end = ACTIVE_RANGE(buf)->start - ACTIVE_RANGE(buf)->len;
    const size_t space_after = sizeof(buf->buf) - end;
    memcpy(
        buf->buf + end,
        tail,
        size > space_after ? space_after : size
    );
    if(size > space_after) {
        memcpy(
            buf->buf,
            tail + space_after,
            size - space_after
        );
    }
    buf->ranges[!buf->active].start = ACTIVE_RANGE(buf)->start;
    buf->ranges[!buf->active].len = ACTIVE_RANGE(buf)->len + size;
    LIBCRASH_HOOK(atomic_ring_buffer_append(buf, !!buf->active));
    atomic_store_explicit(&buf->active, !buf->active, memory_order_release);
}

void atomic_ring_buffer_ltrim(struct atomic_ring_buffer *buf, size_t size) {
    if(size >= ACTIVE_RANGE(buf)->len) {
        buf->ranges[!buf->active].start = 0;
        buf->ranges[!buf->active].len = 0;
    } else {
        buf->ranges[!buf->active].start = (ACTIVE_RANGE(buf)->start + size) % sizeof(buf->buf);
        buf->ranges[!buf->active].len = ACTIVE_RANGE(buf)->len - size;
    }
    LIBCRASH_HOOK(atomic_ring_buffer_ltrim(buf, !!buf->active));
    atomic_store_explicit(&buf->active, !buf->active, memory_order_release);
}

/**
 * Initialize one or two `struct iovec` pointing to the parts of the
 * `atomic_ring_buffer::buffer` occupied by the currently active range.
 * \return `msghdr::msg_iovlen`
 */
size_t set_iovec_used(struct iovec iov[2], struct atomic_ring_buffer *buf) {
    struct iovec *pv = iov;
    const size_t end = ACTIVE_RANGE(buf)->start + ACTIVE_RANGE(buf)->len;
    if(end <= sizeof(buf->buf)) {
        // [            |============|            ]
        //               iov[0]
        *pv++ = (struct iovec){
            .iov_base = buf->buf + ACTIVE_RANGE(buf)->start,
            .iov_len = ACTIVE_RANGE(buf)->len,
        };
    } else {
        // [============|            |============]
        //  iov[1]          unused    iov[0]
        size_t const until_end = sizeof(buf->buf) - ACTIVE_RANGE(buf)->start;
        *pv++ = (struct iovec){
            .iov_base = buf->buf + ACTIVE_RANGE(buf)->start,
            .iov_len = until_end,
        };
        *pv++ = (struct iovec){
            .iov_base = buf->buf,
            .iov_len = ACTIVE_RANGE(buf)->len - until_end,
        };
    }
    return pv - iov;
}

/**
 * Initialize one or two `struct iovec` pointing to the parts of the
 * `atomic_ring_buffer::buffer` not occupied by the currently active range.
 * \return `msghdr::msg_iovlen`
 */
size_t set_iovec_unused(struct iovec iov[2], struct atomic_ring_buffer *buf) {
    struct iovec *pv = iov;
    if(ACTIVE_RANGE(buf)->len == 0) {
        ACTIVE_RANGE(buf)->start = 0;
    }
    // Point the iovecs to the buffer before and after the currently used
    // range.
    const size_t end = ACTIVE_RANGE(buf)->start + ACTIVE_RANGE(buf)->len;
    if(ACTIVE_RANGE(buf)->start == 0) {
        // [============|                         ]
        //      used     iov[0]
        *pv++ = (struct iovec){
            .iov_base = buf->buf + ACTIVE_RANGE(buf)->len,
            .iov_len = sizeof(buf->buf) - ACTIVE_RANGE(buf)->len,
        };
    } else if(end >= sizeof(buf->buf)) {
        // [==|                        |==========]
        //     iov[0]                       used
        const size_t skip = end - sizeof(buf->buf);
        *pv++ = (struct iovec){
            .iov_base = buf->buf + skip,
            .iov_len = ACTIVE_RANGE(buf)->start - skip,
        };
    } else {
        // [            |============|            ]
        //  iov[1]           used     iov[0]
        *pv++ = (struct iovec){
            .iov_base = buf->buf + end,
            .iov_len = sizeof(buf->buf) - end,
        };
        *pv++ = (struct iovec){
            .iov_base = buf->buf,
            .iov_len = ACTIVE_RANGE(buf)->start,
        };
    }
    return pv - iov;
}

int atomic_send(int fd, struct atomic_ring_buffer *buf) {
    if(ACTIVE_RANGE(buf)->len == 0) {
        return 0;
    }

    struct iovec iov[2];
    switch(buf->state) {
    default:
    case ATOMIC_INIT:
        // The following value will be overwritten once sendmmsg returned.
        // So as long as `state == ATOMIC_DO_SYSCALL && mm.msg_len == -1`
        // sendmmsg did not complete.
        _Static_assert(
            (unsigned int)-1 > sizeof(buf->buf),
            "sentinel value cannot be returned by sendmmsg"
        );
        buf->mm.msg_len = -1;
        LIBCRASH_HOOK(atomic_send_prepare(fd, buf));
        atomic_store_explicit(&buf->state, ATOMIC_DO_SYSCALL, memory_order_release);
        __attribute__((fallthrough));
    case ATOMIC_DO_SYSCALL:
        if(buf->mm.msg_len == (unsigned int)-1) {
            buf->mm.msg_hdr = (struct msghdr){
                .msg_iov = iov,
                .msg_iovlen = set_iovec_used(iov, buf),
            };
            LIBCRASH_HOOK(atomic_send_sendmmsg_pre(fd, buf));
            int rc = sendmmsg(fd, &buf->mm, 1, MSG_DONTWAIT);
            LIBCRASH_HOOK(atomic_send_sendmmsg_post(fd, buf, rc));
            if(rc < 0) {
                return -1;
            }
        }
        // The next part gets quite ugly, just remember `case` is just a `goto`.
        // So the if-else is only evaluated when coming from `ATOMIC_DO_SYSCALL`.
        // Only `buf->active = ...` is re-used by `ATOMIC_SWAP_*` and is
        // necessary if the active might have been swapped after memcpy
        // completed, but the state was not changed, which would not be
        // detected and the buffers would be swapped back on the next call.
        if(buf->active) {
            atomic_store_explicit(&buf->state, ATOMIC_SWAP_1to0, memory_order_release);
            __attribute__((fallthrough));
    case ATOMIC_SWAP_1to0:
            buf->active = 1;
        } else {
            atomic_store_explicit(&buf->state, ATOMIC_SWAP_0to1, memory_order_release);
            __attribute__((fallthrough));
    case ATOMIC_SWAP_0to1:
            buf->active = 0;
        }
        atomic_ring_buffer_ltrim(buf, buf->mm.msg_len);
        atomic_store_explicit(&buf->state, ATOMIC_INIT, memory_order_release);
        return buf->mm.msg_len;
    }
}

int atomic_recv(int fd, struct atomic_ring_buffer *buf) {
    struct iovec iov[2];
    switch(buf->state) {
    default:
    case ATOMIC_INIT:
        if(ACTIVE_RANGE(buf)->len >= sizeof(buf->buf)) {
            errno = EOVERFLOW;
            return -1;
        }
        // The following value will be overwritten once sendmmsg returned.
        // So as long as `state == ATOMIC_DO_SYSCALL && mm.msg_len == -1`
        // recvmmsg did not complete.
        _Static_assert(
            (unsigned int)-1 > sizeof(buf->buf),
            "sentinel value cannot be returned by recvmmsg"
        );
        buf->mm.msg_len = -1;
        LIBCRASH_HOOK(atomic_recv_prepare(fd, buf));
        atomic_store_explicit(&buf->state, ATOMIC_DO_SYSCALL, memory_order_release);
        __attribute__((fallthrough));
    case ATOMIC_DO_SYSCALL:
        if(buf->mm.msg_len == (unsigned int)-1) {
            buf->mm.msg_hdr = (struct msghdr){
                .msg_iov = iov,
                .msg_iovlen = set_iovec_unused(iov, buf),
            };
            LIBCRASH_HOOK(atomic_recv_recvmmsg_pre(fd, buf));
            int rc = recvmmsg(fd, &buf->mm, 1, MSG_DONTWAIT, NULL);
            LIBCRASH_HOOK(atomic_recv_recvmmsg_post(fd, buf, rc));
            if(rc < 0) {
                // !!! WARNING !!!
                // If the process were to crash here, the error can get lost.
                // E.g. ECONNRESET is returned only once, afterwards recv(2)
                // will signal EOF.
                return -1;
            }
        }
        // The next part gets quite ugly, just remember `case` is just a `goto`.
        // So the if-else is only evaluated when coming from `ATOMIC_DO_SYSCALL`.
        // Only `buf->active = ...` is re-used by `ATOMIC_SWAP_*` and is
        // necessary if the active might have been swapped after memcpy
        // completed, but the state was not changed yet. This would not be
        // detected and the buffers would be swapped back on the next call.
        if(buf->mm.msg_len == 0) {
            atomic_store_explicit(&buf->state, ATOMIC_INIT, memory_order_release);
            return 0;
        } else  if(buf->active) {
            atomic_store_explicit(&buf->state, ATOMIC_SWAP_1to0, memory_order_release);
            __attribute__((fallthrough));
    case ATOMIC_SWAP_1to0:
            buf->active = 1;
        } else {
            atomic_store_explicit(&buf->state, ATOMIC_SWAP_0to1, memory_order_release);
            __attribute__((fallthrough));
    case ATOMIC_SWAP_0to1:
            buf->active = 0;
        }
        buf->ranges[!buf->active].len = ACTIVE_RANGE(buf)->len + buf->mm.msg_len;
        LIBCRASH_HOOK(atomic_ring_buffer_append(buf, !!buf->active));
        buf->active = !buf->active;
        atomic_store_explicit(&buf->state, ATOMIC_INIT, memory_order_release);
        return buf->mm.msg_len;
    }
}
