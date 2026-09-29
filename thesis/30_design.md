# Design

The proxy is designed to process TCP connections reliably while allowing individual connections to be handled concurrently and independently.
If possible, failures in the proxy should not terminate or disrupt connections.

## Design Goals

The following design goals describe the requirements of the proposed proxy and will subsequently guide its design.
The resulting implementation will be evaluated based on these design goals.

### Transformation

The proxy will evaluate, filter, or modify the connections passing through it, including

 1. rejecting the connection based on the client's address, credentials, or the connection's contents;
 2. selecting the upstream host; or
 3. completely transforming the traffic flowing either or both directions.

This is generalized as *transformations* applied to data received from either peer and can affect not only the data of the connection, but also the handling and the state of the connection.
Transformations can be arbitrarily complex and the transformation mechanisms should be extensible.

### Concurrent Connection Handling

The proxy must handle connections concurrently.
For relatively few parallel connections a new thread or process can be spawned, but for numerous connections the overhead of creating and switching between a large number of threads or processes will become too significant.
Therefore, the proxy will use asynchronous I/O to concurrently process multiple connections per thread.
Its control flow must be designed with the resulting event loop in mind.

### Live Upgrade

Upgrades of the proxy should be possible without interrupting established connections.
A new version of the executable should be able to replace a running process while preserving the connections and their associated state.
After the new executable has been started, processing should continue in the new executable, satisfying all additional requirements outlined in \fullref{connection-persistence}.
Ideally individual components, such as the transformations, should be upgradeable independently.

### Fault Isolation

Failures in one component should have as little impact as possible on the proxy as well as on established connections.
Errors in processing one connection must not affect other connections, and the failure of a process should not unnecessarily terminate connections handled by other processes.

### Connection Persistence

Established connections should persist even if the process currently processing them terminates.
This can be caused by other processes on the system or the operating system itself, e.g., the Linux kernel's out-of-memory killer.
Sufficient redundancy must be implemented in the proxy so that connections and their state can be recovered and processing can continue with minimal interruptions.

Termination can occur at arbitrary points and must be treated as an expected failure mode during design.
Therefore, any operations on connections and their data must be designed in a manner that allows them to be interrupted at any point, without corrupting underlying data structures.
Any recovery of connections must be transparent to peers and resume execution without requiring peers to reconnect.

:::{.hidden}
### OOM Killer

One initial design goal was to recover if one of the proxy's processes is killed by the Linux kernel.
In an out-of-memory situation the Linux kernel will calculate a *badness score* for each process and will terminate processes with the highest *badness score* to free resources.
However a privileged process can adjust its *badness score* rather easily through `proc_pid_oom_score_adj(5)`{.manpage} and protect it from OOM-killing completely.

While this goal can be easily achieved, other scenarios outlined in \fullref{signals} that unconditionally kill a process remain and justify the elaborate architecture.
:::

### Design Constraints

Achieving the previously stated goals will introduce additional overhead.
Redundancy and fault isolation require additional memory or processing, and the architecture is more complex than that of a conventional proxy.
This overhead should be kept to a reasonable minimum without compromising any of the previously stated goals.

## Overview

Proxy connections are established, processed, and terminated similarly to a conventional proxy.
While processing the connection the proxy can receive or send data from or to its peer repeatedly and in any order.
\Cref{fig:connection-lifecycle-simple} greatly simplifies the data flow in that stage.

![Connection lifecycle in a conventional proxy](tikz/connection-lifecycle-simple.tex){#fig:connection-lifecycle-simple}

To achieve *fault isolation*, *connection persistence*, and, more generally, redundancy, the proxy is split into *listener* and *worker* executables and processes.
Connection state is stored in shared memory and connections are held by the *listener* and their respective *worker* for redundancy.
Additionally, the *listener* is running as two redundant processes.
This ensures neither the shared memory nor file descriptors of connections are lost when a single process terminates.
The relationship between the individual processes is visualized in \cref{fig:process-tree}.

![Process architecture of the proxy](tikz/process-tree.tex){#fig:process-tree}

But sharing and handing over the connection between multiple processes greatly complicates the connection handling as shown in \cref{fig:connection-lifecycle-simple} and requires IPC to synchronize the processes.

## Checkpointing

The option to write checkpoints of the process to disk and later resume execution from said checkpoints was discarded early on.
While a checkpoint of the state and, with *libsoccr* or `TCP_REPAIR`{.c}, the connections themselves can be created, periodic checkpoints will always be potentially outdated, and reactive checkpoints cannot be created reliably, as explained in \fullref{signals}.

While any outgoing data can potentially be sent again a second time and the receiving peer will simply discard it due to TCP's properties, any data received by the operating system since the last checkpoint will be lost and will not be resent by the sending peer.
A logger, as described in \fullref{tcp-based-replication}, would need to be implemented to replay incoming TCP packets to restore the new process' state.

Additionally the potential overhead of frequently creating checkpoints was deemed too great, but checkpointing may be used in the future to extend the proxy to also persist connections across reboots of the systems.

## Multi-Process Architecture

Besides potential parallelization, a multi-process architecture isolates individual components from each other.
If merely separated into threads, a misbehaving thread can intentionally terminate the whole process, e.g., with `exit_group(2)`{.manpage}, or can accidentally cause a fault that terminates the whole process, be it through bugs or other unintended and erroneous behavior.
If separated into processes, a misbehaving process will be isolated from other processes.

A common pattern in existing web servers, such as *nginx* [@nginx_admin_manual] or *HAProxy* [@haproxy_manual], is that a single *master* process controls a number of *worker* processes and each *worker* process accepts and processes incoming connections on their own.
This isolates *worker* processes from one another, and crashes will not propagate between them.

This pattern is modified slightly in the proposed proxy.
A single *listener* process accepts incoming connections and subsequently hands them over to a *worker* process; the *worker* processes will not accept or create connections themselves.
Although this leads to the more complex connection setup seen in \cref{fig:connection-lifecycle}, connection file descriptors will be handled more reliably, as explained in \fullref{shared-file-descriptor-table}.

Besides separate processes, the *listener* and *worker* are implemented in separate executables as well, so they can be linked differently, further enabling fault isolation.
The *listener* can maintain minimal dependencies, while the *worker* may be linked against a multitude of external libraries that aid in processing connections.
The executables can also be updated independently and allow to individually migrate parts of the proxy to newer versions.

## Listener Process

The *listener* will accept incoming connections and copy them to a *worker* process.
It is intentionally kept minimal, without external dependencies besides the C standard library, and its operations are limited to

 1. starting and monitoring the *worker* process,
 1. accepting incoming connections, and
 1. creating new outgoing connections.

It is implemented in a protocol agnostic manner, and the connections are instead handed over to the *worker* for processing, allowing the *listener* to be reused for any TCP-based connection protocol.

Besides its active role, the *listener* will act as a backup for *worker* processes by keeping redundant copies of connection file descriptors and a reference of the shared memory containing connection state.
This redundancy is extended further in \fullref{shared-file-descriptor-table}.

Sharing the connection between processes results in the more complex connection lifecycle previously seen in \cref{fig:connection-lifecycle} and is further detailed in \fullref{connection-lifecycle}.

The number of open file descriptors a process can possess may be limited by the *resource limit* `RLIMIT_NOFILE`.
Since a copy of every connection file descriptor is kept in the *listener* process pair, the total number of connections will be limited.
This limit, however, can be raised to allow more than 2 billion open file descriptors [@zim2024maxRlimitNofile], allowing more than 1 billion active connections.

### Connection Lifecycle

For a conventional proxy, the lifecycle of each connection can be split in the following three stages:

Establishment
: The proxy accepts the incoming connection from a client and creates an outgoing connection to the upstream host.

Processing
: Data is repeatedly received from both peers, transformed, and sent to the opposite peer.

Termination
: Once communication has completed, the proxy or its peers will close the connection.

Sharing connections between the *listener* and *worker* process makes it necessary to synchronize the processes and copy connections between them and results in the more complicated connection handling of \cref{fig:connection-lifecycle}.

![Connection lifecycle](tikz/connection-lifecycle.tex){#fig:connection-lifecycle}

The *listener* will establish connections, because it is designed with additional redundancy, as detailed in \fullref{shared-file-descriptor-table}, and can in the future be reused for arbitrary TCP-based protocols due to its protocol-agnostic design.
Processing is then handled by the *worker*, and to terminate the connection, both processes must each close their redundant file descriptors.

The resulting state machine of each connection is shared between the *listener* and *worker* processes, where copying of file descriptors and synchronization of processes is handled through IPC messages.
\Cref{fig:ipc-connect-simple,fig:ipc-close-simple} shows the states traversed by a connection during connection establishment and termination respectively, where edges crossing the dashed border between processes represent IPC messages.

![Distributed state machine during connection establishment](tikz/ipc-connect-simple.tex){#fig:ipc-connect-simple}

![Distributed state machine during connection termination](tikz/ipc-close-simple.tex){#fig:ipc-close-simple}

### Shared File Descriptor Table

To avoid losing a connection if a process were to terminate, its file descriptors must be shared between multiple processes.
Then the connection is only closed once all file descriptors in all processes are closed or all processes terminate.
Therefore, for any connection, file descriptors should exist in more than one process.
During connection processing, the *listener* already acts as a backup for the *worker*
However, even while establishing the connection its file descriptors should be shared between multiple processes.

Newly created file descriptors could be copied to the other process immediately; this, however, still leaves a time window between creation of the file descriptor and subsequent copying, where it only exists in a single process and will be lost if this process terminates.
Arguably, a newly received or created connection will have accumulated very little state and can be retried more cheaply, but ideally even this can be avoided.

With the `clone(2)`{.manpage} syscall, Linux offers an interface to spawn new child processes or threads, similar to `fork(2)`{.manpage}.
However, `clone(2)`{.manpage} offers fine-grained control over which resources will be shared by the two processes.
E.g., `CLONE_VM` results in the processes sharing their memory and is one of the flags used to create threads instead of processes.
With the `CLONE_FILES` flag the new process will share its parent's file descriptor table instead of receiving a copy.
This means any operations that create, close, or change [@F_SETFD] a file descriptor in one of the processes operate on the shared file descriptor table and are immediately visible in both processes.
This avoids the time window introduced by manually copying the file descriptors altogether.

![Processes with a shared file descriptor table](tikz/clone-files.tex){#clone-files}

The file descriptor table is unshared if one of the processes calls `execve(2)`{.manpage}, i.e., processes with different executables cannot use a shared file descriptor table.
Therefore, the *listener* and *worker* processes will not be able to share their file descriptor table, but the *listener* process can be split into a process pair of a *passive listener* and an *active listener*.

The *passive listener* calls `clone(2)`{.manpage} with `CLONE_FILES` and creates a new *active listener*.
The *active listener* will now perform the tasks outlined above while the *passive listener* acts as a backup.
Any file descriptor created by the *active listener* will instantaneously exist in the two processes.
If one of the two *listener* processes terminates, the other will detect it and recreate the process pair to ensure file descriptors are held by more than one process.
The *worker* will not create outgoing connections itself, but instead the *active listener* will create them on behalf of the *worker* to ensure they always exist in multiple processes.
Both *listeners* will keep a reference to the shared memory as well as connections and therefore act as a backup for *worker* processes.

### Inter-process communication {#design:ipc}

As shown in \cref{fig:ipc-connect-simple}, the *active listener* and *worker* must communicate during connection processing, be it to synchronize, command the other process, or to copy file descriptors.

IPC will be performed through a stateless protocol over UNIX sockets.
Each message contains a single command or result thereof and references a connection in shared memory.
Peers sequentially process incoming IPC messages, operate on the referenced connection, and if applicable reply with the result.
File descriptors are copied between processes alongside IPC messages through UNIX sockets' *ancillary messages* [@unix7].

## Worker Process

The *worker* will process the connections received from the *listener*, transforming the data flowing through it.
Since transformations of connections can become arbitrarily complex, the *worker* process has a large potential for bugs and is therefore separated into its own process.
It could theoretically be run with reduced privileges to further isolate failures or potentially compromised *workers* from other parts of the proxy.

The proxy is implemented so that in the future it can be easily extended to spawn multiple *workers* simultaneously, but in its current implementation it will only run with a single *worker* process, intentionally forfeiting parallelisation.
Nevertheless a single *worker* process will process multiple connections concurrently through the use of asynchronous I/O.

### Shared Memory {#design:shared-memory}

As previously mentioned all state associated with a connection is placed in shared memory and both *listener* processes hold a reference to it.
If the *worker* terminates, it is still accessible in the *listener* processes and the *worker's* state can be restored.

However, as explained in \fullref{signals}, a process could potentially be terminated at any time.
If it were to terminate midway during an operation on shared memory it could leave the shared memory in a corrupted or inconsistent state.
All operations on shared memory must therefore be performed in a manner that prevents it from being corrupted or otherwise reaching an invalid state under any circumstances.
This includes the receiving, sending, and transforming of data as well as any transitions of the state machine.

The so-called *transactional memory* [@shavit1995software] will be implemented in software and is closely related to database transactions.
The *ACID* properties can be ensured in the following ways:

Atomicity
: Memory transactions are committed through atomic CPU instructions [@iso9899-2011-n1570].

Consistency
: All mutually dependent state is written by a single atomic instruction.

Isolation
: Shared memory is generally owned by *worker* processes and only accessed by the *active listener* during recovery or if instructed through IPC messages. In most cases only a single process will access an object and isolation from concurrent access is not necessary. With multiple concurrent *worker* processes, each connection will be owned by a single *worker*.

Durability
: Conventionally durability requires data to be written to disk, i.e., non-volatile memory, but in the case of this proxy durability is provided by the operating system and the semantics of shared memory itself. As long as a single process with access to the shared memory remains, the memory will persist. Durability in the case of a complete system failure, e.g., by storing it on disk, is not considered.

*Durability* is provided by the operating system and *isolation* is of lesser importance, since concurrent access to the shared memory is relatively rare, but interrupted partial transactions, due to a crash, are the more likely scenario.
This makes *atomicity* and *consistency* the more important properties during implementation.

Currently the shared memory is one continuous area containing all connections and is completely accessible by every *worker*.
A misbehaving *worker* can potentially corrupt connections not under its purview.
In the future the shared memory could be split into multiple segments, with each connection in a separate segment and *workers* only gain access to the segments of its connections.
This, however, requires an additional file descriptor per connection backing the shared memory segment.

### Transactional I/O

All I/O buffers must be placed in the shared memory to avoid the scenario outlined in \cref{lost_read} and special care has to be taken so that metadata is preserved as well.
Especially return values of syscalls can be difficult to store safely, since, depending on calling conventions, they cannot be written to shared memory directly.
In the example \cref{lost-return}, the data is safely placed in shared memory, but the number of bytes copied is lost, violating the *consistency* property.

:::{.figure #lost-return}
```c
struct shared_memory {
    char buf[BUFSIZE];
    size_t len;
};
struct shared_memory *shared;
ssize_t ax = read(fd, shared->buf,
                  sizeof(shared->buf));
/* CRASH: The number of bytes copied
 *        to the buffer is lost. */
shared->len = ax;
```

:::{.label}
Lost return value
:::
:::

The syscalls `recvmmsg(2)`{.manpage} and `sendmmsg(2)`{.manpage} provide an interface that can write the number of bytes received or sent directly into memory.
By placing the `struct mmsghdr`{.c} in shared memory, see \cref{sendmmsg_recvmmsg}, transactional I/O operations are achieved.

:::{.figure #sendmmsg_recvmmsg}
```{.c}
struct mmsghdr {
    struct msghdr msg_hdr;
    unsigned int  msg_len;
};

int sendmmsg(int              sockfd,
             struct mmsghdr  *msgvec,
             unsigned int     n,
             int              flags);

int recvmmsg(int              sockfd,
             struct mmsghdr  *msgvec,
             unsigned int     n,
             int              flags,
             struct timespec *timeout);
```

:::{.label}
Definitions of `sendmmsg(2)`{.manpage} and `recvmmsg(2)`{.manpage}
:::
:::

Each syscall is wrapped into a state machine, e.g. \cref{fig:atomic-recv-simple}, of individually idempotent steps and traverses between states atomically.
Now when the state machine is interrupted it can resume execution with the step that was interrupted.
Because individual steps are idempotent any step may be repeated whenever it was previously interrupted.

![State machine of the transaction wrapping `recvmmsg(2)`{.manpage}](tikz/atomic-recv-simple.tex){#fig:atomic-recv-simple}

Even though syscalls are not a single atomic CPU instruction and can be interrupted by signals, the I/O operation itself is nevertheless atomic in respect to signals, including `SIGKILL`.
This allows the state machine to encode some of its state with a sentinel value in `unsigned int msg_len`{.c} which will be atomically updated by the syscall itself.
Implementation of the state machine is discussed in detail in \fullref{impl:transactional-io}.

### Transformation

![Inputs and outputs of `transform()`](tikz/transform.tex){#fig:transform}

Transformation of the connection must be implemented as a transaction as well.
It consumes data from the connection’s two receive buffers and appends data to the two send buffers, as visualized in \cref{fig:transform}.
When the transaction is committed, all buffers must then be updated together to ensure *consistency* of the connection data.

Transformations can be arbitrarily complex and can store arbitrary context alongside the connection data, e.g., to implement their own state machine.
The context must be updated as part of the transaction as well to ensure that, if the transformation were to be interrupted, the state of the transaction does not advance even though the buffers did not change.

## Recovery

The most probable scenario is a *worker* process crashing for any reason.
In that case, the *active* listener will detect it and restart a new *worker* process.
Since all connections also exist in the *listener's* processes and their state completely resides in shared memory accessible by the *listener* processes, the new *worker* will simply receive established connections from the *active listener* and resume execution directly from the saved state.

To pass existing connections to the new *worker* process an IPC message flow separate from \cref{fig:ipc-connect-simple} is used and is closely described \fullref{impl:recovery}.
Since all operations on the connection data are implemented as transactions, the *worker* will automatically resume operations where they were interrupted.
I/O transactions will resume at the points indicated by the dashed edges in \cref{fig:atomic-recv-simple} and transformations will be restarted.

If the *active listener* terminates, the *passive listener* will detect its child process terminating and immediately start a new *active listener*.
While the *active listener* is not running, no new incoming connections will be accepted.
However, the operating system will still add incoming connection to the listening socket's queue of pending connections, since the listening socket persists in the *passive listener*.
For the clients this may result in a longer time to connect, but will otherwise be transparent.
In its current implementation, the *worker* process will be restarted as well to return to a well-known state.
This, however, is entirely optional, as the new *active listener* could monitor pre-existing *worker* processes as well as newly created ones.

Should the *passive listener* terminate, it is monitored by the *active listener*, and it will become the new *passive listener*.
The new *passive listener* will then start an *active listener*, which will follow the same steps as outlined in the previous paragraph.
As long as either one of the *listener* processes keeps running, the proxy will recover and continue its execution without any interruption to its peers.

Recovery of each connection depends on the state of the connection, illustrated in \cref{fig:connection-lifecycle-errors}.

![Potential interruptions during a connection's lifecycle](tikz/connection-lifecycle-errors.tex){#fig:connection-lifecycle-errors}

Connections **interrupted while terminating** can be terminated in the remaining process before they are duplicated to the new process.

Once the **connection is fully established**, I/O and transformations are performed in transactions.
When the *worker* process crashes, it will receive the connection from the *active listener* and resume or restart the interrupted transaction.

If **interrupted while connecting**, recovery is the most involved.
Depending on the process affected and the point of the interruption, the outgoing connection may already have been created and not yet duplicated or the *worker* may need to resend the upstream host's address to the *active listener*.
This is discussed further in \fullref{impl:recovery}.

If the *active listener* is **interrupted before or during accept**, the connection remains in socket’s queue of incoming connections.
If terminated right after `accept(2)`{.manpage}, before the connection is duplicated to the *worker*, the file descriptor will persist in the file descriptor table shared with the *passive listener*.
In the unlikely case that the process is terminated after `accept(2)`{.manpage}, but before the number of the file descriptor is stored in shared memory, the file descriptor will have been created in the shared file descriptor table, but *forgotten* and must be recovered by the new *active listener*.
This is discussed in detail in \fullref{forgotten-file-descriptor}.

## Live Upgrade {#design:live-upgrade}

Planned migration can be seen as a subset of recovery from a crash.
If the proxy can recover from a crash at any point, it can recover from a controlled restart and will only need to execute the new binary somewhere during recovery.

If the *worker's* executable is replaced by a new version and the *worker* process is killed, the previously described recovery will simply start a *worker* of the new version.
If the *worker's* version stays ABI-compatible, that is, the new version will work with the previous version's shared memory layout, it will resume execution just as the old *worker* would.
Otherwise, *worker* and *listener* will have to be migrated together, to change the layout of the shared memory.

To migrate the *listener* to a new executable, it will need to execute the new executable during recovery.
If the *passive listener* terminates, the remaining *active listener* will need to re-execute the *listener's* executable instead of changing its role internally.
If the *active listener* terminates, the remaining *passive listener* can re-execute as well.
Thus, if the *listener's* executable is replaced, the new version will be executed.
As the current implementation restarts *worker* processes, a new *worker* will be executed automatically.
Otherwise the new *listener* can kill the old *workers* and start new ones, during start-up, thereby migrating both executables together.
In either case the *listener's* state, including file descriptors and shared memory will be passed to the new executable through command line arguments.

## Summary

Based upon the design outlined above, an implementation is to be developed.
Connection's file descriptors will be kept by multiple processes at all times and any essential state of the proxy resides in shared memory referenced by all processes, to ensure resources are not lost as long as a single process remains running.

Subtasks of the proxy will be separated into *worker* and *listener* processes and executables, to allow for reusable components.
The processes of the proxy will communicate and synchronize through suitable IPC.
