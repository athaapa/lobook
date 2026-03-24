#!/bin/bash
# Usage: ./scripts/bench_runs.sh [N] [binary]
# Runs the server binary N times and reports mean, median, and stddev for P50/P99/P999.
# Must be run from the build directory, e.g.: cd build && ../scripts/bench_runs.sh 10 ./server

N=${1:-50}
BINARY=${2:-./server}

p50s=()
p99s=()
p999s=()

for i in $(seq 1 "$N"); do
    output=$("$BINARY" 2>/dev/null)
    p50=$(echo "$output"  | awk '/^p50:/  {print $2}')
    p99=$(echo "$output"  | awk '/^p99:/  {print $2}')
    p999=$(echo "$output" | awk '/^p999:/ {print $2}')
    p50s+=("$p50")
    p99s+=("$p99")
    p999s+=("$p999")
    echo "run $i: p50=$p50  p99=$p99  p999=$p999"
done

python3 - "${p50s[@]}" "${p99s[@]}" "${p999s[@]}" "$N" <<'EOF'
import sys, statistics

args = sys.argv[1:]
n = int(args[-1])
all_vals = [int(x) for x in args[:-1]]
p50s  = all_vals[0*n:1*n]
p99s  = all_vals[1*n:2*n]
p999s = all_vals[2*n:3*n]

def stats(label, vals):
    print(f"\n{label}")
    print(f"  mean:   {statistics.mean(vals):.0f} ns")
    print(f"  median: {statistics.median(vals):.0f} ns")
    print(f"  stdev:  {statistics.stdev(vals):.0f} ns")
    print(f"  min:    {min(vals)} ns")
    print(f"  max:    {max(vals)} ns")

stats("P50",  p50s)
stats("P99",  p99s)
stats("P999", p999s)
EOF
