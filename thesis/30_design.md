# Design

The proxy is designed to process TCP connections reliably while allowing individual connections to be handled concurrently and independently.
If possible, failures in the proxy should not terminate or disrupt connections.
The goals stated \cref{design-goals} will directly influence the design of the proxy.

## Design Goals

### Transformation

The proxy will evaluate, filter, or modify the connections passing through it, including

 1. rejecting the connection based on the client's address, credentials, or the connection's contents;
 2. selecting the upstream host; or
 3. completely transforming the traffic flowing either or both directions.

This is generalized as *transformations* applied to data received from either peer and can affect not only the data of the connection, but also the handling and the state of the connection.
Transformations can be arbitrarily complex and the transformation mechanisms should be extensible.

### Concurrent Connection Handling

The proxy must handle connections concurrently.
For relatively few parallel connections a new thread or process can be spawned, but for numerous connections the overhead of creating and switching between a large number of threads or processes will become too great.
Therefore, the proxy will use asynchronous I/O to concurrently process multiple connections per thread.
Its control flow must be designed with the resulting event loop in mind.

### Live Upgrade

Upgrades of the proxy should be possible without interrupting established connections.
A new version of the executable should be able to replace a running process while preserving the connections and their associated state.
After the new executable has been started, processing should continue in the new executable, satisfying all additional requirements outlined in \cref{connection-persistence}.
Ideally individual components, such as the transformations, should be upgradeable independently.

### Fault Isolation

Failures in one component should have as little impact as possible on the proxy as well as on established connections.
Errors in processing one connection must not affect other connections, and the failure of a process should not unnecessarily terminate connections handled by other processes.

### Connection Persistence

Established connections should persist even if the process currently processing them terminates.
This can be caused by other processes on the system or the operating system itself, e.g., the Linux kernel's out-of-memory killer.
Sufficient redundancy must be implemented in the proxy so that connections and their state can be recovered and processing can continue with minimal interruptions.

Termination can occur at arbitrary points and must be treated as an expected failure mode during design.
Therefore, any operations on connections and their data must be designed in a manner that allows them to be interrupted at any point without corrupting underlying data structures.
Any recovery of connections must be transparent to peers and resume execution without requiring peers to reconnect.

### Design Constraints

Achieving the previously stated goals will introduce additional overhead.
Redundancy and fault isolation require additional memory or processing, and the architecture is more complex than that of a conventional proxy.
This overhead should be kept to a reasonable minimum without compromising any of the previously stated goals.

## Overview

The proxy is split into *listener* and *worker* executables and processes.
Connection state is stored in shared memory and connections are held by the *listener* and their respective *worker* for redundancy.
Additionally, the *listener* is running as two redundant processes.
This ensures neither the shared memory nor file descriptors of connections are lost when a single process terminates.
The relationship between the individual processes is visualized in \cref{process-tree}.

![Process architecture of the proxy](tikz/process-tree.tex){#process-tree}

The *listener* accepts and creates connections while the *worker* handles the transformation.
Sharing and handing over the connection between multiple processes requires IPC to synchronize the processes and results in the connection handling seen in \cref{connection-lifecycle}.

![Connection lifecycle](tikz/connection-lifecycle.tex){#fig:connection-lifecycle}

## Multi-Process Architecture

Besides potential parallelization, a multi-process architecture isolates individual components from each other.
If merely separated into threads, a misbehaving thread can intentionally terminate the whole process, e.g., with `exit_group(2)`{.manpage}, or can accidentally cause a fault that terminates the whole process, be it through bugs or other unintended and erroneous behaviour.
But if separated into processes, a misbehaving process will be isolated from other processes.

A common pattern in existing web servers such as *nginx* [@nginx_admin_manual] or *HAProxy* [@haproxy_manual] is that a single *master* process controls a handful of *worker* processes and each *worker* process accepts and processes incoming connections on their own.
This isolates *worker* processes from one another, and crashes will not propagate between them.

This pattern is modified slightly in this proxy.
A single *listener* process accepts incoming connections and subsequently hands them over to a *worker* process; the *worker* processes will not accept or create connections themselves.
Although this leads to the more complex connection setup seen in \autoref{connection-lifecycle}, file descriptors are handled more reliably, as explained in \cref{shared-file-descriptor-table}.

Besides separate processes the *listener* and *worker* are implemented in separate executables as well, so they can be linked differently.
The *listener* can maintain minimal dependencies, while the *worker* may be linked against a multitude of external libraries that aid in processing connections.
The executables can also be updated independently and allow to individually migrate parts of the proxy to newer versions.

### Listener Process

The *listener* will accept incoming connections and copy them to a *worker* process.
It is intentionally kept minimal, without external dependencies besides the C standard library, and its operations are limited to

 1. starting and monitoring the *worker* process,
 1. accepting incoming connections, and
 1. creating new outgoing connections.

It is implemented in a protocol agnostic manner, and the connections are instead handed over to the *worker* for processing, allowing the *listener* to be reused for any TCP-based connection protocol.

Besides its active role, the *listener* will act as a backup for *worker* processes by keeping redundant copies of connection file descriptors and a reference of the shared memory used by the *worker* to store connection state.
This redundancy is extended further in \cref{shared-file-descriptor-table}.

Sharing the connection between processes in the more complex connection lifecycle previously seen in \cref{fig:connection-lifecycle} and is further detailed in \cref{connection-lifecycle}.

#### Connection Lifecycle

For a conventional proxy, the lifecycle of each connection can be split in the following three stages, as seen in \cref{fig:connection-lifecycle-simple}:

Establishment
: The proxy accepts the incoming connection from a client and creates an outgoing connection to the upstream host.

Processing
: Data is repeatedly received from both peers, transformed, and sent to the opposite peer.

Termination
: Once communication has completed, the proxy or its peers will close the connection.

![connection lifecycle in conventional proxy](tikz/connection-lifecycle-simple.tex){#fig:connection-lifecycle-simple}

But sharing connections between the *listener* and *worker* process makes it necessary to synchronize the processes and copy connections between them.
The *listener* will establish connections, because it is designed with additional redundancy, as detailed in \cref{shared-file-descriptor-table}, and can in the future be reused for arbitrary TCP-based protocols due to its protocol-agnostic design.
Processing is then handled by the *worker*, and to terminate the connection, both processes must each close their redundant file descriptors.

The resulting state machine of each connection is shared between the *listener* and *worker* processes, where copying of file descriptors and synchronization of processes is handled through IPC messages.
\cref{fig:distributed-state-machine} shows the states typically traversed by a connection, where edges crossing the dashed border between processes represent IPC messages.

![**WIP** Distributed connection state machine](tikz/ipc.tex){#fig:distributed-state-machine}

#### Shared File Descriptor Table

To avoid losing a connection if a process were to terminate, its file descriptors must be shared between multiple processes.
Then the connection is only closed once all file descriptors in all processes are closed or all processes terminate.
Therefore, for any connection, file descriptors should exists in more than one process.
During connection processing, the *listener* already acts as a backup for the *worker*, but even while establishing the connection its file descriptors should be shared between multiple processes.

Newly created file descriptors could be copied to the other process immediately, but this still leaves a window between creation of the file descriptor and subsequent copying, where it only exists in a single process and will be lost if this process terminates.
Arguably, a newly received or created connection will have accumulated very little state and can be retried more cheaply, but ideally even this can be avoided.

With the `clone(2)`{.manpage} syscall, Linux offers an interface to spawn new child processes or threads, similar to `fork(2)`{.manpage}, but `clone(2)`{.manpage} offers fine-grained control over which resources will be shared by the two processes.
E.g., `CLONE_VM` results in the processes sharing their memory and is one of the flags used to create threads instead of processes.
With the `CLONE_FILES` flag the new process will share its parent's file descriptor table instead of receiving a copy.
This means any operations that create, close, or change [@F_SETFD] a file descriptor in one of the processes operate on the shared file descriptor table and are immediately visible in all processes.
This avoids the window introduced by manually copying the file descriptors altogether.

![Processes with a shared file descriptor table](tikz/clone-files.tex){#clone-files}

The file descriptor table is unshared if one of the processes calls `execve(2)`{.manpage}, i.e. processes with different executables cannot use a shared file descriptor table.
Therefore, the *listener* and *worker* processes will not be able to share their file descriptor table, but the *listener* process can be split into process pair of a *passive listener* and an *active listener*.

The *passive listener* calls `clone(2)`{.manpage} with `CLONE_FILES` and creates a new *active listener*.
The *active listener* will now perform the tasks outlined above while the *passive listener* acts as a backup.
Any file descriptor created by the *active listener* will instantaneously exist in the two processes.
If one of the two *listener* processes terminates, the other will detect it and recreate the process pair to ensure file descriptors are held by more than one process.
The *worker* will not create outgoing connections itself, but instead the *active listener* will create them on behalf of the *worker* to ensure they always exist in multiple processes as well.
Both *listeners* will keep a reference to the shared memory as well as connections and therefore act as a backup for *worker* processes.

#### Inter-process communication

As shown in \cref{fig:distributed-state-machine}, the *active listener* and *worker* must communicate during connection processing, be it to synchronize, instruct the other process to perform an operation, or to copy file descriptors.

IPC will be performed through a stateless protocol over UNIX sockets.
Each message contains a single command or result thereof and references a connection in shared memory.
Peers sequentially process incoming IPC messages, operate on the referenced connection, and if applicable reply with the result.
File descriptors are copied between processes alongside IPC messages through UNIX sockets' *ancillary messages* [@unix7].

### Worker Process

The *worker* will process the connections received from the *listener*, transforming the data flowing through it.
Since transformations of connections can become arbitrarily complex, the *worker* process has a large potential for bugs and is therefore separated into its own process.
It could theoretically be run with reduced privileges to further isolate failures or potentially compromised *workers* from other parts of the proxy.

The proxy is implemented so that in the future it can be easily extended to spawn multiple *workers* simultaneously, but in its current implementation it will only run with a single *worker* process, intentionally forfeiting parallelisation.
Nevertheless a single *worker* process will process multiple connections concurrently through the use of asynchronous I/O.

#### Asynchronous I/O

In contrast to *synchronous* or *blocking* I/O, where a process will pause execution until the operations is completed, *asynchronous* or *non-blocking* I/O allows a process to register I/O operations and receive notifications from the operating system when those operations are ready to be processed.
In the meantime other computations can be performed.

Thus, each process will register all its connections, suspend its execution, wake up only when I/O is ready, and then process as much data as available.
This avoids the context switches between processes but the process itself must adapt its control flow.
While some programming languages offer abstractions of this so called *event loop* in the form of coroutines, the resulting state machine will be handled manually.

\cref{poll} illustrates how a process could wait for I/O events on multiple connections simultaneously using `poll(2)`{.manpage}.
Any connection state handling would occur in `perform_io()`{.c} and is omitted.

:::{.figure #poll}
```c
/* Query I/O on all active connections. */
struct pollfd events[NUM_FDS] = {
    { .fd = conn_fds[0], .events = EPOLLIN | EPOLLOUT },
    { .fd = conn_fds[1], .events = EPOLLIN | EPOLLOUT },
    /* ... */
};
/* Event loop */
while(1) {
    /* Execution will be paused until I/O is possible
     * on any of the file descriptors. */
    int ready = poll(events, NUM_FDS, -1);
    for(int i = 0; i < ready; ++i) {
        perform_io(events[i].fd);
    }
}
```

:::{.label}
Event loop with `poll(2)`{.manpage}
:::
:::

#### Shared Memory

As previously mentioned all state associated with a connection is placed in shared memory and both *listener* processes hold a reference to it.
If the *worker* terminates it is still accessible in the *listener* processes and the *worker* can be restored.

But, as explained in \cref{signals}, a process could potentially be terminated at any time.
If it were to terminate midway during an operation on shared memory it could leave the shared memory in a corrupted or inconsistent state.
All operations on shared memory must therefore be performed in a manner that prevents it from being corrupted or otherwise reaching an invalid state under any circumstances.
This includes the receiving, sending, and transforming of data as well as any transitions of the state machine.

The so-called *transactional memory* [@transactional-memory] will be implemented in software and is closely related to database transactions.
The *ACID* properties can be ensured in the following ways:

Atomicity
: Memory transactions are committed through atomic CPU instructions [@iso9899-2011-n1570].

Consistency
: All mutually dependent state is written by a single atomic instruction.

Isolation
: Shared memory is generally owned by a *worker* process and only accessed by other processes during recovery.

Durability
: Durability is provided by the operating system and the semantics of shared memory itself. As long as a single process with access to the shared memory remains, the memory will persist. Durability in the case of a complete system failure, e.g., by storing it on disk, is not considered.

#### Transactional I/O

All I/O buffers must be placed in the shared memory as well to avoid the scenario outlined in \autoref{lost_read}, but special care has to be taken so that metadata is preserved as well.
Especially return values of syscalls can be difficult to store safely, since, depending on calling conventions, they cannot be written to shared memory directly.
In the example \autoref{lost-return}, the data is safely placed in shared memory, but the number of bytes copied is lost, violating the *consistency* property.

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

The syscalls `recvmmsg(2)`{.manpage} and `sendmmsg(2)`{.manpage} provide an interface that can write the number of bytes received or sent directly into shared memory and are used to achieve atomic I/O operations.
This is described in detail in \cref{foo}.

## Recovery

The most probable scenario is a *worker* process crashing for any reason.
In that case, the *active* listener will detect it and restart a new *worker* process.
Since all connections also exist in the *listener's* processes and their state completely resides in shared memory, which is also accessible from the *listener* processes, the new *worker* will simply receive established connections from the *active listener* and resume execution directly from the saved state.

If the *active listener* terminates, the *passive listener* will detect its child process terminating and immediately start a new *active listener*.
While the *active listener* is not running, no new incoming connections will be accepted, but the operating system will still add incoming connection to the listening socket's queue of pending connections, since the listening socket will still be kept alive in the *passive listener*.
This may result in a longer time to connect for the clients, but will otherwise be transparent to clients.
In its current implementation, the *worker* process will be restarted as well to return to a well-known state, but this is not strictly necessary, as the new *active listener* could monitor pre-existing *worker* processes as well as newly create ones.

Should the *passive listener* terminate, it is monitored by the *active listener*, and it will become the new *passive listener*.
The new *passive listener* will then start an *active listener*, which will follow the same steps as outlined in the previous paragraph.

So as long as either one of the *listener* processes keeps running, the proxy will recover and continue its execution without any interruption to its peers.

## Live-Upgrade

Planned migration can be seen as a subset of recovery from a crash.
If the proxy can recover from a crash at any point, it can recover from a controlled restart, but will need to execute the new binary somewhere during recovery.

If the *worker's* executable is replaced by a new version and the *worker* process is killed, the previously described recovery will simply start a *worker* of the new version.
If the *worker's* version stays ABI-compatible, that is the new version will work with the previous version's shared memory, it will resume execution just as the old *worker* would.
Otherwise *worker* and *listener* will have to be migrated together since they both access the shared memory.

To migrate the *listener* to a new executable it will need to execute the new executable during recovery.
If the *passive listener* terminates, the remaining *active listener* will need to re-execute the *listener's* executable instead of changing its role internally.
If the *active listener* termiantes the remaining *passive listener* can re-execute as well.
Thus if the *listener's* executable is replaced, the new version will be executed.
As the *worker* processes will be restarted in the current implementation, a new *worker* will be executed automatically.
Otherwise the new *listener* can kill the old *workers* and start new ones, thereby migrating both executables together.
In either case the *listener's* state including file descriptors and shared memory will be passed to the new executable through command line arguments.
