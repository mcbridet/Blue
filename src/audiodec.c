#include "audiodec.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../third_party/dr_libs/dr_mp3.h"
#include "../third_party/libchdr/include/dr_libs/dr_flac.h"
#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb/stb_vorbis.c"

#define RATE 44100u
#define BUF_FRAMES 4096u
#define SEEK_POINTS_PER_SECOND 1u       /* an MP3's seek table */

enum kind { FLAC, MP3, VORBIS };

struct audiodec {
	enum kind kind;
	int fd;                         /* (FLAC, MP3: read through these) */
	uint64_t fd_pos, fd_size;
	unsigned channels;
	uint64_t frames;                /* its length */
	uint64_t pos;                   /* the frame the decoder gives next */
	drflac *flac;
	drmp3 mp3;
	bool mp3_open;
	drmp3_seek_point *seek;
	stb_vorbis *vorbis;
	int16_t buf[2 * BUF_FRAMES];
	uint8_t bytes[4 * BUF_FRAMES];
};

static size_t on_read(void *u, void *out, size_t n)
{
	struct audiodec *a = u;
	ssize_t got = pread(a->fd, out, n, (off_t)a->fd_pos);
	if (got <= 0)
		return 0;
	a->fd_pos += (uint64_t)got;
	return (size_t)got;
}

/* (origin: 0 from the start, 1 from here, 2 from the end, as both decoders number them) */
static bool on_seek(struct audiodec *a, int offset, int origin)
{
	int64_t to = (origin == 0 ? 0 : origin == 1 ? (int64_t)a->fd_pos : (int64_t)a->fd_size) + offset;
	if (to < 0 || (uint64_t)to > a->fd_size)
		return false;
	a->fd_pos = (uint64_t)to;
	return true;
}

static drflac_bool32 flac_seek(void *u, int offset, drflac_seek_origin origin)
{
	return on_seek(u, offset, origin == DRFLAC_SEEK_SET ? 0 : origin == DRFLAC_SEEK_CUR ? 1 : 2);
}

static drflac_bool32 flac_tell(void *u, drflac_int64 *at)
{
	*at = (drflac_int64)((struct audiodec *)u)->fd_pos;
	return DRFLAC_TRUE;
}

static drmp3_bool32 mp3_seek(void *u, int offset, drmp3_seek_origin origin)
{
	return on_seek(u, offset, origin == DRMP3_SEEK_SET ? 0 : origin == DRMP3_SEEK_CUR ? 1 : 2);
}

static drmp3_bool32 mp3_tell(void *u, drmp3_int64 *at)
{
	*at = (drmp3_int64)((struct audiodec *)u)->fd_pos;
	return DRMP3_TRUE;
}

static int fail(struct audiodec *a, char *why, size_t len, const char *what)
{
	snprintf(why, len, "%s", what);
	audiodec_close(a);
	return -1;
}

int audiodec_open(const char *path, struct audiodec **out, char *why, size_t len)
{
	uint8_t h[64] = { 0 };
	*out = NULL;
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	off_t size = lseek(fd, 0, SEEK_END);
	ssize_t got = pread(fd, h, sizeof h, 0);
	bool flac = got >= 4 && !memcmp(h, "fLaC", 4), ogg = got >= 4 && !memcmp(h, "OggS", 4);
	bool mp3 = got >= 4 && (!memcmp(h, "ID3", 3) || (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0 && (h[1] & 0x06)));
	if (!flac && !ogg && !mp3) {
		close(fd);
		return 0;
	}
	struct audiodec *a = calloc(1, sizeof *a);
	if (!a) {
		close(fd);
		snprintf(why, len, "out of memory");
		return -1;
	}
	a->fd = fd;
	a->fd_size = size > 0 ? (uint64_t)size : 0;
	unsigned rate = 0;
	if (flac) {
		a->kind = FLAC;
		if (!(a->flac = drflac_open(on_read, flac_seek, flac_tell, a, NULL)))
			return fail(a, why, len, "a damaged FLAC file");
		rate = a->flac->sampleRate;
		a->channels = a->flac->channels;
		a->frames = a->flac->totalPCMFrameCount;
	} else if (mp3) {
		a->kind = MP3;
		if (!drmp3_init(&a->mp3, on_read, mp3_seek, mp3_tell, NULL, a, NULL))
			return fail(a, why, len, "a damaged MP3 file");
		a->mp3_open = true;
		rate = a->mp3.sampleRate;
		a->channels = a->mp3.channels;
		a->frames = drmp3_get_pcm_frame_count(&a->mp3);
		drmp3_uint32 points = (drmp3_uint32)(a->frames / RATE * SEEK_POINTS_PER_SECOND + 1);
		if (points > 1 && (a->seek = calloc(points, sizeof *a->seek)) &&
		    drmp3_calculate_seek_points(&a->mp3, &points, a->seek))
			drmp3_bind_seek_table(&a->mp3, points, a->seek);
		if (!drmp3_seek_to_pcm_frame(&a->mp3, 0))
			return fail(a, why, len, "a damaged MP3 file");
	} else {
		a->kind = VORBIS;
		if (got < 36 || memcmp(h + 28, "\x01vorbis", 7))
			return fail(a, why, len, !memcmp(h + 28, "OpusHead", 8) ? "an Ogg Opus file: Blue reads Ogg Vorbis" :
				    "an Ogg file that isn't Ogg Vorbis, which Blue reads");
		FILE *f = fopen(path, "rb");
		int err = 0;
		if (!f || !(a->vorbis = stb_vorbis_open_file(f, 1, &err, NULL)))
			return fail(a, why, len, "a damaged Ogg Vorbis file");
		stb_vorbis_info info = stb_vorbis_get_info(a->vorbis);
		rate = info.sample_rate;
		a->channels = (unsigned)info.channels;
		a->frames = stb_vorbis_stream_length_in_samples(a->vorbis);
	}
	if (rate != RATE) {
		snprintf(why, len, "%s at %u Hz: CD audio is 44.1 kHz", audiodec_kind(a), rate);
		audiodec_close(a);
		return -1;
	}
	if (a->channels < 1 || a->channels > 2)
		return fail(a, why, len, "neither mono nor stereo");
	if (!a->frames || a->frames == UINT64_MAX)
		return fail(a, why, len, "its length isn't known");
	*out = a;
	return 1;
}

void audiodec_close(void *ctx)
{
	struct audiodec *a = ctx;
	if (!a)
		return;
	if (a->flac)
		drflac_close(a->flac);
	if (a->mp3_open)
		drmp3_uninit(&a->mp3);
	free(a->seek);
	if (a->vorbis)
		stb_vorbis_close(a->vorbis);
	close(a->fd);
	free(a);
}

uint64_t audiodec_size(const struct audiodec *a)
{
	return a->frames * 4;
}

const char *audiodec_kind(const struct audiodec *a)
{
	return a->kind == FLAC ? "FLAC" : a->kind == MP3 ? "MP3" : "Ogg Vorbis";
}

static bool seek(struct audiodec *a, uint64_t frame)
{
	bool ok = a->kind == FLAC ? drflac_seek_to_pcm_frame(a->flac, frame) :
		  a->kind == MP3 ? drmp3_seek_to_pcm_frame(&a->mp3, frame) :
		  stb_vorbis_seek(a->vorbis, (unsigned)frame) != 0;
	a->pos = frame;
	return ok;
}

/* Up to n frames into buf (the file's channels, interleaved): how many */
static uint64_t decode(struct audiodec *a, uint64_t n)
{
	if (a->kind == FLAC)
		return drflac_read_pcm_frames_s16(a->flac, n, a->buf);
	if (a->kind == MP3)
		return drmp3_read_pcm_frames_s16(&a->mp3, n, a->buf);
	int got = stb_vorbis_get_samples_short_interleaved(a->vorbis, (int)a->channels, a->buf,
							    (int)(n * a->channels));
	return got > 0 ? (uint64_t)got : 0;
}

int audiodec_read(void *ctx, void *buf, size_t len, uint64_t at)
{
	struct audiodec *a = ctx;
	uint8_t *out = buf;
	while (len) {
		uint64_t frame = at / 4;
		unsigned skip = (unsigned)(at % 4);
		if (frame >= a->frames)
			break;
		if (frame != a->pos && !seek(a, frame))
			return -1;
		uint64_t want = (skip + len + 3) / 4;
		if (want > BUF_FRAMES)
			want = BUF_FRAMES;
		if (want > a->frames - frame)
			want = a->frames - frame;
		uint64_t got = decode(a, want);
		if (!got)
			break;          /* (shorter than it said: silence) */
		a->pos += got;
		for (uint64_t k = 0; k < got; k++) {
			int16_t l = a->buf[k * a->channels], r = a->buf[k * a->channels + a->channels - 1];
			uint8_t *b = a->bytes + 4 * k;
			b[0] = (uint8_t)l, b[1] = (uint8_t)((uint16_t)l >> 8);
			b[2] = (uint8_t)r, b[3] = (uint8_t)((uint16_t)r >> 8);
		}
		size_t n = (size_t)got * 4 - skip < len ? (size_t)got * 4 - skip : len;
		memcpy(out, a->bytes + skip, n);
		out += n, at += n, len -= n;
	}
	memset(out, 0, len);
	return 0;
}
