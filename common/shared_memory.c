#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h> // memfd_create, mmap
#include <sys/socket.h>
#include <unistd.h>

#include "shared_memory.h"
#include "util.h"

static char *dump_shared_fd(const fd_pair_t fds) {
    char *dump = NULL;
    if(asprintf(&dump, "{ \"keeper\": %d, \"worker\": %d }", fds[0], fds[1]) < 0) {
        free(dump);
        return NULL;
    }
    return dump;
}

int shared_memory_open(struct shared_memory_mapping *map, int fd, struct shared_memory *hint) {
#ifndef SHARED_MEMORY_RELOCATABLE
    if(hint) {
        errno = EINVAL;
        return -1;
    }
#endif

    map->fd = fd;
    if(map->fd < 0) {
        map->fd = memfd_create("shared_state", 0);
        if(map->fd < 0) {
            return -1;
        }

        // initialize shared memory
        static const struct shared_memory init = {
            .size = sizeof(init),
        };
        size_t size = sizeof(init);
        if(writeall(map->fd, (void *)&init, &size) < 0) {
            goto error;
        }
    }

    int flags = MAP_SHARED;
    if(hint) {
        flags |= MAP_FIXED_NOREPLACE;
    }
    map->length = sizeof(*map->addr);
    map->addr = mmap((void *)hint, map->length, PROT_READ | PROT_WRITE, flags, map->fd, 0LL);
    if(map->addr == MAP_FAILED) {
        goto error;
    }
    if(shared_memory_resize(map) < 0) {
        goto error;
    }
    return 0;

error:
    if(fd < 0) {
        close(map->fd);
        map->fd = -1;
    }
    return -1;
}

int shared_memory_resize(struct shared_memory_mapping *map) {
    uint64_t new_length = map->addr->size;
    if(new_length == map->length) {
        return 0;
    }

#ifdef NO_USE_MREMAP
    // create new mapping with the new size
    struct shared_memory *new_addr = mmap(NULL, new_length, PROT_READ | PROT_WRITE, MAP_SHARED, map->fd, 0LL);
    if(new_addr == MAP_FAILED) {
        return -1;
    }

    // unmap old mapping
    if(munmap(map->addr, map->length) < 0) {
        // failed to unmap the old mapping
        munmap(new_addr, new_length);
        return -1;
    }
#else
    // Without MREMAP_MAYMOVE the mremap(2) will fail if mapping cannot be
    // resized while keeping its address.
    struct shared_memory *new_addr = mremap((void *)map->addr, map->length, new_length, 0);
    if(new_addr == MAP_FAILED) {
        return -1;
    }
#endif

    map->addr = new_addr;
    map->length = new_length;
    return 0;
}

int shared_memory_truncate(struct shared_memory_mapping *map, size_t size) {
    if(ftruncate(map->fd, size) < 0) {
        return -1;
    }
    map->addr->size = size;
    return 0;
}
