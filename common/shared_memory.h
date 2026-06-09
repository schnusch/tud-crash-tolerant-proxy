#ifndef COMMON_SHARED_MEMORY_H
#define COMMON_SHARED_MEMORY_H

#include <sys/socket.h>

/**
 * File descriptors will differ in the two processes. `fd_pair_t[0]` refers to
 * the keeper's file descriptor, `fd_pair_t[1]` to the worker's.
 */
typedef int fd_pair_t[2];

struct open_connection {
    /** File descriptors of the accepted connection. */
    fd_pair_t connection_fd;

    /** Size of the accepted connection's address. */
    socklen_t addrlen;
    /** Accepted connection's address. */
    struct sockaddr_storage addr;
};

/**
 * Header of the shared memory.
 */
struct shared_memory {
    /** Store the size of the memfd buffer, including the size. */
    size_t size;
    /** Store data per connection. */
    char data[];
};

/**
 * Keep track of the address, length, and file descriptor of the mapping of the
 * shared memory.
 */
struct shared_memory_mapping {
    struct shared_memory *addr;
    size_t length;
    int fd;
};

/**
 * Initialize mapping of shared memory in `fd` at `addr`. Create a new memfd
 * if `fd` less than zero.
 */
int shared_memory_open(struct shared_memory_mapping *map, int fd, struct shared_memory *hint);

/**
 * Update the mapping of the shared memory from the size stored its header.
 */
int shared_memory_resize(struct shared_memory_mapping *map);

/**
 * **TODO**
 */
int shared_memory_truncate(struct shared_memory_mapping *map, size_t size);

ssize_t shared_memory_alloc_slot(struct shared_memory_mapping *map);

#endif
