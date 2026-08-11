#include <alloca.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "cmdline.h"
#include "fd_info.h"
#include "main_active.h"
#include "worker_process.h"
#include "../common/accept.h"
#include "../common/ipc.h"
#include "../common/util.h"
#include "../libcrash/libcrash.h"

/**
 * Set `SO_LINGER` on both file descriptors.
 */
static void set_linger(struct connection *conn) {
    static const struct linger l = {
        .l_onoff = 1,
        .l_linger = 0,
    };
    FOREACH_CONNECTION_ENDPOINT(endpoint, conn) {
        if(setsockopt(endpoint->fd[0], SOL_SOCKET, SO_LINGER, &l, sizeof(l)) < 0) {
            perror("setsockopt(SOL_SOCKET, SO_LINGER)");
        }
    }
}

/**
 * Close both file descriptors.
 */
static void close_connection(struct connection *conn) {
    FOREACH_CONNECTION_ENDPOINT(endpoint, conn) {
        if(closep(&endpoint->fd[0]) < 0) {
            if(errno == EBADF) {
                endpoint->fd[0] = -1;
            } else {
                perror("close");
            }
        }
    }
}

/**
 * Close connections with `CONN_CLOSING` and add all other file descriptors to
 * `fd_info`.
 */
static int cleanup_connections(
    struct fd_info **fd_info,
    size_t *num_fds,
    struct shared_memory_mapping *map,
    const struct worker_process *procs,
    size_t num_procs
) {
    // TODO Sort non-empty connecions to the front, so the size of the shared
    // memory can be reduced. Must synchronize with workers.
    size_t truncate_to = 0;
    FOREACH_CONNECTION(conn, map) {
        size_t slot = conn - map->addr->connections;
        const int state = atomic_load_explicit(&conn->state, memory_order_acquire);
        switch(state & CONN_STATE_BITS) {
        case CONN_ERROR:
            // Set SO_LINGER to send TCP RST instead of FIN.
            set_linger(conn);
            __attribute__((fallthrough));
        case CONN_CLOSING:
            // Close file descriptors.
            close_connection(conn);
            char str[512];
            LOG(LOG_INFO, "slot=%zu cleaned up due to state=%s\n", slot, str_state(str, sizeof(str), state));
            atomic_store_explicit(&conn->state, CONN_UNUSED, memory_order_release);
            break;
        default:
            // Remember the file descriptor.
            FOREACH_CONNECTION_ENDPOINT(endpoint, conn) {
                if(endpoint->fd[0] < 0) {
                    continue;
                }
                struct fd_info *info = fd_info_get(fd_info, num_fds, endpoint->fd[0]);
                if(!info) {
                    perror("realloc");
                    return -1;
                }
                *info = (struct fd_info){
                    .type = FD_TYPE_CONN,
                    .slot = slot,
                };
            }
            truncate_to = slot + 1;
            break;
        }
    }
    // Truncate unused connections.
    size_t length = (char *)&map->addr->connections[truncate_to] - (char *)map->addr;
    // TODO truncate `map`
    return 0;
}

/**
 * Iterate through `/proc/self/fd`. If exactly one file descriptor is found
 * that is not already stored in `fd_info` return it. Otherwise close all
 * unknown file descriptors.
 */
int recover_one_fd(struct fd_info **known_fds, size_t *num_known_fds) {
    // Iterate over the process' file descriptors.
    DIR *d = opendir("/proc/self/fd");
    if(!d) {
        perror("opendir");
        return -1;
    }
    size_t num_unknown_fds = 0;
    int unknown_fd = -1;
    struct dirent *e;
    while((e = readdir(d))) {
        if(
            e->d_name[0] == '.' && (
                e->d_name[1] == '\0'
                || (e->d_name[1] == '.' && e->d_name[2] == '\0')
            )
        ) {
            continue;
        }

        // Parse file descriptor.
        errno = 0;
        char *end;
        long l = strtol(e->d_name, &end, 10);
        if((l == LONG_MIN || l == LONG_MAX) && errno != 0) {
        skip:
            LOG(LOG_ERROR, "ignoring /proc/self/fd/%s: %s\n", e->d_name, strerror(errno));
            continue;
        } else if(*end != '\0') {
            errno = EINVAL;
            goto skip;
        } else if(l < 0 || INT_MAX < l) {
            errno = ERANGE;
            goto skip;
        }
        int fd = l;

        struct fd_info *info = fd_info_get(known_fds, num_known_fds, fd);
        if(info) {
            // File descriptor is known to the listener.
            continue;
        }

        int flags = fcntl(fd, F_GETFD);
        if(flags < 0) {
            perror("fcntl(F_GETFD)");
        } else if(flags & FD_CLOEXEC) {
            // File descriptors marked with FD_CLOEXEC can be recreated by the
            // active listener process without loosing any state. Close FDs of
            // the previous process to free resources.
            if(closep(&fd) < 0) {
                perror("close");
            }
            continue;
        }

        if(num_unknown_fds == 0) {
            // First unknown file descriptor.
            unknown_fd = fd;
        } else {
            // More than one unknown file descriptor.
            if(unknown_fd >= 0) {
                list_fds(LOG_ERROR);
                LOG(LOG_ERROR, "more than one unknown file descriptors: %d\n", unknown_fd);
                if(closep(&unknown_fd) < 0) {
                    perror("close");
                }
            }
            LOG(LOG_ERROR, "more than one unknown file descriptors: %d\n", fd);
            if(closep(&fd) < 0) {
                perror("close");
            }
        }
        ++num_unknown_fds;
    }
    closedir(d);

    return unknown_fd;
}

static int fds_from_cmdline(struct fd_info **known_fds, size_t *num_known_fds, struct cmdline_opts *cmdline) {
    struct fd_info *info;
    for(size_t i = 0; i < 2; ++i) {
        info = fd_info_get(known_fds, num_known_fds, cmdline->ipc_broadcast[i]);
        if(!info) {
            perror("realloc");
            return 1;
        }
        *info = (struct fd_info){ .type = FD_TYPE_IPC };
    }
    for(size_t i = 0, n = worker_process_array_len(&cmdline->worker_procs); i < n; ++i) {
        struct worker_process *proc = worker_process_array_get(&cmdline->worker_procs, i);
        assert(proc);

        if(proc->ipc_fd >= 0) {
            info = fd_info_get(known_fds, num_known_fds, proc->ipc_fd);
            if(!info) {
                perror("realloc");
                return 1;
            }
            *info = (struct fd_info){ .type = FD_TYPE_IPC };
        }

        if(proc->pid_fd >= 0) {
            info = fd_info_get(known_fds, num_known_fds, proc->pid_fd);
            if(!info) {
                perror("realloc");
                return 1;
            }
            *info = (struct fd_info){ .type = FD_TYPE_PID };
        }
    }
    return 0;
}

struct ipc_context {
    struct shared_memory_mapping *map;
    int ipc_fd;
};

static int ipc_connect(const char *action, size_t slot, int fd, const char *tail, void *ctx_) {
    (void)action, (void)tail;
    struct ipc_context *ctx = ctx_;
    struct connection *conn = shared_memory_get_connection(ctx->map, slot);
    assert(conn);

    // `connect` should not receive a file descriptor.
    if(fd >= 0) {
        LOG(LOG_ERROR, "IPC connect should not received a file descriptor fd=%d\n", fd);
        if(closep(&fd) < 0) {
            perror("close");
        }
    }

    // Parse upstream address.
    if(!tail || strncmp(tail, "addr=", 5) != 0) {
        LOG(
            LOG_ERROR,
            "IPC message is missing a 'addr=': %s slot=%zu%s%s\n",
            action,
            slot,
            tail ? " " : "",
            tail ? tail : ""
        );
        goto error;
    }
    struct sockaddr_storage addr;
    if(parse_sockaddr(&addr, tail + 5) < 0) {
        perror("parse_sockaddr");
        goto error;
    }

    // Connect to address. Keep it FD_CLOEXEC until it connected. This means
    // the upstream connection might get dropped.
    // TODO no FD_CLOEXEC, check if the socket is connected during recovery.
    fd = socket(addr.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd < 0) {
        perror("socket");
        goto error;
    }
    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        goto error;
    }
    if(fd_cloexec(fd, 0) < 0) {
        perror("fcntl(F_SETFD)");
        goto error;
    }

    LIBCRASH(connect_post(fd, (struct sockaddr *)&addr, sizeof(addr)));

    // Only now the connection's file descriptor can be recovered. Unconnected
    // sockets are discarded by FD_CLOEXEC.
    conn->upstream.fd[0] = fd;

    // Respond the connected socket.
    if(ipc_send(ctx->ipc_fd, "connected", slot, conn->upstream.fd[0], NULL) < 0) {
        perror("ipc_send");
        goto error;
    }

    return 0;

error:
    set_linger(conn);
    // Close this proceses file descriptors first, then notify the worker. The
    // worker will set the connection to CONN_UNUSED.
    close_connection(conn);
    if(ipc_send(ctx->ipc_fd, "close", slot, -1, NULL) < 0) {
        perror("ipc_send");
        return -2;
    }
    return 0;
}

/**
 * Handle `close ...` IPC messages.
 */
static int ipc_close(const char *action, size_t slot, int fd, const char *tail, void *ctx_) {
    (void)action, (void)tail;
    struct ipc_context *ctx = ctx_;
    struct connection *conn = shared_memory_get_connection(ctx->map, slot);
    assert(conn);

    int state = atomic_load_explicit(&conn->state, memory_order_acquire);

    if(state == CONN_ERROR) {
        set_linger(conn);
    } else {
        assert(state == CONN_CLOSING);
    }
    close_connection(conn);

    atomic_store_explicit(&conn->state, CONN_UNUSED, memory_order_release);

    return 0;
}

/**
 * Send an already connected upstream file descriptor.
 */
static int ipc_orphan_upstream(const char *action, size_t slot, int fd, const char *tail, void *ctx_) {
    (void)action, (void)tail;
    struct ipc_context *ctx = ctx_;
    struct connection *conn = shared_memory_get_connection(ctx->map, slot);
    assert(conn);

    // `connect` should not receive a file descriptor.
    if(fd >= 0) {
        LOG(LOG_ERROR, "IPC connect should not received a file descriptor fd=%d\n", fd);
        if(closep(&fd) < 0) {
            perror("close");
        }
    }

    // Respond the connected socket.
    if(ipc_send(ctx->ipc_fd, "orphan_up", slot, conn->upstream.fd[0], NULL) < 0) {
        perror("ipc_send");
        return -2;
    }

    return 0;
}

static const struct ipc_action_method ipc_methods[] = {
    {"connect", ipc_connect},
    {"close", ipc_close},
    {"orphan_up", ipc_orphan_upstream},
    {NULL, NULL},
};

int main_active(struct cmdline_opts *cmdline, struct shared_memory_mapping *map, int parent_pidfd) {
    // Move the listener and its workers to a separate process group.
    if(setpgid(0, 0) < 0) {
        perror("setpgid");
        return 1;
    }

    // Array of file descriptors known to the listener.
    struct epoll_context ctx = {
        .epfd = -1,
        .fd_info = NULL,
        .num_fds = 0,
    };
    for(int *fd = (int[]){ STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO, -1 }; *fd >= 0; ++fd) {
        struct fd_info *info = fd_info_get(&ctx.fd_info, &ctx.num_fds, *fd);
        if(!info) {
            perror("realloc");
            return 1;
        }
        *info = (struct fd_info){ .type = FD_TYPE_STDIO };
    }
    if(fds_from_cmdline(&ctx.fd_info, &ctx.num_fds, cmdline) < 0) {
        return 1;
    }

    if(
        cleanup_connections(
            &ctx.fd_info,
            &ctx.num_fds,
            map,
            worker_process_array_get(&cmdline->worker_procs, 0),
            worker_process_array_len(&cmdline->worker_procs)
        ) < 0
    ) {
        perror("cleanup_connections");
        return 1;
    }

    int recovered_fd = recover_one_fd(&ctx.fd_info, &ctx.num_fds);
    if(recovered_fd >= 0) {
        // TODO put to connection
    }

    // Initialize epoll.
    __attribute__((cleanup(closep_no_error)))
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if(epfd < 0) {
        perror("epoll_create1");
        return 1;
    }
    ctx.epfd = epfd;

    list_fds(LOG_ALWAYS);

    // Poll existing/start new worker processes.
    for(size_t i = 0, n = worker_process_array_len(&cmdline->worker_procs); i < n; ++i) {
        struct worker_process *proc = worker_process_array_get(&cmdline->worker_procs, i);
        assert(proc);
        if(proc->pid_fd < 0) {
            // Start new worker process.
            if(worker_process_spawn(&ctx, proc, i, cmdline, cmdline->ipc_broadcast[0]) < 0) {
                return 1;
            }
        } else {
            // Poll existing worker process.
            LOG(LOG_INFO, "worker_process[%zu] = { .ipc_fd = %d, .pid_fd = %d, .pid = %d }\n", i, proc->ipc_fd, proc->pid_fd, proc->pid);
            if(
                worker_process_epoll_add(
                    &ctx,
                    proc->pid_fd,
                    EPOLLIN,
                    &(struct fd_info){ .type = FD_TYPE_PID, .slot = i }
                ) < 0 || (
                    proc->ipc_fd >= 0
                    && worker_process_epoll_add(
                        &ctx,
                        proc->ipc_fd,
                        EPOLLIN | EPOLLRDHUP,
                        &(struct fd_info){ .type = FD_TYPE_IPC, .slot = i }
                    ) < 0
                )
            ) {
                return 1;
            }
        }
    }

    // Existing workers could be polled here. No other file descriptors are
    // currently registered to epoll.

    // Poll passive parent process.
    if(worker_process_epoll_add(&ctx, parent_pidfd, EPOLLIN, &(struct fd_info){ .type = FD_TYPE_PID, .slot = -1 }) < 0) {
        return 1;
    }

    // Poll signals SIGUSR*.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
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
    if(worker_process_epoll_add(&ctx, sigfd, EPOLLIN, &(struct fd_info){ .type = FD_TYPE_SIGNAL, .slot = -1 }) < 0) {
        return 1;
    }

    // Poll listening file descriptors.
    for(size_t i = 0; i < cmdline->num_listen_fds; ++i) {
        if(worker_process_epoll_add(&ctx, cmdline->listen_fds[i], EPOLLIN, &(struct fd_info){ .type = FD_TYPE_LISTEN, .slot = -1 }) < 0) {
            return 1;
        }
    }

    // Distribute orphaned connections to workers.
    FOREACH_CONNECTION(conn, map) {
        int state = atomic_load_explicit(&conn->state, memory_order_acquire);
        if(state == CONN_UNUSED || conn->worker_pid >= 0) {
            continue;
        }
        size_t slot = conn - map->addr->connections;
        LOG(
            LOG_INFO,
            "slot=%zu distributing orphaned connection %d <=> %d (0x%X)\n",
            slot,
            conn->downstream.fd[0],
            conn->upstream.fd[0],
            state
        );
        int e = 0;
        switch(state & CONN_STATE_BITS) {
        case CONN_POLL:
        case CONN_SWAP_BUFFERS:
            // Upstream and downstreams file descriptors were saved.
            e = ipc_send(cmdline->ipc_broadcast[1], "orphan_down", slot, conn->downstream.fd[0], NULL);
            break;
        case CONN_ACCEPTING:
        case CONN_CONNECTING:
            // Only the downstream file descriptor was saved, reconnect to
            // upstream.
            e = ipc_send(cmdline->ipc_broadcast[1], "accepted", slot, conn->downstream.fd[0], NULL);
            break;
        default:
            ;
            char str[512];
            LOG(LOG_ERROR, "unexpected state %s\n", str_state(str, sizeof(str), state));
            break;
        }
        if(e < 0) {
            perror("ipc_send");
            return 1;
        }
    }

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
            struct fd_info *info = fd_info_get(&ctx.fd_info, &ctx.num_fds, fd);
            if(!info) {
                perror("realloc");
                return 1;
            }

            switch(info->type) {
            case FD_TYPE_LISTEN:
                LOG(LOG_INFO, "accepting incoming connection on fd=%d...\n", evs[i].data.fd);
                struct connection *conn = accept_connection(map, evs[i].data.fd);
                if(!conn) {
                    return 1;
                }
                size_t slot = conn - map->addr->connections;
                if(ipc_send(cmdline->ipc_broadcast[1], "accepted", slot, conn->downstream.fd[0], NULL) < 0) {
                    perror("ipc_send");
                    return -1;
                }
                break;
            case FD_TYPE_IPC:
                // Handle incoming IPC message.
                switch(
                    ipc_process_incoming(
                        fd,
                        ipc_methods,
                        &(struct ipc_context){
                            .map = map,
                            .ipc_fd = evs[i].data.fd,
                        }
                    )
                ) {
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
            case FD_TYPE_PID:
                if(info->slot == (size_t)-1) {
                    // Parent passive process died.
                    LOG(LOG_ALWAYS, "passive listener terminated\n");
                    // TODO become passive
                } else if(evs[i].events & EPOLLHUP) {
                    // Worker process was reaped.
                    LOG(LOG_ALWAYS, "worker process terminated\n");
                    // TODO restart
                }
                if(close(evs[i].data.fd) < 0) {
                    perror("close");
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
                    case SIGCHLD:
                        // Reap worker processes. Worker processes are restarted
                        // on EPOLLHUP of the PID file descriptor.
                        while(1) {
                            pid_t child;
                            int wstatus;
                            EINTR_RETRY(child, waitpid(-1, &wstatus, WNOHANG | WUNTRACED));
                            if(child < 0) {
                                if(errno == ECHILD) {
                                    // waitpid(2) would hang.
                                    break;
                                } else {
                                    perror("wait");
                                    continue;
                                }
                            }
                            if(WIFSIGNALED(wstatus)) {
                                LOG(
                                    LOG_ALWAYS,
                                    "process %d was killed by %s",
                                    (int)child,
                                    signame(WTERMSIG(wstatus))
                                );
                            } else if(WIFEXITED(wstatus)) {
                                LOG(
                                    LOG_ALWAYS,
                                    "process %d exited with %d",
                                    (int)child,
                                    WEXITSTATUS(wstatus)
                                );
                            } else if(WIFSTOPPED(wstatus)) {
                                LOG(LOG_ALWAYS, "process %d was stopped, continuing...", (int)child);
                                if(kill(child, SIGCONT) < 0) {
                                    perror("kill");
                                    // TODO fatal?
                                }
                            }
                        }
                        break;
                    default:
                        LOG(LOG_INFO, "signal %s is currently ignored", signame(siginfo.ssi_signo));
                        break;
                    }
                }
                break;
            default:
                LOG(LOG_ERROR, "unknown file descriptor %d\n", fd);
                break;
            }
        }
    }
}
