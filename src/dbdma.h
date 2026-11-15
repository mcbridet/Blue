/*
 * Apple's descriptor-based DMA (DBDMA), as in Heathrow-family Mac I/O: 16
 * channels of 0x100 bytes of little-endian registers at Mac I/O + 0x8000,
 * each running a list of 16-byte command descriptors from memory (CHRP I/O
 * Device Reference, chapter 15).
 *
 * A channel moves data between memory and its device's streams. Transfers
 * take no time of their own: a channel runs as far as it can whenever its
 * registers are written, its device has data or room again (dbdma_kick()),
 * or its device status bits change, so everything happens at deterministic
 * points of the guest's execution. Interrupts are pulses (the controller
 * latches them); a channel that ran out of its allowance of commands per
 * run (a list looping on itself) continues at the next dbdma_poll().
 *
 * Addresses are physical: data and descriptors must be in RAM; quadlet
 * loads and stores (KEY_SYSTEM) may also reach device registers.
 */
#ifndef BLUE_DBDMA_H
#define BLUE_DBDMA_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"
#include "mmio.h"

#define DBDMA_CHANNELS 16u
#define DBDMA_CHANNEL_SIZE 0x100u
#define DBDMA_SIZE (DBDMA_CHANNELS * DBDMA_CHANNEL_SIZE)

/* Channel registers */
enum {
	DBDMA_CONTROL = 0x00,
	DBDMA_STATUS = 0x04,
	DBDMA_CMDPTR_HI = 0x08,
	DBDMA_CMDPTR = 0x0C,
	DBDMA_INTR_SEL = 0x10,
	DBDMA_BRANCH_SEL = 0x14,
	DBDMA_WAIT_SEL = 0x18,
};

/* ChannelStatus bits (written through ChannelControl: mask << 16 | value) */
enum {
	DBDMA_RUN = 0x8000,
	DBDMA_PAUSE = 0x4000,
	DBDMA_FLUSH = 0x2000,
	DBDMA_WAKE = 0x1000,
	DBDMA_DEAD = 0x0800,
	DBDMA_ACTIVE = 0x0400,
	DBDMA_BT = 0x0100,              /* the last command branched */
	DBDMA_S = 0x00FF,               /* device status bits s7-s0 */
};

/* Command descriptor: command << 16 | reqCount, address, cmdDep, xferStatus << 16 | resCount */
enum {
	DBDMA_OUTPUT_MORE, DBDMA_OUTPUT_LAST, DBDMA_INPUT_MORE, DBDMA_INPUT_LAST,
	DBDMA_STORE_QUAD, DBDMA_LOAD_QUAD, DBDMA_NOP, DBDMA_STOP,
};
enum { DBDMA_KEY_STREAM0, DBDMA_KEY_STREAM1, DBDMA_KEY_STREAM2, DBDMA_KEY_STREAM3,
       DBDMA_KEY_REGS = 5, DBDMA_KEY_SYSTEM, DBDMA_KEY_DEVICE };
enum { DBDMA_NEVER, DBDMA_IF_SET, DBDMA_IF_CLEAR, DBDMA_ALWAYS };      /* the i, b and w fields */

/* What a channel's streams connect to; either function may be NULL (nothing ever flows) */
struct dbdma_device {
	/*
	 * OUTPUT: takes up to `len` bytes of stream `key` and returns how many
	 * it took. Taking fewer stalls the channel until dbdma_kick(). `last`
	 * (OUTPUT_LAST): the record ends with these bytes, once all are taken.
	 */
	uint32_t (*output)(void *ctx, unsigned key, const uint8_t *data, uint32_t len, bool last);
	/*
	 * INPUT: puts up to `len` bytes of stream `key` at `data` and returns
	 * how many; setting *end ends the command there (the record is
	 * complete). Nothing at all stalls the channel until dbdma_kick().
	 */
	uint32_t (*input)(void *ctx, unsigned key, uint8_t *data, uint32_t len, bool *end);
	void *ctx;
};

struct dbdma_channel {
	uint16_t status;
	uint32_t cmdptr;
	uint32_t regs[DBDMA_CHANNEL_SIZE / 4];  /* the select registers and the rest, as written */
	struct dbdma_device dev;
	unsigned irq;                           /* interrupt source */

	bool idle;                              /* at a STOP, until a wake */

	/* the command in progress */
	bool fetched;
	uint32_t cmd[4];
	uint32_t done;                          /* bytes transferred */
	bool ended;                             /* ... and the transfer is over (completion may wait) */

	bool running, again;                    /* (re-entry from device callbacks) */
	unsigned long commands, interrupts;
};

struct dbdma {
	struct dbdma_channel ch[DBDMA_CHANNELS];
	struct mem *mem;
	uint32_t ram_size;                      /* DMA reaches physical memory [0, ram_size) */
	const struct mmio_bus *bus;             /* and device registers, for quadlets (optional) */
	void (*interrupt)(void *ctx, unsigned source);  /* a pulse */
	void *interrupt_ctx;
	uint32_t poll;                          /* channels to continue in dbdma_poll() */
};

/* Channel n interrupts on source n (Heathrow numbering) until changed. */
void dbdma_init(struct dbdma *d, struct mem *mem, uint32_t ram_size);
void dbdma_attach(struct dbdma *d, unsigned channel, const struct dbdma_device *dev);

/* The device has data or room again: the channel continues a stalled transfer. */
void dbdma_kick(struct dbdma *d, unsigned channel);

/* The device sets status bits s7-s0 (those in `mask`), which may end a wait. */
void dbdma_set_status(struct dbdma *d, unsigned channel, uint8_t mask, uint8_t value);

/* Continues channels that stopped only for their allowance of commands; false if there were none. */
bool dbdma_poll(struct dbdma *d);

extern const struct mmio_ops dbdma_mmio_ops;

#endif
