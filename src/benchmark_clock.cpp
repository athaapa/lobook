// Measures the per-call overhead of clock_gettime(CLOCK_MONOTONIC_RAW).
//
// This is the floor of any wall-clock latency measurement we make. On hosts
// where clock_gettime is vDSO-accelerated (most modern Linux x86) it's ~20 ns;
// on virtualized hosts where it falls back to a syscall it's ~500 ns. Both
// are useful to know before drawing conclusions from end-to-end latency.

#include "bench_common.h"
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <vector>

namespace {

constexpr int kSamples = 1'000'000;

inline uint64_t to_ns(const timespec& ts) {
    return (uint64_t)ts.tv_sec * 1'000'000'000ULL + (uint64_t)ts.tv_nsec;
}

} // namespace

int main() {
    lobook::bench::print_environment_banner("clock_gettime overhead");
    lobook::bench::print_kv("clock", "CLOCK_MONOTONIC_RAW");
    lobook::bench::print_kv("samples", std::to_string(kSamples));

    std::vector<uint64_t> deltas;
    deltas.reserve(kSamples);

    // Warmup: page in vDSO, prime caches.
    for (int i = 0; i < 1000; ++i) {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    }

    for (int i = 0; i < kSamples; ++i) {
        timespec a, b;
        clock_gettime(CLOCK_MONOTONIC_RAW, &a);
        clock_gettime(CLOCK_MONOTONIC_RAW, &b);
        deltas.push_back(to_ns(b) - to_ns(a));
    }

    std::sort(deltas.begin(), deltas.end());
    auto pct = [&](double p) { return deltas[(size_t)((kSamples - 1) * p)]; };

    lobook::bench::print_section("results");
    std::cout << "  p50          " << pct(0.50) << " ns\n";
    std::cout << "  p90          " << pct(0.90) << " ns\n";
    std::cout << "  p99          " << pct(0.99) << " ns\n";
    std::cout << "  p999         " << pct(0.999) << " ns\n";
    std::cout << "  max          " << deltas.back() << " ns\n";

    lobook::bench::print_done();
    return 0;
}
