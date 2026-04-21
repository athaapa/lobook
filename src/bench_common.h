#pragma once

// Shared utilities for all `benchmark_*` binaries:
//   - Timer        : monotonic wall-clock timing
//   - PerfCounters : Linux perf_event_open hardware counters
//   - TSC helpers  : calibrate invariant TSC, convert cycles -> nanoseconds
//   - Output       : standardized environment banner + section headers
//
// Linux x86-64 only.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <linux/perf_event.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <x86intrin.h>

namespace lobook::bench {

    // ---------------------------------------------------------------------------
    //  Timer
    // ---------------------------------------------------------------------------

    class Timer {
        std::chrono::steady_clock::time_point start_;

    public:
        Timer() { reset(); }
        void reset() { start_ = std::chrono::steady_clock::now(); }
        double elapsed_ms() const {
            auto end = std::chrono::steady_clock::now();
            return std::chrono::duration<double, std::milli>(end - start_).count();
        }
        double elapsed_ns() const {
            auto end = std::chrono::steady_clock::now();
            return std::chrono::duration<double, std::nano>(end - start_).count();
        }
    };

    // ---------------------------------------------------------------------------
    //  TSC calibration
    // ---------------------------------------------------------------------------

    // Calibrate invariant TSC frequency by busy-spinning against CLOCK_MONOTONIC_RAW
    // for `calibration_ns` (default 50 ms). Returns Hz.
    inline double calibrate_tsc_hz(uint64_t calibration_ns = 50'000'000ULL) {
        timespec ts1 {}, ts2 {};
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts1);
        uint64_t tsc1 = __rdtsc();

        uint64_t start_ns = (uint64_t)ts1.tv_sec * 1'000'000'000ULL + ts1.tv_nsec;
        uint64_t target = start_ns + calibration_ns;
        while (true) {
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts2);
            uint64_t now = (uint64_t)ts2.tv_sec * 1'000'000'000ULL + ts2.tv_nsec;
            if (now >= target)
                break;
        }
        uint64_t tsc2 = __rdtsc();
        uint64_t actual_ns = (uint64_t)(ts2.tv_sec - ts1.tv_sec) * 1'000'000'000ULL
            + (uint64_t)(ts2.tv_nsec - ts1.tv_nsec);
        return (double)(tsc2 - tsc1) * 1e9 / (double)actual_ns;
    }

    // ---------------------------------------------------------------------------
    //  Hardware counters (Linux perf_event_open)
    // ---------------------------------------------------------------------------

    inline long perf_event_open(
        struct perf_event_attr* attr, pid_t pid, int cpu, int group_fd, unsigned long flags) {
        return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
    }

    class PerfCounter {
        int fd_ = -1;

    public:
        PerfCounter(uint32_t type, uint64_t config) {
            struct perf_event_attr pe;
            std::memset(&pe, 0, sizeof(pe));
            pe.type = type;
            pe.size = sizeof(pe);
            pe.config = config;
            pe.disabled = 1;
            pe.exclude_kernel = 1;
            pe.exclude_hv = 1;
            fd_ = perf_event_open(&pe, 0, -1, -1, 0);
        }
        ~PerfCounter() {
            if (fd_ != -1)
                close(fd_);
        }
        PerfCounter(const PerfCounter&) = delete;
        PerfCounter& operator=(const PerfCounter&) = delete;

        bool available() const { return fd_ != -1; }
        void start() {
            if (fd_ == -1)
                return;
            ioctl(fd_, PERF_EVENT_IOC_RESET, 0);
            ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
        }
        void stop() {
            if (fd_ != -1)
                ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
        }
        uint64_t read_value() {
            if (fd_ == -1)
                return 0;
            uint64_t v = 0;
            ::read(fd_, &v, sizeof(v));
            return v;
        }
    };

    class PerfCounters {
        PerfCounter instructions_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS };
        PerfCounter cycles_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES };
        PerfCounter branches_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS };
        PerfCounter branch_misses_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES };
        PerfCounter l1d_read_misses_ { PERF_TYPE_HW_CACHE,
            PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8)
                | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16) };
        PerfCounter llc_refs_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES };
        PerfCounter llc_misses_ { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES };

    public:
        void start() {
            instructions_.start();
            cycles_.start();
            branches_.start();
            branch_misses_.start();
            l1d_read_misses_.start();
            llc_refs_.start();
            llc_misses_.start();
        }
        void stop() {
            instructions_.stop();
            cycles_.stop();
            branches_.stop();
            branch_misses_.stop();
            l1d_read_misses_.stop();
            llc_refs_.stop();
            llc_misses_.stop();
        }

        void print_summary(uint64_t num_ops) {
            if (!instructions_.available()) {
                std::cout << "      (perf counters unavailable; check "
                             "kernel.perf_event_paranoid)\n";
                return;
            }
            const uint64_t insn = instructions_.read_value();
            const uint64_t cyc = cycles_.read_value();
            const uint64_t br = branches_.read_value();
            const uint64_t bm = branch_misses_.read_value();
            const uint64_t l1m = l1d_read_misses_.read_value();
            const uint64_t lref = llc_refs_.read_value();
            const uint64_t lmiss = llc_misses_.read_value();
            const double ipc = cyc > 0 ? (double)insn / (double)cyc : 0.0;
            const double br_miss_pct = br > 0 ? 100.0 * (double)bm / (double)br : 0.0;
            const double llc_miss_pct = lref > 0 ? 100.0 * (double)lmiss / (double)lref : 0.0;
            const double n = num_ops > 0 ? (double)num_ops : 1.0;

            auto& os = std::cout;
            os << std::fixed;
            os << "      instructions  " << std::setw(8) << std::setprecision(2) << (insn / n)
               << " /op   cycles  " << std::setw(7) << (cyc / n) << " /op   IPC  "
               << std::setprecision(2) << ipc << "\n";
            os << "      branches      " << std::setw(8) << (br / n) << " /op   misses  "
               << std::setprecision(2) << br_miss_pct << " %\n";
            os << "      L1D misses    " << std::setw(8) << std::setprecision(2) << (l1m / n)
               << " /op\n";
            os << "      LLC refs      " << std::setw(8) << (lref / n) << " /op   miss rate  "
               << std::setprecision(2) << llc_miss_pct << " %\n";
        }
    };

    // ---------------------------------------------------------------------------
    //  Environment banner
    // ---------------------------------------------------------------------------

    inline std::string read_first_line(const char* path) {
        std::ifstream f(path);
        std::string line;
        if (f && std::getline(f, line))
            return line;
        return "";
    }

    inline std::string read_proc_cpuinfo_field(const char* field) {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (f && std::getline(f, line)) {
            if (line.rfind(field, 0) == 0) {
                auto pos = line.find(':');
                if (pos != std::string::npos) {
                    std::string v = line.substr(pos + 1);
                    auto first = v.find_first_not_of(" \t");
                    if (first != std::string::npos)
                        return v.substr(first);
                    return v;
                }
            }
        }
        return "";
    }

    inline std::string read_isolated_cores() {
        std::string s = read_first_line("/sys/devices/system/cpu/isolated");
        return s.empty() ? std::string("(none)") : s;
    }

    inline std::string compiler_string() {
#if defined(__clang__)
        return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
        return std::string("gcc ") + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__)
            + "." + std::to_string(__GNUC_PATCHLEVEL__);
#else
        return "unknown";
#endif
    }

    inline void print_environment_banner(const char* benchmark_name) {
        std::cout << "===============================================================\n";
        std::cout << "  lobook benchmark: " << benchmark_name << "\n";
        std::cout << "===============================================================\n";
        std::cout << "  host         " << read_first_line("/proc/sys/kernel/hostname") << "\n";
        std::string model = read_proc_cpuinfo_field("model name");
        if (model.empty())
            model = "(unknown)";
        std::cout << "  cpu          " << model << "\n";
        double tsc_hz = calibrate_tsc_hz();
        std::cout << "  tsc freq     " << std::fixed << std::setprecision(4) << (tsc_hz / 1e9)
                  << " GHz (calibrated)\n";
        std::cout << "  isolcpus     " << read_isolated_cores() << "\n";
        std::cout << "  kernel       " << read_first_line("/proc/sys/kernel/osrelease") << "\n";
        std::cout << "  build        " << compiler_string() << "  C++" << __cplusplus / 100 % 100
                  << "\n";
        std::cout << "===============================================================\n";
    }

    inline void print_section(const char* title) {
        std::cout << "\n---------------------------------------------------------------\n";
        std::cout << "  " << title << "\n";
        std::cout << "---------------------------------------------------------------\n";
    }

    inline void print_kv(const char* key, const std::string& value) {
        std::cout << "  " << std::left << std::setw(13) << key << value << "\n";
    }

    inline void print_runtime(double elapsed_ms, uint64_t num_ops) {
        double ns_per_op = (elapsed_ms * 1e6) / (double)num_ops;
        double ops_per_sec = (double)num_ops / (elapsed_ms / 1000.0);
        std::cout << std::fixed;
        std::cout << "  runtime      " << std::setprecision(3) << elapsed_ms << " ms  ("
                  << std::setprecision(2) << (ops_per_sec / 1e6) << " M ops/s, "
                  << std::setprecision(2) << ns_per_op << " ns/op)\n";
    }

    inline void print_done() {
        std::cout << "\n===============================================================\n";
        std::cout << "  done.\n";
        std::cout << "===============================================================\n";
    }

} // namespace lobook::bench
