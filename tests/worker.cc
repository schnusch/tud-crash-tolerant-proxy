#include <gtest/gtest.h>

extern "C" {
#include <errno.h>
#include <netinet/ip.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include "../common/ipc.h"
#include "../common/util.h"
}

class FileDescriptor {
private:
    int fd = -1;
public:
    FileDescriptor(int fd) : fd(fd) { }
    ~FileDescriptor() {
        this->close();
    }
    FileDescriptor(FileDescriptor&& other) noexcept {
        this->close();
        this->fd = other.fd;
        other.fd = -1;
    }
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if(this != &other) {
            this->close();
            this->fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int close(void) {
        if(this->fd < 0) {
            return 0;
        }
        if(::close(this->fd) < 0) {
            return -1;
        }
        this->fd = -1;
        return 0;
    }

    operator int() const noexcept {
        return this->fd;
    }
};

static std::pair<FileDescriptor, FileDescriptor> socketpair_ipc(void) {
    int fd[2];
    if(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd) < 0) {
        throw std::system_error(errno, std::generic_category(), "socketpair");
    }
    return std::pair(FileDescriptor(fd[0]), FileDescriptor(fd[1]));
}

static std::pair<FileDescriptor, FileDescriptor> socketpair_inet(void) {
    // Create listening socket.
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
    };
    FileDescriptor listen_fd(socket(addr.sin_family, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if(listen_fd < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }
    const int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        throw std::system_error(errno, std::generic_category(), "bind");
    }
    if(listen(listen_fd, SOMAXCONN) < 0) {
        throw std::system_error(errno, std::generic_category(), "listen");
    }

    // Connect to listening socket.
    socklen_t addr_len = sizeof(addr);
    if(getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len) < 0) {
        throw std::system_error(errno, std::generic_category(), "getsockname");
    }
    FileDescriptor conn(socket(addr.sin_family, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if(conn < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }
    if(connect(conn, (struct sockaddr *)&addr, addr_len) < 0) {
        throw std::system_error(errno, std::generic_category(), "connect");
    }

    // Accept connection.
    FileDescriptor acc(accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC));
    if(acc < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }

    return std::pair(std::move(acc), std::move(conn));
}

extern char **environ;

class worker : public testing::Test {
protected:
    std::pair<FileDescriptor, FileDescriptor> ipc = std::pair(FileDescriptor(-1), FileDescriptor(-1));
    FileDescriptor memfd = FileDescriptor(-1);
    pid_t pid = FileDescriptor(-1);

    void SetUp() override {
        signal(SIGPIPE, SIG_IGN);

        this->ipc = socketpair_ipc();

        static char *const argv[] = {
            "../bin/worker",
            "--upstream-addr=[::1]:0",
            "--ipc-direct=0",
            NULL,
        };
        posix_spawn_file_actions_t file_actions;
        ASSERT_EQ(posix_spawn_file_actions_init(&file_actions), 0);
        ASSERT_EQ(posix_spawn_file_actions_adddup2(&file_actions, this->ipc.second, 0), 0);
        ASSERT_EQ(
            posix_spawnp(
                &this->pid,
                argv[0],
                &file_actions,
                NULL,
                argv,
                environ
            ),
            0
        );
        ASSERT_EQ(posix_spawn_file_actions_destroy(&file_actions), 0);
        ASSERT_EQ(this->ipc.second.close(), 0);
    }

    void TearDown() override {
        if(this->pid > 0) {
            EXPECT_EQ(kill(this->pid, SIGINT), 0);
            int wstatus;
            do {
                if(waitpid(this->pid, &wstatus, 0) < 0) {
                    if(errno == EINTR) {
                        continue;
                    }
                    perror("waitpid");
                    break;
                }
            } while(!WIFEXITED(wstatus) && !WIFSIGNALED(wstatus));
            EXPECT_TRUE(WIFSIGNALED(wstatus));
            EXPECT_EQ(WTERMSIG(wstatus), SIGINT);
        }
        signal(SIGPIPE, SIG_DFL);
    }
};

static int ipc_connect(const char *action, size_t slot, int fd, const char *tail, void *ctx) {
    *(size_t *)ctx = slot;
    return 0;
}

static int ipc_error(const char *action, size_t slot, int fd, const char *tail, void *ctx) {
    return -1;
}

static void create_connection(FileDescriptor *pdown, FileDescriptor *pup, FileDescriptor& ipc, size_t slot) {
    auto downstream = socketpair_inet();
    ASSERT_GE(ipc_send(ipc, "accepted", slot, downstream.second, NULL), 0)
        << strerror(errno) << " (" << errno << ")";
    ASSERT_EQ(downstream.second.close(), 0);

    struct pollfd pfd = {
        .fd = ipc,
        .events = POLLIN,
    };
    int npoll;
    do {
        npoll = poll(&pfd, 1, 0);
    } while(npoll == 0);
    ASSERT_EQ(npoll, 1);

    static const struct ipc_action_method ipc_methods[] = {
        {"connect", ipc_connect},
        {NULL, ipc_error},
    };
    size_t connected_slot = -1;
    ASSERT_EQ(ipc_process_incoming(ipc, ipc_methods, &connected_slot), 0);
    ASSERT_EQ(connected_slot, slot);

    auto upstream = socketpair_inet();
    ASSERT_GE(ipc_send(ipc, "connected", slot, upstream.second, NULL), 0);
    ASSERT_EQ(upstream.second.close(), 0);

    *pdown = std::move(downstream.first);
    *pup = std::move(upstream.first);
}

TEST_F(worker, one) {
    FileDescriptor downstream(-1), upstream(-1);
    create_connection(&downstream, &upstream, this->ipc.first, 1);

    char rx[512];
    char tx[512];

    // send upload
    memset(tx, 'a', sizeof(tx));
    ASSERT_EQ(send(downstream, tx, sizeof(tx), 0), sizeof(tx));
    ASSERT_EQ(shutdown(downstream, SHUT_WR), 0);

    // send download
    memset(tx, 'b', sizeof(tx));
    ASSERT_EQ(send(upstream, tx, sizeof(tx), 0), sizeof(tx));

    // recv upload
    ASSERT_EQ(recv(upstream, rx, sizeof(rx), 0), sizeof(rx));
    memset(tx, 'a', sizeof(tx));
    EXPECT_EQ(std::string(rx, sizeof(rx)), std::string(tx, sizeof(tx)));
    ASSERT_EQ(recv(upstream, rx, sizeof(rx), 0), 0);

    // recv download
    ASSERT_EQ(recv(downstream, rx, sizeof(rx), 0), sizeof(rx));
    memset(tx, 'b', sizeof(tx));
    EXPECT_EQ(std::string(rx, sizeof(rx)), std::string(tx, sizeof(tx)));

    // send more download
    memset(tx, 'c', sizeof(tx));
    ASSERT_EQ(send(upstream, tx, sizeof(tx), 0), sizeof(tx));
    ASSERT_EQ(shutdown(upstream, SHUT_WR), 0);

    // recv download
    ASSERT_EQ(recv(downstream, rx, sizeof(rx), 0), sizeof(rx));
    memset(tx, 'c', sizeof(tx));
    EXPECT_EQ(std::string(rx, sizeof(rx)), std::string(tx, sizeof(tx)));
    ASSERT_EQ(recv(downstream, rx, sizeof(rx), 0), 0);
}

TEST_F(worker, two) {
    FileDescriptor downstream1(-1), upstream1(-1);
    create_connection(&downstream1, &upstream1, this->ipc.first, 0);

    FileDescriptor downstream2(-1), upstream2(-1);
    create_connection(&downstream2, &upstream2, this->ipc.first, 1);

    char rx[512];
    char tx[512];

    // send upload on connection 2
    memset(tx, 'A', sizeof(tx));
    ASSERT_EQ(send(downstream2, tx, sizeof(tx), 0), sizeof(tx));
    ASSERT_EQ(shutdown(downstream2, SHUT_WR), 0);
    // send download on connection 2
    memset(tx, 'B', sizeof(tx));
    ASSERT_EQ(send(upstream2, tx, sizeof(tx), 0), sizeof(tx));

    // send upload on connection 1
    memset(tx, 'C', sizeof(tx));
    ASSERT_EQ(send(downstream1, tx, sizeof(tx), 0), sizeof(tx));
    ASSERT_EQ(shutdown(downstream1, SHUT_WR), 0);
    // send download on connection 1
    memset(tx, 'D', sizeof(tx));
    ASSERT_EQ(send(upstream1, tx, sizeof(tx), 0), sizeof(tx));
    ASSERT_EQ(shutdown(upstream1, SHUT_WR), 0);

    // receive upload from connection 2
    ASSERT_EQ(recv(upstream2, rx, sizeof(rx), 0), sizeof(rx));
    memset(tx, 'A', sizeof(tx));
    EXPECT_EQ(std::string(rx, sizeof(rx)), std::string(tx, sizeof(tx)));
    ASSERT_EQ(recv(upstream2, rx, sizeof(rx), 0), 0);

    // receive upload on connection 1
    ASSERT_EQ(recv(upstream1, rx, sizeof(rx), 0), sizeof(rx));
    memset(tx, 'C', sizeof(tx));
    EXPECT_EQ(std::string(rx, sizeof(rx)), std::string(tx, sizeof(tx)));
    ASSERT_EQ(recv(upstream1, rx, sizeof(rx), 0), 0);
    // receive download on connection 1
    ASSERT_EQ(recv(downstream1, rx, sizeof(rx), 0), sizeof(rx));
    memset(tx, 'D', sizeof(tx));
    EXPECT_EQ(std::string(rx, sizeof(rx)), std::string(tx, sizeof(tx)));
    ASSERT_EQ(recv(downstream1, rx, sizeof(rx), 0), 0);

    // receive download on connection 2
    ASSERT_EQ(recv(downstream2, rx, sizeof(rx), 0), sizeof(rx));
    memset(tx, 'B', sizeof(tx));
    EXPECT_EQ(std::string(rx, sizeof(rx)), std::string(tx, sizeof(tx)));

    // send more download on connection 2
    memset(tx, 'E', sizeof(tx));
    ASSERT_EQ(send(upstream2, tx, sizeof(tx), 0), sizeof(tx));
    ASSERT_EQ(shutdown(upstream2, SHUT_WR), 0);

    // receive more download on connection 2
    ASSERT_EQ(recv(downstream2, rx, sizeof(rx), 0), sizeof(rx));
    memset(tx, 'E', sizeof(tx));
    EXPECT_EQ(std::string(rx, sizeof(rx)), std::string(tx, sizeof(tx)));
    ASSERT_EQ(recv(downstream2, rx, sizeof(rx), 0), 0);
}
