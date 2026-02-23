#include "engine/price_bitset.h"
#include <cassert>

int main()
{
    Fast::PriceBitset bids;
    Fast::PriceBitset asks;

    // Empty state
    assert(bids.find_highest() == std::nullopt);
    assert(bids.find_lowest() == std::nullopt);
    assert(asks.find_highest() == std::nullopt);
    assert(asks.find_lowest() == std::nullopt);
    assert(bids.empty());
    assert(asks.empty());

    // Single bid — best bid is highest
    bids.set(100);
    assert(bids.find_highest() == 100);
    assert(!bids.empty());

    // Multiple bids — best bid is highest
    bids.set(5009);
    bids.set(475);
    bids.set(76054);
    assert(bids.find_highest() == 76054);

    // Clear highest — next highest becomes best
    bids.clear(76054);
    assert(bids.find_highest() == 5009);

    bids.clear_all();
    assert(bids.empty());

    // Boundary: price 0
    bids.set(0);
    assert(bids.find_highest() == 0);
    assert(bids.find_lowest() == 0);
    bids.clear_all();

    // Boundary: price 63 (last bit of first l2 word)
    bids.set(63);
    assert(bids.find_highest() == 63);
    bids.clear_all();

    // Boundary: price 64 (first bit of second l2 word)
    bids.set(64);
    assert(bids.find_highest() == 64);
    bids.clear_all();

    // Boundary: price 4096 (crosses l1 word boundary)
    bids.set(4096);
    assert(bids.find_highest() == 4096);
    bids.clear_all();

    // Ask side — best ask is lowest
    asks.set(1000);
    assert(asks.find_lowest() == 1000);
    asks.set(100);
    asks.set(500);
    asks.set(25007);
    assert(asks.find_lowest() == 100);

    // Clear lowest — next lowest becomes best
    asks.clear(100);
    assert(asks.find_lowest() == 500);

    asks.clear_all();
    assert(asks.empty());

    // Bids and asks are independent instances
    bids.set(500);
    asks.set(600);
    assert(bids.find_highest() == 500);
    assert(asks.find_lowest() == 600);
    assert(!bids.test(600));
    assert(!asks.test(500));

    bids.clear_all();
    asks.clear_all();

    // Test test()
    bids.set(42);
    bids.set(999);
    assert(bids.test(42));
    assert(bids.test(999));
    assert(!bids.test(43));
    assert(!bids.test(0));

    bids.clear(42);
    assert(!bids.test(42));
    assert(bids.test(999));

    bids.clear_all();
}