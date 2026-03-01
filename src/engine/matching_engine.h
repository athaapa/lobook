#pragma once
#include "fast_bitset_book.h"
#include "naive_queue.h"
#include "order_message.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <thread>
#include <vector>

class MatchingEngine {
public:
    MatchingEngine(NaiveQueue& queue)
        : queue_(queue)
    {
    }

    // Q: Why are the copy constructor and copy assignment operator explicitly deleted?
    // A (TODO)
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    void start(size_t max_orders)
    {
        // Q: Why launch the engine on a separate thread rather than running it inline
        //    on the caller's thread?
        thread_ = std::thread([this, max_orders]() {
            run(max_orders);
        });
    }

    void stop()
    {
        // Q: Why use a SHUTDOWN sentinel message to stop the thread rather than a
        //    shared atomic<bool> flag checked in the run loop?
        queue_.push({ Type::SHUTDOWN });
        thread_.join();
    }

    void report()
    {
        // Q: Why sort the latency vector before computing percentiles, rather than
        //    maintaining a running sorted structure (e.g., a heap)?
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
        book_.init(max_orders);
        // Q: Why call reserve(max_orders) on latencies_ upfront?
        // A: std::vector is a dynamic array. We call .reserve() to
        //    ensure that this allocation does not happen while we are
        //    measuring latency because it might influence the results.
        latencies_.reserve(max_orders);
        while (true) {
            // Q: Why use blocking pop() here instead of try_pop() with a spin loop?
            // A (TODO)
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
            // A (TODO)
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
    NaiveQueue& queue_;
    std::thread thread_;
    std::vector<uint64_t> latencies_;
};
