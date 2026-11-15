#include "raster.h"

/* (the same pixels on every host: no fused multiply-adds where some have them; GCC fuses none in ISO C) */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ALWAYS_INLINE inline __attribute__((always_inline))

/* Four channels at once (each its own IEEE operation, as one at a time would be) */
typedef float vec4 __attribute__((vector_size(16)));
typedef int32_t vec4i __attribute__((vector_size(16)));
typedef uint8_t vec4b __attribute__((vector_size(4)));

/*
 * The values interpolated across a primitive: depth, 1/w, the fog
 * coordinate and color 0 always, then the colors and texture coordinates
 * its state needs, wherever its layout puts them. Colors are a, r, g, b
 * there, each at a multiple of four.
 */
enum { A_Z, A_W, A_FOG, A_C0 = 4, A_PLAIN = A_C0 + 4, A_MAX = A_C0 + 4 * RASTER_COLORS + 4 * RASTER_UNITS };
_Static_assert(A_MAX % 4 == 0, "whole vectors of attributes");

struct layout {
	unsigned n;
	uint8_t color[RASTER_COLORS];           /* (0: not interpolated) */
	uint8_t tex[RASTER_UNITS], q[RASTER_UNITS], r[RASTER_UNITS];   /* (r: 3D textures') */
};

static inline float clamp01(float x)
{
	return x < 0 ? 0 : x > 1 ? 1 : x;
}

/* The sources a stage's arguments name */
static unsigned arg_sources(const uint8_t arg[3], unsigned op)
{
	unsigned n = op == RASTER_OP_REPLACE ? 1 : op == RASTER_OP_INTERPOLATE ? 3 : 2, used = 0;
	for (unsigned k = 0; k < n; k++)
		used |= 1u << (arg[k] & 15u);
	return used;
}

static unsigned sources_used(const struct raster_state *s)
{
	unsigned used = 0;
	for (unsigned k = 0; k < s->stages && k < RASTER_STAGES; k++)
		for (int g = 0; g < 2; g++)
			used |= arg_sources(s->stage[k].arg[g], s->stage[k].op[g]);
	return used;
}

unsigned raster_units_used(const struct raster_state *s)
{
	return sources_used(s) >> RASTER_TEXTURE0 & ((1u << RASTER_UNITS) - 1);
}

unsigned raster_colors_used(const struct raster_state *s)
{
	return 1u | (sources_used(s) >> RASTER_COLOR0 & ((1u << RASTER_COLORS) - 1));
}

bool raster_textured(const struct raster_state *s, bool textured)
{
	if (!textured || !s->stages)
		return false;
	unsigned units = raster_units_used(s);
	for (unsigned k = 0; k < RASTER_UNITS; k++)
		if (units >> k & 1 && (!s->unit[k].texture || !s->unit[k].texture->levels))
			return false;
	return true;
}

static void stage_rgb(struct raster_stage *st, unsigned op, unsigned a0, unsigned a1, unsigned a2, bool clamp)
{
	st->op[0] = (uint8_t)op;
	st->arg[0][0] = (uint8_t)a0;
	st->arg[0][1] = (uint8_t)a1;
	st->arg[0][2] = (uint8_t)a2;
	st->clamp[0] = clamp;
}

static void stage_alpha(struct raster_stage *st, unsigned op, unsigned a0, unsigned a1)
{
	st->op[1] = (uint8_t)op;
	st->arg[1][0] = (uint8_t)a0;
	st->arg[1][1] = (uint8_t)a1;
}

void raster_rave_stages(struct raster_state *s, unsigned ops)
{
	const unsigned t0 = RASTER_TEXTURE0, t1 = RASTER_TEXTURE0 + 1;
	memset(s->stage, 0, sizeof s->stage);
	struct raster_stage *st = s->stage;
	if (ops & RASTER_TEX_DECAL) {
		stage_rgb(st, RASTER_OP_INTERPOLATE, t0, RASTER_COLOR0, t0 | RASTER_ALPHA, false);
		stage_alpha(st, RASTER_OP_REPLACE, RASTER_COLOR0, 0);
	} else {
		stage_rgb(st, RASTER_OP_REPLACE, t0, 0, 0, false);
		stage_alpha(st, RASTER_OP_MODULATE, t0, RASTER_COLOR0);
	}
	st++;
	if (ops & RASTER_TEX_MODULATE) {
		stage_rgb(st, RASTER_OP_MODULATE, RASTER_PREVIOUS, RASTER_COLOR0 + 2, 0, true);
		stage_alpha(st, RASTER_OP_REPLACE, RASTER_PREVIOUS, 0);
		st++;
	}
	if (s->unit[1].texture) {
		stage_rgb(st, RASTER_OP_MODULATE, RASTER_PREVIOUS, t1, 0, false);
		stage_alpha(st, RASTER_OP_MODULATE, RASTER_PREVIOUS, t1);
		st++;
	} else if (ops & RASTER_TEX_HIGHLIGHT) {
		stage_rgb(st, RASTER_OP_ADD, RASTER_PREVIOUS, RASTER_COLOR0 + 1, 0, true);
		stage_alpha(st, RASTER_OP_REPLACE, RASTER_PREVIOUS, 0);
		st++;
	}
	s->stages = (unsigned)(st - s->stage);
}

/* Where a primitive drawn with this state (textured or not) has its values */
static void layout_of(const struct raster_state *s, bool textured, struct layout *L)
{
	memset(L, 0, sizeof *L);
	L->color[0] = A_C0;
	unsigned n = A_PLAIN;
	if (textured) {
		unsigned colors = raster_colors_used(s), units = raster_units_used(s);
		for (unsigned k = 1; k < RASTER_COLORS; k++)
			if (colors >> k & 1) {
				L->color[k] = (uint8_t)n;
				n += 4;
			}
		for (unsigned k = 0; k < RASTER_UNITS; k++)
			if (units >> k & 1) {
				L->tex[k] = (uint8_t)n;
				n += 2;
				if (s->unit[k].projective)
					L->q[k] = (uint8_t)n++;
				if (s->unit[k].texture && s->unit[k].texture->depth[0])
					L->r[k] = (uint8_t)n++;
			}
	}
	L->n = n;
}

static void attribs(const struct raster_vertex *v, const struct layout *L, float *a)
{
	a[A_Z] = v->z;
	a[A_W] = v->invw;
	a[A_FOG] = v->fog;
	a[A_FOG + 1] = 0;
	for (unsigned k = 0; k < RASTER_COLORS; k++)
		if (L->color[k]) {
			float *c = a + L->color[k];
			c[0] = v->color[k][3];
			c[1] = v->color[k][0];
			c[2] = v->color[k][1];
			c[3] = v->color[k][2];
		}
	for (unsigned k = 0; k < RASTER_UNITS; k++) {
		if (L->tex[k]) {
			a[L->tex[k]] = v->tex[k][0];
			a[L->tex[k] + 1] = v->tex[k][1];
		}
		if (L->q[k])
			a[L->q[k]] = v->tex[k][2];
		if (L->r[k])
			a[L->r[k]] = v->texr[k];
	}
}

float raster_far(const struct raster_state *s)
{
	return s->perspective_z ? 0.0f : 1.0f;
}

/* The pixels of the target a state lets us draw, within rows [rows[0], rows[1]) */
static inline void clip_rows(const struct raster_target *t, const struct raster_state *s, const int rows[2], int *l,
			     int *top, int *r, int *b)
{
	*l = s->clip_left > 0 ? s->clip_left : 0;
	*top = rows[0] > 0 ? rows[0] : 0;
	*r = s->clip_right < t->width ? s->clip_right : t->width;
	*b = rows[1] < t->height ? rows[1] : t->height;
}

static void clear_rect(struct raster_target *t, int left, int top, int right, int bottom, bool color, uint32_t argb,
		       uint32_t keep, bool depth, float z)
{
	left = left < 0 ? 0 : left;
	top = top < 0 ? 0 : top;
	right = right > t->width ? t->width : right;
	bottom = bottom > t->height ? t->height : bottom;
	for (int y = top; y < bottom; y++) {
		size_t row = (size_t)y * (size_t)t->width;
		for (int x = left; x < right; x++) {
			if (color)
				t->color[row + (size_t)x] = (argb & ~keep) | (t->color[row + (size_t)x] & keep);
			if (depth)
				t->depth[row + (size_t)x] = z;
		}
	}
}

/* The stencil's bits in `write` set to v within [left, right) x [top, bottom) */
static void clear_stencil(struct raster_target *t, int left, int top, int right, int bottom, uint8_t v, uint8_t write)
{
	left = left < 0 ? 0 : left;
	top = top < 0 ? 0 : top;
	right = right > t->width ? t->width : right;
	bottom = bottom > t->height ? t->height : bottom;
	for (int y = top; y < bottom && t->stencil; y++) {
		uint8_t *p = t->stencil + (size_t)y * (size_t)t->width;
		for (int x = left; x < right; x++)
			p[x] = (uint8_t)((p[x] & ~write) | (v & write));
	}
}

void raster_clear(struct raster_target *t, int left, int top, int right, int bottom,
		  bool color, uint32_t argb, bool depth, float z)
{
	clear_rect(t, left, top, right, bottom, color, argb, 0, depth, z);
}

static inline bool z_pass(unsigned f, float z, float buf)
{
	switch (f) {
	case RASTER_Z_LT: return z < buf;
	case RASTER_Z_EQ: return z == buf;
	case RASTER_Z_LE: return z <= buf;
	case RASTER_Z_GT: return z > buf;
	case RASTER_Z_NE: return z != buf;
	case RASTER_Z_GE: return z >= buf;
	case RASTER_Z_FALSE: return false;
	default: return true;           /* none, true */
	}
}

static inline bool alpha_pass(unsigned f, float a, float ref)
{
	switch (f) {
	case RASTER_ALPHA_LT: return a < ref;
	case RASTER_ALPHA_EQ: return a == ref;
	case RASTER_ALPHA_LE: return a <= ref;
	case RASTER_ALPHA_GT: return a > ref;
	case RASTER_ALPHA_NE: return a != ref;
	case RASTER_ALPHA_GE: return a >= ref;
	default: return true;
	}
}

/* A pixel's a, r, g and b, 0 to 1 (each byte converted as a whole vector) */
static inline vec4 unpack(uint32_t p)
{
	vec4b b;
	memcpy(&b, &p, sizeof b);
	vec4 f = __builtin_convertvector(b, vec4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	f = __builtin_shufflevector(f, f, 3, 2, 1, 0);
#endif
	return f * (1.0f / 255);
}

/*
 * Each of a, r, g, b clamped to 0..1 (NaN stays), times 255, plus 0.5, made
 * a whole number toward 0 (NaN: 0, as a conversion one at a time gives it on
 * the hosts Blue runs on), as a pixel
 */
static inline uint32_t pack(const float c[4])
{
	vec4 v;
	memcpy(&v, c, sizeof v);
	vec4i below = v < 0.0f, above = v > 1.0f;
	v = (vec4)(((vec4i)v & ~(below | above)) | ((vec4i)(vec4){ 1, 1, 1, 1 } & above));
	v = v * 255.0f + 0.5f;
	v = (vec4)((vec4i)v & (v == v));
	vec4i u = __builtin_convertvector(v, vec4i);
	return (uint32_t)u[0] << 24 | (uint32_t)u[1] << 16 | (uint32_t)u[2] << 8 | (uint32_t)u[3];
}

/* ---- texture sampling ------------------------------------------------------- */

struct sampler {
	const uint32_t *p;
	int w, h, d;                            /* (d: 3D textures') */
	float fw, fh, fd;
	bool clamp_u, clamp_v, clamp_w;
	bool pow2_u, pow2_v, pow2_w;            /* (wrapping by mask) */
	bool bilinear;
};

static void sampler_init(struct sampler *sm, const struct raster_unit *u, unsigned level, bool bilinear)
{
	const struct raster_texture *tx = u->texture;
	sm->p = tx->texels[level];
	sm->w = (int)tx->width[level];
	sm->h = (int)tx->height[level];
	sm->d = (int)tx->depth[level];
	sm->fw = (float)sm->w;
	sm->fh = (float)sm->h;
	sm->fd = (float)sm->d;
	sm->clamp_u = u->clamp_s;
	sm->clamp_v = u->clamp_t;
	sm->clamp_w = u->clamp_r;
	sm->pow2_u = !(sm->w & (sm->w - 1));
	sm->pow2_v = !(sm->h & (sm->h - 1));
	sm->pow2_w = !(sm->d & (sm->d - 1));
	sm->bilinear = bilinear;
}

/* A whole texel coordinate clamped or wrapped into [0, n) */
static ALWAYS_INLINE int texel_wrap(int i, int n, bool clamp, bool pow2)
{
	if (clamp)
		return i < 0 ? 0 : i >= n ? n - 1 : i;
	if (pow2)
		return i & (n - 1);
	i %= n;
	return i < 0 ? i + n : i;
}

/* A texel coordinate from a scaled float one (NaN and huge values come out in range too) */
static ALWAYS_INLINE int texel(float f, int n, bool clamp, bool pow2)
{
	f = fminf(fmaxf(f, -1e9f), 1e9f);
	return texel_wrap((int)floorf(f), n, clamp, pow2);
}

static ALWAYS_INLINE vec4 sample(const struct sampler *sm, float u, float v)
{
	if (!sm->bilinear) {
		int x = texel(u * sm->fw, sm->w, sm->clamp_u, sm->pow2_u);
		int y = texel(v * sm->fh, sm->h, sm->clamp_v, sm->pow2_v);
		return unpack(sm->p[(size_t)y * (size_t)sm->w + (size_t)x]);
	}
	float fx = fminf(fmaxf(u * sm->fw - 0.5f, -1e9f), 1e9f), fy = fminf(fmaxf(v * sm->fh - 0.5f, -1e9f), 1e9f);
	float x0f = floorf(fx), y0f = floorf(fy), ax = fx - x0f, ay = fy - y0f;
	int xa, xb, ya, yb;
	if (fabsf(x0f) < 16777216.0f && fabsf(y0f) < 16777216.0f) {
		/* (whole numbers, and the next ones exactly: texel()'s clamping and floor change nothing) */
		int x = (int)x0f, y = (int)y0f;
		xa = texel_wrap(x, sm->w, sm->clamp_u, sm->pow2_u);
		xb = texel_wrap(x + 1, sm->w, sm->clamp_u, sm->pow2_u);
		ya = texel_wrap(y, sm->h, sm->clamp_v, sm->pow2_v);
		yb = texel_wrap(y + 1, sm->h, sm->clamp_v, sm->pow2_v);
	} else {
		xa = texel(x0f, sm->w, sm->clamp_u, sm->pow2_u);
		xb = texel(x0f + 1, sm->w, sm->clamp_u, sm->pow2_u);
		ya = texel(y0f, sm->h, sm->clamp_v, sm->pow2_v);
		yb = texel(y0f + 1, sm->h, sm->clamp_v, sm->pow2_v);
	}
	vec4 c00 = unpack(sm->p[(size_t)ya * (size_t)sm->w + (size_t)xa]);
	vec4 c10 = unpack(sm->p[(size_t)ya * (size_t)sm->w + (size_t)xb]);
	vec4 c01 = unpack(sm->p[(size_t)yb * (size_t)sm->w + (size_t)xa]);
	vec4 c11 = unpack(sm->p[(size_t)yb * (size_t)sm->w + (size_t)xb]);
	vec4 top = c00 + (c10 - c00) * ax, bottom = c01 + (c11 - c01) * ax;
	return top + (bottom - top) * ay;
}

/* A 3D texture's texel at (u, v, w): nearest, or trilinear (bilinear in each of the two slices nearest w) */
static vec4 sample3(const struct sampler *sm, float u, float v, float w)
{
	size_t slice = (size_t)sm->w * (size_t)sm->h;
	if (!sm->bilinear) {
		int x = texel(u * sm->fw, sm->w, sm->clamp_u, sm->pow2_u);
		int y = texel(v * sm->fh, sm->h, sm->clamp_v, sm->pow2_v);
		int z = texel(w * sm->fd, sm->d, sm->clamp_w, sm->pow2_w);
		return unpack(sm->p[(size_t)z * slice + (size_t)y * (size_t)sm->w + (size_t)x]);
	}
	float fz = fminf(fmaxf(w * sm->fd - 0.5f, -1e9f), 1e9f), z0f = floorf(fz), az = fz - z0f;
	int za = texel(z0f, sm->d, sm->clamp_w, sm->pow2_w), zb = texel(z0f + 1, sm->d, sm->clamp_w, sm->pow2_w);
	struct sampler s2 = *sm;
	s2.p = sm->p + (size_t)za * slice;
	vec4 a = sample(&s2, u, v);
	s2.p = sm->p + (size_t)zb * slice;
	vec4 b = sample(&s2, u, v);
	return a + (b - a) * az;
}

/* ---- fragments -------------------------------------------------------------- */

/* A stage's argument: its source, and whether that's its alpha in every channel, or 1 minus it all */
struct arg {
	uint8_t source;
	bool splat, invert;
};

/* A stage ready to run: its arguments for r, g and b, and for a; what else it needs */
struct run_stage {
	struct arg arg[2][3];
	bool plain;                             /* no argument splats or inverts */
	bool constant;                          /* the constant is an argument */
	bool scale;                             /* shifts */
	bool clamps;
	vec4i clamp;                            /* ... these channels (all bits) */
};

/* What drawing with a state takes, worked out once for the primitives drawn with it */
struct prepared {
	const struct raster_state *s;
	bool textures;                          /* textured primitives have stages (raster_textured()) */
	unsigned units;                         /* ... which sample these (bit k: unit k) */
	struct layout plain, textured;          /* where primitives' values are */
	struct run_stage stage[RASTER_STAGES];
};

/* How a primitive is shaded: where its values are, its units' textures at their levels, its stages */
struct shade {
	const struct layout *L;
	unsigned units;
	const struct run_stage *stage;
	struct sampler sm[RASTER_UNITS];
};

static void prepare(const struct raster_state *s, struct prepared *p)
{
	p->s = s;
	p->textures = raster_textured(s, true);
	p->units = p->textures ? raster_units_used(s) : 0;
	layout_of(s, false, &p->plain);
	layout_of(s, true, &p->textured);
	for (unsigned j = 0; p->textures && j < s->stages && j < RASTER_STAGES; j++) {
		const struct raster_stage *st = &s->stage[j];
		struct run_stage *r = &p->stage[j];
		r->plain = true;
		r->constant = (arg_sources(st->arg[0], st->op[0]) | arg_sources(st->arg[1], st->op[1])) >>
			      RASTER_CONSTANT & 1;
		r->scale = st->shift[0] || st->shift[1];
		r->clamps = st->clamp[0] || st->clamp[1];
		r->clamp = (vec4i){ st->clamp[1] ? -1 : 0, st->clamp[0] ? -1 : 0, st->clamp[0] ? -1 : 0,
				    st->clamp[0] ? -1 : 0 };
		for (int g = 0; g < 2; g++)
			for (int n = 0; n < 3; n++) {
				unsigned arg = st->arg[g][n], source = arg & 15u;
				struct arg *a = &r->arg[g][n];
				a->source = (uint8_t)(source < RASTER_SOURCES ? source : RASTER_PREVIOUS);
				a->splat = g == 0 && (arg & RASTER_ALPHA);      /* (a's are alphas anyway) */
				a->invert = arg & RASTER_INVERT;
				r->plain = r->plain && !a->splat && !a->invert;
			}
	}
}

static ALWAYS_INLINE vec4 arg_value(const vec4 *src, struct arg a, bool plain)
{
	vec4 v = src[a.source];
	if (plain)
		return v;
	if (a.splat)
		v = (vec4){ v[0], v[0], v[0], v[0] };
	return a.invert ? 1.0f - v : v;
}

static ALWAYS_INLINE vec4 stage_value(const vec4 *src, const struct arg arg[3], unsigned op, bool plain)
{
	vec4 x = arg_value(src, arg[0], plain);
	switch (op) {
	case RASTER_OP_MODULATE: return x * arg_value(src, arg[1], plain);
	case RASTER_OP_ADD: return x + arg_value(src, arg[1], plain);
	case RASTER_OP_ADD_SIGNED: return x + arg_value(src, arg[1], plain) - 0.5f;
	case RASTER_OP_INTERPOLATE: {
		vec4 z = arg_value(src, arg[2], plain);
		return x * z + arg_value(src, arg[1], plain) * (1.0f - z);
	}
	case RASTER_OP_SUBTRACT: return x - arg_value(src, arg[1], plain);
	default: return x;
	}
}

/* A stage's result for r, g and b, and for a */
static ALWAYS_INLINE vec4 stage_values(const vec4 *src, const struct raster_stage *st, const struct run_stage *r,
				       bool plain)
{
	vec4 v = stage_value(src, r->arg[0], st->op[0], plain);
	v[0] = stage_value(src, r->arg[1], st->op[1], plain)[0];
	return v;
}

/* A stage: its result is the next's previous */
static ALWAYS_INLINE void run_stage(vec4 *src, const struct raster_stage *st, const struct run_stage *r)
{
	if (r->constant)
		memcpy(&src[RASTER_CONSTANT], st->constant, sizeof *src);
	vec4 v = r->plain ? stage_values(src, st, r, true) : stage_values(src, st, r, false);
	if (r->scale)
		v *= (vec4){ (float)(1u << st->shift[1]), (float)(1u << st->shift[0]), (float)(1u << st->shift[0]),
			     (float)(1u << st->shift[0]) };
	if (r->clamps) {                /* (as clamp01(): NaN stays) */
		vec4i below = (v < 0.0f) & r->clamp, above = (v > 1.0f) & r->clamp;
		v = (vec4)(((vec4i)v & ~(below | above)) | ((vec4i)(vec4){ 1, 1, 1, 1 } & above));
	}
	src[RASTER_PREVIOUS] = v;
}

/* A textured fragment's color (a, r, g, b): its units sampled, then the stages */
static ALWAYS_INLINE void stages(const struct raster_state *s, const struct shade *sh, const float *a, float w,
				 float c[4])
{
	vec4 src[RASTER_SOURCES];
	for (unsigned k = 0; k < RASTER_COLORS; k++)
		if (sh->L->color[k])
			memcpy(&src[RASTER_COLOR0 + k], a + sh->L->color[k], sizeof *src);
	for (unsigned k = 0; k < RASTER_UNITS; k++) {
		if (!(sh->units >> k & 1))
			continue;
		const float *tc = a + sh->L->tex[k];
		float d = w;
		if (sh->L->q[k])
			d = a[sh->L->q[k]] != 0 ? 1.0f / a[sh->L->q[k]] : 1e30f;
		if (sh->L->r[k])
			src[RASTER_TEXTURE0 + k] = sample3(&sh->sm[k], tc[0] * d, tc[1] * d, a[sh->L->r[k]] * d);
		else
			src[RASTER_TEXTURE0 + k] = sample(&sh->sm[k], tc[0] * d, tc[1] * d);
	}
	src[RASTER_PREVIOUS] = src[RASTER_COLOR0];
	_Static_assert(RASTER_STAGES == 4, "the stages, one by one");
	if (s->stages > 0)
		run_stage(src, &s->stage[0], &sh->stage[0]);
	if (s->stages > 1)
		run_stage(src, &s->stage[1], &sh->stage[1]);
	if (s->stages > 2)
		run_stage(src, &s->stage[2], &sh->stage[2]);
	if (s->stages > 3)
		run_stage(src, &s->stage[3], &sh->stage[3]);
	memcpy(c, &src[RASTER_PREVIOUS], sizeof *src);
}

static ALWAYS_INLINE vec4 splat(float x)
{
	return (vec4){ x, x, x, x };
}

/* An OpenGL-style blend factor for each of a, r, g, b */
static ALWAYS_INLINE vec4 blend_factor(unsigned f, vec4 src, vec4 dst, vec4 constant)
{
	switch (f) {
	case RASTER_F_ZERO: return splat(0);
	case RASTER_F_ONE: return splat(1);
	case RASTER_F_SRC_COLOR: return src;
	case RASTER_F_ONE_MINUS_SRC_COLOR: return 1.0f - src;
	case RASTER_F_SRC_ALPHA: return splat(src[0]);
	case RASTER_F_ONE_MINUS_SRC_ALPHA: return splat(1 - src[0]);
	case RASTER_F_DST_ALPHA: return splat(dst[0]);
	case RASTER_F_ONE_MINUS_DST_ALPHA: return splat(1 - dst[0]);
	case RASTER_F_DST_COLOR: return dst;
	case RASTER_F_ONE_MINUS_DST_COLOR: return 1.0f - dst;
	case RASTER_F_CONSTANT_COLOR: return constant;
	case RASTER_F_ONE_MINUS_CONSTANT_COLOR: return 1.0f - constant;
	case RASTER_F_CONSTANT_ALPHA: return splat(constant[0]);
	case RASTER_F_ONE_MINUS_CONSTANT_ALPHA: return splat(1 - constant[0]);
	default: {                              /* source alpha saturate */
		float v = fminf(src[0], 1 - dst[0]);
		return (vec4){ 1, v, v, v };
	}
	}
}

static ALWAYS_INLINE void blend_factors(const struct raster_state *s, float c[4], const uint32_t *dst)
{
	/*
	 * (one and zero, added: the color as it is, but -0 for +0, which packs
	 * the same; the target not read)
	 */
	if (s->blend_src == RASTER_F_ONE && s->blend_dst == RASTER_F_ZERO && s->blend_eq == RASTER_EQ_ADD)
		return;
	vec4 d = unpack(*dst), cv;
	if (s->blend_eq == RASTER_EQ_MIN || s->blend_eq == RASTER_EQ_MAX) {
		for (int k = 0; k < 4; k++)
			c[k] = s->blend_eq == RASTER_EQ_MIN ? fminf(c[k], d[k]) : fmaxf(c[k], d[k]);
		return;
	}
	memcpy(&cv, c, sizeof cv);
	vec4 constant;
	memcpy(&constant, s->blend_color, sizeof constant);
	vec4 sf = blend_factor(s->blend_src, cv, d, constant), df = blend_factor(s->blend_dst, cv, d, constant);
	cv = s->blend_eq == RASTER_EQ_SUBTRACT ? cv * sf - d * df :
	     s->blend_eq == RASTER_EQ_REVERSE_SUBTRACT ? d * df - cv * sf : cv * sf + d * df;
	memcpy(c, &cv, sizeof cv);
}

/* Compile-time variants of the pixel loop (and run-time ones: OpenGL's points and lines, stippled triangles) */
#define F_TEX 1u
#define F_FOG 2u
#define F_COVER 4u                      /* alpha times the coverage (a[A_COVER]) */
#define F_STIPPLE 8u                    /* the polygon stipple */
#define A_COVER (A_FOG + 1)

/* A fragment's color (a, r, g, b) from the interpolated attributes (a) and, if textured, the stages; fogged */
static ALWAYS_INLINE void color_of(const struct raster_state *s, const float *a, const struct shade *sh,
				   unsigned flags, float c[4])
{
	float w = 0;
	if (flags & (F_TEX | F_FOG))
		w = a[A_W] != 0 ? 1.0f / a[A_W] : 1e30f;
	if (flags & F_TEX)
		stages(s, sh, a, w, c);
	else
		memcpy(c, a + A_C0, 4 * sizeof *c);

	if (flags & F_FOG) {
		float f, z = s->fog_coord ? a[A_FOG] * w : w, d = s->fog_density;
		switch (s->fog_mode) {
		case RASTER_FOG_ALPHA:
			f = c[0];
			c[0] = 1;
			break;
		case RASTER_FOG_LINEAR:
			f = z < s->fog_start ? 1 : z > s->fog_end ? 0 : (s->fog_end - z) / (s->fog_end - s->fog_start);
			break;
		case RASTER_FOG_EXP:
			f = expf(-d * z);
			break;
		default:
			f = expf(-(d * z) * (d * z));
			break;
		}
		f = clamp01(f);
		for (int k = 1; k < 4; k++)
			c[k] = f * c[k] + (1 - f) * s->fog_color[k];
	}
	if (flags & F_COVER)
		c[0] *= a[A_COVER];
}

/* OpenGL's logic operation of a color's bits (s) with the target's (d) */
static uint32_t logic(unsigned op, uint32_t s, uint32_t d)
{
	switch (op) {
	case RASTER_LOGIC_CLEAR: return 0;
	case RASTER_LOGIC_AND: return s & d;
	case RASTER_LOGIC_AND_REVERSE: return s & ~d;
	case RASTER_LOGIC_COPY: return s;
	case RASTER_LOGIC_AND_INVERTED: return ~s & d;
	case RASTER_LOGIC_NOOP: return d;
	case RASTER_LOGIC_XOR: return s ^ d;
	case RASTER_LOGIC_OR: return s | d;
	case RASTER_LOGIC_NOR: return ~(s | d);
	case RASTER_LOGIC_EQUIV: return ~(s ^ d);
	case RASTER_LOGIC_INVERT: return ~d;
	case RASTER_LOGIC_OR_REVERSE: return s | ~d;
	case RASTER_LOGIC_COPY_INVERTED: return ~s;
	case RASTER_LOGIC_OR_INVERTED: return ~s | d;
	case RASTER_LOGIC_NAND: return ~(s & d);
	default: return 0xFFFFFFFFu;
	}
}

/* A fragment of color c that's passed the tests, into the target at index i */
static ALWAYS_INLINE void put(struct raster_target *t, const struct raster_state *s, size_t i, float *c, float depth)
{
	/*
	 * Fully transparent fragments leave depth alone (as hardware engines do with
	 * transparent texels: what's behind them, drawn later, still shows), and
	 * color too unless they add some.
	 */
	if (!s->all_fragments && c[0] <= 0 &&
	    (s->blend == RASTER_BLEND_INTERPOLATE || (c[1] <= 0 && c[2] <= 0 && c[3] <= 0)))
		return;
	if (s->z_write && s->z_func != RASTER_Z_NONE && (c[0] > 0 || s->all_fragments))
		t->depth[i] = depth;
	if (s->blend == RASTER_BLEND_FACTORS) {
		blend_factors(s, c, &t->color[i]);
	} else if (s->blend == RASTER_BLEND_LOGIC) {
		uint32_t p = logic(s->logic_op, pack(c), t->color[i]);
		t->color[i] = (p & ~s->color_keep) | (t->color[i] & s->color_keep);
		return;
	} else if (s->blend != RASTER_BLEND_NONE && c[0] < 1) {
		vec4 d = unpack(t->color[i]);
		float as = clamp01(c[0]);
		if (s->blend == RASTER_BLEND_INTERPOLATE)
			for (int k = 1; k < 4; k++)
				c[k] = as * c[k] + (1 - as) * d[k];
		else
			for (int k = 1; k < 4; k++)
				c[k] = c[k] + (1 - as) * d[k];
		c[0] = 1 - (1 - as) * (1 - d[0]);
	}
	uint32_t p = pack(c);
	if (s->color_keep)
		p = (p & ~s->color_keep) | (t->color[i] & s->color_keep);
	t->color[i] = p;
}

/*
 * One fragment: its color, alpha test, then blended into the target at
 * index i (whose depth test has passed)
 */
static ALWAYS_INLINE void fragment(struct raster_target *t, const struct raster_state *s, size_t i, const float *a,
				   const struct shade *sh, unsigned flags, float depth)
{
	float c[4];
	color_of(s, a, sh, flags, c);
	if (s->alpha_test != RASTER_ALPHA_NONE && !alpha_pass(s->alpha_test, c[0], s->alpha_ref))
		return;
	put(t, s, i, c, depth);
}

static uint8_t stencil_op(unsigned op, uint8_t v, uint8_t ref)
{
	switch (op) {
	case RASTER_STENCIL_ZERO: return 0;
	case RASTER_STENCIL_REPLACE: return ref;
	case RASTER_STENCIL_INCR: return v < 255 ? (uint8_t)(v + 1) : v;
	case RASTER_STENCIL_DECR: return v > 0 ? (uint8_t)(v - 1) : v;
	case RASTER_STENCIL_INVERT: return (uint8_t)~v;
	case RASTER_STENCIL_INCR_WRAP: return (uint8_t)(v + 1);
	case RASTER_STENCIL_DECR_WRAP: return (uint8_t)(v - 1);
	default: return v;
	}
}

/* A fragment with stenciling: the alpha test, the stencil test, the z test, the stencil updated, then drawn */
static void stenciled(struct raster_target *t, const struct raster_state *s, size_t i, const float *a,
		      const struct shade *sh, unsigned flags, float depth)
{
	float c[4];
	color_of(s, a, sh, flags, c);
	if (s->alpha_test != RASTER_ALPHA_NONE && !alpha_pass(s->alpha_test, c[0], s->alpha_ref))
		return;
	uint8_t *st = &t->stencil[i], m = s->stencil_mask;
	bool stencil_pass = z_pass(s->stencil_func, (float)(s->stencil_ref & m), (float)(*st & m));
	bool z_ok = stencil_pass && (s->z_func == RASTER_Z_NONE || z_pass(s->z_func, depth, t->depth[i]));
	unsigned op = s->stencil_op[!stencil_pass ? 0 : !z_ok ? 1 : 2];
	*st = (uint8_t)((*st & ~s->stencil_write) | (stencil_op(op, *st, s->stencil_ref) & s->stencil_write));
	if (z_ok)
		put(t, s, i, c, depth);
}

/* A fragment through the tests the state has, in OpenGL's order, and drawn if it passes */
static ALWAYS_INLINE void test(struct raster_target *t, const struct raster_state *s, size_t i, const float *a,
			       const struct shade *sh, unsigned flags, float depth)
{
	if (s->stencil_func != RASTER_Z_NONE && t->stencil)
		stenciled(t, s, i, a, sh, flags, depth);
	else if (s->z_func == RASTER_Z_NONE || z_pass(s->z_func, depth, t->depth[i]))
		fragment(t, s, i, a, sh, flags, depth);
}

/* ---- triangles -------------------------------------------------------------- */

/*
 * A unit's level of detail over a triangle: log2 of texels per pixel, from
 * the areas the triangle covers in the texture and on the screen (0 if
 * either is none)
 */
static float triangle_lod(const struct raster_unit *u, unsigned unit, const struct raster_vertex *v[3], float area2)
{
	const struct raster_texture *tx = u->texture;
	float s[3], t[3];
	for (int k = 0; k < 3; k++) {
		float iw = u->projective ? v[k]->tex[unit][2] : v[k]->invw;
		float inv = iw != 0 ? 1.0f / iw : 0;
		s[k] = v[k]->tex[unit][0] * inv * (float)tx->width[0];
		t[k] = v[k]->tex[unit][1] * inv * (float)tx->height[0];
	}
	float tarea = fabsf((s[1] - s[0]) * (t[2] - t[0]) - (s[2] - s[0]) * (t[1] - t[0]));
	if (!(tarea > 0) || !(area2 > 0))
		return 0;
	return 0.5f * log2f(tarea / area2);
}

static bool linear_filter(unsigned f)
{
	return f == RASTER_LINEAR || f == RASTER_LINEAR_MIPMAP_NEAREST || f == RASTER_LINEAR_MIPMAP_LINEAR;
}

/*
 * A unit's texture over a triangle: magnified or minified, and if minified
 * and mipmapped, at the level nearest its level of detail
 */
static bool lod_needed(const struct raster_unit *u)
{
	return (u->min_filter >= RASTER_NEAREST_MIPMAP_NEAREST && u->texture->levels > 1) ||
	       linear_filter(u->min_filter) != (u->mag_filter == RASTER_LINEAR);
}

/* (at that level of detail, if lod_needed()) */
static void unit_setup_lod(const struct raster_unit *u, float lod, struct sampler *sm)
{
	const struct raster_texture *tx = u->texture;
	bool mipmap = u->min_filter >= RASTER_NEAREST_MIPMAP_NEAREST && tx->levels > 1;
	bool min_linear = linear_filter(u->min_filter), mag_linear = u->mag_filter == RASTER_LINEAR;
	bool linear = mag_linear;
	unsigned level = 0;
	if (mipmap || min_linear != mag_linear) {
		float edge = mag_linear && (u->min_filter == RASTER_NEAREST_MIPMAP_NEAREST ||
					    u->min_filter == RASTER_NEAREST_MIPMAP_LINEAR) ? 0.5f : 0;
		if (lod > edge) {
			linear = min_linear;
			if (mipmap) {
				level = (unsigned)(lod + 0.5f);
				level = level < tx->levels ? level : tx->levels - 1;
			}
		}
	}
	sampler_init(sm, u, level, linear);
}

static void unit_setup(const struct raster_unit *u, unsigned unit, const struct raster_vertex *v[3], float area2,
		       struct sampler *sm)
{
	unit_setup_lod(u, lod_needed(u) ? triangle_lod(u, unit, v, area2) : 0, sm);
}

/*
 * A triangle set up for drawing. Its coverage is exact: the vertices snap
 * to 1/256 of a pixel (as GPUs' and Mesa's do) and the edge functions are
 * 64-bit integers there, so of two triangles sharing an edge, exactly one
 * covers a pixel whose center is on it: the one whose edge is a top or a
 * left one (Direct3D's and Mesa's tie-break). Attributes are interpolated
 * from the vertices as given.
 */
#define SUBPIXEL_BITS 8
#define SNAP_LIMIT 2097152.0f           /* (vertices within 2^21 pixels: the edge functions stay within 2^61) */

struct tri {
	/* edge functions at the pixel center (x0, y0), less 1 if not a top or left edge (so >= 0 covers); steps */
	int64_t ec[3], ex[3], ey[3];
	float a0[A_MAX], ax[A_MAX], ay[A_MAX];  /* attributes at (ox, oy) and their gradients */
	float ox, oy;
	int x0, x1, y0, y1;             /* pixels that may be covered */
	struct shade sh;
};

static bool tri_setup(const struct raster_target *t, const struct prepared *p, const int rows[2],
		      const struct raster_vertex *v0, const struct raster_vertex *v1, const struct raster_vertex *v2,
		      bool textured, struct tri *T)
{
	const struct raster_state *s = p->s;
	const struct raster_vertex *v[3] = { v0, v1, v2 };
	float area2 = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
	if (!(area2 != 0) || !isfinite(area2))
		return false;
	int64_t X[3], Y[3];
	for (int k = 0; k < 3; k++) {
		if (!(fabsf(v[k]->x) < SNAP_LIMIT && fabsf(v[k]->y) < SNAP_LIMIT))
			return false;
		X[k] = llrintf(v[k]->x * (float)(1 << SUBPIXEL_BITS));
		Y[k] = llrintf(v[k]->y * (float)(1 << SUBPIXEL_BITS));
	}
	int64_t snapped = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
	if (snapped == 0 || (snapped > 0) != (area2 > 0))
		return false;           /* (a sliver that snapping made empty or turned over) */
	if (area2 < 0) {                /* one winding for the edge functions */
		v[1] = v2;
		v[2] = v1;
		area2 = -area2;
		int64_t x = X[1], y = Y[1];
		X[1] = X[2];
		Y[1] = Y[2];
		X[2] = x;
		Y[2] = y;
	}
	int l, top, r, b;
	clip_rows(t, s, rows, &l, &top, &r, &b);
	float minx = fminf(v[0]->x, fminf(v[1]->x, v[2]->x)), maxx = fmaxf(v[0]->x, fmaxf(v[1]->x, v[2]->x));
	float miny = fminf(v[0]->y, fminf(v[1]->y, v[2]->y)), maxy = fmaxf(v[0]->y, fmaxf(v[1]->y, v[2]->y));
	if (!(minx < (float)r && maxx > (float)l && miny < (float)b && maxy > (float)top))
		return false;
	T->x0 = minx > (float)l ? (int)floorf(minx) : l;        /* (snapping moves no pixel center out of the box) */
	T->x1 = maxx < (float)r ? (int)ceilf(maxx) : r;
	T->y0 = miny > (float)top ? (int)floorf(miny) : top;
	T->y1 = maxy < (float)b ? (int)ceilf(maxy) : b;

	/* edge i: from v[i+1] to v[i+2], its function v[i]'s barycentric weight times the snapped area2 */
	const int64_t one = 1 << SUBPIXEL_BITS;
	int64_t cx = T->x0 * one + one / 2, cy = T->y0 * one + one / 2;
	for (int i = 0; i < 3; i++) {
		int pi = (i + 1) % 3, qi = (i + 2) % 3;
		int64_t dx = X[qi] - X[pi], dy = Y[qi] - Y[pi];
		bool top_left = dy < 0 || (dy == 0 && dx > 0);
		/* e(x, y) = dx * (y - p.y) - dy * (x - p.x) */
		T->ec[i] = dx * (cy - Y[pi]) - dy * (cx - X[pi]) - (top_left ? 0 : 1);
		T->ex[i] = -dy * one;
		T->ey[i] = dx * one;
	}

	const struct layout *L = T->sh.L = textured ? &p->textured : &p->plain;
	float av[3][A_MAX];
	for (int k = 0; k < 3; k++)
		attribs(v[k], L, av[k]);
	float dx1 = v[1]->x - v[0]->x, dy1 = v[1]->y - v[0]->y, dx2 = v[2]->x - v[0]->x, dy2 = v[2]->y - v[0]->y;
	for (unsigned k = 0; k < L->n; k++) {
		float d1 = av[1][k] - av[0][k], d2 = av[2][k] - av[0][k];
		T->ax[k] = (d1 * dy2 - d2 * dy1) / area2;
		T->ay[k] = (d2 * dx1 - d1 * dx2) / area2;
		T->a0[k] = av[0][k];
	}
	for (unsigned k = L->n; k < A_MAX; k++)        /* (tri_rows() steps whole vectors) */
		T->ax[k] = T->ay[k] = T->a0[k] = 0;
	T->ox = v[0]->x;
	T->oy = v[0]->y;
	T->sh.units = textured ? p->units : 0;
	T->sh.stage = p->stage;
	for (unsigned k = 0; k < RASTER_UNITS; k++)
		if (T->sh.units >> k & 1)
			unit_setup(&s->unit[k], k, v, area2, &T->sh.sm[k]);
	return true;
}

static ALWAYS_INLINE void tri_rows(struct raster_target *t, const struct raster_state *s, const struct tri *T,
				   unsigned flags)
{
	bool ztest = s->z_func != RASTER_Z_NONE, stencil = s->stencil_func != RASTER_Z_NONE && t->stencil;
	const unsigned na = flags & F_TEX ? A_MAX : A_PLAIN;
	for (int y = T->y0; y < T->y1; y++) {
		float py = (float)y + 0.5f;
		/* the pixels every edge function covers (each is linear in x, so they're a span) */
		int64_t xs = 0, xe = T->x1 - T->x0;     /* (from x0) */
		for (int i = 0; i < 3 && xs < xe; i++) {
			int64_t e = T->ec[i] + T->ey[i] * (y - T->y0);
			if (T->ex[i] > 0 && e < 0) {
				int64_t k = (-e + T->ex[i] - 1) / T->ex[i];
				xs = k > xs ? k : xs;
			} else if (T->ex[i] < 0) {
				int64_t k = e < 0 ? -1 : e / -T->ex[i];
				xe = k + 1 < xe ? k + 1 : xe;
			} else if (e < 0) {
				xe = xs;
			}
		}
		if (xs >= xe)
			continue;
		int xa = T->x0 + (int)xs, xb = T->x0 + (int)xe;
		/* (interpolation starts a pixel before the span, within the box, as it always has here: pixels round
		 * as they always have) */
		int xo = xa > T->x0 ? xa - 1 : xa;
		float px = (float)xo + 0.5f;
		float a[A_MAX];
		for (unsigned k = 0; k < na; k++)
			a[k] = T->a0[k] + T->ax[k] * (px - T->ox) + T->ay[k] * (py - T->oy);
		if (xo < xa)
			for (unsigned k = 0; k < na; k++)
				a[k] += T->ax[k];
		size_t row = (size_t)y * (size_t)t->width;
		uint32_t pattern = flags & F_STIPPLE ? s->polygon_stipple[(uint32_t)(t->height - 1 - y) & 31u] : 0;
		for (int x = xa; x < xb; x++) {
			size_t i = row + (size_t)x;
			float depth = s->perspective_z ? -a[A_W] : a[A_Z];
			if (!(flags & F_STIPPLE) || pattern >> (31u - ((uint32_t)x & 31u)) & 1) {
				if (stencil)
					stenciled(t, s, i, a, &T->sh, flags, depth);
				else if (!ztest || z_pass(s->z_func, depth, t->depth[i]))
					fragment(t, s, i, a, &T->sh, flags, depth);
			}
			for (unsigned k = 0; k < na; k++)
				a[k] += T->ax[k];
		}
	}
}

/* (the variants that are seldom drawn: the flags looked at as it goes) */
static void tri_rows_any(struct raster_target *t, const struct raster_state *s, const struct tri *T, unsigned flags)
{
	tri_rows(t, s, T, flags);
}

static void triangle(struct raster_target *t, const struct prepared *p, const int rows[2],
		     const struct raster_vertex *v0, const struct raster_vertex *v1, const struct raster_vertex *v2,
		     bool textured)
{
	const struct raster_state *s = p->s;
	textured = textured && p->textures;
	struct tri T;
	if (!tri_setup(t, p, rows, v0, v1, v2, textured, &T))
		return;
	unsigned flags = (textured ? F_TEX : 0) | (s->fog_mode != RASTER_FOG_NONE ? F_FOG : 0) |
			 (s->polygon_stippled ? F_STIPPLE : 0);
	switch (flags) {
	case 0: tri_rows(t, s, &T, 0); break;
	case F_TEX: tri_rows(t, s, &T, F_TEX); break;
	case F_FOG: tri_rows(t, s, &T, F_FOG); break;
	case F_TEX | F_FOG: tri_rows(t, s, &T, F_TEX | F_FOG); break;
	default: tri_rows_any(t, s, &T, flags); break;
	}
}

void raster_triangle(struct raster_target *t, const struct raster_state *s, const struct raster_vertex *v0,
		     const struct raster_vertex *v1, const struct raster_vertex *v2, bool textured)
{
	const int rows[2] = { s->clip_top, s->clip_bottom };
	struct prepared p;
	prepare(s, &p);
	triangle(t, &p, rows, v0, v1, v2, textured);
}

/* ---- OpenGL's points and lines ------------------------------------------------ */

/*
 * (Window coordinates here go down, as raster.h's: OpenGL's rules, which
 * have them going up, are turned over. In particular OpenGL breaks ties by
 * moving lines' endpoints by (-e, -e^2), which is (-e, +e^2) here.)
 */

/* Where fragments go: through fn, within the clip; a line's left out by its stipple */
struct gl_emit {
	const struct raster_state *s;
	int l, t, r, b;
	raster_fragment_fn *fn;
	void *ctx;
	unsigned counter;               /* (the line's stipple counter at its first fragment) */
};

/* A fragment (of a line, k fragments along it: stippled; of a point, k is -1) */
static void gl_emit(const struct gl_emit *e, int x, int y, float t, float cover, long k)
{
	if (!e->fn || x < e->l || x >= e->r || y < e->t || y >= e->b)
		return;
	if (k >= 0 && e->s->line_stippled) {
		unsigned f = e->s->line_stipple_factor ? e->s->line_stipple_factor : 1;
		if (!(e->s->line_stipple >> ((e->counter + (unsigned long)k) / f % 16) & 1))
			return;
	}
	e->fn(e->ctx, x, y, t, cover);
}

/* A width or size as aliased points and lines take it: rounded, at least 1 */
static int whole_width(float w)
{
	return !(w < 1024) ? 1024 : w < 1.5f ? 1 : (int)(w + 0.5f);
}

/* Whether the perturbed point (x, y) is in pixel (i, j)'s diamond, the points less than half a pixel away in x + y */
static bool in_diamond(double x, double y, int i, int j)
{
	double dx = x - ((double)i + 0.5), dy = y - ((double)j + 0.5), d = fabs(dx) + fabs(dy);
	return d < 0.5 || (d == 0.5 && dx > 0);        /* (on its edge: in if moving left takes it in) */
}

/* The pixel whose diamond the perturbed point is in; false if none (near pixels' corners) */
static bool diamond_of(double x, double y, int *i, int *j)
{
	int x0 = (int)floor(x), y0 = (int)floor(y);
	for (int dj = -1; dj <= 1; dj++)
		for (int di = -1; di <= 1; di++)
			if (in_diamond(x, y, x0 + di, y0 + dj)) {
				*i = x0 + di;
				*j = y0 + dj;
				return true;
			}
	return false;
}

/* Where along a line from a to b a fragment's center is (OpenGL 1.2's t), within 0..1 */
static float along(double xa, double ya, double xb, double yb, double x, double y)
{
	double dx = xb - xa, dy = yb - ya, l2 = dx * dx + dy * dy;
	double t = l2 > 0 ? ((x - xa) * dx + (y - ya) * dy) / l2 : 0;
	return t < 0 ? 0.0f : t > 1 ? 1.0f : (float)t;
}

/*
 * An aliased line (OpenGL 1.2's 3.4.2): the pixels whose diamonds the
 * segment, its endpoints perturbed, goes through, but for the one its end
 * is in; wider (n), moved (n - 1) / 2 down (x major) or left (y major), each
 * such pixel the first of a column up or a row right of n
 */
static unsigned gl_line(const struct gl_emit *e, double xa, double ya, double xb, double yb, int n)
{
	double dx = xb - xa, dy = yb - ya;
	if (dx == 0 && dy == 0)
		return 0;
	bool xmajor = fabs(dx) >= fabs(dy);
	double shift = (double)(n - 1) * 0.5;
	double sxa = xmajor ? xa : xa - shift, sxb = xmajor ? xb : xb - shift;
	double sya = xmajor ? ya + shift : ya, syb = xmajor ? yb + shift : yb;
	int si = 0, sj = 0, ei = 0, ej = 0;
	bool start = diamond_of(sxa, sya, &si, &sj), end = diamond_of(sxb, syb, &ei, &ej);
	double lo = xmajor ? fmin(sxa, sxb) : fmin(sya, syb), hi = xmajor ? fmax(sxa, sxb) : fmax(sya, syb);
	/* the centers crossed: x major [lo, hi) (moved left), y major (lo, hi] (moved down) */
	long c0 = xmajor ? (long)ceil(lo - 0.5) : (long)floor(lo - 0.5) + 1;
	long c1 = xmajor ? (long)ceil(hi - 0.5) - 1 : (long)floor(hi - 0.5);
	bool forward = xmajor ? dx > 0 : dy > 0;
	long k = 0;
	if (start && !(xmajor ? si >= c0 && si <= c1 : sj >= c0 && sj <= c1) && !(end && si == ei && sj == ej)) {
		float t = along(sxa, sya, sxb, syb, (double)si + 0.5, (double)sj + 0.5);
		for (int w = 0; w < n; w++)
			gl_emit(e, xmajor ? si : si + w, xmajor ? sj - w : sj, t, 1, k);
		k++;
	}
	for (long m = 0; m <= c1 - c0; m++) {
		long c = forward ? c0 + m : c1 - m;
		double cc = (double)c + 0.5;
		int i, j;
		if (xmajor) {
			double yv = sya + (cc - sxa) * dy / dx;
			i = (int)c;
			j = (int)(dy / dx < 0 ? ceil(yv) - 1 : floor(yv));
		} else {
			double xv = sxa + (cc - sya) * dx / dy;
			i = (int)(ceil(xv) - 1);
			j = (int)c;
		}
		if (end && i == ei && j == ej)
			continue;
		float t = along(sxa, sya, sxb, syb, (double)i + 0.5, (double)j + 0.5);
		for (int w = 0; w < n; w++)
			gl_emit(e, xmajor ? i : i + w, xmajor ? j - w : j, t, 1, k);
		k++;
	}
	return (unsigned)k;
}

/* The part of polygon p (n points, x and y) where a x + b y + c >= 0, into out; its points */
static int clip_half(const double *p, int n, double a, double b, double c, double *out)
{
	int m = 0;
	for (int k = 0; k < n; k++) {
		const double *u = p + 2 * k, *v = p + 2 * ((k + 1) % n);
		double du = a * u[0] + b * u[1] + c, dv = a * v[0] + b * v[1] + c;
		if (du >= 0) {
			out[2 * m] = u[0];
			out[2 * m + 1] = u[1];
			m++;
		}
		if ((du >= 0) != (dv >= 0)) {
			double f = du / (du - dv);
			out[2 * m] = u[0] + (v[0] - u[0]) * f;
			out[2 * m + 1] = u[1] + (v[1] - u[1]) * f;
			m++;
		}
	}
	return m;
}

/*
 * A smooth line: the pixels a rectangle of the width along the segment
 * covers some of, each by the area covered (its square clipped to the
 * rectangle); stippled by how far along the major axis its center is
 */
static unsigned gl_smooth_line(const struct gl_emit *e, double xa, double ya, double xb, double yb, double width)
{
	double dx = xb - xa, dy = yb - ya, len = sqrt(dx * dx + dy * dy);
	if (!(len > 0))
		return 0;
	double ux = dx / len, uy = dy / len, hw = width * 0.5, major = fmax(fabs(dx), fabs(dy));
	double px = -uy * hw, py = ux * hw;
	double xmin = fmin(xa, xb) - fabs(px), xmax = fmax(xa, xb) + fabs(px);
	double ymin = fmin(ya, yb) - fabs(py), ymax = fmax(ya, yb) + fabs(py);
	for (int j = (int)floor(ymin); j <= (int)floor(ymax); j++)
		for (int i = (int)floor(xmin); i <= (int)floor(xmax); i++) {
			double sq[16] = { i, j, i + 1, j, i + 1, j + 1, i, j + 1 }, a[16], b[16];
			int m = clip_half(sq, 4, ux, uy, -(ux * xa + uy * ya), a);         /* from a on */
			m = clip_half(a, m, -ux, -uy, ux * xb + uy * yb, b);               /* to b */
			m = clip_half(b, m, -uy, ux, uy * xa - ux * ya + hw, a);           /* within the width */
			m = clip_half(a, m, uy, -ux, -uy * xa + ux * ya + hw, b);
			double area = 0;
			for (int k = 0; k < m; k++) {
				const double *u = b + 2 * k, *v = b + 2 * ((k + 1) % m);
				area += u[0] * v[1] - v[0] * u[1];
			}
			area = fabs(area) * 0.5;
			if (!(area > 0))
				continue;
			double cx = i + 0.5, cy = j + 0.5, d = (cx - xa) * ux + (cy - ya) * uy;
			long k = d > 0 ? (long)floor(d / len * major) : 0;
			gl_emit(e, i, j, along(xa, ya, xb, yb, cx, cy), area < 1 ? (float)area : 1.0f, k);
		}
	return (unsigned)floor(major + 0.5);
}

/* Of a pixel, the part a disk covers (to 1/256: its square sampled 16 x 16) */
static float disk_cover(double cx, double cy, double r, int i, int j)
{
	double nx = fmax(i - cx, fmax(0, cx - (i + 1))), ny = fmax(j - cy, fmax(0, cy - (j + 1)));
	if (nx * nx + ny * ny >= r * r)
		return 0;
	double fx = fmax(fabs(i - cx), fabs(i + 1 - cx)), fy = fmax(fabs(j - cy), fabs(j + 1 - cy));
	if (fx * fx + fy * fy <= r * r)
		return 1;
	int in = 0;
	for (int v = 0; v < 16; v++)
		for (int u = 0; u < 16; u++) {
			double x = i + (u + 0.5) / 16 - cx, y = j + (v + 0.5) / 16 - cy;
			in += x * x + y * y < r * r;
		}
	return (float)in / 256;
}

/*
 * A point (OpenGL 1.2's 3.3): aliased, an n x n square whose center is the
 * pixel's the point is in (n odd) or the pixel corner nearest it (n even);
 * smooth, the pixels a disk of the size covers some of
 */
static void gl_point(const struct gl_emit *e, const struct raster_vertex *v)
{
	double x = v->x, y = v->y;
	if (e->s->smooth) {
		double r = e->s->width * 0.5;
		if (!(r > 0) || !(r < 512))
			return;
		for (int j = (int)floor(y - r); j <= (int)floor(y + r); j++)
			for (int i = (int)floor(x - r); i <= (int)floor(x + r); i++) {
				float c = disk_cover(x, y, r, i, j);
				if (c > 0)
					gl_emit(e, i, j, 0, c, -1);
			}
		return;
	}
	int n = whole_width(e->s->width);
	int x0 = n & 1 ? (int)floor(x) - (n - 1) / 2 : (int)floor(x + 0.5) - n / 2;
	int y0 = n & 1 ? (int)ceil(y) - (n + 1) / 2 : (int)ceil(y - 0.5) - n / 2;
	for (int j = y0; j < y0 + n; j++)
		for (int i = x0; i < x0 + n; i++)
			gl_emit(e, i, j, 0, 1, -1);
}

unsigned raster_gl_fragments(const struct raster_state *s, const struct raster_cmd *c, int left, int top, int right,
			     int bottom, raster_fragment_fn *fn, void *ctx)
{
	const struct gl_emit e = { s, left, top, right, bottom, fn, ctx, c->stipple };
	const struct raster_vertex *a = &c->v[0], *b = &c->v[1];
	if (!(fabsf(a->x) < 1e6f && fabsf(a->y) < 1e6f))
		return 0;
	if (c->kind == RASTER_CMD_POINT) {
		gl_point(&e, a);
		return 0;
	}
	if (c->kind != RASTER_CMD_LINE || !(fabsf(b->x) < 1e6f && fabsf(b->y) < 1e6f))
		return 0;
	if (s->smooth)
		return s->width > 0 && s->width < 1024 ? gl_smooth_line(&e, a->x, a->y, b->x, b->y, s->width) : 0;
	return gl_line(&e, a->x, a->y, b->x, b->y, whole_width(s->width));
}

void raster_lerp_vertex(struct raster_vertex *out, const struct raster_vertex *a, const struct raster_vertex *b,
			float t)
{
	out->z = a->z + (b->z - a->z) * t;
	out->invw = a->invw + (b->invw - a->invw) * t;
	for (unsigned k = 0; k < RASTER_COLORS; k++)
		for (int j = 0; j < 4; j++)
			out->color[k][j] = a->color[k][j] + (b->color[k][j] - a->color[k][j]) * t;
	out->fog = a->fog + (b->fog - a->fog) * t;
	for (unsigned u = 0; u < RASTER_UNITS; u++) {
		for (int j = 0; j < 3; j++)
			out->tex[u][j] = a->tex[u][j] + (b->tex[u][j] - a->tex[u][j]) * t;
		out->texr[u] = a->texr[u] + (b->texr[u] - a->texr[u]) * t;
	}
}

float raster_line_lod(const struct raster_unit *u, unsigned unit, const struct raster_vertex *v0,
		      const struct raster_vertex *v1)
{
	const struct raster_texture *tx = u->texture;
	const struct raster_vertex *v[2] = { v0, v1 };
	float s[2], t[2];
	for (int k = 0; k < 2; k++) {
		float iw = u->projective ? v[k]->tex[unit][2] : v[k]->invw;
		float inv = iw != 0 ? 1.0f / iw : 0;
		s[k] = v[k]->tex[unit][0] * inv * (float)tx->width[0];
		t[k] = v[k]->tex[unit][1] * inv * (float)tx->height[0];
	}
	float dx = v1->x - v0->x, dy = v1->y - v0->y, ds = s[1] - s[0], dt = t[1] - t[0];
	float len2 = dx * dx + dy * dy, rho2 = ds * ds + dt * dt;
	if (!(len2 > 0) || !(rho2 > 0) || !(rho2 < 1e30f))
		return 0;
	return 0.5f * log2f(rho2 / len2);
}

/* Drawing them here: each fragment's values from v[0]'s to v[1]'s, then as a triangle's */
struct gl_draw {
	struct raster_target *t;
	const struct raster_state *s;
	struct shade sh;
	unsigned flags;
	float a0[A_MAX], a1[A_MAX];
};

static void gl_fragment(void *ctx, int x, int y, float t, float cover)
{
	const struct gl_draw *d = ctx;
	float a[A_MAX];
	for (unsigned k = 0; k < d->sh.L->n; k++)
		a[k] = d->a0[k] + (d->a1[k] - d->a0[k]) * t;
	a[A_COVER] = cover;
	size_t i = (size_t)y * (size_t)d->t->width + (size_t)x;
	test(d->t, d->s, i, a, &d->sh, d->flags, d->s->perspective_z ? -a[A_W] : a[A_Z]);
}

static void gl_draw(struct raster_target *t, const struct prepared *p, const int rows[2], const struct raster_cmd *c)
{
	const struct raster_state *s = p->s;
	struct gl_draw d = { .t = t, .s = s };
	bool textured = c->textured && p->textures, line = c->kind == RASTER_CMD_LINE;
	d.sh.L = textured ? &p->textured : &p->plain;
	d.sh.units = textured ? p->units : 0;
	d.sh.stage = p->stage;
	for (unsigned k = 0; k < RASTER_UNITS; k++)
		if (d.sh.units >> k & 1)        /* (points: magnified; lines: by the texels along them a pixel) */
			unit_setup_lod(&s->unit[k], line && lod_needed(&s->unit[k]) ?
				       raster_line_lod(&s->unit[k], k, &c->v[0], &c->v[1]) : 0, &d.sh.sm[k]);
	d.flags = (textured ? F_TEX : 0) | (s->fog_mode != RASTER_FOG_NONE ? F_FOG : 0) | (s->smooth ? F_COVER : 0);
	attribs(&c->v[0], d.sh.L, d.a0);
	attribs(line ? &c->v[1] : &c->v[0], d.sh.L, d.a1);
	int l, top, r, b;
	clip_rows(t, s, rows, &l, &top, &r, &b);
	raster_gl_fragments(s, c, l, top, r, b, gl_fragment, &d);
}

/* ---- lines, points, bitmaps ------------------------------------------------- */

static void plot(struct raster_target *t, const struct raster_state *s, const int rows[2], float cx, float cy,
		 const float *a, const struct shade *sh)
{
	float half = (s->width > 1 ? s->width : 1) * 0.5f;
	int l, top, r, b;
	clip_rows(t, s, rows, &l, &top, &r, &b);
	float fx0 = floorf(cx - half + 0.5f), fy0 = floorf(cy - half + 0.5f);
	float fx1 = floorf(cx + half + 0.5f), fy1 = floorf(cy + half + 0.5f);
	if (!(fx1 > (float)l && fx0 < (float)r && fy1 > (float)top && fy0 < (float)b))
		return;
	int x0 = fx0 > (float)l ? (int)fx0 : l, x1 = fx1 < (float)r ? (int)fx1 : r;
	int y0 = fy0 > (float)top ? (int)fy0 : top, y1 = fy1 < (float)b ? (int)fy1 : b;
	float depth = s->perspective_z ? -a[A_W] : a[A_Z];
	unsigned flags = s->fog_mode != RASTER_FOG_NONE ? F_FOG : 0;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++) {
			size_t i = (size_t)y * (size_t)t->width + (size_t)x;
			if (flags)
				test(t, s, i, a, sh, F_FOG, depth);
			else
				test(t, s, i, a, sh, 0, depth);
		}
}

static void point(struct raster_target *t, const struct prepared *p, const int rows[2],
		  const struct raster_vertex *v)
{
	const struct shade sh = { .L = &p->plain };
	float a[A_MAX];
	attribs(v, sh.L, a);
	plot(t, p->s, rows, v->x, v->y, a, &sh);
}

void raster_point(struct raster_target *t, const struct raster_state *s, const struct raster_vertex *v)
{
	const int rows[2] = { s->clip_top, s->clip_bottom };
	struct prepared p;
	prepare(s, &p);
	if (s->gl_points) {
		struct raster_cmd c = { .kind = RASTER_CMD_POINT, .v = { *v } };
		gl_draw(t, &p, rows, &c);
	} else {
		point(t, &p, rows, v);
	}
}

static void line(struct raster_target *t, const struct prepared *p, const int rows[2],
		 const struct raster_vertex *v0, const struct raster_vertex *v1)
{
	const struct shade sh = { .L = &p->plain };
	float a0[A_MAX], a1[A_MAX], a[A_MAX];
	attribs(v0, sh.L, a0);
	attribs(v1, sh.L, a1);
	float dx = v1->x - v0->x, dy = v1->y - v0->y;
	float len = fmaxf(fabsf(dx), fabsf(dy));
	if (!(len < 1e5f))
		return;
	int steps = (int)ceilf(len);
	for (int k = 0; k < steps; k++) {      /* (the last pixel is left to the next segment) */
		float f = (float)k / (float)steps;
		for (unsigned j = 0; j < sh.L->n; j++)
			a[j] = a0[j] + (a1[j] - a0[j]) * f;
		plot(t, p->s, rows, v0->x + dx * f, v0->y + dy * f, a, &sh);
	}
}

void raster_line(struct raster_target *t, const struct raster_state *s, const struct raster_vertex *v0,
		 const struct raster_vertex *v1)
{
	const int rows[2] = { s->clip_top, s->clip_bottom };
	struct prepared p;
	prepare(s, &p);
	if (s->gl_points) {
		struct raster_cmd c = { .kind = RASTER_CMD_LINE, .v = { *v0, *v1 } };
		gl_draw(t, &p, rows, &c);
	} else {
		line(t, &p, rows, v0, v1);
	}
}

static void bitmap(struct raster_target *t, const struct raster_state *s, const int rows[2],
		   const struct raster_texture *image, float x, float y, float z)
{
	int w = (int)image->width[0], h = (int)image->height[0];
	int ox = (int)floorf(x + 0.5f), oy = (int)floorf(y + 0.5f);
	int l, top, r, b;
	clip_rows(t, s, rows, &l, &top, &r, &b);
	const struct shade sh = { .L = NULL };
	for (int j = 0; j < h; j++) {
		int py = oy + j;
		if (py < top || py >= b)
			continue;
		for (int i = 0; i < w; i++) {
			int px = ox + i;
			if (px < l || px >= r)
				continue;
			float a[A_PLAIN] = { z, 1 };
			vec4 c = unpack(image->texels[0][(size_t)j * (size_t)w + (size_t)i]);
			memcpy(a + A_C0, &c, sizeof c);
			size_t k = (size_t)py * (size_t)t->width + (size_t)px;
			float depth = s->perspective_z ? -1.0f : z;
			if (c[0] > 0)
				test(t, s, k, a, &sh, 0, depth);
		}
	}
}

void raster_bitmap(struct raster_target *t, const struct raster_state *s, const struct raster_texture *image,
		   float x, float y, float z)
{
	const int rows[2] = { s->clip_top, s->clip_bottom };
	bitmap(t, s, rows, image, x, y, z);
}

/*
 * The pixels [*lo, *hi) whose centers are in [o, o + z n) (or (o + z n, o]
 * if z is negative), within [l, r)
 */
static void pixel_span(float o, float z, int n, int l, int r, int *lo, int *hi)
{
	float a = o, b = o + z * (float)n;
	if (b < a) {
		float t = a;
		a = b;
		b = t;
	}
	a = fmaxf(a - 1, (float)l);             /* (a pixel more each way: the texel test is exact) */
	b = fminf(b + 1, (float)r);
	*lo = a < b ? (int)floorf(a) : l;
	*hi = a < b ? (int)ceilf(b) : l;
}

/* A texel coordinate of a pixel center c (o, 1 / zoom: see RASTER_CMD_PIXELS); n or more: none */
static inline unsigned texel_of(float c, float o, float inv, unsigned n)
{
	float f = floorf((c - o) * inv);
	return f >= 0 && f < (float)n ? (unsigned)f : n;
}

static void pixels(struct raster_target *t, const struct raster_state *s, const int rows[2], const struct raster_cmd *c)
{
	const struct raster_texture *im = c->image;
	unsigned w = im->width[0], h = im->height[0];
	float ox = c->v[0].x, oy = c->v[0].y, zx = c->v[1].x, zy = c->v[1].y;
	if (!w || !h || !(zx != 0 && zy != 0) || !(fabsf(ox) < 1e6f && fabsf(oy) < 1e6f && fabsf(zx) < 1e4f &&
						    fabsf(zy) < 1e4f))
		return;
	float ix = 1 / zx, iy = 1 / zy;
	int l, top, r, b, x0, x1, y0, y1;
	clip_rows(t, s, rows, &l, &top, &r, &b);
	pixel_span(ox, zx, (int)w, l, r, &x0, &x1);
	pixel_span(oy, zy, (int)h, top, b, &y0, &y1);
	const struct shade sh = { .L = NULL };
	float a[A_PLAIN] = { c->v[0].z, 1 };
	const float *rc = c->v[0].color[0];     /* (r, g, b, a) */
	const vec4 color = { rc[3], rc[0], rc[1], rc[2] };
	for (int y = y0; y < y1; y++) {
		unsigned j = texel_of((float)y + 0.5f, oy, iy, h);
		if (j >= h)
			continue;
		const uint32_t *row = im->texels[0] + (size_t)j * w;
		for (int x = x0; x < x1; x++) {
			unsigned i = texel_of((float)x + 0.5f, ox, ix, w);
			if (i >= w)
				continue;
			uint32_t v = row[i];
			size_t k = (size_t)y * (size_t)t->width + (size_t)x;
			float depth = c->v[0].z;
			vec4 col = color;
			switch (c->pixels) {
			case RASTER_PIXELS_COLOR:
				col = unpack(v);
				break;
			case RASTER_PIXELS_MASK:
				if (!v)
					continue;
				break;
			case RASTER_PIXELS_DEPTH:
				memcpy(&depth, &v, 4);
				break;
			default:
				if (t->stencil)
					t->stencil[k] = (uint8_t)((t->stencil[k] & ~s->stencil_write) | (v & s->stencil_write));
				continue;
			}
			memcpy(a + A_C0, &col, sizeof col);
			a[A_Z] = depth;
			test(t, s, k, a, &sh, 0, depth);
		}
	}
}

/* ---- command lists, run in bands of rows on several threads -------------------- */

#define BAND_ROWS 16
#define MAX_THREADS 8
#define DEFAULT_THREADS 3

static struct {
	pthread_once_t once;
	pthread_mutex_t mu;
	pthread_cond_t go, done;
	unsigned threads;               /* helpers (the caller works too) */
	unsigned long generation;
	unsigned busy;
	bool pending;                   /* a job started, not yet waited for */
	/* the job */
	struct raster_target *t;
	const struct raster_state *states;
	const struct raster_cmd *cmds;
	unsigned n, bands;
	atomic_uint next;
} pool = { .once = PTHREAD_ONCE_INIT, .mu = PTHREAD_MUTEX_INITIALIZER, .go = PTHREAD_COND_INITIALIZER,
	   .done = PTHREAD_COND_INITIALIZER };

static void run_band(struct raster_target *t, const struct raster_state *states, const struct raster_cmd *cmds,
		     unsigned n, int y0, int y1)
{
	struct prepared p = { .s = NULL };
	for (unsigned k = 0; k < n; k++) {
		const struct raster_cmd *c = &cmds[k];
		const struct raster_state *s = &states[c->state];
		const int rows[2] = { s->clip_top > y0 ? s->clip_top : y0, s->clip_bottom < y1 ? s->clip_bottom : y1 };
		if (rows[0] >= rows[1])
			continue;
		if (p.s != s && c->kind <= RASTER_CMD_POINT)
			prepare(s, &p);
		switch (c->kind) {
		case RASTER_CMD_TRIANGLE:
			triangle(t, &p, rows, &c->v[0], &c->v[1], &c->v[2], c->textured);
			break;
		case RASTER_CMD_LINE:
			if (s->gl_points)
				gl_draw(t, &p, rows, c);
			else
				line(t, &p, rows, &c->v[0], &c->v[1]);
			break;
		case RASTER_CMD_POINT:
			if (s->gl_points)
				gl_draw(t, &p, rows, c);
			else
				point(t, &p, rows, &c->v[0]);
			break;
		case RASTER_CMD_BITMAP:
			bitmap(t, s, rows, c->image, c->v[0].x, c->v[0].y, c->v[0].z);
			break;
		case RASTER_CMD_PIXELS:
			pixels(t, s, rows, c);
			break;
		case RASTER_CMD_CLEAR: {
			int top = c->rect[1] > rows[0] ? c->rect[1] : rows[0];
			int bottom = c->rect[3] < rows[1] ? c->rect[3] : rows[1];
			clear_rect(t, c->rect[0], top, c->rect[2], bottom, c->clear_color, c->argb, s->color_keep,
				   c->clear_depth, c->z);
			if (c->clear_stencil)
				clear_stencil(t, c->rect[0], top, c->rect[2], bottom, c->stencil, s->stencil_write);
			break;
		}
		}
	}
}

static void work(void)
{
	unsigned b;
	while ((b = atomic_fetch_add(&pool.next, 1)) < pool.bands) {
		int y0 = (int)b * BAND_ROWS, y1 = y0 + BAND_ROWS < pool.t->height ? y0 + BAND_ROWS : pool.t->height;
		run_band(pool.t, pool.states, pool.cmds, pool.n, y0, y1);
	}
}

static void *helper(void *arg)
{
	(void)arg;
	unsigned long seen = 0;
	pthread_mutex_lock(&pool.mu);
	for (;;) {
		while (pool.generation == seen)
			pthread_cond_wait(&pool.go, &pool.mu);
		seen = pool.generation;
		pthread_mutex_unlock(&pool.mu);
		work();
		pthread_mutex_lock(&pool.mu);
		if (--pool.busy == 0)
			pthread_cond_signal(&pool.done);
	}
	return NULL;
}

static void pool_start(void)
{
	/*
	 * Two helpers by default: the emulated processor's thread runs alongside
	 * them, and more (on an M1 Max, at least) slow it down more than they
	 * speed drawing up.
	 */
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	const char *env = getenv("BLUE_RASTER_THREADS");
	long want = env ? strtol(env, NULL, 10) : cpus < DEFAULT_THREADS ? cpus : DEFAULT_THREADS;
	want = want < 1 ? 1 : want > MAX_THREADS ? MAX_THREADS : want;
	for (long k = 1; k < want; k++) {
		pthread_t th;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&th, &attr, helper, NULL) == 0)
			pool.threads++;
		pthread_attr_destroy(&attr);
	}
}

void raster_start(struct raster_target *t, const struct raster_state *states, const struct raster_cmd *cmds,
		  unsigned n)
{
	raster_wait();
	if (!n || t->height <= 0)
		return;
	pthread_once(&pool.once, pool_start);
	unsigned bands = (unsigned)(t->height + BAND_ROWS - 1) / BAND_ROWS;
	pool.t = t;
	pool.states = states;
	pool.cmds = cmds;
	pool.n = n;
	pool.pending = true;
	if (!pool.threads || bands < 2) {               /* (raster_wait() does it all) */
		pool.bands = 1;
		atomic_store(&pool.next, 0);
		pool.busy = 0;
		return;
	}
	pthread_mutex_lock(&pool.mu);
	pool.bands = bands;
	atomic_store(&pool.next, 0);
	pool.busy = pool.threads;
	pool.generation++;
	pthread_cond_broadcast(&pool.go);
	pthread_mutex_unlock(&pool.mu);
}

void raster_wait(void)
{
	if (!pool.pending)
		return;
	if (pool.bands == 1) {
		if (atomic_fetch_add(&pool.next, 1) == 0)
			run_band(pool.t, pool.states, pool.cmds, pool.n, 0, pool.t->height);
	} else {
		work();
	}
	pthread_mutex_lock(&pool.mu);
	while (pool.busy)
		pthread_cond_wait(&pool.done, &pool.mu);
	pthread_mutex_unlock(&pool.mu);
	pool.pending = false;
}

void raster_run(struct raster_target *t, const struct raster_state *states, const struct raster_cmd *cmds,
		unsigned n)
{
	raster_start(t, states, cmds, n);
	raster_wait();
}
