#include "clock.h"

time_t guest_clock_now(const struct guest_clock *c, long *nsec)
{
	if (c && c->fixed_start) {
		uint64_t tb = *c->tb;
		if (nsec)
			*nsec = (long)(tb % c->tb_hz * 1000000000u / c->tb_hz);
		return (time_t)(c->fixed_start + (int64_t)(tb / c->tb_hz));
	}
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	if (nsec)
		*nsec = ts.tv_nsec;
	return ts.tv_sec;
}
