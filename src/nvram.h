/*
 * The NVRAM chip of Heathrow/Paddington-class Macs (the iMac's), behind Mac
 * I/O at +0x60000: one byte per 16 bytes of address space (byte k at +16k;
 * the low four address bits don't matter), so its 8 KB take 128 KB. Mac OS
 * 9.1's ROM file drives it with its own driver ("Cuda NVRAM Plugin", from
 * the parcel for an nvram node under mac-io), which finds it by the node's
 * AAPL,address. Its bytes are RTAS's (rtas.h): one NVRAM, and one file,
 * whichever way Mac OS reaches it.
 */
#ifndef SYSHM_NVRAM_H
#define SYSHM_NVRAM_H

#include <stdint.h>

#include "mmio.h"

#define NVRAM_STRIDE 16u

struct nvram {
	uint8_t *bytes;
	uint32_t size;                  /* a power of two */
	unsigned long reads, writes;
};

void nvram_init(struct nvram *n, uint8_t *bytes, uint32_t size);
extern const struct mmio_ops nvram_mmio_ops;

#endif
