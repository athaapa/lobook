#include "engine/price_bitset.h"
#include <cassert>

static void test_empty_state() {
    Fast::PriceBitset bids;
    Fast::PriceBitset asks;

    assert(bids.find_highest() == std::nullopt);
    assert(bids.find_lowest() == std::nullopt);
    assert(asks.find_highest() == std::nullopt);
    assert(asks.find_lowest() == std::nullopt);
    assert(bids.empty());
    assert(asks.empty());
}

static void test_single_bid() {
    Fast::PriceBitset bids;
    bids.set(100);
    assert(bids.find_highest() == 100);
    assert(!bids.empty());
}

static void test_multiple_bids() {
    Fast::PriceBitset bids;
    bids.set(5009);
    bids.set(475);
    bids.set(76054);
    assert(bids.find_highest() == 76054);

    bids.clear(76054);
    assert(bids.find_highest() == 5009);

    bids.clear_all();
    assert(bids.empty());
}

static void test_boundaries() {
    Fast::PriceBitset bids;

    bids.set(0);
    assert(bids.find_highest() == 0);
    assert(bids.find_lowest() == 0);
    bids.clear_all();

    bids.set(63);
    assert(bids.find_highest() == 63);
    bids.clear_all();

    bids.set(64);
    assert(bids.find_highest() == 64);
    bids.clear_all();

    bids.set(4096);
    assert(bids.find_highest() == 4096);
    bids.clear_all();
}

static void test_asks() {
    Fast::PriceBitset asks;
    asks.set(1000);
    assert(asks.find_lowest() == 1000);
    asks.set(100);
    asks.set(500);
    asks.set(25007);
    assert(asks.find_lowest() == 100);

    asks.clear(100);
    assert(asks.find_lowest() == 500);

    asks.clear_all();
    assert(asks.empty());
}

static void test_independence() {
    Fast::PriceBitset bids;
    Fast::PriceBitset asks;

    bids.set(500);
    asks.set(600);
    assert(bids.find_highest() == 500);
    assert(asks.find_lowest() == 600);
    assert(!bids.test(600));
    assert(!asks.test(500));
}

static void test_test_method() {
    Fast::PriceBitset bids;
    bids.set(42);
    bids.set(999);
    assert(bids.test(42));
    assert(bids.test(999));
    assert(!bids.test(43));
    assert(!bids.test(0));

    bids.clear(42);
    assert(!bids.test(42));
    assert(bids.test(999));
}

int main() {
    test_empty_state();
    test_single_bid();
    test_multiple_bids();
    test_boundaries();
    test_asks();
    test_independence();
    test_test_method();
    return 0;
}