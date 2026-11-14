/*
 * Host sound output, using SDL2 loaded at run time like the window (no
 * build dependency; without the library or an audio device, sound is
 * dropped). Frames are queued to the device as they come; the queue is
 * kept near a small target so the guest's time and the device's clock
 * can't drift apart into growing delay.
 */
#ifndef SYSHM_AUDIO_H
#define SYSHM_AUDIO_H

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

#endif
