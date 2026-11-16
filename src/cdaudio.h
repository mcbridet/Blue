/*
 * A CD drive's audio player: what Mac OS's CD-ROM driver asks of the drive
 * (play a range of addresses, or a list of them, pause, stop, scan; the
 * position; the drive's volume and how its channels go out), its sound as
 * frames at 44.1 kHz. It's paced by guest time: the position at a guest
 * time follows from where and when play started, whenever it's looked at,
 * so instruction-time runs are reproducible. The frames wait in a FIFO for
 * the sound chip's analog input (burgundy.h), which takes them at its own
 * rate (cdaudio_pull()).
 */
#ifndef BLUE_CDAUDIO_H
#define BLUE_CDAUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "cdimage.h"

#define CDAUDIO_FIFO 8192u              /* frames (0.19 s) */
#define CDAUDIO_LEAD 64u                /* frames played before any are taken (see cdaudio_pull()) */
#define CDAUDIO_STEREO 9u               /* play mode: left to left, right to right */
#define CDAUDIO_SCAN_SPEED 8u           /* scanning covers 8 s a second, ... */
#define CDAUDIO_SCAN_SECTORS 4u         /* ... playing this much of each 8 */

/* Where the drive is: the values of the Apple CD-ROM driver's AudioStatus */
enum cdaudio_state {
	CDAUDIO_PLAYING = 0,
	CDAUDIO_PAUSED = 1,             /* (holding at the position) */
	CDAUDIO_DONE = 3,               /* reached the end */
	CDAUDIO_IDLE = 5,               /* not asked to play, or stopped */
};

/* Sectors from `start` up to `end` */
struct cdaudio_range {
	uint32_t start, end;
};

struct cdaudio {
	const struct cdimage *disc;     /* NULL: none */
	uint64_t hz;                    /* guest time's clock */
	enum cdaudio_state state;
	struct cdaudio_range range[CD_MAX_TRACKS];      /* what's to play, in order */
	unsigned ranges, current;
	bool repeat;                    /* ... over and over */
	uint32_t frame;                 /* the position: audio frames from LBA 0 (588 a sector) */
	uint64_t tb, rem;               /* the guest time it's for (and the part of a frame left over) */
	int scan;                       /* scanning: 1 forward, -1 back, 0 not */
	uint32_t scan_from;             /* ... the start of the bit being played */
	uint8_t volume[2];              /* left, right: 0-255 */
	uint8_t mode;                   /* where the channels go: 4 bits, as the driver's play mode */
	int16_t fifo[CDAUDIO_FIFO][2];  /* frames played, not taken yet */
	unsigned head, count;
	bool taking;                    /* ... since it had CDAUDIO_LEAD of them */
	uint32_t phase;                 /* resampling: 16.16 between `prev` and `next` */
	int16_t prev[2], next[2];
	uint8_t raw[16 * CD_SECTOR];    /* sectors read ahead: `raw_sectors` from `raw_lba` */
	uint32_t raw_lba;
	unsigned raw_sectors;
	unsigned long frames, dropped;  /* frames played, and of them, not taken before the FIFO overflowed */
};

void cdaudio_init(struct cdaudio *p, uint64_t hz);

/* A disc goes in (or out: NULL); it stops playing, silent at once. */
void cdaudio_disc(struct cdaudio *p, const struct cdimage *disc);

/* Brings the position to guest time `tb`, putting what was played in the FIFO. */
void cdaudio_run(struct cdaudio *p, uint64_t tb);

/*
 * Plays the ranges from range[first] on, from sector `at` in it (then from
 * the first range again, with `repeat`), at once; or with `hold`, holds
 * (paused) there. A range from an address to the end is one with end at the
 * lead-out.
 */
void cdaudio_play(struct cdaudio *p, uint64_t tb, const struct cdaudio_range *range, unsigned n, unsigned first,
		  uint32_t at, bool repeat, bool hold);

/* Pauses or goes on; false if it wasn't playing or paused. */
bool cdaudio_pause(struct cdaudio *p, uint64_t tb, bool pause);

void cdaudio_stop(struct cdaudio *p, uint64_t tb);

/* The play ends at `lba` instead (stopping at once if it's past). */
void cdaudio_stop_at(struct cdaudio *p, uint64_t tb, uint32_t lba);

/* ... or after range `k` (once through, if it's playing an earlier one). */
void cdaudio_stop_after(struct cdaudio *p, uint64_t tb, unsigned k);

/* Scans forward (1) or back (-1) from `lba`: bits of it, quickly, until played, paused or stopped. */
void cdaudio_scan(struct cdaudio *p, uint64_t tb, uint32_t lba, int direction);

/* The state and the position (a sector) at guest time `tb` */
enum cdaudio_state cdaudio_status(struct cdaudio *p, uint64_t tb, uint32_t *lba);

void cdaudio_volume(struct cdaudio *p, uint64_t tb, uint8_t left, uint8_t right);
void cdaudio_mode(struct cdaudio *p, uint64_t tb, uint8_t mode);

/* Whether there's sound to take: playing, or the FIFO not yet empty */
bool cdaudio_active(const struct cdaudio *p);

/*
 * Takes `count` frames (left, right) at `rate`, resampled from the FIFO;
 * false (and nothing written) once there's no sound. The taker's clock
 * and the player's both follow guest time, but count frames from different
 * moments: so that the taker never runs ahead, it takes frames only once
 * CDAUDIO_LEAD are waiting (silence meanwhile), and again so after it ran
 * short (the last frame held).
 */
bool cdaudio_pull(struct cdaudio *p, int16_t *frames, unsigned count, unsigned rate);

#endif
