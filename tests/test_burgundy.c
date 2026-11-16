/*
 * The sound chip, a Burgundy: its registers as Mac OS's driver uses them
 * (little-endian, through lwbrx/stwbrx, so mmio_ops values are swapped
 * here), the codec's own registers through them as Mac OS's BurgundyPlugin
 * writes and reads them, and sample frames moving through DBDMA at the pace
 * of guest time: played, mixed and recorded as the codec's registers say.
 */
#include <string.h>

#include "../src/burgundy.h"
#include "../src/be.h"
#include "../src/mem.h"
#include "test.h"

#define RAM 0x100000u
#define LIST 0x1000u
#define BUF 0x8000u

static struct mem mem;
static struct dbdma dma;
static struct burgundy snd;
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
	burgundy_mmio_ops.write(&snd, reg, 4, __builtin_bswap32(v));
}

static uint32_t rd(unsigned reg)
{
	return __builtin_bswap32(burgundy_mmio_ops.read(&snd, reg, 4));
}

/* A codec register of `bytes` bytes written, a byte a command (as BurgundyPlugin's BurgySetRegister) */
static void codec_set(unsigned reg, unsigned bytes, uint32_t v)
{
	for (unsigned k = 0; k < bytes; k++)
		wr(0x10, 0x200000u | reg << 12 | (bytes - 1) << 10 | k << 8 | ((v >> 8 * k) & 0xFF));
}

/* ... read (as SndHWGetHardwareRegister): the first command reads, the rest go on; each byte shows in the status */
static uint32_t codec_get(unsigned reg, unsigned bytes)
{
	uint32_t v = 0;
	unsigned count = (rd(0x20) >> 14) & 3;
	for (unsigned k = 0; k < bytes; k++) {
		wr(0x10, (k ? 0 : 0x100000u) | reg << 12 | (bytes - 1) << 10 | k << 8);
		uint32_t st = rd(0x20);
		CHECK_EQ((st >> 14) & 3, (count + 1 + k) & 3);         /* (a new byte) */
		CHECK_EQ((st >> 12) & 3, k);
		CHECK_EQ(st & 0x00800000u, 0);                          /* (not busy) */
		v |= ((st >> 4) & 0xFF) << 8 * ((st >> 12) & 3);
	}
	return v;
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
	burgundy_init(&snd, &dma, 44100);
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
	CHECK_EQ(rd(0x20) & 0xF000, 0);                 /* (not an AWACS (2) or Screamer (3): GCAwacsPlugin declines it) */
	CHECK_EQ(rd(0x20) & 0x400000, 0);
	snd.valid = true;                               /* (the codec says its status is valid) */
	CHECK_EQ(rd(0x20), 0x400000);
	wr(0x00, 0x00004200);
	CHECK_EQ(rd(0x00), 0x00004200);
	CHECK_EQ(burgundy_rate(&snd), 22050);
	wr(0x00, 0x00004000);
	CHECK_EQ(burgundy_rate(&snd), 44100);

	/* who it is: register 0 reads 0, register 1's top byte 1 (BurgundyPlugin's SndHWGetType: 3, Burgundy) */
	CHECK_EQ(codec_get(0x00, 1), 0);
	CHECK_EQ(codec_get(0x01, 4) >> 24, 1);
	CHECK_EQ((codec_get(0x01, 4) >> 16) & 0xFF, 1);        /* (SndHWGetManf) */
	CHECK_EQ(codec_get(0x18, 1), 0);                        /* (nothing plugged in) */

	/* the registers keep what's written, a byte at a time; never busy */
	codec_set(0x30, 4, 0xFEFDFCFB);
	CHECK_EQ(snd.reg[0x30], 0xFEFDFCFB);
	CHECK_EQ(codec_get(0x30, 4), 0xFEFDFCFB);
	CHECK_EQ(rd(0x10) & 0x01000000, 0);
	codec_set(0x62, 1, 0xBB);
	CHECK_EQ(codec_get(0x62, 1), 0xBB);
	wr(0x10, 0x300000u | 0x64 << 12 | 0x5A);        /* (a byte as Linux writes one) */
	CHECK_EQ(codec_get(0x64, 1), 0x5A);
	codec_set(0x00, 1, 2);                          /* recalibrated at once */
	CHECK_EQ(codec_get(0x00, 1), 0);

	/* output gain: master volume (0xFF: 0 dB, 0.375 dB steps) and the enabled outputs' attenuation (1.5 dB) */
	unsigned l, r;
	codec_set(0x60, 1, 0x06);                       /* the speaker */
	codec_set(0x30, 4, 0xFFFFFFFF);
	codec_set(0x62, 1, 0x00);
	burgundy_gain(&snd, &l, &r);
	CHECK(l == 65536 && r == 65536);
	codec_set(0x62, 1, 0x40);                       /* right 4 steps: 6 dB */
	burgundy_gain(&snd, &l, &r);
	CHECK(l == 65536 && r > 32800 && r < 32900);
	codec_set(0x30, 4, 0xF0FEF0FE);                 /* left down 0.375 dB, right 5.625 dB more */
	burgundy_gain(&snd, &l, &r);
	CHECK(l > 62700 && l < 62800 && r > 17150 && r < 17250);
	codec_set(0x64, 1, 0x00);                       /* headphones, unattenuated, but off */
	burgundy_gain(&snd, &l, &r);
	CHECK(r < 17250);
	codec_set(0x60, 1, 0x66);                       /* ... and on: the louder */
	burgundy_gain(&snd, &l, &r);
	CHECK(r > 32800);
	codec_set(0x30, 4, 0);                          /* muted */
	burgundy_gain(&snd, &l, &r);
	CHECK(l == 0 && r == 0);

	/* the frame counter runs with time and can be set */
	burgundy_set_time(&snd, 1000);
	CHECK_EQ(rd(0x50), 1000);
	wr(0x50, 0);
	burgundy_set_time(&snd, 1500);
	CHECK_EQ(rd(0x50), 500);
	wr(0x00, 0x00000200);                           /* at 22.05 kHz: half as fast */
	burgundy_set_time(&snd, 2500);
	CHECK_EQ(rd(0x50), 1000);
}

static void test_output(void)
{
	reset();
	start_ring(BURGUNDY_DMA_OUT, DBDMA_OUTPUT_MORE);
	CHECK_EQ(played_frames, 0);                     /* nothing until time passes */
	burgundy_set_time(&snd, 100);
	CHECK_EQ(played_frames, 100);
	CHECK_EQ(played_rate, 44100);
	CHECK(!memcmp(played, mem_ptr(&mem, BUF), 400));
	CHECK_EQ(pulses[BURGUNDY_DMA_OUT], 0);
	burgundy_set_time(&snd, 256);
	CHECK_EQ(pulses[BURGUNDY_DMA_OUT], 1);             /* the first buffer is done */
	burgundy_set_time(&snd, 1024 + 10);
	CHECK_EQ(played_frames, 1034);
	CHECK_EQ(pulses[BURGUNDY_DMA_OUT], 4);             /* round the ring twice */
	CHECK(!memcmp(played + 4 * 512, mem_ptr(&mem, BUF), 400));

	/* a stopped channel isn't owed the time it was stopped */
	dma_control(BURGUNDY_DMA_OUT, DBDMA_RUN, 0);
	burgundy_set_time(&snd, 44100);
	dbdma_mmio_ops.write(&dma, BURGUNDY_DMA_OUT * DBDMA_CHANNEL_SIZE + DBDMA_CMDPTR, 4, __builtin_bswap32(LIST));
	dma_control(BURGUNDY_DMA_OUT, DBDMA_RUN, DBDMA_RUN);
	unsigned before = played_frames;
	burgundy_set_time(&snd, 44100 + 50);
	CHECK_EQ(played_frames - before, 50);

	/* frames split across buffers reach the sink whole */
	reset();
	put_cmd(0, DBDMA_OUTPUT_MORE, 0, 0, 6, BUF, 0);
	put_cmd(1, DBDMA_OUTPUT_LAST, 0, 0, 6, BUF + 6, 0);
	put_cmd(2, DBDMA_STOP, 0, 0, 0, 0, 0);
	for (unsigned k = 0; k < 12; k++)
		mem_ptr(&mem, BUF)[k] = (uint8_t)(k + 1);
	dbdma_mmio_ops.write(&dma, BURGUNDY_DMA_OUT * DBDMA_CHANNEL_SIZE + DBDMA_CMDPTR, 4, __builtin_bswap32(LIST));
	dma_control(BURGUNDY_DMA_OUT, DBDMA_RUN, DBDMA_RUN);
	burgundy_set_time(&snd, 10);
	CHECK_EQ(played_frames, 3);
	CHECK(!memcmp(played, mem_ptr(&mem, BUF), 12));
	CHECK(!burgundy_playing(&snd));                    /* idle at the STOP */

	/* two subframes on (as Mac OS has them): two frames a sample period, the first the codec's */
	reset();
	wr(0x00, 0x00000033);
	start_ring(BURGUNDY_DMA_OUT, DBDMA_OUTPUT_MORE);
	burgundy_set_time(&snd, 100);
	CHECK_EQ(played_frames, 100);
	CHECK(!memcmp(played, mem_ptr(&mem, BUF), 4));
	CHECK(!memcmp(played + 4, mem_ptr(&mem, BUF + 8), 4));
	CHECK(!memcmp(played + 4 * 99, mem_ptr(&mem, BUF + 8 * 99), 4));
	CHECK_EQ(snd.frames_played, 100);
	burgundy_set_time(&snd, 128);
	CHECK_EQ(pulses[BURGUNDY_DMA_OUT], 1);             /* (256 frames moved) */

	/* none of them the codec's: nothing played */
	reset();
	wr(0x00, 0x00000020);
	start_ring(BURGUNDY_DMA_OUT, DBDMA_OUTPUT_MORE);
	burgundy_set_time(&snd, 300);
	CHECK_EQ(played_frames, 0);
	CHECK_EQ(pulses[BURGUNDY_DMA_OUT], 1);
}

static void test_input(void)
{
	reset();
	start_ring(BURGUNDY_DMA_IN, DBDMA_INPUT_MORE);
	burgundy_set_time(&snd, 300);
	CHECK_EQ(pulses[BURGUNDY_DMA_IN], 1);
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
	snd.input = (struct burgundy_input){ analog_run, analog_pull, analog_active, NULL };
	analog.on = false;
	analog.pulled = 0;
	CHECK(burgundy_through(&snd));                  /* (as it starts: the CD plays through) */
	CHECK(!burgundy_playing(&snd));
	burgundy_set_time(&snd, 100);
	CHECK_EQ(played_frames, 0);                     /* silent: nothing */

	/* alone */
	analog.on = true;
	analog.l = 1000, analog.r = -1000;
	CHECK(burgundy_playing(&snd));
	CHECK(burgundy_moving(&snd));
	burgundy_set_time(&snd, 300);
	CHECK_EQ(analog.time, 300);
	CHECK_EQ(played_frames, 200);
	CHECK_EQ(played_rate, 44100);
	CHECK_EQ((int16_t)be16(played), 1000);
	CHECK_EQ((int16_t)be16(played + 4 * 199 + 2), -1000);
	CHECK_EQ(snd.frames_through, 200);
	CHECK_EQ(snd.frames_played, 0);

	/* with the DMA's frames, mixed: as many frames as time, the input taken once for each */
	start_ring(BURGUNDY_DMA_OUT, DBDMA_OUTPUT_MORE);
	played_frames = 0;
	analog.pulled = 0;
	burgundy_set_time(&snd, 400);
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
	burgundy_set_time(&snd, 410);
	CHECK_EQ(be16(played), 32767);
	CHECK_EQ(be16(played + 2), 0x8000);

	/* not played through: the CD's channel not among the outputs' (a microphone selected); still taken */
	codec_set(0x2B, 4, 0x01000100);
	CHECK(!burgundy_through(&snd));
	played_frames = 0;
	analog.pulled = 0;
	burgundy_set_time(&snd, 420);
	CHECK_EQ(played_frames, 10);
	CHECK_EQ(be16(played), 0x7000);
	CHECK_EQ(analog.pulled, 10);
	codec_set(0x2B, 4, 0x01010101);
	codec_set(0x11, 1, 0xA0);                       /* ... or the CD not connected */
	CHECK(!burgundy_through(&snd));
	dma_control(BURGUNDY_DMA_OUT, DBDMA_RUN, 0);
	played_frames = 0;
	burgundy_set_time(&snd, 500);
	CHECK_EQ(played_frames, 0);
	CHECK_EQ(analog.pulled, 90);
	codec_set(0x11, 1, 0x0A);                       /* as Mac OS leaves it */
	CHECK(burgundy_through(&snd));
	burgundy_set_time(&snd, 600);
	CHECK_EQ(played_frames, 100);

	/* at its channel's volume: left 0 dB (0xDF), right 6 dB (16 steps of 0.375 dB) up; off */
	analog.l = 1000, analog.r = -1000;
	codec_set(0x20, 4, 0x00EF00DF);
	played_frames = 0;
	burgundy_set_time(&snd, 610);
	CHECK_EQ((int16_t)be16(played), 1000);
	CHECK((int16_t)be16(played + 2) < -1990 && (int16_t)be16(played + 2) > -2000);
	codec_set(0x20, 4, 0);
	played_frames = 0;
	burgundy_set_time(&snd, 620);
	CHECK_EQ(be16(played), 0);

	/* at the codec's rate */
	codec_set(0x20, 4, 0x00DF00DF);
	wr(0x00, 0x00000200);
	analog.rate = 0;
	played_frames = 0;
	burgundy_set_time(&snd, 820);
	CHECK_EQ(analog.rate, 22050);
	CHECK_EQ(played_rate, 22050);
	CHECK_EQ(played_frames, 100);

	analog.on = false;
	CHECK(!burgundy_playing(&snd));
	snd.input = (struct burgundy_input){ 0 };
}

/* The microphone: frames (k, 2k) counting up while `on`, otherwise silent */
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
		frames[2 * k] = mic.next, frames[2 * k + 1] = (int16_t)(2 * mic.next);
	mic.taken += count;
	return true;
}

/* A recorded frame's left (0) or right (1) sample */
static int16_t rec(unsigned frame, unsigned c)
{
	return (int16_t)be16(mem_ptr(&mem, BUF + 4 * frame + 2 * c));
}

/* The sources Mac OS's BurgundyPlugin picks: channel 1 the CD, 2 the sound-in jack, 3 the built-in microphone */
static void pick(unsigned inpsel21, unsigned inpsel3, uint32_t captures, unsigned gain_reg, unsigned gain,
		 unsigned vol_reg, uint32_t vol)
{
	codec_set(0x11, 1, inpsel21);
	codec_set(0x12, 1, inpsel3);
	codec_set(0x2A, 4, captures);
	if (gain_reg)
		codec_set(gain_reg, 1, gain);
	if (vol_reg)
		codec_set(vol_reg, 4, vol);
}

static void test_recording(void)
{
	reset();
	snd.mic = (struct burgundy_input){ NULL, mic_pull, NULL, NULL };
	mic.on = true;
	mic.next = 1;
	mic.taken = 0;

	/* nothing captured: silence, the microphone not taken */
	start_ring(BURGUNDY_DMA_IN, DBDMA_INPUT_MORE);
	CHECK_EQ(burgundy_recording(&snd), 0);
	burgundy_set_time(&snd, 10);
	CHECK_EQ(rec(0, 0), 0);
	CHECK_EQ(rec(9, 1), 0);
	CHECK_EQ(mic.taken, 0);

	/* External Mic or Sound In (channel 2, stereo), as Mac OS picks it: at 6 dB (gain 4, both sides) */
	pick(0xA0, 0x00, 0x00020002, 0x14, 0x44, 0x21, 0x00DF00DF);
	CHECK_EQ(burgundy_recording(&snd), BURGUNDY_IN_LINE);
	mic.next = 1000;
	burgundy_set_time(&snd, 20);
	CHECK_EQ(mic.rate, 44100);
	CHECK_EQ(mic.taken, 10);
	CHECK_EQ(snd.frames_heard, 10);
	CHECK_EQ(rec(10, 0), 1995);
	CHECK_EQ(rec(10, 1), 3990);
	CHECK_EQ(rec(19, 0), 2013);
	CHECK_EQ(rec(19, 1), 4026);

	/* the Built-in Mic (channel 3, one microphone: the sides mixed), at 0 dB */
	pick(0x00, 0x0A, 0x00040004, 0x15, 0x00, 0x22, 0x00DFDF00);
	CHECK_EQ(burgundy_recording(&snd), BURGUNDY_IN_MIC);
	mic.next = 100;
	burgundy_set_time(&snd, 30);
	CHECK_EQ(rec(20, 0), 150);
	CHECK_EQ(rec(20, 1), 150);
	CHECK_EQ(rec(29, 1), 163);

	/* the gain: 15 steps (22.5 dB) on the right, clipped and counted */
	rd(0x30);
	mic.next = 3000;
	codec_set(0x15, 1, 0xF0);
	burgundy_set_time(&snd, 40);
	CHECK_EQ(rec(30, 0), 4500);
	CHECK_EQ(rec(30, 1), 32767);
	CHECK_EQ(rd(0x30), 10);
	codec_set(0x15, 1, 0x00);

	/* run dry: silence */
	mic.on = false;
	burgundy_set_time(&snd, 50);
	CHECK_EQ(rec(40, 0), 0);
	CHECK_EQ(rec(49, 1), 0);

	/* the CD (channel 1): its frames as the codec takes them, played through or not */
	snd.input = (struct burgundy_input){ analog_run, analog_pull, analog_active, NULL };
	analog.on = true;
	analog.l = 500, analog.r = -500;
	analog.pulled = 0;
	pick(0x0A, 0x00, 0x00010001, 0x13, 0x00, 0x20, 0x00DF00DF);
	codec_set(0x2B, 4, 0x01000100);
	CHECK(!burgundy_through(&snd));
	CHECK_EQ(burgundy_recording(&snd), BURGUNDY_IN_CD);
	burgundy_set_time(&snd, 200);
	CHECK_EQ(analog.pulled, 150);
	CHECK_EQ(rec(50 + 64, 0), 500);                 /* (once a few are waiting) */
	CHECK_EQ(rec(199, 1), -500);

	/* the CD and the sound-in jack: mixed */
	mic.on = true;
	mic.next = 1;
	pick(0xAA, 0x00, 0x00030003, 0x14, 0x00, 0, 0);
	CHECK_EQ(burgundy_recording(&snd), BURGUNDY_IN_CD | BURGUNDY_IN_LINE);
	burgundy_set_time(&snd, 210);
	CHECK_EQ(rec(200, 0), 501);
	CHECK_EQ(rec(209, 1), -480);

	/* stopped: not recording */
	dma_control(BURGUNDY_DMA_IN, DBDMA_RUN, 0);
	CHECK_EQ(burgundy_recording(&snd), 0);
	analog.on = false;
	snd.input = (struct burgundy_input){ 0 };

	/* a frame cut short at the end of a command goes on in the next */
	reset();
	snd.mic = (struct burgundy_input){ NULL, mic_pull, NULL, NULL };
	mic.next = 0x1234;
	put_cmd(0, DBDMA_INPUT_MORE, 0, 0, 6, BUF, 0);
	put_cmd(1, DBDMA_INPUT_LAST, 0, 0, 6, BUF + 6, 0);
	put_cmd(2, DBDMA_STOP, 0, 0, 0, 0, 0);
	dbdma_mmio_ops.write(&dma, BURGUNDY_DMA_IN * DBDMA_CHANNEL_SIZE + DBDMA_CMDPTR, 4, __builtin_bswap32(LIST));
	pick(0xA0, 0x00, 0x00020002, 0x14, 0x00, 0x21, 0x00DF00DF);
	dma_control(BURGUNDY_DMA_IN, DBDMA_RUN, DBDMA_RUN);
	burgundy_set_time(&snd, 10);
	CHECK_EQ(rec(0, 0), 0x1234);
	CHECK_EQ(rec(1, 1), 2 * 0x1235);
	CHECK_EQ(rec(2, 0), 0x1236);
	CHECK_EQ(mic.next, 0x1237);
	CHECK_EQ(burgundy_recording(&snd), 0);          /* (idle at the STOP) */

	/* two subframes: the codec's frames in the first, silence in the second */
	reset();
	snd.mic = (struct burgundy_input){ NULL, mic_pull, NULL, NULL };
	mic.next = 1;
	mic.taken = 0;
	wr(0x00, 0x00000033);
	pick(0xA0, 0x00, 0x00020002, 0x14, 0x00, 0x21, 0x00DF00DF);
	start_ring(BURGUNDY_DMA_IN, DBDMA_INPUT_MORE);
	burgundy_set_time(&snd, 10);
	CHECK_EQ(mic.taken, 10);
	CHECK_EQ(snd.frames_recorded, 20);
	CHECK(rec(0, 0) == 1 && rec(1, 0) == 0 && rec(2, 0) == 2 && rec(19, 1) == 0 && rec(18, 1) == 20);
	snd.mic = (struct burgundy_input){ 0 };
}

void test_burgundy(void)
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
