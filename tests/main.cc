#include <gtest/gtest.h>

#include "cmdline.cc"
#include "fd_map.cc"
#include "ipc.cc"
#include "util.cc"

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
