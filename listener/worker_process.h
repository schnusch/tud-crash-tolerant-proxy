#ifndef LISTNER_WORKER_PROCESS_H
#define LISTNER_WORKER_PROCESS_H

#include "fd_info.h"
#include "../common/shared_memory.h"

/**
 * Strictly typed alias for `struct shared_memory_mapping`.
 */
struct worker_process_array {
    struct shared_memory_mapping map;
};

struct worker_process {
    int ipc_fd;
    int pid_fd;
    pid_t pid;
};

/**
 * Create and initialize the memfd.
 */
int worker_process_array_init(struct worker_process_array *arr);

/**
 * **TODO**
 */
size_t worker_process_array_len(struct worker_process_array *arr);

/**
 * **TODO**
 * \return `NULL` on error
 * \return a pointer to the entry
 */
struct worker_process *worker_process_array_get(struct worker_process_array *arr, size_t i);

/** **TODO** */
struct epoll_context {
    int epfd;
    struct fd_info *fd_info;
    size_t num_fds;
};

struct cmdline_opts;

/**
 * Spawn a new worker process.
 */
pid_t worker_process_spawn(
    struct epoll_context *ctx,
    struct worker_process *proc,
    size_t i,
    const struct cmdline_opts *cmdline,
    int ipc_broadcast
);

int worker_process_epoll_add(
    struct epoll_context *ctx,
    int fd,
    uint32_t events,
    const struct fd_info *new_info
);

#endif
