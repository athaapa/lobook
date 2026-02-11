#include "engine/simple_book.h"
#include <iostream>

int main() {
    NaiveOrderBook book;

    book.add_order(1, 100, 100, true);
    book.add_order(2, 100, 100, true);
    book.add_order(3, 100, 150, false);
    std::cout << "Total trades: " << book.total_trades << "\n";
    book.cancel_order(1);
    if (book.has_order(2))
        std::cout << "Order 2 in lookup\n";

    book.cancel_order(2);

    if (!book.has_order(2))
        std::cout << "Order 2 removed from lookup\n";
    book.debug_print();
    book.add_order(4, 100, 10, false);
    std::cout << "Total trades: " << book.total_trades << "\n";
}
