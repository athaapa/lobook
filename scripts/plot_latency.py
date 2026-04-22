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
  python3 scripts/plot_latency.py lat.csv -o lat_log.png --logx   # log x (ns), log-spaced bins
  python3 scripts/plot_latency.py lat.csv --ascii --logx          # same for terminal
"""

from __future__ import annotations

import argparse
import csv
import math
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


def _bin_index_log(v: int, log_lo: float, log_hi: float, n_bins: int) -> int:
    v = max(1, v)
    lv = math.log10(v)
    if log_hi <= log_lo:
        return 0
    t = (lv - log_lo) / (log_hi - log_lo)
    idx = int(t * n_bins)
    if idx < 0:
        return 0
    if idx >= n_bins:
        return n_bins - 1
    return idx


def _ascii_one(
    name: str, values: List[int], n_bins: int, bar_width: int, logx: bool
) -> None:
    if not values:
        print(f"--- {name} --- (no samples after clip)")
        return
    raw_lo, raw_hi = min(values), max(values)
    if raw_lo == raw_hi:
        print(f"--- {name} ---  n={len(values)}  all at {raw_lo} ns")
        return
    n_bins = max(1, n_bins)
    counts = [0] * n_bins
    if not logx:
        for v in values:
            counts[_bin_index(v, raw_lo, raw_hi, n_bins)] += 1
    else:
        lo, hi = max(1, raw_lo), max(1, raw_hi)
        log_lo, log_hi = math.log10(float(lo)), math.log10(float(hi))
        for v in values:
            counts[_bin_index_log(v, log_lo, log_hi, n_bins)] += 1
    mx = max(counts) or 1
    print(f"--- {name} ---  n={len(values)}  ns in [{raw_lo}, {raw_hi}]")
    if logx:
        print("    (log-spaced bins on x, linear count on y)")
    span_linear = raw_hi - raw_lo
    for i, c in enumerate(counts):
        if not logx:
            left = raw_lo + (span_linear * i) // n_bins
            right = (
                raw_lo + (span_linear * (i + 1)) // n_bins
                if i < n_bins - 1
                else raw_hi
            )
        else:
            log_lo = math.log10(float(max(1, raw_lo)))
            log_hi = math.log10(float(raw_hi))
            if log_hi <= log_lo + 1e-15:
                left, right = raw_lo, raw_hi
            else:
                t0, t1 = i / n_bins, (i + 1) / n_bins
                left = int(10 ** (log_lo + t0 * (log_hi - log_lo)))
                right = int(10 ** (log_lo + t1 * (log_hi - log_lo)))
                if i == n_bins - 1:
                    right = raw_hi
        n_hash = 0 if c == 0 else max(1, c * bar_width // mx)
        bar = "#" * n_hash
        print(f"  {left:9d} .. {right:9d}  |{bar} {c}")


def run_ascii(
    e2e: List[int],
    queue: List[int],
    bins: int,
    max_ns: Optional[float],
    bar_width: int,
    logx: bool,
) -> None:
    e2, q2 = _clip(e2e, queue, max_ns)
    _ascii_one("End-to-end latency (ns)", e2, bins, bar_width, logx)
    print()
    _ascii_one("Queue latency (ns)", q2, bins, bar_width, logx)


def run_png(
    e2e: List[int],
    queue: List[int],
    out: str,
    title: str,
    bins: int,
    max_ns: Optional[float],
    logx: bool,
) -> None:
    import numpy as np
    from matplotlib import pyplot as plt

    e2, q2 = _clip(e2e, queue, max_ns)
    e2a = np.asarray(e2, dtype=np.float64)
    qa = np.asarray(q2, dtype=np.float64)

    fig, axes = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
    for ax, data, ptitle in (
        (axes[0], e2a, "End-to-end latency (ns)"),
        (axes[1], qa, "Queue latency (ns)"),
    ):
        if not logx:
            ax.hist(
                data,
                bins=bins,
                color="steelblue",
                edgecolor="white",
                linewidth=0.3,
            )
            ax.set_xlabel("ns")
            if max_ns is not None:
                ax.set_xlim(0, max_ns)
        else:
            # Log scale on latency axis: positive samples only, log-spaced bin edges.
            d = np.maximum(data, 1.0)
            xmin, xmax = float(d.min()), float(d.max())
            if xmax <= xmin * 1.0001:
                edges = np.array([xmin, max(xmin * 1.01, 2.0)])
            else:
                edges = np.logspace(
                    np.log10(xmin), np.log10(xmax), max(2, bins + 1)
                )
            ax.hist(
                d,
                bins=edges,
                color="steelblue",
                edgecolor="white",
                linewidth=0.3,
            )
            ax.set_xscale("log")
            ax.set_xlabel("ns (log scale)")
            ax.set_xlim(xmin * 0.8, xmax * 1.2)
        ax.set_title(ptitle)
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
    ap.add_argument(
        "--logx",
        action="store_true",
        help="Log scale on the latency (x) axis; bins are log-spaced in ns (PNG) or in ASCII",
    )
    args = ap.parse_args()

    if not args.ascii:
        os.environ.setdefault("MPLBACKEND", "Agg")

    e2e, queue = load_csv(args.csv)
    if args.ascii:
        run_ascii(
            e2e,
            queue,
            args.bins,
            args.max_ns,
            args.bar_width,
            args.logx,
        )
        return 0
    try:
        run_png(
            e2e,
            queue,
            args.output,
            os.path.basename(args.csv),
            args.bins,
            args.max_ns,
            args.logx,
        )
    except ImportError as e:
        print("PNG mode needs: pip install matplotlib numpy", file=sys.stderr)
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
