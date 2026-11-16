/*
 * Compressed audio files for a cue sheet's tracks: FLAC, MP3 and Ogg Vorbis
 * at 44.1 kHz, mono or stereo (FLAC of any depth), decoded as they're read
 * into what a CD holds: 16-bit samples, left then right, little-endian (4
 * bytes a frame). A read anywhere seeks there; reading on from where the last
 * read stopped (playing) just decodes on. Told apart by their contents.
 */
#ifndef BLUE_AUDIODEC_H
#define BLUE_AUDIODEC_H

#include <stddef.h>
#include <stdint.h>

struct audiodec;

/*
 * Opens a file of one of these kinds: 1 (and *out), 0 if it isn't one, -1 if
 * it is but can't be played as a CD's audio, with the reason in `why` (its
 * rate, its channels, its length unknown, damaged).
 */
int audiodec_open(const char *path, struct audiodec **out, char *why, size_t len);
void audiodec_close(void *ctx);

/* Its bytes as CD audio, `len` at `at` (zeros past its end): 0, or -1 */
int audiodec_read(void *ctx, void *buf, size_t len, uint64_t at);

/* ... how many (4 a frame), and what it is ("FLAC") */
uint64_t audiodec_size(const struct audiodec *a);
const char *audiodec_kind(const struct audiodec *a);

#endif
