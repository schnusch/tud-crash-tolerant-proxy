#include <gtest/gtest.h>

extern "C" {
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include "../common/util.h"
#include "../libcrash/libcrash.h"
#include "../worker/atomic_send_recv.h"
}

static jmp_buf jmp;
enum {
    ERROR_PREPARE = 1 << 0,
    ERROR_SYSCALL_PRE = 1 << 1,
    ERROR_SYSCALL_POST = 1 << 2,
    ERROR_MEMCPY = 1 << 3,
};
static int error_cases;

extern "C" {
    void libcrash_atomic_send_prepare(int fd, struct double_buffer *buf) {
        if(error_cases & ERROR_PREPARE) {
            longjmp(jmp, ERROR_PREPARE);
        }
    }
    void libcrash_atomic_send_sendmmsg_pre(int fd, struct double_buffer *buf) {
        if(error_cases & ERROR_SYSCALL_PRE) {
            longjmp(jmp, ERROR_SYSCALL_PRE);
        }
    }
    void libcrash_atomic_send_sendmmsg_post(int fd, struct double_buffer *buf, int rc) {
        if(error_cases & ERROR_SYSCALL_POST) {
            longjmp(jmp, ERROR_SYSCALL_POST);
        }
    }
    void libcrash_double_buffer_lshift(struct double_buffer *buf, size_t active) {
        if(error_cases & ERROR_MEMCPY) {
            longjmp(jmp, ERROR_MEMCPY);
        }
    }

    void libcrash_atomic_recv_prepare(int fd, struct double_buffer *buf) {
        if(error_cases & ERROR_PREPARE) {
            longjmp(jmp, ERROR_PREPARE);
        }
    }
    void libcrash_atomic_recv_recvmmsg_pre(int fd, struct double_buffer *buf) {
        if(error_cases & ERROR_SYSCALL_PRE) {
            longjmp(jmp, ERROR_SYSCALL_PRE);
        }
    }
    void libcrash_atomic_recv_recvmmsg_post(int fd, struct double_buffer *buf, int rc) {
        if(error_cases & ERROR_SYSCALL_POST) {
            longjmp(jmp, ERROR_SYSCALL_POST);
        }
    }
    void libcrash_double_buffer_append(struct double_buffer *buf, size_t active) {
        if(error_cases & ERROR_MEMCPY) {
            longjmp(jmp, ERROR_MEMCPY);
        }
    }
}

class Test_atomic : public testing::Test {
protected:
    int sockpair[2]{-1, -1};

    void SetUp() override {
        signal(SIGPIPE, SIG_IGN);
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, this->sockpair), 0);
    }

    void TearDown() override {
        this->sockpair[0] < 0 || close(this->sockpair[0]);
        this->sockpair[1] < 0 || close(this->sockpair[1]);
        signal(SIGPIPE, SIG_DFL);
    }
};

class Test_atomic_send : public Test_atomic { };

static void crash_before_send(int sockpair[2], int e, int active) {
    static const char tx[] = "TODO crash_before_send";
    char rx[4096];

    struct double_buffer buf = DOUBLE_BUFFER_INIT;
    buf.active = !!active;
    ACTIVE_BUFFER(&buf)->used = strlcpy(ACTIVE_BUFFER(&buf)->buf, tx, sizeof(ACTIVE_BUFFER(&buf)->buf));

    int occured_error = setjmp(jmp);
    if(occured_error == 0) {
        error_cases = e;
    } else {
        // atomic_sent was interrupted.
        EXPECT_EQ(occured_error, e);

        // Ensure no data was written at this point.
        ASSERT_EQ(recv(sockpair[0], rx, sizeof(rx), MSG_DONTWAIT), -1);
        EXPECT_EQ(errno, EWOULDBLOCK);

        // Send buffer did not change.
        EXPECT_EQ(buf.mm.msg_len, (unsigned int)-1);
        EXPECT_EQ(
            std::string(ACTIVE_BUFFER(&buf)->buf, ACTIVE_BUFFER(&buf)->used),
            std::string(tx)
        );

        // Allow atomic_send to run uninterrupted.
        error_cases = 0;
    }

    // atomic_send will be interrupted on its first call and will
    // continue successfully on the second call.
    ASSERT_GE(atomic_send(sockpair[1], &buf), 0);
    ASSERT_EQ(error_cases, 0) << "longjmp was not triggered";

    // Send buffer was drained.
    EXPECT_EQ(ACTIVE_BUFFER(&buf)->used, 0);

    // Data sent completely.
    ssize_t n;
    ASSERT_GT((n = recv(sockpair[0], rx, sizeof(rx), MSG_DONTWAIT)), 0);
    rx[n] = '\0';
    ASSERT_STREQ(rx, tx);
}

TEST_F(Test_atomic_send, crash_prepare_active0) {
    crash_before_send(this->sockpair, ERROR_PREPARE, 0);
}
TEST_F(Test_atomic_send, crash_prepare_active1) {
    crash_before_send(this->sockpair, ERROR_PREPARE, 1);
}

TEST_F(Test_atomic_send, crash_sendmmsg_pre_active0) {
    crash_before_send(this->sockpair, ERROR_SYSCALL_PRE, 0);
}
TEST_F(Test_atomic_send, crash_sendmmsg_pre_active1) {
    crash_before_send(this->sockpair, ERROR_SYSCALL_PRE, 1);
}

static void crash_after_send(int sockpair[2], int e, int active) {
    static const char tx[] = "TODO crash_after_send";
    char rx[4096];

    struct double_buffer buf = DOUBLE_BUFFER_INIT;
    buf.active = !!active;
    ACTIVE_BUFFER(&buf)->used = strlcpy(ACTIVE_BUFFER(&buf)->buf, tx, sizeof(ACTIVE_BUFFER(&buf)->buf));

    int occured_error = setjmp(jmp);
    if(occured_error == 0) {
        error_cases = e;
    } else {
        // atomic_sent was interrupted.
        EXPECT_EQ(occured_error, e);

        // Data sent completely.
        ssize_t n;
        ASSERT_GT((n = recv(sockpair[0], rx, sizeof(rx), MSG_DONTWAIT)), 0);
        rx[n] = '\0';
        ASSERT_STREQ(rx, tx);

        // Send buffer did not change.
        EXPECT_NE(buf.mm.msg_len, (unsigned int)-1);
        EXPECT_EQ(
            std::string(ACTIVE_BUFFER(&buf)->buf, ACTIVE_BUFFER(&buf)->used),
            std::string(tx)
        );

        // Allow atomic_send to run uninterrupted.
        error_cases = 0;
    }

    // atomic_send will be interrupted on its first call and will
    // continue successfully on the second call.
    ASSERT_GE(atomic_send(sockpair[1], &buf), 0);
    ASSERT_EQ(error_cases, 0) << "longjmp was not triggered";

    // Send buffer was drained.
    EXPECT_EQ(ACTIVE_BUFFER(&buf)->used, 0);

    // Ensure no data was sent on the second call.
    ASSERT_EQ(recv(sockpair[0], rx, sizeof(rx), MSG_DONTWAIT), -1);
    ASSERT_EQ(errno, EWOULDBLOCK);
}

TEST_F(Test_atomic_send, crash_sendmmsg_post_active0) {
    crash_after_send(this->sockpair, ERROR_SYSCALL_POST, 0);
}
TEST_F(Test_atomic_send, crash_sendmmsg_post_active1) {
    crash_after_send(this->sockpair, ERROR_SYSCALL_POST, 1);
}

TEST_F(Test_atomic_send, crash_sendmmsg_memcpy_active0) {
    crash_after_send(this->sockpair, ERROR_MEMCPY, 0);
}
TEST_F(Test_atomic_send, crash_sendmmsg_memcpy_active1) {
    crash_after_send(this->sockpair, ERROR_MEMCPY, 1);
}

TEST_F(Test_atomic_send, fail_sendmmsg) {
    static const char tx[] = "TODO fail_sendmmsg";
    char rx[4096];

    struct double_buffer buf = DOUBLE_BUFFER_INIT;
    ACTIVE_BUFFER(&buf)->used = strlcpy(ACTIVE_BUFFER(&buf)->buf, tx, sizeof(buf.buffers[buf.active].buf));

    ASSERT_EQ(close(this->sockpair[0]), 0);
    this->sockpair[0] = -1;

    int e = setjmp(jmp);
    if(e == 0) {
        error_cases = 0;
    } else {
        FAIL() << "longjmp should not have been triggered";
    }

    ASSERT_EQ(atomic_send(this->sockpair[1], &buf), -1);
    EXPECT_EQ(errno, EPIPE);
    EXPECT_EQ(buf.state, ATOMIC_DO_SYSCALL);
    EXPECT_EQ(buf.mm.msg_len, -1);
    // Send buffer did not change.
    EXPECT_EQ(
        std::string(ACTIVE_BUFFER(&buf)->buf, ACTIVE_BUFFER(&buf)->used),
        std::string(tx)
    );

    ASSERT_EQ(atomic_send(this->sockpair[1], &buf), -1);
    EXPECT_EQ(errno, EPIPE);
    EXPECT_EQ(buf.state, ATOMIC_DO_SYSCALL);
    EXPECT_EQ(buf.mm.msg_len, -1);
    // Send buffer did not change.
    EXPECT_EQ(
        std::string(ACTIVE_BUFFER(&buf)->buf, ACTIVE_BUFFER(&buf)->used),
        std::string(tx)
    );
}

class Test_atomic_recv : public Test_atomic { };

static void crash_before_recv(int sockpair[2], int e, int active) {
    static const char initial[] = "TODO initial";
    static const char tx[] = "TODO crash_after_recv";
    char rx[4096];

    struct double_buffer buf = DOUBLE_BUFFER_INIT;
    buf.active = !!active;
    // Add initial data to the buffer.
    ACTIVE_BUFFER(&buf)->used = strlcpy(ACTIVE_BUFFER(&buf)->buf, initial, sizeof(ACTIVE_BUFFER(&buf)->buf));

    // Send data to the socket.
    ASSERT_EQ(send(sockpair[1], tx, strlen(tx), MSG_DONTWAIT), strlen(tx));
    // Peek into the kernel's receive buffer.
    EXPECT_EQ(recv(sockpair[0], rx, sizeof(rx), MSG_DONTWAIT | MSG_PEEK), strlen(tx));
    EXPECT_EQ(std::string(rx, strlen(tx)), std::string(tx));

    int occured_error = setjmp(jmp);
    if(occured_error == 0) {
        error_cases = e;
    } else {
        // atomic_recv was interrupted.
        EXPECT_EQ(occured_error, e);

        // Receive buffer is still empty.
        EXPECT_EQ(buf.mm.msg_len, (unsigned int)-1);
        EXPECT_EQ(ACTIVE_BUFFER(&buf)->used, strlen(initial));

        // Ensure no data was read at this point.
        EXPECT_EQ(recv(sockpair[0], rx, sizeof(rx), MSG_DONTWAIT | MSG_PEEK), strlen(tx));
        EXPECT_EQ(std::string(rx, strlen(tx)), std::string(tx));

        // Allow atomic_recv to run uninterrupted.
        error_cases = 0;
    }

    // atomic_recv will be interrupted on its first call and will
    // continue successfully on the second call.
    ASSERT_GT(atomic_recv(sockpair[0], &buf), 0);
    ASSERT_EQ(error_cases, 0) << "longjmp was not triggered";
    EXPECT_EQ(buf.state, ATOMIC_INIT);

    // Ensure the data was received.
    EXPECT_EQ(
        std::string(ACTIVE_BUFFER(&buf)->buf, ACTIVE_BUFFER(&buf)->used),
        std::string(initial) + tx
    );

    // Kernel buffer is empty.
    EXPECT_EQ(recv(sockpair[1], rx, sizeof(rx), MSG_DONTWAIT | MSG_PEEK), -1);
    EXPECT_EQ(errno, EWOULDBLOCK);

    // EWOULDBLOCK
    ASSERT_LT(atomic_recv(sockpair[0], &buf), 0);
    ASSERT_EQ(errno, EWOULDBLOCK);

    // Test EOF.
    ASSERT_EQ(shutdown(sockpair[0], SHUT_WR), 0);
    EXPECT_EQ(atomic_recv(sockpair[1], &buf), 0);
    EXPECT_EQ(atomic_recv(sockpair[1], &buf), 0);
}

TEST_F(Test_atomic_recv, crash_prepare_active0) {
    crash_before_recv(this->sockpair, ERROR_PREPARE, 0);
}
TEST_F(Test_atomic_recv, crash_prepare_active1) {
    crash_before_recv(this->sockpair, ERROR_PREPARE, 1);
}

TEST_F(Test_atomic_recv, crash_recvmmsg_pre_active0) {
    crash_before_recv(this->sockpair, ERROR_SYSCALL_PRE, 0);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_pre_active1) {
    crash_before_recv(this->sockpair, ERROR_SYSCALL_PRE, 1);
}

static void crash_after_recv(int sockpair[2], int e, int active) {
    static const char initial[] = "TODO initial";
    static const char tx[] = "TODO crash_after_recv";
    char rx[4096];

    struct double_buffer buf = DOUBLE_BUFFER_INIT;
    buf.active = !!active;
    // Add initial data to the buffer.
    ACTIVE_BUFFER(&buf)->used = strlcpy(ACTIVE_BUFFER(&buf)->buf, initial, sizeof(ACTIVE_BUFFER(&buf)->buf));

    // Send data to the socket.
    ASSERT_EQ(send(sockpair[1], tx, strlen(tx), MSG_DONTWAIT), strlen(tx));
    // Peek into the kernel's receive buffer.
    EXPECT_EQ(recv(sockpair[0], rx, sizeof(rx), MSG_DONTWAIT | MSG_PEEK), strlen(tx));
    EXPECT_EQ(std::string(rx, strlen(tx)), std::string(tx));

    int occured_error = setjmp(jmp);
    if(occured_error == 0) {
        error_cases = e;
    } else {
        // atomic_recv was interrupted.
        EXPECT_EQ(occured_error, e);

        // Ensure the data was received...
        ASSERT_NE(buf.mm.msg_len, (unsigned int)-1);
        EXPECT_EQ(
            std::string(ACTIVE_BUFFER(&buf)->buf, ACTIVE_BUFFER(&buf)->used + buf.mm.msg_len),
            std::string(initial) + tx
        );
        // ...and copied to the inactive buffer...
        EXPECT_EQ(
            std::string(buf.buffers[!buf.active].buf, buf.buffers[!buf.active].used),
            std::string(initial) + tx
        );
        // ...but the active buffer was not extended.
        EXPECT_EQ(
            std::string(ACTIVE_BUFFER(&buf)->buf, ACTIVE_BUFFER(&buf)->used),
            std::string(initial)
        );

        // Kernel buffer is empty.
        EXPECT_EQ(recv(sockpair[1], rx, sizeof(rx), MSG_DONTWAIT | MSG_PEEK), -1);
        EXPECT_EQ(errno, EWOULDBLOCK);

        // Allow atomic_recv to run uninterrupted.
        error_cases = 0;
    }

    // atomic_recv will be interrupted on its first call and will
    // continue successfully on the second call.
    ASSERT_GT(atomic_recv(sockpair[0], &buf), 0);
    ASSERT_EQ(error_cases, 0) << "longjmp was not triggered";
    EXPECT_EQ(buf.state, ATOMIC_INIT);

    // Ensure the data was received.
    EXPECT_EQ(
        std::string(ACTIVE_BUFFER(&buf)->buf, ACTIVE_BUFFER(&buf)->used),
        std::string(initial) + tx
    );

    // Kernel buffer is empty.
    EXPECT_EQ(recv(sockpair[1], rx, sizeof(rx), MSG_DONTWAIT | MSG_PEEK), -1);
    EXPECT_EQ(errno, EWOULDBLOCK);

    // EWOULDBLOCK
    ASSERT_LT(atomic_recv(sockpair[0], &buf), 0);
    ASSERT_EQ(errno, EWOULDBLOCK);

    // Test EOF.
    ASSERT_EQ(shutdown(sockpair[0], SHUT_WR), 0);
    EXPECT_EQ(atomic_recv(sockpair[1], &buf), 0);
    EXPECT_EQ(atomic_recv(sockpair[1], &buf), 0);
}

TEST_F(Test_atomic_recv, crash_recvmmsg_post_active0) {
    crash_after_send(this->sockpair, ERROR_SYSCALL_POST, 0);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_post_active1) {
    crash_after_send(this->sockpair, ERROR_SYSCALL_POST, 1);
}

TEST_F(Test_atomic_recv, crash_recvmmsg_memcpy_active0) {
    crash_after_recv(this->sockpair, ERROR_MEMCPY, 0);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_memcpy_active1) {
    crash_after_recv(this->sockpair, ERROR_MEMCPY, 1);
}

class Test_atomic_recv_rst : public Test_atomic_recv {
protected:
    int listen_fd = -1;

    void SetUp() override {
        //signal(SIGPIPE, SIG_IGN);

        // Create listening socket.
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = 0,
            .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
        };
        this->listen_fd = socket(addr.sin_family, SOCK_STREAM, 0);
        ASSERT_GE(listen_fd, 0);
        const int one = 1;
        EXPECT_EQ(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), 0);
        ASSERT_EQ(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
        ASSERT_EQ(listen(listen_fd, SOMAXCONN), 0);

        // Connect to listening socket.
        socklen_t addr_len = sizeof(addr);
        ASSERT_EQ(getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len), 0);
        ASSERT_LE(addr_len, sizeof(addr));
        this->sockpair[1] = socket(addr.sin_family, SOCK_STREAM, 0);
        ASSERT_GE(this->sockpair[1], 0);
        ASSERT_EQ(connect(this->sockpair[1], (struct sockaddr *)&addr, addr_len), 0);

        // Accept connection.
        this->sockpair[0] = accept(this->listen_fd, nullptr, nullptr);
        ASSERT_GE(this->sockpair[0], 0);

        // Close listening socket.
        if(close(this->listen_fd) == 0) {
            this->listen_fd = -1;
        }
    }
};

TEST_F(Test_atomic_recv_rst, fail_recvmmsg) {
    struct double_buffer buf = DOUBLE_BUFFER_INIT;

    // Reset connection.
    struct linger l = {
        .l_onoff = 1,
        .l_linger = 0,
    };
    ASSERT_EQ(setsockopt(this->sockpair[1], SOL_SOCKET, SO_LINGER, &l, sizeof(l)), 0);
    ASSERT_EQ(close(this->sockpair[1]), 0);
    this->sockpair[1] = -1;

    int e = setjmp(jmp);
    if(e == 0) {
        error_cases = 0;
    } else {
        FAIL() << "longjmp should not have been triggered";
    }

    ASSERT_EQ(atomic_recv(this->sockpair[0], &buf), -1);
    EXPECT_EQ(errno, ECONNRESET);
    EXPECT_EQ(buf.state, ATOMIC_DO_SYSCALL);
    EXPECT_EQ(buf.mm.msg_len, -1);
    // Receive buffer did not change.
    EXPECT_EQ(ACTIVE_BUFFER(&buf)->used, 0);

    // ECONNRESET is only returned once.
    char rx[4096];
    EXPECT_EQ(recv(sockpair[0], rx, sizeof(rx), MSG_DONTWAIT), 0);
}
