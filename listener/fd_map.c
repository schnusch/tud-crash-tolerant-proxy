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

    struct fd_map_entry *start = (struct fd_map_entry *)CAST(map)->addr->data;
    struct fd_map_entry *end = (struct fd_map_entry *)((char *)CAST(map)->addr + CAST(map)->addr->size);
    assert(((uintptr_t)end - (uintptr_t)start) % sizeof(struct fd_map_entry) == 0);

    struct fd_map_entry entry = {
        .used = 0,
    };
    while(fd >= end - start) {
        size_t size = sizeof(entry);
        if(writeall(CAST(map)->fd, &entry, &size) < 0) {
            shared_memory_truncate(CAST(map), CAST(map)->addr->size);
            return NULL;
        }
        CAST(map)->addr->size += sizeof(entry);
        ++end;
    }

    if(shared_memory_resize(CAST(map)) < 0) {
        shared_memory_truncate(CAST(map), CAST(map)->addr->size - sizeof(entry));
        return NULL;
    }

    start = (struct fd_map_entry *)CAST(map)->addr->data;
    return &start[fd];
}

int fd_map_truncate(struct fd_map *map) {
    struct fd_map_entry *start = (struct fd_map_entry *)CAST(map)->addr->data;
    struct fd_map_entry *end = (struct fd_map_entry *)((char *)CAST(map)->addr + CAST(map)->addr->size);
    assert(((uintptr_t)end - (uintptr_t)start) % sizeof(struct fd_map_entry) == 0);

    while(end > start && !end[-1].used) {
        --end;
    }
    int r = shared_memory_truncate(CAST(map), (char *)end - (char *)CAST(map)->addr);

    start = (struct fd_map_entry *)CAST(map)->addr->data;
    end = (struct fd_map_entry *)((char *)CAST(map)->addr + CAST(map)->addr->size);
    assert(((uintptr_t)end - (uintptr_t)start) % sizeof(struct fd_map_entry) == 0);

    return r;
}
