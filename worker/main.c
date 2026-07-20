#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "atomic_send_recv.h"
#include "cmdline.h"
#include "fd_info.h"
#include "../common/accept.h"
#include "../common/ipc.h"
#include "../common/shared_memory.h"
#include "../common/util.h"

/**
 * Convert string representation of `connection::state`.
 * \param buf   buffer to write string representation to
 * \param size  size of `buf`
 * \param state `connection::state`
 * \return `buf`
 */
static char *str_state(char *buf, size_t size, int state) {
    char *const end = buf + size;
    char *p = buf;
    const char *src;
    switch(state & CONN_STATE_BITS) {
#define CASE(x) case x: src = #x; break;
        CASE(CONN_UNUSED)
        CASE(CONN_ACCEPTING)
        CASE(CONN_CONNECTING)
        CASE(CONN_POLL)
        CASE(CONN_SWAP_BUFFERS)
        CASE(CONN_CLOSING)
        CASE(CONN_ERROR)
#undef CASE
    default:
        goto more;
    }
    p += strlcpy(p, src, end - p);
    if(p >= end) {
trunc:
        if(size >= 4) {
            p = end - 4;
        } else {
            p = buf;
        }
        strlcpy(p, "...", end - p);
        return buf;
    }
    if((state & ~CONN_STATE_BITS) == 0) {
        return buf;
    }
    state &= ~CONN_SWAP_BUFFERS;
    p += strlcpy(p, " | ", end - p);
    if(p >= end) {
        goto trunc;
    }
more:
    ;
    int n = snprintf(p, end - p, "0x%03X", state);
    if(n < 0 || end - p <= n) {
        goto trunc;
    }
    return buf;
}

/**
 * Change `*state` and log the change.
 * \param slot      only used for logging
 * \param state     pointer to `connection::state`
 * \param new_state new state
 */
static void change_state(size_t slot, atomic_int *state, int new_state) {
    char old[512], new[512];
    LOG(
        LOG_DEBUG,
        "slot=%zu state: %s => %s\n",
        slot,
        str_state(old, sizeof(old), atomic_load_explicit(state, memory_order_relaxed)),
        str_state(new, sizeof(new), new_state)
    );
    atomic_store_explicit(state, new_state, memory_order_release);
}

/**
 * Global variables.
 */
struct context {
    /** epoll file descriptor. */
    int epfd;
    /** IPC socket to communicate to the listener 1:1. */
    int ipc_fd;
    /** Memory mapping of the shared memory. */
    struct shared_memory_mapping map;
    /** Array of data for epoll-ed file descriptors. Use `fd_info_get`. */
    struct fd_info *fd_info;
    /** Number of items in `fd_info`. */
    size_t num_fds;
    /** Upstream address to connect to. */
    struct sockaddr_storage *upstream_addr;
};

/**
 * Update epoll events for `fd` according to `events` and `fd_info::events`.
 */
static int epoll_mod(struct context *ctx, int fd, uint32_t events) {
    struct fd_info *info = fd_info_get(&ctx->fd_info, &ctx->num_fds, fd);
    assert(info);
    if(events == info->events) {
        return 0;
    }
    if(
        epoll_ctl(
            ctx->epfd,
            events == 0 ? EPOLL_CTL_DEL : info->events == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD,
            fd,
            &(struct epoll_event){
                .events = events,
                .data.fd = fd,
            }
        ) < 0
    ) {
        return -1;
    }
    info->events = events;
    return 0;
}

/**
 * Set epoll events according to `rx` and `tx` buffers.
 */
static int epoll_from_buffers(struct context *ctx, struct connection_endpoint *endpoint) {
    struct fd_info *info = fd_info_get(&ctx->fd_info, &ctx->num_fds, endpoint->fd[1]);
    assert(info);
    uint32_t events = 0;
    if(ACTIVE_RANGE(&endpoint->tx)->len > 0) {
        events |= EPOLLOUT;
    }
    if(
        ACTIVE_RANGE(&endpoint->rx)->len < sizeof(endpoint->rx.buf)
        && !(endpoint->shutdown & (1 << SHUT_RD))
    ) {
        events |= EPOLLIN | EPOLLRDHUP;
    }
    char epoll[512];
    LOG(
        LOG_DEBUG,
        "slot=%zu epoll: fd=%d%s events=%s\n",
        info->slot,
        endpoint->fd[1],
        (endpoint->shutdown & (1 << SHUT_RD)) ? " (eof)" : "",
        epoll_str(epoll, sizeof(epoll), info->events)
    );
    LOG(
        LOG_DEBUG,
        "slot=%zu     => fd=%d%s events=%s\n",
        info->slot,
        endpoint->fd[1],
        (endpoint->shutdown & (1 << SHUT_RD)) ? " (eof)" : "",
        epoll_str(epoll, sizeof(epoll), events)
    );
    if(epoll_mod(ctx, endpoint->fd[1], events) < 0) {
        perror("epoll_ctl");
        // Only fatal if a new event cannot be added. Failure to remove an
        // event only degrades performance.
        if((info->events & events) != events) {
            return -1;
        }
    }
    return 0;
}

/**
 * Set `1 << SHUT_WR` for `dst` and call `shutdown(src->fd[1], SHUT_RD)`.
 */
static void shutdown_direction(struct connection_endpoint *dst, struct connection_endpoint *src) {
    dst->shutdown |= 1 << SHUT_WR;
    if(src->shutdown & (1 << SHUT_RD)) {
        return;
    }
    if(shutdown(src->fd[1], SHUT_RD) < 0) {
        perror("shutdown(SHUT_RD)");
    } else {
        src->shutdown |= 1 << SHUT_RD;
    }
}

/**
 * For every buffer in `conn` overwrite the inactive range with the active range.
 */
static inline void copy_active_ranges(struct connection *conn) {
    for(
        struct atomic_ring_buffer **pbuf = (struct atomic_ring_buffer *[]){
            &conn->downstream.rx,
            &conn->downstream.tx,
            &conn->upstream.rx,
            &conn->upstream.tx,
            NULL
        };
        *pbuf;
        ++pbuf
    ) {
        memcpy(
            INACTIVE_RANGE(*pbuf),
            ACTIVE_RANGE(*pbuf),
            sizeof(*ACTIVE_RANGE(*pbuf))
        );
    }
}

#ifndef PERFORMANCE_BASELINE
/**
 * Set active `rx` and `tx` buffers according to `conn::state` and epoll events
 * for the file descriptors.
 */
static int swap_buffers(struct context *ctx, struct connection *conn) {
    conn->downstream.rx.active = !!(conn->state & (1 <<  8));
    conn->downstream.tx.active = !!(conn->state & (1 <<  9));
    conn->upstream.rx.active   = !!(conn->state & (1 << 10));
    conn->upstream.tx.active   = !!(conn->state & (1 << 11));
    if(
        epoll_from_buffers(ctx, &conn->downstream) < 0
        || epoll_from_buffers(ctx, &conn->upstream) < 0
    ) {
        return -1;
    }
    change_state(conn - ctx->map.addr->connections, &conn->state, CONN_POLL);
    return 0;
}
#endif

/**
 * 1. Handle pending `CONN_SWAP_BUFFERS`.
 * 2. Perform I/O on `fd` indicated by `events`.
 * 3. Call `transform` on the newly populated buffers.
 * 4. Swap active buffers.
 *
 * \return  0 on success (or recoverable error)
 * \return -1 on fatal errors, should cause the process to exit
 */
static int handle_connection(struct context *ctx, int fd, uint32_t events) {
    struct fd_info *info = fd_info_get(&ctx->fd_info, &ctx->num_fds, fd);
    assert(info);

    char str[512];
    LOG(
        LOG_DEBUG,
        "slot=%zu handle_connection(ctx, %d, %s)\n",
        info->slot,
        fd,
        epoll_str(str, sizeof(str), events)
    );

    struct connection *conn = shared_memory_get_connection(&ctx->map, info->slot);

#ifndef PERFORMANCE_BASELINE
    int state = atomic_load_explicit(&conn->state, memory_order_acquire);
    if((state & CONN_STATE_BITS) == CONN_STATE_BITS && swap_buffers(ctx, conn) < 0) {
        return -1;
    }
#endif

    // Find the buffers belonging to the file descritpro.
    struct connection_endpoint *endpoint;
    if(fd == conn->upstream.fd[1]) {
        endpoint = &conn->upstream;
    } else if(fd == conn->downstream.fd[1]) {
        endpoint = &conn->downstream;
    } else {
        LOG(LOG_ERROR, "unknown file descriptor %d\n", fd);
        return -1;
    }

#ifndef PERFORMANCE_BASELINE
    // Just remeber that case-statements are just goto labels.
    switch(state & CONN_STATE_BITS) {
    case CONN_POLL:
#endif
        // Send as much data as possible.
        if(events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
            int rc;
            size_t old_len = ACTIVE_RANGE(&endpoint->tx)->len;
            do {
                rc = atomic_send(endpoint->fd[1], &endpoint->tx);
                LOG(LOG_DEBUG, "atomic_send(%d, ...) = %d\n", endpoint->fd[1], rc);
            } while(rc > 0 || (rc < 0 && errno == EINTR));
            LOG(
                LOG_DEBUG,
                "slot=%zu send:      %10s.fd[1] = %d\n",
                info->slot,
                endpoint == &conn->downstream ? "downstream" : "upstream",
                endpoint->fd[1]
            );
            LOG(
                LOG_DEBUG,
                "slot=%zu            %10s.tx.len = %4zu => %4zu\n",
                info->slot,
                endpoint == &conn->downstream ? "downstream" : "upstream",
                old_len,
                ACTIVE_RANGE(&endpoint->tx)->len
            );
            if(
                rc < 0
                && errno != EAGAIN
                && errno != EWOULDBLOCK
            ) {
                perror("atomic_send");
                goto error;
            }
            // Shutdown one direction of the connenction, if the send buffers
            // are drained.
            if(
                (endpoint->shutdown & (1 << SHUT_WR))
                && ACTIVE_RANGE(&endpoint->tx)->len == 0
            ) {
                LOG(LOG_DEBUG, "slot=%zu shutdown(%d, SHUT_WR)\n", info->slot, endpoint->fd[1]);
                if(shutdown(endpoint->fd[1], SHUT_WR) < 0 && errno != ENOTCONN) {
                    perror("shutdown");
                    goto error;
                }
            }
        }
        // Read as much data as possible.
        if(
            (events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP))
            && !(endpoint->shutdown & (1 << SHUT_RD))
        ) {
            int rc;
            size_t old_len = ACTIVE_RANGE(&endpoint->rx)->len;
            do {
                rc = atomic_recv(endpoint->fd[1], &endpoint->rx);
                LOG(LOG_DEBUG, "atomic_recv(%d, ...) = %d\n", endpoint->fd[1], rc);
            } while(rc > 0 || (rc < 0 && errno == EINTR));
            LOG(
                LOG_DEBUG,
                "slot=%zu recv:      %10s.fd[1] = %d\n",
                info->slot,
                endpoint == &conn->downstream ? "downstream" : "upstream",
                endpoint->fd[1]
            );
            LOG(
                LOG_DEBUG,
                "slot=%zu            %10s.rx.len = %4zu => %4zu\n",
                info->slot,
                endpoint == &conn->downstream ? "downstream" : "upstream",
                old_len,
                ACTIVE_RANGE(&endpoint->rx)->len
            );
            if(
                rc == 0
                // After EOF recvmmsg will return EPIPE. Returned 0 may be lost
                // so EPIPE must be handled.
                || (rc < 0 && errno == EPIPE)
            ) {
                LOG(LOG_DEBUG, "slot=%zu            eof=1\n", info->slot);
                endpoint->shutdown |= 1 << SHUT_RD;
                if(shutdown(endpoint->fd[1], SHUT_RD) < 0) {
                    perror("shutdown(SHUT_RD)");
                }
            } else if(
                rc < 0
                && errno != EAGAIN
                && errno != EWOULDBLOCK
                && errno != EOVERFLOW // Receive buffer is full.
            ) {
                perror("atomic_recv");
                goto error;
            }
        }
        // Transformation takes place on inactive ranges.
#ifndef PERFORMANCE_BASELINE
        copy_active_ranges(conn);
#endif
        struct transformation_direction down = {
            .out_buf = conn->downstream.tx.buf,
            .out_range = INACTIVE_RANGE(&conn->downstream.tx),
            .in_buf = conn->upstream.rx.buf,
            .in_range = INACTIVE_RANGE(&conn->upstream.rx),
            .eof = !!(conn->upstream.shutdown & (1 << SHUT_RD)),
            .shutdown = !!(conn->downstream.shutdown & (1 << SHUT_WR)),
        };
        struct transformation_direction up = {
            .out_buf = conn->upstream.tx.buf,
            .out_range = INACTIVE_RANGE(&conn->upstream.tx),
            .in_buf = conn->downstream.rx.buf,
            .in_range = INACTIVE_RANGE(&conn->downstream.rx),
            .eof = !!(conn->downstream.shutdown & (1 << SHUT_RD)),
            .shutdown = !!(conn->upstream.shutdown & (1 << SHUT_WR)),
        };
        // Buffers length before transform (for logging).
#define SAVE_LOG_LEN(UP_DOWN, RX_TX) size_t UP_DOWN##_##RX_TX##_len = INACTIVE_RANGE(&conn->UP_DOWN.RX_TX)->len
        SAVE_LOG_LEN(downstream, rx);
        SAVE_LOG_LEN(downstream, tx);
        SAVE_LOG_LEN(upstream, rx);
        SAVE_LOG_LEN(upstream, tx);
#undef SAVE_LOG_LEN
        // Transform from recv buffers to send buffers.
        int rc = transform(&conn->transform_ctx, &down, &up);
#define LOG_LEN(prefix, UP_DOWN, RX_TX) \
    LOG( \
        LOG_DEBUG, \
        "slot=%zu %s %13s.len = %4zu => %4zu\n", \
        info->slot, \
        prefix, \
        #UP_DOWN "." #RX_TX, \
        UP_DOWN##_##RX_TX##_len, \
        INACTIVE_RANGE(&conn->UP_DOWN.RX_TX)->len \
    )
        LOG_LEN("transform:", downstream, rx);
        LOG_LEN("          ", upstream, tx);
        LOG_LEN("transform:", upstream, rx);
        LOG_LEN("          ", downstream, tx);
#undef LOG_CHANGE
        if(rc < 0) {
            goto error;
        }
        if(down.shutdown) {
            shutdown_direction(&conn->downstream, &conn->upstream);
        }
        if(up.shutdown) {
            shutdown_direction(&conn->upstream, &conn->downstream);
        }
        // Both directions were shutdown and send buffers are empty.
        if(
            (conn->downstream.shutdown & (1 << SHUT_WR))
            && INACTIVE_RANGE(&conn->downstream.tx)->len == 0
            && (conn->upstream.shutdown & (1 << SHUT_WR))
            && INACTIVE_RANGE(&conn->upstream.tx)->len == 0
        ) {
            int r;
            change_state(info->slot, &conn->state, CONN_CLOSING);
            if(1) {
#ifndef PERFORMANCE_BASELINE
                __attribute__((fallthrough));
    case CONN_CLOSING:
#endif
                r = 0;
            } else {
    error:
                change_state(info->slot, &conn->state, CONN_ERROR);
#ifndef PERFORMANCE_BASELINE
                __attribute__((fallthrough));
    case CONN_ERROR:
#endif
                r = -1;
            }
            // The listener will need to close the file descriptors as well. After
            // closing them in the worker, the listener will be signaled through
            // IPC. If `state == CONN_CLOSING` the last file descriptors of the
            // connections will be closed, if `state == CONN_ERROR` the listener
            // will set `SO_LINGER` beforehand.
            // https://ndeepak.com/posts/2016-10-21-tcprst/
            list_fds(LOG_DEBUG);
            LOG(LOG_INFO, "slot=%zu close(%d)\n", info->slot, conn->upstream.fd[1]);
            if(close(conn->upstream.fd[1]) < 0) {
                perror("close");
                r = -1;
            }
            LOG(LOG_INFO, "slot=%zu close(%d)\n", info->slot, conn->downstream.fd[1]);
            if(close(conn->downstream.fd[1]) < 0) {
                perror("close");
                r = -1;
            }
#ifdef PERFORMANCE_BASELINE
            // Do not exit on connection error.
            return 0;
#else
            if(ipc_send(ctx->ipc_fd, "close", info->slot, -1, NULL) < 0) {
                perror("ipc_send");
                r = -1;
            }
            return r;
#endif
        }

#ifdef PERFORMANCE_BASELINE
        if(
            epoll_from_buffers(ctx, &conn->downstream) < 0
            || epoll_from_buffers(ctx, &conn->upstream) < 0
        ) {
            return -1;
        }
#else
        // Encode the buffers used by `transform` in the atomic state, see
        // `swap_buffers`.
        change_state(
            info->slot,
            &conn->state,
            CONN_SWAP_BUFFERS
            | (!conn->downstream.rx.active <<  8)
            | (!conn->downstream.tx.active <<  9)
            | (!conn->upstream.rx.active   << 10)
            | (!conn->upstream.tx.active   << 11)
        );
        __attribute__((fallthrough));
    case CONN_SWAP_BUFFERS:
        if(swap_buffers(ctx, conn) < 0) {
            return -1;
        }
#endif
        return 0;

#ifndef PERFORMANCE_BASELINE
    default:
        ;
        char str[512];
        LOG(LOG_ERROR, "slot=%zu unsupported state: %s\n", str_state(str, sizeof(str), state));
        goto error;
    }
#endif
}

/**
 * Receive outgoing connection to upstream.
 * \return -1 cannot be differentiated from an error in `ipc_process_incoming`.
 */
static int ipc_connected(const char *action, size_t slot, int fd, const char *tail, void *ctx_) {
    (void)action, (void)tail;
    struct context *ctx = ctx_;
    struct connection *conn = shared_memory_get_connection(&ctx->map, slot);

    // `connected` must be accompanied by a file descriptor.
    if(fd < 0) {
        errno = EBADF;
        LOG(
            LOG_ERROR,
            "no file descriptor received: %s slot=%zu%s%s\n",
            action,
            slot,
            tail ? " " : "",
            tail ? tail : ""
        );
        return -2;
    }
    if(fd_cloexec(fd, 0) < 0) {
        perror("fcntl(F_SETFD)");
    }

    conn->downstream.rx = ATOMIC_RING_BUFFER_INIT;
    conn->downstream.tx = ATOMIC_RING_BUFFER_INIT;
    conn->upstream.rx = ATOMIC_RING_BUFFER_INIT;
    conn->upstream.tx = ATOMIC_RING_BUFFER_INIT;
    conn->upstream.fd[1] = fd;

    struct fd_info *info;

    // Register downstream connection with epoll.
    info = fd_info_get(&ctx->fd_info, &ctx->num_fds, conn->downstream.fd[1]);
    if(!info) {
        perror("realloc");
        return -2;
    }
    *info = (struct fd_info){
        .type = FD_TYPE_CONN,
        .slot = slot,
        .events = 0,
    };
    if(epoll_mod(ctx, conn->downstream.fd[1], EPOLLIN | EPOLLRDHUP) < 0) {
        perror("epoll_ctl");
        return -2;
    }

    // Register upstream connection with epoll.
    info = fd_info_get(&ctx->fd_info, &ctx->num_fds, conn->upstream.fd[1]);
    if(!info) {
        perror("realloc");
        return -2;
    }
    *info = (struct fd_info){
        .type = FD_TYPE_CONN,
        .slot = slot,
        .events = 0,
    };
    if(epoll_mod(ctx, conn->upstream.fd[1], EPOLLIN | EPOLLRDHUP) < 0) {
        perror("epoll_ctl");
        return -2;
    }

    change_state(info->slot, &conn->state, CONN_POLL);

    // Transform empty buffers. `events == 0` means no data is read before
    // `transform` is called.
    if(handle_connection(ctx, conn->downstream.fd[1], 0) < 0) {
        // Cause the worker to exit. It will be restarted by the listener.
        // Another option would be to set the connection to CONN_ERROR and drop
        // the connection.
        return -2;
    }

    return 0;
}

/**
 * Send a connect IPC message to the listener.
 */
static int indirect_connect(struct context *ctx, size_t slot) {
#ifdef PERFORMANCE_BASELINE
    // Directly connect, do not use IPC.
    int fd = socket(ctx->upstream_addr->ss_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if(fd < 0) {
        perror("socket");
        return -1;
    }
    if(connect(fd, (struct sockaddr *)ctx->upstream_addr, sizeof(*ctx->upstream_addr)) == 0) {
        // Connect finished.
        if(ipc_connected("connected", slot, fd, NULL, &ctx) == 0) {
            return 0;
        } else {
            return -1;
        }
        perror("connect");
        return -1;
    } else if(errno == EINPROGRESS) {
        // Connect is happening asynchronously.
        struct fd_info *info = fd_info_get(&ctx->fd_info, &ctx->num_fds, fd);
        if(!info) {
            perror("realloc");
            return -1;
        }
        *info = (struct fd_info){
            .type = FD_TYPE_CONNECTING,
            .slot = slot,
            .events = 0,
        };
        if(epoll_mod(ctx, fd, EPOLLOUT) < 0) {
            perror("epoll_ctl");
            return -1;
        }
        return 0;
    } else {
        perror("connect");
        return -1;
    }
#else
    char str_addr[FORMAT_SOCKADDR_BUFLEN];
    if(
        !format_sockaddr(str_addr, (struct sockaddr *)ctx->upstream_addr)
        || ipc_send(ctx->ipc_fd, "connect", slot, -1, "addr=%s", str_addr) < 0
    ) {
        return -1;
    } else {
        return 0;
    }
#endif
}

/**
 * Receive a incoming connections from the listener and initiate the outgoing
 * connection.
 * \return -1 cannot be differentiated from an error in `ipc_process_incoming`.
 */
static int ipc_accepted(const char *action, size_t slot, int fd, const char *tail, void *ctx_) {
    (void)action, (void)tail;
    struct context *ctx = ctx_;
    struct connection *conn = shared_memory_get_or_append_connection(&ctx->map, slot);
    if(!conn) {
        // `slot` does not exist in the shared memory and the shared memory
        // cannot be extended. Exit the worker.
        perror("pwritev");
        return -2;
    }

    // `accepted` must be accompanied by a file descriptor.
    if(fd < 0) {
        errno = EBADF;
        // Cause the worker to exit. It will be restarted by the listener and
        // passing the file descriptor will be retried. Another option would be
        // to set the connection to CONN_ERROR and drop the connection.
        LOG(
            LOG_ERROR,
            "no file descriptor received: %s slot=%zu%s%s\n",
            action,
            slot,
            tail ? " " : "",
            tail ? tail : ""
        );
        return -2;
    }
    if(fd_cloexec(fd, 0) < 0) {
        perror("fcntl(F_SETFD)");
    }

    conn->worker_pid = getpid();
    conn->downstream.fd[1] = fd;
    change_state(slot, &conn->state, CONN_CONNECTING);

    if(indirect_connect(ctx, slot) < 0) {
        // Cause the worker to exit. It will be restarted by the listener and
        // passing the file descriptor will be retried. Another option would be
        // to set the connection to CONN_ERROR and drop the connection.
        perror("indirect_connect");
        return -2;
    }

    return 0;
}

static const struct ipc_action_method ipc_methods[] = {
    {"accepted", ipc_accepted},
    {"connected", ipc_connected},
    {NULL, NULL},
};

static void free_context(struct context *ctx) {
    free(ctx->fd_info);
    ctx->fd_info = NULL;
    ctx->num_fds = 0;
}

int main(int argc, char **argv) {
    init_log_level();

    __attribute__((cleanup(free_cmdline)))
    struct cmdline_opts cmdline = { 0 };
    if(parse_cmdline(&cmdline, argc, argv) < 0) {
        return 2;
    }

    // Initialize global data.
    __attribute__((cleanup(free_context)))
    struct context ctx = {
        .ipc_fd = cmdline.ipc_direct,
        .fd_info = NULL,
        .num_fds = 0,
        .upstream_addr = &cmdline.upstream_addr,
    };

    if(shared_memory_open(&ctx.map, cmdline.shared_mem_fd, cmdline.shared_mem_addr) < 0) {
        perror("shared_memory_open");
        return 1;
    }

    // Initialize epoll.
    ctx.epfd = epoll_create1(EPOLL_CLOEXEC);
    if(ctx.epfd < 0) {
        perror("epoll_create1");
        return 1;
    }
    struct fd_info *info;

#ifdef PERFORMANCE_BASELINE
    // Poll listening sockets.
    for(size_t i = 0; i < cmdline.num_listen_fds; ++i) {
        info = fd_info_get(&ctx.fd_info, &ctx.num_fds, cmdline.listen_fds[i]);
        if(!info) {
            perror("realloc");
            return 1;
        }
        *info = (struct fd_info){
            .type = FD_TYPE_LISTEN,
            .events = 0,
        };
        if(epoll_mod(&ctx, cmdline.listen_fds[i], EPOLLIN | EPOLLRDHUP) < 0) {
            perror("epoll_ctl");
            return 1;
        }
    }
#else
    // Poll direct IPC socket.
    info = fd_info_get(&ctx.fd_info, &ctx.num_fds, ctx.ipc_fd);
    if(!info) {
        perror("realloc");
        return 1;
    }
    *info = (struct fd_info){
        .type = FD_TYPE_IPC,
        .events = 0,
    };
    if(epoll_mod(&ctx, ctx.ipc_fd, EPOLLIN | EPOLLRDHUP) < 0) {
        perror("epoll_ctl");
        return 1;
    }

    // Poll broadcast IPC socket.
    if(cmdline.ipc_broadcast >= 0) {
        info = fd_info_get(&ctx.fd_info, &ctx.num_fds, cmdline.ipc_broadcast);
        if(!info) {
            perror("realloc");
            return 1;
        }
        *info = (struct fd_info){
            .type = FD_TYPE_IPC,
            .events = 0,
        };
        if(epoll_mod(&ctx, cmdline.ipc_broadcast, EPOLLIN | EPOLLRDHUP) < 0) {
            perror("epoll_ctl");
            return 1;
        }
    }
#endif

    // Handle SIGUSR*.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    int sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if(sigfd < 0) {
        perror("signalfd");
        return 1;
    }
    if(sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        perror("sigprocmask");
        return 1;
    }
    info = fd_info_get(&ctx.fd_info, &ctx.num_fds, sigfd);
    if(!info) {
        perror("realloc");
        return 1;
    }
    *info = (struct fd_info){
        .type = FD_TYPE_SIGNAL,
        .events = 0,
    };
    if(epoll_mod(&ctx, sigfd, EPOLLIN | EPOLLRDHUP) < 0) {
        perror("epoll_ctl");
        return 1;
    }

#ifdef PERFORMANCE_BASELINE
    // TODO really necessary?
    signal(SIGPIPE, SIG_IGN);
#endif

    while(1) {
        struct epoll_event evs[16];
        int num_events;
        EINTR_RETRY(num_events, epoll_wait(ctx.epfd, evs, sizeof(evs) / sizeof(evs[0]), -1));
        if(num_events < 0) {
            perror("epoll_wait");
            return 1;
        }

        for(size_t i = 0; i < (size_t)num_events; ++i) {
            const int fd = evs[i].data.fd;
            struct fd_info unknown = { .type = FD_TYPE_UNKNOWN };
            info = fd_info_get(&ctx.fd_info, &ctx.num_fds, fd);
            if(!info) {
                info = &unknown;
            }

            switch(info->type) {
#ifdef PERFORMANCE_BASELINE
            case FD_TYPE_LISTEN:
                {
                    struct connection *conn = accept_connection(&ctx.map, evs[i].data.fd);
                    if(!conn || indirect_connect(&ctx, conn - ctx.map.addr->connections) < 0) {
                        return 1;
                    }
                    conn->downstream.fd[1] = conn->downstream.fd[0];
                }
                break;
            case FD_TYPE_CONNECTING:
                {
                    int e;
                    socklen_t len = sizeof(e);
                    if(getsockopt(evs[i].data.fd, SOL_SOCKET, SO_ERROR, &e, &len) < 0) {
                        perror("getsockopt");
                        return 1;
                    }
                    assert(len == sizeof(e));
                    if(e) {
                        errno = e;
                        perror("connect");
                        return 1;
                    }
                    // ipc_connected calls EPOLL_CTL_ADD, so the file descriptor
                    // has to be un-registered first.
                    // TODO remove this
                    if(epoll_mod(&ctx, evs[i].data.fd, 0) < 0) {
                        perror("epoll_ctl");
                        return 1;
                    }
                    if(ipc_connected("connected", info->slot, evs[i].data.fd, NULL, &ctx) != 0) {
                        return 1;
                    }
                }
                break;
#endif
            case FD_TYPE_IPC:
                // Handle incoming IPC message.
                switch(ipc_process_incoming(fd, ipc_methods, &ctx)) {
                case 0:
                    // Success.
                    break;
                case -1:
                    // Error in ipc_process_incoming itself.
                    perror("ipc_process_incoming");
                    return 1;
                default:
                    // Error in an IPC method.
                    return 1;
                }
                break;
            case FD_TYPE_SIGNAL:
                // Handle signals.
                while(1) {
                    struct signalfd_siginfo siginfo;
                    ssize_t n;
                    EINTR_RETRY(n, read(fd, &siginfo, sizeof(siginfo)));
                    if(n < 0) {
                        if(errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        perror("read");
                        return 1;
                    }

                    switch(siginfo.ssi_signo) {
                    case SIGUSR1:
                    case SIGUSR2:
                        list_fds(LOG_ALWAYS);
                        break;
                    default:
                        LOG(LOG_ALWAYS, "signal %s is currently ignored", signame(siginfo.ssi_signo));
                        break;
                    }
                }
                break;
            case FD_TYPE_CONN:
                // Handle connection traffic.
                if(handle_connection(&ctx, fd, evs[i].events) < 0) {
                    return 1;
                }
                break;
            default:
                LOG(LOG_ERROR, "unknown file descriptor %d\n", fd);
                break;
            }
        }
    }

    return 0;
}
