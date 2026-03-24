#pragma once
#include "order_message.h"
#include <array>
#include <atomic>
#include <cstddef>

template <size_t N>

class SPSCQueue {
    // Q: Why must N be a power of 2, and why enforce it with a static_assert
    //    rather than just documenting it?
    // A: N must be a power of 2 so we can utilize the mask trick of ANDing an index with (N - 1).
    // It is the equivalent of doing index % N. It essentially just zeros out all bits that are not
    // between 0 and N - 1. As for why it's a static_assert, that's because asserts are safer and
    // stronger than comments. Comments can be ignored. static_assert will throw a compile-time
    // error.
    static_assert(((N & (N - 1)) == 0), "N must be a power of 2");

public:
    void push(const OrderMessage& msg)
    {
        // Q: Why load tail_ with memory_order_relaxed but load head_ with
        //    memory_order_acquire inside the spin loop?
        // A: We can load tail_ with memory_order_relaxed because the producer is the only one
        // calling push(). Therefore, we don't need strict synchronization guarantees. However,
        // reading head_ requires std::memory_order_acquire because the consumer is writing to it.
        // Therefore, it's important that the consumer's read of the message happened before the
        // producer's write to that slot.
        size_t tail_idx = tail_.load(std::memory_order_relaxed);
        size_t next_tail_idx = (tail_idx + 1) & mask_;
#ifdef LOBOOK_SPSC_USE_CACHED_INDICES
        if (next_tail_idx == head_cached_) {
            size_t h;
            while (next_tail_idx == (h = head_.load(std::memory_order_acquire))) { }
            head_cached_ = h;
        }
#else
        while (next_tail_idx == head_.load(std::memory_order_acquire)) { }
#endif

        buffer_[tail_idx & mask_] = msg;

        // Q: Why use memory_order_release for the store to tail_ here?
        // A: memory_order_release compiles to a plain mov instruction on x86. TSO already
        // guarantees store-store ordering at the hardware level; release is purely a compiler
        // barrier, preventing the buffer write from being reordered after the tail_ store.
        // seq_cst would add MFENCE (draining the store buffer), costing ~40-80ns per cache line
        // invalidation. In a 2-thread SPSC, acquire/release is always sufficient.
        tail_.store(tail_idx + 1, std::memory_order_release);
    }

    // Only ever called by the matching thread
    OrderMessage pop()
    {
        size_t head_idx = head_.load(std::memory_order_relaxed);
        // Q: Why spin (busy-wait) in pop() rather than blocking on a condition
        //    variable or futex when the queue is empty?
        // A: Busy waiting prevents the thread from sleeping. A condition variable or futex puts
        // the thread to sleep, and waking it requires a futex syscall (~1.8us round-trip cost,
        // as measured in the naive queue baseline). Spinning keeps the thread live and ready to
        // pop in nanoseconds. The tradeoff is 100% CPU utilization on the pinned core.
#ifdef LOBOOK_SPSC_USE_CACHED_INDICES
        if (head_idx == tail_cached_) {
            size_t h;
            while (head_idx == (h = tail_.load(std::memory_order_acquire))) { }
            tail_cached_ = h;
        }
#else
        while (head_idx == tail_.load(std::memory_order_acquire)) { }
#endif

        OrderMessage msg = buffer_[head_idx & mask_];
        head_.store(head_idx + 1, std::memory_order_release);
        return msg;
    }

private:
    static constexpr size_t mask_ = N - 1;
    std::array<OrderMessage, N> buffer_;

    // Q: Why are head_ and tail_ each individually aligned to 64 bytes, and
    //    why does it matter that they are on separate cache lines?
    // A: head_ and tail_ are written by different threads (consumer writes head_,
    // producer writes tail_). If they shared a cache line, a write to either would
    // invalidate the other thread's copy of the entire line — forcing a cross-core
    // MESI coherence round-trip even though neither thread touched the other's variable.
    // This is false sharing. alignas(64) ensures each occupies its own cache line.
    // tail_cached_ lives with head_ (consumer reads both); head_cached_ lives with tail_
    // (producer reads both). Each thread has its two hot variables on the same line. Without
    // false-sharing, each thread's cache line could sit in E state, allowing silent E -> M writes
    // with no cross-core traffic at all. False sharing forces the line into S state, which means
    // every write triggers the invalidation cycle.
    //
    alignas(64) std::atomic<size_t> head_ { 0 };
#ifdef LOBOOK_SPSC_USE_CACHED_INDICES
    alignas(64) size_t tail_cached_ { 0 };
#endif
    alignas(64) std::atomic<size_t> tail_ { 0 };
#ifdef LOBOOK_SPSC_USE_CACHED_INDICES
    alignas(64) size_t head_cached_ { 0 };
#endif
};
