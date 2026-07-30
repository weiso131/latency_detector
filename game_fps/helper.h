// Small utilities shared by the layer and the tracepoints.
#ifndef HELPER_H
#define HELPER_H

#include <stdint.h>
#include <time.h>

// CLOCK_MONOTONIC, so timestamps are unaffected by wall-clock adjustments.
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif // HELPER_H
