#ifndef COMMON_UTIL_H
#define COMMON_UTIL_H

#ifdef __cplusplus
#define _Static_assert(cond, msg) static_assert(cond, msg)
#endif

#include <assert.h>
#include <arpa/inet.h>
#include <errno.h> // perror: strerror(errno)
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define UTF8_ARROW_WEST "\xE2\x86\x90"
#define UTF8_ARROW_NORTH "\xE2\x86\x91"
#define UTF8_ARROW_EAST "\xE2\x86\x92"
#define UTF8_ARROW_SOUTH "\xE2\x86\x93"

/**
 * Only display log messages of a level less than or equal to this.
 */
extern int log_level;

/**
 * Set `log_level` from environment variable `$LOG_LEVEL`.
 */
void init_log_level(void);

_Static_assert(sizeof(int) * CHAR_BIT >= 32, "32-bit int");
enum {
    /** Print a backtrace before the log message. */
    LOG_BACKTRACE = 1 << 31,
    /**
     * The most significant bit controls whether a backtrace should be printed.
     * Therefore the actual log levels are multiples of two.
     */
    LOG_ALWAYS = 0,
    LOG_ERROR,
    LOG_INFO,
    LOG_DEBUG,
    LOG_DEBUG_HTTP,
    LOG_DEBUG_IPC,
    LOG_DEBUG_STATE,
    LOG_DEBUG_BYTES,
};

/**
 * Only call this function through `LOG`.
 */
void _log(unsigned int level, const char *filename, unsigned int lineno, const char *func, const char *fmt, ...);

#define LOG(LEVEL, ...) ( \
    ((LEVEL) & ((unsigned int)LOG_BACKTRACE - 1)) > log_level \
    ? (void)0 \
    : _log(LEVEL, __FILE__, __LINE__, __func__, __VA_ARGS__) \
)
#define perror(s) LOG(LOG_ALWAYS | LOG_BACKTRACE, "%s: %s\n", (s), strerror(errno))

#ifdef USE_LIBBACKTRACE
void print_backtrace(int skip);
#endif

/**
 * Repeat the syscall `CALL` if it returned `< 0` and set errno to `EINTR`. The
 * result of `CALL` is stored in `RESULT`.
 */
#define EINTR_RETRY(RESULT, CALL) \
    do { \
        (RESULT) = CALL; \
    } while((RESULT) < 0 && errno == EINTR)

/**
 * Read exactly `*size` bytes from `fd` to `buf`, unless an error occurs.
 * The number of bytes actually read is returned in `*size`.
 * \return the number of bytes actually read
 * \return -1 on error
 */
ssize_t readall(int fd, void *buf, size_t *size);

/**
 * Write exactly all data, unless an error occurs.
 * \return the number of bytes actually written, if it is less then the sum of all `iov_len` an error occured
 */
size_t pwritev_all(int fd, const struct iovec *iov, size_t iovcnt, off_t offset);

/**
 * Set or unset `FD_CLOEXEC` on `fd`.
 * \return 0 on success
 * \reutnr -1 on error
 */
int fd_cloexec(int fd, int set);

/**
 * see [strlcpy(3)](https://man.openbsd.org/strlcpy.3)
 */
size_t strlcpy(char *dst, const char *src, size_t size);

/**
 * Minimum size of `buf` passed to `format_sockaddr`.
 */
#define FORMAT_SOCKADDR_BUFLEN (1 + INET6_ADDRSTRLEN + 2 + 5)
_Static_assert(FORMAT_SOCKADDR_BUFLEN >= INET_ADDRSTRLEN + 1 + 5, "FORMAT_SOCKADDR_BUFLEN fits \"255.255.255.255:12345\0\"");
_Static_assert(FORMAT_SOCKADDR_BUFLEN >= 1 + INET6_ADDRSTRLEN + 2 + 5, "FORMAT_SOCKADDR_BUFLEN fits \"[...]:12345\0\"");

/**
 * Write a human-readable representation of `addr` into `buf`. On error a error
 * message is written into `buf`.
 * \return `buf`
 */
char *format_sockaddr(char *buf, const struct sockaddr *addr);

/**
 * Parse `AF_INET`, `AF_INET6`, and `AF_UNIX` addresses.
 * \return 0 on success
 * \return -1 on error
 */
int parse_sockaddr(struct sockaddr_storage *addr, const char *str_addr);

/**
 * Calculate the size of a buffer needed to store a JSON representation
 * of `str`.
 */
size_t json_bufsize(const char *str, size_t len);

/**
 * Copy a JSON representation of `src` to `dst`.
 */
size_t json_strlcpy(char *json, size_t json_size, const char *src, size_t len);

/**
 * Allocate memory and write JSON representation of `str` to it.
 */
char *json_strdup(const char *str);

/**
 * Allocate memory on the stack and write JSON representation of `str` to it.
 */
#define json_strndupa(src, len) _json_strndupa(alloca(json_bufsize(src, len)), src, len)
static inline char *_json_strndupa(char *dst, const char *src, size_t len) {
    size_t size = json_bufsize(src, len);
    size_t n = json_strlcpy(dst, size, src, len);
    if(n >= size) {
        LOG(LOG_ALWAYS | LOG_BACKTRACE, "overflow: %zu >= %zu\n", n, size);
    }
    return dst;
}

int _list_fds(int level, const char *filename, unsigned int lineno, const char *func);

/**
 * Print the list of open file descriptors of the current process to stderr.
 */
#define list_fds(LEVEL) _list_fds(LEVEL, __FILE__, __LINE__, __func__)

/**
 * `*array` is an array with `*len` elements of size `size`. Realloc `*array`
 * and append copy `size` bytes from `elem` to the end of `*array`. `*len` is
 * increased accordingly.
 */
int array_append(void **array, size_t *len, const void *elem, size_t size);

/**
 * Append `fd` to an array of integers.
 */
static inline int append_fd(int **array, size_t *len, int fd) {
    return array_append((void **)array, len, &fd, sizeof(fd));
}

/**
 * Append a copy of `addr` to an array of `struct sockaddr_storage`.
 */
static inline int append_sockaddr(struct sockaddr_storage **array, size_t *len, const struct sockaddr_storage *addr) {
    return array_append((void **)array, len, addr, sizeof(*addr));
}

/**
 * Convert `str` to an integer. If the parsed value is less than `min` or
 * greater than `max` `EINVAL` is returned.
 * \return 0 on error and `*err` is set to `1`
 */
long strtol_limit(int *err, const char *str, long min, long max);

/**
 * Convert signal number `signum` to the signal name. See signal(7).
 */
const char *signame(int signum);

/**
 * Used by `str_bits`.
 */
struct str_bit {
    uint64_t num;
    const char *str;
};

/**
 * Convert a bitmask to its string representation, see `epoll_str`.
 */
char *str_bits(const struct str_bit *bits, char *buf, size_t size, uint64_t events);

/**
 * Convert epoll bitmask to their string constants.
 */
char *epoll_str(char *buf, size_t size, uint32_t events);

/**
 * Close file descriptor `*fd` if `*fd` is non-negative. Then `*fd` is set to
 * a negative value.
 */
int closep(int *fd);

/**
 * Calls `closep`. Errors are logged with `perror`, previous `errno` is
 * preserved. Useful with `__attribute__((cleanup(closep)))`.
 */
void closep_no_error(int *fd);

#endif
