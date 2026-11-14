/*
 * The Mac I/O cell's first page: a Heathrow-style interrupt controller at
 * +0x10 (two banks of sources, big-endian registers, source n at bit
 * 0x80000000 >> (n % 32), as the ROM and NanoKernel use them) plus
 * miscellaneous registers we just remember. Sources are level-triggered
 * lines, or pulses (DMA channels), which the events register latches until
 * the clear register clears them.
 */
#ifndef SYSHM_MACIO_H
#define SYSHM_MACIO_H

#include <stdbool.h>
#include <stdint.h>

#include "mmio.h"

#define MACIO_PAGE_SIZE 0x1000u
#define MACIO_INTCTL_OFFSET 0x10u

struct macio {
	uint32_t mask[2];       /* [0]: sources 0-31, [1]: sources 32-63 */
	uint32_t levels[2];     /* current interrupt line levels */
	uint32_t events[2];     /* latched pulses */
	uint32_t taken[2];      /* pulses the kernel acknowledged, still reported (see macio_take()) */
	uint32_t regs[MACIO_PAGE_SIZE / 4];
};

void macio_init(struct macio *mio);
void macio_pulse(struct macio *mio, unsigned source);

static inline void macio_set_line(struct macio *mio, unsigned source, bool asserted)
{
	uint32_t bit = 0x80000000u >> (source & 31);
	int bank = source >= 32;
	if (asserted)
		mio->levels[bank] |= bit;
	else
		mio->levels[bank] &= ~bit;
}

static inline uint32_t macio_pending(const struct macio *mio, int bank)
{
	return mio->mask[bank] & (mio->levels[bank] | mio->events[bank] | mio->taken[bank]);
}

/*
 * The kernel's external interrupt handler acknowledges the latched pulses it
 * reports, as on hardware: they leave the events register but stay pending
 * until macio_release(), when Mac OS asks for interrupts to be re-evaluated
 * after handling them.
 */
void macio_take(struct macio *mio);
void macio_release(struct macio *mio);

extern const struct mmio_ops macio_mmio_ops;

#endif
