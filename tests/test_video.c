/*
 * The screen for the window: converting the framebuffer to 32-bit pixels
 * at every depth (the same colours as the RGB screenshots take), keeping
 * the window's copy up to date (comparing it, and watching the
 * framebuffer's pages for writes), and the hardware cursor the driver takes from
 * Mac OS. The display's VBL interrupt: its timing on the timebase, and the
 * driver's handler for it in the node's interrupt set. The 3D engine's
 * frames for the screen, at most one a vertical blank.
 */
#include <stdlib.h>
#include <string.h>

#include "../src/mem.h"
#include "../src/ndrv.h"
#include "../src/video.h"
#include "test.h"

#define TOC 0x10000u
#define FRAME 0x20000u
#define PB 0x30000u
#define PARAM 0x30100u
#define TVECTORS 0x40000u
#define CURSOR_REF 0x50000u
#define FB 0x1000000u
#define FB_SIZE (8u << 20)
#define TIMEBASE 16600000u

/* The driver's imports and entry points, as video.c numbers them */
enum { IMP_IO_COMPLETE, IMP_GET_INT, IMP_INSTALL_INT, IMP_QUEUE_SECONDARY, IMP_PROPERTY_GET, IMP_VSL_NEW_SERVICE,
       IMP_VSL_DO_SERVICE, IMP_VSL_PREPARE_CURSOR, IMPORTS };
enum { ENTRY_DRIVER_IO, ENTRY_VBL_INTERRUPT, ENTRY_VBL_SECONDARY };

static struct mem mem;
static struct video video;

static uint32_t rand_state = 12345;

static uint8_t next_byte(void)
{
	rand_state = rand_state * 1664525u + 1013904223u;
	return (uint8_t)(rand_state >> 24);
}

/* Each depth, at a mode's width and one that isn't a whole number of bytes at 1 bit per pixel */
static void test_convert(void)
{
	struct video *v = &video;
	static const unsigned widths[] = { 640, 1003 };
	video_init(v, &mem, FB, FB_SIZE, TIMEBASE);
	for (int k = 0; k < 256; k++)
		for (int c = 0; c < 3; c++)
			v->clut[k][c] = next_byte();
	for (unsigned w = 0; w < sizeof widths / sizeof widths[0]; w++) {
		v->mode = video_mode_count;     /* a custom mode */
		v->custom[0] = (struct video_mode){ (uint16_t)widths[w], 37, 0 };
		for (unsigned d = 0; d < VIDEO_DEPTHS; d++) {
			v->depth = d;
			unsigned width = video_width(v), height = video_height(v);
			size_t rb = video_row_bytes(v), pitch = width * 4 + 20;
			uint8_t *fb = mem_ptr(&mem, FB);
			for (size_t k = 0; k < rb * height + 8; k++)
				fb[k] = next_byte();
			uint8_t *rgb = malloc((size_t)width * height * 3);
			uint32_t *px = malloc(pitch * height);
			struct video_palette p;
			video_to_rgb(v, rgb);
			video_palette(v, &p);
			/* (in two goes, the second into the middle of the buffer, as bands are) */
			video_convert(&p, fb, rb, width, 5, px, pitch);
			video_convert(&p, fb + 5 * rb, rb, width, height - 5, (uint32_t *)(void *)((uint8_t *)px + 5 * pitch),
				      pitch);
			unsigned wrong = 0;
			for (unsigned y = 0; y < height; y++) {
				for (unsigned x = 0; x < width; x++) {
					uint32_t got = ((uint32_t *)(void *)((uint8_t *)px + y * pitch))[x];
					const uint8_t *want = rgb + ((size_t)y * width + x) * 3;
					wrong += got != (0xFF000000u | (uint32_t)want[0] << 16 | (uint32_t)want[1] << 8 | want[2]);
				}
			}
			if (wrong)
				fprintf(stderr, "convert: %u pixels wrong at %u bits, %u wide\n", wrong, video_bpp(v), width);
			CHECK_EQ(wrong, 0);
			free(rgb);
			free(px);
		}
	}
}

/* Whether rows [from, to) are marked changed, and only they */
static bool marked(const uint8_t *changed, unsigned rows, unsigned from, unsigned to)
{
	for (unsigned y = 0; y < rows; y++)
		if (changed[y] != (y >= from && y < to))
			return false;
	return true;
}

/*
 * The window's copy brought up to date: by comparing, and with the
 * framebuffer's pages watched (writes seen through mem_ok_w(), as the
 * interpreter's and the JIT's checked stores are, and mem_invalidate(), as
 * the host's are), a host page at a time
 */
static void test_copy_changes(void)
{
	struct video *v = &video;
	video_init(v, &mem, FB, FB_SIZE, TIMEBASE);     /* (800 x 600, 8 bits: 600 rows of 800 bytes) */
	enum { ROWS = 600, ROW = 800 };
	size_t unit = (size_t)mem.host_pages * MEM_PAGE_SIZE;
	uint8_t *fb = mem_ptr(&mem, FB), *copy = malloc(ROWS * ROW), changed[ROWS];
	for (size_t k = 0; k < ROWS * ROW; k++)
		fb[k] = next_byte();
	for (int watch = 0; watch < 2; watch++) {
		memset(changed, 0, sizeof changed);
		CHECK_EQ(video_copy_changes(v, copy, changed, ROWS, true, watch), ROWS);
		CHECK(!memcmp(copy, fb, ROWS * ROW));
		CHECK_EQ(mem_watched(&mem, FB), watch);
		CHECK_EQ(mem_watched(&mem, FB + ROWS * ROW - 1), watch);
		memset(changed, 0, sizeof changed);
		CHECK_EQ(video_copy_changes(v, copy, changed, ROWS, false, watch), 0);

		/* a store (into the second host page): the rows of that page */
		uint32_t at = FB + (uint32_t)unit + 100;
		CHECK(mem_ok_w(&mem, at, 1));
		CHECK(!mem_watched(&mem, at));
		CHECK_EQ(mem_watched(&mem, at - unit), watch);
		mem_w8(&mem, at, (uint8_t)~mem_r8(&mem, at));
		unsigned from = (unsigned)(unit / ROW), to = (unsigned)((2 * unit - 1) / ROW) + 1;
		CHECK_EQ(video_copy_changes(v, copy, changed, ROWS, false, watch), to - from);
		CHECK(marked(changed, ROWS, from, to));
		CHECK(!memcmp(copy, fb, ROWS * ROW));
		/* written again (with the same byte), it isn't watched until it's stayed the same a while */
		memset(changed, 0, sizeof changed);
		CHECK(mem_ok_w(&mem, at, 1));
		for (unsigned k = 1; k < VIDEO_QUIET; k++)
			CHECK_EQ(video_copy_changes(v, copy, changed, ROWS, false, watch), 0);
		CHECK(!mem_watched(&mem, at));
		CHECK_EQ(video_copy_changes(v, copy, changed, ROWS, false, watch), 0);
		CHECK_EQ(mem_watched(&mem, at), watch);

		/* the host's writes, as a disk's DMA (the last row: in the last page, part of a host page) */
		at = FB + (ROWS - 1) * ROW;
		mem_invalidate(&mem, at, ROW);
		CHECK(!mem_watched(&mem, at));
		memset(fb + (ROWS - 1) * ROW, 0x5A + watch, ROW);
		from = (unsigned)(((ROWS - 1) * ROW) / unit * unit / ROW);
		CHECK_EQ(video_copy_changes(v, copy, changed, ROWS, false, watch), ROWS - from);
		CHECK(marked(changed, ROWS, from, ROWS));
		CHECK(!memcmp(copy, fb, ROWS * ROW));
		memset(changed, 0, sizeof changed);
		for (unsigned k = 0; k < VIDEO_QUIET; k++)
			CHECK_EQ(video_copy_changes(v, copy, changed, ROWS, false, watch), 0);
		CHECK_EQ(mem_watched(&mem, at), watch);
	}
	/* comparing sees what watching wouldn't: writes no one was told of */
	fb[3 * unit + 5] ^= 1;
	memset(changed, 0, sizeof changed);
	CHECK_EQ(video_copy_changes(v, copy, changed, ROWS, false, true), 0);
	CHECK(video_copy_changes(v, copy, changed, ROWS, false, false) > 0);
	CHECK(!memcmp(copy, fb, ROWS * ROW));
	mem_unwatch(&mem, FB >> MEM_PAGE_SHIFT);
	for (uint32_t p = 0; p < FB_SIZE; p += MEM_PAGE_SIZE)
		mem_unwatch(&mem, (FB + p) >> MEM_PAGE_SHIFT);
	CHECK(!mem_watched(&mem, FB));
	CHECK(mem_ok_w(&mem, FB, 4));
	free(copy);
}

/* A driver request; false if it called out instead of finishing */
static bool driver(struct video *v, struct ndrv_call *c, unsigned code, unsigned cs, uint32_t param)
{
	*c = (struct ndrv_call){ .mem = &mem, .entry = 0, .toc = TOC, .frame = FRAME };
	c->arg[2] = PB;
	c->arg[3] = code;
	c->arg[4] = NDRV_KIND_IMMEDIATE;
	mem_w16(&mem, PB + 26, (uint16_t)cs);
	mem_w32(&mem, PB + 28, param);
	video_driver_call(v, c);
	return c->done;
}

static bool resume(struct video *v, struct ndrv_call *c, uint32_t result)
{
	*c = (struct ndrv_call){ .mem = &mem, .entry = NDRV_ENTRY_RESUME, .toc = TOC, .frame = FRAME };
	c->arg[0] = result;
	video_driver_call(v, c);
	return c->done;
}

static bool has_string(const uint8_t *p, size_t len, const char *s)
{
	for (size_t k = 0; k + strlen(s) <= len; k++)
		if (memcmp(p + k, s, strlen(s)) == 0)
			return true;
	return false;
}

/*
 * The driver's data section, loaded as the Code Fragment Manager would,
 * the imports' TVectors filled in: import k's at TVECTORS + 8k (the
 * hardware cursor's library's, the last, only with the hardware cursor)
 */
static void load_driver(const struct video *v, const uint8_t *ndrv)
{
	const uint8_t *section = ndrv + 40 + 28;
	mem_load(&mem, TOC, ndrv + be32(section + 20), be32(section + 16));
	for (unsigned k = 0; k < (v->hw_cursor ? IMPORTS : IMP_VSL_PREPARE_CURSOR); k++)
		mem_w32(&mem, TOC + v->layout.import[k], TVECTORS + 8 * k);
}

/*
 * The Video Services Library, as the driver calls it: the cursor in the
 * driver's format (8 bits a pixel: indices into the colour map, 254
 * transparent, 255 inverting), a colour map of ColorSpecs after a
 * ColorTable header.
 */
static void vsl_prepare(const struct ndrv_call *c, uint32_t desc, uint32_t info)
{
	CHECK_EQ(c->out[0], CURSOR_REF);
	CHECK_EQ(c->out[1], desc);
	CHECK_EQ(c->out[2], info);
	CHECK_EQ(mem_r16(&mem, desc), 1);                       /* majorVersion */
	CHECK_EQ(mem_r32(&mem, desc + 4), 16);                  /* height, width */
	CHECK_EQ(mem_r32(&mem, desc + 8), 16);
	CHECK_EQ(mem_r32(&mem, desc + 12), 8);                  /* bitDepth */
	CHECK_EQ(mem_r32(&mem, desc + 20), 254);                /* numColors */
	uint32_t encodings = mem_r32(&mem, desc + 24);
	CHECK_EQ(mem_r32(&mem, encodings + 4 * 200), 200);
	CHECK_EQ(mem_r32(&mem, desc + 32), 1 | 4);              /* transparent and inverting */
	CHECK_EQ(mem_r32(&mem, desc + 36), 254);
	CHECK_EQ(mem_r32(&mem, desc + 40), 255);
	CHECK_EQ(mem_r16(&mem, info), 1);
	uint32_t ctab = mem_r32(&mem, info + 12), image = mem_r32(&mem, info + 16);
	mem_w32(&mem, info + 4, 16);                            /* cursorHeight, cursorWidth */
	mem_w32(&mem, info + 8, 16);
	static const uint16_t colors[2][4] = { { 0, 0xFFFF, 0xFFFF, 0xFFFF }, { 1, 0xCC00, 0x3300, 0x0100 } };
	for (unsigned k = 0; k < 2; k++)
		for (unsigned f = 0; f < 4; f++)
			mem_w16(&mem, ctab + 8 + 8 * k + 2 * f, colors[k][f]);
	memset(mem_ptr(&mem, image), 254, 256);
	mem_w8(&mem, image + 0, 0);                             /* (0, 0) white */
	mem_w8(&mem, image + 1, 1);                             /* (1, 0) the colour */
	mem_w8(&mem, image + 16 + 2, 255);                      /* (2, 1) inverting */
}

static void test_cursor(void)
{
	struct video *v = &video;
	static uint8_t ndrv[8192];
	struct ndrv_call c;

	/* without the hardware cursor, the driver is as before: SetHardwareCursor fails, there's none */
	video_init(v, &mem, FB, FB_SIZE, TIMEBASE);
	size_t size = video_build_ndrv(v, ndrv, sizeof ndrv, 1);
	CHECK(size && !has_string(ndrv, size, "VSLPrepareCursorForHardwareCursor"));
	mem_w32(&mem, PARAM, CURSOR_REF);
	CHECK(driver(v, &c, NDRV_CONTROL, 22, PARAM));
	CHECK_EQ(c.result, (uint32_t)-17);
	CHECK(driver(v, &c, NDRV_STATUS, 22, PARAM));
	CHECK_EQ(c.result, (uint32_t)-18);
	CHECK(driver(v, &c, NDRV_CONTROL, 23, PARAM));
	CHECK_EQ(c.result, (uint32_t)-17);

	/* with it */
	video_init(v, &mem, FB, FB_SIZE, TIMEBASE);
	v->hw_cursor = true;
	size = video_build_ndrv(v, ndrv, sizeof ndrv, 1);
	CHECK(size && has_string(ndrv, size, "VSLPrepareCursorForHardwareCursor"));
	load_driver(v, ndrv);
	CHECK(driver(v, &c, NDRV_STATUS, 22, PARAM));
	CHECK_EQ(c.result, 0);
	CHECK_EQ(mem_r32(&mem, PARAM), 1);

	mem_w32(&mem, PARAM, CURSOR_REF);
	CHECK(!driver(v, &c, NDRV_CONTROL, 22, PARAM));
	CHECK_EQ(c.tvector, TVECTORS + 8 * IMP_VSL_PREPARE_CURSOR);
	uint32_t desc = c.out[1], info = c.out[2];
	CHECK(desc > TOC && info > desc);
	vsl_prepare(&c, desc, info);
	CHECK(resume(v, &c, 1));
	CHECK_EQ(c.result, 0);
	CHECK(v->cursor.set);
	CHECK_EQ(v->cursor.images, 1);
	CHECK_EQ(v->cursor.width, 16);
	CHECK_EQ(v->cursor.pixels[0], 0xFFFFFFFFu);
	CHECK_EQ(v->cursor.pixels[1], 0xFFCC3301u);
	CHECK_EQ(v->cursor.pixels[2], VIDEO_CURSOR_CLEAR);
	CHECK_EQ(v->cursor.pixels[16 + 2], VIDEO_CURSOR_INVERT);
	/* the same one again isn't a new image */
	CHECK(!driver(v, &c, NDRV_CONTROL, 22, PARAM));
	vsl_prepare(&c, desc, info);
	CHECK(resume(v, &c, 1));
	CHECK_EQ(v->cursor.images, 1);

	/* DrawHardwareCursor: the image's top left, and whether it's shown */
	mem_w32(&mem, PARAM, (uint32_t)-1);
	mem_w32(&mem, PARAM + 4, 2);
	mem_w32(&mem, PARAM + 8, 1);
	CHECK(driver(v, &c, NDRV_CONTROL, 23, PARAM));
	CHECK_EQ(c.result, 0);
	CHECK(v->cursor.visible);
	CHECK_EQ(v->cursor.x, -1);
	CHECK_EQ(v->cursor.y, 2);
	CHECK(driver(v, &c, NDRV_STATUS, 23, PARAM + 64));
	CHECK_EQ(mem_r32(&mem, PARAM + 64), (uint32_t)-1);
	CHECK_EQ(mem_r32(&mem, PARAM + 64 + 8), 1);
	CHECK_EQ(mem_r32(&mem, PARAM + 64 + 12), 1);

	/* drawn over the screen for screenshots: clipped, colours, inverted, clear */
	unsigned w = video_width(v), h = video_height(v);
	uint8_t *rgb = malloc((size_t)w * h * 3);
	memset(rgb, 0x30, (size_t)w * h * 3);
	video_draw_cursor(v, rgb);
	CHECK_EQ(rgb[(2 * w + 0) * 3 + 0], 0xCC);               /* (1, 0) of the cursor is at (0, 2) */
	CHECK_EQ(rgb[(2 * w + 0) * 3 + 2], 0x01);
	CHECK_EQ(rgb[(3 * w + 1) * 3 + 1], 0xCF);               /* (2, 1) inverts (1, 3) */
	CHECK_EQ(rgb[(2 * w + 1) * 3], 0x30);                   /* (2, 0) is clear */
	free(rgb);

	/* the library couldn't: Mac OS draws that one itself */
	mem_w32(&mem, PARAM, CURSOR_REF);
	CHECK(!driver(v, &c, NDRV_CONTROL, 22, PARAM));
	CHECK(resume(v, &c, 0));
	CHECK_EQ(c.result, (uint32_t)-17);
	CHECK(!v->cursor.set && !v->cursor.visible);
}

/* One of the driver's other entry points, with r3-r6 */
static bool entry(struct video *v, struct ndrv_call *c, uint32_t n, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
	*c = (struct ndrv_call){ .mem = &mem, .entry = n, .toc = TOC, .frame = FRAME };
	c->arg[0] = a0;
	c->arg[1] = a1;
	c->arg[2] = a2;
	c->arg[3] = a3;
	video_driver_call(v, c);
	return c->done;
}

/*
 * Vertical blanks: VIDEO_VBL_HZ a guest second, vertical blank n at
 * timebase n * 16.6 MHz / 60 (rounded up), each latching the status, which
 * raises the line while the interrupt is on; several before an
 * acknowledgement are one interrupt.
 */
static void test_vbl_timing(void)
{
	struct video *v = &video;
	video_init(v, &mem, FB, FB_SIZE, TIMEBASE);
	video_set_time(v, 0);
	CHECK(v->vbl_status && !video_irq(v));          /* (the interrupt is off) */
	CHECK_EQ(video_next_vbl(v), UINT64_MAX);
	v->vbl_enabled = true;                          /* (the driver's to turn on: see test_vbl_driver()) */
	CHECK(video_irq(v));
	CHECK_EQ(video_next_vbl(v), UINT64_MAX);        /* (raised already) */
	v->vbl_status = false;                          /* (acknowledged, as the driver's handler does) */
	CHECK(!video_irq(v));
	CHECK_EQ(video_next_vbl(v), 276667);
	video_set_time(v, 276666);
	CHECK(!video_irq(v));
	video_set_time(v, 276667);
	CHECK(video_irq(v));
	v->vbl_status = false;

	/* a guest second: 60 of them, 276,666 or 276,667 ticks apart, the last a second after the first */
	uint64_t from = 10ull * TIMEBASE, at = from;
	video_set_time(v, from);
	CHECK(video_irq(v));
	v->vbl_status = false;
	unsigned n = 0;
	for (uint64_t next; (next = video_next_vbl(v)) <= from + TIMEBASE; at = next, n++) {
		CHECK(next - at == 276666 || next - at == 276667);
		video_set_time(v, next - 1);
		CHECK(!video_irq(v));
		video_set_time(v, next);
		CHECK(video_irq(v));
		v->vbl_status = false;
	}
	CHECK_EQ(n, 60);
	CHECK_EQ(at, from + TIMEBASE);

	/* 3 s without an acknowledgement: one interrupt; the next where it would have been anyway */
	video_set_time(v, from + 4 * TIMEBASE + 5);
	CHECK(video_irq(v));
	v->vbl_status = false;
	CHECK_EQ(video_next_vbl(v), from + 4 * TIMEBASE + 276667);
	/* time doesn't go back */
	video_set_time(v, from);
	CHECK(!video_irq(v));
}

/* 3D frames for the screen: at most one a vertical blank from each context, the late ones when they come */
static void test_frame_pacing(void)
{
	struct video *v = &video;
	const uint64_t gap = TIMEBASE / VIDEO_VBL_HZ;   /* (276,666 ticks) */
	video_init(v, &mem, FB, FB_SIZE, TIMEBASE);
	uint64_t t = 5ull * TIMEBASE;
	CHECK_EQ(video_frame_shown(v, 0, 0x1000, t), t);                        /* a context's first */
	CHECK_EQ(video_frame_shown(v, 0, 0x1000, t + 16600), t + gap);          /* 1 ms later: a frame later */
	CHECK_EQ(video_frame_shown(v, 0, 0x1000, t + gap + 1), t + 2 * gap);    /* (from when the last was shown) */
	CHECK_EQ(video_frame_shown(v, 0, 0x1000, t + 3 * gap + 99), t + 3 * gap + 99);  /* late: at once */
	CHECK_EQ(video_frame_shown(v, 0, 0x1000, t + 3 * gap + 100), t + 4 * gap + 99);
	/* 400 frames finished in a guest second: shown over 400 vertical blanks */
	uint64_t shown = 0;
	for (unsigned k = 0; k < 400; k++)
		shown = video_frame_shown(v, 1, 0x2000, 10ull * TIMEBASE + k * (TIMEBASE / 400));
	CHECK_EQ(shown, 10ull * TIMEBASE + 399 * gap);
	/* contexts are paced apart; another context in a slot starts afresh */
	CHECK_EQ(video_frame_shown(v, 2, 0x3000, t + 4 * gap + 100), t + 4 * gap + 100);
	CHECK_EQ(video_frame_shown(v, 0, 0x4000, t + 4 * gap + 100), t + 4 * gap + 100);
	CHECK_EQ(video_frame_shown(v, VIDEO_FRAME_SOURCES, 0x5000, t), t);      /* (no slot: not paced) */
}

/*
 * The driver: at Open, its handler in the display node's interrupt set
 * (the set's own refCon, enabler and disabler kept), the VBL service, the
 * interrupt enabled; the handler acknowledges a vertical blank and queues
 * the secondary handler (once), which runs the VBL service; SetInterrupt
 * (Control 7) turns the interrupt off and on.
 */
#define REG_ENTRY 0x30002u              /* (in DriverInitInfo, after the refNum) */
#define SET 0x00037930u
#define REFCON 0x5555u
#define ENABLER 0x7777u
#define DISABLER 0x8888u
#define SERVICE 0x05E0u

static void test_vbl_driver(void)
{
	struct video *v = &video;
	static uint8_t ndrv[8192];
	struct ndrv_call c;

	video_init(v, &mem, FB, FB_SIZE, TIMEBASE);
	size_t size = video_build_ndrv(v, ndrv, sizeof ndrv, 1);
	CHECK(size && !has_string(ndrv, size, "SetInterruptTimer"));
	load_driver(v, ndrv);
	for (unsigned k = 0; k < 16; k++)
		mem_w8(&mem, REG_ENTRY + k, (uint8_t)(0xA0 + k));
	CHECK(driver(v, &c, NDRV_INITIALIZE, 0, 0));
	CHECK_EQ(c.result, 0);

	/* Open: the node's driver-ist; without one, no VBL interrupts (and no VBL service) */
	CHECK(!driver(v, &c, NDRV_OPEN, 0, 0));
	CHECK_EQ(c.tvector, TVECTORS + 8 * IMP_PROPERTY_GET);
	uint32_t reg_entry = c.out[0], ist = c.out[2], ist_size = c.out[3];
	CHECK(!memcmp(mem_ptr(&mem, reg_entry), mem_ptr(&mem, REG_ENTRY), 16));
	CHECK(!strcmp((const char *)mem_ptr(&mem, c.out[1]), "driver-ist"));
	CHECK_EQ(mem_r32(&mem, ist_size), 24);
	CHECK(resume(v, &c, (uint32_t)-2539));          /* nrNotFoundErr */
	CHECK_EQ(c.result, 0);
	CHECK(!v->vbl_service && !v->vbl_enabled);

	/* with one: the handler installed in member 1 of the set, the service, the member enabled */
	CHECK(!driver(v, &c, NDRV_OPEN, 0, 0));
	mem_w32(&mem, ist, SET);
	mem_w32(&mem, ist + 4, 1);
	mem_w32(&mem, ist_size, 24);
	CHECK(!resume(v, &c, 0));
	CHECK_EQ(c.tvector, TVECTORS + 8 * IMP_GET_INT);
	CHECK(c.out[0] == SET && c.out[1] == 1);
	uint32_t funcs = c.out[2];
	CHECK(c.out[3] == funcs + 4 && c.out[4] == funcs + 8 && c.out[5] == funcs + 12);
	mem_w32(&mem, funcs, REFCON);
	mem_w32(&mem, funcs + 4, 0x6666);               /* (the set's default handler) */
	mem_w32(&mem, funcs + 8, ENABLER);
	mem_w32(&mem, funcs + 12, DISABLER);
	CHECK(!resume(v, &c, 0));
	CHECK_EQ(c.tvector, TVECTORS + 8 * IMP_INSTALL_INT);
	CHECK(c.out[0] == SET && c.out[1] == 1 && c.out[2] == REFCON && c.out[4] == ENABLER && c.out[5] == DISABLER);
	CHECK_EQ(c.out[3], TOC + v->layout.entry[ENTRY_VBL_INTERRUPT]);
	CHECK(!resume(v, &c, 0));
	CHECK_EQ(c.tvector, TVECTORS + 8 * IMP_VSL_NEW_SERVICE);
	CHECK(c.out[0] == reg_entry && c.out[1] == 0x76626C20u);       /* 'vbl ' */
	mem_w32(&mem, c.out[2], SERVICE);
	CHECK(!resume(v, &c, 0));
	CHECK(c.tvector == ENABLER && c.out[0] == SET && c.out[1] == 1 && c.out[2] == REFCON);
	CHECK(resume(v, &c, 0));
	CHECK_EQ(c.result, 0);
	CHECK_EQ(v->vbl_service, SERVICE);
	CHECK(v->vbl_enabled);

	/* a vertical blank: the handler acknowledges it and queues the secondary handler */
	video_set_time(v, 5 * TIMEBASE);
	CHECK(video_irq(v));
	CHECK(!entry(v, &c, ENTRY_VBL_INTERRUPT, SET, 1, REFCON, 1));
	CHECK(!video_irq(v));
	CHECK_EQ(c.tvector, TVECTORS + 8 * IMP_QUEUE_SECONDARY);
	CHECK_EQ(c.out[0], TOC + v->layout.entry[ENTRY_VBL_SECONDARY]);
	CHECK(resume(v, &c, 0));
	CHECK_EQ(c.result, 0);                          /* kIsrIsComplete */
	CHECK_EQ(v->vbls, 1);
	/* none to acknowledge: not ours */
	CHECK(entry(v, &c, ENTRY_VBL_INTERRUPT, SET, 1, REFCON, 1));
	CHECK_EQ(c.result, (uint32_t)-1);               /* kIsrIsNotComplete */
	/* another before the secondary handler ran: acknowledged, not queued again */
	video_set_time(v, video_next_vbl(v));
	CHECK(entry(v, &c, ENTRY_VBL_INTERRUPT, SET, 1, REFCON, 1));
	CHECK_EQ(c.result, 0);
	CHECK(!video_irq(v));
	CHECK_EQ(v->vbls, 2);
	/* the secondary handler: Mac OS's VBL tasks */
	CHECK(!entry(v, &c, ENTRY_VBL_SECONDARY, 0, 0, 0, 0));
	CHECK(c.tvector == TVECTORS + 8 * IMP_VSL_DO_SERVICE && c.out[0] == SERVICE);
	CHECK(resume(v, &c, 0));
	CHECK_EQ(c.result, 0);
	CHECK(!v->vbl_queued);

	/* SetInterrupt: off, no more interrupts (GetInterrupt says so); on, the next vertical blank's */
	mem_w8(&mem, PARAM, 1);
	CHECK(driver(v, &c, NDRV_CONTROL, 7, PARAM));
	CHECK(!v->vbl_enabled && video_next_vbl(v) == UINT64_MAX);
	video_set_time(v, 6 * TIMEBASE);
	CHECK(!video_irq(v));
	CHECK(driver(v, &c, NDRV_STATUS, 7, PARAM + 16));
	CHECK_EQ(mem_r8(&mem, PARAM + 16), 1);
	CHECK(entry(v, &c, ENTRY_VBL_SECONDARY, 0, 0, 0, 0));          /* (one queued before: no VBL tasks) */
	mem_w8(&mem, PARAM, 0);
	CHECK(driver(v, &c, NDRV_CONTROL, 7, PARAM));
	CHECK(!video_irq(v));                           /* (the one while it was off doesn't count) */
	CHECK_EQ(video_next_vbl(v), 6 * TIMEBASE + 276667);
}

void test_video(void)
{
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		return;
	}
	mem_map(&mem, 0, 0x100000, MEM_RW);
	mem_map(&mem, FB, FB_SIZE, MEM_RW);
	test_convert();
	test_copy_changes();
	test_cursor();
	test_vbl_timing();
	test_frame_pacing();
	test_vbl_driver();
	mem_free(&mem);
}
