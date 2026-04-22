#!/usr/bin/env python3
"""
Read CSV from `server --dump-latencies=FILE` and either:
  - print a histogram in the terminal (--ascii) — no matplotlib, no file to copy over SSH, or
  - write a PNG (default) using matplotlib + numpy.

On a remote host over SSH, use --ascii for instant feedback; for slides, generate a small PNG
on the same host and `scp` only that file (tens of KB), not a multi-MB CSV.

Simplest one-step:
  ./scripts/latency_hist.sh --workload=100000
  ./scripts/latency_hist.sh --ascii --workload=100000   # no PNG, no scp of data

Custom PNG (CSV kept on host):
  ./build/server --dump-latencies=lat.csv --workload=100000
  python3 scripts/plot_latency.py lat.csv -o lat.png --max-ns=5000
"""

from __future__ import annotations

import argparse
import csv
import os
import sys
from typing import List, Optional, Sequence, Tuple


def load_csv(path: str) -> Tuple[List[int], List[int]]:
    e2e: list[int] = []
    queue: list[int] = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            e2e.append(int(row["e2e_ns"]))
            queue.append(int(row["queue_ns"]))
    return e2e, queue


def _clip(
    e2e: Sequence[int], queue: Sequence[int], max_ns: Optional[float]
) -> Tuple[List[int], List[int]]:
    if max_ns is None:
        return list(e2e), list(queue)
    return (
        [x for x in e2e if x <= max_ns],
        [x for x in queue if x <= max_ns],
    )


def _bin_index(v: int, lo: int, hi: int, n_bins: int) -> int:
    if hi == lo:
        return 0
    idx = (v - lo) * n_bins // (hi - lo)
    if idx < 0:
        return 0
    if idx >= n_bins:
        return n_bins - 1
    return idx


def _ascii_one(name: str, values: List[int], n_bins: int, bar_width: int) -> None:
    if not values:
        print(f"--- {name} --- (no samples after clip)")
        return
    lo, hi = min(values), max(values)
    if lo == hi:
        print(f"--- {name} ---  n={len(values)}  all at {lo} ns")
        return
    n_bins = max(1, n_bins)
    counts = [0] * n_bins
    for v in values:
        counts[_bin_index(v, lo, hi, n_bins)] += 1
    mx = max(counts) or 1
    span = hi - lo
    print(f"--- {name} ---  n={len(values)}  ns in [{lo}, {hi}]")
    for i, c in enumerate(counts):
        left = lo + (span * i) // n_bins
        right = lo + (span * (i + 1)) // n_bins if i < n_bins - 1 else hi
        n_hash = 0 if c == 0 else max(1, c * bar_width // mx)
        bar = "#" * n_hash
        print(f"  {left:9d} .. {right:9d}  |{bar} {c}")


def run_ascii(
    e2e: List[int], queue: List[int], bins: int, max_ns: Optional[float], bar_width: int
) -> None:
    e2, q2 = _clip(e2e, queue, max_ns)
    _ascii_one("End-to-end latency (ns)", e2, bins, bar_width)
    print()
    _ascii_one("Queue latency (ns)", q2, bins, bar_width)


def run_png(
    e2e: List[int],
    queue: List[int],
    out: str,
    title: str,
    bins: int,
    max_ns: Optional[float],
) -> None:
    import numpy as np
    from matplotlib import pyplot as plt

    e2, q2 = _clip(e2e, queue, max_ns)
    e2a = np.asarray(e2, dtype=np.int64)
    qa = np.asarray(q2, dtype=np.int64)
    fig, axes = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
    for ax, data, title in (
        (axes[0], e2a, "End-to-end latency (ns)"),
        (axes[1], qa, "Queue latency (ns)"),
    ):
        ax.hist(data, bins=bins, color="steelblue", edgecolor="white", linewidth=0.3)
        ax.set_title(title)
        ax.set_xlabel("ns")
        if max_ns is not None:
            ax.set_xlim(0, max_ns)
    axes[0].set_ylabel("count")
    fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Histogram from server latency CSV.")
    ap.add_argument("csv", help="Path to e2e_ns,queue_ns CSV")
    ap.add_argument(
        "--ascii",
        action="store_true",
        help="Print histograms in the terminal (stdlib only; use over SSH, no scp).",
    )
    ap.add_argument("-o", "--output", default="latency_hist.png", help="Output PNG path")
    ap.add_argument("--bins", type=int, default=40, help="Number of bins")
    ap.add_argument(
        "--bar-width",
        type=int,
        default=50,
        help="With --ascii: maximum bar width in characters",
    )
    ap.add_argument(
        "--max-ns",
        type=float,
        default=None,
        help="Ignore samples above this (ns) for a clearer main body",
    )
    args = ap.parse_args()

    if not args.ascii:
        os.environ.setdefault("MPLBACKEND", "Agg")

    e2e, queue = load_csv(args.csv)
    if args.ascii:
        run_ascii(e2e, queue, args.bins, args.max_ns, args.bar_width)
        return 0
    try:
        run_png(
            e2e,
            queue,
            args.output,
            os.path.basename(args.csv),
            args.bins,
            args.max_ns,
        )
    except ImportError as e:
        print("PNG mode needs: pip install matplotlib numpy", file=sys.stderr)
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
