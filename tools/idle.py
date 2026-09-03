#!/usr/bin/env python3
import argparse
import asyncio
import hashlib
import sys

import numpy as np

from rand import verify

CHUNK_SIZE = 64 * 1024


async def handle_conn(
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
    i: int,
) -> None:
    print(f"{i} handle_conn()", file=sys.stderr, flush=True)

    h = hashlib.sha256()
    size = 1 * 1024 * 1024
    size = max(size, h.digest_size)
    chunk = f"POST / HTTP/1.0\r\nContent-Length: {size}\r\n\r\n".encode("ascii")
    h.update(chunk)
    size -= h.digest_size
    writer.write(chunk)
    await writer.drain()

    rng = np.random.default_rng()
    while size > 0:
        chunk = rng.bytes(min(size, CHUNK_SIZE))
        h.update(chunk)
        size -= len(chunk)
        writer.write(chunk)
        await writer.drain()

    writer.write(h.digest())
    await writer.drain()
    writer.write_eof()

    await verify(reader, i)


conn_number = 0


async def connect(
    host: str,
    port: int,
    ev: asyncio.Event,
) -> None:
    global conn_number
    i = conn_number
    conn_number += 1
    print(f"{i} connect({host!r}, {port!r})", file=sys.stderr, flush=True)
    reader, writer = await asyncio.open_connection(host, port)
    try:
        # Connections is now active and idle.
        print("ev.wait()", file=sys.stderr, flush=True)
        await ev.wait()
        await handle_conn(reader, writer, i)
    finally:
        writer.close()
        await writer.wait_closed()


async def sleep_and_connect(
    host: str,
    port: int,
    ev: asyncio.Event,
    sleep: int,
) -> None:
    print(f"sleep({sleep})", file=sys.stderr, flush=True)
    await asyncio.sleep(sleep)
    # The proxy should now crash during `accept(2)`.
    print("creating delayed connection...", file=sys.stderr, flush=True)

    global conn_number
    i = conn_number
    conn_number += 1
    print(f"{i} connect({host!r}, {port!r})", file=sys.stderr, flush=True)
    reader, writer = await asyncio.open_connection(host, port)
    try:
        # Recovery should occur in the proxy, meanwhile send on all connections.
        await asyncio.sleep(1)
        ev.set()
        await handle_conn(reader, writer, i)
    finally:
        writer.close()
        await writer.wait_closed()


async def main() -> None:
    p = argparse.ArgumentParser(
        description="Create C-1 idle connections, wait T seconds, create another connection, and then send dummy POST requests on all connections.",
    )
    p.add_argument(
        "-c",
        "--concurrent",
        metavar="C",
        type=int,
        default=2,
        help="establish C concurrent connections",
    )
    p.add_argument(
        "-t",
        "--wait",
        metavar="T",
        type=int,
        required=True,
        help="sleep T seconds before establishing the final connection",
    )
    p.add_argument("host", metavar="HOST")
    p.add_argument("port", metavar="PORT", type=int)
    args = p.parse_args()

    ev = asyncio.Event()
    await asyncio.gather(
        sleep_and_connect(args.host, args.port, ev, args.wait),
        *(connect(args.host, args.port, ev) for _ in range(args.concurrent - 1)),
    )


if __name__ == "__main__":
    asyncio.run(main())
