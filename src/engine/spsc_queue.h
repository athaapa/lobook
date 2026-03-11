#pragma once
#include "order_message.h"
#include <array>
#include <atomic>
#include <cstddef>

template <size_t N>

class SPSCQueue {
    // Q: Why must N be a power of 2, and why enforce it with a static_assert
    //    rather than just documenting it?
    // A:
    static_assert(((N & (N - 1)) == 0), "N must be a power of 2");

public:
    void push(const OrderMessage& msg)
    {
        // Q: Why load tail_ with memory_order_relaxed but load head_ with
        //    memory_order_acquire inside the spin loop?
        // A:
        size_t tail_v = tail_.load(std::memory_order_relaxed);
        while (((tail_v + 1) & mask_) == head_.load(std::memory_order_acquire)) { }

        buffer_[tail_v & mask_] = msg;

        // Q: Why use memory_order_release for the store to tail_ here?
        // A:
        tail_.store(tail_v + 1, std::memory_order_release);
    }
    // Only ever called by the matching thread
    OrderMessage pop()
    {
        size_t head_v = head_.load(std::memory_order_relaxed);
        // Q: Why spin (busy-wait) in pop() rather than blocking on a condition
        //    variable or futex when the queue is empty?
        // A:
        while (head_v == tail_.load(std::memory_order_acquire)) { }

        OrderMessage msg = buffer_[head_v & mask_];
        head_.store(head_v + 1, std::memory_order_release);
        return msg;
    }

private:
    static constexpr size_t mask_ = N - 1;
    std::array<OrderMessage, N> buffer_;

    // Q: Why are head_ and tail_ each individually aligned to 64 bytes, and
    //    why does it matter that they are on separate cache lines?
    // A: The reason head_ and tail_ are on separate cache lines is to prevent false sharing. False
    // sharing is when two variables are close enough to be on the same cache line. The issue arises
    // when two cores are trying to access these variables at the same time. To maintain cache
    // coherency, the core that is accessing the variable will invalidate the cache line and the
    // other core will have to fetch the new value from memory. This is a performance bottleneck. By
    // putting them on separate cache lines, we prevent the two cores from accessing the same cache
    // line at the same time.
    alignas(64) std::atomic<size_t> head_ { 0 };
    alignas(64) std::atomic<size_t> tail_ { 0 };
};
