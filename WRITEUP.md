# Lobook

## Overview
I have spent the past four months writing, refining, and testing this limit order book. This document summarizes my most important findings.

The architecture consists of one producer thread (the network ingress simulator) that pushes timestamped `OrderMessage`s into an SPSC ring buffer and a matching engine that pops them on a pinned consumer core, runs them against the book, and records per-message end-to-end latency.

This document seeks to answer two questions:
- What is the most effective way to represent the book itself?
- How does the producer communicate with the consumer efficiently?

## Headline numbers

| queue mode                | p50        | p99     | p999      |
| ---                       | ---        | ---     | ---       |
| **SPSC, cached indices**  | **180 ns** | 1.99 µs | 6.46 µs   |
| SPSC, uncached            | 190 ns     | 2.00 µs | 6.51 µs   |
| mutex + condvar           | 1.89 µs    | 9.64 µs | 19.96 µs  |

End-to-end p50 latency: **180 ns** (lock-free SPSC + matching engine, 1 µs pacing, N=10 runs). 10.5× faster than a mutex + condvar baseline.

All numbers in this document were measured on the same host in a single session: bare-metal AMD Ryzen 5 2600 @ 3.4 GHz, `isolcpus=6-11`, Fedora kernel 6.19, gcc 15.2 `-O3 -D_GNU_SOURCE`.

## In-memory book layout
The obvious way to represent the book is using a map of prices and linked lists `std::map<Price, std::list<Order>>`. `std::map` gives us O(log n) price lookup, a list per price level for FIFO order. Every order insertion allocates a new list node on the heap. Every match traverses a red-black tree. This sounds decent in theory, but in practice there are a few fundamental problems with this architecture that make it unsuitable for low-latency applications.

The most glaring of these issues is that we are allocating memory on the heap on the hot path. Heap allocation is non-deterministic. On a cold allocator or under memory pressure it can trigger a system call or page fault and take tens to hundreds of microseconds. Even a warm malloc adds ~50–200ns of jitter you can't control.

Heap-allocated linked lists scatter nodes across memory, so traversing a price level becomes a chain of dependent pointer dereferences, each one a potential cache miss. `std::map` has the same problem: tree traversal follows parent and child pointers that are equally scattered.

So, how can we make this better? The first thing we can do is replace the map with a flat `std::array<PriceLevel, MAX_PRICES>`. The price is just an index, so lookup is O(1), and the memory is contiguous, so we get all the nice benefits of spatial locality.

To represent the queue of orders per `PriceLevel`, we can use an intrusive doubly-linked list inside a fixed-capacity pool. The orders themselves are in a fixed-size `std::vector<Order>`. The pool eliminates heap allocation on the hot path entirely. Free list allocation/free is O(1).

We also have an `id_map` (array indexed by order ID) that maps order IDs to their slot in the pool. We need this so we can look up the order's location from just its id.

Together, these changes eliminate dynamic allocation from the hot path entirely.

## O(1) best-bid / best-ask: the hierarchical bitset
The baseline was iterating over the entire price ladder on every order match. Oftentimes, the ladder itself is sparse, causing us to check many prices with no orders.

Iterating over the sparse price ladder array is wasted work. We want to iterate only over the price levels that have active orders.

The idea is to have a hierarchical bitset, basically a bitmask that represents each price. Obviously we can't have a number with 100,000 bits, so we need to split it into 64-bit numbers. In my case, we have three levels from coarsest to finest: L0, L1, and L2.

Why three levels? Because 100,000 prices / 64 bits = 1,563 L2 chunks. If we only had L1 + L2 (two levels), we'd need to scan 1,563 bits to find the active chunk. That's still O(n) for sparse books. By adding L0 as a third level, we can index 1,563 chunks with just 25 L1 chunks, and check all 25 with a single 64-bit L0 integer. Now we scan at most 64+64+64 = 192 bits total (O(1) with small constant).

Let's start with L2, the finest. L2 is an array of 1563 `uint64_t`s. Each bit in each `uint64_t` corresponds to a price level. For example, L2[0] corresponds to the prices 0-63. Therefore, we have 1563 × 64 = 100032 prices. Now, if we go up a level, the L1 array is an array of 25 `uint64_t`s. Similarly, each element in L1 represents 64 indices in the L2 array (one for each bit). If a bit is set, it means that there is a price level with orders in range. Finally, if we go up to L0, we get a single `uint64_t`, where the last 25 bits represent the 25 indices in L1. Likewise, if a bit is set, it means that there are some valid orders there.

This architecture makes it _extremely_ easy to find the highest and lowest set bit. The `gcc` compiler comes with two functions that are helpful here: `__builtin_ctzll` and `__builtin_clzll` which count trailing zeros and leading zeros in an integer respectively **in 1 CPU cycle** (the bitset is guaranteed non-zero before each call, so the undefined-input case never arises). Best ask = lowest set bit (`ctzll` on L0 → L1 → L2), best bid = highest set bit (`clzll` on L0 → L1 → L2). While the prior `FastBook` was fumbling through an entire price ladder, the hierarchical bitset allows us to have functions `find_highest` and `find_lowest` in O(1).

Against three workloads designed to stress different aspects of the book:

| Scenario                                               | FastBook ns/op | FastBitsetBook ns/op | Speedup      |
| ---                                                    | ---            | ---                  | ---          |
| sparse insert (200k orders into 50 levels)             | 53,179         | 8.12                 | **6,548×**   |
| wide-spread match (10k asks at top, 10k crossing buys) | 171,712        | 9.58                 | **17,925×**  |
| mixed churn (100k random submit/cancel/aggressive)     | 58,353         | 48.17                | **1,212×**   |

The sparse-insert and wide-spread-match speedups scale roughly with `MAX_PRICES` (100k here). On a smaller ladder the gap closes proportionally. Mixed churn is the most realistic scenario and still shows 1,212×; even under realistic churn, the book is sparse enough that the bitset's O(1) lookup dominates.

## Wire format: OrderMessage
To facilitate order requests between the consumer and producer, I created a struct called `OrderMessage`. `OrderMessage` consists of 6 fields.

```cpp
struct OrderMessage {
    uint64_t id; // The id of the order
    uint64_t price; // The price of the order
    uint32_t qty; // The quantity of the order
    uint64_t timestamp; // The time that the order placed

    bool is_buy; // Boolean flag for whether the order is a buy/sell
    Type type; // An enum representing the type of request (SUBMIT, SHUTDOWN, or CANCEL)
};
```
The `timestamp` field is set immediately before `push()`, so the measured latency captures the full end-to-end transit: queue wait + pop + matching engine work.

## Native replace primitive
Typically in an exchange you want to support the replacement of orders. That is, if an order is placed, you should be able to change its price and/or quantity if it is currently resting on the book. However, to preserve fairness, changing an order has implications for priority. In particular, if an order changes its price, it will always lose its priority. Similarly, if an order increases its quantity, it loses its priority in the queue. Conversely, if an order decreases its quantity, it preserves its priority. The specifics of why the priority changes are outside the scope of this writeup.

The simple way to implement this functionality is to treat an order update as a new order entirely with an updated price/quantity. However, this implementation can certainly be made better. Both cancel and submit have overhead that is unnecessary for replace semantics. For example, cancel does: `id_map` look up -> unlink from level -> clear bitset bit if level empties -> push order back on the free list. Submit does: pop free list -> new `id_map` entry -> set bitset bit -> append to level tail. Native replace on quantity decrease (same price): a single `id_map` look up -> update order quantity. Native replace skips the free-list, no bitset, and no unlink/relink entirely. Even on the price change path, we still save a free-list round trip and one of the bitset toggles (only bid-side or only ask side is affected by the move, never both).

On 1M random replace ops against a 10k-order book: native replace takes 20.5 ns/op, cancel+submit takes 48.4 ns/op. Native replace retires 74 instructions/op at IPC 1.10 with a 1.66% branch miss rate; cancel+submit retires 138 instructions/op at IPC 0.87 with a 3.41% branch miss rate.

The naive cost model (counting memory accesses and summing their latencies) predicted 88ns for native replace and 112ns for cancel + submit, a 1.27× ratio. The measurement showed 20ns vs 48ns, a 2.36× ratio. The cost model was wrong in two independent ways.

First, absolute latencies overshot by ~5× on both paths because the model treated cache misses as serial. On an out-of-order CPU, independent loads can be overlapped due to memory-level parallelism.

Second, the ratio was understated because the model was memory-centric. L1D miss counts are nearly identical between the two paths (6.34 vs 6.79 per op). The real gap is shown in the measurements: 1.86x more instructions and 1.26× lower IPC. The branch predictor had an easier time with one branchy path than with two. 1.86 × 1.26 = 2.34, almost exactly the measured 2.36×.

The comparison is not technically apples-to-apples: native replace preserves queue priority on quantity decrease at the same price, while cancel+submit always resets priority. On this synthetic workload the semantic gap hits <0.01% of operations so the measured 2.4× is still a fair performance number, but the correct framing is that native replace is both faster and strictly more expressive. Cancel + submit cannot express the priority-preserving quantity-decrease case at all.

## Cross-thread handoff: lock-free SPSC queue
The matching engine runs on a different thread from the code that receives orders over the network, so we need a way to hand off orders between them. Every order in the entire system has to go through this handoff, which makes it a kind of substrate. Every optimization here shows up directly in the end-to-end latency number. That's what makes the queue worth a whole section.

The obvious first cut is the mutex-guarded `std::queue` with a condition variable so the consumer can sleep when there's nothing to do. It's easy to reason about and easy to get right. It's also the slowest thing in the system by an order of magnitude, which makes it a useful place to start.

### Why lock-free
The intuitive implementation uses a `std::queue` guarded by a mutex, plus a `std::condition_variable` so the consumer can sleep while the queue is empty and wake up when the producer signals new work. The mutex guarantees correctness under concurrent access, while the condvar keeps the consumer off the CPU when there's nothing to do. This baseline implementation yields a p50 latency of 1.9µs, nearly an order of magnitude worse than the lock-free path.

The issue is that the condvar sleeps the consumer whenever the queue is empty, and sleeping is expensive. Waking the consumer up requires a futex syscall, and once it wakes up the scheduler has to actually place it back on the CPU. Together that's the entire ~1.9µs baseline. On a non-isolated machine the scheduler occasionally misses its tick, and the tail stretches much further: on a shared UW box (`attu`) the same benchmark measures ~18µs p50 and ~91µs p99, because every few hundred messages the consumer has to wait a full scheduler tick to get rescheduled.

The fix, in short, is to remove both the mutex and the condvar. The mutex only exists because `std::queue` isn't thread-safe. A fixed-size ring buffer with atomic head and tail indices is thread-safe by construction when there's exactly one producer and one consumer. The condvar only exists because the consumer has to sleep when the queue is empty, and on a pinned core with nothing else to do, busy-spinning on the tail atomic is strictly faster than going through the OS.

Beyond that, the topology narrows the choice of lock-free queue. We have exactly one network thread and one engine thread, so an SPSC queue is a natural fit. SPSC buys us a lot here: since there's only one writer to each index, the producer can publish a new tail with a plain `store(release)` instead of a CAS loop: a single store, no retry logic. The consumer does the mirror image on the head. MPMC would break this. If multiple producers can write `tail_`, every push becomes a `compare_exchange` retry loop, which is ~40ns of `lock cmpxchg` even uncontended, plus retry storms under real contention. On top of the CAS cost, the shared `tail_` cacheline would ping-pong between producer cores on every push. SPSC avoids both. The tradeoff is the obvious one: if the architecture ever needs more than one network thread or more than one engine thread, this design breaks and the queue layer has to be rewritten.

### The ring buffer itself
I chose to represent the queue as a circular ring buffer with a power-of-2 size. A power-of-2 size lets me compute the next index with a bitwise AND on `mask_` instead of modulo. Integer division costs 10-90 cycles on x86 depending on the microarchitecture, and this runs on every order.

Fixing the size at compile-time has three benefits. The mask becomes a constant the compiler can fold into the addressing math instead of loading from memory. The queue can never trigger an allocation on the hot path, which would be the worst kind of tail-latency event. And it makes backpressure explicit. When the queue is full, the producer has to decide whether to block, drop, or retry, rather than silently growing memory under load. For an exchange, knowing you've hit capacity is strictly better than degrading silently.

For memory ordering, the producer and consumer use `std::memory_order_relaxed` when loading `tail_` and `head_` respectively. This is because the producer is the only one writing to `tail_`, so program-order guarantees that it always see its own latest store. Same logic applies on the consumer side for its load of `head_`.

Cross-core loads are a different story. The producer loads `head_` (written by the consumer) with `std::memory_order_acquire`, and the consumer loads `tail_` (written by the producer) with `std::memory_order_acquire`. The matching stores use `std::memory_order_release`. The acquire/release pair does real work: it ensures the consumer's read of a buffer slot happens-before the producer's reuse of that slot and vice versa. Without it, the compiler is free to reorder the buffer write past the release store of the index, and the consumer could read a stale slot even after seeing the new tail.

On x86, acquire and release both compile to plain `mov` instructions (the same instructions as relaxed!), thanks to Total Store Ordering (TSO) guarantees. TSO gives you load-load, load-store, and store-store ordering at the hardware level, which covers everything acquire/release needs. (Store-load is the one ordering x86 doesn't give you for free, which is why `seq_cst` stores do emit a full memory barrier, typically `xchg` or `mfence`, but I don't need `seq_cst` here). The ordering is enforced by the compiler, not the hardware, which means the runtime cost of acquire/release is zero on this architecture. On ARM, which uses a weaker memory model, the same code would emit explicit fence instructions, so the ordering choices matter for portability even though they're free here.

`head_` and `tail_` are each `alignas(64)` so they sit on separate cache lines. Without this, both indices would share a 64-byte line, and every producer write to `tail_` would transition that line to Modified in MESI, invalidating the consumer's copy. The consumer's next load of `head_` would then take a coherence miss (roughly 30-50ns on the same socket and ~100ns cross-socket) even though the producer never touched `head_` itself. This is the classic false-sharing pattern: two logically independent variables forced into the same coherence unit, ping-ponging the line between cores. On a 180ns p50 budget, eating one of those misses per push would dominate the entire latency. C++17 has std::hardware_destructive_interference_size for this, but on every x86 chip I care about it's 64 bytes, so I hardcoded it.

The buffer slots themselves are not padded to cache-line boundaries. At 32 bytes per slot, adjacent slots share a cache line, so a producer write to slot N+1 can invalidate the line the consumer is reading from slot N. At 1µs pacing this doesn't fire in practice because the consumer drains each slot long before the producer reaches the next one, but under tight pacing it's a real coherence cost. Padding slots to 64 bytes eliminates it at the cost of doubling the buffer's memory footprint.

### Getting it right
While running the benchmark, I ran into a few issues that resulted in some interesting behavior.

The first was that I made the queue size too small (N=1024). After the first 1023 messages, the producer blocked on every `push()` waiting for the consumer to free a slot. Because the message timestamp was set before the push, the measured "latency" included the producer's wait time, not just transit.

After realizing this, I increased the queue size to N=131072. However, this caused p50 to jump to ~3ms, which was even worse than before.

Turns out that with a huge buffer, the producer has no reason to slow down. It dumps all 100k messages into the queue as fast as it can generate them (essentially a microsecond-scale burst). The consumer, meanwhile, is processing at its own steady rate (~250ns per message). By the time the consumer gets to the 50,000th message, that message has been sitting in the queue for ~3ms. The tell was in the consistency: every run showed ~3ms p50, and random OS jitter would've had variance.

The fix is to add pacing between producer pushes. This simulates a realistic order arrival rate and keeps the producer from overwhelming the consumer. Now the queue is near-empty most of the time, and the measured latency is actual transit time rather than queueing delay.

After fixing pacing, p50 dropped to ~450ns on `attu` (UW's shared Linux cluster), which looked great, but p99 was wildly inconsistent across runs.

| Run         | p50         | p99         | p999        |
| ----------- | ----------- | ----------- | ----------- |
| 1           | 453ns       | 1,818ns     | 4,301ns     |
| 2           | 441ns       | 578,741ns   | 688,812ns   |
| 3           | 487ns       | 729,137ns   | 841,391ns   |

The tell here was bimodality. It wasn't that everything got slower. Most messages were fast, but 1% of them were 1000× slower. And the p99 spikes were consistently in the 1-4ms range, which lines up with a scheduler time slice.

That's exactly what was happening. `attu` is a shared machine, so other users' processes are always running. My consumer is in a tight `pop()` spin-wait, burning 100% of a CPU. From the OS scheduler's perspective, it looks like a well-behaved CPU-bound thread, so it gets preempted at the normal tick boundary to give some other user's thread a slice. When the consumer is off-CPU, messages pile up in the queue for ~1-4ms (exactly one scheduler tick) until the scheduler rotates it back on.

`pthread_setaffinity_np` pins the thread to a core, but on a shared box the scheduler still schedules _other_ threads onto that same core. Pinning doesn't "own" the core, it just tells the scheduler where to put your thread when it runs.

To address this, I moved to an EC2 c5.xlarge spot instance with kernel-level isolation:
- `isolcpus=2,3` tells the scheduler not to place processes on cores 2 and 3 unless explicitly pinned
- `nohz_full=2,3` disables the scheduler tick interrupt on those cores
- `rcu_nocbs=2,3` moves kernel RCU callbacks off those cores

Plus IRQs steered to cores 0-1. The takeaway is that for sub-microsecond tail latency, you need kernel-level isolation on top of pinning, not pinning alone.

On isolated EC2, p50 dropped to ~240ns, but p99 was pegged at ~2.7ms across every run. The fact that it was pegged so consistently told me this wasn't jitter, it was something deterministic. I printed the raw latency of the first 10 messages to see what:

```bash
msg 0: 2,884,341ns
msg 1: 2,883,473ns
msg 2: 2,882,478ns
msg 3: 2,881,414ns
...
```

The first ~2,880 messages all had ~2.88ms latency, each one about 1µs less than the previous. One microsecond is exactly my pacing interval, which gave it away. The matching thread was still in `book_.init()` when the first messages arrived, so they sat in the queue until it finished. The ~2.88ms baseline is how long the 100k-order pool takes to allocate and initialize. Each subsequent message entered the queue one pacing-interval later and therefore waited one pacing-interval less. 2,880 messages at 1µs pacing works out to ~2.88ms, which matches the init time exactly.

The fix was a startup barrier: an `std::atomic<bool> ready_` that the matching thread flips after `init()` returns. The network thread spins on it before sending its first message.

The bigger lesson across all four of these is that aggregates hide structure. p99 = 2.7ms told me nothing on its own, but the raw sample series told me exactly what was wrong in ten lines. Every one of the bugs above looked like random noise in a percentile table and became obvious the moment I looked at individual samples.

### Rigtorp's cached indices
One optimization on top of the typical ring buffer is to cache the peer's index in a thread-local variable and refresh only when the cached value says the queue is full (or empty). This skips a cross-core MESI round-trip on the common path. [Rigtorp's article](https://rigtorp.se/ringbuffer/) explains this mechanism in detail. I won't repeat it here.

Rigtorp's benchmark pushes items as fast as possible through a queue with no consumer work, and measures a 20x throughput gap between cached and uncached. In my benchmark, the matching engine dominates the per-message budget, so at 1 µs pacing the cached-vs-uncached gap shrinks to ~10ns, well within run-to-run variance. Rigtorp's throughput numbers predict a large gap; at low load, the gap mostly disappears. What Rigtorp doesn't measure is what happens at the boundary where producer capacity and consumer drain rate are close enough that a small perturbation flips the system between regimes. That's where the metastability finding lives.

### Getting it stable under load
I wanted to see how the cached indices performed under load compared to uncached, so I ran a pacing-sweep benchmark at 1µs, 100ns, and 10ns. I predicted that the gap would widen under load.

After running the benchmark (10 runs per cell, median-of-medians p50), these were the numbers:

| Pacing      | cached p50  | uncached p50| uncached saturated runs | cached saturated runs |
| ----------- | ----------- | ----------- | ----------------------- | --------------------- |
| 1µs         | 180ns       | 190ns       | 0 / 10                  | 0 / 10                |
| 100ns       | 185ns       | 214µs       | 6 / 10 (200-500µs)      | 0 / 10                |
| 10ns        | 1.1µs       | 1.94µs      | 4 / 10 (2-4ms p50)      | 0 / 10                |

At 1µs pacing, cached is 180ns and uncached is 190ns. A 10ns gap is well within 1 standard deviation of run-to-run variance. If you only looked at this number, you'd conclude the optimization is worthless. At 100ns pacing, however, the story changes. Cached p50 stays at 185ns, but uncached p50 jumps to 214µs, a 1000× increase. And when you look at the individual runs, six out of ten had p50s between 200 and 500µs. The other four ran fine. The distribution isn't "slower on average," it's bimodal.

The uncached implementation has two stable modes. The fast mode (~200ns p50) is when the queue is near-empty: the consumer drains each message faster than the next one arrives, every push sees an empty queue, and end-to-end latency is just push + pop + processing. The slow mode (200-500µs p50) is when the queue is near-full: the producer blocks on every push waiting for a slot, and even though the consumer is still draining at the same rate, every message has to wait through a persistent backlog before being processed. Queue wait time dominates the end-to-end latency. Both modes are self-sustaining. If the queue is empty, a message arrives, gets processed, and the queue stays empty. If the queue is full, the producer keeps hitting queue-full, the consumer keeps draining at the same rate, and the backlog persists indefinitely.

On the uncached implementation, every `push()` call requires a cache-to-cache transfer (due to the producer reading `head_`) which costs ~60-80ns round-trip. The producer's capacity to push is capped by this MESI round-trip cost, and it's barely above the consumer's drain rate, so the system lives very close to the boundary between the two modes. A small perturbation (a slow consumer cycle, a cache miss, a branch misprediction) can push it over the edge into the slow mode, where it stays for the rest of the run. That's why you see 6/10 runs saturated and 4/10 runs fine. On 4 runs, no perturbation big enough to flip the system happened, so it stayed in the fast mode. On 6 runs, something happened early, flipped it to the slow mode, and it stayed stuck.

In the cached case, the cost of `push()` is ~1ns (an L1 hit on `head_cached_`) instead of ~80ns. The producer's capacity is an order of magnitude above the consumer's drain rate, so the "hill" between the two stable modes is now much higher. A tiny disruption can't push the system over, and the queue stays drained no matter what transient jitter happens.

Rigtorp's article sells cached indices as a throughput optimization: 5.5M items/s to 112M items/s on a benchmark that just pushes items as fast as possible. In my system, at 1µs pacing, the steady-state gap is 10ns. That's within noise, and it would be easy to conclude the optimization doesn't matter. What the steady-state number misses is that without cached indices, the system can fall into a regime that's 1000× slower, and stay there. The real value of cached indices isn't a ~10ns steady-state speedup. It's preventing a metastable failure mode that doesn't show up in steady-state benchmarks at all.

One caveat: this failure mode only surfaced after I moved to bare-metal with isolcpus. On attu the scheduler noise was large enough to hide the bimodality (the whole p99 distribution was noisy so the saturated runs looked like tail latency), and on virtualized EC2 the hypervisor timer smearing did the same thing. The finding isn't universal; it requires an environment quiet enough that 200µs spikes stand out.

### Measured end-to-end
1 µs pacing, 100k workload, N=10, median-of-medians:

| queue mode                | p50        | p99     | p999      |
| ---                       | ---        | ---     | ---       |
| SPSC, cached indices      | 180 ns     | 1.99 µs | 6.46 µs   |
| SPSC, uncached            | 190 ns     | 2.00 µs | 6.51 µs   |
| mutex + condvar           | 1.89 µs    | 9.64 µs | 19.96 µs  |

The end-to-end latency considers the latency through the full system i.e. the producer timestamp at push to the consumer timestamp after the matching engine finishes processing. Every number here includes push + cross-thread handoff + pop + matching engine work + timestamp.

The SPSC with cached indices vs mutex + condvar: 10.5× at p50, 4.8× at p99, and 3.1× at p999. It's important to notice that this gap isn't only at the tail. The whole distribution shifted because the futex syscall and scheduler wakeup cost is paid for _every_ `push()`/`pop()` call, not just occasional ones, so the fixed ~1.7µs tax appears even at the median.

## System-level isolation

For sub-microsecond tail latency, `pthread_setaffinity_np` alone isn't enough. The scheduler still preempts your thread to run others on the same core. Kernel-level isolation is required:

- `isolcpus=6-11` — removes cores from the scheduler's pool entirely
- `nohz_full=6-11` — disables the periodic timer tick on those cores
- `rcu_nocbs=6-11` — moves RCU callbacks off those cores
- IRQs steered to cores 0–5

See §5.3 ("Getting it right") for the debugging story that led here.


## Measurement methodology

Hardware counters were measured via `perf_event_open` (instructions, cycles, IPC, branches, branch-misses, L1D read-misses, LLC refs/misses).

For timings, I used region-level timing via `__rdtsc`; invariant TSC frequency calibrated against `CLOCK_MONOTONIC_RAW` at startup (no hardcoded GHz).

Wall-clock timing was measured via `clock_gettime(CLOCK_MONOTONIC_RAW)`. I chose `CLOCK_MONOTONIC_RAW` over `CLOCK_MONOTONIC` to avoid NTP slewing during measurement.

Producer pacing is a busy-spin on `clock_gettime`, not `nanosleep` (sleep wakeup dwarfs the queue cost being measured).

Every benchmark emits a standardized environment banner (host, CPU, calibrated TSC, isolcpus, kernel, compiler) before results, so logs are reproducible and grep-able.

All reported results use N=10 runs, reporting the median-of-medians. A single run has too much variance from transient OS noise. Taking the median across 10 runs gives a stable central estimate without sensitivity to outliers.

## Limits and tradeoffs

The SPSC queue, by design, does not support multiple network or engine threads. If you want to implement this functionality, you must rewrite the queue logic entirely. This was a deliberate choice, as an MPMC design would result in CAS loops, cache line ping-pongs, lock contention. See §5.1 ("Why lock-free"). Similarly, due to the matching engine being single-threaded, there is no parallelism either.

Another major tradeoff is having a fixed queue capacity. This means that the caller owns the backpressure decisions. Having a fixed-size queue has its benefits, however. Most notably, it makes backpressure _explicit_, which is better than silent growth for an exchange.

One limitation of the analysis is that the benchmarks are synthetic. In particular, all the benchmarks have uniform random orders. A more realistic workload would have 90% of the activity in a 10-tick band around the spread. The bitset's L0/L1 words for those ticks would be hotter, the rest colder, which I'd expect to have better cache behavior than uniform random, which spreads accesses across all 100k levels.

Another limitation of the system is that it does not replicate an entire exchange, just the hot path. There is no persistence, no recovery, and no wire protocol.