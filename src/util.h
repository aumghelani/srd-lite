#ifndef RDT_UTIL_H
#define RDT_UTIL_H

#include <stdint.h>
#include <time.h>

/* monotonic time in microseconds */
static inline uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* xorshift32, good enough for fake packet loss */
static inline uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* psn compare that survives wrapping past 2^32 */
static inline int psn_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

#endif
