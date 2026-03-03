#pragma once
#include "order_message.h"
#include <array>
#include <atomic>
#include <cstddef>

template <size_t N>

class SPSCQueue {
    static_assert(((N & (N - 1)) == 0), "N must be a power of 2");

public:
    void push(const OrderMessage& msg)
    {
        size_t tail_v = tail_.load(std::memory_order_relaxed);
        while (((tail_v + 1) & mask_) == head_.load(std::memory_order_acquire)) { }

        buffer_[tail_v & mask_] = msg;

        tail_.store(tail_v + 1, std::memory_order_release);
    }
    // Only ever called by the matching thread
    OrderMessage pop()
    {
        size_t head_v = head_.load(std::memory_order_relaxed);
        while (head_v == tail_.load(std::memory_order_acquire)) { }

        OrderMessage msg = buffer_[head_v & mask_];
        head_.store(head_v + 1, std::memory_order_release);
        return msg;
    }

private:
    static constexpr size_t mask_ = N - 1;
    std::array<OrderMessage, N> buffer_;

    alignas(64) std::atomic<size_t> head_ { 0 };
    alignas(64) std::atomic<size_t> tail_ { 0 };
};
