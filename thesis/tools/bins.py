#!/usr/bin/env python3
import argparse
import contextlib
import csv
import io
import tempfile
import shlex
import subprocess
import sys
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path
from typing import Tuple

from histogram import open_output, open_command_output


def column_path(arg: str) -> Tuple[str, Path]:
    xs = arg.split(":", 1)
    assert len(xs) == 2
    return (xs[0], Path(xs[1]))


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("-m", "--multiply", metavar="M", type=Decimal, default=1)
    p.add_argument("-o", "--output", required=True, type=Path)
    p.add_argument("-c", "--src-column", required=True)
    p.add_argument("column", nargs="+", type=column_path)
    args = p.parse_args()

    with tempfile.TemporaryFile() as merged:
        with contextlib.ExitStack() as exit:
            pass_fds = []
            for column, path in args.column:
                with open(path, "rb") as src:
                    gz = exit.enter_context(open_command_output(["gzip", "-d"], src))
                renamed = exit.enter_context(
                    open_command_output(
                        ["xan", "rename", f"--select={args.src_column}", column], gz
                    )
                )
                padded = exit.enter_context(
                    open_command_output(
                        [
                            "xan",
                            "map",
                            ",".join(
                                (f"{c} as _foo" if c == column else f"-1 as {c}")
                                for c, _ in args.column
                            ),
                        ],
                        renamed,
                    )
                )
                selected = exit.enter_context(
                    open_command_output(
                        [
                            "xan",
                            "select",
                            ",".join(
                                ["timestamp_ns", "_foo"] + [c for c, _ in args.column]
                            ),
                        ],
                        padded,
                    )
                )
                pass_fds.append(selected.fileno())

            merge = ["xan", "merge", "--select=_foo", "--"] + [
                f"/dev/fd/{fd}" for (col, _), fd in zip(args.column, pass_fds)
            ]
            print("$", shlex.join(merge), file=sys.stderr)
            subprocess.run(
                merge,
                stdin=subprocess.DEVNULL,
                stdout=merged.fileno(),
                pass_fds=pass_fds,
                check=True,
            )

        # Calculate bins for the `_foo` column. It contains values of all other
        # columns.
        merged.seek(0)
        with open_command_output(
            ["xan", "bins", "--select=_foo", "--bins=100"], merged
        ) as global_bins:
            rows = iter(csv.DictReader(global_bins))
            row = next(rows)
            bins = [Decimal(row[b]) for b in ["lower_bound", "upper_bound"]]
            for row in rows:
                bins.append(Decimal(row["upper_bound"]))

        # Put each column in the bins chosen previously.
        text = io.TextIOWrapper(merged, encoding="utf-8", newline="")
        count = {}
        foo = {}
        for column, _ in args.column:
            bounds = iter(bins)
            bound = next(bounds)
            count[column] = 0
            foo[column] = []
            current = 0

            print(f"binning {column!r}...", file=sys.stderr)
            text.seek(0)
            for row in csv.DictReader(text):
                val = Decimal(row[column])
                while val >= bound:
                    try:
                        bound = next(bounds)
                    except StopIteration:
                        break
                    foo[column].append(current)
                    current = 0
                if val >= 0:
                    current += 1
                    count[column] += 1

            # Last bin will not have been added.
            foo[column].append(current)
            # Add all trailing bins.
            for _ in bounds:
                foo[column].append(0)

            assert len(foo[column]) == len(
                bins
            ), f"{column} bins: {len(foo[column])!r} != {len(bins)!r}"
            assert (
                sum(foo[column]) == count[column]
            ), f"{column} rows: {sum(foo[column])!r} != {count[column]!r}"

        bar = list(foo.items())
        cols = [c for c, _ in bar]
        vals = [v for _, v in bar]

        # pad for `ybar interval`
        bins.insert(0, 0)
        for vs in vals:
            vs.append(0)

        with open_output(args.output) as fp:
            out = csv.writer(fp)
            out.writerow(["upper_bound"] + cols)
            for upper_bound, row in zip(bins, zip(*vals)):
                out.writerow(
                    [upper_bound * args.multiply]
                    + [
                        (Decimal(v * 100) / count[c]).quantize(
                            Decimal("0.000001"),
                            rounding=ROUND_HALF_UP,
                        )
                        for c, v in zip(cols, row)
                    ]
                )
