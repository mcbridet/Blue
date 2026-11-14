/*
 * RTAS (Run-Time Abstraction Services): the clock, NVRAM, power and PCI
 * configuration services Mac OS reaches through NanoKernel call 6. This is part of the HAL: each
 * service is answered from the host.
 */
#ifndef SYSHM_RTAS_H
#define SYSHM_RTAS_H

#include <stdbool.h>
#include <stdint.h>

#include "clock.h"
#include "mem.h"
#include "pci.h"

#define RTAS_NVRAM_SIZE 8192u

struct rtas {
	uint8_t nvram[RTAS_NVRAM_SIZE];
	const struct guest_clock *clock;
	struct pci_bus *pci;            /* for read/write-pci-config (none: no devices) */
	bool power_off;
	bool reboot;
	unsigned long calls[16];
};

void rtas_init(struct rtas *r);

/* NVRAM persistence: load returns 1 if the file didn't exist (NVRAM left blank). */
int rtas_nvram_load(struct rtas *r, const char *path);
int rtas_nvram_save(const struct rtas *r, const char *path);

/*
 * Services the argument block at guest address `block`:
 *   +0 token, +4 index of a pointer argument (Apple), +6 nargs, +8 nret,
 *   +12 args[nargs], then rets[nret] (rets[0] is the status).
 * Returns false if the block itself is unusable.
 */
bool rtas_call(struct rtas *r, struct mem *mem, uint32_t block);

const char *rtas_name(uint32_t token);

#endif
