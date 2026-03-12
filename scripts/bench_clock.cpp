#include <cstdio>
#include <cstdint>
#include <ctime>
#include <x86intrin.h>

int main() {
    timespec ts;
    int N = 1000000;

    uint64_t start = __rdtsc();
    for (int i = 0; i < N; i++) {
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    }
    uint64_t end = __rdtsc();

    printf("clock_gettime: %lu cycles/call\n", (end - start) / N);
    return 0;
}
