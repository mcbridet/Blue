/*
 * The host processor's cycle and instruction counters for the calling
 * thread (Linux perf events; nothing elsewhere), for measuring how well
 * the emulation runs (SYSHM_PERF).
 */
#ifndef SYSHM_PERF_H
#define SYSHM_PERF_H

#include <stdbool.h>
#include <stdint.h>

struct perf {
	int cycles_fd, insns_fd;
};

/* Starts counting; false if the counters aren't available */
bool perf_open(struct perf *p);

/* Counts since perf_open() */
bool perf_read(const struct perf *p, uint64_t *cycles, uint64_t *insns);

void perf_close(struct perf *p);

#endif
