#include "perf.h"

#if defined(__linux__)

#include <linux/perf_event.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int open_counter(uint64_t config)
{
	struct perf_event_attr a;
	memset(&a, 0, sizeof a);
	a.type = PERF_TYPE_HARDWARE;
	a.size = sizeof a;
	a.config = config;
	a.exclude_kernel = 1;
	a.exclude_hv = 1;
	return (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
}

bool perf_open(struct perf *p)
{
	p->cycles_fd = open_counter(PERF_COUNT_HW_CPU_CYCLES);
	p->insns_fd = open_counter(PERF_COUNT_HW_INSTRUCTIONS);
	if (p->cycles_fd < 0 || p->insns_fd < 0) {
		perf_close(p);
		return false;
	}
	return true;
}

bool perf_read(const struct perf *p, uint64_t *cycles, uint64_t *insns)
{
	return p->cycles_fd >= 0 && read(p->cycles_fd, cycles, sizeof *cycles) == sizeof *cycles &&
	       read(p->insns_fd, insns, sizeof *insns) == sizeof *insns;
}

void perf_close(struct perf *p)
{
	if (p->cycles_fd >= 0)
		close(p->cycles_fd);
	if (p->insns_fd >= 0)
		close(p->insns_fd);
	p->cycles_fd = p->insns_fd = -1;
}

#else

bool perf_open(struct perf *p)
{
	p->cycles_fd = p->insns_fd = -1;
	return false;
}

bool perf_read(const struct perf *p, uint64_t *cycles, uint64_t *insns)
{
	(void)p;
	*cycles = *insns = 0;
	return false;
}

void perf_close(struct perf *p)
{
	(void)p;
}

#endif
