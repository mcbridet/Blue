#include "burgundy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "be.h"

enum {
	REG_CONTROL = 0x00,
	REG_CODEC_CONTROL = 0x10,
	REG_CODEC_STATUS = 0x20,
	REG_CLIP_COUNT = 0x30,
	REG_BYTE_SWAP = 0x40,
	REG_FRAME_COUNT = 0x50,
};

#define CONTROL_RATE_SHIFT 8
#define CONTROL_RATE_MASK 0x700u

/* Codec control: a command to the codec's own registers */
#define CTRL_BUSY 0x01000000u           /* (reads 1 while a command is going on: never here) */
#define CTRL_WRITE 0x00200000u
#define CTRL_READ 0x00100000u

/* Codec status */
#define STATUS_VALID 0x00400000u
#define STATUS_DATA_SHIFT 4             /* the byte read (bits 4-11), ... */
#define STATUS_BYTE_SHIFT 12            /* ... which of its register's (12-13), ... */
#define STATUS_COUNT_SHIFT 14           /* ... and a count of the bytes read (14-15) */

#define FRAME_BYTES 4u
#define MAX_CREDIT (FRAME_BYTES * 4096u)        /* about 0.1 s at 44.1 kHz */
#define MIX_FRAMES 256u

static const unsigned rates[8] = { 44100, 29400, 22050, 17640, 14700, 11025, 8820, 7350 };

unsigned burgundy_rate(const struct burgundy *a)
{
	return rates[(a->control & CONTROL_RATE_MASK) >> CONTROL_RATE_SHIFT];
}

#define CD_REC_LEAD 64u                 /* the CD's frames waiting before recording takes them */

/*
 * The codec's registers that Blue's model uses, as Mac OS's BurgundyPlugin
 * sets them (the names are Linux's, sound/ppc/burgundy.h). Four input
 * channels, each with a source, a coarse gain and a fine volume:
 *   1: the CD, 2: the sound-in jack (External Mic, Sound In), 3: the built-in
 *   microphone, 4: the modem (none here).
 * Bit n of CAPTURESELECTS has channel n + 1's left side recorded (bit 16 + n
 * its right); likewise OUTPUTSELECTS for the outputs, where bits 8 and 24 are
 * the DMA's frames. The outputs (speaker, line out, headphones) each have an
 * enable in MORE_OUTPUTENABLES and an attenuation; MASTER_VOLUME is common.
 */
enum {
	R_INPSEL21 = 0x11,              /* channel 1's source (bits 0-3, not 0: connected), channel 2's (4-7) */
	R_INPSEL3 = 0x12,               /* channel 3's (bits 0-3) */
	R_GAINCH1 = 0x13,               /* ... 0x16: left in bits 0-3, right in 4-7, 1.5 dB steps from 0 dB */
	R_VOLCH1 = 0x20,                /* ... 0x23: bytes L to L, R to L, R to R, L to R (see level()) */
	R_CAPTURESELECTS = 0x2A,
	R_OUTPUTSELECTS = 0x2B,
	R_MASTER = 0x30,                /* left in byte 0, right in 1 (see level()) */
	R_OUTPUTENABLES = 0x60,
	R_ATTEN = 0x62,                 /* speaker; line out 0x63, headphones 0x64: left in bits 0-3, right in 4-7 */
	R_SENSE = 0x18,                 /* what's plugged in (nothing) */
};
#define SELECT_DMA 0x01000100u          /* (OUTPUTSELECTS) */
static const uint8_t enables[3][2] = { { 0x02, 0x04 }, { 0x08, 0x10 }, { 0x20, 0x40 } };

/* A volume byte as a gain (65536: 0 dB): 0 is off, then 0.375 dB steps up to `unity` and on */
static int64_t level(unsigned v, unsigned unity)
{
	return v ? (int64_t)(65536 * pow(10, ((double)v - unity) * 0.375 / 20) + 0.5) : 0;
}

/* 1.5 dB steps: up for a gain, down for an attenuation */
static int64_t steps(unsigned n, bool up)
{
	return (int64_t)(65536 * pow(10, (up ? 1.5 : -1.5) * n / 20) + 0.5);
}

#define CHANNEL_UNITY 0xDFu             /* (the level Mac OS sets them to) */
#define MASTER_UNITY 0xFFu

void burgundy_gain(const struct burgundy *a, unsigned *left, unsigned *right)
{
	/* the host gets the louder of the outputs that are on */
	int64_t g[2] = { 0, 0 };
	for (unsigned k = 0; k < 3; k++)
		for (unsigned c = 0; c < 2; c++) {
			if (!(a->reg[R_OUTPUTENABLES] & enables[k][c]))
				continue;
			int64_t v = level((a->reg[R_MASTER] >> 8 * c) & 0xFF, MASTER_UNITY) *
				    steps((a->reg[R_ATTEN + k] >> 4 * c) & 15, false) / 65536;
			g[c] = v > g[c] ? v : g[c];
		}
	*left = (unsigned)(g[0] > 65536 ? 65536 : g[0]);
	*right = (unsigned)(g[1] > 65536 ? 65536 : g[1]);
}

/* Whether channel n (0-3) has a source */
static bool connected(const struct burgundy *a, unsigned n)
{
	return n < 2 ? (a->reg[R_INPSEL21] >> 4 * n) & 15 : n == 2 ? a->reg[R_INPSEL3] & 15 : false;
}

/* Channel n's sides selected in a register of selects (bit 0: left, 1: right) */
static unsigned selected(const struct burgundy *a, unsigned reg, unsigned n)
{
	return (a->reg[reg] >> n & 1) | (a->reg[reg] >> (16 + n) & 1) << 1;
}

bool burgundy_through(const struct burgundy *a)
{
	return connected(a, 0) && selected(a, R_OUTPUTSELECTS, 0);
}

/*
 * Channel n's frames at its volume, in place: its four bytes take each side
 * in to each side out. Of the sides selected only (`sides`, from a select
 * register).
 */
static void channel_level(const struct burgundy *a, unsigned n, unsigned sides, int16_t *f, unsigned count)
{
	uint32_t v = a->reg[R_VOLCH1 + n];
	const int64_t ll = level(v & 0xFF, CHANNEL_UNITY), rl = level(v >> 8 & 0xFF, CHANNEL_UNITY);
	const int64_t rr = level(v >> 16 & 0xFF, CHANNEL_UNITY), lr = level(v >> 24, CHANNEL_UNITY);
	for (unsigned k = 0; k < count; k++) {
		int64_t l = f[2 * k], r = f[2 * k + 1];
		int64_t ol = (sides & 1) ? (l * ll + r * rl) / 65536 : 0, or = (sides & 2) ? (r * rr + l * lr) / 65536 : 0;
		f[2 * k] = (int16_t)(ol > 32767 ? 32767 : ol < -32768 ? -32768 : ol);
		f[2 * k + 1] = (int16_t)(or > 32767 ? 32767 : or < -32768 ? -32768 : or);
	}
}

static uint64_t frames_at(const struct burgundy *a, uint64_t time)
{
	return a->base + (time - a->since) * burgundy_rate(a) / a->time_hz;
}

static bool channel_moving(const struct burgundy *a, unsigned n)
{
	const struct dbdma_channel *c = &a->dma->ch[n];
	return (c->status & (DBDMA_RUN | DBDMA_ACTIVE)) == (DBDMA_RUN | DBDMA_ACTIVE) && !c->idle;
}

/*
 * The davbus's subframes: in each sample period, DMA moves a frame for each
 * subframe the control register has on (bits 4-7 for output, 0-3 for input;
 * none: one), in order. The codec's is subframe 0 (the sound node's
 * sub-frame); Mac OS 8.5 and later turn on 0 and 1. Frames for others go
 * nowhere, and record silence.
 */
static unsigned subframes(const struct burgundy *a, bool output, unsigned *codec)
{
	unsigned mask = (a->control >> (output ? 4 : 0)) & 15, n = 0;
	for (unsigned k = 0; k < 4; k++)
		n += mask >> k & 1;
	*codec = !mask || (mask & 1) ? 0 : n;   /* (n: not there) */
	return n ? n : 1;
}

unsigned burgundy_recording(const struct burgundy *a)
{
	if (!channel_moving(a, BURGUNDY_DMA_IN))
		return 0;
	static const unsigned in[3] = { BURGUNDY_IN_CD, BURGUNDY_IN_LINE, BURGUNDY_IN_MIC };
	unsigned r = 0;
	for (unsigned n = 0; n < 3; n++)
		if (connected(a, n) && selected(a, R_CAPTURESELECTS, n))
			r |= in[n];
	return r;
}

/* The CD's frames as the codec takes them, kept for input DMA while it records the CD (the oldest go when full) */
static void cd_rec_put(struct burgundy *a, const int16_t *f, unsigned n)
{
	for (unsigned k = 0; k < n; k++) {
		if (a->cd_rec_count == BURGUNDY_CD_REC) {
			a->cd_rec_head = (a->cd_rec_head + 1) % BURGUNDY_CD_REC;
			a->cd_rec_count--;
		}
		int16_t *to = a->cd_rec[(a->cd_rec_head + a->cd_rec_count++) % BURGUNDY_CD_REC];
		to[0] = f ? f[2 * k] : 0;
		to[1] = f ? f[2 * k + 1] : 0;
	}
}

/* ... taken (false: none; silence for any short, until a few are waiting again) */
static bool cd_rec_take(struct burgundy *a, int16_t *f, unsigned n)
{
	if (!a->cd_rec_taking && a->cd_rec_count < CD_REC_LEAD)
		return false;
	a->cd_rec_taking = true;
	for (unsigned k = 0; k < n; k++) {
		if (!a->cd_rec_count) {
			a->cd_rec_taking = false;
			memset(f + 2 * k, 0, (size_t)(n - k) * 2 * sizeof *f);
			break;
		}
		memcpy(f + 2 * k, a->cd_rec[a->cd_rec_head], 2 * sizeof *f);
		a->cd_rec_head = (a->cd_rec_head + 1) % BURGUNDY_CD_REC;
		a->cd_rec_count--;
	}
	return true;
}

static int16_t clip(int64_t v)
{
	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/*
 * Frames to the sink (NULL: the DMA's silent), mixed as OUTPUTSELECTS says:
 * the DMA's, and the CD's channel at its volume if it's played through
 */
static void out(struct burgundy *a, const uint8_t *frames, uint64_t count)
{
	unsigned rate = burgundy_rate(a);
	uint32_t dma = a->reg[R_OUTPUTSELECTS] & SELECT_DMA;
	if (!a->input.pull) {
		if (a->sink && frames && dma == SELECT_DMA)
			a->sink(a->sink_ctx, frames, (unsigned)count, rate);
		return;
	}
	if (frames)
		a->input_mixed += count;
	bool rec = burgundy_recording(a) & BURGUNDY_IN_CD;
	while (count) {
		unsigned n = count < MIX_FRAMES ? (unsigned)count : MIX_FRAMES;
		int16_t in[2 * MIX_FRAMES];
		uint8_t mixed[MIX_FRAMES * FRAME_BYTES];
		bool sounding = a->input.pull(a->input.ctx, in, n, rate), through = sounding && burgundy_through(a);
		if (rec)
			cd_rec_put(a, sounding ? in : NULL, n);
		if (through) {
			channel_level(a, 0, selected(a, R_OUTPUTSELECTS, 0), in, n);
			a->frames_through += n;
		}
		if (frames && dma == SELECT_DMA && !through) {
			if (a->sink)
				a->sink(a->sink_ctx, frames, n, rate);
		} else if (through || (frames && dma)) {
			for (unsigned k = 0; k < 2 * n; k++) {
				bool side = dma & (k & 1 ? 0x01000000u : 0x100u);
				int v = (frames && side ? (int16_t)be16(frames + 2 * k) : 0) + (through ? in[k] : 0);
				put_be16(mixed + 2 * k, (uint16_t)clip(v));
			}
			if (a->sink)
				a->sink(a->sink_ctx, mixed, n, rate);
		}
		if (frames)
			frames += (size_t)n * FRAME_BYTES;
		count -= n;
	}
}

/* Whole frames from output DMA: the codec's subframe's are played */
static void play(struct burgundy *a, const uint8_t *data, unsigned count)
{
	unsigned codec, slots = subframes(a, true, &codec);
	if (slots == 1 && codec == 0) {
		a->frames_played += count;
		out(a, data, count);
		return;
	}
	uint8_t kept[MIX_FRAMES * FRAME_BYTES];
	unsigned n = 0;
	for (unsigned k = 0; k < count; k++) {
		if (a->out_slot == codec)
			memcpy(kept + FRAME_BYTES * n++, data + FRAME_BYTES * k, FRAME_BYTES);
		a->out_slot = (a->out_slot + 1) % slots;
		if (n == MIX_FRAMES || (k + 1 == count && n)) {
			a->frames_played += n;
			out(a, kept, n);
			n = 0;
		}
	}
}

static void emit(struct burgundy *a, const uint8_t *data, uint32_t len)
{
	if (a->npartial) {
		while (a->npartial < FRAME_BYTES && len) {
			a->partial[a->npartial++] = *data++;
			len--;
		}
		if (a->npartial < FRAME_BYTES)
			return;
		a->npartial = 0;
		play(a, a->partial, 1);
	}
	unsigned whole = len / FRAME_BYTES;
	if (whole)
		play(a, data, whole);
	a->npartial = len % FRAME_BYTES;
	memcpy(a->partial, data + whole * FRAME_BYTES, a->npartial);
}

static uint32_t dma_output(void *ctx, unsigned key, const uint8_t *data, uint32_t len, bool last)
{
	struct burgundy *a = ctx;
	(void)key;
	(void)last;
	uint32_t n = len < a->out_credit ? len : a->out_credit;
	a->out_credit -= n;
	emit(a, data, n);
	return n;
}

/*
 * `count` frames (at most MIX_FRAMES) of what the codec records: the
 * channels CAPTURESELECTS has, each at its gain and volume, mixed. Clipped
 * samples count in the clip count.
 */
static void adc(struct burgundy *a, int16_t *f, unsigned count)
{
	unsigned rate = burgundy_rate(a);
	bool mic_taken = false, mic_sounds = false;
	int64_t sum[2 * MIX_FRAMES] = { 0 };
	int16_t mic[2 * MIX_FRAMES];
	for (unsigned n = 0; n < 3; n++) {
		unsigned sides = selected(a, R_CAPTURESELECTS, n);
		if (!connected(a, n) || !sides)
			continue;
		int16_t ch[2 * MIX_FRAMES];
		if (n == 0) {
			if (!cd_rec_take(a, ch, count))
				continue;
		} else {
			if (!mic_taken) {
				mic_taken = true;
				mic_sounds = a->mic.pull && a->mic.pull(a->mic.ctx, mic, count, rate);
				if (mic_sounds)
					a->frames_heard += count;
			}
			if (!mic_sounds)
				continue;
			memcpy(ch, mic, (size_t)count * 2 * sizeof *ch);
			if (n == 2)     /* (the built-in microphone is one: the host's sides mixed) */
				for (unsigned k = 0; k < count; k++)
					ch[2 * k] = ch[2 * k + 1] = (int16_t)((ch[2 * k] + ch[2 * k + 1]) / 2);
		}
		channel_level(a, n, sides, ch, count);
		const int64_t g[2] = { steps(a->reg[R_GAINCH1 + n] & 15, true), steps(a->reg[R_GAINCH1 + n] >> 4 & 15, true) };
		for (unsigned k = 0; k < 2 * count; k++)
			sum[k] += ch[k] * g[k & 1] / 65536;
	}
	for (unsigned k = 0; k < 2 * count; k++) {
		if (sum[k] > 32767 || sum[k] < -32768)
			a->clip_count++;
		f[k] = clip(sum[k]);
	}
}

/*
 * `n` bytes of recorded frames (left, right, big-endian), the codec's in its
 * subframe and silence in the others; a frame cut short goes on in the next
 * call.
 */
static void record(struct burgundy *a, uint8_t *data, uint32_t n)
{
	for (; n && a->rec_at < FRAME_BYTES; n--)
		*data++ = a->rec_frame[a->rec_at++];
	unsigned codec, slots = subframes(a, false, &codec);
	while (n) {
		unsigned count = (n + FRAME_BYTES - 1) / FRAME_BYTES, want = 0;
		count = count < MIX_FRAMES ? count : MIX_FRAMES;
		for (unsigned k = 0; k < count; k++)
			want += (a->in_slot + k) % slots == codec;
		int16_t f[2 * MIX_FRAMES];
		a->run_recorded += want;
		if (want) {
			adc(a, f, want);
			if (a->recorded) {
				uint8_t b[MIX_FRAMES * FRAME_BYTES];
				for (unsigned k = 0; k < 2 * want; k++)
					put_be16(b + 2 * k, (uint16_t)f[k]);
				a->recorded(a->sink_ctx, b, want, burgundy_rate(a));
			}
		}
		for (unsigned k = 0, j = 0; k < count; k++) {
			uint8_t b[FRAME_BYTES] = { 0 };
			if (a->in_slot == codec) {
				put_be16(b, (uint16_t)f[2 * j]);
				put_be16(b + 2, (uint16_t)f[2 * j + 1]);
				j++;
			}
			a->in_slot = (a->in_slot + 1) % slots;
			unsigned take = n < FRAME_BYTES ? n : FRAME_BYTES;
			memcpy(data, b, take);
			data += take;
			n -= take;
			if (take < FRAME_BYTES) {
				memcpy(a->rec_frame, b, sizeof b);
				a->rec_at = take;
			}
		}
	}
}

static uint32_t dma_input(void *ctx, unsigned key, uint8_t *data, uint32_t len, bool *end)
{
	struct burgundy *a = ctx;
	(void)key;
	(void)end;
	uint32_t n = len < a->in_credit ? len : a->in_credit;
	a->in_credit -= n;
	record(a, data, n);
	a->frames_recorded += n / FRAME_BYTES;
	return n;
}

void burgundy_init(struct burgundy *a, struct dbdma *dma, uint64_t time_hz)
{
	memset(a, 0, sizeof *a);
	a->dma = dma;
	a->time_hz = time_hz;
	a->rec_at = FRAME_BYTES;
	/*
	 * Until Mac OS sets the codec up, the DMA's frames and the CD play
	 * through the speaker at full volume.
	 */
	a->reg[BURGUNDY_REG_ID] = BURGUNDY_ID;
	a->reg[R_INPSEL21] = 0x0A;
	a->reg[R_VOLCH1] = 0x00DF00DF;
	a->reg[R_OUTPUTSELECTS] = SELECT_DMA | 0x10001;
	a->reg[R_MASTER] = 0xFFFFFFFF;
	a->reg[R_OUTPUTENABLES] = enables[0][0] | enables[0][1];
	const struct dbdma_device out = { .output = dma_output, .ctx = a };
	const struct dbdma_device in = { .input = dma_input, .ctx = a };
	dbdma_attach(dma, BURGUNDY_DMA_OUT, &out);
	dbdma_attach(dma, BURGUNDY_DMA_IN, &in);
}

static bool input_active(const struct burgundy *a)
{
	return a->input.active && a->input.active(a->input.ctx);
}

bool burgundy_playing(const struct burgundy *a)
{
	return channel_moving(a, BURGUNDY_DMA_OUT) || input_active(a);
}

bool burgundy_moving(const struct burgundy *a)
{
	return channel_moving(a, BURGUNDY_DMA_OUT) || channel_moving(a, BURGUNDY_DMA_IN) || input_active(a);
}

/* A channel gets the bytes of `due` frames to move; one that isn't moving gets none (the time is past). */
static void advance(struct burgundy *a, unsigned n, uint32_t *credit, unsigned *slot, uint64_t due)
{
	if (!channel_moving(a, n)) {
		*credit = 0;
		*slot = 0;
		return;
	}
	unsigned codec, slots = subframes(a, n == BURGUNDY_DMA_OUT, &codec);
	uint64_t c = *credit + due * FRAME_BYTES * slots;
	*credit = c > MAX_CREDIT * slots ? MAX_CREDIT * slots : (uint32_t)c;
	dbdma_kick(a->dma, n);
	if (!channel_moving(a, n)) {
		*credit = 0;
		*slot = 0;
	}
}

void burgundy_set_time(struct burgundy *a, uint64_t time)
{
	if (time <= a->time)
		return;
	a->time = time;
	uint64_t f = frames_at(a, time), due = f - a->frames;
	a->frames = f;
	if (!due)
		return;
	if (a->input.run)
		a->input.run(a->input.ctx, time);
	a->input_mixed = 0;
	advance(a, BURGUNDY_DMA_OUT, &a->out_credit, &a->out_slot, due);
	/* (the analog input alone, before input DMA, which may record it) */
	if (a->input.pull && !channel_moving(a, BURGUNDY_DMA_OUT) && due > a->input_mixed)
		out(a, NULL, due - a->input_mixed);
	advance(a, BURGUNDY_DMA_IN, &a->in_credit, &a->in_slot, due);
	if (!channel_moving(a, BURGUNDY_DMA_IN)) {
		a->run_recorded = 0;
		a->cd_rec_count = 0;
		a->cd_rec_taking = false;
	}
}

/*
 * A command to the codec: a byte of one of its registers written (bits 0-7,
 * byte 8-9 of the register's 1 + bits 10-11), or read: shown in the codec
 * status with which byte it is and a count that changes with each.
 */
static void command(struct burgundy *a, uint32_t v)
{
	unsigned r = (v >> 12) & (BURGUNDY_REGS - 1), k = (v >> 8) & 3, last = (v >> 10) & 3;
	if (v & CTRL_WRITE) {
		a->reg[r] = (a->reg[r] & ~(0xFFu << 8 * k)) | (v & 0xFF) << 8 * k;
		if (k < last)
			return;
		a->codec_writes++;
		if (a->log)
			printf("burgundy: register %02x = %0*x @%llu\n", r, 2 * (last + 1), a->reg[r],
			       (unsigned long long)a->time);
		if (r == BURGUNDY_REG_COMMAND)
			a->reg[r] = 0;          /* (recalibrated at once) */
		return;
	}
	a->reads++;
	a->status = ((a->reg[r] >> 8 * k) & 0xFF) << STATUS_DATA_SHIFT | k << STATUS_BYTE_SHIFT |
		    (a->reads & 3) << STATUS_COUNT_SHIFT;
}

static uint32_t reg_value(struct burgundy *a, unsigned reg)
{
	switch (reg) {
	case REG_CONTROL:
		return a->control;
	case REG_CODEC_CONTROL:
		return a->codec_control & ~CTRL_BUSY;
	case REG_CODEC_STATUS:
		return (a->valid ? STATUS_VALID : 0) | a->status;
	case REG_CLIP_COUNT: {
		uint32_t v = a->clip_count;
		a->clip_count = 0;
		return v;
	}
	case REG_BYTE_SWAP:
		return a->byte_swap;
	case REG_FRAME_COUNT:
		return (uint32_t)(a->frames - a->frame_count_zero);
	default:
		return 0;
	}
}

static void reg_store(struct burgundy *a, unsigned reg, uint32_t v)
{
	switch (reg) {
	case REG_CONTROL:
		if ((v ^ a->control) & 0xFF)
			a->out_slot = a->in_slot = 0;
		if ((v ^ a->control) & CONTROL_RATE_MASK) {
			a->base = a->frames;    /* (frames counted so far stay counted) */
			a->since = a->time;
		}
		a->control = v;
		break;
	case REG_CODEC_CONTROL:
		a->codec_control = v;
		command(a, v);
		break;
	case REG_BYTE_SWAP:
		a->byte_swap = v;
		break;
	case REG_FRAME_COUNT:
		a->frame_count_zero = a->frames - v;
		break;
	}
}

/* Little-endian registers through the big-endian view of mmio_ops (see dbdma.c) */
static uint32_t burgundy_read(void *ctx, uint32_t offset, unsigned size)
{
	struct burgundy *a = ctx;
	unsigned reg = offset & 0xFF0, b = offset & 3;
	uint32_t r = (offset & 0xC) ? 0 : reg_value(a, reg), v = 0;
	for (unsigned k = 0; k < size && b + k < 4; k++)
		v = v << 8 | ((r >> (8 * (b + k))) & 0xFF);
	if (a->log)
		printf("burgundy: read %02x (%u) = %0*x\n", offset, size, 2 * size, v);
	return v;
}

static void burgundy_write(void *ctx, uint32_t offset, unsigned size, uint32_t value)
{
	struct burgundy *a = ctx;
	unsigned reg = offset & 0xFF0, b = offset & 3;
	if (offset & 0xC)
		return;
	uint32_t r = reg == REG_FRAME_COUNT || reg == REG_CONTROL || reg == REG_BYTE_SWAP ? reg_value(a, reg) : 0;
	for (unsigned k = 0; k < size && b + k < 4; k++) {
		unsigned shift = 8 * (b + k);
		r = (r & ~(0xFFu << shift)) | ((value >> (8 * (size - 1 - k))) & 0xFF) << shift;
	}
	if (a->log)
		printf("burgundy: write %02x (%u) = %08x @%llu\n", offset, size, r, (unsigned long long)a->time);
	reg_store(a, reg, r);
}

const struct mmio_ops burgundy_mmio_ops = { burgundy_read, burgundy_write };
