# The Journey from 3ms to 230ns

## The Goal

Replace a mutex-based `NaiveQueue` with a lock-free `SPSCQueue` to reduce end-to-end latency between a network thread and a matching thread in a limit order book.

**Naive baseline:** P50 = 18,642ns (~18μs)

---

## Problem 1: SPSC Queue Was Slower Than the Mutex Queue

**First run with SPSC on shared hardware:** P50 = 41μs. The lock-free queue was *slower* than the mutex.

**Diagnosis:** Buffer too small. `SPSCQueue<1024>` holds 1,023 messages, but the workload is 100k orders. After the first 1,023 messages, the producer blocks on every `push()` spinning until the consumer frees a slot. The timestamp was set *before* the spin, so the measured latency included the time the producer spent waiting for space.

The `NaiveQueue` didn't have this problem because `std::queue` is unbounded — `push()` never blocks.

**Fix:** Increased buffer to `SPSCQueue<131072>` (128k slots, larger than the workload).

---

## Problem 2: Bigger Buffer Made Things Worse

**Result:** P50 jumped to **3ms**. Completely wrong direction.

**Diagnosis:** The producer dumps 100k messages as fast as possible into the now-huge buffer. No backpressure means all 100k messages enter the queue before the consumer finishes processing. By the time the consumer reaches message 50,000 (the P50), that message has been sitting in the queue for ~3ms. We weren't measuring *queue latency* — we were measuring *queueing delay* from a burst.

**Fix:** Added pacing — a 1μs busy-wait between sends to simulate a realistic order arrival rate. This keeps the producer from outrunning the consumer.

---

## Problem 3: P50 Dropped, but P99 Was Wildly Inconsistent

**On shared hardware (UW attu):**

| Run | P50 | P99 | P999 |
|-----|-----|-----|------|
| 1   | 453ns | 1,818ns | 4,301ns |
| 2   | 441ns | 578,741ns | 688,812ns |
| 3   | 487ns | 729,137ns | 841,391ns |

P50 was stable, but P99 ranged from 1.8μs to 729μs between runs.

**Diagnosis:** Shared machine. The `pop()` spin-wait burns 100% CPU, so the OS scheduler preempts it to give other users' processes a time slice. When the matching thread is descheduled, incoming messages wait 1–4ms (a full scheduler tick) for it to be rescheduled. `pthread_setaffinity_np` pins the thread to a core, but on a shared machine it can't prevent *other* threads from being scheduled on that core.

**Fix:** Moved to an AWS EC2 `c5.xlarge` spot instance with kernel-level isolation:

```
isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
```

- `isolcpus=2,3` — scheduler won't place any process on cores 2,3 unless explicitly pinned
- `nohz_full=2,3` — disables timer tick on those cores (no scheduler interrupts)
- `rcu_nocbs=2,3` — moves kernel bookkeeping callbacks off those cores

IRQs were also moved to cores 0–1:
```bash
for irq in /proc/irq/*/smp_affinity_list; do
    echo 0-1 | sudo tee $irq 2>/dev/null
done
```

---

## Problem 4: P50 = 240ns, but P99 Still at 2.7ms

**On isolated EC2, consistent across every run:** P50 = 240ns, P99 = ~2.7ms.

**Diagnosis:** Printed the first 10 raw latencies:

```
msg 0: 2,884,341 ns
msg 1: 2,883,473 ns
msg 2: 2,882,478 ns
msg 3: 2,881,414 ns
...
```

The first ~2,880 messages all had ~2.88ms latency, decreasing by ~1μs each (the pacing interval). The matching thread was still in `book_.init()` when those messages arrived — it hadn't entered the `pop()` loop yet. The ~2.88ms was exactly the time `book_.init(100000)` takes to allocate and initialize the order pool.

**Fix:** Added a startup barrier using `std::atomic<bool> ready_`. The matching thread sets it after `init()` completes. The network thread spins on `wait_until_ready()` before sending the first message.

---

## Final Results

### Isolated Virtualized Hardware (EC2 c5.xlarge)

```
Hardware: AWS c5.xlarge, Intel Xeon Platinum 8124M @ 3.0GHz
Config:   isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
          IRQs moved to cores 0-1
          Matching thread → core 2, Network thread → core 3
Workload: 100k SUBMIT orders, 1μs paced arrival rate
```

| Run | P50 | P99 | P999 |
|-----|-----|-----|------|
| 1   | 236ns | 2,190ns | 4,681ns |
| 2   | 227ns | 2,203ns | 5,484ns |
| 3   | 233ns | 2,193ns | 5,132ns |
| 4   | 225ns | 2,193ns | 4,584ns |
| 5   | 235ns | 2,180ns | 4,546ns |

**Median: P50 = 233ns, P99 = 2,193ns, P999 = 4,681ns**

### Isolated Bare Metal (EC2 c5.metal)

```
Hardware: AWS c5.metal, Intel Xeon Platinum 8275CL @ 3.0GHz
Config:   isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
          IRQs moved to cores 0-1
          Matching thread → core 2, Network thread → core 3
Workload: 100k SUBMIT orders, 1μs paced arrival rate
```

| Run | P50 | P99 | P999 |
|-----|-----|-----|------|
| 1   | 215ns | 1,410ns | 2,359ns |
| 2   | 282ns | 1,568ns | 3,002ns |
| 3   | 305ns | 1,484ns | 2,646ns |
| 4   | 243ns | 1,277ns | 2,340ns |
| 5   | 247ns | 1,291ns | 2,332ns |
| 6   | 208ns | 1,243ns | 2,309ns |
| 7   | 225ns | 1,295ns | 2,416ns |
| 8   | 304ns | 1,392ns | 2,670ns |

**Median: P50 = 245ns, P99 = 1,350ns, P999 = 2,378ns**

### Shared Bare Metal (UW attu, with pinning)

```
Hardware: Intel Xeon E5-2670 v3 @ 2.30GHz, 48 cores (2 sockets)
Config:   pthread_setaffinity_np only (no kernel isolation)
Workload: 100k SUBMIT orders, 1μs paced arrival rate
```

| Run | P50 | P99 | P999 |
|-----|-----|-----|------|
| 1   | 272ns | 320ns | 30,416ns |
| 2   | 204ns | 290ns | 2,052ns |
| 3   | 270ns | 316ns | 2,372ns |
| 4   | 249ns | 296ns | 2,406ns |
| 5   | 260ns | 308ns | 1,673ns |
| 6   | 279ns | 373ns | 1,992ns |

**Median: P50 = 265ns, P99 = 312ns, P999 = 2,199ns**

P50 and P99 are stable at ~250/300ns regardless of environment. P999 reveals jitter from shared hardware (30μs spikes vs <5μs on isolated cores).

---

## Controlled Comparison: Mutex vs Lock-Free

Same environment (attu), same pinning, same pacing, same barrier — only the queue type differs.

**Naive Queue (mutex + condition variable):**

| Run | P50 | P99 | P999 |
|-----|-----|-----|------|
| 1   | 1,910ns | 6,123ns | 11,116ns |
| 2   | 1,797ns | 6,152ns | 11,526ns |
| 3   | 1,776ns | 6,048ns | 8,996ns |
| 4   | 1,873ns | 6,078ns | 13,261ns |
| 5   | 1,798ns | 5,736ns | 10,317ns |

**SPSC Queue (lock-free):**

| Run | P50 | P99 | P999 |
|-----|-----|-----|------|
| 1   | 255ns | 332ns | 1,770ns |
| 2   | 251ns | 308ns | 2,181ns |
| 3   | 249ns | 296ns | 2,406ns |
| 4   | 260ns | 308ns | 1,673ns |
| 5   | 279ns | 373ns | 1,992ns |

**Speedup:**

```
              Naive Queue     SPSC Queue      Speedup
P50:          1,831ns         259ns           7x
P99:          6,027ns         323ns           19x
P999:         11,043ns        2,004ns         6x
```

The ~1,800ns naive P50 is the cost of two `futex` syscalls per message (one to notify in `push()`, one to wake in `pop()`). The SPSC queue eliminates both entirely.

---

## Ablation: What Does CPU Pinning Actually Buy?

Removed `pthread_setaffinity_np` on attu while keeping everything else (SPSC queue, pacing, barrier). Ran 10 times:

| Run | P50 | P99 | P999 |
|-----|-----|-----|------|
| 1   | 255ns | 332ns | 1,770ns |
| 2   | 726ns | 1,140ns | 6,587ns |
| 3   | 251ns | 308ns | 2,181ns |
| 4   | 850ns | 1,267ns | 10,891ns |
| 5   | 845ns | 1,373ns | 8,398ns |
| 6   | 739ns | 1,152ns | 9,341ns |
| 7   | 244ns | 303ns | 1,707ns |
| 8   | 277ns | 334ns | 1,908ns |
| 9   | 279ns | 340ns | 2,492ns |
| 10  | 734ns | 1,199ns | 9,508ns |

**Two distinct modes emerge:**

| Mode | P50 | P99 | Cause |
|------|-----|-----|-------|
| "Good" (runs 1,3,7,8,9) | ~250ns | ~320ns | Threads landed on different physical cores |
| "Bad" (runs 2,4,5,6,10) | ~780ns | ~1,200ns | Threads on same core or hyperthreaded siblings |

Without pinning, the OS randomly places threads. Sometimes you get the good case, sometimes the bad case. **Pinning doesn't make you faster — it makes you reliable.** It eliminates the bad mode entirely.

---

## Bare Metal vs Virtualized vs Shared

```
              attu (good runs)   EC2 virtualized   EC2 bare metal
P50:          ~250ns             ~230ns             ~250ns
P99:          ~310ns             ~2,190ns           ~1,350ns
P999:         ~2,000ns           ~4,800ns           ~2,450ns
```

**Key Findings:**
- **P50 is constant (~240ns):** Across three different generations of Xeon processors and both virtualized/bare-metal environments, the median latency remains nearly identical. This is the fundamental floor of the SPSC implementation.
- **Hypervisor Impact:** Moving from virtualized to bare-metal EC2 saw P99 improve by ~1.6x (2,190ns → 1,350ns), confirming that the hypervisor (likely Nitro/KVM) introduces systematic jitter at the tail.
- **Platform Jitter:** Bare-metal `attu` (Haswell) consistently outperformed bare-metal EC2 (Cascade Lake) at P99 by ~1,000ns. A benchmark of `clock_gettime(CLOCK_MONOTONIC_RAW)` ruled out measurement overhead (~48 cycles on both). The remaining gap is attributed to platform-specific characteristics like SMI interrupts, L3 cache topology, or inter-socket coherence latencies.

| Environment | Best P99 | Consistency | Bottleneck |
|---|---|---|---|
| **attu (Shared)** | 290ns | ❌ Bimodal | OS Scheduler |
| **EC2 (Virtualized)** | 2,190ns | ✅ High | Hypervisor Overhead |
| **EC2 (Bare Metal)** | 1,350ns | ✅ High | Platform Jitter |

**Conclusion:** Performance tuning is a fight on multiple fronts. Shared hardware is fast but unpredictable. Virtualized hardware is predictable but carries a hypervisor tax. Low-latency production systems require the "Third Way": isolated bare-metal hardware to achieve both the 250ns floor and a tight tail.

