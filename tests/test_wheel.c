/*
 * The scroll wheel's lines (wheel.h): a mouse wheel's notches and a
 * trackpad's small movements gathered into whole lines, a turn the other
 * way starting afresh, at most WHEEL_MAX waiting each way, taken by Mac
 * OS, or the arrow keys' (without Blue Enhancements, when it couldn't
 * scroll by them, or when they've waited too long), and the key that wakes
 * the program in front once for them.
 */
#include <string.h>

#include "../src/wheel.h"
#include "test.h"

static void test_lines(void)
{
	struct wheel w;
	int32_t dx, dy;
	memset(&w, 0, sizeof w);
	/* a mouse wheel: a notch a line */
	wheel_turn(&w, 0, -3 * WHEEL_LINE, 100, true);
	CHECK(w.lines[0] == 0 && w.lines[1] == -3 && w.since == 100);
	wheel_turn(&w, WHEEL_LINE, 0, 200, true);
	CHECK(w.lines[0] == 1 && w.lines[1] == -3 && w.since == 100);
	CHECK(wheel_take(&w, WHEEL_MAX, &dx, &dy, 300) && dx == 1 && dy == -3);
	CHECK(!wheel_take(&w, WHEEL_MAX, &dx, &dy, 300) && !dx && !dy);

	/* a trackpad: tenths of a line, 25 of them two and a half lines, the half kept for more the same way */
	for (int k = 0; k < 25; k++)
		wheel_turn(&w, 0, WHEEL_LINE / 10, 400 + (unsigned)k, true);
	CHECK(w.lines[1] == 2 && w.part[1] == WHEEL_LINE / 2 && w.since == 409);
	for (int k = 0; k < 5; k++)
		wheel_turn(&w, 0, WHEEL_LINE / 10, 500, true);
	CHECK(w.lines[1] == 3 && w.part[1] == 0);
	/* ... the other way: what was gathered the first way goes */
	wheel_turn(&w, 0, WHEEL_LINE / 10 * 4, 600, true);
	wheel_turn(&w, 0, -WHEEL_LINE / 2, 600, true);
	CHECK(w.lines[1] == 0 && w.part[1] == -WHEEL_LINE / 2);
	wheel_turn(&w, 0, -WHEEL_LINE, 600, true);
	CHECK(w.lines[1] == -1 && w.part[1] == -WHEEL_LINE / 2);

	/* a quick swipe: no more than WHEEL_MAX waiting; taken a few at a time, the rest waiting afresh */
	wheel_turn(&w, 0, -100 * WHEEL_LINE, 700, true);
	CHECK_EQ(w.lines[1], -WHEEL_MAX);
	CHECK(wheel_take(&w, 4, &dx, &dy, 800) && dy == -4 && w.lines[1] == -(WHEEL_MAX - 4) && w.since == 800);
	/* ... put back (a click the program didn't take): waiting again */
	wheel_again(&w, 0, -2);
	CHECK_EQ(w.lines[1], -(WHEEL_MAX - 2));
	wheel_again(&w, 0, -10);
	CHECK_EQ(w.lines[1], -WHEEL_MAX);

	/* waiting too long: the arrow keys */
	wheel_expire(&w, 899, 100);
	CHECK_EQ(w.lines[1], -WHEEL_MAX);
	wheel_expire(&w, 900, 100);
	CHECK(w.lines[1] == 0 && w.keys[1] == -WHEEL_MAX);
	CHECK(wheel_keys(&w, &dx, &dy) && dx == 0 && dy == -WHEEL_MAX && w.pressed == WHEEL_MAX);
	CHECK(!wheel_keys(&w, &dx, &dy));

	/* not to be scrolled (no Blue Enhancements): the arrow keys at once */
	memset(&w, 0, sizeof w);
	wheel_turn(&w, -2 * WHEEL_LINE, WHEEL_LINE / 2, 0, false);
	wheel_turn(&w, 0, WHEEL_LINE / 2, 0, false);
	CHECK(!w.lines[0] && !w.lines[1]);
	CHECK(wheel_keys(&w, &dx, &dy) && dx == -2 && dy == 1);
	/* ... and lines Mac OS couldn't scroll by */
	wheel_unscrolled(&w, 3, -1);
	CHECK(wheel_keys(&w, &dx, &dy) && dx == 3 && dy == -1 && w.pressed == 7);
	wheel_unscrolled(&w, 0, 40);
	CHECK(wheel_keys(&w, &dx, &dy) && dy == WHEEL_MAX);
}

/* The key that wakes the program in front: once for lines waiting, until it's had it (or they've gone) */
static void test_wake(void)
{
	struct wheel w;
	memset(&w, 0, sizeof w);
	CHECK(!wheel_wake(&w));
	wheel_turn(&w, 0, WHEEL_LINE / 2, 0, true);
	CHECK(!wheel_wake(&w));                                 /* (half a line: nothing yet) */
	wheel_turn(&w, 0, WHEEL_LINE / 2, 0, true);
	CHECK(wheel_wake(&w) && !wheel_wake(&w));
	wheel_turn(&w, 0, WHEEL_LINE, 0, true);
	CHECK(!wheel_wake(&w));                                 /* (the program hasn't had it yet) */
	wheel_woken(&w);
	CHECK(wheel_wake(&w));                                  /* (lines still waiting: woken again) */
	int32_t dx, dy;
	wheel_take(&w, WHEEL_MAX, &dx, &dy, 0);
	wheel_woken(&w);
	CHECK(!wheel_wake(&w));
	wheel_turn(&w, 0, WHEEL_LINE, 0, true);
	CHECK(wheel_wake(&w));
	wheel_expire(&w, 100, 50);                              /* (the program never looked) */
	CHECK(!w.woke && !wheel_wake(&w));
	wheel_turn(&w, 0, WHEEL_LINE, 200, true);
	CHECK(wheel_wake(&w));
}

void test_wheel(void)
{
	test_lines();
	test_wake();
}
