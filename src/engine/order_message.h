#pragma once
#include <cstdint>

// Q: Why use enum class instead of a plain enum?
// A: Plain enums export their enumerators into the surrounding scope, causing namespace
//    pollution. Enum class scopes its enumerators within the enumerator itself.
// Q: Why explicitly set the underlying type to uint8_t?
// A: We want to be explicit about the wire format since we are sending OrderMessage over
// network/writing to shared memory.
//    Another reason is that we don't need to use more space than we need to. On most systems, the
//    enum will default to a 4 byte integer, but we don't need that much, so we can do with a 1 byte
//    integer.
enum class Type : uint8_t { SUBMIT, CANCEL, SHUTDOWN };

struct OrderMessage {
    uint64_t id;
    // Q: Why is price a uint64_t rather than a float or double?
    // A: Floats can't exactly represent most decimal fractions (e.g. 0.1 + 0.2 != 0.3 in
    //    IEEE 754). If two prices that should be equal compare as unequal due to rounding,
    //    a trade that should happen gets missed (or wrongly triggered). Fixed-point integers
    //    (e.g. price in cents or ticks) sidestep this entirely.
    uint64_t price;
    uint32_t qty;
    // Q: Why store the timestamp on the message (at enqueue time) rather than
    // stamping it inside the engine when the message is dequeued?
    // A: To measure the end-to-end latency from when the order is submitted to when
    //    it gets matched. Stamping at dequeue time would only measure processing time
    //    and would miss however long the message sat waiting in the queue.
    uint64_t timestamp;

    bool is_buy;
    Type type;
};
