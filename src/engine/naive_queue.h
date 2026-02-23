#pragma once
#include "order_message.h"
#include <condition_variable>
#include <mutex>
#include <queue>

class NaiveQueue {
public:
    void push(const OrderMessage& msg)
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push(msg);
        }
        cv_.notify_one();
    }
    OrderMessage pop()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [&] { return !queue_.empty(); });
        OrderMessage msg = queue_.front();
        queue_.pop();
        return msg;
    };
    bool try_pop(OrderMessage& out)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.empty())
            return false;
        out = queue_.front();
        queue_.pop();
        return true;
    }

private:
    std::queue<OrderMessage> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
};
