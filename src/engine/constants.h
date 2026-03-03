#pragma once
#include <cstddef>

namespace Fast {
// Q: Why is MAX_PRICES a compile-time constant rather than a runtime parameter?
// A: MAX_PRICES is used as the length to a std::array, which requires its size to be known at compile-time.
// Q: What does each index in the price ladder physically represent, and how would
//    you choose this value for a real instrument?
// A: Each index in the price ladder represents a dollar (in this scenario). In reality, it would depend on the financial instrument.
//    The price range [0,100k] is not sufficient for any and all instruments. The context will change the interpretation of MAX_PRICES.
//    e.g. an instrument that trades at a low dollar value but has high precision within its range will have each price level being
//    some fraction of a dollar (albeit with a total range).
static constexpr size_t MAX_PRICES = 100'000;
}
