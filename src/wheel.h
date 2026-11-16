/*
 * The scroll wheel (and trackpads' scrolling): the host's turns gathered
 * into whole lines, for Blue Enhancements to scroll the window under the
 * pointer by, in the program in front (tools.h), or else to press the
 * arrow keys with, as Blue did before. A turn's parts of a line wait for
 * more, the same way, so that a trackpad's many small ones come to a line
 * now and then; a turn the other way starts afresh. Lines wait a moment
 * for Mac OS's program in front to take them (it does as it next looks for
 * an event: a key no keyboard of the time had wakes it, which it never
 * gets), at most WHEEL_MAX each way: more than that are dropped, so a
 * quick swipe can't flood Mac OS with them.
 */
#ifndef BLUE_WHEEL_H
#define BLUE_WHEEL_H

#include <stdbool.h>
#include <stdint.h>

#define WHEEL_LINE 120                  /* a line, in the host's units: a mouse wheel's notch */
#define WHEEL_MAX 16                    /* lines waiting each way, at most */

struct wheel {
	bool enabled;                   /* Blue Enhancements is to scroll (else the arrow keys, always) */
	int32_t part[2];                /* a line's parts not yet whole, in 1/WHEEL_LINE (x: right, y: up) */
	int32_t lines[2];               /* whole lines waiting for Mac OS ... */
	uint64_t since;                 /* ... since then (the clock wheel_turn() was given) */
	bool woke;                      /* ... a key has woken the program in front for them */
	int32_t keys[2];                /* ... and lines for the arrow keys */
	unsigned long scrolled, pressed;        /* lines scrolled, and lines as the arrow keys */
};

/* A turn (right and up, in 1/WHEEL_LINE lines) at `now`, for Mac OS if `scroll` (else the arrow keys) */
void wheel_turn(struct wheel *w, int32_t dx, int32_t dy, uint64_t now, bool scroll);

/*
 * Whether the program in front is to be woken (lines wait, and no key has
 * woken it since: then one has); Blue Enhancements takes the key away
 * (wheel_woken())
 */
bool wheel_wake(struct wheel *w);
void wheel_woken(struct wheel *w);
#define WHEEL_WAKE_KEY 0x6A             /* F16 (no keyboard of the time had it): an ADB key, and Mac OS's key code */

/* Lines taken but not scrolled by after all, to wait again */
void wheel_again(struct wheel *w, int32_t dx, int32_t dy);

/* Lines waiting for Mac OS: up to `max` each way taken, at `now` (true if any) */
bool wheel_take(struct wheel *w, int32_t max, int32_t *dx, int32_t *dy, uint64_t now);

/* ... and those Mac OS took but couldn't scroll by (or `wait` past `since`, at `now`): for the arrow keys */
void wheel_unscrolled(struct wheel *w, int32_t dx, int32_t dy);
void wheel_expire(struct wheel *w, uint64_t now, uint64_t wait);

/* Lines for the arrow keys, taken (true if any) */
bool wheel_keys(struct wheel *w, int32_t *dx, int32_t *dy);

#endif
