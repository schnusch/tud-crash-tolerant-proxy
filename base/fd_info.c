#include <errno.h>

#include "fd_info.h"
#include "../common/util.h"

struct fd_info *fd_info_get(struct fd_info **fd_info, size_t *num_fds, int fd) {
    if(fd < 0) {
        errno = EINVAL;
        return NULL;
    }
    while((size_t)fd >= *num_fds) {
        static const struct fd_info empty = {
            .type = FD_TYPE_UNKNOWN,
#ifdef FD_INFO_WORKER
            .events = 0,
#endif
            .slot = -1,
        };
        if(array_append((void **)fd_info, num_fds, &empty, sizeof(empty)) < 0) {
            return NULL;
        }
    }
    return &(*fd_info)[fd];
}
