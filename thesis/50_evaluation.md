# Evaluation

The effectivity of the measures to achieve crash tolerance can be determined through tests employing the \fullref{error-injection}.
However, the performance impact of the measures on normal operations will be quantified.
To evaluate only the crash-tolerance measures of the proxy separately from its general architecture, a variant without the crash-tolerance was implemented.

## Baseline Variant

The *baseline* variant of the proxy omits double-buffering and transactions in general but processes connections in exactly the same way as the crash-tolerant variant.
The multi-process architecture was discarded initially, but this resulted in worse performance for parallel connections because `accept(2)`{.manpage} was no longer executed concurrently.
Therefore, a separate *listener* process was retained that only receives incoming connections and passes them to the *baseline's* main *worker* process.

## Memory Overhead of the Crash-Tolerant Variant

Introducing crash-tolerance increases the total size of the `struct connection`{.c} storing all connection data by 424 bytes from 524,688 bytes in the *baseline* to 525,112 bytes in the crash-tolerant variant.

Double-buffered `struct atomic_ring_buffer`{.c} adds members related to the I/O transactions state machine, but only duplicates the range of occupied memory and not the buffer itself, each `struct atomic_ring_buffer`{.c} increases by only 104 bytes.

Since the transformations currently implemented are rather simple, the transformation context is only 4 bytes, the size of the `enum`{.c} storing the transformation's state.
With the double-buffering necessary to implement transactions the size increases to 12 bytes, due to alignment of the `struct`{.c}.

With the relatively large size of 128 KiB, the send or receive buffers dominate the size of `struct connection`{.c} and the memory used per connection increases by only 0.08 % in the crash-tolerant variant.
Naturally the share will increase for smaller buffer sizes, but nevertheless remains very small.

Additional overhead is introduced by the two *listener* processes.
This includes kernel resources associated with each process, as well as the process' stack and memory in userspace.
However the *listener's* implementation was intentionally kept minimal and the majority of the proxy's memory is shared between the *listener* and *worker* processes instead of duplicated.

The *passive* and *active listener* report 32 KiB and 36 KiB of `Private_Dirty` memory, respectively.
This value provides an approximation of the memory exclusively used by a process, as it accounts for memory that is privately allocated and modified rather than shared with other processes.

## Effectivity of Crash-Tolerance

Any crashes of *worker* processes or one of the *listener* processes can be recovered, as long as either the *active* or the *passive listener* process remains.
Terminated processes are detected and restarted through mutual monitoring.
However, during recovery a short window remains, where only a single *listener* process is running and crash-tolerance cannot be guaranteed under all circumstances.
In the future, multiple *passive listener* processes can potentially mitigate this further.

Transactional I/O and transformations allow the *worker* process to be interrupted and the transaction to continue at a later point, without any loss of state or corruption.
The correct behaviour of the transactional I/O routines is verified by unit tests.

However, corruption of the central array of `struct connection`{.c} can result in an irrecoverable state and to protect against this further measures must be developed.
Simplifications during implementation do not allow the recovery of newly created outgoing connections.

## Benchmark Setup

As a HTTP server *nginx* is chosen, to avoid any misconfiguration of the web server itself, the official Docker image [`docker.io/library/nginx:1.30.4-alpine3.24`](https://hub.docker.com/layers/library/nginx/1.30.4-alpine3.24) was chosen.
*nginx* serves static files of different size with the Docker image's default configuration.

This proxy is evaluated against *HAProxy*.
*HAProxy* was chosen because it can be easily configured to run in single-threaded mode and to use either only plain TCP forwarding or HTTP proxying.
A second instance of *nginx* acting as a proxy was discarded, because the identical architecture of the *nginx* instances could introduce implementation-specific performance effects, intentional or otherwise.
Again the official Docker image [`docker.io/library/haproxy:3.3-alpine3.24`](https://hub.docker.com/layers/library/haproxy/3.3-alpine3.24) was chosen.

Both variants of the proxy as well as the benchmark itself are executed in containers and the resulting setup is controlled by `podman-compose`.
Depending on the benchmark a compose file similar to \cref{fig:compose.yaml} is generated.

:::{.figure #fig:compose.yaml}
```yaml
services:
  nginx:
    image: docker.io/library/nginx:1.30.4-alpine3.24
  haproxy:
    image: docker.io/library/haproxy:3.3-alpine3.24

  baseline: # ...
  proxy: # ...

  benchmark: # ...
```

:::{.label}
Shortened `compose.yaml`
:::
:::

All containers will be placed in a dedicated virtual network.
The two proxy variants and *HAProxy* forward all connections to *nginx*, resulting in the connections seen in \cref{fig:veth}.
The benchmark can target the hosts `proxy`, `baseline`, `haproxy`, or `nginx` to measure each proxy or direct connections.

![Test setup created with `podman-compose`](tikz/veth.tex){#fig:veth}

All benchmarks were performed on an Intel Core i9-13900K with 128 GiB of RAM running Debian 13 with Linux kernel 6.12.101.
The CPU's scaling governor was set to `performance` and Turbo Boost was disabled.

## End-to-End Latency

The HTTP benchmarking tool *vegeta* [@vegeta] was chosen to measure the performance of the proxy.
*vegeta* creates concurrent connections and measures the request-response-roundtrip time of each connection.
Tests are repeated with varying number of concurrent connections and size of the response.
For each individual test, *vegeta* is run for 120 s.
Connections performed during the first and last 5 s are discarded to mitigate any unwanted effects during start-up or shutdown.

In the diagrams shown below the X axis represents the time taken by the complete connection round-trip.
The cumulative share of requests that completed in less than or equal to the time is plotted as a line chart.
The histogram with the amount of requests completed in the shown interval is plotted as bars.

### No-op Transformation

The simplest transformation implemented is a no-op.
Any incoming traffic is forwarded to the other peer verbatim.
This allows to evaluate the proxy independently from its HTTP handling and measure its inherent overhead compared to direct connections to *nginx*.
*HAProxy* is configured to forward plain TCP connections and not perform any HTTP processing.

![1 MiB response, 10 concurrent connections](tikz/vegeta-transform_nop-1M-10c.tex){#nop-1M-10c}

![1 MiB response, 100 concurrent connections](tikz/vegeta-transform_nop-1M-100c.tex){#nop-1M-100c}

Direct connections to *nginx* clearly outperform connections through any of the proxies by orders of magnitude.
However, connections passing through any proxy traverse another, albeit virtual, network connection and involve multiple context switches between kernel- and userspace.
Additionally *nginx* runs with multiple processes in parallel and can handle connections concurrently.

Both proxy variants process only a single connection at a time, but `accept(2)`{.manpage} incoming connections concurrently in a separate process.
*HAProxy* is configured to run with only a single thread, which blocks processing during `accept(2)`{.manpage}.
This explains why both variants outperform *HAProxy*.

The prominent plateau and steps in \cref{nop-1M-100c} as well as the subtle steps in \cref{nop-1M-10c} cannot be explained.
However, for small response sizes they can also be observed for *HAProxy* in \cref{nop-1K-100c}.
They require further investigation in the future.

The difference between the median round-trip time of both variants varies widely.
For a relatively small response size of 1 KiB the overhead during connections establishment dominates and the crash-tolerant variant can be up to 21.8 % slower.
As is the case with the unexplained trend of \cref{nop-1M-10c}, where the median of the crash-tolerant variant is 23.6 % greater.
However, for other environments the median overhead of transactional I/O and transformations can be as low as 1.8 % in \cref{nop-1M-1000c}.

In the future, multiple *worker* processes can enable parallel processing of connections and speed up processing, especially for highly concurrent workloads.

### Static HTTP Header Transformation

Another transformation modifies headers of the HTTP requests and responses but does not modify their contents.
The following static HTTP headers are replaced or added:

+-------------------------+----------------------------------------+
| HTTP request headers    | HTTP response headers                  |
+:=======================:+:======================================:+
| ```yaml                 | ```yaml                                |
| Connection: close       | Connection: close                      |
| User-Agent: $user_agent | Server: $user_agent                    |
| DNT: 1                  | X-Clacks-Overhead: GNU Terry Pratchett |
| Sec-GPC: 1              | X-Proxy-PID: $worker_pid               |
| ```                     | ```                                    |
+-------------------------+----------------------------------------+

*HAProxy* operates as a HTTP proxy and adds the same headers.
This test is not performed against *nginx* directly.

![1 MiB response, 1 concurrent connection](tikz/vegeta-transform_headers-1M-1c.tex){#headers-1M-1c}

![1 MiB response, 10 concurrent connections](tikz/vegeta-transform_headers-1M-10c.tex){#headers-1M-10c}

\Cref{headers-1M-1c,headers-1M-10c} show *HAProxy* quickly outperforms both variants when the HTTP request or response is transformed.
This suggests the HTTP handling of the implemented proxy is inferior to *HAProxy*'s.
*HAProxy* spends far less time transforming the HTTP headers and processes concurrent connections more quickly.

However, as the complexity of the transformation increases, the overhead of transactions decreases.
At times, inconsistencies during testing outweigh the overhead of crash-tolerance completely.

### Computationally Expensive Transformation

In \cref{expensive-1M-10c} a small random buffer is hashed with 10,000 iterations of a SHA-256 and the resulting hash is added as the header `X-Random-SHA256`{.email}.

![1 MiB response, 10 concurrent connections](tikz/vegeta-transform_expensive-1M-10c.tex){#expensive-1M-10c}

The limitations of single-threaded connection processing become evident with computationally expensive transformation.
While a connection is processed, and, in this case the SHA-256 hashes are calculated, processing of all other connections is blocked.
As a result steps as seen in \cref{expensive-1M-10c} will form.
If connections are handled by multiple *workers*, each transformation will block fewer connections.

If transformations become more expensive, the influence of transactions diminishes.
In \cref{expensive-1M-10c} the median of the crash-tolerant variant is only 0.3 % higher than the *baseline's* median.

## Byte Latency

Instead of timing a complete connection request-response-roundtrip, the latency of a single byte passing through the proxy is measured.
For this benchmark, the *benchmark* container creates a listening socket.
The proxies will then forward connections back towards the *benchmark* container.
Finally the benchmark sends a single byte through the proxy back to itself and measures the time until the byte is received.

![Test setup looping back to the benchmark](tikz/veth-loop.tex){#fig:veth-loop}

If `select(2)`{.manpage} is called with a `timeout` argument, it will update the `struct timeval`{.c} and the time elapsed during `select(2)`{.manpage} can be calculated.
To gain meaningful results, `select(2)`{.manpage} must be called immediately after `write(2)`{.manpage} returns and the resulting assembly \cref{fig:asm} shows only a limited number of instructions before `select(2)`{.manpage}.

:::{.figure #fig:asm}
```asm
call   1070 <write@plt>
cmp    rax,0x1
jne    1232 <main.cold+0x1f>
mov    r8,QWORD PTR [rbp-0x148]
xor    ecx,ecx
xor    edx,edx
lea    edi,[rbx+0x1]
mov    rsi,QWORD PTR [rbp-0x140]
call   1130 <select@plt>
```

:::{.label}
Assembly of byte latency test
:::
:::

![Byte latency](tikz/first-byte.tex)

While the described setup may not yield exact measurements and is further subject to timer granularity and may experience additional scheduling delays, the results are nevertheless useful.

The results confirm that the proxy's latency is independent of the number of concurrent connections.
The median round-trip through the crash-tolerant variant takes 8.6 % longer than through the *baseline* variant, which is consistent with the measurements of \fullref{end-to-end-latency}, and illustrates the cost of transactions in the current implementation.
However, both variants exhibit approximately an order of magnitude higher latency than established solutions such as *HAProxy* and require further engineering to become competitive.

## Recovery Time

Recovery mechanisms are rather difficult to measure.
Recovery time is influenced by the number of currently active connections and the individual state of each connection.
When an error is induced with \fullref{error-injection}, all connections of the process will be interrupted but only a single connection will be at the state of the induced error.

The following benchmarks aim to measure the time needed for recovery from an injected error.
The benchmark is based on \fullref{byte-latency}, but with a few distinctions:

 1. Create *N - 1* idle connections from the benchmark, through the proxy, back to the benchmark.
 1. Induce a specific error in the proxy.
 1. Create the *N*-th connection or perform I/O to trigger the induced error and subsequently recovery.

No data is sent over the first *N - 1* connections, thus, after the connection is created successfully, they all will settle in `state == CONN_POLL`{.c}.
The *N*-th connection will fail with a well-known state, depending on the induced error.

### Interrupted `recvmmsg(2)`{.manpage} {#interrupt-recvmmsg}

An error is induced that interrupts `recvmmsg(2)`{.manpage}, the *N*-th connection is created, and a single byte is sent to the proxy.
The *worker* process will terminate when it receives the byte and will only be relayed back to the client after recovery was performed.

![Byte latency if interrupted during `recvmmsg(2)`{.manpage}](tikz/fail-recv.tex)

The *active listener* will start a new *worker* process.
Because the sole *worker* process terminated, all connections are *orphaned* and must be passed to the new *worker* process.
The recovery effort is directly proportional to the number of *orphaned* connections: per active connection, three IPC datagrams are sent and two file descriptors are copied.

If in the future multiple *worker* processes each only handle a subset of connections, the termination of a single *worker* process will only result in recovery of its subset of connections.

### Forgotten File Descriptor

An error is induced after `accept(2)`{.manpage} completed but before the file descriptor is stored in shared memory.
The *active listener* will terminate when it receives the final connection from the benchmark and will only create the outgoing connection back to the benchmark during recovery.

![Byte latency if interrupted immediately after `accept(2)`{.manpage}](tikz/fail-accept.tex)

During recovery, the *active listener* will iterate over all its open file descriptors in `/proc/self/fd`.
Each connection consists of two file descriptors, thus the number of open file descriptors is directly proportional to the number of active connections.

However, as stated in \fullref{impl:recovery}, in the current implementation the *worker* process is restarted during recovery.
After the *forgotten* file descriptor is recovered, all connections will be passed to the new *worker* as discussed in \fullref{interrupt-recvmmsg}, vastly overshadowing any impact of the recovery of the open file descriptor.

If in the future this simplification of the recovery process is removed, the *worker* will continue to run and only the *forgotten* file descriptor is recovered and passed to the *worker*, greatly speeding up recovery in this case.
Nevertheless, iterating over the open file descriptors of the process in `/proc/self/fd` will still require linear time.

## Summary

The set design goals set in \fullref{design-goals} were generally met.
*Connection persistence* and reliable *transformation* of the data was fully achieved.
Each *worker* process performs *concurrent connection handling* and this can be further improved upon by introducing multiple concurrent *worker* processes.

Limited *fault isolation* is implemented, terminations of individual processes do not bring down the entire proxy, instead operations are restored after recovery.
However, misbehaving processes are not detected and can potentially corrupt central memory structures, thus impacting other processes.

While the feasibility of *live upgrades* was not shown explicitly, it is possible as discussed in \fullref{design:live-upgrade}.

The memory overhead of the crash-tolerant variant is quite low, while the computational overhead involved is highly dependent on the workload and can be as high as 21.8 %.
However, as more data is processed by a single I/O or transformation transaction, fewer transactions are necessary and the computational overhead decreases substantially.
When transformations of connections are computationally expensive the computational overhead has been shown to become as low as 0.3 %.
Nevertheless, the general feasibility has been shown.

While the recovery speed can be further improved and some simplifications during implementation should be revised, the approach is already effective.
On the test hardware, thousands of concurrent connections can be recovered with sub-second latency.
