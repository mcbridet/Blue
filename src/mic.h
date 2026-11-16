/*
 * Sound input from the host: what the sound chip records from its
 * microphone and sound-in inputs (burgundy.h). Frames come from the host's
 * default input device (audio.h's capture, in real time), opened only while
 * Mac OS records from those inputs and closed a little after it stops, or
 * from a WAV file standing in for the microphone (--mic-file, and scripts'
 * "mic play"), which is read only as Mac OS records, so instruction-time
 * runs stay reproducible. Either way the frames wait in a ring and are
 * resampled to the codec's rate as it takes them: in a straight line
 * between the frames on either side, as the CD's audio is (cdaudio.c).
 * Guest-visible timing never depends on the host: the codec takes frames at
 * the pace of guest time; the host only fills the ring, which drops its
 * oldest frames when full and gives silence when it runs dry.
 */
#ifndef BLUE_MIC_H
#define BLUE_MIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIC_RING 16384u                 /* frames that can wait (0.34 s at 48 kHz) */
#define MIC_LEAD 1024u                  /* the host's frames waiting before any are taken, and again after a gap */
#define MIC_CLOSE_NS 2000000000u        /* the host's device closes this long after Mac OS stops recording */
#define MIC_RETRY_NS 1000000000u        /* ... and is tried at most this often while it can't be opened */

/* Frames waiting: 16-bit, left and right */
struct mic_ring {
	int16_t f[MIC_RING][2];
	unsigned head, count;
	unsigned long dropped;          /* (the oldest, when it was full) */
};

void mic_ring_init(struct mic_ring *r);

/* Puts `frames` frames of `channels` interleaved samples in (mono to both sides; of more, the first two) */
void mic_ring_put(struct mic_ring *r, const int16_t *samples, unsigned frames, unsigned channels);

/* Takes the oldest frame (false: there's none) */
bool mic_ring_get(struct mic_ring *r, int16_t f[2]);

/* Frames at `rate`, taken at another */
struct mic_stream {
	struct mic_ring ring;
	unsigned rate;
	unsigned lead;                  /* frames waiting before any are taken (0: at once) */
	bool paced;                     /* taken up to 0.5% faster or slower to keep about 2 x lead waiting */
	bool taking;
	uint64_t phase;                 /* 32.32: where the next frame out falls between `prev` and `next` */
	int16_t prev[2], next[2];
	double level;                   /* frames waiting, smoothed (paced) */
	unsigned long gaps;             /* times it ran dry while taken */
};

void mic_stream_init(struct mic_stream *s, unsigned rate, unsigned lead, bool paced);

/* `count` frames at `rate` (left, right), resampled; silence for any it doesn't have (false: it had none) */
bool mic_stream_take(struct mic_stream *s, int16_t *out, unsigned count, unsigned rate);

/* A WAV file's sound: 16-bit stereo frames */
struct mic_wav {
	int16_t (*f)[2];
	size_t frames;
	unsigned rate;
};

/*
 * Reads a WAV file (PCM of 8, 16, 24 or 32 bits, or 32-bit floating point;
 * any rate and number of channels): 0, or -1 with the reason in `why`.
 */
int mic_wav_load(const char *path, struct mic_wav *w, char *why, size_t len);
void mic_wav_free(struct mic_wav *w);

/* ... standing in for the microphone: from its start, once or over and over */
struct mic_file {
	struct mic_wav wav;
	size_t at;                      /* the next frame to go in the stream */
	bool loop;
	struct mic_stream stream;
};

void mic_file_start(struct mic_file *f, bool loop);
bool mic_file_take(struct mic_file *f, int16_t *out, unsigned count, unsigned rate);

/* The host's input device (audio.h's capture), or a stand-in for tests */
struct mic_host {
	void *(*open)(void *ctx, unsigned *rate, unsigned *channels);   /* NULL: it can't be, for now */
	void (*close)(void *ctx, void *dev);
	unsigned (*read)(void *ctx, void *dev, int16_t *samples, unsigned max_frames);  /* what it recorded since */
	void *ctx;
};

/* What the codec's microphone and sound-in inputs hear */
struct mic {
	bool off;                       /* sound input turned off (--no-mic, the menu): silence */
	const struct mic_host *host;    /* the host's input, if it may be used (real time), or NULL */
	void *dev;                      /* ... open (while Mac OS records from it) */
	unsigned channels;
	struct mic_stream from_host;
	uint64_t wanted_ns, tried_ns;   /* host time Mac OS last recorded from it, and the device was last tried */
	bool tried;
	struct mic_file *file;          /* a WAV file instead, or NULL */
	unsigned long opens;
};

void mic_init(struct mic *m, const struct mic_host *host);
void mic_close(struct mic *m);

/*
 * Whether Mac OS records from the microphone or sound-in now (host time
 * `now`): the host's device is opened then, and closed MIC_CLOSE_NS after
 * it stops (or at once, when sound input is turned off or a file stands in).
 * It's never opened otherwise.
 */
void mic_want(struct mic *m, bool recording, uint64_t now);

/* Whether the host's device is open */
bool mic_open(const struct mic *m);

/* `count` frames at `rate` of what the microphone hears now (false: silence) */
bool mic_take(struct mic *m, int16_t *out, unsigned count, unsigned rate);

#endif
