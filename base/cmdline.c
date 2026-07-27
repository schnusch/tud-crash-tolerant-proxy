#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cmdline.h"
#include "../common/util.h"

enum getopt_result {
    OPT_END = -1,
    OPT_NULL = 0,
    OPT_LISTEN_ADDR = 'l',
    OPT_UNKNOWN = '?',
    OPT_HELP = 256,
    OPT_IPC_BROADCAST,
    OPT_IPC_DIRECT,
    OPT_LISTENER_BIN,
    OPT_LISTEN_FD,
    OPT_NUM_WORKERS,
    OPT_PID_FD,
    OPT_SHARED_FD,
    OPT_UPSTREAM_ADDR,
    OPT_WORKER_BIN,
    OPT_WORKER_PROCESS,
};

static const struct option long_options[] = {
    {"help", no_argument, NULL, OPT_HELP},
    {"shared-memory-fd", required_argument, NULL, OPT_SHARED_FD},
#ifndef SHARED_MEMORY_RELOCATABLE
    {"shared-memory-address", required_argument, NULL, OPT_SHARED_ADDR},
#endif
    {"upstream-address", required_argument, NULL, OPT_UPSTREAM_ADDR},
    {"ipc-broadcast", required_argument, NULL, OPT_IPC_BROADCAST},
#ifdef CMDLINE_WORKER
    {"ipc-direct", required_argument, NULL, OPT_IPC_DIRECT},
#endif
#if defined(CMDLINE_LISTENER) || defined(PERFORMANCE_BASELINE)
    {"listen-fd", required_argument, NULL, OPT_LISTEN_FD},
#endif
#ifdef CMDLINE_LISTENER
    {"listen", required_argument, NULL, OPT_LISTEN_ADDR},
    {"worker-process", required_argument, NULL, OPT_WORKER_PROCESS},
    {"num-workers", required_argument, NULL, OPT_NUM_WORKERS},
    {"listener", required_argument, NULL, OPT_LISTENER_BIN},
    {"worker", required_argument, NULL, OPT_WORKER_BIN},
#endif
    {NULL, 0, NULL, 0},
};

#ifdef CMDLINE_LISTENER
static const char short_options[] = "l:";
#endif
#ifdef CMDLINE_WORKER
static const char short_options[] = "";
#endif

static const struct {
    int val;
    const char *description;
} help_texts[] = {
    {OPT_HELP, "display this help and exit"},
#ifdef CMDLINE_LISTENER
    {OPT_IPC_BROADCAST, "UNIX socket pair connecting the listener process to all its worker processes"},
#endif
#ifdef CMDLINE_WORKER
    {OPT_IPC_BROADCAST, "UNIX socket connecting the listener process to all its worker processes"},
#endif
    {OPT_IPC_DIRECT, "UNIX socket connecting the listener process to a single worker process"},
    {OPT_LISTEN_ADDR, "listen address"},
    {OPT_LISTENER_BIN, "path to the executable of the listener"},
    {OPT_LISTEN_FD, "listening file descriptors on which incoming connections are accepted"},
    {OPT_NUM_WORKERS, "number of worker processes"},
    {OPT_SHARED_FD, "file descriptor of the shared memory"},
#ifndef SHARED_MEMORY_RELOCATABLE
    {OPT_SHARED_ADDR, "address of the shared memory"},
#endif
    {OPT_UPSTREAM_ADDR, "upstream address to forward incoming connections to"},
    {OPT_WORKER_BIN, "path to the executable of the worker"},
    {OPT_WORKER_PROCESS, "IPC_FD:PID pair of a worker process"},
    {OPT_END, NULL},
};

void usage(FILE *stream, const char *argv0) {
    fprintf(stream, "Usage: %s", argv0);
    for(const struct option *opt = long_options; opt->name; ++opt) {
        const char *bracket_left = "[";
        const char *bracket_right = "]";
        const char *value = "=<value>";
        if(opt->has_arg != optional_argument) {
            bracket_left = "";
            bracket_right = "";
        }
        if(opt->has_arg == no_argument) {
            value = "";
        }
        fprintf(stream, " [--%s%s%s%s]",
            opt->name,
            bracket_left,
            value,
            bracket_right
        );
    }
    fputs("\n", stream);
}

void help(FILE *stream, const char *argv0) {
    usage(stream, argv0);
    fputc('\n', stream);
    for(const struct option *opt = long_options; opt->name; ++opt) {
        // Format short option.
        char short_opt[4] = "   ";
        if(' ' <= opt->val && opt->val <= '~') {
            short_opt[0] = '-';
            short_opt[1] = opt->val;
            short_opt[2] = ',';
        }

        // Format optarg.
        const char *bracket_left = "[";
        const char *bracket_right = "]";
        const char *value = "=<value>";
        if(opt->has_arg != optional_argument) {
            bracket_left = "";
            bracket_right = "";
        }
        if(opt->has_arg == no_argument) {
            value = "";
        }

        // Find description of the option.
        const char *description = NULL;
        for(size_t i = 0; help_texts[i].val != OPT_END; ++i) {
            if(help_texts[i].val == opt->val) {
                description = help_texts[i].description;
            }
        }

        // Print option.
        int n = fprintf(
            stream,
            "  %s --%s%s%s%s",
            short_opt,
            opt->name,
            bracket_left,
            value,
            bracket_right
        );
        static const char indent[] = "                                ";
        if(n < 0 || (size_t)n > strlen(indent) - 1) {
            fputc('\n', stream);
            n = 0;
        }
        if(description) {
            fputs(indent + n, stream);
            fputs(description, stream);
            fputc('\n', stream);
        }
    }
}

/**
 * Parse ´str` to a file descriptor.
 * \return the file descriptor
 * \return -1 on error
 */
static int atofd(const char *str, const char *argv0) {
    int e;
    int fd = strtol_limit(&e, str, 0, INT_MAX);
    if(e) {
        fprintf(
            stderr,
            "%s: invalid file descriptor '%s'%s%s\n",
            argv0,
            str,
            errno == 0 ? "" : ": ",
            errno == 0 ? "" : strerror(errno)
        );
        return -1;
    }
    return fd;
}

#ifndef SHARED_MEMORY_RELOCATABLE
/**
 * Parse `str` to a pointer using `strtoull(2)`.
 * \return the pointer
 * \return NULL on error
 */
static void *atoptr(const char *str, const char *argv0) {
    errno = 0;
    char *end;
    unsigned long long ll = strtoull(optarg, &end, 0);
    if(errno != 0 || *end != '\0') {
        goto error;
    }
    // (Mis-)use __builtin_add_overflow to detect overflow in conversion
    // from unsigned long long to uintptr_t.
    uintptr_t ptr;
    if(__builtin_add_overflow(ll, 0, &ptr)) {
        errno = EINVAL;
        goto error;
    }
    return (void *)ptr;

error:
    fprintf(stderr, "%s: invalid pointer '%s'%s%s\n",
        argv0,
        str,
        errno == 0 ? "" : ": ",
        errno == 0 ? "" : strerror(errno)
    );
    return NULL;
}
#endif

#ifdef CMDLINE_LISTENER
static int parse_worker_process_arg(struct cmdline_opts *cmdline, const char *arg) {
    char *delim_ptr[2];
    delim_ptr[0] = strchr(arg, ',');
    if(!delim_ptr[0]) {
        errno = EINVAL;
        return -1;
    }
    delim_ptr[1] = strchr(delim_ptr[0] + 1, ',');
    if(!delim_ptr[1]) {
        errno = EINVAL;
        return -1;
    }

    int ipc_fd = -1;
    int pid_fd = -1;
    pid_t pid = -1;

    int e;
    char delim_backup[2];
    delim_backup[0] = *delim_ptr[0], *delim_ptr[0] = '\0';
    delim_backup[1] = *delim_ptr[1], *delim_ptr[1] = '\0';
    ipc_fd = strtol_limit(&e, optarg, 0, INT_MAX);
    if(!e) {
        pid_fd = strtol_limit(&e, delim_ptr[0] + 1, 0, INT_MAX);
    }
    if(!e) {
        pid = strtol_limit(&e, delim_ptr[1] + 1, 0, INT_MAX);
    }
    *delim_ptr[0] = delim_backup[0];
    *delim_ptr[1] = delim_backup[1];

    if(e) {
        return -1;
    }

    struct worker_process *proc = worker_process_array_get(
        &cmdline->worker_procs,
        worker_process_array_len(&cmdline->worker_procs)
    );
    if(!proc) {
        return -1;
    }
    *proc = (struct worker_process){
        .ipc_fd = ipc_fd,
        .pid_fd = pid_fd,
        .pid = pid,
    };
    return 0;
}
#endif

void free_cmdline(struct cmdline_opts *cmdline) {
#ifdef CMDLINE_LISTENER
    cmdline->num_listen_addrs = 0;
    free(cmdline->listen_addrs);
    cmdline->listen_addrs = NULL;

    cmdline->num_listen_fds = 0;
    free(cmdline->listen_fds);
    cmdline->listen_fds = NULL;
#endif
}

_Static_assert(
    AF_UNIX != (sa_family_t)-1 && AF_INET != (sa_family_t)-1 && AF_INET6 != (sa_family_t)-1,
    "address family sentinel value"
);

int parse_cmdline(struct cmdline_opts *cmdline, int argc, char **argv) {
    *cmdline = (struct cmdline_opts){
        (int)-1, // shared_mem_fd
        NULL, // shared_mem_addr
        (struct sockaddr_storage){ .ss_family = -1 }, // upstream_addr
#ifdef CMDLINE_WORKER
        (int)-1, // ipc_broadcast
        (int)-1, // ipc_direct
#endif
#if defined(CMDLINE_LISTENER) || defined(PERFORMANCE_BASELINE)
        (size_t)0, // num_listen_fds
        (int *)NULL, // listen_fds
#endif
#ifdef CMDLINE_LISTENER
        (size_t)0, // num_listen_addrs
        (struct sockaddr_storage *)NULL, // listen_addrs
        { -1, -1 }, // ipc_broadcast
        (struct worker_process_array){ // workers_procs
            .map = (struct shared_memory_mapping){
                .fd = -1,
            },
        },
        (char *)LISTENER_PATH, // listener
        (char *)WORKER_PATH, // worker
#endif
    };
#ifdef CMDLINE_LISTENER
    if(worker_process_array_init(&cmdline->worker_procs) < 0) {
        perror("worker_process_array_init");
        return -1;
    }
    size_t num_worker_procs = 1;
#endif

    for(enum getopt_result opt; (opt = getopt_long(argc, argv, short_options, long_options, NULL)) != OPT_END;) {
        switch(opt) {
        case OPT_SHARED_FD:
            if((cmdline->shared_mem_fd = atofd(optarg, argv[0])) < 0) {
                return -1;
            }
            break;
#ifndef SHARED_MEMORY_RELOCATABLE
        case OPT_SHARED_ADDR:
            // Parse shared memory address.
            if(!(cmdline->shared_mem_addr = atoptr(optarg, argv[0]))) {
                return -1;
            }
            break;
#endif
        case OPT_UPSTREAM_ADDR:
            // Parse upstream address.
            if(parse_sockaddr(&cmdline->upstream_addr, optarg) < 0) {
                perror("parse_sockaddr");
                return -1;
            }
            break;
        case OPT_IPC_BROADCAST:
#ifdef CMDLINE_LISTENER
            // Parse IPC file descriptor pair.
            ;
            char *comma = strchr(optarg, ',');
            if(!comma) {
                // TODO
                return -1;
            }
            char *first = strndupa(optarg, comma - optarg);
            if(
                (cmdline->ipc_broadcast[0] = atofd(first, argv[0])) < 0
                || (cmdline->ipc_broadcast[1] = atofd(comma + 1, argv[0])) < 0
            ) {
                return -1;
            }
            break;
#endif
#ifdef CMDLINE_WORKER
            // Parse IPC file descriptor.
            if((cmdline->ipc_broadcast = atofd(optarg, argv[0])) < 0) {
                return -1;
            }
            break;
        case OPT_IPC_DIRECT:
            // Parse IPC file descriptor.
            if((cmdline->ipc_direct = atofd(optarg, argv[0])) < 0) {
                return -1;
            }
            break;
#endif
#if defined(CMDLINE_LISTENER) || defined(PERFORMANCE_BASELINE)
        case OPT_LISTEN_FD:
            // Parse and append listening file descriptor.
            {
                int listen_fd = atofd(optarg, argv[0]);
                if(listen_fd < 0) {
                    return -1;
                }
                // Ignore duplicates.
                for(size_t i = 0; i < cmdline->num_listen_fds; ++i) {
                    if(cmdline->listen_fds[i] == listen_fd) {
                        listen_fd = -1;
                        break;
                    }
                }
                if(listen_fd >= 0 && append_fd(&cmdline->listen_fds, &cmdline->num_listen_fds, listen_fd) < 0) {
                    perror("realloc");
                    return -1;
                }
            }
            break;
#endif
#ifdef CMDLINE_LISTENER
        case OPT_LISTEN_ADDR:
            // Parse and append listening address.
            assert(optarg);
            {
                struct sockaddr_storage addr;
                if(parse_sockaddr(&addr, optarg) < 0) {
                    perror("parse_sockaddr");
                    return -1;
                }
                if(append_sockaddr(&cmdline->listen_addrs, &cmdline->num_listen_addrs, &addr) < 0) {
                    perror("realloc");
                    return -1;
                }
            }
            break;
        case OPT_WORKER_PROCESS:
            if(parse_worker_process_arg(cmdline, optarg) < 0) {
                fprintf(stderr, "%s: cannot parse worker process '%s': %s\n", argv[0], optarg, strerror(errno));
                return -1;
            }
            break;
        case OPT_NUM_WORKERS:
            // Parse number of workers.
            {
                int e;
                num_worker_procs = strtol_limit(&e, optarg, 1, UINT_MAX);
                if(e) {
                    fprintf(
                        stderr,
                        "%s: invalid number of worker processes '%s': %s\n",
                        argv[0],
                        optarg,
                        strerror(errno)
                    );
                    return -1;
                }
            }
            break;
        case OPT_LISTENER_BIN:
            cmdline->listener = optarg;
            break;
        case OPT_WORKER_BIN:
            cmdline->worker = optarg;
            break;
#endif
        case OPT_HELP:
            help(stdout, argv[0]);
            exit(0);
            __builtin_trap();
        default:
            fprintf(stderr, "%s: cannot parse command line\n", argv[0]);
            __attribute__((fallthrough));
        case OPT_UNKNOWN:
            usage(stderr, argv[0]);
            return -1;
        }
    }

#if 1
    size_t size = 0;
    for(int i = 0; i < argc; ++i) {
        size += strlen(argv[i]) + 1;
    }
    char *str_argv = alloca(size);
    char *p = str_argv;
    for(int i = 0; i < argc; ++i) {
        p += strlcpy(p, argv[i], str_argv + size - p);
        assert(p < str_argv + size);
        *p++ = ' ';
    }
    p[-1] = '\0';
    LOG(LOG_DEBUG, "$ %s\n", str_argv);
    list_fds(LOG_DEBUG);
#endif

    if(cmdline->upstream_addr.ss_family == (sa_family_t)-1) {
        fprintf(stderr, "%s: missing required option: --upstream-address\n", argv[0]);
        return -1;
    }
#ifdef CMDLINE_LISTENER
    // Reserve as much `struct worker_proc` as given by --num-workers.
    if(!worker_process_array_get(&cmdline->worker_procs, num_worker_procs - 1)) {
        perror("pwritev");
        return -1;
    }
#endif
#ifdef CMDLINE_WORKER
    if(cmdline->ipc_direct < 0) {
        fprintf(stderr, "%s: missing required option: --ipc-direct\n", argv[0]);
        return -1;
    }
#endif

#ifndef SHARED_MEMORY_RELOCATABLE
    if((cmdline->shared_mem_fd == -1) ^ (cmdline->shared_mem_addr == NULL)) {
        fprintf(stderr, "%s: either both or neither of --shared-memory-fd and --shared-memory-address must be specified\n", argv[0]);
        return -1;
    }
#endif

    return 0;
}

static int argv_append(char ***argv, const char *fmt, ...) {
    size_t argc = 0;
    size_t str_size = 0;
    if(*argv) {
        char *last = NULL;
        while((*argv)[argc]) {
            if((*argv)[argc] > last) {
                last = (*argv)[argc];
            }
            ++argc;
        }
        if(last) {
            str_size = last + strlen(last) + 1 - (char *)(*argv + argc + 1);
        }
    }

    va_list ap, ap2;
    va_start(ap, fmt);

    // Calculate length of new argument.
    va_copy(ap2, ap);
    const int new_len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if(new_len < 0) {
        va_end(ap);
        return -1;
    }

    // Store old starting address to fix pointers into the buffer itself.
    char **const old_argv = *argv;

    char **const new_argv = realloc(*argv, sizeof(*new_argv) * (argc + 2) + str_size + new_len + 1);
    if(!new_argv) {
        va_end(ap);
        return -1;
    }
    *argv = new_argv;

    // Pre-existing argument string will be moved to make room for another item
    // in `char **argv`.
    char *const src = (char *)(*argv + argc + 1);
    char *const dst = (char *)(*argv + argc + 2);

    // New argument string.
    char *const last = dst + str_size;
    const int r = vsnprintf(last, new_len + 1, fmt, ap);
    va_end(ap);
    if(r < 0) {
        return -1;
    } else if(r > new_len) {
        errno = EOVERFLOW;
        return -1;
    }

    // Move pre-existing argument strings.
    memmove(dst, src, str_size);
    // Fix pre-existing argv pointers.
    char *const old_strs = (char *)(old_argv + argc + 1);
    for(size_t i = 0; i < argc; ++i) {
        if(old_strs <= (*argv)[i] && (*argv)[i] < old_strs + str_size) {
            size_t rel_ptr = (*argv)[i] - (char *)old_argv;
            (*argv)[i] = (char *)*argv + rel_ptr + sizeof(**argv);
        }
    }

    // Append new argument.
    (*argv)[argc] = last;
    (*argv)[argc + 1] = NULL;

    return 0;
}

#ifdef CMDLINE_LISTENER
static int cmdline_append_common(char ***argv, const struct cmdline_opts *cmdline) {
    char str_addr[FORMAT_SOCKADDR_BUFLEN];
    return (
        !format_sockaddr(str_addr, (struct sockaddr *)&cmdline->upstream_addr)
        || argv_append(argv, "--upstream-address=%s", str_addr) < 0
        || argv_append(argv, "--shared-memory-fd=%d", cmdline->shared_mem_fd) < 0
#ifndef SHARED_MEMORY_RELOCATABLE
        || argv_append(argv, "--shared-memory-addr=%p", (void *)cmdline->shared_mem_addr) < 0
#endif
    ) ? -1 : 0;
}

char **cmdline_to_listener_argv(struct cmdline_opts *cmdline) {
    char **argv = NULL;
    if(
        argv_append(&argv, "%s", cmdline->listener) < 0
        || argv_append(&argv, "--listener=%s", cmdline->listener) < 0
        || argv_append(&argv, "--worker=%s", cmdline->worker) < 0
        || cmdline_append_common(&argv, cmdline) < 0
        || argv_append(&argv, "--ipc-broadcast=%d,%d", cmdline->ipc_broadcast[0], cmdline->ipc_broadcast[1]) < 0
    ) {
        goto error;
    }
    for(size_t i = 0, n = worker_process_array_len(&cmdline->worker_procs); i < n; ++i) {
        struct worker_process *proc = worker_process_array_get(&cmdline->worker_procs, i);
        assert(proc);
        if(proc->ipc_fd < 0 || proc->pid_fd < 0 || proc->pid < 0) {
            continue;
        }
        if(argv_append(&argv, "--worker-process=%d,%d,%d", proc->ipc_fd, proc->pid_fd, (int)proc->pid) < 0) {
            goto error;
        }
    }
    for(size_t i = 0; i < cmdline->num_listen_fds; ++i) {
        if(argv_append(&argv, "--listen-fd=%d", cmdline->listen_fds[i]) < 0) {
            goto error;
        }
    }
    return argv;

error:
    free(argv);
    return NULL;
}

char **cmdline_to_worker_argv(const struct cmdline_opts *cmdline, int ipc_broadcast, int ipc_direct) {
    char **argv = NULL;
    char str_addr[FORMAT_SOCKADDR_BUFLEN];
    if(
        !format_sockaddr(str_addr, (struct sockaddr *)&cmdline->upstream_addr)
#ifdef USE_VALGRIND
        || argv_append(&argv, "valgrind") < 0
#endif
        || argv_append(&argv, "%s", cmdline->worker) < 0
        || (
            ipc_broadcast >= 0
            && argv_append(&argv, "--ipc-broadcast=%d", ipc_broadcast) < 0
        )
        || argv_append(&argv, "--ipc-direct=%d", ipc_direct) < 0
        || cmdline_append_common(&argv, cmdline) < 0
    ) {
        goto error;
    }
    return argv;

error:
    free(argv);
    return NULL;
}
#endif
