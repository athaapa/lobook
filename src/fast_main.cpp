#include <iostream>
#include "engine/fast_book.h"

int main() {
    FastOrderBook book;
    book.init(100000);

    book.submit_order(1, 100, 100, true);
    book.submit_order(2, 100, 100, true);
    std::cout << "Added two buy orders\n";

    book.submit_order(3, 100, 150, false);
    std::cout << "Executed sell order\n";

    uint32_t qty1 = book.get_quantity(1);
    std::cout << "Order 1 Qty: " << qty1 << " (Expected: 0)\n";

    // Verify Order 2 (Should be partially filled)
    uint32_t qty2 = book.get_quantity(2);
    std::cout << "Order 2 Qty: " << qty2 << " (Expected: 50)\n";
}
