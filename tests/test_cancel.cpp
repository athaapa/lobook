#include "engine/fast_bitset_book.h"
#include <cassert>

static void test_basic_cancel() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, true);
    assert(book.get_quantity(1) == 10);

    book.cancel_order(1);
    assert(book.get_quantity(1) == 0);

    // Ensure it doesn't match
    book.submit_order(2, 100, 10, false);
    assert(book.get_quantity(2) == 10); // Ask stays on book because bid 1 was canceled
}

static void test_cancel_empties_level() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, true);
    book.cancel_order(1);

    // If level is not properly cleared in the index bitset, an aggressive ask might try to match empty orders
    book.submit_order(2, 100, 10, false);
    assert(book.get_quantity(1) == 0);
    assert(book.get_quantity(2) == 10);
}

static void test_cancel_head_and_tail() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, true);
    book.submit_order(2, 100, 10, true);
    book.submit_order(3, 100, 10, true);

    // Cancel head
    book.cancel_order(1);
    
    // Aggressive sell of 10 should hit id=2
    book.submit_order(4, 100, 10, false);
    assert(book.get_quantity(2) == 0);
    assert(book.get_quantity(3) == 10);

    // Cancel tail
    book.cancel_order(3);
    
    // Aggressive sell of 10 should stay on book (no bids left at 100)
    book.submit_order(5, 100, 10, false);
    assert(book.get_quantity(5) == 10);
}

static void test_cancel_invalid_id() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, true);
    
    // Cancel non-existent ID
    book.cancel_order(999);
    
    // Cancel ID out of bounds
    book.cancel_order(200000);

    assert(book.get_quantity(1) == 10);
}

static void test_cancel_fully_filled() {
    Fast::FastBitsetOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 10, true);
    book.submit_order(2, 100, 10, false); // Fills 1 completely

    assert(book.get_quantity(1) == 0);

    // Canceling a fully filled order should be a no-op
    book.cancel_order(1);
    assert(book.get_quantity(1) == 0);
}

int main() {
    test_basic_cancel();
    test_cancel_empties_level();
    test_cancel_head_and_tail();
    test_cancel_invalid_id();
    test_cancel_fully_filled();
    return 0;
}
