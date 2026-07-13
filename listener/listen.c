#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include "listen.h"
#include "../common/util.h"

static int dup_and_devnull(int *fd, int flags) {
    int r = 0;
    int null_fd = -1, dup_fd = -1;

    dup_fd = dup(*fd);
    if(dup_fd < 0) {
        perror("dup");
        goto cleanup;
    }
    assert(dup_fd != *fd);

    null_fd = open("/dev/null", flags | O_CLOEXEC);
    if(null_fd < 0) {
        perror("open");
        goto cleanup;
    }
    assert(null_fd != *fd);
    if(dup2(null_fd, *fd) < 0) {
        perror("dup2");
        goto cleanup;
    }
    *fd = dup_fd;
    dup_fd = -1;

cleanup:
    if(closep(&null_fd) < 0) {
        perror("close");
        r = -1;
    }
    if(closep(&dup_fd) < 0) {
        perror("close");
        r = -1;
    }
    return r;
}

int ensure_stdio_no_listen(struct cmdline_opts *cmdline) {
    for(size_t i = 0; i < cmdline->num_listen_fds; ++i) {
        if(
            (
                cmdline->listen_fds[i] == STDIN_FILENO
                || cmdline->listen_fds[i] == STDOUT_FILENO
                || cmdline->listen_fds[i] == STDERR_FILENO
            ) && dup_and_devnull(
                &cmdline->listen_fds[i],
                cmdline->listen_fds[i] == STDIN_FILENO ? O_RDONLY : O_WRONLY
            ) < 0
        ) {
            return -1;
        }
    }
    return 0;
}

int add_systemd_listen_fds(struct cmdline_opts *cmdline, int unset) {
#ifdef HAVE_SYSTEMD
    // Use listening sockets from systemd.
    int new_listen_fds = sd_listen_fds(unset);
    if(new_listen_fds < 0) {
        errno = new_listen_fds;
        return -1;
    } else if(new_listen_fds == 0) {
        return 0;
    }

    int *listen_fds = realloc(
        cmdline->listen_fds,
        (cmdline->num_listen_fds + new_listen_fds) * sizeof(*listen_fds)
    );
    if(!listen_fds) {
        return -1;
    }
    cmdline->listen_fds = listen_fds;

    int r = 0;
    size_t dst = cmdline->num_listen_fds;
    for(size_t i = 0; i < cmdline->num_listen_fds; ++i) {
        int listen_fd = SD_LISTEN_FDS_START + i;
        int is_socket = sd_is_socket(listen_fd, AF_UNSPEC, SOCK_STREAM, 1);
        if(is_socket < 0) {
            errno = -is_socket;
            r = -1;
        } else if(is_socket) {
            // sd_listen_fds sets FD_CLOEXEC, but the worker process needs to
            // inherit them.
            // https://github.com/systemd/systemd/blob/v214/src/libsystemd/sd-daemon/sd-daemon.c#L75
            if(fd_cloexec(listen_fd, 0) < 0) {
                r = -1;
            }
            cmdline->listen_fds[dst++] = listen_fd;
        } else {
            LOG(LOG_INFO, "closing unsupported systemd file descriptor %d", listen_fd);
            if(closep(&listen_fd) < 0) {
                perror("close");
                r = -1;
            }
        }
    }
    cmdline->num_listen_fds = dst;
    return r;
#else
    return 0;
#endif
}

int bind_listen_addrs(struct cmdline_opts *cmdline) {
    for(size_t i = 0; i < cmdline->num_listen_addrs; ++i) {
        // Bind to listening address.
        const struct sockaddr_storage *addr = &cmdline->listen_addrs[i];
        int listen_fd = socket(addr->ss_family, SOCK_STREAM, 0);
        if(listen_fd < 0) {
            perror("socket");
            return -1;
        }
        const int one = 1;
        if(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
            perror("setsockopt(SO_REUSEADDR");
        }
        if(bind(listen_fd, (struct sockaddr *)addr, sizeof(*addr)) < 0) {
            perror("bind");
            return -1;
        }
        if(listen(listen_fd, SOMAXCONN) < 0) {
            perror("listen");
            return -1;
        }

        // Append listen_fd.
        if(append_fd(&cmdline->listen_fds, &cmdline->num_listen_fds, listen_fd) < 0) {
            perror("realloc");
            return -1;
        }
    }
    return 0;
}
