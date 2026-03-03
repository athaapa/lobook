#pragma once
#include "cpu_pinning.h"
#include "fast_bitset_book.h"
#include "naive_queue.h"
#include "order_message.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <thread>
#include <vector>

template <typename QueueT> class MatchingEngine {
public:
    MatchingEngine(QueueT& queue)
        : queue_(queue)
    {
    }

    // Q: Why are the copy constructor and copy assignment operator explicitly deleted?
    // A: std::thread is not copyable, and NaiveQueue& is a reference member. If a copy were
    //    allowed, two MatchingEngines would share the same underlying thread and queue, meaning
    //    two engines could drive the same order book simultaneously — a data race. Explicit
    //    deletion makes the intent clear and gives a better compiler error.
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    void start(size_t max_orders)
    {
        // Q: Why launch the engine on a separate thread rather than running it inline
        //    on the caller's thread?
        // A: The engine and the producer need to be able to run concurrently. The engine sleeps
        // when the queue is empty. If you ran the engine and the producer on the same thread, you
        // would never be able to send any orders. The producer needs to be able to push messages
        // while the engine processes them.
        thread_ = std::thread([this, max_orders]() { run(max_orders); });
    }

    void stop()
    {
        // Q: Why use a SHUTDOWN sentinel message to stop the thread rather than a
        //    shared atomic<bool> flag checked in the run loop?
        // A: An atomic<bool> flag would only be read _after_ the thread awakens to process a
        // message. If the queue is empty and you set the fflag, it would never get read. The
        // sentinel message fixes this by going through the queue itself. The flag approach could
        // work but you would need to call cv_.notify_one() at which point you've essentially just
        // recreated the sentinel pattern.
        queue_.push({ Type::SHUTDOWN });
        thread_.join();
    }

    void report()
    {
        // Q: Why sort the latency vector before computing percentiles, rather than
        //    maintaining a running sorted structure (e.g., a heap)?
        // A:  report() is called once, after all orders have been processed, completely outside the
        // hot path. Sorting at that point costs O(n log n) but has no impact on the latency numbers
        // you collected. By contrast, inserting into a heap or sorted structure on every message
        // would add overhead to every single iteration of the engine loop — which is exactly the
        // code you're trying to measure. You'd be polluting your own latency numbers by doing extra
        // work during measurement.
        sort(latencies_.begin(), latencies_.end());
        size_t p50_idx = (size_t)(latencies_.size() * 0.50);
        size_t p99_idx = (size_t)(latencies_.size() * 0.99);
        size_t p999_idx = (size_t)(latencies_.size() * 0.999);
        std::cout << "p50: " << latencies_[p50_idx] << "\n";
        std::cout << "p99: " << latencies_[p99_idx] << "\n";
        std::cout << "p999: " << latencies_[p999_idx] << "\n";
    }

private:
    void run(size_t max_orders)
    {
        pin_to_core(1);
        book_.init(max_orders);
        // Q: Why call reserve(max_orders) on latencies_ upfront?
        // A: std::vector is a dynamic array. We call .reserve() to
        //    ensure that this allocation does not happen while we are
        //    measuring latency because it might influence the results.
        latencies_.reserve(max_orders);
        while (true) {
            // Q: Why use blocking pop() here instead of try_pop() with a spin loop?
            // A: A spin loop burns CPU cycles actively doing nothing when the queue is empty,
            //    consuming a full core worth of resources. A blocking pop() puts the thread to
            //    sleep and yields the CPU to other threads until work arrives.
            OrderMessage msg = queue_.pop();

            if (msg.type == Type::SHUTDOWN)
                break;

            if (msg.type == Type::SUBMIT) {
                book_.submit_order(msg.id, msg.price, msg.qty, msg.is_buy);

            } else if (msg.type == Type::CANCEL) {
                book_.cancel_order(msg.id);
            }

            // Q: Why use CLOCK_MONOTONIC_RAW instead of CLOCK_MONOTONIC or
            //    CLOCK_REALTIME for latency measurement?
            // A: CLOCK_REALTIME can jump backwards due to NTP adjustments. CLOCK_MONOTONIC
            //    is monotonic but NTP can still gradually slew (speed up or slow) its rate.
            //    CLOCK_MONOTONIC_RAW reads the raw hardware counter with zero NTP influence,
            //    giving the most stable nanosecond-accurate ticks for latency measurement.
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
            uint64_t now_ns = ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
            // Q: Why measure latency as (now_ns - msg.timestamp) rather than timing
            //    just the book_.submit_order / cancel_order call itself?
            // A: I want to test the end-to-end latency of the entire networking/matching process.
            latencies_.push_back(now_ns - msg.timestamp);
        }
    }

    Fast::FastBitsetOrderBook book_;
    QueueT& queue_;
    std::thread thread_;
    std::vector<uint64_t> latencies_;
};
