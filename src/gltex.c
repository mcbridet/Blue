#include "glfront.h"

#include <stdlib.h>
#include <string.h>

#include "be.h"

/*
 * A level's texels are 0xAARRGGBB, whatever the base format: what the
 * texture environment takes of them (glfront.c's stages) is there.
 *
 *   GL_ALPHA            (A, 0, 0, 0)        GL_RGB      (1, R, G, B)
 *   GL_LUMINANCE        (1, L, L, L)        GL_RGBA     (A, R, G, B)
 *   GL_LUMINANCE_ALPHA  (A, L, L, L)        GL_INTENSITY (I, I, I, I)
 */

enum { GL_TEXTURE_1D_TARGET = 0x0DE0, GL_TEXTURE_2D_TARGET = 0x0DE1, GL_PROXY_TEXTURE_1D = 0x8063,
       GL_PROXY_TEXTURE_2D = 0x8064 };

/* ---- namespaces ---------------------------------------------------------------- */

static void texture_init(struct gl_texture *t, uint32_t name)
{
	memset(t, 0, sizeof *t);
	t->name = name;
	t->target = -1;
	t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
	t->mag_filter = GL_LINEAR;
	t->wrap_s = t->wrap_t = GL_REPEAT;
	t->priority = 1;
	t->min_lod = -1000;
	t->max_lod = 1000;
	t->max_level = 1000;
}

static void retire(struct gl_names *n, struct raster_texture *r)
{
	if (!r)
		return;
	if (n->nretired == n->retired_cap) {
		unsigned cap = n->retired_cap ? 2 * n->retired_cap : 16;
		struct raster_texture **p = realloc(n->retired, cap * sizeof *p);
		if (!p)
			return;                 /* (leaks rather than breaks) */
		n->retired = p;
		n->retired_cap = cap;
	}
	n->retired[n->nretired++] = r;
}

void glf_free_retired(struct gl_names *n, struct gpu *gpu)
{
	for (unsigned k = 0; k < n->nretired; k++) {
		struct raster_texture *r = n->retired[k];
		if (r->gpu && gpu)
			gpu_texture_free(gpu, r);
		for (unsigned l = 0; l < r->levels; l++)
			free(r->texels[l]);
		free(r);
	}
	n->nretired = 0;
}

/* Whether a level's texels are the drawn texture's too (which then frees them) */
static bool shared(const struct gl_texture *t, const uint32_t *texels)
{
	for (unsigned k = 0; t->drawn && texels && k < t->drawn->levels; k++)
		if (t->drawn->texels[k] == texels)
			return true;
	return false;
}

/* The texture is about to change: its texels its own again (copied from the drawn texture's), which is retired */
static bool changing(struct gl_names *n, struct gl_texture *t)
{
	t->serial++;
	if (!t->drawn)
		return true;
	for (unsigned k = 0; k < GLF_TEXTURE_LEVELS; k++) {
		struct gl_level *l = &t->level[k];
		if (!shared(t, l->texels))
			continue;
		size_t bytes = (size_t)l->width * (size_t)l->height * 4;
		uint32_t *copy = malloc(bytes);
		if (!copy)
			return false;
		memcpy(copy, l->texels, bytes);
		l->texels = copy;
	}
	retire(n, t->drawn);
	t->drawn = NULL;
	return true;
}

/* A texture's texels freed (or retired, if they're drawn) */
static void free_texture(struct gl_names *n, struct gl_texture *t)
{
	for (unsigned k = 0; k < GLF_TEXTURE_LEVELS; k++)
		if (!shared(t, t->level[k].texels))
			free(t->level[k].texels);
	retire(n, t->drawn);
	t->drawn = NULL;
}

void glf_textures_init(struct gl_state *st)
{
	for (int k = 0; k < GLF_TARGETS; k++) {
		texture_init(&st->defaults[k], 0);
		st->defaults[k].target = k;
	}
	for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++) {
		for (int k = 0; k < GLF_TARGETS; k++)
			st->units[u].bound[k] = &st->defaults[k];
		st->units[u].env_mode = GL_MODULATE;
	}
	st->pack.alignment = st->unpack.alignment = 4;
}

bool glf_attach(struct gl_state *st, struct gl_state *share)
{
	struct gl_names *n = share ? share->names : calloc(1, sizeof *n);
	if (!n)
		return false;
	n->users++;
	if (!share)
		n->next_name = 1;
	st->names = n;
	return true;
}

void glf_detach(struct gl_state *st, struct gpu *gpu)
{
	struct gl_names *n = st->names;
	if (!n)
		return;
	for (int k = 0; k < GLF_TARGETS; k++)
		free_texture(n, &st->defaults[k]);
	st->names = NULL;
	if (--n->users)
		return;
	for (unsigned c = 0; c < GLF_NAME_CHAINS; c++)
		while (n->chain[c]) {
			struct gl_texture *t = n->chain[c];
			n->chain[c] = t->next;
			free_texture(n, t);
			free(t);
		}
	glf_free_retired(n, gpu);
	free(n->retired);
	free(n);
}

void glf_copy(struct gl_state *to, const struct gl_state *from)
{
	struct gl_names *names = to->names;
	struct gl_texture defaults[GLF_TARGETS];
	memcpy(defaults, to->defaults, sizeof defaults);
	*to = *from;
	to->names = names;
	memcpy(to->defaults, defaults, sizeof defaults);
	for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++)
		for (int k = 0; k < GLF_TARGETS; k++)
			if (from->units[u].bound[k] == &from->defaults[k] || names != from->names)
				to->units[u].bound[k] = &to->defaults[k];
	to->raster_stale = true;
}

static struct gl_texture *find(struct gl_names *n, uint32_t name)
{
	for (struct gl_texture *t = n->chain[name % GLF_NAME_CHAINS]; t; t = t->next)
		if (t->name == name)
			return t;
	return NULL;
}

void glf_gen_textures(struct gl_state *st, int32_t count, uint32_t *out)
{
	if (count < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	struct gl_names *n = st->names;
	for (int32_t k = 0; k < count; k++) {
		while (!n->next_name || find(n, n->next_name))
			n->next_name++;
		out[k] = n->next_name++;
	}
}

void glf_unbind(struct gl_state *st, const struct gl_texture *t)
{
	for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++)
		for (int k = 0; k < GLF_TARGETS; k++)
			if (st->units[u].bound[k] == t) {
				st->units[u].bound[k] = &st->defaults[k];
				st->raster_stale = true;
			}
}

void glf_delete_texture(struct gl_state *st, uint32_t name)
{
	struct gl_names *n = st->names;
	struct gl_texture **p = &n->chain[name % GLF_NAME_CHAINS];
	while (*p && (*p)->name != name)
		p = &(*p)->next;
	if (!name || !*p)
		return;
	struct gl_texture *t = *p;
	*p = t->next;
	glf_unbind(st, t);
	free_texture(n, t);
	free(t);
}

const struct gl_texture *glf_texture(struct gl_state *st, uint32_t name)
{
	return name ? find(st->names, name) : NULL;
}

bool glf_is_texture(struct gl_state *st, uint32_t name)
{
	const struct gl_texture *t = name ? find(st->names, name) : NULL;
	return t && t->target >= 0;
}

static int target_of(uint32_t target)
{
	return target == GL_TEXTURE_2D_TARGET ? GLF_2D : target == GL_TEXTURE_1D_TARGET ? GLF_1D : -1;
}

void glf_bind_texture(struct gl_state *st, uint32_t target, uint32_t name)
{
	int k = target_of(target);
	if (k < 0) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	struct gl_texture *t = name ? find(st->names, name) : &st->defaults[k];
	if (!t) {                               /* (a name not generated is taken, as OpenGL 1.2 has it) */
		t = malloc(sizeof *t);
		if (!t) {
			glf_error(st, GL_OUT_OF_MEMORY);
			return;
		}
		texture_init(t, name);
		t->next = st->names->chain[name % GLF_NAME_CHAINS];
		st->names->chain[name % GLF_NAME_CHAINS] = t;
	}
	if (t->target >= 0 && t->target != k) {
		glf_error(st, GL_INVALID_OPERATION);
		return;
	}
	t->target = k;
	st->units[st->active_texture].bound[k] = t;
	st->raster_stale = true;
}

/* ---- pixel transfer -------------------------------------------------------------- */

enum { GL_COLOR_INDEX = 0x1900, GL_RED = 0x1903, GL_GREEN, GL_BLUE, GL_ABGR_EXT = 0x8000 };
enum { GL_BYTE = 0x1400, GL_SHORT = 0x1402, GL_UNSIGNED_SHORT, GL_INT, GL_UNSIGNED_INT, GL_FLOAT };
enum { GL_UNPACK_SWAP_BYTES = 0x0CF0, GL_UNPACK_LSB_FIRST, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS,
       GL_UNPACK_SKIP_PIXELS, GL_UNPACK_ALIGNMENT, GL_PACK_SWAP_BYTES = 0x0D00, GL_PACK_LSB_FIRST,
       GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_ALIGNMENT };

void glf_pixel_store(struct gl_state *st, uint32_t pname, int32_t v)
{
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

/* Where a format's components go: R, G, B, A, or L (R, G and B); how many there are (0: not a format) */
enum { C_R, C_G, C_B, C_A, C_L };
static unsigned format_layout(uint32_t format, uint8_t comp[4])
{
	static const struct {
		uint32_t format;
		uint8_t n, comp[4];
	} formats[] = {
		{ GL_RED, 1, { C_R } }, { GL_GREEN, 1, { C_G } }, { GL_BLUE, 1, { C_B } }, { GL_ALPHA, 1, { C_A } },
		{ GL_RGB, 3, { C_R, C_G, C_B } }, { GL_RGBA, 4, { C_R, C_G, C_B, C_A } }, { GL_LUMINANCE, 1, { C_L } },
		{ GL_LUMINANCE_ALPHA, 2, { C_L, C_A } }, { GL_BGR, 3, { C_B, C_G, C_R } },
		{ GL_BGRA, 4, { C_B, C_G, C_R, C_A } }, { GL_ABGR_EXT, 4, { C_A, C_B, C_G, C_R } },
	};
	for (unsigned k = 0; k < sizeof formats / sizeof formats[0]; k++)
		if (formats[k].format == format) {
			memcpy(comp, formats[k].comp, 4);
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

/* An image's layout in the program's memory: bytes a pixel and a row, and where the first pixel is */
struct layout {
	uint8_t comp[4];
	unsigned n;
	struct type_info ti;
	size_t pixel, row, start;
};

static bool layout_of(const struct gl_pixel_store *ps, uint32_t format, uint32_t type, int32_t w, struct layout *L)
{
	L->n = format_layout(format, L->comp);
	if (!L->n || !type_info(type, &L->ti) || (L->ti.packed && L->ti.fields != L->n))
		return false;
	size_t elem = L->ti.bytes, group = L->ti.packed ? elem : elem * L->n;
	size_t length = ps->row_length > 0 ? (size_t)ps->row_length : (size_t)(w > 0 ? w : 0);
	size_t a = ps->alignment > 0 ? (size_t)ps->alignment : 4;
	L->pixel = group;
	L->row = elem >= a ? group * length : (group * length + a - 1) / a * a;
	L->start = (size_t)ps->skip_rows * L->row + (size_t)ps->skip_pixels * group;
	return true;
}

size_t glf_image_bytes(const struct gl_pixel_store *ps, uint32_t format, uint32_t type, int32_t w, int32_t h)
{
	struct layout L;
	if (!layout_of(ps, format, type, w, &L))
		return 0;
	if (w <= 0 || h <= 0)
		return 1;
	return L.start + (size_t)(h - 1) * L.row + (size_t)w * L.pixel;
}

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

/* One pixel of an image as R, G, B, A (0..255) */
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

/* R, G, B, A as a texel of a base format (see the top) */
static uint32_t texel(uint32_t base, const uint8_t c[4])
{
	uint32_t r = c[0], g = c[1], b = c[2], a = c[3];
	switch (base) {
	case GL_ALPHA: return a << 24;
	case GL_LUMINANCE: return 0xFF000000u | r << 16 | r << 8 | r;
	case GL_LUMINANCE_ALPHA: return a << 24 | r << 16 | r << 8 | r;
	case GL_INTENSITY: return r << 24 | r << 16 | r << 8 | r;
	case GL_RGB: return 0xFF000000u | r << 16 | g << 8 | b;
	default: return a << 24 | r << 16 | g << 8 | b;
	}
}

/* An image (w x h, from `src` as unpacked) into texels [x, x + w) x [y, y + h) of a level `stride` wide */
static void unpack_image(const struct gl_pixel_store *ps, const struct layout *L, const uint8_t *src, int32_t w,
			 int32_t h, uint32_t base, uint32_t *texels, int32_t stride, int32_t x, int32_t y)
{
	for (int32_t j = 0; j < h; j++) {
		const uint8_t *p = src + L->start + (size_t)j * L->row;
		uint32_t *out = texels + (size_t)(y + j) * (size_t)stride + (size_t)x;
		for (int32_t i = 0; i < w; i++, p += L->pixel) {
			uint8_t c[4];
			unpack_pixel(L, p, ps->swap_bytes, c);
			out[i] = texel(base, c);
		}
	}
}

/* ---- images ---------------------------------------------------------------------- */

/* An internal format's base format, or 0 */
static uint32_t base_format(int32_t internal)
{
	static const struct {
		int32_t first, last;
		uint32_t base;
	} ranges[] = {
		{ 1, 1, GL_LUMINANCE }, { 2, 2, GL_LUMINANCE_ALPHA }, { 3, 3, GL_RGB }, { 4, 4, GL_RGBA },
		{ GL_ALPHA, GL_ALPHA, GL_ALPHA }, { GL_RGB, GL_RGB, GL_RGB }, { GL_RGBA, GL_RGBA, GL_RGBA },
		{ GL_LUMINANCE, GL_LUMINANCE, GL_LUMINANCE }, { GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA },
		{ 0x2A10, 0x2A10, GL_RGB },                                     /* R3_G3_B2 */
		{ 0x803B, 0x803E, GL_ALPHA }, { 0x803F, 0x8042, GL_LUMINANCE }, { 0x8043, 0x8048, GL_LUMINANCE_ALPHA },
		{ 0x8049, 0x804D, GL_INTENSITY }, { 0x804F, 0x8054, GL_RGB }, { 0x8055, 0x805B, GL_RGBA },
		{ GL_BGR, GL_BGR, GL_RGB }, { GL_BGRA, GL_BGRA, GL_RGBA },     /* (not OpenGL 1.2's, but taken) */
	};
	for (unsigned k = 0; k < sizeof ranges / sizeof ranges[0]; k++)
		if (internal >= ranges[k].first && internal <= ranges[k].last)
			return ranges[k].base;
	return 0;
}

static bool power_of_two(int32_t v)
{
	return v > 0 && !(v & (v - 1));
}

/* The texture a target (glTexImage's, glTexParameter's...) has bound in the active unit; NULL: not a target */
static struct gl_texture *bound(struct gl_state *st, uint32_t target)
{
	int k = target_of(target);
	return k < 0 ? NULL : st->units[st->active_texture].bound[k];
}

void glf_tex_image(struct gl_state *st, uint32_t target, int32_t level, int32_t internal, int32_t w, int32_t h,
		   int32_t border, uint32_t format, uint32_t type, const uint8_t *pixels)
{
	if (target == GL_PROXY_TEXTURE_1D || target == GL_PROXY_TEXTURE_2D)
		return;                         /* (proxies: nothing kept) */
	struct gl_texture *t = bound(st, target);
	uint32_t base = base_format(internal);
	struct layout L;
	if (!t) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	if (!layout_of(&st->unpack, format, type, w, &L)) {
		uint8_t comp[4];
		struct type_info ti;
		glf_error(st, format_layout(format, comp) && type_info(type, &ti) ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
		return;
	}
	bool one = target == GL_TEXTURE_1D_TARGET;
	int32_t iw = w - 2 * border, ih = one ? 1 : h - 2 * border;
	if (level < 0 || level >= GLF_TEXTURE_LEVELS || !base || (border != 0 && border != 1) ||
	    !power_of_two(iw) || !power_of_two(ih) || iw > GLF_MAX_TEXTURE_SIZE >> level ||
	    ih > GLF_MAX_TEXTURE_SIZE >> level || (one && h != 1)) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	uint32_t *texels = malloc((size_t)iw * (size_t)ih * 4);
	if (!texels || !changing(st->names, t)) {
		free(texels);
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	if (pixels) {                           /* (a border's texels left out) */
		struct gl_pixel_store ps = st->unpack;
		ps.skip_pixels += border;
		ps.skip_rows += one ? 0 : border;
		if (ps.row_length <= 0)
			ps.row_length = w;
		layout_of(&ps, format, type, iw, &L);
		unpack_image(&ps, &L, pixels, iw, ih, base, texels, iw, 0, 0);
	} else {
		memset(texels, 0, (size_t)iw * (size_t)ih * 4);
	}
	struct gl_level *l = &t->level[level];
	free(l->texels);                        /* (the texture's own: see changing()) */
	*l = (struct gl_level){ iw, ih, (uint32_t)internal, base, texels };
}

void glf_tex_sub_image(struct gl_state *st, uint32_t target, int32_t level, int32_t x, int32_t y, int32_t w,
		       int32_t h, uint32_t format, uint32_t type, const uint8_t *pixels)
{
	struct gl_texture *t = bound(st, target);
	struct layout L;
	if (!t) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	if (!layout_of(&st->unpack, format, type, w, &L)) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	struct gl_level *l = level >= 0 && level < GLF_TEXTURE_LEVELS ? &t->level[level] : NULL;
	if (!l || !l->texels) {
		glf_error(st, l ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
		return;
	}
	if (w < 0 || h < 0 || x < 0 || y < 0 || x + w > l->width || y + h > l->height) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	if (!w || !h || !pixels)
		return;
	if (!changing(st->names, t)) {
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	unpack_image(&st->unpack, &L, pixels, w, h, l->base, l->texels, l->width, x, y);
}

/* ---- parameters ------------------------------------------------------------------ */

enum { GL_TEXTURE_BORDER_COLOR = 0x1004, GL_TEXTURE_WRAP_R = 0x8072, GL_TEXTURE_PRIORITY = 0x8066,
       GL_TEXTURE_RESIDENT = 0x8067, GL_TEXTURE_MIN_LOD = 0x813A, GL_TEXTURE_MAX_LOD, GL_TEXTURE_BASE_LEVEL,
       GL_TEXTURE_MAX_LEVEL };

static bool filter(uint32_t f, bool min)
{
	return f == GL_NEAREST || f == GL_LINEAR || (min && f >= GL_NEAREST_MIPMAP_NEAREST && f <= GL_LINEAR_MIPMAP_LINEAR);
}

static bool wrap(uint32_t w)
{
	return w == GL_CLAMP || w == GL_REPEAT || w == GL_CLAMP_TO_EDGE;
}

void glf_tex_parameter(struct gl_state *st, uint32_t target, uint32_t pname, const float *v)
{
	struct gl_texture *t = bound(st, target);
	uint32_t e = (uint32_t)v[0];
	if (!t) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	switch (pname) {
	case GL_TEXTURE_MIN_FILTER:
	case GL_TEXTURE_MAG_FILTER:
		if (!filter(e, pname == GL_TEXTURE_MIN_FILTER)) {
			glf_error(st, GL_INVALID_ENUM);
			return;
		}
		*(pname == GL_TEXTURE_MIN_FILTER ? &t->min_filter : &t->mag_filter) = e;
		break;
	case GL_TEXTURE_WRAP_S:
	case GL_TEXTURE_WRAP_T:
	case GL_TEXTURE_WRAP_R:
		if (!wrap(e)) {
			glf_error(st, GL_INVALID_ENUM);
			return;
		}
		if (pname != GL_TEXTURE_WRAP_R)
			*(pname == GL_TEXTURE_WRAP_S ? &t->wrap_s : &t->wrap_t) = e;
		break;
	case GL_TEXTURE_BORDER_COLOR:
		for (int k = 0; k < 4; k++)
			t->border[k] = v[k] > 0 ? (v[k] < 1 ? v[k] : 1) : 0;
		return;
	case GL_TEXTURE_PRIORITY:
		t->priority = v[0] > 0 ? (v[0] < 1 ? v[0] : 1) : 0;
		return;
	case GL_TEXTURE_MIN_LOD: t->min_lod = v[0]; return;
	case GL_TEXTURE_MAX_LOD: t->max_lod = v[0]; return;
	case GL_TEXTURE_BASE_LEVEL:
	case GL_TEXTURE_MAX_LEVEL:
		if (v[0] < 0) {
			glf_error(st, GL_INVALID_VALUE);
			return;
		}
		if (!changing(st->names, t)) {          /* (the levels drawn change) */
			glf_error(st, GL_OUT_OF_MEMORY);
			return;
		}
		*(pname == GL_TEXTURE_BASE_LEVEL ? &t->base_level : &t->max_level) = (int)v[0];
		break;
	default:
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	t->serial++;
}

void glf_tex_env(struct gl_state *st, uint32_t target, uint32_t pname, const float *v)
{
	struct gl_unit *u = &st->units[st->active_texture];
	uint32_t e = (uint32_t)v[0];
	if (target != GL_TEXTURE_ENV) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	if (pname == GL_TEXTURE_ENV_MODE) {
		if (e != GL_MODULATE && e != GL_DECAL && e != GL_BLEND && e != GL_REPLACE && e != GL_ADD) {
			glf_error(st, GL_INVALID_ENUM);
			return;
		}
		u->env_mode = e;
	} else if (pname == GL_TEXTURE_ENV_COLOR) {
		for (int k = 0; k < 4; k++)
			u->env_color[k] = v[k] > 0 ? (v[k] < 1 ? v[k] : 1) : 0;
	} else {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	st->raster_stale = true;
}

unsigned glf_get_tex_parameter(struct gl_state *st, uint32_t target, uint32_t pname, double *v)
{
	struct gl_texture *t = bound(st, target);
	if (!t)
		return 0;
	switch (pname) {
	case GL_TEXTURE_MIN_FILTER: v[0] = t->min_filter; return 1;
	case GL_TEXTURE_MAG_FILTER: v[0] = t->mag_filter; return 1;
	case GL_TEXTURE_WRAP_S: v[0] = t->wrap_s; return 1;
	case GL_TEXTURE_WRAP_T: v[0] = t->wrap_t; return 1;
	case GL_TEXTURE_WRAP_R: v[0] = GL_REPEAT; return 1;
	case GL_TEXTURE_BORDER_COLOR: for (int k = 0; k < 4; k++) v[k] = t->border[k]; return 4;
	case GL_TEXTURE_PRIORITY: v[0] = t->priority; return 1;
	case GL_TEXTURE_RESIDENT: v[0] = 1; return 1;
	case GL_TEXTURE_MIN_LOD: v[0] = t->min_lod; return 1;
	case GL_TEXTURE_MAX_LOD: v[0] = t->max_lod; return 1;
	case GL_TEXTURE_BASE_LEVEL: v[0] = t->base_level; return 1;
	case GL_TEXTURE_MAX_LEVEL: v[0] = t->max_level; return 1;
	default: return 0;
	}
}

unsigned glf_get_tex_env(struct gl_state *st, uint32_t target, uint32_t pname, double *v)
{
	const struct gl_unit *u = &st->units[st->active_texture];
	if (target != GL_TEXTURE_ENV)
		return 0;
	if (pname == GL_TEXTURE_ENV_MODE) {
		v[0] = u->env_mode;
		return 1;
	}
	if (pname == GL_TEXTURE_ENV_COLOR) {
		for (int k = 0; k < 4; k++)
			v[k] = u->env_color[k];
		return 4;
	}
	return 0;
}

unsigned glf_get_tex_level_parameter(struct gl_state *st, uint32_t target, int32_t level, uint32_t pname, double *v)
{
	enum { GL_TEXTURE_WIDTH = 0x1000, GL_TEXTURE_HEIGHT, GL_TEXTURE_INTERNAL_FORMAT = 0x1003,
	       GL_TEXTURE_BORDER = 0x1005, GL_TEXTURE_RED_SIZE = 0x805C, GL_TEXTURE_GREEN_SIZE,
	       GL_TEXTURE_BLUE_SIZE, GL_TEXTURE_ALPHA_SIZE, GL_TEXTURE_LUMINANCE_SIZE, GL_TEXTURE_INTENSITY_SIZE };
	struct gl_texture *t = bound(st, target);
	if (!t || level < 0 || level >= GLF_TEXTURE_LEVELS)
		return 0;
	const struct gl_level *l = &t->level[level];
	uint32_t b = l->base;
	switch (pname) {
	case GL_TEXTURE_WIDTH: v[0] = l->width; return 1;
	case GL_TEXTURE_HEIGHT: v[0] = l->height; return 1;
	case GL_TEXTURE_INTERNAL_FORMAT: v[0] = l->internal ? l->internal : 1; return 1;
	case GL_TEXTURE_BORDER: v[0] = 0; return 1;
	case GL_TEXTURE_RED_SIZE: case GL_TEXTURE_GREEN_SIZE: case GL_TEXTURE_BLUE_SIZE:
		v[0] = b == GL_RGB || b == GL_RGBA ? 8 : 0;
		return 1;
	case GL_TEXTURE_ALPHA_SIZE: v[0] = b == GL_ALPHA || b == GL_LUMINANCE_ALPHA || b == GL_RGBA ? 8 : 0; return 1;
	case GL_TEXTURE_LUMINANCE_SIZE: v[0] = b == GL_LUMINANCE || b == GL_LUMINANCE_ALPHA ? 8 : 0; return 1;
	case GL_TEXTURE_INTENSITY_SIZE: v[0] = b == GL_INTENSITY ? 8 : 0; return 1;
	default: return 0;
	}
}

/* ---- drawing ----------------------------------------------------------------------- */

static bool mipmapped(uint32_t f)
{
	return f >= GL_NEAREST_MIPMAP_NEAREST && f <= GL_LINEAR_MIPMAP_LINEAR;
}

/* The levels a texture draws with, from its base level: how many (0: it isn't complete) */
static unsigned complete_levels(const struct gl_texture *t)
{
	int b = t->base_level;
	if (b < 0 || b >= GLF_TEXTURE_LEVELS || b > t->max_level || !t->level[b].texels)
		return 0;
	const struct gl_level *first = &t->level[b];
	int w = first->width, h = first->height;
	unsigned n = 1;
	while ((w > 1 || h > 1) && b + (int)n <= t->max_level && b + (int)n < GLF_TEXTURE_LEVELS) {
		w = w > 1 ? w / 2 : 1;
		h = h > 1 ? h / 2 : 1;
		const struct gl_level *l = &t->level[b + (int)n];
		if (!l->texels || l->width != w || l->height != h || l->internal != first->internal)
			break;
		n++;
	}
	bool chain = (w == 1 && h == 1) || b + (int)n > t->max_level || b + (int)n >= GLF_TEXTURE_LEVELS;
	if (mipmapped(t->min_filter) && !chain)
		return 0;
	return chain && mipmapped(t->min_filter) ? n : 1;
}

struct gl_texture *glf_unit_texture(struct gl_state *st, unsigned unit, struct raster_texture **drawn)
{
	unsigned bit = 1u << unit;
	int k = st->caps[GLF_CAP_TEXTURE_2D] & bit ? GLF_2D : st->caps[GLF_CAP_TEXTURE_1D] & bit ? GLF_1D : -1;
	if (k < 0)
		return NULL;
	struct gl_texture *t = st->units[unit].bound[k];
	unsigned n = complete_levels(t);
	if (!n)
		return NULL;
	if (!t->drawn || t->drawn->levels != n) {
		struct raster_texture *r = calloc(1, sizeof *r);
		if (!r || !changing(st->names, t)) {
			free(r);
			return NULL;
		}
		for (unsigned j = 0; j < n; j++) {
			const struct gl_level *l = &t->level[t->base_level + (int)j];
			r->width[j] = (unsigned)l->width;
			r->height[j] = (unsigned)l->height;
			r->texels[j] = l->texels;
		}
		r->levels = n;
		t->drawn = r;
		t->serial++;
	}
	*drawn = t->drawn;
	return t;
}

/* ---- reading pixels ---------------------------------------------------------------- */

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

void glf_read_pixels(struct gl_state *st, const struct gl_target *t, int32_t x, int32_t y, int32_t w, int32_t h,
		     uint32_t format, uint32_t type, uint8_t *out)
{
	struct layout L;
	if (w < 0 || h < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	if (!layout_of(&st->pack, format, type, w, &L)) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	const struct surface *s = t->surface;
	for (int32_t j = 0; j < h; j++) {
		int32_t gy = y + j, row = t->height - 1 - gy;  /* (OpenGL's rows go up) */
		uint8_t *p = out + L.start + (size_t)j * L.row;
		for (int32_t i = 0; i < w; i++, p += L.pixel) {
			int32_t gx = x + i;
			if (!s || gx < 0 || gx >= t->width || row < 0 || row >= t->height)
				continue;               /* (outside the drawable: undefined, left alone) */
			uint32_t argb = s->target.color[(size_t)row * (size_t)t->width + (size_t)gx];
			uint8_t c[4] = { (uint8_t)(argb >> 16), (uint8_t)(argb >> 8), (uint8_t)argb,
					 (uint8_t)(t->alpha_bits ? argb >> 24 : 255) };
			uint32_t v[4];
			for (unsigned k = 0; k < L.n; k++) {
				unsigned comp = L.comp[k];
				uint8_t value = comp == C_L ? (uint8_t)(c[0] + c[1] + c[2] > 255 ? 255 : c[0] + c[1] + c[2]) :
					c[comp];
				v[k] = from8(&L.ti, value, L.ti.packed ? L.ti.bits[k] : 0);
			}
			if (L.ti.packed) {
				uint32_t word = 0;
				unsigned total = 8u * L.ti.bytes, at = 0;
				for (unsigned k = 0; k < L.n; k++) {
					unsigned b = L.ti.bits[k], shift = L.ti.rev ? at : total - at - b;
					word |= v[k] << shift;
					at += b;
				}
				write_unit(p, L.ti.bytes, st->pack.swap_bytes, word);
			} else {
				for (unsigned k = 0; k < L.n; k++)
					write_unit(p + k * L.ti.bytes, L.ti.bytes, st->pack.swap_bytes, v[k]);
			}
		}
	}
}
