#ifndef COMMON_FD_INFO_H
#define COMMON_FD_INFO_H

#include <stddef.h>
#include <stdint.h>

/**
 * Data associated with a descriptors.
 */
struct fd_info {
    /** Type of the file descriptor. */
    enum {
        FD_TYPE_UNKNOWN = 0,
        /** IPC socket */
        FD_TYPE_IPC,
        /** Signal FD */
        FD_TYPE_SIGNAL,
        /** Connection of the proxy. */
        FD_TYPE_CONN,
#ifdef FD_INFO_LISTENER
        FD_TYPE_LISTEN,
        FD_TYPE_PID,
        FD_TYPE_STDIO,
#endif
    } type;
#ifdef FD_INFO_WORKER
    /** epoll events currently registered for `fd`. */
    uint32_t events;
#endif
    /** Used by `shared_memory_get_connection`. */
    size_t slot;
};

/**
 * \return associated data for file descriptor `fd`
 */
struct fd_info *fd_info_get(struct fd_info **fd_info, size_t *num_fds, int fd);

#endif
