#!/usr/bin/env python3
import argparse
import asyncio
import os
import shlex
import socket
import subprocess
import sys
import tempfile
from contextlib import ExitStack, contextmanager
from pathlib import Path
from typing import Iterator, Tuple


def echo(x):
    print(x, file=sys.stderr)
    return x


def nix_build(nix: Path) -> str:
    p = subprocess.run(
        echo(
            [
                "nix-build",
                "--no-out-link",
                "--argstr",
                "file",
                nix.absolute(),
                "--expr",
                "{ file }: (import <nixpkgs> { }).callPackage file { }",
            ]
        ),
        check=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
    )
    return os.fsdecode(p.stdout.rstrip())


@contextmanager
def tmux_tail(prefix: str = "") -> Iterator[int]:
    with tempfile.TemporaryDirectory(prefix="tmux-fifo.") as temp:
        path = os.path.join(temp, "socket")
        cmd = f"nc -U {shlex.quote(str(path))}"
        cmd = (
            f"{cmd} | sed -e {shlex.quote(f's/^/{prefix}/')}"
            if prefix
            else f"exec {cmd}"
        )
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listen:
            listen.bind(path)
            listen.listen()
            subprocess.run(
                echo(
                    [
                        "tmux",
                        "split-window",
                        "-bd",
                        "; ".join(
                            [
                                "PS4='$ '",
                                "set -x",
                                "tmux set-option remain-on-exit off",
                                cmd,
                            ]
                        ),
                    ]
                )
            )
            conn, _ = listen.accept()
    try:
        conn.shutdown(socket.SHUT_RD)
        yield conn.fileno()
    finally:
        conn.close()


def sed_prefix(prefix: str) -> int:
    p = subprocess.Popen(
        ["sed", "-e", f"s/^/{prefix} /"],
        stdin=subprocess.PIPE,
    )
    assert p.stdin is not None
    return p.stdin.fileno()


async def wait_first(*aws):
    tasks = [asyncio.create_task(aw) for aw in aws]
    try:
        done, _ = await asyncio.wait(
            tasks,
            return_when=asyncio.FIRST_COMPLETED,
        )
        results = [None] * len(tasks)
        for i, task in enumerate(tasks):
            if task in done:
                results[i] = await task
                break
        return (*results,)
    finally:
        for task in tasks:
            if not task.done():
                task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


async def kill_wait(proc):
    try:
        if proc.returncode is None:
            proc.kill()
    finally:
        await proc.wait()


def parse_sockaddr(arg: str) -> Tuple[str, int]:
    host, port = arg.rsplit(":", maxsplit=1)
    return (host.strip("[]"), int(port))


async def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--no-tmux", action="store_false", dest="tmux")
    p.add_argument("--nix", type=Path, default=Path("baseline.nix"))
    p.add_argument("--upstream-addr", type=parse_sockaddr, default=("127.0.0.1", 12345))
    p.add_argument("--listen-addr", type=parse_sockaddr, default=("127.0.0.1", 12346))
    p.add_argument("--strace")
    args, tail = p.parse_known_args()

    stdbuf = ["stdbuf", "-i0", "-o0", "-e0", "--"]
    strace = ["strace", "-e", args.strace, "--"] if args.strace else []

    server_cmd = [
        "iperf3",
        "--server",
        "--one-off",
        "--bind-dev=lo",
        f"--port={args.upstream_addr[1]}",
    ]
    client_cmd = [
        "iperf3",
        f"--client={args.listen_addr[0]}",
        f"--port={args.listen_addr[1]}",
    ]

    pkg = nix_build(args.nix)

    with ExitStack() as exit:
        if args.tmux:
            out_server = exit.enter_context(tmux_tail("server> "))
            out_proxy = exit.enter_context(tmux_tail("proxy>  "))
            out_client = exit.enter_context(tmux_tail("client> "))
        else:
            out_server = sed_prefix("server> ")
            out_proxy = sed_prefix("proxy>  ")
            out_client = sed_prefix("client> ")

        async def start_server():
            server = await asyncio.subprocess.create_subprocess_exec(
                *echo(stdbuf + server_cmd),
                stdin=subprocess.DEVNULL,
                stdout=out_server,
                stderr=out_server,
            )
            try:
                await wait_first(
                    server.wait(),
                    start_proxy(),
                )
            finally:
                await kill_wait(server)

        async def start_proxy():
            proxy = await asyncio.subprocess.create_subprocess_exec(
                *echo(
                    stdbuf
                    + strace
                    + [
                        f"{pkg}/bin/crash-tolerant-proxy",
                        f"-l{args.listen_addr[0]}:{args.listen_addr[1]}",
                        f"--upstream-addr={args.upstream_addr[0]}:{args.upstream_addr[1]}",
                    ]
                ),
                stdin=subprocess.DEVNULL,
                stdout=out_proxy,
                stderr=out_proxy,
            )
            try:
                await wait_first(
                    proxy.wait(),
                    start_client(),
                )
            finally:
                await kill_wait(proxy)

        async def start_client():
            if args.tmux:
                subprocess.run(["tmux", "select-layout", "even-vertical"], check=True)
            client = await asyncio.subprocess.create_subprocess_exec(
                *echo(stdbuf + strace + client_cmd + tail),
                stdin=subprocess.DEVNULL,
                stdout=out_client,
                stderr=out_client,
            )
            try:
                rc = await client.wait()
                assert rc == 0, f"{rc!r} != 0"
            finally:
                await kill_wait(client)

        await start_server()


if __name__ == "__main__":
    asyncio.run(main())
