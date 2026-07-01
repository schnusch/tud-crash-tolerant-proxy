#ifndef WORKER_TRANSFORM_H
#define WORKER_TRANSFORM_H

#include "atomic_send_recv.h"

/**
 * Protocol dependent state.
 */
typedef struct {
} transformation_context_t;

struct transformation_direction {
    /** Ring buffer's internal buffer of size `RING_BUFFER_SIZE` used as output. */
    char *out_buf;
    struct ring_buffer_range *out_range;
    /** Ring buffer's internal buffer of size `RING_BUFFER_SIZE` used as input. */
    const char *in_buf;
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
    transformation_context_t *ctx,
    struct transformation_direction *down,
    struct transformation_direction *up
);

#endif
