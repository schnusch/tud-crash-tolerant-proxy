# Background

While connection retries and fail-over can be used to mitigate crashes of an intermediary service, such as a proxy, they need to be implemented by the peers themselves.
The aim is to implement a transparent proxy and as such it must use the native protocols.
As such all recovery must take place in the proxy itself.

While solutions exist at lower levels of the system, such as checkpointing or live migration of virtual machines [@clark2005live] or containers [@laadan2010linuxcr; @mirkin2008containers], that allow to restart the proxy transparently, the aim of this work is to attempt fault tolerance entirely in the application itself.

## Software fault tolerance

At its core *software fault tolerance* is about how to design robust software, that can continue to operate, even when it encounters faults during its execution.
If an application encounters a fault, normally it can deal with it in one of the following ways:

 1. Ignore or work around the fault, if the operation is non-essential or its result can be achieved in another way.
 1. Retry the operation, possibly freeing system resources beforehand or otherwise changing the execution evironment.
 1. Abandon the operation that caused the fault but continue executing, albeit in a possibly degraded form.
 1. Terminate completely.

Some faults can be unresolvable in the scope of the application and may leave no choice but to terminate completely.
This can be due to

 1. retries taking an unreasonable amount of time,
 1. a programming error that deterministically leads to an invalid state,
 1. unimplemented or permanently unavailable system features, or
 1. faults in the underlying system itself.

In all other cases strategies need to be developed to handle or work around the fault.
Short running software processes can just terminate if they encounter an unexpected or unrecoverable error.
The process as whole can then be retried by the system or the user.
This is only feasible if the internal state of the process can be easily recovered, since it will be lost completely if the userspace process terminates.

For a long running application, such as this proxy service, the internal state must not be lost.
In addition to any global state of the application, such as configuration, each connection passing through the service has state attached to it.
This may include:

 1. data received from a remote peer,
 1. data pending send to a remote peer, and
 1. internal state of the connection's state machine.

If the proxy's processes terminate, their state must therefore be saved, so that execution can be resumed at a later time.
Saving and later resuming from the state is commonly referred to as checkpointing.

## Error Model

On POSIX-compliant operating systems [@posix], or more specifically Linux, faults of the underlying system are delivered to a user-space process in one of two ways:

 1. A system call may return an error code, or
 2. the operating system or other processes on the system can generate a signal.

### System calls

System calls (short *syscalls*) are the interface between user-space process and the operating system.
User-space processes can request the operating system to perform privileged actions for them.
These privileged actions include filesystem access, network operations, memory mappings, or hardware access in general.
If such an action fails the operating system will return an error code (see `errno(3)`{.manpage}) that indicates the underlying fault.
The errors are returned synchronously to the calling process.

Example of such errors are:

`EAGAIN, EWOULDBLOCK`
: The operation cannot be performed at this time, but may be retried at later time.

`ENOSPC`
: The underlying filesystem has not enough free space available to write the requested data.

`EHOSTUNREACH`
: The host cannot be reached over the network.

`ECONNRESET`
: The network connections was aborted.

`ENOSYS`
: The operation is not implemented by the operating system.

These errors can differ wildly in severity but the process' execution will always continue after the failed syscall where they can then be handled.

### Signals

The operating system delivers signals to the process to indicate exceptional states immediately.
They might include:

`SIGFPE`
: Floating point exception, divide by zero

`SIGSEGV`
: Segmentation violation, illegal memory access

`SIGILL`
: The CPU encountered an illegal instruction.

`SIGSYS`
: A seccomp [@seccomp] filter denied a syscall with [`SECCOMP_RET_TRAP`](https://man7.org/linux/man-pages/man2/seccomp.2.html) or [`SECCOMP_RET_KILL`](https://man7.org/linux/man-pages/man2/seccomp.2.html).

`SIGIO, SIGURG`
: I/O on a file descriptor registered through `fcntl(2)`{.manpage} is now possible.

Unlike syscalls they are not necessarily tied to a specific operation and can potentially occur at any time.
Each signal has a default action, e.g. to terminate the process.
The default action can usually be overriden through signal handlers, that are then executed when the respective signal arrives.

Processes can send signals to other processes on the system as well.
This can be used to request a service to terminate (e.g. `SIGTERM` or `SIGQUIT`) or, together with signal handlers, as a form of inter-process communication (short *IPC*) (e.g. `SIGUSR1`, `SIGUSR2`, or [`SIGHUP`](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)).
But since processes can send any signal, this also means a robust application must handle all, even unexpected, signals.

However some signals cannot be handled by signal handlers and their default action is executed by the operating system unconditionally. These signals include:

`SIGKILL`
: Terminate the process.

`SIGSTOP`
: Suspends execution of the process until it is continued with `SIGCONT`.

`SIGSYS`
: Terminate the process with a core dump, if a seccomp filter with [`SECCOMP_RET_KILL`](https://man7.org/linux/man-pages/man2/seccomp.2.html) exists.

Besides other processes the Linux kernel itself can potentially send `SIGKILL` to a process, in an effort to reclaim memory through `oom_kill` [@oom_kill].
This means a process may be terminated unexpectedly without any way to react to it.

## Checkpointing

To recover from a crash an application can be designed to save its internal state to a checkpoint and later resume execution from that checkpoint.
Checkpoints may differ in the strategy used to create them as well as their extent.

### Checkpointing Strategy

If an application is about to terminate it can create a checkpoint reactively, so it can be restored later.
This could potentially happen in a signal handler.
Using this strategy checkpoint can be created right before a process terminates, where no other changes to the state are possible and the checkpoint will contain the very latest state of the process.

But as explained above is not always possible to react to a fault and create a checkpoint
Therefore it may become necessary to create checkpoints proactively.
Proactive checkpoints can be triggered by arbitrary conditions, such a periodic timer or an incoming connection.
In any case all state changes accrued since the last checkpoint will be lost if a process terminates.

Database systems and filesystems take another approach, they first record all operations on the central data structure in a *write-ahead log* (short *WAL*) or *journal*.
In case of a crash the *WAL* or *journal* can then be used to replay failed operations.
These techniques can theoretically be used to engineer an application that keeps a continuously up-to-date checkpoint of itself.
However this requires careful consideration during the design of all parts of the application and may not always be feasible.
The necessary overhead might make it impractical or the interfaces required to operate in such a manner may simply be unavailable.

### Extent of Checkpoints

A checkpoint does not necessarily include the full internal state of the application.
It may omit parts that are non-essential or can be easily recovered.
E.g. in a graphical document editor only changes to the document itself must be saved, but state of the editor, such as window positions, may be discarded.
A proxy service need not save its configuration, if it can be loaded again.

Another reason to reduce the extend of the checkpoint is that the internal state itself grows more complicated and could become inconsistent or corrupted.
An unfortunately corrupted state could alter the program flow such that it leads to a fault by itself.
If the checkpoint is kept minimal the probabilty of inconsistencies or corruption is reduced.

Multiple checkpoints or multiple generations of checkpoints can be kept.
If the latest checkpoint leads to an error an older checkpoint can be tried, which may lead to greater degradation of the service, but continued execution.
But it can be rather difficult or may generally be impossible to determine if the checkpoint is corrupted and leads to a fault deterministically.

### Simple Proactive Checkpointing

On POSIX-compliant operating systems [@posix] a simple way to create checkpoints is through the `fork(2)`{.manpage} syscall.
Essentially, `fork(2)`{.manpage} creates a copy of the process' address space and open file descriptors.

To create a checkpoint a process calls `fork(2)`{.manpage} and the child process will continue execution.
The parent process will wait for its child process to terminate.
If the child process terminates due to an error the parent process can `fork(2)`{.manpage} a new child process with the same inital state as the previous child process.
Obsolete saved states can be discarded by killing its parent or grandparent processes.
If a child process terminates, its changes will be lost when a checkpoint is restored.

![Save process state through `fork(2)`{.manpage} [**TODO** Quelle](https://tu-dresden.de/ing/informatik/sya/se/studium/lehrveranstaltungen/summer-semester/SFT/summer-semester-2025?set_language=en)](tikz/fork-checkpoint.tex)

:::{.figure #lost_read}
```c
/* A socket with pending data "Hello World"
 * already in the kernel's receive buffer. */
int sock_fd;
char buf[8];

if(fork() == 0) {
    /* child process */
    read(sock_fd, buf, sizeof(buf));
    _exit(1); /* CRASH */
}

/* parent process */
wait(NULL);
read(sock_fd, buf, sizeof(buf));
/* `buf` will now contain "rld", the bytes
 * "Hello Wo" will have been lost. */
```

:::{.label}
Socket buffers are not checkpointed by `fork(2)`{.manpage}.
:::
:::

The operating system itself may keep some associated state, that cannot be saved as easily.
In the case of the proxy service most notably kernel buffers associated file descriptors will not be part of the checkpoints as shown in \autoref{lost_read}.
However for the illustrated case advanced operating system interfaces do exists, that allow to completely checkpoint and restore a TCP connection.

## Related Work

### CRIU

*Checkpoint/Restore In Userspace* (short *CRIU*) [@criu] is the current implementation of checkpointing of userspace processes on Linux.
It allows to dump a complete process tree to disk, which can be later used to restore the processes.
*CRIU* is used for checkpointing in *LXC*, *Podman*, *Docker*, and *Kubernetes*.

Its *libsoccr* [@libsoccr] library allows to fully checkpoint a TCP connection.
Active connections are saved by first putting the socket into *repair mode* ([`TCP_REPAIR`](https://lwn.net/Articles/495304/)), this allows the application to access the associated kernel state.
The connection's kernel state is saved and then the connection is terminated without sending a `FIN` or `RST` TCP-packet to the peer.
A new socket can later be created on the same TCP port and with the same kernel state to resume the connection.
As long as the connection is restored quickly enough, nothing will have changed from the peer's perspective and TCP's retransmission mechanism will mask the interruption.

### DMTCP

*Distributed MultiThreaded CheckPointing* (short *DMTCP*) [@ansel2009dmtcp] can be used to checkpoint and restore process trees as well.
*DMTCP* injects a thread into each process, this thread can then be used to dump the process's state without access to special kernel interfaces.

It can only checkpoint connections between processes under its control.
These processes need not run on the same host, but unlike with *libsoccr* external connections cannot be checkpointed.

### systemd File Descriptor Store

The systemd service manager provides a [*file descriptor store*](https://systemd.io/FILE_DESCRIPTOR_STORE/) [@systemd_fdstore].
This store allows services to persist open file descriptors across service restarts through [`sd_pid_notify_with_fds(3)`](https://www.freedesktop.org/software/systemd/man/latest/sd_pid_notify_with_fds.html).
This can theoretically be used to store active external connections and later resume them.
But generally no guarantees are made as to its persistence or the number of file descriptors a service can store.

This feature appears to be relatively unused.
A superficial scan of 36,032 packages from Debian Trixie revealed no packages using it.
