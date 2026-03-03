#pragma once
#include <cstdint>

// Q: Why use enum class instead of a plain enum?
// A (TODO)
// Q: Why explicitly set the underlying type to uint8_t?
// A (TODO)
enum class Type : uint8_t {
    SUBMIT,
    CANCEL,
    SHUTDOWN
};

struct OrderMessage {
    Type type;
    uint64_t id;
    // Q: Why is price a uint64_t rather than a float or double?
    // A: Floats can't exactly represent most decimal fractions (e.g. 0.1 + 0.2 != 0.3 in
    //    IEEE 754). If two prices that should be equal compare as unequal due to rounding,
    //    a trade that should happen gets missed (or wrongly triggered). Fixed-point integers
    //    (e.g. price in cents or ticks) sidestep this entirely.
    uint64_t price;
    uint32_t qty;
    bool is_buy;
    // Q: Why store the timestamp on the message (at enqueue time) rather than
    // stamping it inside the engine when the message is dequeued?
    // A: To measure the end-to-end latency from when the order is submitted to when
    //    it gets matched. Stamping at dequeue time would only measure processing time
    //    and would miss however long the message sat waiting in the queue.
    uint64_t timestamp;
};
