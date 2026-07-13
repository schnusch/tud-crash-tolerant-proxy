#include <alloca.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/util.h"

int main(int argc, char **argv) {
    assert(getopt(argc, argv, "l:") == 'l');

    // Parse launcher's argv.
    struct sockaddr_storage addr;
    assert(parse_sockaddr(&addr, optarg) == 0);
    argc -= optind;
    argv += optind;

    // Initialize listening socket.
    int listen_fd = socket(addr.ss_family, SOCK_STREAM, 0);
    assert(listen_fd >= 0);
    int one = 1;
    assert(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    assert(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(listen_fd, SOMAXCONN) == 0);

    // Stub IPC socket.
    int ipc_fd = open("/dev/null", O_RDWR);
    assert(ipc_fd >= 0);

    // Prepare worker's argv.
    char *arg_ipc, *arg_listen;
    assert(asprintf(&arg_ipc, "--ipc-direct=%d", ipc_fd) >= 0);
    assert(asprintf(&arg_listen, "--listen-fd=%d", listen_fd) >= 0);
    char **new_argv = alloca((argc + 4) * sizeof(*new_argv));
    char **new_arg = new_argv;
    *new_arg++ = WORKER_PATH;
    while(*argv) {
        *new_arg++ = *argv++;
    }
    *new_arg++ = arg_ipc;
    *new_arg++ = arg_listen;
    *new_arg = NULL;

    execvp(new_argv[0], new_argv);
    perror("execvp");
    return 1;
}
