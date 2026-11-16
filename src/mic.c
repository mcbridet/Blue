#include "mic.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mic_ring_init(struct mic_ring *r)
{
	r->head = r->count = 0;
	r->dropped = 0;
}

void mic_ring_put(struct mic_ring *r, const int16_t *samples, unsigned frames, unsigned channels)
{
	if (!channels)
		return;
	for (unsigned k = 0; k < frames; k++, samples += channels) {
		if (r->count == MIC_RING) {     /* nobody took them: the oldest go */
			r->head = (r->head + 1) % MIC_RING;
			r->count--;
			r->dropped++;
		}
		int16_t *f = r->f[(r->head + r->count) % MIC_RING];
		f[0] = samples[0];
		f[1] = channels > 1 ? samples[1] : samples[0];
		r->count++;
	}
}

bool mic_ring_get(struct mic_ring *r, int16_t f[2])
{
	if (!r->count)
		return false;
	f[0] = r->f[r->head][0];
	f[1] = r->f[r->head][1];
	r->head = (r->head + 1) % MIC_RING;
	r->count--;
	return true;
}

void mic_stream_init(struct mic_stream *s, unsigned rate, unsigned lead, bool paced)
{
	memset(s, 0, sizeof *s);
	s->rate = rate;
	s->lead = lead;
	s->paced = paced;
	s->level = 2.0 * lead;
}

/*
 * A paced stream's device and the guest's clock may differ by a fraction of
 * a percent, so frames would pile up in the ring or run short: they're
 * taken up to 0.5% faster or slower as the ring, smoothed, is fuller or
 * emptier than twice the lead (as audio.c paces sound output).
 */
#define MAX_SKEW 0.005
#define SKEW_PER_SECOND 0.1             /* skew per second of difference */
#define LEVEL_TAKES 32.0                /* the level is smoothed over about this many takes */
#define ONE (1ull << 32)

bool mic_stream_take(struct mic_stream *s, int16_t *out, unsigned count, unsigned rate)
{
	struct mic_ring *r = &s->ring;
	memset(out, 0, (size_t)count * 2 * sizeof *out);
	if (!count || !rate || !s->rate)
		return false;
	if (!s->taking) {
		if (r->count < (s->lead > 2 ? s->lead : 2))
			return false;
		if (s->paced) {         /* (a backlog from before isn't kept waiting: it would be a delay) */
			int16_t f[2];
			while (r->count > 2 * s->lead && mic_ring_get(r, f))
				;
			s->level = r->count;
		}
		mic_ring_get(r, s->prev);
		mic_ring_get(r, s->next);
		s->phase = 0;
		s->taking = true;
	}
	uint64_t step = ((uint64_t)s->rate << 32) / rate;
	if (s->paced) {
		s->level += ((double)r->count - s->level) / LEVEL_TAKES;
		double skew = (s->level - 2.0 * s->lead) / s->rate * SKEW_PER_SECOND;
		skew = skew > MAX_SKEW ? MAX_SKEW : skew < -MAX_SKEW ? -MAX_SKEW : skew;
		step = (uint64_t)((double)step * (1 + skew) + 0.5);
	}
	for (unsigned k = 0; k < count; k++) {
		while (s->phase >= ONE) {
			s->prev[0] = s->next[0];
			s->prev[1] = s->next[1];
			if (!mic_ring_get(r, s->next)) {        /* ran dry: silence, until there's a lead again */
				s->taking = false;
				s->gaps++;
				return k > 0;
			}
			s->phase -= ONE;
		}
		int64_t w = (int64_t)(s->phase >> 16);
		for (unsigned c = 0; c < 2; c++)
			out[2 * k + c] = (int16_t)(s->prev[c] + (s->next[c] - s->prev[c]) * w / 0x10000);
		s->phase += step;
	}
	return true;
}

static unsigned le16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A sample of `bits` (floating point if `fp`), as 16 bits */
static int16_t sample16(const uint8_t *p, unsigned bits, bool fp)
{
	if (fp) {
		uint32_t u = le32(p);
		float v;
		memcpy(&v, &u, sizeof v);
		v = v != v ? 0 : v > 1 ? 1 : v < -1 ? -1 : v;
		return (int16_t)(v * 32767);
	}
	switch (bits) {
	case 8:
		return (int16_t)((p[0] - 128) * 256);
	case 16:
		return (int16_t)le16(p);
	case 24:
		return (int16_t)(p[1] | p[2] << 8);
	default:
		return (int16_t)(p[2] | p[3] << 8);
	}
}

#define WAV_MAX (1u << 30)              /* (bytes) */

int mic_wav_load(const char *path, struct mic_wav *w, char *why, size_t len)
{
	memset(w, 0, sizeof *w);
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(why, len, "%s", strerror(errno));
		return -1;
	}
	uint8_t *b = NULL;
	long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
	if (size >= 12 && size <= (long)WAV_MAX && fseek(f, 0, SEEK_SET) == 0 && (b = malloc((size_t)size)) &&
	    fread(b, 1, (size_t)size, f) != (size_t)size) {
		free(b);
		b = NULL;
	}
	fclose(f);
	if (!b || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) {
		snprintf(why, len, "not a WAV file");
		free(b);
		return -1;
	}
	const uint8_t *fmt = NULL, *data = NULL;
	size_t data_size = 0;
	for (size_t at = 12; at + 8 <= (size_t)size;) {
		size_t n = le32(b + at + 4), body = at + 8;
		if (n > (size_t)size - body)
			n = (size_t)size - body;        /* (a data chunk whose size was never filled in: to the end) */
		if (!memcmp(b + at, "fmt ", 4) && n >= 16)
			fmt = b + body;
		else if (!memcmp(b + at, "data", 4) && !data) {
			data = b + body;
			data_size = n;
		}
		at = body + n + (n & 1);
	}
	unsigned format = fmt ? le16(fmt) : 0, channels = fmt ? le16(fmt + 2) : 0, bits = fmt ? le16(fmt + 14) : 0;
	if (format == 0xFFFE && fmt && le16(fmt + 16) >= 22)       /* WAVE_FORMAT_EXTENSIBLE: its subformat's */
		format = le16(fmt + 24);
	bool fp = format == 3;
	w->rate = fmt ? le32(fmt + 4) : 0;
	if (!fmt || !data) {
		snprintf(why, len, "a WAV file without %s", !fmt ? "a format" : "sound data");
	} else if ((format != 1 && !fp) || (fp && bits != 32) || (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
		snprintf(why, len, "a WAV file of a kind Blue can't read (format %u, %u bits; it reads PCM of 8 to 32 "
			 "bits, and 32-bit floating point)", format, bits);
	} else if (!channels || w->rate < 1000 || w->rate > 384000) {
		snprintf(why, len, "a WAV file with %u channels at %u Hz", channels, w->rate);
	} else {
		size_t block = (size_t)channels * bits / 8;
		w->frames = data_size / block;
		w->f = malloc((w->frames ? w->frames : 1) * sizeof *w->f);
		if (w->f) {
			for (size_t k = 0; k < w->frames; k++) {
				const uint8_t *p = data + k * block;
				w->f[k][0] = sample16(p, bits, fp);
				w->f[k][1] = channels > 1 ? sample16(p + bits / 8, bits, fp) : w->f[k][0];
			}
			free(b);
			return 0;
		}
		snprintf(why, len, "%s", strerror(ENOMEM));
	}
	free(b);
	w->frames = 0;
	return -1;
}

void mic_wav_free(struct mic_wav *w)
{
	free(w->f);
	memset(w, 0, sizeof *w);
}

void mic_file_start(struct mic_file *f, bool loop)
{
	f->at = 0;
	f->loop = loop;
	mic_stream_init(&f->stream, f->wav.rate, 0, false);
}

bool mic_file_take(struct mic_file *f, int16_t *out, unsigned count, unsigned rate)
{
	struct mic_stream *s = &f->stream;
	if (rate && s->rate) {
		/* the frames these need, from the file: as many as they step over, and the two either side */
		uint64_t step = ((uint64_t)s->rate << 32) / rate;
		uint64_t need = ((s->taking ? s->phase : 0) + (uint64_t)count * step) / ONE + 2;
		while (s->ring.count < need && s->ring.count < MIC_RING) {
			if (f->at >= f->wav.frames) {
				if (!f->loop || !f->wav.frames)
					break;
				f->at = 0;
			}
			size_t n = f->wav.frames - f->at, room = MIC_RING - s->ring.count;
			n = n < need - s->ring.count ? n : need - s->ring.count;
			n = n < room ? n : room;
			mic_ring_put(&s->ring, f->wav.f[f->at], (unsigned)n, 2);
			f->at += n;
		}
	}
	return mic_stream_take(s, out, count, rate);
}

void mic_init(struct mic *m, const struct mic_host *host)
{
	memset(m, 0, sizeof *m);
	m->host = host;
}

static void close_device(struct mic *m)
{
	if (m->dev)
		m->host->close(m->host->ctx, m->dev);
	m->dev = NULL;
}

void mic_close(struct mic *m)
{
	close_device(m);
}

/* What the device recorded since it was last read, into the ring (or nowhere) */
static void drain(struct mic *m, bool keep)
{
	int16_t buf[4096];
	unsigned most = m->channels ? (unsigned)(sizeof buf / sizeof buf[0]) / m->channels : 0, n;
	while (most && (n = m->host->read(m->host->ctx, m->dev, buf, most)) > 0)
		if (keep)
			mic_ring_put(&m->from_host.ring, buf, n, m->channels);
}

void mic_want(struct mic *m, bool recording, uint64_t now)
{
	if (!m->host || m->off || m->file) {
		close_device(m);
		return;
	}
	if (!recording) {
		m->tried = false;
		if (m->dev && now - m->wanted_ns >= MIC_CLOSE_NS)
			close_device(m);
		else if (m->dev)
			drain(m, false);
		return;
	}
	m->wanted_ns = now;
	if (m->dev || (m->tried && now - m->tried_ns < MIC_RETRY_NS))
		return;
	m->tried = true;
	m->tried_ns = now;
	unsigned rate = 0, channels = 0;
	m->dev = m->host->open(m->host->ctx, &rate, &channels);
	if (m->dev && (!rate || !channels)) {
		close_device(m);
		return;
	}
	if (m->dev) {
		m->channels = channels;
		m->opens++;
		mic_stream_init(&m->from_host, rate, MIC_LEAD, true);
	}
}

bool mic_open(const struct mic *m)
{
	return m->dev != NULL;
}

bool mic_take(struct mic *m, int16_t *out, unsigned count, unsigned rate)
{
	if (!m->off && m->file)
		return mic_file_take(m->file, out, count, rate);
	if (m->off || !m->dev) {
		memset(out, 0, (size_t)count * 2 * sizeof *out);
		return false;
	}
	drain(m, true);
	return mic_stream_take(&m->from_host, out, count, rate);
}
