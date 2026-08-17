#include <arpa/inet.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ASSERT_PERROR(COND) \
    do { \
        if(!(COND)) { \
            perror(#COND); \
            raise(SIGABRT); \
            __builtin_trap(); \
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

    struct sockaddr_in proxy_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
    };
    assert(argc >= 2);
    ASSERT_PERROR(inet_pton(proxy_addr.sin_family, argv[1], &proxy_addr.sin_addr) == 1);
    int conn_out = socket(listen_addr.sin_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_PERROR(conn_out >= 0);
    ASSERT_PERROR(connect(conn_out, (struct sockaddr *)&proxy_addr, sizeof(proxy_addr)) == 0);
    ASSERT_PERROR(shutdown(conn_out, SHUT_RD) == 0);

    int conn_in = accept(listen_fd, NULL, 0);
    ASSERT_PERROR(conn_in >= 0);
    ASSERT_PERROR(shutdown(conn_in, SHUT_WR) == 0);

    struct timeval result = {
        .tv_sec = 5,
        .tv_usec = 0,
    };

    struct timeval timeout = result;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(conn_in, &set);

    struct timespec now;
    ASSERT_PERROR(clock_gettime(CLOCK_REALTIME, &now) == 0);

    ssize_t nwrit = write(conn_out, "?", 1);
    int nfds = select(conn_in + 1, &set, NULL, NULL, &timeout);
    if(nfds == 0) {
        fputs("timed out\n", stderr);
        return 0;
    }
    ASSERT_PERROR(nfds == 1);
    assert(FD_ISSET(conn_in, &set));
    ASSERT_PERROR(nwrit == 1);

    char buf[2];
    ASSERT_PERROR(read(conn_in, buf, sizeof(buf)) == 1);
    assert(buf[0] == '?');

    timeval_sub(&result, &result, &timeout);
    printf(
        "%llu.%09lu,%llu.%06lu\n",
        (unsigned long long)now.tv_sec,
        (unsigned long long)now.tv_nsec,
        (unsigned long long)result.tv_sec,
        (unsigned long)result.tv_usec
    );

    return 0;
}
