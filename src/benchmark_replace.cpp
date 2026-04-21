// Native FastBitsetBook::replace_order vs cancel_order + submit_order.
//
// Workload: 1M random replace operations against a book of 10k live orders.
// Bid and ask price ranges are disjoint so cancel+submit cannot accidentally
// cross and trigger matching - this isolates the data-structure cost.
//
// Per-operation timing for the cancel and submit halves of scenario B is
// captured with rdtsc; the calibrated invariant TSC frequency converts cycles
// to nanoseconds.

#include "bench_common.h"
#include "engine/fast_bitset_book.h"
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

using lobook::bench::PerfCounters;
using lobook::bench::Timer;

namespace {

constexpr size_t kMaxOrders = 100'000;
constexpr uint64_t kMaxPrices = Fast::MAX_PRICES;
constexpr int kLiveOrders = 10'000;
constexpr int kNumOps = 1'000'000;
constexpr uint32_t kRngSeed = 123;

struct Operation {
    uint64_t id;
    uint64_t new_price;
    uint32_t new_qty;
    bool is_buy;
};

// Sides occupy disjoint price ranges so cancel+submit cannot cross.
// Bids:  [100, MAX/2 - 1].  Asks:  [MAX/2, MAX - 100].
std::uniform_int_distribution<uint64_t> bid_dist(100, kMaxPrices / 2 - 1);
std::uniform_int_distribution<uint64_t> ask_dist(kMaxPrices / 2, kMaxPrices - 100);
std::uniform_int_distribution<uint32_t> qty_dist(1, 100);

void seed_book(Fast::FastBitsetOrderBook& book, std::mt19937& rng) {
    book.init(kMaxOrders);
    for (int i = 0; i < kLiveOrders; ++i) {
        bool is_buy = (i % 2) == 0;
        uint64_t price = is_buy ? bid_dist(rng) : ask_dist(rng);
        book.submit_order(i, price, qty_dist(rng), is_buy);
    }
}

std::vector<Operation> generate_ops(std::mt19937& rng) {
    std::vector<Operation> ops;
    ops.reserve(kNumOps);
    for (int i = 0; i < kNumOps; ++i) {
        uint64_t target_id = rng() % kLiveOrders;
        bool is_buy = (target_id % 2) == 0;
        uint64_t price = is_buy ? bid_dist(rng) : ask_dist(rng);
        uint32_t qty = qty_dist(rng);
        ops.push_back({ target_id, price, qty, is_buy });
    }
    return ops;
}

} // namespace

int main() {
    lobook::bench::print_environment_banner("replace_order vs cancel + submit");
    lobook::bench::print_kv("max prices", std::to_string(kMaxPrices));
    lobook::bench::print_kv("max orders", std::to_string(kMaxOrders));
    lobook::bench::print_kv("live orders", std::to_string(kLiveOrders));
    lobook::bench::print_kv("operations", std::to_string(kNumOps));
    lobook::bench::print_kv("rng seed", std::to_string(kRngSeed));

    const double tsc_hz = lobook::bench::calibrate_tsc_hz();

    // Generate the workload once; both scenarios consume the identical sequence.
    std::mt19937 op_rng(kRngSeed);
    auto ops = generate_ops(op_rng);

    Fast::FastBitsetOrderBook book;

    // -----------------------------------------------------------------------
    //  Scenario A - native replace
    // -----------------------------------------------------------------------
    lobook::bench::print_section("scenario A / native replace");
    {
        std::mt19937 seed_rng(kRngSeed + 1);
        seed_book(book, seed_rng);
    }

    PerfCounters perf_a;
    perf_a.start();
    Timer t_a;
    for (const auto& op : ops)
        book.replace_order(op.id, op.new_price, op.new_qty);
    double elapsed_a = t_a.elapsed_ms();
    perf_a.stop();

    lobook::bench::print_runtime(elapsed_a, kNumOps);
    perf_a.print_summary(kNumOps);

    // -----------------------------------------------------------------------
    //  Scenario B - cancel + submit (with rdtsc split timing)
    // -----------------------------------------------------------------------
    lobook::bench::print_section("scenario B / cancel + submit");
    {
        std::mt19937 seed_rng(kRngSeed + 1);
        seed_book(book, seed_rng);
    }

    uint64_t cancel_cycles_total = 0;
    uint64_t submit_cycles_total = 0;

    PerfCounters perf_b;
    perf_b.start();
    Timer t_b;
    for (const auto& op : ops) {
        uint64_t t0 = __rdtsc();
        book.cancel_order(op.id);
        uint64_t t1 = __rdtsc();
        book.submit_order(op.id, op.new_price, op.new_qty, op.is_buy);
        uint64_t t2 = __rdtsc();
        cancel_cycles_total += t1 - t0;
        submit_cycles_total += t2 - t1;
    }
    double elapsed_b = t_b.elapsed_ms();
    perf_b.stop();

    lobook::bench::print_runtime(elapsed_b, kNumOps);
    perf_b.print_summary(kNumOps);

    double cancel_ns = (double)cancel_cycles_total / kNumOps / tsc_hz * 1e9;
    double submit_ns = (double)submit_cycles_total / kNumOps / tsc_hz * 1e9;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "      cancel        " << std::setw(8) << cancel_ns << " ns/op  (rdtsc split)\n";
    std::cout << "      submit        " << std::setw(8) << submit_ns << " ns/op  (rdtsc split)\n";

    lobook::bench::print_done();
    return 0;
}
