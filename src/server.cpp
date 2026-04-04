#include "engine/cpu_pinning.h"
#include "engine/matching_engine.h"
#include "engine/naive_queue.h"
#include "engine/spsc_queue.h"
#include <ctime>
#include <thread>
#include <vector>

// Q: Why is SPSCQueue commented out in favor of NaiveQueue here, and when
//    would you swap them?
// A: It's purely for benchmarking purposes.
using Queue = SPSCQueue<131072>;
// using Queue = NaiveQueue;

int main(int argc, char* argv[])
{
    uint64_t pacing_ns = (argc > 1) ? std::stoull(argv[1]) : 1000;
    Queue queue;
    MatchingEngine<Queue> engine(queue);
    engine.start(100'000);

    // Q: Why pre-build the entire workload into a vector before the network
    //    thread starts, rather than constructing each OrderMessage inline
    //    inside the loop?
    // A: I don't want to measure the overhead of constructing the entire OrderMessage structs.
    std::vector<OrderMessage> workload;
    workload.reserve(100'000);

    for (int i = 0; i < 100'000; i++) {
        workload.push_back({ Type::SUBMIT, (uint64_t)i, 100, 10, true });
    }

    // Q: Why does the network thread call engine.wait_until_ready() before
    //    pushing messages, rather than just starting to push immediately?
    // A: The issue with pushing immediately is that the engine needs time to initialize before it
    // is ready to accept orders. What was happening before that was that engine was getting orders
    // as it was initializing, which caused the first few orders to have a huge latency (on the
    // order of miliseconds). My aim is to measure queue latency, not queue delay. Therefore, to
    // address this, I wait for the engine to finish initializing before beginning the benchmark.
    std::thread network_thread = std::thread([&queue, &workload, &engine, pacing_ns]() {
        pin_to_core(0);
        engine.wait_until_ready();
        for (auto msg : workload) {
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
            // Q: Why stamp the message immediately before pushing it rather
            //    than stamping the whole workload upfront during construction?
            // A: I'm interested in measuring the latency of the queue + matching system. If I
            // timestamped it at construction, I would also be measuring additional overhead like
            // the construction of the other OrderMessages as well as the latency of
            // MatchingEngine::wait_until_ready.
            msg.timestamp = ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
            queue.push(msg);

            // Q: Why busy-spin for 1μs between sends rather than sleeping with
            //    nanosleep or usleep?
            // A: I don't want to incur the wakeup cost of after sleeping a thread.
            uint64_t wait_until = msg.timestamp + pacing_ns;
            while (true) {
                timespec ts2;
                clock_gettime(CLOCK_MONOTONIC_RAW, &ts2);
                if (ts2.tv_sec * 1'000'000'000ULL + ts2.tv_nsec >= wait_until)
                    break;
            }
        }
    });

    // Q: Why join the network_thread before calling engine.stop(), rather than
    //    stopping the engine first?
    // A:
    network_thread.join();
    engine.stop();
    engine.report();
    return 0;
}
