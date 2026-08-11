#include <assert.h>
#include <errno.h>
#include <stdatomic.h>

#include "accept.h"
#include "util.h"
#include "../libcrash/libcrash.h"

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

        // Initialize everything but `conn->state`. Initialization must always
        // write the same, so no synchronization is required. Do it here so any
        // connection with `CONN_ACCEPTING` is guaranteed to be initialized.
        conn->worker_pid = -1;
        memset(&conn->transform_ctx, 0, sizeof(conn->transform_ctx)); // TODO proper initialization
        conn->downstream = (struct connection_endpoint){ .fd = { -1, -1 } };
        conn->upstream   = (struct connection_endpoint){ .fd = { -1, -1 } };

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
    if(conn_fd < 0) {
        perror("accept");
        return NULL;
    }
    assert(conn->downstream.addrlen <= sizeof(conn->downstream.addr));

    LIBCRASH(accept_post(conn_fd, (struct sockaddr *)&conn->downstream.addr, conn->downstream.addrlen));

    // Save file descriptor.
    conn->downstream.fd[0] = conn_fd;

    return conn;
}
