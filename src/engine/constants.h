#pragma once
#include <cstddef>

namespace Fast {
    // Q: Why is MAX_PRICES a compile-time constant rather than a runtime parameter?
    // Q: What does each index in the price ladder physically represent, and how would
    //    you choose this value for a real instrument?
    static constexpr size_t MAX_PRICES = 100'000;
}
