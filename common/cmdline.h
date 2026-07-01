#ifndef COMMON_CMDLINE_H
#define COMMON_CMDLINE_H

#include <stdio.h>

#include "shared_memory.h"

struct cmdline_opts {
    /** File descriptor of the shared memory. */
    int shared_mem_fd;
    /** Address of the shared memory. */
    struct shared_memory *shared_mem_addr;
    /** Upstream address to forward incoming connections to. */
    struct sockaddr_storage upstream_addr;
#ifdef CMDLINE_WORKER
    /**
     * A single UNIX socket connecting the listener process to all its worker
     * processes.
     */
    int ipc_broadcast;
    /**
     * A UNIX socket connecting the listener process to a single worker process.
     */
    int ipc_direct;
#endif
#ifdef CMDLINE_LISTENER
    size_t num_listen_addrs;
    struct sockaddr_storage *listen_addrs;
    /** Number of items in `listen_fds`. */
    size_t num_listen_fds;
    /** Listening file descriptors. */
    int *listen_fds;
    /** Number of worker processes. */
    unsigned int num_workers;
    /** Path to the executable of the keeper. */
    const char *listener;
    /** Path to the executable of the worker. */
    const char *worker;
#endif
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
 * Print command line usage.
 */
void usage(FILE *stream, const char *argv0);

/**
 * Print command line usage and help.
 */
void help(FILE *stream, const char *argv0);

#endif
