/*
 * The window's layout of the screen: where it goes, how it's scaled, and
 * which screen pixel a window position is over; its scale (Auto, from
 * displays' densities) and the screen a window asks for; its icon and font.
 */
#include <math.h>
#include <stdlib.h>

#include "../src/be.h"
#include "../src/density.h"
#include "../src/display.h"
#include "../src/font.h"
#include "../src/icon.h"
#include "../src/video.h"
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
	CHECK_VIEW(display_view(800, 600, 800, 600, false, 1), 0, 0, 800, 600, 0);
	CHECK_VIEW(display_view(800, 600, 1600, 1200, false, 1), 0, 0, 1600, 1200, 0);
	CHECK_VIEW(display_view(800, 600, 1600, 1300, false, 1), 0, 50, 1600, 1200, 0);
	/* a pixel or two short of filling the window is worth an exact multiple */
	CHECK_VIEW(display_view(800, 600, 1601, 1201, false, 1), 0, 0, 1600, 1200, 0);
	CHECK_VIEW(display_view(800, 600, 1602, 1202, false, 1), 1, 1, 1600, 1200, 0);
	/* otherwise filled, keeping the shape: drawn at the multiple above, then filtered */
	CHECK_VIEW(display_view(800, 600, 1360, 1020, false, 1), 0, 0, 1360, 1020, 2);    /* 1.7x */
	CHECK_VIEW(display_view(800, 600, 3024, 1840, false, 1), 285, 0, 2453, 1840, 4);
	CHECK_VIEW(display_view(640, 480, 1000, 1000, false, 1), 0, 125, 1000, 750, 2);
	/* windows smaller than the screen: filtered down */
	CHECK_VIEW(display_view(800, 600, 700, 500, false, 1), 16, 0, 667, 500, 1);
	CHECK_VIEW(display_view(800, 600, 700, 500, true, 1), 16, 0, 667, 500, 1);
	/* integer scaling: the largest whole multiple, centred */
	CHECK_VIEW(display_view(800, 600, 3024, 1840, true, 1), 312, 20, 2400, 1800, 0);
	CHECK_VIEW(display_view(800, 600, 1360, 1020, true, 1), 280, 210, 800, 600, 0);
	/* nothing to draw into */
	CHECK_VIEW(display_view(800, 600, 0, 0, false, 1), 0, 0, 0, 0, 0);

	/* a screen the window's own at its scale: exactly that, what rounding left a border (5K, 3x, the status bar) */
	CHECK_VIEW(display_view(1704, 944, 5120, 2832, false, 3), 4, 0, 5112, 2832, 0);
	CHECK_VIEW(display_view(1920, 1080, 3840, 2160, false, 2), 0, 0, 3840, 2160, 0);
	CHECK_VIEW(display_view(640, 480, 2591, 1923, false, 4), 15, 1, 2560, 1920, 0);
	/* ... not at another scale (the window being resized, or another screen): filled */
	CHECK_VIEW(display_view(640, 480, 2591, 1923, false, 2), 13, 0, 2564, 1923, 5);
	CHECK_VIEW(display_view(1704, 944, 4000, 2832, false, 3), 0, 308, 4000, 2216, 3);
	CHECK_VIEW(display_view(1704, 944, 4000, 2832, true, 3), 296, 472, 3408, 1888, 0);
}

/* Auto's choice: the whole number nearest 80 pixels per inch in proportion, else the display's own scale */
static void test_scale(void)
{
	CHECK_EQ(display_auto_scale(218, 2), 3);        /* a 5K iMac */
	CHECK_EQ(display_auto_scale(163, 1), 2);        /* a 27-inch 4K display */
	CHECK_EQ(display_auto_scale(109, 1), 1);        /* a 27-inch 1440p one */
	CHECK_EQ(display_auto_scale(254, 2), 3);        /* a 14-inch MacBook Pro */
	CHECK_EQ(display_auto_scale(299, 2), 4);        /* a 13-inch MacBook Air looking like 1710 x 1112 */
	CHECK_EQ(display_auto_scale(92, 1), 1);
	CHECK_EQ(display_auto_scale(40, 1), 1);         /* a 55-inch television */
	CHECK_EQ(display_auto_scale(113, 1), 1);        /* (1 and 2 are as far off at 113.1) */
	CHECK_EQ(display_auto_scale(114, 1), 2);
	CHECK_EQ(display_auto_scale(195, 2), 2);        /* (2 and 3 at 196.0) */
	CHECK_EQ(display_auto_scale(197, 2), 3);
	CHECK_EQ(display_auto_scale(600, 2), 4);
	/* not known, or out of reason: the display's own scale, rounded */
	CHECK_EQ(display_auto_scale(0, 1), 1);
	CHECK_EQ(display_auto_scale(0, 2), 2);
	CHECK_EQ(display_auto_scale(0, 1.25), 1);
	CHECK_EQ(display_auto_scale(0, 1.75), 2);
	CHECK_EQ(display_auto_scale(0, 3), 3);
	CHECK_EQ(display_auto_scale(5000, 2), 2);
	CHECK_EQ(display_auto_scale(10, 1), 1);
}

/* The screen a window asks for: its pixels over the scale, then what the driver takes (video_fit_size()) */
static void test_screen_for(void)
{
	int w, h;
	display_screen_for(5120, 2880, 3, &w, &h);      /* 5K, fullscreen */
	CHECK_EQ(w, 1706);
	CHECK_EQ(h, 960);
	unsigned uw = (unsigned)w, uh = (unsigned)h;
	video_fit_size(16u << 20, &uw, &uh);
	CHECK_EQ(uw, 1704);
	CHECK_EQ(uh, 960);
	display_screen_for(5120, 2832, 3, &w, &h);      /* ... with the status bar, at 2 pixels a unit */
	CHECK_EQ(w, 1706);
	CHECK_EQ(h, 944);
	display_screen_for(3840, 2160, 2, &w, &h);      /* 4K */
	CHECK_EQ(w, 1920);
	CHECK_EQ(h, 1080);
	display_screen_for(2560, 1440, 1, &w, &h);
	CHECK_EQ(w, 2560);
	CHECK_EQ(h, 1440);
	display_screen_for(1603, 1201, 2, &w, &h);
	CHECK_EQ(w, 801);
	CHECK_EQ(h, 600);
	uw = (unsigned)w;
	uh = (unsigned)h;
	video_fit_size(16u << 20, &uw, &uh);            /* (whole bytes at 1 bit per pixel) */
	CHECK_EQ(uw, 800);
	CHECK_EQ(uh, 600);
	display_screen_for(1600, 1200, 4, &w, &h);      /* too small a window: the driver's smallest */
	uw = (unsigned)w;
	uh = (unsigned)h;
	video_fit_size(16u << 20, &uw, &uh);
	CHECK_EQ(uw, 512);
	CHECK_EQ(uh, 384);
	display_screen_for(0, -24, 2, &w, &h);
	CHECK_EQ(w, 0);
	CHECK_EQ(h, 0);
	/* 4K at 1x: more than 16 MB holds at 32 bits a pixel, so smaller, keeping its shape */
	uw = 3840;
	uh = 2160;
	video_fit_size(16u << 20, &uw, &uh);
	CHECK(uw % 8 == 0 && (uint64_t)uw * uh * 4 <= 16u << 20 && uw >= 2400);
	CHECK(uh * 16 * 50 > uw * 9 * 49 && uh * 16 * 50 < uw * 9 * 51);         /* (16:9 to 2%) */
}

/* Densities from displays' sizes, and sizes from EDIDs */
static void test_density(void)
{
	CHECK_EQ(lround(density_from_size(5120, 1440, 1193.1, 338.7)), 109);    /* a 49-inch 32:9 display */
	CHECK_EQ(lround(density_from_size(3420, 2224, 290.5, 188.9)), 299);     /* a MacBook Air, scaled */
	CHECK_EQ(lround(density_from_size(5120, 2880, 596.7, 335.6)), 218);     /* a 5K iMac */
	CHECK_EQ(lround(density_from_size(1440, 2560, 597, 336)), 109);         /* turned, its size not */
	CHECK_EQ(density_from_size(1920, 1080, 160, 90), 0);                    /* a television's shape */
	CHECK_EQ(density_from_size(1920, 1080, 600, 600), 0);                   /* pixels far from square */
	CHECK_EQ(density_from_size(1920, 1080, 0, 0), 0);
	CHECK_EQ(density_from_size(0, 0, 600, 340), 0);

	uint8_t edid[128] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
	edid[21] = 60;                                  /* 60 x 34 cm */
	edid[22] = 34;
	uint8_t *t = edid + 54;                         /* 1920 x 1080, 527 x 296 mm */
	t[0] = 0x02;
	t[1] = 0x3A;
	t[2] = 0x80;
	t[4] = 0x70;
	t[5] = 0x38;
	t[7] = 0x40;
	t[12] = 0x0F;
	t[13] = 0x28;
	t[14] = 0x21;
	unsigned w, h;
	double mm_w, mm_h;
	CHECK(density_edid(edid, sizeof edid, &w, &h, &mm_w, &mm_h));
	CHECK_EQ(w, 1920);
	CHECK_EQ(h, 1080);
	CHECK_EQ(mm_w, 527);
	CHECK_EQ(mm_h, 296);
	t[12] = t[13] = t[14] = 0;                      /* no size in the timing: the basic one's */
	CHECK(density_edid(edid, sizeof edid, &w, &h, &mm_w, &mm_h));
	CHECK_EQ(mm_w, 600);
	CHECK_EQ(mm_h, 340);
	edid[21] = 0;                                   /* (its shape only) */
	CHECK(!density_edid(edid, sizeof edid, &w, &h, &mm_w, &mm_h));
	edid[1] = 0;
	CHECK(!density_edid(edid, sizeof edid, &w, &h, &mm_w, &mm_h));
	CHECK(!density_edid(edid, 64, &w, &h, &mm_w, &mm_h));
}

static void test_points(void)
{
	/* a screen sized to the window in points, at 1.7 pixels each: positions are its pixels */
	struct display_view v = display_view(800, 600, 1360, 1020, false, 1);
	int x, y, wrong = 0;
	for (int p = 0; p < 800; p++) {
		display_view_point(&v, (p + 0.5) * 1360 / 800, (p % 600 + 0.5) * 1020 / 600, &x, &y);
		wrong += x != p || y != p % 600;
	}
	CHECK_EQ(wrong, 0);

	/* borders are off the screen */
	v = display_view(800, 600, 3024, 1840, true, 1);
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

/* The font unpacks: a TrueType file, its tables' checksums right (head's whole-file adjustment aside) */
static void test_font(void)
{
	size_t size;
	uint8_t *ttf = font_ttf(&size);
	CHECK(ttf != NULL);
	if (!ttf)
		return;
	CHECK_EQ(be32(ttf), 0x00010000u);
	unsigned tables = be16(ttf + 4), bad = 0;
	CHECK(tables > 8 && 12 + 16 * tables <= size);
	for (unsigned k = 0; k < tables && 12 + 16 * (k + 1) <= size; k++) {
		const uint8_t *t = ttf + 12 + 16 * k;
		uint32_t tag = be32(t), sum = 0, at = be32(t + 8), len = be32(t + 12);
		if (at > size || len > size - at) {
			bad++;
			continue;
		}
		for (uint32_t n = 0; n < len; n += 4)
			for (uint32_t b = 0; b < 4; b++)
				sum += (uint32_t)(n + b < len && !(tag == 0x68656164u && n == 8) ? ttf[at + n + b] : 0)
				       << (24 - 8 * b);
		bad += sum != be32(t + 4);
	}
	CHECK_EQ(bad, 0);
	free(ttf);
}

void test_display(void)
{
	test_view();
	test_points();
	test_scale();
	test_screen_for();
	test_density();
	test_icon();
	test_font();
}
