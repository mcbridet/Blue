/*
 * The AWACS-family sound chip: its registers as Mac OS's driver uses them
 * (little-endian, through lwbrx/stwbrx, so mmio_ops values are swapped
 * here), and sample frames moving through DBDMA at the pace of guest time.
 */
#include <string.h>

#include "../src/awacs.h"
#include "../src/be.h"
#include "../src/mem.h"
#include "test.h"

#define RAM 0x100000u
#define LIST 0x1000u
#define BUF 0x8000u

static struct mem mem;
static struct dbdma dma;
static struct awacs snd;
static unsigned pulses[16];
static uint8_t played[0x10000];
static unsigned played_frames, played_rate;

static void pulse(void *ctx, unsigned source)
{
	(void)ctx;
	pulses[source & 15]++;
}

static void sink(void *ctx, const uint8_t *frames, unsigned count, unsigned rate)
{
	(void)ctx;
	if ((played_frames + count) * 4 <= sizeof played)
		memcpy(played + played_frames * 4, frames, count * 4);
	played_frames += count;
	played_rate = rate;
}

static void wr(unsigned reg, uint32_t v)
{
	awacs_mmio_ops.write(&snd, reg, 4, __builtin_bswap32(v));
}

static uint32_t rd(unsigned reg)
{
	return __builtin_bswap32(awacs_mmio_ops.read(&snd, reg, 4));
}

static void dma_control(unsigned ch, uint16_t mask, uint16_t value)
{
	dbdma_mmio_ops.write(&dma, ch * DBDMA_CHANNEL_SIZE + DBDMA_CONTROL, 4,
			     __builtin_bswap32((uint32_t)mask << 16 | value));
}

static void put_cmd(unsigned k, unsigned cmd, unsigned i, unsigned b, uint32_t req, uint32_t addr, uint32_t dep)
{
	uint8_t *p = mem_ptr(&mem, LIST + 16 * k);
	put_le32(p, (uint32_t)cmd << 28 | i << 20 | b << 18 | req);
	put_le32(p + 4, addr);
	put_le32(p + 8, dep);
	put_le32(p + 12, 0);
}

/* Time counts output frames at 44.1 kHz */
static void reset(void)
{
	dbdma_init(&dma, &mem, RAM);
	dma.interrupt = pulse;
	awacs_init(&snd, &dma, 44100);
	snd.sink = sink;
	memset(pulses, 0, sizeof pulses);
	played_frames = played_rate = 0;
	memset(mem_ptr(&mem, 0), 0, RAM);
}

/* A ring of two 256-frame buffers on channel n, each interrupting when done */
static void start_ring(unsigned n, unsigned cmd)
{
	for (unsigned k = 0; k < 2 * 1024; k++)
		mem_ptr(&mem, BUF)[k] = (uint8_t)k;
	put_cmd(0, cmd, DBDMA_ALWAYS, 0, 1024, BUF, 0);
	put_cmd(1, cmd, DBDMA_ALWAYS, DBDMA_ALWAYS, 1024, BUF + 1024, LIST);
	dbdma_mmio_ops.write(&dma, n * DBDMA_CHANNEL_SIZE + DBDMA_CMDPTR, 4, __builtin_bswap32(LIST));
	dma_control(n, DBDMA_RUN, DBDMA_RUN);
}

static void test_registers(void)
{
	reset();
	CHECK_EQ(rd(0x20) & 0xFF00, 0x2100);            /* an AWACS (2) by Crystal (1) */
	wr(0x00, 0x00004200);
	CHECK_EQ(rd(0x00), 0x00004200);
	CHECK_EQ(awacs_rate(&snd), 22050);
	wr(0x00, 0x00004000);
	CHECK_EQ(awacs_rate(&snd), 44100);

	/* codec writes: register in bits 12-15, data in 0-11; never busy */
	wr(0x10, 0x00402249);
	CHECK_EQ(snd.codec[2], 0x249);
	CHECK_EQ(rd(0x10) & 0x01000000, 0);
	wr(0x10, 0x000043CF);
	CHECK_EQ(snd.codec[4], 0x3CF);
	unsigned l, r;
	awacs_gain(&snd, &l, &r);                       /* the louder output: 9 steps of 1.5 dB */
	CHECK_EQ(l, 13851);
	CHECK_EQ(r, 13851);
	wr(0x10, 0x00002009);                           /* left 0 steps, right 9 */
	awacs_gain(&snd, &l, &r);
	CHECK_EQ(l, 65535);
	CHECK_EQ(r, 13851);
	wr(0x10, 0x000018C0);                           /* as Mac OS leaves it: the speaker (C) muted */
	wr(0x10, 0x000043C0);
	awacs_gain(&snd, &l, &r);
	CHECK_EQ(l, 65535);
	wr(0x10, 0x00001AC0);                           /* and with "Mute" checked, A too */
	awacs_gain(&snd, &l, &r);
	CHECK_EQ(l, 0);
	CHECK_EQ(r, 0);

	/* the frame counter runs with time and can be set */
	awacs_set_time(&snd, 1000);
	CHECK_EQ(rd(0x50), 1000);
	wr(0x50, 0);
	awacs_set_time(&snd, 1500);
	CHECK_EQ(rd(0x50), 500);
	wr(0x00, 0x00000200);                           /* at 22.05 kHz: half as fast */
	awacs_set_time(&snd, 2500);
	CHECK_EQ(rd(0x50), 1000);
}

static void test_output(void)
{
	reset();
	start_ring(AWACS_DMA_OUT, DBDMA_OUTPUT_MORE);
	CHECK_EQ(played_frames, 0);                     /* nothing until time passes */
	awacs_set_time(&snd, 100);
	CHECK_EQ(played_frames, 100);
	CHECK_EQ(played_rate, 44100);
	CHECK(!memcmp(played, mem_ptr(&mem, BUF), 400));
	CHECK_EQ(pulses[AWACS_DMA_OUT], 0);
	awacs_set_time(&snd, 256);
	CHECK_EQ(pulses[AWACS_DMA_OUT], 1);             /* the first buffer is done */
	awacs_set_time(&snd, 1024 + 10);
	CHECK_EQ(played_frames, 1034);
	CHECK_EQ(pulses[AWACS_DMA_OUT], 4);             /* round the ring twice */
	CHECK(!memcmp(played + 4 * 512, mem_ptr(&mem, BUF), 400));

	/* a stopped channel isn't owed the time it was stopped */
	dma_control(AWACS_DMA_OUT, DBDMA_RUN, 0);
	awacs_set_time(&snd, 44100);
	dbdma_mmio_ops.write(&dma, AWACS_DMA_OUT * DBDMA_CHANNEL_SIZE + DBDMA_CMDPTR, 4, __builtin_bswap32(LIST));
	dma_control(AWACS_DMA_OUT, DBDMA_RUN, DBDMA_RUN);
	unsigned before = played_frames;
	awacs_set_time(&snd, 44100 + 50);
	CHECK_EQ(played_frames - before, 50);

	/* frames split across buffers reach the sink whole */
	reset();
	put_cmd(0, DBDMA_OUTPUT_MORE, 0, 0, 6, BUF, 0);
	put_cmd(1, DBDMA_OUTPUT_LAST, 0, 0, 6, BUF + 6, 0);
	put_cmd(2, DBDMA_STOP, 0, 0, 0, 0, 0);
	for (unsigned k = 0; k < 12; k++)
		mem_ptr(&mem, BUF)[k] = (uint8_t)(k + 1);
	dbdma_mmio_ops.write(&dma, AWACS_DMA_OUT * DBDMA_CHANNEL_SIZE + DBDMA_CMDPTR, 4, __builtin_bswap32(LIST));
	dma_control(AWACS_DMA_OUT, DBDMA_RUN, DBDMA_RUN);
	awacs_set_time(&snd, 10);
	CHECK_EQ(played_frames, 3);
	CHECK(!memcmp(played, mem_ptr(&mem, BUF), 12));
	CHECK(!awacs_playing(&snd));                    /* idle at the STOP */
}

static void test_input(void)
{
	reset();
	start_ring(AWACS_DMA_IN, DBDMA_INPUT_MORE);
	awacs_set_time(&snd, 300);
	CHECK_EQ(pulses[AWACS_DMA_IN], 1);
	CHECK_EQ(snd.frames_recorded, 300);
	CHECK_EQ(mem_ptr(&mem, BUF)[0], 0);             /* silence */
	CHECK_EQ(mem_ptr(&mem, BUF + 1023)[0], 0);
	CHECK_EQ(mem_ptr(&mem, BUF + 1300)[0], (uint8_t)1300);         /* not yet */
	CHECK_EQ(played_frames, 0);
}

void test_awacs(void)
{
	if (mem_init(&mem))
		return;
	mem_map(&mem, 0, RAM, MEM_RWX);
	test_registers();
	test_output();
	test_input();
	mem_free(&mem);
}
