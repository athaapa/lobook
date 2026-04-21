#include "engine/constants.h"
#include "engine/fast_bitset_book.h"
#include <cassert>

static void test_sweep_multiple_levels() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, true);
    book.submit_order(2, 99, 10, true);
    book.submit_order(3, 98, 10, true);

    // Aggressive sell sweeps 100 and 99
    book.submit_order(4, 99, 15, false);

    assert(book.get_quantity(1) == 0); // fully filled
    assert(book.get_quantity(2) == 5); // partial fill
    assert(book.get_quantity(3) == 10); // untouched
    assert(book.get_quantity(4) == 0);
}

static void test_price_boundary_guard() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, false);
    book.submit_order(2, 101, 10, false);
    book.submit_order(3, 102, 10, false);

    // Aggressive buy only willing to pay 101, qty 25
    book.submit_order(4, 101, 25, true);

    assert(book.get_quantity(1) == 0); // filled at 100
    assert(book.get_quantity(2) == 0); // filled at 101
    assert(book.get_quantity(3) == 10); // untouched (price > 101)

    // Remaining buy qty rests on book at 101 (qty 5)
    assert(book.get_quantity(4) == 5);
}

static void test_drain_book() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, true);
    book.submit_order(2, 99, 10, true);

    // Sell of 100 exhausts entire bid side
    book.submit_order(3, 90, 100, false);

    assert(book.get_quantity(1) == 0);
    assert(book.get_quantity(2) == 0);

    // Remaining 80 rests on book at 90
    assert(book.get_quantity(3) == 80);
}

static void test_bad_price() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    // Submitting a price out of bounds of MAX_PRICES should hit the fail safe
    // Note: FastBitsetOrderBook size is MAX_PRICES
    book.submit_order(1, Fast::MAX_PRICES + 5, 10, true);
    assert(book.get_quantity(1) == 0); // Not successfully queued
}

int main() {
    test_sweep_multiple_levels();
    test_price_boundary_guard();
    test_drain_book();
    test_bad_price();
    return 0;
}
