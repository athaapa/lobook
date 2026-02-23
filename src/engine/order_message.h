#pragma once
#include <cstdint>

enum class Type : uint8_t {
    SUBMIT,
    CANCEL,
    SHUTDOWN
};

struct OrderMessage {
    Type type;
    uint64_t id;
    uint64_t price;
    uint32_t qty;
    bool is_buy;
    uint64_t timestamp;
};
