#include <gtest/gtest.h>

extern "C" {
#include "../listener/fd_map.h"
}

TEST(fd_map, insert_stderr) {
    struct fd_map fm;
    ASSERT_GE(fd_map_init(&fm), 0);

    // Insert file descriptor.
    struct fd_map_entry *e = fd_map_get(&fm, STDERR_FILENO);
    ASSERT_NE(e, nullptr);

    struct fd_map_entry *start = (struct fd_map_entry *)fm.map.addr->data;
    struct fd_map_entry *end = (struct fd_map_entry *)((char *)fm.map.addr + fm.map.addr->size);
    EXPECT_EQ(e - start, 2);
    EXPECT_EQ(end - start, 3);
    EXPECT_EQ(((uintptr_t)e - (uintptr_t)start) % sizeof(struct fd_map_entry), 0);
    EXPECT_EQ(((uintptr_t)end - (uintptr_t)start) % sizeof(struct fd_map_entry), 0);

    e->used = 1;
    for(struct fd_map_entry *e2 = start; e2 < e; ++e2) {
        EXPECT_FALSE(e2->used);
    }
    EXPECT_TRUE(start[2].used);

    // Insert another file descriptor.
    ASSERT_NE(3, STDERR_FILENO);
    e->used = 0;
    e = fd_map_get(&fm, 3);
    ASSERT_NE(e, nullptr);
    e->used = 1;

    start = (struct fd_map_entry *)fm.map.addr->data;
    end = (struct fd_map_entry *)((char *)fm.map.addr + fm.map.addr->size);
    EXPECT_EQ(e - start, 3);
    EXPECT_EQ(end - start, 4);
    EXPECT_EQ(((uintptr_t)e - (uintptr_t)start) % sizeof(struct fd_map_entry), 0);
    EXPECT_EQ(((uintptr_t)end - (uintptr_t)start) % sizeof(struct fd_map_entry), 0);

    e->used = 1;
    for(struct fd_map_entry *e2 = start; e2 < e; ++e2) {
        EXPECT_FALSE(e2->used);
    }
    EXPECT_TRUE(start[3].used);

}
