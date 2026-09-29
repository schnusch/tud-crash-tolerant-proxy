# Background

While connection retries and fail-over can be used to mitigate crashes of an intermediary service, such as a proxy, they need to be implemented by the peers themselves.
The aim is to implement a transparent proxy and, as such, without altering the underlying protocols.
All recovery will take place in the proxy itself, potentially employing other services to achieve it.

Checkpointing or live migration of virtual machines [@clark2005live] or containers [@laadan2010linuxcr; @mirkin2008containers] can be used to migrate or restore a state of an application.
They allow to move the proxy to a new host or restore it to a previously saved state.
In case of a crash a previous checkpoint can be restored, but all progress since the checkpoint will be lost.
E.g. the proxy could have consumed additional input or changed its internal state.
To handle this, checkpoints would have to be created frequently or triggered by exceptional events.

An alternative to checkpointing is replication between hosts [@zagorodnov2003engineering; @marwah2003tcp; @aghdaie2005transparent], where a primary host performs a service, and if it crashes network traffic is routed to a backup host instead.
In most cases, TCP's retransmission and error detection is employed to transparently switch between hosts.
Additionally incoming IP packets can be saved and replayed to the backup host to replicate any state of the primary.

Rather than relying on existing fault-tolerance mechanisms implemented at lower layers, this work explores whether fault tolerance can be provided entirely in userspace on a single host.
The proxy itself will be designed for robustness, recoverability, and migration.

## Software fault tolerance

At its core *software fault tolerance* is about how to design robust software, that can continue to operate, even when it encounters faults during its execution.
If an application encounters a fault, it can normally deal with it in one of the following ways:

 1. Ignore or work around the fault, if the operation is non-essential or its result can be achieved in another way.
 1. Retry the operation, possibly freeing system resources beforehand or otherwise changing the execution environment.
 1. Abandon the operation that caused the fault but continue executing, albeit in a possibly degraded form.
 1. Terminate completely.

Some faults can be unresolvable in the scope of the application and may leave no choice but to terminate completely.
This can be due to

 1. retries taking an unreasonable amount of time,
 1. a programming error that deterministically leads to an invalid state,
 1. unimplemented or permanently unavailable system features, or
 1. faults in the underlying system itself.

In all other cases, strategies need to be developed to handle or work around the fault.
Short running software processes can just terminate, if they encounter an unexpected or unrecoverable error.
The process as a whole can then be retried by the system or the user.
This is only feasible if the internal state of the process can be easily recovered, since it will be lost completely if the userspace process terminates.

For a long running application, such as the proxy proposed in this work, the internal state must not be lost.
In addition to any global state of the application, such as configuration, each connection passing through the service has state attached to it.
This may include:

 1. data received from a remote peer,
 1. data pending send to a remote peer, and
 1. internal state of the connection's state machine.

If the proxy's processes terminate, their state must therefore be saved so that execution can be resumed at a later time.
Saving and later resuming from the state is commonly referred to as checkpointing.

## Error Model

On POSIX-compliant operating systems [@posix], or more specifically Linux, faults of the underlying system are delivered to a userspace process in one of two ways:

 1. A system call may return an error code, or
 2. the operating system or other processes on the system can generate a signal.

### System calls

System calls (short *syscalls*) are the interface between userspace process and the operating system.
Userspace processes can request the operating system to perform privileged actions for them.
These privileged actions include filesystem access, network operations, memory mappings, or hardware access in general.
If such an action fails, the operating system will return an error code (see `errno(3)`{.manpage}) that indicates the underlying fault.
The errors are returned synchronously to the calling process.

Examples of such errors are:

`EAGAIN, EWOULDBLOCK`
: The operation cannot be performed at this time, but may be retried at a later time.

`ENOSPC`
: The underlying filesystem has not enough free space available to write the requested data.

`EHOSTUNREACH`
: The host cannot be reached over the network.

`ECONNRESET`
: The network connection was aborted.

`ENOSYS`
: The operation is not implemented by the operating system.

These errors can differ wildly in severity but the process's execution will always continue after the failed syscall, where they can then be handled.

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

Unlike syscalls, they are not necessarily tied to a specific operation and can potentially occur at any time.
Each signal has a default action, e.g. to terminate the process.
The default action can usually be overwritten through signal handlers, which are then executed whenever the respective signal arrives.

Processes can send signals to other processes on the system as well.
This can be used to request a service to terminate (e.g. `SIGTERM` or `SIGQUIT`) or, together with signal handlers, as a form of inter-process communication (short *IPC*) (e.g. `SIGUSR1`, `SIGUSR2`, or [`SIGHUP`](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)).
However since processes can send any signal, this also requires the application to handle all, even unexpected, signals, in order to be robust.

However, some signals cannot be handled by signal handlers and their default action is executed by the operating system unconditionally. These signals include:

`SIGKILL`
: Terminate the process.

`SIGSTOP`
: Suspends execution of the process until it is continued with `SIGCONT`.

`SIGSYS`
: Terminate the process with a core dump, if a seccomp filter with [`SECCOMP_RET_KILL`](https://man7.org/linux/man-pages/man2/seccomp.2.html) exists.

Besides other processes the Linux kernel itself can potentially send `SIGKILL` to a process, in an effort to reclaim memory through `oom_kill` [@oom_kill].
This means even if completely isolated from other processes, a process may still be terminated unexpectedly without any way to react to it.

## Checkpointing

To recover from a crash an application can be designed to save its internal state to a checkpoint and later resume execution from said checkpoint.
Checkpoints may differ in the strategy used to create them as well as their extent.

### Checkpointing Strategy

If an application is about to terminate, it can create a checkpoint reactively, so it can be restored later.
This could potentially happen in a signal handler.
Using this strategy, checkpoint can be created right before a process terminates, where no other changes to the state are possible and the checkpoint will contain the very latest state of the process.

But as explained in \fullref{signals} it is not always possible to react to a fault and create a checkpoint.
Therefore it may become necessary to create checkpoints proactively.
Proactive checkpoints can be triggered by arbitrary conditions, such as a periodic timer or an incoming connection.
In any case all state changes accrued since the last checkpoint will be lost if a process terminates.

Database systems and filesystems take another approach: they first record all operations on the central data structure in a *write-ahead log* (short *WAL*) or *journal*.
In case of a crash the *WAL* or *journal* can then be used to replay failed operations.
These techniques can theoretically be used to engineer an application that keeps a continuously up-to-date checkpoint of itself.
However this requires careful consideration during the design of all parts of the application and may not always be feasible.
The necessary overhead might make it impractical or the interfaces required to operate in such a manner may simply be unavailable.

### Extent of Checkpoints

A checkpoint does not necessarily include the full internal state of the application.
It may omit parts that are non-essential or can be easily recovered.
E.g., in a graphical document editor only changes to the document itself must be saved, but the state of the editor, such as window positions, may be discarded.
A proxy service need not save its configuration, if it can be loaded again.

Another reason to reduce the extent of the checkpoint is that the internal state itself grows more complicated and could become inconsistent or corrupted.
A corrupted state could alter the program flow such that it leads to a fault by itself.
If the checkpoint is kept minimal, the probability of inconsistencies or corruption is reduced.

Finally, multiple checkpoints or multiple generations of checkpoints can be kept.
If the latest checkpoint leads to an error, an older checkpoint can be tried, which may lead to greater degradation of the service, but continued execution.
However, it can be rather difficult or may generally be impossible to determine if the checkpoint is corrupted and leads to a fault deterministically.

### Simple Proactive Checkpointing

On POSIX-compliant operating systems [@posix] a simple way to create checkpoints is through the `fork(2)`{.manpage} syscall.
`fork(2)`{.manpage} essentially creates a copy of the process' address space and open file descriptors.
\Cref{fork_checkpoint} illustrates this:

 1. To create a checkpoint a process calls `fork(2)`{.manpage}.
 1. The child process will continue execution, while the parent process will wait for its child process to terminate.
 1. If the child process terminates due to an error, the parent process can `fork(2)`{.manpage} a new child process with the same initial state as the previous child process.
 1. By killing parent or grandparent processes, resources can be freed and the checkpoint stored in them is discarded.

But if a child process terminates, its changes will be lost when a checkpoint is restored.

![Checkpointing through `fork(2)`{.manpage}](tikz/fork-checkpoint.tex){#fork_checkpoint}

The operating system itself may keep some associated state that cannot be saved as easily.
In the case of the proxy service, most notably kernel buffers will not be part of the checkpoints as shown in \cref{lost_read}.

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

However, for the illustrated case solutions nevertheless exist that checkpoint the associated kernel state, either through dedicated workarounds or advanced operating system interfaces.
They will be discussed in the following sections.

### DMTCP

*Distributed MultiThreaded CheckPointing* (short *DMTCP*) [@ansel2009dmtcp] can be used to checkpoint and restore process trees of processes started through *DMTCP*.
On start-up, *DMTCP* injects a thread into each process, this thread is later used to access the process's memory and checkpoint its state.

*DMTCP* does not rely on any dedicated operating system interfaces, but can nevertheless checkpoint connections between processes under its control.
When creating a checkpoint, processes are paused and execution is passed to the injected threads.
The threads will now perform `read(2)`{.manpage} on the connection until its kernel buffers are completely drained.
Since *DMTCP* controls both ends of a connection, the connection can later be recreated by *DMTCP*, the drained data is resent, and only then is execution passed to the checkpointed process.
The checkpointed processes need not run on the same host, but external connections cannot be checkpointed.

### CRIU

*Checkpoint/Restore In Userspace* (short *CRIU*) [@criu] is the current implementation of checkpointing of userspace processes on Linux.
This allows to dump a complete process tree to disk, which can be later used to restore the processes.
*CRIU* is used for checkpointing in *LXC*, *Podman*, *Docker*, and *Kubernetes*.

When *CRIU* creates a checkpoint of a process, the *libcompel* library injects *parasite code* into the running process.
Execution of the process is paused and handed over to the *parasite code*.
This *parasite code* can then access all resources of the process and create the checkpoint.

*CRIU's* *libsoccr* library allows to fully checkpoint a TCP connection, including connections to hosts outside of its control.
*libsoccr* uses the Linux kernel's *repair mode* of TCP sockets ([`TCP_REPAIR`](https://lwn.net/Articles/495304/)).
The active connection is first put into *repair mode*, which allows the application to access the associated kernel state.
The kernel's send and receive buffers as well as all other connection data are saved and the connection is closed.
The Linux kernel does not send a `FIN` or `RST` TCP-packet to the peer, if a socket in *repair mode* is closed.
A new socket can later be created on the same TCP port and with the same kernel state to resume the connection.
As long as the connection is restored quickly enough, nothing will have changed from the peer's perspective and TCP's retransmission mechanism will mask the interruption.
*libsoccr* or the underlying operating system interface can be used by an application to natively checkpoint its TCP connections.

## TCP-based Replication

*FT-TCP* [@zagorodnov2003engineering], *ST-TCP* [@marwah2003tcp], or *CoRAL* [@aghdaie2005transparent] expand upon checkpointing of TCP connections, by employing TCP's properties.
The application is split into two replicas: a *primary* and a *backup*.
The *primary* performs all operations and if it fails TCP packets are rerouted to the *backup*.
A separate service logs raw TCP packets going to the *primary* and, when the fail-over is performed and the *backup* takes over, this service replays incoming raw TCP packets to the *backup*.
Thus the *backup* can re-create the state of the *primary* from the saved incoming traffic.

Any outgoing traffic from the *backup* will be discarded transparently by its peers based on the TCP packet's sequence numbers.
This, however, requires the response of the application to be deterministic.
Data generated by the *backup* is concatenated to the data already sent by the *primary* and the resulting stream will potentially be corrupted if the *backup's* output differs.

As long as the accompanying TCP logging service is reliable, any state of the *primary* can be recreated and the issue of data lost since the last checkpoint is resolved.
However, some of the reliability is externalized to the TCP logging service and elevated privileges are required to access raw IP packets.

## systemd File Descriptor Store

TCP connections can also be persisted beyond a process' termination by sharing the associated file descriptor with another process.
The systemd service manager provides a [*file descriptor store*](https://systemd.io/FILE_DESCRIPTOR_STORE/) [@systemd_fdstore], where services can persist open file descriptors across service restarts through [`sd_pid_notify_with_fds(3)`](https://www.freedesktop.org/software/systemd/man/latest/sd_pid_notify_with_fds.html).

This can potentially be used to store external connections with the service manager and recover them after a crash.
However the *file descriptor store* must be explicitly enabled and the the number of file descriptors a service can store is limited.

This feature appears to be relatively unused.
A superficial scan of 36,032 packages from Debian Trixie revealed no packages, besides systemd itself, that reference it.

## Discussion

*DMTCP* cannot be used, since it only offers a subset of the functionality of *CRIU* and crucially does not allow checkpointing of external connections.
However, *libsoccr* and the systemd *file descriptor store* can be of potential use in the future.
The *file descriptor store* could save a reference to the shared memory or individual connections creating further redundancy.
*libsoccr* could potentially allow checkpoints that persist across reboots of the entire system and allow upgrades of the operating system without an interruption in service.

But in its current implementation neither of the listed technologies is used by the proxy, instead replication between separate processes will be used to achieve reliability.
