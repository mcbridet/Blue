/*
 * DBDMA channels against the CHRP I/O Device Reference, chapter 15, with a
 * fake device on the streams. Registers are accessed as drivers do (stwbrx,
 * lwbrx), so the mmio_ops values are byte-swapped here.
 */
#include <string.h>

#include "../src/be.h"
#include "../src/dbdma.h"
#include "../src/macio.h"
#include "../src/mem.h"
#include "../src/ppc.h"
#include "ppcasm.h"
#include "test.h"

#define RAM 0x100000u
#define LIST 0x1000u
#define BUF 0x8000u
#define REGS 0x80008000u                /* where the channel registers sit on the bus */

struct fake {
	uint8_t out[1024];
	uint32_t out_len, out_room;     /* room: what it takes before it's full */
	unsigned records, last_key;
	uint8_t in[1024];
	uint32_t in_len, in_pos, in_end;        /* the record ends after in_end bytes (0: never) */
	bool kick_self;
	struct dbdma *d;
};

static uint32_t fake_output(void *ctx, unsigned key, const uint8_t *data, uint32_t len, bool last)
{
	struct fake *f = ctx;
	uint32_t n = len < f->out_room ? len : f->out_room;
	memcpy(f->out + f->out_len, data, n);
	f->out_len += n;
	f->out_room -= n;
	f->last_key = key;
	if (last && n == len)
		f->records++;
	return n;
}

static uint32_t fake_input(void *ctx, unsigned key, uint8_t *data, uint32_t len, bool *end)
{
	struct fake *f = ctx;
	uint32_t avail = f->in_len - f->in_pos;
	if (f->in_end && f->in_end - f->in_pos < avail)
		avail = f->in_end - f->in_pos;
	uint32_t n = len < avail ? len : avail;
	memcpy(data, f->in + f->in_pos, n);
	f->in_pos += n;
	f->last_key = key;
	if (f->in_end && f->in_pos == f->in_end) {
		*end = true;
		f->in_end = 0;
	}
	return n;
}

static unsigned pulses, last_source;

static void pulse(void *ctx, unsigned source)
{
	(void)ctx;
	pulses++;
	last_source = source;
}

static unsigned code_changes;

static void code_changed(void *ctx, uint32_t addr, uint32_t len)
{
	(void)ctx;
	(void)addr;
	(void)len;
	code_changes++;
}

static struct mem mem;
static struct dbdma dma;
static struct fake dev;

static void wr(unsigned ch, unsigned reg, uint32_t v)
{
	dbdma_mmio_ops.write(&dma, ch * DBDMA_CHANNEL_SIZE + reg, 4, __builtin_bswap32(v));
}

static uint32_t rd(unsigned ch, unsigned reg)
{
	return __builtin_bswap32(dbdma_mmio_ops.read(&dma, ch * DBDMA_CHANNEL_SIZE + reg, 4));
}

static void control(unsigned ch, uint16_t mask, uint16_t value)
{
	wr(ch, DBDMA_CONTROL, (uint32_t)mask << 16 | value);
}

static void put_cmd(unsigned k, unsigned cmd, unsigned key, unsigned i, unsigned b, unsigned w, uint32_t req,
		    uint32_t addr, uint32_t dep)
{
	uint8_t *p = mem_ptr(&mem, LIST + 16 * k);
	put_le32(p, (uint32_t)cmd << 28 | key << 24 | i << 20 | b << 18 | w << 16 | req);
	put_le32(p + 4, addr);
	put_le32(p + 8, dep);
	put_le32(p + 12, 0);
}

static uint32_t xfer(unsigned k)        /* xferStatus << 16 | resCount */
{
	return le32(mem_ptr(&mem, LIST + 16 * k + 12));
}

/* A fresh channel 2 (stopped) on a fresh device */
static void reset(void)
{
	control(2, DBDMA_RUN | DBDMA_PAUSE | DBDMA_FLUSH | DBDMA_WAKE | DBDMA_S, 0);
	dma.ch[2].status = 0;   /* (bt only changes when a command completes) */
	memset(&dev, 0, sizeof dev);
	dev.out_room = ~0u;
	dev.d = &dma;
	memset(mem_ptr(&mem, 0), 0, RAM);
	pulses = 0;
	wr(2, DBDMA_INTR_SEL, 0);
	wr(2, DBDMA_BRANCH_SEL, 0);
	wr(2, DBDMA_WAIT_SEL, 0);
	wr(2, DBDMA_CMDPTR, LIST);
}

static void start(void)
{
	control(2, DBDMA_RUN, DBDMA_RUN);
}

static void test_registers(void)
{
	reset();
	control(2, 0x00FF, 0x0012);
	CHECK_EQ(rd(2, DBDMA_STATUS), 0x0012);
	CHECK_EQ(rd(2, DBDMA_CONTROL), 0);
	control(2, 0x000F, 0x00FF);                     /* only the masked bits change */
	CHECK_EQ(rd(2, DBDMA_STATUS), 0x001F);
	control(2, DBDMA_ACTIVE | DBDMA_DEAD | DBDMA_BT, 0xFFFF);       /* hardware bits: not writable */
	CHECK_EQ(rd(2, DBDMA_STATUS), 0x001F);
	wr(2, DBDMA_STATUS, 0xFFFF);
	CHECK_EQ(rd(2, DBDMA_STATUS), 0x001F);
	/* half a ChannelControl (the data alone, as sthbrx would write it): no mask, no change */
	dbdma_mmio_ops.write(&dma, 2 * DBDMA_CHANNEL_SIZE, 2, __builtin_bswap16(0x00FF));
	CHECK_EQ(rd(2, DBDMA_STATUS), 0x001F);
	wr(2, DBDMA_BRANCH_SEL, 0x00030001);
	CHECK_EQ(rd(2, DBDMA_BRANCH_SEL), 0x00030001);
	CHECK_EQ(__builtin_bswap16((uint16_t)dbdma_mmio_ops.read(&dma, 2 * DBDMA_CHANNEL_SIZE + DBDMA_BRANCH_SEL + 2, 2)),
		 0x0003);
	wr(2, DBDMA_CMDPTR, 0x2340);
	CHECK_EQ(rd(2, DBDMA_CMDPTR), 0x2340);

	/* CommandPtr is only written while the channel is stopped */
	reset();
	put_cmd(0, DBDMA_INPUT_LAST, 0, 0, 0, 0, 16, BUF, 0);   /* (no data yet: it waits) */
	start();
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN | DBDMA_ACTIVE);
	wr(2, DBDMA_CMDPTR, 0x5000);
	CHECK_EQ(rd(2, DBDMA_CMDPTR), LIST);
}

static void test_output(void)
{
	reset();
	for (unsigned k = 0; k < 64; k++)
		mem_ptr(&mem, BUF)[k] = (uint8_t)(k * 7);
	put_cmd(0, DBDMA_OUTPUT_MORE, 1, DBDMA_NEVER, 0, 0, 10, BUF, 0);
	put_cmd(1, DBDMA_OUTPUT_LAST, 1, DBDMA_ALWAYS, 0, 0, 20, BUF + 10, 0);
	put_cmd(2, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	start();
	CHECK_EQ(dev.out_len, 30);
	CHECK(!memcmp(dev.out, mem_ptr(&mem, BUF), 30));
	CHECK_EQ(dev.records, 1);
	CHECK_EQ(dev.last_key, 1);
	CHECK_EQ(xfer(0), (uint32_t)(DBDMA_RUN | DBDMA_ACTIVE) << 16);
	CHECK_EQ(xfer(1), (uint32_t)(DBDMA_RUN | DBDMA_ACTIVE) << 16);
	CHECK_EQ(xfer(2), 0);                           /* STOP writes nothing back */
	CHECK_EQ(pulses, 1);
	CHECK_EQ(last_source, 2);
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN);       /* idle at the STOP, which it points at */
	CHECK_EQ(rd(2, DBDMA_CMDPTR), LIST + 32);

	/* appending: the STOP becomes a command, a new STOP follows, and wake */
	put_cmd(3, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	put_cmd(2, DBDMA_OUTPUT_LAST, 1, 0, 0, 0, 5, BUF + 30, 0);
	CHECK_EQ(dev.out_len, 30);
	control(2, DBDMA_WAKE, DBDMA_WAKE);
	CHECK_EQ(dev.out_len, 35);
	CHECK_EQ(dev.records, 2);
	CHECK_EQ(rd(2, DBDMA_CMDPTR), LIST + 48);
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN);       /* (wake is cleared by the fetch) */

	/* a device that's full stalls the channel until it kicks it */
	reset();
	dev.out_room = 8;
	put_cmd(0, DBDMA_OUTPUT_LAST, 0, DBDMA_ALWAYS, 0, 0, 20, BUF, 0);
	put_cmd(1, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	start();
	CHECK_EQ(dev.out_len, 8);
	CHECK_EQ(xfer(0), 0);
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN | DBDMA_ACTIVE);
	dbdma_kick(&dma, 2);                            /* (still full: nothing changes) */
	CHECK_EQ(dev.out_len, 8);
	dev.out_room = 100;
	dbdma_kick(&dma, 2);
	CHECK_EQ(dev.out_len, 20);
	CHECK_EQ(dev.records, 1);
	CHECK_EQ(xfer(0), (uint32_t)(DBDMA_RUN | DBDMA_ACTIVE) << 16);
	CHECK_EQ(pulses, 1);

	/* OUTPUT_LAST of nothing still ends the record */
	reset();
	put_cmd(0, DBDMA_OUTPUT_LAST, 0, 0, 0, 0, 0, BUF, 0);
	put_cmd(1, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	start();
	CHECK_EQ(dev.records, 1);
	CHECK_EQ(rd(2, DBDMA_CMDPTR), LIST + 16);
}

static void test_input(void)
{
	reset();
	for (unsigned k = 0; k < 200; k++)
		dev.in[k] = (uint8_t)(k ^ 0x5A);
	dev.in_len = 40;
	dev.in_end = 40;
	put_cmd(0, DBDMA_INPUT_LAST, 0, DBDMA_ALWAYS, 0, 0, 100, BUF, 0);
	put_cmd(1, DBDMA_INPUT_MORE, 2, 0, 0, 0, 100, BUF + 100, 0);
	put_cmd(2, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	start();
	CHECK(!memcmp(mem_ptr(&mem, BUF), dev.in, 40));
	CHECK_EQ(xfer(0), (uint32_t)(DBDMA_RUN | DBDMA_ACTIVE) << 16 | 60);    /* 60 bytes short: the record ended */
	CHECK_EQ(pulses, 1);
	CHECK_EQ(xfer(1), 0);                           /* waiting for data */
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN | DBDMA_ACTIVE);
	dev.in_len = 70;                                /* 30 more: not enough */
	dbdma_kick(&dma, 2);
	CHECK_EQ(xfer(1), 0);
	CHECK_EQ(dev.last_key, 2);
	dev.in_len = 200;
	dbdma_kick(&dma, 2);
	CHECK_EQ(xfer(1), (uint32_t)(DBDMA_RUN | DBDMA_ACTIVE) << 16);
	CHECK(!memcmp(mem_ptr(&mem, BUF + 100), dev.in + 40, 100));
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN);
	CHECK_EQ(pulses, 1);

	/* flush: partial status (flush set in it), and the command goes on */
	reset();
	dev.in_len = 30;
	put_cmd(0, DBDMA_INPUT_MORE, 0, 0, 0, 0, 100, BUF, 0);
	put_cmd(1, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	start();
	control(2, DBDMA_FLUSH, DBDMA_FLUSH);
	CHECK_EQ(xfer(0), (uint32_t)(DBDMA_RUN | DBDMA_ACTIVE | DBDMA_FLUSH) << 16 | 70);
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN | DBDMA_ACTIVE);
	dev.in_len = 100;
	dbdma_kick(&dma, 2);
	CHECK_EQ(xfer(0), (uint32_t)(DBDMA_RUN | DBDMA_ACTIVE) << 16);

	/* abort: clearing run ends the transfer with status, and the interrupt it asks for */
	reset();
	dev.in_len = 10;
	put_cmd(0, DBDMA_INPUT_LAST, 0, DBDMA_ALWAYS, 0, 0, 100, BUF, 0);
	start();
	control(2, DBDMA_RUN, 0);
	CHECK_EQ(xfer(0), (uint32_t)DBDMA_ACTIVE << 16 | 90);
	CHECK_EQ(pulses, 1);
	CHECK_EQ(rd(2, DBDMA_STATUS), 0);

	/* input into translated code tells the JIT */
	reset();
	code_changes = 0;
	mem_set_code(&mem, BUF >> MEM_PAGE_SHIFT, true);
	dev.in_len = 4;
	put_cmd(0, DBDMA_INPUT_LAST, 0, 0, 0, 0, 4, BUF, 0);
	put_cmd(1, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	start();
	CHECK(code_changes > 0);
	mem_set_code(&mem, BUF >> MEM_PAGE_SHIFT, false);
}

static void test_conditions(void)
{
	/* branch if s0 is set; BT says so in the status written back */
	reset();
	wr(2, DBDMA_BRANCH_SEL, 0x00010001);
	put_cmd(0, DBDMA_NOP, 0, 0, DBDMA_IF_SET, 0, 0, 0, LIST + 48);
	put_cmd(1, DBDMA_NOP, 0, 0, DBDMA_IF_SET, 0, 0, 0, LIST + 48);
	put_cmd(2, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	put_cmd(3, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	start();
	CHECK_EQ(rd(2, DBDMA_CMDPTR), LIST + 32);       /* s0 clear: no branches */
	CHECK_EQ(xfer(0), (uint32_t)(DBDMA_RUN | DBDMA_ACTIVE) << 16);
	control(2, DBDMA_RUN, 0);
	wr(2, DBDMA_CMDPTR, LIST);
	control(2, DBDMA_RUN | 1, DBDMA_RUN | 1);
	CHECK_EQ(rd(2, DBDMA_CMDPTR), LIST + 48);
	CHECK_EQ(xfer(0), (uint32_t)(DBDMA_RUN | DBDMA_ACTIVE | DBDMA_BT | 1) << 16);
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN | DBDMA_BT | 1);

	/* interrupt if clear; select mask 0x0C, value 0x04 */
	reset();
	wr(2, DBDMA_INTR_SEL, 0x000C0004);
	put_cmd(0, DBDMA_NOP, 0, DBDMA_IF_CLEAR, 0, 0, 0, 0, 0);       /* s = 0: c false, interrupt */
	put_cmd(1, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	start();
	CHECK_EQ(pulses, 1);
	put_cmd(1, DBDMA_NOP, 0, DBDMA_IF_CLEAR, 0, 0, 0, 0, 0);
	put_cmd(2, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	control(2, DBDMA_WAKE | 0x0F, DBDMA_WAKE | 0x05);       /* s = 5: c true, none */
	CHECK_EQ(pulses, 1);
	CHECK_EQ(rd(2, DBDMA_CMDPTR), LIST + 32);

	/* wait while s1 is set, before any of the completion: then the device clears it */
	reset();
	wr(2, DBDMA_WAIT_SEL, 0x00020002);
	put_cmd(0, DBDMA_NOP, 0, DBDMA_ALWAYS, 0, DBDMA_IF_SET, 0, 0, 0);
	put_cmd(1, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	control(2, DBDMA_RUN | 2, DBDMA_RUN | 2);
	CHECK_EQ(xfer(0), 0);
	CHECK_EQ(pulses, 0);
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN | DBDMA_ACTIVE | 2);
	dbdma_set_status(&dma, 2, 2, 0);
	CHECK_EQ(xfer(0), (uint32_t)(DBDMA_RUN | DBDMA_ACTIVE) << 16);
	CHECK_EQ(pulses, 1);
	CHECK_EQ(rd(2, DBDMA_CMDPTR), LIST + 16);

	/* a ring branching back to itself doesn't hang anything: it continues at each poll */
	reset();
	put_cmd(0, DBDMA_NOP, 0, 0, DBDMA_ALWAYS, 0, 0, 0, LIST);
	start();
	unsigned long before = dma.ch[2].commands;
	CHECK(dma.ch[2].commands > 0);
	CHECK(dbdma_poll(&dma));
	CHECK(dma.ch[2].commands > before);
	control(2, DBDMA_RUN, 0);
	dbdma_poll(&dma);
	CHECK(!dbdma_poll(&dma));
}

static void test_quads(void)
{
	reset();
	put_cmd(0, DBDMA_STORE_QUAD, DBDMA_KEY_SYSTEM, 0, 0, 0, 4, BUF + 3, 0x11223344);       /* (aligned down) */
	put_cmd(1, DBDMA_STORE_QUAD, DBDMA_KEY_SYSTEM, 0, 0, 0, 2, BUF + 9, 0xAABBCCDD);
	put_cmd(2, DBDMA_STORE_QUAD, DBDMA_KEY_SYSTEM, 0, 0, 0, 1, BUF + 12, 0x99);
	put_cmd(3, DBDMA_LOAD_QUAD, DBDMA_KEY_SYSTEM, 0, 0, 0, 4, BUF, 0);
	put_cmd(4, DBDMA_STORE_QUAD, DBDMA_KEY_REGS, 0, 0, 0, 4, DBDMA_BRANCH_SEL, 0x00400040);
	put_cmd(5, DBDMA_STORE_QUAD, DBDMA_KEY_SYSTEM, 0, 0, 0, 4, REGS + 2 * DBDMA_CHANNEL_SIZE + DBDMA_WAIT_SEL,
		0x00010000);
	put_cmd(6, DBDMA_LOAD_QUAD, DBDMA_KEY_REGS, 0, 0, 0, 4, DBDMA_STATUS, 0);
	put_cmd(7, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	start();
	const uint8_t want[13] = { 0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0, 0xDD, 0xCC, 0, 0, 0x99 };
	CHECK(!memcmp(mem_ptr(&mem, BUF), want, sizeof want));
	CHECK_EQ(le32(mem_ptr(&mem, LIST + 3 * 16 + 8)), 0x11223344);  /* into the descriptor's data32 */
	CHECK_EQ(rd(2, DBDMA_BRANCH_SEL), 0x00400040);
	CHECK_EQ(rd(2, DBDMA_WAIT_SEL), 0x00010000);    /* through the bus, as a driver's own write */
	CHECK_EQ(le32(mem_ptr(&mem, LIST + 6 * 16 + 8)), DBDMA_RUN | DBDMA_ACTIVE);
	CHECK_EQ(xfer(0), (uint32_t)(DBDMA_RUN | DBDMA_ACTIVE) << 16);
	CHECK_EQ(rd(2, DBDMA_CMDPTR), LIST + 7 * 16);
}

static void test_errors(void)
{
	/* data outside RAM: dead, with status and an interrupt, until run is cleared */
	reset();
	put_cmd(0, DBDMA_OUTPUT_LAST, 0, 0, 0, 0, 16, RAM - 8, 0);
	start();
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN | DBDMA_DEAD);
	CHECK_EQ(xfer(0), (uint32_t)(DBDMA_RUN | DBDMA_DEAD | DBDMA_ACTIVE) << 16 | 16);
	CHECK_EQ(pulses, 1);
	CHECK_EQ(dev.out_len, 0);
	control(2, DBDMA_WAKE, DBDMA_WAKE);             /* stays dead (wake stays set: there's no fetch) */
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN | DBDMA_DEAD | DBDMA_WAKE);
	control(2, DBDMA_RUN, 0);
	CHECK_EQ(rd(2, DBDMA_STATUS), 0);

	/* reserved commands and keys */
	reset();
	put_cmd(0, 9, 0, 0, 0, 0, 0, 0, 0);
	start();
	CHECK(rd(2, DBDMA_STATUS) & DBDMA_DEAD);
	reset();
	put_cmd(0, DBDMA_OUTPUT_LAST, DBDMA_KEY_DEVICE, 0, 0, 0, 4, BUF, 0);
	start();
	CHECK(rd(2, DBDMA_STATUS) & DBDMA_DEAD);
	reset();
	put_cmd(0, DBDMA_STORE_QUAD, DBDMA_KEY_SYSTEM, 0, 0, 0, 4, 0x70000000u, 0);    /* nothing there */
	start();
	CHECK(rd(2, DBDMA_STATUS) & DBDMA_DEAD);

	/* a command list outside RAM */
	reset();
	wr(2, DBDMA_CMDPTR, RAM);
	start();
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN | DBDMA_DEAD);
	control(2, DBDMA_RUN, 0);
}

static void test_pause(void)
{
	reset();
	put_cmd(0, DBDMA_OUTPUT_MORE, 0, 0, 0, 0, 4, BUF, 0);
	put_cmd(1, DBDMA_OUTPUT_LAST, 0, 0, 0, 0, 4, BUF, 0);
	put_cmd(2, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	control(2, DBDMA_RUN | DBDMA_PAUSE, DBDMA_RUN | DBDMA_PAUSE);   /* paused from the start */
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN | DBDMA_PAUSE);
	CHECK_EQ(dev.out_len, 0);
	control(2, DBDMA_PAUSE, 0);
	CHECK_EQ(dev.out_len, 8);
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN);

	/* paused mid-transfer: the device can't make it go on; unpausing does */
	reset();
	dev.out_room = 2;
	put_cmd(0, DBDMA_OUTPUT_LAST, 0, 0, 0, 0, 8, BUF, 0);
	put_cmd(1, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	start();
	control(2, DBDMA_PAUSE, DBDMA_PAUSE);
	CHECK_EQ(rd(2, DBDMA_STATUS), DBDMA_RUN | DBDMA_PAUSE);
	dev.out_room = 100;
	dbdma_kick(&dma, 2);
	CHECK_EQ(dev.out_len, 2);
	control(2, DBDMA_PAUSE, 0);
	CHECK_EQ(dev.out_len, 8);
	CHECK_EQ(rd(2, DBDMA_CMDPTR), LIST + 16);
}

/* A driver's way in: byte-reversed stores and loads that fault into mmio_emulate() */
static void test_from_guest(struct mmio_bus *bus)
{
	reset();
	put_cmd(0, DBDMA_OUTPUT_LAST, 0, DBDMA_ALWAYS, 0, 0, 6, BUF, 0);
	put_cmd(1, DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
	wr(2, DBDMA_CMDPTR, 0);
	const uint32_t prog[] = {
		LIS(3, (REGS + 2 * DBDMA_CHANNEL_SIZE) >> 16),
		ORI(3, 3, (REGS + 2 * DBDMA_CHANNEL_SIZE) & 0xFFFF),
		LI(4, LIST),
		LI(5, DBDMA_CMDPTR),
		STWBRX(4, 3, 5),                        /* CommandPtrLo = LIST */
		LIS(4, DBDMA_RUN),
		ORI(4, 4, DBDMA_RUN),
		LI(5, DBDMA_CONTROL),
		STWBRX(4, 3, 5),                        /* run */
		LI(5, DBDMA_STATUS),
		LWBRX(6, 3, 5),                         /* r6 = status */
		LI(5, DBDMA_CMDPTR),
		LWBRX(7, 3, 5),                         /* r7 = CommandPtrLo */
		SC,
	};
	const uint32_t code = 0x40000;
	for (size_t k = 0; k < sizeof prog / sizeof prog[0]; k++)
		mem_w32(&mem, code + 4 * (uint32_t)k, prog[k]);
	struct ppc_cpu cpu;
	ppc_init(&cpu, &mem);
	cpu.pc = code;
	enum ppc_exit e;
	while ((e = ppc_run(&cpu, 100)) == PPC_EXIT_DSI)
		CHECK_EQ(mmio_emulate(bus, &cpu), MMIO_DONE);
	CHECK_EQ(e, PPC_EXIT_SC);
	CHECK_EQ(dev.out_len, 6);
	CHECK_EQ(pulses, 1);
	CHECK_EQ(cpu.gpr[6], DBDMA_RUN);
	CHECK_EQ(cpu.gpr[7], LIST + 16);
	CHECK_EQ(xfer(0), (uint32_t)(DBDMA_RUN | DBDMA_ACTIVE) << 16);
}

static void test_macio_latch(void)
{
	struct macio mio;
	macio_init(&mio);
	macio_pulse(&mio, 3);
	CHECK_EQ(macio_pending(&mio, 0), 0);            /* masked */
	macio_mmio_ops.write(&mio, MACIO_INTCTL_OFFSET + 0x14, 4, 0x10000000u);         /* enable source 3 */
	CHECK_EQ(macio_pending(&mio, 0), 0x10000000u);  /* (latched while masked) */
	macio_set_line(&mio, 4, true);
	macio_mmio_ops.write(&mio, MACIO_INTCTL_OFFSET + 0x14, 4, 0x18000000u);
	CHECK_EQ(macio_mmio_ops.read(&mio, MACIO_INTCTL_OFFSET + 0x10, 4), 0x18000000u);        /* events */
	CHECK_EQ(macio_mmio_ops.read(&mio, MACIO_INTCTL_OFFSET + 0x1C, 4), 0x08000000u);        /* levels */
	macio_mmio_ops.write(&mio, MACIO_INTCTL_OFFSET + 0x18, 4, 0x18000000u);         /* clear */
	CHECK_EQ(macio_pending(&mio, 0), 0x08000000u);  /* the level stays */
	macio_pulse(&mio, 40);
	CHECK_EQ(mio.events[1], 0x00800000u);

	/* the kernel takes a pulse when it posts it: gone from the events, pending until released */
	macio_pulse(&mio, 3);
	macio_take(&mio);
	CHECK_EQ(macio_mmio_ops.read(&mio, MACIO_INTCTL_OFFSET + 0x10, 4), 0x08000000u);        /* (the level) */
	CHECK_EQ(macio_pending(&mio, 0), 0x18000000u);
	macio_mmio_ops.write(&mio, MACIO_INTCTL_OFFSET + 0x18, 4, 0x10000000u);         /* (clearing it changes nothing) */
	CHECK_EQ(macio_pending(&mio, 0), 0x18000000u);
	macio_release(&mio);
	CHECK_EQ(macio_pending(&mio, 0), 0x08000000u);
	CHECK_EQ(mio.events[1], 0x00800000u);          /* (masked: not taken) */

	/*
	 * One Mac OS hasn't handled (cleared or masked) by then stays, for one
	 * more re-evaluation (a dispatch that hasn't seen it yet); masking it
	 * handles it, and a new pulse of its source before the release is one
	 * more to handle
	 */
	macio_set_line(&mio, 4, false);
	macio_pulse(&mio, 3);
	macio_take(&mio);
	macio_release(&mio);
	CHECK_EQ(macio_pending(&mio, 0), 0x10000000u);
	macio_release(&mio);
	CHECK_EQ(macio_pending(&mio, 0), 0);
	macio_pulse(&mio, 3);
	macio_take(&mio);
	macio_mmio_ops.write(&mio, MACIO_INTCTL_OFFSET + 0x14, 4, 0x08000000u);         /* masked ... */
	macio_mmio_ops.write(&mio, MACIO_INTCTL_OFFSET + 0x14, 4, 0x18000000u);         /* ... and back */
	CHECK_EQ(macio_pending(&mio, 0), 0x10000000u);
	macio_pulse(&mio, 3);                           /* (again, before the release) */
	macio_take(&mio);
	macio_release(&mio);
	CHECK_EQ(macio_pending(&mio, 0), 0x10000000u);
	macio_mmio_ops.write(&mio, MACIO_INTCTL_OFFSET + 0x18, 4, 0x10000000u);
	macio_release(&mio);
	CHECK_EQ(macio_pending(&mio, 0), 0);
}

void test_dbdma(void)
{
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		return;
	}
	mem_map(&mem, 0, RAM, MEM_RWX);
	mem.code_changed = code_changed;
	struct mmio_bus bus = { 0 };
	mmio_add(&bus, REGS, DBDMA_SIZE, &dbdma_mmio_ops, &dma, "dbdma");
	dbdma_init(&dma, &mem, RAM);
	dma.bus = &bus;
	dma.interrupt = pulse;
	dbdma_attach(&dma, 2, &(struct dbdma_device){ fake_output, fake_input, &dev });

	test_registers();
	test_output();
	test_input();
	test_conditions();
	test_quads();
	test_errors();
	test_pause();
	test_from_guest(&bus);
	test_macio_latch();
	mem_free(&mem);
}
