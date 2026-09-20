#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ASSERT_PERROR(COND) \
    do { \
        if(!__builtin_expect(!!(COND), 1)) { \
            perror(#COND); \
            abort(); \
        } \
    } while(0)

void timeval_sub(struct timeval *result, const struct timeval *a, const struct timeval *b) {
    result->tv_sec = a->tv_sec - b->tv_sec;
    result->tv_usec = a->tv_usec - b->tv_usec;
    if(result->tv_usec < 0) {
        --result->tv_sec;
        result->tv_usec += 1000000L;
    }
}

int create_listen_sock(struct sockaddr *addr, socklen_t len) {
    int listen_fd = socket(addr->sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_PERROR(listen_fd >= 0);
    const int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ASSERT_PERROR(bind(listen_fd, addr, len) == 0);
    ASSERT_PERROR(listen(listen_fd, SOMAXCONN) == 0);
    return listen_fd;
}

int main(int argc, char **argv) {
    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_addr = { .s_addr = INADDR_ANY },
        .sin_port = htons(80),
    };
    int listen_fd = create_listen_sock((struct sockaddr *)&listen_addr, sizeof(listen_addr));
    if(dup2(listen_fd, STDIN_FILENO) >= 0) {
        close(listen_fd);
        listen_fd = STDIN_FILENO;
    }

    struct sockaddr_in proxy_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
    };
    assert(argc > 1);
    ASSERT_PERROR(inet_pton(proxy_addr.sin_family, argv[1], &proxy_addr.sin_addr) == 1);

    // Create idle connections.
    long concurrent = 1;
    if(argc > 2) {
        errno = 0;
        concurrent = strtol(argv[2], NULL, 0);
        ASSERT_PERROR(errno == 0);
    }
    for(long i = 1; i < concurrent; ++i) {
        int conn_out = socket(listen_addr.sin_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
        ASSERT_PERROR(conn_out >= 0);
        ASSERT_PERROR(connect(conn_out, (struct sockaddr *)&proxy_addr, sizeof(proxy_addr)) == 0);

        int conn_in = accept(listen_fd, NULL, 0);
        ASSERT_PERROR(conn_in >= 0);

        ASSERT_PERROR(write(conn_out, "?", 1) == 1);
        ASSERT_PERROR(write(conn_in, "?", 1) == 1);

        fprintf(stderr, "%ld idle\n", i);
    }

    // Sleep before creating final connection.
    if(argc > 3) {
        struct timespec t = { .tv_nsec = 0 };
        errno = 0;
        t.tv_sec = strtol(argv[3], NULL, 0);
        ASSERT_PERROR(errno == 0);
        while(nanosleep(&t, &t) < 0) {
            ASSERT_PERROR(errno == EINTR);
        }
    }

    fd_set set;
    struct timeval timeout;
    int nfds;

    struct timespec now;
    ASSERT_PERROR(clock_gettime(CLOCK_REALTIME, &now) == 0);

    // Establish connection.
    int conn_out = socket(listen_addr.sin_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_PERROR(conn_out >= 0);

    struct timeval incoming = {
        .tv_sec = 5,
        .tv_usec = 0,
    };
    timeout = incoming;
    FD_ZERO(&set);
    FD_SET(listen_fd, &set);
    int max_fd = listen_fd + 1;

    ASSERT_PERROR(connect(conn_out, (struct sockaddr *)&proxy_addr, sizeof(proxy_addr)) == 0);
    ASSERT_PERROR(select(max_fd, &set, NULL, NULL, &timeout) == 1);
    assert(FD_ISSET(listen_fd, &set));

    timeval_sub(&incoming, &incoming, &timeout);
    printf(
        "%llu.%09llu,%ld,%llu.%06lu",
        (unsigned long long)now.tv_sec,
        (unsigned long long)now.tv_nsec,
        concurrent,
        (unsigned long long)incoming.tv_sec,
        (unsigned long)incoming.tv_usec
    );

    int conn_in = accept(listen_fd, NULL, 0);
    ASSERT_PERROR(conn_in >= 0);

    fprintf(stderr, "%ld connected\n", concurrent);

    // Send a single byte.
    ASSERT_PERROR(shutdown(conn_out, SHUT_WR) == 0);
    ASSERT_PERROR(shutdown(conn_in, SHUT_RD) == 0);

    struct timeval result = {
        .tv_sec = 5,
        .tv_usec = 0,
    };

    timeout = result;
    FD_ZERO(&set);
    FD_SET(conn_out, &set);
    max_fd = conn_out + 1;

    ASSERT_PERROR(write(conn_in, "?", 1) == 1);
    ASSERT_PERROR(select(max_fd, &set, NULL, NULL, &timeout) == 1);
    assert(FD_ISSET(conn_out, &set));

    char buf[2];
    ASSERT_PERROR(read(conn_out, buf, sizeof(buf)) == 1);
    assert(buf[0] == '?');

    timeval_sub(&result, &result, &timeout);
    printf(
        ",%llu.%06lu\n",
        (unsigned long long)result.tv_sec,
        (unsigned long)result.tv_usec
    );

    return 0;
}
