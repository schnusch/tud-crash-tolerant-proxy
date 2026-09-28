#!/usr/bin/env python3
import csv
import sys
from decimal import Decimal

from histogram import median

if __name__ == "__main__":
    with open(sys.stdin.fileno(), "r", encoding="utf-8", newline="") as fp:
        rows = sorted(
            csv.DictReader(fp),
            key=lambda x: Decimal(x["write"]),
        )

    m = median([Decimal(row["write"]) for row in rows]) * 1_000_000
    with open(3, "w", encoding="utf-8") as fp:
        fp.write(
            r"\expandafter ".join(
                [
                    r"",
                    r"\addplot",
                    r"[\mediancolor] coordinates {(0,%s) (7000,%s)};" % (m, m),
                ]
            )
        )
        fp.write("\n")
        fp.write(
            r"\expandafter ".join(
                [
                    r"",
                    r"\addlegendentry",
                    r"{",
                    r"\rlap",
                    r"{\medianlabel}\vphantom{HAProxy}\hphantom{crash-tolerant variant} (median %s\,µs)}"
                    % (str(m).rstrip("0").rstrip("."),),
                ]
            )
        )
        fp.write("\n")

    with open(sys.stdout.fileno(), "w", encoding="utf-8", newline="") as fp:
        out = csv.writer(fp)
        out.writerow(("parallel", "accept", "write"))
        for row in rows:
            out.writerow(
                [row["parallel"]]
                + [Decimal(row[k]) * 1_000_000 for k in ["accept", "write"]]
            )
