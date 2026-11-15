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

/*
 * The values interpolated across a primitive: depth, 1/w, the fog
 * coordinate and color 0 always, then the colors and texture coordinates
 * its state needs, wherever its layout puts them. Colors are a, r, g, b
 * there, each at a multiple of four.
 */
enum { A_Z, A_W, A_FOG, A_C0 = 4, A_PLAIN = A_C0 + 4, A_MAX = A_C0 + 4 * RASTER_COLORS + 3 * RASTER_UNITS + 2 };
_Static_assert(A_MAX % 4 == 0, "whole vectors of attributes");

struct layout {
	unsigned n;
	uint8_t color[RASTER_COLORS];           /* (0: not interpolated) */
	uint8_t tex[RASTER_UNITS], q[RASTER_UNITS];
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

static inline vec4 unpack(uint32_t p)
{
	return (vec4){ (float)(p >> 24), (float)(p >> 16 & 255), (float)(p >> 8 & 255), (float)(p & 255) } *
	       (1.0f / 255);
}

static inline uint32_t pack(const float c[4])
{
	return (uint32_t)(clamp01(c[0]) * 255 + 0.5f) << 24 | (uint32_t)(clamp01(c[1]) * 255 + 0.5f) << 16 |
	       (uint32_t)(clamp01(c[2]) * 255 + 0.5f) << 8 | (uint32_t)(clamp01(c[3]) * 255 + 0.5f);
}

/* ---- texture sampling ------------------------------------------------------- */

struct sampler {
	const uint32_t *p;
	int w, h;
	float fw, fh;
	bool clamp_u, clamp_v;
	bool pow2_u, pow2_v;                    /* (wrapping by mask) */
	bool bilinear;
};

static void sampler_init(struct sampler *sm, const struct raster_unit *u, unsigned level, bool bilinear)
{
	const struct raster_texture *tx = u->texture;
	sm->p = tx->texels[level];
	sm->w = (int)tx->width[level];
	sm->h = (int)tx->height[level];
	sm->fw = (float)sm->w;
	sm->fh = (float)sm->h;
	sm->clamp_u = u->clamp_s;
	sm->clamp_v = u->clamp_t;
	sm->pow2_u = !(sm->w & (sm->w - 1));
	sm->pow2_v = !(sm->h & (sm->h - 1));
	sm->bilinear = bilinear;
}

/* A texel coordinate from a scaled float one (NaN and huge values come out in range too) */
static ALWAYS_INLINE int texel(float f, int n, bool clamp, bool pow2)
{
	f = fminf(fmaxf(f, -1e9f), 1e9f);
	int i = (int)floorf(f);
	if (clamp)
		return i < 0 ? 0 : i >= n ? n - 1 : i;
	if (pow2)
		return i & (n - 1);
	i %= n;
	return i < 0 ? i + n : i;
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
	int xa = texel(x0f, sm->w, sm->clamp_u, sm->pow2_u), xb = texel(x0f + 1, sm->w, sm->clamp_u, sm->pow2_u);
	int ya = texel(y0f, sm->h, sm->clamp_v, sm->pow2_v), yb = texel(y0f + 1, sm->h, sm->clamp_v, sm->pow2_v);
	vec4 c00 = unpack(sm->p[(size_t)ya * (size_t)sm->w + (size_t)xa]);
	vec4 c10 = unpack(sm->p[(size_t)ya * (size_t)sm->w + (size_t)xb]);
	vec4 c01 = unpack(sm->p[(size_t)yb * (size_t)sm->w + (size_t)xa]);
	vec4 c11 = unpack(sm->p[(size_t)yb * (size_t)sm->w + (size_t)xb]);
	vec4 top = c00 + (c10 - c00) * ax, bottom = c01 + (c11 - c01) * ax;
	return top + (bottom - top) * ay;
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

/* An OpenGL-style blend factor for each of a, r, g, b */
static void blend_factor(unsigned f, const float src[4], const float dst[4], const float constant[4], float out[4])
{
	for (int k = 0; k < 4; k++) {
		float v;
		switch (f) {
		case RASTER_F_ZERO: v = 0; break;
		case RASTER_F_ONE: v = 1; break;
		case RASTER_F_SRC_COLOR: v = src[k]; break;
		case RASTER_F_ONE_MINUS_SRC_COLOR: v = 1 - src[k]; break;
		case RASTER_F_SRC_ALPHA: v = src[0]; break;
		case RASTER_F_ONE_MINUS_SRC_ALPHA: v = 1 - src[0]; break;
		case RASTER_F_DST_ALPHA: v = dst[0]; break;
		case RASTER_F_ONE_MINUS_DST_ALPHA: v = 1 - dst[0]; break;
		case RASTER_F_DST_COLOR: v = dst[k]; break;
		case RASTER_F_ONE_MINUS_DST_COLOR: v = 1 - dst[k]; break;
		case RASTER_F_CONSTANT_COLOR: v = constant[k]; break;
		case RASTER_F_ONE_MINUS_CONSTANT_COLOR: v = 1 - constant[k]; break;
		case RASTER_F_CONSTANT_ALPHA: v = constant[0]; break;
		case RASTER_F_ONE_MINUS_CONSTANT_ALPHA: v = 1 - constant[0]; break;
		default: v = k ? fminf(src[0], 1 - dst[0]) : 1; break;  /* source alpha saturate */
		}
		out[k] = v;
	}
}

static void blend_factors(const struct raster_state *s, float c[4], uint32_t dst)
{
	float d[4], sf[4], df[4];
	vec4 dv = unpack(dst);
	memcpy(d, &dv, sizeof d);
	if (s->blend_eq == RASTER_EQ_MIN || s->blend_eq == RASTER_EQ_MAX) {
		for (int k = 0; k < 4; k++)
			c[k] = s->blend_eq == RASTER_EQ_MIN ? fminf(c[k], d[k]) : fmaxf(c[k], d[k]);
		return;
	}
	blend_factor(s->blend_src, c, d, s->blend_color, sf);
	blend_factor(s->blend_dst, c, d, s->blend_color, df);
	for (int k = 0; k < 4; k++)
		c[k] = s->blend_eq == RASTER_EQ_SUBTRACT ? c[k] * sf[k] - d[k] * df[k] :
		       s->blend_eq == RASTER_EQ_REVERSE_SUBTRACT ? d[k] * df[k] - c[k] * sf[k] :
		       c[k] * sf[k] + d[k] * df[k];
}

/* Compile-time variants of the pixel loop */
#define F_TEX 1u
#define F_FOG 2u

/*
 * One fragment: its color from the interpolated attributes (a) and, if
 * textured, the stages; fog, alpha test, then blended into the target at
 * index i (whose depth test has passed).
 */
static ALWAYS_INLINE void fragment(struct raster_target *t, const struct raster_state *s, size_t i, const float *a,
				   const struct shade *sh, unsigned flags, float depth)
{
	float c[4];                     /* a, r, g, b */
	float w = 0;
	if (flags & (F_TEX | F_FOG))
		w = a[A_W] != 0 ? 1.0f / a[A_W] : 1e30f;
	if (flags & F_TEX)
		stages(s, sh, a, w, c);
	else
		memcpy(c, a + A_C0, sizeof c);

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

	if (s->alpha_test != RASTER_ALPHA_NONE && !alpha_pass(s->alpha_test, c[0], s->alpha_ref))
		return;
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
		blend_factors(s, c, t->color[i]);
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
static void unit_setup(const struct raster_unit *u, unsigned unit, const struct raster_vertex *v[3], float area2,
		       struct sampler *sm)
{
	const struct raster_texture *tx = u->texture;
	bool mipmap = u->min_filter >= RASTER_NEAREST_MIPMAP_NEAREST && tx->levels > 1;
	bool min_linear = linear_filter(u->min_filter), mag_linear = u->mag_filter == RASTER_LINEAR;
	bool linear = mag_linear;
	unsigned level = 0;
	if (mipmap || min_linear != mag_linear) {
		float lod = triangle_lod(u, unit, v, area2);
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
	bool ztest = s->z_func != RASTER_Z_NONE;
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
		for (int x = xa; x < xb; x++) {
			size_t i = row + (size_t)x;
			float depth = s->perspective_z ? -a[A_W] : a[A_Z];
			if (!ztest || z_pass(s->z_func, depth, t->depth[i]))
				fragment(t, s, i, a, &T->sh, flags, depth);
			for (unsigned k = 0; k < na; k++)
				a[k] += T->ax[k];
		}
	}
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
	unsigned flags = (textured ? F_TEX : 0) | (s->fog_mode != RASTER_FOG_NONE ? F_FOG : 0);
	switch (flags) {
	case 0: tri_rows(t, s, &T, 0); break;
	case F_TEX: tri_rows(t, s, &T, F_TEX); break;
	case F_FOG: tri_rows(t, s, &T, F_FOG); break;
	default: tri_rows(t, s, &T, F_TEX | F_FOG); break;
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
			if (s->z_func == RASTER_Z_NONE || z_pass(s->z_func, depth, t->depth[i])) {
				if (flags)
					fragment(t, s, i, a, sh, F_FOG, depth);
				else
					fragment(t, s, i, a, sh, 0, depth);
			}
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
	point(t, &p, rows, v);
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
	line(t, &p, rows, v0, v1);
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
			if (c[0] > 0 && (s->z_func == RASTER_Z_NONE || z_pass(s->z_func, depth, t->depth[k])))
				fragment(t, s, k, a, &sh, 0, depth);
		}
	}
}

void raster_bitmap(struct raster_target *t, const struct raster_state *s, const struct raster_texture *image,
		   float x, float y, float z)
{
	const int rows[2] = { s->clip_top, s->clip_bottom };
	bitmap(t, s, rows, image, x, y, z);
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
			line(t, &p, rows, &c->v[0], &c->v[1]);
			break;
		case RASTER_CMD_POINT:
			point(t, &p, rows, &c->v[0]);
			break;
		case RASTER_CMD_BITMAP:
			bitmap(t, s, rows, c->image, c->v[0].x, c->v[0].y, c->v[0].z);
			break;
		case RASTER_CMD_CLEAR: {
			int top = c->rect[1] > rows[0] ? c->rect[1] : rows[0];
			int bottom = c->rect[3] < rows[1] ? c->rect[3] : rows[1];
			clear_rect(t, c->rect[0], top, c->rect[2], bottom, c->clear_color, c->argb, s->color_keep,
				   c->clear_depth, c->z);
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
	const char *env = getenv("SYSHM_RASTER_THREADS");
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
