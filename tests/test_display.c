/*
 * The window's layout of the screen: where it goes, how it's scaled, and
 * which screen pixel a window position is over; and its icon.
 */
#include <stdlib.h>

#include "../src/display.h"
#include "../src/icon.h"
#include "test.h"

#define CHECK_VIEW(v, vx, vy, vw, vh, k)        \
	do {                                    \
		CHECK_EQ((v).x, vx);            \
		CHECK_EQ((v).y, vy);            \
		CHECK_EQ((v).w, vw);            \
		CHECK_EQ((v).h, vh);            \
		CHECK_EQ((v).prescale, k);      \
	} while (0)

static void test_view(void)
{
	/* the same size, and whole multiples: drawn directly */
	CHECK_VIEW(display_view(800, 600, 800, 600, false), 0, 0, 800, 600, 0);
	CHECK_VIEW(display_view(800, 600, 1600, 1200, false), 0, 0, 1600, 1200, 0);
	CHECK_VIEW(display_view(800, 600, 1600, 1300, false), 0, 50, 1600, 1200, 0);
	/* a pixel or two short of filling the window is worth an exact multiple */
	CHECK_VIEW(display_view(800, 600, 1601, 1201, false), 0, 0, 1600, 1200, 0);
	CHECK_VIEW(display_view(800, 600, 1602, 1202, false), 1, 1, 1600, 1200, 0);
	/* otherwise filled, keeping the shape: drawn at the multiple above, then filtered */
	CHECK_VIEW(display_view(800, 600, 1360, 1020, false), 0, 0, 1360, 1020, 2);    /* 1.7x */
	CHECK_VIEW(display_view(800, 600, 3024, 1840, false), 285, 0, 2453, 1840, 4);
	CHECK_VIEW(display_view(640, 480, 1000, 1000, false), 0, 125, 1000, 750, 2);
	/* windows smaller than the screen: filtered down */
	CHECK_VIEW(display_view(800, 600, 700, 500, false), 16, 0, 667, 500, 1);
	CHECK_VIEW(display_view(800, 600, 700, 500, true), 16, 0, 667, 500, 1);
	/* integer scaling: the largest whole multiple, centred */
	CHECK_VIEW(display_view(800, 600, 3024, 1840, true), 312, 20, 2400, 1800, 0);
	CHECK_VIEW(display_view(800, 600, 1360, 1020, true), 280, 210, 800, 600, 0);
	/* nothing to draw into */
	CHECK_VIEW(display_view(800, 600, 0, 0, false), 0, 0, 0, 0, 0);
}

static void test_points(void)
{
	/* a screen sized to the window in points, at 1.7 pixels each: positions are its pixels */
	struct display_view v = display_view(800, 600, 1360, 1020, false);
	int x, y, wrong = 0;
	for (int p = 0; p < 800; p++) {
		display_view_point(&v, (p + 0.5) * 1360 / 800, (p % 600 + 0.5) * 1020 / 600, &x, &y);
		wrong += x != p || y != p % 600;
	}
	CHECK_EQ(wrong, 0);

	/* borders are off the screen */
	v = display_view(800, 600, 3024, 1840, true);
	display_view_point(&v, 311.5, 19.5, &x, &y);
	CHECK(x < 0 && y < 0);
	display_view_point(&v, 312, 20, &x, &y);
	CHECK_EQ(x, 0);
	CHECK_EQ(y, 0);
	display_view_point(&v, 312 + 2399.5, 20 + 1799.5, &x, &y);
	CHECK_EQ(x, 799);
	CHECK_EQ(y, 599);
	display_view_point(&v, 312 + 2400, 20 + 1800, &x, &y);
	CHECK_EQ(x, 800);
	CHECK_EQ(y, 600);
}

/* The icon unpacks: clear around the tile, opaque inside it */
static void test_icon(void)
{
	uint32_t *icon = malloc(ICON_SIZE * ICON_SIZE * sizeof *icon);
	CHECK(icon && icon_argb(icon));
	if (!icon)
		return;
	CHECK_EQ(icon[0] >> 24, 0);
	CHECK_EQ(icon[ICON_SIZE * ICON_SIZE - 1] >> 24, 0);
	CHECK_EQ(icon[ICON_SIZE / 2 * ICON_SIZE + ICON_SIZE / 2] >> 24, 0xFF);
	CHECK((icon[ICON_SIZE * 40 + ICON_SIZE / 2] & 0xFFF0F0F0u) == 0xFFF0F0F0u);  /* (the tile's top: white) */
	free(icon);
}

void test_display(void)
{
	test_view();
	test_points();
	test_icon();
}
