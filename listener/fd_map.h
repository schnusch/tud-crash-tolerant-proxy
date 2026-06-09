#ifndef LISTENER_FD_MAP_H
#define LISTENER_FD_MAP_H

#include "../common/shared_memory.h"

/**
 * Strictly typed alias for `struct shared_memory_mapping`.
 */
struct fd_map {
    struct shared_memory_mapping map;
};

struct fd_map_entry {
    /** TODO */
    int used;
    /**
     * > The name may consist of arbitrary ASCII characters except control
     * > characters or ":". It may not be longer than 255 characters. If a
     * > submitted name does not follow these restrictions, it is ignored.
     */
    char sd_name[256];
    // TODO
};

/**
 * Create and initialize the memfd.
 */
int fd_map_init(struct fd_map *map);

/**
 * **TODO**
 * \return `NULL` on error
 * \return a pointer to the entry
 */
struct fd_map_entry *fd_map_get(struct fd_map *map, int fd);

/**
 * Truncate unused entries at the end of the `struct fd_map`.
 */
int fd_map_truncate(struct fd_map *map);

#endif
