#pragma once
#include "order_message.h"
#include <condition_variable>
#include <mutex>
#include <queue>

// Q: Why is this called "NaiveQueue"? What would a faster alternative look like?

class NaiveQueue {
public:
    void push(const OrderMessage& msg)
    {
        {
            // Q: Why release the lock before calling notify_one() (i.e., why the inner braces)?
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push(msg);
        }
        cv_.notify_one();
    }

    // Q: Why does pop() use unique_lock while push() and try_pop() use lock_guard?
    OrderMessage pop()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        // Q: Why pass a lambda predicate to wait() instead of calling wait(lock) directly?
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
    // Q: Why std::queue and not std::deque or std::vector directly?
    std::queue<OrderMessage> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
};
