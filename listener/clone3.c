#include <sys/syscall.h>
#include <unistd.h>

#include "clone3.h"

long clone3(struct clone_args *cl_args, size_t size) {
    return syscall(SYS_clone3, cl_args, size);
}
