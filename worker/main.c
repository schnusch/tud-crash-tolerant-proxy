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
#include "../libcrash/libcrash.h"

/**
 * Log with a common prefix identifying the current connection.
 * Required variables: `slot`, `conn`
 */
#define LOG_CONN(level, fmt, ...) LOG( \
    level, \
    "slot=%zu %d" UTF8_ARROW_EAST "%d: " fmt, \
    slot, \
    conn->downstream.fd[1], \
    conn->upstream.fd[1], \
    __VA_ARGS__ \
)

/**
 * Log result of transform.
 * Required variables: `slot`, `conn`
 */
#define LOG_XFRM(LEVEL, ARROW, PRE_LEN, PRE_DIFF, POST_LEN, POST_DIFF, SHUTDOWN) LOG( \
    LEVEL, \
    (SHUTDOWN) \
    ? "slot=%zu %d" UTF8_ARROW_EAST "%d: xfrm " ARROW " %6zu%-+7zd " UTF8_ARROW_EAST " %6zu%-+7zd (SHUT_WR)\n" \
    : "slot=%zu %d" UTF8_ARROW_EAST "%d: xfrm " ARROW " %6zu%-+7zd " UTF8_ARROW_EAST " %6zu%+zd\n", \
    slot, \
    conn->downstream.fd[1], \
    conn->upstream.fd[1], \
    PRE_LEN, \
    PRE_DIFF, \
    POST_LEN, \
    POST_DIFF \
)

/**
 * Change `*state` and log the change.
 * \param slot      only used for logging
 * \param state     pointer to `connection::state`
 * \param new_state new state
 */
static void change_state(size_t slot, struct connection *conn, atomic_int *state, int new_state) {
    char old[512], new[512];
    LOG_CONN(
        LOG_DEBUG_STATE,
        "%s " UTF8_ARROW_EAST " %s\n",
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

    size_t slot = info->slot;
    // Do not use `shared_memory_get_connection`, it could remap the shared
    // memory and make `endpoint` an invalid pointer.
    struct connection *conn = &ctx->map.addr->connections[slot];

    uint32_t events = 0;
    if(ACTIVE_RANGE(&endpoint->tx)->len > 0) {
        events |= EPOLLOUT;
    } else if(ACTIVE_RANGE(&endpoint->tx)->shutdown) {
        // Send buffer is drained and SHUT_WR was indicated by transform.
        // TODO only once
        LOG_CONN(LOG_DEBUG, "shutdown(%d, SHUT_WR)\n", endpoint->fd[1]);
        if(shutdown(endpoint->fd[1], SHUT_WR) < 0 && errno != ENOTCONN) {
            perror("shutdown");
            return -1;
        }
    }
    if(
        ACTIVE_RANGE(&endpoint->rx)->len < sizeof(endpoint->rx.buf)
        && !ACTIVE_RANGE(&endpoint->rx)->shutdown
    ) {
        events |= EPOLLIN | EPOLLRDHUP;
    }

    char old[512], new[512];
    LOG_CONN(
        LOG_DEBUG_BYTES,
        "poll %d%s %s " UTF8_ARROW_EAST " %s\n",
        endpoint->fd[1],
        ACTIVE_RANGE(&endpoint->rx)->shutdown ? " (eof)" : "",
        epoll_str(old, sizeof(old), info->events),
        epoll_str(new, sizeof(new), events)
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
    INACTIVE_RANGE(&dst->tx)->shutdown = -1;
    if(INACTIVE_RANGE(&src->rx)->shutdown) {
        return;
    }
    if(shutdown(src->fd[1], SHUT_RD) < 0) {
        perror("shutdown(SHUT_RD)");
    } else {
        INACTIVE_RANGE(&src->rx)->shutdown = -1;
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
    conn->transform_ctx.active = !!(conn->state & (1 << 12));
    if(
        epoll_from_buffers(ctx, &conn->downstream) < 0
        || epoll_from_buffers(ctx, &conn->upstream) < 0
    ) {
        return -1;
    }
    change_state(conn - ctx->map.addr->connections, conn, &conn->state, CONN_POLL);
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
    struct connection *conn = shared_memory_get_connection(&ctx->map, info->slot);
    assert(conn);

    const int slot = info->slot; // Required by LOG_CONN

    char str[512];
    LOG_CONN(LOG_DEBUG_BYTES, "poll %d %s\n", fd, epoll_str(str, sizeof(str), events));

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
        LOG_CONN(LOG_ERROR, "unknown file descriptor %d\n", fd);
        return -1;
    }

#ifndef PERFORMANCE_BASELINE
    // Just remeber that case-statements are just goto labels.
    switch(state & CONN_STATE_BITS) {
    case CONN_POLL:
#endif
        // Send as much data as possible.
        if(events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
            int rc = 0;
            size_t old_len = ACTIVE_RANGE(&endpoint->tx)->len;
            while(ACTIVE_RANGE(&endpoint->tx)->len > 0) {
                rc = atomic_send(endpoint->fd[1], &endpoint->tx);
                if(rc < 0 && errno != EINTR) {
                    break;
                }
                assert(rc > 0);
            }
            LOG_CONN(
                LOG_DEBUG_BYTES,
                "send %s %6zu%+zd\n",
                endpoint == &conn->downstream ? UTF8_ARROW_SOUTH : UTF8_ARROW_NORTH,
                old_len,
                ACTIVE_RANGE(&endpoint->tx)->len - old_len
            );
            if(
                rc < 0
                && errno != EAGAIN
                && errno != EWOULDBLOCK
            ) {
                perror("atomic_send");
                goto error;
            }
        }
        // Read as much data as possible.
        if(
            (events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP))
            && !ACTIVE_RANGE(&endpoint->rx)->shutdown
        ) {
            size_t old_len = ACTIVE_RANGE(&endpoint->rx)->len;
            assert(ACTIVE_RANGE(&endpoint->rx)->len < sizeof(endpoint->rx.buf));
            int rc;
            do {
                rc = atomic_recv(endpoint->fd[1], &endpoint->rx);
                if(rc == 0 || (rc < 0 && errno != EINTR)) {
                    break;
                }
            } while(ACTIVE_RANGE(&endpoint->rx)->len < sizeof(endpoint->rx.buf));
            LOG_CONN(
                LOG_DEBUG_BYTES,
                "recv %s %6zu%+zd%s\n",
                endpoint == &conn->upstream ? UTF8_ARROW_SOUTH : UTF8_ARROW_NORTH,
                old_len,
                ACTIVE_RANGE(&endpoint->rx)->len - old_len,
                rc == 0 || (rc < 0 && errno == EPIPE) ? " (eof)" : ""
            );
            if(
                rc == 0
                // After EOF recvmmsg will return EPIPE. Returned 0 may be lost
                // so EPIPE must be handled.
                || (rc < 0 && errno == EPIPE)
            ) {
                ACTIVE_RANGE(&endpoint->rx)->shutdown = -1;
                // shutdown(SHUT_RD) can be lost, it will be redone on EPIPE.
                if(shutdown(endpoint->fd[1], SHUT_RD) < 0 && errno != ENOTCONN) {
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
            .eof = !!INACTIVE_RANGE(&conn->upstream.rx)->shutdown,
            .shutdown = !!INACTIVE_RANGE(&conn->downstream.tx)->shutdown,
        };
        struct transformation_direction up = {
            .out_buf = conn->upstream.tx.buf,
            .out_range = INACTIVE_RANGE(&conn->upstream.tx),
            .in_buf = conn->downstream.rx.buf,
            .in_range = INACTIVE_RANGE(&conn->downstream.rx),
            .eof = !!INACTIVE_RANGE(&conn->downstream.rx)->shutdown,
            .shutdown = !!INACTIVE_RANGE(&conn->upstream.tx)->shutdown,
        };
        // Buffers length before transform (for logging).
#define SAVE_LOG_LEN(UP_DOWN, RX_TX) size_t UP_DOWN##_##RX_TX##_len = INACTIVE_RANGE(&conn->UP_DOWN.RX_TX)->len
        SAVE_LOG_LEN(downstream, rx);
        SAVE_LOG_LEN(downstream, tx);
        SAVE_LOG_LEN(upstream, rx);
        SAVE_LOG_LEN(upstream, tx);
#undef SAVE_LOG_LEN
        // Transform from recv buffers to send buffers.
        int rc = transform(info->slot, &conn->transform_ctx, &down, &up);
        LOG_XFRM(
            LOG_DEBUG_BYTES,
            UTF8_ARROW_NORTH,
            downstream_rx_len,
            INACTIVE_RANGE(&conn->downstream.rx)->len - downstream_rx_len,
            upstream_tx_len,
            INACTIVE_RANGE(&conn->upstream.tx)->len - upstream_tx_len,
            up.shutdown
        );
        LOG_XFRM(
            LOG_DEBUG_BYTES,
            UTF8_ARROW_SOUTH,
            upstream_rx_len,
            INACTIVE_RANGE(&conn->upstream.rx)->len - upstream_rx_len,
            downstream_tx_len,
            INACTIVE_RANGE(&conn->downstream.tx)->len - downstream_tx_len,
            down.shutdown
        );
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
            INACTIVE_RANGE(&conn->downstream.tx)->shutdown
            && INACTIVE_RANGE(&conn->downstream.tx)->len == 0
            && INACTIVE_RANGE(&conn->upstream.tx)->shutdown
            && INACTIVE_RANGE(&conn->upstream.tx)->len == 0
        ) {
            int r;
            change_state(info->slot, conn, &conn->state, CONN_CLOSING);
            if(1) {
#ifndef PERFORMANCE_BASELINE
                __attribute__((fallthrough));
    case CONN_CLOSING:
#endif
                r = 0;
            } else {
    error:
                change_state(info->slot, conn, &conn->state, CONN_ERROR);
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
            // The listener will set the connection to CONN_UNUSED.
            LOG(LOG_DEBUG, "connections: %s\n", connection_status_all(&ctx->map, slot));
            FOREACH_CONNECTION_ENDPOINT(endpoint, conn) {
                LOG_CONN(LOG_DEBUG_BYTES, "close(%d)\n", endpoint->fd[1]);
                if(closep(&endpoint->fd[1]) < 0) {
                    if(errno == EBADF) {
                        endpoint->fd[1] = -1;
                    }
                    perror("close");
                    r = -1;
                }
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
            conn,
            &conn->state,
            CONN_SWAP_BUFFERS
            | (!conn->downstream.rx.active <<  8)
            | (!conn->downstream.tx.active <<  9)
            | (!conn->upstream.rx.active   << 10)
            | (!conn->upstream.tx.active   << 11)
            | (!conn->transform_ctx.active << 12)
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
        LOG_CONN(LOG_ERROR, "unknown state: %s\n", str_state(str, sizeof(str), state));
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

    LOG(LOG_DEBUG, "connections: %s\n", connection_status_all(&ctx->map, slot));

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

    struct connection *conn = shared_memory_get_connection(&ctx->map, slot);
    assert(conn);

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

    change_state(info->slot, conn, &conn->state, CONN_POLL);

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

    LOG(LOG_DEBUG, "connections: %s\n", connection_status_all(&ctx->map, slot));

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

    struct connection *conn = shared_memory_get_or_append_connection(&ctx->map, slot);
    assert(conn);

    conn->worker_pid = getpid();
    conn->downstream.fd[1] = fd;
    change_state(slot, conn, &conn->state, CONN_CONNECTING);

    if(indirect_connect(ctx, slot) < 0) {
        // Cause the worker to exit. It will be restarted by the listener and
        // passing the file descriptor will be retried. Another option would be
        // to set the connection to CONN_ERROR and drop the connection.
        perror("indirect_connect");
        return -2;
    }

    return 0;
}

static int ipc_close(const char *action, size_t slot, int fd, const char *tail, void *ctx_) {
    (void)action, (void)tail;
    struct context *ctx = ctx_;

    LOG(LOG_DEBUG, "connections: %s\n", connection_status_all(&ctx->map, slot));

    int rc = 0;
    if(fd >= 0) {
        LOG(LOG_ERROR, "IPC close should not received a file descriptor fd=%d\n", fd);
        if(closep(&fd) < 0) {
            perror("close");
        }
        rc = -2;
    }

    struct connection *conn = shared_memory_get_connection(&ctx->map, slot);
    assert(conn);
    FOREACH_CONNECTION_ENDPOINT(endpoint, conn) {
        if(closep(&endpoint->fd[1]) < 0) {
            if(errno == EBADF) {
                endpoint->fd[1] = -1;
            } else {
                perror("close");
            }
        }
    }
    atomic_store_explicit(&conn->state, CONN_UNUSED, memory_order_release);
    return rc;
}

/**
 * Receive an already accepted file descriptor.
 */
static int ipc_orphan_downstream(const char *action, size_t slot, int fd, const char *tail, void *ctx_) {
    (void)action, (void)tail;
    struct context *ctx = ctx_;

    LOG(LOG_DEBUG, "connections: %s\n", connection_status_all(&ctx->map, slot));

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

    struct connection *conn = shared_memory_get_or_append_connection(&ctx->map, slot);
    assert(conn);

    conn->worker_pid = getpid();
    conn->downstream.fd[1] = fd;
    if(ipc_send(ctx->ipc_fd, "orphan_up", slot, -1, NULL) < 0) {
        perror("ipc_send");
        return -2;
    }
    return 0;
}

/**
 * Receive an already connected file descriptor.
 */
static int ipc_orphan_upstream(const char *action, size_t slot, int fd, const char *tail, void *ctx_) {
    (void)action, (void)tail;
    struct context *ctx = ctx_;

    LOG(LOG_DEBUG, "connections: %s\n", connection_status_all(&ctx->map, slot));

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

    struct connection *conn = shared_memory_get_or_append_connection(&ctx->map, slot);
    assert(conn);

    conn->worker_pid = getpid();
    conn->upstream.fd[1] = fd;

    LOG(LOG_ERROR, "slot=%zu orphaned connection received\n", slot);
    list_fds(LOG_ERROR);

    // Register orphaned connection with epoll.
    FOREACH_CONNECTION_ENDPOINT(endpoint, conn) {
        struct fd_info *info = fd_info_get(&ctx->fd_info, &ctx->num_fds, endpoint->fd[1]);
        if(!info) {
            perror("realloc");
            return -2;
        }
        *info = (struct fd_info){
            .type = FD_TYPE_CONN,
            .slot = slot,
            .events = 0,
        };
        if(epoll_from_buffers(ctx, endpoint) < 0) {
            return -2;
        }
    }

    return 0;
}

static const struct ipc_action_method ipc_methods[] = {
    {"accepted", ipc_accepted},
    {"connected", ipc_connected},
    {"close", ipc_close},
    {"orphan_down", ipc_orphan_downstream},
    {"orphan_up", ipc_orphan_upstream},
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

    if(shared_memory_open(&ctx.map, cmdline.shared_mem_fd) < 0) {
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
#ifdef USE_LIBCRASH
    sigdelset(&mask, LIBCRASH_SIGNAL);
    if(libcrash_init(LIBCRASH_SIGNAL) < 0) {
        return 1;
    }
#endif
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
