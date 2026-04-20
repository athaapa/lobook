# lobook

Low-latency limit order book matching engine in C++. Built as a vehicle for measurement-driven systems work — every optimization is paired with a cost model, a prediction, and a measured delta.

**Queue-only P50 latency: 174 ns** (EC2 c5 isolated, cached-index SPSC, 1 μs pacing). 80× faster than a mutex baseline.

## Architecture

- **Ladder book** — flat `PriceLevel[100k]` array replacing `std::map`. Eliminates pointer chasing; ~3.7× fewer L1D misses and ~67× fewer TLB misses vs the sparse baseline.
- **Hierarchical bitset** — 3-level bitmask over the price ladder for O(1) best bid/ask via `_tzcnt_u64`/`_lzcnt_u64`.
- **SPSC ring buffer** — lock-free, cache-line padded, with Rigtorp cached head/tail to eliminate one cross-core MESI transfer per push/pop.
- **Native cancel/replace** — preserves FIFO price-time priority on size decrease; resets priority on price change or size increase. Faster than a cancel+submit pair (see below).
- **CPU pinning** — `pthread_setaffinity_np` + `isolcpus` + `nohz_full` + `rcu_nocbs` for bimodal-latency elimination on the matching core.

## Measured results

All numbers on attu (Intel Xeon Gold 6132 @ 2.6 GHz, shared), unless noted.

### End-to-end latency (SPSC + matching engine, 1 μs pacing)

| Variant | P50 | Notes |
|---|---|---|
| `std::mutex` baseline | ~18 μs | — |
| Lock-free SPSC, no Rigtorp | 242 ns | One cross-core `tail_` read per pop |
| Lock-free SPSC + Rigtorp | **184 ns** | Eliminates one MESI round-trip |
| Queue-only (push→pop) P50 | 174 ns | Isolates coherence from engine cost |

### Replace primitive vs cancel+submit (1M random ops, segregated prices)

| Path | ns/op | insn/op | IPC | Branch miss % |
|---|---|---|---|---|
| **Native replace** | **20.2** | 75.5 | 1.14 | 1.47% |
| Cancel + submit | 47.5 | 135.7 | 0.86 | 3.35% |

Native replace is 2.35× faster: 1.80× fewer instructions (single lookup vs double) and 25% higher IPC (fewer branch mispredicts). L1D miss counts are nearly equal across paths — the gap is compute + pipeline, not memory.

## Methodology

Every optimization follows a fixed loop: **cost model → numeric prediction → measure → log delta**. Predictions are recorded in `misc/predictions.md` before measurement; outcomes and corrections follow. The prediction log is the primary learning artifact — wrong predictions with a well-articulated mechanism are valued over correct ones without.

Tooling: `perf_event_open` for hardware counters (cycles, instructions, IPC, L1D misses, branch mispredicts), `__rdtsc` for region-level timing, `CLOCK_MONOTONIC_RAW` for wall-clock percentiles. See `src/benchmark_replace.cpp` for a representative harness.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Default targets: `server`, `server_cached`, `server_nocached` (latency harness binaries).

Optional:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLOBOOK_BUILD_BENCHMARKS=ON \
  -DLOBOOK_BUILD_BENCHMARK_SPARSE=ON \
  -DLOBOOK_BUILD_TESTS=ON
cmake --build build -j
```

## Repo layout

```
src/engine/           core matching engine + data structures
  fast_bitset_book.h  ladder + hierarchical bitset book
  spsc_queue.h        Rigtorp-cached SPSC ring buffer
  matching_engine.h   consumer thread, latency capture
  price_bitset.h      3-level hierarchical bitmask
  cpu_pinning.h       core affinity helpers

src/benchmark_*.cpp   microbenchmarks (ladder, sparse, replace)
src/server.cpp        end-to-end latency harness (producer + engine)
tests/                correctness tests (price-time priority, crossing)
misc/                 predictions log, progress writeup, design notes
```

## Target hardware

Developed on macOS (M4); benchmarked on Linux x86-64 (attu shared cluster, EC2 c5 with `isolcpus`, bare-metal Fedora on Ryzen 5 2600). `rdtsc`-based measurements assume invariant TSC. `clock_gettime(CLOCK_MONOTONIC_RAW)` is vDSO-accelerated on attu (~20 ns) but not on EC2 c5.xlarge (~510 ns) — a finding reflected in which measurements are valid where.
