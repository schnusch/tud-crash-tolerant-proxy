#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h> // struct msghdr, ...

#include "ipc.h"
#include "util.h"

/**
 * The maximum size of IPC datagrams including the terminating NUL-byte.
 */
#define IPC_MAXSIZE 511

int ipc_send(int ipc_fd, const char *action, size_t slot, int fd, const char *fmt, ...) {
    // Messages have the following format: "${action} slot=${slot}\0"
    char buf[IPC_MAXSIZE];
    int n = snprintf(buf, sizeof(buf), "%s slot=%zu", action, slot);
    if(n < 0) {
        return -1;
    } else if((size_t)n >= sizeof(buf)) {
        errno = EOVERFLOW;
        return -1;
    }
    size_t size = n;

    // Append a space and the format string.
    if(fmt) {
        buf[size++] = ' ';

        va_list va;
        va_start(va, fmt);
        n = vsnprintf(buf + size, sizeof(buf) - size, fmt, va);
        va_end(va);

        if(n < 0) {
            return -1;
        } else if((size_t)n >= sizeof(buf) - size) {
            errno = EOVERFLOW;
            return -1;
        }
        size += n;
    }

    // Include terminating NUL byte.
    ++size;

    // Send `buf`.
    struct iovec io = {
        .iov_base = buf,
        .iov_len = size,
    };
    struct msghdr msg = {
        .msg_iov = &io,
        .msg_iovlen = 1,
        .msg_control = NULL,
        .msg_controllen = 0,
    };

    // Send file descriptor.
    char cmsgbuf[CMSG_SPACE(sizeof(int))] = {0};
    if(fd >= 0) {
        msg.msg_control = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        // > **CMSG_DATA()**
        // >     returns a pointer to the data portion of a cmsghdr.  The
        // >     pointer returned cannot be assumed to be suitably aligned
        // >     for accessing arbitrary payload data types.  Applications
        // >     should not cast it to a pointer type matching the payload,
        // >     but should instead use memcpy(3) to copy data to or from a
        // >     suitably declared object.
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    }

    ssize_t m;
    EINTR_RETRY(m, sendmsg(ipc_fd, &msg, 0));
    return m;
}

/**
 * Discard all ancillary data except for the first received file descriptor.
 */
static int extract_one_fd(struct msghdr *msg) {
    int fd = -1;
    size_t num_received = 0;
    size_t num_closed = 0;
    for(struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if(cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            // On Linux only one SCM_RIGHTS item can be sent, but if for any
            // reason this is not the case close all other received file
            // descriptors.
            int received_fd;
            // > **CMSG_DATA()**
            // >     returns a pointer to the data portion of a cmsghdr.  The
            // >     pointer returned cannot be assumed to be suitably aligned
            // >     for accessing arbitrary payload data types.  Applications
            // >     should not cast it to a pointer type matching the payload,
            // >     but should instead use memcpy(3) to copy data to or from a
            // >     suitably declared object.
            memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(received_fd));
            ++num_received;
            if(fd < 0) {
                if(fd_cloexec(received_fd, 0) < 0) {
                    perror("fcntl");
                }
                fd = received_fd;
            } else if(close(received_fd) >= 0) {
                ++num_closed;
            }
        }
    }
    if(num_received > 1) {
        LOG("received %zu file descriptors instead of 1, closed %zu\n", num_received, num_closed);
    }
    return fd;
}

int ipc_process_incoming(
    int ipc_fd,
    const struct ipc_action_method *methods,
    void *ctx
) {
    int received_fd = -1;
    while(1) {
        if(received_fd >= 0 && close(received_fd) < 0) {
            perror("close");
            if(fd_cloexec(received_fd, 1) < 0) {
                perror("fcntl");
                // TODO fatal? The received_fd will be forgotten but inherited
                // across exec(2).
            }
        }

        char buf[IPC_MAXSIZE + 1];
        struct iovec io = {
            .iov_base = buf,
            .iov_len = sizeof(buf)
        };
        char cmsgbuf[CMSG_SPACE(sizeof(int))] = {0};
        struct msghdr msg = {
            .msg_iov = &io,
            .msg_iovlen = 1,
            .msg_control = cmsgbuf,
            .msg_controllen = sizeof(cmsgbuf),
        };
        ssize_t n;
        EINTR_RETRY(n, recvmsg(ipc_fd, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT));
        if(n < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            } else {
                return -1;
            }
        } else if(n == 0) {
            // EOF, other end disconnected.
            errno = ECONNRESET; // FIXME
            return -1;
        } else if(n == sizeof(buf)) {
            // Message might be truncated, ignore.
            LOG("ignoring potentially truncated IPC message: %.*s\n", (int)n, buf);
            continue; // Poor-man's tail recursion.
        }

        // NUL-terminate the received data.
        buf[n] = '\0';

        // Get file descriptor from `msg_control`.
        received_fd = extract_one_fd(&msg);

        // Parse message.
        LOG("received IPC message: %s\n", buf);
        ssize_t action_len = -1;
        size_t slot = -1;
        ssize_t consumed = -1;
        int num_scanned = sscanf(buf, "%*s%zn slot=%zu%zn", &action_len, &slot, &consumed);
        if(num_scanned < 1 || num_scanned == EOF) {
            LOG("ignoring unparseable IPC message: %s\n", buf);
            continue; // Poor-man's tail recursion.
        } else if(action_len < 0 || consumed < 0) {
            errno = ENOTSUP;
            return -1;
        }

        // Any trailing data of the message is passed to the IPC methods verbatim.
        char *tail = buf + consumed;
        if(*tail == ' ') {
            ++tail;
        } else {
            // If the trailing data is malformed strip it.
            *tail = '\0';
            tail = NULL;
        }

        // NUL-terminate the message's action.
        buf[action_len] = '\0';

        const struct ipc_action_method *m = methods;
        for(; m->action; ++m) {
            if(strcmp(m->action, buf) == 0) {
                break;
            }
        }
        if(m->method) {
            int rc = m->method(buf, slot, received_fd, tail, ctx);
            received_fd = -1;
            if(rc != 0) {
                return rc;
            }
        } else if(m->action) {
            // .action was not NULL but .method was NULL
        } else {
            // No matching method found, no fallback method.
        }

        continue; // Poor-man's tail recursion.
    }
}
