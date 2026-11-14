#include "awacs.h"

#include <stdio.h>
#include <string.h>

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

#define FRAME_BYTES 4u
#define MAX_CREDIT (FRAME_BYTES * 4096u)        /* about 0.1 s at 44.1 kHz */

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
enum { CODEC_MUTES = 1, CODEC_OUT_A = 2, CODEC_OUT_C = 4 };
#define MUTE_A 0x200u
#define MUTE_C 0x080u

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

static uint64_t frames_at(const struct awacs *a, uint64_t time)
{
	return a->base + (time - a->since) * awacs_rate(a) / a->time_hz;
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
		if (a->sink)
			a->sink(a->sink_ctx, a->partial, 1, awacs_rate(a));
	}
	unsigned whole = len / FRAME_BYTES;
	if (whole) {
		a->frames_played += whole;
		if (a->sink)
			a->sink(a->sink_ctx, data, whole, awacs_rate(a));
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

static uint32_t dma_input(void *ctx, unsigned key, uint8_t *data, uint32_t len, bool *end)
{
	struct awacs *a = ctx;
	(void)key;
	(void)end;
	uint32_t n = len < a->in_credit ? len : a->in_credit;
	a->in_credit -= n;
	memset(data, 0, n);
	a->frames_recorded += n / FRAME_BYTES;
	return n;
}

void awacs_init(struct awacs *a, struct dbdma *dma, uint64_t time_hz)
{
	memset(a, 0, sizeof *a);
	a->dma = dma;
	a->time_hz = time_hz;
	const struct dbdma_device out = { .output = dma_output, .ctx = a };
	const struct dbdma_device in = { .input = dma_input, .ctx = a };
	dbdma_attach(dma, AWACS_DMA_OUT, &out);
	dbdma_attach(dma, AWACS_DMA_IN, &in);
}

static bool channel_moving(const struct awacs *a, unsigned n)
{
	const struct dbdma_channel *c = &a->dma->ch[n];
	return (c->status & (DBDMA_RUN | DBDMA_ACTIVE)) == (DBDMA_RUN | DBDMA_ACTIVE) && !c->idle;
}

bool awacs_playing(const struct awacs *a)
{
	return channel_moving(a, AWACS_DMA_OUT);
}

bool awacs_moving(const struct awacs *a)
{
	return channel_moving(a, AWACS_DMA_OUT) || channel_moving(a, AWACS_DMA_IN);
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
	advance(a, AWACS_DMA_OUT, &a->out_credit, due);
	advance(a, AWACS_DMA_IN, &a->in_credit, due);
}

static uint32_t reg_value(struct awacs *a, unsigned reg)
{
	switch (reg) {
	case REG_CONTROL:
		return a->control;
	case REG_CODEC_CONTROL:
		return a->codec_control & ~0x01000000u;         /* the write is done */
	case REG_CODEC_STATUS:
		return CODEC_REVISION << 12 | CODEC_MANUFACTURER << 8;
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
