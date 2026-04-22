#!/usr/bin/env bash
# One command: run the latency harness, write a temp CSV, plot, delete the CSV.
#
#   ./scripts/latency_hist.sh
#   ./scripts/latency_hist.sh --workload=100000 --pacing-ns=1000 --queue=uncached
#   ./scripts/latency_hist.sh --ascii --workload=100000
#     # ^ print histogram in this SSH session (no PNG, no matplotlib, nothing to scp)
#   ./scripts/latency_hist.sh --logx --workload=100000
#     # ^ PNG with log x-axis (ns)
#   ./scripts/latency_hist.sh --style=line --workload=100000
#     # line + markers (or --style=dots)
#   ./scripts/latency_hist.sh --style=kde --workload=100000
#     # smooth density (needs: pip install scipy)
#
# LOBOOK_SERVER   path to server (default: <repo>/build/server)
# LOBOOK_PLOT_OUT output PNG      (default: ./latency_hist.png; ignored with --ascii)

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
SERVER="${LOBOOK_SERVER:-$ROOT/build/server}"
OUT="${LOBOOK_PLOT_OUT:-$PWD/latency_hist.png}"
ASCII=0
LOGX=0
STYLE=
BINS=
XMAX_PCT=
while [[ $# -gt 0 ]]; do
  case $1 in
    --ascii)            ASCII=1; shift ;;
    --logx)             LOGX=1;  shift ;;
    --style=*)          STYLE="${1#--style=}"; shift ;;
    --bins=*)           BINS="${1#--bins=}"; shift ;;
    --xmax-pct=*)       XMAX_PCT="${1#--xmax-pct=}"; shift ;;
    *)                  break   ;;
  esac
done
CSV="$(mktemp "${TMPDIR:-/tmp}/lobook_latency.XXXXXX.csv")"
cleanup() { rm -f "$CSV"; }
trap cleanup EXIT

"$SERVER" --dump-latencies="$CSV" "$@"
extra=()
if [[ "$LOGX" -eq 1 ]]; then
  extra+=(--logx)
fi
if [[ -n "$STYLE" ]]; then
  extra+=(--style "$STYLE")
fi
if [[ -n "$BINS" ]]; then
  extra+=(--bins "$BINS")
fi
if [[ -n "$XMAX_PCT" ]]; then
  extra+=(--xmax-pct "$XMAX_PCT")
fi
if [[ "$ASCII" -eq 1 ]]; then
  python3 "$ROOT/scripts/plot_latency.py" "${extra[@]}" --ascii "$CSV"
else
  python3 "$ROOT/scripts/plot_latency.py" "${extra[@]}" "$CSV" -o "$OUT"
fi
