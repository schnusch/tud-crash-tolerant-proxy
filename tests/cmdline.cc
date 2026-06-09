extern "C" {
#include "../listener/cmdline.h"
}

TEST(parse_cmdline, keeper) {
    __attribute__((cleanup(free_cmdline)))
    struct cmdline_opts cmdline = {
        .listen_fds = NULL,
    };

    const char *argv[] = {
        "keeper",
        "--listener=foo",
        "--worker=bar",
        "--num-workers=2",
        "--shared-memory-fd=3",
        "--parent-pidfd=4",
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
    EXPECT_EQ(cmdline.num_workers, 2);
    EXPECT_EQ(cmdline.shared_mem_fd, 3);
    EXPECT_EQ(cmdline.parent_pidfd, 4);
}

TEST(cmdline_to_argv, keeper) {
    struct cmdline_opts cmdline = {
        .shared_mem_fd = 3,
        .num_workers = 2,
        .parent_pidfd = 4,
        .listener = "foo",
        .worker = "bar",
    };
    char **argv = cmdline_to_argv(&cmdline, "foo", 0);
    ASSERT_TRUE(argv);

    const char *expected[] = {
#ifdef USE_VALGRIND
        "valgrind",
#endif
        "foo",
        "--listener=foo",
        "--worker=bar",
        "--num-workers=2",
        "--shared-memory-fd=3",
        "--ipc-fd=0",
        "--parent-pidfd=4",
        NULL
    };
    size_t i = 0;
    do {
        EXPECT_STREQ(argv[i], expected[i]);
        if(!argv[i] || !expected[i]) {
            break;
        }
    } while(++i);
    free(argv);
}
