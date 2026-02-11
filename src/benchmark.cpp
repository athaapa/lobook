#include <iostream>
#include <chrono>

// Include your two engines
#include "engine/simple_book.h"
#include "engine/fast_book.h"

// --- TIMING HELPER ---
class Timer {
    std::chrono::high_resolution_clock::time_point start;
public:
    Timer() { reset(); }
    void reset() { start = std::chrono::high_resolution_clock::now(); }
    double elapsed_ms() {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end - start).count();
    }
};

const int NUM_ORDERS = 1000000; // 1 Million Orders

void benchmark_naive() {
    std::cout << "--- NAIVE BOOK (STL Maps) ---\n";
    Naive::NaiveOrderBook book;
    Timer t;

    // 1. INSERTION
    t.reset();
    for(int i = 0; i < NUM_ORDERS; ++i) {
        // ID, Price, Qty, IsBuy
        book.add_order(i, 100, 10, true);
    }
    double add_time = t.elapsed_ms();
    std::cout << "Add 1M Orders: " << add_time << " ms ("
              << (NUM_ORDERS / add_time) * 1000 << " ops/sec)\n";

    // 2. MATCHING (Sweep)
    // Sell 10M shares @ 90 (Crosses everyone)
    t.reset();
    // ID=MAX, Price=90, Qty=Huge, IsBuy=False
    book.add_order(NUM_ORDERS + 1, 90, NUM_ORDERS * 10, false);
    double match_time = t.elapsed_ms();
    std::cout << "Match All:     " << match_time << " ms\n";
}

void benchmark_fast() {
    std::cout << "\n--- FAST BOOK (Array Pool) ---\n";
    Fast::FastOrderBook book;
    book.init(NUM_ORDERS + 100); // Pre-allocate
    Timer t;

    // 1. INSERTION
    t.reset();
    for(int i = 0; i < NUM_ORDERS; ++i) {
        // Note: submit_order calls match() then add_order()
        book.submit_order(i, 100, 10, true);
    }
    double add_time = t.elapsed_ms();
    std::cout << "Add 1M Orders: " << add_time << " ms ("
              << (NUM_ORDERS / add_time) * 1000 << " ops/sec)\n";

    // 2. MATCHING (Sweep)
    t.reset();
    // ID=MAX, Price=90, Qty=Huge, IsBuy=False
    book.submit_order(NUM_ORDERS + 1, 90, NUM_ORDERS * 10, false);
    double match_time = t.elapsed_ms();
    std::cout << "Match All:     " << match_time << " ms\n";
}

int main() {
    benchmark_naive();
    benchmark_fast();
    return 0;
}
