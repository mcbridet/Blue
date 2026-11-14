#include "cdaudio.h"

#include <string.h>

#define RAW_SECTORS (sizeof ((struct cdaudio *)0)->raw / CD_SECTOR)

void cdaudio_init(struct cdaudio *p, uint64_t hz)
{
	memset(p, 0, sizeof *p);
	p->hz = hz;
	p->state = CDAUDIO_IDLE;
	p->volume[0] = p->volume[1] = 255;
	p->mode = CDAUDIO_STEREO;
}

static void push(struct cdaudio *p, int l, int r)
{
	if (p->count == CDAUDIO_FIFO) {         /* nobody takes them: the oldest go */
		p->head = (p->head + 1) % CDAUDIO_FIFO;
		p->count--;
		p->dropped++;
	}
	int16_t *f = p->fifo[(p->head + p->count) % CDAUDIO_FIFO];
	f[0] = (int16_t)l;
	f[1] = (int16_t)r;
	p->count++;
}

/* A channel of the drive's output: the inputs the play mode sends it (both: their mean), at its volume */
static int channel(int l, int r, bool from_l, bool from_r, uint8_t volume)
{
	int v = from_l && from_r ? (l + r) / 2 : from_l ? l : from_r ? r : 0;
	return v * volume / 255;
}

/* A sector's bytes, read ahead with the next ones */
static const uint8_t *sector(struct cdaudio *p, uint32_t lba)
{
	if (lba - p->raw_lba >= p->raw_sectors) {
		p->raw_lba = lba;
		p->raw_sectors = RAW_SECTORS;
		if (!p->disc || cdimage_read_raw(p->disc, lba, RAW_SECTORS, p->raw))
			memset(p->raw, 0, sizeof p->raw);
	}
	return p->raw + (size_t)(lba - p->raw_lba) * CD_SECTOR;
}

/* Plays `count` frames from frame `at` on, into the FIFO (what's not audio is silence) */
static void render(struct cdaudio *p, uint32_t at, uint64_t count)
{
	p->frames += count;
	if (count > CDAUDIO_FIFO) {             /* (more than it holds: only the last are kept) */
		p->dropped += count - CDAUDIO_FIFO;
		at += (uint32_t)(count - CDAUDIO_FIFO);
		count = CDAUDIO_FIFO;
	}
	while (count) {
		uint32_t lba = at / CD_FRAMES, off = at % CD_FRAMES;
		const uint8_t *s = sector(p, lba);
		bool audio = p->disc && cdimage_audio(p->disc, lba);
		for (; off < CD_FRAMES && count; off++, at++, count--) {
			const uint8_t *b = s + 4 * off;
			int l = audio ? (int16_t)(b[0] | b[1] << 8) : 0, r = audio ? (int16_t)(b[2] | b[3] << 8) : 0;
			push(p, channel(l, r, p->mode & 8, p->mode & 4, p->volume[0]),
			     channel(l, r, p->mode & 2, p->mode & 1, p->volume[1]));
		}
	}
}

/* Plays on for `n` frames of time */
static void play_for(struct cdaudio *p, uint64_t n)
{
	while (n && p->state == CDAUDIO_PLAYING) {
		const struct cdaudio_range *r = &p->range[p->current];
		uint32_t end = r->end * CD_FRAMES;
		if (p->scan) {
			uint32_t bit_end = p->scan_from + CDAUDIO_SCAN_SECTORS * CD_FRAMES;
			if (bit_end > end)
				bit_end = end;
			if (p->frame < bit_end) {
				uint64_t take = n < bit_end - p->frame ? n : bit_end - p->frame;
				render(p, p->frame, take);
				p->frame += (uint32_t)take;
				n -= take;
				continue;
			}
			uint32_t jump = CDAUDIO_SCAN_SPEED * CDAUDIO_SCAN_SECTORS * CD_FRAMES;
			if (bit_end >= end) {
				p->scan = 0;
				p->state = CDAUDIO_DONE;
			} else if (p->scan < 0 && p->scan_from < jump) {
				p->scan = 0;    /* back at the start: plays on from there */
				p->frame = 0;
			} else {
				p->scan_from = p->scan > 0 ? p->scan_from + jump : p->scan_from - jump;
				p->frame = p->scan_from < end ? p->scan_from : end;
			}
			continue;
		}
		if (p->frame >= end) {
			if (++p->current >= p->ranges) {
				if (!p->repeat) {
					p->current--;
					p->state = CDAUDIO_DONE;
					break;
				}
				p->current = 0;
			}
			p->frame = p->range[p->current].start * CD_FRAMES;
			continue;
		}
		uint64_t take = n < end - p->frame ? n : end - p->frame;
		render(p, p->frame, take);
		p->frame += (uint32_t)take;
		n -= take;
	}
}

void cdaudio_run(struct cdaudio *p, uint64_t tb)
{
	if (tb <= p->tb)
		return;
	uint64_t elapsed = tb - p->tb;
	p->tb = tb;
	if (p->state != CDAUDIO_PLAYING) {
		p->rem = 0;
		return;
	}
	/* (in pieces, so that a long time can't overflow) */
	while (elapsed) {
		uint64_t dt = elapsed < p->hz ? elapsed : p->hz;
		uint64_t ticks = dt * CD_RATE + p->rem;
		p->rem = ticks % p->hz;
		elapsed -= dt;
		play_for(p, ticks / p->hz);
	}
}

void cdaudio_disc(struct cdaudio *p, const struct cdimage *disc)
{
	p->disc = disc;
	p->state = CDAUDIO_IDLE;
	p->scan = 0;
	p->ranges = p->current = 0;
	p->frame = 0;
	p->count = 0;
	p->taking = false;
	p->raw_sectors = 0;
}

static void start(struct cdaudio *p, uint64_t tb)
{
	p->tb = tb;
	p->rem = 0;
}

void cdaudio_play(struct cdaudio *p, uint64_t tb, const struct cdaudio_range *range, unsigned n, unsigned first,
		  uint32_t at, bool repeat, bool hold)
{
	cdaudio_run(p, tb);
	if (n > CD_MAX_TRACKS)
		n = CD_MAX_TRACKS;
	memcpy(p->range, range, n * sizeof *range);
	p->ranges = n;
	p->current = first < n ? first : 0;
	p->repeat = repeat;
	p->scan = 0;
	p->frame = n ? at * CD_FRAMES : 0;
	p->state = !n ? CDAUDIO_IDLE : hold ? CDAUDIO_PAUSED : CDAUDIO_PLAYING;
	start(p, tb);
}

bool cdaudio_pause(struct cdaudio *p, uint64_t tb, bool pause)
{
	cdaudio_run(p, tb);
	if (p->state != CDAUDIO_PLAYING && p->state != CDAUDIO_PAUSED)
		return false;
	p->scan = 0;
	p->state = pause ? CDAUDIO_PAUSED : CDAUDIO_PLAYING;
	start(p, tb);
	return true;
}

void cdaudio_stop(struct cdaudio *p, uint64_t tb)
{
	cdaudio_run(p, tb);
	p->scan = 0;
	p->state = CDAUDIO_IDLE;
}

void cdaudio_stop_at(struct cdaudio *p, uint64_t tb, uint32_t lba)
{
	cdaudio_run(p, tb);
	if (!p->ranges)
		return;
	struct cdaudio_range *r = &p->range[p->current];
	r->end = lba;
	if (r->start > lba)
		r->start = lba;
	p->ranges = p->current + 1;     /* (nothing after it) */
	p->repeat = false;
	if (p->frame >= lba * CD_FRAMES && (p->state == CDAUDIO_PLAYING || p->state == CDAUDIO_PAUSED)) {
		p->scan = 0;
		p->state = CDAUDIO_DONE;
	}
}

void cdaudio_scan(struct cdaudio *p, uint64_t tb, uint32_t lba, int direction)
{
	cdaudio_run(p, tb);
	uint32_t end = p->disc ? p->disc->leadout : 0;
	if (p->ranges && lba < p->range[p->current].end)
		end = p->range[p->current].end;
	p->range[0] = (struct cdaudio_range){ lba, end };
	p->ranges = 1;
	p->current = 0;
	p->repeat = false;
	p->frame = p->scan_from = lba * CD_FRAMES;
	p->scan = direction < 0 ? -1 : 1;
	p->state = lba < end ? CDAUDIO_PLAYING : CDAUDIO_DONE;
	start(p, tb);
}

enum cdaudio_state cdaudio_status(struct cdaudio *p, uint64_t tb, uint32_t *lba)
{
	cdaudio_run(p, tb);
	if (lba)
		*lba = p->frame / CD_FRAMES;
	return p->state;
}

void cdaudio_volume(struct cdaudio *p, uint64_t tb, uint8_t left, uint8_t right)
{
	cdaudio_run(p, tb);
	p->volume[0] = left;
	p->volume[1] = right;
}

void cdaudio_mode(struct cdaudio *p, uint64_t tb, uint8_t mode)
{
	cdaudio_run(p, tb);
	p->mode = mode & 15;
}

bool cdaudio_active(const struct cdaudio *p)
{
	return p->state == CDAUDIO_PLAYING || p->count;
}

/* The next frame from the FIFO (or the last one again, if it's empty) */
static void pop(struct cdaudio *p, int16_t f[2])
{
	if (p->count) {
		memcpy(p->next, p->fifo[p->head], sizeof p->next);
		p->head = (p->head + 1) % CDAUDIO_FIFO;
		p->count--;
	} else {
		p->taking = false;
	}
	f[0] = p->next[0];
	f[1] = p->next[1];
}

bool cdaudio_pull(struct cdaudio *p, int16_t *frames, unsigned count, unsigned rate)
{
	if (!cdaudio_active(p) || !rate)
		return false;
	if (!p->taking && p->count < CDAUDIO_LEAD && p->state == CDAUDIO_PLAYING) {
		memset(frames, 0, (size_t)count * 2 * sizeof *frames);
		return true;
	}
	p->taking = true;
	if (rate == CD_RATE) {
		for (unsigned k = 0; k < count; k++)
			pop(p, frames + 2 * k);
		return true;
	}
	/* another rate: between the frames on either side, in a straight line */
	uint32_t step = (uint32_t)(((uint64_t)CD_RATE << 16) / rate);
	for (unsigned k = 0; k < count; k++) {
		while (p->phase >= 0x10000u) {
			memcpy(p->prev, p->next, sizeof p->prev);
			int16_t f[2];
			pop(p, f);
			p->phase -= 0x10000u;
		}
		for (int c = 0; c < 2; c++)
			frames[2 * k + c] = (int16_t)(p->prev[c] + (int64_t)(p->next[c] - p->prev[c]) * p->phase / 0x10000);
		p->phase += step;
	}
	return true;
}
