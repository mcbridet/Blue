/*
 * The screen for the window: converting the framebuffer to 32-bit pixels
 * at every depth (the same colours as the RGB screenshots take), finding
 * the rows that changed, and the hardware cursor the driver takes from
 * Mac OS.
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
	video_init(v, &mem, FB, FB_SIZE);
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

static void test_changed_rows(void)
{
	enum { ROWS = 100, ROW = 64 };
	static uint8_t a[ROWS * ROW], b[ROWS * ROW];
	struct video_rows r[8];
	memset(a, 0x55, sizeof a);
	memcpy(b, a, sizeof b);
	CHECK_EQ(video_changed_rows(a, b, ROW, ROWS, r, 8), 0);

	/* rows near each other are one band; a row 49 after the last starts another */
	b[5 * ROW] ^= 1;
	b[6 * ROW + 63] ^= 1;
	b[30 * ROW + 10] ^= 1;
	b[80 * ROW] ^= 1;
	b[99 * ROW + 63] ^= 1;
	CHECK_EQ(video_changed_rows(a, b, ROW, ROWS, r, 8), 2);
	CHECK_EQ(r[0].from, 5);
	CHECK_EQ(r[0].to, 31);
	CHECK_EQ(r[1].from, 80);
	CHECK_EQ(r[1].to, 100);
	/* past the last band there may be, the rest go into it */
	CHECK_EQ(video_changed_rows(a, b, ROW, ROWS, r, 1), 1);
	CHECK_EQ(r[0].from, 5);
	CHECK_EQ(r[0].to, 100);

	memcpy(b, a, sizeof b);
	b[0] ^= 0x80;
	CHECK_EQ(video_changed_rows(a, b, ROW, ROWS, r, 8), 1);
	CHECK_EQ(r[0].from, 0);
	CHECK_EQ(r[0].to, 1);
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
	video_driver_call(v, c, 16600000);
	return c->done;
}

static bool resume(struct video *v, struct ndrv_call *c, uint32_t result)
{
	*c = (struct ndrv_call){ .mem = &mem, .entry = NDRV_ENTRY_RESUME, .toc = TOC, .frame = FRAME };
	c->arg[0] = result;
	video_driver_call(v, c, 16600000);
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
	video_init(v, &mem, FB, FB_SIZE);
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
	video_init(v, &mem, FB, FB_SIZE);
	v->hw_cursor = true;
	size = video_build_ndrv(v, ndrv, sizeof ndrv, 1);
	CHECK(size && has_string(ndrv, size, "VSLPrepareCursorForHardwareCursor"));
	/* (its data section, loaded as the Code Fragment Manager would, the imports' TVectors filled in) */
	const uint8_t *section = ndrv + 40 + 28;
	mem_load(&mem, TOC, ndrv + be32(section + 20), be32(section + 16));
	for (unsigned k = 0; k < 6; k++)                        /* (the last is the library's) */
		mem_w32(&mem, TOC + v->layout.import[k], TVECTORS + 8 * k);
	CHECK(driver(v, &c, NDRV_STATUS, 22, PARAM));
	CHECK_EQ(c.result, 0);
	CHECK_EQ(mem_r32(&mem, PARAM), 1);

	mem_w32(&mem, PARAM, CURSOR_REF);
	CHECK(!driver(v, &c, NDRV_CONTROL, 22, PARAM));
	CHECK_EQ(c.tvector, TVECTORS + 8 * 5);
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

void test_video(void)
{
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		return;
	}
	mem_map(&mem, 0, 0x100000, MEM_RW);
	mem_map(&mem, FB, FB_SIZE, MEM_RW);
	test_convert();
	test_changed_rows();
	test_cursor();
	mem_free(&mem);
}
