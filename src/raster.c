#include "raster.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { A_Z, A_W, A_R, A_G, A_B, A_A, A_UW, A_VW, A_KD, A_KS = A_KD + 3, A_COUNT = A_KS + 3 };

#define ALWAYS_INLINE inline __attribute__((always_inline))

static void attribs(const struct raster_vertex *v, float *a)
{
	a[A_Z] = v->z;
	a[A_W] = v->invw;
	a[A_R] = v->r;
	a[A_G] = v->g;
	a[A_B] = v->b;
	a[A_A] = v->a;
	a[A_UW] = v->uw;
	a[A_VW] = v->vw;
	for (int k = 0; k < 3; k++) {
		a[A_KD + k] = v->kd[k];
		a[A_KS + k] = v->ks[k];
	}
}

static inline float clamp01(float x)
{
	return x < 0 ? 0 : x > 1 ? 1 : x;
}

float raster_far(const struct raster_state *s)
{
	return s->perspective_z ? 0.0f : 1.0f;
}

/* The rows of the target a state lets us draw */
static inline void clip_rows(const struct raster_target *t, const struct raster_state *s, int *l, int *top, int *r,
			     int *b)
{
	*l = s->clip_left > 0 ? s->clip_left : 0;
	*top = s->clip_top > 0 ? s->clip_top : 0;
	*r = s->clip_right < t->width ? s->clip_right : t->width;
	*b = s->clip_bottom < t->height ? s->clip_bottom : t->height;
}

void raster_clear(struct raster_target *t, int left, int top, int right, int bottom,
		  bool color, uint32_t argb, bool depth, float z)
{
	left = left < 0 ? 0 : left;
	top = top < 0 ? 0 : top;
	right = right > t->width ? t->width : right;
	bottom = bottom > t->height ? t->height : bottom;
	for (int y = top; y < bottom; y++) {
		size_t row = (size_t)y * (size_t)t->width;
		for (int x = left; x < right; x++) {
			if (color)
				t->color[row + (size_t)x] = argb;
			if (depth)
				t->depth[row + (size_t)x] = z;
		}
	}
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

static inline void unpack(uint32_t p, float c[4])
{
	c[0] = (float)(p >> 24) * (1.0f / 255);
	c[1] = (float)(p >> 16 & 255) * (1.0f / 255);
	c[2] = (float)(p >> 8 & 255) * (1.0f / 255);
	c[3] = (float)(p & 255) * (1.0f / 255);
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
};

static void sampler_init(struct sampler *sm, const struct raster_texture *tx, unsigned level,
			 const struct raster_state *s)
{
	sm->p = tx->texels[level];
	sm->w = (int)tx->width[level];
	sm->h = (int)tx->height[level];
	sm->fw = (float)sm->w;
	sm->fh = (float)sm->h;
	sm->clamp_u = s->clamp_u;
	sm->clamp_v = s->clamp_v;
	sm->pow2_u = !(sm->w & (sm->w - 1));
	sm->pow2_v = !(sm->h & (sm->h - 1));
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

static ALWAYS_INLINE void sample(const struct sampler *sm, bool bilinear, float u, float v, float c[4])
{
	if (!bilinear) {
		int x = texel(u * sm->fw, sm->w, sm->clamp_u, sm->pow2_u);
		int y = texel(v * sm->fh, sm->h, sm->clamp_v, sm->pow2_v);
		unpack(sm->p[(size_t)y * (size_t)sm->w + (size_t)x], c);
		return;
	}
	float fx = fminf(fmaxf(u * sm->fw - 0.5f, -1e9f), 1e9f), fy = fminf(fmaxf(v * sm->fh - 0.5f, -1e9f), 1e9f);
	float x0f = floorf(fx), y0f = floorf(fy), ax = fx - x0f, ay = fy - y0f;
	int xa = texel(x0f, sm->w, sm->clamp_u, sm->pow2_u), xb = texel(x0f + 1, sm->w, sm->clamp_u, sm->pow2_u);
	int ya = texel(y0f, sm->h, sm->clamp_v, sm->pow2_v), yb = texel(y0f + 1, sm->h, sm->clamp_v, sm->pow2_v);
	float c00[4], c10[4], c01[4], c11[4];
	unpack(sm->p[(size_t)ya * (size_t)sm->w + (size_t)xa], c00);
	unpack(sm->p[(size_t)ya * (size_t)sm->w + (size_t)xb], c10);
	unpack(sm->p[(size_t)yb * (size_t)sm->w + (size_t)xa], c01);
	unpack(sm->p[(size_t)yb * (size_t)sm->w + (size_t)xb], c11);
	for (int k = 0; k < 4; k++) {
		float top = c00[k] + (c10[k] - c00[k]) * ax, bottom = c01[k] + (c11[k] - c01[k]) * ax;
		c[k] = top + (bottom - top) * ay;
	}
}

/* ---- fragments -------------------------------------------------------------- */

/* An OpenGL-style blend factor for each of a, r, g, b */
static void blend_factor(unsigned f, const float src[4], const float dst[4], float out[4])
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
		default: v = k ? fminf(src[0], 1 - dst[0]) : 1; break;  /* source alpha saturate */
		}
		out[k] = v;
	}
}

/* Compile-time variants of the pixel loop */
#define F_TEX 1u
#define F_FOG 2u
#define F_BILINEAR 4u

/*
 * One fragment: its color from the interpolated attributes (a) and the
 * texture (and the second texture, with sm2), fog, alpha test, then blended
 * into the target at index i (whose depth test has passed).
 */
static ALWAYS_INLINE void fragment(struct raster_target *t, const struct raster_state *s, size_t i, const float *a,
				   const struct sampler *sm, const struct sampler *sm2, unsigned flags, float depth)
{
	float c[4];                     /* a, r, g, b */
	float w = 0;
	if (flags & (F_TEX | F_FOG))
		w = a[A_W] != 0 ? 1.0f / a[A_W] : 1e30f;
	if (flags & F_TEX) {
		sample(sm, flags & F_BILINEAR, a[A_UW] * w, a[A_VW] * w, c);
		if (s->texture_op & RASTER_TEX_DECAL) {
			for (int k = 1; k < 4; k++)
				c[k] = c[0] * c[k] + (1 - c[0]) * a[A_R + k - 1];
			c[0] = a[A_A];
		} else {
			c[0] *= a[A_A];
		}
		if (s->texture_op & RASTER_TEX_MODULATE)
			for (int k = 1; k < 4; k++)
				c[k] = clamp01(c[k] * a[A_KD + k - 1]);
		if (sm2) {
			float w2 = a[A_KS + 2] != 0 ? 1.0f / a[A_KS + 2] : 1e30f, c2[4];
			sample(sm2, true, a[A_KS] * w2, a[A_KS + 1] * w2, c2);
			for (int k = 0; k < 4; k++)
				c[k] *= c2[k];
		} else if (s->texture_op & RASTER_TEX_HIGHLIGHT) {
			for (int k = 1; k < 4; k++)
				c[k] = clamp01(c[k] + a[A_KS + k - 1]);
		}
	} else {
		c[0] = a[A_A];
		c[1] = a[A_R];
		c[2] = a[A_G];
		c[3] = a[A_B];
	}

	if (flags & F_FOG) {
		float f, d = s->fog_end != 0 ? s->fog_density / s->fog_end : s->fog_density;
		switch (s->fog_mode) {
		case RASTER_FOG_ALPHA:
			f = c[0];
			c[0] = 1;
			break;
		case RASTER_FOG_LINEAR:
			f = w < s->fog_start ? 1 : w > s->fog_end ? 0 : (s->fog_end - w) / (s->fog_end - s->fog_start);
			break;
		case RASTER_FOG_EXP:
			f = expf(-d * w);
			break;
		default:
			f = expf(-(d * w) * (d * w));
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
	if (c[0] <= 0 && (s->blend == RASTER_BLEND_INTERPOLATE || (c[1] <= 0 && c[2] <= 0 && c[3] <= 0)))
		return;
	if (s->z_write && s->z_func != RASTER_Z_NONE && c[0] > 0)
		t->depth[i] = depth;
	if (s->blend == RASTER_BLEND_FACTORS) {
		float d[4], sf[4], df[4];
		unpack(t->color[i], d);
		blend_factor(s->blend_src, c, d, sf);
		blend_factor(s->blend_dst, c, d, df);
		for (int k = 0; k < 4; k++)
			c[k] = c[k] * sf[k] + d[k] * df[k];
	} else if (c[0] < 1) {
		float d[4];
		unpack(t->color[i], d);
		float as = clamp01(c[0]);
		if (s->blend == RASTER_BLEND_INTERPOLATE)
			for (int k = 1; k < 4; k++)
				c[k] = as * c[k] + (1 - as) * d[k];
		else
			for (int k = 1; k < 4; k++)
				c[k] = c[k] + (1 - as) * d[k];
		c[0] = 1 - (1 - as) * (1 - d[0]);
	}
	t->color[i] = pack(c);
}

/* ---- triangles -------------------------------------------------------------- */

/*
 * Mipmap level for a triangle: texels per pixel, from the areas the triangle
 * covers (in the second texture, at the coordinates in ks)
 */
static unsigned triangle_level(const struct raster_texture *tx, const struct raster_vertex *v[3], float area2,
			       bool second)
{
	if (tx->levels <= 1)
		return 0;
	float u[3], w[3];
	for (int k = 0; k < 3; k++) {
		float iw = second ? v[k]->ks[2] : v[k]->invw;
		float inv = iw != 0 ? 1.0f / iw : 0;
		u[k] = (second ? v[k]->ks[0] : v[k]->uw) * inv * (float)tx->width[0];
		w[k] = (second ? v[k]->ks[1] : v[k]->vw) * inv * (float)tx->height[0];
	}
	float tarea = fabsf((u[1] - u[0]) * (w[2] - w[0]) - (u[2] - u[0]) * (w[1] - w[0]));
	if (!(tarea > 0) || !(area2 > 0))
		return 0;
	float lod = 0.5f * log2f(tarea / area2);
	if (!(lod > 0))
		return 0;
	unsigned level = (unsigned)(lod + 0.5f);
	return level < tx->levels ? level : tx->levels - 1;
}

/* A triangle set up for drawing */
struct tri {
	float ex[3], ey[3], ec[3];      /* edge functions: e = ec + ex * x + ey * y */
	bool tl[3];                     /* top or left edges (which own pixels exactly on them) */
	float a0[A_COUNT], ax[A_COUNT], ay[A_COUNT];    /* attributes at (ox, oy) and their gradients */
	float ox, oy;
	int x0, x1, y0, y1;             /* pixels that may be covered */
	unsigned na;
	struct sampler sm;
	bool second;                    /* sm2 samples the second texture */
	struct sampler sm2;
};

static bool tri_setup(const struct raster_target *t, const struct raster_state *s, const struct raster_vertex *v0,
		      const struct raster_vertex *v1, const struct raster_vertex *v2, bool textured, struct tri *T)
{
	const struct raster_vertex *v[3] = { v0, v1, v2 };
	float area2 = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
	if (!(area2 != 0) || !isfinite(area2))
		return false;
	if (area2 < 0) {                /* one winding for the edge functions */
		v[1] = v2;
		v[2] = v1;
		area2 = -area2;
	}
	int l, top, r, b;
	clip_rows(t, s, &l, &top, &r, &b);
	float minx = fminf(v[0]->x, fminf(v[1]->x, v[2]->x)), maxx = fmaxf(v[0]->x, fmaxf(v[1]->x, v[2]->x));
	float miny = fminf(v[0]->y, fminf(v[1]->y, v[2]->y)), maxy = fmaxf(v[0]->y, fmaxf(v[1]->y, v[2]->y));
	if (!(minx < (float)r && maxx > (float)l && miny < (float)b && maxy > (float)top))
		return false;
	T->x0 = minx > (float)l ? (int)floorf(minx) : l;
	T->x1 = maxx < (float)r ? (int)ceilf(maxx) : r;
	T->y0 = miny > (float)top ? (int)floorf(miny) : top;
	T->y1 = maxy < (float)b ? (int)ceilf(maxy) : b;

	/* edge i: from v[i+1] to v[i+2], its function v[i]'s barycentric weight times area2 */
	for (int i = 0; i < 3; i++) {
		const struct raster_vertex *p = v[(i + 1) % 3], *q = v[(i + 2) % 3];
		float dx = q->x - p->x, dy = q->y - p->y;
		/* e(x, y) = dx * (y - p.y) - dy * (x - p.x) */
		T->ex[i] = -dy;
		T->ey[i] = dx;
		T->ec[i] = -dx * p->y + dy * p->x;
		T->tl[i] = dy < 0 || (dy == 0 && dx > 0);
	}

	float av[3][A_COUNT];
	for (int k = 0; k < 3; k++)
		attribs(v[k], av[k]);
	float dx1 = v[1]->x - v[0]->x, dy1 = v[1]->y - v[0]->y, dx2 = v[2]->x - v[0]->x, dy2 = v[2]->y - v[0]->y;
	T->na = textured ? A_COUNT : A_UW;
	for (unsigned k = 0; k < T->na; k++) {
		float d1 = av[1][k] - av[0][k], d2 = av[2][k] - av[0][k];
		T->ax[k] = (d1 * dy2 - d2 * dy1) / area2;
		T->ay[k] = (d2 * dx1 - d1 * dx2) / area2;
		T->a0[k] = av[0][k];
	}
	T->ox = v[0]->x;
	T->oy = v[0]->y;
	T->second = false;
	if (textured) {
		sampler_init(&T->sm, s->texture, triangle_level(s->texture, v, area2, false), s);
		if (s->texture2 && s->texture2->levels) {
			T->second = true;
			sampler_init(&T->sm2, s->texture2, triangle_level(s->texture2, v, area2, true), s);
			T->sm2.clamp_u = T->sm2.clamp_v = true;
		}
	}
	return true;
}

static ALWAYS_INLINE bool inside(const float e[3], const bool tl[3])
{
	return (e[0] > 0 || (e[0] == 0 && tl[0])) && (e[1] > 0 || (e[1] == 0 && tl[1])) &&
	       (e[2] > 0 || (e[2] == 0 && tl[2]));
}

static ALWAYS_INLINE void tri_rows(struct raster_target *t, const struct raster_state *s, const struct tri *T,
				   unsigned flags)
{
	bool ztest = s->z_func != RASTER_Z_NONE;
	const unsigned na = flags & F_TEX ? A_COUNT : A_UW;
	for (int y = T->y0; y < T->y1; y++) {
		float py = (float)y + 0.5f;
		/* the span where every edge function can be >= 0 (then each pixel is tested exactly) */
		float xs = (float)T->x0, xe = (float)T->x1;
		bool empty = false;
		for (int i = 0; i < 3 && !empty; i++) {
			float e0 = T->ec[i] + T->ey[i] * py;
			if (T->ex[i] > 0)
				xs = fmaxf(xs, floorf(-e0 / T->ex[i] - 0.5f));
			else if (T->ex[i] < 0)
				xe = fminf(xe, ceilf(-e0 / T->ex[i] - 0.5f) + 1);
			else if (e0 < 0 || (e0 == 0 && !T->tl[i]))
				empty = true;
		}
		if (empty || !(xs < xe))
			continue;
		int xa = (int)xs, xb = (int)xe;
		float px = (float)xa + 0.5f;
		float e[3], a[A_COUNT];
		for (int i = 0; i < 3; i++)
			e[i] = T->ec[i] + T->ex[i] * px + T->ey[i] * py;
		for (unsigned k = 0; k < na; k++)
			a[k] = T->a0[k] + T->ax[k] * (px - T->ox) + T->ay[k] * (py - T->oy);
		size_t row = (size_t)y * (size_t)t->width;
		for (int x = xa; x < xb; x++) {
			if (inside(e, T->tl)) {
				size_t i = row + (size_t)x;
				float depth = s->perspective_z ? -a[A_W] : a[A_Z];
				if (!ztest || z_pass(s->z_func, depth, t->depth[i]))
					fragment(t, s, i, a, &T->sm, T->second ? &T->sm2 : NULL, flags, depth);
			}
			for (int k = 0; k < 3; k++)
				e[k] += T->ex[k];
			for (unsigned k = 0; k < na; k++)
				a[k] += T->ax[k];
		}
	}
}

void raster_triangle(struct raster_target *t, const struct raster_state *s, const struct raster_vertex *v0,
		     const struct raster_vertex *v1, const struct raster_vertex *v2, bool textured)
{
	if (textured && (!s->texture || !s->texture->levels))
		textured = false;
	struct tri T;
	if (!tri_setup(t, s, v0, v1, v2, textured, &T))
		return;
	unsigned flags = (textured ? F_TEX : 0) | (s->fog_mode != RASTER_FOG_NONE ? F_FOG : 0) |
			 (textured && s->bilinear ? F_BILINEAR : 0);
	switch (flags) {
	case 0: tri_rows(t, s, &T, 0); break;
	case F_TEX: tri_rows(t, s, &T, F_TEX); break;
	case F_FOG: tri_rows(t, s, &T, F_FOG); break;
	case F_TEX | F_FOG: tri_rows(t, s, &T, F_TEX | F_FOG); break;
	case F_TEX | F_BILINEAR: tri_rows(t, s, &T, F_TEX | F_BILINEAR); break;
	default: tri_rows(t, s, &T, F_TEX | F_FOG | F_BILINEAR); break;
	}
}

/* ---- lines, points, bitmaps ------------------------------------------------- */

static void plot(struct raster_target *t, const struct raster_state *s, float cx, float cy, const float *a)
{
	float half = (s->width > 1 ? s->width : 1) * 0.5f;
	int l, top, r, b;
	clip_rows(t, s, &l, &top, &r, &b);
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
					fragment(t, s, i, a, NULL, NULL, F_FOG, depth);
				else
					fragment(t, s, i, a, NULL, NULL, 0, depth);
			}
		}
}

void raster_point(struct raster_target *t, const struct raster_state *s, const struct raster_vertex *v)
{
	float a[A_COUNT];
	attribs(v, a);
	plot(t, s, v->x, v->y, a);
}

void raster_line(struct raster_target *t, const struct raster_state *s, const struct raster_vertex *v0,
		 const struct raster_vertex *v1)
{
	float a0[A_COUNT], a1[A_COUNT], a[A_COUNT];
	attribs(v0, a0);
	attribs(v1, a1);
	float dx = v1->x - v0->x, dy = v1->y - v0->y;
	float len = fmaxf(fabsf(dx), fabsf(dy));
	if (!(len < 1e5f))
		return;
	int steps = (int)ceilf(len);
	for (int k = 0; k < steps; k++) {      /* (the last pixel is left to the next segment) */
		float f = (float)k / (float)steps;
		for (int j = 0; j < A_UW; j++)
			a[j] = a0[j] + (a1[j] - a0[j]) * f;
		plot(t, s, v0->x + dx * f, v0->y + dy * f, a);
	}
}

void raster_bitmap(struct raster_target *t, const struct raster_state *s, const struct raster_texture *image,
		   float x, float y, float z)
{
	int w = (int)image->width[0], h = (int)image->height[0];
	int ox = (int)floorf(x + 0.5f), oy = (int)floorf(y + 0.5f);
	int l, top, r, b;
	clip_rows(t, s, &l, &top, &r, &b);
	struct raster_state flat = *s;
	flat.texture_op = 0;
	flat.fog_mode = RASTER_FOG_NONE;
	for (int j = 0; j < h; j++) {
		int py = oy + j;
		if (py < top || py >= b)
			continue;
		for (int i = 0; i < w; i++) {
			int px = ox + i;
			if (px < l || px >= r)
				continue;
			float a[A_COUNT] = { 0 };
			float c[4];
			unpack(image->texels[0][(size_t)j * (size_t)w + (size_t)i], c);
			a[A_Z] = z;
			a[A_W] = 1;
			a[A_A] = c[0];
			a[A_R] = c[1];
			a[A_G] = c[2];
			a[A_B] = c[3];
			size_t k = (size_t)py * (size_t)t->width + (size_t)px;
			float depth = s->perspective_z ? -1.0f : z;
			if (c[0] > 0 && (s->z_func == RASTER_Z_NONE || z_pass(s->z_func, depth, t->depth[k])))
				fragment(t, &flat, k, a, NULL, NULL, 0, depth);
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
	for (unsigned k = 0; k < n; k++) {
		const struct raster_cmd *c = &cmds[k];
		struct raster_state s = states[c->state];
		s.clip_top = s.clip_top > y0 ? s.clip_top : y0;
		s.clip_bottom = s.clip_bottom < y1 ? s.clip_bottom : y1;
		if (s.clip_top >= s.clip_bottom)
			continue;
		switch (c->kind) {
		case RASTER_CMD_TRIANGLE:
			raster_triangle(t, &s, &c->v[0], &c->v[1], &c->v[2], c->textured);
			break;
		case RASTER_CMD_LINE:
			raster_line(t, &s, &c->v[0], &c->v[1]);
			break;
		case RASTER_CMD_POINT:
			raster_point(t, &s, &c->v[0]);
			break;
		case RASTER_CMD_BITMAP:
			raster_bitmap(t, &s, c->image, c->v[0].x, c->v[0].y, c->v[0].z);
			break;
		case RASTER_CMD_CLEAR: {
			int top = c->rect[1] > s.clip_top ? c->rect[1] : s.clip_top;
			int bottom = c->rect[3] < s.clip_bottom ? c->rect[3] : s.clip_bottom;
			raster_clear(t, c->rect[0], top, c->rect[2], bottom, c->clear_color, c->argb, c->clear_depth, c->z);
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
