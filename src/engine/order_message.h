#pragma once
#include <cstdint>

// Q: Why use enum class instead of a plain enum?
// Q: Why explicitly set the underlying type to uint8_t?
enum class Type : uint8_t {
    SUBMIT,
    CANCEL,
    SHUTDOWN
};

struct OrderMessage {
    Type type;
    uint64_t id;
    // Q: Why is price a uint64_t rather than a float or double?
    uint64_t price;
    uint32_t qty;
    bool is_buy;
    // Q: Why store the timestamp on the message (at enqueue time) rather than
    // stamping it inside the engine when the message is dequeued?
    uint64_t timestamp;
};
