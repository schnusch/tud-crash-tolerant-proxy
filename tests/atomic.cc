#include <gtest/gtest.h>

extern "C" {
#include <errno.h>
#include <netinet/ip.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/socket.h>
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
    void libcrash_atomic_send_prepare(int fd, struct atomic_ring_buffer *buf) {
        if(error_cases & ERROR_PREPARE) {
            longjmp(jmp, ERROR_PREPARE);
        }
    }
    void libcrash_atomic_send_sendmmsg_pre(int fd, struct atomic_ring_buffer *buf) {
        if(error_cases & ERROR_SYSCALL_PRE) {
            longjmp(jmp, ERROR_SYSCALL_PRE);
        }
    }
    void libcrash_atomic_send_sendmmsg_post(int fd, struct atomic_ring_buffer *buf, int *rc) {
        if(error_cases & ERROR_SYSCALL_POST) {
            longjmp(jmp, ERROR_SYSCALL_POST);
        }
    }
    void libcrash_atomic_ring_buffer_ltrim(struct atomic_ring_buffer *buf, size_t active) {
        if(error_cases & ERROR_MEMCPY) {
            longjmp(jmp, ERROR_MEMCPY);
        }
    }

    void libcrash_atomic_recv_prepare(int fd, struct atomic_ring_buffer *buf) {
        if(error_cases & ERROR_PREPARE) {
            longjmp(jmp, ERROR_PREPARE);
        }
    }
    void libcrash_atomic_recv_recvmmsg_pre(int fd, struct atomic_ring_buffer *buf) {
        if(error_cases & ERROR_SYSCALL_PRE) {
            longjmp(jmp, ERROR_SYSCALL_PRE);
        }
    }
    void libcrash_atomic_recv_recvmmsg_post(int fd, struct atomic_ring_buffer *buf, int *rc) {
        if(error_cases & ERROR_SYSCALL_POST) {
            longjmp(jmp, ERROR_SYSCALL_POST);
        }
    }
    void libcrash_atomic_ring_buffer_append(struct atomic_ring_buffer *buf, size_t active) {
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

static std::string string_from_ringbuffer(struct atomic_ring_buffer *buf, size_t more) {
    const size_t end = ACTIVE_RANGE(buf)->start + ACTIVE_RANGE(buf)->len + more;
    if(end < sizeof(buf->buf)) {
        return std::string(buf->buf + ACTIVE_RANGE(buf)->start, ACTIVE_RANGE(buf)->len + more);
    } else {
        const size_t until_end = sizeof(buf->buf) - ACTIVE_RANGE(buf)->start;
        return (
            std::string(buf->buf + ACTIVE_RANGE(buf)->start, until_end)
            + std::string(buf->buf, ACTIVE_RANGE(buf)->len + more - until_end)
        );
    }
}

static void init_ringbuf(struct atomic_ring_buffer *buf, ssize_t start, const char *data, size_t n) {
    ASSERT_LE(n, sizeof(buf->buf));
    if(start < 0) {
        start = sizeof(buf->buf) - n + start;
    }
    ASSERT_LT(start, sizeof(buf->buf));
    ACTIVE_RANGE(buf)->start = start;
    ACTIVE_RANGE(buf)->len = 0;
    ring_buffer_append(buf->buf, ACTIVE_RANGE(buf), data, n);
    ASSERT_EQ(ACTIVE_RANGE(buf)->start, start);
    ASSERT_EQ(ACTIVE_RANGE(buf)->len, n);
    ASSERT_EQ(string_from_ringbuffer(buf, 0), std::string(data, n));
}

static void crash_before_send(int sockpair[2], int e, ssize_t start, int active) {
    static const char tx[] = "TODO crash_before_send";
    char rx[4096];

    struct atomic_ring_buffer buf = ATOMIC_RING_BUFFER_INIT;
    buf.active = !!active;
    init_ringbuf(&buf, start, tx, strlen(tx));

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
            string_from_ringbuffer(&buf, 0),
            std::string(tx)
        );

        // Allow atomic_send to run uninterrupted.
        error_cases = 0;
    }

    // atomic_send will be interrupted on its first call and will
    // continue successfully on the second call.
    ASSERT_GT(atomic_send(sockpair[1], &buf), 0);
    ASSERT_EQ(error_cases, 0) << "longjmp was not triggered";

    // Send buffer was drained.
    EXPECT_EQ(ACTIVE_RANGE(&buf)->len, 0);

    // Data sent completely.
    ssize_t n;
    ASSERT_GT((n = recv(sockpair[0], rx, sizeof(rx), MSG_DONTWAIT)), 0);
    rx[n] = '\0';
    ASSERT_STREQ(rx, tx);
}

TEST_F(Test_atomic_send, crash_prepare_simple0) {
    crash_before_send(this->sockpair, ERROR_PREPARE, 32, 0);
}
TEST_F(Test_atomic_send, crash_prepare_simple1) {
    crash_before_send(this->sockpair, ERROR_PREPARE, 32, 1);
}
TEST_F(Test_atomic_send, crash_prepare_wrapped0) {
    crash_before_send(this->sockpair, ERROR_PREPARE, RING_BUFFER_SIZE - 2, 0);
}
TEST_F(Test_atomic_send, crash_prepare_wrapped1) {
    crash_before_send(this->sockpair, ERROR_PREPARE, RING_BUFFER_SIZE - 2, 1);
}

TEST_F(Test_atomic_send, crash_sendmmsg_pre_simple0) {
    crash_before_send(this->sockpair, ERROR_SYSCALL_PRE, 0, 0);
}
TEST_F(Test_atomic_send, crash_sendmmsg_pre_simple1) {
    crash_before_send(this->sockpair, ERROR_SYSCALL_PRE, 0, 1);
}
TEST_F(Test_atomic_send, crash_sendmmsg_pre_wrapped0) {
    crash_before_send(this->sockpair, ERROR_SYSCALL_PRE, RING_BUFFER_SIZE - 2, 0);
}
TEST_F(Test_atomic_send, crash_sendmmsg_pre_wrapped1) {
    crash_before_send(this->sockpair, ERROR_SYSCALL_PRE, RING_BUFFER_SIZE - 2, 1);
}

static void crash_after_send(int sockpair[2], int e, ssize_t start, int active) {
    static const char tx[] = "TODO crash_after_send";
    char rx[4096];

    struct atomic_ring_buffer buf = ATOMIC_RING_BUFFER_INIT;
    buf.active = !!active;
    init_ringbuf(&buf, start, tx, strlen(tx));

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
            string_from_ringbuffer(&buf, 0),
            std::string(tx)
        );

        // Allow atomic_send to run uninterrupted.
        error_cases = 0;
    }

    // atomic_send will be interrupted on its first call and will
    // continue successfully on the second call.
    ASSERT_GT(atomic_send(sockpair[1], &buf), 0);
    ASSERT_EQ(error_cases, 0) << "longjmp was not triggered";

    // Send buffer was drained.
    EXPECT_EQ(ACTIVE_RANGE(&buf)->len, 0);

    // Ensure no data was sent on the second call.
    ASSERT_EQ(recv(sockpair[0], rx, sizeof(rx), MSG_DONTWAIT), -1);
    ASSERT_EQ(errno, EWOULDBLOCK);
}

TEST_F(Test_atomic_send, crash_sendmmsg_post_simple0) {
    crash_after_send(this->sockpair, ERROR_SYSCALL_POST, 0, 0);
}
TEST_F(Test_atomic_send, crash_sendmmsg_post_simple1) {
    crash_after_send(this->sockpair, ERROR_SYSCALL_POST, 0, 1);
}
TEST_F(Test_atomic_send, crash_sendmmsg_post_wrapped0) {
    crash_after_send(this->sockpair, ERROR_SYSCALL_POST, RING_BUFFER_SIZE - 2, 0);
}
TEST_F(Test_atomic_send, crash_sendmmsg_post_wrapped1) {
    crash_after_send(this->sockpair, ERROR_SYSCALL_POST, RING_BUFFER_SIZE - 2, 1);
}

TEST_F(Test_atomic_send, crash_sendmmsg_memcpy_simple0) {
    crash_after_send(this->sockpair, ERROR_MEMCPY, 0, 0);
}
TEST_F(Test_atomic_send, crash_sendmmsg_memcpy_simple1) {
    crash_after_send(this->sockpair, ERROR_MEMCPY, 0, 1);
}
TEST_F(Test_atomic_send, crash_sendmmsg_memcpy_wrapped0) {
    crash_after_send(this->sockpair, ERROR_MEMCPY, RING_BUFFER_SIZE - 2, 0);
}
TEST_F(Test_atomic_send, crash_sendmmsg_memcpy_wrapped1) {
    crash_after_send(this->sockpair, ERROR_MEMCPY, RING_BUFFER_SIZE - 2, 1);
}

TEST_F(Test_atomic_send, fail_sendmmsg) {
    static const char tx[] = "TODO fail_sendmmsg";
    char rx[4096];

    struct atomic_ring_buffer buf = ATOMIC_RING_BUFFER_INIT;
    init_ringbuf(&buf, 0, tx, strlen(tx));

    ASSERT_EQ(close(this->sockpair[0]), 0);
    this->sockpair[0] = -1;

    int e = setjmp(jmp);
    if(e == 0) {
        error_cases = 0;
    } else {
        FAIL() << "longjmp should not have been triggered";
    }

    ASSERT_LT(atomic_send(this->sockpair[1], &buf), 0);
    EXPECT_EQ(errno, EPIPE);
    EXPECT_EQ(buf.state, ATOMIC_DO_SYSCALL);
    EXPECT_EQ(buf.mm.msg_len, -1);
    // Send buffer did not change.
    EXPECT_EQ(
        string_from_ringbuffer(&buf, 0),
        std::string(tx)
    );

    ASSERT_LT(atomic_send(this->sockpair[1], &buf), 0);
    EXPECT_EQ(errno, EPIPE);
    EXPECT_EQ(buf.state, ATOMIC_DO_SYSCALL);
    EXPECT_EQ(buf.mm.msg_len, -1);
    // Send buffer did not change.
    EXPECT_EQ(
        string_from_ringbuffer(&buf, 0),
        std::string(tx)
    );
}

class Test_atomic_recv : public Test_atomic { };

TEST_F(Test_atomic_recv, recvmmsg_nonblock) {
    struct atomic_ring_buffer buf = ATOMIC_RING_BUFFER_INIT;

    EXPECT_EQ(shutdown(sockpair[0], SHUT_WR), 0);

    // Check MSG_DONTWAIT.
    ASSERT_LT(atomic_recv(sockpair[0], &buf), 0);
    ASSERT_EQ(errno, EWOULDBLOCK);

    // Send data to the socket.
    char tx[4096];
    memset(tx, 'a', sizeof(tx));
    ASSERT_EQ(send(sockpair[1], tx, sizeof(tx), MSG_DONTWAIT), sizeof(tx));

    // Receive data.
    error_cases = 0;
    ASSERT_EQ(atomic_recv(sockpair[0], &buf), sizeof(tx));

    // Ensure the data was received.
    EXPECT_EQ(
        string_from_ringbuffer(&buf, 0),
        std::string(tx, sizeof(tx))
    );

    // Check MSG_DONTWAIT.
    ASSERT_LT(atomic_recv(sockpair[0], &buf), 0);
    ASSERT_EQ(errno, EWOULDBLOCK);

    // Check EOF.
    EXPECT_EQ(shutdown(sockpair[1], SHUT_WR), 0);
    ASSERT_EQ(atomic_recv(sockpair[0], &buf), 0);
}

static void crash_before_recv(int sockpair[2], int e, ssize_t start, int active) {
    static const char initial[] = "TODO initial";
    static const char tx[] = "TODO crash_after_recv";
    char rx[4096];

    struct atomic_ring_buffer buf = ATOMIC_RING_BUFFER_INIT;
    buf.active = !!active;
    init_ringbuf(&buf, start, initial, strlen(initial));

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
        EXPECT_EQ(ACTIVE_RANGE(&buf)->len, strlen(initial));

        // Ensure no data was read at this point.
        ASSERT_EQ(recv(sockpair[0], rx, sizeof(rx), MSG_DONTWAIT | MSG_PEEK), strlen(tx));
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
        string_from_ringbuffer(&buf, 0),
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

TEST_F(Test_atomic_recv, crash_prepare_after0) {
    crash_before_recv(this->sockpair, ERROR_PREPARE, 0, 0);
}
TEST_F(Test_atomic_recv, crash_prepare_after1) {
    crash_before_recv(this->sockpair, ERROR_PREPARE, 0, 1);
}
TEST_F(Test_atomic_recv, crash_prepare_wrapping0) {
    crash_before_recv(this->sockpair, ERROR_PREPARE, -4, 0);
}
TEST_F(Test_atomic_recv, crash_prepare_wrapping1) {
    crash_before_recv(this->sockpair, ERROR_PREPARE, -4, 1);
}
TEST_F(Test_atomic_recv, crash_prepare_inbetween0) {
    crash_before_recv(this->sockpair, ERROR_PREPARE, RING_BUFFER_SIZE - 2, 0);
}
TEST_F(Test_atomic_recv, crash_prepare_inbetween1) {
    crash_before_recv(this->sockpair, ERROR_PREPARE, RING_BUFFER_SIZE - 2, 1);
}

TEST_F(Test_atomic_recv, crash_recvmmsg_pre_after0) {
    crash_before_recv(this->sockpair, ERROR_SYSCALL_PRE, 0, 0);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_pre_after1) {
    crash_before_recv(this->sockpair, ERROR_SYSCALL_PRE, 0, 1);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_pre_wrapping0) {
    crash_before_recv(this->sockpair, ERROR_SYSCALL_PRE, -4, 0);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_pre_wrapping1) {
    crash_before_recv(this->sockpair, ERROR_SYSCALL_PRE, -4, 1);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_pre_inbetween0) {
    crash_before_recv(this->sockpair, ERROR_SYSCALL_PRE, RING_BUFFER_SIZE - 2, 0);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_pre_inbetween1) {
    crash_before_recv(this->sockpair, ERROR_SYSCALL_PRE, RING_BUFFER_SIZE - 2, 1);
}

static void crash_after_recv(int sockpair[2], int e, ssize_t start, int active) {
    static const char initial[] = "TODO initial";
    static const char tx[] = "TODO crash_after_recv";
    char rx[4096];

    struct atomic_ring_buffer buf = ATOMIC_RING_BUFFER_INIT;
    buf.active = !!active;
    init_ringbuf(&buf, start, initial, strlen(initial));

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
            string_from_ringbuffer(&buf, buf.mm.msg_len),
            std::string(initial) + tx
        );
        // ...but the active buffer was not extended.
        EXPECT_EQ(
            string_from_ringbuffer(&buf, 0),
            std::string(initial)
        );

        // Kernel buffer is empty.
        ASSERT_EQ(recv(sockpair[1], rx, sizeof(rx), MSG_DONTWAIT | MSG_PEEK), -1);
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
        string_from_ringbuffer(&buf, 0),
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

TEST_F(Test_atomic_recv, crash_recvmmsg_post_after0) {
    crash_after_recv(this->sockpair, ERROR_SYSCALL_POST, 0, 0);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_post_after1) {
    crash_after_recv(this->sockpair, ERROR_SYSCALL_POST, 0, 1);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_post_wrapping0) {
    crash_after_recv(this->sockpair, ERROR_SYSCALL_POST, -4, 0);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_post_wrapping1) {
    crash_after_recv(this->sockpair, ERROR_SYSCALL_POST, -4, 1);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_post_inbetween0) {
    crash_after_recv(this->sockpair, ERROR_SYSCALL_POST, RING_BUFFER_SIZE - 2, 0);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_post_inbetween1) {
    crash_after_recv(this->sockpair, ERROR_SYSCALL_POST, RING_BUFFER_SIZE - 2, 1);
}

TEST_F(Test_atomic_recv, crash_recvmmsg_memcpy_after0) {
    crash_after_recv(this->sockpair, ERROR_MEMCPY, 0, 0);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_memcpy_after1) {
    crash_after_recv(this->sockpair, ERROR_MEMCPY, 0, 1);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_memcpy_wrapping0) {
    crash_after_recv(this->sockpair, ERROR_MEMCPY, -4, 0);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_memcpy_wrapping1) {
    crash_after_recv(this->sockpair, ERROR_MEMCPY, -4, 1);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_memcpy_inbetween0) {
    crash_after_recv(this->sockpair, ERROR_MEMCPY, RING_BUFFER_SIZE - 2, 0);
}
TEST_F(Test_atomic_recv, crash_recvmmsg_memcpy_inbetween1) {
    crash_after_recv(this->sockpair, ERROR_MEMCPY, RING_BUFFER_SIZE - 2, 1);
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
    struct atomic_ring_buffer buf = ATOMIC_RING_BUFFER_INIT;

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
    EXPECT_EQ(ACTIVE_RANGE(&buf)->len, 0);

    // ECONNRESET is only returned once.
    char rx[4096];
    EXPECT_EQ(recv(sockpair[0], rx, sizeof(rx), MSG_DONTWAIT), 0);
}
