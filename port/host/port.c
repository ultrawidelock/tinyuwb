/** @file port.c — host port: a monotonic clock for the range store's age. */

#include "tinyuwb/port.h"

#include <time.h>

int64_t tu_uptime_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
