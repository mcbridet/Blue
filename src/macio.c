#include "macio.h"

#include <string.h>

/* Register offsets within a bank; bank for sources 0-31 sits 0x10 above the other. */
enum { REG_EVENTS = 0x0, REG_MASK = 0x4, REG_CLEAR = 0x8, REG_LEVELS = 0xC };

void macio_init(struct macio *mio)
{
	memset(mio, 0, sizeof *mio);
}

void macio_set_line(struct macio *mio, unsigned source, bool asserted)
{
	uint32_t bit = 0x80000000u >> (source & 31);
	int bank = source >= 32;
	if (asserted)
		mio->levels[bank] |= bit;
	else
		mio->levels[bank] &= ~bit;
}

void macio_pulse(struct macio *mio, unsigned source)
{
	mio->events[source >= 32] |= 0x80000000u >> (source & 31);
}

uint32_t macio_pending(const struct macio *mio, int bank)
{
	return mio->mask[bank] & (mio->levels[bank] | mio->events[bank] | mio->taken[bank]);
}

void macio_take(struct macio *mio)
{
	for (int b = 0; b < 2; b++) {
		uint32_t t = mio->events[b] & mio->mask[b];
		mio->taken[b] |= t;
		mio->events[b] &= ~t;
	}
}

void macio_release(struct macio *mio)
{
	mio->taken[0] = mio->taken[1] = 0;
}

/* Reads `size` bytes at `offset` out of a big-endian 32-bit register value. */
static uint32_t extract(uint32_t word, uint32_t offset, unsigned size)
{
	unsigned shift = 8 * (4 - size - (offset & 3));
	return size == 4 ? word : (word >> shift) & ((1u << (8 * size)) - 1);
}

static uint32_t insert(uint32_t word, uint32_t offset, unsigned size, uint32_t value)
{
	if (size == 4)
		return value;
	unsigned shift = 8 * (4 - size - (offset & 3));
	uint32_t m = ((1u << (8 * size)) - 1) << shift;
	return (word & ~m) | ((value << shift) & m);
}

static bool intctl_reg(uint32_t offset, int *bank, unsigned *reg)
{
	if (offset < MACIO_INTCTL_OFFSET || offset >= MACIO_INTCTL_OFFSET + 0x20)
		return false;
	uint32_t o = offset - MACIO_INTCTL_OFFSET;
	*bank = o >= 0x10 ? 0 : 1;
	*reg = o & 0xC;
	return true;
}

static uint32_t macio_read(void *ctx, uint32_t offset, unsigned size)
{
	struct macio *mio = ctx;
	int bank;
	unsigned reg;

	if (intctl_reg(offset, &bank, &reg)) {
		uint32_t w = reg == REG_MASK ? mio->mask[bank] : reg == REG_CLEAR ? 0 :
			     reg == REG_EVENTS ? mio->levels[bank] | mio->events[bank] : mio->levels[bank];
		return extract(w, offset, size);
	}
	return extract(mio->regs[(offset & (MACIO_PAGE_SIZE - 1)) / 4], offset, size);
}

static void macio_write(void *ctx, uint32_t offset, unsigned size, uint32_t value)
{
	struct macio *mio = ctx;
	int bank;
	unsigned reg;

	if (intctl_reg(offset, &bank, &reg)) {
		if (reg == REG_MASK)
			mio->mask[bank] = insert(mio->mask[bank], offset, size, value);
		else if (reg == REG_CLEAR)      /* (level-triggered sources stay as their lines are) */
			mio->events[bank] &= ~insert(0, offset, size, value);
		return;
	}
	uint32_t *w = &mio->regs[(offset & (MACIO_PAGE_SIZE - 1)) / 4];
	*w = insert(*w, offset, size, value);
}

const struct mmio_ops macio_mmio_ops = { macio_read, macio_write };
