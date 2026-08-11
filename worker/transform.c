#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "transform.h"
#include "../common/util.h"

#include "../thirdparty/picohttpparser/picohttpparser.h"

#ifndef USER_AGENT
#define USER_AGENT "crash-tolerant-proxy"
#endif

enum {
    PHR_TRUNC = -2,
    PHR_ERROR = -1,
};

struct http_request {
    /** [Method](https://datatracker.ietf.org/doc/html/rfc1945#section-5.1.1) */
    const char *method;
    size_t method_len;
    /** [Request-URI](https://datatracker.ietf.org/doc/html/rfc1945#section-5.1.2) */
    const char *path;
    size_t path_len;
    /** [HTTP Minor Version](https://datatracker.ietf.org/doc/html/rfc1945#section-3.1) */
    int minor_version;
    /** [Request Header Fields](https://datatracker.ietf.org/doc/html/rfc1945#section-5.2) */
    struct phr_header *headers;
    size_t num_headers;
};

struct http_response {
    /** [HTTP Minor Version](https://datatracker.ietf.org/doc/html/rfc1945#section-3.1) */
    int minor_version;
    /** [Status Code](https://datatracker.ietf.org/doc/html/rfc1945#section-6.1.1) */
    int status;
    /** [Reason Phrase](https://datatracker.ietf.org/doc/html/rfc1945#section-6.1.1) */
    const char *msg;
    size_t msg_len;
    /** [Response Header Fields](https://datatracker.ietf.org/doc/html/rfc1945#section-6.2) */
    struct phr_header *headers;
    size_t num_headers;
};

/**
 * Generic wrapper around `phr_parse_*`.
 */
#define WRAP_PHR(CALL) \
    do { \
        size_t max_headers = r->num_headers; \
        if(max_headers == 0) { \
            goto resize; \
        } \
        while(1) { \
            int ret = CALL; \
            LOG(LOG_DEBUG, "%s = %d\n", #CALL, ret); \
            if(ret == PHR_TRUNC) { \
                return 0; \
            } else if(ret == PHR_ERROR) { \
                if(r->num_headers == max_headers) { \
    resize: \
                    max_headers += 16; \
                    struct phr_header *new_headers = realloc(r->headers, sizeof(*r->headers) * max_headers); \
                    if(!new_headers) { \
                        return -3; \
                    } \
                    r->headers = new_headers; \
                    r->num_headers = max_headers; \
                } else { \
                    return -1; \
                } \
            } else { \
                assert(ret > 0); \
                return ret; \
            } \
        } \
    } while(0)

/**
 * Wrapper around `phr_parse_request` that reallocs `headers` until all
 * headers fit.
 * \return -1 on error
 * \return 0  if the request is truncated
 * \return the number of bytes consumed
 */
static ssize_t parse_request(struct http_request *r, const char *buf, size_t len) {
    static const size_t last_len = 0;
    WRAP_PHR(
        phr_parse_request(
            buf, len,
            &r->method, &r->method_len,
            &r->path, &r->path_len,
            &r->minor_version,
            r->headers, &r->num_headers,
            last_len
        )
    );
}

/**
 * Wrapper around `phr_parse_response` that reallocs `headers` until all
 * headers fit.
 * \return -1 on error
 * \return 0  if the response is truncated
 * \return the number of bytes consumed
 */
static int parse_response(struct http_response *r, const char *buf, size_t len) {
    static const size_t last_len = 0;
    WRAP_PHR(
        phr_parse_response(
            buf, len,
            &r->minor_version,
            &r->status,
            &r->msg, &r->msg_len,
            r->headers, &r->num_headers,
            last_len
        )
    );
}

/**
 * Strip every header but the following:
 *   * `Host`
 *   * `Content-Length`
 *   * `Transfer-Encoding`
 */
static size_t strip_headers(struct phr_header *headers, size_t num_headers) {
    size_t dst = 0;
    for(size_t i = 0; i < num_headers; ++i) {
        if(
            strncasecmp(headers[i].name, "Host", headers[i].name_len) == 0
            || strncasecmp(headers[i].name, "Content-Length", headers[i].name_len) == 0
            || strncasecmp(headers[i].name, "Transfer-Encoding", headers[i].name_len) == 0
        ) {
            if(i > dst) {
                headers[dst] = headers[i];
            }
            ++dst;
        }
    }
    return dst;
}

/**
 * Remove all HTTP headers named `name` and append `${name}: ${value}`.
 */
static int replace_header(
    struct phr_header **headers,
    size_t *num_headers,
    const char *name,
    const char *value
) {
    size_t dst = 0;

    // Remove all matching headers.
    int replaced = 0;
    for(size_t i = 0; i < *num_headers; ++i) {
        if(strncasecmp((*headers)[i].name, name, (*headers)[i].name_len) != 0) {
            (*headers)[dst++] = (*headers)[i];
        } else if(!replaced) {
            // Replace the first occurence of the header.
            (*headers)[dst++] = (struct phr_header){
                .name = (*headers)[i].name,
                .name_len = (*headers)[i].name_len,
                .value = value,
                .value_len = strlen(value),
            };
            replaced = 1;
        }
    }
    if(replaced) {
        *num_headers = dst;
        return 0;
    }

    // Append new header.
    if(dst >= *num_headers) {
        struct phr_header *tmp = realloc(*headers, sizeof(**headers) * (dst + 1));
        if(!tmp) {
            return -1;
        }
        *headers = tmp;
    }
    (*headers)[dst++] = (struct phr_header){
        .name = name,
        .name_len = strlen(name),
        .value = value,
        .value_len = strlen(value),
    };
    *num_headers = dst;

    return 0;
}

/**
 * The contents of `range` wrap at the end of `buf`, then copy to `linear`.
 * \return a pointer to a linear buffer of `range->len` bytes
 */
static const char *linearize_ring_buffer(
    char linear[RING_BUFFER_SIZE],
    const char buf[RING_BUFFER_SIZE],
    struct ring_buffer_range *range
) {
    if(range->len <= RING_BUFFER_SIZE - range->start) {
        return buf + range->start;
    }
    // The ring buffer wraps around, copy it.
    const size_t  first_len = RING_BUFFER_SIZE - range->start;
    const size_t second_len = range->len - first_len;
    memcpy(linear            , buf + range->start,  first_len);
    memcpy(linear + first_len, buf               , second_len);
    return linear;
}

/**
 * Write the HTTP status line `fmt` followed by `headers` to `buf`.
 */
static ssize_t serialize_http(
    char *buf,
    size_t size,
    struct phr_header *headers,
    size_t num_headers,
    const char *fmt,
    ...
) {
    va_list va;
    va_start(va, fmt);
    int n = vsnprintf(buf, size, fmt, va);
    va_end(va);
    if(n < 0) {
        return -1;
    } else if((size_t)n > size) {
        errno = EOVERFLOW;
        return -1;
    }

    size_t left = size - n;
    char *p = buf + n;

#define APPEND(src, len) \
    do { \
        if(left < (len)) { \
            errno = EOVERFLOW; \
            return -1; \
        } \
        memcpy(p, (src), (len)); \
        p += (len); \
        left -= (len); \
    } while(0)

    for(size_t i = 0; i < num_headers; ++i) {
        APPEND(headers[i].name, headers[i].name_len);
        APPEND(": ", 2);
        APPEND(headers[i].value, headers[i].value_len);
        APPEND("\r\n", 2);
    }
    APPEND("\r\n", 2);

#undef APPEND

    return p - buf;
}

int looks_like_http_request(const char buf[RING_BUFFER_SIZE], size_t len) {
    size_t i = 0;
    size_t n = len;
    // Empty content.
    if(n == 0) {
        return 1;
    }
    // At least one capital letter...
    if(buf[i] < 'A' || 'Z' < buf[i]) {
        return 0;
    }
    // ...followed by more...
    while(--n > 0) {
        ++i;
        if(buf[i] < 'A' || 'Z' < buf[i]) {
            // ...and a space characters.
            return buf[i] == ' ';
        }
    }
    // Unterminated capital letters.
    return 1;
}

int looks_like_http_response(const char buf[RING_BUFFER_SIZE], size_t len) {
    size_t i = 0;
    size_t n = len;
    // Starts with "HTTP/1."...
    for(const char *p = "HTTP/1."; *p && n > 0; ++p, ++i, --n) {
        if(*p != buf[i]) {
            return 0;
        }
    }
    // ...followed by digits...
    while(n > 0) {
        if(buf[i] < '0' || '9' < buf[i]) {
            // ...and a space character.
            return buf[i] == ' ';
        }
        ++i, --n;
    }
    // Unterminated decimal digits.
    return 1;
}

static int eval_parse_result(
    ssize_t consumed,
    const char linear[RING_BUFFER_SIZE],
    const struct transformation_direction *direction,
    int (*looks_like)(const char *, size_t),
    const char *msg,
    size_t slot
) {
    if(consumed == 0) {
        if(direction->in_range->len == RING_BUFFER_SIZE) {
            LOG(LOG_ERROR, "slot=%zu HTTP %s headers do not fit in the receive buffer\n", slot, msg);
            return -1;
        } else if(direction->eof) {
            LOG(LOG_INFO, "slot=%zu truncated HTTP %s\n", slot, msg);
            return -1;
        } else if(!looks_like(linear, direction->in_range->len)) {
            LOG(LOG_INFO, "slot=%zu does not look like a HTTP %s\n", slot, msg);
            return -1;
        } else {
            LOG(LOG_DEBUG, "slot=%zu incomplete HTTP %s\n", slot, msg);
            return 1;
        }
    } else if(consumed < 0) {
        LOG(LOG_INFO, "slot=%zu cannot parse HTTP %s\n", slot, msg);
        return -1;
    } else {
        return 0;
    }
}

int transform(
    int slot,
    transformation_context_t *ctx,
    struct transformation_direction *down,
    struct transformation_direction *up
) {
#ifdef PERFORMANCE_BASELINE
    // Skip copying, always use the inactive copy (= 0).
    ctx->active = 1;
#else
    // Copy the active to inactive and use only inactive from now on.
    ctx->copies[!ctx->active] = ctx->copies[!!ctx->active];
#endif

    int state = ctx->copies[!ctx->active].state;
    char buf[RING_BUFFER_SIZE];

    if(!(state & HTTP_GOT_REQUEST)) {
        // Parse request.
        const char *linear = linearize_ring_buffer(buf, up->in_buf, up->in_range);
        struct http_request req = { 0 };
        ssize_t consumed = parse_request(&req, linear, up->in_range->len);
        switch(eval_parse_result(consumed, linear, up, looks_like_http_request, "request", slot)) {
        case 0:
            // Modify headers.
            req.num_headers = strip_headers(req.headers, req.num_headers);
            if(
                replace_header(&req.headers, &req.num_headers, "Connection", "close") < 0
                || replace_header(&req.headers, &req.num_headers, "User-Agent", USER_AGENT) < 0
                || replace_header(&req.headers, &req.num_headers, "DNT", "1") < 0
                || replace_header(&req.headers, &req.num_headers, "Sec-GPC", "1") < 0
            ) {
                perror("replace_header");
            } else {
                // Serialize request.
                assert(up->out_range.len == 0);
                ssize_t produced = serialize_http(
                    up->out_buf,
                    RING_BUFFER_SIZE,
                    req.headers,
                    req.num_headers,
                    "%.*s %.*s HTTP/1.1\r\n",
                    (int)req.method_len, req.method,
                    (int)req.path_len,   req.path
                );
                if(produced < 0) {
                    LOG(LOG_ERROR, "slot=%zu HTTP request does not fit in the send buffer\n", slot);
                } else {
                    LOG(LOG_DEBUG, "slot=%zu received HTTP request:     %s\n", slot, json_strndupa(linear, consumed));
                    LOG(LOG_DEBUG, "slot=%zu transformed HTTP request:  %s\n", slot, json_strndupa(up->out_buf, produced));
                    // Advance buffers.
                    up->in_range->start = (up->in_range->start + consumed) % RING_BUFFER_SIZE;
                    up->in_range->len -= consumed;
                    *up->out_range = (struct ring_buffer_range){
                        .start = 0,
                        .len = produced,
                    };
                }
            }
            __attribute__((fallthrough));
        case -1:
            LOG(LOG_INFO, "slot=%zu forwarding upstream traffic verbatim\n", slot);
            state |= HTTP_GOT_REQUEST;
            __attribute__((fallthrough));
        default:
            break;
        }
        free(req.headers);
    }

    if(state & HTTP_GOT_REQUEST) {
        // Forward any upstream traffic.
        ring_buffer_move(
            up->out_buf, up->out_range,
            up->in_buf,  up->in_range
        );
        if(up->eof) {
            up->shutdown = 1;
        }
    }

    if(!(state & HTTP_GOT_RESPONSE)) {
        // Parse response.
        const char *linear = linearize_ring_buffer(buf, down->in_buf, down->in_range);
        struct http_response resp = { 0 };
        ssize_t consumed = parse_response(&resp, down->in_buf + down->in_range->start, down->in_range->len);
        switch(eval_parse_result(consumed, linear, down, looks_like_http_response, "response", slot)) {
        case 0:
            // Modify headers.
            if(
                replace_header(&resp.headers, &resp.num_headers, "Connection", "close") < 0
                || replace_header(&resp.headers, &resp.num_headers, "Server", USER_AGENT) < 0
                || replace_header(&resp.headers, &resp.num_headers, "X-Clacks-Overhead", "GNU Terry Pratchett") < 0
            ) {
                perror("replace_header");
            } else {
                // Serialize response.
                assert(down->out_range.len == 0);
                ssize_t produced = serialize_http(
                    down->out_buf,
                    RING_BUFFER_SIZE,
                    resp.headers,
                    resp.num_headers,
                    "HTTP/1.1 %d %.*s\r\n",
                    resp.status,
                    (int)resp.msg_len, resp.msg
                );
                if(produced < 0) {
                    LOG(LOG_ERROR, "slot=%zu HTTP response does not fit in the send buffer\n", slot);
                } else {
                    LOG(LOG_DEBUG, "slot=%zu received HTTP response:    %s\n", slot, json_strndupa(linear, consumed));
                    LOG(LOG_DEBUG, "slot=%zu transformed HTTP response: %s\n", slot, json_strndupa(down->out_buf, produced));
                    // Advance buffers.
                    down->in_range->start = (down->in_range->start + consumed) % RING_BUFFER_SIZE;
                    down->in_range->len -= consumed;
                    *down->out_range = (struct ring_buffer_range){
                        .start = 0,
                        .len = produced,
                    };
                }
            }
            __attribute__((fallthrough));
        case -1:
            LOG(LOG_INFO, "slot=%zu forwarding downstream traffic verbatim\n", slot);
            state |= HTTP_GOT_RESPONSE;
            __attribute__((fallthrough));
        default:
            break;
        }
        free(resp.headers);
    }

    if(state & HTTP_GOT_RESPONSE) {
        // Forward any downstream traffic.
        ring_buffer_move(
            down->out_buf, down->out_range,
            down->in_buf,  down->in_range
        );
        if(down->eof) {
            down->shutdown = 1;
        }
    }

    ctx->copies[!ctx->active].state = state;

    return 0;
}
