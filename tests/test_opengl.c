/*
 * The guest's OpenGL libraries' host side, driven as the guest's calls
 * would drive it: pixel formats and renderer info, contexts with Apple's
 * dispatch table, drawables (a window's port, clipped to its regions; an
 * offscreen buffer), clears and swaps, strings and errors, the dispatch
 * table's calls; OpenGLMemory's allocations; the fragments themselves.
 * OpenGLLibrary's own code, run by the processor (interpreted and by the
 * JIT), recording calls. Scenes drawn through it all in software and on
 * the GPU, and (on Linux) by Mesa's OpenGL in its compatibility profile,
 * the reference.
 */
#include <stdlib.h>
#include <string.h>

#include "../src/be.h"
#include "../src/jit.h"
#include "../src/mem.h"
#include "../src/opengl.h"
#include "../src/opengl_table.h"
#include "../src/png.h"
#include "../src/ppc.h"
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
#define GL_VIEWPORT 0x0BA2u

static struct mem mem;
static struct opengl gl;

/* The libraries' functions: their names, arguments' types and dispatch table offsets (opengl_table.h) */
static const struct {
	const char *name, *args;
	unsigned offset;
} functions[] = {
#define X(name, result, args, offset) { #name, args, offset },
	OPENGL_LIBRARY_FUNCTIONS(X)
#undef X
};

static unsigned library_functions_offset(const char *name)
{
	for (unsigned k = 0; k < sizeof functions / sizeof functions[0]; k++)
		if (!strcmp(functions[k].name, name))
			return functions[k].offset;
	return 0;
}

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
	static uint8_t buf[OPENGL_LIBRARY_MAX];
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
	unsigned recorded = 0;                                  /* (see opengl.c's "recording") */
	const char *types;
	for (unsigned k = 0; k < 440; k++)
		recorded += opengl_recorded(k, &types) != 0;
	CHECK_EQ(recorded, 317);
	CHECK(opengl_recorded((unsigned)opengl_entry("glLoadMatrixd"), &types) == 132 && strlen(types) == 16);
	CHECK(!opengl_recorded((unsigned)opengl_entry("glDrawArrays"), &types));
	CHECK(!opengl_recorded((unsigned)opengl_entry("glGetError"), &types));
	CHECK(!opengl_recorded((unsigned)opengl_entry("glVertexPointer"), &types));
	CHECK(opengl_build_memory(buf, sizeof buf, &l) > 0);
	CHECK(opengl_build_utility(buf, sizeof buf, &l) > 0);
}

static void test_agl(void)
{
	/* the library's data, as the Code Fragment Manager loads it (but for relocations) */
	static uint8_t buf[OPENGL_LIBRARY_MAX];
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

/* ---- recording, by the processor ------------------------------------------------- */

#define CODE 0x200000u          /* OpenGLLibrary's code */
#define CODE_SIZE 0x40000u
#define RETURN (CODE + CODE_SIZE - 16)  /* (where the processor's calls return: a host call that stops it) */
#define STACK 0x1F0000u         /* (its caller's frame: the parameter area 24 bytes on) */
#define ARRAY 0x48400u          /* arrays the calls pass */
#define GL_ENTRIES 440u         /* (gl's functions) */

static struct ppc_cpu cpu;
static unsigned host_calls;

/* OpenGLLibrary as the Code Fragment Manager loads it: its code at CODE, its data at TOC, TVectors relocated */
static void load_library(void)
{
	static uint8_t buf[OPENGL_LIBRARY_MAX];
	struct ndrv_layout l;
	CHECK(opengl_build_library(buf, sizeof buf, &l) > 0);
	const uint8_t *code = buf + 40, *data = buf + 40 + 28;
	CHECK(be32(code + 12) <= CODE_SIZE - 16);
	mem_invalidate(&mem, CODE, CODE_SIZE);
	memcpy(mem_ptr(&mem, CODE), buf + be32(code + 20), be32(code + 12));
	memset(mem_ptr(&mem, TOC), 0, be32(data + 8));
	memcpy(mem_ptr(&mem, TOC), buf + be32(data + 20), be32(data + 12));
	for (unsigned k = 0; k < OPENGL_ENTRY_TABLE + GL_ENTRIES; k++) {
		mem_w32(&mem, TOC + l.entry[k], mem_r32(&mem, TOC + l.entry[k]) + CODE);
		mem_w32(&mem, TOC + l.entry[k] + 4, TOC);
	}
	mem_w32(&mem, RETURN, PPC_HOSTCALL_INSN(0x3FF));
}

/*
 * A call by the processor to a TVector, with words (past the eighth in the
 * parameter area) and doubles; the library's host calls done as nk.c does
 */
static void guest_call(uint32_t tvector, const uint32_t *w, unsigned nw, const double *f, unsigned nf)
{
	cpu.gpr[1] = STACK;
	for (unsigned j = 0; j < nw; j++) {
		if (j < 8)
			cpu.gpr[3 + j] = w[j];
		else
			mem_w32(&mem, STACK + 24 + 4 * j, w[j]);
	}
	for (unsigned j = 0; j < nf; j++)
		memcpy(&cpu.fpr[1 + j], &f[j], 8);
	cpu.pc = mem_r32(&mem, tvector);
	cpu.gpr[2] = mem_r32(&mem, tvector + 4);
	cpu.lr = RETURN;
	for (int k = 0; k < 16; k++) {
		enum ppc_exit e = ppc_run(&cpu, 100000);
		CHECK_EQ(e, PPC_EXIT_HOSTCALL);
		if (e != PPC_EXIT_HOSTCALL || cpu.pc == RETURN)
			return;
		CHECK_EQ(mem_r32(&mem, cpu.pc), PPC_HOSTCALL_INSN(0x104));     /* (HOSTCALL_TOOLS_GL) */
		struct ndrv_call c;
		memset(&c, 0, sizeof c);
		c.mem = &mem;
		c.entry = cpu.gpr[11];
		c.toc = cpu.gpr[2];
		c.frame = cpu.gpr[1];
		memcpy(c.arg, &cpu.gpr[3], sizeof c.arg);
		memcpy(c.f, &cpu.fpr[1], sizeof c.f);
		opengl_library_call(&gl, &c);
		CHECK(c.done);
		cpu.gpr[3] = c.result;
		cpu.cr = (cpu.cr & 0x0FFFFFFFu) | 0x20000000u;
		cpu.pc += 4;
		host_calls++;
	}
	CHECK(!"the call didn't return");
}

/* A function's TVector (or as the dispatch table of the context at `table` calls it) */
static uint32_t tvector(const char *name, uint32_t table)
{
	int fn = opengl_entry(name);
	CHECK(fn >= 0 && (!table || fn < (int)GL_ENTRIES));
	if (table)
		return mem_r32(&mem, table + library_functions_offset(name));
	return TOC + gl.library.entry[fn];
}

#define WORDS(...) (const uint32_t[]){ __VA_ARGS__ }, sizeof (uint32_t[]){ __VA_ARGS__ } / 4
#define DOUBLES(...) (const double[]){ __VA_ARGS__ }, sizeof (double[]){ __VA_ARGS__ } / 8
#define NO_WORDS NULL, 0
#define NO_DOUBLES NULL, 0

static uint32_t record_at;              /* where the next record should be */

/* The record made last: of this function, these words after the first */
static void expect_record(const char *name, const uint32_t *w, unsigned n)
{
	const unsigned fn = (unsigned)opengl_entry(name), size = 4 * (n + 1);
	const char *types;
	CHECK_EQ(opengl_recorded(fn, &types), size);
	CHECK_EQ(mem_r32(&mem, record_at), fn << 16 | size);
	for (unsigned k = 0; k < n; k++)
		CHECK_EQ(mem_r32(&mem, record_at + 4 + 4 * k), w[k]);
	record_at += size;
	CHECK_EQ(mem_r32(&mem, TOC + gl.library.extra + OPENGL_DATA_RECORD), record_at);
}

static uint32_t fbits(float f)
{
	uint32_t b;
	memcpy(&b, &f, 4);
	return b;
}

static uint64_t dbits(double d)
{
	uint64_t b;
	memcpy(&b, &d, 8);
	return b;
}

#define D(x) (uint32_t)(dbits(x) >> 32), (uint32_t)dbits(x)

static struct opengl_context *context_of(uint32_t guest)
{
	for (unsigned i = 0; i < OPENGL_INSTANCES; i++)
		for (unsigned k = 0; k < OPENGL_CONTEXTS; k++)
			if (gl.inst[i].toc && gl.inst[i].ctx[k].used && gl.inst[i].ctx[k].guest == guest)
				return &gl.inst[i].ctx[k];
	return NULL;
}

static void test_recording(bool jit)
{
	ppc_init(&cpu, &mem);
	if (jit) {
		setenv("SYSHM_JIT_HOT", "0", 1);        /* (translated at once) */
		cpu.jit = jit_create(&mem);
		unsetenv("SYSHM_JIT_HOT");
		if (!cpu.jit)
			return;
	}
	load_library();
	call_entry(OPENGL_ENTRY_INIT, NULL, 0, NULL, 0);
	const uint32_t data = TOC + gl.library.extra, start = mem_r32(&mem, data + OPENGL_DATA_RECORD);
	CHECK_EQ(mem_r32(&mem, data + OPENGL_DATA_RECORD + 4), start + OPENGL_RECORD_SIZE);
	make_port(32);
	const uint32_t attribs[] = { AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 16, AGL_NONE };
	put_attribs(attribs, 5);
	uint32_t pf = CALL(aglChoosePixelFormat, 0, 0, ATTRIBS);
	uint32_t ctx = CALL(aglCreateContext, pf, 0), other = CALL(aglCreateContext, pf, 0);
	CHECK_EQ(CALL(aglSetOffScreen, ctx, 8, 8, 32, OFFSCREEN), 1);
	CHECK_EQ(CALL(aglSetCurrentContext, ctx), 1);
	struct opengl_context *x = context_of(ctx), *y = context_of(other);
	CHECK(x && y);
	if (!x || !y)
		return;
	host_calls = 0;
	record_at = start;

	/* recorded as the registers have them (bytes masked when done) */
	guest_call(tvector("glColor3ub", 0), WORDS(0xFFFFFF12u, 0x34, 0x56), NO_DOUBLES);
	expect_record("glColor3ub", WORDS(0xFFFFFF12u, 0x34, 0x56));
	/* arrays: floats, bytes (zero-extended), halfwords, doubles */
	mem_w32(&mem, ARRAY, fbits(1.5f));
	mem_w32(&mem, ARRAY + 4, fbits(-2.25f));
	mem_w32(&mem, ARRAY + 8, fbits(3.0f));
	guest_call(tvector("glVertex3fv", 0), WORDS(ARRAY), NO_DOUBLES);
	expect_record("glVertex3fv", WORDS(fbits(1.5f), fbits(-2.25f), fbits(3.0f)));
	static const uint8_t bytes[3] = { 0x80, 0x7F, 0x01 };
	memcpy(mem_ptr(&mem, ARRAY + 0x20), bytes, 3);
	guest_call(tvector("glNormal3bv", 0), WORDS(ARRAY + 0x20), NO_DOUBLES);
	expect_record("glNormal3bv", WORDS(0x80, 0x7F, 0x01));
	static const uint16_t halves[4] = { 0xFFFF, 0x8000, 0, 0x1234 };
	for (int k = 0; k < 4; k++)
		mem_w16(&mem, ARRAY + 0x30 + 2 * k, halves[k]);
	guest_call(tvector("glColor4usv", 0), WORDS(ARRAY + 0x30), NO_DOUBLES);
	expect_record("glColor4usv", WORDS(0xFFFF, 0x8000, 0, 0x1234));
	uint32_t matrix[32];
	for (int k = 0; k < 16; k++) {
		mem_w64(&mem, ARRAY + 0x40 + 8 * k, dbits(k + 0.5));
		matrix[2 * k] = (uint32_t)(dbits(k + 0.5) >> 32);
		matrix[2 * k + 1] = (uint32_t)dbits(k + 0.5);
	}
	guest_call(tvector("glRotated", 0), NO_WORDS, DOUBLES(30, 0, 0, 1));
	expect_record("glRotated", WORDS(D(30.0), D(0.0), D(0.0), D(1.0)));
	guest_call(tvector("glLoadMatrixd", 0), WORDS(ARRAY + 0x40), NO_DOUBLES);
	expect_record("glLoadMatrixd", matrix, 32);
	/* words and floats: each float takes a word's place too */
	guest_call(tvector("glMultiTexCoord2fARB", 0), WORDS(0x84C1), DOUBLES(0.25, 0.75));
	expect_record("glMultiTexCoord2fARB", WORDS(0x84C1, fbits(0.25f), fbits(0.75f)));
	/* through the dispatch table of the current context */
	guest_call(tvector("glColor4f", ctx), WORDS(ctx), DOUBLES(0.5, 0.25, 0.125, 1));
	expect_record("glColor4f", WORDS(fbits(0.5f), fbits(0.25f), fbits(0.125f), fbits(1.0f)));
	/* a ninth word, from the parameter area (then taken back: there's no such function yet) */
	uint32_t before = record_at;
	guest_call(tvector("glCopyTexSubImage3D", 0), WORDS(1, 2, 3, 4, 5, 6, 7, 8, 9), NO_DOUBLES);
	expect_record("glCopyTexSubImage3D", WORDS(1, 2, 3, 4, 5, 6, 7, 8, 9));
	guest_call(tvector("glCopyTexSubImage2D", ctx), WORDS(ctx, 1, 2, 3, 4, 5, 6, 7, 8), NO_DOUBLES);
	expect_record("glCopyTexSubImage2D", WORDS(1, 2, 3, 4, 5, 6, 7, 8));
	mem_w32(&mem, data + OPENGL_DATA_RECORD, before);
	record_at = before;
	CHECK_EQ(host_calls, 0);
	CHECK(x->st.color[0] == 1 && x->st.normal[2] == 1);     /* (nothing done yet) */

	/* any host call does what's recorded first */
	guest_call(tvector("glGetError", 0), NO_WORDS, NO_DOUBLES);
	CHECK_EQ(host_calls, 1);
	CHECK_EQ(cpu.gpr[3], 0);
	CHECK_EQ(mem_r32(&mem, data + OPENGL_DATA_RECORD), start);
	CHECK(x->st.color[0] == 0.5f && x->st.color[1] == 0.25f && x->st.color[2] == 0.125f && x->st.color[3] == 1);
	CHECK(x->st.normal[0] == -1 && x->st.normal[1] == 1 && x->st.normal[2] == (float)(3.0 / 255));
	CHECK(x->st.tex[1][0] == 0.25f && x->st.tex[1][1] == 0.75f && x->st.tex[1][3] == 1);
	for (int k = 0; k < 16; k++)
		CHECK(x->st.modelview.m[0][k] == (float)(k + 0.5));
	CHECK_EQ(gl.bad_records, 0);
	/* (and the host makes the same record of a call made to it, and does it so) */
	guest_call(tvector("glNormal3f", 0), NO_WORDS, DOUBLES(0, 0, 1));
	call_entry((unsigned)opengl_entry("glNormal3bv"), WORDS(ARRAY + 0x20), NULL, 0);
	CHECK(x->st.normal[0] == -1 && x->st.normal[1] == 1 && x->st.normal[2] == (float)(3.0 / 255));
	call_entry((unsigned)opengl_entry("glColor3ub"), WORDS(0xFFFFFF12u, 0x34, 0x56), NULL, 0);
	CHECK(x->st.color[0] == (float)(0x12 / 255.0) && x->st.color[2] == (float)(0x56 / 255.0));

	/* the buffer full: a host call after all (floats as callers pass them: as floats) */
	host_calls = 0;
	uint32_t next = mem_r32(&mem, data + OPENGL_DATA_RECORD);
	mem_w32(&mem, data + OPENGL_DATA_RECORD + 4, next + 8);
	guest_call(tvector("glColor3f", 0), NO_WORDS, DOUBLES(0.125, 0.25, 0.375));
	CHECK_EQ(host_calls, 1);
	CHECK(x->st.color[0] == 0.125f && x->st.color[1] == 0.25f && x->st.color[2] == 0.375f);
	CHECK_EQ(mem_r32(&mem, data + OPENGL_DATA_RECORD), start);
	CHECK_EQ(mem_r32(&mem, data + OPENGL_DATA_RECORD + 4), start + OPENGL_RECORD_SIZE);
	/* through the table of a context that isn't current: a host call, to that context */
	guest_call(tvector("glClearColor", other), WORDS(other), DOUBLES(0.5, 0.5, 0.5, 0.5));
	CHECK_EQ(host_calls, 2);
	CHECK(y->st.clear_color[0] == 0.5f && x->st.clear_color[0] == 0);
	CHECK_EQ(mem_r32(&mem, data + OPENGL_DATA_RECORD), start);

	/* a triangle, recorded, drawn when swapped */
	guest_call(tvector("glLoadIdentity", 0), NO_WORDS, NO_DOUBLES);
	guest_call(tvector("glClear", 0), WORDS(0x4000), NO_DOUBLES);
	guest_call(tvector("glBegin", 0), WORDS(4), NO_DOUBLES);
	guest_call(tvector("glVertex2f", 0), NO_WORDS, DOUBLES(-1, -1));
	guest_call(tvector("glVertex2f", 0), NO_WORDS, DOUBLES(3, -1));
	guest_call(tvector("glVertex2f", 0), NO_WORDS, DOUBLES(-1, 3));
	guest_call(tvector("glEnd", 0), NO_WORDS, NO_DOUBLES);
	CHECK_EQ(host_calls, 2);
	guest_call(tvector("aglSwapBuffers", 0), WORDS(ctx), NO_DOUBLES);
	CHECK_EQ(host_calls, 3);
	CHECK_EQ(mem_r32(&mem, OFFSCREEN + 32 * 7 + 4 * 7), 0xFF204060u);   /* (0.125, 0.25, 0.375) */
	CHECK_EQ(gl.bad_records, 0);

	CALL(aglDestroyContext, other);
	CALL(aglDestroyContext, ctx);
	call_entry(OPENGL_ENTRY_TERM, NULL, 0, NULL, 0);
	if (cpu.jit)
		jit_free(cpu.jit);
	cpu.jit = NULL;
}

/* ---- scenes ------------------------------------------------------------------------ */

#define SW 96                   /* the scenes' size */
#define SH 72

/* A call by name, its arguments as doubles (each made its parameter's type) */
struct op {
	const char *name;
	double a[6];
};

#define ORTHO { "glMatrixMode", { GL_PROJECTION } }, { "glLoadIdentity", { 0 } }, \
	{ "glOrtho", { 0, SW, 0, SH, -1, 1 } }, { "glMatrixMode", { GL_MODELVIEW } }, { "glLoadIdentity", { 0 } }
#define FRUSTUM { "glMatrixMode", { GL_PROJECTION } }, { "glLoadIdentity", { 0 } }, \
	{ "glFrustum", { -1, 1, -0.75, 0.75, 1, 10 } }, { "glMatrixMode", { GL_MODELVIEW } }, \
	{ "glLoadIdentity", { 0 } }
#define CLEAR(r, g, b) { "glClearColor", { r, g, b, 1 } }, { "glClearDepth", { 1 } }, \
	{ "glClear", { GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT } }
#define C3(r, g, b) { "glColor3f", { r, g, b } }
#define C4(r, g, b, a) { "glColor4f", { r, g, b, a } }
#define V2(x, y) { "glVertex2f", { x, y } }
#define V3(x, y, z) { "glVertex3f", { x, y, z } }
#define BEGIN(mode) { "glBegin", { mode } }
#define END { "glEnd", { 0 } }

/* (vertices off pixels' centers and edges, so that rounding doesn't decide what's covered) */
static const struct op scene_gradient[] = {
	ORTHO, CLEAR(0.1, 0.1, 0.1),
	BEGIN(GL_TRIANGLES), C3(1, 0, 0), V2(3.3, 4.1), C3(0, 1, 0), V2(90.7, 9.9), C3(0, 0, 1), V2(20.2, 68.6), END,
	/* (colors as a linear function of position: which diagonal splits the quad doesn't matter) */
	BEGIN(GL_QUADS), C3(0.1325, 0.116667, 0.5), V2(60.3, 40.2), C3(0.94, 0.0583333, 0.5), V2(92.6, 38.1),
	C3(0.9675, 0.936111, 0.5), V2(93.7, 69.7), C3(0.0775, 0.844444, 0.5), V2(58.1, 66.4), END,
	{ NULL, { 0 } },
};

static const struct op scene_depth[] = {
	ORTHO, CLEAR(0, 0, 0), { "glEnable", { GL_DEPTH_TEST } },
	BEGIN(GL_TRIANGLES), C3(1, 0.5, 0), V3(5.3, 5.2, -0.8), V3(90.1, 7.7, 0.8), V3(40.4, 66.9, 0.1), END,
	{ "glDepthFunc", { GL_LEQUAL } },
	BEGIN(GL_TRIANGLES), C3(0, 0.5, 1), V3(10.2, 60.3, 0), V3(85.7, 50.9, 0), V3(50.6, 2.4, 0), END,
	{ "glDepthMask", { 0 } },
	BEGIN(GL_QUADS), C3(0.2, 1, 0.2), V3(30.3, 20.2, 0.9), V3(60.7, 20.2, 0.9), V3(60.7, 40.4, 0.9),
	V3(30.3, 40.4, 0.9), END,
	{ "glDepthMask", { 1 } }, { "glDepthFunc", { GL_GREATER } },
	BEGIN(GL_TRIANGLES), C3(1, 1, 1), V3(2.2, 30.1, 0.5), V3(93.3, 30.6, 0.5), V3(47.9, 70.3, 0.5), END,
	{ NULL, { 0 } },
};

/* in perspective: a floor through the near plane, turned triangles and a quad, a fan; flat or smooth */
#define PERSPECTIVE_SCENE(shade) \
	FRUSTUM, CLEAR(0.1, 0.1, 0.2), { "glEnable", { GL_DEPTH_TEST } }, { "glShadeModel", { shade } }, \
	{ "glTranslatef", { 0, 0, -3 } }, \
	BEGIN(GL_TRIANGLE_STRIP), C3(0.6, 0.6, 0.6), V3(-3, -1, 2.5), C3(0.9, 0.5, 0.5), V3(3, -1, 2.5), \
	C3(0.1, 0.3, 0.1), V3(-3, -1, -6), C3(0.1, 0.1, 0.5), V3(3, -1, -6), END, \
	{ "glPushMatrix", { 0 } }, { "glRotatef", { 25, 0, 1, 0 } }, { "glRotatef", { -10, 1, 0, 0 } }, \
	BEGIN(GL_TRIANGLES), C3(1, 0, 0), V3(-1, -0.8, 0), C3(0, 1, 0), V3(1, -0.8, 0), C3(0, 0, 1), \
	V3(0, 0.9, 0), END, \
	BEGIN(GL_QUADS), C3(1, 0.9, 0.1), V3(-1.2, -0.3, 0.6), V3(1.2, -0.3, 0.6), V3(1.2, 0.3, -0.6), \
	V3(-1.2, 0.3, -0.6), END, \
	{ "glPopMatrix", { 0 } }, \
	BEGIN(GL_TRIANGLE_FAN), C3(1, 1, 0), V3(1.6, 0.6, 0), C3(1, 0, 1), V3(2.2, 0.6, 0), C3(0, 1, 1), \
	V3(2.0, 1.1, 0), C3(1, 1, 1), V3(1.4, 1.2, 0), END, \
	{ NULL, { 0 } }
static const struct op scene_flat[] = { PERSPECTIVE_SCENE(GL_FLAT) };
static const struct op scene_smooth[] = { PERSPECTIVE_SCENE(GL_SMOOTH) };

/* every kind of polygon, culled, in a viewport and scissor box; points */
static const struct op scene_primitives[] = {
	CLEAR(0.2, 0.2, 0.2),
	{ "glViewport", { 6, 5, 80, 60 } }, { "glScissor", { 10, 8, 76, 52 } }, { "glEnable", { GL_SCISSOR_TEST } },
	{ "glMatrixMode", { GL_PROJECTION } }, { "glLoadIdentity", { 0 } }, { "glOrtho", { 0, 80, 0, 60, -1, 1 } },
	{ "glMatrixMode", { GL_MODELVIEW } }, { "glLoadIdentity", { 0 } },
	{ "glEnable", { GL_CULL_FACE } }, { "glShadeModel", { GL_FLAT } },
	BEGIN(GL_QUAD_STRIP), C3(1, 0, 0), V2(-5.3, 20.2), C3(0, 1, 0), V2(-5.3, 3.3), C3(0, 0, 1), V2(20.4, 21.1),
	C3(1, 1, 0), V2(20.4, 3.6), C3(0, 1, 1), V2(40.1, 19.7), C3(1, 0, 1), V2(40.3, 2.8), END,
	BEGIN(GL_POLYGON), C3(1, 0.5, 0), V2(50.2, 5.3), C3(0, 0, 1), V2(70.7, 6.1), C3(0, 1, 0), V2(78.3, 18.6),
	V2(62.6, 27.3), V2(48.1, 17.2), END,
	BEGIN(GL_TRIANGLE_FAN), C3(1, 1, 1), V2(20.3, 40.3), C3(1, 0, 0), V2(35.1, 38.2), C3(0, 1, 0),
	V2(30.2, 55.4), C3(0, 0, 1), V2(8.4, 52.6), C3(1, 1, 0), V2(4.6, 35.2), END,
	{ "glFrontFace", { GL_CW } },
	BEGIN(GL_TRIANGLES), C3(0, 1, 0.5), V2(45.1, 30.2), V2(60.3, 58.4), V2(75.6, 31.1),
	C3(1, 0, 0), V2(45.1, 50.2), V2(55.3, 35.4), V2(65.6, 51.1), END,
	{ "glDisable", { GL_CULL_FACE } }, { "glPointSize", { 4 } },
	BEGIN(GL_POINTS), C3(1, 1, 1), V2(40.3, 30.7), C3(1, 0.5, 0.5), V2(77.6, 58.2), END,
	{ "glPointSize", { 1 } },
	BEGIN(GL_POINTS), C3(0, 1, 1), V2(10.3, 30.7), V2(11.3, 30.7), V2(12.8, 31.9), END,
	{ NULL, { 0 } },
};

/* blending, the alpha test, color masks */
static const struct op scene_blend[] = {
	ORTHO, CLEAR(0.3, 0.3, 0.6),
	{ "glEnable", { GL_BLEND } }, { "glBlendFunc", { GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA } },
	BEGIN(GL_TRIANGLES), C4(1, 0, 0, 0.5), V2(5.3, 5.2), V2(70.1, 7.7), V2(30.4, 60.9), C4(0, 1, 0, 0.25),
	V2(25.2, 10.3), C4(0, 1, 0, 1), V2(90.7, 40.9), V2(40.6, 68.4), END,
	{ "glBlendFunc", { GL_ONE, GL_ONE } }, { "glColorMask", { 1, 0, 1, 1 } },
	BEGIN(GL_QUADS), C4(0.3, 0.3, 0.3, 1), V2(50.3, 2.2), V2(93.7, 2.2), V2(93.7, 30.4), V2(50.3, 30.4), END,
	{ "glColorMask", { 1, 1, 1, 1 } }, { "glDisable", { GL_BLEND } },
	{ "glEnable", { GL_ALPHA_TEST } }, { "glAlphaFunc", { GL_GREATER, 0.5 } },
	BEGIN(GL_TRIANGLES), C4(1, 1, 0, 0), V2(4.3, 40.2), C4(1, 1, 0, 1), V2(40.3, 40.6), C4(1, 0.5, 0, 1),
	V2(4.6, 70.3), END,
	{ NULL, { 0 } },
};

/*
 * Against Mesa's: pixels that may differ by more than 2, and that one draws
 * on and the other doesn't. Those are where a pixel's center is on an edge
 * (where the floor's edges are a third of a pixel down every pixel across,
 * say) or a fragment's alpha is the alpha test's reference, which each
 * decides by its own rounding; and in "smooth", Mesa corrects colors for
 * perspective and ours are linear in screen space (see glfront.h), most
 * of all on the floor that goes from the near plane to the far distance.
 */
static const struct {
	const char *name;
	const struct op *ops;
	unsigned allowed, edges;
} scenes[] = {
	{ "gradient", scene_gradient, 0, 0 }, { "depth", scene_depth, 2, 2 }, { "flat", scene_flat, 12, 12 },
	{ "smooth", scene_smooth, SW * SH / 2, 12 }, { "primitives", scene_primitives, 0, 0 },
	{ "blend", scene_blend, 4, 4 },
};
#define SCENES (sizeof scenes / sizeof scenes[0])

static uint32_t images[2][SCENES][SW * SH];     /* drawn in software, on the GPU: 0xAARRGGBB, rows top first */

static void our_invoke(const struct op *op)
{
	int fn = opengl_entry(op->name);
	CHECK(fn >= 0);
	if (fn < 0)
		return;
	uint32_t w[16] = { 0 };
	double f[13];
	unsigned nw = 0, nf = 0;
	for (const char *c = functions[fn].args; *c; c++) {
		double v = op->a[c - functions[fn].args];
		if (*c == 'f') {                        /* (as a compiled caller passes a float) */
			f[nf++] = (float)v;
			nw++;
		} else if (*c == 'd') {
			f[nf++] = v;
			nw += 2;
		} else {
			w[nw++] = (uint32_t)(int64_t)v;
		}
	}
	call_entry((unsigned)fn, w, nw, f, nf);
}

/* A scene drawn through the library (its offscreen drawable read) */
static void our_scene(const struct op *ops, uint32_t *out)
{
	call_entry(OPENGL_ENTRY_INIT, NULL, 0, NULL, 0);
	make_port(32);
	const uint32_t attribs[] = { AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 24, AGL_NONE };
	put_attribs(attribs, 5);
	uint32_t pf = CALL(aglChoosePixelFormat, 0, 0, ATTRIBS), ctx = CALL(aglCreateContext, pf, 0);
	CHECK_EQ(CALL(aglSetOffScreen, ctx, SW, SH, SW * 4, OFFSCREEN), 1);
	CHECK_EQ(CALL(aglSetCurrentContext, ctx), 1);
	for (const struct op *op = ops; op->name; op++)
		our_invoke(op);
	CHECK_EQ(CALL0(glGetError), 0);
	CALL(aglSwapBuffers, ctx);
	for (unsigned k = 0; k < SW * SH; k++)
		out[k] = mem_r32(&mem, OFFSCREEN + 4 * k);
	CALL(aglDestroyContext, ctx);
	call_entry(OPENGL_ENTRY_TERM, NULL, 0, NULL, 0);
}

/* SYSHM_TEST_SAVE=DIR: each scene's pixels saved there, as SCENE-soft.png, SCENE-gpu.png and SCENE-mesa.png */
static void save(const char *what, unsigned k, const uint32_t *img)
{
	const char *dir = getenv("SYSHM_TEST_SAVE");
	if (!dir)
		return;
	static uint8_t rgb[SW * SH * 3];
	for (unsigned i = 0; i < SW * SH; i++) {
		rgb[3 * i] = (uint8_t)(img[i] >> 16);
		rgb[3 * i + 1] = (uint8_t)(img[i] >> 8);
		rgb[3 * i + 2] = (uint8_t)img[i];
	}
	char path[512];
	snprintf(path, sizeof path, "%s/%s-%s.png", dir, scenes[k].name, what);
	png_write_rgb(path, rgb, SW, SH);
}

static void test_scenes(unsigned backend)
{
	for (unsigned k = 0; k < SCENES; k++) {
		our_scene(scenes[k].ops, images[backend][k]);
		save(backend ? "gpu" : "soft", k, images[backend][k]);
	}
}

static unsigned channel_diff(uint32_t a, uint32_t b)
{
	unsigned worst = 0;
	for (int sh = 0; sh < 32; sh += 8) {
		int d = (int)(a >> sh & 255) - (int)(b >> sh & 255);
		unsigned ad = (unsigned)(d < 0 ? -d : d);
		worst = ad > worst ? ad : worst;
	}
	return worst;
}

/* Pixels differing by more than `tolerance` in some channel; the most any differs by */
static unsigned differing(const uint32_t *a, const uint32_t *b, unsigned tolerance, unsigned *most)
{
	unsigned n = 0;
	*most = 0;
	for (unsigned k = 0; k < SW * SH; k++) {
		unsigned d = channel_diff(a[k], b[k]);
		n += d > tolerance;
		*most = d > *most ? d : *most;
	}
	return n;
}

/* SYSHM_TEST_VERBOSE=1: how far apart the scenes are */
static bool verbose(void)
{
	const char *v = getenv("SYSHM_TEST_VERBOSE");
	return v && strcmp(v, "0");
}

#ifdef __linux__
#include <dlfcn.h>

/*
 * Mesa's OpenGL, the reference: a compatibility-profile context on EGL's
 * surfaceless platform, drawing into a framebuffer object
 */
enum { EGL_NONE_ = 0x3038, EGL_RENDERABLE_TYPE_ = 0x3040, EGL_SURFACE_TYPE_ = 0x3033, EGL_OPENGL_BIT_ = 0x0008,
       EGL_OPENGL_API_ = 0x30A2, EGL_PLATFORM_SURFACELESS_MESA_ = 0x31DD };
static struct {
	void *lib, *gl_lib, *display, *config;
	void (*(*GetProcAddress)(const char *))(void);
	void *(*GetPlatformDisplay)(unsigned, void *, const intptr_t *);
	unsigned (*Initialize)(void *, int32_t *, int32_t *);
	unsigned (*BindAPI)(unsigned);
	unsigned (*ChooseConfig)(void *, const int32_t *, void **, int32_t, int32_t *);
	void *(*CreateContext)(void *, void *, void *, const int32_t *);
	unsigned (*DestroyContext)(void *, void *);
	unsigned (*MakeCurrent)(void *, void *, void *, void *);
} egl;

static void (*mesa_fn(const char *name))(void)
{
	void (*f)(void) = egl.GetProcAddress(name);
	if (!f && egl.gl_lib)
		*(void **)&f = dlsym(egl.gl_lib, name);
	return f;
}

static bool mesa_open(void)
{
	if (egl.display)
		return true;
	egl.lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!egl.lib)
		return false;
	*(void **)&egl.GetProcAddress = dlsym(egl.lib, "eglGetProcAddress");
	*(void **)&egl.GetPlatformDisplay = dlsym(egl.lib, "eglGetPlatformDisplay");
	*(void **)&egl.Initialize = dlsym(egl.lib, "eglInitialize");
	*(void **)&egl.BindAPI = dlsym(egl.lib, "eglBindAPI");
	*(void **)&egl.ChooseConfig = dlsym(egl.lib, "eglChooseConfig");
	*(void **)&egl.CreateContext = dlsym(egl.lib, "eglCreateContext");
	*(void **)&egl.DestroyContext = dlsym(egl.lib, "eglDestroyContext");
	*(void **)&egl.MakeCurrent = dlsym(egl.lib, "eglMakeCurrent");
	if (!egl.GetProcAddress || !egl.GetPlatformDisplay || !egl.Initialize || !egl.BindAPI || !egl.ChooseConfig ||
	    !egl.CreateContext || !egl.DestroyContext || !egl.MakeCurrent)
		return false;
	egl.gl_lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
	void *d = egl.GetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA_, NULL, NULL);
	int32_t major, minor, n = 0;
	const int32_t attribs[] = { EGL_RENDERABLE_TYPE_, EGL_OPENGL_BIT_, EGL_SURFACE_TYPE_, 0, EGL_NONE_ };
	if (!d || !egl.Initialize(d, &major, &minor) || !egl.BindAPI(EGL_OPENGL_API_) ||
	    !egl.ChooseConfig(d, attribs, &egl.config, 1, &n) || n < 1)
		return false;
	egl.display = d;
	return true;
}

#define GLFN(type, name) ((type)mesa_fn(name))

/* A call by its signature (those the scenes use) */
static void mesa_invoke(const struct op *op)
{
	int fn = opengl_entry(op->name);
	void (*p)(void) = fn >= 0 ? mesa_fn(op->name) : NULL;
	CHECK(p != NULL);
	if (!p)
		return;
	const char *sig = functions[fn].args;
	const double *a = op->a;
#define U(k) ((uint32_t)(int64_t)a[k])
#define F(k) ((float)a[k])
#define B(k) ((uint8_t)a[k])
	if (!strcmp(sig, ""))
		((void (*)(void))p)();
	else if (!strcmp(sig, "I"))
		((void (*)(uint32_t))p)(U(0));
	else if (!strcmp(sig, "II"))
		((void (*)(uint32_t, uint32_t))p)(U(0), U(1));
	else if (!strcmp(sig, "If"))
		((void (*)(uint32_t, float))p)(U(0), F(1));
	else if (!strcmp(sig, "f"))
		((void (*)(float))p)(F(0));
	else if (!strcmp(sig, "ff"))
		((void (*)(float, float))p)(F(0), F(1));
	else if (!strcmp(sig, "fff"))
		((void (*)(float, float, float))p)(F(0), F(1), F(2));
	else if (!strcmp(sig, "ffff"))
		((void (*)(float, float, float, float))p)(F(0), F(1), F(2), F(3));
	else if (!strcmp(sig, "d"))
		((void (*)(double))p)(a[0]);
	else if (!strcmp(sig, "dddddd"))
		((void (*)(double, double, double, double, double, double))p)(a[0], a[1], a[2], a[3], a[4], a[5]);
	else if (!strcmp(sig, "B"))
		((void (*)(uint8_t))p)(B(0));
	else if (!strcmp(sig, "BBBB"))
		((void (*)(uint8_t, uint8_t, uint8_t, uint8_t))p)(B(0), B(1), B(2), B(3));
	else if (!strcmp(sig, "iiii"))
		((void (*)(int32_t, int32_t, int32_t, int32_t))p)((int32_t)a[0], (int32_t)a[1], (int32_t)a[2],
								 (int32_t)a[3]);
	else
		CHECK(!"a signature mesa_invoke doesn't take");
#undef U
#undef F
#undef B
}

/* A scene drawn by Mesa (a fresh context's), as our_scene() reads ours */
static bool mesa_scene(const struct op *ops, uint32_t *out)
{
	const int32_t none[] = { EGL_NONE_ };
	void *ctx = egl.CreateContext(egl.display, egl.config, NULL, none);
	if (!ctx || !egl.MakeCurrent(egl.display, NULL, NULL, ctx))
		return false;
	enum { GL_FRAMEBUFFER = 0x8D40, GL_RENDERBUFFER = 0x8D41, GL_RGBA8 = 0x8058, GL_DEPTH_COMPONENT24 = 0x81A6,
	       GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00, GL_RGBA = 0x1908,
	       GL_UNSIGNED_BYTE = 0x1401 };
	uint32_t fbo, rb[2];
	GLFN(void (*)(int32_t, uint32_t *), "glGenFramebuffers")(1, &fbo);
	GLFN(void (*)(int32_t, uint32_t *), "glGenRenderbuffers")(2, rb);
	GLFN(void (*)(uint32_t, uint32_t), "glBindFramebuffer")(GL_FRAMEBUFFER, fbo);
	for (int k = 0; k < 2; k++) {
		GLFN(void (*)(uint32_t, uint32_t), "glBindRenderbuffer")(GL_RENDERBUFFER, rb[k]);
		GLFN(void (*)(uint32_t, uint32_t, int32_t, int32_t), "glRenderbufferStorage")(
			GL_RENDERBUFFER, k ? GL_DEPTH_COMPONENT24 : GL_RGBA8, SW, SH);
		GLFN(void (*)(uint32_t, uint32_t, uint32_t, uint32_t), "glFramebufferRenderbuffer")(
			GL_FRAMEBUFFER, k ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[k]);
	}
	/* (as ours start: the viewport and scissor box the drawable's) */
	GLFN(void (*)(int32_t, int32_t, int32_t, int32_t), "glViewport")(0, 0, SW, SH);
	GLFN(void (*)(int32_t, int32_t, int32_t, int32_t), "glScissor")(0, 0, SW, SH);
	for (const struct op *op = ops; op->name; op++)
		mesa_invoke(op);
	CHECK_EQ(GLFN(uint32_t (*)(void), "glGetError")(), 0);
	static uint8_t rgba[SW * SH * 4];
	GLFN(void (*)(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void *), "glReadPixels")(
		0, 0, SW, SH, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	for (int y = 0; y < SH; y++)                    /* (OpenGL's rows go up) */
		for (int x = 0; x < SW; x++) {
			const uint8_t *p = rgba + 4 * ((SH - 1 - y) * SW + x);
			out[y * SW + x] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
		}
	GLFN(void (*)(int32_t, const uint32_t *), "glDeleteFramebuffers")(1, &fbo);
	GLFN(void (*)(int32_t, const uint32_t *), "glDeleteRenderbuffers")(2, rb);
	egl.MakeCurrent(egl.display, NULL, NULL, NULL);
	egl.DestroyContext(egl.display, ctx);
	return true;
}

/* Ours against Mesa's */
static void test_mesa(void)
{
	if (!mesa_open()) {
		const char *need = getenv("SYSHM_TEST_GPU");
		printf("opengl: no Mesa to compare with\n");
		CHECK(!(need && !strcmp(need, "1")));
		return;
	}
	static uint32_t ref[SW * SH];
	for (unsigned k = 0; k < SCENES; k++) {
		CHECK(mesa_scene(scenes[k].ops, ref));
		save("mesa", k, ref);
		unsigned most, n = differing(images[0][k], ref, 2, &most);
		unsigned covered = 0;                   /* (pixels one draws on and the other doesn't) */
		for (unsigned i = 0; i < SW * SH; i++)
			covered += (images[0][k][i] == images[0][k][0]) != (ref[i] == ref[0]);
		if (verbose())
			printf("opengl: %-10s against Mesa: %u pixels differ by more than 2, the most by %u; "
			       "%u covered by one only\n", scenes[k].name, n, most, covered);
		CHECK(n <= scenes[k].allowed);
		CHECK(covered <= scenes[k].edges);
	}
}
#endif

static void run(struct gpu *gpu)
{
	if (mem_init(&mem)) {
		CHECK(!"mem_init failed");
		return;
	}
	mem_map(&mem, 0, 0x200000, MEM_RW);
	mem_map(&mem, CODE, CODE_SIZE, MEM_RWX);
	opengl_init(&gl, &mem);
	gl.gpu = gpu;
	test_fragment();
	test_agl();
	test_memory();
	CHECK_EQ(gl.gpu_frames, gpu ? 4 : 0);
	test_recording(false);
	test_recording(true);
	test_scenes(gpu != NULL);
	opengl_free(&gl);
	mem_free(&mem);
}

void test_opengl(void)
{
	run(NULL);
	/* the same through the GPU backend, if there's OpenGL (any: llvmpipe will do): the same but on some edges */
	struct gpu *g = gpu_open(true);
	if (g && gpu_ready(g, NULL)) {
		run(g);
		for (unsigned k = 0; k < SCENES; k++) {
			unsigned most, n = differing(images[0][k], images[1][k], 2, &most);
			if (verbose())
				printf("opengl: %-10s on the GPU: %u pixels differ by more than 2, the most by %u\n",
				       scenes[k].name, n, most);
			CHECK(n <= 16);
		}
	}
	gpu_close(g);
#ifdef __linux__
	test_mesa();
#endif
}
