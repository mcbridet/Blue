/*
 * The guest's OpenGL libraries' host side, driven as the guest's calls
 * would drive it: pixel formats and renderer info, contexts with Apple's
 * dispatch table, drawables (a window's port, clipped to its regions; an
 * offscreen buffer), clears and swaps, strings and errors, the dispatch
 * table's calls; OpenGLMemory's allocations; the fragments themselves.
 */
#include <string.h>

#include "../src/be.h"
#include "../src/mem.h"
#include "../src/opengl.h"
#include "test.h"

#define TOC 0x10000u            /* OpenGLLibrary's data (a process's instance) */
#define MEM_TOC 0x38000u        /* OpenGLMemory's */
#define FRAME 0x40000u          /* the stub's stack frame */
#define ATTRIBS 0x48000u
#define OUT 0x48100u
#define PORT 0x50000u           /* a window's CGrafPort, its pixmap, regions */
#define PIXMAP_H 0x50100u
#define PIXMAP 0x50110u
#define VIS_H 0x50200u
#define VIS 0x50210u
#define CLIP_H 0x50300u
#define CLIP 0x50310u
#define DEVICE_H 0x50400u       /* the main device */
#define DEVICE 0x50410u
#define PIXELS 0x100000u        /* the screen: 64 x 48 */
#define OFFSCREEN 0x110000u
#define SCREEN_W 64
#define SCREEN_H 48
#define LM_MAIN_DEVICE 0x08A4u

/* AGL's and OpenGL's numbers */
enum { AGL_NONE, AGL_RGBA = 4, AGL_DOUBLEBUFFER = 5, AGL_DEPTH_SIZE = 12, AGL_ACCUM_RED_SIZE = 14,
       AGL_PIXEL_SIZE = 50, AGL_RENDERER_ID = 70, AGL_ACCELERATED = 73, AGL_VIDEO_MEMORY = 120,
       AGL_SWAP_RECT = 200 };
enum { AGL_BAD_ATTRIBUTE = 10000, AGL_BAD_CONTEXT = 10004, AGL_BAD_DRAWABLE = 10005 };
#define GL_COLOR_BUFFER_BIT 0x4000u
#define GL_SCISSOR_TEST 0x0C11u
#define GL_INVALID_VALUE 0x0501u
#define GL_VIEWPORT 0x0BA2u

static struct mem mem;
static struct opengl gl;

/* A call to entry k with these words and doubles (f1 on); its result */
static uint32_t call_entry(unsigned k, const uint32_t *w, unsigned nw, const double *f, unsigned nf)
{
	struct ndrv_call c;
	memset(&c, 0, sizeof c);
	c.mem = &mem;
	c.entry = k;
	c.toc = TOC;
	c.frame = FRAME;
	for (unsigned j = 0; j < nw && j < 8; j++)
		c.arg[j] = w[j];
	for (unsigned j = 8; j < nw; j++)               /* (the caller's parameter area) */
		mem_w32(&mem, FRAME + NDRV_FRAME_SIZE + 24 + 4 * j, w[j]);
	for (unsigned j = 0; j < nf; j++)
		memcpy(&c.f[j], &f[j], 8);
	opengl_library_call(&gl, &c);
	CHECK(c.done);
	return c.result;
}

#define CALL(name, ...) call_entry((unsigned)opengl_entry(#name), (const uint32_t[]){ __VA_ARGS__ }, \
				   sizeof (uint32_t[]){ __VA_ARGS__ } / 4, NULL, 0)
#define CALL0(name) call_entry((unsigned)opengl_entry(#name), NULL, 0, NULL, 0)
#define CALL_F(name, ...) call_entry((unsigned)opengl_entry(#name), NULL, 0, (const double[]){ __VA_ARGS__ }, \
				     sizeof (double[]){ __VA_ARGS__ } / 8)

static void put_attribs(const uint32_t *a, unsigned n)
{
	for (unsigned k = 0; k < n; k++)
		mem_w32(&mem, ATTRIBS + 4 * k, a[k]);
}

/* A pixmap (32 or 16 bits) at PIXELS whose (0, 0) is the port's (-8, -4): a window at (8, 4) on the screen */
static void make_port(unsigned bits)
{
	memset(mem_ptr(&mem, PORT), 0, 0x500);
	memset(mem_ptr(&mem, PIXELS), 0, SCREEN_W * SCREEN_H * 4);
	mem_w32(&mem, PORT + 2, PIXMAP_H);
	mem_w16(&mem, PORT + 6, 0xC000);                /* a color port */
	static const int16_t port_rect[4] = { 0, 0, 24, 32 };   /* top, left, bottom, right */
	for (int k = 0; k < 4; k++)
		mem_w16(&mem, PORT + 16 + 2 * k, (uint16_t)port_rect[k]);
	mem_w32(&mem, PORT + 24, VIS_H);
	mem_w32(&mem, PORT + 28, CLIP_H);
	mem_w32(&mem, PIXMAP_H, PIXMAP);
	mem_w32(&mem, PIXMAP, PIXELS);
	mem_w16(&mem, PIXMAP + 4, (uint16_t)(0x8000 | SCREEN_W * bits / 8));
	static const int16_t bounds[4] = { -4, -8, SCREEN_H - 4, SCREEN_W - 8 };
	for (int k = 0; k < 4; k++)
		mem_w16(&mem, PIXMAP + 6 + 2 * k, (uint16_t)bounds[k]);
	mem_w16(&mem, PIXMAP + 32, (uint16_t)bits);
	/* visRgn: the port's rectangle; clipRgn: that with a hole in rows 8-15, columns 10-19 */
	mem_w32(&mem, VIS_H, VIS);
	mem_w16(&mem, VIS, 10);
	for (int k = 0; k < 4; k++)
		mem_w16(&mem, VIS + 2 + 2 * k, (uint16_t)port_rect[k]);
	mem_w32(&mem, CLIP_H, CLIP);
	static const int16_t clip[] = { 44, 0, 0, 24, 32, 0, 0, 32, 32767, 8, 10, 20, 32767, 16, 10, 20, 32767,
					24, 0, 32, 32767, 32767 };
	for (unsigned k = 0; k < sizeof clip / sizeof clip[0]; k++)
		mem_w16(&mem, CLIP + 2 * k, (uint16_t)clip[k]);
	/* the main device, at the same depth */
	mem_w32(&mem, LM_MAIN_DEVICE, DEVICE_H);
	mem_w32(&mem, DEVICE_H, DEVICE);
	mem_w32(&mem, DEVICE + 22, PIXMAP_H);
}

/* The screen's pixel at the port's (x, y), as 0x00RRGGBB */
static uint32_t screen(int x, int y, unsigned bits)
{
	uint32_t at = PIXELS + (uint32_t)(y + 4) * SCREEN_W * bits / 8 + (uint32_t)(x + 8) * bits / 8;
	if (bits == 32)
		return mem_r32(&mem, at);
	uint32_t v = mem_r16(&mem, at);
	return (v >> 10 & 31) << 19 | (v >> 5 & 31) << 11 | (v & 31) << 3;
}

static void test_fragment(void)
{
	static uint8_t buf[64 << 10];
	struct ndrv_layout l;
	size_t n = opengl_build_library(buf, sizeof buf, &l);
	CHECK(n > 0);
	const uint8_t *ld = buf + be32(buf + 40 + 56 + 20);
	CHECK_EQ(be32(ld + 52), OPENGL_FUNCTIONS + 1);          /* 440 gl, 33 agl, glc_cc */
	CHECK_EQ(be32(ld + 8), 1);                              /* initialization */
	CHECK_EQ(be32(ld + 12), l.entry[OPENGL_ENTRY_INIT]);
	CHECK_EQ(be32(ld + 20), l.entry[OPENGL_ENTRY_TERM]);    /* termination */
	CHECK_EQ(be32(buf + 28), 1);                            /* current version, as Apple's */
	const uint8_t *data = buf + be32(buf + 40 + 28 + 20);
	CHECK(!memcmp(data + l.extra, "syGL", 4));
	CHECK_EQ(opengl_entry("glAccum"), 0);                   /* (the dispatch table's order) */
	CHECK_EQ(opengl_entry("aglChoosePixelFormat"), 440);
	CHECK_EQ(opengl_entry("glNoSuchThing"), -1);
	CHECK(opengl_build_memory(buf, sizeof buf, &l) > 0);
	CHECK(opengl_build_utility(buf, sizeof buf, &l) > 0);
}

static void test_agl(void)
{
	/* the library's data, as the Code Fragment Manager loads it (but for relocations) */
	static uint8_t buf[64 << 10];
	struct ndrv_layout l;
	CHECK(opengl_build_library(buf, sizeof buf, &l) > 0);
	const uint8_t *section = buf + 40 + 28;
	memcpy(mem_ptr(&mem, TOC), buf + be32(section + 20), be32(section + 12));
	call_entry(OPENGL_ENTRY_INIT, NULL, 0, NULL, 0);
	const uint32_t data = TOC + gl.library.extra;
	make_port(32);

	/* pixel formats: what's asked for (or more), else none */
	const uint32_t bad[] = { AGL_RGBA, AGL_ACCUM_RED_SIZE, 8, AGL_NONE };
	put_attribs(bad, 4);
	CHECK_EQ(CALL(aglChoosePixelFormat, 0, 0, ATTRIBS), 0);
	const uint32_t unknown[] = { AGL_RGBA, 999, AGL_NONE };
	put_attribs(unknown, 3);
	CHECK_EQ(CALL(aglChoosePixelFormat, 0, 0, ATTRIBS), 0);
	CHECK_EQ(CALL0(aglGetError), AGL_BAD_ATTRIBUTE);
	CHECK_EQ(CALL0(aglGetError), 0);
	const uint32_t good[] = { AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 20, AGL_ACCELERATED, AGL_NONE };
	put_attribs(good, 6);
	uint32_t pf = CALL(aglChoosePixelFormat, 0, 0, ATTRIBS);
	CHECK(pf != 0);
	CHECK_EQ(CALL(aglDescribePixelFormat, pf, AGL_DEPTH_SIZE, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 24);
	CHECK_EQ(CALL(aglDescribePixelFormat, pf, AGL_PIXEL_SIZE, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 32);
	CHECK_EQ(CALL(aglDescribePixelFormat, pf, AGL_RENDERER_ID, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 0x00021000);
	uint32_t devices = CALL(aglDevicesOfPixelFormat, pf, OUT);
	CHECK(devices && mem_r32(&mem, OUT) == 1 && mem_r32(&mem, devices) == DEVICE_H);
	uint32_t ri = CALL(aglQueryRendererInfo, 0, 0);
	CHECK(ri != 0);
	CHECK_EQ(CALL(aglDescribeRenderer, ri, AGL_VIDEO_MEMORY, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 32u << 20);
	CHECK_EQ(CALL(aglNextRendererInfo, ri), 0);
	CALL(aglDestroyRendererInfo, ri);
	CALL(aglGetVersion, OUT, OUT + 4);
	CHECK(mem_r32(&mem, OUT) == 1 && mem_r32(&mem, OUT + 4) == 1);

	/* a context: Apple's layout, its handle first, then each gl function's TVector */
	uint32_t ctx = CALL(aglCreateContext, pf, 0);
	CHECK(ctx != 0);
	CHECK_EQ(mem_r32(&mem, ctx), ctx);
	CHECK_EQ(mem_r32(&mem, ctx + 0x02C), TOC + gl.library.entry[OPENGL_ENTRY_TABLE + opengl_entry("glClear")]);
	CHECK_EQ(CALL(aglSetCurrentContext, 0x1234), 0);
	CHECK_EQ(CALL0(aglGetError), AGL_BAD_CONTEXT);
	uint32_t msg = CALL(aglErrorString, AGL_BAD_CONTEXT);
	CHECK(!strcmp((const char *)mem_ptr(&mem, msg), "invalid context"));
	CHECK_EQ(CALL(aglSetDrawable, ctx, 0x7FFF0000u), 0);    /* (not a port) */
	CHECK_EQ(CALL0(aglGetError), AGL_BAD_DRAWABLE);
	CHECK_EQ(CALL(aglSetDrawable, ctx, PORT), 1);
	CHECK_EQ(CALL(aglGetDrawable, ctx), PORT);
	CHECK_EQ(CALL(aglSetCurrentContext, ctx), 1);
	CHECK_EQ(mem_r32(&mem, data + OPENGL_DATA_CURRENT), ctx);  /* glc_cc */
	CHECK_EQ(CALL0(aglGetCurrentContext), ctx);

	/* strings, errors, the viewport the drawable's size */
	const char *renderer = (const char *)mem_ptr(&mem, CALL(glGetString, 0x1F01));
	CHECK(!strcmp(renderer, "Rage 128 OpenGL Engine"));
	CHECK(!strcmp((const char *)mem_ptr(&mem, CALL(glGetString, 0x1F00)), "ATI Technologies Inc."));
	CALL(glGetIntegerv, GL_VIEWPORT, OUT);
	CHECK(mem_r32(&mem, OUT + 8) == 32 && mem_r32(&mem, OUT + 12) == 24);
	CALL(glClear, 0x8000);
	CHECK_EQ(CALL0(glGetError), GL_INVALID_VALUE);
	CHECK_EQ(CALL0(glGetError), 0);

	/* a clear, then (scissored, from the bottom left) another; swapped into the window, but for the clip's hole */
	CALL_F(glClearColor, 0.25, 0.5, 0.75, 1.0);
	CALL(glClear, GL_COLOR_BUFFER_BIT);
	CALL(glEnable, GL_SCISSOR_TEST);
	CALL(glScissor, 0, 0, 4, 4);
	CALL_F(glClearColor, 1.0, 0.0, 0.0, 1.0);
	CALL(glClear, GL_COLOR_BUFFER_BIT);
	CALL(glDisable, GL_SCISSOR_TEST);
	CHECK_EQ(screen(5, 5, 32), 0);                          /* (not until it's swapped) */
	CALL(aglSwapBuffers, ctx);
	CHECK_EQ(screen(5, 5, 32), 0x4080BF);
	CHECK_EQ(screen(31, 0, 32), 0x4080BF);
	CHECK_EQ(screen(15, 12, 32), 0);                        /* (the hole) */
	CHECK_EQ(screen(9, 12, 32), 0x4080BF);
	CHECK_EQ(screen(20, 15, 32), 0x4080BF);
	CHECK_EQ(screen(1, 22, 32), 0xFF0000);                  /* bottom left */
	CHECK_EQ(screen(4, 22, 32), 0x4080BF);
	CHECK_EQ(screen(1, 19, 32), 0x4080BF);
	CHECK_EQ(screen(32, 5, 32), 0);                         /* (outside the port) */

	/* through the dispatch table: the handle first, then the arguments */
	const uint32_t w[] = { ctx };
	const double f[] = { 0.0, 1.0, 0.0, 1.0 };
	call_entry(OPENGL_ENTRY_TABLE + (unsigned)opengl_entry("glClearColor"), w, 1, f, 4);
	const uint32_t clear[] = { ctx, GL_COLOR_BUFFER_BIT };
	call_entry(OPENGL_ENTRY_TABLE + (unsigned)opengl_entry("glClear"), clear, 2, NULL, 0);

	/* a swap rectangle (from the bottom left) */
	const uint32_t rect[] = { 0, 0, 8, 2 };
	put_attribs(rect, 4);
	CHECK_EQ(CALL(aglSetInteger, ctx, AGL_SWAP_RECT, ATTRIBS), 1);
	CHECK_EQ(CALL(aglEnable, ctx, AGL_SWAP_RECT), 1);
	CALL(aglSwapBuffers, ctx);
	CHECK_EQ(screen(1, 23, 32), 0x00FF00);
	CHECK_EQ(screen(1, 21, 32), 0xFF0000);
	CHECK_EQ(screen(9, 23, 32), 0x4080BF);
	CALL(aglDisable, ctx, AGL_SWAP_RECT);

	/* 16 bits a pixel */
	make_port(16);
	CALL(aglUpdateContext, ctx);
	CALL(aglSwapBuffers, ctx);
	CHECK_EQ(screen(5, 5, 16), 0x00F800);
	CHECK_EQ(screen(15, 12, 16), 0);

	/* offscreen: rows of 32 bits a pixel with alpha */
	CHECK_EQ(CALL(aglSetOffScreen, ctx, 4, 2, 32, OFFSCREEN), 1);
	CALL_F(glClearColor, 0.0, 0.0, 1.0, 0.5);
	CALL(glClear, GL_COLOR_BUFFER_BIT);
	CALL0(glFinish);
	CHECK_EQ(mem_r32(&mem, OFFSCREEN), 0);                  /* (double buffered: not until swapped) */
	CALL(aglSwapBuffers, ctx);
	CHECK_EQ(mem_r32(&mem, OFFSCREEN + 32 + 12), 0x800000FFu);

	CHECK_EQ(CALL(aglDestroyContext, ctx), 1);
	CHECK_EQ(CALL0(aglGetCurrentContext), 0);
	CALL(aglDestroyPixelFormat, pf);
	call_entry(OPENGL_ENTRY_TERM, NULL, 0, NULL, 0);
}

/* OpenGLMemory: blocks from the Memory Manager, by callouts */
static void test_memory(void)
{
	mem_w32(&mem, MEM_TOC + gl.memory.import[0], 0x7000);   /* NewPtr's TVector, NewPtrClear's... */
	mem_w32(&mem, MEM_TOC + gl.memory.import[2], 0x7020);
	mem_w32(&mem, MEM_TOC + gl.memory.import[3], 0x7030);
	struct ndrv_call c;
	memset(&c, 0, sizeof c);
	c.mem = &mem;
	c.toc = MEM_TOC;
	c.frame = FRAME;
	c.entry = 8;                                            /* glmMalloc(100) */
	c.arg[0] = 100;
	opengl_memory_call(&gl, &c);
	CHECK(!c.done && c.tvector == 0x7000 && c.out[0] == 100);
	c.entry = NDRV_ENTRY_RESUME;
	c.arg[0] = 0x9000;
	opengl_memory_call(&gl, &c);
	CHECK(c.done && c.result == 0x9000);
	/* glmRealloc(0x9000, 200): its size, a new block, the contents copied, the old one freed */
	memset(mem_ptr(&mem, 0x9000), 0x5A, 100);
	c.entry = 10;
	c.arg[0] = 0x9000;
	c.arg[1] = 200;
	opengl_memory_call(&gl, &c);
	CHECK(!c.done && c.tvector == 0x7030);
	c.entry = NDRV_ENTRY_RESUME;
	c.arg[0] = 100;
	opengl_memory_call(&gl, &c);
	CHECK(!c.done && c.tvector == 0x7000 && c.out[0] == 200);
	c.arg[0] = 0xA000;
	opengl_memory_call(&gl, &c);
	CHECK(!c.done && c.tvector == 0x7020 && c.out[0] == 0x9000);
	CHECK_EQ(mem_r8(&mem, 0xA000 + 99), 0x5A);
	c.arg[0] = 0;
	opengl_memory_call(&gl, &c);
	CHECK(c.done && c.result == 0xA000);
}

static void run(struct gpu *gpu)
{
	if (mem_init(&mem)) {
		CHECK(!"mem_init failed");
		return;
	}
	mem_map(&mem, 0, 0x200000, MEM_RW);
	opengl_init(&gl, &mem);
	gl.gpu = gpu;
	test_fragment();
	test_agl();
	test_memory();
	CHECK_EQ(gl.gpu_frames, gpu ? 4 : 0);
	opengl_free(&gl);
	mem_free(&mem);
}

void test_opengl(void)
{
	run(NULL);
	/* the same through the GPU backend, if there's OpenGL (any: llvmpipe will do) */
	struct gpu *g = gpu_open(true);
	if (g && gpu_ready(g, NULL))
		run(g);
	gpu_close(g);
}
