#include <assert.h>
#include <errno.h>
#include <stdio.h> // snprintf
#include <stdlib.h> // alloca
#include <sys/mman.h> // memfd_create, mmap
#include <sys/stat.h>
#include <unistd.h>

#include "shared_memory.h"
#include "util.h"

char *str_state(char *buf, size_t size, int state) {
    char *const end = buf + size;
    char *p = buf;
    const char *src;
    switch(state & CONN_STATE_BITS) {
#define CASE(x) case x: src = #x; break;
        CASE(CONN_UNUSED)
        CASE(CONN_ACCEPTING)
        CASE(CONN_CONNECTING)
        CASE(CONN_POLL)
        CASE(CONN_SWAP_BUFFERS)
        CASE(CONN_CLOSING)
        CASE(CONN_ERROR)
#undef CASE
    default:
        goto more;
    }
    p += strlcpy(p, src, end - p);
    if(p >= end) {
trunc:
        if(size >= 4) {
            p = end - 4;
        } else {
            p = buf;
        }
        strlcpy(p, "...", end - p);
        return buf;
    }
    if((state & ~CONN_STATE_BITS) == 0) {
        return buf;
    }
    state &= ~CONN_SWAP_BUFFERS;
    p += strlcpy(p, " | ", end - p);
    if(p >= end) {
        goto trunc;
    }
more:
    ;
    int n = snprintf(p, end - p, "0x%03X", state);
    if(n < 0 || end - p <= n) {
        goto trunc;
    }
    return buf;
}

int shared_memory_open(struct shared_memory_mapping *map, int fd) {
    static const struct shared_memory init = {
        .size = sizeof(init),
    };
    static const struct iovec iov = {
        .iov_base = &init,
        .iov_len = offsetof(struct shared_memory, connections),
    };

    map->fd = fd;
    if(map->fd < 0) {
        map->fd = memfd_create("connections", 0);
        if(map->fd < 0) {
            return -1;
        }
init:
        // Initialize shared memory.
        if(pwritev_all(map->fd, &iov, 1, 0) < sizeof(init)) {
            goto error;
        }
    } else {
        // Ensure the shared memory is not truncated.
        struct stat st;
        if(fstat(map->fd, &st) < 0) {
            goto error;
        }
        if((size_t)st.st_size < sizeof(init)) {
            goto init;
        }
    }

    map->length = sizeof(*map->addr);
    map->addr = mmap(NULL, map->length, PROT_READ | PROT_WRITE, MAP_SHARED, map->fd, 0LL);
    if(map->addr == MAP_FAILED) {
        goto error;
    }
    if(shared_memory_resize(map) < 0) {
        goto error;
    }
    return 0;

error:
    if(fd < 0) {
        int errnum = errno;
        closep(&map->fd);
        errno = errnum;
    }
    return -1;
}

int shared_memory_resize(struct shared_memory_mapping *map) {
    uint64_t new_length = map->addr->size;
    if(new_length == map->length) {
        return 0;
    }

    // Without MREMAP_MAYMOVE the mremap(2) will fail if mapping cannot be
    // resized while keeping its address.
    struct shared_memory *new_addr = mremap(
        (void *)map->addr,
        map->length,
        new_length,
        MREMAP_MAYMOVE
    );
    if(new_addr == MAP_FAILED) {
        return -1;
    }

    map->addr = new_addr;
    map->length = new_length;
    return 0;
}

int shared_memory_append(
    struct shared_memory_mapping *map,
    void *entry,
    size_t size,
    size_t count
) {
    size_t old_total = atomic_load_explicit(&map->addr->size, memory_order_acquire);

    const size_t current_length = (old_total - offsetof(struct shared_memory, connections)) / size;
    struct iovec *iov = alloca(sizeof(*iov) * count);
    memset(iov, 0, sizeof(*iov) * count); // fix -Wmaybe-uninitialized
    for(size_t i = 0; i < count; ++i) {
        iov[i] = (struct iovec){
            .iov_base = entry,
            .iov_len = size,
        };
    }
    const size_t num_written = pwritev_all(
        map->fd,
        iov,
        count,
        offsetof(struct shared_memory, connections) + current_length * size
    );

    // !!! WARNING !!!
    // Potential race condition
    // A: shared_memory_append starts (old_size)
    // A: write new entries
    // B: shared_memory_append starts (old_size)
    // A: update size
    // A: work on entry at i=old_size
    // !!! RACE CONDITION !!!
    // B: entry at i=old_size is overwritten
    // B: update size

    // If two processes are competing to update `size` the process with the
    // greater `size` will win since so no newly added entries are lost. This
    // does not resolve the above race condition.
    const size_t new_total = old_total + num_written;
    while(
        !atomic_compare_exchange_weak_explicit(
            &map->addr->size,
            &old_total,
            new_total,
             // TODO memory_order?
            memory_order_release,
            memory_order_relaxed
        )
        && old_total < new_total
    ) { }

    if(num_written < count * size || shared_memory_resize(map) < 0) {
        return -1;
    } else {
        return 0;
    }
}

struct connection *shared_memory_get_connection(struct shared_memory_mapping *map, size_t slot) {
    if(shared_memory_resize(map) < 0) {
        return NULL;
    }
    size_t current_length = (map->addr->size - offsetof(struct shared_memory, connections)) / sizeof(*map->addr->connections);
    if(slot >= current_length) {
        return NULL;
    }
    return &map->addr->connections[slot];
}

struct connection *shared_memory_get_or_append_connection(struct shared_memory_mapping *map, size_t slot) {
    struct connection *conn = shared_memory_get_connection(map, slot);
    if(conn) {
        return conn;
    }
    size_t current_length = (map->addr->size - offsetof(struct shared_memory, connections)) / sizeof(*map->addr->connections);
    if(
        shared_memory_append(
            map,
            &(struct connection){
                .state = CONN_UNUSED,
            },
            sizeof(struct connection),
            slot + 1 - current_length
        ) < 0
    ) {
        return NULL;
    }
    return &map->addr->connections[slot];
}

int shared_memory_truncate(struct shared_memory_mapping *map, size_t size) {
    if(size > map->addr->size) {
        errno = EINVAL;
        return -1;
    }
    if(ftruncate(map->fd, size) < 0) {
        return -1;
    }
    map->addr->size = size;
    return shared_memory_resize(map);
}
