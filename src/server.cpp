#include "engine/cpu_pinning.h"
#include "engine/matching_engine.h"
#include "engine/naive_queue.h"
#include "engine/spsc_queue.h"
#include <ctime>
#include <thread>
#include <vector>

using Queue = SPSCQueue<131072>;
// using Queue = NaiveQueue;

int main()
{
    Queue queue;
    MatchingEngine<Queue> engine(queue);
    engine.start(100'000);

    std::vector<OrderMessage> workload;
    workload.reserve(100'000);

    for (int i = 0; i < 100'000; i++) {
        workload.push_back({ Type::SUBMIT, (uint64_t)i, 100, 10, true });
    }

    std::thread network_thread = std::thread([&queue, &workload, &engine]() {
        pin_to_core(2);
        engine.wait_until_ready();
        for (auto msg : workload) {
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
            msg.timestamp = ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
            queue.push(msg);

            uint64_t wait_until = msg.timestamp + 1000; // 1μs after stamp
            while (true) {
                timespec ts2;
                clock_gettime(CLOCK_MONOTONIC_RAW, &ts2);
                if (ts2.tv_sec * 1'000'000'000ULL + ts2.tv_nsec >= wait_until)
                    break;
            }
        }
    });

    network_thread.join();
    engine.stop();
    engine.report();
    return 0;
}
