#include <assert.h>
#include <errno.h>
#include <stdatomic.h>

#include "accept.h"
#include "util.h"

static struct connection *get_empty_connection(struct shared_memory_mapping *map) {
    for(size_t i = 0;; ++i) {
        struct connection *conn = shared_memory_get_or_append_connection(map, i);
        if(!conn || atomic_load_explicit(&conn->state, memory_order_acquire) == CONN_UNUSED) {
            return conn;
        }
    }
}

struct connection *accept_connection(struct shared_memory_mapping *map, int listen_fd) {
    // Find a new slot for the incoming connection and set its
    // state to CONN_ACCEPTING.
    struct connection *conn;
    int old_state;
    do {
        conn = get_empty_connection(map);
        if(!conn) {
            perror("shared_memory_get_or_append_connection");
            return NULL;
        }
        old_state = CONN_UNUSED;
    } while(
        !atomic_compare_exchange_strong_explicit(
            &conn->state,
            &old_state,
            CONN_ACCEPTING,
            memory_order_release,
            memory_order_relaxed
        )
    );

    // Accept connection.
    conn->downstream.addrlen = sizeof(conn->downstream.addr);
    int conn_fd = accept(
        listen_fd,
        (struct sockaddr *)&conn->downstream.addr,
        &conn->downstream.addrlen
    );
    // TODO error hook
    if(conn_fd < 0) {
        perror("accept");
        return NULL;
    }
    assert(conn->downstream.addrlen <= sizeof(conn->downstream.addr));

    // Save file descriptor.
    // Don't forget to reset `*.shutdown`.
    conn->downstream = (struct connection_endpoint){ .fd = { conn_fd, -1 } };
    conn->upstream   = (struct connection_endpoint){ .fd = { -1, -1 } };
    conn->worker_pid = -1;

    return conn;
}
