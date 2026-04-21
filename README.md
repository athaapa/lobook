# lobook

Low-latency limit order book matching engine in C++.

End-to-end p50 latency: **170 ns** (lock-free SPSC + matching engine, 1 μs pacing). 11.5× faster than a mutex + condvar baseline.

All numbers in this README were measured on the same host in a single session: bare-metal AMD Ryzen 5 2600 @ 3.4 GHz, `isolcpus=6-11`, Fedora kernel 6.19, gcc 15.2 `-O3 -D_GNU_SOURCE`.

## Architecture

One producer thread (the network ingress simulator) pushes timestamped `OrderMessage`s into an SPSC ring buffer; the matching engine pops them on a pinned consumer core, runs them against the book, and records per-message end-to-end latency. The book holds order pool memory in a fixed-capacity arena — no heap allocation on the hot path.

- **Ladder book** — `PriceLevel[MAX_PRICES]` array; orders are intrusive doubly-linked nodes from a fixed-capacity pool indexed by `uint32_t`. Replaces an `std::map<price, list<Order>>` baseline.
- **Hierarchical bitset** — 3-level bitmask over the ladder. `_tzcnt_u64` / `_lzcnt_u64` find the best bid/ask in O(1) regardless of where it sits on the ladder.
- **SPSC ring buffer** — lock-free, cache-line padded, with Rigtorp's cached-index optimization that eliminates one cross-core MESI transfer on the common (non-empty / non-full) fast path.
- **Native cancel/replace** — preserves FIFO price-time priority on size decrease; resets it on price change or size increase. ~2.4× faster than `cancel + submit`.
- **CPU pinning** — `pthread_setaffinity_np` against an `isolcpus` set.

## Measured results

### End-to-end latency

`./server --queue=<mode> --pacing-ns=1000 --workload=100000`

| queue mode                | p50      | p99     | p999     |
| ---                       | ---      | ---     | ---      |
| **SPSC, cached indices**  | **170 ns** | 1.97 μs | 4.27 μs  |
| SPSC, uncached            | 200 ns   | 2.01 μs | 6.45 μs  |
| mutex + condvar (naive)   | 1.95 μs  | 9.82 μs | 34.77 μs |

Cached SPSC is **11.5× faster than the mutex baseline at p50**, and **8.1× faster at p999** — the futex wakeup floor dominates the mutex path's tail. Caching the indices saves one MESI round-trip on the fast path: ~30 ns at p50 vs uncached, and a 33% improvement at p999. Queue-only latency (push → pop) is ~20 ns less than end-to-end across all rows; the engine itself adds ~20 ns.

### `replace` vs `cancel + submit`

1M random replace operations against a 10k-order book; bid and ask price ranges are disjoint so `cancel + submit` cannot accidentally cross.

| path                 | ns/op    | insn/op | IPC  | branch miss % | L1D miss/op |
| ---                  | ---      | ---     | ---  | ---           | ---         |
| **native replace**   | **20.3** | 73.8    | 1.11 | 1.66 %        | 6.43        |
| cancel + submit      | 48.0     | 137.6   | 0.88 | 3.42 %        | 6.79        |

Native replace is **2.36× faster**, driven by **1.86× fewer instructions** (single id-map lookup, single bitset update) and **26% higher IPC** (less branch-prediction pressure: 1.66 % vs 3.42 % miss rate). L1D miss counts are nearly identical — the gap is compute and pipeline efficiency, not memory.

### Hierarchical bitset vs linear ladder scan

Same book interface, two implementations: `FastBook` (linear scan over 100k price levels in `match()`) vs `FastBitsetBook` (bitset lookup). Three workloads:

| scenario                                                  | FastBook ns/op | BitsetBook ns/op | speedup    |
| ---                                                       | ---            | ---              | ---        |
| sparse insert (200k orders into 50 levels)                | 53,179         | 8.12             | **6,548×** |
| wide-spread match (10k asks at top, 10k crossing buys)    | 171,712        | 9.58             | **17,925×** |
| mixed churn (100k random submit/cancel/aggressive)        | 58,353         | 48.17            | **1,212×** |

`FastBook::match()` scans the full 100k ladder per call; `FastBitsetBook` does one `tzcnt` to jump to the best price. Speedups scale linearly with `MAX_PRICES`; on a smaller ladder the gap closes proportionally.

### Clock overhead

`clock_gettime(CLOCK_MONOTONIC_RAW)` p50 = **20 ns** (vDSO-accelerated). This is the floor of any wall-clock measurement here; latencies below ~20 ns are not resolvable with this clock.

## Methodology

- **Hardware counters** via `perf_event_open`: instructions, cycles, IPC, branches, branch-misses, L1D read-misses, LLC refs/misses.
- **Region-level timing** via `__rdtsc`; invariant TSC frequency calibrated against `CLOCK_MONOTONIC_RAW` at startup (no hardcoded GHz).
- **Wall-clock timing** via `clock_gettime(CLOCK_MONOTONIC_RAW)` — chosen over `CLOCK_MONOTONIC` to avoid NTP slewing during measurement.
- **Producer pacing** is a busy-spin on `clock_gettime`, not `nanosleep` (sleep wakeup dwarfs the queue cost being measured).
- **Core isolation** via kernel command-line `isolcpus=6-11`; benchmark threads pinned with `pthread_setaffinity_np`.

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
