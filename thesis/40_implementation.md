# Implementation

* Impl-Details. begründen
* Impl-State Machine
* epoll mit SIGIO

## Signal handlers

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
