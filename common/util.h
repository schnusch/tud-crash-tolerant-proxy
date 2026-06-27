#ifndef COMMON_UTIL_H
#define COMMON_UTIL_H

#ifdef __cplusplus
#define _Static_assert(cond, msg) static_assert(cond, msg)
#endif

#include <arpa/inet.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define perror(s)    (fprintf(stderr, "[%d] %s:%d\t%s: ",     getpid(), __FILE__, __LINE__, __func__), perror(s))
#define LOG(fmt, ...) fprintf(stderr, "[%d] %s:%d\t%s: " fmt, getpid(), __FILE__, __LINE__, __func__, __VA_ARGS__)

/**
 * Divide two integers rounding up.
 * \param NUM numerator
 * \param DEM denominator
 */
#define DIV_ROUND_UP(NUM, DEM) (((NUM) + (DEM) - 1) / (DEM))

/**
 * Buffer size needed for the decimal representation of an int. Excluding the
 * terminating NUL byte.
 */
#define INT_DEC_BUFSIZE (1 + DIV_ROUND_UP(sizeof(int) * CHAR_BIT - 1, 3))

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
 * Receive into `buf` and a single file descriptor into `*fd` from `sock`. If
 * no file descriptor is received `-1` is returned in `*fd`.
 */
ssize_t recv_fd(int sock, void *buf, size_t size, int *fd, int flags);

/**
 * Send `buf` and a single file descriptor `fd` over `sock`.
 */
ssize_t send_fd(int sock, const void *buf, size_t size, int fd, int flags);

/**
 * A single call to `recv_fd` followed by `readall`.
 * Same semantics as `readall` apply.
 */
ssize_t recvall_fd(int sock, void *buf, size_t *size, int *fd);

/**
 * A single call to `send_fd` followed by `writeall`.
 * Same semantics as `writeall` apply.
 */
ssize_t sendall_fd(int sock, const void *buf, size_t *size, int fd);

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
 * Allocate memory and write JSON representation of `str` to it.
 */
char *json_strdup(const char *str);

/**
 * Print the list of open file descriptors of the current process to stderr.
 */
int list_fds(const char *prefix);

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
 * Close file descriptor `*fd`. `*fd` is set to `-1` on success. Errors are
 * logged with `perror`. Useful with `__attribute__((cleanup(closep)))`.
 */
void closep(int *fd);

#endif
