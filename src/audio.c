#include "audio.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "be.h"

/* The parts of the SDL2 audio ABI we use (stable across SDL 2.x). */
#define SDL_INIT_AUDIO 0x00000010u
#define AUDIO_S16MSB 0x9010u

typedef struct {
	int freq;
	uint16_t format;
	uint8_t channels, silence;
	uint16_t samples, padding;
	uint32_t size;
	void (*callback)(void *, uint8_t *, int);
	void *userdata;
} SDL_AudioSpec;

struct sdl_audio {
	void *lib;
	int (*InitSubSystem)(uint32_t);
	void (*QuitSubSystem)(uint32_t);
	const char *(*GetError)(void);
	uint32_t (*OpenAudioDevice)(const char *, int, const SDL_AudioSpec *, SDL_AudioSpec *, int);
	void (*CloseAudioDevice)(uint32_t);
	void (*PauseAudioDevice)(uint32_t, int);
	int (*QueueAudio)(uint32_t, const void *, uint32_t);
	uint32_t (*GetQueuedAudioSize)(uint32_t);
};

#define FRAME_BYTES 4u
#define CHUNK_FRAMES 256u               /* frames gathered before they're queued */
#define TARGET_MS 60u                   /* queue kept at least this full ... */
#define MAX_MS 250u                     /* ... and at most this */

/*
 * The guest's frames come at the pace of guest time, which in real time is
 * the host clock's, and the device plays them by its own clock, which may
 * differ by a fraction of a percent; a stall of the host (a sleep that
 * overran, the window being dragged) also leaves the queue fuller once the
 * guest has caught up, if it ran dry meanwhile. Either way the queue would
 * drift from its target until it ran dry or overflowed, which is heard. So
 * frames are resampled to play up to 0.5% faster or slower as the queue,
 * smoothed, is fuller or emptier than its goal: its target plus half the
 * device's buffer (the queue's level saws by that much as the device takes
 * its buffers). A 10 ms difference plays at 0.1%, so it's made up in about
 * 10 s; drifts of up to 0.5% stay within 50 ms of the goal.
 */
#define MAX_SKEW 0.005
#define SKEW_PER_SECOND 0.1             /* skew per second of difference from the goal */
#define LEVEL_CHUNKS 32u                /* the level is smoothed over about this many chunks */

struct audio {
	struct sdl_audio sdl;
	uint32_t dev;
	unsigned rate;                  /* of the open device (0: none) */
	unsigned device_frames;         /* ... and its buffer */
	uint8_t chunk[CHUNK_FRAMES * FRAME_BYTES];
	unsigned frames;                /* in chunk */
	struct audio_pace pace;
	uint8_t paced[(CHUNK_FRAMES + CHUNK_FRAMES / 128 + 2) * FRAME_BYTES];
	unsigned long dropped, padded;
	struct audio_stats stats;
};

static bool load(struct sdl_audio *s)
{
	/* (bare names aren't looked for in Homebrew's or MacPorts' directories on macOS) */
	static const char *const names[] = { "libSDL2-2.0.so.0", "libSDL2.so", "libSDL2-2.0.dylib",
					     "/opt/homebrew/lib/libSDL2-2.0.dylib", "/opt/local/lib/libSDL2-2.0.dylib",
					     "SDL2.framework/SDL2" };
	for (size_t k = 0; k < sizeof names / sizeof names[0] && !s->lib; k++)
		s->lib = dlopen(names[k], RTLD_NOW | RTLD_LOCAL);
	if (!s->lib)
		return false;
#define SYM(field, name) \
	do { \
		*(void **)&s->field = dlsym(s->lib, name); \
		if (!s->field) \
			return false; \
	} while (0)
	SYM(InitSubSystem, "SDL_InitSubSystem");
	SYM(QuitSubSystem, "SDL_QuitSubSystem");
	SYM(GetError, "SDL_GetError");
	SYM(OpenAudioDevice, "SDL_OpenAudioDevice");
	SYM(CloseAudioDevice, "SDL_CloseAudioDevice");
	SYM(PauseAudioDevice, "SDL_PauseAudioDevice");
	SYM(QueueAudio, "SDL_QueueAudio");
	SYM(GetQueuedAudioSize, "SDL_GetQueuedAudioSize");
#undef SYM
	return true;
}

struct audio *audio_open(void)
{
	struct audio *a = calloc(1, sizeof *a);
	if (!a)
		return NULL;
	if (!load(&a->sdl)) {
		fprintf(stderr, "audio: SDL2 is not available; running without sound\n");
		goto fail;
	}
	if (a->sdl.InitSubSystem(SDL_INIT_AUDIO) != 0) {
		fprintf(stderr, "audio: %s; running without sound\n", a->sdl.GetError());
		goto fail;
	}
	return a;
fail:
	if (a->sdl.lib)
		dlclose(a->sdl.lib);
	free(a);
	return NULL;
}

void audio_close(struct audio *a)
{
	if (!a)
		return;
	if (a->dev)
		a->sdl.CloseAudioDevice(a->dev);
	a->sdl.QuitSubSystem(SDL_INIT_AUDIO);
	dlclose(a->sdl.lib);
	free(a);
}

/* A device at `rate` (SDL converts to what the hardware takes); false if there is none. */
static bool device_at(struct audio *a, unsigned rate)
{
	if (a->dev && a->rate == rate)
		return true;
	if (a->dev)
		a->sdl.CloseAudioDevice(a->dev);
	a->rate = rate;
	SDL_AudioSpec want = { .freq = (int)rate, .format = AUDIO_S16MSB, .channels = 2, .samples = 1024 }, have;
	a->dev = a->sdl.OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!a->dev) {
		fprintf(stderr, "audio: %s\n", a->sdl.GetError());
		return false;
	}
	a->sdl.PauseAudioDevice(a->dev, 0);
	a->device_frames = have.samples ? have.samples : want.samples;
	audio_pace_start(&a->pace, rate * TARGET_MS / 1000 + a->device_frames / 2);
	return true;
}

void audio_pace_start(struct audio_pace *p, unsigned goal)
{
	memset(p, 0, sizeof *p);
	p->goal = p->level = goal;
	p->step = 1ull << 32;
}

static int sample_at(const struct audio_pace *p, const uint8_t *in, uint64_t k, unsigned c)
{
	return k ? (int16_t)be16(in + (k - 1) * FRAME_BYTES + 2 * c) : p->last[c];
}

unsigned audio_pace(struct audio_pace *p, unsigned queued, unsigned rate, const uint8_t *in, unsigned count,
		    uint8_t *out)
{
	if (!count)
		return 0;
	p->level += ((double)queued - p->level) / LEVEL_CHUNKS;
	double skew = (p->level - p->goal) / rate * SKEW_PER_SECOND;
	skew = skew > MAX_SKEW ? MAX_SKEW : skew < -MAX_SKEW ? -MAX_SKEW : skew;
	p->step = (uint64_t)((1 + skew) * (double)(1ull << 32) + 0.5);
	/* frame k is `last` for 0, in[k - 1] after; a frame out between k and k + 1 is weighted by `at`'s fraction */
	unsigned n = 0;
	for (; p->at < (uint64_t)count << 32; p->at += p->step, n++) {
		uint64_t k = p->at >> 32, w = p->at & 0xFFFFFFFFu;
		for (unsigned c = 0; c < 2; c++) {
			int64_t a = sample_at(p, in, k, c), b = sample_at(p, in, k + 1, c);
			put_be16(out + n * FRAME_BYTES + 2 * c, (uint16_t)(a + ((b - a) * (int64_t)w >> 32)));
		}
	}
	p->at -= (uint64_t)count << 32;
	for (unsigned c = 0; c < 2; c++)
		p->last[c] = (int16_t)be16(in + (count - 1) * FRAME_BYTES + 2 * c);
	return n;
}

static void flush(struct audio *a)
{
	if (!a->frames)
		return;
	uint32_t queued = a->sdl.GetQueuedAudioSize(a->dev) / FRAME_BYTES;
	uint32_t target = a->rate * TARGET_MS / 1000, max = a->rate * MAX_MS / 1000;
	struct audio_stats *s = &a->stats;
	if (!s->flushes || queued < s->queued_min)
		s->queued_min = queued;
	if (queued > s->queued_max)
		s->queued_max = queued;
	if (queued == 0) {
		/* (re)starting, or the guest fell behind: start again with some slack */
		static const uint8_t silence[CHUNK_FRAMES * FRAME_BYTES];
		for (; queued < target; queued += CHUNK_FRAMES)
			a->sdl.QueueAudio(a->dev, silence, sizeof silence);
		a->padded += queued;
		s->pads++;
		audio_pace_start(&a->pace, target + a->device_frames / 2);
	}
	unsigned n = audio_pace(&a->pace, queued, a->rate, a->chunk, a->frames, a->paced);
	if (queued + n > max) {
		a->dropped += a->frames;        /* far ahead of the device (not running in real time) */
		s->drops++;
	} else {
		a->sdl.QueueAudio(a->dev, a->paced, n * FRAME_BYTES);
	}
	int skew = (int)(((int64_t)a->pace.step - (1ll << 32)) * 1000000 >> 32);       /* (ppm) */
	if (!s->flushes || skew < s->skew_min)
		s->skew_min = skew;
	if (!s->flushes || skew > s->skew_max)
		s->skew_max = skew;
	s->flushes++;
	a->frames = 0;
}

void audio_play(struct audio *a, const uint8_t *frames, unsigned count, unsigned rate)
{
	if (rate != a->rate) {
		if (a->dev)
			flush(a);
		if (!device_at(a, rate))
			return;
	}
	if (!a->dev)
		return;
	while (count) {
		unsigned n = CHUNK_FRAMES - a->frames < count ? CHUNK_FRAMES - a->frames : count;
		memcpy(a->chunk + a->frames * FRAME_BYTES, frames, n * FRAME_BYTES);
		a->frames += n;
		frames += n * FRAME_BYTES;
		count -= n;
		if (a->frames == CHUNK_FRAMES)
			flush(a);
	}
}

unsigned long audio_dropped(const struct audio *a)
{
	return a->dropped;
}

unsigned long audio_padded(const struct audio *a)
{
	return a->padded;
}

void audio_take_stats(struct audio *a, struct audio_stats *s)
{
	*s = a->stats;
	s->rate = a->rate;
	memset(&a->stats, 0, sizeof a->stats);
}
