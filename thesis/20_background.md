# Background

see `NOTES.md`

Proxies are usually separated in the two categories of *forward* and *reverse proxies*.

*Forward proxies* work on behalf of the client.
They receive a connection from a client and the connection indicates where to the proxy server should create the outgoing connection.
They may used for content filtering [@content-filtering] or network separation, where only the host, the proxy is running on, is connected to another separate networks and allows the clients to connect to that network.

*Reverse proxies* work on behalf of the server.
They also receive connections from clients but the hosts they connect to are set.
Their uses encompass load-balancing between multiple servers [@load-balancing], abuse prevention [@anubis; @go-away], routing to upstream applications [@proxy-routing], TLS termination [@tls-termination], or may also separate a upstream network from the downstream network they are reachable from.

## not intro

Crash >= Restart

abgrenzung zu vm migration

## Software Fault Tolerance

Hardening software against and possibly recovering from unexpected failures is the subject of *software fault tolerance*. [@sft]
Failures may originate from the host's hardware or its software.
Besides defects in the hardware, hardware faults might temporarily include network or more generally I/O errors.

Software faults can originate from invalid memory access, floating point exceptions, sandboxing [@seccomp], or errors in the program's state itself.
On POSIX-compliant operating systems most of these failures are indicated by the syscalls themselves [@errno] or are delivered to the process through signals [@signal7].
Linux [@linux] overcommits its virtual memory [@overcommit] and will, in memory-pressure situations, trigger its out-of-memory killer which will pick and terminate processes to reclaim resources.
The last group of errors ...

## Fehlermodelle

## ACID/Transactions

## Related Work
