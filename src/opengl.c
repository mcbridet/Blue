#include "opengl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "be.h"
#include "opengl_table.h"
#include "tools.h"

/* ---- the functions (opengl_table.h) --------------------------------------------- */

struct function {
	const char *name, *result, *args;
	uint16_t offset;                /* in a context's dispatch table (gl) */
};

static const struct function library_functions[] = {
#define X(name, result, args, offset) { #name, result, args, offset },
	OPENGL_LIBRARY_FUNCTIONS(X)
#undef X
};

enum {
#define X(name, result, args, offset) F_##name,
	OPENGL_LIBRARY_FUNCTIONS(X)
#undef X
	F_LIBRARY
};

static const struct function memory_functions[] = {
#define X(name, result, args, offset) { #name, result, args, offset },
	OPENGL_MEMORY_FUNCTIONS(X)
#undef X
};

enum {
#define X(name, result, args, offset) M_##name,
	OPENGL_MEMORY_FUNCTIONS(X)
#undef X
	M_LIBRARY
};

static const struct function utility_functions[] = {
#define X(name, result, args, offset) { #name, result, args, offset },
	OPENGL_UTILITY_FUNCTIONS(X)
#undef X
};

enum {
#define X(name, result, args, offset) U_##name,
	OPENGL_UTILITY_FUNCTIONS(X)
#undef X
	U_LIBRARY
};

#define GL_FUNCTIONS F_aglChoosePixelFormat     /* (gl's first, in the dispatch table's order) */
#define TABLE_SLOTS 441u                        /* the dispatch table's, after the handle: to 0x6E4 */
#define CONTEXT_SIZE 0x700u                     /* (the handle and the table, rounded up) */
_Static_assert(GL_FUNCTIONS == 440 && 4 * TABLE_SLOTS + 4 <= CONTEXT_SIZE, "opengl_table.h");

/*
 * OpenGLLibrary's entries: each function; its initialization and
 * termination routines; one that does nothing (the dispatch table's slot
 * for glBlendFuncSeparate, which Mac OS 9 doesn't have); then each gl
 * function as the dispatch table calls it, with the context's handle first.
 */
enum { E_INIT = F_LIBRARY, E_TERM, E_NOTHING, E_TABLE, E_LIBRARY = E_TABLE + GL_FUNCTIONS };
_Static_assert(E_INIT == OPENGL_ENTRY_INIT && E_TERM == OPENGL_ENTRY_TERM && E_TABLE == OPENGL_ENTRY_TABLE,
	       "opengl.h's entries");

/* ---- OpenGL's and AGL's numbers ------------------------------------------------- */

enum { GL_NO_ERROR, GL_INVALID_ENUM = 0x0500, GL_INVALID_VALUE, GL_INVALID_OPERATION, GL_STACK_OVERFLOW,
       GL_STACK_UNDERFLOW, GL_OUT_OF_MEMORY };
#define GL_DEPTH_BUFFER_BIT 0x0100u
#define GL_ACCUM_BUFFER_BIT 0x0200u
#define GL_STENCIL_BUFFER_BIT 0x0400u
#define GL_COLOR_BUFFER_BIT 0x4000u
enum { GL_VENDOR = 0x1F00, GL_RENDERER, GL_VERSION, GL_EXTENSIONS };
enum { GL_SCISSOR_BOX = 0x0C10, GL_SCISSOR_TEST = 0x0C11, GL_VIEWPORT = 0x0BA2, GL_COLOR_CLEAR_VALUE = 0x0C22,
       GL_COLOR_WRITEMASK = 0x0C23, GL_DEPTH_CLEAR_VALUE = 0x0B73, GL_DEPTH_WRITEMASK = 0x0B72,
       GL_DOUBLEBUFFER = 0x0C32, GL_MAX_TEXTURE_SIZE = 0x0D33, GL_MAX_VIEWPORT_DIMS = 0x0D3A,
       GL_RED_BITS = 0x0D52, GL_GREEN_BITS, GL_BLUE_BITS, GL_ALPHA_BITS, GL_DEPTH_BITS, GL_STENCIL_BITS,
       GL_MAX_TEXTURE_UNITS_ARB = 0x84E2 };

/* AGL's (agl.h) */
enum { AGL_NONE, AGL_ALL_RENDERERS, AGL_BUFFER_SIZE, AGL_LEVEL, AGL_RGBA, AGL_DOUBLEBUFFER, AGL_STEREO,
       AGL_AUX_BUFFERS, AGL_RED_SIZE, AGL_GREEN_SIZE, AGL_BLUE_SIZE, AGL_ALPHA_SIZE, AGL_DEPTH_SIZE,
       AGL_STENCIL_SIZE, AGL_ACCUM_RED_SIZE, AGL_ACCUM_GREEN_SIZE, AGL_ACCUM_BLUE_SIZE, AGL_ACCUM_ALPHA_SIZE,
       AGL_PIXEL_SIZE = 50, AGL_MINIMUM_POLICY, AGL_MAXIMUM_POLICY, AGL_OFFSCREEN, AGL_FULLSCREEN,
       AGL_SAMPLE_BUFFERS_ARB, AGL_SAMPLES_ARB, AGL_RENDERER_ID = 70, AGL_SINGLE_RENDERER, AGL_NO_RECOVERY,
       AGL_ACCELERATED, AGL_CLOSEST_POLICY, AGL_ROBUST, AGL_BACKING_STORE, AGL_MP_SAFE = 78, AGL_WINDOW = 80,
       AGL_MULTISCREEN, AGL_VIRTUAL_SCREEN, AGL_COMPLIANT,
       AGL_BUFFER_MODES = 100, AGL_MIN_LEVEL, AGL_MAX_LEVEL, AGL_COLOR_MODES, AGL_ACCUM_MODES, AGL_DEPTH_MODES,
       AGL_STENCIL_MODES, AGL_MAX_AUX_BUFFERS, AGL_VIDEO_MEMORY = 120, AGL_TEXTURE_MEMORY, AGL_RENDERER_COUNT = 128 };
enum { AGL_SWAP_RECT = 200, AGL_BUFFER_RECT = 202, AGL_SWAP_LIMIT, AGL_COLORMAP_TRACKING = 210,
       AGL_COLORMAP_ENTRY = 212, AGL_RASTERIZATION = 220, AGL_SWAP_INTERVAL = 222, AGL_STATE_VALIDATION = 230 };
enum { AGL_NO_ERROR, AGL_BAD_ATTRIBUTE = 10000, AGL_BAD_PROPERTY, AGL_BAD_PIXELFMT, AGL_BAD_RENDINFO,
       AGL_BAD_CONTEXT, AGL_BAD_DRAWABLE, AGL_BAD_GDEV, AGL_BAD_STATE, AGL_BAD_VALUE, AGL_BAD_MATCH, AGL_BAD_ENUM,
       AGL_BAD_OFFSCREEN, AGL_BAD_FULLSCREEN, AGL_BAD_WINDOW, AGL_BAD_POINTER, AGL_BAD_MODULE, AGL_BAD_ALLOC,
       AGL_ERRORS };
#define AGL_MONOSCOPIC_BIT 0x1u
#define AGL_SINGLEBUFFER_BIT 0x4u
#define AGL_DOUBLEBUFFER_BIT 0x8u
#define AGL_0_BIT 0x1u
#define AGL_8_BIT 0x80u
#define AGL_16_BIT 0x400u
#define AGL_24_BIT 0x800u
#define AGL_32_BIT 0x1000u
#define AGL_RGB555_BIT 0x200u
#define AGL_RGB888_BIT 0x4000u
#define AGL_ARGB8888_BIT 0x8000u

/* AGL's options for aglEnable(), as bits */
enum { OPTION_SWAP_RECT, OPTION_BUFFER_RECT, OPTION_SWAP_LIMIT, OPTION_COLORMAP_TRACKING, OPTION_RASTERIZATION,
       OPTION_STATE_VALIDATION };

/* ---- identity ------------------------------------------------------------------- */

/* As Apple's ATI renderer (see docs/opengl-plan.md, Identity) */
#define RENDERER_ID 0x00021000u
#define VIDEO_MEMORY (32u << 20)
#define TEXTURE_MEMORY (32u << 20)
#define MAX_TEXTURE_SIZE 1024
#define TEXTURE_UNITS 2
static const char vendor[] = "ATI Technologies Inc.";
static const char renderer[] = "Rage 128 OpenGL Engine";
static const char version[] = "1.2 syshm";
static const char extensions[] = "";
/* aglErrorString()'s, as Apple's */
static const char *const agl_errors[AGL_ERRORS - AGL_BAD_ATTRIBUTE] = {
	"invalid pixel format attribute", "invalid renderer property", "invalid pixel format",
	"invalid renderer info", "invalid context", "invalid drawable", "invalid graphics device",
	"invalid context state", "invalid numerical value", "invalid share context", "invalid enumerant",
	"invalid offscreen drawable", "invalid fullscreen drawable", "invalid window", "invalid pointer",
	"invalid code module", "memory allocation failure",
};

/* ---- the library's data ----------------------------------------------------------- */

/*
 * After the entries' TVectors: what it is and its version (which goes up
 * when the fragment changes, so that one installed by an older syshm is
 * told apart), the current context (glc_cc), the recording buffer's
 * bounds, the strings glGetString() and aglErrorString() give; then,
 * zero-filled, the contexts, pixel formats and renderer infos.
 */
#define DATA_MAGIC 0x7379474Cu          /* 'syGL' */
#define DATA_VERSION 1u
enum { D_MAGIC = 0, D_VERSION = 4, D_CURRENT = OPENGL_DATA_CURRENT, D_RECORD = 12, D_RECORD_END = 16,
       D_STRINGS = 20 };
#define FORMATS 16
#define FORMAT_SIZE 32u
#define RENDERER_INFOS 4
#define RENDERER_INFO_SIZE 16u
#define FORMAT_MAGIC 0x73795046u        /* 'syPF' */
#define RENDERER_INFO_MAGIC 0x73795249u /* 'syRI' */
/* A pixel format's words: magic, next, flags, depth, stencil and pixel sizes, its device (a list of one) */
enum { PF_MAGIC = 0, PF_NEXT = 4, PF_FLAGS = 8, PF_DEPTH = 12, PF_STENCIL = 16, PF_PIXEL = 20, PF_DEVICE = 24 };
#define PF_DOUBLE 0x01u
#define PF_WINDOW 0x02u
#define PF_FULLSCREEN 0x04u
#define PF_OFFSCREEN 0x08u
#define PF_NO_RECOVERY 0x10u
#define PF_ALL_RENDERERS 0x20u
#define PF_POLICY_MASK 0x1C0u           /* (minimum 0x40, maximum 0x80, closest 0x100) */

static struct {
	uint8_t data[1024];
	size_t len;
	uint32_t vendor, renderer, version, extensions;
	uint32_t agl_error[AGL_ERRORS - AGL_BAD_ATTRIBUTE], no_error, unknown_error;
	uint32_t contexts, formats, renderer_infos, end;       /* the zero-filled parts */
} lib;

static uint32_t put_string(const char *s)
{
	uint32_t at = (uint32_t)lib.len;
	size_t n = strlen(s) + 1;
	memcpy(lib.data + lib.len, s, n);
	lib.len += n;
	return at;
}

static void make_data(void)
{
	if (lib.len)
		return;
	put_be32(lib.data + D_MAGIC, DATA_MAGIC);
	put_be32(lib.data + D_VERSION, DATA_VERSION);
	lib.len = D_STRINGS;
	lib.vendor = put_string(vendor);
	lib.renderer = put_string(renderer);
	lib.version = put_string(version);
	lib.extensions = put_string(extensions);
	lib.no_error = put_string("no error");
	lib.unknown_error = put_string("no such error code");
	for (unsigned k = 0; k < AGL_ERRORS - AGL_BAD_ATTRIBUTE; k++)
		lib.agl_error[k] = put_string(agl_errors[k]);
	lib.contexts = (uint32_t)(lib.len + 15) & ~15u;
	lib.formats = lib.contexts + OPENGL_CONTEXTS * CONTEXT_SIZE;
	lib.renderer_infos = lib.formats + FORMATS * FORMAT_SIZE;
	lib.end = lib.renderer_infos + RENDERER_INFOS * RENDERER_INFO_SIZE;
}

size_t opengl_build_library(uint8_t *out, size_t size, struct ndrv_layout *layout)
{
	static struct ndrv_export exports[F_LIBRARY + 1];
	make_data();
	for (unsigned k = 0; k < F_LIBRARY; k++)
		exports[k] = (struct ndrv_export){ .name = library_functions[k].name, .entry = k };
	exports[F_LIBRARY] = (struct ndrv_export){ .name = "glc_cc", .data = true, .offset = D_CURRENT };
	const struct ndrv_spec spec = {
		.kind = FRAG_LIBRARY,
		.entries = E_LIBRARY,
		.init_entry = E_INIT,
		.term_entry = E_TERM,
		.exports = exports,
		.export_count = F_LIBRARY + 1,
		.hostcall = HOSTCALL_TOOLS_GL,
		.extra = lib.data,
		.extra_len = lib.len,
		.zero_len = lib.end - (uint32_t)lib.len,
		.current_version = 1,           /* (as Apple's: see docs/opengl-plan.md) */
	};
	return ndrv_build(&spec, out, size, layout);
}

/* OpenGLMemory's allocations are Memory Manager blocks */
enum { MI_NEW_PTR, MI_NEW_PTR_CLEAR, MI_DISPOSE_PTR, MI_GET_PTR_SIZE, MI_COUNT };
static const struct ndrv_import memory_imports[MI_COUNT] = {
	[MI_NEW_PTR] = { "InterfaceLib", "NewPtr" },
	[MI_NEW_PTR_CLEAR] = { "InterfaceLib", "NewPtrClear" },
	[MI_DISPOSE_PTR] = { "InterfaceLib", "DisposePtr" },
	[MI_GET_PTR_SIZE] = { "InterfaceLib", "GetPtrSize" },
};

size_t opengl_build_memory(uint8_t *out, size_t size, struct ndrv_layout *layout)
{
	static struct ndrv_export exports[M_LIBRARY];
	for (unsigned k = 0; k < M_LIBRARY; k++)
		exports[k] = (struct ndrv_export){ .name = memory_functions[k].name, .entry = k };
	const struct ndrv_spec spec = {
		.kind = FRAG_LIBRARY, .entries = M_LIBRARY, .init_entry = -1, .exports = exports,
		.export_count = M_LIBRARY, .hostcall = HOSTCALL_TOOLS_GLM, .imports = memory_imports,
		.import_count = MI_COUNT, .current_version = 1,
	};
	return ndrv_build(&spec, out, size, layout);
}

size_t opengl_build_utility(uint8_t *out, size_t size, struct ndrv_layout *layout)
{
	static struct ndrv_export exports[U_LIBRARY];
	for (unsigned k = 0; k < U_LIBRARY; k++)
		exports[k] = (struct ndrv_export){ .name = utility_functions[k].name, .entry = k };
	const struct ndrv_spec spec = {
		.kind = FRAG_LIBRARY, .entries = U_LIBRARY, .init_entry = -1, .exports = exports,
		.export_count = U_LIBRARY, .hostcall = HOSTCALL_TOOLS_GLU, .current_version = 1,
	};
	return ndrv_build(&spec, out, size, layout);
}

int opengl_entry(const char *name)
{
	for (unsigned k = 0; k < F_LIBRARY; k++)
		if (!strcmp(library_functions[k].name, name))
			return (int)k;
	return -1;
}

void opengl_init(struct opengl *o, struct mem *mem)
{
	static uint8_t scratch[64 << 10];
	memset(o, 0, sizeof *o);
	o->mem = mem;
	opengl_build_library(scratch, sizeof scratch, &o->library);
	opengl_build_memory(scratch, sizeof scratch, &o->memory);
	opengl_build_utility(scratch, sizeof scratch, &o->utility);
}

/* ---- guest memory ---------------------------------------------------------------- */

static bool readable(const struct mem *m, uint32_t addr, uint32_t len)
{
	if (!len)
		return true;
	if (addr + len - 1 < addr)
		return false;
	for (uint32_t p = addr >> MEM_PAGE_SHIFT; p <= (addr + len - 1) >> MEM_PAGE_SHIFT; p++)
		if (!(m->pageflags[p] & MEM_R))
			return false;
	return true;
}

/* Writable, with any translated code there dropped (the host is about to write) */
static bool writable(struct mem *m, uint32_t addr, uint32_t len)
{
	if (!len)
		return true;
	if (addr + len - 1 < addr)
		return false;
	for (uint32_t p = addr >> MEM_PAGE_SHIFT; p <= (addr + len - 1) >> MEM_PAGE_SHIFT; p++) {
		if (!(m->pageflags[p] & MEM_W))
			return false;
		if (m->pageflags[p] & MEM_CODE)
			mem_invalidate(m, p << MEM_PAGE_SHIFT, MEM_PAGE_SIZE);
	}
	return true;
}

static void put_word(struct mem *m, uint32_t addr, uint32_t v)
{
	if (addr && writable(m, addr, 4))
		mem_w32(m, addr, v);
}

/* ---- instances, contexts ------------------------------------------------------------ */

static struct opengl_instance *instance_of(struct opengl *o, uint32_t toc, bool make)
{
	struct opengl_instance *free_one = NULL;
	for (unsigned k = 0; k < OPENGL_INSTANCES; k++) {
		if (o->inst[k].toc == toc)
			return &o->inst[k];
		if (!o->inst[k].toc && !free_one)
			free_one = &o->inst[k];
	}
	if (!make || !free_one)
		return NULL;
	memset(free_one, 0, sizeof *free_one);
	free_one->toc = toc;
	return free_one;
}

static uint32_t data_of(const struct opengl *o, const struct opengl_instance *in)
{
	return in->toc + o->library.extra;
}

static void free_context(struct opengl *o, struct opengl_context *x)
{
	(void)o;
	if (x->has_surface)
		surface_free(&x->surface);
	memset(x, 0, sizeof *x);
}

static void free_instance(struct opengl *o, struct opengl_instance *in)
{
	for (unsigned k = 0; k < OPENGL_CONTEXTS; k++)
		if (in->ctx[k].used)
			free_context(o, &in->ctx[k]);
	memset(in, 0, sizeof *in);
}

void opengl_free(struct opengl *o)
{
	for (unsigned k = 0; k < OPENGL_INSTANCES; k++)
		if (o->inst[k].toc)
			free_instance(o, &o->inst[k]);
}

/* The context whose AGLContext this is (in this instance), or NULL */
static struct opengl_context *context_at(struct opengl *o, struct opengl_instance *in, uint32_t guest)
{
	if (!in || !guest)
		return NULL;
	uint32_t first = data_of(o, in) + lib.contexts;
	uint32_t k = (guest - first) / CONTEXT_SIZE;
	if (guest < first || (guest - first) % CONTEXT_SIZE || k >= OPENGL_CONTEXTS || !in->ctx[k].used)
		return NULL;
	return &in->ctx[k];
}

/* The GPU, if contexts are to be drawn by it: asked once, when the first is made */
static struct gpu *gpu_of(struct opengl *o)
{
	if (o->gpu && !o->gpu_asked) {
		const char *why = NULL;
		o->gpu_asked = true;
		if (!gpu_ready(o->gpu, &why) || gpu_failed(o->gpu)) {
			printf("opengl: drawing in software (%s)\n", why ? why : "OpenGL failed");
			o->gpu = NULL;
		} else {
			printf("opengl: drawing with %s\n", gpu_renderer(o->gpu));
		}
		fflush(stdout);
	}
	return o->gpu;
}

/* The GPU failed: every context is drawn in software from now on */
static void lose_gpu(struct opengl *o)
{
	if (!o->gpu)
		return;
	printf("opengl: OpenGL failed; drawing in software from now on\n");
	fflush(stdout);
	for (unsigned i = 0; i < OPENGL_INSTANCES; i++)
		for (unsigned k = 0; k < OPENGL_CONTEXTS; k++)
			if (o->inst[i].ctx[k].has_surface)
				surface_lose_gpu(&o->inst[i].ctx[k].surface);
	o->gpu = NULL;
}

/* ---- drawables ---------------------------------------------------------------------- */

enum { DRAWABLE_NONE, DRAWABLE_PORT, DRAWABLE_OFFSCREEN };

/* A color port's pixmap: base, row bytes, bounds (top, left, bottom, right), bits a pixel; false if none */
static bool port_pixmap(const struct mem *m, uint32_t port, uint32_t *base, uint32_t *row_bytes, int bounds[4],
			unsigned *bits)
{
	if (!readable(m, port, 32) || (mem_r16(m, port + 6) & 0xC000) != 0xC000)
		return false;
	uint32_t pmh = mem_r32(m, port + 2);
	if (!readable(m, pmh, 4))
		return false;
	uint32_t pm = mem_r32(m, pmh);
	if (!readable(m, pm, 36))
		return false;
	*base = mem_r32(m, pm);
	*row_bytes = mem_r16(m, pm + 4) & 0x3FFF;
	for (int k = 0; k < 4; k++)
		bounds[k] = (int16_t)mem_r16(m, pm + 6 + 2 * k);
	*bits = mem_r16(m, pm + 32);
	return true;
}

/* A port's portRect: top, left, bottom, right */
static bool port_rect(const struct mem *m, uint32_t port, int r[4])
{
	if (!readable(m, port, 24))
		return false;
	for (int k = 0; k < 4; k++)
		r[k] = (int16_t)mem_r16(m, port + 16 + 2 * k);
	return r[2] > r[0] && r[3] > r[1];
}

/*
 * The drawable's size and where it is, from the port (or the buffer
 * rectangle, which counts from the bottom left of the port), and buffers of
 * that size; false if there are none
 */
static bool fit_drawable(struct opengl *o, struct opengl_context *x)
{
	int w = x->width, h = x->height, ox = x->origin_x, oy = x->origin_y;
	if (x->kind == DRAWABLE_PORT) {
		int r[4];
		if (!port_rect(o->mem, x->port, r))
			return false;
		w = r[3] - r[1];
		h = r[2] - r[0];
		ox = r[1];
		oy = r[0];
		if (x->enabled & 1u << OPTION_BUFFER_RECT && x->buffer_rect[2] > 0 && x->buffer_rect[3] > 0) {
			ox = r[1] + x->buffer_rect[0];
			oy = r[2] - x->buffer_rect[1] - x->buffer_rect[3];
			w = x->buffer_rect[2];
			h = x->buffer_rect[3];
		}
	}
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
		return false;
	x->origin_x = ox;
	x->origin_y = oy;
	if (x->has_surface && w == x->width && h == x->height)
		return true;
	if (x->has_surface)
		surface_free(&x->surface);
	x->has_surface = surface_init(&x->surface, w, h, gpu_of(o));
	if (!x->has_surface)
		return false;
	x->width = w;
	x->height = h;
	if (!x->attached) {             /* (as OpenGL says: the drawable's size, when first attached) */
		x->attached = true;
		x->st.viewport[2] = x->st.scissor[2] = w;
		x->st.viewport[3] = x->st.scissor[3] = h;
	}
	return true;
}

/* The frame drawn so far, into the drawable (but for the swap rectangle's outside) */
static void show(struct opengl *o, struct opengl_context *x)
{
	if (!x->has_surface || x->kind == DRAWABLE_NONE)
		return;
	if (!surface_finish(&x->surface))
		lose_gpu(o);
	else if (x->surface.gpu)
		o->gpu_frames++;
	x->frames++;
	o->frames++;
	int l = 0, t = 0, r = x->width, b = x->height;
	if (x->enabled & 1u << OPTION_SWAP_RECT) {
		l = x->swap_rect[0];
		r = l + x->swap_rect[2];
		b = x->height - x->swap_rect[1];
		t = b - x->swap_rect[3];
	}
	struct surface_dest d;
	memset(&d, 0, sizeof d);
	if (x->kind == DRAWABLE_OFFSCREEN) {
		d.base = x->base;
		d.row_bytes = x->row_bytes;
		d.bits = x->pixel_size;
		d.right = x->width;
		d.bottom = x->height;
		d.alpha = true;
	} else {
		int bounds[4];
		if (!port_pixmap(o->mem, x->port, &d.base, &d.row_bytes, bounds, &d.bits))
			return;
		d.right = bounds[3] - bounds[1];
		d.bottom = bounds[2] - bounds[0];
		d.x = x->origin_x - bounds[1];
		d.y = x->origin_y - bounds[0];
		d.region[0] = mem_r32(o->mem, x->port + 24);    /* visRgn, clipRgn */
		d.region[1] = mem_r32(o->mem, x->port + 28);
		d.region_x = x->origin_x;
		d.region_y = x->origin_y;
	}
	surface_present(&x->surface, o->mem, &d, l, t, r, b);
}

/* ---- arguments ------------------------------------------------------------------------ */

union arg {
	uint32_t u;
	int32_t i;
	float f;
	double d;
};

/* A call's arguments by its function's signature (after `skip` words) */
static unsigned get_args(const struct ndrv_call *c, const char *sig, unsigned skip, union arg *a)
{
	struct ndrv_args it = ndrv_args(c);
	for (unsigned k = 0; k < skip; k++)
		ndrv_next_word(&it);
	unsigned n = 0;
	for (; sig[n]; n++) {
		uint32_t w;
		uint64_t q;
		switch (sig[n]) {
		case 'f':
			w = ndrv_next_float(&it);
			memcpy(&a[n].f, &w, 4);
			break;
		case 'd':
			q = ndrv_next_double(&it);
			memcpy(&a[n].d, &q, 8);
			break;
		case 'b': a[n].i = (int8_t)ndrv_next_word(&it); break;
		case 'B': a[n].u = ndrv_next_word(&it) & 0xFF; break;
		case 's': a[n].i = (int16_t)ndrv_next_word(&it); break;
		case 'S': a[n].u = ndrv_next_word(&it) & 0xFFFF; break;
		default: a[n].u = ndrv_next_word(&it); break;
		}
	}
	return n;
}

static void log_call(const char *lib_name, const struct function *f, const union arg *a, unsigned n,
		     const char *what)
{
	printf("%s: %s(", lib_name, f->name);
	for (unsigned k = 0; k < n; k++) {
		const char *sep = k ? ", " : "";
		switch (f->args[k]) {
		case 'f': printf("%s%g", sep, (double)a[k].f); break;
		case 'd': printf("%s%g", sep, a[k].d); break;
		case 'b': case 's': case 'i': printf("%s%d", sep, a[k].i); break;
		default: printf("%s0x%X", sep, a[k].u); break;
		}
	}
	printf(")%s\n", what);
	fflush(stdout);
}

/* ---- GL ------------------------------------------------------------------------------- */

static void gl_error(struct opengl_context *x, uint32_t e)
{
	if (!x->st.error)
		x->st.error = e;
}

static float clampf(float v)
{
	return v > 0 ? (v < 1 ? v : 1) : 0;
}

static void gl_clear(struct opengl_context *x, uint32_t mask)
{
	if (mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT)) {
		gl_error(x, GL_INVALID_VALUE);
		return;
	}
	const struct gl_state *st = &x->st;
	bool color = mask & GL_COLOR_BUFFER_BIT && (st->color_mask[0] || st->color_mask[1] || st->color_mask[2] ||
						    st->color_mask[3]);
	bool depth = mask & GL_DEPTH_BUFFER_BIT && st->depth_mask && x->depth_bits;
	if (!x->has_surface || (!color && !depth))
		return;
	struct raster_state s;
	memset(&s, 0, sizeof s);
	s.clip_right = x->width;
	s.clip_bottom = x->height;
	s.color_keep = (st->color_mask[0] ? 0 : 0x00FF0000u) | (st->color_mask[1] ? 0 : 0x0000FF00u) |
		       (st->color_mask[2] ? 0 : 0x000000FFu) | (st->color_mask[3] ? 0 : 0xFF000000u);
	struct raster_cmd *c = surface_cmd(&x->surface, &s, RASTER_CMD_CLEAR);
	if (!c) {
		gl_error(x, GL_OUT_OF_MEMORY);
		return;
	}
	c->rect[2] = x->width;
	c->rect[3] = x->height;
	if (st->scissor_test) {         /* (counting from the bottom) */
		c->rect[0] = st->scissor[0];
		c->rect[2] = st->scissor[0] + st->scissor[2];
		c->rect[1] = x->height - st->scissor[1] - st->scissor[3];
		c->rect[3] = x->height - st->scissor[1];
	}
	c->clear_color = color;
	c->clear_depth = depth;
	const float *cc = st->clear_color;
	c->argb = (uint32_t)(clampf(cc[3]) * 255 + 0.5f) << 24 | (uint32_t)(clampf(cc[0]) * 255 + 0.5f) << 16 |
		  (uint32_t)(clampf(cc[1]) * 255 + 0.5f) << 8 | (uint32_t)(clampf(cc[2]) * 255 + 0.5f);
	c->z = (float)(st->clear_depth < 0 ? 0 : st->clear_depth > 1 ? 1 : st->clear_depth);
}

/* An integer state's values (glGetIntegerv and friends); 0 for an enum it doesn't know */
static unsigned gl_get(const struct opengl_context *x, uint32_t pname, double *v)
{
	const struct gl_state *st = &x->st;
	bool deep = x->pixel_size == 32;
	switch (pname) {
	case GL_VIEWPORT: for (int k = 0; k < 4; k++) v[k] = st->viewport[k]; return 4;
	case GL_SCISSOR_BOX: for (int k = 0; k < 4; k++) v[k] = st->scissor[k]; return 4;
	case GL_SCISSOR_TEST: v[0] = st->scissor_test; return 1;
	case GL_COLOR_CLEAR_VALUE: for (int k = 0; k < 4; k++) v[k] = st->clear_color[k]; return 4;
	case GL_COLOR_WRITEMASK: for (int k = 0; k < 4; k++) v[k] = st->color_mask[k]; return 4;
	case GL_DEPTH_CLEAR_VALUE: v[0] = st->clear_depth; return 1;
	case GL_DEPTH_WRITEMASK: v[0] = st->depth_mask; return 1;
	case GL_DOUBLEBUFFER: v[0] = (x->format & PF_DOUBLE) != 0; return 1;
	case GL_MAX_TEXTURE_SIZE: v[0] = MAX_TEXTURE_SIZE; return 1;
	case GL_MAX_TEXTURE_UNITS_ARB: v[0] = TEXTURE_UNITS; return 1;
	case GL_MAX_VIEWPORT_DIMS: v[0] = v[1] = 4096; return 2;
	case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: v[0] = deep ? 8 : 5; return 1;
	case GL_ALPHA_BITS: v[0] = deep ? 8 : 0; return 1;
	case GL_DEPTH_BITS: v[0] = x->depth_bits; return 1;
	case GL_STENCIL_BITS: v[0] = x->stencil_bits; return 1;
	default: return 0;
	}
}

static uint32_t gl_call(struct opengl *o, struct ndrv_call *c, struct opengl_context *x, unsigned fn,
			const union arg *a)
{
	struct gl_state *st = &x->st;
	struct mem *m = o->mem;
	switch (fn) {
	case F_glClearColor:
		for (int k = 0; k < 4; k++)
			st->clear_color[k] = clampf(a[k].f);
		return 0;
	case F_glClearDepth:
		st->clear_depth = a[0].d < 0 ? 0 : a[0].d > 1 ? 1 : a[0].d;
		return 0;
	case F_glClear:
		gl_clear(x, a[0].u);
		return 0;
	case F_glViewport:
	case F_glScissor:
		if (a[2].i < 0 || a[3].i < 0) {
			gl_error(x, GL_INVALID_VALUE);
			return 0;
		}
		for (int k = 0; k < 4; k++)
			(fn == F_glViewport ? st->viewport : st->scissor)[k] = a[k].i;
		if (fn == F_glViewport)         /* (as GL_MAX_VIEWPORT_DIMS) */
			for (int k = 2; k < 4; k++)
				st->viewport[k] = st->viewport[k] < 4096 ? st->viewport[k] : 4096;
		return 0;
	case F_glEnable:
	case F_glDisable:
		if (a[0].u == GL_SCISSOR_TEST)
			st->scissor_test = fn == F_glEnable;
		return 0;
	case F_glIsEnabled:
		return a[0].u == GL_SCISSOR_TEST ? st->scissor_test : 0;
	case F_glColorMask:
		for (int k = 0; k < 4; k++)
			st->color_mask[k] = a[k].u != 0;
		return 0;
	case F_glDepthMask:
		st->depth_mask = a[0].u != 0;
		return 0;
	case F_glGetError: {
		uint32_t e = st->error;
		st->error = GL_NO_ERROR;
		return e;
	}
	case F_glGetString: {
		uint32_t base = data_of(o, x->inst);
		switch (a[0].u) {
		case GL_VENDOR: return base + lib.vendor;
		case GL_RENDERER: return base + lib.renderer;
		case GL_VERSION: return base + lib.version;
		case GL_EXTENSIONS: return base + lib.extensions;
		default:
			gl_error(x, GL_INVALID_ENUM);
			return 0;
		}
	}
	case F_glGetBooleanv:
	case F_glGetIntegerv:
	case F_glGetFloatv:
	case F_glGetDoublev: {
		double v[16];
		unsigned n = gl_get(x, a[0].u, v);
		unsigned size = fn == F_glGetBooleanv ? 1 : fn == F_glGetDoublev ? 8 : 4;
		if (!n) {
			gl_error(x, GL_INVALID_ENUM);
			return 0;
		}
		if (!writable(m, a[1].u, n * size))
			return 0;
		for (unsigned k = 0; k < n; k++) {
			uint32_t at = a[1].u + k * size;
			if (fn == F_glGetBooleanv) {
				mem_w8(m, at, v[k] != 0);
			} else if (fn == F_glGetIntegerv) {
				mem_w32(m, at, (uint32_t)(int32_t)v[k]);
			} else if (fn == F_glGetFloatv) {
				float f = (float)v[k];
				uint32_t b;
				memcpy(&b, &f, 4);
				mem_w32(m, at, b);
			} else {
				uint64_t b;
				memcpy(&b, &v[k], 8);
				mem_w64(m, at, b);
			}
		}
		return 0;
	}
	case F_glFlush:
	case F_glFinish:
		if (!(x->format & PF_DOUBLE))
			show(o, x);
		return 0;
	default:
		if (!o->unimplemented[fn]) {
			o->unimplemented[fn] = 1;
			printf("opengl: %s isn't done yet\n", library_functions[fn].name);
			fflush(stdout);
		}
		(void)c;
		return 0;
	}
}

/* ---- AGL ------------------------------------------------------------------------------- */

static void agl_error(struct opengl_instance *in, uint32_t e)
{
	in->agl_error = e;
}

/* A device's depth (bits a pixel), or 0 */
static unsigned device_depth(const struct mem *m, uint32_t gdh)
{
	if (!readable(m, gdh, 4))
		return 0;
	uint32_t gd = mem_r32(m, gdh);
	if (!readable(m, gd, 26))
		return 0;
	uint32_t pmh = mem_r32(m, gd + 22);
	if (!readable(m, pmh, 4))
		return 0;
	uint32_t pm = mem_r32(m, pmh);
	return readable(m, pm, 34) ? mem_r16(m, pm + 32) : 0;
}

#define LM_MAIN_DEVICE 0x08A4u

/* The pixel format at this address (in this instance), or 0 */
static uint32_t format_at(struct opengl *o, struct opengl_instance *in, uint32_t pf)
{
	uint32_t first = data_of(o, in) + lib.formats;
	if (pf < first || (pf - first) % FORMAT_SIZE || (pf - first) / FORMAT_SIZE >= FORMATS ||
	    mem_r32(o->mem, pf + PF_MAGIC) != FORMAT_MAGIC)
		return 0;
	return pf;
}

static uint32_t renderer_info_at(struct opengl *o, struct opengl_instance *in, uint32_t ri)
{
	uint32_t first = data_of(o, in) + lib.renderer_infos;
	if (ri < first || (ri - first) % RENDERER_INFO_SIZE || (ri - first) / RENDERER_INFO_SIZE >= RENDERER_INFOS ||
	    mem_r32(o->mem, ri) != RENDERER_INFO_MAGIC)
		return 0;
	return ri;
}

/*
 * aglChoosePixelFormat(): one renderer, accelerated, RGBA in the device's 16
 * or 32 bits, single or double buffered, depth to 32 bits, stencil to 8, no
 * accumulation, auxiliary or stereo buffers; no 8-bit devices.
 */
static uint32_t choose_pixel_format(struct opengl *o, struct opengl_instance *in, uint32_t devices, int32_t ndev,
				    uint32_t attribs)
{
	struct mem *m = o->mem;
	uint32_t flags = PF_WINDOW, depth = 0, stencil = 0, pixel = 0;
	bool rgba = false;
	for (unsigned k = 0; k < 256; k++) {
		if (!readable(m, attribs + 4 * k, 4)) {
			agl_error(in, AGL_BAD_POINTER);
			return 0;
		}
		uint32_t at = mem_r32(m, attribs + 4 * k), v = 0;
		if (at == AGL_NONE)
			break;
		bool valued = (at >= AGL_BUFFER_SIZE && at <= AGL_ACCUM_ALPHA_SIZE && at != AGL_RGBA &&
			       at != AGL_DOUBLEBUFFER && at != AGL_STEREO) || at == AGL_PIXEL_SIZE ||
			      at == AGL_RENDERER_ID || at == AGL_SAMPLE_BUFFERS_ARB || at == AGL_SAMPLES_ARB;
		if (valued) {
			k++;
			if (!readable(m, attribs + 4 * k, 4)) {
				agl_error(in, AGL_BAD_POINTER);
				return 0;
			}
			v = mem_r32(m, attribs + 4 * k);
		}
		switch (at) {
		case AGL_RGBA: rgba = true; break;
		case AGL_DOUBLEBUFFER: flags |= PF_DOUBLE; break;
		case AGL_DEPTH_SIZE: depth = v; break;
		case AGL_STENCIL_SIZE: stencil = v; break;
		case AGL_PIXEL_SIZE: case AGL_BUFFER_SIZE: pixel = v; break;
		case AGL_FULLSCREEN: flags |= PF_FULLSCREEN; break;
		case AGL_OFFSCREEN: flags |= PF_OFFSCREEN; break;
		case AGL_WINDOW: flags |= PF_WINDOW; break;
		case AGL_NO_RECOVERY: flags |= PF_NO_RECOVERY; break;
		case AGL_ALL_RENDERERS: flags |= PF_ALL_RENDERERS; break;
		case AGL_MINIMUM_POLICY: flags |= 0x40; break;
		case AGL_MAXIMUM_POLICY: flags |= 0x80; break;
		case AGL_CLOSEST_POLICY: flags |= 0x100; break;
		case AGL_STEREO:
			return 0;
		case AGL_LEVEL: case AGL_AUX_BUFFERS: case AGL_ACCUM_RED_SIZE: case AGL_ACCUM_GREEN_SIZE:
		case AGL_ACCUM_BLUE_SIZE: case AGL_ACCUM_ALPHA_SIZE: case AGL_SAMPLE_BUFFERS_ARB: case AGL_SAMPLES_ARB:
			if (v)
				return 0;       /* (none of those) */
			break;
		case AGL_RENDERER_ID:
			if (v != RENDERER_ID)
				return 0;
			break;
		case AGL_RED_SIZE: case AGL_GREEN_SIZE: case AGL_BLUE_SIZE: case AGL_ALPHA_SIZE:
			if (v > 8)
				return 0;
			break;
		case AGL_ACCELERATED: case AGL_SINGLE_RENDERER: case AGL_ROBUST: case AGL_BACKING_STORE: case AGL_MP_SAFE:
		case AGL_MULTISCREEN: case AGL_VIRTUAL_SCREEN: case AGL_COMPLIANT:
			break;
		default:
			agl_error(in, AGL_BAD_ATTRIBUTE);
			return 0;
		}
	}
	if (!rgba || depth > 32 || stencil > 8)
		return 0;
	/* the devices (none given: the main device) must all be 16 or 32 bits a pixel */
	uint32_t device = 0;
	if (devices && ndev > 0) {
		for (int32_t k = 0; k < ndev && k < 16; k++) {
			if (!readable(m, devices + 4 * (uint32_t)k, 4)) {
				agl_error(in, AGL_BAD_GDEV);
				return 0;
			}
			uint32_t gdh = mem_r32(m, devices + 4 * (uint32_t)k);
			unsigned bits = device_depth(m, gdh);
			if (!bits) {
				agl_error(in, AGL_BAD_GDEV);
				return 0;
			}
			if (bits != 16 && bits != 32)
				return 0;
			device = device ? device : gdh;
		}
	} else {
		device = readable(m, LM_MAIN_DEVICE, 4) ? mem_r32(m, LM_MAIN_DEVICE) : 0;
		unsigned bits = device_depth(m, device);
		if (bits && bits != 16 && bits != 32 && !(flags & (PF_FULLSCREEN | PF_OFFSCREEN)))
			return 0;
	}
	unsigned bits = device_depth(m, device);
	pixel = bits == 16 || bits == 32 ? bits : pixel > 16 || !pixel ? 32 : 16;
	depth = !depth ? 0 : depth <= 16 ? 16 : depth <= 24 ? 24 : 32;
	stencil = stencil ? 8 : 0;
	uint32_t first = data_of(o, in) + lib.formats;
	for (unsigned k = 0; k < FORMATS; k++) {
		uint32_t pf = first + k * FORMAT_SIZE;
		if (mem_r32(m, pf + PF_MAGIC) == FORMAT_MAGIC)
			continue;
		if (!writable(m, pf, FORMAT_SIZE))
			break;
		memset(mem_ptr(m, pf), 0, FORMAT_SIZE);
		mem_w32(m, pf + PF_MAGIC, FORMAT_MAGIC);
		mem_w32(m, pf + PF_FLAGS, flags);
		mem_w32(m, pf + PF_DEPTH, depth);
		mem_w32(m, pf + PF_STENCIL, stencil);
		mem_w32(m, pf + PF_PIXEL, pixel);
		mem_w32(m, pf + PF_DEVICE, device);
		return pf;
	}
	agl_error(in, AGL_BAD_ALLOC);
	return 0;
}

/* aglDescribePixelFormat(): false for an attribute it doesn't know */
static bool describe_pixel_format(const struct mem *m, uint32_t pf, uint32_t attrib, int32_t *v)
{
	uint32_t flags = mem_r32(m, pf + PF_FLAGS), pixel = mem_r32(m, pf + PF_PIXEL);
	switch (attrib) {
	case AGL_ALL_RENDERERS: *v = (flags & PF_ALL_RENDERERS) != 0; break;
	case AGL_BUFFER_SIZE: case AGL_PIXEL_SIZE: *v = (int32_t)pixel; break;
	case AGL_LEVEL: case AGL_STEREO: case AGL_AUX_BUFFERS: case AGL_ACCUM_RED_SIZE: case AGL_ACCUM_GREEN_SIZE:
	case AGL_ACCUM_BLUE_SIZE: case AGL_ACCUM_ALPHA_SIZE: case AGL_SAMPLE_BUFFERS_ARB: case AGL_SAMPLES_ARB:
	case AGL_ROBUST: case AGL_BACKING_STORE: case AGL_MP_SAFE: case AGL_MULTISCREEN: case AGL_VIRTUAL_SCREEN:
		*v = 0;
		break;
	case AGL_RGBA: case AGL_ACCELERATED: case AGL_SINGLE_RENDERER: case AGL_COMPLIANT: *v = 1; break;
	case AGL_DOUBLEBUFFER: *v = (flags & PF_DOUBLE) != 0; break;
	case AGL_RED_SIZE: case AGL_GREEN_SIZE: case AGL_BLUE_SIZE: *v = pixel == 32 ? 8 : 5; break;
	case AGL_ALPHA_SIZE: *v = pixel == 32 ? 8 : 0; break;
	case AGL_DEPTH_SIZE: *v = (int32_t)mem_r32(m, pf + PF_DEPTH); break;
	case AGL_STENCIL_SIZE: *v = (int32_t)mem_r32(m, pf + PF_STENCIL); break;
	case AGL_MINIMUM_POLICY: *v = (flags & 0x40) != 0; break;
	case AGL_MAXIMUM_POLICY: *v = (flags & 0x80) != 0; break;
	case AGL_CLOSEST_POLICY: *v = (flags & 0x100) != 0; break;
	case AGL_OFFSCREEN: *v = (flags & PF_OFFSCREEN) != 0; break;
	case AGL_FULLSCREEN: *v = (flags & PF_FULLSCREEN) != 0; break;
	case AGL_WINDOW: *v = (flags & PF_WINDOW) != 0; break;
	case AGL_NO_RECOVERY: *v = (flags & PF_NO_RECOVERY) != 0; break;
	case AGL_RENDERER_ID: *v = (int32_t)RENDERER_ID; break;
	default: return false;
	}
	return true;
}

/* aglDescribeRenderer(): the one renderer there is */
static bool describe_renderer(uint32_t prop, int32_t *v)
{
	switch (prop) {
	case AGL_RENDERER_ID: *v = (int32_t)RENDERER_ID; break;
	case AGL_OFFSCREEN: case AGL_FULLSCREEN: case AGL_WINDOW: case AGL_ACCELERATED: case AGL_COMPLIANT: *v = 1; break;
	case AGL_ROBUST: case AGL_BACKING_STORE: case AGL_MP_SAFE: case AGL_MULTISCREEN: case AGL_MIN_LEVEL:
	case AGL_MAX_LEVEL: case AGL_ACCUM_MODES: case AGL_MAX_AUX_BUFFERS:
		*v = 0;
		break;
	case AGL_BUFFER_MODES: *v = AGL_MONOSCOPIC_BIT | AGL_SINGLEBUFFER_BIT | AGL_DOUBLEBUFFER_BIT; break;
	case AGL_COLOR_MODES: *v = AGL_RGB555_BIT | AGL_RGB888_BIT | AGL_ARGB8888_BIT; break;
	case AGL_DEPTH_MODES: *v = AGL_0_BIT | AGL_16_BIT | AGL_24_BIT | AGL_32_BIT; break;
	case AGL_STENCIL_MODES: *v = AGL_0_BIT | AGL_8_BIT; break;
	case AGL_VIDEO_MEMORY: *v = VIDEO_MEMORY; break;
	case AGL_TEXTURE_MEMORY: *v = TEXTURE_MEMORY; break;
	case AGL_RENDERER_COUNT: *v = 1; break;
	default: return false;
	}
	return true;
}

static struct opengl_context *new_context(struct opengl *o, struct opengl_instance *in, uint32_t pf)
{
	struct mem *m = o->mem;
	unsigned k = 0;
	while (k < OPENGL_CONTEXTS && in->ctx[k].used)
		k++;
	uint32_t guest = data_of(o, in) + lib.contexts + k * CONTEXT_SIZE;
	if (k == OPENGL_CONTEXTS || !writable(m, guest, CONTEXT_SIZE))
		return NULL;
	struct opengl_context *x = &in->ctx[k];
	memset(x, 0, sizeof *x);
	x->used = true;
	x->inst = in;
	x->guest = guest;
	x->format = mem_r32(m, pf + PF_FLAGS);
	x->depth_bits = mem_r32(m, pf + PF_DEPTH);
	x->stencil_bits = mem_r32(m, pf + PF_STENCIL);
	x->pixel_size = mem_r32(m, pf + PF_PIXEL);
	x->swap_interval = 0;
	struct gl_state *st = &x->st;
	st->clear_depth = 1;
	for (int j = 0; j < 4; j++)
		st->color_mask[j] = true;
	st->depth_mask = true;
	/* the handle (itself), then the dispatch table: each gl function's TVector, taking the handle first */
	mem_w32(m, guest, guest);
	for (unsigned slot = 0; slot < TABLE_SLOTS; slot++)
		mem_w32(m, guest + 4 + 4 * slot, in->toc + o->library.entry[E_NOTHING]);
	for (unsigned f = 0; f < GL_FUNCTIONS; f++)
		mem_w32(m, guest + library_functions[f].offset, in->toc + o->library.entry[E_TABLE + f]);
	o->contexts++;
	return x;
}

static uint32_t agl_call(struct opengl *o, struct ndrv_call *c, struct opengl_instance *in, unsigned fn,
			 const union arg *a)
{
	struct mem *m = o->mem;
	uint32_t data = data_of(o, in);
	struct opengl_context *x = NULL;
	switch (fn) {
	case F_aglChoosePixelFormat:
		return choose_pixel_format(o, in, a[0].u, a[1].i, a[2].u);
	case F_aglDestroyPixelFormat:
		if (format_at(o, in, a[0].u))
			put_word(m, a[0].u + PF_MAGIC, 0);
		else
			agl_error(in, AGL_BAD_PIXELFMT);
		return 0;
	case F_aglNextPixelFormat:
		if (!format_at(o, in, a[0].u))
			agl_error(in, AGL_BAD_PIXELFMT);
		return 0;
	case F_aglDescribePixelFormat: {
		int32_t v;
		if (!format_at(o, in, a[0].u)) {
			agl_error(in, AGL_BAD_PIXELFMT);
			return 0;
		}
		if (!describe_pixel_format(m, a[0].u, a[1].u, &v)) {
			agl_error(in, AGL_BAD_ATTRIBUTE);
			return 0;
		}
		put_word(m, a[2].u, (uint32_t)v);
		return 1;
	}
	case F_aglDevicesOfPixelFormat:
		if (!format_at(o, in, a[0].u)) {
			agl_error(in, AGL_BAD_PIXELFMT);
			return 0;
		}
		put_word(m, a[1].u, mem_r32(m, a[0].u + PF_DEVICE) ? 1 : 0);
		return a[0].u + PF_DEVICE;
	case F_aglQueryRendererInfo: {
		uint32_t first = data + lib.renderer_infos;
		for (unsigned k = 0; k < RENDERER_INFOS; k++) {
			uint32_t ri = first + k * RENDERER_INFO_SIZE;
			if (mem_r32(m, ri) != RENDERER_INFO_MAGIC && writable(m, ri, RENDERER_INFO_SIZE)) {
				mem_w32(m, ri, RENDERER_INFO_MAGIC);
				return ri;
			}
		}
		agl_error(in, AGL_BAD_ALLOC);
		return 0;
	}
	case F_aglDestroyRendererInfo:
		if (renderer_info_at(o, in, a[0].u))
			put_word(m, a[0].u, 0);
		else
			agl_error(in, AGL_BAD_RENDINFO);
		return 0;
	case F_aglNextRendererInfo:
		if (!renderer_info_at(o, in, a[0].u))
			agl_error(in, AGL_BAD_RENDINFO);
		return 0;
	case F_aglDescribeRenderer: {
		int32_t v;
		if (!renderer_info_at(o, in, a[0].u)) {
			agl_error(in, AGL_BAD_RENDINFO);
			return 0;
		}
		if (!describe_renderer(a[1].u, &v)) {
			agl_error(in, AGL_BAD_PROPERTY);
			return 0;
		}
		put_word(m, a[2].u, (uint32_t)v);
		return 1;
	}
	case F_aglCreateContext:
		if (!format_at(o, in, a[0].u)) {
			agl_error(in, AGL_BAD_PIXELFMT);
			return 0;
		}
		if (a[1].u && !context_at(o, in, a[1].u)) {
			agl_error(in, AGL_BAD_MATCH);
			return 0;
		}
		x = new_context(o, in, a[0].u);
		if (!x) {
			agl_error(in, AGL_BAD_ALLOC);
			return 0;
		}
		return x->guest;
	case F_aglGetVersion:           /* (1.1, as Mac OS 9.2.2's) */
		put_word(m, a[0].u, 1);
		put_word(m, a[1].u, 1);
		return 0;
	case F_aglConfigure:
		return 1;
	case F_aglGetError: {
		uint32_t e = in->agl_error;
		in->agl_error = AGL_NO_ERROR;
		return e;
	}
	case F_aglErrorString:
		return data + (a[0].u == AGL_NO_ERROR ? lib.no_error :
			       a[0].u >= AGL_BAD_ATTRIBUTE && a[0].u < AGL_ERRORS ? lib.agl_error[a[0].u - AGL_BAD_ATTRIBUTE] :
			       lib.unknown_error);
	case F_aglResetLibrary: {
		uint32_t toc = in->toc;
		free_instance(o, in);
		in->toc = toc;
		put_word(m, data + D_CURRENT, 0);
		return 0;
	}
	case F_aglGetCurrentContext:
		return readable(m, data + D_CURRENT, 4) ? mem_r32(m, data + D_CURRENT) : 0;
	case F_aglSetCurrentContext:
		if (a[0].u && !context_at(o, in, a[0].u)) {
			agl_error(in, AGL_BAD_CONTEXT);
			return 0;
		}
		put_word(m, data + D_CURRENT, a[0].u);
		return 1;
	case F_aglUseFont:
	case F_aglSetFullScreen:
		if (!o->unimplemented[fn]) {
			o->unimplemented[fn] = 1;
			printf("opengl: %s isn't done yet\n", library_functions[fn].name);
			fflush(stdout);
		}
		break;                  /* (checks the context, then fails) */
	default:
		break;
	}

	/* the rest take a context first */
	x = context_at(o, in, a[0].u);
	if (!x) {
		agl_error(in, AGL_BAD_CONTEXT);
		return 0;
	}
	switch (fn) {
	case F_aglDestroyContext:
		if (readable(m, data + D_CURRENT, 4) && mem_r32(m, data + D_CURRENT) == x->guest)
			put_word(m, data + D_CURRENT, 0);
		put_word(m, x->guest, 0);
		free_context(o, x);
		return 1;
	case F_aglCopyContext:
		if (!context_at(o, in, a[1].u)) {
			agl_error(in, AGL_BAD_CONTEXT);
			return 0;
		}
		context_at(o, in, a[1].u)->st = x->st;          /* (all of it, whatever the mask) */
		return 1;
	case F_aglUpdateContext:
		if (x->kind == DRAWABLE_PORT)
			fit_drawable(o, x);
		return 1;
	case F_aglSetDrawable:
		if (!a[1].u) {
			x->kind = DRAWABLE_NONE;
			x->port = 0;
			return 1;
		}
		{
			int r[4];
			if (!port_rect(m, a[1].u, r)) {
				agl_error(in, AGL_BAD_DRAWABLE);
				return 0;
			}
		}
		x->kind = DRAWABLE_PORT;
		x->port = a[1].u;
		if (!fit_drawable(o, x)) {
			x->kind = DRAWABLE_NONE;
			x->port = 0;
			agl_error(in, AGL_BAD_ALLOC);
			return 0;
		}
		return 1;
	case F_aglSetOffScreen:
		if (a[1].i <= 0 || a[2].i <= 0 || (uint32_t)a[3].i < (uint32_t)a[1].i * x->pixel_size / 8 || !a[4].u) {
			agl_error(in, AGL_BAD_OFFSCREEN);
			return 0;
		}
		x->kind = DRAWABLE_OFFSCREEN;
		x->port = 0;
		x->base = a[4].u;
		x->row_bytes = (uint32_t)a[3].i;
		x->width = a[1].i;              /* (fit_drawable() takes these) */
		x->height = a[2].i;
		x->origin_x = x->origin_y = 0;
		if (x->has_surface && (x->surface.target.width != a[1].i || x->surface.target.height != a[2].i)) {
			surface_free(&x->surface);
			x->has_surface = false;
		}
		if (!fit_drawable(o, x)) {
			x->kind = DRAWABLE_NONE;
			agl_error(in, AGL_BAD_ALLOC);
			return 0;
		}
		return 1;
	case F_aglSetFullScreen:
		agl_error(in, AGL_BAD_FULLSCREEN);
		return 0;
	case F_aglGetDrawable:
		return x->kind == DRAWABLE_PORT ? x->port : 0;
	case F_aglSetVirtualScreen:
		if (a[1].i) {
			agl_error(in, AGL_BAD_VALUE);
			return 0;
		}
		return 1;
	case F_aglGetVirtualScreen:
		return 0;
	case F_aglSwapBuffers:
		show(o, x);
		return 0;
	case F_aglEnable:
	case F_aglDisable:
	case F_aglIsEnabled: {
		static const uint32_t names[] = { AGL_SWAP_RECT, AGL_BUFFER_RECT, AGL_SWAP_LIMIT, AGL_COLORMAP_TRACKING,
						  AGL_RASTERIZATION, AGL_STATE_VALIDATION };
		unsigned k = 0;
		while (k < sizeof names / sizeof names[0] && names[k] != a[1].u)
			k++;
		if (k == sizeof names / sizeof names[0]) {
			agl_error(in, AGL_BAD_ENUM);
			return 0;
		}
		if (fn == F_aglIsEnabled)
			return (x->enabled >> k & 1) != 0;
		bool was = x->enabled >> k & 1;
		x->enabled = fn == F_aglEnable ? x->enabled | 1u << k : x->enabled & ~(1u << k);
		if (k == OPTION_BUFFER_RECT && was != (fn == F_aglEnable) && x->kind == DRAWABLE_PORT)
			fit_drawable(o, x);
		return 1;
	}
	case F_aglSetInteger:
	case F_aglGetInteger: {
		int32_t *v = a[1].u == AGL_SWAP_RECT ? x->swap_rect : a[1].u == AGL_BUFFER_RECT ? x->buffer_rect :
			     a[1].u == AGL_SWAP_INTERVAL ? &x->swap_interval : NULL;
		unsigned n = a[1].u == AGL_SWAP_RECT || a[1].u == AGL_BUFFER_RECT ? 4 : 1;
		if (!v) {
			if (a[1].u == AGL_SWAP_LIMIT || a[1].u == AGL_COLORMAP_ENTRY || a[1].u == AGL_RASTERIZATION ||
			    a[1].u == AGL_STATE_VALIDATION) {
				if (fn == F_aglGetInteger)
					put_word(m, a[2].u, 0);
				return 1;
			}
			agl_error(in, AGL_BAD_ENUM);
			return 0;
		}
		if (!(fn == F_aglSetInteger ? readable(m, a[2].u, 4 * n) : writable(m, a[2].u, 4 * n))) {
			agl_error(in, AGL_BAD_POINTER);
			return 0;
		}
		for (unsigned k = 0; k < n; k++) {
			if (fn == F_aglSetInteger)
				v[k] = (int32_t)mem_r32(m, a[2].u + 4 * k);
			else
				mem_w32(m, a[2].u + 4 * k, (uint32_t)v[k]);
		}
		if (fn == F_aglSetInteger && a[1].u == AGL_BUFFER_RECT && x->enabled & 1u << OPTION_BUFFER_RECT &&
		    x->kind == DRAWABLE_PORT)
			fit_drawable(o, x);
		return 1;
	}
	case F_aglUseFont:
	default:
		(void)c;
		return 0;
	}
}

/* ---- calls ----------------------------------------------------------------------------- */

static void finish_call(struct ndrv_call *c, const char *result, uint32_t v)
{
	ndrv_finish(c, result[0] == 'v' ? 0 : result[0] == 'B' ? (v ? 1 : 0) : v);
}

void opengl_library_call(struct opengl *o, struct ndrv_call *c)
{
	o->calls++;
	if (c->entry == E_INIT || c->entry == E_TERM) {
		/* CFM's initialization (a new process: its instance from scratch) and termination */
		struct opengl_instance *in = instance_of(o, c->toc, c->entry == E_INIT);
		if (in) {
			uint32_t toc = c->toc;
			free_instance(o, in);
			if (c->entry == E_INIT) {
				in->toc = toc;
				put_word(o->mem, data_of(o, in) + D_CURRENT, 0);
			}
		}
		ndrv_finish(c, 0);
		return;
	}
	unsigned fn = c->entry, skip = 0;
	if (fn >= E_TABLE && fn < E_LIBRARY) {          /* (through the dispatch table: the handle first) */
		fn -= E_TABLE;
		skip = 1;
	} else if (fn >= F_LIBRARY) {
		ndrv_finish(c, 0);
		return;
	}
	const struct function *f = &library_functions[fn];
	union arg a[16];
	unsigned n = get_args(c, f->args, skip, a);
	struct opengl_instance *in = instance_of(o, c->toc, true);
	if (!in) {
		ndrv_finish(c, 0);
		return;
	}
	uint32_t result;
	if (fn >= GL_FUNCTIONS) {
		result = agl_call(o, c, in, fn, a);
	} else {
		uint32_t data = data_of(o, in);
		uint32_t guest = skip ? ndrv_arg(c, 0) : readable(o->mem, data + D_CURRENT, 4) ?
			mem_r32(o->mem, data + D_CURRENT) : 0;
		struct opengl_context *x = context_at(o, in, guest);
		result = x ? gl_call(o, c, x, fn, a) : 0;
	}
	if (o->log) {
		char what[32] = "";
		if (f->result[0] != 'v')
			snprintf(what, sizeof what, " = 0x%X", result);
		log_call("gl", f, a, n, what);
	}
	finish_call(c, f->result, result);
}

/* OpenGLMemory: blocks from the Memory Manager (state word 0: the step; 1, 2: the call's) */
enum { MS_NONE, MS_REALLOC_SIZE, MS_REALLOC_NEW, MS_REALLOC_DISPOSE, MS_RESULT };

void opengl_memory_call(struct opengl *o, struct ndrv_call *c)
{
	struct mem *m = o->mem;
	if (c->entry == NDRV_ENTRY_RESUME) {
		uint32_t r = c->arg[0], step = ndrv_state(c, 0);
		switch (step) {
		case MS_REALLOC_SIZE:           /* (the old block's size: then a new block) */
			ndrv_set_state(c, 3, r);
			ndrv_set_state(c, 0, MS_REALLOC_NEW);
			ndrv_callout(c, ndrv_import(c, &o->memory, MI_NEW_PTR), ndrv_state(c, 2), 0, 0, 0);
			return;
		case MS_REALLOC_NEW: {          /* (its contents copied, the old one freed) */
			uint32_t old = ndrv_state(c, 1), n = ndrv_state(c, 3), size = ndrv_state(c, 2);
			n = n < size ? n : size;
			if (!r) {
				ndrv_finish(c, 0);
				return;
			}
			if (readable(m, old, n) && writable(m, r, n))
				memmove(mem_ptr(m, r), mem_ptr(m, old), n);
			ndrv_set_state(c, 1, r);
			ndrv_set_state(c, 0, MS_RESULT);
			ndrv_callout(c, ndrv_import(c, &o->memory, MI_DISPOSE_PTR), old, 0, 0, 0);
			return;
		}
		case MS_RESULT:
			ndrv_finish(c, ndrv_state(c, 1));
			return;
		default:
			ndrv_finish(c, r);
			return;
		}
	}
	if (c->entry >= M_LIBRARY) {
		ndrv_finish(c, 0);
		return;
	}
	const struct function *f = &memory_functions[c->entry];
	union arg a[8];
	unsigned n = get_args(c, f->args, 0, a);
	if (o->log)
		log_call("glm", f, a, n, "");
	ndrv_set_state(c, 0, MS_NONE);
	switch (c->entry) {
	case M_glmMalloc: case M_glmVecAlloc: case M_glmDCBAlloc:
		ndrv_callout(c, ndrv_import(c, &o->memory, MI_NEW_PTR), a[0].u, 0, 0, 0);
		return;
	case M_glmCalloc:
		ndrv_callout(c, ndrv_import(c, &o->memory, MI_NEW_PTR_CLEAR), a[0].u * a[1].u, 0, 0, 0);
		return;
	case M_glmFree: case M_glmVecFree: case M_glmDCBFree:
		if (a[0].u)
			ndrv_callout(c, ndrv_import(c, &o->memory, MI_DISPOSE_PTR), a[0].u, 0, 0, 0);
		else
			ndrv_finish(c, 0);
		return;
	case M_glmRealloc: case M_glmVecRealloc: case M_glmDCBRealloc:
		if (!a[0].u) {
			ndrv_callout(c, ndrv_import(c, &o->memory, MI_NEW_PTR), a[1].u, 0, 0, 0);
			return;
		}
		ndrv_set_state(c, 0, MS_REALLOC_SIZE);
		ndrv_set_state(c, 1, a[0].u);
		ndrv_set_state(c, 2, a[1].u);
		ndrv_callout(c, ndrv_import(c, &o->memory, MI_GET_PTR_SIZE), a[0].u, 0, 0, 0);
		return;
	case M_glmZero:
		if (writable(m, a[0].u, a[1].u))
			memset(mem_ptr(m, a[0].u), 0, a[1].u);
		break;
	case M_glmCopy:
		if (readable(m, a[0].u, a[2].u) && writable(m, a[1].u, a[2].u))
			memmove(mem_ptr(m, a[1].u), mem_ptr(m, a[0].u), a[2].u);
		break;
	case M_glmSetUByte: case M_glmSetUShort: case M_glmSetUInt: case M_glmSetDouble: {
		unsigned size = c->entry == M_glmSetUByte ? 1 : c->entry == M_glmSetUShort ? 2 :
				c->entry == M_glmSetUInt ? 4 : 8;
		if (a[1].u > 0x1000000u || !writable(m, a[0].u, a[1].u * size))
			break;
		for (uint32_t k = 0; k < a[1].u; k++) {
			uint32_t at = a[0].u + k * size;
			if (size == 1)
				mem_w8(m, at, (uint8_t)a[2].u);
			else if (size == 2)
				mem_w16(m, at, (uint16_t)a[2].u);
			else if (size == 4)
				mem_w32(m, at, a[2].u);
			else {
				uint64_t b;
				memcpy(&b, &a[2].d, 8);
				mem_w64(m, at, b);
			}
		}
		break;
	}
	default:                        /* (modes, debugging, status: accepted) */
		break;
	}
	ndrv_finish(c, 0);
}

void opengl_utility_call(struct opengl *o, struct ndrv_call *c)
{
	if (c->entry >= U_LIBRARY) {
		ndrv_finish(c, 0);
		return;
	}
	const struct function *f = &utility_functions[c->entry];
	union arg a[16];
	unsigned n = get_args(c, f->args, 0, a);
	if (o->log)
		log_call("glu", f, a, n, "");
	if (!(o->glu_told >> c->entry & 1)) {
		o->glu_told |= 1ull << c->entry;
		printf("opengl: %s isn't done yet\n", f->name);
		fflush(stdout);
	}
	ndrv_finish(c, 0);
}
