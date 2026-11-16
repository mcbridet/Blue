/*
 * Host sound output, using SDL2 loaded at run time like the window (no
 * build dependency; without the library or an audio device, sound is
 * dropped). Frames are queued to the device as they come; the queue is
 * kept near a small target so the guest's time and the device's clock
 * can't drift apart into growing delay: frames are resampled to play a
 * little faster or slower as it's fuller or emptier (see audio.c). And the
 * host's sound input, the same way, for the microphone.
 */
#ifndef BLUE_AUDIO_H
#define BLUE_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

struct audio;

/* Returns NULL (with a message on stderr) if there is no sound output. */
struct audio *audio_open(void);
void audio_close(struct audio *a);

/* Plays `count` frames of 16-bit big-endian stereo samples at `rate` Hz. */
void audio_play(struct audio *a, const uint8_t *frames, unsigned count, unsigned rate);

/* Frames dropped because the device was too far behind, and silence added because it ran dry */
unsigned long audio_dropped(const struct audio *a);
unsigned long audio_padded(const struct audio *a);

/* What the queue did since the last call (BLUE_LOG_AUDIO) */
struct audio_stats {
	unsigned rate;
	unsigned long flushes;          /* chunks given to the device */
	unsigned queued_min, queued_max;        /* frames queued before them */
	int skew_min, skew_max;         /* how much faster they played (parts per million) */
	unsigned long drops, pads;      /* chunks dropped, and times silence was added */
};
void audio_take_stats(struct audio *a, struct audio_stats *s);

/*
 * The host's default sound input, for the sound chip's microphone (mic.h):
 * 16-bit samples at the device's own rate, with its own number of channels.
 * On macOS it opens only once the system lets Blue use the microphone
 * (asking the user the first time); until then, or without a device, NULL
 * (and a line on stderr saying why, once).
 */
struct audio_capture;
struct audio_capture *audio_capture_open(unsigned *rate, unsigned *channels);
void audio_capture_close(struct audio_capture *c);

/* Takes up to `max_frames` frames it recorded since; returns how many */
unsigned audio_capture_read(struct audio_capture *c, int16_t *samples, unsigned max_frames);

/* Whether the host lets Blue use the microphone: 1 yes, 0 no, -1 not yet (it's asking the user) */
int audio_mic_permission(void);

/* The queue's pace (in audio.c; here for tests) */
struct audio_pace {
	double goal, level;             /* frames queued: the goal, and lately (smoothed) */
	uint64_t step;                  /* frames in for each frame out (32.32 fixed point) */
	uint64_t at;                    /* where the next frame out falls, in frames in from `last` */
	int16_t last[2];                /* the last frame in */
};
void audio_pace_start(struct audio_pace *p, unsigned goal);
/*
 * `count` frames (16-bit big-endian stereo) to frames out, resampled for
 * `queued` frames in the queue now; `out` has room for count + count / 128
 * + 2. The frames out lag by one frame in.
 */
unsigned audio_pace(struct audio_pace *p, unsigned queued, unsigned rate, const uint8_t *in, unsigned count,
		    uint8_t *out);

#endif
