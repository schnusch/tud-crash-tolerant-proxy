# Implementation

:::{.hidden}
* Impl-Details. begründen
* Impl-State Machine
* epoll mit SIGIO
:::

C was chosen as the programming language to implement the proxy, mostly due to its widespread use in systems programming and familiarity.
While it does not offer any memory safety or high-level abstraction such as coroutines nativly, it offers great control over memory management and operating system interfaces.
In the future memory safety could potentially be retrofitted with Fil-C [@fil-c].

In its current implementation the proxy approaches 6000 lines of code with an almost even split between *listener* and *worker*.

## Passive Listener

The *passive listener* is by far the simplest of the proxies processes and shares little functionality with the *active listener* or *worker*.
Its control flow is summed up in \autoref{state-machine-passive}.

![Main loop of the *passive listener*](tikz/automata-passive.tex){#state-machine-passive}

As the initial process the *passive listener* will initialize listening socket, shared memory, and IPC sockets later used by all processes.
A memory file descriptor, short *memfd*, i.e. an anonymous file residing in memory instead of the filesystem, is created, this file is later mapped into the processes' address space and backs the shared memory.

It will then call `clone3(2)`{.manpage} to spawn the *active listener* and suspend until the process receives a signal.
`clone3(2)`{.manpage} is chosen over `clone(2)`{.manpage}, even though it is not exposed by the GNU C Library [@glibc-clone3], because it is more ergonomical and in particular does not require the caller to manually allocate a stack for the child process.
Sadly `clone3(2)`{.manpage} is not supported by Valgrind [@kde-420906] and its *memcheck* tool can no longer be used to debug the *listener*.

Signals are received and processed in a loop.
Instead of conventional asynchronous signal handler the process will poll for signals synchronously through `sigwait(3)`{.manpage}.
This avoid issues with the signal handlers preemptive control flow and `signal-safety(7)`{.manpage} in general.
While most signals are ignored, some keep their default action, either because they represent exceptional process conditions or because their default behavior should not be altered.
Selected signals are handled explicitly:

`SIGINT`
: This signal was picked to terminate the proxy. If received the *passive listener* will not restart the *active listener* but instead cease operations. This is also the signal that is generated when the user presses `Ctrl+C` on a terminal.

`SIGCHLD`
: If a child process terminates the operating system will send this signal to its parent process, as is the case if the *active listener* terminates where the *passive listener* will be signaled.

When the *active listner* terminates the *passive listner* will restart it.
But first It will try to re-execute its binary, which resets the process and discard all unintentionally collected state, but active connections, shared memory, listening sockets, and IPC sockets are passed to the new executable.
If the new executable cannot be executed the current executable will restart the *active listener* it itself.

## Common Components

Since the *active listener* and *worker* process both access shared memory and communicate through IPC they share some common components.

### Shared Memory

*listener* and *worker* share connection state through a memory region backed by a *memfd*.
Each process uses `mmap(2)`{.manpage} to create a shared mapping [@map-shared] of the *memfd* in its address space.
The layout of the shared memory is a simple `struct`{.c}, whose definitions are shown in \autoref{shmem-layout}, with the following fields:

`struct connection connections[]`{.c}
: The array contains the complete state of each connection of the proxy. In the current implementation this is an array of fixed size elements, meaning data stored per connection must be of fixed size.

`atomic_size_t size`{.c}
: This field contains the length of the complete *memfd* including the `size` field itself. When a new connection is appended to the array, the size of the *memfd* will be updated and other processes can in turn update their mappings. Since multiple process can operate on this field simultaneously it must support atomic operations.

:::{.figure #shmem-layout}
```c
struct shared_memory {
    atomic_size_t size;
    struct connection connections[];
};
```

:::{.label}
Layout of the shared memory
:::
:::

Fields of the connection state `struct connection`{.c} will be explained in detail when needed, but an already important field is `atomic_int state`{.c}.
This field store the possible states the connections state machine or `CONN_UNUSED` if the item of the `connections`{.c} array is not currently in use.
When the *listener* accepts a new connection it will pick an item with `CONN_UNUSED` or append a new `struct connection`{.c} to the *memfd*.

Each process tracks its shared memory mapping with a `struct`{.c} from \autoref{shmem-map}.
This stores the following values:

`int fd`{.c}
: The *memfd* is needed to create and resize the mapping. It is stored alongside the mapping.

`struct shared_memory *addr`{.c}
: The pointer to the mapped memory region.

`size_t length`{.c}
: The number of bytes currently mapped in the process. This can differ from `size` field in `struct shared_memory`{.c} if new connections were appended or the *memfd* was truncated the mapping will need to be updated.

:::{.figure #shmem-map}
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

#### Double-Buffering

Any operations on shared memory must satisfy the properties outlined in \cref{shared-memory}.
Elementary types are directly supported by C11 [@iso9899-2011-n1570] but more complex type such as arrays or structs must be implemented on top of elemtentary types.

:::{.figure #double-buffer}
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

Operations on any more advanced object can be performed atomically through a wrapper.
The wrapper stores two copies of the object and atomically switches between the copies.
In \autoref{double-buffer} `active` is the index of `copies`.
Now any write operation reads from `copies[active]`, writes to `copies[!active]`, and then toggles `active` atomically.
This will only work for a single writer, since concurrent writers will operate on and potentially corrupt the same copy `copies[!active]`.
In most cases, however, this is sufficient, as transactions primarily serve to prevent partial writes rather than to synchronize competing writes.

The *worker* will use double-buffering to atomically perform receiving, sending, and transformation.

### IPC

**TODO** all messages

why signalfd? syscalls in sighandler, signal safety

## Inter-process Communication

### Shared Memory

### Shared File Descriptors

## Listener Process Pair

**TODO** why?

### Error cases

### `CLONE_FILES`

`clone(2)`{.manpage}

### Recovery

#### Open Connections

#### Forgotten File Descriptors

```sh
ls -ahlp /proc/self/fd
```

## Worker Process

### Error cases

### Atomicity and consistentency

#### `send(2)`{.manpage} and `recv(2)`{.manpage}

~~**TODO** explain **AC**ID~~ nur Referenz, viell. doch nochmal als Erinnerung

**TODO** syscalls are atomic

To communicate over sockets the syscalls of the `send(2)`{.manpage}- and `recv(2)`{.manpage}-family are generally used. During the `recv(2)`{.manpage} syscall the kernel will copy data from the kernel's TCP buffer into a user supplied buffer and return the number of bytes written to said buffer. The buffer passed to `recv(2)`{.manpage} can reside in shared memory so that the memory is not lost if the process crashes. The kernel will write the received data directly to the shared memory, but the return value will be passed on the stack or in a register.^[**TODO** something about calling conventions] If the program were to crash after the syscall returned but before that return value is copied to shared memory as well it will be lost. During recovery, while the received data will still be available, the length of that data will no longer be known, rendering the received data unusable.

The same limits apply for `send(2)`{.manpage} which will send the bytes from its buffer but with the same unfortunately timed crash the number of bytes sent over the socket would be lost. During recovery it would not be known how much data was sent already and could now be discarded.

`sendmsg(2)`{.manpage} and `recvmsg(2)`{.manpage} exist as extensions of `send(2)`{.manpage} and `recv(2)`{.manpage} respectively. They allow the caller to read from or write to fragmented memory locations using `struct iovec`{.c} in a *scatter/gather* fashion. Additionally depending on the socket's underlying protocol ancillary data can be sent or received. Unfortunately their interface is similar to `send(2)`{.manpage} and `recv(2)`{.manpage} and as such return the number of bytes sent or received on the stack or in a register as well. Thus the same limitations apply.

The `sendmmsg(2)`{.manpage} and `recvmmsg(2)`{.manpage} syscalls offer a different interface. These syscalls are intended to perform multiple `sendmsg(2)`{.manpage} and `recvmsg(2)`{.manpage} calls sequentially without repeatedly changing between user and kernel space.

::: {.figure #sendmmsg_recvmmsg}
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

Definitions of `sendmmsg(2)`{.manpage} and `recvmmsg(2)`{.manpage}
:::

`sendmmsg(2)`{.manpage} or `recvmmsg(2)`{.manpage} each operate on an array of `struct mmsghdr`{.c}, see <#sendmmsg_recvmmsg>. For each `struct mmsghdr`{.c} the operations of `sendmsg(2)`{.manpage} or `recvmsg(2)`{.manpage} will be performed. The value that `sendmsg(2)`{.manpage} or `recvmsg(2)`{.manpage} would return is written to the field `msg_len`{.c}. By placing a `struct mmsghdr`{.c} in shared memory the number of bytes sent or received will now be written to the shared memory directly. Now if the program were to crash immediately after `sendmmsg(2)`{.manpage} or `recvmmsg(2)`{.manpage} the number of bytes sent or received will no longer be lost. With these syscalls, together with carefully crafted state machines, send and receive routines were created, that can recover from crashes anywhere but inside the syscalls themselves.

#### Transformation

**TODO**

![Connection state: `CONN_SWAP_BUFFERS`](tikz/CONN_SWAP_BUFFERS.tex)
