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
  python3 scripts/plot_latency.py lat.csv -o full.png --xmax-pct=100   # x-axis to global max
  python3 scripts/plot_latency.py lat.csv -o lat_log.png --logx   # log x (ns), log-spaced bins
  python3 scripts/plot_latency.py lat.csv --ascii --logx          # same for terminal
  python3 scripts/plot_latency.py lat.csv -o cur.png --style=line  # bin centers with lines
  python3 scripts/plot_latency.py lat.csv -o step.png --style=step --bins=200 # high-res step histogram like HDR
  python3 scripts/plot_latency.py lat.csv -o smooth.png --style=kde  # needs: scipy
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
    style: str,
    xmax_pct: float,
) -> None:
    import numpy as np
    from matplotlib import pyplot as plt

    e2, q2 = _clip(e2e, queue, max_ns)
    e2a = np.asarray(e2, dtype=np.float64)
    qa = np.asarray(q2, dtype=np.float64)

    def display_xlim(pooled: "np.ndarray") -> tuple[float, float]:
        """Shared x-axis for both panels: low end and (by default) a high-percentile cap."""
        if pooled.size == 0:
            return (0.0, 1.0)
        pct = float(np.clip(xmax_pct, 0.01, 100.0))
        x_top = float(np.percentile(pooled, pct))
        if not logx:
            return (0.0, max(x_top * 1.02, 1.0))
        lo = max(1.0, float(np.min(pooled)))
        return (lo * 0.85, max(x_top * 1.08, lo * 1.2))

    def kde_xs_ys(
        data: "np.ndarray", ngrid: int
    ) -> tuple["np.ndarray", "np.ndarray"]:
        """1D Gaussian KDE in linear ns; y is probability density (integrates to 1)."""
        try:
            from scipy.stats import gaussian_kde
        except ImportError as e:  # pragma: no cover
            raise ImportError("KDE style requires SciPy: pip install scipy") from e

        d = np.asarray(data, dtype=np.float64)
        if logx:
            d = np.maximum(d, 1.0)
        if d.size < 2:
            raise ValueError("KDE needs at least 2 samples after clip")
        if float(np.ptp(d)) == 0.0:
            raise ValueError("KDE needs at least 2 distinct values")
        kde = gaussian_kde(d)
        lo, hi = float(d.min()), float(d.max())
        ngrid = max(200, min(2048, ngrid))
        if not logx:
            xs = np.linspace(lo, hi, ngrid)
        else:
            xs = np.logspace(np.log10(lo), np.log10(hi), ngrid)
        ys = kde(xs)
        return xs, ys

    def binned(
        data: "np.ndarray",
    ) -> tuple["np.ndarray", "np.ndarray", float, float, bool]:
        """Return counts, x_centers, x_min, x_max, is_logx_bins."""
        if not logx:
            counts, edges = np.histogram(data, bins=bins)
            x_cent = 0.5 * (edges[:-1] + edges[1:])
            return counts, x_cent, float(edges[0]), float(edges[-1]), False
        d = np.maximum(data, 1.0)
        xmin, xmax = float(d.min()), float(d.max())
        if xmax <= xmin * 1.0001:
            edges = np.array([xmin, max(xmin * 1.01, 2.0)])
        else:
            edges = np.logspace(
                np.log10(xmin), np.log10(xmax), max(2, bins + 1)
            )
        counts, edges = np.histogram(d, bins=edges)
        x_cent = np.sqrt(np.maximum(edges[:-1] * edges[1:], 1e-300))
        return counts, x_cent, xmin, xmax, True

    pooled = np.concatenate((e2a.ravel(), qa.ravel()))
    x_lo, x_hi = display_xlim(pooled)

    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2), sharex=True, sharey=True)
    color = "steelblue"
    grid_kws = {"alpha": 0.3, "linestyle": ":", "linewidth": 0.6}
    for ax, data, ptitle in (
        (axes[0], e2a, "End-to-end latency"),
        (axes[1], qa, "Queue latency"),
    ):
        if style == "kde":
            xs, ys = kde_xs_ys(data, ngrid=bins * 6)
            ax.plot(xs, ys, color=color, linewidth=1.3)
            ax.grid(True, **grid_kws)
        elif style == "bars":
            if not logx:
                ax.hist(
                    data,
                    bins=bins,
                    color=color,
                    edgecolor="white",
                    linewidth=0.3,
                )
            else:
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
                    log=logx,
                    color=color,
                    edgecolor="white",
                    linewidth=0.3,
                )
        elif style == "step":
            ax.set_facecolor("#f0f0f0") # Light grey background like ggplot
            if not logx:
                ax.hist(data, bins=bins, histtype="step", color="#3498db", linewidth=1.5)
            else:
                d = np.maximum(data, 1.0)
                xmin, xmax = float(d.min()), float(d.max())
                if xmax <= xmin * 1.0001:
                    edges = np.array([xmin, max(xmin * 1.01, 2.0)])
                else:
                    edges = np.logspace(
                        np.log10(xmin), np.log10(xmax), max(2, bins + 1)
                    )
                ax.hist(d, bins=edges, log=logx, histtype="step", color="#3498db", linewidth=1.5)
            # Draw Median line
            if data.size > 0:
                median_val = float(np.percentile(data, 50))
                ax.axvline(median_val, color="#e74c3c", linewidth=1.0, alpha=0.8, label=f"{int(median_val)}")
                ax.legend(loc="upper right", frameon=False, fontsize="small", labelcolor="dimgrey")
        elif style == "smooth":
            ax.set_facecolor("#f0f0f0") # Light grey background
            counts, x_cent, _xmin, _xmax, _was_logx = binned(data)
            y = np.asarray(counts, dtype=np.float64)
            # Smooth using simple gaussian-like numpy convolution
            window_size = max(5, bins // 20)
            window = np.exp(-0.5 * (np.arange(window_size) - window_size//2)**2 / (window_size/4)**2)
            window /= window.sum()
            y_padded = np.pad(y, (window_size//2, window_size - 1 - window_size//2), mode='edge')
            y_smooth = np.convolve(y_padded, window, mode='valid')
            
            y_plot = np.where(y_smooth > 0.5, y_smooth, np.nan) if logx else y_smooth
            ax.plot(x_cent, y_plot, color="#3498db", linewidth=1.5)
            # Fill under the curve slightly
            y_fill = np.nan_to_num(y_plot, nan=0.5)
            ax.fill_between(x_cent, y_fill, 0.5, alpha=0.15, color="#3498db")
            
            # Draw Median line
            if data.size > 0:
                median_val = float(np.percentile(data, 50))
                ax.axvline(median_val, color="#e74c3c", linewidth=1.0, alpha=0.8, label=f"{int(median_val)}")
                ax.legend(loc="upper right", frameon=False, fontsize="small", labelcolor="dimgrey")
        else:
            counts, x_cent, _xmin, _xmax, _was_logx = binned(data)
            y = np.asarray(counts, dtype=np.float64)
            if logx:
                y = np.where(y > 0, y, np.nan)
            if style == "line":
                ax.plot(
                    x_cent,
                    y,
                    color=color,
                    linewidth=1.5,
                )
            else:  # dots
                ax.plot(
                    x_cent,
                    y,
                    "o",
                    color=color,
                    markersize=5,
                    linestyle="none",
                )
            ax.grid(True, **grid_kws)
        if logx:
            ax.set_xscale("log")
            if style in ("kde", "line", "dots", "smooth"):
                ax.set_yscale("log")
                if style != "kde":
                    ax.set_ylim(bottom=0.8)
            # style == "bars": ax.hist(..., log=True) set the y axis to log
        ax.set_xlabel("Latency (ns)")
        ax.set_title(ptitle)
    axes[0].set_xlim(x_lo, x_hi)
    if style == "kde":
        axes[0].set_ylabel("probability density")
    else:
        axes[0].set_ylabel("samples")
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
        help="Log scale for latency: PNG uses log x (and log y for counts, i.e. log–log) "
        "with log-spaced x bins; ASCII uses log-spaced bins only",
    )
    ap.add_argument(
        "--style",
        choices=("bars", "line", "dots", "kde", "step", "smooth"),
        default="smooth",
        help="bars: histogram; line/dots: bin centers; kde: smooth density; step: line histogram (HDR); smooth: smoothed line",
    )
    ap.add_argument(
        "--xmax-pct",
        type=float,
        default=99.5,
        metavar="PCT",
        help="PNG x-axis max = PCT percentile of combined e2e+queue (default 99.5, tighter view). "
        "Use 100 to include the global max sample on the x-axis.",
    )
    args = ap.parse_args()

    # Step/smooth styles generally want a lot of bins for the line curve to look good
    if args.style in ("step", "smooth") and "--bins" not in sys.argv:
        args.bins = max(args.bins, 200)

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
            args.style,
            args.xmax_pct,
        )
    except ImportError as e:
        print("PNG: pip install matplotlib numpy", file=sys.stderr)
        if "scipy" in str(e).lower() or "kde" in str(e).lower():
            print("  --style=kde: pip install scipy", file=sys.stderr)
        print(e, file=sys.stderr)
        return 1
    except ValueError as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
