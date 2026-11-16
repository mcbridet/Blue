/*
 * The guest's OpenGL libraries' host side, driven as the guest's calls
 * would drive it: pixel formats and renderer info (ATI's renderer's and the
 * generic one's), contexts with Apple's dispatch table, drawables (a
 * window's port, clipped to its regions, dithered at 16 bits; an
 * offscreen buffer), clears and swaps, strings and errors, the dispatch
 * table's calls; OpenGLMemory's allocations; the fragments themselves.
 * OpenGLLibrary's own code, run by the processor (interpreted and by the
 * JIT), recording calls. OpenGLUtility's. Scenes drawn through it all in
 * software and on the GPU, and (on Linux) by Mesa's OpenGL in its
 * compatibility profile, the reference (and SGI's GLU, Mesa's libGLU, if
 * BLUE_TEST_GLU is where it is); selection's and feedback's records,
 * against Mesa's too.
 */
#include <math.h>
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
#define GLU_TOC 0x3C000u        /* OpenGLUtility's */
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
enum { AGL_NONE, AGL_RGBA = 4, AGL_DOUBLEBUFFER = 5, AGL_AUX_BUFFERS = 7, AGL_DEPTH_SIZE = 12, AGL_STENCIL_SIZE = 13,
       AGL_ACCUM_RED_SIZE = 14, AGL_ACCUM_GREEN_SIZE, AGL_ACCUM_BLUE_SIZE, AGL_ACCUM_ALPHA_SIZE, AGL_PIXEL_SIZE = 50,
       AGL_FULLSCREEN = 54, AGL_RENDERER_ID = 70, AGL_ACCELERATED = 73, AGL_ACCUM_MODES = 104, AGL_VIDEO_MEMORY = 120,
       AGL_SWAP_RECT = 200 };
enum { AGL_BAD_ATTRIBUTE = 10000, AGL_BAD_RENDINFO = 10003, AGL_BAD_CONTEXT = 10004, AGL_BAD_DRAWABLE = 10005 };
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

/* OpenGLUtility's functions: their names and arguments' types */
static const struct {
	const char *name, *args;
} glu_functions[] = {
#define X(name, result, args, offset) { #name, args },
	OPENGL_UTILITY_FUNCTIONS(X)
#undef X
};

static unsigned glu_entry(const char *name)
{
	for (unsigned k = 0; k < sizeof glu_functions / sizeof glu_functions[0]; k++)
		if (!strcmp(glu_functions[k].name, name))
			return k;
	CHECK(!"no such glu function");
	return 0;
}

/*
 * A call to OpenGLUtility, its callouts answered: NewPtrClear's blocks
 * from GLU_HEAP on, DisposePtr's counted, others (quadrics' error
 * callbacks) noted
 */
#define GLU_HEAP 0x4C000u
#define NEW_PTR_CLEAR_TV 0x7040u
#define DISPOSE_PTR_TV 0x7050u
#define ERROR_TV 0x7060u
static uint32_t glu_heap = GLU_HEAP, glu_disposed, glu_error;
static unsigned glu_errors;

static uint32_t call_glu(const char *name, const uint32_t *w, unsigned nw, const double *f, unsigned nf)
{
	struct ndrv_call c;
	memset(&c, 0, sizeof c);
	c.mem = &mem;
	c.entry = glu_entry(name);
	c.toc = GLU_TOC;
	c.frame = FRAME;
	for (unsigned j = 0; j < nw && j < 8; j++)
		c.arg[j] = w[j];
	for (unsigned j = 8; j < nw; j++)
		mem_w32(&mem, FRAME + NDRV_FRAME_SIZE + 24 + 4 * j, w[j]);
	for (unsigned j = 0; j < nf; j++)
		memcpy(&c.f[j], &f[j], 8);
	opengl_utility_call(&gl, &c);
	for (unsigned n = 0; !c.done && n < 4; n++) {
		uint32_t r = 0;
		if (c.tvector == NEW_PTR_CLEAR_TV) {
			r = glu_heap;
			memset(mem_ptr(&mem, r), 0, c.out[0]);
			glu_heap += (c.out[0] + 15) & ~15u;
		} else if (c.tvector == DISPOSE_PTR_TV) {
			glu_disposed = c.out[0];
		} else {
			CHECK_EQ(c.tvector, ERROR_TV);
			glu_errors++;
			glu_error = c.out[0];
		}
		c.entry = NDRV_ENTRY_RESUME;
		c.arg[0] = r;
		opengl_utility_call(&gl, &c);
	}
	CHECK(c.done);
	return c.result;
}

#define CALLU(name, ...) call_glu(#name, (const uint32_t[]){ __VA_ARGS__ }, sizeof (uint32_t[]){ __VA_ARGS__ } / 4, \
				  NULL, 0)
#define CALLU0(name) call_glu(#name, NULL, 0, NULL, 0)

/* OpenGLUtility as the Code Fragment Manager loads it (its data at GLU_TOC, its imports resolved) */
static void glu_load(void)
{
	static uint8_t buf[OPENGL_LIBRARY_MAX];
	struct ndrv_layout l;
	CHECK(opengl_build_utility(buf, sizeof buf, &l) > 0);
	const uint8_t *section = buf + 40 + 28;
	memset(mem_ptr(&mem, GLU_TOC), 0, be32(section + 8));
	memcpy(mem_ptr(&mem, GLU_TOC), buf + be32(section + 20), be32(section + 12));
	mem_w32(&mem, GLU_TOC + gl.utility.import[0], TOC + gl.library.extra + OPENGL_DATA_CURRENT);
	mem_w32(&mem, GLU_TOC + gl.utility.import[1], NEW_PTR_CLEAR_TV);
	mem_w32(&mem, GLU_TOC + gl.utility.import[2], DISPOSE_PTR_TV);
}

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
	const uint32_t bad[] = { AGL_RGBA, AGL_AUX_BUFFERS, 1, AGL_NONE };
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
	uint32_t next = CALL(aglNextRendererInfo, ri);         /* (the generic renderer) */
	CHECK(next != 0);
	CHECK_EQ(CALL(aglDescribeRenderer, next, AGL_RENDERER_ID, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 0x00020200);
	CHECK_EQ(CALL(aglDescribeRenderer, next, AGL_ACCELERATED, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 0);
	CHECK_EQ(CALL(aglDescribeRenderer, next, AGL_ACCUM_MODES, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 0x00C0C000);
	CHECK_EQ(CALL(aglDescribeRenderer, ri, AGL_ACCUM_MODES, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 0);
	CHECK_EQ(CALL(aglNextRendererInfo, next), 0);
	CALL(aglDestroyRendererInfo, ri);
	CHECK_EQ(CALL(aglDescribeRenderer, next, AGL_RENDERER_ID, OUT), 0);
	CHECK_EQ(CALL0(aglGetError), AGL_BAD_RENDINFO);
	/* an accumulation buffer: the generic renderer's, not accelerated (none if that's asked for) */
	const uint32_t accum[] = { AGL_RGBA, AGL_ACCUM_RED_SIZE, 8, AGL_ACCUM_GREEN_SIZE, 8, AGL_ACCUM_BLUE_SIZE, 8,
				   AGL_NONE };
	put_attribs(accum, 8);
	uint32_t apf = CALL(aglChoosePixelFormat, 0, 0, ATTRIBS);
	CHECK(apf != 0);
	CHECK_EQ(CALL(aglDescribePixelFormat, apf, AGL_RENDERER_ID, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 0x00020200);
	CHECK_EQ(CALL(aglDescribePixelFormat, apf, AGL_ACCELERATED, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 0);
	CHECK_EQ(CALL(aglDescribePixelFormat, apf, AGL_ACCUM_GREEN_SIZE, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 8);
	CHECK_EQ(CALL(aglDescribePixelFormat, apf, AGL_ACCUM_ALPHA_SIZE, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 0);
	CALL(aglDestroyPixelFormat, apf);
	const uint32_t deep[] = { AGL_RGBA, AGL_ACCUM_RED_SIZE, 12, AGL_ACCUM_ALPHA_SIZE, 1, AGL_NONE };
	put_attribs(deep, 6);
	apf = CALL(aglChoosePixelFormat, 0, 0, ATTRIBS);
	CHECK(apf != 0);
	CHECK_EQ(CALL(aglDescribePixelFormat, apf, AGL_ACCUM_BLUE_SIZE, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 16);
	CHECK_EQ(CALL(aglDescribePixelFormat, apf, AGL_ACCUM_ALPHA_SIZE, OUT), 1);
	CHECK_EQ(mem_r32(&mem, OUT), 16);
	/*
	 * (its contexts: the generic renderer's strings, as Mac OS 9.2.2's engine gave them: the renderer's
	 * version, the engine's extensions and then the renderer's, each followed by a space)
	 */
	uint32_t actx = CALL(aglCreateContext, apf, 0);
	CHECK(actx != 0);
	CHECK_EQ(CALL(aglSetCurrentContext, actx), 1);
	CHECK(!strcmp((const char *)mem_ptr(&mem, CALL(glGetString, 0x1F00)), "Apple"));
	CHECK(!strcmp((const char *)mem_ptr(&mem, CALL(glGetString, 0x1F01)), "Generic"));
	CHECK(!strcmp((const char *)mem_ptr(&mem, CALL(glGetString, 0x1F02)), "1.1"));
	CHECK(!strcmp((const char *)mem_ptr(&mem, CALL(glGetString, 0x1F03)),
		      "GL_APPLE_specular_vector GL_APPLE_transform_hint GL_APPLE_packed_pixel GL_ARB_transpose_matrix "
		      "GL_EXT_clip_volume_hint GL_EXT_rescale_normal GL_NV_texgen_reflection GL_ARB_texture_env_add "
		      "GL_EXT_blend_color GL_EXT_blend_minmax GL_EXT_blend_subtract GL_EXT_texture_env_add "
		      "GL_EXT_texture_lod_bias GL_EXT_abgr GL_EXT_bgra GL_SGIS_texture_edge_clamp GL_ARB_multitexture "));
	CHECK_EQ(CALL0(glGetError), 0);
	CALL(glGetIntegerv, 0x0D5A, OUT);                       /* GL_ACCUM_BLUE_BITS */
	CHECK_EQ(mem_r32(&mem, OUT), 16);
	CALL(glGetIntegerv, 0x0D5B, OUT);                       /* GL_ACCUM_ALPHA_BITS */
	CHECK_EQ(mem_r32(&mem, OUT), 16);
	CHECK_EQ(CALL(aglSetCurrentContext, 0), 1);
	CHECK_EQ(CALL(aglDestroyContext, actx), 1);
	CALL(aglDestroyPixelFormat, apf);
	const uint32_t refused[][6] = { { AGL_RGBA, AGL_ACCUM_RED_SIZE, 8, AGL_ACCELERATED, AGL_NONE },
					{ AGL_RGBA, AGL_ACCUM_RED_SIZE, 8, AGL_RENDERER_ID, 0x00021000, AGL_NONE },
					{ AGL_RGBA, AGL_ACCUM_RED_SIZE, 24, AGL_NONE },
					{ AGL_RGBA, AGL_ACCUM_RED_SIZE, 8, AGL_FULLSCREEN, AGL_NONE } };
	for (unsigned k = 0; k < 4; k++) {
		put_attribs(refused[k], 6);
		CHECK_EQ(CALL(aglChoosePixelFormat, 0, 0, ATTRIBS), 0);
	}
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

	/* strings (ATI's renderer's: Blue's version, what Blue does of the engine's extensions), errors, the viewport */
	const char *renderer = (const char *)mem_ptr(&mem, CALL(glGetString, 0x1F01));
	CHECK(!strcmp(renderer, "Rage 128 OpenGL Engine"));
	CHECK(!strcmp((const char *)mem_ptr(&mem, CALL(glGetString, 0x1F00)), "ATI Technologies Inc."));
	CHECK(!strcmp((const char *)mem_ptr(&mem, CALL(glGetString, 0x1F02)), "1.2 Blue"));
	const char *ext = (const char *)mem_ptr(&mem, CALL(glGetString, 0x1F03));
	CHECK(!strcmp(ext, "GL_APPLE_specular_vector GL_APPLE_transform_hint GL_APPLE_packed_pixel "
			   "GL_ARB_transpose_matrix GL_EXT_clip_volume_hint GL_EXT_rescale_normal GL_NV_texgen_reflection "
			   "GL_ARB_multitexture GL_ARB_texture_env_add GL_EXT_blend_color GL_EXT_blend_minmax "
			   "GL_EXT_blend_subtract GL_EXT_compiled_vertex_array GL_EXT_texture_env_add "
			   "GL_EXT_texture_env_combine GL_EXT_texture_lod_bias GL_EXT_abgr GL_EXT_bgra "
			   "GL_SGIS_texture_edge_clamp"));
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
	CHECK(gl.shown && gl.shown_id == ctx);                  /* (a frame for the screen: paced, see nk.c) */
	CALL0(glGetError);
	CHECK_EQ(gl.shown, 0);
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
	/* dithered (GL_DITHER, on at first): 128 between 15 and 16 in 5 bits, 9 of a 4 x 4 block rounding up; or not */
	CALL_F(glClearColor, 0.5, 0.5, 0.5, 1.0);
	CALL(glClear, GL_COLOR_BUFFER_BIT);
	for (int off = 0; off < 2; off++) {
		CALL(aglSwapBuffers, ctx);
		unsigned up = 0, down = 0;
		for (int y = 4; y < 8; y++)
			for (int x = 4; x < 8; x++) {
				uint32_t c = screen(x, y, 16);
				CHECK_EQ(c >> 16 & 0xFF, c >> 8 & 0xFF);
				up += (c >> 19 & 31) == 16;
				down += (c >> 19 & 31) == 15;
			}
		CHECK_EQ(up, off ? 16u : 9u);
		CHECK_EQ(down, off ? 0u : 7u);
		CALL(glDisable, GL_DITHER);
	}
	CALL(glEnable, GL_DITHER);

	/* offscreen: rows of 32 bits a pixel with alpha */
	CHECK_EQ(CALL(aglSetOffScreen, ctx, 4, 2, 32, OFFSCREEN), 1);
	CALL_F(glClearColor, 0.0, 0.0, 1.0, 0.5);
	CALL(glClear, GL_COLOR_BUFFER_BIT);
	CALL0(glFinish);
	CHECK_EQ(mem_r32(&mem, OFFSCREEN), 0);                  /* (double buffered: not until swapped) */
	CALL(aglSwapBuffers, ctx);
	CHECK_EQ(mem_r32(&mem, OFFSCREEN + 32 + 12), 0x800000FFu);
	CHECK_EQ(gl.shown, 0);                                  /* (offscreen: not paced) */

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
		setenv("BLUE_JIT_HOT", "0", 1);         /* (translated at once) */
		cpu.jit = jit_create(&mem);
		unsetenv("BLUE_JIT_HOT");
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
	double a[12];
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

/* Textures: (glTexImage2D's arguments: target, level, internal format, width, height, then the image's number) */
#define TEXDATA 0x120000u       /* (the image, in the guest's memory) */
#define TEX(name, w, h, image) { "glTexImage2D", { 0x0DE1, 0, 0x8058, w, h, image } }
#define TEXLEVEL(level, w, h, image) { "glTexImage2D", { 0x0DE1, level, 0x8058, w, h, image } }
#define TEXP(pname, v) { "glTexParameteri", { 0x0DE1, pname, v } }
#define TC(s, t) { "glTexCoord2f", { s, t } }
enum { IMAGE_CHECKER, IMAGE_GRADIENT, IMAGE_LEVEL0, IMAGES = IMAGE_LEVEL0 + 6 };

/* An image's RGBA bytes (8 x 8 at most; IMAGE_LEVEL0 + k, a color of its own, 32 x 32) */
static const uint8_t *image(unsigned k, int w, int h)
{
	static const uint8_t level[6][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 },
					     { 0, 255, 255 }, { 255, 0, 255 } };
	static uint8_t rgba[IMAGES][32 * 32 * 4];
	uint8_t *p = rgba[k];
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++, p += 4) {
			if (k >= IMAGE_LEVEL0) {
				memcpy(p, level[k - IMAGE_LEVEL0], 3);
				p[3] = 255;
			} else if (k == IMAGE_CHECKER) {        /* (four colors, alpha falling) */
				p[0] = (x + y) & 1 ? 230 : 40;
				p[1] = x < w / 2 ? 200 : 60;
				p[2] = y < h / 2 ? 50 : 210;
				p[3] = (uint8_t)(255 - 20 * x);
			} else {                        /* (smooth, bright to its edges) */
				p[0] = (uint8_t)(40 + 50 * x);
				p[1] = (uint8_t)(220 - 40 * y);
				p[2] = 180;
				p[3] = 255;
			}
		}
	return rgba[k];
}

/*
 * Arrays the scenes draw from (glVertexPointer's and the rest, and
 * glDrawElements' indices take one's number for the pointer): in the guest's
 * memory big-endian, as a program has them, and Mesa's in the host's order
 */
#define VECDATA 0x130000u       /* (a parameter vector: glLightfv's, glFogfv's...) */
#define ARRAYDATA 0x140000u     /* (array k at ARRAYDATA + k * 0x1000) */
enum { ARR_MESH_POS, ARR_MESH_NORMAL, ARR_MESH_INDEX, ARR_SHORT_POS, ARR_UBYTE_COLOR, ARR_DOUBLE_POS, ARR_FLOAT_RGB,
       ARR_BYTE_INDEX, ARR_INTERLEAVED, ARR_UINT_INDEX, ARR_SECONDARY, ARR_BYTE_NORMAL, ARR_LIST_UBYTE,
       ARR_LIST_2BYTES, ARR_LIST_FLOAT, ARR_PIX_RGBA, ARR_PIX_RGB, ARR_PIX_LUM, ARR_MAP4, ARR_BITMAP, ARR_PIX_DEPTH,
       ARR_PIX_STENCIL, ARR_MAP_IDENTITY, ARR_VOLUME, ARR_SLICE, ARR_MIP, ARR_EVAL_CURVE, ARR_EVAL_COLORS,
       ARR_EVAL_ARC, ARR_EVAL_PATCH, ARR_EVAL_BENT, ARR_EVAL_PATCH_COLORS, ARR_EVAL_PATCH_TEX, ARR_EVAL_BUMPS,
       ARR_EVAL_CYLINDER, ARR_EVAL_FLAT, ARR_EVAL_NORMALS, ARR_STIPPLE, ARRAYS };
#define MESH_X 9                /* (a mesh's vertices: a bent sheet, its normals) */
#define MESH_Y 7
#define MESH_INDICES ((MESH_X - 1) * (MESH_Y - 1) * 6)

static bool host_big(void)
{
	const uint16_t one = 1;
	uint8_t b;
	memcpy(&b, &one, 1);
	return b == 0;
}

/* A value of a type at p, big-endian or the host's way; its size */
static size_t put_value(uint8_t *p, uint32_t type, double v, bool big)
{
	uint8_t b[8];
	size_t n;
	switch (type) {
	case GL_BYTE: case GL_UNSIGNED_BYTE: p[0] = (uint8_t)(int)v; return 1;
	case GL_SHORT: case GL_UNSIGNED_SHORT: { uint16_t s = (uint16_t)(int32_t)v; memcpy(b, &s, 2); n = 2; break; }
	case GL_INT: case GL_UNSIGNED_INT: { uint32_t s = (uint32_t)(int64_t)v; memcpy(b, &s, 4); n = 4; break; }
	case GL_FLOAT: { float f = (float)v; memcpy(b, &f, 4); n = 4; break; }
	default: memcpy(b, &v, 8); n = 8; break;
	}
	for (size_t k = 0; k < n; k++)
		p[k] = big != host_big() ? b[n - 1 - k] : b[k];
	return n;
}

static void mesh_vertex(int i, int j, float v[3], float n[3])
{
	v[0] = -1.2f + 0.3f * (float)i + 0.011f;
	v[1] = -0.9f + 0.3f * (float)j + 0.007f;
	v[2] = 0.6f * (1 - v[0] * v[0] / 1.44f);
	float nx = 1.2f * v[0] / 1.44f, len = sqrtf(nx * nx + 1);
	n[0] = nx / len;
	n[1] = 0;
	n[2] = 1 / len;
}

/* Array k's bytes (big-endian, or the host's way) into out; how many */
static size_t array_bytes(unsigned k, bool big, uint8_t *out)
{
	uint8_t *p = out;
#define PUT(type, v) (p += put_value(p, type, v, big))
	switch (k) {
	case ARR_MESH_POS:
	case ARR_MESH_NORMAL:
	case ARR_BYTE_NORMAL:
		for (int j = 0; j < MESH_Y; j++)
			for (int i = 0; i < MESH_X; i++) {
				float v[3], n[3];
				mesh_vertex(i, j, v, n);
				for (int c = 0; c < 3; c++) {
					if (k == ARR_MESH_POS)
						PUT(GL_FLOAT, v[c]);
					else if (k == ARR_MESH_NORMAL)
						PUT(GL_FLOAT, n[c]);
					else
						PUT(GL_BYTE, floor(n[c] * 127 + 0.5));
				}
				if (k == ARR_BYTE_NORMAL)
					PUT(GL_BYTE, 0);                /* (a stride of 4) */
			}
		break;
	case ARR_MESH_INDEX:
		for (int j = 0; j + 1 < MESH_Y; j++)
			for (int i = 0; i + 1 < MESH_X; i++) {
				int a = j * MESH_X + i, b = a + 1, c = a + MESH_X + 1, d = a + MESH_X;
				const int tri[6] = { a, b, c, a, c, d };
				for (int t = 0; t < 6; t++)
					PUT(GL_UNSIGNED_SHORT, tri[t]);
			}
		break;
	case ARR_SHORT_POS:                     /* (a strip, x and y) */
		for (int v = 0; v < 8; v++) {
			PUT(GL_SHORT, 4 + 10 * v);
			PUT(GL_SHORT, v & 1 ? 21 : 5);
		}
		break;
	case ARR_UBYTE_COLOR:
		for (int v = 0; v < 8; v++) {
			PUT(GL_UNSIGNED_BYTE, 30 * v);
			PUT(GL_UNSIGNED_BYTE, 255 - 25 * v);
			PUT(GL_UNSIGNED_BYTE, v & 1 ? 200 : 60);
			PUT(GL_UNSIGNED_BYTE, 255);
		}
		break;
	case ARR_DOUBLE_POS: {                  /* (two triangles) */
		static const double xy[6][2] = { { 4.3, 26.2 }, { 40.7, 28.1 }, { 18.4, 46.6 }, { 44.1, 26.3 },
						 { 52.2, 48.9 }, { 30.6, 47.7 } };
		for (int v = 0; v < 6; v++) {
			PUT(GL_DOUBLE, xy[v][0]);
			PUT(GL_DOUBLE, xy[v][1]);
			PUT(GL_DOUBLE, -0.2 * v);
		}
		break;
	}
	case ARR_FLOAT_RGB:
		for (int v = 0; v < 6; v++) {
			PUT(GL_FLOAT, v & 1 ? 0.9 : 0.1);
			PUT(GL_FLOAT, 0.15 * v);
			PUT(GL_FLOAT, v < 3 ? 0.8 : 0.3);
		}
		break;
	case ARR_BYTE_INDEX: {
		static const int idx[6] = { 2, 0, 1, 5, 3, 4 };
		for (int v = 0; v < 6; v++)
			PUT(GL_UNSIGNED_BYTE, idx[v]);
		break;
	}
	case ARR_INTERLEAVED: {                 /* (GL_T2F_C4UB_V3F: a quad) */
		static const double q[4][7] = { { 0, 0, 255, 255, 255, 255, 0 }, { 1, 0, 255, 128, 128, 255, 0 },
						{ 1, 1, 128, 255, 128, 255, 0 }, { 0, 1, 128, 128, 255, 255, 0 } };
		static const double xy[4][2] = { { 58.3, 4.4 }, { 92.6, 6.1 }, { 90.2, 40.7 }, { 60.1, 38.3 } };
		for (int v = 0; v < 4; v++) {
			PUT(GL_FLOAT, q[v][0]);
			PUT(GL_FLOAT, q[v][1]);
			for (int c = 0; c < 4; c++)
				PUT(GL_UNSIGNED_BYTE, q[v][2 + c]);
			PUT(GL_FLOAT, xy[v][0]);
			PUT(GL_FLOAT, xy[v][1]);
			PUT(GL_FLOAT, 0);
		}
		break;
	}
	case ARR_UINT_INDEX:
		for (int v = 0; v < 4; v++)
			PUT(GL_UNSIGNED_INT, v);
		break;
	case ARR_PIX_RGBA:                      /* (images: 16 x 12, 8 x 6) */
		for (int j = 0; j < 12; j++)
			for (int i = 0; i < 16; i++) {
				PUT(GL_UNSIGNED_BYTE, 16 * i);
				PUT(GL_UNSIGNED_BYTE, 21 * j);
				PUT(GL_UNSIGNED_BYTE, (i ^ j) & 4 ? 230 : 40);
				PUT(GL_UNSIGNED_BYTE, 255);
			}
		break;
	case ARR_PIX_RGB:
		for (int j = 0; j < 6; j++)
			for (int i = 0; i < 8; i++) {
				PUT(GL_UNSIGNED_BYTE, i == 0 || j == 0 ? 255 : 30 * i);
				PUT(GL_UNSIGNED_BYTE, 200 - 30 * j);
				PUT(GL_UNSIGNED_BYTE, (i + j) & 1 ? 250 : 10);
			}
		break;
	case ARR_PIX_LUM:
		for (int j = 0; j < 12; j++)
			for (int i = 0; i < 16; i++)
				PUT(GL_UNSIGNED_BYTE, 15 * i + 3 * j);
		break;
	case ARR_MAP4:                          /* (a pixel map) */
		PUT(GL_FLOAT, 0.9);
		PUT(GL_FLOAT, 0.1);
		PUT(GL_FLOAT, 0.7);
		PUT(GL_FLOAT, 0.3);
		break;
	case ARR_VOLUME:                        /* (4 x 4 x 4 RGBA; a 4 x 4 slice) */
		for (int k = 0; k < 4; k++)
			for (int j = 0; j < 4; j++)
				for (int i = 0; i < 4; i++) {
					PUT(GL_UNSIGNED_BYTE, 60 * i + 30);
					PUT(GL_UNSIGNED_BYTE, 60 * j + 30);
					PUT(GL_UNSIGNED_BYTE, 80 * k);
					PUT(GL_UNSIGNED_BYTE, 255);
				}
		break;
	case ARR_SLICE:
		for (int j = 0; j < 16; j++) {
			PUT(GL_UNSIGNED_BYTE, j & 1 ? 255 : 20);
			PUT(GL_UNSIGNED_BYTE, 230);
			PUT(GL_UNSIGNED_BYTE, j & 2 ? 0 : 200);
			PUT(GL_UNSIGNED_BYTE, 255);
		}
		break;
	case ARR_MIP:                           /* (16 x 8 RGB, for mipmaps: checkered by 4 x 4) */
		for (int j = 0; j < 8; j++)
			for (int i = 0; i < 16; i++) {
				PUT(GL_UNSIGNED_BYTE, (i / 4 + j / 4) & 1 ? 240 : 30);
				PUT(GL_UNSIGNED_BYTE, 15 * i);
				PUT(GL_UNSIGNED_BYTE, 30 * j + 20);
			}
		break;
	case ARR_MAP_IDENTITY:                  /* (256 entries: 8 bits each as they are) */
		for (int k = 0; k < 256; k++)
			PUT(GL_FLOAT, k / 255.0);
		break;
	case ARR_BITMAP: {                      /* (16 x 8, rows of 4 bytes) */
		static const uint16_t rows[8] = { 0x0FF0, 0x1818, 0x300C, 0x6006, 0x7FFE, 0x6006, 0x6006, 0xF00F };
		for (int j = 0; j < 8; j++) {
			PUT(GL_UNSIGNED_BYTE, rows[j] >> 8);
			PUT(GL_UNSIGNED_BYTE, rows[j] & 0xFF);
			PUT(GL_UNSIGNED_BYTE, 0);
			PUT(GL_UNSIGNED_BYTE, 0);
		}
		break;
	}
	case ARR_STIPPLE:                       /* (a polygon stipple: diagonal stripes, a block cut out) */
		for (int j = 0; j < 32; j++)
			for (int i = 0; i < 4; i++) {
				uint32_t row = 0;
				for (int b = 0; b < 8; b++) {
					int x = 8 * i + b;
					row = row << 1 | (((x + j) % 6 < 3) && !(x >= 8 && x < 16 && j >= 4 && j < 12));
				}
				PUT(GL_UNSIGNED_BYTE, row);
			}
		break;
	case ARR_PIX_DEPTH:
		for (int j = 0; j < 6; j++)
			for (int i = 0; i < 8; i++)
				PUT(GL_FLOAT, 0.204 + 0.08 * i + 0.01 * j);
		break;
	case ARR_PIX_STENCIL:
		for (int j = 0; j < 6; j++)
			for (int i = 0; i < 8; i++)
				PUT(GL_UNSIGNED_BYTE, (i + j) % 3 == 0 ? 1 : 2);
		break;
	case ARR_LIST_UBYTE:                    /* (glCallLists' names) */
		for (int v = 0; v < 3; v++)
			PUT(GL_UNSIGNED_BYTE, v);
		break;
	case ARR_LIST_2BYTES:
		for (int v = 1; v < 3; v++) {
			PUT(GL_UNSIGNED_BYTE, 0);
			PUT(GL_UNSIGNED_BYTE, v);
		}
		break;
	case ARR_LIST_FLOAT:
		PUT(GL_FLOAT, 2);
		PUT(GL_FLOAT, 0);
		break;
	/* evaluators' control points: [v][u], each padded to 4 values if it's fewer (and as the scenes' strides say) */
	case ARR_EVAL_CURVE: {                  /* (a cubic curve, x y z) */
		static const double xy[4][2] = { { 4.3, 40.2 }, { 20.1, 70.4 }, { 40.7, 40.3 }, { 58.6, 66.8 } };
		for (int i = 0; i < 4; i++) {
			PUT(GL_FLOAT, xy[i][0]);
			PUT(GL_FLOAT, xy[i][1]);
			PUT(GL_FLOAT, 0.1 * i);
			PUT(GL_FLOAT, 99);
		}
		break;
	}
	case ARR_EVAL_COLORS:                   /* (red, green, blue) */
		for (int i = 0; i < 3; i++)
			for (int c = 0; c < 4; c++)
				PUT(GL_FLOAT, c == i || c == 3 ? 1 : 0.1);
		break;
	case ARR_EVAL_ARC: {                    /* (doubles: a quarter circle, rational, x y z w) */
		const double w = sqrt(0.5);
		static const double xy[3][2] = { { 90.3, 45.2 }, { 90.3, 70.2 }, { 65.3, 70.2 } };
		for (int i = 0; i < 3; i++) {
			double wi = i == 1 ? w : 1;
			PUT(GL_DOUBLE, xy[i][0] * wi);
			PUT(GL_DOUBLE, xy[i][1] * wi);
			PUT(GL_DOUBLE, 0);
			PUT(GL_DOUBLE, wi);
		}
		break;
	}
	/*
	 * (4 x 4, x y z: in window coordinates, x and y on a grid (so a function
	 * of u and v as linear as the color and texture maps': whichever way a
	 * quad's split, they're interpolated alike), z bent; bent all over; in
	 * ORTHO_UNITS', bumpy)
	 */
	case ARR_EVAL_PATCH:
	case ARR_EVAL_BENT:
	case ARR_EVAL_BUMPS:
		for (int j = 0; j < 4; j++)
			for (int i = 0; i < 4; i++) {
				static const double z[4][4] = { { 0, 0.1, 0, -0.1 }, { 0.2, 0.9, -0.5, 0 },
								 { 0, -0.6, 0.8, 0.1 }, { -0.1, 0, 0.2, 0 } };
				if (k == ARR_EVAL_PATCH) {
					PUT(GL_FLOAT, 5.27 + 12.1 * i + 2.3 * j);     /* (grid points off edges) */
					PUT(GL_FLOAT, 3.63 + 1.2 * i + 9.7 * j);
					PUT(GL_FLOAT, z[j][i]);
				} else if (k == ARR_EVAL_BENT) {
					PUT(GL_FLOAT, 5.3 + 12.1 * i + (j == 1 || j == 2 ? 2.2 : 0));
					PUT(GL_FLOAT, 3.6 + 9.7 * j + (i == 1 ? 3.1 : i == 2 ? -2.4 : 0));
					PUT(GL_FLOAT, (i + j) & 1 ? 0.4 : -0.3);
				} else {
					PUT(GL_FLOAT, -1.5 + 0.4 * i);
					PUT(GL_FLOAT, -1.043 + 0.647 * j);
					PUT(GL_FLOAT, z[j][i]);
				}
			}
		break;
	case ARR_EVAL_PATCH_COLORS: {           /* (2 x 2, r g b a: linear in u and v) */
		static const double rgb[4][3] = { { 0.9, 0.2, 0.2 }, { 0.5, 0.9, 0.2 }, { 0.5, 0.2, 0.9 },
						  { 0.1, 0.9, 0.9 } };
		for (int i = 0; i < 4; i++) {
			for (int c = 0; c < 3; c++)
				PUT(GL_FLOAT, rgb[i][c]);
			PUT(GL_FLOAT, 1);
		}
		break;
	}
	case ARR_EVAL_PATCH_TEX:                /* (2 x 2, s t) */
		for (int i = 0; i < 4; i++) {
			PUT(GL_FLOAT, i & 1 ? 2 : 0);
			PUT(GL_FLOAT, i & 2 ? 2 : 0);
		}
		break;
	case ARR_EVAL_CYLINDER: {               /* (doubles, 3 x 2: a quarter of a cylinder, rational, x y z w) */
		const double w = sqrt(0.5);
		static const double xz[3][2] = { { -0.4, 0 }, { -0.4, 0.6 }, { 0.2, 0.6 } };
		for (int j = 0; j < 2; j++)
			for (int i = 0; i < 3; i++) {
				double wi = i == 1 ? w : 1;
				PUT(GL_DOUBLE, xz[i][0] * wi);
				PUT(GL_DOUBLE, (j ? 0.9 : -0.9) * wi);
				PUT(GL_DOUBLE, xz[i][1] * wi);
				PUT(GL_DOUBLE, wi);
			}
		break;
	}
	case ARR_EVAL_FLAT:                     /* (2 x 2, x y z: flat) */
	case ARR_EVAL_NORMALS:                  /* (2 x 2, x y z: not unit length) */
		for (int i = 0; i < 4; i++) {
			PUT(GL_FLOAT, k == ARR_EVAL_FLAT ? 0.9 + 0.6 * (i & 1) : (i & 1 ? 0.6 : -0.6));
			PUT(GL_FLOAT, k == ARR_EVAL_FLAT ? -1.1 + 2.2 * (i >> 1) : (i & 2 ? 0.5 : -0.5));
			PUT(GL_FLOAT, k == ARR_EVAL_FLAT ? 0 : 2);
		}
		break;
	case ARR_SECONDARY:                     /* (rgb, a stride of 4) */
		for (int v = 0; v < 8; v++) {
			PUT(GL_UNSIGNED_BYTE, v & 1 ? 0 : 90);
			PUT(GL_UNSIGNED_BYTE, 40);
			PUT(GL_UNSIGNED_BYTE, v * 20);
			PUT(GL_UNSIGNED_BYTE, 0);
		}
		break;
	}
#undef PUT
	return (size_t)(p - out);
}

/* Whether a call's pointer argument names an array (else it's a parameter vector, from the values after it) */
static bool takes_array(const char *name)
{
	return strstr(name, "Pointer") || !strcmp(name, "glInterleavedArrays") || !strcmp(name, "glDrawElements") ||
	       !strcmp(name, "glDrawRangeElements") || !strcmp(name, "glCallLists") || !strcmp(name, "glDrawPixels") ||
	       !strcmp(name, "glBitmap") || !strcmp(name, "glPixelMapfv") || !strcmp(name, "glTexImage3D") ||
	       !strcmp(name, "glTexSubImage3D") || !strncmp(name, "glMap1", 6) || !strncmp(name, "glMap2", 6) ||
	       !strcmp(name, "glPolygonStipple");
}

/* A parameter vector's values (from a[first]): as doubles for glClipPlane, else floats */
static bool vector_of_doubles(const char *name)
{
	return !strcmp(name, "glClipPlane");
}

static const struct op scene_texture[] = {
	ORTHO, CLEAR(0.2, 0.2, 0.3), { "glEnable", { GL_TEXTURE_2D } },
	{ "glBindTexture", { 0x0DE1, 1 } }, TEX(1, 8, 8, IMAGE_CHECKER),
	TEXP(GL_TEXTURE_MIN_FILTER, GL_NEAREST), TEXP(GL_TEXTURE_MAG_FILTER, GL_NEAREST),
	/* (texel edges a third of a pixel off pixels' centers) */
	BEGIN(GL_QUADS), C3(1, 0.8, 0.6), TC(0, 0), V2(4.3, 4.3), TC(2, 0), V2(36.3, 4.3), TC(2, 2), V2(36.3, 36.3),
	TC(0, 2), V2(4.3, 36.3), END,
	{ "glTexEnvi", { GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE } },
	BEGIN(GL_QUADS), TC(0, 0), V2(44.3, 4.3), TC(1, 0), V2(92.3, 4.3), TC(1, 1), V2(92.3, 36.3), TC(0, 1),
	V2(44.3, 36.3), END,
	{ "glTexEnvi", { GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL } },
	BEGIN(GL_TRIANGLES), C3(0.2, 0.9, 0.4), TC(0, 0), V2(4.3, 40.3), TC(1, 0), V2(52.3, 40.3), TC(0.5, 1), V2(28.3, 68.3),
	END,
	{ "glTexEnvi", { GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE } }, { "glEnable", { GL_BLEND } },
	{ "glBlendFunc", { GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA } },
	BEGIN(GL_TRIANGLES), C4(1, 1, 1, 0.8), TC(0, 0), V2(52.3, 40.3), TC(1, 0), V2(92.3, 44.3), TC(1, 1), V2(70.3, 68.3),
	END,
	{ NULL, { 0 } },
};

/* the same smooth image, magnified with linear filtering past its edges: wrap (ws, wt) */
#define CLAMP_SCENE(ws, wt) \
	ORTHO, CLEAR(0, 0, 0), { "glEnable", { GL_TEXTURE_2D } }, { "glBindTexture", { 0x0DE1, 2 } }, \
	TEX(2, 4, 4, IMAGE_GRADIENT), TEXP(GL_TEXTURE_MIN_FILTER, GL_LINEAR), TEXP(GL_TEXTURE_MAG_FILTER, GL_LINEAR), \
	TEXP(GL_TEXTURE_WRAP_S, ws), TEXP(GL_TEXTURE_WRAP_T, wt), \
	{ "glTexEnvi", { GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE } }, \
	BEGIN(GL_QUADS), TC(-0.25, -0.25), V2(8.3, 4.3), TC(1.25, -0.25), V2(88.3, 4.3), TC(1.25, 1.25), V2(88.3, 68.3), \
	TC(-0.25, 1.25), V2(8.3, 68.3), END, { NULL, { 0 } }
static const struct op scene_clamp[] = { CLAMP_SCENE(GL_CLAMP, GL_CLAMP) };
static const struct op scene_edge[] = { CLAMP_SCENE(GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE) };

/* two units, modulated; textured in perspective (linear: the same level of detail everywhere) */
static const struct op scene_multitexture[] = {
	FRUSTUM, CLEAR(0.1, 0.1, 0.1), { "glEnable", { GL_DEPTH_TEST } },
	{ "glActiveTextureARB", { GL_TEXTURE0_ARB } }, { "glEnable", { GL_TEXTURE_2D } },
	{ "glBindTexture", { 0x0DE1, 1 } }, TEX(1, 8, 8, IMAGE_CHECKER),
	TEXP(GL_TEXTURE_MIN_FILTER, GL_NEAREST), TEXP(GL_TEXTURE_MAG_FILTER, GL_NEAREST),
	{ "glActiveTextureARB", { GL_TEXTURE0_ARB + 1 } }, { "glEnable", { GL_TEXTURE_2D } },
	{ "glBindTexture", { 0x0DE1, 2 } }, TEX(2, 4, 4, IMAGE_GRADIENT),
	TEXP(GL_TEXTURE_MIN_FILTER, GL_LINEAR), TEXP(GL_TEXTURE_MAG_FILTER, GL_LINEAR),
	{ "glTranslatef", { 0, 0, -3 } }, { "glRotatef", { -60, 1, 0, 0 } },
	BEGIN(GL_QUADS), C3(1, 1, 1),
	{ "glMultiTexCoord2fARB", { GL_TEXTURE0_ARB, 0, 0 } }, { "glMultiTexCoord2fARB", { GL_TEXTURE0_ARB + 1, 0.1, 0.1 } },
	V3(-1.53, -1.47, 0),
	{ "glMultiTexCoord2fARB", { GL_TEXTURE0_ARB, 3, 0 } }, { "glMultiTexCoord2fARB", { GL_TEXTURE0_ARB + 1, 0.9, 0.1 } },
	V3(1.47, -1.53, 0),
	{ "glMultiTexCoord2fARB", { GL_TEXTURE0_ARB, 3, 3 } }, { "glMultiTexCoord2fARB", { GL_TEXTURE0_ARB + 1, 0.9, 0.9 } },
	V3(1.53, 1.47, 0),
	{ "glMultiTexCoord2fARB", { GL_TEXTURE0_ARB, 0, 3 } }, { "glMultiTexCoord2fARB", { GL_TEXTURE0_ARB + 1, 0.1, 0.9 } },
	V3(-1.47, 1.53, 0), END,
	{ NULL, { 0 } },
};

/* Lighting, fog, texture coordinate generation, client arrays, stencil, polygon offset, clip planes */
enum { GL_VERTEX_ARRAY = 0x8074, GL_NORMAL_ARRAY, GL_COLOR_ARRAY, GL_TEXTURE_COORD_ARRAY = 0x8078,
       GL_SECONDARY_COLOR_ARRAY_EXT = 0x845E, GL_T2F_C4UB_V3F = 0x2A29 };
#define ORTHO_UNITS { "glMatrixMode", { GL_PROJECTION } }, { "glLoadIdentity", { 0 } }, \
	{ "glOrtho", { -1.6, 1.6, -1.2, 1.2, 1, 20 } }, { "glMatrixMode", { GL_MODELVIEW } }, \
	{ "glLoadIdentity", { 0 } }, { "glTranslatef", { 0, 0, -5 } }
#define EN(cap) { "glEnable", { cap } }
#define DIS(cap) { "glDisable", { cap } }
#define N3(x, y, z) { "glNormal3f", { x, y, z } }
#define LIGHTV(l, pname, a, b, c, d) { "glLightfv", { l, pname, a, b, c, d } }
#define MATV(face, pname, a, b, c, d) { "glMaterialfv", { face, pname, a, b, c, d } }
#define MESH(dx, dy, sx, sy) { "glPushMatrix", { 0 } }, { "glTranslatef", { dx, dy, 0 } }, \
	{ "glScalef", { sx, sy, 0.5 } }, { "glEnableClientState", { GL_VERTEX_ARRAY } }, \
	{ "glEnableClientState", { GL_NORMAL_ARRAY } }, { "glVertexPointer", { 3, GL_FLOAT, 0, ARR_MESH_POS } }, \
	{ "glNormalPointer", { GL_FLOAT, 0, ARR_MESH_NORMAL } }, \
	{ "glDrawElements", { GL_TRIANGLES, MESH_INDICES, GL_UNSIGNED_SHORT, ARR_MESH_INDEX } }, \
	{ "glDisableClientState", { GL_VERTEX_ARRAY } }, { "glDisableClientState", { GL_NORMAL_ARRAY } }, \
	{ "glPopMatrix", { 0 } }
#define QUAD2(x0, y0, x1, y1) BEGIN(GL_QUADS), V2(x0, y0), V2(x1, y0), V2(x1, y1), V2(x0, y1), END

/* (orthographic: Mesa corrects colors for perspective, ours are linear in screen space) */
static const struct op scene_lighting[] = {
	ORTHO_UNITS, CLEAR(0.05, 0.05, 0.1), EN(GL_DEPTH_TEST), EN(GL_LIGHTING),
	{ "glLightModeli", { GL_LIGHT_MODEL_LOCAL_VIEWER, 1 } }, { "glLightModeli", { GL_LIGHT_MODEL_TWO_SIDE, 1 } },
	EN(GL_LIGHT0), LIGHTV(GL_LIGHT0, GL_POSITION, 0.3, 0.5, 1, 0), LIGHTV(GL_LIGHT0, GL_DIFFUSE, 0.4, 0.4, 0.35, 1),
	LIGHTV(GL_LIGHT0, GL_SPECULAR, 0.4, 0.4, 0.4, 1),
	EN(GL_LIGHT0 + 1), LIGHTV(GL_LIGHT0 + 1, GL_POSITION, 1.2, -0.8, 1.5, 1),
	LIGHTV(GL_LIGHT0 + 1, GL_DIFFUSE, 0.8, 0.3, 0.1, 1), LIGHTV(GL_LIGHT0 + 1, GL_SPECULAR, 0.5, 0.5, 0.5, 1),
	{ "glLightf", { GL_LIGHT0 + 1, GL_LINEAR_ATTENUATION, 0.4 } },
	EN(GL_LIGHT0 + 2), LIGHTV(GL_LIGHT0 + 2, GL_POSITION, -1, 1, 2, 1),
	LIGHTV(GL_LIGHT0 + 2, GL_SPOT_DIRECTION, 0.5, -0.6, -1, 0), { "glLightf", { GL_LIGHT0 + 2, GL_SPOT_CUTOFF, 35 } },
	{ "glLightf", { GL_LIGHT0 + 2, GL_SPOT_EXPONENT, 6 } }, LIGHTV(GL_LIGHT0 + 2, GL_DIFFUSE, 0.2, 0.4, 1, 1),
	MATV(GL_FRONT, GL_AMBIENT, 0.2, 0.2, 0.2, 1), MATV(GL_FRONT, GL_DIFFUSE, 0.6, 0.6, 0.5, 1),
	MATV(GL_FRONT, GL_SPECULAR, 0.6, 0.6, 0.6, 1), { "glMaterialf", { GL_FRONT, GL_SHININESS, 24 } },
	MATV(GL_BACK, GL_DIFFUSE, 0.2, 0.8, 0.3, 1),
	EN(GL_NORMALIZE), MESH(-0.35, 0.2, 0.9, 0.9), DIS(GL_NORMALIZE),
	/* a back face (two-sided: the back's material, its normal turned round, to the lights) */
	BEGIN(GL_QUADS), N3(0, 0, -1), V3(0.9, -1.1, 0.3), V3(0.9, -0.3, 0.3), V3(1.5, -0.3, 0.3), V3(1.5, -1.1, 0.3), END,
	/* the material's ambient and diffuse colors tracking the current color; scaled, normals rescaled */
	{ "glColorMaterial", { GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE } }, EN(GL_COLOR_MATERIAL),
	EN(GL_RESCALE_NORMAL), { "glPushMatrix", { 0 } }, { "glTranslatef", { 1.2, 0.6, 0 } },
	{ "glScalef", { 0.8, 0.8, 0.8 } },
	BEGIN(GL_TRIANGLES), C3(1, 0.2, 0.2), N3(0, 0.3, 0.95), V3(-0.37, -0.48, 0.2), C3(0.2, 1, 0.2), N3(0.3, 0, 0.95),
	V3(0.38, -0.37, 0.2), C3(0.2, 0.2, 1), N3(-0.2, -0.2, 0.96), V3(0.01, 0.51, 0.2), END,
	{ "glPopMatrix", { 0 } }, DIS(GL_RESCALE_NORMAL), DIS(GL_COLOR_MATERIAL),
	{ NULL, { 0 } },
};

/* separate specular over a texture (the color sum), then the single color */
static const struct op scene_specular[] = {
	ORTHO_UNITS, CLEAR(0.1, 0.1, 0.1), EN(GL_DEPTH_TEST), EN(GL_LIGHTING), EN(GL_LIGHT0),
	LIGHTV(GL_LIGHT0, GL_POSITION, 0.4, 0.6, 1, 0), MATV(GL_FRONT, GL_DIFFUSE, 0.6, 0.6, 0.6, 1),
	MATV(GL_FRONT, GL_SPECULAR, 0.5, 0.5, 0.5, 1), { "glMaterialf", { GL_FRONT, GL_SHININESS, 40 } },
	EN(GL_TEXTURE_2D), { "glBindTexture", { 0x0DE1, 2 } }, TEX(2, 4, 4, IMAGE_GRADIENT),
	TEXP(GL_TEXTURE_MIN_FILTER, GL_LINEAR), TEXP(GL_TEXTURE_MAG_FILTER, GL_LINEAR),
	TEXP(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE), TEXP(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE),
	{ "glTexGeni", { GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR } },
	{ "glTexGenfv", { GL_S, GL_OBJECT_PLANE, 0.4, 0, 0, 0.5 } },
	{ "glTexGeni", { GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR } },
	{ "glTexGenfv", { GL_T, GL_OBJECT_PLANE, 0, 0.5, 0, 0.5 } }, EN(GL_TEXTURE_GEN_S), EN(GL_TEXTURE_GEN_S + 1),
	{ "glLightModeli", { GL_LIGHT_MODEL_COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR } }, EN(GL_NORMALIZE),
	MESH(-0.8, 0.1, 0.62, 1),
	{ "glLightModeli", { GL_LIGHT_MODEL_COLOR_CONTROL, GL_SINGLE_COLOR } },
	MESH(0.8, 0.1, 0.62, 1),
	{ NULL, { 0 } },
};

/* each fog mode on a strip going away, flat (constant colors: the fog coordinate is all that's interpolated) */
#define STRIP(k, c) C3(c, c, c), BEGIN(GL_QUADS), V3(-1.43 + 0.7 * (k), -1, -1.5), V3(-0.87 + 0.7 * (k), -1, -1.5), \
	V3(6 * (-0.87 + 0.7 * (k)), 6, -9), V3(6 * (-1.43 + 0.7 * (k)), 6, -9), END
static const struct op scene_fog[] = {
	FRUSTUM, CLEAR(0.5, 0.6, 0.7), EN(GL_DEPTH_TEST), { "glShadeModel", { GL_FLAT } }, EN(GL_FOG),
	{ "glFogfv", { GL_FOG_COLOR, 0.5, 0.6, 0.7, 1 } },
	{ "glFogi", { GL_FOG_MODE, GL_LINEAR } }, { "glFogf", { GL_FOG_START, 2 } }, { "glFogf", { GL_FOG_END, 9 } },
	STRIP(0, 0.9),
	{ "glFogi", { GL_FOG_MODE, GL_EXP } }, { "glFogf", { GL_FOG_DENSITY, 0.25 } }, STRIP(1, 0.1),
	{ "glFogi", { GL_FOG_MODE, GL_EXP2 } }, { "glFogf", { GL_FOG_DENSITY, 0.15 } }, STRIP(2, 1),
	{ "glFogi", { GL_FOG_MODE, GL_LINEAR } }, { "glFogf", { GL_FOG_START, -3 } }, STRIP(3, 0.4),
	{ NULL, { 0 } },
};

/* texture coordinates generated: object and eye linear, the sphere map, the reflection vector */
static const struct op scene_texgen[] = {
	ORTHO_UNITS, CLEAR(0.1, 0.1, 0.1), EN(GL_TEXTURE_2D), { "glBindTexture", { 0x0DE1, 2 } },
	TEX(2, 4, 4, IMAGE_GRADIENT), TEXP(GL_TEXTURE_MIN_FILTER, GL_LINEAR), TEXP(GL_TEXTURE_MAG_FILTER, GL_LINEAR),
	TEXP(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE), TEXP(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE),
	{ "glTexEnvi", { GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE } },
	{ "glTexGeni", { GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR } },
	{ "glTexGenfv", { GL_S, GL_OBJECT_PLANE, 0.8, 0.1, 0, 0.5 } },
	{ "glTexGeni", { GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR } },
	{ "glTexGenfv", { GL_T, GL_OBJECT_PLANE, 0, 0.9, 0, 0.45 } }, EN(GL_TEXTURE_GEN_S), EN(GL_TEXTURE_GEN_S + 1),
	{ "glPushMatrix", { 0 } }, { "glTranslatef", { -0.9, 0.55, 0 } }, { "glRotatef", { 20, 0, 0, 1 } },
	BEGIN(GL_QUADS), V3(-0.5, -0.4, 0), V3(0.5, -0.4, 0), V3(0.5, 0.4, 0), V3(-0.5, 0.4, 0), END,
	{ "glPopMatrix", { 0 } },
	/* (the eye planes as the modelview is when they're given, the quad turned) */
	{ "glTexGeni", { GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR } },
	{ "glTexGenfv", { GL_S, GL_EYE_PLANE, 0.7, 0, 0, 0.5 } },
	{ "glTexGeni", { GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR } },
	{ "glTexGenfv", { GL_T, GL_EYE_PLANE, 0, 0.7, 0.2, 0.4 } },
	{ "glPushMatrix", { 0 } }, { "glTranslatef", { 0.8, 0.55, 0 } }, { "glRotatef", { -30, 0, 0, 1 } },
	BEGIN(GL_QUADS), V3(-0.5, -0.4, 0), V3(0.5, -0.4, 0), V3(0.5, 0.4, 0), V3(-0.5, 0.4, 0), END,
	{ "glPopMatrix", { 0 } },
	{ "glTexGeni", { GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP } },
	{ "glTexGeni", { GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP } },
	MESH(-0.8, -0.6, 0.6, 0.55),
	{ "glTexGeni", { GL_S, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP_NV } },
	{ "glTexGeni", { GL_T, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP_NV } },
	MESH(0.8, -0.6, 0.6, 0.55),
	{ NULL, { 0 } },
};

/* client arrays of each type, interleaved, indexed by each type, by glArrayElement; a secondary color */
static const struct op scene_arrays[] = {
	ORTHO, CLEAR(0.15, 0.15, 0.15),
	{ "glEnableClientState", { GL_VERTEX_ARRAY } }, { "glEnableClientState", { GL_COLOR_ARRAY } },
	{ "glVertexPointer", { 2, GL_SHORT, 0, ARR_SHORT_POS } },
	{ "glColorPointer", { 4, GL_UNSIGNED_BYTE, 0, ARR_UBYTE_COLOR } },
	{ "glLockArraysEXT", { 0, 8 } }, { "glDrawArrays", { GL_TRIANGLE_STRIP, 0, 8 } }, { "glUnlockArraysEXT", { 0 } },
	{ "glVertexPointer", { 3, GL_DOUBLE, 0, ARR_DOUBLE_POS } },
	{ "glColorPointer", { 3, GL_FLOAT, 0, ARR_FLOAT_RGB } },
	{ "glDrawElements", { GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, ARR_BYTE_INDEX } },
	EN(GL_TEXTURE_2D), { "glBindTexture", { 0x0DE1, 2 } }, TEX(2, 4, 4, IMAGE_GRADIENT),
	TEXP(GL_TEXTURE_MIN_FILTER, GL_LINEAR), TEXP(GL_TEXTURE_MAG_FILTER, GL_LINEAR),
	{ "glInterleavedArrays", { GL_T2F_C4UB_V3F, 0, ARR_INTERLEAVED } },
	{ "glDrawElements", { GL_QUADS, 4, GL_UNSIGNED_INT, ARR_UINT_INDEX } },
	DIS(GL_TEXTURE_2D), { "glDisableClientState", { GL_TEXTURE_COORD_ARRAY } },
	{ "glDisableClientState", { GL_COLOR_ARRAY } },
	{ "glVertexPointer", { 2, GL_SHORT, 0, ARR_SHORT_POS } }, { "glTranslatef", { 0.3, 46.2, 0 } },
	{ "glEnableClientState", { GL_SECONDARY_COLOR_ARRAY_EXT } },
	{ "glSecondaryColorPointerEXT", { 3, GL_UNSIGNED_BYTE, 4, ARR_SECONDARY } }, EN(GL_COLOR_SUM_EXT),
	C3(0.3, 0.3, 0.6), BEGIN(GL_TRIANGLE_STRIP), { "glArrayElement", { 0 } }, { "glArrayElement", { 1 } },
	{ "glArrayElement", { 2 } }, { "glArrayElement", { 3 } }, { "glArrayElement", { 4 } },
	{ "glArrayElement", { 5 } }, END,
	DIS(GL_COLOR_SUM_EXT), { "glDisableClientState", { GL_SECONDARY_COLOR_ARRAY_EXT } },
	{ "glDisableClientState", { GL_VERTEX_ARRAY } },
	{ NULL, { 0 } },
};

/*
 * The stencil: a shape, counting, inverting with a write mask, after the
 * alpha test (its alpha 0.5 between pixels' centers, not on one: there,
 * rounding would decide), when the depth test fails
 */
static const struct op scene_stencil[] = {
	ORTHO, { "glClearColor", { 0.1, 0.1, 0.1, 1 } }, { "glClearDepth", { 1 } }, { "glClearStencil", { 0 } },
	{ "glClear", { GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT } }, EN(GL_STENCIL_TEST),
	{ "glStencilFunc", { GL_ALWAYS, 1, 0xFF } }, { "glStencilOp", { GL_KEEP, GL_KEEP, GL_REPLACE } },
	{ "glColorMask", { 0, 0, 0, 0 } },
	BEGIN(GL_TRIANGLES), V2(4.3, 4.2), V2(44.6, 8.1), V2(20.1, 40.7), END,
	{ "glColorMask", { 1, 1, 1, 1 } },
	{ "glStencilFunc", { GL_EQUAL, 1, 0xFF } }, { "glStencilOp", { GL_KEEP, GL_KEEP, GL_KEEP } },
	C3(1, 0.2, 0.2), QUAD2(2.2, 2.3, 46.4, 44.6),
	{ "glStencilFunc", { GL_ALWAYS, 0, 0xFF } }, { "glStencilOp", { GL_KEEP, GL_KEEP, GL_INCR } },
	{ "glColorMask", { 0, 0, 0, 0 } },
	BEGIN(GL_TRIANGLES), V2(50.3, 4.4), V2(92.5, 6.6), V2(70.2, 40.8), V2(52.1, 30.3), V2(60.4, 2.1),
	V2(90.7, 22.2), END,
	{ "glColorMask", { 1, 1, 1, 1 } }, { "glStencilFunc", { GL_LESS, 1, 0xFF } },
	{ "glStencilOp", { GL_KEEP, GL_KEEP, GL_KEEP } }, C3(0.2, 1, 0.2), QUAD2(48.2, 1.3, 94.6, 44.7),
	{ "glStencilMask", { 0x0F } }, { "glStencilFunc", { GL_ALWAYS, 0, 0xFF } },
	{ "glStencilOp", { GL_KEEP, GL_KEEP, GL_INVERT } }, { "glColorMask", { 0, 0, 0, 0 } },
	QUAD2(60.3, 10.2, 80.6, 30.7),
	{ "glColorMask", { 1, 1, 1, 1 } }, { "glStencilMask", { 0xFF } }, { "glStencilFunc", { GL_EQUAL, 0x0D, 0x0F } },
	{ "glStencilOp", { GL_KEEP, GL_KEEP, GL_KEEP } }, C3(0.2, 0.3, 1), QUAD2(48.2, 1.3, 94.6, 44.7),
	EN(GL_ALPHA_TEST), { "glAlphaFunc", { GL_GREATER, 0.5 } }, { "glStencilFunc", { GL_ALWAYS, 5, 0xFF } },
	{ "glStencilOp", { GL_KEEP, GL_KEEP, GL_REPLACE } }, { "glColorMask", { 0, 0, 0, 0 } },
	BEGIN(GL_QUADS), C4(1, 1, 1, 0), V2(3.4, 48.3), C4(1, 1, 1, 1), V2(44.6, 48.3), V2(44.6, 70.2), C4(1, 1, 1, 0),
	V2(3.4, 70.2), END,
	DIS(GL_ALPHA_TEST), { "glColorMask", { 1, 1, 1, 1 } }, { "glStencilFunc", { GL_EQUAL, 5, 0xFF } },
	{ "glStencilOp", { GL_KEEP, GL_KEEP, GL_KEEP } }, C3(1, 1, 0.3), QUAD2(4.4, 48.3, 44.6, 70.2),
	EN(GL_DEPTH_TEST), { "glStencilFunc", { GL_ALWAYS, 0, 0xFF } }, C3(0.5, 0.5, 0.5),
	BEGIN(GL_QUADS), V3(50.3, 48.2, 0.5), V3(80.4, 48.2, 0.5), V3(80.4, 68.3, 0.5), V3(50.3, 68.3, 0.5), END,
	{ "glColorMask", { 0, 0, 0, 0 } }, { "glStencilOp", { GL_KEEP, GL_INCR, GL_KEEP } },
	BEGIN(GL_QUADS), V3(60.2, 52.4, -0.5), V3(92.6, 52.4, -0.5), V3(92.6, 70.5, -0.5), V3(60.2, 70.5, -0.5), END,
	{ "glColorMask", { 1, 1, 1, 1 } }, DIS(GL_DEPTH_TEST), { "glStencilFunc", { GL_EQUAL, 1, 0xFF } },
	{ "glStencilOp", { GL_KEEP, GL_KEEP, GL_KEEP } }, C3(0.9, 0.4, 0.9), QUAD2(48.3, 46.4, 95.2, 71.5),
	{ NULL, { 0 } },
};

/* decals on a floor and a wall: offset forward (shown), back (hidden) */
static const struct op scene_offset[] = {
	FRUSTUM, CLEAR(0.1, 0.1, 0.1), EN(GL_DEPTH_TEST), { "glShadeModel", { GL_FLAT } },
	C3(0.5, 0.5, 0.5), BEGIN(GL_QUADS), V3(-2.1, -1, -1.5), V3(2.1, -1, -1.5), V3(2.1, -1, -9), V3(-2.1, -1, -9), END,
	EN(GL_POLYGON_OFFSET_FILL), { "glPolygonOffset", { -1, -2 } },
	C3(1, 0.2, 0.2), BEGIN(GL_QUADS), V3(-1.2, -1, -2), V3(-0.2, -1, -2), V3(-0.2, -1, -6), V3(-1.2, -1, -6), END,
	{ "glPolygonOffset", { 1, 2 } },
	C3(0.2, 0.2, 1), BEGIN(GL_QUADS), V3(0.2, -1, -2), V3(1.2, -1, -2), V3(1.2, -1, -6), V3(0.2, -1, -6), END,
	DIS(GL_POLYGON_OFFSET_FILL),
	C3(0.2, 0.8, 0.3), BEGIN(GL_QUADS), V3(-0.53, -1, -5), V3(0.47, -1, -5), V3(0.47, 0.5, -5), V3(-0.53, 0.5, -5), END,
	EN(GL_POLYGON_OFFSET_FILL), { "glPolygonOffset", { 0, -40 } },
	C3(1, 1, 0), BEGIN(GL_QUADS), V3(-0.31, -0.6, -5), V3(0.29, -0.6, -5), V3(0.29, 0.2, -5), V3(-0.31, 0.2, -5), END,
	{ NULL, { 0 } },
};

/* user clip planes (given with the modelview turned) */
static const struct op scene_clip[] = {
	ORTHO_UNITS, CLEAR(0.2, 0.1, 0.1),
	{ "glPushMatrix", { 0 } }, { "glRotatef", { 30, 0, 0, 1 } }, { "glClipPlane", { GL_CLIP_PLANE0, 1, 0.3, 0, 0.2 } },
	{ "glPopMatrix", { 0 } }, EN(GL_CLIP_PLANE0),
	BEGIN(GL_TRIANGLE_FAN), C3(1, 1, 1), V3(-0.71, 0.03, 0), C3(1, 0, 0), V3(-1.52, -0.02, 0), C3(1, 1, 0),
	V3(-1.13, 0.83, 0), C3(0, 1, 0), V3(-0.31, 1.07, 0), C3(0, 1, 1), V3(0.12, 0.21, 0), C3(0, 0, 1),
	V3(-0.38, -0.97, 0), C3(1, 0, 1), V3(-1.27, -0.88, 0), C3(1, 0, 0), V3(-1.52, -0.02, 0), END,
	{ "glClipPlane", { GL_CLIP_PLANE0 + 1, 0, -1, 0, 0.5 } }, EN(GL_CLIP_PLANE0 + 1),
	BEGIN(GL_QUADS), C3(0.9, 0.6, 0.1), V3(0.31, -1.13, 0), C3(0.1, 0.6, 0.9), V3(1.48, -1.07, 0), C3(0.9, 0.9, 0.9),
	V3(1.42, 1.11, 0), C3(0.3, 0.9, 0.3), V3(0.37, 1.04, 0), END,
	DIS(GL_CLIP_PLANE0), DIS(GL_CLIP_PLANE0 + 1),
	{ NULL, { 0 } },
};

/*
 * Display lists: nested, made and done at once, called by name when run
 * (a list redefined), the client's arrays and a texture's image (and the
 * pixel store it was unpacked by) as they were when made, glCallLists'
 * types with a list base, deleted lists
 */
enum { GL_COMPILE = 0x1300, GL_COMPILE_AND_EXECUTE, GL_2_BYTES = 0x1407, GL_UNPACK_SKIP_PIXELS = 0x0CF4 };
#define LIST(n, mode) { "glNewList", { n, mode } }
#define ENDLIST { "glEndList", { 0 } }
#define CALLLIST(n) { "glCallList", { n } }
static const struct op scene_lists[] = {
	ORTHO, CLEAR(0.1, 0.1, 0.2),
	LIST(1, GL_COMPILE), C3(0.9, 0.3, 0.2), BEGIN(GL_TRIANGLES), V2(2.3, 2.2), V2(13.6, 3.1), V2(6.2, 14.7), END,
	{ "glTranslatef", { 15, 0, 0 } }, ENDLIST,
	LIST(2, GL_COMPILE_AND_EXECUTE), { "glPushMatrix", { 0 } }, { "glTranslatef", { 0, 17, 0 } }, CALLLIST(1),
	CALLLIST(1), { "glPopMatrix", { 0 } }, ENDLIST,
	CALLLIST(1), CALLLIST(2),
	/* list 1 again: list 2 calls the new one */
	LIST(1, GL_COMPILE), C3(0.2, 0.9, 0.3), { "glRectf", { 2.3, 2.2, 11.6, 12.7 } }, { "glTranslatef", { 15, 0, 0 } },
	ENDLIST,
	{ "glLoadIdentity", { 0 } }, { "glTranslatef", { 0, 34, 0 } }, CALLLIST(2),
	/* arrays: as they were when the list was made */
	{ "glEnableClientState", { GL_VERTEX_ARRAY } }, { "glEnableClientState", { GL_COLOR_ARRAY } },
	{ "glVertexPointer", { 2, GL_SHORT, 0, ARR_SHORT_POS } },
	{ "glColorPointer", { 4, GL_UNSIGNED_BYTE, 0, ARR_UBYTE_COLOR } },
	LIST(3, GL_COMPILE), { "glDrawArrays", { GL_TRIANGLE_STRIP, 0, 8 } }, ENDLIST,
	{ "glVertexPointer", { 3, GL_DOUBLE, 0, ARR_DOUBLE_POS } }, { "glDisableClientState", { GL_COLOR_ARRAY } },
	{ "glLoadIdentity", { 0 } }, { "glTranslatef", { 0, 50, 0 } }, CALLLIST(3),
	{ "glDisableClientState", { GL_VERTEX_ARRAY } },
	/* glCallLists' types, from the list base */
	LIST(10, GL_COMPILE), C3(1, 1, 0.2), { "glRecti", { 50, 4, 56, 10 } }, { "glTranslatef", { 7, 0, 0 } }, ENDLIST,
	LIST(11, GL_COMPILE), C3(0.2, 1, 1), { "glRecti", { 50, 4, 56, 10 } }, { "glTranslatef", { 7, 0, 0 } }, ENDLIST,
	LIST(12, GL_COMPILE), C3(1, 0.2, 1), { "glRecti", { 50, 4, 56, 10 } }, { "glTranslatef", { 7, 0, 0 } }, ENDLIST,
	{ "glLoadIdentity", { 0 } }, { "glListBase", { 10 } },
	{ "glCallLists", { 3, GL_UNSIGNED_BYTE, ARR_LIST_UBYTE } }, { "glTranslatef", { -21, 8, 0 } },
	{ "glCallLists", { 2, GL_2_BYTES, ARR_LIST_2BYTES } }, { "glTranslatef", { -14, 8, 0 } },
	{ "glCallLists", { 2, GL_FLOAT, ARR_LIST_FLOAT } },
	{ "glDeleteLists", { 11, 2 } }, { "glTranslatef", { -14, 8, 0 } },
	{ "glCallLists", { 3, GL_UNSIGNED_BYTE, ARR_LIST_UBYTE } }, { "glListBase", { 0 } },
	/* a texture's image (unpacked as it was when the list was made) */
	{ "glBindTexture", { 0x0DE1, 3 } }, TEXP(GL_TEXTURE_MIN_FILTER, GL_NEAREST),
	TEXP(GL_TEXTURE_MAG_FILTER, GL_NEAREST),
	LIST(20, GL_COMPILE), TEX(3, 4, 4, IMAGE_CHECKER), ENDLIST,
	TEX(3, 4, 4, IMAGE_GRADIENT), { "glPixelStorei", { GL_UNPACK_SKIP_PIXELS, 1 } }, CALLLIST(20),
	{ "glPixelStorei", { GL_UNPACK_SKIP_PIXELS, 0 } },
	{ "glLoadIdentity", { 0 } }, EN(GL_TEXTURE_2D), C3(1, 1, 1),
	BEGIN(GL_QUADS), TC(0, 0), V2(70.3, 40.2), TC(1, 0), V2(90.6, 40.2), TC(1, 1), V2(90.6, 66.7), TC(0, 1),
	V2(70.3, 66.7), END, DIS(GL_TEXTURE_2D),
	{ NULL, { 0 } },
};

/*
 * Pixel rectangles: images of each kind of format at the raster position,
 * zoomed by whole numbers (once upside down: Mesa's fractional zooms put
 * rows where neither OpenGL 1.2's rectangles nor rounding would, see
 * test_glfront.c), through the pixel transfer's scales and biases (no
 * maps: see mesa_invoke(); test_glfront.c checks them); bitmaps moving the
 * raster position; a copy of what's drawn; a depth image tested against
 * (none of its depths a quad's: there, rounding would decide), a stencil
 * image stenciled by
 */
enum { GL_RGBA_T = 0x1908, GL_RGB_T = 0x1907, GL_LUMINANCE_T = 0x1909, GL_RED_SCALE = 0x0D14, GL_BLUE_SCALE = 0x0D1A,
       GL_BLUE_BIAS = 0x0D1B, GL_COLOR_T = 0x1800, GL_DEPTH_COMPONENT_T = 0x1902,
       GL_STENCIL_INDEX_T = 0x1901 };
#define RPOS(x, y) { "glRasterPos2f", { x, y } }
#define DRAWPIX(w, h, format, type, image) { "glDrawPixels", { w, h, format, type, image } }
static const struct op scene_pixels[] = {
	ORTHO, CLEAR(0.1, 0.15, 0.2),
	RPOS(4.3, 4.2), DRAWPIX(16, 12, GL_RGBA_T, GL_UNSIGNED_BYTE, ARR_PIX_RGBA),
	RPOS(24.3, 4.2), { "glPixelZoom", { 2, 1 } }, DRAWPIX(8, 6, GL_RGB_T, GL_UNSIGNED_BYTE, ARR_PIX_RGB),
	{ "glPixelZoom", { 1, -2 } }, RPOS(48.6, 21.4), DRAWPIX(8, 6, GL_RGB_T, GL_UNSIGNED_BYTE, ARR_PIX_RGB),
	{ "glPixelZoom", { 1, 1 } },
	{ "glPixelTransferf", { GL_RED_SCALE, 0.5 } }, { "glPixelTransferf", { GL_BLUE_BIAS, 0.3 } },
	RPOS(64.3, 4.2), DRAWPIX(16, 12, GL_LUMINANCE_T, GL_UNSIGNED_BYTE, ARR_PIX_LUM),
	{ "glPixelTransferf", { GL_RED_SCALE, 1 } }, { "glPixelTransferf", { GL_BLUE_BIAS, 0 } },
	{ "glPixelTransferf", { GL_BLUE_SCALE, -0.5 } }, { "glPixelTransferf", { GL_ALPHA_SCALE, 0.4 } },
	RPOS(80.3, 4.2), DRAWPIX(16, 12, GL_RGBA_T, GL_UNSIGNED_BYTE, ARR_PIX_RGBA),
	{ "glPixelTransferf", { GL_BLUE_SCALE, 1 } }, { "glPixelTransferf", { GL_ALPHA_SCALE, 1 } },
	/* bitmaps, each moving the raster position on */
	C3(1, 0.9, 0.2), RPOS(4.5, 22.5),
	{ "glBitmap", { 16, 8, 0, 0, 18, 0, ARR_BITMAP } }, { "glBitmap", { 16, 8, 0, 0, 18, 1, ARR_BITMAP } },
	C3(0.3, 1, 1), { "glBitmap", { 16, 8, 2.5, 0, 18, 0, ARR_BITMAP } },
	/* a copy of the first image */
	RPOS(76.4, 36.3), { "glCopyPixels", { 4, 4, 16, 12, GL_COLOR_T } },
	/* a depth image (its color the raster position's), then a quad in front of some of it */
	EN(GL_DEPTH_TEST), { "glDepthFunc", { GL_LESS } }, C3(0.9, 0.2, 0.9), RPOS(4.3, 36.2),
	{ "glPixelZoom", { 3, 2 } }, DRAWPIX(8, 6, GL_DEPTH_COMPONENT_T, GL_FLOAT, ARR_PIX_DEPTH),
	{ "glPixelZoom", { 1, 1 } }, C3(0.2, 0.9, 0.3),
	BEGIN(GL_QUADS), V3(2.2, 34.3, -0.2), V3(30.6, 34.3, -0.2), V3(30.6, 50.4, -0.2), V3(2.2, 50.4, -0.2), END,
	DIS(GL_DEPTH_TEST),
	/* a stencil image, then a quad where it's 1 */
	{ "glClearStencil", { 0 } }, { "glClear", { GL_STENCIL_BUFFER_BIT } }, RPOS(40.3, 36.2),
	{ "glPixelZoom", { 3, 3 } }, DRAWPIX(8, 6, GL_STENCIL_INDEX_T, GL_UNSIGNED_BYTE, ARR_PIX_STENCIL),
	{ "glPixelZoom", { 1, 1 } }, EN(GL_STENCIL_TEST), { "glStencilFunc", { GL_EQUAL, 1, 0xFF } },
	C3(1, 0.5, 0.1), QUAD2(38.2, 34.3, 66.6, 56.5), DIS(GL_STENCIL_TEST),
	/* the depth image and the quad copied as depths (the GPU's read back), a quad in front of some of them */
	EN(GL_DEPTH_TEST), C3(0.3, 0.3, 0.9), RPOS(68.3, 59.2), { "glCopyPixels", { 4, 36, 24, 12, GL_DEPTH } },
	C3(0.9, 0.9, 0.2),
	BEGIN(GL_QUADS), V3(66.2, 57.3, -0.4), V3(94.6, 57.3, -0.4), V3(94.6, 71.6, -0.4), V3(66.2, 71.6, -0.4), END,
	DIS(GL_DEPTH_TEST),
	{ NULL, { 0 } },
};

/*
 * 3D textures: nearest and trilinear, r clamped and repeated, r generated
 * (object linear), a slice replaced
 */
#define TC3(s, t, r) { "glTexCoord3f", { s, t, r } }
#define TEX3P(pname, v) { "glTexParameteri", { GL_TEXTURE_3D, pname, v } }
enum { GL_TEXTURE_WRAP_R_T = 0x8072 };
static const struct op scene_texture3d[] = {
	ORTHO, CLEAR(0.1, 0.1, 0.1), EN(GL_TEXTURE_3D), { "glBindTexture", { GL_TEXTURE_3D, 5 } },
	{ "glTexImage3D", { GL_TEXTURE_3D, 0, 0x8058, 4, 4, 4, 0, 0x1908, GL_UNSIGNED_BYTE, ARR_VOLUME } },
	TEX3P(GL_TEXTURE_MIN_FILTER, GL_NEAREST), TEX3P(GL_TEXTURE_MAG_FILTER, GL_NEAREST),
	TEX3P(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE), TEX3P(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE),
	TEX3P(GL_TEXTURE_WRAP_R_T, GL_CLAMP_TO_EDGE), { "glTexEnvi", { GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE } },
	BEGIN(GL_QUADS), TC3(0, 0, 0.05), V2(4.3, 4.2), TC3(1, 0, 0.95), V2(44.6, 4.2), TC3(1, 1, 0.95), V2(44.6, 34.7),
	TC3(0, 1, 0.05), V2(4.3, 34.7), END,
	/* trilinear, r repeated past both ends */
	TEX3P(GL_TEXTURE_MIN_FILTER, GL_LINEAR), TEX3P(GL_TEXTURE_MAG_FILTER, GL_LINEAR),
	TEX3P(GL_TEXTURE_WRAP_R_T, GL_REPEAT),
	BEGIN(GL_QUADS), TC3(0, 0, -0.3), V2(50.3, 4.2), TC3(1, 0, -0.3), V2(92.6, 4.2), TC3(1, 1, 1.3), V2(92.6, 34.7),
	TC3(0, 1, 1.3), V2(50.3, 34.7), END,
	/* a slice replaced; r from the object's y */
	{ "glTexSubImage3D", { GL_TEXTURE_3D, 0, 0, 0, 2, 4, 4, 1, 0x1908, GL_UNSIGNED_BYTE, ARR_SLICE } },
	TEX3P(GL_TEXTURE_MIN_FILTER, GL_NEAREST), TEX3P(GL_TEXTURE_MAG_FILTER, GL_NEAREST),
	{ "glTexGeni", { GL_R, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR } },
	{ "glTexGenfv", { GL_R, GL_OBJECT_PLANE, 0, 0.0283, 0, -1.03 } }, EN(GL_TEXTURE_GEN_S + 2),
	BEGIN(GL_QUADS), TC3(0, 0, 0), V2(4.3, 40.2), TC3(1, 0, 0), V2(92.6, 40.2), TC3(1, 1, 0), V2(92.6, 69.7),
	TC3(0, 1, 0), V2(4.3, 69.7), END,
	DIS(GL_TEXTURE_GEN_S + 2), DIS(GL_TEXTURE_3D),
	{ NULL, { 0 } },
};

/*
 * OpenGLUtility's: a perspective and a viewpoint (gluPerspective,
 * gluLookAt), quadrics of each kind (the scene's quadric's), lit smooth
 * and flat, one inside out, textured from gluBuild2DMipmaps' levels (the
 * first, and the third alone), and drawn as lines, silhouettes and points
 */
enum { GLU_SMOOTH_T = 100000, GLU_FLAT_T, GLU_POINT_T = 100010, GLU_LINE_T, GLU_FILL_T, GLU_SILHOUETTE_T,
       GLU_OUTSIDE_T = 100020, GLU_INSIDE_T, GLU_ERROR_T = 100103, GLU_INVALID_ENUM_T = 100900, GLU_INVALID_VALUE_T };
#define QSET(name, v) { name, { 0, v } }
#define PUSH { "glPushMatrix", { 0 } }
#define POP { "glPopMatrix", { 0 } }
#define MOVE(x, y, z) { "glTranslatef", { x, y, z } }
#define TURN(a, x, y, z) { "glRotatef", { a, x, y, z } }
static const struct op scene_glu[] = {
	CLEAR(0.1, 0.1, 0.15), { "glMatrixMode", { GL_PROJECTION } }, { "glLoadIdentity", { 0 } },
	{ "gluPerspective", { 40, 96.0 / 72, 1, 20 } }, { "glMatrixMode", { GL_MODELVIEW } }, { "glLoadIdentity", { 0 } },
	{ "gluLookAt", { 0.3, 1.7, 6.1, 0.1, 0, 0, 0, 1, 0 } },
	EN(GL_DEPTH_TEST), EN(GL_LIGHTING), EN(GL_LIGHT0), LIGHTV(GL_LIGHT0, GL_POSITION, 0.4, 0.8, 1, 0),
	MATV(GL_FRONT, GL_DIFFUSE, 0.8, 0.6, 0.4, 1),
	PUSH, MOVE(-1.7, 0.7, 0), { "gluSphere", { 0, 0.9, 14, 9 } }, POP,
	QSET("gluQuadricNormals", GLU_FLAT_T), PUSH, MOVE(0.2, 0.4, -0.3), TURN(-75, 1, 0, 0),
	{ "gluCylinder", { 0, 0.6, 0.25, 1.3, 9, 3 } }, POP, QSET("gluQuadricNormals", GLU_SMOOTH_T),
	QSET("gluQuadricOrientation", GLU_INSIDE_T), PUSH, MOVE(-1.4, -1.1, 0.3), TURN(-60, 1, 0, 0),
	{ "gluDisk", { 0, 0.2, 0.8, 10, 2 } }, POP, QSET("gluQuadricOrientation", GLU_OUTSIDE_T), DIS(GL_LIGHTING),
	/* textured: the first level, then the third alone (4 x 2) */
	C3(1, 1, 1), { "glBindTexture", { GL_TEXTURE_2D, 3 } },
	{ "gluBuild2DMipmaps", { GL_TEXTURE_2D, 3, 16, 8, GL_RGB, GL_UNSIGNED_BYTE, ARR_MIP } },
	TEXP(GL_TEXTURE_MIN_FILTER, GL_LINEAR), TEXP(GL_TEXTURE_MAG_FILTER, GL_LINEAR), EN(GL_TEXTURE_2D),
	QSET("gluQuadricTexture", 1), PUSH, MOVE(1.8, 0.8, 0), TURN(-90, 1, 0, 0), { "gluSphere", { 0, 0.8, 16, 10 } },
	POP, TEXP(0x813C, 2), TEXP(0x813D, 2), PUSH, MOVE(1.1, -1.0, 0.6), { "gluDisk", { 0, 0, 0.6, 12, 1 } }, POP,
	QSET("gluQuadricTexture", 0), DIS(GL_TEXTURE_2D),
	/* lines, silhouettes, points */
	C3(1, 1, 0.3), QSET("gluQuadricDrawStyle", GLU_LINE_T), PUSH, MOVE(-0.4, -1.5, 0.8),
	{ "gluPartialDisk", { 0, 0.2, 0.7, 7, 2, 20, 230 } }, POP,
	C3(0.3, 1, 1), QSET("gluQuadricDrawStyle", GLU_SILHOUETTE_T), PUSH, MOVE(0.9, 0.3, 1.2), TURN(-80, 1, 0, 0),
	{ "gluCylinder", { 0, 0.3, 0.3, 0.5, 8, 2 } }, POP,
	C3(1, 0.5, 1), QSET("gluQuadricDrawStyle", GLU_POINT_T), PUSH, MOVE(-0.3, 1.7, -1), { "gluSphere", { 0, 0.5, 8, 5 } },
	POP, QSET("gluQuadricDrawStyle", GLU_FILL_T), DIS(GL_DEPTH_TEST),
	{ NULL, { 0 } },
};

/*
 * Against Mesa's: pixels that may differ by more than 2, and that one draws
 * on and the other doesn't. Both snap vertices to 1/256 of a pixel and give
 * a pixel whose center is on an edge to the same side (none differ so in
 * Mesa 25.2), but the transforms round each their own way, and a center on
 * an edge (the floor's edges are a third of a pixel down every pixel across)
 * may go to the triangle on either side; a fragment's alpha at the alpha
 * test's reference is decided by each's rounding too, and in "lighting" the
 * back face (lit by the attenuated light) is a few levels apart in places
 * (milestone 4's scenes otherwise differ by 2 at most). In "smooth", Mesa
 * corrects colors for perspective and ours are linear in screen space (see
 * glfront.h), most of all on the floor that goes from the near plane to the
 * far distance; and in "clamp", GL_CLAMP past the texture's edges is Mesa's
 * blend with the border color, and clamping to the edge texels in ours (see
 * glfront.h; checked the same where the coordinates are within the
 * texture, test_mesa()). In "glu", lines at any slope go from one pixel to
 * the next at different points in Mesa's and ours (only the lines differ:
 * the quadrics' filled faces are drawn alike).
 */
/*
 * Evaluators: a curve's points colored by a color map, then the current
 * color as it was; a rational curve (doubles, a domain run backwards) at
 * coordinates and grid points; a patch colored and textured by maps, its
 * grid's points over it; a list's map (the patch's control points the
 * other way round: v along the array) and part of its mesh. (Filled
 * meshes are quad strips, as OpenGL 1.2's 5.1 has them, where Mesa's are
 * triangle strips: a quad split the other way. So x, y, colors and
 * texture coordinates go linearly with u and v here, and fb_eval compares
 * curved ones' values exactly. The current color is set with all four
 * components: see fb_eval.)
 */
#define MAP1(target, u1, u2, stride, order, arr) { "glMap1f", { target, u1, u2, stride, order, arr } }
#define MAP2(target, u1, u2, us, uo, v1, v2, vs, vo, arr) { "glMap2f", { target, u1, u2, us, uo, v1, v2, vs, vo, arr } }
enum { GL_MAP1_COLOR_4_T = 0x0D90, GL_MAP1_VERTEX_3_T = 0x0D97, GL_MAP1_VERTEX_4_T, GL_MAP2_COLOR_4_T = 0x0DB0,
       GL_MAP2_NORMAL_T = 0x0DB2, GL_MAP2_TEXTURE_COORD_2_T = 0x0DB4, GL_MAP2_VERTEX_3_T = 0x0DB7, GL_MAP2_VERTEX_4_T,
       GL_AUTO_NORMAL_T = 0x0D80, GL_POINT_T = 0x1B00, GL_LINE_T, GL_FILL_T };
static const struct op scene_eval[] = {
	ORTHO, CLEAR(0.15, 0.1, 0.1),
	MAP1(GL_MAP1_VERTEX_3_T, 0, 1, 4, 4, ARR_EVAL_CURVE), MAP1(GL_MAP1_COLOR_4_T, 0, 1, 4, 3, ARR_EVAL_COLORS),
	EN(GL_MAP1_VERTEX_3_T), EN(GL_MAP1_COLOR_4_T), C4(1, 1, 1, 1),
	{ "glMapGrid1f", { 30, 0, 1 } }, { "glEvalMesh1", { GL_POINT_T, 0, 30 } }, DIS(GL_MAP1_COLOR_4_T),
	{ "glTranslatef", { 0.3, -4.3, 0 } }, { "glEvalMesh1", { GL_POINT_T, 2, 28 } }, { "glLoadIdentity", { 0 } },
	{ "glMap1d", { GL_MAP1_VERTEX_4_T, 2, -1, 4, 3, ARR_EVAL_ARC } }, EN(GL_MAP1_VERTEX_4_T),
	{ "glPointSize", { 3 } },
	C3(1, 1, 0.3), BEGIN(GL_POINTS), { "glEvalCoord1d", { 2 } }, { "glEvalCoord1d", { 1.5 } },
	{ "glEvalCoord1f", { 0.7 } }, { "glEvalCoord1d", { -1 } }, END,
	{ "glMapGrid1f", { 6, -1, 2 } }, C3(0.3, 1, 1), BEGIN(GL_POINTS), { "glEvalPoint1", { 1 } },
	{ "glEvalPoint1", { 3 } }, END, { "glPointSize", { 1 } }, DIS(GL_MAP1_VERTEX_4_T), DIS(GL_MAP1_VERTEX_3_T),
	MAP2(GL_MAP2_VERTEX_3_T, 0, 1, 3, 4, 0, 1, 12, 4, ARR_EVAL_PATCH),
	MAP2(GL_MAP2_COLOR_4_T, 0, 1, 4, 2, 0, 1, 8, 2, ARR_EVAL_PATCH_COLORS),
	MAP2(GL_MAP2_TEXTURE_COORD_2_T, 0, 1, 2, 2, 0, 1, 4, 2, ARR_EVAL_PATCH_TEX),
	EN(GL_MAP2_VERTEX_3_T), EN(GL_MAP2_COLOR_4_T), EN(GL_MAP2_TEXTURE_COORD_2_T),
	EN(GL_TEXTURE_2D), { "glBindTexture", { 0x0DE1, 1 } }, TEX(1, 8, 8, IMAGE_CHECKER),
	TEXP(GL_TEXTURE_MIN_FILTER, GL_LINEAR), TEXP(GL_TEXTURE_MAG_FILTER, GL_LINEAR),
	{ "glMapGrid2f", { 8, 0, 1, 6, 0, 1 } }, { "glEvalMesh2", { GL_FILL_T, 0, 8, 0, 6 } },
	DIS(GL_TEXTURE_2D), DIS(GL_MAP2_COLOR_4_T), C3(1, 1, 1), { "glEvalMesh2", { GL_POINT_T, 0, 8, 0, 6 } },
	EN(GL_MAP2_COLOR_4_T),
	LIST(1, GL_COMPILE), MAP2(GL_MAP2_VERTEX_3_T, 0, 1, 12, 4, 0, 1, 3, 4, ARR_EVAL_PATCH),
	{ "glMapGrid2f", { 6, 0, 1, 5, 0, 1 } }, { "glEvalMesh2", { GL_FILL_T, 1, 5, 0, 5 } }, ENDLIST,
	{ "glTranslatef", { 48, 0, 0 } }, CALLLIST(1),
	{ NULL, { 0 } },
};

/*
 * Evaluators, lit: a bumpy patch's normals made (GL_AUTO_NORMAL; its grid
 * points in triangle strips), a rational one's (a quarter of a cylinder,
 * doubles: GL_MAP2_VERTEX_4 over GL_MAP2_VERTEX_3; lit alike along v, so
 * however its quads are split), a flat one's normals mapped (normalized),
 * its points at coordinates
 */
#define EP2(i, j) { "glEvalPoint2", { i, j } }
#define EVAL_STRIP(j) BEGIN(GL_TRIANGLE_STRIP), EP2(0, j), EP2(0, j + 1), EP2(1, j), EP2(1, j + 1), EP2(2, j), \
	EP2(2, j + 1), EP2(3, j), EP2(3, j + 1), EP2(4, j), EP2(4, j + 1), EP2(5, j), EP2(5, j + 1), EP2(6, j), \
	EP2(6, j + 1), END
static const struct op scene_eval_lit[] = {
	ORTHO_UNITS, CLEAR(0.05, 0.05, 0.1), EN(GL_DEPTH_TEST), EN(GL_LIGHTING), EN(GL_LIGHT0),
	LIGHTV(GL_LIGHT0, GL_POSITION, 0.3, 0.5, 1, 0), LIGHTV(GL_LIGHT0, GL_DIFFUSE, 0.8, 0.8, 0.7, 1),
	LIGHTV(GL_LIGHT0, GL_SPECULAR, 0.5, 0.5, 0.5, 1),
	MATV(GL_FRONT, GL_DIFFUSE, 0.6, 0.6, 0.5, 1), MATV(GL_FRONT, GL_SPECULAR, 0.6, 0.6, 0.6, 1),
	{ "glMaterialf", { GL_FRONT, GL_SHININESS, 20 } },
	MAP2(GL_MAP2_VERTEX_3_T, 0, 1, 3, 4, 0, 1, 12, 4, ARR_EVAL_BUMPS), EN(GL_MAP2_VERTEX_3_T), EN(GL_AUTO_NORMAL_T),
	{ "glMapGrid2f", { 6, 0, 1, 6, 0, 1 } }, EVAL_STRIP(0), EVAL_STRIP(1), EVAL_STRIP(2), EVAL_STRIP(3),
	EVAL_STRIP(4), EVAL_STRIP(5),
	{ "glMap2d", { GL_MAP2_VERTEX_4_T, 0, 1, 4, 3, 0, 1, 12, 2, ARR_EVAL_CYLINDER } }, EN(GL_MAP2_VERTEX_4_T),
	MATV(GL_FRONT, GL_DIFFUSE, 0.3, 0.7, 0.9, 1),
	{ "glMapGrid2d", { 8, 0, 1, 4, 0, 1 } }, { "glEvalMesh2", { GL_FILL_T, 0, 8, 0, 4 } },
	DIS(GL_MAP2_VERTEX_4_T), DIS(GL_AUTO_NORMAL_T), EN(GL_NORMALIZE),
	MAP2(GL_MAP2_VERTEX_3_T, 0, 1, 3, 2, 0, 1, 6, 2, ARR_EVAL_FLAT),
	MAP2(GL_MAP2_NORMAL_T, 0, 1, 3, 2, 0, 1, 6, 2, ARR_EVAL_NORMALS), EN(GL_MAP2_NORMAL_T),
	MATV(GL_FRONT, GL_DIFFUSE, 0.9, 0.5, 0.3, 1),
	BEGIN(GL_TRIANGLES), { "glEvalCoord2f", { 0, 0 } }, { "glEvalCoord2f", { 1, 0 } },
	{ "glEvalCoord2f", { 1, 1 } }, { "glEvalCoord2f", { 0, 0 } }, { "glEvalCoord2f", { 1, 1 } },
	{ "glEvalCoord2f", { 0, 1 } }, END,
	{ NULL, { 0 } },
};

/*
 * OpenGL 1.2's rest: logic ops over four colors, blending on too (which
 * they override), a line drawn twice by XOR (gone again). Every color is a
 * whole number of 255ths, flat: the bits logic ops take are then the same
 * everywhere, where a gradient's (radeonsi's a step from llvmpipe's here
 * and there) or a half step's rounding would change them.
 */
enum { GL_COLOR_LOGIC_OP_T = 0x0BF2, GL_AND_T = 0x1501, GL_XOR_T = 0x1506, GL_OR_T = 0x1507, GL_EQUIV_T = 0x1509,
       GL_INVERT_T = 0x150A, GL_COPY_INVERTED_T = 0x150C, GL_NAND_T = 0x150E, GL_LINE_STIPPLE_T = 0x0B24,
       GL_POLYGON_STIPPLE_T = 0x0B42, GL_POINT_SMOOTH_T = 0x0B10, GL_LINE_SMOOTH_T = 0x0B20 };
#define LOGIC(op) { "glLogicOp", { op } }
#define C255(r, g, b) C3((r) / 255.0, (g) / 255.0, (b) / 255.0)
static const struct op scene_logicop[] = {
	ORTHO, CLEAR(0.2, 0.4, 0.6),
	C255(128, 51, 26), QUAD2(2.2, 2.3, 48, 36), C255(230, 128, 51), QUAD2(48, 2.3, 93.7, 36),
	C255(51, 77, 204), QUAD2(2.2, 36, 48, 69.6), C255(153, 153, 230), QUAD2(48, 36, 93.7, 69.6),
	EN(GL_COLOR_LOGIC_OP_T), EN(GL_BLEND), { "glBlendFunc", { GL_ONE, GL_ONE } },
	LOGIC(GL_XOR_T), C255(128, 191, 64), QUAD2(5.3, 5.2, 30.6, 30.4),
	LOGIC(GL_AND_T), C255(230, 77, 153), QUAD2(35.3, 5.2, 60.6, 30.4),
	LOGIC(GL_OR_T), C255(26, 128, 51), QUAD2(65.3, 5.2, 90.6, 30.4),
	LOGIC(GL_INVERT_T), QUAD2(5.3, 35.2, 30.6, 50.4),
	LOGIC(GL_COPY_INVERTED_T), C255(51, 179, 230), QUAD2(35.3, 35.2, 60.6, 50.4),
	LOGIC(GL_NAND_T), C255(153, 153, 26), QUAD2(65.3, 35.2, 90.6, 50.4),
	LOGIC(GL_EQUIV_T), C255(77, 26, 204), QUAD2(5.3, 55.2, 45.6, 68.4),
	LOGIC(GL_XOR_T), C3(1, 1, 1), BEGIN(GL_LINES), V2(50.3, 57.6), V2(91.6, 66.3), V2(50.3, 57.6), V2(91.6, 66.3), END,
	BEGIN(GL_LINES), V2(50.3, 60.6), V2(91.6, 69.3), END,
	DIS(GL_COLOR_LOGIC_OP_T), DIS(GL_BLEND),
	{ NULL, { 0 } },
};

/*
 * Stippled lines (a strip and a loop going on, separate ones starting
 * again, wide ones), stippled polygons. (Mesa's stipple counter goes on
 * from segment to segment of a strip by their lengths, not their
 * fragments as OpenGL 1.2's 3.4.2 has it: dashes there may end a pixel
 * later or sooner.)
 */
static const struct op scene_stipple[] = {
	ORTHO, CLEAR(0.1, 0.1, 0.1),
	EN(GL_LINE_STIPPLE_T), { "glLineStipple", { 2, 0x1C47 } }, C3(1, 1, 0),
	BEGIN(GL_LINE_STRIP), V2(3.3, 3.6), V2(40.6, 5.7), V2(44.2, 30.6), V2(10.3, 25.7), END,
	{ "glLineStipple", { 1, 0xF0F0 } }, C3(0, 1, 1),
	BEGIN(GL_LINES), V2(3.3, 35.6), V2(44.6, 37.4), V2(3.3, 40.6), V2(44.6, 42.4), END,
	{ "glLineWidth", { 3 } }, { "glLineStipple", { 3, 0xAAAA } }, C3(1, 0.5, 0),
	BEGIN(GL_LINE_LOOP), V2(8.3, 48.6), V2(40.6, 50.7), V2(30.2, 68.4), END,
	{ "glLineWidth", { 1 } }, DIS(GL_LINE_STIPPLE_T),
	EN(GL_POLYGON_STIPPLE_T), { "glPolygonStipple", { ARR_STIPPLE } }, C3(0.3, 0.6, 1), QUAD2(50.3, 4.2, 92.6, 34.7),
	BEGIN(GL_TRIANGLES), C3(1, 0, 0), V2(52.3, 40.2), C3(0, 1, 0), V2(92.6, 44.7), C3(0, 0, 1), V2(70.4, 69.3), END,
	DIS(GL_POLYGON_STIPPLE_T),
	{ NULL, { 0 } },
};

/*
 * Lines by the diamond-exit rule (every way, ends in and out of diamonds),
 * wide ones (2, 3, 5), points of sizes rounded (1, 2, 2.6, 4, 5)
 */
static const struct op scene_lines[] = {
	ORTHO, CLEAR(0.11, 0.15, 0.11), { "glShadeModel", { GL_FLAT } },  /* (not 0.1: 25.5, rounded either way) */
	BEGIN(GL_LINES), C3(1, 1, 1), V2(2.3, 2.6), V2(30.7, 9.2), C3(1, 0.5, 0), V2(30.4, 12.3), V2(2.2, 4.4),
	C3(0, 1, 0), V2(4.6, 14.3), V2(9.2, 40.7), C3(0, 1, 1), V2(14.3, 40.6), V2(12.6, 14.2), C3(1, 0, 1), V2(20.4, 15.4),
	V2(33.6, 28.7), C3(1, 1, 0), V2(35.3, 15.6), V2(22.8, 37.3), C3(0.5, 0.5, 1), V2(3.6, 44.4), V2(3.8, 44.6), END,
	{ "glLineWidth", { 2 } }, BEGIN(GL_LINES), C3(1, 0.2, 0.2), V2(38.3, 3.3), V2(64.6, 12.4), END,
	{ "glLineWidth", { 3 } }, BEGIN(GL_LINES), C3(0.2, 1, 0.2), V2(40.2, 18.3), V2(46.6, 40.4), END,
	{ "glLineWidth", { 5 } }, BEGIN(GL_LINES), C3(0.3, 0.3, 1), V2(52.4, 22.7), V2(90.6, 34.3), END,
	{ "glLineWidth", { 1 } },
	{ "glPointSize", { 2 } }, BEGIN(GL_POINTS), C3(1, 1, 1), V2(70.3, 4.3), V2(76.6, 4.4), END,
	{ "glPointSize", { 2.6 } }, BEGIN(GL_POINTS), C3(1, 1, 0), V2(82.2, 6.3), END,
	{ "glPointSize", { 4 } }, BEGIN(GL_POINTS), C3(0, 1, 1), V2(88.7, 12.2), V2(70.2, 14.6), END,
	{ "glPointSize", { 5 } }, BEGIN(GL_POINTS), C3(1, 0, 1), V2(80.3, 18.4), END,
	{ "glPointSize", { 1 } }, BEGIN(GL_POINTS), C3(1, 0.5, 0.5), V2(66.3, 20.6), V2(67.6, 20.3), END,
	{ "glShadeModel", { GL_SMOOTH } },
	BEGIN(GL_LINE_LOOP), C3(1, 0, 0), V2(10.3, 50.6), C3(0, 1, 0), V2(60.4, 47.2), C3(0, 0, 1), V2(40.7, 68.3), END,
	{ NULL, { 0 } },
};

/*
 * Antialiased lines and points over what's there, blended by their
 * coverage: the area of each pixel a rectangle or disk covers, where
 * Mesa's llvmpipe ramps lines' alpha by distance and draws smooth points
 * as round aliased ones, and radeonsi covers thinner (OpenGL leaves the
 * coverage to them: allowed for both)
 */
static const struct op scene_smooth_lines[] = {
	ORTHO, CLEAR(0.1, 0.1, 0.2), EN(GL_BLEND), { "glBlendFunc", { GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA } },
	EN(GL_LINE_SMOOTH_T), C3(1, 1, 0.5),
	BEGIN(GL_LINES), V2(3.3, 4.6), V2(60.7, 22.1), V2(4.2, 30.3), V2(30.6, 66.7), END,
	{ "glLineWidth", { 2.5 } }, C3(0.5, 1, 1),
	BEGIN(GL_LINES), V2(40.3, 66.6), V2(90.4, 40.2), END,
	{ "glLineWidth", { 1 } }, DIS(GL_LINE_SMOOTH_T),
	EN(GL_POINT_SMOOTH_T), { "glPointSize", { 7.5 } }, C3(1, 0.5, 0.5), BEGIN(GL_POINTS), V2(75.3, 12.7), END,
	{ "glPointSize", { 3.2 } }, BEGIN(GL_POINTS), V2(85.6, 26.2), END,
	{ "glPointSize", { 1 } }, DIS(GL_POINT_SMOOTH_T), DIS(GL_BLEND),
	{ NULL, { 0 } },
};

/*
 * Textured lines and points (nearest: magnified; linear: minified along a
 * line, near half a texel a pixel, where a little moves the bilinear mix)
 */
static const struct op scene_texlines[] = {
	ORTHO, CLEAR(0.2, 0.2, 0.3), { "glEnable", { GL_TEXTURE_2D } },
	{ "glBindTexture", { 0x0DE1, 1 } }, TEX(1, 8, 8, IMAGE_CHECKER),
	TEXP(GL_TEXTURE_MIN_FILTER, GL_NEAREST), TEXP(GL_TEXTURE_MAG_FILTER, GL_NEAREST),
	{ "glTexEnvi", { GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE } },
	{ "glLineWidth", { 3 } },
	BEGIN(GL_LINES), TC(0.05, 0.3), V2(4.3, 6.6), TC(0.95, 0.4), V2(90.4, 10.3), TC(0.1, 0.1), V2(6.2, 20.4),
	TC(0.9, 0.9), V2(60.7, 66.3), END,
	{ "glLineWidth", { 1 } },
	TEXP(GL_TEXTURE_MIN_FILTER, GL_LINEAR), TEXP(GL_TEXTURE_MAG_FILTER, GL_LINEAR),
	BEGIN(GL_LINES), TC(0.02, 0.06), V2(20.3, 30.6), TC(3.97, 3.94), V2(88.6, 50.7), END,
	TEXP(GL_TEXTURE_MIN_FILTER, GL_NEAREST), TEXP(GL_TEXTURE_MAG_FILTER, GL_NEAREST),
	{ "glPointSize", { 6 } },
	BEGIN(GL_POINTS), TC(0.2, 0.3), V2(70.3, 22.6), TC(0.7, 0.6), V2(80.6, 24.4), TC(0.6, 0.1), V2(86.2, 60.3), END,
	{ "glPointSize", { 1 } }, { "glDisable", { GL_TEXTURE_2D } },
	{ NULL, { 0 } },
};

/*
 * GL_EXT_texture_lod_bias: a texture whose levels (32 x 32 to 1 x 1) are
 * each a color of their own, on squares whose level of detail is the same
 * at every pixel (32 texels over 12 pixels: 1.415; 6, 20, 40), biased up
 * and down (into the next level, past the last, under 0: magnified), a
 * magnified quad biased past GL_LINEAR's 0.5 into minifying; unit 1's
 * bias, with unit 0 off
 */
#define BIAS(b) { "glTexEnvf", { 0x8500, 0x8501, b } }
#define TSQUARE(x, y, w, h) BEGIN(GL_QUADS), TC(0, 0), V2(x, y), TC(1, 0), V2((x) + (w), y), \
	TC(1, (double)(h) / (w)), V2((x) + (w), (y) + (h)), TC(0, (double)(h) / (w)), V2(x, (y) + (h)), END
static const struct op scene_lodbias[] = {
	ORTHO, CLEAR(0.2, 0.2, 0.2), EN(GL_TEXTURE_2D), { "glBindTexture", { 0x0DE1, 1 } },
	TEXLEVEL(0, 32, 32, IMAGE_LEVEL0), TEXLEVEL(1, 16, 16, IMAGE_LEVEL0 + 1), TEXLEVEL(2, 8, 8, IMAGE_LEVEL0 + 2),
	TEXLEVEL(3, 4, 4, IMAGE_LEVEL0 + 3), TEXLEVEL(4, 2, 2, IMAGE_LEVEL0 + 4), TEXLEVEL(5, 1, 1, IMAGE_LEVEL0 + 5),
	TEXP(GL_TEXTURE_MIN_FILTER, 0x2700), TEXP(GL_TEXTURE_MAG_FILTER, GL_NEAREST),      /* (nearest mipmap, nearest) */
	{ "glTexEnvi", { GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE } },
	TSQUARE(2, 2, 12, 12), BIAS(1), TSQUARE(16, 2, 12, 12), BIAS(2.3), TSQUARE(30, 2, 12, 12),
	BIAS(-1), TSQUARE(44, 2, 12, 12), BIAS(-3), TSQUARE(58, 2, 12, 12), BIAS(0.2), TSQUARE(72, 2, 12, 12),
	BIAS(20), TSQUARE(2, 18, 12, 12), BIAS(-0.6), TSQUARE(16, 18, 12, 12), BIAS(-2), TSQUARE(30, 18, 6, 6),
	BIAS(0.5), TSQUARE(44, 18, 20, 20), BIAS(0),
	TEXP(GL_TEXTURE_MAG_FILTER, GL_LINEAR), TSQUARE(2, 40, 40, 16), BIAS(1), TSQUARE(50, 40, 40, 16), BIAS(0),
	TEXP(GL_TEXTURE_MAG_FILTER, GL_NEAREST),
	DIS(GL_TEXTURE_2D), { "glActiveTextureARB", { GL_TEXTURE0_ARB + 1 } }, EN(GL_TEXTURE_2D),
	{ "glBindTexture", { 0x0DE1, 1 } }, { "glTexEnvi", { GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE } }, BIAS(2),
	BEGIN(GL_QUADS), { "glMultiTexCoord2fARB", { GL_TEXTURE0_ARB + 1, 0, 0 } }, V2(66, 58),
	{ "glMultiTexCoord2fARB", { GL_TEXTURE0_ARB + 1, 1, 0 } }, V2(78, 58),
	{ "glMultiTexCoord2fARB", { GL_TEXTURE0_ARB + 1, 1, 1 } }, V2(78, 70),
	{ "glMultiTexCoord2fARB", { GL_TEXTURE0_ARB + 1, 0, 1 } }, V2(66, 70), END,
	BIAS(0), DIS(GL_TEXTURE_2D), { "glActiveTextureARB", { GL_TEXTURE0_ARB } },
	{ NULL, { 0 } },
};

/*
 * GL_TEXTURE_MIN_LOD and _MAX_LOD (and GL_TEXTURE_BASE_LEVEL and
 * _MAX_LEVEL): the levels-in-colors texture on 12-pixel squares (1.415),
 * its level of detail clamped from above and below, biased too, on a
 * square whose coordinates don't change, on a magnified quad clamped into
 * minifying; then from level 1 to 3 only, and from 2
 */
#define TEXPF(pname, v) { "glTexParameterf", { 0x0DE1, pname, v } }
#define LODS(lo, hi) TEXPF(0x813A, lo), TEXPF(0x813B, hi)
#define UNCHANGING(x, y) BEGIN(GL_QUADS), TC(0.3, 0.6), V2(x, y), V2((x) + 12, y), V2((x) + 12, (y) + 12), \
	V2(x, (y) + 12), END
static const struct op scene_lodrange[] = {
	ORTHO, CLEAR(0.2, 0.2, 0.2), EN(GL_TEXTURE_2D), { "glBindTexture", { 0x0DE1, 1 } },
	TEXLEVEL(0, 32, 32, IMAGE_LEVEL0), TEXLEVEL(1, 16, 16, IMAGE_LEVEL0 + 1), TEXLEVEL(2, 8, 8, IMAGE_LEVEL0 + 2),
	TEXLEVEL(3, 4, 4, IMAGE_LEVEL0 + 3), TEXLEVEL(4, 2, 2, IMAGE_LEVEL0 + 4), TEXLEVEL(5, 1, 1, IMAGE_LEVEL0 + 5),
	TEXP(GL_TEXTURE_MIN_FILTER, 0x2700), TEXP(GL_TEXTURE_MAG_FILTER, GL_NEAREST),
	{ "glTexEnvi", { GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE } },
	LODS(2.7, 1000), TSQUARE(2, 2, 12, 12), LODS(-1000, 0.3), TSQUARE(16, 2, 12, 12),
	LODS(-1000, 1.3), BIAS(2), TSQUARE(30, 2, 12, 12), BIAS(0), LODS(1.2, 1.2), TSQUARE(44, 2, 12, 12),
	LODS(4.6, 1000), TSQUARE(58, 2, 12, 12), LODS(0.8, 2), BIAS(-3), TSQUARE(72, 2, 12, 12), BIAS(0),
	LODS(2.2, 1000), UNCHANGING(2, 18), LODS(-1000, 1000), UNCHANGING(16, 18),
	TEXP(GL_TEXTURE_MAG_FILTER, GL_LINEAR), LODS(1.2, 1000), TSQUARE(30, 18, 40, 16), LODS(-1000, 1000),
	TEXP(GL_TEXTURE_MAG_FILTER, GL_NEAREST),
	TEXP(0x813C, 1), TEXP(0x813D, 3), TSQUARE(2, 40, 12, 12), BIAS(4), TSQUARE(16, 40, 12, 12), BIAS(0),  /* (BASE_LEVEL, MAX_LEVEL) */
	TEXP(0x813C, 2), TSQUARE(30, 40, 12, 12), LODS(0.6, 1000), TSQUARE(44, 40, 12, 12),
	DIS(GL_TEXTURE_2D),
	{ NULL, { 0 } },
};

/*
 * GL_EXT_texture_env_combine (as Apple's 9.2.2 takes it: its five
 * functions, four sources, the operands, scales of 1, 2 and 4): unit 0's
 * checkered texture (alpha falling) and a primary color changing linearly
 * across each 16-pixel square, combined by each function for r, g, b and
 * for a, from each source, through each operand, scaled; textures of each
 * base format (alpha, luminance, intensity, luminance and alpha, RGB);
 * then the built-in modulate again. List 50 sets the defaults.
 */
#define ENVI(pname, v) { "glTexEnvi", { GL_TEXTURE_ENV, pname, v } }
#define ENVF(pname, v) { "glTexEnvf", { GL_TEXTURE_ENV, pname, v } }
#define MTC(u, s, t) { "glMultiTexCoord2fARB", { GL_TEXTURE0_ARB + (u), s, t } }
#define CQUAD(x, y) BEGIN(GL_QUADS), C4(0.9, 0.1, 0.3, 0.8), MTC(0, 0, 0), MTC(1, 0, 1), V2(x, y), \
	C4(0.3, 0.6, 0.5, 0.4), MTC(0, 1, 0), MTC(1, 1, 1), V2((x) + 16, y), C4(0.3, 0.9, 1, 0.1), MTC(0, 1, 1), \
	MTC(1, 1, 0), V2((x) + 16, (y) + 16), C4(0.9, 0.4, 0.8, 0.5), MTC(0, 0, 1), MTC(1, 0, 0), V2(x, (y) + 16), END
#define DEFAULTS { "glCallList", { 50 } }
#define RGB_FN(f) ENVI(GL_COMBINE_RGB_EXT, f)
#define A_FN(f) ENVI(GL_COMBINE_ALPHA_EXT, f)
#define SRC(k, s) ENVI(GL_SOURCE0_RGB_EXT + (k), s)
#define ASRC(k, s) ENVI(GL_SOURCE0_ALPHA_EXT + (k), s)
#define OPND(k, o) ENVI(GL_OPERAND0_RGB_EXT + (k), o)
#define AOPND(k, o) ENVI(GL_OPERAND0_ALPHA_EXT + (k), o)
#define COMBINE_DEFAULTS { "glNewList", { 50, GL_COMPILE } }, ENVI(GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT), \
	RGB_FN(GL_MODULATE), A_FN(GL_MODULATE), SRC(0, GL_TEXTURE), SRC(1, GL_PREVIOUS_EXT), SRC(2, GL_CONSTANT_EXT), \
	ASRC(0, GL_TEXTURE), ASRC(1, GL_PREVIOUS_EXT), ASRC(2, GL_CONSTANT_EXT), OPND(0, GL_SRC_COLOR), \
	OPND(1, GL_SRC_COLOR), OPND(2, GL_SRC_ALPHA), AOPND(0, GL_SRC_ALPHA), AOPND(1, GL_SRC_ALPHA), \
	AOPND(2, GL_SRC_ALPHA), ENVF(GL_RGB_SCALE_EXT, 1), ENVF(GL_ALPHA_SCALE, 1), ENDLIST
#define TEXFMT(internal) { "glTexImage2D", { 0x0DE1, 0, internal, 8, 8, IMAGE_CHECKER } }
static const struct op scene_combine[] = {
	ORTHO, CLEAR(0.2, 0.2, 0.2), COMBINE_DEFAULTS, EN(GL_TEXTURE_2D), { "glBindTexture", { 0x0DE1, 1 } },
	TEX(1, 8, 8, IMAGE_CHECKER), TEXP(GL_TEXTURE_MIN_FILTER, GL_NEAREST), TEXP(GL_TEXTURE_MAG_FILTER, GL_NEAREST),
	{ "glTexEnvfv", { GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, 0.3, 0.7, 0.5, 0.6 } },
	DEFAULTS, RGB_FN(GL_REPLACE), A_FN(GL_REPLACE), CQUAD(0, 4),
	DEFAULTS, CQUAD(16, 4),
	DEFAULTS, RGB_FN(GL_ADD), A_FN(GL_ADD), CQUAD(32, 4),
	DEFAULTS, RGB_FN(GL_ADD_SIGNED_EXT), A_FN(GL_ADD_SIGNED_EXT), CQUAD(48, 4),
	DEFAULTS, RGB_FN(GL_INTERPOLATE_EXT), A_FN(GL_INTERPOLATE_EXT), CQUAD(64, 4),
	DEFAULTS, ENVF(GL_RGB_SCALE_EXT, 2), ENVF(GL_ALPHA_SCALE, 4), CQUAD(80, 4),
	DEFAULTS, RGB_FN(GL_REPLACE), A_FN(GL_REPLACE), SRC(0, GL_CONSTANT_EXT), ASRC(0, GL_PRIMARY_COLOR_EXT),
	CQUAD(0, 20),
	DEFAULTS, RGB_FN(GL_REPLACE), A_FN(GL_REPLACE), SRC(0, GL_PRIMARY_COLOR_EXT), ASRC(0, GL_CONSTANT_EXT),
	CQUAD(16, 20),
	DEFAULTS, RGB_FN(GL_REPLACE), A_FN(GL_REPLACE), OPND(0, GL_ONE_MINUS_SRC_COLOR),
	AOPND(0, GL_ONE_MINUS_SRC_ALPHA), CQUAD(32, 20),
	DEFAULTS, RGB_FN(GL_REPLACE), OPND(0, GL_SRC_ALPHA), CQUAD(48, 20),
	DEFAULTS, RGB_FN(GL_REPLACE), OPND(0, GL_ONE_MINUS_SRC_ALPHA), A_FN(GL_REPLACE), ASRC(0, GL_PREVIOUS_EXT),
	CQUAD(64, 20),
	DEFAULTS, RGB_FN(GL_INTERPOLATE_EXT), SRC(0, GL_CONSTANT_EXT), SRC(1, GL_TEXTURE),
	OPND(1, GL_ONE_MINUS_SRC_COLOR), SRC(2, GL_PRIMARY_COLOR_EXT), OPND(2, GL_ONE_MINUS_SRC_COLOR), CQUAD(80, 20),
	DEFAULTS, SRC(1, GL_CONSTANT_EXT), OPND(1, GL_SRC_ALPHA), ENVF(GL_RGB_SCALE_EXT, 2), A_FN(GL_ADD_SIGNED_EXT),
	ASRC(0, GL_PRIMARY_COLOR_EXT), ASRC(1, GL_CONSTANT_EXT), AOPND(1, GL_ONE_MINUS_SRC_ALPHA),
	ENVF(GL_ALPHA_SCALE, 2), CQUAD(0, 36),
	DEFAULTS, RGB_FN(GL_ADD), SRC(0, GL_PRIMARY_COLOR_EXT), OPND(0, GL_SRC_ALPHA), SRC(1, GL_CONSTANT_EXT),
	OPND(1, GL_ONE_MINUS_SRC_COLOR), AOPND(0, GL_ONE_MINUS_SRC_ALPHA), ASRC(1, GL_CONSTANT_EXT), CQUAD(16, 36),
	DEFAULTS, RGB_FN(GL_ADD_SIGNED_EXT), OPND(1, GL_ONE_MINUS_SRC_ALPHA), ENVF(GL_RGB_SCALE_EXT, 4),
	A_FN(GL_INTERPOLATE_EXT), ASRC(0, GL_PRIMARY_COLOR_EXT), ASRC(1, GL_CONSTANT_EXT), ASRC(2, GL_TEXTURE),
	AOPND(2, GL_ONE_MINUS_SRC_ALPHA), CQUAD(32, 36),
	DEFAULTS, RGB_FN(GL_INTERPOLATE_EXT), SRC(1, GL_PRIMARY_COLOR_EXT), SRC(2, GL_TEXTURE), OPND(2, GL_SRC_COLOR),
	CQUAD(48, 36),
	DEFAULTS, RGB_FN(GL_MODULATE), SRC(1, GL_TEXTURE), OPND(1, GL_ONE_MINUS_SRC_COLOR), A_FN(GL_ADD),
	ASRC(1, GL_TEXTURE), CQUAD(64, 36),
	DEFAULTS, RGB_FN(GL_ADD), SRC(1, GL_CONSTANT_EXT), OPND(1, GL_ONE_MINUS_SRC_ALPHA), A_FN(GL_REPLACE),
	ASRC(0, GL_CONSTANT_EXT), AOPND(0, GL_ONE_MINUS_SRC_ALPHA), ENVF(GL_ALPHA_SCALE, 4), CQUAD(80, 36),
	{ "glBindTexture", { 0x0DE1, 2 } }, TEXFMT(0x803C), TEXP(GL_TEXTURE_MIN_FILTER, GL_NEAREST),    /* (alpha) */
	DEFAULTS, CQUAD(0, 52),
	{ "glBindTexture", { 0x0DE1, 3 } }, TEXFMT(0x8040), TEXP(GL_TEXTURE_MIN_FILTER, GL_NEAREST),    /* (luminance) */
	DEFAULTS, RGB_FN(GL_ADD), CQUAD(16, 52),
	{ "glBindTexture", { 0x0DE1, 4 } }, TEXFMT(0x804B), TEXP(GL_TEXTURE_MIN_FILTER, GL_NEAREST),    /* (intensity) */
	DEFAULTS, RGB_FN(GL_INTERPOLATE_EXT), SRC(2, GL_TEXTURE), CQUAD(32, 52),
	{ "glBindTexture", { 0x0DE1, 5 } }, TEXFMT(0x8045), TEXP(GL_TEXTURE_MIN_FILTER, GL_NEAREST),    /* (L, A) */
	DEFAULTS, RGB_FN(GL_REPLACE), OPND(0, GL_SRC_ALPHA), A_FN(GL_REPLACE), CQUAD(48, 52),
	{ "glBindTexture", { 0x0DE1, 6 } }, TEXFMT(0x8051), TEXP(GL_TEXTURE_MIN_FILTER, GL_NEAREST),    /* (RGB) */
	DEFAULTS, A_FN(GL_REPLACE), CQUAD(64, 52),
	{ "glBindTexture", { 0x0DE1, 1 } }, ENVI(GL_TEXTURE_ENV_MODE, GL_MODULATE), CQUAD(80, 52),
	DIS(GL_TEXTURE_2D),
	{ NULL, { 0 } },
};

/*
 * ... on two units: unit 1's (a gradient, flipped) combined with unit 0's
 * result (GL_PREVIOUS_EXT), the primary color and its own constant; unit 0
 * built in and unit 1 combining, and the other way; unit 0 off (unit 1's
 * previous the primary color)
 */
#define UNIT(u) { "glActiveTextureARB", { GL_TEXTURE0_ARB + (u) } }
static const struct op scene_combine2[] = {
	ORTHO, CLEAR(0.2, 0.2, 0.2), COMBINE_DEFAULTS,
	UNIT(0), EN(GL_TEXTURE_2D), { "glBindTexture", { 0x0DE1, 1 } }, TEX(1, 8, 8, IMAGE_CHECKER),
	TEXP(GL_TEXTURE_MIN_FILTER, GL_NEAREST), TEXP(GL_TEXTURE_MAG_FILTER, GL_NEAREST),
	{ "glTexEnvfv", { GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, 0.3, 0.7, 0.5, 0.6 } }, DEFAULTS,
	UNIT(1), EN(GL_TEXTURE_2D), { "glBindTexture", { 0x0DE1, 2 } }, TEX(2, 8, 8, IMAGE_GRADIENT),
	TEXP(GL_TEXTURE_MIN_FILTER, GL_NEAREST), TEXP(GL_TEXTURE_MAG_FILTER, GL_NEAREST),
	{ "glTexEnvfv", { GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, 0.9, 0.2, 0.6, 0.35 } },
	DEFAULTS, RGB_FN(GL_ADD), SRC(0, GL_PREVIOUS_EXT), SRC(1, GL_TEXTURE), A_FN(GL_ADD), CQUAD(0, 4),
	DEFAULTS, RGB_FN(GL_INTERPOLATE_EXT), SRC(1, GL_PREVIOUS_EXT), SRC(2, GL_PREVIOUS_EXT), A_FN(GL_REPLACE),
	ASRC(0, GL_PREVIOUS_EXT), CQUAD(16, 4),
	DEFAULTS, SRC(0, GL_PREVIOUS_EXT), SRC(1, GL_CONSTANT_EXT), OPND(1, GL_ONE_MINUS_SRC_COLOR),
	A_FN(GL_ADD_SIGNED_EXT), ASRC(0, GL_PREVIOUS_EXT), ASRC(1, GL_TEXTURE), CQUAD(32, 4),
	DEFAULTS, RGB_FN(GL_REPLACE), SRC(0, GL_PREVIOUS_EXT), OPND(0, GL_ONE_MINUS_SRC_ALPHA), A_FN(GL_REPLACE),
	ASRC(0, GL_PRIMARY_COLOR_EXT), CQUAD(48, 4),
	DEFAULTS, RGB_FN(GL_ADD_SIGNED_EXT), SRC(0, GL_PRIMARY_COLOR_EXT), SRC(1, GL_CONSTANT_EXT),
	ENVF(GL_RGB_SCALE_EXT, 2), ASRC(0, GL_CONSTANT_EXT), ENVF(GL_ALPHA_SCALE, 2), CQUAD(64, 4),
	DEFAULTS, RGB_FN(GL_INTERPOLATE_EXT), SRC(0, GL_PREVIOUS_EXT), SRC(1, GL_CONSTANT_EXT), SRC(2, GL_TEXTURE),
	OPND(2, GL_ONE_MINUS_SRC_COLOR), A_FN(GL_INTERPOLATE_EXT), ASRC(1, GL_PRIMARY_COLOR_EXT),
	ASRC(2, GL_PREVIOUS_EXT), AOPND(2, GL_ONE_MINUS_SRC_ALPHA), CQUAD(80, 4),
	UNIT(0), ENVI(GL_TEXTURE_ENV_MODE, GL_REPLACE), UNIT(1), DEFAULTS, CQUAD(0, 24),
	UNIT(0), DEFAULTS, RGB_FN(GL_ADD_SIGNED_EXT), UNIT(1), ENVI(GL_TEXTURE_ENV_MODE, GL_ADD), CQUAD(16, 24),
	UNIT(0), DIS(GL_TEXTURE_2D), UNIT(1), DEFAULTS, RGB_FN(GL_REPLACE), SRC(0, GL_PREVIOUS_EXT),
	OPND(0, GL_SRC_ALPHA), CQUAD(32, 24),
	DIS(GL_TEXTURE_2D), UNIT(0),
	{ NULL, { 0 } },
};

/*
 * The accumulation buffer (the generic renderer's: not Mesa's, which has
 * none for framebuffer objects): two images averaged, scaled, moved, the
 * result returned within a scissor box, cleared in part
 */
static const struct op scene_accum[] = {
	ORTHO, CLEAR(0.1, 0.1, 0.1), { "glClearAccum", { 0.25, -0.5, 0.125, 1 } },
	{ "glClear", { GL_ACCUM_BUFFER_BIT } },
	BEGIN(GL_TRIANGLES), C3(1, 0, 0), V2(4.3, 4.6), C3(0, 1, 0), V2(80.4, 10.2), C3(0, 0, 1), V2(30.7, 66.3), END,
	{ "glAccum", { GL_LOAD, 0.5 } },
	CLEAR(0.1, 0.1, 0.1), C3(0.9, 0.9, 0.2), QUAD2(40.3, 20.6, 90.4, 60.2),
	{ "glAccum", { GL_ACCUM, 0.5 } }, { "glAccum", { GL_MULT, 1.5 } }, { "glAccum", { GL_ADD, -0.1 } },
	{ "glScissor", { 10, 8, 60, 50 } }, EN(GL_SCISSOR_TEST), { "glClearAccum", { 0.6, 0.2, 0.8, 1 } },
	{ "glClear", { GL_ACCUM_BUFFER_BIT } }, { "glScissor", { 0, 0, SW, SH } },
	CLEAR(0.3, 0.3, 0.3), { "glScissor", { 4, 2, 80, 64 } }, { "glColorMask", { 1, 1, 0, 1 } },
	{ "glAccum", { GL_RETURN, 1.0 } }, { "glColorMask", { 1, 1, 1, 1 } }, DIS(GL_SCISSOR_TEST),
	{ NULL, { 0 } },
};

static const struct {
	const char *name;
	const struct op *ops;
	unsigned allowed, edges;
} scenes[] = {
	{ "gradient", scene_gradient, 0, 0 }, { "depth", scene_depth, 2, 2 }, { "flat", scene_flat, 2, 2 },
	{ "smooth", scene_smooth, SW * SH / 2, 2 }, { "primitives", scene_primitives, 0, 0 },
	{ "blend", scene_blend, 4, 4 }, { "texture", scene_texture, 2, 2 }, { "clamp", scene_clamp, SW * SH, 0 },
	{ "edge", scene_edge, 0, 0 }, { "multitexture", scene_multitexture, 0, 0 },
	{ "lighting", scene_lighting, 24, 2 }, { "specular", scene_specular, 4, 2 }, { "fog", scene_fog, 4, 2 },
	{ "texgen", scene_texgen, 4, 2 }, { "arrays", scene_arrays, 4, 2 }, { "stencil", scene_stencil, 4, 2 },
	{ "offset", scene_offset, 4, 2 }, { "clip", scene_clip, 4, 2 }, { "lists", scene_lists, 4, 2 },
	{ "pixels", scene_pixels, 4, 2 }, { "texture3d", scene_texture3d, 4, 2 }, { "glu", scene_glu, 40, 40 },
	{ "eval", scene_eval, 0, 0 }, { "eval_lit", scene_eval_lit, 0, 0 }, { "logicop", scene_logicop, 0, 0 },
	{ "stipple", scene_stipple, 48, 48 }, { "lines", scene_lines, 4, 0 }, { "smoothlines", scene_smooth_lines, 450, 150 },
	{ "texlines", scene_texlines, 64, 0 }, { "accum", scene_accum, 0, 0 }, { "lodbias", scene_lodbias, 0, 0 },
	{ "lodrange", scene_lodrange, 0, 0 }, { "combine", scene_combine, 0, 0 }, { "combine2", scene_combine2, 0, 0 },
};
#define SCENES (sizeof scenes / sizeof scenes[0])

static uint32_t images[2][SCENES][SW * SH];     /* drawn in software, on the GPU: 0xAARRGGBB, rows top first */

/*
 * Selection and feedback: what each glRenderMode returned, and the program's
 * buffer then (the words it holds, up to FB_WORDS; set to FB_UNSET first),
 * ours and Mesa's, to be compared
 */
#define FBBUF 0x1A0000u                 /* (ours: in the guest's memory) */
#define FB_WORDS 1024
#define FB_SNAPS 8
#define FB_UNSET 0x7FC0DEADu
struct fb_snap {
	int32_t result;
	uint32_t mode;                          /* (the mode it left) */
	uint32_t words[FB_WORDS];
};
static struct {
	struct fb_snap snap[FB_SNAPS];
	unsigned n, size;
	uint32_t mode;
} fb[2];

/* A glRenderMode's result, and the buffer as it left the mode */
static void fb_snapshot(unsigned side, int32_t result, uint32_t new_mode, const uint32_t *words)
{
	if (fb[side].n < FB_SNAPS) {
		struct fb_snap *s = &fb[side].snap[fb[side].n++];
		s->result = result;
		s->mode = fb[side].mode ? fb[side].mode : GL_RENDER;
		memcpy(s->words, words, fb[side].size * sizeof *words);
		for (unsigned k = fb[side].size; k < FB_WORDS; k++)
			s->words[k] = FB_UNSET;
	}
	fb[side].mode = new_mode;
}

/* A scene's call to OpenGLUtility (a quadric's: the scene's quadric's) */
static uint32_t scene_quadric;

static void glu_invoke(const struct op *op)
{
	const char *sig = glu_functions[glu_entry(op->name)].args;
	uint32_t w[24] = { 0 };                 /* (gluLookAt's nine doubles take 18 words) */
	double f[13];
	unsigned nw = 0, nf = 0;
	for (const char *c = sig; *c; c++) {
		double v = op->a[c - sig];
		if (*c == 'p' && c == sig) {
			w[nw++] = scene_quadric;
		} else if (*c == 'p') {
			uint32_t at = ARRAYDATA + 0x1000u * (unsigned)v;
			array_bytes((unsigned)v, true, mem_ptr(&mem, at));
			w[nw++] = at;
		} else if (*c == 'f') {
			f[nf++] = (float)v;
			nw++;
		} else if (*c == 'd') {
			f[nf++] = v;
			nw += 2;
		} else {
			w[nw++] = (uint32_t)(int64_t)v;
		}
	}
	call_glu(op->name, w, nw, f, nf);
}

static void our_invoke(const struct op *op)
{
	if (!strncmp(op->name, "glu", 3)) {
		glu_invoke(op);
		return;
	}
	int fn = opengl_entry(op->name);
	CHECK(fn >= 0);
	if (fn < 0)
		return;
	bool select = !strcmp(op->name, "glSelectBuffer"), feedback = !strcmp(op->name, "glFeedbackBuffer");
	if (select || feedback) {               /* (the buffer at FBBUF, unset) */
		fb[0].size = op->a[0] < FB_WORDS ? (unsigned)op->a[0] : FB_WORDS;
		for (unsigned k = 0; k < FB_WORDS; k++)
			mem_w32(&mem, FBBUF + 4 * k, FB_UNSET);
		if (select)
			call_entry((unsigned)fn, WORDS((uint32_t)op->a[0], FBBUF), NULL, 0);
		else
			call_entry((unsigned)fn, WORDS((uint32_t)op->a[0], (uint32_t)op->a[1], FBBUF), NULL, 0);
		return;
	}
	if (!strcmp(op->name, "glRenderMode")) {
		uint32_t words[FB_WORDS];
		int32_t r = (int32_t)call_entry((unsigned)fn, WORDS((uint32_t)op->a[0]), NULL, 0);
		for (unsigned k = 0; k < FB_WORDS; k++)
			words[k] = mem_r32(&mem, FBBUF + 4 * k);
		fb_snapshot(0, r, (uint32_t)op->a[0], words);
		return;
	}
	if (!strcmp(op->name, "glTexImage2D")) {
		int w = (int)op->a[3], h = (int)op->a[4];
		memcpy(mem_ptr(&mem, TEXDATA), image((unsigned)op->a[5], w, h), (size_t)(w * h * 4));
		const uint32_t words[9] = { (uint32_t)op->a[0], (uint32_t)op->a[1], (uint32_t)op->a[2], (uint32_t)w,
					    (uint32_t)h, 0, 0x1908, 0x1401, TEXDATA };
		call_entry((unsigned)fn, words, 9, NULL, 0);
		return;
	}
	uint32_t w[16] = { 0 };
	double f[13];
	unsigned nw = 0, nf = 0;
	const char *sig = functions[fn].args, *pointer = strchr(sig, 'p');
	uint32_t at = 0;                        /* (the pointer argument, if there's one) */
	if (pointer) {
		unsigned k = (unsigned)(pointer - sig);
		if (takes_array(op->name)) {
			unsigned id = (unsigned)op->a[k];
			at = ARRAYDATA + 0x1000u * id;
			array_bytes(id, true, mem_ptr(&mem, at));
		} else {
			at = VECDATA;
			for (unsigned j = k; j < 6; j++)
				put_value(mem_ptr(&mem, VECDATA + (vector_of_doubles(op->name) ? 8 : 4) * (j - k)),
					  vector_of_doubles(op->name) ? GL_DOUBLE : GL_FLOAT, op->a[j], true);
		}
	}
	for (const char *c = sig; *c; c++) {
		double v = op->a[c - sig];
		if (*c == 'p') {
			w[nw++] = at;
		} else if (*c == 'f') {                 /* (as a compiled caller passes a float) */
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
static bool scene_accum_buffer;         /* (our_scene()'s pixel format has an accumulation buffer) */

static void our_scene(const struct op *ops, uint32_t *out)
{
	call_entry(OPENGL_ENTRY_INIT, NULL, 0, NULL, 0);
	make_port(32);
	const uint32_t attribs[] = { AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 24, AGL_STENCIL_SIZE, 8,
				     scene_accum_buffer ? AGL_ACCUM_RED_SIZE : AGL_NONE, 16, AGL_ACCUM_ALPHA_SIZE, 16,
				     AGL_NONE };
	put_attribs(attribs, 11);
	uint32_t pf = CALL(aglChoosePixelFormat, 0, 0, ATTRIBS), ctx = CALL(aglCreateContext, pf, 0);
	CALL(aglDestroyPixelFormat, pf);
	CHECK_EQ(CALL(aglSetOffScreen, ctx, SW, SH, SW * 4, OFFSCREEN), 1);
	CHECK_EQ(CALL(aglSetCurrentContext, ctx), 1);
	glu_heap = GLU_HEAP;
	scene_quadric = CALLU0(gluNewQuadric);
	for (const struct op *op = ops; op->name; op++)
		our_invoke(op);
	CALLU(gluDeleteQuadric, scene_quadric);
	CHECK_EQ(CALL0(glGetError), 0);
	CALL(aglSwapBuffers, ctx);
	for (unsigned k = 0; k < SW * SH; k++)
		out[k] = mem_r32(&mem, OFFSCREEN + 4 * k);
	CALL(aglDestroyContext, ctx);
	call_entry(OPENGL_ENTRY_TERM, NULL, 0, NULL, 0);
}

/* BLUE_TEST_SAVE=DIR: each scene's pixels saved there, as SCENE-soft.png, SCENE-gpu.png and SCENE-mesa.png */
static void save(const char *what, unsigned k, const uint32_t *img)
{
	const char *dir = getenv("BLUE_TEST_SAVE");
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

static unsigned differing(const uint32_t *a, const uint32_t *b, unsigned tolerance, unsigned *most);

/* A context on an offscreen drawable of the scenes' size, current; its AGLContext */
static uint32_t scene_context(void)
{
	call_entry(OPENGL_ENTRY_INIT, NULL, 0, NULL, 0);
	make_port(32);
	const uint32_t attribs[] = { AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 24, AGL_STENCIL_SIZE, 8, AGL_NONE };
	put_attribs(attribs, 7);
	uint32_t pf = CALL(aglChoosePixelFormat, 0, 0, ATTRIBS), ctx = CALL(aglCreateContext, pf, 0);
	CALL(aglDestroyPixelFormat, pf);
	CHECK_EQ(CALL(aglSetOffScreen, ctx, SW, SH, SW * 4, OFFSCREEN), 1);
	CHECK_EQ(CALL(aglSetCurrentContext, ctx), 1);
	return ctx;
}

/* Display lists' names, errors and state: what's kept and what's done there and then, nesting, glRect's v forms */
static void test_lists(void)
{
	uint32_t ctx = scene_context();
	const struct opengl_context *x = context_of(ctx);
	CHECK(x != NULL);
	if (!x)
		return;
	uint32_t first = CALL(glGenLists, 3);
	CHECK(first && CALL(glIsList, first) && CALL(glIsList, first + 2) && !CALL(glIsList, first + 3));
	CHECK_EQ(CALL(glGenLists, 0), 0);
	CALL(glDeleteLists, first + 1, 1);
	CHECK(!CALL(glIsList, first + 1) && CALL(glIsList, first + 2));
	CHECK_EQ(CALL(glGenLists, 2), first + 3);
	CHECK_EQ(CALL0(glGetError), 0);
	CALL(glGenLists, (uint32_t)-1);
	CHECK_EQ(CALL0(glGetError), GL_INVALID_VALUE);
	CALL(glNewList, 0, GL_COMPILE);
	CHECK_EQ(CALL0(glGetError), GL_INVALID_VALUE);
	CALL(glNewList, 5, 0x1234);
	CHECK_EQ(CALL0(glGetError), GL_INVALID_ENUM);
	CALL0(glEndList);
	CHECK_EQ(CALL0(glGetError), GL_INVALID_OPERATION);
	/* while one's made: its name and mode; state kept, not set; queries answered */
	CALL(glNewList, 5, GL_COMPILE);
	CALL(glGetIntegerv, 0x0B33, OUT);                       /* GL_LIST_INDEX */
	CALL(glGetIntegerv, 0x0B30, OUT + 4);                   /* GL_LIST_MODE */
	CHECK(mem_r32(&mem, OUT) == 5 && mem_r32(&mem, OUT + 4) == GL_COMPILE);
	CALL(glNewList, 6, GL_COMPILE);
	CHECK_EQ(CALL0(glGetError), GL_INVALID_OPERATION);
	CALL_F(glClearColor, 1, 0.5, 0, 1);
	CALL0(glEndList);
	CALL(glGetIntegerv, 0x0B33, OUT);
	CHECK(mem_r32(&mem, OUT) == 0 && x->st.clear_color[0] == 0);
	CALL(glCallList, 5);
	CHECK(x->st.clear_color[0] == 1 && x->st.clear_color[1] == 0.5f);
	CALL_F(glClearColor, 0, 0, 0, 0);
	CALL(glNewList, 9, GL_COMPILE_AND_EXECUTE);
	CALL_F(glClearColor, 0.25, 0, 0, 1);
	CALL0(glEndList);
	CHECK(x->st.clear_color[0] == 0.25f && CALL(glIsList, 9));
	/* a list calling itself: as deep as lists nest (64), then no deeper */
	CALL(glNewList, 7, GL_COMPILE);
	CALL_F(glTranslatef, 1, 0, 0);
	CALL(glCallList, 7);
	CALL0(glEndList);
	CALL0(glLoadIdentity);
	CALL(glCallList, 7);
	CALL(glGetDoublev, 0x0BA6, OUT);                        /* GL_MODELVIEW_MATRIX */
	uint64_t b = mem_r64(&mem, OUT + 8 * 12);
	double tx;
	memcpy(&tx, &b, 8);
	CHECK(tx == 64);
	CALL0(glLoadIdentity);
	/* glRectfv kept as its values were */
	static const float corners[4] = { 2.5f, 3.5f, 10.5f, 12.5f };
	for (int k = 0; k < 4; k++)
		mem_w32(&mem, ARRAY + 4 * (uint32_t)k, fbits(corners[k]));
	CALL(glNewList, 8, GL_COMPILE);
	CALL(glRectfv, ARRAY, ARRAY + 8);
	CALL0(glEndList);
	for (int k = 0; k < 4; k++)
		mem_w32(&mem, ARRAY + 4 * (uint32_t)k, fbits(40));
	CALL(glMatrixMode, GL_PROJECTION);
	CALL_F(glOrtho, 0, SW, 0, SH, -1, 1);
	CALL(glMatrixMode, GL_MODELVIEW);
	CALL(glClear, GL_COLOR_BUFFER_BIT);
	CALL(glCallList, 8);
	CHECK_EQ(CALL0(glGetError), 0);
	CALL(aglSwapBuffers, ctx);
	CHECK(mem_r32(&mem, OFFSCREEN + 4 * ((SH - 1 - 5) * SW + 5)) == 0xFFFFFFFFu);    /* (white: the color) */
	CHECK(mem_r32(&mem, OFFSCREEN + 4 * ((SH - 1 - 20) * SW + 20)) != 0xFFFFFFFFu);
	/* glListBase: its attribute group's */
	CALL(glListBase, 30);
	CALL(glPushAttrib, 0x20000);                            /* GL_LIST_BIT */
	CALL(glListBase, 40);
	CALL0(glPopAttrib);
	CALL(glGetIntegerv, 0x0B32, OUT);                       /* GL_LIST_BASE */
	CHECK_EQ(mem_r32(&mem, OUT), 30);
	CALL(glNewList, 10, GL_COMPILE);                        /* (a context gone while it makes one) */
	CALL(aglDestroyContext, ctx);
	call_entry(OPENGL_ENTRY_TERM, NULL, 0, NULL, 0);
}

/* ---- OpenGLUtility ------------------------------------------------------------------- */

enum { GL_MODELVIEW_MATRIX_T = 0x0BA6, GL_PROJECTION_MATRIX_T = 0x0BA7, GL_TEXTURE_WIDTH_T = 0x1000 };

static bool string_at(uint32_t at, const char *s)
{
	return at && !strcmp((const char *)mem_ptr(&mem, at), s);
}

static double mem_double(uint32_t at)
{
	uint64_t b = mem_r64(&mem, at);
	double d;
	memcpy(&d, &b, 8);
	return d;
}

static void put_doubles(uint32_t at, const double *v, unsigned n)
{
	for (unsigned k = 0; k < n; k++)
		mem_w64(&mem, at + 8 * k, dbits(v[k]));
}

/* A matrix (glGetDoublev's) as expected: within float's precision */
static bool matrix_is(uint32_t pname, const double want[16])
{
	CALL(glGetDoublev, pname, OUT);
	for (unsigned k = 0; k < 16; k++)
		if (fabs(mem_double(OUT + 8 * k) - want[k]) > 1e-6 * (1 + fabs(want[k])))
			return false;
	return true;
}

/* The width of a texture's level (0: none) */
static uint32_t level_width(uint32_t target, uint32_t level)
{
	CALL(glGetTexLevelParameteriv, target, level, GL_TEXTURE_WIDTH_T, OUT);
	return mem_r32(&mem, OUT);
}

/* As SGI's GLU scales: four 8-bit components' mean in 16 bits, back to 8 */
static uint8_t mean4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	return (uint8_t)(((uint32_t)(a + b + c + d) * 257 + 2) / 4 >> 8);
}

static void test_glu(void)
{
	/* the fragment: OpenGLLibrary's glc_cc imported as data (as Apple's OpenGLUtility does), the Memory Manager's */
	static uint8_t buf[OPENGL_LIBRARY_MAX];
	struct ndrv_layout l;
	CHECK(opengl_build_utility(buf, sizeof buf, &l) > 0);
	const uint8_t *ld = buf + be32(buf + 40 + 56 + 20), *symbols = ld + 56 + 24 * 2;
	CHECK(be32(ld + 24) == 2 && be32(ld + 28) == 3);       /* (two libraries, three symbols) */
	CHECK(symbols[0] == 1 && symbols[4] == 2 && symbols[8] == 2);   /* (data, then TVectors) */
	glu_load();
	call_entry(OPENGL_ENTRY_INIT, NULL, 0, NULL, 0);

	/* strings, as Apple's */
	CHECK(string_at(CALLU(gluGetString, 100800), "1.2 MacOS 8.0"));
	CHECK(string_at(CALLU(gluGetString, 100801), "GL_EXT_texture"));
	CHECK_EQ(CALLU(gluGetString, 5), 0);
	CHECK(string_at(CALLU(gluErrorString, 0), "no error"));
	CHECK(string_at(CALLU(gluErrorString, 0x0503), "stack overflow"));
	CHECK(string_at(CALLU(gluErrorString, GLU_INVALID_VALUE_T), "invalid value"));
	CHECK(string_at(CALLU(gluErrorString, 100251), "spline order un-supported"));
	CHECK(string_at(CALLU(gluErrorString, 100261), "missing or extra domain data"));
	CHECK(string_at(CALLU(gluErrorString, 100287), "duplicate point on pwlcurve"));
	CHECK(string_at(CALLU(gluErrorString, 100156), "need combine callback"));
	CHECK_EQ(CALLU(gluErrorString, 100903), 0);             /* (GLU_INCOMPATIBLE_GL_VERSION: Apple's has none) */

	/* quadrics: the program's NewPtrClear's blocks, with the defaults; errors to their callback */
	glu_heap = GLU_HEAP;
	uint32_t q = CALLU0(gluNewQuadric);
	CHECK(q == GLU_HEAP && mem_r32(&mem, q) == 0x73795175u && mem_r32(&mem, q + 4) == GLU_FILL_T);
	CHECK(mem_r32(&mem, q + 8) == GLU_SMOOTH_T && mem_r32(&mem, q + 12) == GLU_OUTSIDE_T);
	CALLU(gluQuadricCallback, q, GLU_ERROR_T, ERROR_TV);
	glu_errors = 0;
	CALLU(gluQuadricDrawStyle, q, 5);
	CHECK(glu_errors == 1 && glu_error == GLU_INVALID_ENUM_T);
	call_glu("gluSphere", WORDS(q, 0, 0, 4, 4), DOUBLES(-1));
	CHECK(glu_errors == 2 && glu_error == GLU_INVALID_VALUE_T);
	CALLU(gluQuadricNormals, q, GLU_FLAT_T);
	CHECK_EQ(mem_r32(&mem, q + 8), GLU_FLAT_T);
	/* tessellators' and NURBS renderers' properties */
	uint32_t t = CALLU0(gluNewTess), n = CALLU0(gluNewNurbsRenderer);
	CHECK(t && n && t != q && n != t);
	call_glu("gluTessProperty", WORDS(t, 100140), DOUBLES(100132));        /* GLU_TESS_WINDING_POSITIVE */
	CALLU(gluGetTessProperty, t, 100140, OUT);
	CHECK(mem_double(OUT) == 100132);
	call_glu("gluTessProperty", WORDS(t, 100142), DOUBLES(2));             /* (a tolerance past 1: kept as it was) */
	CALLU(gluGetTessProperty, t, 100142, OUT);
	CHECK(mem_double(OUT) == 0);
	CALLU(gluGetNurbsProperty, n, 100203, OUT);                             /* GLU_SAMPLING_TOLERANCE */
	CHECK_EQ(mem_r32(&mem, OUT), fbits(50));
	call_glu("gluNurbsProperty", WORDS(n, 100203), DOUBLES(25));
	CALLU(gluGetNurbsProperty, n, 100203, OUT);
	CHECK_EQ(mem_r32(&mem, OUT), fbits(25));
	glu_disposed = 0;
	CALLU(gluDeleteNurbsRenderer, n);
	CHECK_EQ(glu_disposed, n);
	CALLU(gluDeleteTess, t);
	CHECK_EQ(glu_disposed, t);
	CALLU(gluDeleteQuadric, q);
	CHECK(glu_disposed == q && mem_r32(&mem, q) == 0);
	glu_disposed = 0;
	CALLU(gluDeleteQuadric, q);                             /* (no longer one: nothing) */
	CHECK_EQ(glu_disposed, 0);

	/* matrices, as SGI's make them */
	uint32_t ctx = scene_context();
	struct opengl_context *x = context_of(ctx);
	CHECK(x != NULL);
	if (!x)
		return;
	CALL(glMatrixMode, GL_PROJECTION);
	call_glu("gluOrtho2D", NULL, 0, DOUBLES(0, 96, 0, 72));
	const double ortho[16] = { 2.0 / 96, 0, 0, 0, 0, 2.0 / 72, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1 };
	CHECK(matrix_is(GL_PROJECTION_MATRIX_T, ortho));
	CALL0(glLoadIdentity);
	call_glu("gluPerspective", NULL, 0, DOUBLES(90, 2, 1, 3));
	const double persp[16] = { 0.5, 0, 0, 0, 0, 1, 0, 0, 0, 0, -2, -1, 0, 0, -3, 0 };
	CHECK(matrix_is(GL_PROJECTION_MATRIX_T, persp));
	CALL(glMatrixMode, GL_MODELVIEW);
	call_glu("gluLookAt", NULL, 0, DOUBLES(1, 2, 5, 1, 2, 0, 0, 1, 0));
	const double look[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1, -2, -5, 1 };
	CHECK(matrix_is(GL_MODELVIEW_MATRIX_T, look));
	CALL0(glLoadIdentity);
	call_glu("gluLookAt", NULL, 0, DOUBLES(0, 0, 0, 3, 0, 0, 0, 0, 1));    /* (looking along x, z up) */
	const double side[16] = { 0, 0, -1, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1 };
	CHECK(matrix_is(GL_MODELVIEW_MATRIX_T, side));
	CALL0(glLoadIdentity);
	const uint32_t vp = ARRAY + 0x100;
	const uint32_t viewport[4] = { 0, 0, 96, 72 };
	for (int k = 0; k < 4; k++)
		mem_w32(&mem, vp + 4 * (uint32_t)k, viewport[k]);
	call_glu("gluPickMatrix", WORDS(0, 0, 0, 0, 0, 0, 0, 0, vp), DOUBLES(24, 36, 4, 6));
	const double pick[16] = { 24, 0, 0, 0, 0, 12, 0, 0, 0, 0, 1, 0, 12, 0, 0, 1 };
	CHECK(matrix_is(GL_MODELVIEW_MATRIX_T, pick));
	CALL0(glLoadIdentity);

	/* gluProject, and gluUnProject back: through a translation and a frustum, onto a viewport */
	const double model[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5, -0.25, -3, 1 };
	const double proj[16] = { 1.5, 0, 0, 0, 0, 2, 0, 0, 0, 0, -11.0 / 9, -1, 0, 0, -20.0 / 9, 0 };
	const uint32_t mm = ARRAY, pm = ARRAY + 0x80, vp2 = ARRAY + 0x110;
	const uint32_t viewport2[4] = { 10, 20, 200, 100 };
	put_doubles(mm, model, 16);
	put_doubles(pm, proj, 16);
	for (int k = 0; k < 4; k++)
		mem_w32(&mem, vp2 + 4 * (uint32_t)k, viewport2[k]);
	CHECK_EQ(call_glu("gluProject", WORDS(0, 0, 0, 0, 0, 0, mm, pm, vp2, OUT, OUT + 8, OUT + 16),
			  DOUBLES(0.2, 0.3, 0.1)), 1);
	/* (eye (0.7, 0.05, -2.9); clip w 2.9) */
	double ex = 1.5 * 0.7 / 2.9, ey = 2 * 0.05 / 2.9, ez = (-11.0 / 9 * -2.9 - 20.0 / 9) / 2.9;
	CHECK(fabs(mem_double(OUT) - ((ex * 0.5 + 0.5) * 200 + 10)) < 1e-9);
	CHECK(fabs(mem_double(OUT + 8) - ((ey * 0.5 + 0.5) * 100 + 20)) < 1e-9);
	CHECK(fabs(mem_double(OUT + 16) - (ez * 0.5 + 0.5)) < 1e-9);
	double win[3] = { mem_double(OUT), mem_double(OUT + 8), mem_double(OUT + 16) };
	CHECK_EQ(call_glu("gluUnProject", WORDS(0, 0, 0, 0, 0, 0, mm, pm, vp2, OUT + 32, OUT + 40, OUT + 48),
			  DOUBLES(win[0], win[1], win[2])), 1);
	CHECK(fabs(mem_double(OUT + 32) - 0.2) < 1e-9 && fabs(mem_double(OUT + 40) - 0.3) < 1e-9 &&
	      fabs(mem_double(OUT + 48) - 0.1) < 1e-9);
	const double zeros[16] = { 0 };
	put_doubles(mm, zeros, 16);
	CHECK_EQ(call_glu("gluUnProject", WORDS(0, 0, 0, 0, 0, 0, mm, pm, vp2, OUT, OUT + 8, OUT + 16),
			  DOUBLES(1, 2, 0.5)), 0);      /* (no inverse) */

	/* gluScaleImage: 16-bit means (2 x 2 halved), a pixel enlarged, luminance boxed; errors */
	static const uint8_t px[16] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 131, 141, 151, 161 };
	memcpy(mem_ptr(&mem, ARRAY), px, 16);
	CHECK_EQ(CALLU(gluScaleImage, GL_RGBA, 2, 2, GL_UNSIGNED_BYTE, ARRAY, 1, 1, GL_UNSIGNED_BYTE, OUT), 0);
	for (int k = 0; k < 4; k++)
		CHECK_EQ(mem_r8(&mem, OUT + (uint32_t)k), mean4(px[k], px[4 + k], px[8 + k], px[12 + k]));
	CHECK_EQ(CALLU(gluScaleImage, GL_RGBA, 1, 1, GL_UNSIGNED_BYTE, ARRAY, 3, 2, GL_UNSIGNED_BYTE, OUT), 0);
	CHECK(!memcmp(mem_ptr(&mem, OUT + 20), px, 4));
	mem_w8(&mem, ARRAY, 10);
	mem_w8(&mem, ARRAY + 1, 21);
	CHECK_EQ(CALLU(gluScaleImage, GL_LUMINANCE, 2, 1, GL_UNSIGNED_BYTE, ARRAY, 1, 1, GL_UNSIGNED_BYTE, OUT), 0);
	CHECK_EQ(mem_r8(&mem, OUT), 15);                        /* ((10 + 21) x 257 / 2: 3983.75, of 65535) */
	CHECK_EQ(CALLU(gluScaleImage, GL_RGBA, -1, 1, GL_UNSIGNED_BYTE, ARRAY, 1, 1, GL_UNSIGNED_BYTE, OUT),
		 GLU_INVALID_VALUE_T);
	CHECK_EQ(CALLU(gluScaleImage, GL_COLOR_INDEX, 1, 1, GL_UNSIGNED_BYTE, ARRAY, 1, 1, GL_UNSIGNED_BYTE, OUT),
		 GLU_INVALID_ENUM_T);
	CHECK_EQ(CALLU(gluScaleImage, GL_RGBA, 0, 1, GL_UNSIGNED_BYTE, ARRAY, 1, 1, GL_UNSIGNED_BYTE, OUT), 0);

	/* mipmaps: halved down to 1 x 1, from the image scaled to powers of two (as SGI's GLU 1.2) */
	memcpy(mem_ptr(&mem, ARRAY), px, 16);
	memcpy(mem_ptr(&mem, ARRAY + 16), px, 16);
	CALL(glBindTexture, GL_TEXTURE_2D, 7);
	CHECK_EQ(CALLU(gluBuild2DMipmaps, GL_TEXTURE_2D, GL_RGBA, 4, 2, GL_RGBA, GL_UNSIGNED_BYTE, ARRAY), 0);
	CHECK(level_width(GL_TEXTURE_2D, 0) == 4 && level_width(GL_TEXTURE_2D, 1) == 2);
	CHECK(level_width(GL_TEXTURE_2D, 2) == 1 && level_width(GL_TEXTURE_2D, 3) == 0);
	CALL(glPixelStorei, 0x0D05, 1);                         /* GL_PACK_ALIGNMENT */
	CALL(glGetTexImage, GL_TEXTURE_2D, 1, GL_RGBA, GL_UNSIGNED_BYTE, OUT);
	for (int k = 0; k < 4; k++)                             /* (the 4 x 2 image's left half) */
		CHECK(abs(mem_r8(&mem, OUT + (uint32_t)k) - mean4(px[k], px[4 + k], px[k], px[4 + k])) <= 1);
	CHECK_EQ(CALLU(gluBuild2DMipmaps, GL_TEXTURE_2D, 3, 3, 3, GL_RGB, GL_UNSIGNED_BYTE, ARRAY), 0);
	CHECK(level_width(GL_TEXTURE_2D, 0) == 4 && level_width(GL_TEXTURE_2D, 2) == 1);
	CALL(glBindTexture, GL_TEXTURE_1D, 8);
	CHECK_EQ(CALLU(gluBuild1DMipmaps, GL_TEXTURE_1D, 3, 5, GL_RGB, GL_UNSIGNED_BYTE, ARRAY), 0);
	CHECK(level_width(GL_TEXTURE_1D, 0) == 4 && level_width(GL_TEXTURE_1D, 2) == 1);
	CHECK_EQ(CALL0(glGetError), 0);
	CHECK_EQ(CALLU(gluBuild2DMipmaps, GL_TEXTURE_2D, 3, 0, 3, GL_RGB, GL_UNSIGNED_BYTE, ARRAY), GLU_INVALID_VALUE_T);
	CHECK_EQ(CALLU(gluBuild2DMipmaps, GL_TEXTURE_2D, 3, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, ARRAY),
		 GLU_INVALID_ENUM_T);

	/* a quadric made into a display list: its gl calls kept; drawn */
	q = CALLU0(gluNewQuadric);
	CALL(glNewList, 3, GL_COMPILE);
	call_glu("gluDisk", WORDS(q, 0, 0, 0, 0, 12, 1), DOUBLES(0, 0.5));
	CALL0(glEndList);
	const struct gl_list *list = glf_list(&x->st, 3);
	CHECK(list && list->len > 0);
	CALL(glMatrixMode, GL_PROJECTION);
	CALL0(glLoadIdentity);
	CALL(glMatrixMode, GL_MODELVIEW);
	CALL(glClear, GL_COLOR_BUFFER_BIT);
	CALL(glCallList, 3);
	CHECK_EQ(CALL0(glGetError), 0);
	CALL(aglSwapBuffers, ctx);
	CHECK_EQ(mem_r32(&mem, OFFSCREEN + 4 * ((SH / 2) * SW + SW / 2)), 0xFFFFFFFFu);         /* (white) */
	CHECK_EQ(mem_r32(&mem, OFFSCREEN), 0);                  /* (the clear color: 0, 0, 0, 0) */
	CALL(aglDestroyContext, ctx);
	call_entry(OPENGL_ENTRY_TERM, NULL, 0, NULL, 0);
	/* no context (the process gone): nothing drawn, images still scaled */
	call_glu("gluSphere", WORDS(q, 0, 0, 4, 4), DOUBLES(1));
	CHECK_EQ(CALLU(gluScaleImage, GL_RGBA, 1, 1, GL_UNSIGNED_BYTE, ARRAY, 1, 1, GL_UNSIGNED_BYTE, OUT), 0);
	CHECK(!memcmp(mem_ptr(&mem, OUT), px, 4));
}

/*
 * Each scene made into a display list, then called: exactly the same pixels from Blue's rasterizer; a
 * GPU's driver needn't round alike from one drawing to the next (Apple's software renderer, on CI's
 * Macs, differs by a step now and then), so there within one step
 */
static void test_scenes_listed(unsigned backend)
{
	static struct op listed[1024];
	static uint32_t img[SW * SH];
	for (unsigned k = 0; k < SCENES; k++) {
		unsigned n = 0;
		bool lists = false;                     /* (lists aren't made while one is) */
		listed[n++] = (struct op){ "glNewList", { 99, GL_COMPILE } };
		for (const struct op *op = scenes[k].ops; op->name && n < 1020; op++) {
			listed[n++] = *op;
			lists |= !strcmp(op->name, "glNewList");
		}
		if (lists)
			continue;
		listed[n++] = (struct op){ "glEndList", { 0 } };
		listed[n++] = (struct op){ "glCallList", { 99 } };
		listed[n] = (struct op){ NULL, { 0 } };
		scene_accum_buffer = scenes[k].ops == scene_accum;
		our_scene(listed, img);
		scene_accum_buffer = false;
		unsigned most, d = differing(img, images[backend][k], 0, &most);
		if (d)
			printf("opengl: %s made into a list: %u pixels differ, by up to %u\n", scenes[k].name, d, most);
		if (backend)
			CHECK(most <= 1);
		else
			CHECK_EQ(d, 0);
	}
}

static void test_scenes(unsigned backend)
{
	for (unsigned k = 0; k < SCENES; k++) {
		scene_accum_buffer = scenes[k].ops == scene_accum;
		our_scene(scenes[k].ops, images[backend][k]);
		scene_accum_buffer = false;
		save(backend ? "gpu" : "soft", k, images[backend][k]);
	}
	test_scenes_listed(backend);
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

/* BLUE_TEST_VERBOSE=1: how far apart the scenes are */
static bool verbose(void)
{
	const char *v = getenv("BLUE_TEST_VERBOSE");
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

static void *glu_lib, *mesa_quadric;   /* (SGI's GLU: BLUE_TEST_GLU's) */
static char mesa_renderer[128];         /* (its GL_RENDERER) */

/*
 * What a Mesa driver draws wrong, not compared on it: radeonsi (Mesa
 * 26.2, on a Radeon RX 6800 XT) draws glCopyPixels' copies of depths with
 * no color at all (rather than the raster color), and writes their depths
 * with the depth test off too; and it draws an aliased point of size 2.6
 * as a square 2.6 pixels across (as core profiles' points are), where
 * OpenGL 1.2's 3.3 rounds the size to 3. llvmpipe draws them as OpenGL
 * says.
 */
static const struct {
	const char *scene, *renderer, *what;
	int left, top, right, bottom;           /* (pixels; rows top first) */
} mesa_wrong[] = {
	{ "pixels", "radeonsi", "glCopyPixels' depths", 68, SH - 71, 92, SH - 59 },
	{ "lines", "radeonsi", "an aliased point 2.6 across", 80, SH - 9, 85, SH - 4 },
};

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
	const char *glu = getenv("BLUE_TEST_GLU");
	if (glu && *glu) {
		glu_lib = dlopen(glu, RTLD_NOW | RTLD_LOCAL);
		CHECK(glu_lib != NULL);
	}
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

/* A call to SGI's GLU by its signature (those the scenes use; a quadric's: the scene's) */
static void mesa_glu_invoke(const struct op *op)
{
	void (*p)(void) = NULL;
	if (glu_lib)
		*(void **)&p = dlsym(glu_lib, op->name);
	CHECK(p != NULL);
	if (!p)
		return;
	const char *sig = glu_functions[glu_entry(op->name)].args;
	const double *a = op->a;
	void *q = mesa_quadric;
	static uint8_t data[0x1000];
	if (!strcmp(sig, "dddd"))
		((void (*)(double, double, double, double))p)(a[0], a[1], a[2], a[3]);
	else if (!strcmp(sig, "ddddddddd"))
		((void (*)(double, double, double, double, double, double, double, double, double))p)(
			a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
	else if (!strcmp(sig, "pdii"))
		((void (*)(void *, double, int32_t, int32_t))p)(q, a[1], (int32_t)a[2], (int32_t)a[3]);
	else if (!strcmp(sig, "pdddii"))
		((void (*)(void *, double, double, double, int32_t, int32_t))p)(q, a[1], a[2], a[3], (int32_t)a[4],
										   (int32_t)a[5]);
	else if (!strcmp(sig, "pddii"))
		((void (*)(void *, double, double, int32_t, int32_t))p)(q, a[1], a[2], (int32_t)a[3], (int32_t)a[4]);
	else if (!strcmp(sig, "pddiidd"))
		((void (*)(void *, double, double, int32_t, int32_t, double, double))p)(q, a[1], a[2], (int32_t)a[3],
											 (int32_t)a[4], a[5], a[6]);
	else if (!strcmp(sig, "pI"))
		((void (*)(void *, uint32_t))p)(q, (uint32_t)a[1]);
	else if (!strcmp(sig, "pB"))
		((void (*)(void *, uint8_t))p)(q, (uint8_t)a[1]);
	else if (!strcmp(sig, "IiiiIIp")) {
		array_bytes((unsigned)a[6], false, data);
		CHECK_EQ(((int32_t (*)(uint32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *))p)(
				 (uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (uint32_t)a[4], (uint32_t)a[5],
				 data), 0);
	} else {
		CHECK(!"a signature mesa_glu_invoke doesn't take");
	}
}

/* A call by its signature (those the scenes use) */
static void mesa_invoke(const struct op *op)
{
	if (!strncmp(op->name, "glu", 3)) {
		mesa_glu_invoke(op);
		return;
	}
	/*
	 * Mesa 25.2's glDrawPixels with GL_MAP_COLOR on frees a block it goes on
	 * using: its blue and alpha come out as garbage (identity maps too), and
	 * when the context goes it writes into the block, corrupting the heap
	 * (glibc aborts on a later free, or something crashes: in some runs,
	 * as threads' allocations fall)
	 */
	if (!strcmp(op->name, "glPixelTransferi") && op->a[0] == 0x0D10 && op->a[1] != 0) {
		CHECK(!"GL_MAP_COLOR in a scene drawn by Mesa");
		return;
	}
	int fn = opengl_entry(op->name);
	void (*p)(void) = fn >= 0 ? mesa_fn(op->name) : NULL;
	CHECK(p != NULL);
	if (!p)
		return;
	static uint32_t fbbuf[FB_WORDS];        /* (the program's selection or feedback buffer) */
	if (!strcmp(op->name, "glSelectBuffer") || !strcmp(op->name, "glFeedbackBuffer")) {
		fb[1].size = op->a[0] < FB_WORDS ? (unsigned)op->a[0] : FB_WORDS;
		for (unsigned k = 0; k < FB_WORDS; k++)
			fbbuf[k] = FB_UNSET;
		if (!strcmp(op->name, "glSelectBuffer"))
			((void (*)(int32_t, uint32_t *))p)((int32_t)op->a[0], fbbuf);
		else
			((void (*)(int32_t, uint32_t, void *))p)((int32_t)op->a[0], (uint32_t)op->a[1], fbbuf);
		return;
	}
	if (!strcmp(op->name, "glRenderMode")) {
		int32_t r = ((int32_t (*)(uint32_t))p)((uint32_t)op->a[0]);
		fb_snapshot(1, r, (uint32_t)op->a[0], fbbuf);
		return;
	}
	const char *sig = functions[fn].args;
	const double *a = op->a;
	if (!strcmp(op->name, "glTexImage2D")) {
		int w = (int)a[3], h = (int)a[4];
		((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *))p)(
			(uint32_t)a[0], (int32_t)a[1], (int32_t)a[2], w, h, 0, 0x1908, 0x1401, image((unsigned)a[5], w, h));
		return;
	}
#define U(k) ((uint32_t)(int64_t)a[k])
#define F(k) ((float)a[k])
#define B(k) ((uint8_t)a[k])
#define I(k) ((int32_t)a[k])
	const char *pointer = strchr(sig, 'p');
	const void *hp = NULL;                  /* (the pointer argument: an array, or a parameter vector) */
	static uint8_t arrays[ARRAYS][0x1000];
	static union {
		float f[6];
		double d[6];
	} vec;
	if (pointer) {
		unsigned k = (unsigned)(pointer - sig);
		if (takes_array(op->name)) {
			unsigned id = (unsigned)a[k];
			array_bytes(id, false, arrays[id]);
			hp = arrays[id];
		} else {
			for (unsigned j = k; j < 6; j++) {
				vec.d[j - k] = a[j];
				if (!vector_of_doubles(op->name))
					vec.f[j - k] = (float)a[j];
			}
			hp = &vec;
		}
	}
	if (!strcmp(sig, "IIp"))
		((void (*)(uint32_t, uint32_t, const void *))p)(U(0), U(1), hp);
	else if (!strcmp(sig, "iIp"))
		((void (*)(int32_t, uint32_t, const void *))p)(I(0), U(1), hp);
	else if (!strcmp(sig, "dddd"))
		((void (*)(double, double, double, double))p)(a[0], a[1], a[2], a[3]);
	else if (!strcmp(sig, "IiiiiiiIIp"))
		((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t,
			   const void *))p)(U(0), I(1), I(2), I(3), I(4), I(5), I(6), U(7), U(8), hp);
	else if (!strcmp(sig, "IiiiiiiiIIp"))
		((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t,
			   const void *))p)(U(0), I(1), I(2), I(3), I(4), I(5), I(6), I(7), U(8), U(9), hp);
	else if (!strcmp(sig, "iiIIp"))
		((void (*)(int32_t, int32_t, uint32_t, uint32_t, const void *))p)(I(0), I(1), U(2), U(3), hp);
	else if (!strcmp(sig, "iiffffp"))
		((void (*)(int32_t, int32_t, float, float, float, float, const void *))p)(I(0), I(1), F(2), F(3), F(4),
											 F(5), hp);
	else if (!strcmp(sig, "iiiiI"))
		((void (*)(int32_t, int32_t, int32_t, int32_t, uint32_t))p)(I(0), I(1), I(2), I(3), U(4));
	else if (!strcmp(sig, "Ip"))
		((void (*)(uint32_t, const void *))p)(U(0), hp);
	else if (!strcmp(sig, "p"))
		((void (*)(const void *))p)(hp);
	else if (!strcmp(sig, "iS"))
		((void (*)(int32_t, uint16_t))p)(I(0), (uint16_t)a[1]);
	else if (!strcmp(sig, "iIip"))
		((void (*)(int32_t, uint32_t, int32_t, const void *))p)(I(0), U(1), I(2), hp);
	else if (!strcmp(sig, "Iip"))
		((void (*)(uint32_t, int32_t, const void *))p)(U(0), I(1), hp);
	else if (!strcmp(sig, "IiIp"))
		((void (*)(uint32_t, int32_t, uint32_t, const void *))p)(U(0), I(1), U(2), hp);
	else if (!strcmp(sig, "Iii"))
		((void (*)(uint32_t, int32_t, int32_t))p)(U(0), I(1), I(2));
	else if (!strcmp(sig, "ii"))
		((void (*)(int32_t, int32_t))p)(I(0), I(1));
	else if (!strcmp(sig, "i"))
		((void (*)(int32_t))p)(I(0));
	else if (!strcmp(sig, "IIf"))
		((void (*)(uint32_t, uint32_t, float))p)(U(0), U(1), F(2));
	else if (!strcmp(sig, "Ii"))
		((void (*)(uint32_t, int32_t))p)(U(0), I(1));
	else if (!strcmp(sig, "IiI"))
		((void (*)(uint32_t, int32_t, uint32_t))p)(U(0), I(1), U(2));
	else if (!strcmp(sig, "III"))
		((void (*)(uint32_t, uint32_t, uint32_t))p)(U(0), U(1), U(2));
	else if (!strcmp(sig, ""))
		((void (*)(void))p)();
	else if (!strcmp(sig, "I"))
		((void (*)(uint32_t))p)(U(0));
	else if (!strcmp(sig, "II"))
		((void (*)(uint32_t, uint32_t))p)(U(0), U(1));
	else if (!strcmp(sig, "If"))
		((void (*)(uint32_t, float))p)(U(0), F(1));
	else if (!strcmp(sig, "IIi"))
		((void (*)(uint32_t, uint32_t, int32_t))p)(U(0), U(1), (int32_t)a[2]);
	else if (!strcmp(sig, "Iff"))
		((void (*)(uint32_t, float, float))p)(U(0), F(1), F(2));
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
	else if (!strcmp(sig, "Iffiip"))
		((void (*)(uint32_t, float, float, int32_t, int32_t, const void *))p)(U(0), F(1), F(2), I(3), I(4), hp);
	else if (!strcmp(sig, "Iddiip"))
		((void (*)(uint32_t, double, double, int32_t, int32_t, const void *))p)(U(0), a[1], a[2], I(3), I(4),
											 hp);
	else if (!strcmp(sig, "Iffiiffiip"))
		((void (*)(uint32_t, float, float, int32_t, int32_t, float, float, int32_t, int32_t, const void *))p)(
			U(0), F(1), F(2), I(3), I(4), F(5), F(6), I(7), I(8), hp);
	else if (!strcmp(sig, "Iddiiddiip"))
		((void (*)(uint32_t, double, double, int32_t, int32_t, double, double, int32_t, int32_t,
			   const void *))p)(U(0), a[1], a[2], I(3), I(4), a[5], a[6], I(7), I(8), hp);
	else if (!strcmp(sig, "iff"))
		((void (*)(int32_t, float, float))p)(I(0), F(1), F(2));
	else if (!strcmp(sig, "iffiff"))
		((void (*)(int32_t, float, float, int32_t, float, float))p)(I(0), F(1), F(2), I(3), F(4), F(5));
	else if (!strcmp(sig, "iddidd"))
		((void (*)(int32_t, double, double, int32_t, double, double))p)(I(0), a[1], a[2], I(3), a[4], a[5]);
	else if (!strcmp(sig, "Iiiii"))
		((void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t))p)(U(0), I(1), I(2), I(3), I(4));
	else
		CHECK(!"a signature mesa_invoke doesn't take");
#undef U
#undef F
#undef B
#undef I
}

/* A scene drawn by Mesa (a fresh context's), as our_scene() reads ours */
static bool mesa_scene(const struct op *ops, uint32_t *out)
{
	const int32_t none[] = { EGL_NONE_ };
	void *ctx = egl.CreateContext(egl.display, egl.config, NULL, none);
	if (!ctx || !egl.MakeCurrent(egl.display, NULL, NULL, ctx))
		return false;
	enum { GL_FRAMEBUFFER = 0x8D40, GL_RENDERBUFFER = 0x8D41, GL_RGBA8 = 0x8058, GL_DEPTH24_STENCIL8 = 0x88F0,
	       GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_STENCIL_ATTACHMENT = 0x821A, GL_RGBA = 0x1908,
	       GL_RENDERER_T = 0x1F01 };
	if (!mesa_renderer[0]) {
		const char *r = GLFN(const char *(*)(uint32_t), "glGetString")(GL_RENDERER_T);
		snprintf(mesa_renderer, sizeof mesa_renderer, "%s", r ? r : "?");
	}
	uint32_t fbo, rb[2];
	GLFN(void (*)(int32_t, uint32_t *), "glGenFramebuffers")(1, &fbo);
	GLFN(void (*)(int32_t, uint32_t *), "glGenRenderbuffers")(2, rb);
	GLFN(void (*)(uint32_t, uint32_t), "glBindFramebuffer")(GL_FRAMEBUFFER, fbo);
	for (int k = 0; k < 2; k++) {
		GLFN(void (*)(uint32_t, uint32_t), "glBindRenderbuffer")(GL_RENDERBUFFER, rb[k]);
		GLFN(void (*)(uint32_t, uint32_t, int32_t, int32_t), "glRenderbufferStorage")(
			GL_RENDERBUFFER, k ? GL_DEPTH24_STENCIL8 : GL_RGBA8, SW, SH);
		GLFN(void (*)(uint32_t, uint32_t, uint32_t, uint32_t), "glFramebufferRenderbuffer")(
			GL_FRAMEBUFFER, k ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[k]);
	}
	/* (as ours start: the viewport and scissor box the drawable's) */
	GLFN(void (*)(int32_t, int32_t, int32_t, int32_t), "glViewport")(0, 0, SW, SH);
	GLFN(void (*)(int32_t, int32_t, int32_t, int32_t), "glScissor")(0, 0, SW, SH);
	void *(*new_quadric)(void) = NULL;
	void (*delete_quadric)(void *) = NULL;
	if (glu_lib) {
		*(void **)&new_quadric = dlsym(glu_lib, "gluNewQuadric");
		*(void **)&delete_quadric = dlsym(glu_lib, "gluDeleteQuadric");
		mesa_quadric = new_quadric();
	}
	for (const struct op *op = ops; op->name; op++)
		mesa_invoke(op);
	if (glu_lib)
		delete_quadric(mesa_quadric);
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

/* Whether a scene calls OpenGLUtility */
static bool uses_glu(const struct op *ops)
{
	for (const struct op *op = ops; op->name; op++)
		if (!strncmp(op->name, "glu", 3))
			return true;
	return false;
}

/* Ours against Mesa's */
static void test_mesa(void)
{
	if (!mesa_open()) {
		const char *need = getenv("BLUE_TEST_GPU");
		printf("opengl: no Mesa to compare with\n");
		CHECK(!(need && !strcmp(need, "1")));
		return;
	}
	static uint32_t ref[SW * SH];
	for (unsigned k = 0; k < SCENES; k++) {
		if (uses_glu(scenes[k].ops) && !glu_lib) {
			printf("opengl: %s: no GLU to compare with (BLUE_TEST_GLU: Mesa's libGLU)\n", scenes[k].name);
			continue;
		}
		if (scenes[k].ops == scene_accum)
			continue;          /* (Mesa's framebuffer objects have no accumulation buffer) */
		CHECK(mesa_scene(scenes[k].ops, ref));
		save("mesa", k, ref);
		for (unsigned w = 0; w < sizeof mesa_wrong / sizeof mesa_wrong[0]; w++) {
			if (strcmp(mesa_wrong[w].scene, scenes[k].name) || !strstr(mesa_renderer, mesa_wrong[w].renderer))
				continue;
			printf("opengl: %s: %s not compared (%s draws them wrong)\n", scenes[k].name, mesa_wrong[w].what,
			       mesa_wrong[w].renderer);
			for (int y = mesa_wrong[w].top; y < mesa_wrong[w].bottom; y++)
				for (int x = mesa_wrong[w].left; x < mesa_wrong[w].right; x++)
					ref[y * SW + x] = images[0][k][y * SW + x];
		}
		unsigned most, n = differing(images[0][k], ref, 2, &most);
		unsigned covered = 0;                   /* (pixels one draws on and the other doesn't) */
		for (unsigned i = 0; i < SW * SH; i++)
			covered += (images[0][k][i] == images[0][k][0]) != (ref[i] == ref[0]);
		if (verbose())
			printf("opengl: %-10s against Mesa: %u pixels differ by more than 2, the most by %u; "
			       "%u covered by one only\n", scenes[k].name, n, most, covered);
		CHECK(n <= scenes[k].allowed);
		CHECK(covered <= scenes[k].edges);
		if (scenes[k].ops == scene_clamp) {     /* (where s and t are within 0.125-0.875: the same) */
			unsigned inside = 0;
			for (int y = 22; y < 50; y++)
				for (int x = 30; x < 66; x++)
					inside += channel_diff(images[0][k][y * SW + x], ref[y * SW + x]) > 2;
			CHECK_EQ(inside, 0);
		}
	}
}
#endif

/* ---- aglSetFullScreen and aglUseFont: the Toolbox's calls answered by fakes ----------------------- */

#define TOOLBOX_TV 0x7100u      /* (import k's fake TVector: TOOLBOX_TV + 16 k) */
#define FS_DEVICE_H 0x50600u    /* a device for full screens, its pixmap; its screen at FS_PIXELS */
#define FS_DEVICE 0x50610u
#define FS_PIXMAP_H 0x50700u
#define FS_PIXMAP 0x50710u
#define FS_PIXELS 0x400000u     /* (the fake screen's framebuffer: 1280 x 1024 x 32 bits at most) */
#define FS_SIZE 0x500000u
#define FONT_PORT 0x51000u      /* the port aglUseFont's caller has; the offscreen worlds, their pixmap, pixels */
#define FONT_DEVICE 0x51100u
#define FONT_WORLD 0x51200u
#define FONT_PIXMAP_H 0x51300u
#define FONT_PIXMAP 0x51310u
#define FONT_PIXELS 0x52000u

static struct video vid;
static struct {
	unsigned calls[32];                     /* (each import's) */
	unsigned switches;                      /* DMSetDisplayMode's: the last's mode and depth mode */
	uint32_t mode, depth;
	bool fail_switch, fail_world;           /* (the next DMSetDisplayMode fails; NewGWorld does) */
	uint32_t port, device;                  /* SetGWorld's */
	unsigned worlds, disposed;
	int size, pen_h, pen_v, width, height, row_bytes;
} tb;

/* The fake screen's device in the fake display's mode (as the Display Manager leaves it) */
static void fs_device(void)
{
	unsigned w = video_width(&vid), h = video_height(&vid), bits = video_bpp(&vid);
	mem_w32(&mem, FS_DEVICE_H, FS_DEVICE);
	mem_w32(&mem, FS_DEVICE + 22, FS_PIXMAP_H);
	mem_w32(&mem, FS_PIXMAP_H, FS_PIXMAP);
	mem_w32(&mem, FS_PIXMAP, FS_PIXELS);
	mem_w16(&mem, FS_PIXMAP + 4, (uint16_t)(0x8000 | w * bits / 8));
	const uint16_t bounds[4] = { 0, 0, (uint16_t)h, (uint16_t)w };
	for (int k = 0; k < 4; k++)
		mem_w16(&mem, FS_PIXMAP + 6 + 2 * k, bounds[k]);
	mem_w16(&mem, FS_PIXMAP + 32, (uint16_t)bits);
}

/* The fake glyph of character c drawn with the pen at (h, v): a stem, the baseline's row, a dot by c */
static bool fake_glyph(int c, int h, int v, int x, int y)
{
	int ascent = tb.size * 3 / 4, descent = tb.size / 4, width = 4 + c % 3;
	return (x == h && y >= v - ascent && y < v + descent) || (y == v - 1 && x >= h && x < h + width) ||
	       (x == h + 1 + c % 3 && y == v - ascent);
}

/* Import k's fake: its result */
static uint32_t toolbox(unsigned k, const uint32_t *a)
{
	const char *name = opengl_library_import(k);
	tb.calls[k]++;
	if (!strcmp(name, "DMSetDisplayMode")) {
		CHECK_EQ(a[0], FS_DEVICE_H);
		if (tb.fail_switch) {
			tb.fail_switch = false;
			return 0xFFFFFFFFu;
		}
		tb.switches++;
		tb.mode = a[1];
		tb.depth = mem_r32(&mem, a[2]);
		vid.mode = tb.mode - 1;
		vid.depth = tb.depth - 0x80;
		fs_device();
	} else if (!strcmp(name, "RealFont")) {
		return a[0] == 3;
	} else if (!strcmp(name, "GetGWorld")) {
		mem_w32(&mem, a[0], FONT_PORT);
		mem_w32(&mem, a[1], FONT_DEVICE);
	} else if (!strcmp(name, "SetGWorld")) {
		tb.port = a[0];
		tb.device = a[1];
	} else if (!strcmp(name, "NewGWorld")) {
		CHECK_EQ(a[1], 1);
		if (tb.fail_world)
			return 0xFFFFFF94u;     /* (memFullErr) */
		tb.worlds++;
		tb.width = (int16_t)mem_r16(&mem, a[2] + 6) - (int16_t)mem_r16(&mem, a[2] + 2);
		tb.height = (int16_t)mem_r16(&mem, a[2] + 4) - (int16_t)mem_r16(&mem, a[2]);
		tb.row_bytes = (tb.width + 31) / 32 * 4;
		mem_w32(&mem, FONT_PIXMAP_H, FONT_PIXMAP);
		mem_w16(&mem, FONT_PIXMAP + 4, (uint16_t)(0x8000 | tb.row_bytes));
		mem_w32(&mem, a[0], FONT_WORLD);
	} else if (!strcmp(name, "DisposeGWorld")) {
		CHECK_EQ(a[0], FONT_WORLD);
		CHECK(tb.port != FONT_WORLD);
		tb.disposed++;
	} else if (!strcmp(name, "GetGWorldPixMap")) {
		return FONT_PIXMAP_H;
	} else if (!strcmp(name, "LockPixels")) {
		return 1;
	} else if (!strcmp(name, "GetPixBaseAddr")) {
		return FONT_PIXELS;
	} else if (!strcmp(name, "TextSize")) {
		tb.size = (int16_t)a[0];
	} else if (!strcmp(name, "GetFontInfo")) {
		mem_w16(&mem, a[0], (uint16_t)(tb.size * 3 / 4));      /* ascent, descent, widest, leading */
		mem_w16(&mem, a[0] + 2, (uint16_t)(tb.size / 4));
		mem_w16(&mem, a[0] + 4, (uint16_t)(tb.size * 3 / 5));
		mem_w16(&mem, a[0] + 6, 1);
	} else if (!strcmp(name, "CharWidth")) {
		return 4 + a[0] % 3;
	} else if (!strcmp(name, "EraseRect")) {
		CHECK_EQ(tb.port, FONT_WORLD);
		memset(mem_ptr(&mem, FONT_PIXELS), 0, (size_t)(tb.row_bytes * tb.height));
	} else if (!strcmp(name, "MoveTo")) {
		tb.pen_h = (int16_t)a[0];
		tb.pen_v = (int16_t)a[1];
	} else if (!strcmp(name, "DrawChar")) {
		for (int y = 0; y < tb.height; y++)
			for (int x = 0; x < tb.width; x++)
				if (fake_glyph((int)a[0], tb.pen_h, tb.pen_v, x, y)) {
					uint32_t at = FONT_PIXELS + (uint32_t)(y * tb.row_bytes + x / 8);
					mem_w8(&mem, at, (uint8_t)(mem_r8(&mem, at) | 0x80 >> (x % 8)));
				}
	}
	return 0;
}

/* The library's imports resolved to the fakes */
static void toolbox_load(void)
{
	for (unsigned k = 0; opengl_library_import(k); k++)
		mem_w32(&mem, TOC + gl.library.import[k], TOOLBOX_TV + 16 * k);
}

/* A call whose callouts the fakes answer */
static uint32_t call_toolbox(unsigned entry, const uint32_t *w, unsigned nw)
{
	struct ndrv_call c;
	memset(&c, 0, sizeof c);
	c.mem = &mem;
	c.entry = entry;
	c.toc = TOC;
	c.frame = FRAME;
	for (unsigned j = 0; j < nw && j < 8; j++)
		c.arg[j] = w[j];
	opengl_library_call(&gl, &c);
	for (unsigned n = 0; !c.done && n < 10000; n++) {
		unsigned k = (c.tvector - TOOLBOX_TV) / 16;
		CHECK(c.tvector >= TOOLBOX_TV && k < 32 && opengl_library_import(k));
		if (!(c.tvector >= TOOLBOX_TV && k < 32 && opengl_library_import(k)))
			break;
		uint32_t r = toolbox(k, c.out);
		c.entry = NDRV_ENTRY_RESUME;
		c.arg[0] = r;
		opengl_library_call(&gl, &c);
	}
	CHECK(c.done);
	return c.result;
}

#define TOOLBOX(name, ...) call_toolbox((unsigned)opengl_entry(#name), (const uint32_t[]){ __VA_ARGS__ }, \
					sizeof (uint32_t[]){ __VA_ARGS__ } / 4)
enum { AGL_BAD_STATE = 10007, AGL_BAD_VALUE = 10008, AGL_BAD_FULLSCREEN = 10012,
       AGL_BAD_ALLOC = 10016 };

/*
 * aglSetFullScreen: the screen as it is; a size that's no mode's; a switch
 * that fails; one to a size (and the pixel format's depth), the frame then
 * on the whole screen; the mode back when the drawable goes, and when the
 * process does
 */
static void test_full_screen(void)
{
	mem_map(&mem, FS_PIXELS, FS_SIZE, MEM_RW);
	call_entry(OPENGL_ENTRY_INIT, NULL, 0, NULL, 0);
	toolbox_load();
	make_port(32);
	video_init(&vid, &mem, FS_PIXELS, FS_SIZE, 1000000);
	vid.depth = 5;                                          /* (32 bits a pixel) */
	gl.video = &vid;
	fs_device();
	memset(&tb, 0, sizeof tb);
	uint32_t was_mode, was_depth, w = video_width(&vid), h = video_height(&vid);
	video_mode_now(&vid, &was_mode, &was_depth);
	const uint32_t window[] = { AGL_RGBA, AGL_DOUBLEBUFFER, AGL_NONE };
	put_attribs(window, 3);
	uint32_t windowed = CALL(aglCreateContext, CALL(aglChoosePixelFormat, 0, 0, ATTRIBS), 0);
	CHECK_EQ(TOOLBOX(aglSetFullScreen, windowed, 0, 0, 0, 0), 0);
	CHECK_EQ(CALL0(aglGetError), AGL_BAD_FULLSCREEN);
	mem_w32(&mem, OUT + 16, FS_DEVICE_H);
	const uint32_t full[] = { AGL_RGBA, AGL_DOUBLEBUFFER, AGL_FULLSCREEN, AGL_NONE };
	put_attribs(full, 4);
	uint32_t pf = CALL(aglChoosePixelFormat, OUT + 16, 1, ATTRIBS), ctx = CALL(aglCreateContext, pf, 0);
	CHECK(pf && ctx);
	CHECK_EQ(CALL(aglSetCurrentContext, ctx), 1);
	CHECK_EQ(TOOLBOX(aglSetFullScreen, ctx, 0, 0, 0, 0), 1);
	CHECK_EQ(tb.switches, 0);
	CALL(glGetIntegerv, GL_VIEWPORT, OUT);
	CHECK(mem_r32(&mem, OUT + 8) == w && mem_r32(&mem, OUT + 12) == h);
	CHECK_EQ(TOOLBOX(aglSetFullScreen, ctx, 700, 500, 0, 0), 0);
	CHECK_EQ(CALL0(aglGetError), AGL_BAD_FULLSCREEN);
	tb.fail_switch = true;
	CHECK_EQ(TOOLBOX(aglSetFullScreen, ctx, 640, 480, 0, 0), 0);
	CHECK_EQ(CALL0(aglGetError), AGL_BAD_FULLSCREEN);
	CHECK_EQ(TOOLBOX(aglSetFullScreen, ctx, 640, 480, 75, 0), 1);
	CHECK(tb.switches == 1 && tb.mode == 1 && tb.depth == 0x85);
	CHECK(video_width(&vid) == 640 && video_height(&vid) == 480);
	CALL(glGetIntegerv, GL_VIEWPORT, OUT);
	CHECK(mem_r32(&mem, OUT + 8) == 640 && mem_r32(&mem, OUT + 12) == 480);
	CALL_F(glClearColor, 1.0, 0.5, 0.0, 1.0);
	CALL(glClear, 0x4000);
	CALL(aglSwapBuffers, ctx);
	CHECK_EQ(mem_r32(&mem, FS_PIXELS) & 0xFFFFFF, 0xFF8000);
	CHECK_EQ(mem_r32(&mem, FS_PIXELS + 4 * (640 * 480 - 1)) & 0xFFFFFF, 0xFF8000);
	CHECK_EQ(CALL(aglGetDrawable, ctx), 0);
	CHECK_EQ(TOOLBOX(aglSetDrawable, ctx, 0), 1);           /* (the mode back) */
	CHECK(tb.switches == 2 && tb.mode == was_mode && tb.depth == was_depth);
	CHECK_EQ(TOOLBOX(aglSetDrawable, ctx, 0), 1);           /* (nothing to go back to) */
	CHECK_EQ(tb.switches, 2);
	CHECK_EQ(TOOLBOX(aglSetFullScreen, ctx, 1024, 768, 0, 0), 1);
	CHECK(tb.switches == 3 && video_width(&vid) == 1024);
	CHECK_EQ(TOOLBOX(aglSetFullScreen, ctx, 640, 480, 0, 0), 1);   /* (from one full screen to another) */
	call_toolbox(OPENGL_ENTRY_TERM, NULL, 0);               /* (the process goes: the first mode back) */
	CHECK(tb.switches == 5 && tb.mode == was_mode && tb.depth == was_depth);
	gl.video = NULL;
}

/*
 * aglUseFont: no such font, a list being made, no memory; a plain font's
 * glyphs and a styled one's (each taller and wider by half its size), as
 * QuickDraw (here, the fakes) draws them, drawn by glCallLists from a
 * raster position
 */
static void test_use_font(void)
{
	call_entry(OPENGL_ENTRY_INIT, NULL, 0, NULL, 0);
	toolbox_load();
	make_port(32);
	memset(&tb, 0, sizeof tb);
	enum { W = 64, H = 32 };
	const uint32_t attribs[] = { AGL_RGBA, AGL_NONE };
	put_attribs(attribs, 2);
	uint32_t ctx = CALL(aglCreateContext, CALL(aglChoosePixelFormat, 0, 0, ATTRIBS), 0);
	CHECK_EQ(CALL(aglSetOffScreen, ctx, W, H, W * 4, OFFSCREEN), 1);
	CHECK_EQ(CALL(aglSetCurrentContext, ctx), 1);
	CHECK_EQ(TOOLBOX(aglUseFont, ctx, 99, 0, 12, 'A', 3, 100), 0);
	CHECK_EQ(CALL0(aglGetError), AGL_BAD_VALUE);
	CALL(glNewList, 7, 0x1300);
	CHECK_EQ(TOOLBOX(aglUseFont, ctx, 3, 0, 12, 'A', 3, 100), 0);
	CHECK_EQ(CALL0(aglGetError), AGL_BAD_STATE);
	CALL0(glEndList);
	tb.fail_world = true;
	CHECK_EQ(TOOLBOX(aglUseFont, ctx, 3, 0, 12, 'A', 3, 100), 0);
	CHECK_EQ(CALL0(aglGetError), AGL_BAD_ALLOC);
	CHECK_EQ(tb.port, 0);                                   /* (no world: the port as it was) */
	tb.fail_world = false;
	CHECK_EQ(TOOLBOX(aglUseFont, ctx, 3, 0, 12, 'A', 3, 100), 1);
	CHECK(tb.worlds == 2 && tb.disposed == 2 && tb.port == FONT_PORT && tb.device == FONT_DEVICE);
	CHECK(tb.width == 7 && tb.height == 12);                /* (the widest by ascent + descent) */
	CHECK_EQ(TOOLBOX(aglUseFont, ctx, 3, 1, 12, 'a', 2, 200), 1);
	CHECK(tb.width == 7 + 12 && tb.height == 12 + 12);
	CHECK(CALL(glIsList, 100) && CALL(glIsList, 102) && CALL(glIsList, 201) && !CALL(glIsList, 103));
	CHECK_EQ(CALL0(glGetError), 0);

	/* drawn: the plain ones from (4.5, 20.5), the styled ones from (30.5, 9.5) */
	CALL(glMatrixMode, 0x1701);
	CALL0(glLoadIdentity);
	CALL_F(glOrtho, 0.0, W, 0.0, H, -1.0, 1.0);
	CALL(glMatrixMode, 0x1700);
	CALL0(glLoadIdentity);
	CALL_F(glClearColor, 0.0, 0.0, 0.0, 1.0);
	CALL(glClear, 0x4000);
	CALL_F(glColor3f, 1.0, 1.0, 1.0);
	static const uint8_t names[3] = { 0, 1, 2 };
	memcpy(mem_ptr(&mem, ARRAY), names, 3);
	CALL_F(glRasterPos2f, 4.5, 20.5);
	CALL(glListBase, 100);
	CALL(glCallLists, 3, 0x1401, ARRAY);
	CALL_F(glRasterPos2f, 30.5, 9.5);
	CALL(glListBase, 200);
	CALL(glCallLists, 2, 0x1401, ARRAY);
	CALL(aglSwapBuffers, ctx);
	static bool want[H][W];
	memset(want, 0, sizeof want);
	for (int s = 0; s < 2; s++) {
		int e = s ? 6 : 0, ascent = 9, descent = 3, height = ascent + descent + 2 * e, x = s ? 30 : 4;
		int y = s ? 9 : 20;
		for (int g = 0; g < (s ? 2 : 3); g++) {
			int c = (s ? 'a' : 'A') + g, cw = 4 + c % 3;
			int x0 = x - e, y0 = y - (descent + e - 1);     /* (the bitmap's bottom left: the origin's) */
			for (int j = 0; j < height; j++)
				for (int i = 0; i < cw + 2 * e; i++)
					if (fake_glyph(c, e, ascent + e, i, height - 1 - j) && y0 + j >= 0 && y0 + j < H &&
					    x0 + i >= 0 && x0 + i < W)
						want[y0 + j][x0 + i] = true;
			x += cw;
		}
	}
	unsigned wrong = 0, lit = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			bool on = (mem_r32(&mem, OFFSCREEN + 4 * (uint32_t)((H - 1 - y) * W + x)) & 0xFFFFFF) == 0xFFFFFF;
			wrong += on != want[y][x];
			lit += on;
		}
	CHECK_EQ(wrong, 0);
	CHECK(lit > 60);
	CHECK_EQ(TOOLBOX(aglUseFont, 0x1234, 3, 0, 12, 'A', 1, 300), 0);
	CHECK_EQ(CALL0(aglGetError), AGL_BAD_CONTEXT);
	CALL(aglDestroyContext, ctx);

	/* a library an older version made (its data's version another one's): left alone */
	uint32_t version = TOC + gl.library.extra + 4;
	mem_w32(&mem, version, mem_r32(&mem, version) - 1);
	call_entry(OPENGL_ENTRY_INIT, NULL, 0, NULL, 0);
	CHECK_EQ(CALL(aglChoosePixelFormat, 0, 0, ATTRIBS), 0);
	CHECK_EQ(CALL0(aglGetError), 0);
	call_entry(OPENGL_ENTRY_TERM, NULL, 0, NULL, 0);
	mem_w32(&mem, version, mem_r32(&mem, version) + 1);
}

/* ---- selection and feedback, against Mesa's ---------------------------------------------- */

enum { GL_2D_T = 0x0600, GL_3D_T, GL_3D_COLOR_T, GL_3D_COLOR_TEXTURE_T, GL_4D_COLOR_TEXTURE_T };
#define RENDER(mode) { "glRenderMode", { mode } }

/*
 * Feedback in window coordinates: triangles, a quad, a polygon, a triangle
 * clipped, strips and a fan, lines of each kind, points (one outside), a
 * pass-through token, a polygon outlined, a back face culled, the raster
 * position's tokens. (Mesa's feedback departs from OpenGL 1.2's in two
 * ways test_glfront.c checks instead: it gives lines' segments
 * GL_LINE_TOKEN where the stipple was reset, and with flat shading each
 * vertex its own color.)
 */
static const struct op fb_window[] = {
	ORTHO, { "glFeedbackBuffer", { 1024, GL_3D_COLOR_TEXTURE_T } }, RENDER(GL_FEEDBACK),
	BEGIN(GL_TRIANGLES), C3(1, 0, 0), TC(0.25, 0.5), V3(10.5, 10.25, 0.5), C3(0, 1, 0), TC(0.75, 0.5),
	V3(30.5, 12.25, -0.25), C3(0, 0, 1), TC(0.5, 1), V3(20.5, 30.25, 0), END,
	BEGIN(GL_QUADS), C3(1, 1, 0), V2(40.5, 10.5), V2(60.5, 10.5), V2(60.5, 30.5), V2(40.5, 30.5), END,
	BEGIN(GL_POLYGON), C3(0, 1, 1), V2(70.5, 10.5), V2(90.5, 12.5), V2(92.5, 25.5), V2(80.5, 34.5), V2(68.5, 25.5),
	END,
	BEGIN(GL_TRIANGLES), C3(1, 0, 1), V2(-10.5, 40.5), V2(20.5, 42.5), V2(5.5, 60.5), END,
	BEGIN(GL_TRIANGLE_STRIP), C3(0.5, 0.5, 1), V2(2.5, 2.5), V2(4.5, 8.5), V2(8.5, 2.5), V2(9.5, 7.5), END,
	BEGIN(GL_TRIANGLE_FAN), V2(32.5, 2.5), V2(38.5, 3.5), V2(37.5, 8.5), V2(31.5, 8.5), END,
	BEGIN(GL_QUAD_STRIP), V2(62.5, 2.5), V2(62.5, 8.5), V2(66.5, 2.5), V2(66.5, 8.5), V2(70.5, 3.5),
	V2(70.5, 7.5), END,
	BEGIN(GL_LINE_STRIP), V2(30.5, 40.5), V2(40.5, 50.5), V2(50.5, 40.5), END,
	BEGIN(GL_LINES), V2(55.5, 40.5), V2(65.5, 50.5), V2(56.5, 45.5), V2(66.5, 55.5), END,
	BEGIN(GL_LINE_LOOP), V2(70.5, 40.5), V2(80.5, 41.5), V2(75.5, 50.5), END,
	BEGIN(GL_POINTS), V2(5.5, 65.5), V2(-3.5, 65.5), V2(15.5, 66.5), END,
	{ "glPassThrough", { 7.5 } },
	{ "glPolygonMode", { GL_FRONT_AND_BACK, GL_LINE } },
	BEGIN(GL_TRIANGLES), V2(30.5, 60.5), V2(40.5, 60.5), V2(35.5, 68.5), END,
	{ "glPolygonMode", { GL_FRONT_AND_BACK, GL_FILL } },
	EN(GL_CULL_FACE), BEGIN(GL_TRIANGLES), V2(45.5, 60.5), V2(50.5, 68.5), V2(55.5, 60.5), V2(60.5, 60.5),
	V2(70.5, 60.5), V2(65.5, 68.5), END, DIS(GL_CULL_FACE),
	C3(0.5, 0.25, 1), RPOS(40.5, 35.5), { "glBitmap", { 16, 8, 0, 0, 5, 1, ARR_BITMAP } },
	DRAWPIX(8, 6, GL_RGB_T, GL_UNSIGNED_BYTE, ARR_PIX_RGB), { "glCopyPixels", { 0, 0, 4, 4, GL_COLOR_T } },
	RENDER(GL_RENDER),
	{ NULL, { 0 } },
};

/* Feedback in perspective, with w: depths, a triangle through the near plane, offset, lines */
static const struct op fb_perspective[] = {
	FRUSTUM, { "glFeedbackBuffer", { 1024, GL_4D_COLOR_TEXTURE_T } }, RENDER(GL_FEEDBACK),
	BEGIN(GL_TRIANGLES), C3(1, 0.5, 0), TC(0.1, 0.2), V3(-0.5, -0.4, -2), TC(0.9, 0.3), V3(0.6, -0.3, -3),
	TC(0.4, 0.8), V3(0, 0.5, -5), END,
	BEGIN(GL_TRIANGLES), V3(-0.3, -0.2, -0.5), V3(0.4, -0.1, -3), V3(0.1, 0.4, -2.5), END,
	{ "glPolygonOffset", { 1, 2 } }, EN(GL_POLYGON_OFFSET_FILL),
	BEGIN(GL_TRIANGLES), V3(-0.2, -0.2, -2), V3(0.3, -0.25, -4), V3(0.1, 0.3, -3), END, DIS(GL_POLYGON_OFFSET_FILL),
	BEGIN(GL_LINES), C3(0.2, 0.9, 0.4), V3(-0.4, 0.2, -1.5), V3(0.5, 0.3, -6), END,
	RENDER(GL_RENDER),
	{ NULL, { 0 } },
};

/* Feedback past the buffer's end: -1; then the same mode again, from the start */
static const struct op fb_overflow[] = {
	ORTHO, { "glFeedbackBuffer", { 10, GL_2D_T } }, RENDER(GL_FEEDBACK),
	BEGIN(GL_TRIANGLES), V2(10.5, 10.5), V2(20.5, 10.5), V2(15.5, 20.5), V2(30.5, 10.5), V2(40.5, 10.5),
	V2(35.5, 20.5), END,
	RENDER(GL_FEEDBACK), BEGIN(GL_POINTS), V2(5.5, 5.5), END, RENDER(GL_RENDER),
	{ NULL, { 0 } },
};

/*
 * Selection: the name stack loaded, pushed and popped between primitives in
 * perspective (one outside the view: no hit), a raster position's; then a
 * buffer too small for the hits
 */
static const struct op fb_select[] = {
	FRUSTUM, { "glSelectBuffer", { 256 } }, RENDER(GL_SELECT), { "glInitNames", { 0 } },
	{ "glPushName", { 1 } }, BEGIN(GL_TRIANGLES), V3(-0.5, -0.4, -2), V3(0.6, -0.3, -3), V3(0, 0.5, -5), END,
	{ "glLoadName", { 2 } }, BEGIN(GL_TRIANGLES), V3(10, 10, -2), V3(11, 10, -2), V3(10, 11, -2), END,
	{ "glPushName", { 3 } }, BEGIN(GL_LINES), V3(-0.4, 0.2, -1.5), V3(0.5, 0.3, -6), END,
	BEGIN(GL_POINTS), V3(0, 0, -8), END, { "glPopName", { 0 } }, { "glLoadName", { 4 } },
	{ "glRasterPos3f", { 0.1, 0.1, -4 } }, { "glBitmap", { 16, 8, 0, 0, 5, 1, ARR_BITMAP } },
	RENDER(GL_RENDER),
	{ "glSelectBuffer", { 4 } }, RENDER(GL_SELECT), { "glInitNames", { 0 } }, { "glPushName", { 5 } },
	{ "glPushName", { 6 } }, BEGIN(GL_TRIANGLES), V3(-0.5, -0.4, -2), V3(0.6, -0.3, -3), V3(0, 0.5, -5), END,
	{ "glPopName", { 0 } }, RENDER(GL_RENDER),
	{ NULL, { 0 } },
};

/*
 * Evaluators' values fed back (filled meshes aside: see scene_eval): a
 * curve's points and lines, colored; a rational curve's points (a domain
 * run backwards); a bent patch's points and lines, colored and textured
 * (the texture coordinates' domain another one). (The current color set
 * first, all four components: once a color map has been evaluated, Mesa
 * 25.2 loses the current color's components the last glColor didn't set,
 * all of them if it was never called: vertices after the map's turned off
 * come out (0, 0, 0, 0), or with alpha 0 after glColor3.)
 */
static const struct op fb_eval[] = {
	ORTHO, { "glFeedbackBuffer", { 1024, GL_4D_COLOR_TEXTURE_T } }, RENDER(GL_FEEDBACK), C4(0.8, 0.7, 0.6, 1),
	MAP1(GL_MAP1_VERTEX_3_T, 0, 1, 4, 4, ARR_EVAL_CURVE), MAP1(GL_MAP1_COLOR_4_T, 0, 1, 4, 3, ARR_EVAL_COLORS),
	EN(GL_MAP1_VERTEX_3_T), EN(GL_MAP1_COLOR_4_T), { "glMapGrid1f", { 5, 0, 1 } },
	{ "glEvalMesh1", { GL_POINT_T, 0, 5 } }, { "glEvalMesh1", { GL_LINE_T, 1, 4 } }, DIS(GL_MAP1_COLOR_4_T),
	{ "glMap1d", { GL_MAP1_VERTEX_4_T, 2, -1, 4, 3, ARR_EVAL_ARC } }, EN(GL_MAP1_VERTEX_4_T),
	BEGIN(GL_POINTS), { "glEvalCoord1d", { 2 } }, { "glEvalCoord1d", { 1.5 } }, { "glEvalCoord1f", { -0.25 } }, END,
	DIS(GL_MAP1_VERTEX_4_T), DIS(GL_MAP1_VERTEX_3_T),
	MAP2(GL_MAP2_VERTEX_3_T, 0, 1, 3, 4, 0, 1, 12, 4, ARR_EVAL_BENT),
	MAP2(GL_MAP2_COLOR_4_T, 0, 1, 4, 2, 0, 1, 8, 2, ARR_EVAL_PATCH_COLORS),
	MAP2(GL_MAP2_TEXTURE_COORD_2_T, 0.5, 1.5, 2, 2, 0, 1, 4, 2, ARR_EVAL_PATCH_TEX),
	EN(GL_MAP2_VERTEX_3_T), EN(GL_MAP2_COLOR_4_T), EN(GL_MAP2_TEXTURE_COORD_2_T),
	{ "glMapGrid2f", { 3, 0, 1, 2, 0, 1 } }, { "glEvalMesh2", { GL_POINT_T, 0, 3, 0, 2 } },
	{ "glEvalMesh2", { GL_LINE_T, 0, 2, 0, 1 } },
	RENDER(GL_RENDER),
	{ NULL, { 0 } },
};

/* ... lit: the colors of the bumpy patch's points, the cylinder's (rational), the flat one's (normals mapped) */
static const struct op fb_eval_lit[] = {
	ORTHO_UNITS, { "glFeedbackBuffer", { 1024, GL_3D_COLOR_T } }, RENDER(GL_FEEDBACK),
	EN(GL_LIGHTING), EN(GL_LIGHT0), LIGHTV(GL_LIGHT0, GL_POSITION, 0.3, 0.5, 1, 0),
	LIGHTV(GL_LIGHT0, GL_DIFFUSE, 0.8, 0.8, 0.7, 1), LIGHTV(GL_LIGHT0, GL_SPECULAR, 0.5, 0.5, 0.5, 1),
	MATV(GL_FRONT, GL_DIFFUSE, 0.6, 0.6, 0.5, 1), MATV(GL_FRONT, GL_SPECULAR, 0.6, 0.6, 0.6, 1),
	{ "glMaterialf", { GL_FRONT, GL_SHININESS, 20 } },
	MAP2(GL_MAP2_VERTEX_3_T, 0, 1, 3, 4, 0, 1, 12, 4, ARR_EVAL_BUMPS), EN(GL_MAP2_VERTEX_3_T), EN(GL_AUTO_NORMAL_T),
	{ "glMapGrid2f", { 5, 0, 1, 5, 0, 1 } }, { "glEvalMesh2", { GL_POINT_T, 0, 5, 0, 5 } },
	{ "glMap2d", { GL_MAP2_VERTEX_4_T, 0, 1, 4, 3, 0, 1, 12, 2, ARR_EVAL_CYLINDER } }, EN(GL_MAP2_VERTEX_4_T),
	{ "glMapGrid2d", { 4, 0, 1, 2, 0, 1 } }, { "glEvalMesh2", { GL_POINT_T, 0, 4, 0, 2 } },
	DIS(GL_MAP2_VERTEX_4_T), DIS(GL_AUTO_NORMAL_T), EN(GL_NORMALIZE),
	MAP2(GL_MAP2_VERTEX_3_T, 0, 1, 3, 2, 0, 1, 6, 2, ARR_EVAL_FLAT),
	MAP2(GL_MAP2_NORMAL_T, 0, 1, 3, 2, 0, 1, 6, 2, ARR_EVAL_NORMALS), EN(GL_MAP2_NORMAL_T),
	{ "glMapGrid2f", { 2, 0, 1, 2, 0, 1 } }, { "glEvalMesh2", { GL_POINT_T, 0, 2, 0, 2 } },
	RENDER(GL_RENDER),
	{ NULL, { 0 } },
};

static const struct {
	const char *name;
	const struct op *ops;
} fb_scenes[] = {
	{ "window", fb_window }, { "perspective", fb_perspective }, { "overflow", fb_overflow },
	{ "select", fb_select }, { "eval", fb_eval }, { "eval_lit", fb_eval_lit },
};

#ifdef __linux__
/* Whether a word of ours is Mesa's: as floats (feedback), or as a selection's record's part (k: which) */
static bool fb_word_same(uint32_t mode, uint32_t a, uint32_t b, unsigned part)
{
	if (a == FB_UNSET || b == FB_UNSET)
		return a == b;
	if (mode == GL_FEEDBACK) {
		float x, y;
		memcpy(&x, &a, 4);
		memcpy(&y, &b, 4);
		bool lx = x == GL_LINE_TOKEN || x == GL_LINE_RESET_TOKEN, ly = y == GL_LINE_TOKEN || y == GL_LINE_RESET_TOKEN;
		if (lx && ly)
			return true;            /* (see fb_window) */
		return fabsf(x - y) <= 1e-3f + 1e-5f * fabsf(y);
	}
	if (part == 1 || part == 2)             /* (depths: within a millionth) */
		return (a > b ? a - b : b - a) <= 4295u;
	return a == b;
}

/* Ours (fb[0]) against Mesa's (fb[1]): the same results, the same records */
static void fb_compare(const char *name)
{
	CHECK_EQ(fb[0].n, fb[1].n);
	for (unsigned k = 0; k < fb[0].n && k < fb[1].n; k++) {
		const struct fb_snap *a = &fb[0].snap[k], *b = &fb[1].snap[k];
		CHECK_EQ(a->mode, b->mode);
		CHECK_EQ(a->result, b->result);
		unsigned wrong = 0, first = FB_WORDS;
		for (unsigned i = 0, part = 0, names = 0; i < FB_WORDS; i++) {
			if (!fb_word_same(a->mode, a->words[i], b->words[i], part)) {
				wrong++;
				first = first < i ? first : i;
			}
			if (a->mode == GL_SELECT) {     /* (records: names, least and greatest depth, the names) */
				if (part == 0)
					names = b->words[i];
				part = part < 2 + names ? part + 1 : 0;
			}
		}
		if (wrong) {
			printf("opengl: feedback %s, mode %u: %u words differ, the first at %u:", name, k, wrong,
			       first);
			for (unsigned i = first; i < first + 8 && i < FB_WORDS; i++)
				printf(" %08X/%08X", a->words[i], b->words[i]);
			printf("\n");
			if (getenv("BLUE_TEST_FB_DUMP"))
				for (unsigned i = 0; i < FB_WORDS; i++) {
					if (a->words[i] == FB_UNSET && b->words[i] == FB_UNSET)
						break;
					float x, y;
					memcpy(&x, &a->words[i], 4);
					memcpy(&y, &b->words[i], 4);
					printf("  %4u %12g %12g%s\n", i, (double)x, (double)y,
					       fb_word_same(a->mode, a->words[i], b->words[i], 0) ? "" : "  <");
				}
		}
		CHECK_EQ(wrong, 0);
	}
}
#endif

#define FB_SCENES (sizeof fb_scenes / sizeof fb_scenes[0])
static struct fb_saved {
	struct fb_snap snap[FB_SNAPS];
	unsigned n;
} fb_ours[FB_SCENES];

/* Ours: each scene's records (compared with Mesa's in test_mesa_feedback()) */
static void test_feedback(void)
{
	static uint32_t img[SW * SH];
	for (unsigned k = 0; k < FB_SCENES; k++) {
		memset(fb, 0, sizeof fb);
		our_scene(fb_scenes[k].ops, img);
		CHECK(fb[0].n >= 1);
		memcpy(fb_ours[k].snap, fb[0].snap, sizeof fb_ours[k].snap);
		fb_ours[k].n = fb[0].n;
	}
}

#ifdef __linux__
static void test_mesa_feedback(void)
{
	static uint32_t img[SW * SH];
	if (!mesa_open())
		return;                 /* (test_mesa() has said so) */
	for (unsigned k = 0; k < FB_SCENES; k++) {
		memset(fb, 0, sizeof fb);
		memcpy(fb[0].snap, fb_ours[k].snap, sizeof fb_ours[k].snap);
		fb[0].n = fb_ours[k].n;
		CHECK(mesa_scene(fb_scenes[k].ops, img));
		fb_compare(fb_scenes[k].name);
		if (verbose())
			printf("opengl: feedback %-11s against Mesa: %u glRenderMode results, their records compared\n",
			       fb_scenes[k].name, fb[1].n);
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
	CHECK_EQ(gl.gpu_frames, gpu ? 6 : 0);
	test_recording(false);
	test_recording(true);
	test_lists();
	test_glu();
	test_full_screen();
	test_use_font();
	if (!gpu)
		test_feedback();
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
	test_mesa_feedback();
#endif
}
