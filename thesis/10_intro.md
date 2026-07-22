# Introduction

A proxy server or service (short *proxy*) is a service that forward network traffic.
It listens for incoming connections from *downstream* clients and creates outgoing connections to *upstream* servers on behalf of those clients.
Proxies are usually separated in the two categories of *forward* and *reverse proxies*.

Forward proxies work on behalf of the client.
They receive a connection from a client and the connection indicates where to the proxy server should create the outgoing connection.
They may used for content filtering [@content-filtering] or network separation, where only the host, the proxy is running on, is connected to another separate networks and allows the clients to connect to that network.

Reverse proxies work on behalf of the server.
They also receive connections from clients but the hosts they connect to are set.
Their uses encompass load-balancing between multiple servers [@load-balancing], abuse prevention [@anubis; @go-away], routing to upstream applications [@proxy-routing], TLS termination [@tls-termination], or may also separate a upstream network from the downstream network they are reachable from.

In a database environment a proxy might perform load-balancing or fail-over between multiple redundant database servers.
The database connections can be very long-lived and computationally expensive to redo.
But the proxy itself becomes a single point of failure since all connections go through it.
Therefore any interruption to the proxy will interrupt the connections passing through it and must be avoided.
For a large number of concurrent connections, it is generally not feasible to create a dedicated process per connection [@TODO], which would isolate connections and reduce the impact of a crash.
If a proxy's process terminates, multiple connections will be lost simultaneously.

If the proxy is restarted, e.g. to perform an upgrade, the new instance could simply handle all new connections, while the old instance finishes up its active connections and terminates.
But this may not work for long-lived connections, which may need to be migrated to the new instance of the proxy *in-flight*.

Aside from planned interruptions the proxy might encounter a critical system error.
The propability of such an error occuring is increased, if the processes run for a long time. [@runtime]
If they are not handled properly its processes may terminate unexpectedly, losing active connections.

While most system errors can be handled by the proxy's processes themselves, some might cause a proxy's process to terminate immediately, and others cannot be handled at all or might just not fall within the scope of the proxy, e.g. the host or operating system, the proxy is running on, might crash unexpectedly.

In userspace the network connections of are exposed through file descriptors. [@file-descriptor]
If a proxy's process terminates, its file descriptors are closed and the operating system will usually terminate the connections. [@close]
But a network connection can be shared between multiple processes if each process holds a file descriptor of that connection.
Then the operating system will only terminate the connection once all its file descriptors in all processes are closed.
So even if the process handling a connection terminates, the connection may still be recovered as long as another process that holds a file descriptor of it is still running.

The objective of this work is to create a proxy for Linux [@linux], that can upgrade itself in the described manner and more generally recover from system errors or crashes without losing or corrupting any connections or their associated state.
~~Performance may be degraded during recovery.~~
The recovery mechanisms will be implemented in userspace.
The hardware, network, and operating system of the host the proxy is running on are assumed to be reliable.
