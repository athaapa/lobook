#include "engine/fast_bitset_book.h"
#include <cassert>

// Existing bid-side tests

static void test_price_change_resets_priority_bid() {
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

static void test_increase_qty_resets_priority_bid() {
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

static void test_decrease_qty_keeps_priority_bid() {
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

static void test_noop_replace_keeps_priority_bid() {
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

static void test_replace_invalid_id_is_noop_bid() {
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

// New ask-side mirror tests

static void test_price_change_resets_priority_ask() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    // FIFO at same ask price: id=11 then id=12
    book.submit_order(11, 100, 10, false);
    book.submit_order(12, 100, 10, false);

    // Price change away and back should reset priority for id=11.
    book.replace_order(11, 101, 10);
    book.replace_order(11, 100, 10);

    // Aggressive buy should hit best ask at 100.
    book.submit_order(2000, 100, 10, true);

    // id=12 should fill first after id=11 reset.
    assert(book.get_quantity(12) == 0);
    assert(book.get_quantity(11) == 10);
}

static void test_increase_qty_resets_priority_ask() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(13, 100, 10, false);
    book.submit_order(14, 100, 10, false);

    // Increase qty at same price => reset priority
    book.replace_order(13, 100, 20);

    // Aggressive buy should consume head of ask queue.
    book.submit_order(2001, 100, 10, true);

    // id=14 should go first if id=13 was reset.
    assert(book.get_quantity(14) == 0);
    assert(book.get_quantity(13) == 20);
}

static void test_decrease_qty_keeps_priority_ask() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(15, 100, 20, false);
    book.submit_order(16, 100, 10, false);

    // Decrease qty at same price => keep priority
    book.replace_order(15, 100, 10);

    book.submit_order(2002, 100, 10, true);

    // id=15 should remain first in queue.
    assert(book.get_quantity(15) == 0);
    assert(book.get_quantity(16) == 10);
}

// Fill-state scenarios

static void test_replace_after_partial_fill_bid() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    // id=21 partially filled first
    book.submit_order(21, 100, 20, true);
    book.submit_order(22, 100, 10, true);

    // Partial fill 5 against id=21
    book.submit_order(3000, 100, 5, false);
    assert(book.get_quantity(21) == 15);
    assert(book.get_quantity(22) == 10);

    // Decrease remaining qty at same price should preserve priority
    book.replace_order(21, 100, 10);

    // Next 10 should still hit id=21 first
    book.submit_order(3001, 100, 10, false);

    assert(book.get_quantity(21) == 0);
    assert(book.get_quantity(22) == 10);
}

static void test_replace_fully_filled_order_is_noop_bid() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(31, 100, 10, true);

    // Fully fill id=31
    book.submit_order(4000, 100, 10, false);
    assert(book.get_quantity(31) == 0);

    // Replace should not resurrect order or affect other flow.
    book.replace_order(31, 101, 50);

    // Add fresh liquidity and verify normal matching unaffected.
    book.submit_order(32, 100, 10, true);
    book.submit_order(4001, 100, 10, false);

    assert(book.get_quantity(32) == 0);
    assert(book.get_quantity(31) == 0);
}

int main() {
    test_price_change_resets_priority_bid();
    test_increase_qty_resets_priority_bid();
    test_decrease_qty_keeps_priority_bid();
    test_noop_replace_keeps_priority_bid();
    test_replace_invalid_id_is_noop_bid();

    test_price_change_resets_priority_ask();
    test_increase_qty_resets_priority_ask();
    test_decrease_qty_keeps_priority_ask();

    test_replace_after_partial_fill_bid();
    test_replace_fully_filled_order_is_noop_bid();

    return 0;
}