#include <alloca.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h> // open
#include <poll.h>
#include <sched.h> // clone
#include <signal.h>
#include <stdlib.h> // free
#include <sys/wait.h>

#include "clone3.h"
#include "listen.h"
#include "main_active.h"
#include "main_passive.h"
#include "pidfd.h"
#include "../common/util.h"

static int set_oom_score_adj(int oom_adj) {
    if(oom_adj < -1000 || 1000 < oom_adj) {
        errno = EINVAL;
        return -1;
    }

    int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
    if(fd < 0) {
        return -1;
    }
    int e = dprintf(fd, "%d\n", oom_adj);
    if(closep(&fd) < 0 || e < 0) {
        return -1;
    }
    return 0;
}

struct context {
    struct cmdline_opts *cmdline;
    struct shared_memory_mapping *map;
    int pidfd;
};

/**
 * Reset settings made by `main_passive` and then call `main_active`.
 */
static int start_listener(void *ctx_) {
    struct context *ctx = ctx_;
    sigset_t unblock;
    sigfillset(&unblock);
    if(sigprocmask(SIG_UNBLOCK, &unblock, NULL) < 0) {
        perror("sigprocmask");
        return 1;
    }
    if(set_oom_score_adj(0) < 0) {
        perror("set_oom_score_adj");
    }
    return main_active(ctx->cmdline, ctx->map, ctx->pidfd);
}

static int kill_active(pid_t active_pid, struct worker_process_array *worker_procs, int signal) {
    int r = 0;

    if(kill(active_pid, signal) < 0) {
        r = -1;
    }

    for(size_t i = 0, n = worker_process_array_len(worker_procs); i < n; ++i) {
        struct worker_process *proc = worker_process_array_get(worker_procs, i);
        assert(proc);
        if(proc->pid < 0) {
            continue;
        }
        if(proc->pid_fd >= 0 && pidfd_send_signal(proc->pid_fd, signal, NULL, 0) < 0) {
            r = -1;
        }
    }

    return r;
}

static int compare_ints(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

int main_passive(int argc, char **argv) {
    __attribute__((cleanup(free_cmdline)))
    struct cmdline_opts cmdline = {
        .listen_fds = NULL,
        .listen_addrs = NULL,
        .worker_procs = (struct worker_process_array){
            .map = (struct shared_memory_mapping){
                .fd = -1,
            },
        },
    };
    if(parse_cmdline(&cmdline, argc, argv) < 0) {
        return 2;
    }

    // Prepare all listen_fds.
    if(ensure_stdio_no_listen(&cmdline) < 0) {
        return 1;
    }
    if(add_systemd_listen_fds(&cmdline, 1) < 0) {
        perror("add_systemd_listen_fds");
        return 1;
    }
    if(bind_listen_addrs(&cmdline) < 0) {
        return 1;
    }
    qsort(cmdline.listen_fds, cmdline.num_listen_fds, sizeof(*cmdline.listen_fds), compare_ints);

    // No longer needed.
    free(cmdline.listen_addrs);
    cmdline.listen_addrs = NULL;

    // Initialize shared memory.
    struct shared_memory_mapping map;
    if(shared_memory_open(&map, cmdline.shared_mem_fd, cmdline.shared_mem_addr) < 0) {
        perror("shared_memory_open");
        return 1;
    }
    cmdline.shared_mem_fd = map.fd;
    cmdline.shared_mem_addr = map.addr;

    // Create broadcast IPC socket pair.
    if(cmdline.ipc_broadcast[0] < 0 || cmdline.ipc_broadcast[1] < 0) {
        for(size_t i = 0; i < 2; ++i) {
            if(closep(&cmdline.ipc_broadcast[i]) < 0) {
                perror("close");
                return 1;
            }
        }
        if(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, cmdline.ipc_broadcast) < 0) {
            perror("socketpair");
            return 1;
        }
        if(
            shutdown(cmdline.ipc_broadcast[0], SHUT_WR) < 0
            || shutdown(cmdline.ipc_broadcast[1], SHUT_RD) < 0
        ) {
            perror("shutdown");
            return 1;
        }
    }

    // > The value of oom_score_adj is added to the badness score before it is
    // > used to determine which task to kill. Acceptable values range from
    // > -1000 (OOM_SCORE_ADJ_MIN) to +1000 (OOM_SCORE_ADJ_MAX).  This allows
    // > user space to control the preference for OOM-killing, ranging from
    // > always preferring a certain task or completely disabling it from
    // > OOM-killing.  The lowest possible value, -1000, is equivalent to
    // > disabling OOM-killing entirely for that task, since it will always
    // > report a badness score of 0.
    // https://man7.org/linux/man-pages/man5/proc_pid_oom_score_adj.5.html
    if(set_oom_score_adj(-1000) < 0) {
        perror("set_oom_score_adj");
    }

    // Create PID FD so that the active listener can detect if its parent exits.
    struct context ctx = {
        .cmdline = &cmdline,
        .map = &map,
    };
    ctx.pidfd = pidfd_open(getpid(), 0);
    if(ctx.pidfd < 0) {
        perror("pidfd_open");
        return 1;
    }
    if(fd_cloexec(ctx.pidfd, 0) < 0) {
        perror("fcntl(F_SETFD)");
        return 1;
    }
    // FD_CLOEXEC is set by pidfd_open.

    // Signals are later handled with sigwait(2).
    sigset_t blocked_signals;
    sigfillset(&blocked_signals);

    // Do not block these signals.
    sigdelset(&blocked_signals, SIGABRT);
    sigdelset(&blocked_signals, SIGBUS);
    sigdelset(&blocked_signals, SIGFPE);
    sigdelset(&blocked_signals, SIGIOT);
    sigdelset(&blocked_signals, SIGSEGV);
    sigdelset(&blocked_signals, SIGSTKFLT);
    sigdelset(&blocked_signals, SIGTSTP);
    sigdelset(&blocked_signals, SIGSYS);
    sigdelset(&blocked_signals, SIGTRAP);
    sigdelset(&blocked_signals, SIGVTALRM);

    if(sigprocmask(SIG_SETMASK, &blocked_signals, NULL) < 0) {
        perror("sigprocmask");
        return 1;
    }

    while(1) {
        // clone3 allocates its own stack unless CLONE_VM is specified.
        // valgrind does not implement clone3.
        // https://bugs.kde.org/show_bug.cgi?id=420906
        struct clone_args cl_args = {
            .flags = CLONE_FILES,
            .exit_signal = SIGCHLD,
        };
        pid_t active_pid = clone3(&cl_args, sizeof(cl_args));
        if(active_pid < 0) {
            perror("clone");
            return 1;
        } else if(active_pid == 0) {
            _exit(start_listener(&ctx));
        }
        LOG("active listener process %d started\n", (int)active_pid);

        // Action set by signals.
        enum {
            SIGNAL_SHUTDOWN = 1,
        } action = 0;

        // Process signals until the active listner process terminates.
        for(int exited = 0; !exited;) {
            int e, signum;
            do {
                e = sigwait(&blocked_signals, &signum);
            } while(e == EINTR);
            if(e > 0) {
                errno = e;
                perror("sigwait");
                continue;
            }

            switch(signum) {
            case SIGINT:
                action = SIGNAL_SHUTDOWN;
                __attribute__((fallthrough));
            case SIGHUP:
            case SIGUSR1:
            case SIGUSR2:
                // Relay signals to other processes.
                if(kill_active(active_pid, &cmdline.worker_procs, signum) < 0) {
                    perror("kill");
                }
                break;
            case SIGCHLD:
                // Reap child process.
                while(1) {
                    pid_t child;
                    int wstatus;
                    EINTR_RETRY(child, waitpid(active_pid, &wstatus, WNOHANG | WUNTRACED));
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
                            "active listener process %d was killed by %s\n",
                            (int)child,
                            signame(WTERMSIG(wstatus))
                        );
                        exited = 1;
                    } else if(WIFEXITED(wstatus)) {
                        LOG(
                            "active listener process %d exited with %d\n",
                            (int)child,
                            WEXITSTATUS(wstatus)
                        );
                        exited = 1;
                    } else if(WIFSTOPPED(wstatus) && child == active_pid) {
                        LOG("active listener process %d was stopped, continuing...\n", (int)child);
                        if(kill(child, SIGCONT) < 0) {
                            perror("kill");
                            // TODO fatal?
                        }
                    }
                }
                break;
            default:
                LOG("signal %s is currently ignored\n", signame(signum));
                break;
            }
        }
        // Active listener process terminated.

        if(action == SIGNAL_SHUTDOWN) {
            // Exit.
            return 0;
        }

        // Try re-exec.
        char **argv = cmdline_to_listener_argv(&cmdline);
        if(argv) {
            execvp(argv[0], argv);
            free(argv);
        }
        LOG(
            "cannot exec %s continuing with old executable: %s\n",
            cmdline.listener,
            strerror(errno)
        );
    }
}
