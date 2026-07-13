#include <errno.h>
#include <string.h>

#include "atomic_send_recv.h"
#include "../libcrash/libcrash.h"

/**
 * Initialize one or two `struct iovec` pointing to the parts of the
 * `atomic_ring_buffer::buffer` occupied by the currently active range.
 * \return `msghdr::msg_iovlen`
 */
size_t set_iovec_used(
    struct iovec iov[2],
    const char buf[RING_BUFFER_SIZE],
    struct ring_buffer_range *range
) {
    struct iovec *pv = iov;
    const size_t end = range->start + range->len;
    if(end <= RING_BUFFER_SIZE) {
        // [            |============|            ]
        //               iov[0]
        *pv++ = (struct iovec){
            .iov_base = buf + range->start,
            .iov_len = range->len,
        };
    } else {
        // [============|            |============]
        //  iov[1]          unused    iov[0]
        size_t const until_end = RING_BUFFER_SIZE - range->start;
        *pv++ = (struct iovec){
            .iov_base = buf + range->start,
            .iov_len = until_end,
        };
        *pv++ = (struct iovec){
            .iov_base = buf,
            .iov_len = range->len - until_end,
        };
    }
    return pv - iov;
}

/**
 * Initialize one or two `struct iovec` pointing to the parts of the
 * `atomic_ring_buffer::buffer` not occupied by the currently active range.
 * \return `msghdr::msg_iovlen`
 */
size_t set_iovec_unused(
    struct iovec iov[2],
    char buf[RING_BUFFER_SIZE],
    struct ring_buffer_range *range
) {
    struct iovec *pv = iov;
    if(range->len == 0) {
        range->start = 0;
    }
    // Point the iovecs to the buffer before and after the currently used
    // range.
    const size_t end = range->start + range->len;
    if(range->start == 0) {
        // [============|                         ]
        //      used     iov[0]
        *pv++ = (struct iovec){
            .iov_base = buf + range->len,
            .iov_len = RING_BUFFER_SIZE - range->len,
        };
    } else if(end >= RING_BUFFER_SIZE) {
        // [==|                        |==========]
        //     iov[0]                       used
        const size_t skip = end - RING_BUFFER_SIZE;
        *pv++ = (struct iovec){
            .iov_base = buf + skip,
            .iov_len = range->start - skip,
        };
    } else {
        // [            |============|            ]
        //  iov[1]           used     iov[0]
        *pv++ = (struct iovec){
            .iov_base = buf + end,
            .iov_len = RING_BUFFER_SIZE - end,
        };
        *pv++ = (struct iovec){
            .iov_base = buf,
            .iov_len = range->start,
        };
    }
    return pv - iov;
}

size_t ring_buffer_append(
    char buf[RING_BUFFER_SIZE],
    struct ring_buffer_range *range,
    const char *tail,
    size_t size
) {
    size_t copied = 0;
    struct iovec iov[2];
    for(size_t i = 0, n = set_iovec_unused(iov, buf, range); i < n && size > 0; ++i) {
        if(size < iov[i].iov_len) {
            iov[i].iov_len = size;
        }
        memcpy(iov[i].iov_base, tail, iov[i].iov_len);
        tail += iov[i].iov_len;
        size -= iov[i].iov_len;
        copied += iov[i].iov_len;
    }
    return copied;
}

void ring_buffer_ltrim(struct ring_buffer_range *range, size_t size) {
    if(size >= range->len) {
        range->start = 0;
        range->len = 0;
    } else {
        range->start = (range->start + size) % RING_BUFFER_SIZE;
        range->len -= size;
    }
}

size_t ring_buffer_move(
    char dst_buf[RING_BUFFER_SIZE],
    struct ring_buffer_range *dst_range,
    const char src_buf[RING_BUFFER_SIZE],
    struct ring_buffer_range *src_range
) {
    struct iovec v_dst[2];
    const size_t n_dst = set_iovec_unused(v_dst, dst_buf, dst_range);
    size_t i_dst = 0;

    struct iovec v_src[2];
    const size_t n_src = set_iovec_used(v_src, src_buf, src_range);
    size_t i_src = 0;

    size_t copied = 0;
    while(i_dst < n_dst && i_src < n_src) {
#define MIN(a, b) ((a) <= (b) ? (a) : (b))
        const size_t min = MIN(v_dst[i_dst].iov_len, v_src[i_src].iov_len);
#undef MIN
        memcpy(v_dst[i_dst].iov_base, v_src[i_src].iov_base, min);
        copied += min;

        v_dst[i_dst].iov_base = (char *)v_dst[i_dst].iov_base + min;
        v_dst[i_dst].iov_len -= min;
        i_dst += (v_dst[i_dst].iov_len == 0);

        v_src[i_src].iov_base = (char *)v_src[i_src].iov_base + min;
        v_src[i_src].iov_len -= min;
        i_src += (v_src[i_src].iov_len == 0);
    }
    dst_range->len += copied;
    ring_buffer_ltrim(src_range, copied);
    return copied;
}

#ifdef PERFORMANCE_BASELINE
int atomic_send(int fd, struct atomic_ring_buffer *buf) {
    struct iovec iov[2];
    ssize_t n = sendmsg(
        fd,
        &(struct msghdr){
            .msg_iov = iov,
            .msg_iovlen = set_iovec_used(iov, buf->buf, ACTIVE_RANGE(buf)),
        },
        MSG_DONTWAIT
    );
    if(n > 0) {
        ring_buffer_ltrim(ACTIVE_RANGE(buf), n);
    }
    return n;
}
#else
int atomic_send(int fd, struct atomic_ring_buffer *buf) {
    if(ACTIVE_RANGE(buf)->len == 0) {
        return 0;
    }

    struct iovec iov[2];
    int state = atomic_load_explicit(&buf->state, memory_order_acquire);
    switch(state & ATOMIC_SWAP) {
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
        atomic_store_explicit(
            &buf->state,
            state = ATOMIC_DO_SYSCALL,
            memory_order_release
        );
        __attribute__((fallthrough));
    case ATOMIC_DO_SYSCALL:
        if(buf->mm.msg_len == (unsigned int)-1) {
            buf->mm.msg_hdr = (struct msghdr){
                .msg_iov = iov,
                .msg_iovlen = set_iovec_used(iov, buf->buf, ACTIVE_RANGE(buf)),
            };
            LIBCRASH_HOOK(atomic_send_sendmmsg_pre(fd, buf));
            int rc = sendmmsg(fd, &buf->mm, 1, MSG_DONTWAIT);
            LIBCRASH_HOOK(atomic_send_sendmmsg_post(fd, buf, &rc));
            if(rc < 0) {
                return -1;
            }
        }
        // Encode the active range in the state so can be restored on crash.
        atomic_store_explicit(
            &buf->state,
            state = ATOMIC_SWAP | (!!buf->active << 7),
            memory_order_release
        );
        __attribute__((fallthrough));
    case ATOMIC_SWAP:
        // Left-trim the sent data on the inactive range, then swap the ranges.
        buf->active = !!(state & (1 << 7));
        *INACTIVE_RANGE(buf) = *ACTIVE_RANGE(buf);
        ring_buffer_ltrim(INACTIVE_RANGE(buf), buf->mm.msg_len);
        LIBCRASH_HOOK(atomic_ring_buffer_ltrim(buf, !!buf->active));
        buf->active = !buf->active;
        atomic_store_explicit(
            &buf->state,
            state = ATOMIC_INIT,
            memory_order_release
        );
        return buf->mm.msg_len;
    }
}
#endif

#ifdef PERFORMANCE_BASELINE
int atomic_recv(int fd, struct atomic_ring_buffer *buf) {
    struct iovec iov[2];
    ssize_t n = recvmsg(
        fd,
        &(struct msghdr){
            .msg_iov = iov,
            .msg_iovlen = set_iovec_unused(iov, buf->buf, ACTIVE_RANGE(buf)),
        },
        MSG_DONTWAIT
    );
    if(n > 0) {
        ACTIVE_RANGE(buf)->len += n;
    }
    return n;
}
#else
int atomic_recv(int fd, struct atomic_ring_buffer *buf) {
    struct iovec iov[2];
    int state = atomic_load_explicit(&buf->state, memory_order_acquire);
    switch(state & ATOMIC_SWAP) {
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
        atomic_store_explicit(
            &buf->state,
            state = ATOMIC_DO_SYSCALL,
            memory_order_release
        );
        __attribute__((fallthrough));
    case ATOMIC_DO_SYSCALL:
        if(buf->mm.msg_len == (unsigned int)-1) {
            buf->mm.msg_hdr = (struct msghdr){
                .msg_iov = iov,
                .msg_iovlen = set_iovec_unused(iov, buf->buf, ACTIVE_RANGE(buf)),
            };
            LIBCRASH_HOOK(atomic_recv_recvmmsg_pre(fd, buf));
            int rc = recvmmsg(fd, &buf->mm, 1, MSG_DONTWAIT, NULL);
            LIBCRASH_HOOK(atomic_recv_recvmmsg_post(fd, buf, &rc));
            if(rc < 0) {
                // !!! WARNING !!!
                // If the process were to crash here, the error can get lost.
                // E.g. ECONNRESET is returned only once, afterwards recv(2)
                // will signal EOF.
                return -1;
            }
        }
        if(buf->mm.msg_len == 0) {
            atomic_store_explicit(&buf->state, ATOMIC_INIT, memory_order_release);
            return 0;
        }
        // Encode the inactive range in the state so can be restored on crash.
        atomic_store_explicit(
            &buf->state,
            state = ATOMIC_SWAP | (!buf->active << 7),
            memory_order_release
        );
        __attribute__((fallthrough));
    case ATOMIC_SWAP:
        // Extend the inactive range to include the received data.
        buf->active = !(state & (1 << 7));
        *INACTIVE_RANGE(buf) = (struct ring_buffer_range){
            .start = ACTIVE_RANGE(buf)->start,
            .len = ACTIVE_RANGE(buf)->len + buf->mm.msg_len,
        };
        LIBCRASH_HOOK(atomic_ring_buffer_append(buf, !!buf->active));
        buf->active = !buf->active;
        atomic_store_explicit(
            &buf->state,
            state = ATOMIC_INIT,
            memory_order_release
        );
        return buf->mm.msg_len;
    }
}
#endif
