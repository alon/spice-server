#ifndef UTILS_H_
# define UTILS_H_

#include <time.h>

/* FIXME: consider g_get_monotonic_time (), but in microseconds */
static inline red_time_t red_get_monotonic_time(void)
{
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec * (1000 * 1000 * 1000) + time.tv_nsec;
}

#endif /* UTILS_H_ */
