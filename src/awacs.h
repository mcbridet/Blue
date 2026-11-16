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
 *                       codec, and its input sense bits (0-3); bit 22 says
 *                       the codec's status is valid (`valid`)
 *   0x30 clip count     samples clipped since last read
 *   0x50 frame count    sample frames since reset, counting all the time
 *
 * Output is 16-bit big-endian stereo frames from DBDMA channel 8, taken at
 * the sample rate of guest time (awacs_set_time()) and handed to a sink;
 * input on channel 9 records at the same pace, in the same format.
 *
 * The codec's analog input (`input`) is where the iMac's CD drive plays:
 * the codec plays it through to its outputs, mixed with the DMA's frames
 * (or alone), when its input selector has the CD and loop-through is on,
 * or when no input was ever selected (Mac OS 9 sets up the input side
 * only with an input component, which Blue's sound node doesn't load).
 *
 * What input DMA records is what the input selector (register 0) has: the
 * microphone or sound-in (`mic`, the host's sound input), the CD (its
 * frames as the codec takes them), both mixed, or with none, silence; at
 * the input gain (register 0: left in bits 4-7, right in 0-3, 1.5 dB steps
 * from 0 dB), samples clipped counting in the clip count.
 */
#ifndef BLUE_AWACS_H
#define BLUE_AWACS_H

#include <stdbool.h>
#include <stdint.h>

#include "dbdma.h"
#include "mmio.h"

#define AWACS_SIZE 0x1000u
#define AWACS_DMA_OUT 8u
#define AWACS_DMA_IN 9u
#define AWACS_CODEC_REGS 8u
#define AWACS_CD_REC 4096u              /* the CD's frames kept for recording (MAX_CREDIT's worth) */

/* The input selector's inputs (codec register 0) */
#define AWACS_IN_CD 0x200u
#define AWACS_IN_MIC 0x400u
#define AWACS_IN_LINE 0x800u            /* (sound in) */

/* Where played frames go: `count` frames of big-endian left/right samples */
typedef void awacs_sink(void *ctx, const uint8_t *frames, unsigned count, unsigned rate);

/* The analog input */
struct awacs_input {
	void (*run)(void *ctx, uint64_t time);          /* brings it to `time` */
	/* its next `count` frames (left, right) at `rate`; false if it's silent */
	bool (*pull)(void *ctx, int16_t *frames, unsigned count, unsigned rate);
	bool (*active)(const void *ctx);                /* whether it may sound (time passes at a fine grain then) */
	void *ctx;
};

struct awacs {
	uint32_t control;
	uint32_t codec_control;                 /* last write */
	uint16_t codec[AWACS_CODEC_REGS];
	uint32_t byte_swap;
	uint32_t clip_count;
	bool valid;                             /* codec status bit 22, as a running codec's (see nk.c) */

	struct dbdma *dma;
	awacs_sink *sink;
	void *sink_ctx;
	struct awacs_input input;               /* (none if pull is NULL) */
	uint64_t input_mixed;                   /* frames of it played with the DMA's in this awacs_set_time() */
	struct awacs_input mic;                 /* the microphone and sound in (pull only; none if it's NULL) */

	/* recording: the CD's frames the codec took while input DMA records it, waiting for it */
	int16_t cd_rec[AWACS_CD_REC][2];
	unsigned cd_rec_head, cd_rec_count;
	bool cd_rec_taking;                     /* ... since a few were waiting */
	uint8_t rec_frame[4];                   /* a frame recorded, ... */
	unsigned rec_at;                        /* ... its bytes moved so far (4: none waiting) */
	uint64_t run_recorded;                  /* frames input DMA moved since it last started */

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
	unsigned long frames_through;           /* ... with the analog input in them */
	unsigned long frames_heard;             /* recorded with the microphone's sound in them */
};

void awacs_init(struct awacs *a, struct dbdma *dma, uint64_t time_hz);

/* Guest time advances to `time` (in time_hz units): the DMA channels move what's due. */
void awacs_set_time(struct awacs *a, uint64_t time);

/* Sample rate in Hz, from the control register */
unsigned awacs_rate(const struct awacs *a);

/* Output gain of each channel (65536 = full scale), from the codec's attenuation */
void awacs_gain(const struct awacs *a, unsigned *left, unsigned *right);

/* Is output DMA running, or the analog input sounding (they need time to pass at a fine grain)? */
bool awacs_playing(const struct awacs *a);

/* ... or input DMA? */
bool awacs_moving(const struct awacs *a);

/* Whether the analog input is played through to the outputs */
bool awacs_through(const struct awacs *a);

/* What input DMA records from now (AWACS_IN_ bits; 0 if it isn't moving, or nothing is selected) */
unsigned awacs_recording(const struct awacs *a);

extern const struct mmio_ops awacs_mmio_ops;

#endif
