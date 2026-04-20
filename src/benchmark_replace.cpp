#include "engine/fast_bitset_book.h"
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>
#include <x86intrin.h>

// ---- Linux perf_event support ----
#ifdef __linux__
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PERF_AVAILABLE 1

static long perf_event_open(
    struct perf_event_attr* hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

class PerfCounter {
    int fd_ = -1;
    bool available_ = false;

public:
    PerfCounter(uint32_t type, uint64_t config) {
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
    ~PerfCounter() {
        if (fd_ != -1)
            close(fd_);
    }
    bool is_available() const { return available_; }
    void start() {
        if (fd_ != -1) {
            ioctl(fd_, PERF_EVENT_IOC_RESET, 0);
            ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
        }
    }
    void stop() {
        if (fd_ != -1)
            ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
    }
    uint64_t read_value() {
        if (fd_ == -1)
            return 0;
        uint64_t count = 0;
        ::read(fd_, &count, sizeof(count));
        return count;
    }
};

class PerfCounters {
    PerfCounter l1d_read_miss_ { PERF_TYPE_HW_CACHE,
        (PERF_COUNT_HW_CACHE_L1D) | (PERF_COUNT_HW_CACHE_OP_READ << 8)
            | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) };
    PerfCounter cache_refs_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES };
    PerfCounter cache_misses_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES };
    PerfCounter instructions_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS };
    PerfCounter cycles_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES };
    PerfCounter branch_misses_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES };
    PerfCounter branches_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS };

public:
    void start_all() {
        l1d_read_miss_.start();
        cache_refs_.start();
        cache_misses_.start();
        instructions_.start();
        cycles_.start();
        branch_misses_.start();
        branches_.start();
    }
    void stop_all() {
        l1d_read_miss_.stop();
        cache_refs_.stop();
        cache_misses_.stop();
        instructions_.stop();
        cycles_.stop();
        branch_misses_.stop();
        branches_.stop();
    }
    void print_report(uint64_t num_ops = 0) {
        bool any = l1d_read_miss_.is_available() || cache_refs_.is_available()
            || instructions_.is_available();
        if (!any) {
            std::cout << "      (hw counters unavailable)\n";
            return;
        }
        std::cout << std::fixed << std::setprecision(2);
        if (instructions_.is_available() && cycles_.is_available()) {
            uint64_t insn = instructions_.read_value();
            uint64_t cyc = cycles_.read_value();
            double ipc = cyc > 0 ? (double)insn / (double)cyc : 0;
            std::cout << "      Instructions:    " << std::setw(12) << insn;
            if (num_ops > 0)
                std::cout << "  (" << (double)insn / num_ops << " insn/op)";
            std::cout << "\n";
            std::cout << "      Cycles:          " << std::setw(12) << cyc;
            if (num_ops > 0)
                std::cout << "  (" << (double)cyc / num_ops << " cyc/op)";
            std::cout << "\n";
            std::cout << "      IPC:             " << std::setw(12) << ipc << "\n";
        }
        if (branches_.is_available() && branch_misses_.is_available()) {
            uint64_t br = branches_.read_value();
            uint64_t bm = branch_misses_.read_value();
            double br_miss_rate = br > 0 ? 100.0 * (double)bm / br : 0;
            std::cout << "      Branches:        " << std::setw(12) << br;
            if (num_ops > 0)
                std::cout << "  (" << (double)br / num_ops << " br/op)";
            std::cout << "\n";
            std::cout << "      Branch Misses:   " << std::setw(12) << bm << "  (" << br_miss_rate
                      << "%)\n";
        }
        if (l1d_read_miss_.is_available()) {
            uint64_t l1dm = l1d_read_miss_.read_value();
            std::cout << "      L1D Read Misses: " << std::setw(12) << l1dm;
            if (num_ops > 0)
                std::cout << "  (" << (double)l1dm / num_ops << " miss/op)";
            std::cout << "\n";
        }
        if (cache_refs_.is_available()) {
            uint64_t refs = cache_refs_.read_value();
            uint64_t misses = cache_misses_.read_value();
            double miss_rate = refs > 0 ? 100.0 * (double)misses / refs : 0;
            std::cout << "      Cache Refs:      " << std::setw(12) << refs << "\n";
            std::cout << "      Cache Misses:    " << std::setw(12) << misses << "  (" << miss_rate
                      << "%)\n";
        }
    }
};
#else
#define PERF_AVAILABLE 0
class PerfCounters {
public:
    void start_all() { }
    void stop_all() { }
    void print_report(uint64_t /*num_ops*/ = 0) { std::cout << "      (hw counters unavailable)\n"; }
};
#endif

// ---- Timer ----
class Timer {
    std::chrono::high_resolution_clock::time_point start_;

public:
    Timer() { reset(); }
    void reset() { start_ = std::chrono::high_resolution_clock::now(); }
    double elapsed_ms() {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end - start_).count();
    }
};

static void print_result(const char* label, double time_ms, int ops) {
    double ops_per_sec = (ops / time_ms) * 1000.0;
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "    " << std::left << std::setw(22) << label << std::right << std::setw(10)
              << time_ms << " ms"
              << "    (" << std::setprecision(0) << std::setw(12) << ops_per_sec << " ops/sec)\n";
    std::cout << std::setprecision(3);
}

static constexpr size_t MAX_ORDERS = 100000;
static constexpr uint64_t MAX_PRICES = Fast::MAX_PRICES;

struct Operation {
    uint64_t id;
    uint64_t new_price;
    uint32_t new_qty;
    bool is_buy;
};

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║       REPLACE BENCHMARK: Native vs Cancel + Submit           ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ MAX_PRICES = " << std::setw(6) << MAX_PRICES
              << "   MAX_ORDERS = " << std::setw(6) << MAX_ORDERS << "                  ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

#if PERF_AVAILABLE
    std::cout << "[Info] Using Linux perf_event_open for hardware counters\n";
#else
    std::cout << "[Info] Hardware counters not available\n";
#endif

    Fast::FastBitsetOrderBook book;
    std::mt19937 rng(123);
    std::uniform_int_distribution<uint64_t> bid_dist(100, MAX_PRICES / 2 - 1);
    std::uniform_int_distribution<uint64_t> ask_dist(MAX_PRICES / 2, MAX_PRICES - 100);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 100);

    constexpr int NUM_LIVE_ORDERS = 10000;
    constexpr int NUM_OPERATIONS = 1000000;

    std::vector<Operation> ops;
    ops.reserve(NUM_OPERATIONS);

    // Pre-generate operations uniformly modifying price up/down, keeping qty the same,
    // and scaling qty down (where native should be strictly O(1)).
    for (int i = 0; i < NUM_OPERATIONS; i++) {
        uint64_t target_id = rng() % NUM_LIVE_ORDERS;
        bool is_buy = (target_id % 2) == 0;
        uint64_t price = is_buy ? bid_dist(rng) : ask_dist(rng);
        uint32_t qty = qty_dist(rng);
        ops.push_back({ target_id, price, qty, is_buy });
    }

    auto reset_book = [&]() {
        book.init(MAX_ORDERS);
        for (int i = 0; i < NUM_LIVE_ORDERS; i++) {
            bool is_buy = (i % 2) == 0;
            uint64_t price = is_buy ? bid_dist(rng) : ask_dist(rng);
            book.submit_order(i, price, qty_dist(rng), is_buy);
        }
    };

    // --- Scenario A: Native Replace ---
    std::cout << "\n  --- Native Replace ---\n";
    reset_book();

    PerfCounters perf1;
    perf1.start_all();
    Timer t1;

    for (const auto& op : ops) {
        book.replace_order(op.id, op.new_price, op.new_qty);
    }

    double elapsed1 = t1.elapsed_ms();
    perf1.stop_all();

    print_result("Total Time", elapsed1, NUM_OPERATIONS);
    perf1.print_report(NUM_OPERATIONS);

    // --- Scenario B: Cancel + Submit ---
    std::cout << "\n  --- Cancel + Submit ---\n";
    rng.seed(123);
    reset_book();

    uint64_t cancel_cycles_total = 0;
    uint64_t submit_cycles_total = 0;

    PerfCounters perf2;
    perf2.start_all();
    Timer t2;

    for (const auto& op : ops) {
        // Technically this simulates knowing whether the existing order is a buy or sell.
        // We will assume buy=true for simplicity, since FastBook logic for inserts is symmetrical.
        uint64_t t0 = __rdtsc();
        book.cancel_order(op.id);
        uint64_t t1 = __rdtsc();
        book.submit_order(op.id, op.new_price, op.new_qty, op.is_buy);
        uint64_t t_end = __rdtsc();
        cancel_cycles_total += t1 - t0;
        submit_cycles_total += t_end - t1;
    }

    double elapsed2 = t2.elapsed_ms();
    perf2.stop_all();

    double tsc_hz = 2.6e9; // attu Xeon Gold 6132 nominal (look up exact via /proc/cpuinfo)
    double cancel_ns = (cancel_cycles_total / (double)NUM_OPERATIONS) / tsc_hz * 1e9;
    double submit_ns = (submit_cycles_total / (double)NUM_OPERATIONS) / tsc_hz * 1e9;
    std::cout << "    cancel avg: " << cancel_ns << " ns/op\n";
    std::cout << "    submit avg: " << submit_ns << " ns/op\n";

    print_result("Total Time", elapsed2, NUM_OPERATIONS);
    perf2.print_report(NUM_OPERATIONS);

    std::cout << "\n══════════════════════════════════════════════════════════════\n";
    std::cout << "Done.\n";
    return 0;
}
