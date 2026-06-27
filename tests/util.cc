#include <gtest/gtest.h>

#include <climits>
extern "C" {
#include <sys/epoll.h>
#include <sys/un.h>
#include "../common/util.h"
}

TEST(format_sockaddr, sockaddr_in) {
    char buf[FORMAT_SOCKADDR_BUFLEN];
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(12345),
    };
    EXPECT_STREQ(
        format_sockaddr(buf, (struct sockaddr *)&addr),
        "0.0.0.0:12345"
    );
}

TEST(format_sockaddr, sockaddr_in6) {
    char buf[FORMAT_SOCKADDR_BUFLEN];
    struct sockaddr_in6 addr = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(12345),
    };
    EXPECT_STREQ(
        format_sockaddr(buf, (struct sockaddr *)&addr),
        "[::]:12345"
    );
}

TEST(parse_sockaddr, sockaddr_in) {
    struct sockaddr_storage addr;
    ASSERT_EQ(parse_sockaddr(&addr, "127.0.0.1:12345"), 0);
    ASSERT_EQ(addr.ss_family, AF_INET);
    EXPECT_EQ(
        ((struct sockaddr_in *)&addr)->sin_port,
        htons(12345)
    );
}

TEST(parse_sockaddr, sockaddr_in6) {
    struct sockaddr_storage addr;
    ASSERT_EQ(parse_sockaddr(&addr, "[::1]:12345"), 0);
    ASSERT_EQ(addr.ss_family, AF_INET6);
    EXPECT_EQ(
        ((struct sockaddr_in6 *)&addr)->sin6_port,
        htons(12345)
    );
}

TEST(parse_sockaddr, sockaddr_un) {
    struct sockaddr_storage addr;
    ASSERT_EQ(parse_sockaddr(&addr, "/socket"), 0);
    ASSERT_EQ(addr.ss_family, AF_UNIX);
    EXPECT_STREQ(
        ((struct sockaddr_un *)&addr)->sun_path,
        "/socket"
    );
}

TEST(parse_sockaddr, sockaddr_un_trunc) {
    char path[sizeof(((struct sockaddr_un *)NULL)->sun_path) + 1];
    path[0] = '/';
    memset(path + 1, '?', sizeof(path) - 2);
    path[sizeof(path) - 1] = '\0';

    struct sockaddr_storage addr;
    ASSERT_EQ(parse_sockaddr(&addr, path), -1);
    EXPECT_EQ(errno, EOVERFLOW);
}

TEST(strlcpy, short) {
    char dst[13];
    ASSERT_EQ(strlcpy(dst, "Hello", sizeof(dst)), 5);
    EXPECT_STREQ(dst, "Hello");
}

TEST(strlcpy, equal) {
    char dst[13];
    ASSERT_EQ(strlcpy(dst, "Hello World!", sizeof(dst)), 12);
    EXPECT_STREQ(dst, "Hello World!");
}

TEST(strlcpy, trunc) {
    char dst[6];
    ASSERT_EQ(strlcpy(dst, "Hello World!", sizeof(dst)), 12);
    EXPECT_STREQ(dst, "Hello");
}

TEST(strtol_limit, gt_0) {
    int e;
    long l;
    EXPECT_EQ(l = strtol_limit(&e, "-1", 0, LONG_MAX), 0);
    ASSERT_TRUE(e);
    EXPECT_EQ(errno, ERANGE);
}

TEST(strtol_limit, le_INT_MAX) {
    int e;
    long l;
    EXPECT_EQ(l = strtol_limit(&e, "2147483648", 0, INT_MAX), 0);
    ASSERT_TRUE(e);
    EXPECT_EQ(errno, ERANGE);
}

TEST(strtol_limit, negative) {
    int e;
    long l;
    EXPECT_EQ(l = strtol_limit(&e, "-1", LONG_MIN, LONG_MAX), -1);
    EXPECT_FALSE(e);
}

TEST(strtol_limit, trailing_space) {
    int e;
    long l;
    EXPECT_EQ(l = strtol_limit(&e, " 1 ", LONG_MIN, LONG_MAX), 1);
    EXPECT_FALSE(e);
}

TEST(epoll_str, many) {
    char buf[512];
    epoll_str(buf, sizeof(buf), EPOLLIN | EPOLLOUT | EPOLLRDHUP);
    EXPECT_STREQ(buf, "EPOLLIN | EPOLLOUT | EPOLLRDHUP");
}

TEST(epoll_str, leftover) {
    char buf[512];
    epoll_str(buf, sizeof(buf), -1);
    EXPECT_STREQ(buf, "EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLPRI | EPOLLERR | EPOLLHUP | EPOLLET | EPOLLONESHOT | EPOLLWAKEUP | EPOLLEXCLUSIVE | 0xFFFDFE0");
}

TEST(epoll_str, trunc) {
    char buf[5];
    epoll_str(buf, sizeof(buf), EPOLLIN);
    EXPECT_STREQ(buf, "E...");
}

TEST(epoll_str, very_trunc) {
    char buf[3];
    epoll_str(buf, sizeof(buf), EPOLLIN);
    EXPECT_STREQ(buf, "..");
}

TEST(epoll_str, unknown) {
    char buf[512];
    epoll_str(buf, sizeof(buf), 0x08000000);
    EXPECT_STREQ(buf, "0x8000000");
}

TEST(epoll_str, zero) {
    char buf[512];
    epoll_str(buf, sizeof(buf), 0);
    EXPECT_STREQ(buf, "0x0");
}
