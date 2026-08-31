#!/usr/bin/env nix-shell
#!nix-shell -i python3 -p python3.pkgs.python-debian
import csv
import gzip
import hashlib
import os
import re
import subprocess
import sys
import urllib.request
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import BinaryIO, Iterator, Optional, TextIO, Union

from debian.deb822 import Sources

baseurl = "http://deb.debian.org/debian"


@contextmanager
def fetch_sources(*, component: str = "main") -> Iterator[gzip.GzipFile]:
    with urllib.request.urlopen(
        f"{baseurl}/dists/trixie/{component}/source/Sources.gz"
    ) as resp:
        yield gzip.GzipFile(fileobj=resp)


def pool_url(package: str, filename: str, *, component: str = "main") -> str:
    if package.startswith("lib"):
        prefix = package[:4]
    else:
        prefix = package[:1]
    return f"{baseurl}/pool/{component}/{prefix}/{package}/{filename}"


def sources_as_csv(fileobj: TextIO) -> None:
    """Parse Debian Sources and write package info and tarball URL as CSV to
    ``fileobj``."""
    with fetch_sources() as sources:
        csvout = csv.writer(
            fileobj,
            delimiter=";",
            quoting=csv.QUOTE_ALL,
        )
        csvout.writerow(["package", "version", "tarball", "size", "md5"])
        for entry in Sources.iter_paragraphs(sources):
            name = entry["Package"]
            version = entry["Version"]
            files = entry["files"]

            orig = None
            for file in files:
                if re.match(r"^.*\.orig\.tar\.[^.]+$", file["name"]) is not None:
                    orig = file
                    break
            if orig is None:
                continue

            csvout.writerow(
                [
                    name,
                    version,
                    pool_url(name, orig["name"]),
                    orig["size"],
                    orig["md5sum"],
                ]
            )


def download_into(out: BinaryIO, url: str, size: int, md5sum: str) -> None:
    h = hashlib.md5()
    with urllib.request.urlopen(url) as resp:
        for chunk in iter(lambda: resp.read(16384), b""):
            size -= len(chunk)
            assert size >= 0
            assert out.write(chunk) == len(chunk)
            h.update(chunk)
    assert size == 0
    assert h.hexdigest() == md5sum


def for_each_source(
    fileobj: TextIO,
    command: Union[str, bytes, os.PathLike[str], os.PathLike[bytes]],
    *,
    after: Optional[str] = None,
    until: Optional[str] = None,
    max_size: Optional[int] = 16 * 1024 * 1024,
) -> None:
    """Read ``fileobj`` produced by ``sources_as_csv``, download the tarball of
    each entry, and run ``command`` for each entry."""
    csvin = csv.DictReader(fileobj, delimiter=";")
    for row in csvin:
        if after is not None and row["package"] <= after:
            continue
        if until is not None and row["package"] > until:
            continue

        size = int(row["size"], 10)

        print(
            f"{row['package']} {row['version']}: ",
            end="",
            flush=True,
            file=sys.stderr,
        )
        if max_size is not None and size > max_size:
            print("skipped, too large", file=sys.stderr)
            continue

        with tempfile.TemporaryDirectory() as _temp:
            temp = Path(_temp)
            tarball = temp / row["tarball"].rsplit("/", 1)[-1]

            with open(tarball, "xb") as tar:
                download_into(
                    tar,
                    url=row["tarball"],
                    size=size,
                    md5sum=row["md5"].lower(),
                )

            print(row["tarball"], file=sys.stderr)
            subprocess.run([command], cwd=temp, stdin=subprocess.DEVNULL, check=True)


if __name__ == "__main__":
    with open("debian.csv", "x", encoding="utf-8", newline="") as fileobj:
        sources_as_csv(fileobj)
    with open("debian.csv", "r", encoding="utf-8", newline="") as fileobj:
        for_each_source(
            fileobj,
            command=Path(__file__).parent / "check-src.sh",
        )
