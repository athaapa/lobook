#!/usr/bin/env bash
# One command: run the latency harness, write a temp CSV, plot, delete the CSV.
#
#   ./scripts/latency_hist.sh
#   ./scripts/latency_hist.sh --workload=100000 --pacing-ns=1000 --queue=uncached
#   ./scripts/latency_hist.sh --ascii --workload=100000
#     # ^ print histogram in this SSH session (no PNG, no matplotlib, nothing to scp)
#
# LOBOOK_SERVER   path to server (default: <repo>/build/server)
# LOBOOK_PLOT_OUT output PNG      (default: ./latency_hist.png; ignored with --ascii)

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
SERVER="${LOBOOK_SERVER:-$ROOT/build/server}"
OUT="${LOBOOK_PLOT_OUT:-$PWD/latency_hist.png}"
ASCII=0
if [[ "${1-}" == --ascii ]]; then
  ASCII=1
  shift
fi
CSV="$(mktemp "${TMPDIR:-/tmp}/lobook_latency.XXXXXX.csv")"
cleanup() { rm -f "$CSV"; }
trap cleanup EXIT

"$SERVER" --dump-latencies="$CSV" "$@"
if [[ "$ASCII" -eq 1 ]]; then
  python3 "$ROOT/scripts/plot_latency.py" --ascii "$CSV"
else
  python3 "$ROOT/scripts/plot_latency.py" "$CSV" -o "$OUT"
  echo "Wrote $OUT"
fi
