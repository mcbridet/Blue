/*
 * The sound chip: the iMac's Burgundy codec on the Mac I/O "davbus", which
 * Mac OS drives with its own sound driver (.DAVAudio in the System file, and
 * SndHWLib's BurgundyPlugin).
 *
 * The davbus's registers are little-endian, 0x10 apart, at Mac I/O + 0x14000:
 *
 *   0x00 control        input and output subframes on (bits 0-3, 4-7), sample
 *                       rate (bits 8-10), the subframe whose status 0x20
 *                       shows (bits 15-16), interrupt enables
 *   0x10 codec control  a command to the codec's own registers (see
 *                       burgundy.c); bit 24 reads 1 while one is in progress
 *                       (never here)
 *   0x20 codec status   the byte a read command fetched (bits 4-11), which of
 *                       its register's (12-13) and a count of them (14-15);
 *                       bit 22 says it's valid (`valid`)
 *   0x30 clip count     samples clipped since last read
 *   0x50 frame count    sample frames since reset, counting all the time
 *
 * Output is 16-bit big-endian stereo frames from DBDMA channel 8, taken at
 * the sample rate of guest time (burgundy_set_time()), mixed as the codec's
 * registers say and handed to a sink; input on channel 9 records at the same
 * pace, in the same format. With two subframes on (as Mac OS has them) each
 * channel moves two frames a sample period, the codec's first.
 *
 * The codec's inputs: channel 1 the CD (`input`, the CD-ROM drives' audio),
 * which it plays through to its outputs when Mac OS monitors it, mixed with
 * the DMA's frames (or alone); channels 2 and 3 the sound-in jack and the
 * built-in microphone (`mic`, the host's sound input). What input DMA
 * records is the channels Mac OS captures, at their gains and volumes.
 */
#ifndef BLUE_BURGUNDY_H
#define BLUE_BURGUNDY_H

#include <stdbool.h>
#include <stdint.h>

#include "dbdma.h"
#include "mmio.h"

#define BURGUNDY_SIZE 0x1000u
#define BURGUNDY_DMA_OUT 8u
#define BURGUNDY_DMA_IN 9u
#define BURGUNDY_REGS 128u
#define BURGUNDY_REG_COMMAND 0x00u         /* (bit 1: recalibrate) */
#define BURGUNDY_REG_ID 0x01u              /* the codec's identity: Burgundy (byte 3: 1), its maker (byte 2) */
#define BURGUNDY_ID 0x01010000u
#define BURGUNDY_CD_REC 4096u              /* the CD's frames kept for recording (MAX_CREDIT's worth) */

/* What's recorded (burgundy_recording()) */
#define BURGUNDY_IN_CD 0x1u
#define BURGUNDY_IN_LINE 0x2u              /* the sound-in jack (External Mic, Sound In) */
#define BURGUNDY_IN_MIC 0x4u               /* the built-in microphone */

/* Where played frames go: `count` frames of big-endian left/right samples */
typedef void burgundy_sink(void *ctx, const uint8_t *frames, unsigned count, unsigned rate);

/* The analog input */
struct burgundy_input {
	void (*run)(void *ctx, uint64_t time);          /* brings it to `time` */
	/* its next `count` frames (left, right) at `rate`; false if it's silent */
	bool (*pull)(void *ctx, int16_t *frames, unsigned count, unsigned rate);
	bool (*active)(const void *ctx);                /* whether it may sound (time passes at a fine grain then) */
	void *ctx;
};

struct burgundy {
	uint32_t control;
	uint32_t codec_control;                 /* last write */
	uint32_t reg[BURGUNDY_REGS];            /* the codec's registers (up to 4 bytes each) */
	uint32_t status;                        /* codec status: the last byte read */
	unsigned long reads;
	uint32_t byte_swap;
	uint32_t clip_count;
	bool valid;                             /* codec status bit 22, as a running codec's (see nk.c) */

	struct dbdma *dma;
	burgundy_sink *sink;
	void *sink_ctx;
	struct burgundy_input input;               /* (none if pull is NULL) */
	uint64_t input_mixed;                   /* frames of it played with the DMA's in this burgundy_set_time() */
	struct burgundy_input mic;              /* the microphone and sound in (pull only; none if it's NULL) */
	burgundy_sink *recorded;                /* what input DMA records goes here too, if set */

	/* recording: the CD's frames the codec took while input DMA records it, waiting for it */
	int16_t cd_rec[BURGUNDY_CD_REC][2];
	unsigned cd_rec_head, cd_rec_count;
	bool cd_rec_taking;                     /* ... since a few were waiting */
	uint8_t rec_frame[4];                   /* a frame recorded, ... */
	unsigned rec_at;                        /* ... its bytes moved so far (4: none waiting) */
	uint64_t run_recorded;                  /* the codec's frames input DMA moved since it last started */

	/* frames since reset at the current rate: base + (time - since) * rate / time_hz */
	uint64_t time, time_hz;
	uint64_t since, base;
	uint64_t frames;                        /* ... as of the last burgundy_set_time() */
	uint64_t frame_count_zero;              /* frame count register origin */
	uint32_t out_credit, in_credit;         /* bytes each DMA direction may move now */
	unsigned out_slot, in_slot;             /* the subframe each one's next frame is for (see subframes()) */
	uint8_t partial[4];                     /* output bytes short of a whole frame */
	unsigned npartial;

	bool log;
	unsigned long frames_played, frames_recorded, codec_writes;
	unsigned long frames_through;           /* ... with the analog input in them */
	unsigned long frames_heard;             /* recorded with the microphone's sound in them */
};

void burgundy_init(struct burgundy *a, struct dbdma *dma, uint64_t time_hz);

/* Guest time advances to `time` (in time_hz units): the DMA channels move what's due. */
void burgundy_set_time(struct burgundy *a, uint64_t time);

/* Sample rate in Hz, from the control register */
unsigned burgundy_rate(const struct burgundy *a);

/* Output gain of each side (65536: 0 dB), from the codec's master volume and its outputs' attenuation */
void burgundy_gain(const struct burgundy *a, unsigned *left, unsigned *right);

/* Is output DMA running, or the analog input sounding (they need time to pass at a fine grain)? */
bool burgundy_playing(const struct burgundy *a);

/* ... or input DMA? */
bool burgundy_moving(const struct burgundy *a);

/* Whether the CD is played through to the outputs */
bool burgundy_through(const struct burgundy *a);

/* What input DMA records from now (BURGUNDY_IN_ bits; 0 if it isn't moving, or nothing is captured) */
unsigned burgundy_recording(const struct burgundy *a);

extern const struct mmio_ops burgundy_mmio_ops;

#endif
