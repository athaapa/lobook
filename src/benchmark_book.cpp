// FastBook (linear price scan) vs FastBitsetBook (hierarchical bitset).
//
// Three workloads exercise different parts of the engine:
//   1. Sparse insert + sweep    -- many price levels, one big crossing trade
//   2. Wide-spread matching     -- asks at top of ladder, buys cross one-by-one
//   3. Mixed churn              -- 100k random submit / cancel / aggressive ops
//
// Both books expose the same generic interface via runner shims so they can
// share scenario code without ODR conflicts (they both live in `Fast::`).

#include "bench_common.h"
#include "engine/constants.h"
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

// Forward-declared runner interfaces (defined in bench_runners/*.cpp).
namespace FastBookRunner {
void init(size_t max_orders);
void submit_order(uint64_t id, uint64_t price, uint32_t qty, bool is_buy);
void cancel_order(uint64_t id);
uint32_t get_quantity(uint64_t id);
}
namespace BitsetBookRunner {
void init(size_t max_orders);
void submit_order(uint64_t id, uint64_t price, uint32_t qty, bool is_buy);
void cancel_order(uint64_t id);
uint32_t get_quantity(uint64_t id);
}

using lobook::bench::PerfCounters;
using lobook::bench::Timer;

namespace {

constexpr size_t kMaxPrices = Fast::MAX_PRICES;
constexpr size_t kMaxOrders = 500'000;

struct BookOps {
    const char* name;
    void (*init)(size_t);
    void (*submit_order)(uint64_t, uint64_t, uint32_t, bool);
    void (*cancel_order)(uint64_t);
    uint32_t (*get_quantity)(uint64_t);
};

const BookOps fast_ops {
    "FastBook (linear scan)",
    FastBookRunner::init, FastBookRunner::submit_order,
    FastBookRunner::cancel_order, FastBookRunner::get_quantity
};

const BookOps bitset_ops {
    "BitsetBook (hierarchical bitset)",
    BitsetBookRunner::init, BitsetBookRunner::submit_order,
    BitsetBookRunner::cancel_order, BitsetBookRunner::get_quantity
};

// ---------------------------------------------------------------------------
//  Scenario 1 - Sparse insert + sweep
// ---------------------------------------------------------------------------
constexpr int kSparseLevels = 50;
constexpr int kSparseOrdersPerLevel = 20;
constexpr int kSparseRounds = 200;

void scenario_sparse(const BookOps& ops) {
    lobook::bench::print_section(
        (std::string("scenario 1 / sparse insert + sweep / ") + ops.name).c_str());
    lobook::bench::print_kv("setup",
        std::to_string(kSparseLevels) + " price levels x "
            + std::to_string(kSparseOrdersPerLevel) + " orders x "
            + std::to_string(kSparseRounds) + " rounds, then 1 sweeping order");

    ops.init(kMaxOrders);

    std::vector<uint64_t> prices;
    prices.reserve(kSparseLevels);
    for (int i = 0; i < kSparseLevels; ++i)
        prices.push_back((uint64_t)i * (kMaxPrices / kSparseLevels) + 1);

    uint64_t next_id = 0;
    const int total_inserted = kSparseRounds * kSparseLevels * kSparseOrdersPerLevel;

    Timer t;
    for (int round = 0; round < kSparseRounds; ++round)
        for (int i = 0; i < kSparseLevels; ++i)
            for (int j = 0; j < kSparseOrdersPerLevel; ++j)
                ops.submit_order(next_id++, prices[i], 1, false);
    double insert_ms = t.elapsed_ms();
    std::cout << "  -- insert phase\n";
    lobook::bench::print_runtime(insert_ms, total_inserted);

    PerfCounters perf;
    perf.start();
    t.reset();
    ops.submit_order(next_id++, kMaxPrices - 1, total_inserted, true);
    double match_ms = t.elapsed_ms();
    perf.stop();
    std::cout << "  -- match sweep phase\n";
    lobook::bench::print_runtime(match_ms, total_inserted);
    perf.print_summary(total_inserted);
}

// ---------------------------------------------------------------------------
//  Scenario 2 - Wide spread matching
// ---------------------------------------------------------------------------
constexpr int kWideOrders = 10'000;

void scenario_wide(const BookOps& ops) {
    lobook::bench::print_section(
        (std::string("scenario 2 / wide spread / ") + ops.name).c_str());
    lobook::bench::print_kv("setup",
        std::to_string(kWideOrders) + " asks at top, then "
            + std::to_string(kWideOrders) + " buys crossing 1-by-1");

    ops.init(kMaxOrders);

    uint64_t next_id = 0;
    Timer t;
    for (int i = 0; i < kWideOrders; ++i) {
        uint64_t price = kMaxPrices - 1 - (i % 10);
        ops.submit_order(next_id++, price, 1, false);
    }
    double insert_ms = t.elapsed_ms();
    std::cout << "  -- insert phase\n";
    lobook::bench::print_runtime(insert_ms, kWideOrders);

    PerfCounters perf;
    perf.start();
    t.reset();
    for (int i = 0; i < kWideOrders; ++i)
        ops.submit_order(next_id++, kMaxPrices - 1, 1, true);
    double match_ms = t.elapsed_ms();
    perf.stop();
    std::cout << "  -- match phase\n";
    lobook::bench::print_runtime(match_ms, kWideOrders);
    perf.print_summary(kWideOrders);
}

// ---------------------------------------------------------------------------
//  Scenario 3 - Mixed churn
// ---------------------------------------------------------------------------
constexpr int kChurnOps = 100'000;

void scenario_churn(const BookOps& ops) {
    lobook::bench::print_section(
        (std::string("scenario 3 / mixed churn / ") + ops.name).c_str());
    lobook::bench::print_kv("setup",
        std::to_string(kChurnOps)
            + " ops, 70% submit / 20% cancel / 10% aggressive marketable");

    ops.init(kMaxOrders);

    std::mt19937 rng(123);
    std::uniform_int_distribution<uint64_t> price_dist(0, kMaxPrices - 1);

    uint64_t next_id = 0;
    std::vector<uint64_t> live_ids;
    live_ids.reserve(kChurnOps);

    PerfCounters perf;
    perf.start();
    Timer t;
    for (int i = 0; i < kChurnOps; ++i) {
        uint64_t price = price_dist(rng);
        bool is_buy = (rng() & 1);
        int action = rng() % 10;
        if (action < 7) {
            ops.submit_order(next_id, price, 1 + (rng() % 10), is_buy);
            live_ids.push_back(next_id++);
        } else if (action < 9 && !live_ids.empty()) {
            size_t idx = rng() % live_ids.size();
            ops.cancel_order(live_ids[idx]);
            live_ids[idx] = live_ids.back();
            live_ids.pop_back();
        } else {
            uint64_t aggr_price = is_buy ? (kMaxPrices - 1) : 0;
            ops.submit_order(next_id++, aggr_price, 1, is_buy);
        }
    }
    double elapsed = t.elapsed_ms();
    perf.stop();
    lobook::bench::print_runtime(elapsed, kChurnOps);
    perf.print_summary(kChurnOps);
}

} // namespace

int main() {
    lobook::bench::print_environment_banner("FastBook vs BitsetBook");
    lobook::bench::print_kv("max prices", std::to_string(kMaxPrices));
    lobook::bench::print_kv("max orders", std::to_string(kMaxOrders));

    scenario_sparse(fast_ops);
    scenario_sparse(bitset_ops);
    scenario_wide(fast_ops);
    scenario_wide(bitset_ops);
    scenario_churn(fast_ops);
    scenario_churn(bitset_ops);

    lobook::bench::print_done();
    return 0;
}
