#define _DEFAULT_SOURCE // pwritev
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/uio.h> // pwritev
#include <sys/un.h>
#include <unistd.h>

#include "util.h"

static void print_log_prefix(const char *filename, unsigned int lineno, const char *func) {
    static const char indent[] = "                                       ";
    static const char *const indent_end = indent + sizeof(indent) - 1;
    _Static_assert(sizeof(indent) - 1 >= 39, "indent buffer too small");
    int n;

    n = fprintf(stderr, "[%d] %s:%d ", (int)getpid(), filename, lineno);
    if(0 <= n && (size_t)n < 39) {
        fputs(indent_end - 39 + n, stderr);
    }

    n = fprintf(stderr, "%s: ", func);
    if(0 <= n && (size_t)n < 26) {
        fputs(indent_end - 26 + n, stderr);
    }
}

#ifdef USE_LIBBACKTRACE
#include <backtrace.h>

static struct backtrace_state *bt_state = NULL;

static void bt_error(void *ctx_, const char *msg, int errnum) {
    (void)ctx_;
    LOG(LOG_ERROR, "libbacktrace: %s: %s\n", msg, strerror(errnum));
}

struct backtrace_entry {
    struct backtrace_entry *next;
    const char *function;
    const char *filename;
    int lineno;
};

static int store_backtrace(void *ctx_, uintptr_t pc, const char *filename, int lineno, const char *function) {
    (void)pc;
    struct backtrace_entry **root = ctx_;
    if(!function) {
        function = "<?>";
    }
    if(!filename) {
        filename = "<?>";
    }

    const size_t size = sizeof(**root) + strlen(function) + 1 + strlen(filename) + 1;

    struct backtrace_entry *const entry = malloc(size);
    if(!entry) {
        perror("malloc");
        return -1;
    }
    *entry = (struct backtrace_entry){
        .next = *root,
        .lineno = lineno,
    };

    char *const end = (char *)entry + size;
    char *p = (char *)entry + sizeof(*entry);

    entry->function = p;
    p += strlcpy(p, function, end - p);
    ++p; // NUL-byte
    assert(p <= end);

    entry->filename = p;
    p += strlcpy(p, filename, end - p);
    ++p; // NUL-byte
    assert(p <= end);

    *root = entry;

    return 0;
}

void print_backtrace(int skip) {
    if(!bt_state) {
        return;
    }
    struct backtrace_entry *root = NULL;
    if(backtrace_full(bt_state, skip + 2, store_backtrace, bt_error, &root) == 0) {
        for(struct backtrace_entry *e = root; e; e = e->next) {
            print_log_prefix(e->filename, e->lineno, e->function);
            fputs("\xE2\x86\xB5\n", stderr);
        }
    }
    while(root) {
        struct backtrace_entry *next = root->next;
        free(root);
        root = next;
    }
}
#endif

#ifndef DEFAULT_LOG_LEVEL
#define DEFAULT_LOG_LEVEL (INT_MAX & ~1)
#endif
int log_level = DEFAULT_LOG_LEVEL;

void init_log_level(void) {
    const char *level = getenv("LOG_LEVEL");
    if(level) {
        int e;
        log_level = strtol_limit(&e, level, INT_MIN, INT_MAX);
        if(e) {
            log_level = DEFAULT_LOG_LEVEL;
            perror("strtol");
        }
        log_level &= ~1;
    }

#ifdef USE_LIBBACKTRACE
    bt_state = backtrace_create_state(NULL, 0, &bt_error, NULL);
#endif
}

void _log(int level, const char *filename, unsigned int lineno, const char *func, const char *fmt, ...) {
    int errnum = errno;

    int unlock = fcntl(STDERR_FILENO, F_SETLKW, &(struct flock){ .l_type = F_WRLCK }) == 0;

#ifdef USE_LIBBACKTRACE
    if(level & LOG_BACKTRACE) {
        print_backtrace(1);
    }
#endif
    print_log_prefix(filename, lineno, func);

    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);

    if(unlock) {
        fcntl(STDERR_FILENO, F_SETLKW, &(struct flock){ .l_type = F_UNLCK });
    }

    errno = errnum;
}

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

size_t pwritev_all(int fd, const struct iovec *iov, size_t iovcnt, off_t offset) {
    struct iovec *iov_copy = alloca(iovcnt * sizeof(*iov));
    memcpy(iov_copy, iov, iovcnt * sizeof(*iov));
    size_t total = 0;
    while(iovcnt > 0) {
        ssize_t n;
        EINTR_RETRY(n, pwritev(fd, iov_copy, iovcnt, offset));
        if(n <= 0) {
            break;
        }
        offset += n;
        total += n;
        // Drop the first `n` bytes.
        while(n > 0) {
            assert(iovcnt > 0);
            if((size_t)n >= iov_copy->iov_len) {
                n -= iov_copy->iov_len;
                ++iov_copy;
                --iovcnt;
            } else {
                iov_copy->iov_base = (char *)iov_copy->iov_base + n;
                iov_copy->iov_len -= n;
                break; // n = 0;
            }
        }
    }
    return total;
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

size_t strlcpy(char *dst, const char *src, size_t size) {
    char *end = stpncpy(dst, src, size);
    if(end < dst + size) {
        // strncpy null-terminated the string
        return end - dst;
    }
    if(end > dst) {
        end[-1] = '\0';
    }
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
            return NULL;
        }
        port = ((struct sockaddr_in *)addr)->sin_port;
        p += strlen(p);
        break;
    case AF_INET6:
        *p++ = '[';
        if(!inet_ntop(AF_INET6, &((struct sockaddr_in6 *)addr)->sin6_addr, p, INET6_ADDRSTRLEN)) {
            return NULL;
        }
        port = ((struct sockaddr_in6 *)addr)->sin6_port;
        p += strlen(p);
        *p++ = ']';
        break;
    default:
        errno = ENOTSUP;
        return NULL;
    }
    size_t remaining = FORMAT_SOCKADDR_BUFLEN - (p - buf);
    int used = snprintf(p, remaining, ":%"PRIu16, ntohs(port));
    if(used < 0) {
        return NULL;
    } else if((size_t)used >= remaining) {
        errno = EOVERFLOW;
        return NULL;
    }
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
        *in6 = (struct sockaddr_in6){
            .sin6_family = AF_INET6,
        };

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

static const char json_chars[256] = {
    0, 0, 0,   0, 0, 0, 0, 0, 'b', 't', 'n', 0, 'f',  'r', 0, 0,
    0, 0, 0,   0, 0, 0, 0, 0, 0,   0,   0,   0, 0,    0,   0, 0,
    1, 1, '"', 1, 1, 1, 1, 1, 1,   1,   1,   1, 1,    1,   1, 1,
    1, 1, 1,   1, 1, 1, 1, 1, 1,   1,   1,   1, 1,    1,   1, 1,
    1, 1, 1,   1, 1, 1, 1, 1, 1,   1,   1,   1, 1,    1,   1, 1,
    1, 1, 1,   1, 1, 1, 1, 1, 1,   1,   1,   1, '\\', 1,   1, 1,
    1, 1, 1,   1, 1, 1, 1, 1, 1,   1,   1,   1, 1,    1,   1, 1,
    1, 1, 1,   1, 1, 1, 1, 1, 1,   1,   1,   1, 1,    1,   1
};

size_t json_bufsize(const char *str, size_t len) {
    size_t n = 3;
    for(size_t i = 0; i < len; ++i) {
        switch(json_chars[(unsigned char)str[i]]) {
        case 0:
            n += 6;
            break;
        case 1:
            n += 1;
            break;
        default:
            n += 2;
            break;
        }
    }
    return n;
}

size_t json_strlcpy(char *json, size_t json_size, const char *src, size_t len) {
    char *dst = json;
    *dst++ = '"';
    for(; len > 0; --len, ++src) {
        char esc = json_chars[(unsigned char)*src];
        if(esc == 0) {
            if(dst + 6 > json + json_size) {
                goto overflow;
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
        } else if(esc == 1) {
            if(dst + 1 > json + json_size) {
                goto overflow;
            }
            *dst++ = *src;
        } else {
            if(dst + 2 > json + json_size) {
                goto overflow;
            }
            *dst++ = '\\';
            *dst++ = esc;
        }
    }
    if(dst + 2 > json + json_size) {
        goto overflow;
    }
    *dst++ = '"';
    *dst = '\0';

    return dst - json;

overflow:
    if(json_size > 0) {
        json[json_size - 1] = '\0';
    }
    // Subtract NUL-byte a doubled leading quote (").
    return dst - json + json_bufsize(src, len) - 2;
}

char *json_strdup(const char *str) {
    size_t len = strlen(str);
    size_t json_size = json_bufsize(str, len);
    char *json = malloc(json_size);
    if(!json) {
        return NULL;
    }
    if(json_strlcpy(json, json_size, str, len) >= json_size) {
        free(json);
        errno = EOVERFLOW;
        return NULL;
    }
    return json;
}

int _list_fds(int level, const char *filename, unsigned int lineno, const char *func) {
    if(level > log_level) {
        return 0;
    }

    int unlock = 0;

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

    unlock = fcntl(STDERR_FILENO, F_SETLKW, &(struct flock){ .l_type = F_WRLCK }) == 0;
#ifdef USE_LIBBACKTRACE
    if(level & LOG_BACKTRACE) {
        print_backtrace(1);
    }
#endif

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
        static const struct str_bit bits[] = {
            { .num = FD_CLOEXEC, .str = "FD_CLOEXEC" },
            {0, NULL}
        };

        print_log_prefix(filename, lineno, func);
        char str_flags[512];
        fprintf(
            stderr,
            "fd=%d fdflags=%s -> %s\n",
            fd,
            (flags < 0) ? "-1" : str_bits(bits, str_flags, sizeof(str_flags), flags),
            target
        );
    }

    rc = 0;
cleanup:
    if(unlock) {
        fcntl(STDERR_FILENO, F_SETLKW, &(struct flock){ .l_type = F_UNLCK });
    }
    ;int errnum = errno;
    free(target);
    if(d) {
        closedir(d);
    } else {
        closep(&dir_fd);
    }
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

char *str_bits(const struct str_bit *bits, char *buf, size_t size, uint64_t events) {
    uint64_t rm = 0;
    char *const end = buf + size;
    char *p = buf;
    for(size_t i = 0; bits[i].str && p < end; ++i) {
        if((events & bits[i].num) == bits[i].num) {
            rm |= bits[i].num;
            p += strlcpy(p, bits[i].str, end - p);
            if(p >= end) {
                break;
            }
            p += strlcpy(p, " | ", end - p);
        }
    }
    if(p >= end) {
trunc:
        if(size >= 4) {
            p = end - 4;
        } else {
            p = buf;
        }
        strlcpy(p, "...", end - p);
        return buf;
    }
    events &= ~rm;
    if(events != 0 || p == buf) {
        int n = snprintf(p, end - p, "0x%"PRIX64, events);
        if(n < 0 || end - p <= n) {
            goto trunc;
        }
    } else {
        assert(p - buf >= 3);
        p[-3] = '\0';
    }
    return buf;
}

char *epoll_str(char *buf, size_t size, uint32_t events) {
    static const struct str_bit bits[] = {
#define EPOLL_STR(x) { .num = x, .str = #x }
        EPOLL_STR(EPOLLIN),
        EPOLL_STR(EPOLLOUT),
        EPOLL_STR(EPOLLRDHUP),
        EPOLL_STR(EPOLLPRI),
        EPOLL_STR(EPOLLERR),
        EPOLL_STR(EPOLLHUP),
        EPOLL_STR(EPOLLET),
        EPOLL_STR(EPOLLONESHOT),
        EPOLL_STR(EPOLLWAKEUP),
        EPOLL_STR(EPOLLEXCLUSIVE),
#undef EPOLL_STR
        {0, NULL}
    };
    return str_bits(bits, buf, size, events);
}

int closep(int *fd) {
    if(*fd < 0) {
        return 0;
    }
    int e = close(*fd);
    // > Retrying the close() after a failure return is the wrong thing to do,
    // > since this may cause a reused file descriptor from another thread to
    // > be closed.
    // see "Dealing with error returns from close()" at
    // https://man7.org/linux/man-pages/man2/close.2.html#CAVEATS
    if(e == 0 || errno != EBADF) {
        *fd = -1;
    }
    return e;
}

void closep_no_error(int *fd) {
    int errbak = errno;
    if(closep(fd) < 0) {
        perror("close");
    }
    errno = errbak;
}
