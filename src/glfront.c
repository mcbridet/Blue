#include "glfront.h"

/* (the same results on every host: no fused multiply-adds where some have them; GCC fuses none in ISO C) */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

#include <math.h>
#include <string.h>

void glf_error(struct gl_state *st, uint32_t e)
{
	if (!st->error)
		st->error = e;
}

/* ---- capabilities ---------------------------------------------------------------- */

/* Those of OpenGL 1.2 and its imaging subset that glEnable takes; per texture unit, or not; on at first */
static const struct cap {
	uint16_t name;
	bool unit, on;
} caps[] = {
	[GLF_CAP_DEPTH_TEST] = { GL_DEPTH_TEST, false, false },
	[GLF_CAP_CULL_FACE] = { GL_CULL_FACE, false, false },
	[GLF_CAP_BLEND] = { GL_BLEND, false, false },
	[GLF_CAP_ALPHA_TEST] = { GL_ALPHA_TEST, false, false },
	[GLF_CAP_SCISSOR_TEST] = { GL_SCISSOR_TEST, false, false },
	[GLF_CAP_DITHER] = { GL_DITHER, false, true },
	[GLF_CAP_TEXTURE_1D] = { GL_TEXTURE_1D, true, false },
	[GLF_CAP_TEXTURE_2D] = { GL_TEXTURE_2D, true, false },
	[GLF_CAP_TEXTURE_GEN_S] = { GL_TEXTURE_GEN_S, true, false }, { GL_TEXTURE_GEN_S + 1, true, false },
	{ GL_TEXTURE_GEN_S + 2, true, false }, { GL_TEXTURE_GEN_S + 3, true, false },
	[GLF_CAP_LIGHTING] = { GL_LIGHTING, false, false },
	[GLF_CAP_LIGHT0] = { GL_LIGHT0, false, false }, { GL_LIGHT0 + 1, false, false }, { GL_LIGHT0 + 2, false, false },
	{ GL_LIGHT0 + 3, false, false }, { GL_LIGHT0 + 4, false, false }, { GL_LIGHT0 + 5, false, false },
	{ GL_LIGHT0 + 6, false, false }, { GL_LIGHT0 + 7, false, false },
	[GLF_CAP_NORMALIZE] = { GL_NORMALIZE, false, false },
	[GLF_CAP_RESCALE_NORMAL] = { GL_RESCALE_NORMAL, false, false },
	[GLF_CAP_COLOR_MATERIAL] = { GL_COLOR_MATERIAL, false, false },
	[GLF_CAP_FOG] = { GL_FOG, false, false },
	[GLF_CAP_COLOR_SUM] = { GL_COLOR_SUM_EXT, false, false },
	[GLF_CAP_CLIP_PLANE0] = { GL_CLIP_PLANE0, false, false }, { GL_CLIP_PLANE0 + 1, false, false },
	{ GL_CLIP_PLANE0 + 2, false, false }, { GL_CLIP_PLANE0 + 3, false, false }, { GL_CLIP_PLANE0 + 4, false, false },
	{ GL_CLIP_PLANE0 + 5, false, false },
	[GLF_CAP_OFFSET_POINT] = { GL_POLYGON_OFFSET_POINT, false, false },
	[GLF_CAP_OFFSET_LINE] = { GL_POLYGON_OFFSET_LINE, false, false },
	[GLF_CAP_OFFSET_FILL] = { GL_POLYGON_OFFSET_FILL, false, false },
	[GLF_CAP_STENCIL_TEST] = { GL_STENCIL_TEST, false, false },
	[GLF_CAP_TEXTURE_3D] = { GL_TEXTURE_3D, true, false },
	[GLF_CAP_POINT_SMOOTH] = { GL_POINT_SMOOTH, false, false },
	[GLF_CAP_LINE_SMOOTH] = { GL_LINE_SMOOTH, false, false },
	[GLF_CAP_LINE_STIPPLE] = { GL_LINE_STIPPLE, false, false },
	/* (polygons aren't antialiased: see docs/3d.md; color index's logic op: there's none) */
	[GLF_CAP_POLYGON_SMOOTH] = { GL_POLYGON_SMOOTH, false, false },
	[GLF_CAP_POLYGON_STIPPLE] = { GL_POLYGON_STIPPLE, false, false },
	[GLF_CAP_INDEX_LOGIC_OP] = { GL_INDEX_LOGIC_OP, false, false },
	[GLF_CAP_COLOR_LOGIC_OP] = { GL_COLOR_LOGIC_OP, false, false },
	/* (evaluators': GL_AUTO_NORMAL, the maps) */
	{ 0x0D80, false, false },
	{ 0x0D90, false, false }, { 0x0D91, false, false }, { 0x0D92, false, false }, { 0x0D93, false, false },
	{ 0x0D94, false, false }, { 0x0D95, false, false }, { 0x0D96, false, false }, { 0x0D97, false, false },
	{ 0x0D98, false, false }, { 0x0DB0, false, false }, { 0x0DB1, false, false }, { 0x0DB2, false, false },
	{ 0x0DB3, false, false }, { 0x0DB4, false, false }, { 0x0DB5, false, false }, { 0x0DB6, false, false },
	{ 0x0DB7, false, false }, { 0x0DB8, false, false },
	/* (kept, not drawn by) */
	{ 0x80D0, false, false },
	{ 0x80D1, false, false }, { 0x80D2, false, false }, { 0x8010, false, false }, { 0x8011, false, false },
	{ 0x8012, false, false }, { 0x8024, false, false }, { 0x802E, false, false },
};
_Static_assert(sizeof caps / sizeof caps[0] <= GLF_CAPS, "gl_state.caps");

static int cap_index(uint32_t name)
{
	for (unsigned k = 0; k < sizeof caps / sizeof caps[0]; k++)
		if (caps[k].name == name)
			return (int)k;
	return -1;
}

int glf_cap(uint32_t name)
{
	return cap_index(name);
}

bool glf_enable(struct gl_state *st, uint32_t cap, bool on)
{
	int k = cap_index(cap);
	if (k < 0)
		return false;
	uint8_t bit = caps[k].unit ? (uint8_t)(1u << st->active_texture) : 1;
	st->caps[k] = on ? st->caps[k] | bit : st->caps[k] & (uint8_t)~bit;
	st->raster_stale = true;
	return true;
}

int glf_is_enabled(const struct gl_state *st, uint32_t cap)
{
	int k = cap_index(cap);
	if (k < 0)
		return glf_array_enabled(st, cap);
	return (st->caps[k] >> (caps[k].unit ? st->active_texture : 0) & 1) != 0;
}

/* ---- state ----------------------------------------------------------------------------- */

static void identity(float m[16])
{
	memset(m, 0, 16 * sizeof *m);
	m[0] = m[5] = m[10] = m[15] = 1;
}

static void stack_init(struct gl_matrices *s, unsigned max)
{
	s->depth = 1;
	s->max = max;
	identity(s->m[0]);
}

static void viewport_transform(struct gl_state *st)
{
	const int32_t *v = st->viewport;
	st->vp_scale[0] = (float)v[2] * 0.5f;
	st->vp_offset[0] = (float)v[0] + (float)v[2] * 0.5f;
	st->vp_scale[1] = (float)v[3] * 0.5f;
	st->vp_offset[1] = (float)v[1] + (float)v[3] * 0.5f;
	st->vp_scale[2] = (float)((st->depth_range[1] - st->depth_range[0]) * 0.5);
	st->vp_offset[2] = (float)((st->depth_range[1] + st->depth_range[0]) * 0.5);
}

void glf_init(struct gl_state *st)
{
	memset(st, 0, sizeof *st);
	for (unsigned k = 0; k < sizeof caps / sizeof caps[0]; k++)
		st->caps[k] = caps[k].on;
	st->clear_depth = 1;
	st->depth_range[1] = 1;
	for (int k = 0; k < 4; k++)
		st->color_mask[k] = true;
	st->depth_mask = true;
	st->depth_func = GL_LESS;
	st->blend_src = GL_ONE;
	st->blend_dst = GL_ZERO;
	st->blend_eq = GL_FUNC_ADD;
	st->alpha_func = GL_ALWAYS;
	st->cull_mode = GL_BACK;
	st->front_face = GL_CCW;
	st->shade_model = GL_SMOOTH;
	st->point_size = st->line_width = 1;
	st->polygon_mode[0] = st->polygon_mode[1] = GL_FILL;
	st->line_stipple_factor = 1;
	st->line_stipple = 0xFFFF;
	memset(st->polygon_stipple, 0xFF, sizeof st->polygon_stipple);
	st->logic_op = GL_COPY;
	st->stencil_func = GL_ALWAYS;
	st->stencil_fail = st->stencil_zfail = st->stencil_zpass = GL_KEEP;
	st->stencil_mask = st->stencil_write_mask = 0xFFFFFFFFu;
	for (int k = 0; k < 7; k++)
		st->hints[k] = GL_DONT_CARE;
	st->matrix_mode = GL_MODELVIEW;
	stack_init(&st->modelview, GLF_MODELVIEW_DEPTH);
	stack_init(&st->projection, GLF_PROJECTION_DEPTH);
	for (unsigned k = 0; k < GLF_TEXTURE_UNITS; k++) {
		stack_init(&st->texture[k], GLF_TEXTURE_DEPTH);
		st->tex[k][3] = 1;
	}
	st->mvp_stale = st->texmat_stale = st->normal_stale = true;
	glf_textures_init(st);
	glf_lighting_init(st);
	glf_arrays_init(st);
	glf_pixels_init(st);
	st->select.mode = GL_RENDER;
	st->grid1_n = st->grid2_n[0] = st->grid2_n[1] = 1;
	st->grid1[1] = st->grid2[1] = st->grid2[3] = 1;
	st->color[0] = st->color[1] = st->color[2] = st->color[3] = 1;
	st->secondary_color[3] = 1;
	st->normal[2] = 1;
	st->edge_flag = true;
	viewport_transform(st);
	st->raster_stale = true;
}

void glf_viewport(struct gl_state *st, int32_t x, int32_t y, int32_t w, int32_t h)
{
	if (w < 0 || h < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	st->viewport[0] = x;
	st->viewport[1] = y;
	st->viewport[2] = w < GLF_MAX_SIZE ? w : GLF_MAX_SIZE;         /* (as GL_MAX_VIEWPORT_DIMS) */
	st->viewport[3] = h < GLF_MAX_SIZE ? h : GLF_MAX_SIZE;
	viewport_transform(st);
	st->raster_stale = true;
}

void glf_scissor(struct gl_state *st, int32_t x, int32_t y, int32_t w, int32_t h)
{
	if (w < 0 || h < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	st->scissor[0] = x;
	st->scissor[1] = y;
	st->scissor[2] = w;
	st->scissor[3] = h;
	st->raster_stale = true;
}

static double clampd(double v)
{
	return v > 0 ? (v < 1 ? v : 1) : 0;
}

static float clampf(float v)
{
	return v > 0 ? (v < 1 ? v : 1) : 0;
}

void glf_depth_range(struct gl_state *st, double n, double f)
{
	st->depth_range[0] = clampd(n);
	st->depth_range[1] = clampd(f);
	viewport_transform(st);
}

static bool is_func(uint32_t f)
{
	return f >= GL_NEVER && f <= GL_ALWAYS;
}

void glf_depth_func(struct gl_state *st, uint32_t func)
{
	if (!is_func(func)) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	st->depth_func = func;
	st->raster_stale = true;
}

void glf_depth_mask(struct gl_state *st, bool on)
{
	st->depth_mask = on;
	st->raster_stale = true;
}

void glf_color_mask(struct gl_state *st, bool r, bool g, bool b, bool a)
{
	st->color_mask[0] = r;
	st->color_mask[1] = g;
	st->color_mask[2] = b;
	st->color_mask[3] = a;
	st->raster_stale = true;
}

/* A blend factor as raster.h's, or -1 */
static int blend_factor(uint32_t f)
{
	if (f == GL_ZERO || f == GL_ONE)
		return f == GL_ZERO ? RASTER_F_ZERO : RASTER_F_ONE;
	if (f >= GL_SRC_COLOR && f <= GL_SRC_ALPHA_SATURATE)
		return RASTER_F_SRC_COLOR + (int)(f - GL_SRC_COLOR);
	if (f >= GL_CONSTANT_COLOR && f <= GL_ONE_MINUS_CONSTANT_ALPHA)
		return RASTER_F_CONSTANT_COLOR + (int)(f - GL_CONSTANT_COLOR);
	return -1;
}

void glf_blend_func(struct gl_state *st, uint32_t src, uint32_t dst)
{
	if (blend_factor(src) < 0 || blend_factor(dst) < 0 || dst == GL_SRC_ALPHA_SATURATE) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	st->blend_src = src;
	st->blend_dst = dst;
	st->raster_stale = true;
}

void glf_blend_equation(struct gl_state *st, uint32_t eq)
{
	if (eq != GL_FUNC_ADD && eq != GL_MIN && eq != GL_MAX && eq != GL_FUNC_SUBTRACT &&
	    eq != GL_FUNC_REVERSE_SUBTRACT) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	st->blend_eq = eq;
	st->raster_stale = true;
}

void glf_blend_color(struct gl_state *st, float r, float g, float b, float a)
{
	st->blend_color[0] = clampf(r);
	st->blend_color[1] = clampf(g);
	st->blend_color[2] = clampf(b);
	st->blend_color[3] = clampf(a);
	st->raster_stale = true;
}

void glf_logic_op(struct gl_state *st, uint32_t op)
{
	if (op < GL_CLEAR || op > GL_SET) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	st->logic_op = op;
	st->raster_stale = true;
}

void glf_alpha_func(struct gl_state *st, uint32_t func, float ref)
{
	if (!is_func(func)) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	st->alpha_func = func;
	st->alpha_ref = clampf(ref);
	st->raster_stale = true;
}

void glf_cull_face(struct gl_state *st, uint32_t mode)
{
	if (mode != GL_FRONT && mode != GL_BACK && mode != GL_FRONT_AND_BACK)
		glf_error(st, GL_INVALID_ENUM);
	else
		st->cull_mode = mode;
}

void glf_front_face(struct gl_state *st, uint32_t mode)
{
	if (mode != GL_CW && mode != GL_CCW)
		glf_error(st, GL_INVALID_ENUM);
	else
		st->front_face = mode;
}

void glf_shade_model(struct gl_state *st, uint32_t mode)
{
	if (mode != GL_FLAT && mode != GL_SMOOTH)
		glf_error(st, GL_INVALID_ENUM);
	else
		st->shade_model = mode;
}

void glf_point_size(struct gl_state *st, float size)
{
	if (!(size > 0))
		glf_error(st, GL_INVALID_VALUE);
	else
		st->point_size = size;
}

void glf_line_width(struct gl_state *st, float width)
{
	if (!(width > 0))
		glf_error(st, GL_INVALID_VALUE);
	else
		st->line_width = width;
}

void glf_active_texture(struct gl_state *st, uint32_t unit)
{
	if (unit < GL_TEXTURE0_ARB || unit >= GL_TEXTURE0_ARB + GLF_TEXTURE_UNITS)
		glf_error(st, GL_INVALID_ENUM);
	else
		st->active_texture = unit - GL_TEXTURE0_ARB;
}

void glf_polygon_mode(struct gl_state *st, uint32_t face, uint32_t mode)
{
	if ((face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) || mode < GL_POINT || mode > GL_FILL) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	if (face != GL_BACK)
		st->polygon_mode[0] = mode;
	if (face != GL_FRONT)
		st->polygon_mode[1] = mode;
}

void glf_polygon_offset(struct gl_state *st, float factor, float units)
{
	st->offset_factor = factor;
	st->offset_units = units;
}

void glf_line_stipple(struct gl_state *st, int32_t factor, uint16_t pattern)
{
	st->line_stipple_factor = factor < 1 ? 1 : factor > 256 ? 256 : factor;
	st->line_stipple = pattern;
}

void glf_hint(struct gl_state *st, uint32_t target, uint32_t mode)
{
	static const uint32_t targets[7] = { 0x0C50, 0x0C51, 0x0C52, 0x0C53, 0x0C54, 0x80F0, 0x85B1 };
	unsigned k = 0;
	while (k < 7 && targets[k] != target)
		k++;
	if (k == 7 || mode < GL_DONT_CARE || mode > GL_NICEST) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	st->hints[k] = mode;            /* (taken: the same drawing either way) */
}

void glf_stencil_func(struct gl_state *st, uint32_t func, int32_t ref, uint32_t mask)
{
	if (!is_func(func)) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	st->stencil_func = func;
	st->stencil_ref = ref;
	st->stencil_mask = mask;
	st->raster_stale = true;
}

static bool is_stencil_op(uint32_t op)
{
	return op == GL_KEEP || op == GL_ZERO || op == GL_REPLACE || op == GL_INCR || op == GL_DECR ||
	       op == GL_INVERT || op == GL_INCR_WRAP_EXT || op == GL_DECR_WRAP_EXT;
}

void glf_stencil_op(struct gl_state *st, uint32_t fail, uint32_t zfail, uint32_t zpass)
{
	if (!is_stencil_op(fail) || !is_stencil_op(zfail) || !is_stencil_op(zpass)) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	st->stencil_fail = fail;
	st->stencil_zfail = zfail;
	st->stencil_zpass = zpass;
	st->raster_stale = true;
}

void glf_stencil_mask(struct gl_state *st, uint32_t mask)
{
	st->stencil_write_mask = mask;
	st->raster_stale = true;
}

/* ---- matrices -------------------------------------------------------------------------- */

static struct gl_matrices *stack(struct gl_state *st)
{
	switch (st->matrix_mode) {
	case GL_PROJECTION: return &st->projection;
	case GL_TEXTURE: return &st->texture[st->active_texture];
	default: return &st->modelview;
	}
}

float *glf_top(struct gl_state *st)
{
	struct gl_matrices *s = stack(st);
	return s->m[s->depth - 1];
}

static void touched(struct gl_state *st)
{
	if (st->matrix_mode != GL_TEXTURE)
		st->mvp_stale = st->normal_stale = true;
	else
		st->texmat_stale = true;
}

/* out = a b (out may be either) */
static void mult(float out[16], const float a[16], const float b[16])
{
	float r[16];
	for (int c = 0; c < 4; c++)
		for (int i = 0; i < 4; i++)
			r[4 * c + i] = a[i] * b[4 * c] + a[4 + i] * b[4 * c + 1] + a[8 + i] * b[4 * c + 2] +
				       a[12 + i] * b[4 * c + 3];
	memcpy(out, r, sizeof r);
}

void glf_matrix_mode(struct gl_state *st, uint32_t mode)
{
	if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE)
		glf_error(st, GL_INVALID_ENUM);
	else
		st->matrix_mode = mode;
}

void glf_load(struct gl_state *st, const float m[16])
{
	memcpy(glf_top(st), m, 16 * sizeof *m);
	touched(st);
}

void glf_mult(struct gl_state *st, const float m[16])
{
	float *top = glf_top(st);
	mult(top, top, m);
	touched(st);
}

void glf_identity(struct gl_state *st)
{
	identity(glf_top(st));
	touched(st);
}

void glf_translate(struct gl_state *st, float x, float y, float z)
{
	float m[16];
	identity(m);
	m[12] = x;
	m[13] = y;
	m[14] = z;
	glf_mult(st, m);
}

void glf_scale(struct gl_state *st, float x, float y, float z)
{
	float m[16];
	identity(m);
	m[0] = x;
	m[5] = y;
	m[10] = z;
	glf_mult(st, m);
}

/*
 * sin and cos by their series on at most 45 degrees (after exact steps of
 * 90), so not the host's libm, whose last bits differ between systems
 */
void glf_sincos(double degrees, double *s, double *c)
{
	if (!isfinite(degrees)) {
		*s = *c = NAN;
		return;
	}
	double a = fmod(degrees, 360.0);
	if (a < 0)
		a += 360.0;
	unsigned quadrant = 0;
	while (a >= 90.0 && quadrant < 4) {
		a -= 90.0;
		quadrant++;
	}
	bool flip = a > 45.0;           /* (sin of the rest: cos of 90 less it) */
	double x = (flip ? 90.0 - a : a) * (3.14159265358979323846 / 180.0), x2 = x * x;
	double sn = x * (1 + x2 * (-1.0 / 6 + x2 * (1.0 / 120 + x2 * (-1.0 / 5040 + x2 * (1.0 / 362880 +
		    x2 * (-1.0 / 39916800 + x2 * (1.0 / 6227020800.0 + x2 * (-1.0 / 1307674368000.0 +
		    x2 * (1.0 / 355687428096000.0)))))))));
	double cs = 1 + x2 * (-1.0 / 2 + x2 * (1.0 / 24 + x2 * (-1.0 / 720 + x2 * (1.0 / 40320 +
		    x2 * (-1.0 / 3628800 + x2 * (1.0 / 479001600 + x2 * (-1.0 / 87178291200.0 +
		    x2 * (1.0 / 20922789888000.0))))))));
	if (flip) {
		double t = sn;
		sn = cs;
		cs = t;
	}
	switch (quadrant & 3) {
	case 0: *s = sn; *c = cs; break;
	case 1: *s = cs; *c = -sn; break;
	case 2: *s = -sn; *c = -cs; break;
	default: *s = -cs; *c = sn; break;
	}
}

void glf_rotate(struct gl_state *st, float angle, float x, float y, float z)
{
	float len = sqrtf(x * x + y * y + z * z);
	if (!(len > 0))
		return;                 /* (as Mesa: no axis, no rotation) */
	x /= len;
	y /= len;
	z /= len;
	double sd, cd;
	glf_sincos(angle, &sd, &cd);
	float s = (float)sd, c = (float)cd, one_c = 1 - c;
	float m[16];
	identity(m);
	m[0] = one_c * x * x + c;
	m[4] = one_c * x * y - z * s;
	m[8] = one_c * z * x + y * s;
	m[1] = one_c * x * y + z * s;
	m[5] = one_c * y * y + c;
	m[9] = one_c * y * z - x * s;
	m[2] = one_c * z * x - y * s;
	m[6] = one_c * y * z + x * s;
	m[10] = one_c * z * z + c;
	glf_mult(st, m);
}

/* (as Mesa: the arguments as floats, then the matrix in floats) */
void glf_frustum(struct gl_state *st, double l, double r, double b, double t, double n, double f)
{
	float L = (float)l, R = (float)r, B = (float)b, T = (float)t, N = (float)n, F = (float)f;
	if (!(n > 0) || !(f > 0) || l == r || b == t || n == f) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	float m[16] = { 0 };
	m[0] = 2 * N / (R - L);
	m[5] = 2 * N / (T - B);
	m[8] = (R + L) / (R - L);
	m[9] = (T + B) / (T - B);
	m[10] = -(F + N) / (F - N);
	m[11] = -1;
	m[14] = -(2 * F * N) / (F - N);
	glf_mult(st, m);
}

void glf_ortho(struct gl_state *st, double l, double r, double b, double t, double n, double f)
{
	float L = (float)l, R = (float)r, B = (float)b, T = (float)t, N = (float)n, F = (float)f;
	if (l == r || b == t || n == f) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	float m[16] = { 0 };
	m[0] = 2 / (R - L);
	m[5] = 2 / (T - B);
	m[10] = -2 / (F - N);
	m[12] = -(R + L) / (R - L);
	m[13] = -(T + B) / (T - B);
	m[14] = -(F + N) / (F - N);
	m[15] = 1;
	glf_mult(st, m);
}

void glf_push(struct gl_state *st)
{
	struct gl_matrices *s = stack(st);
	if (s->depth >= s->max) {
		glf_error(st, GL_STACK_OVERFLOW);
		return;
	}
	memcpy(s->m[s->depth], s->m[s->depth - 1], sizeof s->m[0]);
	s->depth++;
}

void glf_pop(struct gl_state *st)
{
	struct gl_matrices *s = stack(st);
	if (s->depth <= 1) {
		glf_error(st, GL_STACK_UNDERFLOW);
		return;
	}
	s->depth--;
	touched(st);
}

static const float *mvp(struct gl_state *st)
{
	if (st->mvp_stale) {
		mult(st->mvp, st->projection.m[st->projection.depth - 1], st->modelview.m[st->modelview.depth - 1]);
		st->mvp_stale = false;
	}
	return st->mvp;
}

/* ---- the raster state ------------------------------------------------------------------- */

static unsigned z_func(uint32_t f)
{
	static const unsigned map[] = { RASTER_Z_FALSE, RASTER_Z_LT, RASTER_Z_EQ, RASTER_Z_LE, RASTER_Z_GT,
					RASTER_Z_NE, RASTER_Z_GE, RASTER_Z_TRUE };
	return map[(f - GL_NEVER) & 7];
}

static unsigned blend_eq(uint32_t eq)
{
	switch (eq) {
	case GL_MIN: return RASTER_EQ_MIN;
	case GL_MAX: return RASTER_EQ_MAX;
	case GL_FUNC_SUBTRACT: return RASTER_EQ_SUBTRACT;
	case GL_FUNC_REVERSE_SUBTRACT: return RASTER_EQ_REVERSE_SUBTRACT;
	default: return RASTER_EQ_ADD;
	}
}

/* [l, r) x [t, b) cut to a rectangle of OpenGL's (x, y up from the bottom, w, h) in a target h high */
static void cut(int *l, int *t, int *r, int *b, const int32_t gl[4], int height)
{
	int64_t x0 = gl[0], x1 = (int64_t)gl[0] + gl[2];
	int64_t y0 = (int64_t)height - ((int64_t)gl[1] + gl[3]), y1 = (int64_t)height - gl[1];
	*l = x0 > *l ? (int)x0 : *l;
	*r = x1 < *r ? (int)x1 : *r;
	*t = y0 > *t ? (int)y0 : *t;
	*b = y1 < *b ? (int)y1 : *b;
}

/*
 * A unit's stage: its texture environment's function (OpenGL 1.2's table
 * 3.18, and GL_ADD's), for its texture's base format, as raster.h's
 * operations on the previous stage's color (P), the texture's (T) and the
 * environment's (C)
 */
static void env_stage(struct raster_stage *s, unsigned unit, uint32_t mode, uint32_t base, const float color[4])
{
	enum { P = RASTER_PREVIOUS, C = RASTER_CONSTANT, TA = RASTER_ALPHA };
	const uint8_t T = (uint8_t)(RASTER_TEXTURE0 + unit);
	/* (each: rgb's operation and arguments, then alpha's) */
	struct f {
		uint8_t op, a0, a1, a2;
	} rgb = { RASTER_OP_REPLACE, P, 0, 0 }, alpha = { RASTER_OP_REPLACE, P, 0, 0 };
	const struct f keep = { RASTER_OP_REPLACE, P, 0, 0 }, take = { RASTER_OP_REPLACE, T, 0, 0 };
	const struct f modulate = { RASTER_OP_MODULATE, P, T, 0 }, add = { RASTER_OP_ADD, P, T, 0 };
	const struct f blend = { RASTER_OP_INTERPOLATE, C, P, T };
	bool a = base == GL_ALPHA, l = base == GL_LUMINANCE, la = base == GL_LUMINANCE_ALPHA, i = base == GL_INTENSITY;
	bool rgba = base == GL_RGBA, with_alpha = a || la || rgba;
	switch (mode) {
	case GL_REPLACE:
		rgb = a ? keep : take;
		alpha = with_alpha || i ? take : keep;
		break;
	case GL_DECAL:                          /* (only RGB and RGBA have it) */
		if (base == GL_RGB) {
			rgb = take;
		} else if (rgba) {
			rgb = (struct f){ RASTER_OP_INTERPOLATE, T, P, T | TA };
		}
		break;
	case GL_BLEND:
		rgb = a ? keep : blend;
		alpha = i ? blend : with_alpha ? modulate : keep;
		break;
	case GL_ADD:
		rgb = a ? keep : add;
		alpha = i ? add : with_alpha ? modulate : keep;
		break;
	default:                                /* GL_MODULATE */
		rgb = a ? keep : modulate;
		alpha = with_alpha || i ? modulate : keep;
		break;
	}
	(void)l;
	memset(s, 0, sizeof *s);
	s->op[0] = rgb.op;
	s->op[1] = alpha.op;
	const uint8_t args[2][3] = { { rgb.a0, rgb.a1, rgb.a2 }, { alpha.a0, alpha.a1, alpha.a2 } };
	memcpy(s->arg, args, sizeof args);
	s->clamp[0] = s->clamp[1] = true;
	s->constant[0] = color[3];
	for (int k = 0; k < 3; k++)
		s->constant[k + 1] = color[k];
}

/* GL_EXT_texture_env_combine's: the stage as the unit's functions, sources, operands and scales have it */
static void combine_stage(struct raster_stage *s, unsigned unit, const struct gl_combine *cb, const float color[4])
{
	memset(s, 0, sizeof *s);
	for (int g = 0; g < 2; g++) {
		uint32_t f = cb->function[g];
		s->op[g] = f == GL_REPLACE ? RASTER_OP_REPLACE : f == GL_ADD ? RASTER_OP_ADD :
			   f == GL_ADD_SIGNED_EXT ? RASTER_OP_ADD_SIGNED : f == GL_INTERPOLATE_EXT ? RASTER_OP_INTERPOLATE :
			   RASTER_OP_MODULATE;
		for (int k = 0; k < 3; k++) {
			uint32_t src = cb->source[g][k], op = cb->operand[g][k];
			unsigned a = src == GL_TEXTURE ? RASTER_TEXTURE0 + unit : src == GL_CONSTANT_EXT ? RASTER_CONSTANT :
				     src == GL_PRIMARY_COLOR_EXT ? RASTER_COLOR0 : RASTER_PREVIOUS;
			if (op == GL_SRC_ALPHA || op == GL_ONE_MINUS_SRC_ALPHA)
				a |= RASTER_ALPHA;
			if (op == GL_ONE_MINUS_SRC_COLOR || op == GL_ONE_MINUS_SRC_ALPHA)
				a |= RASTER_INVERT;
			s->arg[g][k] = (uint8_t)a;
		}
		s->shift[g] = cb->scale[g] == 4 ? 2 : cb->scale[g] == 2 ? 1 : 0;
		s->clamp[g] = true;
	}
	s->constant[0] = color[3];
	for (int k = 0; k < 3; k++)
		s->constant[k + 1] = color[k];
}

/* The texture a unit would draw with (enabled and bound; complete or not) */
static const struct gl_texture *unit_candidate(const struct gl_state *st, unsigned unit)
{
	unsigned bit = 1u << unit;
	int k = st->caps[GLF_CAP_TEXTURE_3D] & bit ? GLF_3D : st->caps[GLF_CAP_TEXTURE_2D] & bit ? GLF_2D :
		st->caps[GLF_CAP_TEXTURE_1D] & bit ? GLF_1D : -1;
	return k < 0 ? NULL : st->units[unit].bound[k];
}

/* Whether the raster state made last has the textures there are to draw with */
static bool textures_current(const struct gl_state *st)
{
	for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++) {
		const struct gl_texture *t = unit_candidate(st, u);
		if (t != st->raster_tex[u] || (t && t->serial != st->raster_serial[u]))
			return false;
	}
	return true;
}

/* The units' textures and stages */
static void textures(struct gl_state *st, struct raster_state *s)
{
	unsigned n = 0;
	for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++) {
		struct raster_texture *drawn = NULL;
		struct gl_texture *t = glf_unit_texture(st, u, &drawn);
		const struct gl_texture *c = unit_candidate(st, u);
		st->raster_tex[u] = c;
		st->raster_serial[u] = c ? c->serial : 0;
		if (!t)
			continue;
		struct raster_unit *r = &s->unit[u];
		r->texture = drawn;
		r->min_filter = (uint8_t)(t->min_filter >= GL_NEAREST_MIPMAP_NEAREST ?
					  RASTER_NEAREST_MIPMAP_NEAREST + (t->min_filter - GL_NEAREST_MIPMAP_NEAREST) :
					  RASTER_NEAREST + (t->min_filter - GL_NEAREST));
		r->mag_filter = (uint8_t)(RASTER_NEAREST + (t->mag_filter - GL_NEAREST));
		r->clamp_s = t->wrap_s != GL_REPEAT;           /* (GL_CLAMP as GL_CLAMP_TO_EDGE: see glfront.h) */
		r->clamp_t = t->wrap_t != GL_REPEAT;
		r->clamp_r = t->wrap_r != GL_REPEAT;
		struct raster_lod *l = &s->lod[u];
		float bias = st->lod_bias[u];
		l->bias = bias < -GLF_MAX_LOD_BIAS ? -GLF_MAX_LOD_BIAS : bias > GLF_MAX_LOD_BIAS ? GLF_MAX_LOD_BIAS :
			  bias == bias ? bias : 0;
		if (t->min_lod > -1000 || t->max_lod < 1000) {         /* (GL_TEXTURE_MIN_LOD's, _MAX_LOD's: not as at first) */
			l->clamped = true;
			l->min = t->min_lod;
			l->max = t->max_lod;
		}
		if (st->units[u].env_mode == GL_COMBINE_EXT)
			combine_stage(&s->stage[n++], u, &st->combine[u], st->units[u].env_color);
		else
			env_stage(&s->stage[n++], u, st->units[u].env_mode, t->level[t->base_level].base,
				  st->units[u].env_color);
	}
	s->stages = n;
	st->textured = n > 0;
}

static unsigned stencil_op(uint32_t op)
{
	switch (op) {
	case GL_ZERO: return RASTER_STENCIL_ZERO;
	case GL_REPLACE: return RASTER_STENCIL_REPLACE;
	case GL_INCR: return RASTER_STENCIL_INCR;
	case GL_DECR: return RASTER_STENCIL_DECR;
	case GL_INVERT: return RASTER_STENCIL_INVERT;
	case GL_INCR_WRAP_EXT: return RASTER_STENCIL_INCR_WRAP;
	case GL_DECR_WRAP_EXT: return RASTER_STENCIL_DECR_WRAP;
	default: return RASTER_STENCIL_KEEP;
	}
}

/* Fog; the secondary color added after texturing (separate specular, or GL_COLOR_SUM_EXT's); stenciling */
static void fog_sum_stencil(struct gl_state *st, const struct gl_target *t, struct raster_state *s)
{
	if (st->caps[GLF_CAP_FOG]) {
		s->fog_mode = st->fog_mode == GL_LINEAR ? RASTER_FOG_LINEAR :
			      st->fog_mode == GL_EXP ? RASTER_FOG_EXP : RASTER_FOG_EXP2;
		s->fog_coord = true;
		s->fog_color[0] = st->fog_color[3];
		for (int k = 0; k < 3; k++)
			s->fog_color[k + 1] = st->fog_color[k];
		s->fog_start = st->fog_start;
		s->fog_end = st->fog_end;
		s->fog_density = st->fog_density;
	}
	bool sum = st->caps[GLF_CAP_LIGHTING] ? st->separate_specular : st->caps[GLF_CAP_COLOR_SUM] != 0;
	if (sum && s->stages < RASTER_STAGES) {
		struct raster_stage *g = &s->stage[s->stages++];
		memset(g, 0, sizeof *g);
		g->op[0] = RASTER_OP_ADD;
		g->arg[0][0] = RASTER_PREVIOUS;
		g->arg[0][1] = RASTER_COLOR1;
		g->op[1] = RASTER_OP_REPLACE;
		g->arg[1][0] = RASTER_PREVIOUS;
		g->clamp[0] = g->clamp[1] = true;
		st->textured = true;            /* (drawn with the stages, textures or not) */
	}
	if (st->caps[GLF_CAP_STENCIL_TEST] && t->stencil_bits) {
		s->stencil_func = z_func(st->stencil_func);
		s->stencil_ref = (uint8_t)(st->stencil_ref < 0 ? 0 : st->stencil_ref > 255 ? 255 : st->stencil_ref);
		s->stencil_mask = (uint8_t)st->stencil_mask;
		s->stencil_write = (uint8_t)st->stencil_write_mask;
		s->stencil_op[0] = (uint8_t)stencil_op(st->stencil_fail);
		s->stencil_op[1] = (uint8_t)stencil_op(st->stencil_zfail);
		s->stencil_op[2] = (uint8_t)stencil_op(st->stencil_zpass);
	}
}

/* What draws primitives in the current state into the target */
static const struct raster_state *raster(struct gl_state *st, const struct gl_target *t)
{
	if (!st->raster_stale && st->raster_width == t->width && st->raster_height == t->height &&
	    textures_current(st))
		return &st->raster;
	struct raster_state *s = &st->raster;
	memset(s, 0, sizeof *s);
	textures(st, s);
	fog_sum_stencil(st, t, s);
	bool depth = st->caps[GLF_CAP_DEPTH_TEST] && t->depth_bits;
	s->z_func = depth ? z_func(st->depth_func) : RASTER_Z_NONE;
	s->z_write = depth && st->depth_mask;
	s->all_fragments = true;
	bool alpha_test = st->caps[GLF_CAP_ALPHA_TEST] && st->alpha_func != GL_ALWAYS;
	s->alpha_test = alpha_test ? (unsigned)(RASTER_ALPHA_LT + (st->alpha_func - GL_LESS)) : RASTER_ALPHA_NONE;
	s->alpha_ref = st->alpha_ref;
	if (st->caps[GLF_CAP_COLOR_LOGIC_OP]) {     /* (blending's off then) */
		s->blend = st->logic_op == GL_COPY ? RASTER_BLEND_NONE : RASTER_BLEND_LOGIC;
		s->logic_op = st->logic_op - GL_CLEAR;
	} else if (st->caps[GLF_CAP_BLEND]) {
		s->blend = RASTER_BLEND_FACTORS;
		s->blend_src = (unsigned)blend_factor(st->blend_src);
		s->blend_dst = (unsigned)blend_factor(st->blend_dst);
		s->blend_eq = blend_eq(st->blend_eq);
		s->blend_color[0] = st->blend_color[3];
		for (int k = 0; k < 3; k++)
			s->blend_color[k + 1] = st->blend_color[k];
	} else {
		s->blend = RASTER_BLEND_NONE;
	}
	/* (no alpha bits: alpha stays 1, as the clears leave it) */
	s->color_keep = (st->color_mask[0] ? 0 : 0x00FF0000u) | (st->color_mask[1] ? 0 : 0x0000FF00u) |
			(st->color_mask[2] ? 0 : 0x000000FFu) | (st->color_mask[3] && t->alpha_bits ? 0 : 0xFF000000u);
	s->width = 1;
	s->gl_points = true;
	if (st->caps[GLF_CAP_POLYGON_STIPPLE]) {
		s->polygon_stippled = true;
		memcpy(s->polygon_stipple, st->polygon_stipple, sizeof s->polygon_stipple);
	}
	int l = 0, top = 0, r = t->width, b = t->height;
	cut(&l, &top, &r, &b, st->viewport, t->height);
	if (st->caps[GLF_CAP_SCISSOR_TEST])
		cut(&l, &top, &r, &b, st->scissor, t->height);
	s->clip_left = l;
	s->clip_top = top;
	s->clip_right = r > l ? r : l;
	s->clip_bottom = b > top ? b : top;
	/* nothing is drawn with alpha never passing, or every channel masked (and no stencil written) */
	bool stencil_writes = s->stencil_func != RASTER_Z_NONE && s->stencil_write;
	st->draw_none = (st->caps[GLF_CAP_ALPHA_TEST] && st->alpha_func == GL_NEVER) ||
			(s->color_keep == 0xFFFFFFFFu && !s->z_write && !stencil_writes) || r <= l || b <= top;
	st->raster_stale = false;
	st->raster_width = t->width;
	st->raster_height = t->height;
	return s;
}

/* ---- primitives -------------------------------------------------------------------------- */

/*
 * Clip planes: near and far, then the guard band's (the view volume's
 * sides, for selection and feedback: guard 1), then the user's (a vertex
 * is inside plane k if dist(v, k) >= 0; a user plane that isn't enabled is
 * 0 at every vertex)
 */
enum { P_NEAR, P_FAR, P_LEFT, P_RIGHT, P_BOTTOM, P_TOP, P_USER, PLANES = P_USER + GLF_CLIP_PLANES };
#define MAX_POLY (3 + PLANES)

static float dist(const struct gl_vertex *v, unsigned plane, float guard)
{
	const float *c = v->clip;
	switch (plane) {
	case P_NEAR: return c[3] + c[2];
	case P_FAR: return c[3] - c[2];
	case P_LEFT: return guard * c[3] + c[0];
	case P_RIGHT: return guard * c[3] - c[0];
	case P_BOTTOM: return guard * c[3] + c[1];
	case P_TOP: return guard * c[3] - c[1];
	default: return v->user[plane - P_USER];
	}
}

/* The planes a vertex is outside (bit k: plane k) */
static unsigned outside(const struct gl_vertex *v, float guard)
{
	unsigned out = 0;
	for (unsigned k = 0; k < PLANES; k++)
		if (dist(v, k, guard) < 0)
			out |= 1u << k;
	return out;
}

static float guard_of(const struct gl_state *st)
{
	return glf_selecting(st) ? 1.0f : GLF_GUARD;
}

/*
 * The point t of the way from a (inside) to b (outside): always from the
 * inside, so primitives sharing an edge share where it's cut
 */
static void lerp(struct gl_vertex *out, const struct gl_vertex *a, const struct gl_vertex *b, float t)
{
	const float *p = (const float *)a, *q = (const float *)b;
	float *o = (float *)out;
	for (size_t k = 0; k < sizeof *a / sizeof(float); k++)
		o[k] = p[k] + (q[k] - p[k]) * t;
}

/* Its window coordinates (x, y up from the bottom), and as raster.h's (y down), with a face's colors */
static bool project(const struct gl_state *st, const struct gl_target *t, const struct gl_vertex *v, unsigned face,
		    struct raster_vertex *r, float win[2])
{
	if (!(v->clip[3] > 0))
		return false;
	float iw = 1.0f / v->clip[3];
	float x = v->clip[0] * iw * st->vp_scale[0] + st->vp_offset[0];
	float y = v->clip[1] * iw * st->vp_scale[1] + st->vp_offset[1];
	memset(r, 0, sizeof *r);
	r->x = x;
	r->y = (float)t->height - y;
	r->z = v->clip[2] * iw * st->vp_scale[2] + st->vp_offset[2];
	r->invw = iw;
	memcpy(r->color[0], v->color[face], sizeof v->color[face]);
	memcpy(r->color[1], v->secondary[face], sizeof v->secondary[face]);
	r->fog = v->fog * iw;
	for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++) {      /* (divided by w: see raster.h) */
		r->tex[u][0] = v->tex[u][0] * iw;
		r->tex[u][1] = v->tex[u][1] * iw;
		r->tex[u][2] = v->tex[u][3] * iw;
		r->texr[u] = v->tex[u][2] * iw;
	}
	win[0] = x;
	win[1] = y;
	return true;
}

/*
 * Lines and points into the backend, as given: textured as triangles are
 * (their coordinates divided by q if a vertex's q isn't 1), smooth or not,
 * sizes within GL_POINT_SIZE_RANGE's; lines stippled, the counter going on
 * from the line before unless the stipple's been reset since (each separate
 * line's, each strip's or loop's first, each polygon's first edge: see
 * glf_vertex())
 */
static struct raster_cmd *emit(struct gl_state *st, const struct gl_target *t, const struct raster_vertex *v,
			       unsigned n, uint8_t kind, float width, bool smooth)
{
	struct raster_state s = *raster(st, t);
	s.width = width < 1 ? 1 : width > GLF_MAX_POINT_SIZE ? GLF_MAX_POINT_SIZE : width;
	s.smooth = smooth;
	for (unsigned u = 0; st->textured && u < GLF_TEXTURE_UNITS; u++)
		for (unsigned k = 0; k < n; k++)
			s.unit[u].projective |= s.unit[u].texture && v[k].tex[u][2] != v[k].invw;
	if (kind == RASTER_CMD_LINE && st->caps[GLF_CAP_LINE_STIPPLE]) {
		s.line_stippled = true;
		s.line_stipple = st->line_stipple;
		s.line_stipple_factor = (uint16_t)st->line_stipple_factor;
	}
	struct raster_cmd *cmd = surface_cmd(t->surface, &s, kind);
	if (!cmd) {
		glf_error(st, GL_OUT_OF_MEMORY);
		return NULL;
	}
	cmd->textured = st->textured;
	memcpy(cmd->v, v, n * sizeof *v);
	st->primitives++;
	if (kind == RASTER_CMD_LINE) {
		if (st->select.line_reset)
			st->stipple_counter = 0;
		st->select.line_reset = false;
		cmd->stipple = st->stipple_counter;
		if (s.line_stippled)
			st->stipple_counter += raster_gl_fragments(&s, cmd, 0, 0, 0, 0, NULL, NULL);
	}
	return cmd;
}

static void emit_line(struct gl_state *st, const struct gl_target *t, const struct raster_vertex *a,
		      const struct raster_vertex *b)
{
	const struct raster_vertex v[2] = { *a, *b };
	emit(st, t, v, 2, RASTER_CMD_LINE, st->line_width, st->caps[GLF_CAP_LINE_SMOOTH]);
}

static void emit_point(struct gl_state *st, const struct gl_target *t, const struct raster_vertex *v)
{
	emit(st, t, v, 1, RASTER_CMD_POINT, st->point_size, st->caps[GLF_CAP_POINT_SMOOTH]);
}

/* A vertex for selection and feedback: its window coordinates (y up), its depth (offset), a face's color */
static void fb_vertex(const struct gl_vertex *v, const float win[2], float z, unsigned face, struct glf_fb_vertex *f)
{
	f->win[0] = win[0];
	f->win[1] = win[1];
	f->win[2] = z;
	f->win[3] = v->clip[3];
	memcpy(f->color, v->color[face], sizeof f->color);
	memcpy(f->tex, v->tex[0], sizeof f->tex);
}

/* A line for selection and feedback (the first since the stipple was reset: GL_LINE_RESET_TOKEN) */
static void select_line(struct gl_state *st, const struct glf_fb_vertex v[2])
{
	glf_select_primitive(st, st->select.line_reset ? GL_LINE_RESET_TOKEN : GL_LINE_TOKEN, v, 2);
	st->select.line_reset = false;
}

/*
 * Polygon offset (OpenGL 1.2's 3.5.5): the polygon's largest depth slope
 * times the factor, plus the units times the depth buffer's resolution
 */
static float offset_of(const struct gl_state *st, const struct gl_target *t, const struct raster_vertex *rv,
		       unsigned n)
{
	float m = 0;
	for (unsigned k = 1; k + 1 < n; k++) {
		float x1 = rv[k].x - rv[0].x, y1 = rv[k].y - rv[0].y, z1 = rv[k].z - rv[0].z;
		float x2 = rv[k + 1].x - rv[0].x, y2 = rv[k + 1].y - rv[0].y, z2 = rv[k + 1].z - rv[0].z;
		float area = x1 * y2 - x2 * y1;
		if (!(area != 0))
			continue;
		float dzdx = fabsf((z1 * y2 - z2 * y1) / area), dzdy = fabsf((x1 * z2 - x2 * z1) / area);
		m = dzdx > dzdy ? dzdx : dzdy;
		break;
	}
	unsigned bits = t->depth_bits && t->depth_bits < 24 ? t->depth_bits : 24;
	return st->offset_factor * m + st->offset_units / (float)((1u << bits) - 1);
}

/* Edges of a triangle that are its polygon's (bit k: from vertex k to the next); with E_FLAGS, edge flags count */
#define E_AB 1u
#define E_BC 2u
#define E_CA 4u
#define E_FLAGS 8u

/*
 * A triangle, clipped, facing the way its window area says, culled, then
 * filled or outlined (polygon modes: its polygon's edges as lines, or
 * their first vertices as points, but for edges clipping made)
 */
static void triangle(struct gl_state *st, const struct gl_target *t, const struct gl_vertex *a,
		     const struct gl_vertex *b, const struct gl_vertex *c, unsigned edges)
{
	st->polygon_face = -1;
	struct gl_vertex buf[2][MAX_POLY];
	const struct gl_vertex *in[3] = { a, b, c };
	unsigned codes = 0, all = ~0u;
	float guard = guard_of(st);
	for (int k = 0; k < 3; k++) {
		unsigned o = outside(in[k], guard);
		codes |= o;
		all &= o;
	}
	if (all)
		return;
	unsigned n = 3, cur = 0;
	for (int k = 0; k < 3; k++) {
		buf[0][k] = *in[k];
		buf[0][k].edge = edges >> k & 1 && (!(edges & E_FLAGS) || in[k]->edge > 0.5f) ? 1 : 0;
	}
	for (unsigned plane = 0; plane < PLANES && codes; plane++) {
		if (!(codes >> plane & 1))
			continue;
		const struct gl_vertex *p = buf[cur];
		struct gl_vertex *o = buf[cur ^ 1];
		unsigned m = 0;
		for (unsigned i = 0; i < n; i++) {
			const struct gl_vertex *u = &p[i], *v = &p[(i + 1) % n];
			float du = dist(u, plane, guard), dv = dist(v, plane, guard);
			if (du >= 0)
				o[m++] = *u;
			if ((du >= 0) != (dv >= 0)) {
				if (du >= 0) {
					lerp(&o[m], u, v, du / (du - dv));
					o[m++].edge = 0;                /* (along the plane: not the polygon's) */
				} else {
					lerp(&o[m], v, u, dv / (dv - du));
					o[m++].edge = u->edge;
				}
			}
		}
		n = m;
		cur ^= 1;
		if (n < 3)
			return;
	}
	const struct gl_vertex *poly = buf[cur];
	struct raster_vertex rv[MAX_POLY];
	float win[MAX_POLY][2];
	for (unsigned k = 0; k < n; k++)
		if (!project(st, t, &poly[k], 0, &rv[k], win[k]))
			return;
	/* facing: the clipped polygon's area in window coordinates (counterclockwise positive) */
	float area = 0;
	for (unsigned k = 1; k + 1 < n; k++)
		area += (win[k][0] - win[0][0]) * (win[k + 1][1] - win[0][1]) -
			(win[k + 1][0] - win[0][0]) * (win[k][1] - win[0][1]);
	if (!(area != 0))
		return;
	bool front = (area > 0) == (st->front_face == GL_CCW);
	if (st->caps[GLF_CAP_CULL_FACE] &&
	    (st->cull_mode == GL_FRONT_AND_BACK || (st->cull_mode == GL_FRONT) == front))
		return;
	unsigned face = front ? 0 : 1;
	if (face)                               /* (the back's colors: two-sided lighting's) */
		for (unsigned k = 0; k < n && k < MAX_POLY; k++) {
			memcpy(rv[k].color[0], poly[k].color[1], sizeof rv[k].color[0]);
			memcpy(rv[k].color[1], poly[k].secondary[1], sizeof rv[k].color[1]);
		}
	uint32_t mode = st->polygon_mode[face];
	unsigned offset_cap = mode == GL_FILL ? GLF_CAP_OFFSET_FILL : mode == GL_LINE ? GLF_CAP_OFFSET_LINE :
			      GLF_CAP_OFFSET_POINT;
	float offset = st->caps[offset_cap] ? offset_of(st, t, rv, n) : 0;
	if (offset != 0)
		for (unsigned k = 0; k < n; k++) {
			float z = rv[k].z + offset;
			rv[k].z = z > 0 ? (z < 1 ? z : 1) : 0;
		}
	st->polygon_face = (int)face;
	st->polygon_offset = offset;
	if (glf_selecting(st)) {
		/*
		 * Instead: as triangles, in Mesa's order (the order is OpenGL's
		 * to choose): a polygon's, or a clipped one's, as (v[i], v[i + 1],
		 * v[0]); outlined, the closing edge first (each the first of a
		 * polygon's lines since the stipple was reset: see glf_vertex())
		 */
		struct glf_fb_vertex fv[MAX_POLY];
		for (unsigned k = 0; k < n; k++)
			fb_vertex(&poly[k], win[k], rv[k].z, face, &fv[k]);
		for (unsigned j = 0; mode != GL_FILL && j < n; j++) {
			unsigned k = mode == GL_LINE ? (j + n - 1) % n : j;
			if (!(poly[k].edge > 0.5f))
				continue;
			if (mode == GL_LINE)
				select_line(st, (const struct glf_fb_vertex[2]){ fv[k], fv[(k + 1) % n] });
			else
				glf_select_primitive(st, GL_POINT_TOKEN, &fv[k], 1);
		}
		if (mode == GL_FILL && !codes && st->mode != GL_POLYGON)
			glf_select_primitive(st, GL_POLYGON_TOKEN, fv, 3);
		else if (mode == GL_FILL)
			for (unsigned k = 1; k + 1 < n; k++)
				glf_select_primitive(st, GL_POLYGON_TOKEN,
						     (const struct glf_fb_vertex[3]){ fv[k], fv[k + 1], fv[0] }, 3);
		return;
	}
	if (mode != GL_FILL) {
		for (unsigned k = 0; k < n; k++) {
			if (!(poly[k].edge > 0.5f))
				continue;
			if (mode == GL_LINE)
				emit_line(st, t, &rv[k], &rv[(k + 1) % n]);
			else
				emit_point(st, t, &rv[k]);
		}
		return;
	}
	const struct raster_state *s = raster(st, t);
	struct raster_state projective;
	if (st->textured) {                     /* (q not 1: divided by q/w, per pixel) */
		bool q = false;
		for (unsigned k = 0; k < n && !q; k++)
			for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++)
				q |= s->unit[u].texture && poly[k].tex[u][3] != 1;
		if (q) {
			projective = *s;
			for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++)
				projective.unit[u].projective = projective.unit[u].texture != NULL;
			s = &projective;
		}
	}
	for (unsigned k = 1; k + 1 < n; k++) {
		struct raster_cmd *cmd = surface_cmd(t->surface, s, RASTER_CMD_TRIANGLE);
		if (!cmd) {
			glf_error(st, GL_OUT_OF_MEMORY);
			return;
		}
		cmd->textured = st->textured;
		cmd->v[0] = rv[0];
		cmd->v[1] = rv[k];
		cmd->v[2] = rv[k + 1];
		st->primitives++;
	}
}

/* A segment cut to the clip planes: false if none of it is left */
static bool clip_line(struct gl_vertex *a, struct gl_vertex *b, float guard)
{
	unsigned oa = outside(a, guard), ob = outside(b, guard);
	if (oa & ob)
		return false;
	for (unsigned plane = 0; plane < PLANES && (oa | ob); plane++) {
		if (!((oa | ob) >> plane & 1))
			continue;
		float da = dist(a, plane, guard), db = dist(b, plane, guard);
		if (da < 0 && db < 0)
			return false;
		if (da < 0)
			lerp(a, b, a, db / (db - da));
		else if (db < 0)
			lerp(b, a, b, da / (da - db));
	}
	return true;
}

/* A line, with a face's colors (a polygon's edge: and its offset) */
static void line(struct gl_state *st, const struct gl_target *t, const struct gl_vertex *a,
		 const struct gl_vertex *b, unsigned face, float offset)
{
	struct gl_vertex p = *a, q = *b;
	if (!clip_line(&p, &q, guard_of(st)))
		return;
	struct raster_vertex rp, rq;
	float wp[2], wq[2];
	if (!project(st, t, &p, face, &rp, wp) || !project(st, t, &q, face, &rq, wq))
		return;
	if (offset != 0) {
		rp.z = fminf(fmaxf(rp.z + offset, 0), 1);
		rq.z = fminf(fmaxf(rq.z + offset, 0), 1);
	}
	if (glf_selecting(st)) {
		struct glf_fb_vertex fv[2];
		fb_vertex(&p, wp, rp.z, face, &fv[0]);
		fb_vertex(&q, wq, rq.z, face, &fv[1]);
		select_line(st, fv);
		return;
	}
	emit_line(st, t, &rp, &rq);
}

static void point(struct gl_state *st, const struct gl_target *t, const struct gl_vertex *v, unsigned face,
		  float offset)
{
	const float *c = v->clip;
	if (!(c[0] >= -c[3] && c[0] <= c[3] && c[1] >= -c[3] && c[1] <= c[3] && c[2] >= -c[3] && c[2] <= c[3]))
		return;
	for (unsigned k = 0; k < GLF_CLIP_PLANES; k++)
		if (v->user[k] < 0)
			return;
	struct raster_vertex r;
	float win[2];
	if (!project(st, t, v, face, &r, win))
		return;
	if (offset != 0)
		r.z = fminf(fmaxf(r.z + offset, 0), 1);
	if (glf_selecting(st)) {
		struct glf_fb_vertex fv;
		fb_vertex(v, win, r.z, face, &fv);
		glf_select_primitive(st, GL_POINT_TOKEN, &fv, 1);
		return;
	}
	emit_point(st, t, &r);
}

/* ---- glBegin, glVertex, glEnd ------------------------------------------------------------- */

void glf_begin(struct gl_state *st, uint32_t mode)
{
	if (mode > GL_POLYGON) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	st->inside = true;
	st->mode = mode;
	st->count = 0;
	st->polygon_face = -1;
	st->select.line_reset = true;
}

/* Whether this target is drawn into now (or the primitives selected or fed back) */
static bool drawing(struct gl_state *st, const struct gl_target *t)
{
	if (glf_selecting(st))
		return true;
	if (!t->surface || t->width <= 0 || t->height <= 0)
		return false;
	raster(st, t);
	return !st->draw_none;
}

/* Flat shading: the primitive's vertices with the colors of the one that gives them */
static void flat(const struct gl_vertex *const in[], struct gl_vertex *out, unsigned n, const struct gl_vertex *by)
{
	for (unsigned k = 0; k < n; k++) {
		out[k] = *in[k];
		memcpy(out[k].color, by->color, sizeof by->color);
		memcpy(out[k].secondary, by->secondary, sizeof by->secondary);
	}
}

static void tri(struct gl_state *st, const struct gl_target *t, const struct gl_vertex *a, const struct gl_vertex *b,
		const struct gl_vertex *c, const struct gl_vertex *by, unsigned edges)
{
	if (st->shade_model == GL_FLAT) {
		const struct gl_vertex *in[3] = { a, b, c };
		struct gl_vertex v[3];
		flat(in, v, 3, by);
		triangle(st, t, &v[0], &v[1], &v[2], edges);
	} else {
		triangle(st, t, a, b, c, edges);
	}
}

static void seg(struct gl_state *st, const struct gl_target *t, const struct gl_vertex *a, const struct gl_vertex *b,
		const struct gl_vertex *by, unsigned face, float offset)
{
	if (st->shade_model == GL_FLAT) {
		const struct gl_vertex *in[2] = { a, b };
		struct gl_vertex v[2];
		flat(in, v, 2, by);
		line(st, t, &v[0], &v[1], face, offset);
	} else {
		line(st, t, a, b, face, offset);
	}
}

const struct raster_state *glf_raster_state(struct gl_state *st, const struct gl_target *t)
{
	return raster(st, t);
}

void glf_raster_pos(struct gl_state *st, const float obj[4])
{
	const float *m = mvp(st);
	struct gl_vertex v;
	for (int i = 0; i < 4; i++)
		v.clip[i] = m[i] * obj[0] + m[4 + i] * obj[1] + m[8 + i] * obj[2] + m[12 + i] * obj[3];
	glf_shade(st, obj, &v);
	struct gl_raster_pos *r = &st->rpos;
	const float *c = v.clip;
	r->valid = c[0] >= -c[3] && c[0] <= c[3] && c[1] >= -c[3] && c[1] <= c[3] && c[2] >= -c[3] && c[2] <= c[3];
	for (unsigned k = 0; k < GLF_CLIP_PLANES; k++)
		r->valid = r->valid && !(v.user[k] < 0);
	if (!r->valid)
		return;                         /* (the rest of it undefined: as it was) */
	float iw = 1.0f / c[3];
	for (int k = 0; k < 3; k++)
		r->win[k] = c[k] * iw * st->vp_scale[k] + st->vp_offset[k];
	r->win[3] = c[3];
	memcpy(r->color, v.color[0], sizeof r->color);
	memcpy(r->secondary, v.secondary[0], sizeof r->secondary);
	memcpy(r->tex, v.tex, sizeof r->tex);
	const float *mv = st->modelview.m[st->modelview.depth - 1];
	float eye[3];
	for (int i = 0; i < 3; i++)
		eye[i] = mv[i] * obj[0] + mv[4 + i] * obj[1] + mv[8 + i] * obj[2] + mv[12 + i] * obj[3];
	r->distance = sqrtf(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
}

void glf_vertex(struct gl_state *st, const struct gl_target *t, const float obj[4])
{
	if (!st->inside)
		return;                 /* (undefined: nothing) */
	const float *m = mvp(st);
	struct gl_vertex v;
	for (int i = 0; i < 4; i++)
		v.clip[i] = m[i] * obj[0] + m[4 + i] * obj[1] + m[8 + i] * obj[2] + m[12 + i] * obj[3];
	glf_shade(st, obj, &v);
	unsigned k = st->count++;
	struct gl_vertex *vb = st->vb;
	bool draw = drawing(st, t);
	switch (st->mode) {
	case GL_POINTS:
		if (draw)
			point(st, t, &v, 0, 0);
		break;
	case GL_LINES:                          /* (the stipple reset for each) */
		if (k & 1) {
			st->select.line_reset = true;
			if (draw)
				seg(st, t, &vb[0], &v, &v, 0, 0);
		} else {
			vb[0] = v;
		}
		break;
	case GL_LINE_STRIP:
	case GL_LINE_LOOP:                      /* (vb[0]: the first; vb[1]: the last) */
		if (k == 0)
			vb[0] = v;
		else if (draw)
			seg(st, t, &vb[1], &v, &v, 0, 0);
		vb[1] = v;
		break;
	/* (polygons: each a run of lines since the stipple was reset, when outlined: a triangle, a quad, a polygon) */
	case GL_TRIANGLES:
		if (k % 3 < 2) {
			vb[k % 3] = v;
		} else if (draw) {
			st->select.line_reset = true;
			tri(st, t, &vb[0], &vb[1], &v, &v, E_AB | E_BC | E_CA | E_FLAGS);
		}
		break;
	case GL_TRIANGLE_STRIP:                 /* (odd triangles the other way round, for their facing) */
		st->select.line_reset = true;
		if (k >= 2 && draw) {
			if (k & 1)
				tri(st, t, &vb[1], &vb[0], &v, &v, E_AB | E_BC | E_CA);
			else
				tri(st, t, &vb[0], &vb[1], &v, &v, E_AB | E_BC | E_CA);
		}
		vb[0] = vb[1];
		vb[1] = v;
		break;
	case GL_TRIANGLE_FAN:
		st->select.line_reset = true;
		if (k >= 2 && draw)
			tri(st, t, &vb[0], &vb[1], &v, &v, E_AB | E_BC | E_CA);
		vb[k ? 1 : 0] = v;
		break;
	case GL_POLYGON:                        /* (its color is its first vertex's; its closing edge at glEnd) */
		if (k >= 2 && draw)
			tri(st, t, &vb[0], &vb[1], &v, &vb[0], (k == 2 ? E_AB : 0) | E_BC | E_FLAGS);
		vb[k ? 1 : 0] = v;
		break;
	case GL_QUADS:                          /* (as (v0, v1, v3) and (v1, v2, v3)) */
		if (k % 4 < 3) {
			vb[k % 4] = v;
		} else if (draw) {
			st->select.line_reset = true;
			tri(st, t, &vb[0], &vb[1], &v, &v, E_AB | E_CA | E_FLAGS);
			tri(st, t, &vb[1], &vb[2], &v, &v, E_AB | E_BC | E_FLAGS);
		}
		break;
	case GL_QUAD_STRIP:                     /* (quad i: 2i, 2i + 1, 2i + 3, 2i + 2) */
		if (k < 3 || !(k & 1)) {
			vb[k < 3 ? k : 2] = v;
		} else {
			if (draw) {
				st->select.line_reset = true;
				tri(st, t, &vb[2], &vb[0], &v, &v, E_AB | E_CA);
				tri(st, t, &vb[0], &vb[1], &v, &v, E_AB | E_BC);
			}
			vb[0] = vb[2];
			vb[1] = v;
		}
		break;
	}
}

void glf_end(struct gl_state *st, const struct gl_target *t)
{
	if (!st->inside) {
		glf_error(st, GL_INVALID_OPERATION);
		return;
	}
	if (st->mode == GL_LINE_LOOP && st->count >= 2 && drawing(st, t))
		seg(st, t, &st->vb[1], &st->vb[0], &st->vb[0], 0, 0);
	/* a polygon outlined: its closing edge, from its last vertex (facing as its last triangle) */
	int face = st->polygon_face;
	if (st->mode == GL_POLYGON && st->count >= 3 && face >= 0 && st->polygon_mode[face] != GL_FILL &&
	    st->vb[1].edge > 0.5f && drawing(st, t)) {
		if (st->polygon_mode[face] == GL_LINE) {
			seg(st, t, &st->vb[1], &st->vb[0], &st->vb[0], (unsigned)face, st->polygon_offset);
		} else {
			struct gl_vertex last = st->vb[1];
			if (st->shade_model == GL_FLAT) {
				memcpy(last.color, st->vb[0].color, sizeof last.color);
				memcpy(last.secondary, st->vb[0].secondary, sizeof last.secondary);
			}
			point(st, t, &last, (unsigned)face, st->polygon_offset);
		}
	}
	st->inside = false;
}

/* ---- clearing ----------------------------------------------------------------------------- */

void glf_clear(struct gl_state *st, const struct gl_target *t, uint32_t mask)
{
	if (mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT)) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	bool color = mask & GL_COLOR_BUFFER_BIT && (st->color_mask[0] || st->color_mask[1] || st->color_mask[2] ||
						    (st->color_mask[3] && t->alpha_bits));
	bool depth = mask & GL_DEPTH_BUFFER_BIT && st->depth_mask && t->depth_bits;
	bool stencil = mask & GL_STENCIL_BUFFER_BIT && t->stencil_bits && (st->stencil_write_mask & 0xFF);
	if (glf_selecting(st))
		return;                         /* (selecting or feeding back: nothing, as Mesa) */
	if (mask & GL_ACCUM_BUFFER_BIT)
		glf_clear_accum_buffer(st, t);
	if (!t->surface || (!color && !depth && !stencil))
		return;
	struct raster_state s;
	memset(&s, 0, sizeof s);
	s.clip_right = t->width;
	s.clip_bottom = t->height;
	s.stencil_write = (uint8_t)st->stencil_write_mask;
	s.color_keep = (st->color_mask[0] ? 0 : 0x00FF0000u) | (st->color_mask[1] ? 0 : 0x0000FF00u) |
		       (st->color_mask[2] ? 0 : 0x000000FFu) | (st->color_mask[3] || !t->alpha_bits ? 0 : 0xFF000000u);
	struct raster_cmd *c = surface_cmd(t->surface, &s, RASTER_CMD_CLEAR);
	if (!c) {
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	c->rect[0] = 0;
	c->rect[1] = 0;
	c->rect[2] = t->width;
	c->rect[3] = t->height;
	if (st->caps[GLF_CAP_SCISSOR_TEST])
		cut(&c->rect[0], &c->rect[1], &c->rect[2], &c->rect[3], st->scissor, t->height);
	c->clear_color = color;
	c->clear_depth = depth;
	c->clear_stencil = stencil;
	c->stencil = (uint8_t)st->clear_stencil;
	const float *cc = st->clear_color;
	float alpha = t->alpha_bits ? clampf(cc[3]) : 1;        /* (no alpha bits: alpha 1) */
	c->argb = (uint32_t)(alpha * 255 + 0.5f) << 24 | (uint32_t)(clampf(cc[0]) * 255 + 0.5f) << 16 |
		  (uint32_t)(clampf(cc[1]) * 255 + 0.5f) << 8 | (uint32_t)(clampf(cc[2]) * 255 + 0.5f);
	c->z = (float)clampd(st->clear_depth);
}

/* ---- queries ------------------------------------------------------------------------------ */

enum { GL_CURRENT_COLOR = 0x0B00, GL_CURRENT_NORMAL = 0x0B02, GL_CURRENT_TEXTURE_COORDS = 0x0B03,
       GL_POINT_SIZE = 0x0B11, GL_LINE_WIDTH = 0x0B21, GL_CULL_FACE_MODE = 0x0B45, GL_FRONT_FACE = 0x0B46,
       GL_SHADE_MODEL = 0x0B54, GL_DEPTH_RANGE = 0x0B70, GL_DEPTH_WRITEMASK = 0x0B72,
       GL_DEPTH_CLEAR_VALUE = 0x0B73, GL_DEPTH_FUNC = 0x0B74, GL_MATRIX_MODE = 0x0BA0, GL_VIEWPORT = 0x0BA2,
       GL_MODELVIEW_STACK_DEPTH = 0x0BA3, GL_PROJECTION_STACK_DEPTH = 0x0BA4, GL_TEXTURE_STACK_DEPTH = 0x0BA5,
       GL_MODELVIEW_MATRIX = 0x0BA6, GL_PROJECTION_MATRIX = 0x0BA7, GL_TEXTURE_MATRIX = 0x0BA8,
       GL_ALPHA_TEST_FUNC = 0x0BC1, GL_ALPHA_TEST_REF = 0x0BC2, GL_BLEND_DST = 0x0BE0, GL_BLEND_SRC = 0x0BE1,
       GL_SCISSOR_BOX = 0x0C10, GL_COLOR_CLEAR_VALUE = 0x0C22, GL_COLOR_WRITEMASK = 0x0C23,
       GL_MAX_MODELVIEW_STACK_DEPTH = 0x0D36, GL_MAX_PROJECTION_STACK_DEPTH = 0x0D38,
       GL_MAX_TEXTURE_STACK_DEPTH = 0x0D39, GL_MAX_VIEWPORT_DIMS = 0x0D3A, GL_BLEND_COLOR = 0x8005,
       GL_BLEND_EQUATION = 0x8009, GL_ACTIVE_TEXTURE_ARB = 0x84E0, GL_MAX_TEXTURE_UNITS_ARB = 0x84E2,
       GL_CURRENT_SECONDARY_COLOR_EXT = 0x8459, GL_CURRENT_FOG_COORDINATE_EXT = 0x8453, GL_EDGE_FLAG = 0x0B43,
       GL_POINT_SIZE_RANGE = 0x0B12, GL_LINE_WIDTH_RANGE = 0x0B22 };

static unsigned values(double *v, const float *f, unsigned n)
{
	for (unsigned k = 0; k < n; k++)
		v[k] = f[k];
	return n;
}

unsigned glf_get(struct gl_state *st, uint32_t pname, double *v)
{
	int on = glf_is_enabled(st, pname);
	if (on >= 0) {
		v[0] = on;
		return 1;
	}
	switch (pname) {
	case GL_VIEWPORT: for (int k = 0; k < 4; k++) v[k] = st->viewport[k]; return 4;
	case GL_SCISSOR_BOX: for (int k = 0; k < 4; k++) v[k] = st->scissor[k]; return 4;
	case GL_COLOR_CLEAR_VALUE: return values(v, st->clear_color, 4);
	case GL_COLOR_WRITEMASK: for (int k = 0; k < 4; k++) v[k] = st->color_mask[k]; return 4;
	case GL_DEPTH_CLEAR_VALUE: v[0] = st->clear_depth; return 1;
	case GL_DEPTH_WRITEMASK: v[0] = st->depth_mask; return 1;
	case GL_DEPTH_RANGE: v[0] = st->depth_range[0]; v[1] = st->depth_range[1]; return 2;
	case GL_DEPTH_FUNC: v[0] = st->depth_func; return 1;
	case GL_BLEND_SRC: v[0] = st->blend_src; return 1;
	case GL_BLEND_DST: v[0] = st->blend_dst; return 1;
	case GL_BLEND_EQUATION: v[0] = st->blend_eq; return 1;
	case GL_BLEND_COLOR: return values(v, st->blend_color, 4);
	case GL_ALPHA_TEST_FUNC: v[0] = st->alpha_func; return 1;
	case GL_ALPHA_TEST_REF: v[0] = st->alpha_ref; return 1;
	case GL_CULL_FACE_MODE: v[0] = st->cull_mode; return 1;
	case GL_FRONT_FACE: v[0] = st->front_face; return 1;
	case GL_SHADE_MODEL: v[0] = st->shade_model; return 1;
	case GL_POINT_SIZE: v[0] = st->point_size; return 1;
	case GL_LINE_WIDTH: v[0] = st->line_width; return 1;
	case GL_POINT_SIZE_RANGE: case GL_LINE_WIDTH_RANGE: v[0] = 1; v[1] = GLF_MAX_POINT_SIZE; return 2;
	case GL_MATRIX_MODE: v[0] = st->matrix_mode; return 1;
	case GL_MODELVIEW_MATRIX: return values(v, st->modelview.m[st->modelview.depth - 1], 16);
	case GL_PROJECTION_MATRIX: return values(v, st->projection.m[st->projection.depth - 1], 16);
	case GL_TEXTURE_MATRIX: {
		const struct gl_matrices *s = &st->texture[st->active_texture];
		return values(v, s->m[s->depth - 1], 16);
	}
	case GL_MODELVIEW_STACK_DEPTH: v[0] = st->modelview.depth; return 1;
	case GL_PROJECTION_STACK_DEPTH: v[0] = st->projection.depth; return 1;
	case GL_TEXTURE_STACK_DEPTH: v[0] = st->texture[st->active_texture].depth; return 1;
	case GL_MAX_MODELVIEW_STACK_DEPTH: v[0] = GLF_MODELVIEW_DEPTH; return 1;
	case GL_MAX_PROJECTION_STACK_DEPTH: v[0] = GLF_PROJECTION_DEPTH; return 1;
	case GL_MAX_TEXTURE_STACK_DEPTH: v[0] = GLF_TEXTURE_DEPTH; return 1;
	case GL_MAX_VIEWPORT_DIMS: v[0] = v[1] = GLF_MAX_SIZE; return 2;
	case GL_ACTIVE_TEXTURE_ARB: v[0] = GL_TEXTURE0_ARB + st->active_texture; return 1;
	case GL_MAX_TEXTURE_UNITS_ARB: v[0] = GLF_TEXTURE_UNITS; return 1;
	case GL_MAX_TEXTURE_LOD_BIAS_EXT: v[0] = GLF_MAX_LOD_BIAS; return 1;
	case GL_CURRENT_COLOR: return values(v, st->color, 4);
	case GL_CURRENT_SECONDARY_COLOR_EXT: return values(v, st->secondary_color, 4);
	case GL_CURRENT_NORMAL: return values(v, st->normal, 3);
	case GL_CURRENT_TEXTURE_COORDS: return values(v, st->tex[st->active_texture], 4);
	case GL_CURRENT_FOG_COORDINATE_EXT: v[0] = st->fog_coord; return 1;
	case GL_EDGE_FLAG: v[0] = st->edge_flag; return 1;
	case 0x8068: case 0x8069: case 0x806A: {        /* GL_TEXTURE_BINDING_1D, 2D, 3D */
		const struct gl_texture *t = st->units[st->active_texture].bound[pname - 0x8068];
		v[0] = t->name;
		return 1;
	}
	case 0x84E1: v[0] = GL_TEXTURE0_ARB + st->client_active_texture; return 1;   /* GL_CLIENT_ACTIVE_TEXTURE_ARB */
	case 0x0CF0: case 0x0D00: v[0] = (pname == 0x0D00 ? &st->pack : &st->unpack)->swap_bytes; return 1;
	case 0x0CF1: case 0x0D01: v[0] = (pname == 0x0D01 ? &st->pack : &st->unpack)->lsb_first; return 1;
	case 0x0CF2: case 0x0D02: v[0] = (pname == 0x0D02 ? &st->pack : &st->unpack)->row_length; return 1;
	case 0x0CF3: case 0x0D03: v[0] = (pname == 0x0D03 ? &st->pack : &st->unpack)->skip_rows; return 1;
	case 0x0CF4: case 0x0D04: v[0] = (pname == 0x0D04 ? &st->pack : &st->unpack)->skip_pixels; return 1;
	case 0x0CF5: case 0x0D05: v[0] = (pname == 0x0D05 ? &st->pack : &st->unpack)->alignment; return 1;
	/* polygons, lines */
	case 0x0B40: v[0] = st->polygon_mode[0]; v[1] = st->polygon_mode[1]; return 2;  /* GL_POLYGON_MODE */
	case 0x8038: v[0] = st->offset_factor; return 1;                                /* GL_POLYGON_OFFSET_FACTOR */
	case 0x2A00: v[0] = st->offset_units; return 1;                                 /* ..._UNITS */
	case 0x0B25: v[0] = st->line_stipple; return 1;                                 /* GL_LINE_STIPPLE_PATTERN */
	case 0x0B26: v[0] = st->line_stipple_factor; return 1;                          /* ..._REPEAT */
	case 0x0B13: case 0x0B23: v[0] = 0.125; return 1;       /* GL_POINT_SIZE_GRANULARITY, GL_LINE_WIDTH_... */
	/* stencil */
	case 0x0B91: v[0] = st->clear_stencil; return 1;                                /* GL_STENCIL_CLEAR_VALUE */
	case 0x0B92: v[0] = st->stencil_func; return 1;
	case 0x0B93: v[0] = st->stencil_mask; return 1;
	case 0x0B94: v[0] = st->stencil_fail; return 1;
	case 0x0B95: v[0] = st->stencil_zfail; return 1;
	case 0x0B96: v[0] = st->stencil_zpass; return 1;
	case 0x0B97: v[0] = st->stencil_ref; return 1;
	case 0x0B98: v[0] = st->stencil_write_mask; return 1;
	/* hints */
	case 0x0C50: case 0x0C51: case 0x0C52: case 0x0C53: case 0x0C54: v[0] = st->hints[pname - 0x0C50]; return 1;
	case 0x80F0: v[0] = st->hints[5]; return 1;                     /* GL_CLIP_VOLUME_CLIPPING_HINT_EXT */
	case 0x85B1: v[0] = st->hints[6]; return 1;                     /* GL_TRANSFORM_HINT_APPLE */
	/* the attribute stacks, limits, modes */
	case 0x0BB0: v[0] = st->attrib_depth; return 1;                 /* GL_ATTRIB_STACK_DEPTH */
	case 0x0BB1: v[0] = st->client_depth; return 1;                 /* GL_CLIENT_ATTRIB_STACK_DEPTH */
	case 0x0D35: case 0x0D3B: v[0] = GLF_ATTRIB_DEPTH; return 1;    /* GL_MAX_(CLIENT_)ATTRIB_STACK_DEPTH */
	case 0x0D50: v[0] = 8; return 1;                                /* GL_SUBPIXEL_BITS (raster.h's) */
	case 0x80E8: case 0x80E9: v[0] = GLF_MAX_ELEMENTS; return 1;    /* GL_MAX_ELEMENTS_VERTICES, _INDICES */
	case 0x0C31: v[0] = 1; return 1;                                /* GL_RGBA_MODE */
	case 0x0C30: case 0x0C33: case 0x0C00: case 0x0D51: case 0x0C20: v[0] = 0; return 1;
						/* (color index mode, stereo, auxiliary buffers, index bits, its clear) */
	case 0x0B80: return values(v, st->clear_accum, 4);             /* GL_ACCUM_CLEAR_VALUE */
	case 0x0C21: v[0] = 255; return 1;                              /* GL_INDEX_WRITEMASK */
	case 0x0BF0: v[0] = st->logic_op; return 1;                     /* GL_LOGIC_OP_MODE */
	case 0x0C40: v[0] = st->select.mode; return 1;                  /* GL_RENDER_MODE */
	case 0x0B32: v[0] = st->list_base; return 1;                    /* GL_LIST_BASE */
	case 0x0D70: v[0] = st->select.depth; return 1;                 /* GL_NAME_STACK_DEPTH */
	case 0x0DF4: v[0] = st->select.select_size; return 1;           /* GL_SELECTION_BUFFER_SIZE */
	case 0x0DF1: v[0] = st->select.feedback_size; return 1;         /* GL_FEEDBACK_BUFFER_SIZE */
	case 0x0DF2: v[0] = st->select.feedback_type; return 1;         /* GL_FEEDBACK_BUFFER_TYPE */
	case 0x0B31: v[0] = 64; return 1;                               /* GL_MAX_LIST_NESTING */
	case 0x0D37: v[0] = GLF_NAME_STACK; return 1;                   /* GL_MAX_NAME_STACK_DEPTH */
	case 0x0D30: v[0] = GLF_EVAL_ORDER; return 1;                   /* GL_MAX_EVAL_ORDER */
	case 0x0DD0: v[0] = st->grid1[0]; v[1] = st->grid1[1]; return 2;        /* GL_MAP1_GRID_DOMAIN */
	case 0x0DD1: v[0] = st->grid1_n; return 1;                      /* GL_MAP1_GRID_SEGMENTS */
	case 0x0DD2: for (int k = 0; k < 4; k++) v[k] = st->grid2[k]; return 4; /* GL_MAP2_GRID_DOMAIN */
	case 0x0DD3: v[0] = st->grid2_n[0]; v[1] = st->grid2_n[1]; return 2;    /* GL_MAP2_GRID_SEGMENTS */
	case 0x8073: v[0] = GLF_MAX_3D_TEXTURE_SIZE; return 1;          /* GL_MAX_3D_TEXTURE_SIZE */
	case 0x806B: v[0] = st->pack.skip_images; return 1;             /* GL_PACK_SKIP_IMAGES */
	case 0x806C: v[0] = st->pack.image_height; return 1;
	case 0x806D: v[0] = st->unpack.skip_images; return 1;
	case 0x806E: v[0] = st->unpack.image_height; return 1;
	default: {
		unsigned n = glf_get_lighting(st, pname, v);
		n = n ? n : glf_get_pixels(st, pname, v);
		return n ? n : glf_get_arrays(st, pname, v);
	}
	}
}
