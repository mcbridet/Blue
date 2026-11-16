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
       GL_PROXY_TEXTURE_2D = 0x8064, GL_PROXY_TEXTURE_3D = 0x8070, GL_TEXTURE_DEPTH = 0x8071 };

/* ---- namespaces ---------------------------------------------------------------- */

static void texture_init(struct gl_texture *t, uint32_t name)
{
	memset(t, 0, sizeof *t);
	t->name = name;
	t->target = -1;
	t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
	t->mag_filter = GL_LINEAR;
	t->wrap_s = t->wrap_t = t->wrap_r = GL_REPEAT;
	t->priority = 1;
	t->min_lod = -1000;
	t->max_lod = 1000;
	t->max_level = 1000;
}

void glf_retire(struct gl_names *n, struct raster_texture *r)
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
		size_t bytes = (size_t)l->width * (size_t)l->height * (size_t)l->depth * 4;
		uint32_t *copy = malloc(bytes);
		if (!copy)
			return false;
		memcpy(copy, l->texels, bytes);
		l->texels = copy;
	}
	glf_retire(n, t->drawn);
	t->drawn = NULL;
	return true;
}

/* A texture's texels freed (or retired, if they're drawn) */
static void free_texture(struct gl_names *n, struct gl_texture *t)
{
	for (unsigned k = 0; k < GLF_TEXTURE_LEVELS; k++)
		if (!shared(t, t->level[k].texels))
			free(t->level[k].texels);
	glf_retire(n, t->drawn);
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
	glf_free(st);
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
	glf_free_lists(n);
	glf_free_retired(n, gpu);
	free(n->retired);
	free(n);
}

void glf_copy(struct gl_state *to, const struct gl_state *from)
{
	struct gl_names *names = to->names;
	struct gl_texture defaults[GLF_TARGETS];
	memcpy(defaults, to->defaults, sizeof defaults);
	struct gl_saved *stack[GLF_ATTRIB_DEPTH];       /* (the attribute stacks stay the destination's) */
	struct gl_client_saved *client[GLF_ATTRIB_DEPTH];
	unsigned depth = to->attrib_depth, client_depth = to->client_depth;
	memcpy(stack, to->attrib_stack, sizeof stack);
	memcpy(client, to->client_stack, sizeof client);
	struct gl_select select = to->select;   /* (and its render mode and records) */
	struct gl_eval *eval = to->eval;        /* (evaluators' control points: in no attribute group) */
	*to = *from;
	to->select = select;
	to->eval = eval;
	memcpy(to->attrib_stack, stack, sizeof stack);
	memcpy(to->client_stack, client, sizeof client);
	to->attrib_depth = depth;
	to->client_depth = client_depth;
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
	return target == GL_TEXTURE_2D_TARGET ? GLF_2D : target == GL_TEXTURE_1D_TARGET ? GLF_1D :
	       target == GL_TEXTURE_3D ? GLF_3D : -1;
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

/* An image's R, G, B, A bytes (w x h) into texels [x, x + w) x [y, y + h) of a level `stride` wide */
static void put_texels(uint32_t *texels, int32_t stride, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t base,
		       const uint8_t *rgba)
{
	for (int32_t j = 0; j < h; j++) {
		uint32_t *out = texels + (size_t)(y + j) * (size_t)stride + (size_t)x;
		for (int32_t i = 0; i < w; i++, rgba += 4)
			out[i] = texel(base, rgba);
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

/* A level's image defined (w x h x d, slices one after another, from R, G, B, A bytes; NULL: texels undefined) */
static void define(struct gl_state *st, struct gl_texture *t, int32_t level, int32_t internal, uint32_t base,
		   int32_t w, int32_t h, int32_t d, const uint8_t *rgba)
{
	size_t n = (size_t)w * (size_t)h * (size_t)d;
	uint32_t *texels = malloc(n * 4);
	if (!texels || !changing(st->names, t)) {
		free(texels);
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	if (rgba)
		put_texels(texels, w, 0, 0, w, h * d, base, rgba);
	else
		memset(texels, 0, n * 4);
	struct gl_level *l = &t->level[level];
	free(l->texels);                        /* (the texture's own: see changing()) */
	*l = (struct gl_level){ w, h, d, (uint32_t)internal, base, texels };
}

/* glTexImage's and glCopyTexImage's checks: the texture, and the image's size without its border (false: an error) */
static struct gl_texture *image_target(struct gl_state *st, uint32_t target, int32_t level, int32_t internal,
				       int32_t w, int32_t h, int32_t border, int32_t *iw, int32_t *ih)
{
	struct gl_texture *t = bound(st, target);
	if (!t) {
		glf_error(st, GL_INVALID_ENUM);
		return NULL;
	}
	bool one = target == GL_TEXTURE_1D_TARGET;
	*iw = w - 2 * border;
	*ih = one ? 1 : h - 2 * border;
	if (level < 0 || level >= GLF_TEXTURE_LEVELS || !base_format(internal) || (border != 0 && border != 1) ||
	    !power_of_two(*iw) || !power_of_two(*ih) || *iw > GLF_MAX_TEXTURE_SIZE >> level ||
	    *ih > GLF_MAX_TEXTURE_SIZE >> level || (one && h != 1)) {
		glf_error(st, GL_INVALID_VALUE);
		return NULL;
	}
	return t;
}

/* Whether a format and type make a texture's image (colors or color indices), else the error they make */
static uint32_t image_check(uint32_t format, uint32_t type)
{
	if (format == GL_STENCIL_INDEX || format == GL_DEPTH_COMPONENT)
		return GL_INVALID_ENUM;
	return glf_pixel_check(format, type);
}

void glf_tex_image(struct gl_state *st, uint32_t target, int32_t level, int32_t internal, int32_t w, int32_t h,
		   int32_t border, uint32_t format, uint32_t type, const uint8_t *pixels)
{
	if (target == GL_PROXY_TEXTURE_1D || target == GL_PROXY_TEXTURE_2D)
		return;                         /* (proxies: nothing kept) */
	if (target != GL_TEXTURE_1D_TARGET && target != GL_TEXTURE_2D_TARGET) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	uint32_t e = image_check(format, type);
	if (e) {
		glf_error(st, e);
		return;
	}
	int32_t iw, ih;
	struct gl_texture *t = image_target(st, target, level, internal, w, h, border, &iw, &ih);
	if (!t)
		return;
	uint8_t *rgba = NULL;
	if (pixels) {                           /* (a border's texels left out) */
		struct gl_pixel_store ps = st->unpack;
		ps.skip_pixels += border;
		ps.skip_rows += target == GL_TEXTURE_1D_TARGET ? 0 : border;
		if (ps.row_length <= 0)
			ps.row_length = w;
		rgba = malloc((size_t)iw * (size_t)ih * 4);
		if (!rgba) {
			glf_error(st, GL_OUT_OF_MEMORY);
			return;
		}
		glf_unpack_rgba(st, &ps, format, type, iw, ih, pixels, rgba, true);
	}
	define(st, t, level, internal, base_format(internal), iw, ih, 1, rgba);
	free(rgba);
}

void glf_copy_tex_image(struct gl_state *st, const struct gl_target *tg, uint32_t target, int32_t level,
			int32_t internal, int32_t x, int32_t y, int32_t w, int32_t h, int32_t border)
{
	int32_t iw, ih;
	if (target != GL_TEXTURE_1D_TARGET && target != GL_TEXTURE_2D_TARGET) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	struct gl_texture *t = image_target(st, target, level, internal, w, h, border, &iw, &ih);
	if (!t)
		return;
	uint8_t *rgba = malloc((size_t)iw * (size_t)ih * 4);
	if (!rgba) {
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	glf_read_rgba(st, tg, x + border, y + (target == GL_TEXTURE_1D_TARGET ? 0 : border), iw, ih, rgba);
	define(st, t, level, internal, base_format(internal), iw, ih, 1, rgba);
	free(rgba);
}

/* glTexSubImage's and glCopyTexSubImage's checks: the level, the rectangle within it (NULL: an error, or nothing) */
static struct gl_level *sub_target(struct gl_state *st, uint32_t target, int32_t level, int32_t x, int32_t y,
				   int32_t w, int32_t h, struct gl_texture **tp)
{
	struct gl_texture *t = bound(st, target);
	if (!t) {
		glf_error(st, GL_INVALID_ENUM);
		return NULL;
	}
	struct gl_level *l = level >= 0 && level < GLF_TEXTURE_LEVELS ? &t->level[level] : NULL;
	if (!l || !l->texels) {
		glf_error(st, l ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
		return NULL;
	}
	if (w < 0 || h < 0 || x < 0 || y < 0 || x + w > l->width || y + h > l->height) {
		glf_error(st, GL_INVALID_VALUE);
		return NULL;
	}
	if (!w || !h)
		return NULL;
	*tp = t;
	return l;
}

void glf_tex_sub_image(struct gl_state *st, uint32_t target, int32_t level, int32_t x, int32_t y, int32_t w,
		       int32_t h, uint32_t format, uint32_t type, const uint8_t *pixels)
{
	if (target != GL_TEXTURE_1D_TARGET && target != GL_TEXTURE_2D_TARGET) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	uint32_t e = image_check(format, type);
	if (e) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	struct gl_texture *t;
	struct gl_level *l = sub_target(st, target, level, x, y, w, h, &t);
	if (!l || !pixels)
		return;
	uint8_t *rgba = malloc((size_t)w * (size_t)h * 4);
	if (!rgba || !changing(st->names, t)) {
		free(rgba);
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	glf_unpack_rgba(st, &st->unpack, format, type, w, h, pixels, rgba, true);
	put_texels(l->texels, l->width, x, y, w, h, l->base, rgba);
	free(rgba);
}

void glf_copy_tex_sub_image(struct gl_state *st, const struct gl_target *tg, uint32_t target, int32_t level,
			    int32_t xoffset, int32_t yoffset, int32_t x, int32_t y, int32_t w, int32_t h)
{
	if (target != GL_TEXTURE_1D_TARGET && target != GL_TEXTURE_2D_TARGET) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	struct gl_texture *t;
	struct gl_level *l = sub_target(st, target, level, xoffset, yoffset, w, h, &t);
	if (!l)
		return;
	uint8_t *rgba = malloc((size_t)w * (size_t)h * 4);
	if (!rgba || !changing(st->names, t)) {
		free(rgba);
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	glf_read_rgba(st, tg, x, y, w, h, rgba);
	put_texels(l->texels, l->width, xoffset, yoffset, w, h, l->base, rgba);
	free(rgba);
}

/* ---- 3D images ---- */

void glf_tex_image_3d(struct gl_state *st, uint32_t target, int32_t level, int32_t internal, int32_t w, int32_t h,
		      int32_t d, int32_t border, uint32_t format, uint32_t type, const uint8_t *pixels)
{
	if (target == GL_PROXY_TEXTURE_3D)
		return;                         /* (proxies: nothing kept) */
	if (target != GL_TEXTURE_3D) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	uint32_t e = image_check(format, type);
	if (e) {
		glf_error(st, e);
		return;
	}
	struct gl_texture *t = bound(st, target);
	int32_t iw = w - 2 * border, ih = h - 2 * border, id = d - 2 * border, most = GLF_MAX_3D_TEXTURE_SIZE >> level;
	if (level < 0 || most < 1 || !base_format(internal) || (border != 0 && border != 1) || !power_of_two(iw) ||
	    !power_of_two(ih) || !power_of_two(id) || iw > most || ih > most || id > most) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	size_t slice = (size_t)iw * (size_t)ih * 4;
	uint8_t *rgba = NULL;
	if (pixels) {                           /* (a border's texels left out: rows, pixels and images) */
		struct gl_pixel_store ps = st->unpack;
		ps.skip_pixels += border;
		ps.skip_rows += border;
		if (ps.row_length <= 0)
			ps.row_length = w;
		size_t stride = glf_image_stride(&ps, format, type, w, h);
		rgba = malloc(slice * (size_t)id);
		if (!rgba) {
			glf_error(st, GL_OUT_OF_MEMORY);
			return;
		}
		for (int32_t k = 0; k < id; k++)
			glf_unpack_rgba(st, &ps, format, type, iw, ih,
					pixels + ((size_t)ps.skip_images + (size_t)border + (size_t)k) * stride,
					rgba + (size_t)k * slice, true);
	}
	define(st, t, level, internal, base_format(internal), iw, ih, id, rgba);
	free(rgba);
}

/* glTexSubImage3D's and glCopyTexSubImage3D's checks: the level, the box within it (NULL: an error, or nothing) */
static struct gl_level *sub_target_3d(struct gl_state *st, uint32_t target, int32_t level, int32_t x, int32_t y,
				      int32_t z, int32_t w, int32_t h, int32_t d, struct gl_texture **tp)
{
	if (target != GL_TEXTURE_3D) {
		glf_error(st, GL_INVALID_ENUM);
		return NULL;
	}
	struct gl_texture *t = bound(st, target);
	struct gl_level *l = level >= 0 && level < GLF_TEXTURE_LEVELS ? &t->level[level] : NULL;
	if (!l || !l->texels) {
		glf_error(st, l ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
		return NULL;
	}
	if (w < 0 || h < 0 || d < 0 || x < 0 || y < 0 || z < 0 || x + w > l->width || y + h > l->height ||
	    z + d > l->depth) {
		glf_error(st, GL_INVALID_VALUE);
		return NULL;
	}
	if (!w || !h || !d || !changing(st->names, t))
		return NULL;
	*tp = t;
	return l;
}

void glf_tex_sub_image_3d(struct gl_state *st, uint32_t target, int32_t level, int32_t x, int32_t y, int32_t z,
			  int32_t w, int32_t h, int32_t d, uint32_t format, uint32_t type, const uint8_t *pixels)
{
	if (image_check(format, type)) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	struct gl_texture *t;
	struct gl_level *l = sub_target_3d(st, target, level, x, y, z, w, h, d, &t);
	if (!l || !pixels)
		return;
	size_t slice = (size_t)w * (size_t)h * 4, stride = glf_image_stride(&st->unpack, format, type, w, h);
	uint8_t *rgba = malloc(slice);
	if (!rgba) {
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	for (int32_t k = 0; k < d; k++) {
		glf_unpack_rgba(st, &st->unpack, format, type, w, h,
				pixels + ((size_t)st->unpack.skip_images + (size_t)k) * stride, rgba, true);
		put_texels(l->texels + (size_t)(z + k) * (size_t)l->width * (size_t)l->height, l->width, x, y, w, h,
			   l->base, rgba);
	}
	free(rgba);
}

void glf_copy_tex_sub_image_3d(struct gl_state *st, const struct gl_target *tg, uint32_t target, int32_t level,
			       int32_t xoffset, int32_t yoffset, int32_t zoffset, int32_t x, int32_t y, int32_t w,
			       int32_t h)
{
	struct gl_texture *t;
	struct gl_level *l = sub_target_3d(st, target, level, xoffset, yoffset, zoffset, w, h, 1, &t);
	if (!l)
		return;
	uint8_t *rgba = malloc((size_t)w * (size_t)h * 4);
	if (!rgba) {
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	glf_read_rgba(st, tg, x, y, w, h, rgba);
	put_texels(l->texels + (size_t)zoffset * (size_t)l->width * (size_t)l->height, l->width, xoffset, yoffset, w,
		   h, l->base, rgba);
	free(rgba);
}

/* A level of a texture glGetTexImage reads (NULL: an error, or nothing to read) */
static const struct gl_level *get_level(struct gl_state *st, uint32_t target, int32_t level, uint32_t format,
					uint32_t type)
{
	if (target != GL_TEXTURE_1D_TARGET && target != GL_TEXTURE_2D_TARGET && target != GL_TEXTURE_3D) {
		glf_error(st, GL_INVALID_ENUM);
		return NULL;
	}
	uint32_t e = format == GL_COLOR_INDEX || format == GL_STENCIL_INDEX || format == GL_DEPTH_COMPONENT ||
		     type == GL_BITMAP ? GL_INVALID_ENUM : glf_pixel_check(format, type);
	if (e) {
		glf_error(st, e);
		return NULL;
	}
	if (level < 0 || level >= GLF_TEXTURE_LEVELS) {
		glf_error(st, GL_INVALID_VALUE);
		return NULL;
	}
	const struct gl_level *l = &bound(st, target)->level[level];
	return l->texels ? l : NULL;
}

size_t glf_tex_image_bytes(struct gl_state *st, uint32_t target, int32_t level, uint32_t format, uint32_t type)
{
	struct gl_texture *t = bound(st, target);
	if (!t || level < 0 || level >= GLF_TEXTURE_LEVELS || !t->level[level].texels)
		return 0;
	const struct gl_level *l = &t->level[level];
	return glf_image_bytes_3d(&st->pack, format, type, l->width, l->height, l->depth);
}

void glf_get_tex_image(struct gl_state *st, uint32_t target, int32_t level, uint32_t format, uint32_t type,
		       uint8_t *out)
{
	const struct gl_level *l = get_level(st, target, level, format, type);
	if (!l)
		return;
	size_t n = (size_t)l->width * (size_t)l->height * (size_t)l->depth;
	uint8_t *rgba = malloc(n * 4);
	if (!rgba) {
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	/* (as OpenGL 1.2's table 6.1: luminance and intensity as red, what the base format hasn't 0, alpha 1) */
	for (size_t k = 0; k < n; k++) {
		uint32_t v = l->texels[k];
		uint8_t *c = rgba + 4 * k, a = (uint8_t)(v >> 24), r = (uint8_t)(v >> 16);
		bool rgb = l->base == GL_RGB || l->base == GL_RGBA;
		c[0] = l->base == GL_ALPHA ? 0 : r;
		c[1] = rgb ? (uint8_t)(v >> 8) : 0;
		c[2] = rgb ? (uint8_t)v : 0;
		c[3] = l->base == GL_ALPHA || l->base == GL_LUMINANCE_ALPHA || l->base == GL_RGBA ? a : 255;
	}
	size_t stride = glf_image_stride(&st->pack, format, type, l->width, l->height), slice = n / (size_t)l->depth;
	for (int32_t k = 0; k < l->depth; k++)          /* (slices image_height rows apart, skip_images first) */
		glf_pack_rgba(st, format, type, l->width, l->height, rgba + (size_t)k * slice * 4,
			      out + ((size_t)st->pack.skip_images + (size_t)k) * stride);
	free(rgba);
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
		*(pname == GL_TEXTURE_WRAP_S ? &t->wrap_s : pname == GL_TEXTURE_WRAP_T ? &t->wrap_t : &t->wrap_r) = e;
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
	if (target == GL_TEXTURE_FILTER_CONTROL_EXT && pname == GL_TEXTURE_LOD_BIAS_EXT) {
		u->lod_bias = v[0];
		st->raster_stale = true;
		return;
	}
	if (target != GL_TEXTURE_ENV) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	if (pname == GL_TEXTURE_ENV_MODE) {
		uint32_t e = v[0] >= 0 && v[0] < 65536 ? (uint32_t)v[0] : 0;
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
	case GL_TEXTURE_WRAP_R: v[0] = t->wrap_r; return 1;
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
	if (target == GL_TEXTURE_FILTER_CONTROL_EXT && pname == GL_TEXTURE_LOD_BIAS_EXT) {
		v[0] = u->lod_bias;
		return 1;
	}
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
	case GL_TEXTURE_DEPTH: v[0] = l->texels ? l->depth : 0; return 1;
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
	int w = first->width, h = first->height, d = first->depth;
	unsigned n = 1;
	while ((w > 1 || h > 1 || d > 1) && b + (int)n <= t->max_level && b + (int)n < GLF_TEXTURE_LEVELS) {
		int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1, nd = d > 1 ? d / 2 : 1;
		const struct gl_level *l = &t->level[b + (int)n];
		if (!l->texels || l->width != nw || l->height != nh || l->depth != nd || l->internal != first->internal)
			break;
		w = nw;
		h = nh;
		d = nd;
		n++;
	}
	bool chain = (w == 1 && h == 1 && d == 1) || b + (int)n > t->max_level || b + (int)n >= GLF_TEXTURE_LEVELS;
	if (mipmapped(t->min_filter) && !chain)
		return 0;
	return chain && mipmapped(t->min_filter) ? n : 1;
}

struct gl_texture *glf_unit_texture(struct gl_state *st, unsigned unit, struct raster_texture **drawn)
{
	unsigned bit = 1u << unit;
	int k = st->caps[GLF_CAP_TEXTURE_3D] & bit ? GLF_3D : st->caps[GLF_CAP_TEXTURE_2D] & bit ? GLF_2D :
		st->caps[GLF_CAP_TEXTURE_1D] & bit ? GLF_1D : -1;
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
			r->depth[j] = k == GLF_3D ? (unsigned)l->depth : 0;
			r->texels[j] = l->texels;
		}
		r->levels = n;
		t->drawn = r;
		t->serial++;
	}
	*drawn = t->drawn;
	return t;
}
