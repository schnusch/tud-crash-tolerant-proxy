#include <gtest/gtest.h>

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
}
