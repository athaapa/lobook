#pragma once
#include <cstdio>
#include <pthread.h>
#include <sched.h>

void pin_to_core(int core_id)
{
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core_id, &cpu_set);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);
}
