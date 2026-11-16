#include "wheel.h"

static int32_t clamp(int64_t v)
{
	return v > WHEEL_MAX ? WHEEL_MAX : v < -WHEEL_MAX ? -WHEEL_MAX : (int32_t)v;
}

void wheel_turn(struct wheel *w, int32_t dx, int32_t dy, uint64_t now, bool scroll)
{
	const int32_t d[2] = { dx, dy };
	for (unsigned k = 0; k < 2; k++) {
		if (!d[k])
			continue;
		/* (the other way: what was gathered the first way goes) */
		if ((int64_t)d[k] * w->part[k] < 0 || (int64_t)d[k] * w->lines[k] < 0 || (int64_t)d[k] * w->keys[k] < 0)
			w->part[k] = w->lines[k] = w->keys[k] = 0;
		int64_t part = (int64_t)w->part[k] + d[k], whole = part / WHEEL_LINE;
		w->part[k] = (int32_t)(part - whole * WHEEL_LINE);
		if (!whole)
			continue;
		if (!scroll) {
			w->keys[k] = clamp(w->keys[k] + whole);
			continue;
		}
		if (!w->lines[0] && !w->lines[1])
			w->since = now;
		w->lines[k] = clamp(w->lines[k] + whole);
	}
}

bool wheel_wake(struct wheel *w)
{
	if (w->woke || (!w->lines[0] && !w->lines[1]))
		return false;
	w->woke = true;
	return true;
}

void wheel_woken(struct wheel *w)
{
	w->woke = false;
}

void wheel_again(struct wheel *w, int32_t dx, int32_t dy)
{
	w->lines[0] = clamp((int64_t)w->lines[0] + dx);
	w->lines[1] = clamp((int64_t)w->lines[1] + dy);
}

bool wheel_take(struct wheel *w, int32_t max, int32_t *dx, int32_t *dy, uint64_t now)
{
	int32_t *out[2] = { dx, dy };
	for (unsigned k = 0; k < 2; k++) {
		int32_t n = w->lines[k] > max ? max : w->lines[k] < -max ? -max : w->lines[k];
		*out[k] = n;
		w->lines[k] -= n;
	}
	w->since = now;                 /* (the rest wait afresh) */
	return *dx || *dy;
}

void wheel_unscrolled(struct wheel *w, int32_t dx, int32_t dy)
{
	w->keys[0] = clamp((int64_t)w->keys[0] + dx);
	w->keys[1] = clamp((int64_t)w->keys[1] + dy);
}

void wheel_expire(struct wheel *w, uint64_t now, uint64_t wait)
{
	if ((w->lines[0] || w->lines[1]) && now - w->since >= wait) {
		wheel_unscrolled(w, w->lines[0], w->lines[1]);
		w->lines[0] = w->lines[1] = 0;
		w->woke = false;
	}
}

bool wheel_keys(struct wheel *w, int32_t *dx, int32_t *dy)
{
	*dx = w->keys[0];
	*dy = w->keys[1];
	w->keys[0] = w->keys[1] = 0;
	w->pressed += (unsigned long)(*dx < 0 ? -*dx : *dx) + (unsigned long)(*dy < 0 ? -*dy : *dy);
	return *dx || *dy;
}
