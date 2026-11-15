#include "glfront.h"

/* (the same results on every host: no fused multiply-adds where some have them; GCC fuses none in ISO C) */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * Pixels between the program's memory and the GL: the pixel store's
 * layouts, images unpacked (glTexImage's, glDrawPixels', glBitmap's) and
 * packed (glReadPixels', glGetTexImage's), the pixel transfer (scales and
 * biases, index shifts and offsets, maps), and drawing at the raster
 * position: glBitmap, glDrawPixels and glCopyPixels, as images the backend
 * draws (RASTER_CMD_PIXELS: each pixel's fragment where OpenGL 1.2's 3.6.4
 * puts it, through the per-fragment operations; not textured or fogged).
 * Images are drawn from the raster position rounded to the nearest pixel
 * corner, as Mesa draws them (OpenGL 1.2 takes it as it is: the same pixels
 * at a zoom of 1, but where it's half way), so a zoomed image's pixels don't
 * depend on its fraction.
 *
 * Color components go through 8 bits each as they're unpacked (as
 * textures keep them), then the pixel transfer in floats, if it's not
 * the identity.
 */

enum { GL_ABGR_EXT = 0x8000 };
enum { GL_UNPACK_SWAP_BYTES = 0x0CF0, GL_UNPACK_LSB_FIRST, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS,
       GL_UNPACK_SKIP_PIXELS, GL_UNPACK_ALIGNMENT, GL_PACK_SWAP_BYTES = 0x0D00, GL_PACK_LSB_FIRST,
       GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_ALIGNMENT };
enum { GL_MAP_COLOR = 0x0D10, GL_MAP_STENCIL, GL_INDEX_SHIFT, GL_INDEX_OFFSET, GL_RED_SCALE, GL_RED_BIAS, GL_ZOOM_X,
       GL_ZOOM_Y, GL_GREEN_SCALE, GL_GREEN_BIAS, GL_BLUE_SCALE, GL_BLUE_BIAS, GL_ALPHA_SCALE, GL_ALPHA_BIAS,
       GL_DEPTH_SCALE, GL_DEPTH_BIAS };
enum { GL_PIXEL_MAP_I_TO_I = 0x0C70, GL_PIXEL_MAP_I_TO_I_SIZE = 0x0CB0 };

/* ---- the pixel store, the pixel transfer --------------------------------------------- */

void glf_pixel_store(struct gl_state *st, uint32_t pname, int32_t v)
{
	if (pname >= 0x806B && pname <= 0x806E) {       /* GL_PACK_SKIP_IMAGES ... GL_UNPACK_IMAGE_HEIGHT */
		struct gl_pixel_store *ps = pname < 0x806D ? &st->pack : &st->unpack;
		if (v < 0)
			glf_error(st, GL_INVALID_VALUE);
		else
			*(pname & 1 ? &ps->skip_images : &ps->image_height) = v;
		return;
	}
	bool pack = pname >= GL_PACK_SWAP_BYTES;
	struct gl_pixel_store *ps = pack ? &st->pack : &st->unpack;
	switch (pack ? pname - GL_PACK_SWAP_BYTES + GL_UNPACK_SWAP_BYTES : pname) {
	case GL_UNPACK_SWAP_BYTES: ps->swap_bytes = v != 0; return;
	case GL_UNPACK_LSB_FIRST: ps->lsb_first = v != 0; return;
	case GL_UNPACK_ROW_LENGTH:
	case GL_UNPACK_SKIP_ROWS:
	case GL_UNPACK_SKIP_PIXELS:
		if (v < 0) {
			glf_error(st, GL_INVALID_VALUE);
			return;
		}
		*(pname % 16 == 2 ? &ps->row_length : pname % 16 == 3 ? &ps->skip_rows : &ps->skip_pixels) = v;
		return;
	case GL_UNPACK_ALIGNMENT:
		if (v != 1 && v != 2 && v != 4 && v != 8)
			glf_error(st, GL_INVALID_VALUE);
		else
			ps->alignment = v;
		return;
	default:
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
}

void glf_pixels_init(struct gl_state *st)
{
	struct gl_raster_pos *r = &st->rpos;
	memset(r, 0, sizeof *r);
	r->win[3] = 1;
	r->valid = true;
	for (int k = 0; k < 4; k++)
		r->color[k] = 1;
	for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++)
		r->tex[u][3] = 1;
	struct gl_transfer *t = &st->transfer;
	memset(t, 0, sizeof *t);
	for (int k = 0; k < 4; k++)
		t->scale[k] = 1;
	t->depth_scale = 1;
	t->zoom[0] = t->zoom[1] = 1;
	for (unsigned k = 0; k < GLF_MAPS; k++) {
		st->maps[k].size = 1;
		st->maps[k].v[0] = 0;
	}
}

void glf_pixel_transfer(struct gl_state *st, uint32_t pname, float v)
{
	struct gl_transfer *t = &st->transfer;
	switch (pname) {
	case GL_MAP_COLOR: t->map_color = v != 0; return;
	case GL_MAP_STENCIL: t->map_stencil = v != 0; return;
	case GL_INDEX_SHIFT: t->index_shift = (int32_t)floorf(v + 0.5f); return;
	case GL_INDEX_OFFSET: t->index_offset = (int32_t)floorf(v + 0.5f); return;
	case GL_RED_SCALE: t->scale[0] = v; return;
	case GL_RED_BIAS: t->bias[0] = v; return;
	case GL_GREEN_SCALE: t->scale[1] = v; return;
	case GL_GREEN_BIAS: t->bias[1] = v; return;
	case GL_BLUE_SCALE: t->scale[2] = v; return;
	case GL_BLUE_BIAS: t->bias[2] = v; return;
	case GL_ALPHA_SCALE: t->scale[3] = v; return;
	case GL_ALPHA_BIAS: t->bias[3] = v; return;
	case GL_DEPTH_SCALE: t->depth_scale = v; return;
	case GL_DEPTH_BIAS: t->depth_bias = v; return;
	default: glf_error(st, GL_INVALID_ENUM); return;
	}
}

void glf_pixel_zoom(struct gl_state *st, float x, float y)
{
	st->transfer.zoom[0] = x;
	st->transfer.zoom[1] = y;
}

void glf_pixel_map(struct gl_state *st, uint32_t map, int32_t size, const float *v)
{
	if (map - GL_PIXEL_MAP_I_TO_I >= GLF_MAPS) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	unsigned k = map - GL_PIXEL_MAP_I_TO_I;
	bool index = k <= GLF_MAP_I_TO_A;               /* (maps from indices: power of two sizes) */
	if (size < 1 || size > GLF_PIXEL_MAP || (index && (size & (size - 1)))) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	struct gl_pixel_map *m = &st->maps[k];
	m->size = (unsigned)size;
	for (int32_t j = 0; j < size; j++)              /* (colors clamped; indices as they are) */
		m->v[j] = k <= GLF_MAP_S_TO_S ? v[j] : v[j] < 0 ? 0 : v[j] > 1 ? 1 : v[j];
}

unsigned glf_get_pixel_map(struct gl_state *st, uint32_t map, float *v)
{
	if (map - GL_PIXEL_MAP_I_TO_I >= GLF_MAPS) {
		glf_error(st, GL_INVALID_ENUM);
		return 0;
	}
	const struct gl_pixel_map *m = &st->maps[map - GL_PIXEL_MAP_I_TO_I];
	memcpy(v, m->v, m->size * sizeof *v);
	return m->size;
}

unsigned glf_get_pixels(struct gl_state *st, uint32_t pname, double *v)
{
	const struct gl_transfer *t = &st->transfer;
	const struct gl_raster_pos *r = &st->rpos;
	if (pname - GL_PIXEL_MAP_I_TO_I_SIZE < GLF_MAPS) {
		v[0] = st->maps[pname - GL_PIXEL_MAP_I_TO_I_SIZE].size;
		return 1;
	}
	switch (pname) {
	case GL_MAP_COLOR: v[0] = t->map_color; return 1;
	case GL_MAP_STENCIL: v[0] = t->map_stencil; return 1;
	case GL_INDEX_SHIFT: v[0] = t->index_shift; return 1;
	case GL_INDEX_OFFSET: v[0] = t->index_offset; return 1;
	case GL_RED_SCALE: v[0] = t->scale[0]; return 1;
	case GL_GREEN_SCALE: v[0] = t->scale[1]; return 1;
	case GL_BLUE_SCALE: v[0] = t->scale[2]; return 1;
	case GL_ALPHA_SCALE: v[0] = t->scale[3]; return 1;
	case GL_RED_BIAS: v[0] = t->bias[0]; return 1;
	case GL_GREEN_BIAS: v[0] = t->bias[1]; return 1;
	case GL_BLUE_BIAS: v[0] = t->bias[2]; return 1;
	case GL_ALPHA_BIAS: v[0] = t->bias[3]; return 1;
	case GL_ZOOM_X: v[0] = t->zoom[0]; return 1;
	case GL_ZOOM_Y: v[0] = t->zoom[1]; return 1;
	case GL_DEPTH_SCALE: v[0] = t->depth_scale; return 1;
	case GL_DEPTH_BIAS: v[0] = t->depth_bias; return 1;
	case 0x0D34: v[0] = GLF_PIXEL_MAP; return 1;            /* GL_MAX_PIXEL_MAP_TABLE */
	case 0x0B07: for (int k = 0; k < 4; k++) v[k] = r->win[k]; return 4;   /* GL_CURRENT_RASTER_POSITION */
	case 0x0B08: v[0] = r->valid; return 1;
	case 0x0B04: for (int k = 0; k < 4; k++) v[k] = r->color[k]; return 4; /* ..._COLOR */
	case 0x0B06: for (int k = 0; k < 4; k++) v[k] = r->tex[st->active_texture][k]; return 4;
	case 0x0B09: v[0] = r->distance; return 1;
	case 0x0B05: v[0] = 1; return 1;                        /* ..._INDEX */
	default: return 0;
	}
}

/* Whether the color transfer does anything */
static bool color_transfer(const struct gl_state *st)
{
	const struct gl_transfer *t = &st->transfer;
	for (int k = 0; k < 4; k++)
		if (t->scale[k] != 1 || t->bias[k] != 0)
			return true;
	return t->map_color;
}

static float clamp01(float v)
{
	return v > 0 ? (v < 1 ? v : 1) : 0;
}

static uint8_t to_byte(float v)
{
	return (uint8_t)(clamp01(v) * 255 + 0.5f);
}

/* A component looked up in a map (by v scaled to its size, as Mesa does) */
static float lookup(const struct gl_pixel_map *m, float v)
{
	int i = (int)floorf(clamp01(v) * (float)(m->size - 1) + 0.5f);
	return m->v[i];
}

/* R, G, B, A bytes through the color transfer: scale and bias, then the maps */
static void transfer_rgba(const struct gl_state *st, uint8_t c[4])
{
	const struct gl_transfer *t = &st->transfer;
	for (int k = 0; k < 4; k++) {
		float v = clamp01((float)c[k] / 255 * t->scale[k] + t->bias[k]);
		if (t->map_color)
			v = lookup(&st->maps[GLF_MAP_R_TO_R + k], v);
		c[k] = to_byte(v);
	}
}

/* An index shifted and offset; a color index then mapped (if MAP_COLOR), a stencil index (if MAP_STENCIL) */
static int64_t transfer_index(const struct gl_state *st, int64_t i, bool stencil)
{
	const struct gl_transfer *t = &st->transfer;
	int s = t->index_shift;
	i = s >= 0 ? (s < 32 ? i * ((int64_t)1 << s) : 0) : (s > -32 ? i >> -s : (i < 0 ? -1 : 0));
	i += t->index_offset;
	if (stencil ? t->map_stencil : t->map_color) {
		const struct gl_pixel_map *m = &st->maps[stencil ? GLF_MAP_S_TO_S : GLF_MAP_I_TO_I];
		i = (int64_t)floorf(m->v[(uint64_t)i & (m->size - 1)] + 0.5f);
	}
	return i;
}

/* A color index as R, G, B, A bytes, by the I_TO_ maps */
static void index_rgba(const struct gl_state *st, int64_t i, uint8_t c[4])
{
	for (int k = 0; k < 4; k++) {
		const struct gl_pixel_map *m = &st->maps[GLF_MAP_I_TO_R + k];
		c[k] = to_byte(m->v[(uint64_t)i & (m->size - 1)]);
	}
}

/* ---- layouts ------------------------------------------------------------------------- */

/* What a format is; where its components go: R, G, B, A, or L (R, G and B); how many there are (0: not a format) */
enum { K_COLOR, K_INDEX, K_STENCIL, K_DEPTH };
enum { C_R, C_G, C_B, C_A, C_L };
static unsigned format_layout(uint32_t format, uint8_t comp[4], unsigned *kind)
{
	static const struct {
		uint32_t format;
		uint8_t n, kind, comp[4];
	} formats[] = {
		{ GL_RED, 1, K_COLOR, { C_R } }, { GL_GREEN, 1, K_COLOR, { C_G } }, { GL_BLUE, 1, K_COLOR, { C_B } },
		{ GL_ALPHA, 1, K_COLOR, { C_A } }, { GL_RGB, 3, K_COLOR, { C_R, C_G, C_B } },
		{ GL_RGBA, 4, K_COLOR, { C_R, C_G, C_B, C_A } }, { GL_LUMINANCE, 1, K_COLOR, { C_L } },
		{ GL_LUMINANCE_ALPHA, 2, K_COLOR, { C_L, C_A } }, { GL_BGR, 3, K_COLOR, { C_B, C_G, C_R } },
		{ GL_BGRA, 4, K_COLOR, { C_B, C_G, C_R, C_A } }, { GL_ABGR_EXT, 4, K_COLOR, { C_A, C_B, C_G, C_R } },
		{ GL_COLOR_INDEX, 1, K_INDEX, { 0 } }, { GL_STENCIL_INDEX, 1, K_STENCIL, { 0 } },
		{ GL_DEPTH_COMPONENT, 1, K_DEPTH, { 0 } },
	};
	for (unsigned k = 0; k < sizeof formats / sizeof formats[0]; k++)
		if (formats[k].format == format) {
			memcpy(comp, formats[k].comp, 4);
			*kind = formats[k].kind;
			return formats[k].n;
		}
	return 0;
}

/* A type: bytes an element takes (packed: a pixel), and packed fields' widths, from the top bits (rev: the bottom) */
struct type_info {
	uint8_t bytes, fields;
	bool packed, rev, sign, real;
	uint8_t bits[4];
};

static bool type_info(uint32_t type, struct type_info *ti)
{
	static const struct {
		uint32_t type;
		struct type_info ti;
	} types[] = {
		{ GL_UNSIGNED_BYTE, { 1, 0, false, false, false, false, { 0 } } },
		{ GL_BYTE, { 1, 0, false, false, true, false, { 0 } } },
		{ GL_UNSIGNED_SHORT, { 2, 0, false, false, false, false, { 0 } } },
		{ GL_SHORT, { 2, 0, false, false, true, false, { 0 } } },
		{ GL_UNSIGNED_INT, { 4, 0, false, false, false, false, { 0 } } },
		{ GL_INT, { 4, 0, false, false, true, false, { 0 } } },
		{ GL_FLOAT, { 4, 0, false, false, false, true, { 0 } } },
		{ 0x8032, { 1, 3, true, false, false, false, { 3, 3, 2 } } },           /* 3_3_2 */
		{ 0x8362, { 1, 3, true, true, false, false, { 3, 3, 2 } } },            /* 2_3_3_REV */
		{ 0x8363, { 2, 3, true, false, false, false, { 5, 6, 5 } } },           /* 5_6_5 */
		{ 0x8364, { 2, 3, true, true, false, false, { 5, 6, 5 } } },
		{ 0x8033, { 2, 4, true, false, false, false, { 4, 4, 4, 4 } } },        /* 4_4_4_4 */
		{ 0x8365, { 2, 4, true, true, false, false, { 4, 4, 4, 4 } } },
		{ 0x8034, { 2, 4, true, false, false, false, { 5, 5, 5, 1 } } },        /* 5_5_5_1 */
		{ 0x8366, { 2, 4, true, true, false, false, { 5, 5, 5, 1 } } },         /* 1_5_5_5_REV */
		{ 0x8035, { 4, 4, true, false, false, false, { 8, 8, 8, 8 } } },        /* 8_8_8_8 */
		{ 0x8367, { 4, 4, true, true, false, false, { 8, 8, 8, 8 } } },
		{ 0x8036, { 4, 4, true, false, false, false, { 10, 10, 10, 2 } } },     /* 10_10_10_2 */
		{ 0x8368, { 4, 4, true, true, false, false, { 10, 10, 10, 2 } } },
	};
	for (unsigned k = 0; k < sizeof types / sizeof types[0]; k++)
		if (types[k].type == type) {
			*ti = types[k].ti;
			return true;
		}
	return false;
}

/*
 * An image's layout in the program's memory: its format's kind and
 * components, its type, bytes a pixel and a row, and where the first pixel
 * is (bitmaps: rows of bits, `skip` bits into the row first)
 */
struct layout {
	unsigned kind, n;
	uint8_t comp[4];
	struct type_info ti;
	bool bitmap;
	size_t pixel, row, start;
	unsigned skip;
};

static bool layout_of(const struct gl_pixel_store *ps, uint32_t format, uint32_t type, int32_t w, struct layout *L)
{
	memset(L, 0, sizeof *L);
	L->n = format_layout(format, L->comp, &L->kind);
	if (!L->n)
		return false;
	size_t length = ps->row_length > 0 ? (size_t)ps->row_length : (size_t)(w > 0 ? w : 0);
	size_t a = ps->alignment > 0 ? (size_t)ps->alignment : 4;
	if (type == GL_BITMAP) {
		if (L->kind != K_INDEX && L->kind != K_STENCIL)
			return false;
		L->bitmap = true;
		L->row = ((length + 7) / 8 + a - 1) / a * a;
		L->start = (size_t)ps->skip_rows * L->row + (size_t)ps->skip_pixels / 8;
		L->skip = (unsigned)ps->skip_pixels % 8;
		return true;
	}
	if (!type_info(type, &L->ti) || (L->ti.packed && (L->ti.fields != L->n || L->kind != K_COLOR)))
		return false;
	size_t elem = L->ti.bytes, group = L->ti.packed ? elem : elem * L->n;
	L->pixel = group;
	L->row = elem >= a ? group * length : (group * length + a - 1) / a * a;
	L->start = (size_t)ps->skip_rows * L->row + (size_t)ps->skip_pixels * group;
	return true;
}

uint32_t glf_pixel_check(uint32_t format, uint32_t type)
{
	uint8_t comp[4];
	unsigned kind;
	struct type_info ti;
	if (!format_layout(format, comp, &kind) || (type != GL_BITMAP && !type_info(type, &ti)))
		return GL_INVALID_ENUM;
	if (type == GL_BITMAP)
		return kind == K_INDEX || kind == K_STENCIL ? GL_NO_ERROR : GL_INVALID_ENUM;
	struct gl_pixel_store ps = { 0, 0, 0, 4, false, false, 0, 0 };
	struct layout L;
	return layout_of(&ps, format, type, 1, &L) ? GL_NO_ERROR : GL_INVALID_OPERATION;
}

size_t glf_image_bytes(const struct gl_pixel_store *ps, uint32_t format, uint32_t type, int32_t w, int32_t h)
{
	struct layout L;
	if (!layout_of(ps, format, type, w, &L))
		return 0;
	if (w <= 0 || h <= 0)
		return 1;
	if (L.bitmap)
		return L.start + (size_t)(h - 1) * L.row + (L.skip + (size_t)w + 7) / 8;
	return L.start + (size_t)(h - 1) * L.row + (size_t)w * L.pixel;
}

size_t glf_image_stride(const struct gl_pixel_store *ps, uint32_t format, uint32_t type, int32_t w, int32_t h)
{
	struct layout L;
	if (!layout_of(ps, format, type, w, &L))
		return 0;
	return L.row * (size_t)(ps->image_height > 0 ? ps->image_height : h > 0 ? h : 0);
}

size_t glf_image_bytes_3d(const struct gl_pixel_store *ps, uint32_t format, uint32_t type, int32_t w, int32_t h,
			  int32_t d)
{
	size_t one = glf_image_bytes(ps, format, type, w, h);
	if (!one || w <= 0 || h <= 0 || d <= 0)
		return one;
	return ((size_t)ps->skip_images + (size_t)d - 1) * glf_image_stride(ps, format, type, w, h) + one;
}

/* ---- unpacking --------------------------------------------------------------------- */

static uint32_t read_unit(const uint8_t *p, unsigned bytes, bool swap)
{
	uint32_t v = 0;
	for (unsigned k = 0; k < bytes; k++)
		v = v << 8 | p[swap ? bytes - 1 - k : k];
	return v;
}

static void write_unit(uint8_t *p, unsigned bytes, bool swap, uint32_t v)
{
	for (unsigned k = 0; k < bytes; k++)
		p[swap ? k : bytes - 1 - k] = (uint8_t)(v >> 8 * k);
}

/* A component (of a type, its bits as read) as 0..255 */
static uint8_t to8(const struct type_info *ti, uint32_t v)
{
	if (ti->real) {
		float f;
		memcpy(&f, &v, 4);
		return (uint8_t)(f > 0 ? (f < 1 ? f * 255 + 0.5f : 255) : 0);
	}
	unsigned bits = 8u * ti->bytes;
	if (ti->sign) {                         /* ((2c + 1) / (2^bits - 1), from below 0 clamped) */
		int64_t c = bits == 8 ? (int8_t)v : bits == 16 ? (int16_t)v : (int32_t)v;
		if (c < 0)
			return 0;
		uint64_t max = (1ull << bits) - 1;
		return (uint8_t)(((uint64_t)(2 * c + 1) * 255 + max / 2) / max);
	}
	uint64_t max = (1ull << bits) - 1;
	return (uint8_t)(((uint64_t)v * 255 + max / 2) / max);
}

static uint8_t field8(uint32_t v, unsigned bits)
{
	uint32_t max = (1u << bits) - 1;
	return (uint8_t)((v * 255 + max / 2) / max);
}

/* One pixel of a color image as R, G, B, A (0..255) */
static void unpack_pixel(const struct layout *L, const uint8_t *p, bool swap, uint8_t rgba[4])
{
	uint8_t c[4];
	if (L->ti.packed) {
		uint32_t v = read_unit(p, L->ti.bytes, swap);
		unsigned total = 8u * L->ti.bytes, at = 0;
		for (unsigned k = 0; k < L->n; k++) {
			unsigned b = L->ti.bits[k], shift = L->ti.rev ? at : total - at - b;
			c[k] = field8(v >> shift & ((1u << b) - 1), b);
			at += b;
		}
	} else {
		for (unsigned k = 0; k < L->n; k++)
			c[k] = to8(&L->ti, read_unit(p + k * L->ti.bytes, L->ti.bytes, swap));
	}
	rgba[0] = rgba[1] = rgba[2] = 0;
	rgba[3] = 255;
	for (unsigned k = 0; k < L->n; k++) {
		if (L->comp[k] == C_L)
			rgba[0] = rgba[1] = rgba[2] = c[k];
		else
			rgba[L->comp[k]] = c[k];
	}
}

/* Pixel i of a row of an index (or stencil index) image, as an integer */
static int64_t unpack_index(const struct layout *L, const uint8_t *row, int32_t i, const struct gl_pixel_store *ps)
{
	if (L->bitmap) {
		unsigned bit = L->skip + (unsigned)i;
		uint8_t byte = row[bit / 8];
		return ps->lsb_first ? byte >> (bit % 8) & 1 : byte >> (7 - bit % 8) & 1;
	}
	uint32_t v = read_unit(row + (size_t)i * L->pixel, L->ti.bytes, ps->swap_bytes);
	if (L->ti.real) {
		float f;
		memcpy(&f, &v, 4);
		return f > -2147483648.0f && f < 2147483648.0f ? (int64_t)floorf(f + 0.5f) : 0;
	}
	if (L->ti.sign)
		return L->ti.bytes == 1 ? (int8_t)v : L->ti.bytes == 2 ? (int16_t)v : (int32_t)v;
	return v;
}

/* Pixel i of a row of a depth image, 0..1 (before the depth transfer) */
static float unpack_depth(const struct layout *L, const uint8_t *row, int32_t i, bool swap)
{
	uint32_t v = read_unit(row + (size_t)i * L->pixel, L->ti.bytes, swap);
	if (L->ti.real) {
		float f;
		memcpy(&f, &v, 4);
		return f;
	}
	unsigned bits = 8u * L->ti.bytes;
	double max = (double)((1ull << bits) - 1);
	if (L->ti.sign) {
		int64_t c = bits == 8 ? (int8_t)v : bits == 16 ? (int16_t)v : (int32_t)v;
		return (float)((2.0 * (double)c + 1) / max);
	}
	return (float)((double)v / max);
}

bool glf_unpack_rgba(const struct gl_state *st, const struct gl_pixel_store *ps, uint32_t format, uint32_t type,
		     int32_t w, int32_t h, const uint8_t *src, uint8_t *rgba, bool transfer)
{
	struct layout L;
	if (!layout_of(ps, format, type, w, &L) || (L.kind != K_COLOR && L.kind != K_INDEX))
		return false;
	transfer = transfer && L.kind == K_COLOR && color_transfer(st);
	for (int32_t j = 0; j < h; j++) {
		const uint8_t *row = src + L.start + (size_t)j * L.row;
		uint8_t *out = rgba + (size_t)j * (size_t)w * 4;
		for (int32_t i = 0; i < w; i++, out += 4) {
			if (L.kind == K_INDEX) {
				index_rgba(st, transfer_index(st, unpack_index(&L, row, i, ps), false), out);
				continue;
			}
			unpack_pixel(&L, row + (size_t)i * L.pixel, ps->swap_bytes, out);
			if (transfer)
				transfer_rgba(st, out);
		}
	}
	return true;
}

/* ---- packing ----------------------------------------------------------------------- */

static uint32_t from8(const struct type_info *ti, uint8_t c, unsigned bits)
{
	if (ti->packed) {
		uint32_t max = (1u << bits) - 1;
		return (c * max + 127) / 255;
	}
	if (ti->real) {
		float f = c / 255.0f;
		uint32_t v;
		memcpy(&v, &f, 4);
		return v;
	}
	unsigned b = 8u * ti->bytes;
	uint64_t max = ti->sign ? (1ull << (b - 1)) - 1 : (1ull << b) - 1;
	return (uint32_t)(((uint64_t)c * max + 127) / 255);
}

/* R, G, B, A bytes into a pixel of a color layout (luminance: R + G + B, clamped) */
static void pack_pixel(const struct layout *L, uint8_t *p, bool swap, const uint8_t c[4])
{
	uint32_t v[4];
	for (unsigned k = 0; k < L->n; k++) {
		unsigned comp = L->comp[k];
		uint8_t value = comp == C_L ? (uint8_t)(c[0] + c[1] + c[2] > 255 ? 255 : c[0] + c[1] + c[2]) : c[comp];
		v[k] = from8(&L->ti, value, L->ti.packed ? L->ti.bits[k] : 0);
	}
	if (L->ti.packed) {
		uint32_t word = 0;
		unsigned total = 8u * L->ti.bytes, at = 0;
		for (unsigned k = 0; k < L->n; k++) {
			unsigned b = L->ti.bits[k], shift = L->ti.rev ? at : total - at - b;
			word |= v[k] << shift;
			at += b;
		}
		write_unit(p, L->ti.bytes, swap, word);
	} else {
		for (unsigned k = 0; k < L->n; k++)
			write_unit(p + k * L->ti.bytes, L->ti.bytes, swap, v[k]);
	}
}

/* An integer (a stencil index) into pixel i of a row (bitmaps: its lowest bit) */
static void pack_index(const struct layout *L, uint8_t *row, int32_t i, const struct gl_pixel_store *ps, int64_t v)
{
	if (L->bitmap) {
		unsigned bit = L->skip + (unsigned)i;
		uint8_t m = (uint8_t)(ps->lsb_first ? 1u << (bit % 8) : 0x80u >> (bit % 8));
		row[bit / 8] = (uint8_t)(v & 1 ? row[bit / 8] | m : row[bit / 8] & ~m);
		return;
	}
	uint32_t w;
	if (L->ti.real) {
		float f = (float)v;
		memcpy(&w, &f, 4);
	} else {
		w = (uint32_t)v;
	}
	write_unit(row + (size_t)i * L->pixel, L->ti.bytes, ps->swap_bytes, w);
}

/* A depth (0..1) into pixel i of a row */
static void pack_depth(const struct layout *L, uint8_t *row, int32_t i, bool swap, float d)
{
	uint32_t w;
	if (L->ti.real) {
		memcpy(&w, &d, 4);
	} else {
		unsigned bits = 8u * L->ti.bytes;
		double max = (double)((1ull << bits) - 1), c = clamp01(d);
		w = L->ti.sign ? (uint32_t)(int32_t)floor((max * c - 1) / 2 + 0.5) : (uint32_t)floor(max * c + 0.5);
	}
	write_unit(row + (size_t)i * L->pixel, L->ti.bytes, swap, w);
}

void glf_pack_rgba(const struct gl_state *st, uint32_t format, uint32_t type, int32_t w, int32_t h,
		   const uint8_t *rgba, uint8_t *out)
{
	struct layout L;
	if (!layout_of(&st->pack, format, type, w, &L) || L.kind != K_COLOR)
		return;
	for (int32_t j = 0; j < h; j++) {
		uint8_t *p = out + L.start + (size_t)j * L.row;
		for (int32_t i = 0; i < w; i++, p += L.pixel)
			pack_pixel(&L, p, st->pack.swap_bytes, rgba + ((size_t)j * (size_t)w + (size_t)i) * 4);
	}
}

/* ---- the drawable's pixels ------------------------------------------------------------- */

/* The target's pixel at (x, y) (OpenGL's: from the bottom), or false if it's outside */
static bool at(const struct gl_target *t, int64_t x, int64_t y, size_t *i)
{
	if (!t->surface || x < 0 || x >= t->width || y < 0 || y >= t->height)
		return false;
	*i = (size_t)(t->height - 1 - y) * (size_t)t->width + (size_t)x;
	return true;
}

/* A drawable's color as R, G, B, A (alpha 1 without alpha bits) */
static void color_at(const struct gl_target *t, size_t i, uint8_t c[4])
{
	uint32_t argb = t->surface->target.color[i];
	c[0] = (uint8_t)(argb >> 16);
	c[1] = (uint8_t)(argb >> 8);
	c[2] = (uint8_t)argb;
	c[3] = (uint8_t)(t->alpha_bits ? argb >> 24 : 255);
}

void glf_read_rgba(const struct gl_state *st, const struct gl_target *t, int32_t x, int32_t y, int32_t w, int32_t h,
		   uint8_t *rgba)
{
	bool transfer = color_transfer(st);
	for (int32_t j = 0; j < h; j++)
		for (int32_t i = 0; i < w; i++) {
			uint8_t *c = rgba + ((size_t)j * (size_t)w + (size_t)i) * 4;
			size_t k;
			if (!at(t, (int64_t)x + i, (int64_t)y + j, &k)) {
				memset(c, 0, 4);        /* (outside the drawable: undefined) */
				continue;
			}
			color_at(t, k, c);
			if (transfer)
				transfer_rgba(st, c);
		}
}

void glf_read_pixels(struct gl_state *st, const struct gl_target *t, int32_t x, int32_t y, int32_t w, int32_t h,
		     uint32_t format, uint32_t type, uint8_t *out)
{
	struct layout L;
	uint32_t e = glf_pixel_check(format, type);
	if (e) {
		glf_error(st, e);
		return;
	}
	if (w < 0 || h < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	layout_of(&st->pack, format, type, w, &L);
	if (L.kind == K_INDEX || (L.kind == K_DEPTH && !t->depth_bits) || (L.kind == K_STENCIL && !t->stencil_bits)) {
		glf_error(st, GL_INVALID_OPERATION);
		return;
	}
	bool transfer = L.kind == K_COLOR && color_transfer(st);
	const struct raster_target *rt = t->surface ? &t->surface->target : NULL;
	for (int32_t j = 0; j < h; j++) {
		uint8_t *row = out + L.start + (size_t)j * L.row;
		for (int32_t i = 0; i < w; i++) {
			size_t k;
			if (!at(t, (int64_t)x + i, (int64_t)y + j, &k))
				continue;               /* (outside the drawable: undefined, left alone) */
			if (L.kind == K_DEPTH) {
				const struct gl_transfer *tr = &st->transfer;
				pack_depth(&L, row, i, st->pack.swap_bytes, rt->depth[k] * tr->depth_scale + tr->depth_bias);
			} else if (L.kind == K_STENCIL) {
				pack_index(&L, row, i, &st->pack, transfer_index(st, rt->stencil ? rt->stencil[k] : 0, true));
			} else {
				uint8_t c[4];
				color_at(t, k, c);
				if (transfer)
					transfer_rgba(st, c);
				pack_pixel(&L, row + (size_t)i * L.pixel, st->pack.swap_bytes, c);
			}
		}
	}
}

/* ---- drawing at the raster position ---------------------------------------------------- */

/*
 * An image (w x h texels as RASTER_PIXELS_ kind has them, from malloc(): the
 * backend's to free, once drawn) drawn with its row 0 at the bottom, its
 * corner at window (x, y), each texel zoom pixels wide and high
 */
static void emit(struct gl_state *st, const struct gl_target *t, unsigned kind, uint32_t *texels, int32_t w,
		 int32_t h, float x, float y, float zx, float zy)
{
	struct raster_texture *image = calloc(1, sizeof *image);
	if (!image) {
		free(texels);
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	image->levels = 1;
	image->width[0] = (unsigned)w;
	image->height[0] = (unsigned)h;
	image->texels[0] = texels;
	glf_retire(st->names, image);           /* (freed once nothing recorded can draw it) */
	struct raster_state s = *glf_raster_state(st, t);
	s.stencil_write = (uint8_t)st->stencil_write_mask;     /* (stencil images write it, whatever the test) */
	struct raster_cmd *c = surface_cmd(t->surface, &s, RASTER_CMD_PIXELS);
	if (!c) {
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	c->pixels = (uint8_t)kind;
	c->image = image;
	c->v[0].x = x;                          /* (raster.h's coordinates: y down) */
	c->v[0].y = (float)t->height - y;
	c->v[0].z = st->rpos.win[2];
	c->v[0].invw = 1;
	memcpy(c->v[0].color[0], st->rpos.color, sizeof st->rpos.color);
	c->v[1].x = zx;
	c->v[1].y = -zy;
}

/* Whether drawing at the raster position does anything */
static bool drawable(struct gl_state *st, const struct gl_target *t, int32_t w, int32_t h)
{
	return st->rpos.valid && t->surface && t->width > 0 && t->height > 0 && w > 0 && h > 0 &&
	       st->transfer.zoom[0] != 0 && st->transfer.zoom[1] != 0;
}

void glf_bitmap(struct gl_state *st, const struct gl_target *t, int32_t w, int32_t h, float xorig, float yorig,
		float xmove, float ymove, const uint8_t *bits)
{
	if (w < 0 || h < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	if (!st->rpos.valid)
		return;
	struct gl_raster_pos *r = &st->rpos;
	if (bits && t->surface && t->width > 0 && t->height > 0 && w > 0 && h > 0 && (size_t)w * (size_t)h <= (1u << 24)) {
		struct layout L;
		layout_of(&st->unpack, GL_COLOR_INDEX, GL_BITMAP, w, &L);
		uint32_t *texels = malloc((size_t)w * (size_t)h * 4);
		if (!texels) {
			glf_error(st, GL_OUT_OF_MEMORY);
			return;
		}
		bool any = false;
		for (int32_t j = 0; j < h; j++)
			for (int32_t i = 0; i < w; i++) {
				uint32_t v = (uint32_t)unpack_index(&L, bits + L.start + (size_t)j * L.row, i, &st->unpack);
				texels[(size_t)j * (size_t)w + (size_t)i] = v;
				any |= v != 0;
			}
		/* (its bottom left pixel: floor(raster position - origin), as OpenGL 1.2's 3.7 has it) */
		if (any)
			emit(st, t, RASTER_PIXELS_MASK, texels, w, h, floorf(r->win[0] - xorig), floorf(r->win[1] - yorig),
			     1, 1);
		else
			free(texels);
	}
	r->win[0] += xmove;
	r->win[1] += ymove;
}

void glf_draw_pixels(struct gl_state *st, const struct gl_target *t, int32_t w, int32_t h, uint32_t format,
		     uint32_t type, const uint8_t *pixels)
{
	uint32_t e = glf_pixel_check(format, type);
	if (w < 0 || h < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	if (e) {
		glf_error(st, e);
		return;
	}
	struct layout L;
	layout_of(&st->unpack, format, type, w, &L);
	if ((L.kind == K_DEPTH && !t->depth_bits) || (L.kind == K_STENCIL && !t->stencil_bits)) {
		glf_error(st, GL_INVALID_OPERATION);
		return;
	}
	if (!pixels || !drawable(st, t, w, h) || (size_t)w * (size_t)h > (1u << 24))
		return;
	uint32_t *texels = malloc((size_t)w * (size_t)h * 4);
	if (!texels) {
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	unsigned kind = RASTER_PIXELS_COLOR;
	if (L.kind == K_COLOR || L.kind == K_INDEX) {
		uint8_t *rgba = (uint8_t *)texels;
		glf_unpack_rgba(st, &st->unpack, format, type, w, h, pixels, rgba, true);
		for (size_t k = 0; k < (size_t)w * (size_t)h; k++) {
			const uint8_t *c = rgba + 4 * k;
			texels[k] = (uint32_t)c[3] << 24 | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
		}
	} else {
		const struct gl_transfer *tr = &st->transfer;
		kind = L.kind == K_DEPTH ? RASTER_PIXELS_DEPTH : RASTER_PIXELS_STENCIL;
		for (int32_t j = 0; j < h; j++) {
			const uint8_t *row = pixels + L.start + (size_t)j * L.row;
			for (int32_t i = 0; i < w; i++) {
				uint32_t v;
				if (L.kind == K_DEPTH) {
					float d = clamp01(unpack_depth(&L, row, i, st->unpack.swap_bytes) * tr->depth_scale +
							  tr->depth_bias);
					memcpy(&v, &d, 4);
				} else {
					v = (uint32_t)transfer_index(st, unpack_index(&L, row, i, &st->unpack), true) & 0xFF;
				}
				texels[(size_t)j * (size_t)w + (size_t)i] = v;
			}
		}
	}
	emit(st, t, kind, texels, w, h, floorf(st->rpos.win[0] + 0.5f), floorf(st->rpos.win[1] + 0.5f),
	     st->transfer.zoom[0], st->transfer.zoom[1]);
}

void glf_copy_pixels(struct gl_state *st, const struct gl_target *t, int32_t x, int32_t y, int32_t w, int32_t h,
		     uint32_t type)
{
	if (type != GL_COLOR && type != GL_DEPTH && type != GL_STENCIL) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	if (w < 0 || h < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	if ((type == GL_DEPTH && !t->depth_bits) || (type == GL_STENCIL && !t->stencil_bits)) {
		glf_error(st, GL_INVALID_OPERATION);
		return;
	}
	if (!drawable(st, t, w, h) || (size_t)w * (size_t)h > (1u << 24))
		return;
	uint32_t *texels = malloc((size_t)w * (size_t)h * 4);
	if (!texels) {
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	const struct raster_target *rt = &t->surface->target;
	const struct gl_transfer *tr = &st->transfer;
	bool transfer = color_transfer(st);
	for (int32_t j = 0; j < h; j++)
		for (int32_t i = 0; i < w; i++) {
			uint32_t *out = &texels[(size_t)j * (size_t)w + (size_t)i];
			size_t k;
			bool in = at(t, (int64_t)x + i, (int64_t)y + j, &k);
			if (type == GL_COLOR) {
				uint8_t c[4] = { 0, 0, 0, 0 };
				if (in)
					color_at(t, k, c);
				if (transfer)
					transfer_rgba(st, c);
				*out = (uint32_t)c[3] << 24 | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
			} else if (type == GL_DEPTH) {
				float d = clamp01((in ? rt->depth[k] : 0) * tr->depth_scale + tr->depth_bias);
				memcpy(out, &d, 4);
			} else {
				*out = (uint32_t)transfer_index(st, in && rt->stencil ? rt->stencil[k] : 0, true) & 0xFF;
			}
		}
	unsigned kind = type == GL_COLOR ? RASTER_PIXELS_COLOR : type == GL_DEPTH ? RASTER_PIXELS_DEPTH :
		RASTER_PIXELS_STENCIL;
	emit(st, t, kind, texels, w, h, floorf(st->rpos.win[0] + 0.5f), floorf(st->rpos.win[1] + 0.5f), tr->zoom[0],
	     tr->zoom[1]);
}
