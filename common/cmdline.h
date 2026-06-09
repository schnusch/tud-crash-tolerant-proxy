#ifndef COMMON_CMDLINE_H
#define COMMON_CMDLINE_H

#include <stdio.h>

#include "shared_memory.h"

struct cmdline_opts {
    /** File descriptor of the shared memory. */
    int shared_mem_fd;
    /** Address of the shared memory. */
    struct shared_memory *shared_mem_addr;
#ifdef CMDLINE_LISTENER
    size_t num_listen_addrs;
    struct sockaddr_storage *listen_addrs;
#endif
    /** Number of items in `listen_fds`. */
    size_t num_listen_fds;
    /** Listening file descriptors. */
    int *listen_fds;
    /** Number of worker processes. */
    unsigned int num_workers;
#ifdef CMDLINE_WORKER
    /** UNIX socket connecting the worker to the keeper process. */
    int ipc_fd;
#endif
    /** PID FD to the keeper process. Only parsed in the worker command line. */
    int parent_pidfd;
    /** Path to the executable of the keeper. */
    const char *listener;
    /** Path to the executable of the worker. */
    const char *worker;
};

/**
 * Can be used with `__attribute__((cleanup(free_cmdline)))`.
 */
void free_cmdline(struct cmdline_opts *cmdline);

/**
 * Parse `argv` and populate `opts`.
 * \return 0 on success
 * \return -1 on error
 */
int parse_cmdline(struct cmdline_opts *cmdline, int argc, char **argv);

/**
 * Convert `cmdline` to a argv array.
 * \return pointer should be freed with `free(3)`
 */
char **cmdline_to_argv(const struct cmdline_opts *cmdline, const char *argv0, int ipc_fd);

/**
 * Print command line usage.
 */
void usage(FILE *stream, const char *argv0);

/**
 * Print command line usage and help.
 */
void help(FILE *stream, const char *argv0);

#endif
