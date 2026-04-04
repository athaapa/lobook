#!/bin/bash
# Usage: ./scripts/bench_runs.sh [N] [binary] [pacing_ns]
# Runs the server binary N times and reports mean, median, and stddev for
# both E2E and queue-only P50/P99/P999.
# Must be run from the build directory, e.g.: cd build && ../scripts/bench_runs.sh 10 ./server 1000

N=${1:-50}
BINARY=${2:-./server}
PACING_NS=${3:-1000}

e2e_p50s=()
e2e_p99s=()
e2e_p999s=()
q_p50s=()
q_p99s=()
q_p999s=()

for i in $(seq 1 "$N"); do
    output=$("$BINARY" "$PACING_NS" 2>/dev/null)

    # E2E: lines before QUEUE LATENCIES header
    e2e_p50=$(echo  "$output" | awk '/QUEUE LATENCIES/{exit} /^p50:/{print $2}')
    e2e_p99=$(echo  "$output" | awk '/QUEUE LATENCIES/{exit} /^p99:/{print $2}')
    e2e_p999=$(echo "$output" | awk '/QUEUE LATENCIES/{exit} /^p999:/{print $2}')

    # Queue-only: lines after QUEUE LATENCIES header
    q_p50=$(echo  "$output" | awk '/QUEUE LATENCIES/{found=1; next} found && /^p50:/{print $2}')
    q_p99=$(echo  "$output" | awk '/QUEUE LATENCIES/{found=1; next} found && /^p99:/{print $2}')
    q_p999=$(echo "$output" | awk '/QUEUE LATENCIES/{found=1; next} found && /^p999:/{print $2}')

    e2e_p50s+=("$e2e_p50")
    e2e_p99s+=("$e2e_p99")
    e2e_p999s+=("$e2e_p999")
    q_p50s+=("$q_p50")
    q_p99s+=("$q_p99")
    q_p999s+=("$q_p999")

    echo "run $i: e2e_p50=$e2e_p50  queue_p50=$q_p50"
done

python3 - "${e2e_p50s[@]}" "${e2e_p99s[@]}" "${e2e_p999s[@]}" \
          "${q_p50s[@]}"   "${q_p99s[@]}"   "${q_p999s[@]}" "$N" <<'EOF'
import sys, statistics

args = sys.argv[1:]
n = int(args[-1])
all_vals = [int(x) for x in args[:-1]]
e2e_p50s  = all_vals[0*n:1*n]
e2e_p99s  = all_vals[1*n:2*n]
e2e_p999s = all_vals[2*n:3*n]
q_p50s    = all_vals[3*n:4*n]
q_p99s    = all_vals[4*n:5*n]
q_p999s   = all_vals[5*n:6*n]

def stats(label, vals):
    print(f"\n{label}")
    print(f"  mean:   {statistics.mean(vals):.0f} ns")
    print(f"  median: {statistics.median(vals):.0f} ns")
    print(f"  stdev:  {statistics.stdev(vals):.0f} ns")
    print(f"  min:    {min(vals)} ns")
    print(f"  max:    {max(vals)} ns")

print("\n=== END-TO-END LATENCIES ===")
stats("P50",  e2e_p50s)
stats("P99",  e2e_p99s)
stats("P999", e2e_p999s)

print("\n=== QUEUE-ONLY LATENCIES ===")
stats("P50",  q_p50s)
stats("P99",  q_p99s)
stats("P999", q_p999s)
EOF
