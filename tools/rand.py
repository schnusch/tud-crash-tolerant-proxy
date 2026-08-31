#!/usr/bin/env python3
import argparse
import asyncio
import hashlib
import random
import sys

import numpy as np

CHUNK_SIZE = 64 * 1024


async def verify(reader: asyncio.StreamReader, i: int) -> None:
    n = 0
    h = hashlib.sha256()
    saved = b""
    while True:
        chunk = await reader.read(CHUNK_SIZE)
        if not chunk:
            break
        n += len(chunk)
        # Do not hash the last `h.digest_size` (32) bytes.
        if len(chunk) < h.digest_size:
            i = len(saved) + len(chunk) - h.digest_size
            h.update(memoryview(saved)[:i])
            saved = saved[i:] + chunk
        else:
            h.update(saved)
            h.update(memoryview(chunk)[: -h.digest_size])
            saved = chunk[-h.digest_size :]
    # The last `h.digest_size` (32) bytes are the hash of the sent data.
    assert len(saved) == h.digest_size
    got = h.digest()
    assert got == saved, f"{i} received {got.hex()}, sent {saved.hex()}"
    print(f"{i} success ({n - len(saved)} bytes)", file=sys.stderr, flush=True)


async def write_random(
    writer: asyncio.StreamWriter,
    *,
    size: Optional[int] = None,
) -> None:
    h = hashlib.sha256()
    rng = np.random.default_rng()

    if size is None:
        size = random.randrange(128 * 1024 * 1024) + 1
    while size > 0:
        chunk = rng.bytes(min(size, CHUNK_SIZE))
        h.update(chunk)
        size -= len(chunk)

        writer.write(chunk)
        await writer.drain()

    writer.write(h.digest())
    await writer.drain()
    writer.write_eof()


conn_number = 0


async def handle_conn(
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
) -> None:
    global conn_number
    i = conn_number
    conn_number += 1
    print(f"{i} handle_conn", file=sys.stderr, flush=True)
    try:
        await asyncio.gather(
            verify(reader, i),
            write_random(writer),
        )
    finally:
        writer.close()
        await writer.wait_closed()


async def connect(host: str, port: int, sem: asyncio.Semaphore) -> None:
    async with sem:
        reader, writer = await asyncio.open_connection(host, port)
        await handle_conn(reader, writer)


async def main() -> None:
    p = argparse.ArgumentParser(description="Generate and verify random data streams.")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("-l", "--listen", action="store_true", help="listen on HOST:PORT")
    g.add_argument(
        "-n",
        "--connections",
        metavar="N",
        type=int,
        help="connect N times to HOST:PORT",
    )
    p.add_argument(
        "-c",
        "--concurrent",
        metavar="C",
        type=int,
        default=2,
        help="perform C connections concurrently",
    )
    p.add_argument("host", metavar="HOST")
    p.add_argument("port", metavar="PORT", type=int)
    args = p.parse_args()

    if args.listen:
        server = await asyncio.start_server(handle_conn, args.host, args.port)
        async with server:
            await server.serve_forever()
    else:
        sem = asyncio.Semaphore(args.concurrent)
        await asyncio.gather(
            *(connect(args.host, args.port, sem) for _ in range(args.connections))
        )


if __name__ == "__main__":
    asyncio.run(main())
