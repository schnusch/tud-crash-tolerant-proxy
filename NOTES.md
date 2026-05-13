# Notes

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

<div style="break-after: page;"></div>

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
