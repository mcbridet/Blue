/*
 * The 3D engine driven as QuickDraw 3D would drive it: gestalt, a draw
 * context on a memory device, a texture (which way up its rows go), and
 * enough triangles in a frame to be drawn in several background batches;
 * in software, and again with the GPU if there's OpenGL.
 */
#include <string.h>

#include "../src/be.h"
#include "../src/mem.h"
#include "../src/rave.h"
#include "test.h"

/* TQAEngineMethodTag, TQADrawMethodTag, TQAGestaltSelector, TQAImagePixelType, TQATagPtr */
enum { EM_DRAW_PRIVATE_NEW, EM_DRAW_PRIVATE_DELETE, EM_CHECK_DEVICE, EM_GESTALT, EM_TEXTURE_NEW,
       EM_TEXTURE_DELETE = 6 };
enum { DM_SET_INT = 1, DM_SET_PTR = 2, DM_DRAW_TRI_GOURAUD = 8, DM_DRAW_TRI_TEXTURE = 9, DM_RENDER_START = 13,
       DM_RENDER_END = 14 };
enum { GESTALT_VENDOR = 2, GESTALT_ENGINE = 3, GESTALT_NAME_LENGTH = 5 };
enum { PIXEL_ARGB32 = 4 };
enum { TAG_TEXTURE_OP = 12, TAG_TEXTURE = 13 };

#define TOC 0x1000u
#define FRAME 0x2000u
#define CTX 0x3000u             /* TQADrawContext (its drawPrivate first) */
#define DEVICE 0x3100u          /* TQADevice */
#define RECT 0x3200u
#define IMAGE 0x3300u           /* TQAImage */
#define PIXELS 0x3400u
#define OUT 0x3500u
#define LM_IMAGE 0x3600u        /* a second texture */
#define LM_PIXELS 0x3700u
#define VERTS 0x4000u
#define FB 0x10000u             /* the memory device: 16 x 16, ARGB32 */
#define SIZE 16

static struct mem mem;

static uint32_t call(struct rave *r, unsigned entry, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
	struct ndrv_call c;
	memset(&c, 0, sizeof c);
	c.mem = &mem;
	c.entry = entry;
	c.arg[0] = a0;
	c.arg[1] = a1;
	c.arg[2] = a2;
	c.arg[3] = a3;
	c.arg[4] = a4;
	c.toc = TOC;
	c.frame = FRAME;
	rave_call(r, &c);
	return c.result;
}

static uint32_t gestalt(struct rave *r, uint32_t selector)
{
	CHECK_EQ(call(r, RAVE_ENTRY_ENGINE + EM_GESTALT, selector, OUT, 0, 0, 0), 0);
	return mem_r32(&mem, OUT);
}

static void put_float(uint32_t addr, float f)
{
	uint32_t b;
	memcpy(&b, &f, 4);
	mem_w32(&mem, addr, b);
}

/* A TQAVTexture (or the TQAVGouraud at its start) */
static void vertex(uint32_t addr, float x, float y, float z, float rgb, float u, float v)
{
	static const float rest[] = { 1, 1, 1, 0, 0, 0 };       /* kd, ks */
	float f[10] = { x, y, z, 1, rgb, rgb, rgb, 1, u, v };
	for (int k = 0; k < 10; k++)
		put_float(addr + 4 * k, f[k]);
	for (int k = 0; k < 6; k++)
		put_float(addr + 40 + 4 * k, rest[k]);
}

static uint32_t pixel(int x, int y)
{
	return mem_r32(&mem, FB + (uint32_t)(y * SIZE + x) * 4);
}

static void run(struct gpu *gpu);

void test_rave(void)
{
	run(NULL);
	/* the same through the GPU backend, if there's OpenGL (any: llvmpipe will do) */
	struct gpu *g = gpu_open(true);
	if (g && gpu_ready(g, NULL))
		run(g);
	gpu_close(g);
}

static void run(struct gpu *gpu)
{
	if (mem_init(&mem)) {
		CHECK(!"mem_init failed");
		return;
	}
	mem_map(&mem, 0, 0x20000, MEM_RW);
	static struct rave r;
	rave_init(&r, &mem);
	r.gpu = gpu;

	CHECK_EQ(gestalt(&r, GESTALT_VENDOR), 1);       /* ATI's, as games want */
	CHECK_EQ(gestalt(&r, GESTALT_ENGINE), 5);
	CHECK_EQ(gestalt(&r, GESTALT_NAME_LENGTH), 8);  /* "syshm 3D" */

	uint32_t device[6] = { 0, SIZE * 4, PIXEL_ARGB32, SIZE, SIZE, FB };    /* memory: rowBytes, type, w, h, base */
	for (int k = 0; k < 6; k++)
		mem_w32(&mem, DEVICE + 4 * k, device[k]);
	uint32_t rect[4] = { 0, SIZE, 0, SIZE };        /* left, right, top, bottom */
	for (int k = 0; k < 4; k++)
		mem_w32(&mem, RECT + 4 * k, rect[k]);
	CHECK_EQ(call(&r, RAVE_ENTRY_ENGINE + EM_CHECK_DEVICE, DEVICE, 0, 0, 0, 0), 0);
	call(&r, RAVE_ENTRY_ENGINE + EM_DRAW_PRIVATE_NEW, CTX, DEVICE, RECT, 0, 0);    /* (then registers methods) */
	uint32_t context = mem_r32(&mem, CTX);
	CHECK(context != 0);

	/* a 2 x 2 texture, rows top to bottom in memory: v = 0 is the bottom one */
	static const uint32_t texels[4] = { 0xFF0000AAu, 0xFF0000BBu, 0xFF0000CCu, 0xFF0000DDu };
	for (int k = 0; k < 4; k++)
		mem_w32(&mem, PIXELS + 4 * k, texels[k]);
	uint32_t image[4] = { 2, 2, 8, PIXELS };        /* width, height, rowBytes, pixmap */
	for (int k = 0; k < 4; k++)
		mem_w32(&mem, IMAGE + 4 * k, image[k]);
	CHECK_EQ(call(&r, RAVE_ENTRY_ENGINE + EM_TEXTURE_NEW, 0, PIXEL_ARGB32, IMAGE, OUT, 0), 0);
	uint32_t texture = mem_r32(&mem, OUT);

	call(&r, RAVE_ENTRY_DRAW + DM_SET_PTR, CTX, TAG_TEXTURE, texture, 0, 0);
	call(&r, RAVE_ENTRY_DRAW + DM_SET_INT, CTX, TAG_TEXTURE_OP, 0, 0, 0);
	CHECK_EQ(call(&r, RAVE_ENTRY_DRAW + DM_RENDER_START, CTX, 0, 0, 0, 0), 0);
	vertex(VERTS, 0, 0, 0.5f, 1, 0, 0);
	vertex(VERTS + 64, SIZE, 0, 0.5f, 1, 1, 0);
	vertex(VERTS + 128, SIZE, SIZE, 0.5f, 1, 1, 1);
	vertex(VERTS + 192, 0, SIZE, 0.5f, 1, 0, 1);
	call(&r, RAVE_ENTRY_DRAW + DM_DRAW_TRI_TEXTURE, CTX, VERTS, VERTS + 64, VERTS + 128, 0);
	call(&r, RAVE_ENTRY_DRAW + DM_DRAW_TRI_TEXTURE, CTX, VERTS, VERTS + 128, VERTS + 192, 0);
	call(&r, RAVE_ENTRY_DRAW + DM_RENDER_END, CTX, 0, 0, 0, 0);
	CHECK_EQ(pixel(1, 1), 0xFF0000CCu);
	CHECK_EQ(pixel(14, 1), 0xFF0000DDu);
	CHECK_EQ(pixel(1, 14), 0xFF0000AAu);
	CHECK_EQ(pixel(14, 14), 0xFF0000BBu);

	/* many triangles, each nearer than the last: the frame shows the last whichever batch drew it */
	call(&r, RAVE_ENTRY_DRAW + DM_RENDER_START, CTX, 0, 0, 0, 0);
	enum { N = 1500 };
	for (int k = 0; k < N; k++) {
		float z = 1 - (float)(k + 1) / (N + 1), grey = (float)(k % 251) / 255;
		vertex(VERTS, -1, -1, z, grey, 0, 0);
		vertex(VERTS + 64, 3 * SIZE, -1, z, grey, 0, 0);
		vertex(VERTS + 128, -1, 3 * SIZE, z, grey, 0, 0);
		call(&r, RAVE_ENTRY_DRAW + DM_DRAW_TRI_GOURAUD, CTX, VERTS, VERTS + 64, VERTS + 128, 0);
	}
	call(&r, RAVE_ENTRY_DRAW + DM_RENDER_END, CTX, 0, 0, 0, 0);
	uint32_t last = (N - 1) % 251 * 0x010101u | 0xFF000000u;
	unsigned wrong = 0;
	for (int y = 0; y < SIZE; y++)
		for (int x = 0; x < SIZE; x++)
			wrong += pixel(x, y) != last;
	CHECK_EQ(wrong, 0);

	/*
	 * ATI's extensions (as Unreal Tournament uses them): only for an engine
	 * whose data says it's this syshm's, then a Rage 128 and a table of
	 * methods, among them replacing a texture's pixels for the frames after.
	 */
	enum { DM_GET_INT = 4, DM_GET_PTR = 5, DM_CLEAR_DRAW_BUFFER = 29, DM_CLEAR_Z_BUFFER = 30 };
	CHECK_EQ(call(&r, RAVE_ENTRY_DRAW + DM_GET_INT, CTX, 1011, 0, 0, 0), 0);
	CHECK_EQ(call(&r, RAVE_ENTRY_DRAW + DM_GET_PTR, CTX, 1021, 0, 0, 0), 0);
	mem_w32(&mem, TOC + r.layout.extra, 0x73793364u);      /* (what the fragment's data starts with) */
	mem_w32(&mem, TOC + r.layout.extra + 4, 2);
	CHECK_EQ(call(&r, RAVE_ENTRY_DRAW + DM_GET_INT, CTX, 1011, 0, 0, 0), 0x500);
	uint32_t table = call(&r, RAVE_ENTRY_DRAW + DM_GET_PTR, CTX, 1021, 0, 0, 0);
	CHECK_EQ(table, TOC + r.layout.extra + 8);
	CHECK_EQ(mem_r32(&mem, table), TOC + r.layout.entry[RAVE_ENTRY_DRAW + DM_CLEAR_DRAW_BUFFER]);
	CHECK_EQ(mem_r32(&mem, table + 4), TOC + r.layout.entry[RAVE_ENTRY_DRAW + DM_CLEAR_Z_BUFFER]);
	CHECK_EQ(mem_r32(&mem, table + 8), TOC + r.layout.entry[RAVE_ENTRY_ATI_TEXTURE_UPDATE]);
	static const uint32_t updated[4] = { 0xFF00AA00u, 0xFF00BB00u, 0xFF00CC00u, 0xFF00DD00u };
	for (int k = 0; k < 4; k++)
		mem_w32(&mem, PIXELS + 4 * k, updated[k]);
	CHECK_EQ(call(&r, RAVE_ENTRY_ATI_TEXTURE_UPDATE, 0, PIXEL_ARGB32, IMAGE, texture, 0), 0);
	call(&r, RAVE_ENTRY_DRAW + DM_RENDER_START, CTX, 0, 0, 0, 0);
	vertex(VERTS, 0, 0, 0.5f, 1, 0, 0);
	vertex(VERTS + 64, SIZE, 0, 0.5f, 1, 1, 0);
	vertex(VERTS + 128, SIZE, SIZE, 0.5f, 1, 1, 1);
	vertex(VERTS + 192, 0, SIZE, 0.5f, 1, 0, 1);
	call(&r, RAVE_ENTRY_DRAW + DM_DRAW_TRI_TEXTURE, CTX, VERTS, VERTS + 64, VERTS + 128, 0);
	call(&r, RAVE_ENTRY_DRAW + DM_DRAW_TRI_TEXTURE, CTX, VERTS, VERTS + 128, VERTS + 192, 0);
	call(&r, RAVE_ENTRY_DRAW + DM_RENDER_END, CTX, 0, 0, 0, 0);
	CHECK_EQ(pixel(1, 1), 0xFF00CC00u);
	CHECK_EQ(pixel(14, 14), 0xFF00BB00u);

	/* ATI's multitexturing: a second texture (an even grey) modulating, at the coordinates in ks */
	static const uint32_t grey[4] = { 0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u };
	for (int k = 0; k < 4; k++)
		mem_w32(&mem, LM_PIXELS + 4 * k, grey[k]);
	uint32_t lm_image[4] = { 2, 2, 8, LM_PIXELS };
	for (int k = 0; k < 4; k++)
		mem_w32(&mem, LM_IMAGE + 4 * k, lm_image[k]);
	CHECK_EQ(call(&r, RAVE_ENTRY_ENGINE + EM_TEXTURE_NEW, 0, PIXEL_ARGB32, LM_IMAGE, OUT, 0), 0);
	uint32_t lightmap = mem_r32(&mem, OUT);
	call(&r, RAVE_ENTRY_DRAW + DM_SET_PTR, CTX, 1018, lightmap, 0, 0);
	call(&r, RAVE_ENTRY_DRAW + DM_SET_INT, CTX, 1012, 1, 0, 0);
	call(&r, RAVE_ENTRY_DRAW + DM_RENDER_START, CTX, 0, 0, 0, 0);
	for (uint32_t k = 0; k < 4; k++) {
		put_float(VERTS + 64 * k + 52, k == 1 || k == 2 ? 1.0f : 0.0f);    /* ks: u/w, v/w, 1/w */
		put_float(VERTS + 64 * k + 56, k >= 2 ? 1.0f : 0.0f);
		put_float(VERTS + 64 * k + 60, 1);
	}
	call(&r, RAVE_ENTRY_DRAW + DM_DRAW_TRI_TEXTURE, CTX, VERTS, VERTS + 64, VERTS + 128, 0);
	call(&r, RAVE_ENTRY_DRAW + DM_DRAW_TRI_TEXTURE, CTX, VERTS, VERTS + 128, VERTS + 192, 0);
	call(&r, RAVE_ENTRY_DRAW + DM_RENDER_END, CTX, 0, 0, 0, 0);
	CHECK_EQ(pixel(1, 1), 0xFF006600u);            /* 0xCC times 0x80 / 0xFF */
	CHECK_EQ(pixel(14, 14), 0xFF005E00u);          /* 0xBB ... */
	call(&r, RAVE_ENTRY_DRAW + DM_SET_INT, CTX, 1012, 0, 0, 0);
	call(&r, RAVE_ENTRY_ENGINE + EM_TEXTURE_DELETE, lightmap, 0, 0, 0, 0);

	call(&r, RAVE_ENTRY_ENGINE + EM_TEXTURE_DELETE, texture, 0, 0, 0, 0);
	call(&r, RAVE_ENTRY_ENGINE + EM_DRAW_PRIVATE_DELETE, context, 0, 0, 0, 0);
	CHECK_EQ(r.frames, 4);
	CHECK_EQ(r.gpu_frames, gpu ? 4 : 0);
	rave_free(&r);
	mem_free(&mem);
}
