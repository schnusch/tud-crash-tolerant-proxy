#!/usr/bin/env python3
import argparse
import csv
import gzip
import os
import shlex
import subprocess
import sys
import tempfile
from decimal import Decimal
from pathlib import Path
from histogram import open_output

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("column")
    args = p.parse_args()

    with tempfile.TemporaryFile(
        mode="x+", encoding="utf-8", newline=""
    ) as temp_by_column:
        # Slice earliest and lastest rows.
        with tempfile.TemporaryFile(
            mode="x+",
            encoding="utf-8",
            newline="",
        ) as temp_by_time:
            with open(
                sys.stdin.fileno(), "r", encoding="utf-8", newline="", closefd=False
            ) as stdin:
                reader = csv.DictReader(stdin)
                writer = None
                for i, row in enumerate(reader):
                    dot = 10_000
                    exl = 1_000_000
                    if i % 10_000 == 0:
                        print(
                            end="!" if i % exl == exl - dot else ".",
                            file=sys.stderr,
                            flush=True,
                        )
                    if writer is None:
                        writer = csv.DictWriter(temp_by_time, row.keys())
                        writer.writeheader()
                        lowest = row
                        highest = row
                    else:
                        lowest = min(
                            lowest, row, key=lambda x: Decimal(x["timestamp_ns"])
                        )
                        highest = max(
                            highest, row, key=lambda x: Decimal(x["timestamp_ns"])
                        )
                    writer.writerow(row)
            print(file=sys.stderr, flush=True)
            temp_by_time.flush()

            # Slice and sort by `args.column`.
            skip_ns = 5_000_000_000
            os.lseek(temp_by_time.fileno(), 0, os.SEEK_SET)
            filter = f"{Decimal(lowest['timestamp_ns']) + skip_ns} <= timestamp_ns && timestamp_ns + {args.column} < {Decimal(highest['timestamp_ns']) - skip_ns}"
            cmd = f"xan filter {shlex.quote(filter)} | xan sort --parallel --external --numeric --select={shlex.quote(args.column)}"
            print("$", cmd, file=sys.stderr)
            subprocess.run(
                "set -eo pipefail; " + cmd,
                shell=True,
                stdin=temp_by_time.fileno(),
                stdout=temp_by_column.fileno(),
                check=True,
            )

            # Print stats.
            for fp in [temp_by_time, temp_by_column]:
                fp.seek(0)
                subprocess.run(
                    ["xan", "count"],
                    stdin=fp.fileno(),
                    stdout=sys.stderr.fileno(),
                    check=True,
                )

        temp_by_column.seek(0)
        subprocess.run(
            ["cat"],
            stdin=temp_by_column.fileno(),
            stdout=sys.stdout.fileno(),
            check=True,
        )
