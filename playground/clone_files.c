#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <linux/sched.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int childproc(void *ctx) {
#ifdef OPEN_BEFORE_EXEC
    // Suspend this process.
    assert(raise(SIGSTOP) == 0);
#endif
    execl(
        "/bin/sh",
        "sh",
        "-xc",
#ifndef OPEN_BEFORE_EXEC
        // Suspend shell.
        "kill -STOP $$ && "
#endif
        "sleep 1 && exec ls -ahlp /proc/self/fd",
        NULL
    );
    perror("execl");
    return 1;
}

int main(void) {
    // Use clone3 to avoid allocating a stack.
    struct clone_args cl_args = {
        .flags = CLONE_FILES,
        .stack = (uintptr_t)NULL,
        .stack_size = 0,
        .exit_signal = SIGCHLD,
    };
    long child = syscall(SYS_clone3, &cl_args, sizeof(cl_args));
    if(child == 0) {
        _exit(childproc(NULL));
    }
    assert(child >= 0);

    // Wait for the child process to suspend itself.
    int wstatus;
    assert(waitpid(child, &wstatus, WUNTRACED) == child);
    assert(WIFSTOPPED(wstatus));

    // Open new file descriptor.
    fputs("open(\"/dev/null\", O_RDONLY)...\n", stderr);
    int fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);

    // Resume child process.
    assert(kill(child, SIGCONT) == 0);

    // Wait for child process to exit.
    assert(waitpid(child, &wstatus, 0) == child);
    assert(WIFEXITED(wstatus));

    return WEXITSTATUS(wstatus);
}
