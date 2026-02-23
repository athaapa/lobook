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

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    void start(size_t max_orders)
    {
        thread_ = std::thread([this, max_orders]() {
            run(max_orders);
        });
    }
    void stop()
    {
        queue_.push({ Type::SHUTDOWN });
        thread_.join();
    }
    void report()
    {
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
        latencies_.reserve(max_orders);
        while (true) {
            OrderMessage msg = queue_.pop();

            if (msg.type == Type::SHUTDOWN)
                break;

            if (msg.type == Type::SUBMIT) {
                book_.submit_order(msg.id, msg.price, msg.qty, msg.is_buy);

            } else if (msg.type == Type::CANCEL) {
                book_.cancel_order(msg.id);
            }

            timespec ts;
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
            uint64_t now_ns = ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
            latencies_.push_back(now_ns - msg.timestamp);
        }
    }

    Fast::FastBitsetOrderBook book_;
    NaiveQueue& queue_;
    std::thread thread_;
    std::vector<uint64_t> latencies_;
};
