#ifndef LISTENER_LISTEN_H
#define LISTENER_LISTEN_H

#include "cmdline.h"

/**
 * If `STDIN_FILENO` is a listen file descriptor, dup(2) it and use `/dev/null`
 * as the new `STDIN_FILENO`.
 */
int ensure_stdio_no_listen(struct cmdline_opts *cmdline);

/**
 * Extend `cmdline_opts::listen_fds` by the file descriptors returned by
 * `sd_listen_fds`.
 */
int add_systemd_listen_fds(struct cmdline_opts *cmdline, int unset);

/**
 * Bind to `listen_addrs` and extend `listen_fds`.
 */
int bind_listen_addrs(struct cmdline_opts *cmdline);

#endif
