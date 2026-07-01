#include "transform.h"
#include "../common/util.h"

int transform(
    transformation_context_t *ctx,
    struct transformation_direction *down,
    struct transformation_direction *up
) {
    (void)ctx;
    ring_buffer_move(
        down->out_buf, down->out_range,
        down->in_buf,  down->in_range
    );
    if(down->eof) {
        down->shutdown = 1;
    }
    ring_buffer_move(
        up->out_buf, up->out_range,
        up->in_buf,  up->in_range
    );
    if(up->eof) {
        up->shutdown = 1;
    }
    return 0;
}
