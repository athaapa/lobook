#pragma once
#include "order_message.h"
#include <array>
#include <atomic>
#include <cstddef>

// Single-producer, single-consumer ring buffer.
//
// Template parameters:
//   N      - capacity (must be a power of 2)
//   Cached - if true, each side caches the other side's index to avoid a
//            cross-core MESI round-trip on the common (non-empty / non-full)
//            fast path. See the comments inside push()/pop() below.
template <size_t N, bool Cached = true>
class SPSCQueue {
    // Q: Why must N be a power of 2, and why enforce it with a static_assert
    //    rather than just documenting it?
    // A: We index slots with `idx & (N - 1)` instead of `idx % N`; the bit-mask
    //    only equals modulo when N is a power of two. static_assert turns a
    //    documentation comment into a compile-time error if anyone tries to
    //    instantiate with a non-power-of-two.
    static_assert(((N & (N - 1)) == 0), "N must be a power of 2");

public:
    void push(const OrderMessage& msg) {
        // Q: Why load tail_ with memory_order_relaxed but load head_ with
        //    memory_order_acquire inside the spin loop?
        // A: tail_ is written only by this thread (the producer), so we don't
        //    need any inter-thread ordering on its load. head_ is written by
        //    the consumer; we need acquire so that the buffer slot read by the
        //    consumer happens-before our reuse of that slot.
        size_t tail_idx = tail_.load(std::memory_order_relaxed);
        size_t next_tail_idx = (tail_idx + 1) & mask_;

        if constexpr (Cached) {
            // Fast path: the cached value of head_ tells us the queue isn't
            // full without touching the contended atomic. Only on the slow
            // path (cache says it might be full) do we read the real head_
            // and refresh the cache.
            if (next_tail_idx == head_cached_) {
                size_t h;
                while (next_tail_idx == (h = head_.load(std::memory_order_acquire))) { }
                head_cached_ = h;
            }
        } else {
            while (next_tail_idx == head_.load(std::memory_order_acquire)) { }
        }

        buffer_[tail_idx & mask_] = msg;

        // Q: Why use memory_order_release for the store to tail_ here?
        // A: release compiles to a plain mov on x86 (TSO already orders
        //    store-store), but it's a compiler barrier that prevents the
        //    buffer write from being reordered after the tail_ publish.
        //    seq_cst would emit MFENCE (~40-80 ns) and isn't needed for SPSC.
        tail_.store(tail_idx + 1, std::memory_order_release);
    }

    OrderMessage pop() {
        size_t head_idx = head_.load(std::memory_order_relaxed);

        if constexpr (Cached) {
            // Fast path: the cached value of tail_ tells us the queue isn't
            // empty without touching the contended atomic.
            if (head_idx == tail_cached_) {
                size_t t;
                while (head_idx == (t = tail_.load(std::memory_order_acquire))) { }
                tail_cached_ = t;
            }
        } else {
            // Q: Why busy-spin instead of blocking on a condition variable?
            // A: cv/futex wake-up is microseconds; spinning is nanoseconds.
            //    Acceptable cost: 100% utilization on the pinned consumer core.
            while (head_idx == tail_.load(std::memory_order_acquire)) { }
        }

        OrderMessage msg = buffer_[head_idx & mask_];
        head_.store(head_idx + 1, std::memory_order_release);
        return msg;
    }

private:
    static constexpr size_t mask_ = N - 1;
    std::array<OrderMessage, N> buffer_;

    // Layout note (matters in Cached mode):
    //   head_         is consumer-write / producer-read
    //   tail_cached_  is consumer-only (placed adjacent to head_ so the
    //                 consumer's two hot variables share a cache line)
    //   tail_         is producer-write / consumer-read
    //   head_cached_  is producer-only (placed adjacent to tail_ so the
    //                 producer's two hot variables share a cache line)
    //
    // Without false sharing, each producer/consumer cache line stays in the
    // E (exclusive) MESI state and writes are silent E -> M with no cross-core
    // traffic. Sharing a line between writers forces S, costing one MESI
    // round-trip (~40-80 ns on Intel) per write.
    //
    // In !Cached mode the cached_ slots are present but unused. The 128 bytes
    // of waste is negligible vs. the structural simplicity of one definition.
    alignas(64) std::atomic<size_t> head_ { 0 };
    alignas(64) size_t tail_cached_ { 0 };
    alignas(64) std::atomic<size_t> tail_ { 0 };
    alignas(64) size_t head_cached_ { 0 };
};
