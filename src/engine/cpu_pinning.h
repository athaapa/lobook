#pragma once
#include <cstdio>
#include <pthread.h>
#include <sched.h>

void pin_to_core(int core_id)
{
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core_id, &cpu_set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);
    if (rc != 0) {
        fprintf(stderr, "Failed to pin to core %d: error %d\n", core_id, rc);
    }
}
