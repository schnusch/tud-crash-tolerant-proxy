extern "C" {
#include "../common/ipc.h"
}

class Test_ipc : public testing::Test {
protected:
    int sockpair[2]{-1, -1};

    void SetUp() override {
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, this->sockpair), 0);
    }

    void TearDown() override {
        this->sockpair[0] < 0 || close(this->sockpair[0]);
        this->sockpair[1] < 0 || close(this->sockpair[1]);
    }
};

class Test_ipc_send : public Test_ipc { };

TEST_F(Test_ipc_send, success) {
    const size_t slot = 123;
    const int fd = -1;
    ASSERT_GE(ipc_send(this->sockpair[1], "accept", slot, fd, "foo=%d", -456), 0);

    char buf[512] = {0};
    ASSERT_GT(read(this->sockpair[0], buf, sizeof(buf) - 1), 0);
    EXPECT_STREQ(buf, "accept slot=123 foo=-456");
}

class Test_ipc_process_incoming : public Test_ipc { };

static char test_ctx[1];

static int ipc_fallback(const char *action, size_t slot, int fd, const char *tail, void *ctx) {
    EXPECT_STREQ(action, "accept");
    EXPECT_EQ(slot, 123);
    EXPECT_EQ(fd, -1);
    EXPECT_STREQ(tail, "foo=-456");
    EXPECT_EQ(ctx, test_ctx);
    return -2;
}

TEST_F(Test_ipc_process_incoming, success) {
    const char msg[] = "accept slot=123 foo=-456";
    ASSERT_EQ(write(this->sockpair[1], msg, sizeof(msg) - 1), sizeof(msg) - 1);

    const struct ipc_action_method methods[] = {
        {NULL, ipc_fallback}
    };
    ASSERT_EQ(ipc_process_incoming(this->sockpair[0], methods, test_ctx), -2);
}
