#ifndef WORKER_TRANSFORM_H
#define WORKER_TRANSFORM_H

#include <stdbool.h>

#include "atomic_send_recv.h"

/**
 * Protocol dependent state.
 */
typedef struct {
    /** Index of `copies`. */
    bool active;
    /**
     * Double-buffered state used by `transform`. `copies[active]` is the
     * input, `copies[!active]` is the output. `transform` must not modify
     * `copies[active]` to avoid corruption.
     */
    struct {
        enum {
            HTTP_GOT_REQUEST = 1 << 0,
            HTTP_GOT_RESPONSE = 1 << 1,
        } state;
    } copies[
#ifdef PERFORMANCE_BASELINE
        1
#else
        2
#endif
    ];
} transformation_context_t;

struct transformation_direction {
    /** Ring buffer's internal buffer of size `RING_BUFFER_SIZE` used as output. */
    char *out_buf;
    struct ring_buffer_range *out_range;
    /** Ring buffer's internal buffer of size `RING_BUFFER_SIZE` used as input. */
    char *in_buf;
    struct ring_buffer_range *in_range;
    /** EOF was reached on the read end. */
    int eof;
    /**
     * If set to non-zero by `transform` the send buffers will be drained and
     * `shutdown(SHUT_WR)` will be called.
     */
    int shutdown;
};

int transform(
    int slot,
    transformation_context_t *ctx,
    struct transformation_direction *down,
    struct transformation_direction *up
);

#endif
