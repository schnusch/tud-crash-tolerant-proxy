#ifndef LISTENER_PIDFD_OPEN_H
#define LISTENER_PIDFD_OPEN_H

#include <sys/types.h>

/**
 * see [pidfd_open(2)](https://man7.org/linux/man-pages/man2/pidfd_open.2.html)
 */
int pidfd_open(pid_t pid, unsigned int flags);

/**
 * see [pidfd_send_signal(2)](https://man7.org/linux/man-pages/man2/pidfd_send_signal.2.html)
 */
int pidfd_send_signal(int pidfd, int sig, siginfo_t *info, unsigned int flags);

#endif
