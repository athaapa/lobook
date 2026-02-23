#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

// Forward-declared runner interfaces (compiled in separate TUs)
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

// Use constants directly (100'000)
static constexpr size_t MAX_PRICES = 100'000;

// ---- Linux perf_event support ----
#ifdef __linux__
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PERF_AVAILABLE 1

static long perf_event_open(struct perf_event_attr* hw_event, pid_t pid,
    int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

class PerfCounter {
    int fd_ = -1;
    bool available_ = false;

public:
    PerfCounter(uint32_t type, uint64_t config)
    {
        struct perf_event_attr pe;
        memset(&pe, 0, sizeof(pe));
        pe.type = type;
        pe.size = sizeof(pe);
        pe.config = config;
        pe.disabled = 1;
        pe.exclude_kernel = 1;
        pe.exclude_hv = 1;
        fd_ = perf_event_open(&pe, 0, -1, -1, 0);
        available_ = (fd_ != -1);
    }
    ~PerfCounter()
    {
        if (fd_ != -1)
            close(fd_);
    }
    bool is_available() const { return available_; }
    void start()
    {
        if (fd_ != -1) {
            ioctl(fd_, PERF_EVENT_IOC_RESET, 0);
            ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
        }
    }
    void stop()
    {
        if (fd_ != -1)
            ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
    }
    uint64_t read_value()
    {
        if (fd_ == -1)
            return 0;
        uint64_t count = 0;
        ::read(fd_, &count, sizeof(count));
        return count;
    }
};

class PerfCounters {
    PerfCounter l1d_read_miss_ { PERF_TYPE_HW_CACHE,
        (PERF_COUNT_HW_CACHE_L1D) | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) };
    PerfCounter cache_refs_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES };
    PerfCounter cache_misses_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES };

public:
    void start_all()
    {
        l1d_read_miss_.start();
        cache_refs_.start();
        cache_misses_.start();
    }
    void stop_all()
    {
        l1d_read_miss_.stop();
        cache_refs_.stop();
        cache_misses_.stop();
    }
    void print_report()
    {
        bool any = l1d_read_miss_.is_available() || cache_refs_.is_available();
        if (!any) {
            std::cout << "      (hw counters unavailable)\n";
            return;
        }
        std::cout << std::fixed << std::setprecision(2);
        if (l1d_read_miss_.is_available())
            std::cout << "      L1D Read Misses: " << std::setw(12) << l1d_read_miss_.read_value() << "\n";
        if (cache_refs_.is_available()) {
            uint64_t refs = cache_refs_.read_value();
            uint64_t misses = cache_misses_.read_value();
            double miss_rate = refs > 0 ? 100.0 * (double)misses / refs : 0;
            std::cout << "      Cache Refs:      " << std::setw(12) << refs << "\n";
            std::cout << "      Cache Misses:    " << std::setw(12) << misses
                      << "  (" << miss_rate << "%)\n";
        }
    }
};
#else
#define PERF_AVAILABLE 0
class PerfCounters {
public:
    void start_all() { }
    void stop_all() { }
    void print_report() { std::cout << "      (hw counters unavailable)\n"; }
};
#endif

// ---- Timer ----
class Timer {
    std::chrono::high_resolution_clock::time_point start_;

public:
    Timer() { reset(); }
    void reset() { start_ = std::chrono::high_resolution_clock::now(); }
    double elapsed_ms()
    {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end - start_).count();
    }
};

// ---- Formatting ----
static void print_header(const char* title)
{
    std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  " << std::left << std::setw(60) << title << "║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
}

static void print_result(const char* label, double time_ms, int ops)
{
    double ops_per_sec = (ops / time_ms) * 1000.0;
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "    " << std::left << std::setw(22) << label
              << std::right << std::setw(10) << time_ms << " ms"
              << "    (" << std::setprecision(0) << std::setw(12) << ops_per_sec << " ops/sec)\n";
    std::cout << std::setprecision(3);
}

// ---- Generic book interface via function pointers ----
struct BookOps {
    void (*init)(size_t);
    void (*submit_order)(uint64_t, uint64_t, uint32_t, bool);
    void (*cancel_order)(uint64_t);
    uint32_t (*get_quantity)(uint64_t);
};

static BookOps fast_book_ops {
    FastBookRunner::init,
    FastBookRunner::submit_order,
    FastBookRunner::cancel_order,
    FastBookRunner::get_quantity
};

static BookOps bitset_book_ops {
    BitsetBookRunner::init,
    BitsetBookRunner::submit_order,
    BitsetBookRunner::cancel_order,
    BitsetBookRunner::get_quantity
};

// =========================================================================
//  Scenario 1: Sparse Insert & Match
// =========================================================================
static constexpr int N_LEVELS = 50;
static constexpr int ORDERS_PER_LEVEL = 20;
static constexpr int MATCH_ROUNDS = 200;

void scenario1(BookOps& ops, size_t max_orders, const char* name)
{
    std::cout << "\n  --- " << name << " ---\n";
    ops.init(max_orders);

    std::vector<uint64_t> prices;
    for (int i = 0; i < N_LEVELS; ++i) {
        prices.push_back((uint64_t)i * (MAX_PRICES / N_LEVELS) + 1);
    }

    uint64_t next_id = 0;

    Timer t;
    for (int round = 0; round < MATCH_ROUNDS; ++round) {
        for (int i = 0; i < N_LEVELS; ++i) {
            for (int j = 0; j < ORDERS_PER_LEVEL; ++j) {
                ops.submit_order(next_id++, prices[i], 1, false); // ask
            }
        }
    }
    double insert_time = t.elapsed_ms();
    int total_inserted = MATCH_ROUNDS * N_LEVELS * ORDERS_PER_LEVEL;
    print_result("Insert", insert_time, total_inserted);

    PerfCounters perf;
    perf.start_all();
    t.reset();
    ops.submit_order(next_id++, MAX_PRICES - 1, total_inserted, true);
    double match_time = t.elapsed_ms();
    perf.stop_all();

    print_result("Match sweep", match_time, total_inserted);
    perf.print_report();
}

// =========================================================================
//  Scenario 2: Wide Spread Matching
// =========================================================================
static constexpr int WIDE_ORDERS = 10000;

void scenario2(BookOps& ops, size_t max_orders, const char* name)
{
    std::cout << "\n  --- " << name << " ---\n";
    ops.init(max_orders);

    uint64_t next_id = 0;

    Timer t;
    for (int i = 0; i < WIDE_ORDERS; ++i) {
        uint64_t price = MAX_PRICES - 1 - (i % 10);
        ops.submit_order(next_id++, price, 1, false); // ask
    }
    double insert_time = t.elapsed_ms();
    print_result("Insert asks (high)", insert_time, WIDE_ORDERS);

    PerfCounters perf;
    perf.start_all();
    t.reset();
    for (int i = 0; i < WIDE_ORDERS; ++i) {
        ops.submit_order(next_id++, MAX_PRICES - 1, 1, true);
    }
    double match_time = t.elapsed_ms();
    perf.stop_all();

    print_result("Match 1-by-1", match_time, WIDE_ORDERS);
    perf.print_report();
}

// =========================================================================
//  Scenario 3: Incremental Sparse Churn
// =========================================================================
static constexpr int CHURN_OPS = 100000;

void scenario3(BookOps& ops, size_t max_orders, const char* name)
{
    std::cout << "\n  --- " << name << " ---\n";
    ops.init(max_orders);

    std::mt19937 rng(123);
    std::uniform_int_distribution<uint64_t> price_dist(0, MAX_PRICES - 1);

    uint64_t next_id = 0;
    std::vector<uint64_t> live_ids;
    live_ids.reserve(CHURN_OPS);

    PerfCounters perf;
    perf.start_all();
    Timer t;

    for (int i = 0; i < CHURN_OPS; ++i) {
        uint64_t price = price_dist(rng);
        bool is_buy = (rng() & 1);

        int action = rng() % 10;
        if (action < 7) {
            ops.submit_order(next_id, price, 1 + (rng() % 10), is_buy);
            live_ids.push_back(next_id);
            next_id++;
        } else if (action < 9 && !live_ids.empty()) {
            size_t idx = rng() % live_ids.size();
            ops.cancel_order(live_ids[idx]);
            live_ids[idx] = live_ids.back();
            live_ids.pop_back();
        } else {
            uint64_t aggr_price = is_buy ? (MAX_PRICES - 1) : 0;
            ops.submit_order(next_id, aggr_price, 1, is_buy);
            next_id++;
        }
    }

    double elapsed = t.elapsed_ms();
    perf.stop_all();

    print_result("Mixed churn", elapsed, CHURN_OPS);
    perf.print_report();
}

// =========================================================================
//  Main
// =========================================================================
int main()
{
    constexpr int MAX_ORDERS = 500000;

    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║       SPARSE WORKLOAD BENCHMARK: FastBook vs BitsetBook      ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ MAX_PRICES = " << std::setw(6) << MAX_PRICES
              << "   MAX_ORDERS = " << std::setw(6) << MAX_ORDERS << "                  ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

#if PERF_AVAILABLE
    std::cout << "[Info] Using Linux perf_event_open for hardware counters\n";
#else
    std::cout << "[Info] Hardware counters not available\n";
#endif

    // Scenario 1
    print_header("Scenario 1: Sparse Insert & Match (50 levels x 200 rounds)");
    scenario1(fast_book_ops, MAX_ORDERS, "FastOrderBook (linear scan)");
    scenario1(bitset_book_ops, MAX_ORDERS, "FastBitsetOrderBook (bitset)");

    // Scenario 2
    print_header("Scenario 2: Wide Spread Matching (asks at top, buys sweep)");
    scenario2(fast_book_ops, MAX_ORDERS, "FastOrderBook (linear scan)");
    scenario2(bitset_book_ops, MAX_ORDERS, "FastBitsetOrderBook (bitset)");

    // Scenario 3
    print_header("Scenario 3: Incremental Sparse Churn (100K mixed ops)");
    scenario3(fast_book_ops, MAX_ORDERS, "FastOrderBook (linear scan)");
    scenario3(bitset_book_ops, MAX_ORDERS, "FastBitsetOrderBook (bitset)");

    std::cout << "\n══════════════════════════════════════════════════════════════\n";
    std::cout << "Done.\n";
    return 0;
}
