#pragma once
#include "order_message.h"
#include <condition_variable>
#include <mutex>
#include <queue>

// Q: Why is this called "NaiveQueue"? What would a faster alternative look like?
// A: The goal is to make an SPSC (Single Producer Single Consumer) queue. That way, we will never
// have any sleeping threads. We want every cycle to count. Right now, we have mutexes and guards
// that prevent data races, but if we have SPSC, we don't need that (since we only have a single
// producer and a single consumer).

class NaiveQueue {
public:
    void push(const OrderMessage& msg) {
        {
            // Q: Why release the lock before calling notify_one() (i.e., why the inner braces)?
            // A: The lock only releases after it goes out of scope. If I call notify_one() while
            //    the lock is in scope, the pop() method will try to take control while the lock is
            //    active.
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push(msg);
        }
        cv_.notify_one();
    }

    // Q: Why does pop() use unique_lock while push() and try_pop() use lock_guard?
    // A: condition_variable.wait() takes in a unique_lock rather than a lock_guard. The reason for
    // this
    //    is that lock_guard does not support locking and unlocking before it goes out of scope,
    //    which is what std::condition_variable needs.
    OrderMessage pop() {
        std::unique_lock<std::mutex> lock(mtx_);
        // Q: Why pass a lambda predicate to wait() instead of calling wait(lock) directly?
        // A: To guard against spurious wakeups — the OS can wake a sleeping thread even when
        //    notify_one() was never called. Without the predicate, pop() would proceed on an
        //    empty queue and crash. The predicate-based overload re-checks the condition after
        //    every wakeup and goes back to sleep if it's still false.
        cv_.wait(lock, [&] { return !queue_.empty(); });
        OrderMessage msg = queue_.front();
        queue_.pop();
        return msg;
    };

    bool try_pop(OrderMessage& out) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.empty())
            return false;
        out = queue_.front();
        queue_.pop();
        return true;
    }

private:
    // Q: Why std::queue and not std::deque or std::vector directly?
    // A: A FIFO queue needs two cheap operations: push to the back, pop from the front.
    //    std::deque (which std::queue adapts) gives O(1) for both. std::vector is O(n)
    //    for pop_front (it must shift all elements). std::queue wraps std::deque and
    //    restricts the interface to only the operations a queue actually needs.
    std::queue<OrderMessage> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
};
