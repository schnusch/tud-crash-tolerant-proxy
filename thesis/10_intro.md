# Introduction

Proxies are a common part of computer networks.
They are an intermediary between clients and hosts and mediate network traffic.
Clients connect to the proxy, the proxy evaluates and potentially transforms the connection and forwards it to another host.
Proxies can be connected to multiple separate networks and allow access across networks.

![A proxy forwarding connections from clients to database servers on a separate network](tikz/proxy-network.tex)

Since connections pass through the proxy, it puts them in a central role that allows them to inspect or transform traffic.
With full control over the connection, proxies can perform tasks such as

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
 1. separate the database server's network, or
 1. transform client queries or a server responses altogether.

However, the central role of the proxy puts it in a critical position: any interruptions to the proxy will interrupt all connections passing through it as well.
If its operations are impacted, it may become impossible for adjacent services to function.

In high-availability environments any downtime of such a central service may become unacceptable.
For example, if database connections were interrupted, re-establishing potentially long-lived connections could be computationally expensive and impractical.
A proxy in a critical role must, therefore, be designed and operated with special care.
Strategies must be developed to protect or gracefully recover from any interruptions of the proxy.

## Problem description

Interruptions in the proxy's operation could be planned restarts, such as during an upgrade, where established connections and their associated state is migrated to a new instance of the application.
These can generally be prepared for and performed at opportune moments.

Other interruptions may be unexpected and may occur at any point in time.
They can stem from

 1. its underlying hardware or operating system,
 1. the network,
 1. other processes running on the proxy's host, or
 1. from the proxy itself.

The severity of their impact may vary and some of these interruptions, such as critical hardware failures or disasters, cannot be handled in the scope of the proxy service at all.
However recovery from less impactful interruptions is feasible and will become necessary.
This may encompass prepared migration during an upgrade, careful design around operating system interfaces, or redundancy in the proxy itself.

## Objective

The objective of this work is to create a proxy for Linux [@linux] that can upgrade itself in the described manner and is generally robust to system errors or crashes.
It should not lose or corrupt any connections or their associated state under these conditions.

This work means to explore the feasibility of such an implementation.
HTTP/1.1 [@rfc9112] is picked as a relatively simple network protocol.
The recovery mechanisms will be implemented in userspace.
The hardware, network, and operating system of the host the proxy is running on, are assumed to be reliable.
