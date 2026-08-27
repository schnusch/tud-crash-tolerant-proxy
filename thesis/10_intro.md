# Introduction

A proxy server or service (short *proxy*) is a service that forward network traffic.
It listens for incoming connections from clients and creates outgoing connections to servers on behalf of those clients.

Usually a proxy operates on OSI Transport Layer/layer 4 and above and forwards TCP traffic verbatim or implement a higher level protocol such as HTTP.
As such a proxy is in the position to inspect or transform the traffic passing through it.
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

## Problem description

Interruptions in the proxy's operation could be planned restarts, such as during an upgrade, where established connections and their associated state is migrated to a new instance of the proxy.
These can generally be prepared for and performed at opportune moments.

Other interruptions may be unexpected and may occur at any point in time.
They can stem from

 1. its underlying hardware or operating system,
 1. the network,
 1. other processes running on the proxy's host, or
 1. from the proxy itself.

Severity of their impact may vary and some of these interruptions, such as critical hardware failures or disasters, cannot be handled in the scope of the proxy service at all.
But recovery from less impactful interruptions is feasible and may become necessary.
This may encompass prepared migration during an upgrade, careful design around operating system interfaces, or redundancy in the proxy itself.

## Objective

The objective of this work is to create a proxy for Linux [@linux], that can upgrade itself in the described manner and is generally robust to system errors or crashes.
It should not loose or corrupt any connections or their associated state under these conditions.

This works means to explore the feasibility of such an implementation.
HTTP/1.1 [@rfc9112] is picked as a relatively simple protocol.
The recovery mechanisms will be implemented in userspace.
The hardware, network, and operating system of the host, the proxy is running on, are assumed to be reliable.
