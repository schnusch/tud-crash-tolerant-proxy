#include <assert.h>
#include <errno.h>
#include <fcntl.h> // O_CLOEXEC
#include <limits.h> // INT_MAX, PIPE_BUF
#include <signal.h>
#include <stdio.h> // perror
#include <stdlib.h> // free
#include <sys/epoll.h>
#include <sys/mman.h> // memfd_create
#include <sys/wait.h>
#include <unistd.h>

#include "cmdline.h"
#include "fd_info.h"
#include "pidfd.h"
#include "worker_process.h"
#include "../common/util.h"

#define CAST(x) (&(x)->map)

int worker_process_array_init(struct worker_process_array *arr) {
    int fd = -1;
    if(CAST(arr)->fd < 0) {
        fd = memfd_create("worker_processes", MFD_CLOEXEC);
        if(fd < 0) {
            return -1;
        }
        CAST(arr)->fd = fd;
    }
    if(shared_memory_open(CAST(arr), CAST(arr)->fd) < 0) {
        if(fd >= 0) {
            closep(&CAST(arr)->fd);
        }
        return -1;
    }
    return 0;
}

size_t worker_process_array_len(struct worker_process_array *arr) {
    struct worker_process *start = (struct worker_process *)CAST(arr)->addr->connections;
    struct worker_process *end = (struct worker_process *)((char *)CAST(arr)->addr + CAST(arr)->addr->size);
    const size_t size = (char *)end - (char *)start;
    end = (struct worker_process *)(
        (char *)end - (size % sizeof(struct worker_process))
    );
    return end - start;
}

struct worker_process *worker_process_array_get(struct worker_process_array *arr, size_t i) {
    size_t len = worker_process_array_len(arr);
    if(i >= len) {
        struct worker_process empty = {
            .ipc_fd = -1,
            .pid_fd = -1,
            .pid = -1,
        };
        if(shared_memory_append(CAST(arr), &empty, sizeof(empty), i + 1 - len) < 0) {
            return NULL;
        }
    }
    struct worker_process *start = (struct worker_process *)CAST(arr)->addr->connections;
    return &start[i];
}

static int write_errno(int fd) {
    int e = errno;
    _Static_assert(sizeof(e) <= PIPE_BUF, "sizeof(int) <= PIPE_BUF");
    char *p = (char *)&e;
    size_t remaining = sizeof(e);
    while(remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if(n < 0) {
            return -1;
        }
        p += n;
        remaining -= n;
    }
    return 0;
}

static int read_errno(int fd) {
    int e = 0;
    char *p = (char *)&e;
    size_t remaining = sizeof(e);
    while(remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if(n < 0) {
            return -1;
        } else if(n == 0) {
            if(remaining < sizeof(e)) {
                e = EPIPE;
            }
            break;
        }
        p += n;
        remaining -= n;
    }
    errno = e;
    return 0;
}

pid_t worker_process_spawn(
    struct epoll_context *ctx,
    struct worker_process *proc,
    size_t i,
    const struct cmdline_opts *cmdline,
    int ipc_broadcast
) {
    int ipc_fd[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    pid_t child = -1;
    int pid_fd = -1;
    sigset_t *restore_sigset = NULL;

    if(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ipc_fd) < 0) {
        perror("socketpair");
        goto error;
    }

    if(pipe2(err_pipe, O_CLOEXEC) < 0) {
        perror("pipe");
        goto error;
    }

    // SIGCHLD must not interfere with the call to waitpid.
    sigset_t sigchld, oldset;
    sigemptyset(&sigchld);
    sigaddset(&sigchld, SIGCHLD);
    if(sigprocmask(SIG_BLOCK, &sigchld, &oldset) < 0) {
        perror("sigprocmask");
        goto error;
    }
    restore_sigset = &oldset;

    child = fork();
    if(child < 0) {
        perror("fork");
        goto error;
    } else if(child == 0) {
        // Try to use STDIN_FILENO for ipc_broadcast.
        int dup_fd = dup2(ipc_broadcast, STDIN_FILENO);
        if(dup_fd < 0) {
            dup_fd = ipc_broadcast;
        }
        // Close all other file descriptors.
        char **argv;
        if(
            sigprocmask(SIG_SETMASK, restore_sigset, NULL) == 0
            && close_range(STDERR_FILENO + 1, INT_MAX, CLOSE_RANGE_CLOEXEC) == 0
            && fd_cloexec(dup_fd, 0) == 0
            && fd_cloexec(ipc_fd[1], 0) == 0
            && fd_cloexec(cmdline->shared_mem_fd, 0) == 0
            && (argv = cmdline_to_worker_argv(cmdline, dup_fd, ipc_fd[1]))
        ) {
            list_fds(LOG_INFO);
            LOG(LOG_INFO, "$");
            for(char **p = argv; *p; ++p) {
                fprintf(stderr, " %s", *p);
            }
            fprintf(stderr, "\n");

            execvp(argv[0], argv);
            free(argv);
        }
        // Send errno to parent process.
        if(write_errno(ipc_fd[1]) < 0) {
            perror("write");
        }
        _exit(1);
    }
    // Close the write end of the errno pipe, so that the child process has the
    // only writer and a successful exec(3) causes EOF.
    if(closep(&err_pipe[1]) < 0) {
        perror("close");
        goto error;
    }
    err_pipe[1] = -1;
    // Receive errno from child process.
    if(read_errno(err_pipe[0]) < 0) {
        perror("read");
        goto error;
    } else if(errno != 0) {
        perror("execvp");
        goto error;
    }

    // Open PID FD.
    pid_fd = pidfd_open(child, 0);
    if(pid_fd < 0) {
        perror("pidfd_open");
        goto error;
    }
    if(fd_cloexec(pid_fd, 0) < 0) {
        perror("fcntl(F_SETFD)");
        if(closep(&pid_fd) < 0) {
            perror("close");
        }
        goto error;
    }

    // Register file descriptors with epoll.
    if(
        worker_process_epoll_add(
            ctx,
            ipc_fd[0],
            EPOLLIN | EPOLLRDHUP,
            &(struct fd_info){ .type = FD_TYPE_IPC, .slot = i }
        ) < 0
        || worker_process_epoll_add(
            ctx,
            pid_fd,
            EPOLLIN,
            &(struct fd_info){ .type = FD_TYPE_PID, .slot = i }
        ) < 0
    ) {
        goto error;
    };

    // Success.
    *proc = (struct worker_process){
        .ipc_fd = ipc_fd[0],
        .pid_fd = pid_fd,
        .pid = child,
    };

    if(0) {
error:
        if(closep(&ipc_fd[0]) < 0) {
            perror("close");
            proc->ipc_fd = -1; // EBADF
        }

        if(child >= 0) {
            // Kill and reap the child process.
            if(kill(child, SIGKILL) < 0) {
                perror("kill");
            } else {
                pid_t r;
                int wstatus;
                EINTR_RETRY(r, waitpid(proc->pid, &wstatus, 0));
                if(r < 0) {
                    perror("waitpid");
                } else if(WIFEXITED(wstatus) || WIFSIGNALED(wstatus)) {
                    // Cleanup `*proc`.
                    proc->pid = -1;
                    if(closep(&proc->pid_fd) < 0) {
                        perror("close");
                        proc->pid_fd = -1; // EBADF
                    }
                } else {
                    LOG(LOG_INFO, "process %d neither exited nor was killed by a signal: wstatus=%d\n", (int)child, wstatus);
                }
            }
        }
        child = -1;
    }
    if(restore_sigset) {
        if(sigprocmask(SIG_SETMASK, restore_sigset, NULL) < 0) {
            perror("sigprocmask");
        }
        restore_sigset = NULL;
    }
    if(closep(&ipc_fd[1]) < 0 || closep(&err_pipe[0]) < 0 || closep(&err_pipe[1]) < 0) {
        perror("close");
        goto error;
    }
    LOG(
        LOG_DEBUG,
        "new  worker_process[%zu] = { .ipc_fd = %d, .pid_fd = %d, .pid = %d }\n",
        i,
        proc->ipc_fd,
        proc->pid_fd,
        (int)proc->pid
    );
    return child;
}

int worker_process_epoll_add(
    struct epoll_context *ctx,
    int fd,
    uint32_t events,
    const struct fd_info *new_info
) {
    struct fd_info *info = fd_info_get(&ctx->fd_info, &ctx->num_fds, fd);
    if(!info) {
        perror("realloc");
        return -1;
    }
    *info = *new_info;
    if(
        epoll_ctl(
            ctx->epfd,
            EPOLL_CTL_ADD,
            fd,
            &(struct epoll_event){
                .events = events,
                .data.fd = fd,
            }
        ) < 0
    ) {
        perror("epoll_ctl");
        return -1;
    }
    return 0;
}
