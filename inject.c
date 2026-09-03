#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libcrash/libcrash.h"

static void usage(const char *prog) {
    fprintf(
        stderr,
        "Usage: %s <pid> <crash...>\n"
        "       %s -a <pid>\n",
        prog,
        prog
    );
}

int main(int argc, char **argv) {
    static const char optstring[] = "a";
    int value = 0;

    for(int opt; (opt = getopt(argc, argv, optstring)) != -1;) {
        switch (opt) {
        case 'a':
            if(value != 0) {
                usage(argv[0]);
                return 2;
            }
            value = -1;
            break;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    // Parse <pid>.
    if(optind >= argc) {
        usage(argv[0]);
        return 2;
    }
    errno = 0;
    char *end;
    long pid = strtol(argv[optind], &end, 10);
    if(*end != '\0') {
        fprintf(stderr, "invalid PID: %s\n", argv[optind]);
        return 2;
    } else if(errno != 0) {
        perror("strtol");
        return 2;
    }
    ++optind;

    if(value == 0) {
        // Parse <crash...>.
        if(optind >= argc) {
            fprintf(stderr, "%s: missing crash\n", argv[0]);
            usage(argv[0]);
            return 2;
        }
        for(int i = optind; i < argc; ++i) {
            static const struct {
                const char *str;
                int val;
            } errors[] = {
#define CRASH(x) {#x, CRASH_##x}
                CRASH(ATOMIC_RECV_PREPARE),
                CRASH(ACCEPT_POST),
                CRASH(CONNECT_POST),
                CRASH(ATOMIC_RECV_PREPARE),
                CRASH(ATOMIC_RECV_RECVMMSG_PRE),
                CRASH(ATOMIC_RECV_RECVMMSG_POST),
                CRASH(ATOMIC_RING_BUFFER_LTRIM),
                CRASH(ATOMIC_RING_BUFFER_APPEND),
                CRASH(ATOMIC_SEND_PREPARE),
                CRASH(ATOMIC_SEND_SENDMMSG_PRE),
                CRASH(ATOMIC_SEND_SENDMMSG_POST),
#undef CRASH
                {NULL, 0}
            };
            int new_value = -1;
            for(size_t j = 0; errors[j].str; ++j) {
                if(strcasecmp(argv[i], errors[j].str) == 0) {
                    new_value = errors[j].val;
                    break;
                }
            }
            if(new_value == -1) {
                fprintf(stderr, "%s: unknown error %s\n", argv[0], argv[i]);
                usage(argv[0]);
                return 2;
            }
            value |= new_value;
        }
    } else if(optind != argc) {
        usage(argv[0]);
        return 2;
    }

    union sigval sv;
    sv.sival_int = value;

    if(sigqueue(pid, LIBCRASH_SIGNAL, sv) == -1) {
        perror("sigqueue");
        return 1;
    }

    return 0;
}
