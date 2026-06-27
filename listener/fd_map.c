#include <assert.h>
#include <errno.h>
#include <stdint.h>

#include "fd_map.h"
#include "../common/shared_memory.h"
#include "../common/util.h"

#define CAST(x) (&(x)->map)

int fd_map_init(struct fd_map *map) {
    return shared_memory_open(CAST(map), -1, NULL);
}

struct fd_map_entry *fd_map_get(struct fd_map *map, int fd) {
    if(fd < 0) {
        errno = EINVAL;
        return NULL;
    }

    struct fd_map_entry *start = (struct fd_map_entry *)CAST(map)->addr->connections;
    struct fd_map_entry *end = (struct fd_map_entry *)((char *)CAST(map)->addr + CAST(map)->addr->size);
    const size_t size = (char *)end - (char *)start;
    end = (struct fd_map_entry *)(
        (char *)end - (size % sizeof(struct fd_map_entry))
    );

    if(fd >= end - start) {
        struct fd_map_entry empty = {
            .used = 0,
        };
        if(shared_memory_append(CAST(map), &empty, sizeof(empty), fd + 1 - (end - start)) < 0) {
            return NULL;
        }
        start = (struct fd_map_entry *)CAST(map)->addr->connections;
    }

    return &start[fd];
}

int fd_map_truncate(struct fd_map *map) {
    struct fd_map_entry *start = (struct fd_map_entry *)CAST(map)->addr->connections;
    struct fd_map_entry *end = (struct fd_map_entry *)((char *)CAST(map)->addr + CAST(map)->addr->size);
    assert(((uintptr_t)end - (uintptr_t)start) % sizeof(struct fd_map_entry) == 0);

    while(end > start && !end[-1].used) {
        --end;
    }
    int r = shared_memory_truncate(CAST(map), (char *)end - (char *)CAST(map)->addr);

    start = (struct fd_map_entry *)CAST(map)->addr->connections;
    end = (struct fd_map_entry *)((char *)CAST(map)->addr + CAST(map)->addr->size);
    assert(((uintptr_t)end - (uintptr_t)start) % sizeof(struct fd_map_entry) == 0);

    return r;
}
