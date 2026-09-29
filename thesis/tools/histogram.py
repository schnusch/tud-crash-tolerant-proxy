#!/usr/bin/env python3
import argparse
import contextlib
import csv
import locale
import os
import shlex
import subprocess
import sys
import tempfile
from contextlib import contextmanager
from decimal import Decimal
from pathlib import Path
from typing import Dict, Iterator, Optional, TextIO, Union


def median(values: List[Decimal]) -> Decimal:
    return (values[(len(values) - 1) // 2] + values[len(values) // 2]) / 2


def histogram(
    out: TextIO,
    reader: csv.DictReader,
    count: int,
    args: argparse.Namespace,
) -> None:
    writer = csv.writer(out)
    writer.writerow(("i", "percentage", args.column))
    last_row = (0, 0, 0)
    writer.writerow(last_row)
    if args.downsample:
        step = Decimal(count) / args.downsample
        next_i = 1
        for i, row in enumerate(reader, start=1):
            if i < next_i:
                continue
            last_row = (
                i,
                Decimal(i * 100) / count,
                Decimal(row[args.column]) * args.multiply,
            )
            writer.writerow(last_row)
            if i == count:
                break
            next_i = min(round(next_i + step), count)
        assert last_row[0] == count
    else:
        for i, row in enumerate(reader, start=1):
            writer.writerow(
                (i, Decimal(i * 100) / count, Decimal(row[args.column]) * args.multiply)
            )


def plot_derivation(
    out: TextIO,
    center: Optional[Decimal],
    step: Decimal,
    floor: Decimal,
    ceil: Decimal,
    *,
    color: str = "blue",
) -> None:
    bars = [
        f"\\draw[draw={color},dashed] (axis cs:{floor},0) -- (axis cs:{floor},100);",
        f"\\draw[draw={color}] (axis cs:{center},0) -- (axis cs:{center},100);",
        f"\\draw[draw={color},dashed] (axis cs:{ceil},0) -- (axis cs:{ceil},100);",
    ]
    if center is not None:
        lo = hi = center
        while True:
            lo -= step
            hi += step
            if lo < floor or ceil < hi:
                break
            bars.append(
                f"\\draw[draw=none,fill={color},fill opacity=0.15] (axis cs:{lo},0) rectangle (axis cs:{hi},100);",
            )
            break
    out.write("\\begin{scope}[on background layer]\n")
    for bar in reversed(bars):
        # print(bar, file=sys.stderr)
        out.write(f"    {bar}\n")
    out.write("\\end{scope}\n")


def standard_derivation(
    out: TextIO,
    rows,
    args: argparse.Namespace,
) -> None:
    floor = None
    ceil = None

    avg = Decimal(0)
    var = Decimal(0)
    n = 0
    for row in rows:
        y = Decimal(row[args.column])
        floor = y if floor is None else min(floor, y)
        ceil = y if ceil is None else max(ceil, y)
        avg += y
        var += y**2
        n += 1
    avg /= n
    var = (var / n) - avg**2
    std = var.sqrt()
    print(f"avg {avg}, standard derivation {std}", file=sys.stderr)
    plot_derivation(out, center=avg, step=std, floor=floor, ceil=ceil, color=args.color)


def mad(out: TextIO, rows, args: argparse.Namespace) -> None:
    values = sorted(row[args.column] for row in rows)
    m = median(values)
    absolute_derivations = sorted(abs(v - m) for v in values)
    median_absolute_derivation = median(absolute_derivations)
    print(
        f"median {m}, median absolute derivation {median_absolute_derivation}",
        file=sys.stderr,
    )
    plot_derivation(
        out,
        center=m,
        step=median_absolute_derivation,
        floor=values[0],
        ceil=values[-1],
        color=args.color,
    )


@contextlib.contextmanager
def open_output(path: Optional[Path]) -> Iterator[TextIO]:
    kwargs = dict(encoding="utf-8", newline="")
    if path is None:
        with open(sys.stdout.fileno(), "w", closefd=False, **kwargs) as out:
            yield out
    else:
        with tempfile.TemporaryDirectory(dir=path.parent) as _temp:
            temp = Path(_temp) / "out"
            with open(temp, "x+", **kwargs) as out:
                yield out
            os.replace(temp, path)


def parse_column(row: Dict[str, str], column: str, factor: Union[int, Decimal] = 1):
    try:
        value = Decimal(row[column])
    except KeyError:
        raise ValueError(f"{column!r} does not exist in {row!r}")
    value *= factor
    return row | {column: value}


@contextlib.contextmanager
def open_command_output(cmd: List[str], src) -> BytesIO:
    r, w = os.pipe()
    with open(r, encoding="utf-8", newline="") as out:
        try:
            p = subprocess.Popen(cmd, stdin=src.fileno(), stdout=w)
        finally:
            os.close(w)
        try:
            print(p.pid, "$", shlex.join(cmd), file=sys.stderr)
            yield out
        except BaseException:
            p.kill()
            raise
        finally:
            try:
                assert p.wait(1) == 0
            except subprocess.TimeoutExpired:
                p.kill()


def xan_get_first_row(cmd: List[Str], src: Path) -> Dict[str, str]:
    with open(src, "rb") as src, open_command_output(
        ["gzip", "-d"], src
    ) as src, open_command_output(["xan"] + cmd, src) as out:
        reader = csv.DictReader(out)
        for row in reader:
            return row
    raise ValueError("csv is empty")


def xan_quantile(src: Path, count: int, q: Decimal) -> Decimal:
    assert 0 < q and q < 1
    assert count > 1
    pos = q * (count - 1)
    lower = int(pos)
    upper = lower + int(pos != lower)
    with open(src, "rb") as src, open_command_output(
        ["gzip", "-d"], src
    ) as src, open_command_output(
        ["xan", "slice", f"--start={lower}", f"--end={upper + 1}"],
        src,
    ) as out:
        values = [Decimal(row[args.column]) for row in csv.DictReader(out)]
    if len(values) == 2:
        return q * values[0] + (1 - q) * values[1]
    else:
        assert len(values) == 1
        return values[0]


def xan_median(src, count: int) -> Decimal:
    with open_command_output(
        ["xan", "slice", f"--start={(count - 1) // 2}", f"--end={count // 2 + 1}"], src
    ) as out:
        median = Decimal(0)
        n = 0
        for row in csv.DictReader(out):
            median += Decimal(row[args.column])
            n += 1
        median /= n
        print(f"{median=} {n=} {count=}")
    return median


if __name__ == "__main__":
    locale.setlocale(locale.LC_NUMERIC, "C.UTF-8")

    p = argparse.ArgumentParser()
    p.add_argument(
        "-d",
        "--downsample",
        metavar="N",
        type=int,
        help="downsample to N+1 samples",
    )
    p.add_argument("-c", "--color")
    p.add_argument("-m", "--multiply", metavar="M", type=Decimal, default=1)
    p.add_argument("-o", "--output", type=Path)
    p.add_argument("-H", "--histogram", type=Path)
    p.add_argument("-Q", "--quartiles", type=Path)
    p.add_argument("column")
    p.add_argument("src", type=Path)

    args = p.parse_args()
    # print(args, file=sys.stderr)

    with open(args.src, "rb") as src, open_command_output(
        ["gzip", "-d"], src
    ) as src, open_command_output(["xan", "count"], src) as out:
        count = int(out.read())

    # Histogram
    if args.histogram is not None:
        print("histogramming...", file=sys.stderr)
        with open(args.src, "rb") as src, open_command_output(
            ["pv"], src
        ) as pv, open_command_output(["gzip", "-d"], pv) as src, open_output(
            args.histogram
        ) as out:
            histogram(out, csv.DictReader(src), count, args)

    # Quartiles
    if args.quartiles is not None:
        print("quartiling...", file=sys.stderr)
        floor, ceil = (
            Decimal(row[args.column])
            for row in (
                xan_get_first_row(["head", "--limit=1"], args.src),
                xan_get_first_row(["tail", "--limit=1"], args.src),
            )
        )
        quartiles = [
            xan_quantile(args.src, count, Decimal(q)) for q in ["0.25", "0.5", "0.75"]
        ]
        floor, q1, median, q2, ceil = [
            x * args.multiply for x in [floor, *quartiles, ceil]
        ]
        with open_output(args.quartiles) as out:
            m = f"{median.quantize(Decimal('1.00')):,}"
            m = m.rstrip("0").rstrip(".")
            out.writelines(
                [
                    "\\providecommand*{\\legendinfo}[2]{%\n",
                    "    \\addlegendentry{%\n",
                    "        \\rlap{#1}\\vphantom{#1}\\hphantom{crash-tolerant variant} \\csname foo_#2\\endcsname\n",
                    "    }%\n",
                    "}%\n",
                    "\\expandafter\\def\\csname foo_" + args.color + "\\endcsname{%\n",
                    f"    ({count:,} total requests, median {m}\\,ms)%%\n",
                    "}%\n",
                ]
            )

            """
            for line in [
                "\\begin{scope}[on background layer]\n",
                f"    \\draw[draw={args.color     },dashed] (axis cs:{floor },0) -- ({{axis cs:{floor },0}} |- {{rel axis cs:0,1}});\n",
                # f"    \\fill[fill={args.color},opacity=0.3] (axis cs:{q1    },0) rectangle ({{axis cs:{q2},0}} |- {{rel axis cs:0,1}});\n",
                f"    \\draw[draw={args.color            }] (axis cs:{median},0) -- ({{axis cs:{median},0}} |- {{rel axis cs:0,1}});\n",
                f"    \\draw[draw={args.color     },dashed] (axis cs:{ceil  },0) -- ({{axis cs:{ceil  },0}} |- {{rel axis cs:0,1}});\n",
                "\\end{scope}\n",
            ]:
                out.write(line)
            """
