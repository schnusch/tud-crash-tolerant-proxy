#include <gtest/gtest.h>

extern "C" {
#include "../listener/cmdline.h"
}

TEST(parse_cmdline, keeper) {
    __attribute__((cleanup(free_cmdline)))
    struct cmdline_opts cmdline = {
        .listen_fds = NULL,
    };
    *(struct sockaddr_in6 *)&cmdline.upstream_addr = (struct sockaddr_in6){
        .sin6_family = AF_INET6,
        .sin6_port = htons(12345),
        .sin6_addr = IN6ADDR_LOOPBACK_INIT,
    };

    const char *argv[] = {
        "keeper",
        "--listener=foo",
        "--worker=bar",
        "--num-workers=2",
        "--upstream-address=[::1]:12345",
        "--shared-memory-fd=3",
        NULL
    };
    int argc = 0;
    while(argv[argc]) {
        ++argc;
    }

    optind = 1;
    ASSERT_EQ(parse_cmdline(&cmdline, argc, (char **)argv), 0);
    EXPECT_STREQ(cmdline.listener, "foo");
    EXPECT_STREQ(cmdline.worker, "bar");
    EXPECT_EQ(worker_process_array_len(&cmdline.worker_procs), 2);
    EXPECT_EQ(cmdline.shared_mem_fd, 3);
}

static void test_worker_argv(
    const struct cmdline_opts *cmdline,
    int ipc_broadcast,
    int ipc_direct,
    const char **expected
) {
    char **argv = cmdline_to_worker_argv(cmdline, ipc_broadcast, ipc_direct);
    ASSERT_TRUE(argv);
    size_t i = 0;
    do {
        EXPECT_STREQ(argv[i], expected[i]);
        if(!argv[i] || !expected[i]) {
            break;
        }
    } while(++i);
    free(argv);
}

TEST(cmdline_to_worker_argv, broadcast) {
    struct cmdline_opts cmdline = {
        .shared_mem_fd = 4,
        .worker = "foo",
    };
    *(struct sockaddr_in6 *)&cmdline.upstream_addr = (struct sockaddr_in6){
        .sin6_family = AF_INET6,
        .sin6_port = htons(12345),
        .sin6_addr = IN6ADDR_LOOPBACK_INIT,
    };
    const char *expected[] = {
#ifdef USE_VALGRIND
        "valgrind",
#endif
        "foo",
        "--ipc-broadcast=3",
        "--ipc-direct=0",
        "--upstream-address=[::1]:12345",
        "--shared-memory-fd=4",
        NULL
    };
    test_worker_argv(&cmdline, 3, 0, expected);
}

TEST(cmdline_to_worker_argv, no_broadcast) {
    struct cmdline_opts cmdline = {
        .shared_mem_fd = 3,
        .worker = "foo",
    };
    *(struct sockaddr_in6 *)&cmdline.upstream_addr = (struct sockaddr_in6){
        .sin6_family = AF_INET6,
        .sin6_port = htons(12345),
        .sin6_addr = IN6ADDR_LOOPBACK_INIT,
    };
    const char *expected[] = {
#ifdef USE_VALGRIND
        "valgrind",
#endif
        "foo",
        "--ipc-direct=0",
        "--upstream-address=[::1]:12345",
        "--shared-memory-fd=3",
        NULL
    };
    test_worker_argv(&cmdline, -1, 0, expected);
}
