#ifndef COMMON_SHARED_MEMORY_H
#define COMMON_SHARED_MEMORY_H

#include <stdatomic.h>

#include "../common/util.h" // _Static_assert
#include "../worker/atomic_send_recv.h" // struct atomic_ring_buffer
#include "../worker/transform.h" // transformation_state_t

enum {
    /**
     * Only the lower bits encode the actual connection state. And with
     * `connection::state` to get actual connection state.
     */
    CONN_STATE_BITS = 0xFF,
    /** Connection is unused. */
    CONN_UNUSED = 0,
    /** accept(2) is about to be called on this connection. */
    CONN_ACCEPTING,
    /** connect(2) call is outstanding. */
    CONN_CONNECTING,
    /** Receive and transform more data. */
    CONN_POLL,
    /**
     * Swap active ranges of the `rx`/`tx` buffers encoded in higher bits,
     * see `swap_buffers` and `handle_connection`.
     */
    CONN_SWAP_BUFFERS,
    /** close(2) is about to be called on all file descriptors. */
    CONN_CLOSING,
    /** Set `SO_LINGER` then close(2) the all file descriptors. */
    CONN_ERROR,
};
_Static_assert(CONN_ERROR <= CONN_STATE_BITS, "connection state does not fit in the lower bits");

/**
 * Convert string representation of `connection::state`.
 * \param buf   buffer to write string representation to
 * \param size  size of `buf`
 * \param state `connection::state`
 * \return `buf`
 */
char *str_state(char *buf, size_t size, int state);

/**
 * File descriptors will differ in the two processes. `fd_pair_t[0]` refers to
 * the listener's file descriptor, `fd_pair_t[1]` to the worker's.
 */
typedef int fd_pair_t[2];

struct connection_endpoint {
    fd_pair_t fd;
    /** Remote address of the incoming connection, set by `accept(2)`. */
    struct sockaddr_storage addr;
    /** Remote address of the incoming connection, set by `accept(2)`. */
    socklen_t addrlen;
    /** Received data. */
    struct atomic_ring_buffer rx;
    /** Pending data to send. */
    struct atomic_ring_buffer tx;
};

struct connection {
    /** State of the connection. */
    atomic_int state;
    /** PID of the worker process. */
    pid_t worker_pid;
    /** Arbitrary data for `transform`. */
    transformation_context_t transform_ctx;
    /** Accepted connection. */
    struct connection_endpoint downstream;
    /** Outgoing connection. */
    struct connection_endpoint upstream;
};

/**
 * Iterate `struct connection_endpoint *var` over `&conn->upstream` and
 * `&conn->downstream`.
 *
 * (A helper variable `struct connection_endpoint **${var}_ptr` is used.)
 */
#define FOREACH_CONNECTION_ENDPOINT(var, conn) \
    for( \
        struct connection_endpoint *var, **var##_ptr = (struct connection_endpoint *[]){ \
            &(conn)->upstream, \
            &(conn)->downstream, \
            NULL \
        }; \
        (var = *var##_ptr); \
        ++var##_ptr \
    )

// C++ does not know about atomic_size_t.
#ifdef __cplusplus
#include <atomic>
typedef std::atomic<size_t> atomic_size_t;
#endif

/**
 * Header of the shared memory.
 */
struct shared_memory {
    /** Store the size of the memfd buffer, including the size. */
    atomic_size_t size;
    /** Store data per connection. */
    struct connection connections[];
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
 * Iterate over `map->addr->connections`.
 */
#define FOREACH_CONNECTION(var, map) \
    for( \
        struct connection *var = (map)->addr->connections; \
        (size_t)((char *)(var + 1) - (char *)(map)->addr) <= (map)->length; \
        ++var \
    )

/**
 * The number of complete `struct connection` mapped in `map`.
 */
#define COUNT_CONNECTIONS(map) \
    ( \
        ((map)->length - offsetof(struct shared_memory, connections)) \
        / sizeof(*(map)->addr->connections) \
    )

/**
 * Initialize mapping of shared memory in `fd` at `addr`. Create a new memfd
 * if `fd` less than zero.
 */
int shared_memory_open(struct shared_memory_mapping *map, int fd);

/**
 * Update the mapping of the shared memory from the size stored its header.
 */
int shared_memory_resize(struct shared_memory_mapping *map);

/**
 * Truncate the shared memory to `size` bytes and update the mapping. `size`
 * must be less than the current size of the shared memory.
 */
int shared_memory_truncate(struct shared_memory_mapping *map, size_t size);

/**
 * Get the existing `struct connection` and index `slot`.
 */
struct connection *shared_memory_get_connection(struct shared_memory_mapping *map, size_t slot);

/**
 * Get the existing `struct connection` and index `slot` or append new
 * connections until `slot` is allocated and return it.
 * **MAY BE SUBJECT TO RACE CONDITIONS**
 */
struct connection *shared_memory_get_or_append_connection(struct shared_memory_mapping *map, size_t slot);

/**
 * Append `entry` of size `size` (usually `sizeof(struct connection)`) to the
 * array `shared_memory_mapping::connections` `count` times.
 */
int shared_memory_append(struct shared_memory_mapping *map, void *entry, size_t size, size_t count);

#define connection_status_all(map, highlight) _connection_status_all(alloca(COUNT_CONNECTIONS(map) + 1), map, highlight)
char *_connection_status_all(char *buf, struct shared_memory_mapping *map, size_t highlight);

#endif
