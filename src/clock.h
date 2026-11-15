/*
 * The wall-clock time the guest's real-time clock reports. Normally the
 * host's; with a fixed start it counts from that start in guest time, so
 * runs are reproducible.
 */
#ifndef BLUE_CLOCK_H
#define BLUE_CLOCK_H

#include <stdint.h>
#include <time.h>

struct guest_clock {
	int64_t fixed_start;            /* Unix time at boot, or 0 for the host clock */
	const uint64_t *tb;             /* guest timebase */
	uint32_t tb_hz;
};

/* Current time (Unix seconds and nanoseconds); `c` may be NULL for the host clock. */
time_t guest_clock_now(const struct guest_clock *c, long *nsec);

#endif
