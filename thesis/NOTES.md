# 2026-05-13

## Prior Art

* [`TCP_REPAIR`](https://lwn.net/Articles/495304/)
* [CRIU](https://criu.org/)
  * [TCP connection](https://criu.org/TCP_connection)
    * [libsoccr](https://criu.org/Libsoccr) uses [`TCP_REPAIR`](https://github.com/checkpoint-restore/criu/blob/v4.2/soccr/soccr.c#L124-L146)
  * used by [crun](https://github.com/containers/crun/blob/1.27.1/src/libcrun/criu.c#L835) 
    ([`do_dump_one_inet_fd`](https://github.com/checkpoint-restore/criu/blob/v4.2/criu/sk-inet.c#L605))
* [DMTCP](https://github.com/dmtcp/dmtcp):
  [paper](https://dmtcp.sourceforge.io/papers/dmtcp.pdf),
  [publications](https://dmtcp.sourceforge.io/publications.html)
  * `$LD_PRELOAD`
  * injects thread into every process
  * snapshot of sockets between snapshotted processes
    * sockets are drained on snapshot
    * sockets are prefilled on resume
  * external sockets not supported
* [DejaVu](https://www.cecs.uci.edu/~papers/ipdps07/pdfs/IPDPS-1569010987-paper-1.pdf)
  * seems similar to DMTCP
  * not much info
* HTCondor
  * [checkpointing](https://htcondor.org/checkpointing.html) and host migration
* write-ahead log like database engines
  * difficult with network I/O
* VM migration: e.g. Xen [Remus](https://wiki.xenproject.org/wiki/Remus)
* OpenVz (and other old methods): in kernel
* Apache2/[nginx](https://github.com/nginx/nginx/blob/release-1.30.0/src/os/unix/ngx_process_cycle.c#L73-L275)/haproxy
  * no upgrade/re-exec during connection handling
  * workers finish their [`accept(2)`](https://man7.org/linux/man-pages/man2/accept.2.html)-ed
    requests and exit
  * new workers are spawned
* [systemd fdstore](https://systemd.io/FILE_DESCRIPTOR_STORE/)

## [TU Dresden/Software Fault Tolerance](https://tu-dresden.de/ing/informatik/sya/se/studium/lehrveranstaltungen/summer-semester/SFT?set_language=en)

* checkpointing/rejuvenation
  * rejuvenation: loose all state
  * checkpointing: save (all) state
  * frequency?
  * [`fork(2)`](https://man7.org/linux/man-pages/man2/fork.2.html) as checkpointing insufficient
    * new incoming connections, lost updates (see `08-SFT-Mechanisms2.pdf#page=20`)
    * read/written network traffic
  * (Apache uses rejuvenation (see `08-SFT-Mechanisms2.pdf#page=42`))
  * micro-reboots
    * restart components separately
    * applicable?
* process-pair
  * primary/leader/worker and backup/follower/master (farmer)
  * [`sendmsg(2)`](https://man7.org/linux/man-pages/man2/sendmsg.2.html)/[`recvmsg(2)`](https://man7.org/linux/man-pages/man2/recvmsg.2.html)
    file descriptors
  * pro
    * resistent to `SIGKILL`
    * safe from syscalls like [`_exit(2)`](https://man7.org/linux/man-pages/man2/_exit.2.html) in library
    * passive follower (no I/O), smaller attack surface
    * distinct executable: follower can be linked differently
    * (re-)[`exec(3p)`](https://man7.org/linux/man-pages/man3/exec.3p.html):
      safe from some memory leaks (outside of shared memory)
  * contra
    * complex
    * problems with pointers in shared memory
      * shared memory at fixed address
      * all objects relocatable
      * ASLR?
* farmer/master and workers
  * same as process pairs
  * track worker process of the passed file descriptors
* single process
  * pros/cons see process-pair
  * "simply" serialize state, re-exec, and restore state

## Architecture

* process-pairs
  * atomic read/write buffer length
    * [recvmmsg(2)](https://man7.org/linux/man-pages/man2/recvmmsg.2.html)
      ([musl](https://git.musl-libc.org/cgit/musl/tree/src/network/recvmmsg.c#n37))
    * [sendmmsg(2)](https://man7.org/linux/man-pages/man2/sendmmsg.2.html)
      ([musl](https://git.musl-libc.org/cgit/musl/tree/src/network/sendmmsg.c#n28))
  * "atomic" [`accept(2)`](https://man7.org/linux/man-pages/man2/accept.2.html)
    * ~~[`TCP_SAVE_SYN`](https://man7.org/linux/man-pages/man7/tcp.7.html) (since Linux 4.3)~~

      > ~~Saves incoming SYN packet contents of the listening socket until it is read with `TCP_SAVED_SYN` once. Could be set before or after the [`listen(2)`](https://man7.org/linux/man-pages/man2/listen.2.html) call.~~

      does not delay `SYN-ACK`
    * `SYN-ACK` is sent before [`accept(2)`](https://man7.org/linux/man-pages/man2/accept.2.html)
    * [`close(2)`](https://man7.org/linux/man-pages/man2/close.2.html) before [`accept(2)`](https://man7.org/linux/man-pages/man2/accept.2.html)
      sends `RST`
    * [`SO_LINGER`](https://ndeepak.com/posts/2016-10-21-tcprst/):
      abort connection (`RST` instead of `FIN`)
      * not necessary
    * see [below](#accept2--connect2)
  * maybe `$LD_PRELOAD` for [`accept(2)`](https://man7.org/linux/man-pages/man2/accept.2.html)
    and [`socket(2)`](https://man7.org/linux/man-pages/man2/socket.2.html)
  * user-space TCP?
* [picohttpparser](https://github.com/h2o/picohttpparser)
  * used in Perl
  * relocatable

### IPC

#### `accept(2)` / `connect(2)`

<table><thead><tr>
<th>Farmer</th><th>Worker</th>
</tr></thead><tbody><tr><td>

```c
////////////////////////////////////////

struct shared_memory *shared;














////////////////////////////////////////

// CRASH: connection remains in worker

/* struct msghdr */
recvmsg(...);

int fd = /* ... */;
// CRASH: connection remains in worker
shared->connections[i].fd[0] = fd;

////////////////////////////////////////
```

</td><td>

```c
////////////////////////////////////////

struct shared_memory *shared;

// CRASH: connection is still pending

int fd = accept(...);
// CRASH: connection is lost
shared->connections[i].fd[1] = fd;

// CRASH: connection is lost

/* struct msghdr */
sendmsg("newfd slot=$i ...", fd=fd)

// CRASH: connection remains in farmer

////////////////////////////////////////










////////////////////////////////////////
```

</td></tr></tbody></table>

#### `close(2)`

<table><thead><tr>
<th>Farmer</th><th>Worker</th>
</tr></thead><tbody><tr><td>

```c
////////////////////////////////////////

struct shared_memory *shared;












////////////////////////////////////////

/* struct msghdr */
recvmsg(...);

close(shared->connections[i].fd[0]);

////////////////////////////////////////
```

</td><td>

```c
////////////////////////////////////////

struct shared_memory *shared;

if (shared->connections[i].state == CLOSED)
    return;

shared->connections[i].state = CLOSED;
// CRASH: skip FD on respawn (e.g. FD_CLOEXEC)

close(shared->connections[i].fd[1]);

/* struct msghdr */
sendmsg("close slot=$i ...", fd=fd)

////////////////////////////////////////






////////////////////////////////////////
```

</td></tr></tbody></table>

# 2026-05-26

## Architecture

* [`CLONE_FILES`](https://man7.org/linux/man-pages/man2/clone.2.html)
  * example: [`playground/clone_files.c`](./playground/clone_files.c)
    * `open(2)` after `exec(3)` unless `OPEN_BEFORE_EXEC` is defined
  * [`execve(2)`](https://man7.org/linux/man-pages/man2/execve.2.html):

    > The file descriptor table is unshared, undoing the effect of the `CLONE_FILES` flag of [`clone(2)`](https://man7.org/linux/man-pages/man2/clone.2.html).

### Listener

```{.dot}
digraph {
  subgraph cluster {
    label="CLONE_FILES";
    keeper [label="Keeper"];
    listener1 [label="Listener 1"];
    listener2 [label="Listener 2"];
    listener3 [label="..."];
    keeper -> listener1 [label="clone(2)"];
    keeper -> listener2 [label="clone(2)"];
    keeper -> listener3;
  }

  worker1 [label="Worker"];
  worker2 [label="Worker"];
  worker3 [label="..."]
  worker4 [label="..."]
  listener1 -> worker1 [label="exec(3)"];
  listener2 -> worker2 [label="exec(3)"];
  listener1 -> worker3;
  listener2 -> worker4;

  listener1 -> listener1 [label="1. accept(2)",style=dashed];
  listener1 -> worker1 [label="2. incoming\nconnection",style=dashed];

  worker2 -> listener2 [label="1. connect\nto...",style=dashed];
  listener2 -> listener2 [label="2. connect(2)",style=dashed];
  listener2 -> worker2 [label="3. outgoing\nconnection",style=dashed];
}
```

* *Worker* is separated executable from *Keeper*/*Listener*
* crash after `accept(2)` in *Listener*:
  * ```c
    int fd = accept(...);
    // CRASH
    shared->connections[i].fd = fd;
    ```
  * recovery:

    ```c
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    while(e = readdir(d)) {
      if(!is_known_fd(e)) {
        // file descriptor "forgotten" after accept(2)
        shared->connections[i].fd = atoi(e->d_name);
      }
    }
    ```
  * multiple file descriptors cannot be recovered properly
  * **locking around `accept(2)`**
* `connect(2)`
  * can be dropped?
* *Keeper* or *Listener* can die
  * no state lost
  * (new) *Keeper* starts new *Listeners*
    * maybe re-`exec(2)`
* *Worker* dies
  * *Listener* just restarts it with its file descriptors

### Master-Workers

```{.dot}
digraph {
  master [label="Master"];

  subgraph cluster_1 {
    label="CLONE_FILES";
    backup1 [label="Backup 1"];
    worker1 [label="Worker 1"];
    backup1 -> worker1 [label="clone(2)"];
  }
  master -> backup1 [label="exec(3)"];

  subgraph cluster_2 {
    label="CLONE_FILES";
    backup2 [label="Backup 2"];
    worker2 [label="Worker 2"];
    backup2 -> worker2 [label="clone(2)"];
  }
  master -> backup2 [label="exec(3)"];

  backup1 -> backup2 [constraint=false,dir=both,style=dashed,label="shared\nlistening\nsockets"];

  worker1 -> worker1 [label="accept(2)",style=dashed];

  worker2 -> worker2 [label="connect(2)",style=dashed];
}
```

* *Backup* and *Worker* must be same executable
* crash after `accept(2)` in *Worker*:
  * recovery like in [Listener](#listener)
  * single-threaded *Worker*
    * at most only one new file descriptor
    * still parallel `accept(2)`
* Worker-Backup-process-pair might die
  * connections of that process pair will be lost
    * **easy**
  * `sendmsg(2)` file descriptors to Master?
* Master dies?
  * **TODO**

### Multiple Services

```{.dot}
digraph {
  rankdir=TD;
  subgraph cluster {
    label="listener.service";

    backup [label="Backup"];
    listener1 [label="Listener"];
    listener2 [label="Listener"];
    listener3 [label="Listener"];
    listener4 [label="Listener"];

    backup -> listener1 [taillabel="clone(CLONE_FILES)"];
    listener1 -> listener2 [style=dashed];
    listener2 -> listener3 [style=dashed];
    listener3 -> listener4 [style=dashed];
  }

  subgraph cluster_1 {
    label="worker.service";

    worker1 [label="Worker"];
    worker2 [label="Worker"];
    worker3 [label="Worker"];
    worker4 [label="Worker"];
    worker5 [label="Worker"];
    worker6 [label="Worker"];
    worker1 -> worker2 [style=dashed];
    worker2 -> worker3 [style=dashed];
    worker3 -> worker4 [style=dashed];
    worker4 -> worker5 [style=dashed];
    worker5 -> worker6 [style=dashed,label="only now\nuse the\nconnection"];
  }

  systemd -> backup;
  systemd -> worker1;

  worker1 -> listener1 [constraint=false,taillabel="connect(\"/run/.../socket\")"];
  listener1 -> worker2 [constraint=false,label="file descriptors,\nshared memory"];
  listener2 -> listener2 [constraint=false,label="accept(listen_fd)"];
  listener3 -> worker3 [constraint=false,label="accepted\nfile descriptor"];
  worker4 -> worker4 [constraint=false,label="connect(...)"];
  worker5 -> listener4 [constraint=false,label="backup\nfile descriptor"];
}
```

* separate systemd services
  * `keeper.service`
    * process pairs
    * single-threaded or locking around `accept(2)`/`recvmsg(2)`
    * `accept(2)` incoming connection, dispatch to worker(s)
  * `worker.service`
    * can crash/be restarted without state loss
    * state kept by keeper
    * unsafe `connect(2)`, will be redone

### Transformation

* ```c
  struct buffer {
    char buf[];
    size_t len;
  }

  struct {
    int state;
    struct buffer downstream_rx;
    struct buffer downstream_tx;
    struct buffer upstream_rx;
    struct buffer upstream_tx;
  };

  struct mmsghdr msg;
  switch(state) {

  case PRE_RECV:
    msg = {
      .msg_hdr = /* ...downstream_rx... */;
      .msg_len = -1;
    };
    state = RECV;

  case RECV:
    if(msg.msg_len == -1) {
      recvmmsg(fd, &msg, 1, ...);
    }
    state = TRANSFORM;

  case TRANSFORM:
    transform(&downstream_rx, &upstream_tx)
    state = PRE_SEND;

  case PRE_SEND:
    msg = {
      .msg_hdr = /* ...upstream_tx... */;
      .msg_len = -1;
    };
    state = SEND;

  case SEND:
    if(msg.msg_len == -1) {
      sendmmsg(fd, &msg, 1, ...);
    }
    state = START;

  }
  ```
* ```{.dot}
  digraph {
    {
      rank=same;
      pre_recv [label="msg.msg_len := -1\lstate := RECV"];
      post_send [label="state := PRE_RECV"];
    }
    {
      rank=same;
      recv [label="recvmmsg(fd, &msg, ...)"];
      send [label="sendmmsg(fd, &msg, ...)"];
    }
    {
      rank=same;
      post_recv [label="state := TRANSFORM"];
      pre_send [label="msg.msg_len := -1\lstate := SEND"];
    }
    {
      rank=same;
      transform [label="transform(&downstream_rx,\r&upstream_tx)"];
      post_transform [label="state := PRE_SEND"];
    }

    pre_recv -> recv;
    recv -> post_recv;
    post_recv -> transform;
    transform -> post_transform;
    post_transform -> pre_send;
    pre_send -> send;
    send -> post_send;
    post_send -> pre_recv [constraint=false];

    start_init [shape=point];
    start_init -> pre_recv [label="state == PRE_RECV"];

    start_recv [shape=point];
    start_recv -> recv [style=dashed,label="state == RECV &&\nmsg.msg_len == -1"];

    start_post_recv [shape=point];
    start_post_recv -> post_recv [style=dashed,label="state == RECV &&\nmsg.msg_len != -1"];

    start_transform [shape=point];
    start_transform -> transform [style=dashed,label="state == TRANSFORM"];

    start_pre_send [shape=point];
    start_pre_send -> pre_send [style=dashed,label="state == PRE_SEND"];

    start_send [shape=point];
    start_send -> send [style=dashed,label="state == SEND &&\nmsg.msg_len == -1"];

    start_post_send [shape=point];
    start_post_send -> post_send [style=dashed,label="state == SEND &&\nmsg.msg_len != -1"];
  }
  ```
