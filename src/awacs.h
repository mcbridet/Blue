/*
 * The sound chip: an AWACS-family codec on the Mac I/O "davbus", as in the
 * beige G3 and the iMac, which Mac OS drives with its own sound driver
 * (.DAVAudio in the System file).
 *
 * Its registers are little-endian, 0x10 apart, at Mac I/O + 0x14000:
 *
 *   0x00 control        sample rate (bits 8-10), the subframe whose status
 *                       0x20 shows (bits 15-16), interrupt enables
 *   0x10 codec control  writes go to the codec's own registers: data in bits
 *                       0-11, register number in bits 12-15; bit 24 reads 1
 *                       while a write is in progress (never here)
 *   0x20 codec status   revision (bits 12-15) and manufacturer (8-11) of the
 *                       codec, and its input sense bits (0-3)
 *   0x30 clip count     samples clipped since last read
 *   0x50 frame count    sample frames since reset, counting all the time
 *
 * Output is 16-bit big-endian stereo frames from DBDMA channel 8, taken at
 * the sample rate of guest time (awacs_set_time()) and handed to a sink;
 * input on channel 9 records silence at the same pace.
 */
#ifndef SYSHM_AWACS_H
#define SYSHM_AWACS_H

#include <stdbool.h>
#include <stdint.h>

#include "dbdma.h"
#include "mmio.h"

#define AWACS_SIZE 0x1000u
#define AWACS_DMA_OUT 8u
#define AWACS_DMA_IN 9u
#define AWACS_CODEC_REGS 8u

/* Where played frames go: `count` frames of big-endian left/right samples */
typedef void awacs_sink(void *ctx, const uint8_t *frames, unsigned count, unsigned rate);

struct awacs {
	uint32_t control;
	uint32_t codec_control;                 /* last write */
	uint16_t codec[AWACS_CODEC_REGS];
	uint32_t byte_swap;
	uint32_t clip_count;

	struct dbdma *dma;
	awacs_sink *sink;
	void *sink_ctx;

	/* frames since reset at the current rate: base + (time - since) * rate / time_hz */
	uint64_t time, time_hz;
	uint64_t since, base;
	uint64_t frames;                        /* ... as of the last awacs_set_time() */
	uint64_t frame_count_zero;              /* frame count register origin */
	uint32_t out_credit, in_credit;         /* bytes each DMA direction may move now */
	uint8_t partial[4];                     /* output bytes short of a whole frame */
	unsigned npartial;

	bool log;
	unsigned long frames_played, frames_recorded, codec_writes;
};

void awacs_init(struct awacs *a, struct dbdma *dma, uint64_t time_hz);

/* Guest time advances to `time` (in time_hz units): the DMA channels move what's due. */
void awacs_set_time(struct awacs *a, uint64_t time);

/* Sample rate in Hz, from the control register */
unsigned awacs_rate(const struct awacs *a);

/* Output gain of each channel (65536 = full scale), from the codec's attenuation */
void awacs_gain(const struct awacs *a, unsigned *left, unsigned *right);

/* Is output DMA running (it needs time to pass at a fine grain)? */
bool awacs_playing(const struct awacs *a);

/* ... or input DMA? */
bool awacs_moving(const struct awacs *a);

extern const struct mmio_ops awacs_mmio_ops;

#endif
