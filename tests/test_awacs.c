/*
 * The AWACS-family sound chip: its registers as Mac OS's driver uses them
 * (little-endian, through lwbrx/stwbrx, so mmio_ops values are swapped
 * here), and sample frames moving through DBDMA at the pace of guest time:
 * played, and recorded from the input selector's inputs at the input gain.
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
	CHECK_EQ(rd(0x20) & 0x400000, 0);
	snd.valid = true;                               /* (the codec says its status is valid) */
	CHECK_EQ(rd(0x20), 0x402100);
	snd.valid = false;
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

/* An analog input of constant frames, while `on` */
static struct {
	bool on;
	int16_t l, r;
	unsigned long pulled;
	unsigned rate;
	uint64_t time;
} analog;

static void analog_run(void *ctx, uint64_t time)
{
	(void)ctx;
	analog.time = time;
}

static bool analog_pull(void *ctx, int16_t *frames, unsigned count, unsigned rate)
{
	(void)ctx;
	if (!analog.on)
		return false;
	for (unsigned k = 0; k < count; k++)
		frames[2 * k] = analog.l, frames[2 * k + 1] = analog.r;
	analog.pulled += count;
	analog.rate = rate;
	return true;
}

static bool analog_active(const void *ctx)
{
	(void)ctx;
	return analog.on;
}

static void test_analog_input(void)
{
	reset();
	snd.input = (struct awacs_input){ analog_run, analog_pull, analog_active, NULL };
	analog.on = false;
	analog.pulled = 0;
	CHECK(awacs_through(&snd));                     /* (no input selected yet) */
	CHECK(!awacs_playing(&snd));
	awacs_set_time(&snd, 100);
	CHECK_EQ(played_frames, 0);                     /* silent: nothing */

	/* alone */
	analog.on = true;
	analog.l = 1000, analog.r = -1000;
	CHECK(awacs_playing(&snd));
	CHECK(awacs_moving(&snd));
	awacs_set_time(&snd, 300);
	CHECK_EQ(analog.time, 300);
	CHECK_EQ(played_frames, 200);
	CHECK_EQ(played_rate, 44100);
	CHECK_EQ((int16_t)be16(played), 1000);
	CHECK_EQ((int16_t)be16(played + 4 * 199 + 2), -1000);
	CHECK_EQ(snd.frames_through, 200);
	CHECK_EQ(snd.frames_played, 0);

	/* with the DMA's frames, mixed: as many frames as time, the input taken once for each */
	start_ring(AWACS_DMA_OUT, DBDMA_OUTPUT_MORE);
	played_frames = 0;
	analog.pulled = 0;
	awacs_set_time(&snd, 400);
	CHECK_EQ(played_frames, 100);
	CHECK_EQ(analog.pulled, 100);
	const uint8_t *buf = mem_ptr(&mem, BUF);
	CHECK_EQ((int16_t)be16(played), (int16_t)be16(buf) + 1000);
	CHECK_EQ((int16_t)be16(played + 4 * 50 + 2), (int16_t)be16(buf + 4 * 50 + 2) - 1000);
	CHECK_EQ(snd.frames_played, 100);

	/* clipped, not wrapped */
	analog.l = 32000, analog.r = -32000;
	for (unsigned k = 0; k < 2048; k += 4)
		put_be16(mem_ptr(&mem, BUF + k), 0x7000), put_be16(mem_ptr(&mem, BUF + k + 2), 0x9000);
	played_frames = 0;
	awacs_set_time(&snd, 410);
	CHECK_EQ(be16(played), 32767);
	CHECK_EQ(be16(played + 2), 0x8000);

	/* not played through: the microphone selected, or the CD without loop-through; still taken */
	wr(0x10, 0x00000400);
	CHECK(!awacs_through(&snd));
	played_frames = 0;
	analog.pulled = 0;
	awacs_set_time(&snd, 420);
	CHECK_EQ(played_frames, 10);
	CHECK_EQ(be16(played), 0x7000);
	CHECK_EQ(analog.pulled, 10);
	wr(0x10, 0x00000233);
	wr(0x10, 0x00001880);
	CHECK(!awacs_through(&snd));
	dma_control(AWACS_DMA_OUT, DBDMA_RUN, 0);
	played_frames = 0;
	awacs_set_time(&snd, 500);
	CHECK_EQ(played_frames, 0);
	CHECK_EQ(analog.pulled, 90);
	wr(0x10, 0x000018C0);                           /* as Mac OS 8.5 leaves it */
	CHECK(awacs_through(&snd));
	awacs_set_time(&snd, 600);
	CHECK_EQ(played_frames, 100);

	/* at the codec's rate */
	wr(0x00, 0x00000200);
	analog.rate = 0;
	awacs_set_time(&snd, 800);
	CHECK_EQ(analog.rate, 22050);
	CHECK_EQ(played_rate, 22050);
	CHECK_EQ(played_frames, 200);

	analog.on = false;
	CHECK(!awacs_playing(&snd));
	snd.input = (struct awacs_input){ 0 };
}

/* The microphone: frames (k, -k) counting up while `on`, otherwise silent */
static struct {
	bool on;
	int16_t next;
	unsigned long taken;
	unsigned rate;
} mic;

static bool mic_pull(void *ctx, int16_t *frames, unsigned count, unsigned rate)
{
	(void)ctx;
	mic.rate = rate;
	if (!mic.on)
		return false;
	for (unsigned k = 0; k < count; k++, mic.next++)
		frames[2 * k] = mic.next, frames[2 * k + 1] = (int16_t)-mic.next;
	mic.taken += count;
	return true;
}

/* A recorded frame's left (0) or right (1) sample */
static int16_t rec(unsigned frame, unsigned c)
{
	return (int16_t)be16(mem_ptr(&mem, BUF + 4 * frame + 2 * c));
}

static void test_recording(void)
{
	reset();
	snd.mic = (struct awacs_input){ NULL, mic_pull, NULL, NULL };
	mic.on = true;
	mic.next = 1;
	mic.taken = 0;

	/* nothing selected: silence, the microphone not taken */
	start_ring(AWACS_DMA_IN, DBDMA_INPUT_MORE);
	CHECK_EQ(awacs_recording(&snd), 0);
	awacs_set_time(&snd, 10);
	CHECK_EQ(rec(0, 0), 0);
	CHECK_EQ(rec(9, 1), 0);
	CHECK_EQ(mic.taken, 0);

	/* the microphone, or sound in: its frames at the codec's rate, big-endian, left then right */
	wr(0x10, 0x00000400);
	CHECK_EQ(awacs_recording(&snd), AWACS_IN_MIC);
	awacs_set_time(&snd, 20);
	CHECK_EQ(rec(10, 0), 1);
	CHECK_EQ(rec(10, 1), -1);
	CHECK_EQ(rec(19, 0), 10);
	CHECK_EQ(mic.taken, 10);
	CHECK_EQ(mic.rate, 44100);
	CHECK_EQ(snd.frames_heard, 10);
	wr(0x10, 0x00000800);
	CHECK_EQ(awacs_recording(&snd), AWACS_IN_LINE);
	awacs_set_time(&snd, 30);
	CHECK_EQ(rec(20, 0), 11);

	/* the input gain: left 3 steps (4.5 dB), right 15 (22.5 dB: clipped, and counted) */
	rd(0x30);
	mic.next = 1000;
	wr(0x10, 0x0000043F);
	awacs_set_time(&snd, 40);
	CHECK_EQ(rec(30, 0), 1678);
	CHECK_EQ(rec(30, 1), -13335);
	CHECK_EQ(rd(0x30), 0);
	mic.next = 3000;
	awacs_set_time(&snd, 50);
	CHECK_EQ(rec(40, 0), 5036);
	CHECK_EQ(rec(40, 1), -32768);
	CHECK_EQ(rd(0x30), 10);

	/* run dry: silence */
	mic.on = false;
	wr(0x10, 0x00000400);
	awacs_set_time(&snd, 60);
	CHECK_EQ(rec(50, 0), 0);
	CHECK_EQ(rec(59, 1), 0);

	/* the CD: its frames as the codec takes them, played through or not */
	snd.input = (struct awacs_input){ analog_run, analog_pull, analog_active, NULL };
	analog.on = true;
	analog.l = 500, analog.r = -500;
	analog.pulled = 0;
	wr(0x10, 0x00000200);
	CHECK(!awacs_through(&snd));
	awacs_set_time(&snd, 200);
	CHECK_EQ(analog.pulled, 140);
	CHECK_EQ(rec(60, 0), 500);
	CHECK_EQ(rec(199, 1), -500);
	CHECK_EQ(mic.taken, 40);                        /* (not taken since: not selected) */

	/* both: mixed */
	mic.on = true;
	mic.next = 1;
	wr(0x10, 0x00000600);
	awacs_set_time(&snd, 210);
	CHECK_EQ(rec(200, 0), 501);
	CHECK_EQ(rec(209, 1), -510);

	/* stopped: not recording */
	dma_control(AWACS_DMA_IN, DBDMA_RUN, 0);
	CHECK_EQ(awacs_recording(&snd), 0);
	analog.on = false;
	snd.input = (struct awacs_input){ 0 };

	/* a frame cut short at the end of a command goes on in the next */
	reset();
	snd.mic = (struct awacs_input){ NULL, mic_pull, NULL, NULL };
	mic.next = 0x1234;
	put_cmd(0, DBDMA_INPUT_MORE, 0, 0, 6, BUF, 0);
	put_cmd(1, DBDMA_INPUT_LAST, 0, 0, 6, BUF + 6, 0);
	put_cmd(2, DBDMA_STOP, 0, 0, 0, 0, 0);
	dbdma_mmio_ops.write(&dma, AWACS_DMA_IN * DBDMA_CHANNEL_SIZE + DBDMA_CMDPTR, 4, __builtin_bswap32(LIST));
	wr(0x10, 0x00000400);
	dma_control(AWACS_DMA_IN, DBDMA_RUN, DBDMA_RUN);
	awacs_set_time(&snd, 10);
	CHECK_EQ(rec(0, 0), 0x1234);
	CHECK_EQ(rec(1, 1), -0x1235);
	CHECK_EQ(rec(2, 0), 0x1236);
	CHECK_EQ(mic.next, 0x1237);
	CHECK_EQ(awacs_recording(&snd), 0);             /* (idle at the STOP) */
	snd.mic = (struct awacs_input){ 0 };
}

void test_awacs(void)
{
	if (mem_init(&mem))
		return;
	mem_map(&mem, 0, RAM, MEM_RWX);
	test_registers();
	test_output();
	test_input();
	test_analog_input();
	test_recording();
	mem_free(&mem);
}
