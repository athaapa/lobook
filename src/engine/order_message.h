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
    // A: To guarantee that it is always a 64-bit unsigned integer.
    uint64_t price;
    uint32_t qty;
    bool is_buy;
    // Q: Why store the timestamp on the message (at enqueue time) rather than
    // stamping it inside the engine when the message is dequeued?
    // A (TODO): I want to store the time the order was made..?
    uint64_t timestamp;
};
