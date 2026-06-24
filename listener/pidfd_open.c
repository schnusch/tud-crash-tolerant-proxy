#include <sys/syscall.h>
#include <unistd.h>

#include "pidfd_open.h"

int pidfd_open(pid_t pid, unsigned int flags) {
    return syscall(SYS_pidfd_open, pid, flags);
}
