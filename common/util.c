#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

#include "util.h"

ssize_t readall(int fd, void *buf, size_t *size) {
    char *p = buf;
    size_t remaining = *size;
    while(remaining > 0) {
        ssize_t n;
        EINTR_RETRY(n, read(fd, p, remaining));
        if(n < 0) {
            *size = p - (char *)buf;
            return -1;
        }
        if(n == 0) {
            break;
        }
        p += n;
        remaining -= n;
    }
    *size = p - (char *)buf;
    return *size;
}

ssize_t writeall(int fd, const void *buf, size_t *size) {
    const char *p = buf;
    size_t remaining = *size;
    while(remaining > 0) {
        ssize_t n;
        EINTR_RETRY(n, write(fd, p, remaining));
        if(n < 0) {
            *size = p - (char *)buf;
            return -1;
        }
        p += n;
        remaining -= n;
    }
    *size = p - (char *)buf;
    return *size;
}

int fd_cloexec(int fd, int set) {
    int flags = fcntl(fd, F_GETFD);
    if(flags < 0) {
        return -1;
    }
    if(set) {
        if(!(flags & FD_CLOEXEC) && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
            return -1;
        }
    } else {
        if((flags & FD_CLOEXEC) && fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) < 0) {
            return -1;
        }
    }
    return 0;
}

ssize_t recv_fd(int sock, void *buf, size_t size, int *fd, int flags) {
    struct iovec io = {
        .iov_base = buf,
        .iov_len = size,
    };
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {
        .msg_iov = &io,
        .msg_iovlen = 1,
        .msg_control = cmsgbuf,
        .msg_controllen = sizeof(cmsgbuf),
    };

    ssize_t n;
    EINTR_RETRY(n, recvmsg(sock, &msg, flags));
    if(n < 0) {
        return -1;
    }

    *fd = -1;
    for(struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if(cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            // On Linux only one SCM_RIGHTS item can be sent, but if for any
            // reason this is not the case close all other received file
            // descriptors.
            int received_fd = *((int *)CMSG_DATA(cmsg));
            if(*fd < 0) {
                *fd = received_fd;
            } else {
                close(received_fd);
            }
        }
    }

    return n;
}

ssize_t send_fd(int sock, const void *buf, size_t size, int fd, int flags) {
    struct iovec io = {
        .iov_base = (void *)buf,
        .iov_len = size,
    };
    struct msghdr msg = {
        .msg_iov = &io,
        .msg_iovlen = 1,
        .msg_control = NULL,
        .msg_controllen = 0,
    };

    char cmsgbuf[CMSG_SPACE(sizeof(int))] = {0};
    if(fd >= 0) {
        msg.msg_control = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        *((int *)CMSG_DATA(cmsg)) = fd;
    }

    ssize_t n;
    EINTR_RETRY(n, sendmsg(sock, &msg, flags));
    return n;
}

size_t strlcpy(char *dst, const char *src, size_t size) {
    char *end = stpncpy(dst, src, size);
    if(end < dst + size) {
        // strncpy null-terminated the string
        return end - dst;
    }
    end[-1] = '\0';
    // `src` is at least `end - dst` bytes long, no need to run strlen on that
    // part again.
    size_t len = end - dst;
    len += strlen(src + len);
    return len;
}

char *format_sockaddr(char *buf, const struct sockaddr *addr) {
    char *p = buf;
    uint16_t port = 0;
    switch(addr->sa_family) {
    case AF_INET:
        if(!inet_ntop(AF_INET, &((struct sockaddr_in *)addr)->sin_addr, p, INET_ADDRSTRLEN)) {
            strlcpy(buf, "ERROR FORMATTING ADDRESS", FORMAT_SOCKADDR_BUFLEN);
            return buf;
        }
        port = ((struct sockaddr_in *)addr)->sin_port;
        p += strlen(p);
        break;
    case AF_INET6:
        *p++ = '[';
        if(!inet_ntop(AF_INET6, &((struct sockaddr_in6 *)addr)->sin6_addr, p, INET6_ADDRSTRLEN)) {
            strlcpy(buf, "ERROR FORMATTING ADDRESS", FORMAT_SOCKADDR_BUFLEN);
            return buf;
        }
        port = ((struct sockaddr_in6 *)addr)->sin6_port;
        p += strlen(p);
        *p++ = ']';
        break;
    case AF_UNIX:
        strlcpy(buf, "AF_UNIX", FORMAT_SOCKADDR_BUFLEN);
        return buf;
    default:
        strlcpy(buf, "UNSUPPORTED ADDRESS", FORMAT_SOCKADDR_BUFLEN);
        return buf;
    }
    snprintf(p, FORMAT_SOCKADDR_BUFLEN - (p - buf), ":%"PRIu16, ntohs(port));
    buf[FORMAT_SOCKADDR_BUFLEN - 1] = '\0';
    return buf;
}

int parse_sockaddr(struct sockaddr_storage *addr, const char *str_addr) {
    if(strchr(str_addr, '/')) {
        struct sockaddr_un *un = (struct sockaddr_un *)addr;
        un->sun_family = AF_UNIX;
        if(strlcpy(un->sun_path, str_addr, sizeof(un->sun_path)) >= sizeof(un->sun_path)) {
            errno = EOVERFLOW;
            return -1;
        }
        return 0;
    }

    const char *str_port;
    void *dst_addr;
    uint16_t *dst_port;

    if(*str_addr == '[') {
        // format [::1]:12345
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)addr;
        in6->sin6_family = AF_INET6;

        ++str_addr;
        str_port = strchr(str_addr, ']');
        if(!str_port || str_port[1] != ':') {
            errno = EINVAL;
            return -1;
        }
        str_addr = strndupa(str_addr, str_port - str_addr);
        str_port += 2;

        dst_addr = &in6->sin6_addr;
        dst_port = &in6->sin6_port;
    } else {
        // format 127.0.0.1:12345
        struct sockaddr_in *in4 = (struct sockaddr_in *)addr;
        in4->sin_family = AF_INET;

        str_port = strchr(str_addr, ':');
        if(!str_port) {
            errno = EINVAL;
            return -1;
        }
        str_addr = strndupa(str_addr, str_port - str_addr);
        ++str_port;

        dst_addr = &in4->sin_addr;
        dst_port = &in4->sin_port;
    }

    int e = inet_pton(addr->ss_family, str_addr, dst_addr);
    if(e < 0) {
        return -1;
    } else if(e == 0) {
        errno = EINVAL;
        return -1;
    }

    errno = 0;
    char *end;
    long l = strtol(str_port, &end, 0);
    if(errno != 0) {
        return -1;
    } else if(*end) {
        errno = EINVAL;
        return -1;
    } else if(l < 0 || UINT16_MAX < l) {
        errno = ERANGE;
        return -1;
    }
    *dst_port = htons(l);

    return 0;
}

char *json_strdup(const char *str) {
    // Calculate length.
    size_t json_size = 3; // two quotes and the NUL byte
    for(const char *src = str; *src; ++src) {
        if(*src < ' ' || *src == '"' || *src == '\\' || '~' < *src) {
            json_size += 6; // \u00XX
        } else {
            ++json_size;
        }
    }

    char *json = malloc(json_size);
    if(!json) {
        return NULL;
    }

    // Escape string.
    char *dst = json;
    *dst++ = '"';
    for(const char *src = str; *src; ++src) {
        if(*src < ' ' || *src == '"' || *src == '\\' || '~' < *src) {
            if(dst + 6 > json + json_size) {
                goto eoverflow;
            }
            static const char hex_digits[] = {
                '0', '1', '2', '3', '4', '5', '6', '7',
                '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
            };
            *dst++ = '\\';
            *dst++ = 'u';
            *dst++ = '0';
            *dst++ = '0';
            *dst++ = hex_digits[(*src >> 4) & 0x0F];
            *dst++ = hex_digits[*src & 0x0F];
        } else {
            if(dst + 1 > json + json_size) {
                goto eoverflow;
            }
            *dst++ = *src;
        }
    }
    if(dst + 2 > json + json_size) {
        goto eoverflow;
    }
    *dst++ = '"';
    *dst++ = '\0';

    return json;

eoverflow:
    free(json);
    errno = EOVERFLOW;
    return NULL;
}

int list_fds(const char *prefix) {
    int rc = -1;
    int dir_fd = -1;
    char *target = NULL;
    DIR *d = NULL;

    dir_fd = open("/proc/self/fd", O_DIRECTORY | O_RDONLY);
    if(dir_fd < 0) {
        goto cleanup;
    }

    d = fdopendir(dir_fd);
    if(!d) {
        goto cleanup;
    }

    size_t target_size = 1024;
    target = malloc(target_size);
    if(!target) {
        goto cleanup;
    }

    struct dirent *e;
    while((e = readdir(d))) {
        if(
            e->d_name[0] == '.' && (
                e->d_name[1] == '\0'
                || (e->d_name[1] == '.' && e->d_name[2] == '\0')
            )
        ) {
            continue;
        }

        // Parse file descriptor.
        errno = 0;
        char *end;
        unsigned long l = strtoul(e->d_name, &end, 10);
        if(errno != 0) {
            goto cleanup;
        } else if(l > INT_MAX) {
            errno = ERANGE;
            goto cleanup;
        }
        int fd = l;

        // readlinkat(2)
        ssize_t n;
        while(1) {
            n = readlinkat(dir_fd, e->d_name, target, target_size);
            if(n < 0) {
                perror("readlinkat");
                goto cleanup;
            } else if((size_t)n < target_size) {
                target[n] = '\0';
                break;
            }

            // Grow readlink buffer.
            char *new_target = realloc(target, target_size + 1024);
            if(!new_target) {
                goto cleanup;
            }
            target = new_target;
            target_size += 1024;
        }

        int flags = fcntl(fd, F_GETFD);

        // ...
        LOG(
            "%s"
            "%s"
            "%2d"
            " fdflags=%s"
            " -> %s\n",
            prefix,
            (fd == dir_fd) ? "dir_fd=" : "       ",
            fd,
            (flags < 0) ? "-1        " : (flags & FD_CLOEXEC) ? "FD_CLOEXEC" : "0         ",
            target
        );
    }

    rc = 0;
cleanup:
    ;int errnum = errno;
    free(target);
    if(d) {
        closedir(d);
    }
    // Even though Linux' fdopendir(2) says
    //
    // > After a successful call to fdopendir(), fd is used internally by the
    // > implementation, and should not otherwise be used by the application.
    //
    // and [POSIX](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fdopendir.html)
    // says
    //
    // > Upon calling closedir() the file descriptor shall be closed.
    //
    // it still needs to be closed manually.
    close(dir_fd);
    errno = errnum;
    return rc;
}

int array_append(void **array, size_t *len, const void *elem, size_t size) {
    void *new_array = realloc(*array, (*len + 1) * size);
    if(!new_array) {
        return -1;
    }
    *array = new_array;
    memcpy((char *)*array + (*len * size), elem, size);
    ++(*len);
    return 0;
}

long strtol_limit(int *err, const char *str, long min, long max) {
    errno = 0;
    char *end;
    long l = strtol(str, &end, 0);
    if((l == LONG_MIN || l == LONG_MAX) && errno != 0) {
        *err = 1;
        return 0;
    }
    for(; *end != '\0'; ++end) {
        if(!isspace(*end)) {
            errno = EINVAL;
            *err = 1;
            return 0;
        }
    }
    if(l < min || max < l) {
        errno = ERANGE;
        *err = 1;
        return 0;
    }
    *err = 0;
    return l;
}

const char *signame(int signum) {
    static char unknown[] = "unknown signal -2147483648";
    switch(signum) {
#define SIG_MACRO(x) case x: return #x;
#include "siglist.h"
#undef SIG_MACRO
    default:
        snprintf(unknown, sizeof(unknown), "unknown signal %d", signum);
        return unknown;
    }
}

void closep(int *fd) {
    int errbak = errno;
    if(close(*fd) < 0) {
        perror("close");
    } else {
        *fd = -1;
    }
    errno = errbak;
}
