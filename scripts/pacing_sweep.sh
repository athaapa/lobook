#!/bin/bash
# Usage: ./scripts/pacing_sweep.sh [N] [build_dir] [results_dir]
# Runs server_cached and server_nocached at 4 pacings (2000, 1000, 500, 100 ns),
# N runs each, saving output to results_dir.

set -euo pipefail

N=${1:-50}
BUILD_DIR=${2:-build}
RESULTS_DIR=${3:-results}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

mkdir -p "$REPO_DIR/$RESULTS_DIR"

PACINGS=(2000 1000 500 100)
BINARIES=(server_cached server_nocached)

for pacing in "${PACINGS[@]}"; do
    for binary in "${BINARIES[@]}"; do
        out="$REPO_DIR/$RESULTS_DIR/attu_${binary}_${pacing}.txt"
        echo "=== $binary @ ${pacing}ns pacing ($N runs) ===" | tee "$out"
        "$REPO_DIR/scripts/bench_runs.sh" "$N" \
            "$REPO_DIR/$BUILD_DIR/$binary" "$pacing" | tee -a "$out"
        echo ""
    done
done

echo "=== SWEEP COMPLETE. Summary (queue-only P50 medians) ==="
echo ""
printf "%-12s %-18s %-18s %-8s\n" "pacing" "cached_q_p50" "nocached_q_p50" "G(ns)"
for pacing in "${PACINGS[@]}"; do
    cached_file="$REPO_DIR/$RESULTS_DIR/attu_server_cached_${pacing}.txt"
    nocached_file="$REPO_DIR/$RESULTS_DIR/attu_server_nocached_${pacing}.txt"

    # Extract queue-only P50 median from each file
    # Format: "  median: 186 ns"
    c_med=$(awk '/QUEUE-ONLY LATENCIES/,0' "$cached_file"  | awk '/^P50/,/^P99/' | awk '/median:/{print $2; exit}')
    n_med=$(awk '/QUEUE-ONLY LATENCIES/,0' "$nocached_file" | awk '/^P50/,/^P99/' | awk '/median:/{print $2; exit}')

    if [[ -n "$c_med" && -n "$n_med" ]]; then
        g=$(( n_med - c_med ))
        printf "%-12s %-18s %-18s %-8s\n" "${pacing}ns" "${c_med}ns" "${n_med}ns" "${g}ns"
    else
        printf "%-12s %-18s %-18s %-8s\n" "${pacing}ns" "?" "?" "?"
    fi
done
