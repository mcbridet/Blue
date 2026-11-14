#include "audio.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

struct audio {
	struct sdl_audio sdl;
	uint32_t dev;
	unsigned rate;                  /* of the open device (0: none) */
	uint8_t chunk[CHUNK_FRAMES * FRAME_BYTES];
	unsigned frames;                /* in chunk */
	unsigned long dropped, padded;
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
	return true;
}

static void flush(struct audio *a)
{
	if (!a->frames)
		return;
	uint32_t queued = a->sdl.GetQueuedAudioSize(a->dev) / FRAME_BYTES;
	uint32_t target = a->rate * TARGET_MS / 1000, max = a->rate * MAX_MS / 1000;
	if (queued == 0) {
		/* (re)starting, or the guest fell behind: start again with some slack */
		static const uint8_t silence[CHUNK_FRAMES * FRAME_BYTES];
		for (uint32_t n = 0; n < target; n += CHUNK_FRAMES)
			a->sdl.QueueAudio(a->dev, silence, sizeof silence);
		a->padded += target;
	}
	if (queued + a->frames > max)
		a->dropped += a->frames;        /* far ahead of the device (not running in real time) */
	else
		a->sdl.QueueAudio(a->dev, a->chunk, a->frames * FRAME_BYTES);
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
