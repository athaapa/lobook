#include "engine/matching_engine.h"
#include "engine/naive_queue.h"
#include <ctime>
#include <thread>
#include <vector>

int main()
{
    NaiveQueue queue;
    MatchingEngine engine(queue);
    engine.start(100'000);

    std::vector<OrderMessage> workload;
    workload.reserve(100'000);

    for (int i = 0; i < 100'000; i++) {
        workload.push_back({ Type::SUBMIT, (uint64_t)i, 100, 10, true });
    }

    std::thread network_thread = std::thread([&queue, &workload]() {
        for (auto msg : workload) {
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
            msg.timestamp = ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
            queue.push(msg);
        }
    });

    network_thread.join();
    engine.stop();
    engine.report();
    return 0;
}
