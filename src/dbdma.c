#include "dbdma.h"

#include <string.h>

#include "be.h"

#define MAX_COMMANDS 256u               /* completed per run of a channel; then dbdma_poll() */

enum { W_COMMAND, W_ADDRESS, W_DEP, W_STATUS };         /* descriptor words */

static const uint16_t control_bits = DBDMA_RUN | DBDMA_PAUSE | DBDMA_FLUSH | DBDMA_WAKE | DBDMA_S;

void dbdma_init(struct dbdma *d, struct mem *mem, uint32_t ram_size)
{
	memset(d, 0, sizeof *d);
	d->mem = mem;
	d->ram_size = ram_size;
	for (unsigned n = 0; n < DBDMA_CHANNELS; n++)
		d->ch[n].irq = n;
}

void dbdma_attach(struct dbdma *d, unsigned channel, const struct dbdma_device *dev)
{
	d->ch[channel % DBDMA_CHANNELS].dev = *dev;
}

static bool in_ram(const struct dbdma *d, uint32_t addr, uint32_t len)
{
	return addr < d->ram_size && len <= d->ram_size - addr;
}

static unsigned cmd_of(const struct dbdma_channel *c) { return c->cmd[W_COMMAND] >> 28; }
static unsigned key_of(const struct dbdma_channel *c) { return (c->cmd[W_COMMAND] >> 24) & 7; }
static uint32_t req_of(const struct dbdma_channel *c) { return c->cmd[W_COMMAND] & 0xFFFF; }

/* The i, b or w field (`shift` 20, 18, 16) against its select register */
static bool condition(const struct dbdma_channel *c, unsigned shift, unsigned select)
{
	uint32_t sel = c->regs[select / 4];
	unsigned mask = (sel >> 16) & 0xFF, value = sel & 0xFF;
	bool cond = (c->status & mask) == (value & mask);
	switch ((c->cmd[W_COMMAND] >> shift) & 3) {
	case DBDMA_IF_SET:   return cond;
	case DBDMA_IF_CLEAR: return !cond;
	case DBDMA_ALWAYS:   return true;
	default:             return false;
	}
}

static bool interrupt_wanted(const struct dbdma_channel *c) { return condition(c, 20, DBDMA_INTR_SEL); }
static bool branch_wanted(const struct dbdma_channel *c) { return condition(c, 18, DBDMA_BRANCH_SEL); }
static bool wait_wanted(const struct dbdma_channel *c) { return condition(c, 16, DBDMA_WAIT_SEL); }

static void interrupt(struct dbdma *d, struct dbdma_channel *c)
{
	c->interrupts++;
	if (d->interrupt)
		d->interrupt(d->interrupt_ctx, c->irq);
}

/* xferStatus (always with active set, so software can tell it was written) and resCount, in one store */
static void write_status(struct dbdma *d, struct dbdma_channel *c, uint32_t res)
{
	uint32_t at = c->cmdptr + 12;
	if (!in_ram(d, at, 4))
		return;
	mem_invalidate(d->mem, at, 4);
	put_le32(mem_ptr(d->mem, at), (uint32_t)(c->status | DBDMA_ACTIVE) << 16 | (res & 0xFFFF));
}

static uint32_t residual(const struct dbdma_channel *c)
{
	return cmd_of(c) <= DBDMA_INPUT_LAST ? req_of(c) - c->done : 0;
}

/* A bus or command error: the channel stops until software clears run */
static void die(struct dbdma *d, struct dbdma_channel *c)
{
	c->status = (uint16_t)((c->status | DBDMA_DEAD) & ~DBDMA_ACTIVE);
	if (c->fetched)
		write_status(d, c, residual(c));
	c->fetched = false;
	interrupt(d, c);
}

/*
 * The end of a command: the wait (false while it holds), then the branch
 * decision, status, interrupt and the next command.
 */
static bool complete(struct dbdma *d, struct dbdma_channel *c)
{
	if (wait_wanted(c))
		return false;
	unsigned cmd = cmd_of(c);
	bool quad = cmd == DBDMA_STORE_QUAD || cmd == DBDMA_LOAD_QUAD;      /* (b is reserved) */
	bool branch = !quad && branch_wanted(c);
	if (!quad)
		c->status = (uint16_t)(branch ? c->status | DBDMA_BT : c->status & ~DBDMA_BT);
	write_status(d, c, residual(c));
	if (interrupt_wanted(c))
		interrupt(d, c);
	c->cmdptr = branch ? c->cmd[W_DEP] & ~15u : c->cmdptr + 16;
	c->fetched = false;
	c->commands++;
	return true;
}

static uint32_t dbdma_read(void *ctx, uint32_t offset, unsigned size);
static void dbdma_write(void *ctx, uint32_t offset, unsigned size, uint32_t value);

/* Between a little-endian quadlet value and the big-endian view of mmio_ops, for `size` bytes */
static uint32_t swap_bytes(uint32_t v, unsigned size)
{
	return size == 4 ? __builtin_bswap32(v) : size == 2 ? __builtin_bswap16((uint16_t)v) : v & 0xFF;
}

/* STORE_QUAD / LOAD_QUAD of `size` bytes at `addr` (aligned); false on an error */
static bool quad(struct dbdma *d, unsigned n, bool store, unsigned key, uint32_t addr, unsigned size, uint32_t *v)
{
	const struct mmio_region *r;

	if (key == DBDMA_KEY_REGS) {
		uint32_t off = n * DBDMA_CHANNEL_SIZE + (addr & (DBDMA_CHANNEL_SIZE - 1));
		if (store)
			dbdma_write(d, off, size, swap_bytes(*v, size));
		else
			*v = swap_bytes(dbdma_read(d, off, size), size);
		return true;
	}
	if (key != DBDMA_KEY_SYSTEM)
		return false;           /* device spaces aren't implemented; stream keys are illegal here */
	if (in_ram(d, addr, size)) {
		uint8_t *p = mem_ptr(d->mem, addr);
		if (store) {
			mem_invalidate(d->mem, addr, size);
			for (unsigned k = 0; k < size; k++)
				p[k] = (uint8_t)(*v >> (8 * k));
		} else {
			*v = 0;
			for (unsigned k = 0; k < size; k++)
				*v |= (uint32_t)p[k] << (8 * k);
		}
		return true;
	}
	if (!d->bus || !(r = mmio_find(d->bus, addr)))
		return false;
	if (store)
		r->ops->write(r->ctx, addr - r->base, size, swap_bytes(*v, size));
	else
		*v = swap_bytes(r->ops->read(r->ctx, addr - r->base, size), size);
	return true;
}

/* Runs channel n's current command as far as it goes; false when the channel can't go on */
static bool step(struct dbdma *d, unsigned n)
{
	struct dbdma_channel *c = &d->ch[n];
	if ((c->status & (DBDMA_RUN | DBDMA_PAUSE | DBDMA_DEAD | DBDMA_ACTIVE)) != (DBDMA_RUN | DBDMA_ACTIVE))
		return false;
	if (!c->fetched) {
		if (!in_ram(d, c->cmdptr, 16)) {
			die(d, c);
			return false;
		}
		const uint8_t *p = mem_ptr(d->mem, c->cmdptr);
		for (unsigned k = 0; k < 4; k++)
			c->cmd[k] = le32(p + 4 * k);
		c->fetched = true;
		c->done = 0;
		c->ended = false;
		c->status &= (uint16_t)~DBDMA_WAKE;
	}

	unsigned cmd = cmd_of(c), key = key_of(c);
	uint32_t req = req_of(c), addr = c->cmd[W_ADDRESS];
	switch (cmd) {
	case DBDMA_OUTPUT_MORE: case DBDMA_OUTPUT_LAST: case DBDMA_INPUT_MORE: case DBDMA_INPUT_LAST: {
		if (key > DBDMA_KEY_STREAM3 || !in_ram(d, addr, req)) {
			die(d, c);      /* (two-address transfers through Data2Ptr aren't implemented) */
			return false;
		}
		bool input = cmd >= DBDMA_INPUT_MORE, last = cmd == DBDMA_OUTPUT_LAST || cmd == DBDMA_INPUT_LAST;
		while (!c->ended) {
			uint32_t left = req - c->done, got = 0;
			uint8_t *p = mem_ptr(d->mem, addr + c->done);
			bool end = false;
			if (input && left && c->dev.input) {
				mem_invalidate(d->mem, addr + c->done, left);
				got = c->dev.input(c->dev.ctx, key, p, left, &end);
			} else if (!input && (left || last) && c->dev.output) {
				got = c->dev.output(c->dev.ctx, key, p, left, last);
			} else if (!left) {
				end = true;
			}
			c->done += got < left ? got : left;
			if (end || c->done == req)
				c->ended = true;
			else if (!got)
				return false;   /* stalled: the device kicks the channel when it can go on */
		}
		return complete(d, c);
	}
	case DBDMA_STORE_QUAD: case DBDMA_LOAD_QUAD:
		if (!c->ended) {
			unsigned size = req & 4 ? 4 : req & 2 ? 2 : 1;
			uint32_t v = c->cmd[W_DEP];
			if (!quad(d, n, cmd == DBDMA_STORE_QUAD, key, addr & ~(size - 1), size, &v)) {
				die(d, c);
				return false;
			}
			if (cmd == DBDMA_LOAD_QUAD) {
				c->cmd[W_DEP] = v;
				mem_invalidate(d->mem, c->cmdptr + 8, 4);
				put_le32(mem_ptr(d->mem, c->cmdptr + 8), v);
			}
			c->ended = true;
		}
		return complete(d, c);
	case DBDMA_NOP:
		c->ended = true;
		return complete(d, c);
	case DBDMA_STOP:
		c->status &= (uint16_t)~DBDMA_ACTIVE;   /* idle at the STOP, until a wake fetches it again */
		c->fetched = false;
		c->idle = true;
		return false;
	default:
		die(d, c);
		return false;
	}
}

static void run(struct dbdma *d, unsigned n)
{
	struct dbdma_channel *c = &d->ch[n];
	if (c->running) {               /* a device callback came back to this channel */
		c->again = true;
		return;
	}
	c->running = true;
	unsigned left = MAX_COMMANDS;
	do {
		c->again = false;
		while (left && step(d, n))
			left--;
	} while (c->again && left);
	c->running = false;
	if (!left)
		d->poll |= 1u << n;
}

void dbdma_kick(struct dbdma *d, unsigned channel)
{
	run(d, channel % DBDMA_CHANNELS);
}

void dbdma_set_status(struct dbdma *d, unsigned channel, uint8_t mask, uint8_t value)
{
	struct dbdma_channel *c = &d->ch[channel % DBDMA_CHANNELS];
	c->status = (uint16_t)((c->status & ~mask) | (value & mask));
	run(d, channel % DBDMA_CHANNELS);
}

bool dbdma_poll(struct dbdma *d)
{
	uint32_t p = d->poll;
	d->poll = 0;
	for (unsigned n = 0; n < DBDMA_CHANNELS; n++)
		if (p & (1u << n))
			run(d, n);
	return p != 0;
}

/* A ChannelControl write: mask << 16 | value */
static void control(struct dbdma *d, unsigned n, uint32_t v)
{
	struct dbdma_channel *c = &d->ch[n];
	uint16_t mask = (uint16_t)(v >> 16) & control_bits, old = c->status;
	c->status = (uint16_t)((old & ~mask) | (v & mask));

	if ((old & DBDMA_RUN) && !(c->status & DBDMA_RUN)) {
		/* abort: the transfer ends here, with status and the interrupt the command asks for */
		if (c->fetched) {
			write_status(d, c, residual(c));
			if (interrupt_wanted(c))
				interrupt(d, c);
		}
		c->fetched = false;
		c->idle = false;
		c->status &= (uint16_t)~(DBDMA_ACTIVE | DBDMA_DEAD | DBDMA_FLUSH | DBDMA_WAKE);
		return;
	}
	if (!(old & DBDMA_RUN) && (c->status & DBDMA_RUN)) {   /* start at CommandPtr */
		c->fetched = false;
		c->idle = false;
	}
	if (c->status & DBDMA_WAKE)             /* (an idle channel fetches its STOP again) */
		c->idle = false;
	if ((c->status & (DBDMA_RUN | DBDMA_PAUSE | DBDMA_DEAD)) == DBDMA_RUN && !c->idle)
		c->status |= DBDMA_ACTIVE;
	else
		c->status &= (uint16_t)~DBDMA_ACTIVE;
	if (!(c->status & DBDMA_RUN))
		c->status &= (uint16_t)~DBDMA_WAKE;

	run(d, n);

	if (c->status & DBDMA_FLUSH) {
		/* input so far goes to memory now: a partial status, flagged by flush */
		if (c->fetched && !c->ended && (cmd_of(c) == DBDMA_INPUT_MORE || cmd_of(c) == DBDMA_INPUT_LAST))
			write_status(d, c, residual(c));
		c->status &= (uint16_t)~DBDMA_FLUSH;
	}
}

static uint32_t reg_value(const struct dbdma_channel *c, unsigned reg)
{
	switch (reg) {
	case DBDMA_CONTROL:
	case DBDMA_CMDPTR_HI: return 0;
	case DBDMA_STATUS:    return c->status;
	case DBDMA_CMDPTR:    return c->cmdptr;
	default:              return c->regs[reg / 4];
	}
}

static void reg_store(struct dbdma *d, unsigned n, unsigned reg, uint32_t v)
{
	struct dbdma_channel *c = &d->ch[n];
	switch (reg) {
	case DBDMA_CONTROL:
		control(d, n, v);
		break;
	case DBDMA_STATUS:
	case DBDMA_CMDPTR_HI:
		break;
	case DBDMA_CMDPTR:
		if (!(c->status & (DBDMA_RUN | DBDMA_ACTIVE)))
			c->cmdptr = v & ~15u;
		break;
	default:
		c->regs[reg / 4] = v;
		break;
	}
}

/*
 * Registers are little-endian (byte k of a register is its bits 8k-8k+7);
 * mmio_ops values are the accessed bytes read big-endian. Drivers use the
 * byte-reversed instructions, so the values come out right.
 */
static uint32_t dbdma_read(void *ctx, uint32_t offset, unsigned size)
{
	struct dbdma *d = ctx;
	unsigned n = (offset / DBDMA_CHANNEL_SIZE) % DBDMA_CHANNELS, reg = offset & 0xFC, b = offset & 3;
	uint32_t r = reg_value(&d->ch[n], reg), v = 0;
	for (unsigned k = 0; k < size && b + k < 4; k++)
		v = v << 8 | ((r >> (8 * (b + k))) & 0xFF);
	return v;
}

static void dbdma_write(void *ctx, uint32_t offset, unsigned size, uint32_t value)
{
	struct dbdma *d = ctx;
	unsigned n = (offset / DBDMA_CHANNEL_SIZE) % DBDMA_CHANNELS, reg = offset & 0xFC, b = offset & 3;
	uint32_t r = reg == DBDMA_CONTROL ? 0 : reg_value(&d->ch[n], reg);  /* (unwritten mask bytes: 0) */
	for (unsigned k = 0; k < size && b + k < 4; k++) {
		unsigned shift = 8 * (b + k);
		r = (r & ~(0xFFu << shift)) | ((value >> (8 * (size - 1 - k))) & 0xFF) << shift;
	}
	reg_store(d, n, reg, r);
}

const struct mmio_ops dbdma_mmio_ops = { dbdma_read, dbdma_write };
