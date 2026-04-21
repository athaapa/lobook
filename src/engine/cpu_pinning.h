#pragma once
#include <cstdlib>
#include <pthread.h>
#include <sched.h>

// Default core layout. Override per host with LOBOOK_NETWORK_CORE /
// LOBOOK_MATCHING_CORE when topology or IRQ routing makes another pair better.
static constexpr int kDefaultNetworkBenchCore = 0;
static constexpr int kDefaultMatchingBenchCore = 2;

inline int configured_bench_core(const char* env_name, int default_core) {
    const char* value = std::getenv(env_name);
    if (value == nullptr || *value == '\0')
        return default_core;

    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0)
        return default_core;

    return static_cast<int>(parsed);
}

inline int network_bench_core() {
    return configured_bench_core("LOBOOK_NETWORK_CORE", kDefaultNetworkBenchCore);
}

inline int matching_bench_core() {
    return configured_bench_core("LOBOOK_MATCHING_CORE", kDefaultMatchingBenchCore);
}

// Q: Why pin the matching engine thread to a specific core at all, rather than
//    letting the OS scheduler place it wherever it sees fit?
// A:

// Q: Why use pthread_setaffinity_np instead of the POSIX sched_setaffinity
//    syscall directly?
// A:
inline void pin_to_core(int core_id) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core_id, &cpu_set);
    // Q: Why is the return value of pthread_setaffinity_np not checked here?
    // A:
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);
}
