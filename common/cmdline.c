#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cmdline.h"
#include "util.h"

enum getopt_result {
    OPT_END = -1,
    OPT_NULL = 0,
#ifdef CMDLINE_LISTENER
    OPT_LISTEN_ADDR = 'l',
#endif
    OPT_UNKNOWN = '?',
    OPT_HELP = 256,
    OPT_UPSTREAM_ADDR,
    OPT_LISTEN_FD,
    OPT_SHARED_FD,
#ifndef SHARED_MEMORY_RELOCATABLE
    OPT_UPSTREAM_ADDR,
#endif
    OPT_NUM_WORKERS,
#ifdef CMDLINE_WORKER
    OPT_IPC_BROADCAST,
    OPT_IPC_DIRECT,
#endif
    OPT_PID_FD,
    OPT_LISTENER_BIN,
    OPT_WORKER_BIN,
};

static const struct option long_options[] = {
    {"help", no_argument, NULL, OPT_HELP},
    {"shared-memory-fd", required_argument, NULL, OPT_SHARED_FD},
#ifndef SHARED_MEMORY_RELOCATABLE
    {"shared-memory-address", required_argument, NULL, OPT_SHARED_ADDR},
#endif
    {"upstream-address", required_argument, NULL, OPT_UPSTREAM_ADDR},
#ifdef CMDLINE_WORKER
    {"ipc-broadcast", required_argument, NULL, OPT_IPC_BROADCAST},
    {"ipc-direct", required_argument, NULL, OPT_IPC_DIRECT},
#endif
#ifdef CMDLINE_LISTENER
    {"listen", required_argument, NULL, OPT_LISTEN_ADDR},
    {"listen-fd", required_argument, NULL, OPT_LISTEN_FD},
    {"num-workers", required_argument, NULL, OPT_NUM_WORKERS},
    {"listener", required_argument, NULL, OPT_LISTENER_BIN},
    {"worker", required_argument, NULL, OPT_WORKER_BIN},
#endif
    {NULL, 0, NULL, 0},
};

static const struct {
    int val;
    const char *description;
} help_texts[] = {
    {OPT_HELP, "display this help and exit"},
    {OPT_SHARED_FD, "file descriptor of the shared memory"},
#ifndef SHARED_MEMORY_RELOCATABLE
    {OPT_SHARED_ADDR, "address of the shared memory"},
#endif
    {OPT_UPSTREAM_ADDR, "upstream address to forward incoming connections to"},
#ifdef CMDLINE_WORKER
    {OPT_IPC_BROADCAST, "UNIX socket connecting the listener process to all its worker processes"},
    {OPT_IPC_DIRECT, "UNIX socket connecting the listener process to a single worker process"},
#endif
#ifdef CMDLINE_LISTENER
    {OPT_LISTEN_ADDR, "listen address"},
    {OPT_LISTEN_FD, "listening file descriptors on which incoming connections are accepted"},
    {OPT_NUM_WORKERS, "number of worker processes"},
    {OPT_LISTENER_BIN, "path to the executable of the listener"},
    {OPT_WORKER_BIN, "path to the executable of the worker"},
#endif
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

void free_cmdline(struct cmdline_opts *cmdline) {
#ifdef CMDLINE_LISTENER
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
#ifdef CMDLINE_LISTENER
        (size_t)0, // num_listen_addrs
        (struct sockaddr_storage *)NULL, // listen_addrs
        (size_t)0, // num_listen_fds
        (int *)NULL, // listen_fds
        (unsigned int)1, // num_workers
        (char *)NULL, // listener
        (char *)NULL, // worker
#endif
    };

    for (enum getopt_result opt; (opt = getopt_long(argc, argv, "l", long_options, NULL)) != OPT_END;) {
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
#ifdef CMDLINE_WORKER
        case OPT_IPC_BROADCAST:
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
#ifdef CMDLINE_LISTENER
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
        case OPT_LISTEN_ADDR:
            // Parse and append listening address.
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
        case OPT_NUM_WORKERS:
            // Parse number of workers.
            {
                int e;
                cmdline->num_workers = strtol_limit(&e, optarg, 1, UINT_MAX);
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

#ifdef CMDLINE_WORKER
    if(cmdline->ipc_direct < 0) {
        fprintf(stderr, "%s: missing required option: --ipc-direct\n", argv[0]);
        return -1;
    }
    if(cmdline->upstream_addr.ss_family == (sa_family_t)-1) {
        fprintf(stderr, "%s: missing required option: --upstream-address\n", argv[0]);
        return -1;
    }
#endif

#ifndef SHARED_MEMORY_RELOCATABLE
    if((cmdline->shared_mem_fd == -1) ^ (cmdline->shared_mem_addr == NULL)) {
        fprintf(stderr, "%s: either both or neither of --shared-memory-fd and --shared-memory-address must be specified\n", argv[0]);
        return -1;
    }
#endif

#if 1
    LOG("%c", '$');
    for(int i = 0; i < argc; ++i) {
        fputc(' ', stderr);
        fputs(argv[i], stderr);
    }
    fputc('\n', stderr);
    list_fds("parse_cmdline");
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
