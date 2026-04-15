#include "engine/fast_bitset_book.h"
#include <cassert>

static void test_price_change_resets_priority() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    // FIFO at same price starts as: id=1 then id=2
    book.submit_order(1, 100, 10, true);
    book.submit_order(2, 100, 10, true);

    // Price change should reset priority of id=1.
    book.replace_order(1, 99, 10);
    book.replace_order(1, 100, 10);

    // Aggressive sell fills highest-priority bid first.
    book.submit_order(1000, 100, 10, false);

    // id=2 should fill first after id=1 priority reset.
    assert(book.get_quantity(2) == 0);
    assert(book.get_quantity(1) == 10);
}

static void test_increase_qty_resets_priority() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, true);
    book.submit_order(2, 100, 10, true);

    // Increasing quantity at same price should reset priority.
    book.replace_order(1, 100, 20);

    // Fill one lot from best bid at 100.
    book.submit_order(1001, 100, 10, false);

    // id=2 should fill first if id=1 lost priority.
    assert(book.get_quantity(2) == 0);
    assert(book.get_quantity(1) == 20);
}

static void test_decrease_qty_keeps_priority() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    // Give id=1 enough size so we can decrease and still test fill ordering.
    book.submit_order(1, 100, 20, true);
    book.submit_order(2, 100, 10, true);

    // Decreasing quantity at same price should keep priority.
    book.replace_order(1, 100, 10);

    book.submit_order(1002, 100, 10, false);

    // id=1 should still be first.
    assert(book.get_quantity(1) == 0);
    assert(book.get_quantity(2) == 10);
}

static void test_noop_replace_keeps_priority() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, true);
    book.submit_order(2, 100, 10, true);

    // No-op replace (same price, same qty): should keep priority.
    book.replace_order(1, 100, 10);

    book.submit_order(1003, 100, 10, false);

    // id=1 should still fill first.
    assert(book.get_quantity(1) == 0);
    assert(book.get_quantity(2) == 10);
}

static void test_replace_invalid_id_is_noop() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, true);
    book.submit_order(2, 100, 10, true);

    // Invalid ID replace should not perturb book state or priority of live orders.
    book.replace_order(999999, 90, 123);

    book.submit_order(1004, 100, 10, false);

    // Original ordering should remain intact: id=1 fills first.
    assert(book.get_quantity(1) == 0);
    assert(book.get_quantity(2) == 10);
}

int main() {
    test_price_change_resets_priority();
    test_increase_qty_resets_priority();
    test_decrease_qty_keeps_priority();
    test_noop_replace_keeps_priority();
    test_replace_invalid_id_is_noop();
    return 0;
}