#include "awacs.h"

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

/* The codec identifies itself as an AWACS (revision 2) made by Crystal (1) */
#define CODEC_REVISION 2u
#define CODEC_MANUFACTURER 1u
#define CODEC_VALID 0x00400000u

#define FRAME_BYTES 4u
#define MAX_CREDIT (FRAME_BYTES * 4096u)        /* about 0.1 s at 44.1 kHz */
#define MIX_FRAMES 256u

static const unsigned rates[8] = { 44100, 29400, 22050, 17640, 14700, 11025, 8820, 7350 };

unsigned awacs_rate(const struct awacs *a)
{
	return rates[(a->control & CONTROL_RATE_MASK) >> CONTROL_RATE_SHIFT];
}

/* 1.5 dB steps: 65536 * 10^(-1.5 k / 20) */
static const uint16_t attenuation[16] = {
	65535, 55142, 46396, 39037, 32846, 27636, 23253, 19565,
	16462, 13851, 11654, 9806, 8250, 6942, 5841, 4914,
};

/*
 * The codec's two outputs: A (line out and headphones) and C (speaker).
 * Each has an attenuation register (left in bits 6-9, right in 0-3) and a
 * mute bit in register 1, which Mac OS fades around when muting.
 */
enum { CODEC_INPUT = 0, CODEC_MUTES = 1, CODEC_OUT_A = 2, CODEC_OUT_C = 4 };
#define MUTE_A 0x200u
#define MUTE_C 0x080u
#define LOOP_THROUGH 0x040u             /* (register 1) */
#define INPUT_CD AWACS_IN_CD            /* register 0's input selector: CD, microphone, line in */
#define INPUT_SELECT (AWACS_IN_CD | AWACS_IN_MIC | AWACS_IN_LINE)
#define CD_REC_LEAD 64u                 /* the CD's frames waiting before recording takes them */

/* Input gain: 1.5 dB steps up from 0 dB, 65536 * 10^(1.5 k / 20) */
static const uint32_t input_gain[16] = {
	65536, 77890, 92572, 110022, 130762, 155410, 184706, 219523,
	260904, 310084, 368536, 438006, 520571, 618700, 735326, 873937,
};

void awacs_gain(const struct awacs *a, unsigned *left, unsigned *right)
{
	/* the host gets the louder of the outputs that are on */
	*left = *right = 0;
	const struct { unsigned reg, mute; } out[2] = { { CODEC_OUT_A, MUTE_A }, { CODEC_OUT_C, MUTE_C } };
	for (unsigned k = 0; k < 2; k++) {
		if (a->codec[CODEC_MUTES] & out[k].mute)
			continue;
		unsigned l = attenuation[(a->codec[out[k].reg] >> 6) & 15], r = attenuation[a->codec[out[k].reg] & 15];
		*left = l > *left ? l : *left;
		*right = r > *right ? r : *right;
	}
}

bool awacs_through(const struct awacs *a)
{
	unsigned in = a->codec[CODEC_INPUT] & INPUT_SELECT;
	return !in || ((in & INPUT_CD) && (a->codec[CODEC_MUTES] & LOOP_THROUGH));
}

static uint64_t frames_at(const struct awacs *a, uint64_t time)
{
	return a->base + (time - a->since) * awacs_rate(a) / a->time_hz;
}

static bool channel_moving(const struct awacs *a, unsigned n)
{
	const struct dbdma_channel *c = &a->dma->ch[n];
	return (c->status & (DBDMA_RUN | DBDMA_ACTIVE)) == (DBDMA_RUN | DBDMA_ACTIVE) && !c->idle;
}

unsigned awacs_recording(const struct awacs *a)
{
	return channel_moving(a, AWACS_DMA_IN) ? a->codec[CODEC_INPUT] & INPUT_SELECT : 0;
}

/* The CD's frames as the codec takes them, kept for input DMA while it records the CD (the oldest go when full) */
static void cd_rec_put(struct awacs *a, const int16_t *f, unsigned n)
{
	for (unsigned k = 0; k < n; k++) {
		if (a->cd_rec_count == AWACS_CD_REC) {
			a->cd_rec_head = (a->cd_rec_head + 1) % AWACS_CD_REC;
			a->cd_rec_count--;
		}
		int16_t *to = a->cd_rec[(a->cd_rec_head + a->cd_rec_count++) % AWACS_CD_REC];
		to[0] = f ? f[2 * k] : 0;
		to[1] = f ? f[2 * k + 1] : 0;
	}
}

/* ... taken (false: none; silence for any short, until a few are waiting again) */
static bool cd_rec_take(struct awacs *a, int16_t *f, unsigned n)
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
		a->cd_rec_head = (a->cd_rec_head + 1) % AWACS_CD_REC;
		a->cd_rec_count--;
	}
	return true;
}

/* Frames to the sink (NULL: the DMA's silent), with the analog input if it's played through */
static void out(struct awacs *a, const uint8_t *frames, uint64_t count)
{
	unsigned rate = awacs_rate(a);
	if (!a->input.pull) {
		if (a->sink && frames)
			a->sink(a->sink_ctx, frames, (unsigned)count, rate);
		return;
	}
	if (frames)
		a->input_mixed += count;
	bool rec = awacs_recording(a) & INPUT_CD;
	while (count) {
		unsigned n = count < MIX_FRAMES ? (unsigned)count : MIX_FRAMES;
		int16_t in[2 * MIX_FRAMES];
		uint8_t mixed[MIX_FRAMES * FRAME_BYTES];
		bool sounding = a->input.pull(a->input.ctx, in, n, rate), through = sounding && awacs_through(a);
		if (rec)
			cd_rec_put(a, sounding ? in : NULL, n);
		if (through) {
			for (unsigned k = 0; k < 2 * n; k++) {
				int v = (frames ? (int16_t)be16(frames + 2 * k) : 0) + in[k];
				put_be16(mixed + 2 * k, (uint16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v));
			}
			a->frames_through += n;
		}
		if (a->sink && (through || frames))
			a->sink(a->sink_ctx, through ? mixed : frames, n, rate);
		if (frames)
			frames += (size_t)n * FRAME_BYTES;
		count -= n;
	}
}

static void emit(struct awacs *a, const uint8_t *data, uint32_t len)
{
	if (a->npartial) {
		while (a->npartial < FRAME_BYTES && len) {
			a->partial[a->npartial++] = *data++;
			len--;
		}
		if (a->npartial < FRAME_BYTES)
			return;
		a->npartial = 0;
		a->frames_played++;
		out(a, a->partial, 1);
	}
	unsigned whole = len / FRAME_BYTES;
	if (whole) {
		a->frames_played += whole;
		out(a, data, whole);
	}
	a->npartial = len % FRAME_BYTES;
	memcpy(a->partial, data + whole * FRAME_BYTES, a->npartial);
}

static uint32_t dma_output(void *ctx, unsigned key, const uint8_t *data, uint32_t len, bool last)
{
	struct awacs *a = ctx;
	(void)key;
	(void)last;
	uint32_t n = len < a->out_credit ? len : a->out_credit;
	a->out_credit -= n;
	emit(a, data, n);
	return n;
}

/* `count` frames (at most MIX_FRAMES) of what the codec records: the inputs selected, at the input gain */
static void adc(struct awacs *a, int16_t *f, unsigned count)
{
	unsigned in = a->codec[CODEC_INPUT], rate = awacs_rate(a);
	bool any = false;
	memset(f, 0, (size_t)count * 2 * sizeof *f);
	if ((in & (AWACS_IN_MIC | AWACS_IN_LINE)) && a->mic.pull && a->mic.pull(a->mic.ctx, f, count, rate)) {
		a->frames_heard += count;
		any = true;
	}
	int16_t cd[2 * MIX_FRAMES];
	if ((in & INPUT_CD) && cd_rec_take(a, cd, count)) {
		for (unsigned k = 0; k < 2 * count; k++) {
			int v = f[k] + cd[k];
			f[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		any = true;
	}
	if (!any)
		return;
	const int64_t gain[2] = { input_gain[(in >> 4) & 15], input_gain[in & 15] };
	if (gain[0] == 65536 && gain[1] == 65536)
		return;
	for (unsigned k = 0; k < 2 * count; k++) {
		int64_t v = f[k] * gain[k & 1] / 65536;
		if (v > 32767 || v < -32768) {
			a->clip_count++;
			v = v > 0 ? 32767 : -32768;
		}
		f[k] = (int16_t)v;
	}
}

/* `n` bytes of recorded frames (left, right, big-endian), a frame cut short going on in the next call */
static void record(struct awacs *a, uint8_t *data, uint32_t n)
{
	for (; n && a->rec_at < FRAME_BYTES; n--)
		*data++ = a->rec_frame[a->rec_at++];
	while (n) {
		unsigned count = (n + FRAME_BYTES - 1) / FRAME_BYTES;
		count = count < MIX_FRAMES ? count : MIX_FRAMES;
		int16_t f[2 * MIX_FRAMES];
		adc(a, f, count);
		for (unsigned k = 0; k < count; k++) {
			uint8_t b[FRAME_BYTES];
			put_be16(b, (uint16_t)f[2 * k]);
			put_be16(b + 2, (uint16_t)f[2 * k + 1]);
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
	struct awacs *a = ctx;
	(void)key;
	(void)end;
	uint32_t n = len < a->in_credit ? len : a->in_credit;
	a->in_credit -= n;
	record(a, data, n);
	a->frames_recorded += n / FRAME_BYTES;
	a->run_recorded += n / FRAME_BYTES;
	return n;
}

void awacs_init(struct awacs *a, struct dbdma *dma, uint64_t time_hz)
{
	memset(a, 0, sizeof *a);
	a->dma = dma;
	a->time_hz = time_hz;
	a->rec_at = FRAME_BYTES;
	const struct dbdma_device out = { .output = dma_output, .ctx = a };
	const struct dbdma_device in = { .input = dma_input, .ctx = a };
	dbdma_attach(dma, AWACS_DMA_OUT, &out);
	dbdma_attach(dma, AWACS_DMA_IN, &in);
}

static bool input_active(const struct awacs *a)
{
	return a->input.active && a->input.active(a->input.ctx);
}

bool awacs_playing(const struct awacs *a)
{
	return channel_moving(a, AWACS_DMA_OUT) || input_active(a);
}

bool awacs_moving(const struct awacs *a)
{
	return channel_moving(a, AWACS_DMA_OUT) || channel_moving(a, AWACS_DMA_IN) || input_active(a);
}

/* A channel gets the bytes of `due` frames to move; one that isn't moving gets none (the time is past). */
static void advance(struct awacs *a, unsigned n, uint32_t *credit, uint64_t due)
{
	if (!channel_moving(a, n)) {
		*credit = 0;
		return;
	}
	uint64_t c = *credit + due * FRAME_BYTES;
	*credit = c > MAX_CREDIT ? MAX_CREDIT : (uint32_t)c;
	dbdma_kick(a->dma, n);
	if (!channel_moving(a, n))
		*credit = 0;
}

void awacs_set_time(struct awacs *a, uint64_t time)
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
	advance(a, AWACS_DMA_OUT, &a->out_credit, due);
	/* (the analog input alone, before input DMA, which may record it) */
	if (a->input.pull && !channel_moving(a, AWACS_DMA_OUT) && due > a->input_mixed)
		out(a, NULL, due - a->input_mixed);
	advance(a, AWACS_DMA_IN, &a->in_credit, due);
	if (!channel_moving(a, AWACS_DMA_IN)) {
		a->run_recorded = 0;
		a->cd_rec_count = 0;
		a->cd_rec_taking = false;
	}
}

static uint32_t reg_value(struct awacs *a, unsigned reg)
{
	switch (reg) {
	case REG_CONTROL:
		return a->control;
	case REG_CODEC_CONTROL:
		return a->codec_control & ~0x01000000u;         /* the write is done */
	case REG_CODEC_STATUS:
		return CODEC_REVISION << 12 | CODEC_MANUFACTURER << 8 | (a->valid ? CODEC_VALID : 0);
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

static void reg_store(struct awacs *a, unsigned reg, uint32_t v)
{
	switch (reg) {
	case REG_CONTROL:
		if ((v ^ a->control) & CONTROL_RATE_MASK) {
			a->base = a->frames;    /* (frames counted so far stay counted) */
			a->since = a->time;
		}
		a->control = v;
		break;
	case REG_CODEC_CONTROL:
		a->codec_control = v;
		a->codec[(v >> 12) & (AWACS_CODEC_REGS - 1)] = v & 0xFFF;
		a->codec_writes++;
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
static uint32_t awacs_read(void *ctx, uint32_t offset, unsigned size)
{
	struct awacs *a = ctx;
	unsigned reg = offset & 0xFF0, b = offset & 3;
	uint32_t r = (offset & 0xC) ? 0 : reg_value(a, reg), v = 0;
	for (unsigned k = 0; k < size && b + k < 4; k++)
		v = v << 8 | ((r >> (8 * (b + k))) & 0xFF);
	if (a->log)
		printf("awacs: read %02x (%u) = %0*x\n", offset, size, 2 * size, v);
	return v;
}

static void awacs_write(void *ctx, uint32_t offset, unsigned size, uint32_t value)
{
	struct awacs *a = ctx;
	unsigned reg = offset & 0xFF0, b = offset & 3;
	if (offset & 0xC)
		return;
	uint32_t r = reg == REG_FRAME_COUNT || reg == REG_CONTROL || reg == REG_BYTE_SWAP ? reg_value(a, reg) : 0;
	for (unsigned k = 0; k < size && b + k < 4; k++) {
		unsigned shift = 8 * (b + k);
		r = (r & ~(0xFFu << shift)) | ((value >> (8 * (size - 1 - k))) & 0xFF) << shift;
	}
	if (a->log)
		printf("awacs: write %02x (%u) = %08x @%llu\n", offset, size, r, (unsigned long long)a->time);
	reg_store(a, reg, r);
}

const struct mmio_ops awacs_mmio_ops = { awacs_read, awacs_write };
