#include <algorithm>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <vector>

uint64_t get_ns(struct timespec& ts)
{
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main()
{
    const int N = 1'000'000;
    std::vector<uint64_t> results;
    results.reserve(N);

    // Warmup
    for (int i = 0; i < 1000; ++i) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    }

    for (int i = 0; i < N; ++i) {
        struct timespec ts1, ts2;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts1);
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts2);

        uint64_t start = get_ns(ts1);
        uint64_t end = get_ns(ts2);
        results.push_back(end - start);
    }

    std::sort(results.begin(), results.end());
    std::cout << "clock_gettime (CLOCK_MONOTONIC_RAW) overhead:\n";
    std::cout << "p50: " << results[N * 0.50] << " ns\n";
    std::cout << "p99: " << results[N * 0.99] << " ns\n";
    std::cout << "p999: " << results[N * 0.999] << " ns\n";

    return 0;
}
