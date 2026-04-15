#include "engine/fast_bitset_book.h"
#include <cassert>

static void test_price_change_resets_priority() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, true);
    book.submit_order(2, 100, 10, true);

    // Changing price -> resets priority
    book.replace_order(1, 99, 10);
    book.replace_order(1, 100, 10);

    book.submit_order(1000, 100, 10, false);

    assert(book.get_quantity(2) == 0);
    assert(book.get_quantity(1) == 10);
}

static void test_increase_qty_resets_priority() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, true);
    book.submit_order(2, 100, 10, true);

    // Increasing quantity -> resets priority
    book.replace_order(1, 99, 10);
    book.replace_order(1, 100, 10);

    book.submit_order(1000, 100, 10, false);

    assert(book.get_quantity(2) == 0);
    assert(book.get_quantity(1) == 20);
}

static void test_decrease_qty_keeps_priority() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, true);
    book.submit_order(2, 100, 10, true);

    // Decreasing quantity -> keeps priority
    book.replace_order(1, 99, 10);
    book.replace_order(1, 100, 10);

    book.submit_order(1000, 100, 10, false);

    assert(book.get_quantity(1) == 0);
    assert(book.get_quantity(2) == 10);
}

int main() {
    test_price_change_resets_priority();
    test_increase_qty_resets_priority();
    test_decrease_qty_keeps_priority();
    return 0;
}
