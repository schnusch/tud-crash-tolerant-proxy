# Conclusions

This work investigated the feasibility of a crash-tolerant proxy, implemented entirely in userspace, running on a single host.
The primary design goal was to persist connections across unexpected process terminations, without interrupting or corrupting any active connections.
This is achieved through transactional I/O and transformation of data, as well as redundant processes, allowing recovery of resources.

The implementation demonstrates that connection persistence can be achieved without relying on checkpointing or external replication.
Connections are held by multiple processes, while essential state is stored in shared memory.
As long as a process with access to these resources remains, the proxy can recover completely and resume operations.

All processing of a connection's data as well as its underlying state is handled transactionally, ensuring the shared memory is always in a consistent state.
Custom procedures enable transactional commit of mutually dependent objects in memory.
Transactional socket I/O is implemented entirely through the `recvmmsg(2)`{.manpage} and `sendmmsg(2)`{.manpage} syscalls.

Processing and transformation of connections and their underlying protocol is performed by dedicated *worker* processes, with each *worker* handling multiple connections concurrently.
Unexpected terminations of processes are isolated and do not terminate the service, instead the affected parts are restarted during recovery.
However, a faulty process can corrupt central data structures, impacting other parts of the service.

Besides the resources of redundant processes, the memory overhead of crash-tolerance in comparison to a similar architecture is as low as 424 bytes per connection.
When compared to a similar architecture, computational overhead of the necessary transactions varies widely and was measured between 0.3 % and 21.8 %, depending on the number of concurrent connections, number of transferred bytes, and complexity of the performed transformation.
Comparison with a conventional proxy, such as *HAProxy*, shows deficits in HTTP processing and connection latency.

Overall, the results show that the proposed approach is feasible.
TCP connections can be recovered transparently from process failures with relatively little memory overhead.
This shows that the required fault tolerance can be achieved entirely in userspace.

## Future Work

The plateau and steps discussed in \fullref{no-op-transformation} require further investigation.
During this work, only conjectures regarding their origin could be made, but no definitive explanation was found.

The implementation allows for multiple concurrent *worker* processes, however, this was not yet attempted.
Monitoring of *worker* processes must be extended and recovery of only a subset of active connections was not tested.
However, multiple *workers* will utilize multi-processor systems and greatly improve performance.

The comparatively newer `io_uring(7)`{.manpage} interface of the Linux kernel allows for more efficient I/O, where the operating system uses userspace buffers directly instead of copying data between kernel- and userspace.
This could supersede the current `epoll(7)`{.manpage} based approach and the way I/O is currently handled.
Whether transactional I/O is possible using this interface needs to be explored.
However, the handling of *forgotten* file descriptors around `accept(2)`{.manpage} could potentially be improved by `io_uring_prep_accept(3)`{.manpage}.

An entirely different approach could bypass the operating system's conventional TCP stack and instead handle TCP manually or through extensive use of Linux's TCP *repair mode*.
Instead of designing transactions around the kernel-userspace border, they could instead be designed around TCP itself.
When receiving incoming data, the acknowledging `ACK` packet could be sent only once the data is stored safely.
If a crash happens, the sender will automatically retransmit unacknowledged data to the proxy.
Similarly, send buffers would only be updated, once the peer acknowledged the received data.
If a crash happens during send, the unchanged buffer will be retransmitted, while the peer ignores duplicated data and resends the acknowledging `ACK` packet.
\Cref{fig:ack-recv} shows how this can be extended to receive buffers in persistent storage and retry transactions even across faults affecting the entire system, e.g. power loss, thereby extending the achieved fault tolerance.

![`ACK` as part of an I/O transaction](tikz/ack-recv.tex){#fig:ack-recv}

State shared across multiple connections, e.g. a *session*, is not yet considered.
A *session* could span multiple connections with the connections potentially referencing each other.
The `transform()`{.c} function can be arbitrarily complex and potentially implement a global store used by such *sessions*.
This, however, must be integrated with transactions of the individual connections to ensure the global state and connection state remain consistent.

To persist connections across reboots of the system, *CRIU* could be used to create checkpoints of the proxy's processes.
This may already be possible for the current implementation or may need further integration with *libsoccr*.

The current implementation could be extended with memory-safety guarantees by compiling the application with Fil-C.
This could improve robustness and prevent accidental corruption of central data structures.
Additionally, each connection could use a dedicated shared memory segment, as discussed in \fullref{design:shared-memory}.

The directions outlined above provide opportunities to further improve the performance or robustness of the proposed crash-tolerant proxy.
