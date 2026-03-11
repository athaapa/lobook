#pragma once
#include <cstdio>
#include <pthread.h>
#include <sched.h>

// Q: Why pin the matching engine thread to a specific core at all, rather than
//    letting the OS scheduler place it wherever it sees fit?
// A: 

// Q: Why use pthread_setaffinity_np instead of the POSIX sched_setaffinity
//    syscall directly?
// A:
void pin_to_core(int core_id)
{
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core_id, &cpu_set);
    // Q: Why is the return value of pthread_setaffinity_np not checked here?
    // A:
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);
}
