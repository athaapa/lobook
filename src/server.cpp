// End-to-end latency harness.
//
// One producer thread pushes timestamped OrderMessages into a queue at a fixed
// pacing interval; the matching engine pops them on a pinned consumer core and
// records per-message end-to-end latency. Reports p50/p99/p999 for both the
// queue (push -> pop) and the full pipeline (push -> match completion).
//
// Usage:
//   server [--queue=cached|uncached|naive] [--pacing-ns=N] [--workload=N]
//
// Default: --queue=cached --pacing-ns=1000 --workload=100000
//
// The three queue modes:
//   cached    SPSCQueue<N, true>   (Rigtorp's cached-index optimization)
//   uncached  SPSCQueue<N, false>  (lock-free, but pays MESI round-trip per op)
//   naive     mutex + condition_variable

#include "bench_common.h"
#include "engine/cpu_pinning.h"
#include "engine/matching_engine.h"
#include "engine/naive_queue.h"
#include "engine/spsc_queue.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

    constexpr size_t kQueueCapacity = 131072;
    constexpr size_t kBookOrders = 100'000;

    struct Args {
        std::string queue_mode = "cached";
        uint64_t pacing_ns = 1000;
        uint64_t workload = 100'000;
    };

    void print_usage(const char* prog) {
        std::cerr << "Usage: " << prog
                  << " [--queue=cached|uncached|naive] [--pacing-ns=N] [--workload=N]\n";
    }

    bool parse_args(int argc, char* argv[], Args& out) {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--help" || a == "-h") {
                print_usage(argv[0]);
                std::exit(0);
            } else if (a.rfind("--queue=", 0) == 0) {
                out.queue_mode = a.substr(8);
            } else if (a.rfind("--pacing-ns=", 0) == 0) {
                out.pacing_ns = std::stoull(a.substr(12));
            } else if (a.rfind("--workload=", 0) == 0) {
                out.workload = std::stoull(a.substr(11));
            } else {
                std::cerr << "unknown argument: " << a << "\n";
                print_usage(argv[0]);
                return false;
            }
        }
        if (out.queue_mode != "cached" && out.queue_mode != "uncached"
            && out.queue_mode != "naive") {
            std::cerr << "invalid --queue value: " << out.queue_mode << "\n";
            print_usage(argv[0]);
            return false;
        }
        return true;
    }

    template <typename QueueT> void run(const Args& args) {
        QueueT queue;
        MatchingEngine<QueueT> engine(queue);
        engine.start(kBookOrders);

        // Q: Why pre-build the entire workload into a vector before the network
        //    thread starts, rather than constructing each OrderMessage inline
        //    inside the loop?
        // A: Construction overhead would be charged against every per-op latency
        //    sample. We want to measure queue + engine, not OrderMessage init.
        std::vector<OrderMessage> workload;
        workload.reserve(args.workload);
        for (uint64_t i = 0; i < args.workload; ++i) {
            workload.push_back({ Type::SUBMIT, i, 100, 10, true, 0 });
        }

        std::thread network_thread([&]() {
            pin_to_core(network_bench_core());
            engine.wait_until_ready();
            for (auto msg : workload) {
                timespec ts;
                clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
                // Q: Why stamp the message immediately before pushing, rather than
                //    upfront during construction?
                // A: We want to measure queue + engine, not setup. Stamping at
                //    push time excludes wait_until_ready() and workload init.
                msg.timestamp = (uint64_t)ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
                queue.push(msg);

                // Q: Why busy-spin for pacing rather than nanosleep?
                // A: nanosleep wake-up latency dwarfs the queue cost we're trying
                //    to measure. Spinning is the only way to get sub-microsecond
                //    pacing accuracy on commodity Linux.
                uint64_t wait_until = msg.timestamp + args.pacing_ns;
                while (true) {
                    timespec ts2;
                    clock_gettime(CLOCK_MONOTONIC_RAW, &ts2);
                    if ((uint64_t)ts2.tv_sec * 1'000'000'000ULL + ts2.tv_nsec >= wait_until)
                        break;
                }
            }
        });

        network_thread.join();
        engine.stop();
        engine.report();
    }

} // anonymous namespace

int main(int argc, char* argv[]) {
    Args args;
    if (!parse_args(argc, argv, args))
        return 1;

    lobook::bench::print_environment_banner("end-to-end latency harness");
    lobook::bench::print_kv("queue", args.queue_mode);
    lobook::bench::print_kv("pacing", std::to_string(args.pacing_ns) + " ns");
    lobook::bench::print_kv("workload", std::to_string(args.workload) + " ops");
    lobook::bench::print_kv("network core", std::to_string(network_bench_core()));
    lobook::bench::print_kv("matching core", std::to_string(matching_bench_core()));
    std::cout << "\n";

    if (args.queue_mode == "cached") {
        run<SPSCQueue<kQueueCapacity, true>>(args);
    } else if (args.queue_mode == "uncached") {
        run<SPSCQueue<kQueueCapacity, false>>(args);
    } else {
        run<NaiveQueue>(args);
    }
    return 0;
}
