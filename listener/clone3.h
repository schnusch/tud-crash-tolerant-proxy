#ifndef LISTENER_CLONE3_H
#define LISTENER_CLONE3_H

#include <linux/sched.h>
#include <sched.h>

/**
 * see [clone(2)](https://man7.org/linux/man-pages/man2/clone.2.html)
 */
long clone3(struct clone_args *cl_args, size_t size);

#endif
