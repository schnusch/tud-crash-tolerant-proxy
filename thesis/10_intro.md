# Introduction

A proxy server or service (short *proxy*) is a service that forward network traffic.
It listens for incoming connections from clients and creates outgoing connections to servers on behalf of those clients.

A proxy is in the position to inspect or transform the traffic passing through it.
This allows the proxy to perform tasks such as

 1. encryption,
 1. content-filtering,
 1. abuse prevention [@anubis; @go-away],
 1. re-routing of connections, or
 1. load-balancing.

Additionally the host the proxy is running on may be connected to multiple separate networks and may allow the users access across networks.

In a database environment a proxy may be used to perform

 1. authorization,
 1. load-balancing between multiple database servers,
 1. separate the databases server's network, or
 1. transform a client queries or a server responses altogether.

![A proxy forwarding connections from clients to database servers on a separate network](tikz/proxy-network.tex)

Database connections especially can be rather long-lived and computationally expensive to perform.
In any case the proxy becomes a single point of failure.
If it is interrupted, connections passing through it will be interrupted as well.
If its service is impacted, it may become impossible for adjacent services to function.

In a high-availability environment any downtime of central services may become unacceptable.
Therefore a proxy service in a critical role must be designed and operated with special care.

::: {.hidden}
In a database environment a proxy may perform load-balancing or fail-over between multiple redundant database servers, or .
The database connections can be very long-lived and computationally expensive to redo.
**TODO** more

**TODO** Vorgriffe OK

**TODO** abstrakt, keine Details

**TODO** objective: low cost of redundancy
:::

## Problem description

Interruptions in the proxy's operation could be planned restarts, such as during an upgrade, where established connections and their associated state is migrated to a new instance of the proxy.
These can be generally prepared for and performed at opportune moments.

Other interruptions may be unexpected and may occur at any point in time.
They can stem from

 1. its underlying hardware or operating system,
 1. the network,
 1. other processes running on the proxy's host, or
 1. from the proxy itself.

Severity of their impact may vary and some of these interruptions, such as critical hardware failures or disasters, cannot be handled in the scope of the proxy service at all.
But recovery from less impactful interruptions is feasible and may become necessary.

This may encompass prepared migration during an upgrade, careful design around operating system interfaces, or redundancy in the proxy itself.

::: {.hidden}
This can range from simply avoiding or handling minor errors to 
The impact of these interruptions can vary and might be temporary or permanent and may or may not be recoverable.
The proxy itself could misbehave and potentially impact its connections.

In any case the proxy itself becomes a single point of failure since all connections pass through it.
Therefore any interruption to the proxy will interrupt the connections passing through it and must be avoided.
For a large number of concurrent connections, it is generally not feasible to create a dedicated process per connection [@TODO], which could isolate connections and reduce the impact of an interruption.
If a proxy's process terminates, multiple connections will be lost simultaneously.

If the proxy is restarted, e.g. to perform an upgrade, the new instance could simply handle all new connections, while the old instance finishes up its active connections and terminates.
But this may not work for long-lived connections, which may need to be migrated to the new instance of the proxy *in-flight*, meaning transparently to client and servers.

Aside from planned interruptions the proxy might encounter a critical system error.
The probability of such an error occuring is increased, if the processes run for a long time. [@runtime] **TODO** explain
If they are not handled properly its processes may terminate unexpectedly, losing active connections.
These errors might stem from the underlying hardware, operating system, or other processes running on the host.
While most system errors can be handled by the proxy's processes themselves, some might cause a proxy's process to terminate immediately, and others cannot be handled at all or might just not fall within the scope of the proxy, e.g. the host or operating system, the proxy is running on, might crash unexpectedly.

On POSIX-compliant operating systems network connections of are exposed in userspace through file descriptors. [@file-descriptor]
If a proxy's process terminates, its file descriptors are closed and the operating system will usually terminate the connections. [@close]
But a network connection can be shared between multiple processes, if each process holds a file descriptor of that connection.
Then the operating system will only terminate the connection once all its file descriptors in all processes are closed.
So even if the process handling a connection terminates, the connection may still be recovered as long as another process that holds a file descriptor of it is still running.
:::

## Objective

The objective of this work is to create a proxy for Linux [@linux], that can upgrade itself in the described manner and is generally robust to system errors or crashes.
It should not loose or corrupt any connections or their associated state under these conditions.

The recovery mechanisms will be implemented in userspace.
Performance may be degraded during recovery.
The hardware, network, and operating system of the host the proxy is running on are assumed to be reliable.

::: {.hidden}
## Organization

was erwartet in der Arbeit
:::
