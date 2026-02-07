#include <iostream>
#include <chrono>
#include <random>
#include "naive/simple_book.h"

int main() {
    NaiveOrderBook book;

    // Better random number generation
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> buy_sell_dist(0, 1);
    std::uniform_int_distribution<> qty_dist(1, 100);

    std::uniform_int_distribution<int> bid_price_dist(90000, 95000);
    std::uniform_int_distribution<int> ask_price_dist(105000, 110000);

    const int TOTAL_ORDERS = 1000000;

    std::cout << "Starting benchmark with " << TOTAL_ORDERS << " orders..." << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < TOTAL_ORDERS; ++i) {
        bool is_buy = buy_sell_dist(gen);

        uint64_t price;
        if (is_buy) {
            price = bid_price_dist(gen);
        } else {
            price = ask_price_dist(gen);
        }

        uint32_t qty = qty_dist(gen);
        book.add_order(i, price, qty, is_buy);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "Finished in " << diff.count() << " seconds." << std::endl;
    std::cout << "Throughput: " << (TOTAL_ORDERS / diff.count()) / 1000.0 << "k orders/sec" << std::endl;
    std::cout << "Final Trade Count: " << book.total_trades << "\n";

    return 0;
}
