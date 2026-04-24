# lobook

Low-latency limit order book matching engine in C++.

End-to-end p50 latency: **180 ns** (lock-free SPSC + matching engine, 1 µs pacing, N=10 runs). 10.5× faster than a mutex + condvar baseline.

All numbers were measured on bare-metal AMD Ryzen 5 2600 @ 3.4 GHz, `isolcpus=6-11`, Fedora kernel 6.19, gcc 15.2 `-O3 -D_GNU_SOURCE`.

## Architecture

One producer thread pushes timestamped `OrderMessage`s into a lock-free SPSC ring buffer. The matching engine pops them on a pinned consumer core, runs them against the book, and records per-message end-to-end latency.

The book uses a flat `PriceLevel[MAX_PRICES]` array with orders as intrusive doubly-linked nodes from a fixed-capacity pool — no heap allocation on the hot path. A 3-level hierarchical bitset sits on top of the ladder so `find_best_bid()` and `find_best_ask()` are O(1) via `tzcnt`/`lzcnt` regardless of book sparsity. The SPSC queue is cache-line padded with Rigtorp's cached-index optimization to avoid a cross-core MESI round-trip on the common fast path. Replace is implemented natively: priority is preserved on size decrease, reset on price change or size increase.

## Measured results

### End-to-end latency

`./server --queue=<mode> --pacing-ns=1000 --workload=100000`. N=10 runs, median-of-medians.

| queue mode                | p50        | p99     | p999      |
| ---                       | ---        | ---     | ---       |
| **SPSC, cached indices**  | **180 ns** | 1.99 µs | 6.46 µs   |
| SPSC, uncached            | 190 ns     | 2.00 µs | 6.51 µs   |
| mutex + condvar (naive)   | 1.89 µs    | 9.64 µs | 19.96 µs  |

Cached SPSC is **10.5× faster than the mutex baseline at p50** and **3.1× at p999**. The futex wakeup and scheduler wakeup cost is paid on every push, not just occasionally, so the gap shows up across the whole distribution. At 1 µs pacing, cached and uncached are nearly indistinguishable at p50 (10 ns gap, within noise) because the queue stays near-empty. The stability difference only appears under load — see the pacing sweep below.

### Cached vs uncached SPSC indices: the stability story

At 1 µs pacing the two implementations are nearly identical. Under heavier producer pressure they diverge catastrophically.

`./server --queue=<mode> --pacing-ns=<N>`, 10 runs per cell, median-of-medians p50:

| pacing  | cached p50 | uncached p50 | uncached saturated runs  |
| ---     | ---        | ---          | ---                      |
| 1 µs    | 180 ns     | 190 ns       | 0 / 10                   |
| 100 ns  | 185 ns     | **214 µs**   | 6 / 10 (200–500 µs P50)  |
| 10 ns   | 1.1 µs     | **1.94 µs**  | 4 / 10 (2–4 ms P50)      |

At 100 ns pacing, uncached enters a **metastable saturation mode** on 6 of 10 runs. Every uncached push pays one cross-core MESI round-trip (~60–80 ns), which caps producer throughput below what the consumer needs to drain the queue. Once the queue fills, the producer blocks on queue-full and end-to-end latency balloons by hundreds of µs.

Cached indices avoid this. With `head_cached_` in L1, push cost is near-zero and the producer can keep the queue drained even under bursts. The real value of the optimization isn't the ~10 ns steady-state gap — it's preventing a failure mode that doesn't show up in low-load benchmarks at all.

### `replace` vs `cancel + submit`

1M random replace operations against a 10k-order book; bid and ask price ranges are disjoint so `cancel + submit` cannot accidentally cross.

| path                 | ns/op    | insn/op | IPC  | branch miss % | L1D miss/op |
| ---                  | ---      | ---     | ---  | ---           | ---         |
| **native replace**   | **20.3** | 73.8    | 1.11 | 1.66 %        | 6.43        |
| cancel + submit      | 48.0     | 137.6   | 0.88 | 3.42 %        | 6.79        |

Native replace is **2.36× faster**, driven by **1.86× fewer instructions** and **26% higher IPC**. L1D miss counts are nearly identical — the gap is compute and pipeline efficiency, not memory. Native replace is also semantically stronger: cancel + submit always resets priority, so it cannot express the priority-preserving size-decrease case at all.

### Hierarchical bitset vs linear ladder scan

Same book interface, two implementations: `FastBook` (linear scan over 100k price levels in `match()`) vs `FastBitsetBook` (bitset lookup). Three workloads:

| scenario                                                  | FastBook ns/op | BitsetBook ns/op | speedup    |
| ---                                                       | ---            | ---              | ---        |
| sparse insert (200k orders into 50 levels)                | 53,179         | 8.12             | **6,548×** |
| wide-spread match (10k asks at top, 10k crossing buys)    | 171,712        | 9.58             | **17,925×** |
| mixed churn (100k random submit/cancel/aggressive)        | 58,353         | 48.17            | **1,212×** |

`FastBook::match()` scans the full 100k ladder per call; `FastBitsetBook` does one `tzcnt` to jump to the best price. Speedups scale with `MAX_PRICES` — on a smaller ladder the gap closes proportionally.

### Clock overhead

`clock_gettime(CLOCK_MONOTONIC_RAW)` p50 = **20 ns** (vDSO-accelerated). This is the floor of any wall-clock measurement here; latencies below ~20 ns are not resolvable with this clock.

## Methodology

- **Hardware counters** via `perf_event_open`: instructions, cycles, IPC, branches, branch-misses, L1D read-misses, LLC refs/misses.
- **Region-level timing** via `__rdtsc`; invariant TSC frequency calibrated against `CLOCK_MONOTONIC_RAW` at startup (no hardcoded GHz).
- **Wall-clock timing** via `clock_gettime(CLOCK_MONOTONIC_RAW)`, chosen over `CLOCK_MONOTONIC` to avoid NTP slewing during measurement.
- **Producer pacing** is a busy-spin on `clock_gettime`, not `nanosleep` (sleep wakeup dwarfs the queue cost being measured).
- **Core isolation** via kernel command-line `isolcpus=6-11`; benchmark threads pinned with `pthread_setaffinity_np`.
- **N=10 runs**, median-of-medians. Single runs have too much variance from transient OS noise.

Every benchmark emits a standardized environment banner (host, CPU, calibrated TSC, isolcpus, kernel, compiler) before results, so logs are reproducible and grep-able.

## Build & run

Linux x86-64 only (uses `perf_event_open`, `pthread_setaffinity_np`, `/proc`, `rdtsc`).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLOBOOK_BUILD_BENCHMARKS=ON \
  -DLOBOOK_BUILD_TESTS=ON
cmake --build build -j
```

```bash
# end-to-end latency harness
./build/server --queue=cached|uncached|naive --pacing-ns=N --workload=N

# microbenchmarks
./build/benchmark_book       # FastBook vs FastBitsetBook
./build/benchmark_replace    # replace vs cancel + submit
./build/benchmark_clock      # clock_gettime overhead floor
```

## Repo layout

```
src/
  bench_common.h           Timer, perf counters, TSC calibration, environment banner
  benchmark_book.cpp       FastBook vs FastBitsetBook
  benchmark_replace.cpp    native replace vs cancel + submit
  benchmark_clock.cpp      clock_gettime overhead
  server.cpp               end-to-end latency harness
  bench_runners/           per-book interface shims (separate TUs for ODR)
  engine/
    fast_bitset_book.h     production book (ladder + hierarchical bitset)
    fast_book.h            baseline ladder book (linear price scan)
    spsc_queue.h           lock-free ring (cached/uncached via bool template parameter)
    naive_queue.h          mutex + condvar baseline
    matching_engine.h      pinned consumer thread, latency capture
    price_bitset.h         3-level hierarchical bitmask
    cpu_pinning.h          pthread_setaffinity_np helpers
    order_message.h        wire format
tests/                     correctness tests (price-time priority, crossing, cancel)
```
