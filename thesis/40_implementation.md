# Implementation

C was chosen as the programming language to implement the proxy, mostly due to its widespread use in systems programming and familiarity.
While it does not offer any memory safety or high-level abstraction such as coroutines natively, it offers great control over memory management and operating system interfaces.
In the future memory safety could potentially be retrofitted with Fil-C [@fil-c].

In its current implementation, the proxy comprises nearly 6,000 lines of code, with approximately 2,000 lines each for the *worker*, *listener*, and shared components.

:::{.hidden}
```
      52 text files.
      48 unique files.
       5 files ignored.

github.com/AlDanial/cloc v 2.10  T=0.08 s (597.3 files/s, 91921.7 lines/s)
-------------------------------------------------------------------------------
Language                     files          blank        comment           code
-------------------------------------------------------------------------------
C                               22            496            528           5130
C/C++ Header                    23            169            470            574
make                             1              4              0              8
Markdown                         2              1              0              7
-------------------------------------------------------------------------------
SUM:                            48            670            998           5719
-------------------------------------------------------------------------------
```
:::

## Passive Listener

The *passive listener* is by far the simplest of the proxy's processes and shares little functionality with the *active listener* or *worker*.
Its control flow is summed up in \cref{fig:state-machine-passive}.

![Main loop of the *passive listener*](tikz/automata-passive.tex){#fig:state-machine-passive}

As the initial process, the *passive listener* will initialize listening sockets, shared memory, and IPC sockets later used by all processes.
A memory file descriptor, short *memfd*, i.e., an anonymous file residing in memory instead of the filesystem, is created; this file is later mapped into the processes' address space and backs the shared memory.

The *passive listener* will then call `clone3(2)`{.manpage} to spawn the *active listener* and suspend until the process receives a signal.
`clone3(2)`{.manpage} is chosen over `clone(2)`{.manpage}, even though it is not exposed by the GNU C Library [@glibc-clone3], because it is more ergonomical and, in particular, does not require the caller to manually allocate a stack for the child process.

Unfortunately `clone3(2)`{.manpage} is not supported by Valgrind [@kde-420906] and its *memcheck* tool can no longer be used to debug the *listener*.
To allow debugging, an alternative code path spawns the *active listener* through fork; this allows to debug the *listener*, however breaks proper file descriptor handling.

Signals are received and processed in a loop.
Instead of conventional asynchronous signal handlers the process will poll for signals synchronously through `sigwait(3)`{.manpage}.
This avoids issues with the signal handler's preemptive control flow and `signal-safety(7)`{.manpage} in general.
While most signals are ignored, some keep their default action, either because they represent exceptional process conditions or because their default behavior should not be altered.
Selected signals are handled explicitly:

`SIGINT`
: This signal was picked to terminate the proxy. If received, the *passive listener* will not restart the *active listener* but instead cease operations. This is also the signal that is generated when the user presses `Ctrl+C` on a terminal.

`SIGCHLD`
: If a child process terminates, the operating system will send this signal to its parent process, as is the case if the *active listener* terminates where the *passive listener* will be signaled.

When the *active listener* terminates the *passive listener* will restart it.
It will try to re-execute its binary, which resets the process and discards all unintentionally collected state, but active connections, shared memory, listening sockets, and IPC sockets are passed to the new executable.
If the new executable cannot be executed, the current executable will restart the *active listener* itself.

### Command-Line Interface

The proxy will usually be invoked with the command line arguments shown in \cref{fig:cmdline-listener}.
The proxy will listen on localhost on typical HTTP port and forward connections to the upstream address `192.0.2.1:80`.
The `--listener`{.sh} and `--worker`{.sh} arguments provide the paths to the proxy's executables.

:::{.figure #fig:cmdline-listener}
```sh
/opt/crash-tolerant-proxy/bin/listener \
    --upstream-addr="192.0.2.1:80" \
    --listen-addr="127.0.0.1:80" \
    --listen-addr="[::1]:80" \
    --listener="/opt/crash-tolerant-proxy/bin/listener" \
    --worker="/opt/crash-tolerant-proxy/bin/worker"
```

:::{.label}
Example command line of the *listener*
:::
:::

If the *listener* re-executes itself, sockets and other file descriptors will be passed to the new executable directly.
This is done through the command line parameters seen in \cref{fig:cmdline-listener-restart}, with the following exemplary values:

`--listen-fd=3` and `--listen-fd=4`
: Listening file descriptors on the addresses provided in \cref{fig:cmdline-listener}.

`--shared-memory-fd=5`
: The shared memory's *memfd* containing connection states.

`--ipc-broadcast=6,7`
: `socketpair(2)`{.manpage} used to communicate with existing *worker* processes.

`--worker-process=${worker_ipc_fd},${worker_pid},${worker_pid_fd}`{.sh}
: IPC socket file descriptor; process ID; and *process file descriptor*, explained in \fullref{active-listener}, to communicate with and control an existing *worker* process. The argument will be repeated for each *worker* process.

:::{.figure #fig:cmdline-listener-restart}
```sh
/opt/crash-tolerant-proxy/bin/listener \
    --listener="/opt/crash-tolerant-proxy/bin/listener" \
    --worker="/opt/crash-tolerant-proxy/bin/worker" \
    --upstream-addr="192.0.2.1:80" \
    --shared-memory-fd=5 \
    --ipc-broadcast=6,7 \
    --worker-process="${worker_ipc_fd},${worker_pid},${worker_pid_fd}" \
    --listen-fd=3 \
    --listen-fd=4
```

:::{.label}
Example command line of the *listener* during re-execution
:::
:::

## Common Components

Both the *active listener* and *worker* process operate on shared memory structures and communicate over a shared channel.
Common components are implemented once and reused by both executables.

### Shared Memory {#impl:shared-memory}

*listener* and *worker* share connection state through a memory region backed by a *memfd*.
Each process uses `mmap(2)`{.manpage} to create a shared mapping [@mmap2] of the *memfd* in its address space.
The layout of the shared memory is a simple `struct`{.c}, with the following fields shown in \cref{fig:shmem-layout}:

`struct connection connections[]`{.c}
: The array contains the complete state of each connection of the proxy. In the current implementation this is an array of elements of fixed size, meaning data stored per connection must be of fixed size.

`atomic_size_t size`{.c}
: This field contains the length of the complete *memfd* including the `size` field itself. When a new connection is appended to the array, the size of the *memfd* will be updated, so other processes can in turn update their mappings. Since multiple processes can operate on this field simultaneously it must support atomic operations.

:::{.figure #fig:shmem-layout}
```c
struct shared_memory {
    atomic_size_t size;
    struct connection connections[];
};

struct connection {
    /** State of the connection */
    atomic_int state;
    /* ... */
};
```

:::{.label}
Layout of the shared memory
:::
:::

The connection state `struct connection`{.c} contains more fields than shown in \cref{fig:shmem-layout} and will be explained in \fullref{connection-processing}.
However, `atomic_int state`{.c} is used to store the possible states of the connection's state machine.
When the *listener* accepts a new connection it will pick an already allocated slot with `state == CONN_UNUSED`{.c} or append a new `struct connection`{.c} to the *memfd*.

Each process tracks its shared memory mapping with a `struct shared_memory_mapping`{.c} from \cref{fig:shmem-map}.
This stores the following values:

`int fd`{.c}
: The *memfd* is needed to create and resize the mapping. It is stored alongside the mapping.

`struct shared_memory *addr`{.c}
: The pointer to the mapped memory region.

`size_t length`{.c}
: The number of bytes currently mapped in the process. This can differ from the `size` field in `struct shared_memory`{.c} if new connections were appended or the *memfd* was truncated and the mapping will need to be updated.

:::{.figure #fig:shmem-map}
```c
struct shared_memory_mapping {
    struct shared_memory *addr;
    size_t length;
    int fd;
};
```

:::{.label}
Mapping of the shared memory
:::
:::

### Inter-process communication {#impl:ipc}

As stated in \fullref{design:ipc}, IPC will be performed over UNIX sockets.
Datagrams are chosen over streams, because they preserve message boundaries, which makes buffering of partial messages no longer necessary and handling of *ancillary messages* easier, since they will be tied directly to a datagram.
While IP datagram sockets use the UDP protocol without any reliability guarantees, UNIX datagram sockets are reliable [@unix7].
However, `SOCK_SEQPACKET` is chosen over `SOCK_DGRAM` since it is datagram- as well as connection-oriented and will signal connection hang-ups should the other process terminate.

![The two available IPC channels](tikz/ipc-sockets.tex){#ipc-sockets}

In anticipation of parallelization each *worker* is connected to the *listener* by two IPC sockets.
One of the sockets is for 1:1 communication between the *worker* and *listener*, used for most IPC datagrams.
The other socket is a 1:*N* channel shared by all *worker* processes.
The *listener* will send newly accepted connections into this 1:*N* channel and the first *worker* that reads from the 1:*N* socket will receive the datagram and accompanying connection.
Incoming connections will thus be automatically distributed among all *worker* processes.

File descriptors are copied through *ancillary messages*: the sender prepares a *control message buffer*, places file descriptors in it, and sends the *control message buffer* alongside a normal datagram with `sendmsg(2)`{.manpage}.
On the receiving end the *control message buffer* can be received with `recvmsg(2)`{.manpage}.
All file descriptors that fit into the receiver's buffer are created in the process, further file descriptors will be dropped silently.
While it is possible to copy multiple file descriptors at once, each IPC message will be accompanied by at most one file descriptor.

![Typical IPC messages](tikz/ipc-msgs.tex){#ipc-msgs}

During normal operations the IPC messages seen in \cref{ipc-msgs} are sent.
The *listener* will only send `"close slot=%d"` to indicate an error to the *worker* if it cannot connect to the upstream host.
During recovery, when preexisting connections are passed to a *worker*, this is extended by additional messages.

The first word of each message is the *action*, followed by the *slot* argument.
The string `%d` is a `printf(3)`{.manpage} placeholder for a decimal integer, that will be used as the index of the shared array of connections.
Further arbitrary arguments can be appended, which is currently only used by `connect` messages.
In its current implementation IPC messages are limited to 511 bytes.
The example in \cref{ipc-send} will send the message `"connect slot=1 addr=192.0.2.1:80"`, the *listener* receives it, and creates an outgoing connection for `shared->connections[1]`.

:::{.figure #ipc-send}
```c
int ipc_send(int ipc_fd,
             const char *action,
             size_t slot,
             int fd,
             const char *fmt,
             ...);
/* ... */
ipc_send("connect", 1, -1, "addr=%s", "192.0.2.1:80");
```

:::{.label}
Usage of `ipc_send`
:::
:::

\Cref{ipc-recv} shows how messages are received.
`ipc_process_incoming` receives and parses as many messages as possible, without blocking, and calls the first callback method with a matching *action*.
This allows to easily add IPC messages with new *actions*.
In the example above, `ipc_connect`{.c} would be called.

:::{.figure #ipc-recv}
```c
typedef int (*ipc_method_t)(const char *action,
                            size_t slot,
                            int fd,
                            const char *tail,
                            void *ctx);
struct ipc_action_method {
    const char *action;
    ipc_method_t method;
};
int ipc_process_incoming(int ipc_fd,
                         const struct ipc_action_method *methods,
                         void *ctx);
/* ... */
ipc_method_t ipc_connect;
ipc_method_t ipc_close;
const struct ipc_action_method methods[] = {
    {"connect", ipc_connect},
    {"close", ipc_close},
    {NULL, NULL}
};
ipc_process_incoming(ipc_fd, methods, &ctx);
```

:::{.label}
Usage of `ipc_process_incoming`
:::
:::

## Active Listener

While the *active listener* starts and monitors the *worker* process, it accepts incoming connections on all listening sockets and responds to IPC messages from the *worker* processes.
To poll for incoming connections and monitor *worker* processes `epoll(7)`{.manpage} is used.

The *active listener* handles signals using a `signalfd(2)`{.manpage} instead of `sigwait(3)`{.manpage}.
While `F_SETOWN(2const)`{.manpage} and `SIGIO` can instead be used to monitor sockets, e.g. when a new connection is received, it is not supported by all types of file descriptors, especially *process file descriptors* needed to monitor other processes.

### Starting and Monitoring Workers

The *active listener* will start new processes during its initial run or whenever a *worker* process terminates.
*Workers* will be started with the necessary file descriptors required by \fullref{design:ipc} and the *memfd* of the shared memory.
All other file descriptors are marked with [`FD_CLOEXEC`](https://man7.org/linux/man-pages/man2/F_SETFD.2const.html) using `close_range(2)`{.manpage} and are, therefore, not inherited by the *worker* executable.

Normally processes can only `wait(2)`{.manpage} for their child processes, but if the *active listener* is restarted for any reason, previously started *worker* processes will have been orphaned and will no longer be a child process of the *active listener*.
To also monitor these *adopted* *worker* processes, a *process file descriptor* or *pidfd* is created for each *worker* process and monitored with `epoll(7)`{.manpage}.
Conceivably, the *worker's* 1:1 IPC socket mentioned in \fullref{design:ipc} could be used instead, since it will be closed when the *worker* process terminates.

### Accepting Connections

When I/O on a listening socket becomes ready, i.e., an incoming connection is pending, the *listener* needs to allocate a new connection in shared memory.
A connection with `state == CONN_UNUSED`{.c} is selected or a new connection is appended to shared memory.
This is one of the the only times when multiple processes can access the same connection simultaneously, the *worker* could change the state of a connection, while the *active listener* iterates over all connections to find an unused slot.

Once a connection is selected, its state will be changed to `CONN_ACCEPTING`{.c} prior to calling `accept(2)`{.manpage}.
`accept(2)`{.manpage} will return a file descriptor to the incoming connection.
Depending on calling conventions of the system, it will be returned in a register or on the stack but cannot be placed in the shared memory directly.
If the process were to terminate before the return value is copied to shared memory, the file descriptor will have been created in the *active* and *passive listeners*, but its number will have been *forgotten*.
\Fullref{recovery} discusses recovery of *forgotten* file descriptors.

After the file descriptor to the connection is created and safely stored in shared memory, it will be copied to the *worker* process and processing of the connection will continue in the *worker*.
The *worker* may pass control flow back to the *active listener* to connect to the upstream host.

The exact control flow in \cref{fig:ipc-connect} will be further discussed in \fullref{worker:accept}.

### Monitoring the Passive Listener

The *active listener* monitors its parent process, the *passive listener*, as well, and assumes the *passive listener's* role if it terminates.

Conventionally, this would be achieved through a `pipe(7)`{.manpage}, where the parent process would own the *write end* and passes the *read end* to the child process.
If the parent process terminates, the write end will be closed, and the child process will detect `EOF` on its end of the pipe.
But since both *listeners* share the same file descriptor table the write end will not be closed when the *passive listener* terminates.

Instead, the *passive listener* creates a *process file descriptor* referring to itself, passes it to its child process, and the *active listener* will use it to poll its parent process.

## Worker

After the *active listener* and *worker* created the connection together, the *worker* will be solely responsible for the next phase of the connection's lifecycle.
For each connection the *worker* repeatedly receives data from both peers, transforms it, and sends the data to the peers, while keeping track of the connection's state.
This processing happens for all connections in parallel.

### Asynchronous I/O

In contrast to *synchronous* or *blocking* I/O, where a process will pause execution until the operation is completed, *asynchronous* or *non-blocking* I/O allows a process to register I/O operations and receive notifications from the operating system when those operations are ready to be processed.
In the meantime other computations can be performed.

Thus, the *worker* will register all its connections, suspend its execution, wake up only when I/O is ready, and then process as much data as available.
This avoids expensive context switches between threads, however, the process itself must adapt its control flow.
While some programming languages offer abstractions of this so-called *event loop* in the form of coroutines, the resulting state machine will be handled manually.

:::{.hidden}
\Cref{poll} illustrates how a process could wait for I/O events on multiple connections simultaneously using `poll(2)`{.manpage}.
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

:::

### Main Loop

Each connection passing through the proxy consists of two file descriptors: the file descriptor of the downstream connection to the client and the file descriptor of the upstream connection to the server.
So the number of file descriptors that need to be monitored, can grow substantially.
`epoll(7)`{.manpage} is designed to perform well under these conditions, so the *worker* will register all its connection file descriptors, its two IPC sockets, and potentially a `signalfd(2)`{.manpage} with `epoll(7)`{.manpage}, and wait for any file descriptors to become ready.

Then, depending on the type of file descriptor that became ready, an action is taken.
It may need to process pending signals or IPC messages, receive data from, or send data to a client or host.
When the *worker* registers file descriptors with `epoll(7)`{.manpage} it will also save the type of the file descriptor and, in the case of connection file descriptors, the index of the `struct connection`{.c} in an accompanying data structure.
When I/O becomes available the *worker* will look up the file descriptor's type so that the proper action can be selected.

`ipc_process_incoming`, previously seen in \fullref{impl:ipc}, handles IPC sockets and with it connection establishment, while signals are handled individually.
In the current implementation only `SIGUSR1` and `SIGUSR2` are handled explicitly and will print debug information, while all other signals retain their default behaviour.
For any connection file descriptor `handle_connection()`{.c} is called, its operations are described in \fullref{connection-processing}.

### Command-Line Interface

The *active listener* will start the *worker* with a command-line similar to \cref{fig:cmdline-worker}.
During startup, it will only receive the IPC sockets, the shared memory, and, in its current implementation, the address of the upstream host, which could alternatively be extracted from the incoming connections themselves.
It will then receive connections through the IPC sockets.

:::{.figure #fig:cmdline-worker}
```sh
/opt/crash-tolerant-proxy/bin/worker \
    --ipc-broadcast=0 \
    --ipc-direct=8 \
    --shared-memory-fd=5 \
    --upstream-addr="192.0.2.1:80"
```

:::{.label}
Example command line of the *worker*
:::
:::

## Connection Establishment {#worker:accept}

While establishing the connection the *worker* first receives the incoming connection from the *active listener* and then requests the *active listener* to create the outgoing connection to the upstream host.

:::{.figure #fig:http-connect}
```email
CONNECT 192.0.2.1:80 HTTP/1.1
Host: 192.0.2.1:80
```

:::{.label}
HTTP/1.1 CONNECT request
:::
:::

It can receive and evaluate data from the client before establishing the outgoing connection, e.g., to extract the address from the HTTP/1.1 CONNECT request in \cref{fig:http-connect}, or instead of creating an upstream connection the *worker* can potentially process the client's data itself, e.g., respond with an error message or not return any data at all and simply close the client's connection.
As a limitation, the current implementation will always request a connection to the upstream host configured on the command line.
When the *active listener* created the outgoing connection, the *worker* will receive the file descriptor and begin to forward traffic.

![Distributed state machine during connection establishment](tikz/ipc-connect.tex){#fig:ipc-connect}

Besides the peer's address and the state of the connection, the *worker* will tag all connections processed by it, by writing its process ID to the field `worker_pid`{.c} of the `struct connection`{.c} shown in \cref{fig:struct_connection}.
When the *worker* terminates, the *listener* can use this field to determine which connections are no longer handled by a *worker*, i.e., have been *orphaned*, and pass them to newly spawned or other running *worker* processes.

The detailed state machine that handles establishing the connection, seen in \cref{fig:ipc-connect}, is designed to be interruptible at any time.
The connection's `state`{.c}, seen in \cref{fig:struct_connection}, is used to coordinate state transitions between processes and is accessed only with the correct memory ordering.
When reading it is accessed with `memory_order_acquire`{.c}, so no memory access is moved before it; when updating it is accessed with `memory_order_release`{.c}, so no memory access is moved after it; to ensure that any access to the `struct connection`{.c} is synchronized properly.

With the established connection the *worker* will then perform the necessary I/O and transformations.

## Connection Processing

When `epoll(7)`{.manpage} reports I/O events on a connection file descriptor the *worker* will, depending on the events reported by `epoll(7)`{.manpage}, send any pending data, receive incoming data from the operating system, and then transform any received data.
For the two peers or *endpoints* of each connection the following data is stored:

`int fd[2]`{.c}
: File descriptors of the connection to the peer. The file descriptor integers will most likely differ in the *listener* and *worker* process, when copied. `fd[0]` contains the file descriptor in the *listener*, `fd[1]` the file descriptor in the *worker*.

`struct sockaddr_storage addr`{.c}, `socklen_t addrlen`{.c}
: Address of the remote peer, written by the *active listener* with `accept(2)`{.manpage}.

`struct atomic_ring_buffer rx`{.c}
: Receive buffer of the connection endpoint.

`struct atomic_ring_buffer tx`{.c}
: Send buffer of the connection endpoint.

:::{.figure #fig:struct_connection}
```c
struct connection {
    /** State of the connection */
    atomic_int state;
    /** PID of the worker process */
    pid_t worker_pid;
    /** Arbitrary data for `transform` */
    transformation_context_t transform_ctx;
    /** Accepted connection */
    struct connection_endpoint downstream;
    /** Outgoing connection */
    struct connection_endpoint upstream;
};

struct connection_endpoint {
    /** File descriptors of the connection to the peer */
    int fd[2];
    /**
     * Remote address of the incoming connection,
     * set by `accept(2)`.
     */
    struct sockaddr_storage addr;
    socklen_t addrlen;
    /** Receive buffer */
    struct atomic_ring_buffer rx;
    /** Sent buffer */
    struct atomic_ring_buffer tx;
};
```

:::{.label}
Layout of shared memory
:::
:::

The `transform()`{.c} function implements the connection transformation, consuming data from the connection's two receive buffers and appending data to the two send buffers, as visualized previously in \cref{fig:transform}.
The transformation can keep context, e.g. to implement its own internal state machine, which is stored in the shared memory alongside connection data as `transformation_context_t transform_ctx`{.c} from \cref{fig:struct_connection}.

### Transactions

All operations that write to the shared memory, e.g., sending, receiving, and transforming data, must fulfill the properties outlined in \fullref{design:shared-memory}.
Each operation is split into idempotent or atomic blocks and wrapped by a state machine.
The last step of each block atomically updates the state machine to the next block.
If the operation is interrupted and restarted execution will continue with the previously saved block.

This results in transactions with the properties outlined in \fullref{design:shared-memory} for I/O as well as transformations.

### Double-Buffering

Atomic operations on elementary types are directly supported by C11 [@iso9899-2011-n1570] and easily satisfy *ACID* properties.
However more complex types such as arrays or structs, or a single atomic commit of mutually dependent objects must be implemented on top of elementary types.

:::{.figure #fig:double-buffer}
```c
struct double_buffered {
    atomic_bool active;
    struct content copies[2];
};
```

:::{.label}
Double-buffered struct
:::
:::

Operations on any more advanced object are wrapped.
The wrapper stores two copies of the object and atomically switches between the copies.
In \cref{fig:double-buffer} `active` contains the index of `copies`.
Now any write operations are performed on `copies[!active]` and subsequently *activated* by toggling `active` atomically.
This will only work for a single writer, since concurrent writers will operate on and potentially corrupt the same inactive copy `copies[!active]`.
This, however, is sufficient, as transactions primarily serve to prevent partial writes rather than to synchronize competing writes.

The *worker* uses double-buffering to transactionally receive, send, and transform data.

### Transactional I/O {#impl:transactional-io}

Generally, to communicate over sockets the syscalls of the `send(2)`{.manpage}- and `recv(2)`{.manpage}-family are used.
However, as discussed in \fullref{transactional-io}, they cannot be used to atomically copy data between user-supplied and kernel buffers, instead the syscalls `recvmmsg(2)`{.manpage} [@recvmmsg] and `sendmmsg(2)`{.manpage} [@sendmmsg] are employed.

These syscalls are intended to perform multiple `recvmsg(2)`{.manpage} or `sendmsg(2)`{.manpage} calls sequentially without repeatedly changing between user and kernel space.
They each operate on an array of `struct mmsghdr`{.c}; arguments that would be passed to `sendmsg(2)`{.manpage} or `recvmsg(2)`{.manpage} are retrieved from it and, more importantly, their return value will be written to it.
By placing a `struct mmsghdr`{.c} in shared memory the number of bytes sent or received will now be written to the shared memory directly.

I/O is performed on a `struct atomic_ring_buffer`{.c} seen in \cref{fig:double-buffer}.
In a ring buffer, data is only removed from the front or appended to the end, but data in already occupied areas is not changed.
This means a transaction on a ring buffer needs to only change the range of used bytes, i.e. the occupied area, not the contents of the buffer itself.

:::{.figure #fig:atomic_ring_buffer}
```c
struct atomic_ring_buffer {
    /** state of the I/O transaction */
    atomic_int state;
    /** contains sentinel and return value */
    struct mmsghdr mm;
    /** index of currently the active `range` */
    atomic_bool active;
    /** occupied area of the ring buffer */
    struct {
        size_t start;
        size_t len;
    } ranges[2];
    /** backing buffer */
    char buf[RING_BUFFER_SIZE];
};
```

:::{.label}
Transactional ring buffer
:::
:::

The state machine of \cref{fig:atomic-recv-simple}, used to implement a transaction around `recvmmsg(2)`{.manpage}, is extended to \cref{fig:atomic-recv}.
`sendmmsg(2)`{.manpage}'s state machine is analogous, only the update of the used range must be adapted.

![State machine wrapping `recvmmsg(2)`{.manpage}](tikz/atomic-recv.tex){#fig:atomic-recv}

![After I/O, the active range is encoded in `state`.](tikz/ATOMIC_SWAP.tex){#fig:atomic-swap}

The I/O operation is split into the following blocks, when traversing between block `state`{.c} is updated through atomic CPU instructions:

 1. Initialize the field in which the number of bytes will be returned with a sentinel value, e.g., `-1`, that will never be returned by the syscall.
    As long as the sentinel value is still present, the syscall will not yet have returned and the data is still available in the kernel buffers.
    When the sentinel value is no longer present the next block will have completed.
 2. Execute the syscall itself.
    This will overwrite the sentinel value, atomically activating the next block.
 3. Encode the currently active range in the state.
    This ensures that the next state will always start with the same active and inactive range, even when the transaction is interrupted and later resumed.
 4. Append the number of bytes to the used length of the ring buffer.
    Before this, the index of the currently active range is restored, so that the number of bytes is only ever appended once and the scenario, shown in \cref{fig:double-update}, where the update is applied multiple times is averted.

:::{.figure #fig:double-update}
```c
/* Initial run */
ranges[!active] := append_bytes(ranges[active])
active := !active
/* CRASH before `state` is updated */

/* Rerun from `state == ATOMIC_SWAP` */
ranges[!active] := append_bytes(ranges[active])
active := !active
state := ATOMIC_INIT
```

:::{.label}
Double update
:::
:::

One of the state changes is performed by the `recvmmsg(2)`{.manpage} syscall itself by overwriting a sentinel value.
Syscalls are not a single atomic CPU instruction and can be interrupted by signals, but the I/O operation is nevertheless atomic in respect to signals, including `SIGKILL`{.c}.

Syscalls will check for pending signals deliberately or automatically during a wait with `TASK_INTERRUPTIBLE`{.c} [@linux_wait_event_interruptible] and subsequently interrupt the syscall.
The kernel will then call the signal handler or perform the signal's default operation when it returns to userspace.
`recvmmsg(2)`{.manpage} or `sendmmsg(2)`{.manpage} may be interrupted by a signal and terminate the process, but, as seen in \cref{linux-recvmmsg}, once an individual `recvmsg(2)`{.manpage} or `sendmsg(2)`{.manpage} operation was performed successfully, its return value will be written to the `struct mmsghdr`{.c} before any signals will be processed.

:::{.figure #linux-recvmmsg}
```c
err = ___sys_recvmsg(sock,
        (struct user_msghdr __user *)entry,
        &msg_sys, flags & ~MSG_WAITFORONE,
        datagrams);
if (err < 0)
    break;
err = put_user(err, &entry->msg_len);
```

:::{.label}
[Linux 7.2 implementation of `recvmmsg(2)`{.manpage}](https://github.com/torvalds/linux/blob/v7.2/net/socket.c#L3041-L3047)
:::
:::

### Transformation

While connections are processed, received data will be transformed by `transform()`{.c}.
Arbitrarily complex transformations can be implemented in `transform()`{.c} and it can save its state, or any arbitrary data, in a context alongside the connection.

Each call to `transform()`{.c} is performed as a single transaction on the five buffers shown in \cref{fig:transform}.
Input and output buffers are `struct atomic_ring_buffer`{.c} from \cref{fig:atomic_ring_buffer}
`transform()`{.c} consumes data from the start of input buffers and appends to output buffers.
Data already written to the output buffers must not be modified, only newly appended data will be part of the transaction.
The context must be stored as a double-buffered object, so that changes to it are part of the transactions and are only committed if the transaction completes.
The steps of the transaction surrounding `transform()`{.c} are the following:

 1. All active copies of double-buffered objects are copied to inactive copies.
 1. `transform()`{.c} operates on the inactive copies.
    This ensures the copies active at the start of the transaction are never modified and the transaction will be restarted with the same initial state.
 1. Swap all active and inactive ranges atomically.
    When only some of the double-buffered objects are swapped, the consistency property would be violated and restarting the transaction would use a different initial state.

![State machine wrapping `transform()`{.manpage}](tikz/atomic-transform.tex){#fig:atomic-transform}

After `transform()`{.c} returns, the active copy of all double-buffered objects must be *swapped* atomically, i.e., all objects are *swapped over* or none at all, so that consistency is observed.
To commit all five buffers atomically the working copies used by `transform()`{.c} are atomically encoded in the connection's state, just as in \fullref{impl:transactional-io}.
Once in the `CONN_SWAP_BUFFERS`{.c} state, the state machine will only progress once all buffers are updated as encoded in the state, as seen in \cref{fig:conn-swap-buffers}.

![After transformation, buffers are encoded in `state`.](tikz/CONN_SWAP_BUFFERS.tex){#fig:conn-swap-buffers}

### HTTP Parser

To parse the HTTP protocol an existing library was used instead of implementing a custom parser.
The parser must be stateless or its state must be updated as part of the transformation's transaction.

At first Node.js' HTTP library *llhttp* [@llhttp] was evaluated.
This library however is unsuitable, the parser stores function and data pointers in its state.
Since the shared memory needs to be remapped frequently, e.g. when its size changes, the address of the mapping is not guaranteed to be stable, invalidating any pointers to data in the shared memory.
Although the caller can request the operating system to not move a memory mapping, it is not guaranteed to be possible.
Additionally, if the *worker* process is restarted, the new mapping would have to be created at the same address as in the previous process, which may not be possible.

Function pointers pose a similar risk, albeit only during restarts of the *worker* process.
When a process is started the address of library functions is often randomized by *address space layout randomization*.
So when a new *worker* process is started function pointers to methods of *llhttp* in the old process will no longer be valid.
Pointers could potentially be manipulated during recovery but this would require great understanding of the internals of *llhttp* and the effort was deemed too large and *llhttp* was discarded.

Instead the much simpler HTTP/1.1 parser *picohttpparser* [@picohttpparser] was chosen, since it is in large parts stateless.
It however requires the complete HTTP request or response headers to be loaded in memory and cannot incrementally parse incoming HTTP headers.
This imposes some minor restrictions on the implementation.

## Connection Termination

Once connection processing is concluded, the *worker* and *listener* must close their file descriptors to terminate the connection.
Normally, the *worker* will first change the connection's state, so termination can be performed during recovery; closes its file descriptors; and then messages the *active listener* to close its file descriptors as well and release the `struct connection`{.c}.
This flow is shown in \cref{fig:ipc-close}.

![Distributed state machine during connection termination](tikz/ipc-close.tex){#fig:ipc-close}

In the case of an error during connection establishment, as briefly shown in \cref{ipc-msgs}, the *listener* may initiate the termination of the connection instead.
In case of an error, instead of cleanly terminating the TCP connections with a `FIN` packet, the proxy instead sends a TCP `RST` packet.
This is done by setting the `SO_LINGER` option with a linger timeout of zero with `setsockopt(2)`{.manpage}.
Then, queued messages will no longer be sent over the connection, once the connection is closed, instead the connection is aborted.

## Recovery {#impl:recovery}

When a process of the proxy terminates, it is restarted and recovery sets in to continue operating.
Depending on which process terminated and the state of each connection, recovery is performed using a specific strategy.

### Worker Recovery

When the *worker* process terminates, the *active listener* can easily discern all `struct connection`{.c} belonging to the *worker* process, due to its `pid_t worker_pid`{.c} tag.
Depending on the state of each `struct connection`{.c}, recovery will be handled differently:

##### `state == CONN_CLOSING`{.c} {.unnumbered}

The *worker* terminated while in the process of closing the connection.
The *active listener* can simply close the connection itself, instead of passing it to a *worker* process.

##### `state == CONN_ACCEPTING`{.c} {.unnumbered}

The *worker* terminated while receiving a new incoming connection.
The connection will not have been processed in any way and the IPC `accept` message can be resent.

##### `state == CONN_POLL || state & 0xFF == CONN_SWAP_BUFFERS`{.c} {.unnumbered #recover-conn-poll}

The connection was fully established.
The *active listener* will hand over both the connection's file descriptors to a *worker* process.
This is handled through a separate IPC flow, seen in \cref{fig:recover-conn-poll}.
Interrupted I/O or transformation transactions are resumed.

![Recovery of an established connection](tikz/ipc-orphan.tex){#fig:recover-conn-poll}

##### `state == CONN_CONNECTING`{.c} {.unnumbered}

The *worker* terminated just before requesting the *active listener* to create an outgoing connection or while awaiting the connection.
If the *active listener* received the IPC `connect` message, the outgoing connection will be created, the connection's state will be updated to `CONN_POLL`{.c}, and the connection will be passed to the *worker* just as with `state == CONN_POLL`{.c}.
If the IPC message was not sent, the *active listener* will nevertheless send the incoming connection through the recovery IPC flow.
The *worker* will receive a connection that is already in `state == CONN_CONNECTING`{.c} and will resend the IPC `connect` message, as seen in \cref{fig:recover-conn-connecting}, thereby restoring the original control flow.

![Recovery of interrupted `connect(2)`{.manpage} (abridged)](tikz/ipc-connect-recover.tex){#fig:recover-conn-connecting}

### Listener Recovery

When the *active listener* terminates, its file descriptors will be saved by the *passive listener*.
In its current implementation, all *worker* processes will be killed and restarted, so that recovery of a connection does not need to take the individual state of its *workers* into account.
This, however, is entirely optional and only allows for a less elaborate recovery.
Nevertheless, recovery of connections will be performed depending on their `state`.

##### `state == CONN_CLOSING`{.c} {.unnumbered}

Just as with the *worker* terminating, the *active listener* will simply close the connection itself.

##### `state == CONN_ACCEPTING`{.c} {.unnumbered}

The previous *active listener* crashed while receiving a new incoming connection; before, during, or right after `accept(2)`{.manpage}.
If it crashed before `accept(2)`{.manpage} completed, the connection's file descriptor still remains in the listening socket's queue of incoming connections.
The `struct connection`{.c} can be released and `accept(2)`{.manpage} will later return the connection again.
If the process crashed after a successful `accept(2)`{.manpage} and the file descriptor was saved, e.g., while copying the file descriptor to a *worker*, the IPC `accept` message can simply be resent to a *worker* to hand-over the recovered connection.
The remaining unlikely case that `accept(2)`{.manpage} completed and a file descriptor was created, but the process crashed before it was saved to shared memory, is discussed in \fullref{forgotten-file-descriptor}.

![Recovery of interrupted `accept(2)`{.manpage} (abridged)](tikz/ipc-accept-recover.tex){#fig:recover-conn-accept}

##### `state == CONN_CONNECTING`{.c} {.unnumbered}

A crash occurred while establishing an upstream connection.
Conceptually this case is quite similar to `state == CONN_ACCEPTING`{.c} in that file descriptors have to be recovered.
The file descriptor is created with the `FD_CLOEXEC`{.c} flag set and the flag is only removed once the socket is connected.
Any file descriptors with `FD_CLOEXEC`{.c} are closed during recovery, thus only fully connected sockets will be recovered.
This can, however, lead to the case that a newly connected socket may be lost, because the *active listener* terminated just before the flag was removed.
However, newly created outgoing connections are deemed non-essential and can be recreated easily.
Depending on the point of the interruption, the *worker* may need to retransmit the upstream host's address to the *active listener*.

##### `state == CONN_POLL || state & 0xFF == CONN_SWAP_BUFFERS`{.c} {.unnumbered}

The connection was fully established and will be handed over as described in \fullref{recover-conn-poll}.

### Forgotten File Descriptor

:::{.figure #fig:lost-accept}
```c
struct shared_memory *shared;

int ax = accept(listen_fd, NULL, 0);
/* CRASH: The return value is lost. */
shared->fd = ax;
```

:::{.label}
*Forgotten* file descriptor
:::
:::

If the *active listener* crashes immediately after it created a file descriptor, but before it is saved in shared memory, as seen in \cref{fig:lost-accept}, the file descriptor will have been created and will exist in the file descriptor table shared with the *passive listener* but will have been *forgotten*, just like return values in \fullref{transactional-io}.

Luckily a process' file descriptors are also accessible through the `proc(5)`{.manpage} pseudo-filesystem and `proc_pid_fd(5)`{.manpage}.
Therefore, a process can iterate over its open file descriptors by iterating over the entries in the `/proc/self/fd` directory.
If the directory contains a file descriptor that is unknown to the *listener* and a connection with `state == CONN_ACCEPTING`{.c} or `state == CONN_CONNECTING`{.c} exists, the *forgotten* file descriptor can be recovered.

This, unfortunately, can only recover a single file descriptor.
If multiple *forgotten* file descriptors are found, then it can no longer unambiguously discern if the file descriptor is a downstream or upstream connection, or to which `struct connection`{.c} it belongs.
So a *listener* may only ever perform one `accept(2)`{.manpage} or `socket(2)`{.manpage} and `connect(2)`{.manpage} at a time.

Multiple incoming connections can potentially be recovered in parallel by comparing the socket's peer address, retrieved with `getpeername(2)`{.manpage}, with the address stored by `accept(2)`{.manpage} in `struct connection`{.c}.
However this is not implemented.

## Error Injection

To evaluate the recovery of the proxy it is necessary to induce process crashes at exact points of the execution.
While it might be possible to predict the state of the proxy from external observation and then induce an error by killing a process, this is not always feasible.
Especially the errors of \fullref{forgotten-file-descriptor} are subject to very precise timing.

:::{.figure #libcrash}
```c
void libcrash_atomic_recv_recvmmsg_post(
        int fd,
        struct atomic_ring_buffer *buf,
        int *rc);
/* ... */
int rc = recvmmsg(fd, &buf->mm, 1, MSG_DONTWAIT, NULL);
libcrash_atomic_recv_recvmmsg_post(fd, buf, &rc);
```

:::{.label}
Definition and usage of a *libcrash* callback
:::
:::

Error injection is thus implemented in process through the dynamic library *libcrash*.
*libcrash* implements callbacks that will be called by the proxy and can then induce a crash in the proxy, as seen in \cref{libcrash}.
By externalizing the error injection into a dynamic library, the strategy of error injection can be easily changed without recompiling the proxy.

The most useful implementation of *libcrash* allows to externally induce errors at exact points of the execution with `sigqueue(3)`{.manpage}:

 1. The signal `SIGUSR2`{.c} is masked by the processes loading `libcrash.so`.
 1. Each call to a *libcrash* callback checks for a pending `SIGUSR2`{.c} through `sigtimedwait(2)`{.manpage} and stores its `sival_int`{.c}.
 1. If the current callback was selected through `sival_int`{.c} *libcrash* will call `_exit(2)`{.manpage}, unconditionally terminating the calling process.
    Otherwise the callback will return normally.

*libcrash* callbacks will be called relatively frequently, but with a single non-blocking syscall to `sigtimedwait(2)`{.manpage} this implementation is relatively performant.

With this variant of *libcrash*, external tests can induce an error at an exact point of execution the next time it will be reached.
