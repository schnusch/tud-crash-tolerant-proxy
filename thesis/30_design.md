# Design

The lifecycle of each connection is shown in \autoref{connection-lifecycle-simple} and can be split in the following three stages:

 1. To establish the connection the proxy accepts incoming connection from a client.
    It then connects to the server.
 2. After the connections are established data is received from the peers repeatedly.
    Any received data is transformed and sent to the opposite peer.
    The transformations can evaluate, filter, or even manipulate the transmitted data.
 3. Once the communication has completed the proxy or the peers will close the connection.

![Connection lifecycle](tikz/connection-lifecycle-simple.tex){#connection-lifecycle-simple}

A multi-threaded or multi-process architecture lends itself to implementing a proxy, since it handles multiple connections concurrently.
If the number of concurrent connections is relatively few, each connection can be handled by a dedicated thread or process.
But for numerous concurrent connections this is no longer feasible.
The overhead of each process and the context switching between processes will become to great.
A single process needs to handle multiple connections in parallel using \cref{asynchronous-io}.

## Asynchronous I/O

In contrast to *synchronous* or *blocking* I/O where a process will pause execution until the operations is completed, *asynchronous* or *non-blocking* I/O allows a process register I/O operations and receive notifications from the operating systm when those operations are ready to be processed.
In the meantime other computations can be performed.

So each process will register all its connections, suspend its execution, wake up only when I/O is ready, and then process as much data as available.
This avoids the context switches between processes but the process itself must adapt its control flow, since processing of connections will be interwoven.
While some programming languages offer abstractions of this so called *event loop* in the form of coroutines, the resulting state machine is handled manually.

:::{.figure}
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

## Multi-Process Architecture

Besides potential parallelization a multi-process architecture it isolates individual operations from each other as well.
If merely separated in threads, a misbehaving thread can intentionally termiante the whole process, e.g. with `exit_group(2)`{.manpage}, or can accidentally cause a fault that terminate the whole process, be it through bugs or other unintended and erroneous behaviour.
But if separated in processes, a misbehaving processes will be isolated from other processes.
This can be further improved by executing potentially risky operations with reduced privileges to further mitigate any impact.

A common pattern in existing web servers such as *nginx* [@nginx_admin_manual] or *HAProxy* [@haproxy_manual] is that a single *master* process controls a handful of *worker* processes and each *worker* process accepts and processes incoming connections on their own.
This pattern is modified slightly in this proxy and will be described in \cref{listener-worker-architecture}.

### Listener-Worker-Architecture

A single *listener* process accepts incoming connections and subsequently hands them over to the *worker* processes, the *worker* processes will not accept or create connections themselves.
Although this leads to a more complex connection setup as seen in \autoref{connection-lifecycle} it allows to handle file descriptors more reliably and will be explained \cref{shared-file-descriptor-table}.

The *listener* is intentionally kept minimal without external dependencies besides the C standard library and its operations are limited to

 1. starting and monitoring the worker process,
 1. accepting incoming connections, and
 1. creating new outgoing connections.

It is implemented in a protocol agnostic manner and the connections are instead handed over to the *worker* for processing, allowing the *listener* to be reused for any TCP-based connection protocol.

![Connection lifecycle with a separate listener](tikz/connection-lifecycle.tex){#connection-lifecycle}

The *worker* will process the connections received from the *listener*.
Besides separate processes the *listener* and *worker* are implemented in separate executables as well, so they can be linked differently.
The *listener* can maintain its minimal dependencies, while the *worker* may be linked against a multitude of external libraries that aid in processing of the connections.
The executables can also be updated independently and allow to individually migrate parts of the proxy to newer versions.
This is discussed in detail in \cref{live-update}.

![Process architecture of the proxy](tikz/process-tree-simple.tex){#process-tree-simple}

The substantially more complex *worker* process offers a much larger potential for bugs than the *listener* and could theoretically be run with reduced privileges.
This would further isolate failures or potentially compromised workers without affecting the listener or other workers.
The *listener* could potentially monitor the *worker* for any anomalous behaviour and terminate or restart it.

The proxy is implemented so that in the future it can be easily extended to spawn multiple *worker* processes simultaneously but in its current implementation it will only run with a single *worker* process intentionally forfeiting parallelisation.
Nevertheless a single *worker* process will process multiple connections concurrently.

### Shared Memory

To persist the state associated with each connection it is placed in shared memory, so that it is persisted even if the *worker* process terminates.
This extends the architecture from \autoref{process-tree-simple} to \autoref{process-tree-simple-shmem}.

![Process architecture of the proxy](tikz/process-tree-simple-shmem.tex){#process-tree-simple-shmem}

But as explained in \cref{signals} a process could potentially be terminated at any time.
If it were to terminate midway during an operation on shared memory it could leave the shared memory in a corrupted or inconsistent state.
All operations on shared memory must therefore be performed in a manner that prevents it from being corrupted or otherwise reaching an invalid state under any circumstances.

This is closely related to the *ACID* properties of database transactions and can be ensured in the following ways:

Atomicity
: Memory transactions are commited through atomic CPU instructions [@stdatomic].

Consistency
: All mutually dependent state is written by a single atomic instruction.

Isolation
: Shared memory is generally owned by a *worker* process and only accessed by other processes during recovery.

Durability
: The durability is provided by the operating system and the semantics of shared memory itself. As long as a single process with access to the shared memory remains, the memory will persist. Durability in the case a complete system failure, e.g. by storing it on disk, is not considered.

#### Transactional I/O

All I/O buffers must be placed in the shared memory as well to avoid the scenario outlined in \autoref{lost_read}, but special care has to be taken so that metadata is preserved as well.
Especially return values of syscalls can be difficult to store safely, since, depending on calling conventions, they cannot be written to shared memory directly.
In the example \autoref{lost-return} the data is safely placed in shared memory but number of bytes copied is lost, violating the *consistency* property.

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

### Shared File Descriptor Table

To not loose a connection if a process were to terminate but instead persist it, it can be shared between process.
Then the connection is only closed once all file descriptors in all processes were closed or the processes terminated.
Therefore for any connection file descriptors should exists in more than one process.

Any newly created file descriptors must immediately be copied to the other process, so that they are *backed up*.
But if a connections is to be closed intentionally, all its file descriptors must be closed.
This means both processes need to close it.

Even if file descriptors are copied immediately from the *listener* to the *worker* a short window persists in which the connection is only available in a single process.
It could be argued the newly created connection can be more easily recovered, since relatively little state is yet to have been accrued, but ideally even for newly created file descriptors redundancy can be provided.

With the `clone(2)`{.manpage} syscall Linux offers an interface to spawn new child processes or threads, similar to `fork(2)`{.manpage}, but `clone(2)`{.manpage} offers fine-grained control over which resources will be shared by the two processes.
E.g. `CLONE_VM` results in the processes sharing their memory and is one of the flags used to create threads instead of processes.
With the `CLONE_FILES` flag the new process will share its parent's file descriptor table instead of receiving a copy.
This means any operations that create, close, or change [@F_SETFD] a file descriptor in one of the processes operate on the shared file descriptor table and are immediately visible in all processes.
This avoids the window introduced by manually copying the file descriptors altogether.

![Processes with a shared file descriptor table](tikz/clone-files.tex){#clone-files}

The file descriptor table is unshared if one of the processes calls `execve(2)`{.manpage}, i.e. processes with different executables cannot use a shared file descriptor table.
The *listener* and *worker* processes will not be able to share their file descriptor table but the *listener* process can be split into process pair of a *passive listener* and an *active listener* and the architecture of the proxy can be further extended as seen in \autoref{process-tree}.

The *passive listener* calls `clone(2)`{.manpage} with `CLONE_FILES` and creates a new *active listener*.
The *active listener* will now perform the tasks outlined above while the *passive listener* acts as a backup.
Any file descriptor created by the *active listener* will instantaneously exist in the two processes.
If one of the two *listener* processes terminates the other will detect it and will re-create the process pair to ensure file descriptors are held by more than one process.

Both *listeners* will act as a backup for the *worker* process.

![Final architecture of the proxy](tikz/process-tree.tex){#process-tree}

### Inter-process communication

Since redundant copies of file descriptors are kept in separate processes the connections state machine is distributed between processes as well.
\autoref{connection-state-machine} visualizes said state machine excluding possible error states.
Edges crossing the dashed border between the processes are IPC messages used to synchronize between processes.

![Connection state machine](tikz/ipc.tex){#connection-state-machine}

File descriptors are copied between processes alongside IPC messages through UNIX socket's *ancillary messages* [@unix7].
The IPC protocol is stateless, each messages contains a single command or result thereof and references a connection in shared memory.
Peers sequentially process incoming IPC messages, operate on the referenced connection, and if applicable reply with the result.

## OOM-Killer

One initial design goal was to recover if one the proxies processes is killed by the Linux kernel.
In an out-of-memory situation the Linux kernel will calculate a *badness score* for each process and will terminate processes with the highest *badness score* to free resources.
However a privileged process can adjust its *badness score* rather easily through `proc_pid_oom_score_adj(5)`{.manpage} and protect it from OOM-killing completely.

While this goal can be easily achieved other scenarios outlined in \cref{signals} that unconditionally kill a process remain and justify the elaborate architecture.

## Recovery

The most probable scenario is a *worker* process crashing for any reason.
In that case the *active* listener will detect it and restart a new *worker* process.
Since all connections also exist in the *listener's* processes and their state completely resides in shared memory, that is also accessible from the *listener* processes, the new *worker* will simply receive established connections from the *active listener* and resume execution directly from the saved state.

If the *active listener* terminates, the *passive listener* will detect its child process terminating and immediately start a new *active listener*.
While the *active listener* is not running no new incoming connections will be accepted, but the operating system will still add incoming connection to the listening socket's queue of pending connections, since the listening socket will still be kept alive in the *passive listener*.
This may result in a longer time to connect for the clients, but will otherwise be transparent to clients.
In its current implementation *worker* process will be restarted as well to return to a well-known state, but this is not strictly necessary, as the new *active listener* could monitor pre-existing *worker* processes as well as newly create ones.

Should the *passive listener* terminate, it is monitored by the *active listener* and the it will become the new *passive listener*.
The new *passive listener* will then start an *active listener*, which will follow the same steps as outlined in the previous paragraph.

So as long as either one of the *listener* processes keeps running, the proxy will recover and continue its execution without any interruption to its peers.

## Live-Upgrade

Planned migration can be seen as a subset of recovery from a crash.
If the proxy can recover from a crash at any point, it can recover from a controlled restart, but will need to execute the new binary somewhere during recovery.

If the *worker's* executable is replaced by a new version and the *worker* process is killed, the previously described recovery will simply start a *worker* of the new version.
If the *worker's* version stay ABI-compatible, that is the new version will work with the previous' version's shared memory, it resume execution just as the old *worker* would.
Otherwise *worker* and *listener* will have to be migrated together since they both access the shared memory.

To migrate the *listener* to a new executable it will need to execute the new executable during recovery.
If the *passive listener* terminates the remaining *active listener* will need to re-execute the *listener's* executable instead of changing its role internally.
If the *active listener* termiantes the remaining *passive listener* can re-execute as well.
Thus if the *listener's* executable is replaced, the new version will be executed.
As the *worker* processes will be restarted in the current implementation, a new *worker* will be executed automatically.
Otherwise the new *listener* can kill the old *workers* and start new ones, thereby migrating both executables together.
In either case the *listener's* state including file descriptors and shared memory will be passed to the new executable through command line arguments.
