#include "rave.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "be.h"
#include "tools.h"

/* ---- RAVE's ABI (RAVE.h, RAVESystem.h) ---------------------------------------- */

enum { QA_NO_ERR, QA_ERROR, QA_OUT_OF_MEMORY, QA_NOT_SUPPORTED, QA_OUT_OF_DATE, QA_PARAM_ERR,
       QA_GESTALT_UNKNOWN };

enum { DEVICE_MEMORY, DEVICE_GDEVICE };
enum { PIXEL_ALPHA1, PIXEL_RGB16, PIXEL_ARGB16, PIXEL_RGB32, PIXEL_ARGB32, PIXEL_CL4, PIXEL_CL8,
       PIXEL_RGB8_332 = 9, PIXEL_ARGB16_4444, PIXEL_ACL16_88, PIXEL_I8, PIXEL_AI16_88 };

/* Engine methods (TQAEngineMethodTag) */
enum { EM_DRAW_PRIVATE_NEW, EM_DRAW_PRIVATE_DELETE, EM_CHECK_DEVICE, EM_GESTALT, EM_TEXTURE_NEW,
       EM_TEXTURE_DETACH, EM_TEXTURE_DELETE, EM_BITMAP_NEW, EM_BITMAP_DETACH, EM_BITMAP_DELETE,
       EM_COLOR_TABLE_NEW, EM_COLOR_TABLE_DELETE, EM_TEXTURE_BIND_COLOR_TABLE, EM_BITMAP_BIND_COLOR_TABLE,
       EM_ACCESS_TEXTURE, EM_ACCESS_TEXTURE_END, EM_ACCESS_BITMAP, EM_ACCESS_BITMAP_END, EM_COUNT };

/* Draw methods (TQADrawMethodTag) */
enum { DM_SET_FLOAT, DM_SET_INT, DM_SET_PTR, DM_GET_FLOAT, DM_GET_INT, DM_GET_PTR, DM_DRAW_POINT, DM_DRAW_LINE,
       DM_DRAW_TRI_GOURAUD, DM_DRAW_TRI_TEXTURE, DM_DRAW_V_GOURAUD, DM_DRAW_V_TEXTURE, DM_DRAW_BITMAP,
       DM_RENDER_START, DM_RENDER_END, DM_RENDER_ABORT, DM_FLUSH, DM_SYNC, DM_SUBMIT_V_GOURAUD,
       DM_SUBMIT_V_TEXTURE, DM_TRI_MESH_GOURAUD, DM_TRI_MESH_TEXTURE, DM_SET_NOTICE, DM_GET_NOTICE,
       DM_SUBMIT_MULTI_TEXTURE, DM_ACCESS_DRAW_BUFFER, DM_ACCESS_DRAW_BUFFER_END, DM_ACCESS_Z_BUFFER,
       DM_ACCESS_Z_BUFFER_END, DM_CLEAR_DRAW_BUFFER, DM_CLEAR_Z_BUFFER, DM_TEXTURE_FROM_CONTEXT,
       DM_BITMAP_FROM_CONTEXT, DM_BUSY, DM_SWAP_BUFFERS, DM_COUNT };

/* State tags */
enum { TAG_Z_FUNCTION = 0, TAG_ANTIALIAS = 8, TAG_BLEND = 9, TAG_PERSPECTIVE_Z = 10, TAG_TEXTURE_FILTER = 11,
       TAG_TEXTURE_OP = 12, TAG_TEXTURE = 13, TAG_FOG_MODE = 17, TAG_CHANNEL_MASK = 27, TAG_Z_BUFFER_MASK = 28,
       TAG_Z_SORTED_HINT = 29,
       TAG_ALPHA_TEST_FUNC = 31, TAG_GL_WRAP_U = 101, TAG_GL_WRAP_V = 102, TAG_GL_BLEND_SRC = 109,
       TAG_GL_BLEND_DST = 110 };
/*
 * ATI's engines answer an engine-specific tag with their hardware's
 * generation; Unreal Tournament offers RAVE only for 0x304 and up (a Rage
 * Pro), with its best settings from 0x500 (a Rage 128). With those, it gets
 * a table of ATI's extensions with another (a pointer): clearing the draw
 * buffer and the z buffer, as RAVE 1.6's methods (which it calls through
 * the table), and replacing a texture's pixels: (flags, pixel type, image,
 * texture). It calls them without checking for the table.
 *
 * It also multitextures the Rage 128's way: a second texture (its
 * lightmaps) modulates the first while TAG_ATI_MULTITEXTURE is on, at
 * coordinates in the vertices' ks (u/w, v/w, 1/w). The other tags it sets
 * meanwhile (1013 to 1016, 1019 and 1039, always the same, so presumably
 * the second stage's filtering, clamping and operation) are taken as
 * bilinear, clamped, modulating.
 */
#define TAG_ATI_HARDWARE 1011u
#define TAG_ATI_MULTITEXTURE 1012u
#define TAG_ATI_TEXTURE2 1018u          /* (a pointer) */
#define TAG_ATI_EXTENSIONS 1021u
#define ATI_HARDWARE_RAGE_128 0x500u
enum { BLEND_PREMULTIPLY, BLEND_INTERPOLATE, BLEND_OPENGL };
enum { TAG_COLOR_BG_A = 1, TAG_COLOR_BG_R, TAG_COLOR_BG_G, TAG_COLOR_BG_B, TAG_WIDTH, TAG_FOG_A = 18,
       TAG_FOG_R, TAG_FOG_G, TAG_FOG_B, TAG_FOG_START, TAG_FOG_END, TAG_FOG_DENSITY, TAG_FOG_MAX_DEPTH,
       TAG_ALPHA_TEST_REF = 46, TAG_BITMAP_SCALE_X = 52, TAG_BITMAP_SCALE_Y = 53 };

#define CONTEXT_NO_Z_BUFFER 0x01u
#define TEXTURE_MIPMAP 0x02u
#define TEXTURE_FLIP 0x40u
#define BITMAP_FLIP 0x40u

/* Engine gestalt (TQAGestaltSelector) */
enum { GESTALT_OPTIONAL, GESTALT_FAST, GESTALT_VENDOR, GESTALT_ENGINE, GESTALT_REVISION, GESTALT_NAME_LENGTH,
       GESTALT_NAME, GESTALT_TEXTURE_MEMORY, GESTALT_FAST_TEXTURE_MEMORY, GESTALT_CONTEXT_TYPES,
       GESTALT_CONTEXT_TYPES_PREFERRED, GESTALT_TEXTURE_TYPES, GESTALT_TEXTURE_TYPES_PREFERRED,
       GESTALT_BITMAP_TYPES, GESTALT_BITMAP_TYPES_PREFERRED, GESTALT_OPTIONAL2, GESTALT_MULTI_TEXTURE_MAX };

#define OPTIONAL_FEATURES (1u << 0 | 1u << 1 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 8 | 1u << 16 | 1u << 17 | \
			   1u << 18 | 1u << 22 | 1u << 23)      /* deep z, texture (color), blend (alpha), */
							/* perspective z, no dither, fog, z mask, alpha test */
#define FAST_FEATURES (1u << 0 | 1u << 1 | 1u << 2 | 1u << 4 | 1u << 9 | 1u << 10)
#define VENDOR_ATI 1u
#define ENGINE_ID 5u                    /* (Bugdom wants an ATI engine past 4) */
#define ENGINE_REVISION 32u
#define ENGINE_NAME "syshm 3D"
#define TEXTURE_MEMORY (64u << 20)
#define CONTEXT_TYPES (1u << PIXEL_RGB16 | 1u << PIXEL_ARGB16 | 1u << PIXEL_RGB32 | 1u << PIXEL_ARGB32)
#define IMAGE_TYPES (CONTEXT_TYPES | 1u << PIXEL_RGB8_332 | 1u << PIXEL_ARGB16_4444 | 1u << PIXEL_I8 | \
		     1u << PIXEL_AI16_88)

/* Handles given to the guest: opaque to it, an index here */
#define HANDLE_CONTEXT 0x52440000u      /* 'RD' */
#define HANDLE_IMAGE 0x52490000u        /* 'RI' */

/* ---- the code fragment -------------------------------------------------------- */

enum { E_INIT, E_GET_METHOD, E_ENGINE, E_DRAW = E_ENGINE + EM_COUNT, E_ATI_TEXTURE_UPDATE = E_DRAW + DM_COUNT,
       E_COUNT };
_Static_assert(E_ENGINE == RAVE_ENTRY_ENGINE && E_DRAW == RAVE_ENTRY_DRAW &&
	       E_ATI_TEXTURE_UPDATE == RAVE_ENTRY_ATI_TEXTURE_UPDATE, "rave.h's entry numbers");
enum { IMP_REGISTER_ENGINE, IMP_REGISTER_DRAW_METHOD, IMP_COUNT };

static const struct ndrv_import engine_imports[IMP_COUNT] = {
	[IMP_REGISTER_ENGINE] = { "QuickDraw\xAA 3D Accelerator", "QARegisterEngine" },
	[IMP_REGISTER_DRAW_METHOD] = { "QuickDraw\xAA 3D Accelerator", "QARegisterDrawMethod" },
};

/*
 * In the fragment's data: what it is and its version (which goes up when
 * the fragment changes, so that one installed by an older syshm is told
 * apart: the guest keeps the engine in its Extensions folder), then ATI's
 * extensions (see TAG_ATI_EXTENSIONS): TVector pointers, filled in when
 * they're asked for.
 */
#define ENGINE_DATA_MAGIC 0x73793364u   /* 'sy3d' */
#define ENGINE_DATA_VERSION 2u
enum { DATA_MAGIC, DATA_VERSION, DATA_ATI, DATA_WORDS = DATA_ATI + 3 };
enum { ATI_CLEAR_DRAW_BUFFER, ATI_CLEAR_Z_BUFFER, ATI_TEXTURE_UPDATE };
static const uint8_t engine_data[4 * DATA_WORDS] = { 's', 'y', '3', 'd', 0, 0, 0, ENGINE_DATA_VERSION };

size_t rave_build_engine(uint8_t *out, size_t size, struct ndrv_layout *layout)
{
	const struct ndrv_spec spec = {
		.kind = FRAG_LIBRARY,
		.entries = E_COUNT,
		.init_entry = E_INIT,
		.hostcall = HOSTCALL_TOOLS_RAVE,
		.imports = engine_imports,
		.import_count = IMP_COUNT,
		.extra = engine_data,
		.extra_len = sizeof engine_data,
	};
	return ndrv_build(&spec, out, size, layout);
}

void rave_init(struct rave *r, struct mem *mem)
{
	static uint8_t scratch[8192];
	memset(r, 0, sizeof *r);
	r->mem = mem;
	rave_build_engine(scratch, sizeof scratch, &r->layout);
}

static void free_context(struct rave_context *x)
{
	raster_wait();
	if (x->gpu_target)
		gpu_target_free(x->gpu, x->gpu_target);
	free(x->target.color);
	free(x->target.depth);
	free(x->verts);
	for (int k = 0; k < 2; k++) {
		free(x->batch[k].cmds);
		free(x->batch[k].states);
	}
	free(x->deferred);
	free(x->dstates);
	memset(x, 0, sizeof *x);
}

static void free_texture(struct rave *r, struct raster_texture *t)
{
	if (!t)
		return;
	if (t->gpu && r->gpu)
		gpu_texture_free(r->gpu, t);
	for (unsigned k = 0; k < t->levels; k++)
		free(t->texels[k]);
	free(t);
}

static void free_image(struct rave *r, struct rave_image *im)
{
	free_texture(r, im->tex);
	memset(im, 0, sizeof *im);
}

static bool drawing_recorded(const struct rave *r)
{
	for (unsigned k = 0; k < RAVE_MAX_CONTEXTS; k++)
		if (r->ctx[k].used && (r->ctx[k].batch[0].ncmds || r->ctx[k].batch[1].ncmds || r->ctx[k].ndeferred))
			return true;
	return false;
}

/* A texture done with: freed now, or once no recorded drawing can use it */
static void retire_texture(struct rave *r, struct raster_texture *t)
{
	if (!t)
		return;
	if (!drawing_recorded(r)) {
		free_texture(r, t);
		return;
	}
	if (r->ndoomed == r->doomed_cap) {
		unsigned cap = r->doomed_cap ? 2 * r->doomed_cap : 16;
		struct raster_texture **d = realloc(r->doomed, cap * sizeof *d);
		if (!d)
			return;                 /* (leaks rather than breaks) */
		r->doomed = d;
		r->doomed_cap = cap;
	}
	r->doomed[r->ndoomed++] = t;
}

static void delete_image(struct rave *r, struct rave_image *im)
{
	retire_texture(r, im->tex);
	im->tex = NULL;
	free_image(r, im);
}

static void free_doomed(struct rave *r)
{
	if (drawing_recorded(r))
		return;
	for (unsigned k = 0; k < r->ndoomed; k++)
		free_texture(r, r->doomed[k]);
	r->ndoomed = 0;
}

void rave_free(struct rave *r)
{
	for (unsigned k = 0; k < RAVE_MAX_CONTEXTS; k++)
		free_context(&r->ctx[k]);
	for (unsigned k = 0; k < r->nimages; k++)
		free_image(r, &r->images[k]);
	free(r->images);
	r->images = NULL;
	r->nimages = 0;
	free_doomed(r);
	free(r->doomed);
	r->doomed = NULL;
}

/* ---- guest memory -------------------------------------------------------------- */

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

static float fbits(uint32_t b)
{
	float f;
	memcpy(&f, &b, sizeof f);
	return f;
}

#define VGOURAUD_SIZE 32u
#define VTEXTURE_SIZE 64u

/* A TQAVGouraud or TQAVTexture (big-endian floats): color 0 the vertex's, 1 ks, 2 kd; unit 0 at u/w, v/w */
static void read_vertex(const uint8_t *p, bool textured, struct raster_vertex *v)
{
	memset(v, 0, sizeof *v);
	v->x = fbits(be32(p));
	v->y = fbits(be32(p + 4));
	v->z = fbits(be32(p + 8));
	v->invw = fbits(be32(p + 12));
	for (int k = 0; k < 4; k++)
		v->color[0][k] = fbits(be32(p + 16 + 4 * k));
	if (textured) {
		v->tex[0][0] = fbits(be32(p + 32));
		v->tex[0][1] = fbits(be32(p + 36));
		v->tex[0][2] = v->invw;
		for (int k = 0; k < 3; k++) {
			v->color[2][k] = fbits(be32(p + 40 + 4 * k));
			v->color[1][k] = v->tex[1][k] = fbits(be32(p + 52 + 4 * k));    /* (ATI's second texture's) */
		}
	} else {
		v->color[2][0] = v->color[2][1] = v->color[2][2] = 1;
	}
}

static bool read_vertices(const struct mem *m, uint32_t addr, unsigned n, bool textured, struct raster_vertex *v)
{
	uint32_t size = textured ? VTEXTURE_SIZE : VGOURAUD_SIZE;
	if (n > 0x100000u || !readable(m, addr, n * size))
		return false;
	for (unsigned k = 0; k < n; k++)
		read_vertex(mem_ptr(m, addr + k * size), textured, &v[k]);
	return true;
}

/* ---- images (textures and bitmaps) ---------------------------------------- */

static int image_bytes(uint32_t type)
{
	switch (type) {
	case PIXEL_RGB16: case PIXEL_ARGB16: case PIXEL_ARGB16_4444: case PIXEL_AI16_88: return 2;
	case PIXEL_RGB32: case PIXEL_ARGB32: return 4;
	case PIXEL_RGB8_332: case PIXEL_I8: return 1;
	default: return 0;
	}
}

static inline uint32_t expand5(uint32_t v) { return v << 3 | v >> 2; }

static uint32_t to_argb(uint32_t type, const uint8_t *p)
{
	switch (type) {
	case PIXEL_RGB16: case PIXEL_ARGB16: {
		uint32_t v = be16(p);
		uint32_t a = type == PIXEL_RGB16 || (v & 0x8000) ? 0xFFu : 0;
		return a << 24 | expand5(v >> 10 & 31) << 16 | expand5(v >> 5 & 31) << 8 | expand5(v & 31);
	}
	case PIXEL_RGB32: return 0xFF000000u | (be32(p) & 0xFFFFFF);
	case PIXEL_ARGB32: return be32(p);
	case PIXEL_RGB8_332: {
		uint32_t v = *p, r = v >> 5, g = v >> 2 & 7, b = v & 3;
		return 0xFF000000u | (r * 255 / 7) << 16 | (g * 255 / 7) << 8 | b * 85;
	}
	case PIXEL_ARGB16_4444: {
		uint32_t v = be16(p);
		return (v >> 12) * 17 << 24 | (v >> 8 & 15) * 17 << 16 | (v >> 4 & 15) * 17 << 8 | (v & 15) * 17;
	}
	case PIXEL_I8: return 0xFF000000u | *p * 0x010101u;
	case PIXEL_AI16_88: return (uint32_t)p[0] << 24 | p[1] * 0x010101u;
	default: return 0xFF000000u;
	}
}

/* Copies a TQAImage's pixels; false if they can't be read */
static bool load_level(const struct mem *m, uint32_t image, uint32_t type, bool flip, struct raster_texture *t,
		       unsigned level)
{
	uint32_t w = mem_r32(m, image), h = mem_r32(m, image + 4), rb = mem_r32(m, image + 8);
	uint32_t pix = mem_r32(m, image + 12);
	int bpp = image_bytes(type);
	if (!bpp || !w || !h || w > 4096 || h > 4096 || rb < w * (uint32_t)bpp || !readable(m, pix, rb * h))
		return false;
	uint32_t *out = malloc((size_t)w * h * 4);
	if (!out)
		return false;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *row = mem_ptr(m, pix + (flip ? h - 1 - y : y) * rb);
		for (uint32_t x = 0; x < w; x++)
			out[(size_t)y * w + x] = to_argb(type, row + x * (uint32_t)bpp);
	}
	t->width[level] = w;
	t->height[level] = h;
	t->texels[level] = out;
	return true;
}

static uint32_t new_image(struct rave *r)
{
	for (unsigned k = 0; k < r->nimages; k++)
		if (!r->images[k].used)
			return k;
	if (r->nimages >= 0x10000)
		return UINT32_MAX;
	struct rave_image *im = realloc(r->images, (r->nimages + 64) * sizeof *im);
	if (!im)
		return UINT32_MAX;
	memset(im + r->nimages, 0, 64 * sizeof *im);
	r->images = im;
	r->nimages += 64;
	return r->nimages - 64;
}

static struct rave_image *image_of(struct rave *r, uint32_t handle)
{
	uint32_t k = handle - HANDLE_IMAGE;
	return (handle & 0xFFFF0000u) == HANDLE_IMAGE && k < r->nimages && r->images[k].used ? &r->images[k] : NULL;
}

/* QATextureNew / QABitmapNew: copies the image(s) now (so detaching is a no-op) */
static uint32_t image_new(struct rave *r, bool bitmap, uint32_t flags, uint32_t type, uint32_t images, uint32_t out)
{
	struct mem *m = r->mem;
	if (!image_bytes(type) || !readable(m, images, 16) || !writable(m, out, 4))
		return QA_PARAM_ERR;
	uint32_t k = new_image(r);
	if (k == UINT32_MAX)
		return QA_OUT_OF_MEMORY;
	struct rave_image *im = &r->images[k];
	im->tex = calloc(1, sizeof *im->tex);
	if (!im->tex)
		return QA_OUT_OF_MEMORY;
	/* texture rows go bottom up (v = 0 is an image's bottom row, as in QuickDraw 3D); bitmaps' top down */
	bool flip = bitmap ? (flags & BITMAP_FLIP) != 0 : !(flags & TEXTURE_FLIP);
	unsigned levels = 1;
	if (!bitmap && (flags & TEXTURE_MIPMAP)) {
		uint32_t w = mem_r32(m, images), h = mem_r32(m, images + 4);
		while ((w > 1 || h > 1) && levels < RASTER_MAX_LEVELS) {
			w = w > 1 ? w / 2 : 1;
			h = h > 1 ? h / 2 : 1;
			levels++;
		}
	}
	for (unsigned l = 0; l < levels; l++) {
		if (!readable(m, images + 16 * l, 16) || !load_level(m, images + 16 * l, type, flip, im->tex, l)) {
			if (l == 0) {
				free_image(r, im);
				return QA_PARAM_ERR;
			}
			break;          /* (fewer levels than a full chain) */
		}
		im->tex->levels = l + 1;
	}
	im->used = true;
	im->bitmap = bitmap;
	im->flip = flip;
	r->textures++;
	if (r->log) {
		unsigned clear = 0, opaque = 0;
		uint32_t first_clear = 0;
		size_t n = (size_t)im->tex->width[0] * im->tex->height[0];
		for (size_t i = 0; i < n; i++) {
			uint32_t t = im->tex->texels[0][i];
			if (t >> 24 == 0) {
				if (!clear++)
					first_clear = t;
			} else {
				opaque++;
			}
		}
		printf("rave: image %u: %ux%u type %u flags %x levels %u: %u clear (first %08x), %u opaque\n", k,
		       im->tex->width[0], im->tex->height[0], type, flags, im->tex->levels, clear, first_clear, opaque);
	}
	mem_w32(m, out, HANDLE_IMAGE + k);
	return QA_NO_ERR;
}

/*
 * ATI's texture update (see TAG_ATI_EXTENSIONS): new pixels for a
 * texture's first level, the others kept if it's the same size. The texture
 * is a new one under the same handle, so drawing recorded with the old one
 * draws with that.
 */
static uint32_t ati_texture_update(struct rave *r, uint32_t type, uint32_t image, uint32_t handle)
{
	struct rave_image *im = image_of(r, handle);
	if (!im || !image_bytes(type) || !readable(r->mem, image, 16))
		return QA_PARAM_ERR;
	struct raster_texture *old = im->tex, *t = calloc(1, sizeof *t);
	if (!t)
		return QA_OUT_OF_MEMORY;
	if (!load_level(r->mem, image, type, im->flip, t, 0)) {
		free(t);
		return QA_PARAM_ERR;
	}
	t->levels = 1;
	if (old && old->width[0] == t->width[0] && old->height[0] == t->height[0]) {
		for (unsigned l = 1; l < old->levels; l++) {
			size_t n = (size_t)old->width[l] * old->height[l];
			t->texels[l] = malloc(n * 4);
			if (!t->texels[l])
				break;
			memcpy(t->texels[l], old->texels[l], n * 4);
			t->width[l] = old->width[l];
			t->height[l] = old->height[l];
			t->levels = l + 1;
		}
	}
	im->tex = t;
	retire_texture(r, old);
	return QA_NO_ERR;
}

/*
 * Whether the guest's engine is this syshm's (see ENGINE_DATA_MAGIC): what
 * it lacks, an older one doesn't get asked for, nor its data written past.
 */
static bool engine_current(struct rave *r, const struct ndrv_call *c)
{
	uint32_t d = c->toc + r->layout.extra;
	return readable(r->mem, d, 8) && mem_r32(r->mem, d + 4 * DATA_MAGIC) == ENGINE_DATA_MAGIC &&
	       mem_r32(r->mem, d + 4 * DATA_VERSION) >= ENGINE_DATA_VERSION;
}

/* The table of ATI's extensions, in the fragment's data (TVector pointers to the methods) */
static uint32_t ati_extensions_table(struct rave *r, const struct ndrv_call *c)
{
	uint32_t table = c->toc + r->layout.extra + 4 * DATA_ATI;
	if (!engine_current(r, c) || !writable(r->mem, table, 12))
		return 0;
	mem_w32(r->mem, table + 4 * ATI_CLEAR_DRAW_BUFFER, c->toc + r->layout.entry[E_DRAW + DM_CLEAR_DRAW_BUFFER]);
	mem_w32(r->mem, table + 4 * ATI_CLEAR_Z_BUFFER, c->toc + r->layout.entry[E_DRAW + DM_CLEAR_Z_BUFFER]);
	mem_w32(r->mem, table + 4 * ATI_TEXTURE_UPDATE, c->toc + r->layout.entry[E_ATI_TEXTURE_UPDATE]);
	return table;
}

/* ---- draw contexts ------------------------------------------------------------- */

static struct rave_context *context_of(struct rave *r, uint32_t draw_context)
{
	if (!readable(r->mem, draw_context, 4))
		return NULL;
	uint32_t k = mem_r32(r->mem, draw_context) - HANDLE_CONTEXT;
	return k < RAVE_MAX_CONTEXTS && r->ctx[k].used ? &r->ctx[k] : NULL;
}

/* Where a device's pixels are: base, row bytes, depth, and the origin of the context's rect there */
struct dest {
	uint32_t base, row_bytes, pixel_type;   /* (PIXEL_ types) */
	int32_t width, height;                  /* of the device */
	int32_t x, y;                           /* where the context's (0, 0) goes */
};

static bool device_dest(const struct rave *r, const struct rave_context *x, struct dest *d)
{
	const struct mem *m = r->mem;
	if (x->device_type == DEVICE_MEMORY) {
		d->row_bytes = x->device[0];
		d->pixel_type = x->device[1];
		d->width = (int32_t)x->device[2];
		d->height = (int32_t)x->device[3];
		d->base = x->device[4];
		d->x = x->left;
		d->y = x->top;
		return d->pixel_type >= PIXEL_RGB16 && d->pixel_type <= PIXEL_ARGB32;
	}
	uint32_t gdh = x->device[0];
	if (!readable(m, gdh, 4))
		return false;
	uint32_t gd = mem_r32(m, gdh);
	if (!readable(m, gd, 42))
		return false;
	uint32_t pmh = mem_r32(m, gd + 22);
	if (!readable(m, pmh, 4))
		return false;
	uint32_t pm = mem_r32(m, pmh);
	if (!readable(m, pm, 36))
		return false;
	int16_t top = (int16_t)mem_r16(m, gd + 34), left = (int16_t)mem_r16(m, gd + 36);
	int16_t bottom = (int16_t)mem_r16(m, gd + 38), right = (int16_t)mem_r16(m, gd + 40);
	unsigned depth = mem_r16(m, pm + 32);
	d->base = mem_r32(m, pm);
	d->row_bytes = mem_r16(m, pm + 4) & 0x3FFF;
	d->pixel_type = depth == 16 ? PIXEL_RGB16 : depth == 32 ? PIXEL_RGB32 : 0;
	d->width = right - left;
	d->height = bottom - top;
	d->x = x->left - left;
	d->y = x->top - top;
	return d->pixel_type != 0;
}

static bool device_supported(const struct rave *r, uint32_t device)
{
	const struct mem *m = r->mem;
	if (!readable(m, device, 24))
		return false;
	uint32_t type = mem_r32(m, device);
	if (type == DEVICE_MEMORY) {
		uint32_t pt = mem_r32(m, device + 8);
		return pt >= PIXEL_RGB16 && pt <= PIXEL_ARGB32;
	}
	if (type != DEVICE_GDEVICE)
		return false;
	struct rave_context probe = { .device_type = DEVICE_GDEVICE, .device = { mem_r32(m, device + 4) } };
	struct dest d;
	return device_dest(r, &probe, &d);
}

/* The clip region's bounding box, in the context's pixels; false if there is none */
static void clip_box(const struct rave *r, const struct rave_context *x, int *l, int *t, int *rt, int *b)
{
	*l = 0;
	*t = 0;
	*rt = x->target.width;
	*b = x->target.height;
	if (!x->clip || !readable(r->mem, x->clip, 4))
		return;
	uint32_t rgn = mem_r32(r->mem, x->clip);
	if (!readable(r->mem, rgn, 10))
		return;
	int top = (int16_t)mem_r16(r->mem, rgn + 2) - x->top, left = (int16_t)mem_r16(r->mem, rgn + 4) - x->left;
	int bottom = (int16_t)mem_r16(r->mem, rgn + 6) - x->top, right = (int16_t)mem_r16(r->mem, rgn + 8) - x->left;
	*l = left > *l ? left : *l;
	*t = top > *t ? top : *t;
	*rt = right < *rt ? right : *rt;
	*b = bottom < *b ? bottom : *b;
}

/* Copies [l, r) x [t, b) of a context's buffer to its device */
static void present(struct rave *r, struct rave_context *x, int l, int t, int rt, int b)
{
	struct dest d;
	if (!device_dest(r, x, &d))
		return;
	int cl, ct, cr, cb;
	clip_box(r, x, &cl, &ct, &cr, &cb);
	l = l > cl ? l : cl;
	t = t > ct ? t : ct;
	rt = rt < cr ? rt : cr;
	b = b < cb ? b : cb;
	/* ... and the device */
	l = l > -d.x ? l : -d.x;
	t = t > -d.y ? t : -d.y;
	rt = rt < d.width - d.x ? rt : d.width - d.x;
	b = b < d.height - d.y ? b : d.height - d.y;
	if (l >= rt || t >= b)
		return;
	int bpp = d.pixel_type <= PIXEL_ARGB16 ? 2 : 4;
	for (int y = t; y < b; y++) {
		uint32_t addr = d.base + (uint32_t)(d.y + y) * d.row_bytes + (uint32_t)(d.x + l) * (uint32_t)bpp;
		uint32_t len = (uint32_t)(rt - l) * (uint32_t)bpp;
		if (!writable(r->mem, addr, len))
			continue;
		uint8_t *p = mem_ptr(r->mem, addr);
		const uint32_t *src = x->target.color + (size_t)y * (size_t)x->target.width + (size_t)l;
		for (int k = 0; k < rt - l; k++) {
			uint32_t c = src[k];
			switch (d.pixel_type) {
			case PIXEL_RGB16:
			case PIXEL_ARGB16:
				put_be16(p + 2 * k, (uint16_t)((d.pixel_type == PIXEL_ARGB16 && c >> 31 ? 0x8000u : 0) |
							       (c >> 9 & 0x7C00) | (c >> 6 & 0x03E0) | (c >> 3 & 0x001F)));
				break;
			case PIXEL_RGB32:
				put_be32(p + 4 * k, c & 0x00FFFFFFu);
				break;
			default:
				put_be32(p + 4 * k, c);
				break;
			}
		}
	}
}

static float unit(float f)
{
	return f > 0 ? (f < 1 ? f : 1) : 0;
}

/* OpenGL blend factor constants to the rasterizer's */
static unsigned gl_factor(uint32_t gl)
{
	if (gl <= 1)
		return gl;              /* GL_ZERO, GL_ONE */
	if (gl >= 0x300 && gl <= 0x308) {
		static const uint8_t f[9] = { RASTER_F_SRC_COLOR, RASTER_F_ONE_MINUS_SRC_COLOR, RASTER_F_SRC_ALPHA,
					      RASTER_F_ONE_MINUS_SRC_ALPHA, RASTER_F_DST_ALPHA,
					      RASTER_F_ONE_MINUS_DST_ALPHA, RASTER_F_DST_COLOR,
					      RASTER_F_ONE_MINUS_DST_COLOR, RASTER_F_SRC_ALPHA_SATURATE };
		return f[gl - 0x300];
	}
	return RASTER_F_ONE;
}

/* The raster state from a context's tags */
static void state_of(struct rave *r, const struct rave_context *x, struct raster_state *s)
{
	memset(s, 0, sizeof *s);
	s->z_func = x->flags & CONTEXT_NO_Z_BUFFER ? RASTER_Z_NONE : x->ints[TAG_Z_FUNCTION];
	s->z_write = x->ints[TAG_Z_BUFFER_MASK] != 0;
	s->perspective_z = x->ints[TAG_PERSPECTIVE_Z] != 0;
	struct rave_image *im = image_of(r, x->texture);
	if (im && !im->bitmap) {
		/*
		 * The texture filter tag: Fast is nearest, from the nearest mipmap
		 * level; Mid bilinear, from the nearest level; Best trilinear (on the
		 * GPU, with anisotropic filtering where there's some).
		 */
		static const uint8_t min[3] = { RASTER_NEAREST_MIPMAP_NEAREST, RASTER_LINEAR_MIPMAP_NEAREST,
						RASTER_LINEAR_MIPMAP_LINEAR };
		uint32_t filter = x->ints[TAG_TEXTURE_FILTER] < 2 ? x->ints[TAG_TEXTURE_FILTER] : 2;
		struct raster_unit *u = &s->unit[0];
		u->texture = im->tex;
		u->min_filter = min[filter];
		u->mag_filter = filter ? RASTER_LINEAR : RASTER_NEAREST;
		u->clamp_s = x->ints[TAG_GL_WRAP_U] != 0;
		u->clamp_t = x->ints[TAG_GL_WRAP_V] != 0;
		u->anisotropy = filter == 2 ? 8 : 0;
		struct rave_image *im2 = x->ati_multitexture ? image_of(r, x->texture2) : NULL;
		if (im2 && !im2->bitmap && im2->tex->levels) {  /* (bilinear, clamped: see TAG_ATI_MULTITEXTURE) */
			u = &s->unit[1];
			u->texture = im2->tex;
			u->min_filter = RASTER_LINEAR_MIPMAP_NEAREST;
			u->mag_filter = RASTER_LINEAR;
			u->clamp_s = u->clamp_t = true;
			u->projective = true;
		}
		raster_rave_stages(s, x->ints[TAG_TEXTURE_OP]);
	}
	s->blend = x->ints[TAG_BLEND] == BLEND_INTERPOLATE ? RASTER_BLEND_INTERPOLATE :
		   x->ints[TAG_BLEND] == BLEND_OPENGL ? RASTER_BLEND_FACTORS : RASTER_BLEND_PREMULTIPLY;
	s->blend_src = gl_factor(x->ints[TAG_GL_BLEND_SRC]);
	s->blend_dst = gl_factor(x->ints[TAG_GL_BLEND_DST]);
	static const uint8_t fog_modes[5] = { RASTER_FOG_NONE, RASTER_FOG_ALPHA, RASTER_FOG_LINEAR, RASTER_FOG_EXP,
					      RASTER_FOG_EXP2 };
	s->fog_mode = x->ints[TAG_FOG_MODE] < 5 ? fog_modes[x->ints[TAG_FOG_MODE]] : RASTER_FOG_NONE;
	/*
	 * Fog parameters are taken from 0 to 1, as ATI's engine does (QuickDraw 3D
	 * passes densities scaled by the camera's range, far above 1; games tuned
	 * on ATI hardware expect a light haze from them), and the exponential
	 * fogs' density is per fog end: exp(-(density / end * w)^n).
	 */
	for (int k = 0; k < 4; k++)
		s->fog_color[k] = unit(x->floats[TAG_FOG_A + k]);
	s->fog_start = unit(x->floats[TAG_FOG_START]);
	s->fog_end = unit(x->floats[TAG_FOG_END]);
	float density = unit(x->floats[TAG_FOG_DENSITY]);
	s->fog_density = s->fog_end != 0 ? density / s->fog_end : density;
	s->alpha_test = x->ints[TAG_ALPHA_TEST_FUNC] <= RASTER_ALPHA_TRUE ? x->ints[TAG_ALPHA_TEST_FUNC] : 0;
	s->alpha_ref = x->floats[TAG_ALPHA_TEST_REF];
	s->width = x->floats[TAG_WIDTH];
	s->clip_left = 0;
	s->clip_top = 0;
	s->clip_right = x->target.width;
	s->clip_bottom = x->target.height;
}

static void context_defaults(struct rave_context *x)
{
	memset(x->ints, 0, sizeof x->ints);
	memset(x->floats, 0, sizeof x->floats);
	x->ints[TAG_Z_FUNCTION] = x->flags & CONTEXT_NO_Z_BUFFER ? RASTER_Z_NONE : RASTER_Z_LT;
	x->ints[TAG_CHANNEL_MASK] = 0xF;
	x->ints[TAG_Z_BUFFER_MASK] = 1;
	x->ints[TAG_GL_BLEND_SRC] = 1;          /* GL_ONE, GL_ZERO */
	x->floats[TAG_WIDTH] = 1;
	x->floats[TAG_FOG_END] = 1;
	x->floats[TAG_FOG_DENSITY] = 1;
	x->floats[TAG_FOG_MAX_DEPTH] = 1;
	x->floats[TAG_BITMAP_SCALE_X] = 1;
	x->floats[TAG_BITMAP_SCALE_Y] = 1;
	x->texture = 0;
	x->ati_multitexture = false;
	x->texture2 = 0;
}

/*
 * The GPU, if contexts are to be drawn by it: whether it can is asked when
 * the first is made (Mac OS makes none before the Finder is up, long after
 * the GPU's checks are done).
 */
static struct gpu *gpu_of(struct rave *r)
{
	if (r->gpu && !r->gpu_asked) {
		const char *why = NULL;
		r->gpu_asked = true;
		if (!gpu_ready(r->gpu, &why) || gpu_failed(r->gpu)) {
			printf("3d: drawing in software (%s)\n", why ? why : "OpenGL failed");
			r->gpu = NULL;
		} else {
			printf("3d: drawing with %s\n", gpu_renderer(r->gpu));
		}
		fflush(stdout);
	}
	return r->gpu;
}

/* The GPU failed: every context is drawn in software from now on (what it drew last is lost) */
static void lose_gpu(struct rave *r)
{
	if (!r->gpu)
		return;
	printf("3d: OpenGL failed; drawing in software from now on\n");
	fflush(stdout);
	for (unsigned k = 0; k < RAVE_MAX_CONTEXTS; k++) {
		struct rave_context *x = &r->ctx[k];
		if (x->gpu_target)
			gpu_target_free(x->gpu, x->gpu_target);
		x->gpu = NULL;
		x->gpu_target = NULL;
	}
	r->gpu = NULL;
}

static uint32_t context_new(struct rave *r, uint32_t guest, uint32_t device, uint32_t rect, uint32_t clip,
			    uint32_t flags)
{
	struct mem *m = r->mem;
	if (!device_supported(r, device) || !readable(m, rect, 16) || !writable(m, guest, 4))
		return QA_NOT_SUPPORTED;
	int32_t left = (int32_t)mem_r32(m, rect), right = (int32_t)mem_r32(m, rect + 4);
	int32_t top = (int32_t)mem_r32(m, rect + 8), bottom = (int32_t)mem_r32(m, rect + 12);
	if (right <= left || bottom <= top || right - left > 4096 || bottom - top > 4096)
		return QA_PARAM_ERR;
	unsigned k = 0;
	while (k < RAVE_MAX_CONTEXTS && r->ctx[k].used)
		k++;
	if (k == RAVE_MAX_CONTEXTS)
		return QA_OUT_OF_MEMORY;
	struct rave_context *x = &r->ctx[k];
	memset(x, 0, sizeof *x);
	x->target.width = right - left;
	x->target.height = bottom - top;
	size_t n = (size_t)x->target.width * (size_t)x->target.height;
	x->target.color = calloc(n, 4);
	x->target.depth = calloc(n, sizeof(float));
	if (!x->target.color || !x->target.depth) {
		free_context(x);
		return QA_OUT_OF_MEMORY;
	}
	struct gpu *gpu = gpu_of(r);
	if (gpu) {
		x->gpu_target = gpu_target_new(gpu, x->target.width, x->target.height);
		if (x->gpu_target)
			x->gpu = gpu;
		else
			lose_gpu(r);
	}
	x->used = true;
	x->guest = guest;
	x->device_type = mem_r32(m, device);
	for (int j = 0; j < 5; j++)
		x->device[j] = mem_r32(m, device + 4 + 4 * j);
	x->left = left;
	x->top = top;
	x->right = right;
	x->bottom = bottom;
	x->clip = clip && readable(m, clip, 8) && mem_r32(m, clip) == 0 ? mem_r32(m, clip + 4) : 0;
	x->flags = flags;
	context_defaults(x);
	mem_w32(m, guest, HANDLE_CONTEXT + k);  /* drawPrivate */
	r->contexts++;
	return QA_NO_ERR;
}

/* The state for recorded drawing, added to `states` unless it's the last one there */
static bool push_state(struct raster_state **states, unsigned *n, unsigned *cap, const struct raster_state *s,
		       unsigned *index)
{
	if (!*n || memcmp(&(*states)[*n - 1], s, sizeof *s)) {
		if (*n == *cap) {
			unsigned c = *cap ? 2 * *cap : 64;
			struct raster_state *st = realloc(*states, c * sizeof *st);
			if (!st)
				return false;
			*states = st;
			*cap = c;
		}
		(*states)[(*n)++] = *s;
	}
	*index = *n - 1;
	return true;
}

#define BATCH_CMDS 512u

/* A batch drawn in the background, by the context's backend */
static void draw_batch(struct rave_context *x, const struct rave_batch *b)
{
	if (x->gpu)
		gpu_start(x->gpu, x->gpu_target, b->states, b->cmds, b->ncmds);
	else
		raster_start(&x->target, b->states, b->cmds, b->ncmds);
}

/* The batch recorded so far drawn in the background, and the other (done with, then) recorded next */
static void start_batch(struct rave_context *x)
{
	draw_batch(x, &x->batch[x->rec]);
	x->rec ^= 1;
	x->batch[x->rec].ncmds = 0;
	x->batch[x->rec].nstates = 0;
	x->clear_recorded = false;
}

static struct raster_cmd *push_cmd(struct rave_context *x, const struct raster_state *s, uint8_t kind)
{
	struct rave_batch *b = &x->batch[x->rec];
	if (b->ncmds >= BATCH_CMDS + x->clear_recorded) {      /* (renderStart's clear doesn't count) */
		start_batch(x);
		b = &x->batch[x->rec];
	}
	unsigned state;
	if (!push_state(&b->states, &b->nstates, &b->states_cap, s, &state))
		return NULL;
	if (b->ncmds == b->cmds_cap) {
		unsigned cap = b->cmds_cap ? 2 * b->cmds_cap : BATCH_CMDS;
		struct raster_cmd *c = realloc(b->cmds, cap * sizeof *c);
		if (!c)
			return NULL;
		b->cmds = c;
		b->cmds_cap = cap;
	}
	struct raster_cmd *c = &b->cmds[b->ncmds++];
	memset(c, 0, offsetof(struct raster_cmd, v));
	c->kind = kind;
	c->state = state;
	return c;
}

/*
 * A triangle, recorded in order; or, while the client hints that what it
 * draws is transparent and wants it depth-sorted (kQATag_ZSortedHint, which
 * QuickDraw 3D sets with depth writes off), kept to be drawn back to front
 * at the end of the frame.
 */
static void triangle(struct rave_context *x, const struct raster_state *s, const struct raster_vertex *v0,
		     const struct raster_vertex *v1, const struct raster_vertex *v2, bool textured)
{
	if (!x->ints[TAG_Z_SORTED_HINT]) {
		struct raster_cmd *c = push_cmd(x, s, RASTER_CMD_TRIANGLE);
		if (c) {
			c->textured = textured;
			c->v[0] = *v0;
			c->v[1] = *v1;
			c->v[2] = *v2;
		}
		return;
	}
	unsigned state;
	if (!push_state(&x->dstates, &x->ndstates, &x->dstates_cap, s, &state))
		return;
	if (x->ndeferred == x->deferred_cap) {
		unsigned cap = x->deferred_cap ? 2 * x->deferred_cap : 256;
		struct rave_deferred *d = realloc(x->deferred, cap * sizeof *d);
		if (!d)
			return;
		x->deferred = d;
		x->deferred_cap = cap;
	}
	struct rave_deferred *d = &x->deferred[x->ndeferred];
	d->v[0] = *v0;
	d->v[1] = *v1;
	d->v[2] = *v2;
	d->state = state;
	d->textured = textured;
	d->order = x->ndeferred++;
	d->distance = 0;
	for (int k = 0; k < 3; k++)
		d->distance += s->perspective_z ? (d->v[k].invw > 0 ? 1.0f / d->v[k].invw : 1e30f) : d->v[k].z;
}

static int farther_first(const void *a, const void *b)
{
	const struct rave_deferred *p = a, *q = b;
	if (p->distance != q->distance)
		return p->distance > q->distance ? -1 : 1;
	return p->order < q->order ? -1 : 1;
}

/* (once drawing in the background is done) */
static void forget_frame(struct rave_context *x)
{
	raster_wait();
	if (x->gpu)
		gpu_wait(x->gpu);
	for (int k = 0; k < 2; k++) {
		x->batch[k].ncmds = 0;
		x->batch[k].nstates = 0;
	}
	x->clear_recorded = false;
	x->ndeferred = 0;
	x->ndstates = 0;
}

/*
 * Draws the frame recorded so far, sorted transparent triangles last, and
 * with `show`, has its pixels in the context's buffer to present. False if
 * they aren't there (the GPU failed: from now on everything is drawn in
 * software).
 */
static bool draw_frame(struct rave *r, struct rave_context *x, bool show)
{
	if (x->ndeferred) {
		qsort(x->deferred, x->ndeferred, sizeof *x->deferred, farther_first);
		for (unsigned k = 0; k < x->ndeferred; k++) {
			const struct rave_deferred *d = &x->deferred[k];
			struct raster_cmd *c = push_cmd(x, &x->dstates[d->state], RASTER_CMD_TRIANGLE);
			if (!c)
				break;
			c->textured = d->textured;
			memcpy(c->v, d->v, sizeof c->v);
		}
	}
	struct timespec t0, t1;
	if (r->fps)
		clock_gettime(CLOCK_MONOTONIC, &t0);
	draw_batch(x, &x->batch[x->rec]);
	forget_frame(x);
	bool shown = true;
	if (show && x->gpu) {
		shown = gpu_read(x->gpu, x->gpu_target, x->target.color);
		r->gpu_frames += shown;
	}
	free_doomed(r);
	if (r->fps) {
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double now = (double)t1.tv_sec + 1e-9 * (double)t1.tv_nsec;
		r->fps_raster += now - ((double)t0.tv_sec + 1e-9 * (double)t0.tv_nsec);
		r->fps_frames++;
		if (!r->fps_since)
			r->fps_since = now;
		else if (now - r->fps_since >= 1) {
			double span = now - r->fps_since;
			printf("3d: %.1f frames/s %s, waiting for drawing %.0f%% of the time\n", r->fps_frames / span,
			       x->gpu ? "on the GPU" : "in software", 100 * r->fps_raster / span);
			fflush(stdout);
			r->fps_since = now;
			r->fps_frames = 0;
			r->fps_raster = 0;
		}
	}
	if (!shown)
		lose_gpu(r);
	return shown;
}

/* Triangles of a vertex list by mode (TQAVertexMode) */
static void draw_vertices(struct rave *r, struct rave_context *x, const struct raster_vertex *v, unsigned n,
			  uint32_t mode, bool textured)
{
	struct raster_state s;
	state_of(r, x, &s);
	switch (mode) {
	case 0:                                         /* points */
		for (unsigned k = 0; k < n; k++) {
			struct raster_cmd *c = push_cmd(x, &s, RASTER_CMD_POINT);
			if (c)
				c->v[0] = v[k];
		}
		break;
	case 1:                                         /* lines */
	case 2:                                         /* polyline */
		for (unsigned k = 0; k + 1 < n; k += mode == 1 ? 2 : 1) {
			struct raster_cmd *c = push_cmd(x, &s, RASTER_CMD_LINE);
			if (c) {
				c->v[0] = v[k];
				c->v[1] = v[k + 1];
			}
		}
		break;
	case 3:                                         /* triangles */
		for (unsigned k = 0; k + 2 < n; k += 3)
			triangle(x, &s, &v[k], &v[k + 1], &v[k + 2], textured);
		r->triangles += n / 3;
		break;
	case 4:                                         /* strip */
		for (unsigned k = 0; k + 2 < n; k++)
			triangle(x, &s, &v[k], &v[k + 1], &v[k + 2], textured);
		r->triangles += n > 2 ? n - 2 : 0;
		break;
	case 5:                                         /* fan */
		for (unsigned k = 1; k + 1 < n; k++)
			triangle(x, &s, &v[0], &v[k], &v[k + 1], textured);
		r->triangles += n > 2 ? n - 2 : 0;
		break;
	}
}

static uint32_t render_start(struct rave *r, struct rave_context *x, uint32_t initial)
{
	forget_frame(x);
	struct rave_context *from = initial ? context_of(r, initial) : NULL;
	size_t n = (size_t)x->target.width * (size_t)x->target.height;
	if (from && from != x && from->target.width == x->target.width && from->target.height == x->target.height) {
		draw_frame(r, from, false);
		/* (both are drawn by the same backend: losing the GPU moves every context to software) */
		if (x->gpu && from->gpu) {
			gpu_copy(x->gpu, x->gpu_target, from->gpu_target);
			return QA_NO_ERR;
		}
		if (!x->gpu && !from->gpu) {
			memcpy(x->target.color, from->target.color, n * 4);
			memcpy(x->target.depth, from->target.depth, n * sizeof(float));
			return QA_NO_ERR;
		}
	}
	float bg[4];
	for (int k = 0; k < 4; k++)
		bg[k] = x->floats[TAG_COLOR_BG_A + k];
	uint32_t argb = 0;
	for (int k = 0; k < 4; k++) {
		float c = bg[k] < 0 ? 0 : bg[k] > 1 ? 1 : bg[k];
		argb = argb << 8 | (uint32_t)(c * 255 + 0.5f);
	}
	/* (cleared as the frame's first command, by whichever backend draws it) */
	struct raster_state s;
	state_of(r, x, &s);
	struct raster_cmd *c = push_cmd(x, &s, RASTER_CMD_CLEAR);
	if (c) {
		c->rect[0] = 0;
		c->rect[1] = 0;
		c->rect[2] = x->target.width;
		c->rect[3] = x->target.height;
		c->clear_color = c->clear_depth = true;
		c->argb = argb;
		c->z = raster_far(&s);
		x->clear_recorded = true;
	}
	return QA_NO_ERR;
}

/*
 * An abandoned frame isn't drawn, but its clear is: another context's frame
 * may start from this one's buffers.
 */
static void render_abort(struct rave *r, struct rave_context *x)
{
	if (x->clear_recorded) {
		struct rave_batch *b = &x->batch[x->rec];
		b->ncmds = 1;
		draw_batch(x, b);
	}
	forget_frame(x);
	free_doomed(r);
}

static void rect_of(const struct mem *m, uint32_t rect, const struct rave_context *x, int *l, int *t, int *rt,
		    int *b)
{
	*l = 0;
	*t = 0;
	*rt = x->target.width;
	*b = x->target.height;
	if (!rect || !readable(m, rect, 16))
		return;
	int32_t left = (int32_t)mem_r32(m, rect), right = (int32_t)mem_r32(m, rect + 4);
	int32_t top = (int32_t)mem_r32(m, rect + 8), bottom = (int32_t)mem_r32(m, rect + 12);
	*l = left > 0 ? left : 0;
	*t = top > 0 ? top : 0;
	*rt = right < *rt ? right : *rt;
	*b = bottom < *b ? bottom : *b;
}

/* ---- calls --------------------------------------------------------------------- */

static const char *const draw_names[DM_COUNT] = {
	"setFloat", "setInt", "setPtr", "getFloat", "getInt", "getPtr", "drawPoint", "drawLine", "drawTriGouraud",
	"drawTriTexture", "drawVGouraud", "drawVTexture", "drawBitmap", "renderStart", "renderEnd", "renderAbort",
	"flush", "sync", "submitVerticesGouraud", "submitVerticesTexture", "drawTriMeshGouraud",
	"drawTriMeshTexture", "setNoticeMethod", "getNoticeMethod", "submitMultiTextureParams", "accessDrawBuffer",
	"accessDrawBufferEnd", "accessZBuffer", "accessZBufferEnd", "clearDrawBuffer", "clearZBuffer",
	"textureFromContext", "bitmapFromContext", "busy", "swapBuffers",
};

static const char *const engine_names[EM_COUNT] = {
	"drawPrivateNew", "drawPrivateDelete", "engineCheckDevice", "engineGestalt", "textureNew",
	"textureDetach", "textureDelete", "bitmapNew", "bitmapDetach", "bitmapDelete", "colorTableNew",
	"colorTableDelete", "textureBindColorTable", "bitmapBindColorTable", "accessTexture", "accessTextureEnd",
	"accessBitmap", "accessBitmapEnd",
};

static uint32_t gestalt(struct rave *r, uint32_t selector, uint32_t response)
{
	struct mem *m = r->mem;
	uint32_t v;
	switch (selector) {
	case GESTALT_OPTIONAL: v = OPTIONAL_FEATURES; break;
	case GESTALT_FAST: v = FAST_FEATURES; break;
	case GESTALT_VENDOR: v = VENDOR_ATI; break;
	case GESTALT_ENGINE: v = ENGINE_ID; break;
	case GESTALT_REVISION: v = ENGINE_REVISION; break;
	case GESTALT_NAME_LENGTH: v = (uint32_t)strlen(ENGINE_NAME); break;
	case GESTALT_NAME:
		if (!writable(m, response, sizeof ENGINE_NAME))
			return QA_PARAM_ERR;
		memcpy(mem_ptr(m, response), ENGINE_NAME, sizeof ENGINE_NAME);
		return QA_NO_ERR;
	case GESTALT_TEXTURE_MEMORY: case GESTALT_FAST_TEXTURE_MEMORY: v = TEXTURE_MEMORY; break;
	case GESTALT_CONTEXT_TYPES: case GESTALT_CONTEXT_TYPES_PREFERRED: v = CONTEXT_TYPES; break;
	case GESTALT_TEXTURE_TYPES: case GESTALT_BITMAP_TYPES: v = IMAGE_TYPES; break;
	case GESTALT_TEXTURE_TYPES_PREFERRED: case GESTALT_BITMAP_TYPES_PREFERRED: v = CONTEXT_TYPES; break;
	case GESTALT_OPTIONAL2: v = 0; break;
	case GESTALT_MULTI_TEXTURE_MAX: v = 0; break;
	default: return QA_GESTALT_UNKNOWN;
	}
	if (!writable(m, response, 4))
		return QA_PARAM_ERR;
	mem_w32(m, response, v);
	return QA_NO_ERR;
}

/* Engine methods */
static void engine_call(struct rave *r, struct ndrv_call *c, unsigned method)
{
	struct mem *m = r->mem;
	const uint32_t *a = c->arg;
	uint32_t err = QA_NO_ERR;
	switch (method) {
	case EM_DRAW_PRIVATE_NEW:
		err = context_new(r, a[0], a[1], a[2], a[3], a[4]);
		if (err == QA_NO_ERR) {
			/* then the draw methods, one QARegisterDrawMethod() at a time */
			ndrv_set_state(c, 0, 1);
			ndrv_set_state(c, 1, a[0]);
			ndrv_set_state(c, 2, 0);
			ndrv_callout(c, ndrv_import(c, &r->layout, IMP_REGISTER_DRAW_METHOD), a[0], 0,
				     c->toc + r->layout.entry[E_DRAW], 0);
			return;
		}
		break;
	case EM_DRAW_PRIVATE_DELETE: {
		uint32_t k = a[0] - HANDLE_CONTEXT;
		if (k < RAVE_MAX_CONTEXTS) {
			free_context(&r->ctx[k]);
			free_doomed(r);
		}
		break;
	}
	case EM_CHECK_DEVICE:
		err = device_supported(r, a[0]) ? QA_NO_ERR : QA_NOT_SUPPORTED;
		break;
	case EM_GESTALT:
		err = gestalt(r, a[0], a[1]);
		break;
	case EM_TEXTURE_NEW:
		err = image_new(r, false, a[0], a[1], a[2], a[3]);
		break;
	case EM_BITMAP_NEW:
		/* (one image, not an array) */
		err = image_new(r, true, a[0], a[1], a[2], a[3]);
		break;
	case EM_TEXTURE_DETACH: case EM_BITMAP_DETACH:
		break;
	case EM_TEXTURE_DELETE: case EM_BITMAP_DELETE: {
		struct rave_image *im = image_of(r, a[0]);
		if (im)
			delete_image(r, im);
		break;
	}
	default:
		err = QA_NOT_SUPPORTED;
		break;
	}
	(void)m;
	ndrv_finish(c, err);
}

static void draw_call(struct rave *r, struct ndrv_call *c, unsigned method)
{
	struct mem *m = r->mem;
	const uint32_t *a = c->arg;
	struct rave_context *x = context_of(r, a[0]);
	uint32_t result = QA_NO_ERR;
	if (!x) {
		ndrv_finish(c, QA_PARAM_ERR);
		return;
	}
	switch (method) {
	case DM_SET_FLOAT: {
		/* (the float argument arrives in f1, as a double) */
		double d;
		memcpy(&d, &c->f1, sizeof d);
		if (a[1] < RAVE_TAGS)
			x->floats[a[1]] = (float)d;
		break;
	}
	case DM_SET_INT:
		if (a[1] < RAVE_TAGS)
			x->ints[a[1]] = a[2];
		else if (a[1] == TAG_ATI_MULTITEXTURE)
			x->ati_multitexture = a[2] != 0;
		break;
	case DM_SET_PTR:
		if (a[1] == TAG_TEXTURE)
			x->texture = a[2];
		else if (a[1] == TAG_ATI_TEXTURE2)
			x->texture2 = a[2];
		break;
	case DM_GET_FLOAT: {
		double d = a[1] < RAVE_TAGS ? x->floats[a[1]] : 0;
		memcpy(&c->f1_out, &d, sizeof d);
		c->set_f1 = true;
		break;
	}
	case DM_GET_INT:
		result = a[1] < RAVE_TAGS ? x->ints[a[1]] :
			 a[1] == TAG_ATI_HARDWARE && engine_current(r, c) ? ATI_HARDWARE_RAGE_128 :
			 a[1] == TAG_ATI_MULTITEXTURE ? x->ati_multitexture : 0;
		break;
	case DM_GET_PTR:
		result = a[1] == TAG_TEXTURE ? x->texture : a[1] == TAG_ATI_TEXTURE2 ? x->texture2 :
			 a[1] == TAG_ATI_EXTENSIONS ? ati_extensions_table(r, c) : 0;
		break;
	case DM_DRAW_POINT: case DM_DRAW_LINE: case DM_DRAW_TRI_GOURAUD: case DM_DRAW_TRI_TEXTURE: {
		bool textured = method == DM_DRAW_TRI_TEXTURE;
		unsigned n = method == DM_DRAW_POINT ? 1 : method == DM_DRAW_LINE ? 2 : 3;
		struct raster_vertex v[3];
		bool ok = true;
		for (unsigned k = 0; k < n && ok; k++)
			ok = read_vertices(m, a[1 + k], 1, textured, &v[k]);
		if (ok)
			draw_vertices(r, x, v, n, n == 1 ? 0 : n == 2 ? 1 : 3, textured);
		break;
	}
	case DM_DRAW_V_GOURAUD: case DM_DRAW_V_TEXTURE: {
		bool textured = method == DM_DRAW_V_TEXTURE;
		unsigned n = a[1];
		struct raster_vertex *v = n <= 0x100000u ? malloc((n ? n : 1) * sizeof *v) : NULL;
		if (v && read_vertices(m, a[3], n, textured, v))
			draw_vertices(r, x, v, n, a[2], textured && a[2] >= 3);
		free(v);
		break;
	}
	case DM_SUBMIT_V_GOURAUD: case DM_SUBMIT_V_TEXTURE: {
		bool textured = method == DM_SUBMIT_V_TEXTURE;
		unsigned n = a[1];
		if (n > 0x100000u)
			break;
		if (n > x->verts_cap) {
			struct raster_vertex *v = realloc(x->verts, n * sizeof *v);
			if (!v)
				break;
			x->verts = v;
			x->verts_cap = n;
		}
		x->nverts = read_vertices(m, a[2], n, textured, x->verts) ? n : 0;
		x->verts_textured = textured;
		break;
	}
	case DM_TRI_MESH_GOURAUD: case DM_TRI_MESH_TEXTURE: {
		bool textured = method == DM_TRI_MESH_TEXTURE && x->verts_textured;
		unsigned n = a[1];
		if (n > 0x100000u || !readable(m, a[2], n * 16))
			break;
		struct raster_state s;
		state_of(r, x, &s);
		for (unsigned k = 0; k < n; k++) {
			const uint8_t *t = mem_ptr(m, a[2] + 16 * k);
			uint32_t i0 = be32(t + 4), i1 = be32(t + 8), i2 = be32(t + 12);
			if (be32(t) & 1)
				r->backfacing++;
			if (i0 < x->nverts && i1 < x->nverts && i2 < x->nverts)
				triangle(x, &s, &x->verts[i0], &x->verts[i1], &x->verts[i2], textured);
		}
		r->triangles += n;
		break;
	}
	case DM_DRAW_BITMAP: {
		struct raster_vertex v;
		struct rave_image *im = image_of(r, a[2]);
		if (im && read_vertices(m, a[1], 1, false, &v)) {
			struct raster_state s;
			state_of(r, x, &s);
			struct raster_cmd *cmd = push_cmd(x, &s, RASTER_CMD_BITMAP);
			if (cmd) {
				cmd->v[0] = v;
				cmd->image = im->tex;
			}
		}
		break;
	}
	case DM_RENDER_START:
		result = render_start(r, x, a[2]);
		break;
	case DM_RENDER_END: case DM_SWAP_BUFFERS: {
		int l, t, rt, b;
		rect_of(m, a[1], x, &l, &t, &rt, &b);
		if (draw_frame(r, x, true))
			present(r, x, l, t, rt, b);
		x->frames++;
		r->frames++;
		break;
	}
	case DM_RENDER_ABORT:
		render_abort(r, x);
		break;
	case DM_FLUSH: case DM_SYNC:
		break;
	case DM_SET_NOTICE:
		if (a[1] < 5) {
			x->notice[a[1]][0] = a[2];
			x->notice[a[1]][1] = a[3];
		}
		break;
	case DM_GET_NOTICE:
		if (a[1] < 5 && writable(m, a[2], 4) && writable(m, a[3], 4)) {
			mem_w32(m, a[2], x->notice[a[1]][0]);
			mem_w32(m, a[3], x->notice[a[1]][1]);
		}
		break;
	case DM_CLEAR_DRAW_BUFFER: case DM_CLEAR_Z_BUFFER: {
		int l, t, rt, b;
		rect_of(m, a[1], x, &l, &t, &rt, &b);
		struct raster_state s;
		state_of(r, x, &s);
		uint32_t argb = 0;
		for (int k = 0; k < 4; k++) {
			float f = x->floats[TAG_COLOR_BG_A + k];
			argb = argb << 8 | (uint32_t)((f < 0 ? 0 : f > 1 ? 1 : f) * 255 + 0.5f);
		}
		struct raster_cmd *cmd = push_cmd(x, &s, RASTER_CMD_CLEAR);
		if (cmd) {
			cmd->rect[0] = l;
			cmd->rect[1] = t;
			cmd->rect[2] = rt;
			cmd->rect[3] = b;
			cmd->clear_color = method == DM_CLEAR_DRAW_BUFFER;
			cmd->clear_depth = method == DM_CLEAR_Z_BUFFER;
			cmd->argb = argb;
			cmd->z = raster_far(&s);
		}
		break;
	}
	case DM_BUSY:
		result = 0;
		break;
	default:
		result = QA_NOT_SUPPORTED;
		break;
	}
	ndrv_finish(c, result);
}

void rave_call(struct rave *r, struct ndrv_call *c)
{
	r->calls++;
	if (c->entry == NDRV_ENTRY_RESUME) {
		/* registering draw methods (state 0: 1), one per step */
		if (ndrv_state(c, 0) != 1) {
			ndrv_finish(c, QA_NO_ERR);
			return;
		}
		uint32_t next = ndrv_state(c, 2) + 1;
		if (next >= DM_COUNT) {
			ndrv_finish(c, QA_NO_ERR);
			return;
		}
		ndrv_set_state(c, 2, next);
		ndrv_callout(c, ndrv_import(c, &r->layout, IMP_REGISTER_DRAW_METHOD), ndrv_state(c, 1), next,
			     c->toc + r->layout.entry[E_DRAW + next], 0);
		return;
	}
	if (r->log && c->entry == E_DRAW + DM_SET_FLOAT) {
		double d;
		memcpy(&d, &c->f1, sizeof d);
		printf("rave: setFloat(%08x %u %g) from %08x\n", c->arg[0], c->arg[1], d, ndrv_caller(c));
	} else if (r->log && !(c->entry >= E_DRAW && c->entry - E_DRAW >= DM_DRAW_POINT &&
			c->entry - E_DRAW <= DM_TRI_MESH_TEXTURE && c->entry - E_DRAW != DM_RENDER_START &&
			c->entry - E_DRAW != DM_RENDER_END))
		printf("rave: %s(%08x %08x %08x %08x %08x) from %08x\n",
		       c->entry == E_INIT ? "init" : c->entry == E_GET_METHOD ? "getMethod" :
		       c->entry < E_DRAW ? engine_names[c->entry - E_ENGINE] :
		       c->entry < E_ATI_TEXTURE_UPDATE ? draw_names[c->entry - E_DRAW] :
		       c->entry == E_ATI_TEXTURE_UPDATE ? "atiTextureUpdate" : "?",
		       c->arg[0], c->arg[1], c->arg[2], c->arg[3], c->arg[4], ndrv_caller(c));

	if (c->entry == E_INIT) {
		/* the fragment's initialization: QARegisterEngine(our getMethod) */
		if (!engine_current(r, c)) {
			printf("3d: the guest's syshm 3D Engine is older than this syshm: install it again from the "
			       "syshm Tools volume (--tools) into Extensions, and restart\n");
			fflush(stdout);
		}
		ndrv_set_state(c, 0, 0);
		r->registered = true;
		ndrv_callout(c, ndrv_import(c, &r->layout, IMP_REGISTER_ENGINE), c->toc + r->layout.entry[E_GET_METHOD],
			     0, 0, 0);
		return;
	}
	if (c->entry == E_GET_METHOD) {
		uint32_t tag = c->arg[0], out = c->arg[1];
		bool known = tag <= EM_TEXTURE_DELETE || tag == EM_BITMAP_NEW || tag == EM_BITMAP_DETACH ||
			     tag == EM_BITMAP_DELETE;
		if (!known || !writable(r->mem, out, 4)) {
			ndrv_finish(c, QA_NOT_SUPPORTED);
			return;
		}
		mem_w32(r->mem, out, c->toc + r->layout.entry[E_ENGINE + tag]);
		ndrv_finish(c, QA_NO_ERR);
		return;
	}
	if (c->entry < E_DRAW)
		engine_call(r, c, c->entry - E_ENGINE);
	else if (c->entry < E_ATI_TEXTURE_UPDATE)
		draw_call(r, c, c->entry - E_DRAW);
	else if (c->entry == E_ATI_TEXTURE_UPDATE)
		ndrv_finish(c, ati_texture_update(r, c->arg[1], c->arg[2], c->arg[3]));
	else
		ndrv_finish(c, QA_NOT_SUPPORTED);
}
