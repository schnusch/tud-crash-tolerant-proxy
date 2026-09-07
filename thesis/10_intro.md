# Introduction

Proxies are a common part of computer networks.
They are an intermediary between hosts and forward network traffic.
Clients connect to the proxy, the proxy evaluates the connection and forwards it to another host.
Proxies can be connected to multiple separate networks and allow access accross networks.

![A proxy forwarding connections from clients to database servers on a separate network](tikz/proxy-network.tex)

Since connections pass through the proxy, it puts them in a central role, that allows them to inspect or transform traffic.
With full control over the connection proxies can perform tasks such as

 1. logging,
 1. content-filtering,
 1. network separation,
 1. encryption [@rfc8446],
 1. abuse prevention [@anubis; @go-away],
 1. re-routing of connections, or
 1. load-balancing.

Some of the common functionality of the peers can be offloaded to the proxy itself, which can lead to an overall simpler architecture.
In a database environment a proxy may be used to additionally provide any of the following tasks or take them over from the database servers:

 1. authorization,
 1. load-balancing between multiple database servers,
 1. separate the databases server's network, or
 1. transform a client queries or a server responses altogether.

But in any case the the central role of the proxy puts it in a critical position.
Any interruptions to the proxy will interrupt all connections passing through it as well.
If its operations are impacted, it may become impossible for adjacent services to function.

In high-availability environments any downtime of such a central services may become unacceptable.
E.g. if a database connections were to be interrupted its transactions will be aborted, the possibly long-lived connection can be computationally expensive to perform and impractical to retry.
Therefore a proxy in a critical role must be designed and operated with special care.
Strategies must be developed to protect or gracefully recover from any interruptions of the proxy.

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
But recovery from less impactful interruptions is feasible and will become necessary.
This may encompass prepared migration during an upgrade, careful design around operating system interfaces, or redundancy in the proxy itself.

## Objective

The objective of this work is to create a proxy for Linux [@linux], that can upgrade itself in the described manner and is generally robust to system errors or crashes.
It should not loose or corrupt any connections or their associated state under these conditions.

This works means to explore the feasibility of such an implementation.
HTTP/1.1 [@rfc9112] is picked as a relatively simple network protocol.
The recovery mechanisms will be implemented in userspace.
The hardware, network, and operating system of the host, the proxy is running on, are assumed to be reliable.
