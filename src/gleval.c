#include "glfront.h"

/* (the same results on every host: no fused multiply-adds where some have them; GCC fuses none in ISO C) */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * Evaluators (OpenGL 1.2's 5.1): a map's value at (u, v) is the sum of its
 * control points R[j][i] weighted by Bernstein polynomials of u' = (u -
 * u1) / (u2 - u1) and v' likewise. EvalCoord issues what the enabled maps
 * give as a vertex's color, normal and texture coordinates (unit 0's), the
 * current ones as they were afterwards, as Mesa (the index map is kept, but
 * there's no color index mode).
 */

enum { GL_MAP1_COLOR_4 = 0x0D90, GL_MAP2_COLOR_4 = 0x0DB0, GL_AUTO_NORMAL = 0x0D80, GL_COEFF = 0x0A00, GL_ORDER,
       GL_DOMAIN };
enum { K_COLOR, K_INDEX, K_NORMAL, K_TEX1, K_TEX2, K_TEX3, K_TEX4, K_VERTEX3, K_VERTEX4 };

static const uint8_t components[GLF_EVAL_KINDS] = { 4, 1, 3, 1, 2, 3, 4, 3, 4 };

unsigned glf_map_components(uint32_t target, unsigned *dims)
{
	if (target >= GL_MAP1_COLOR_4 && target < GL_MAP1_COLOR_4 + GLF_EVAL_KINDS) {
		*dims = 1;
		return components[target - GL_MAP1_COLOR_4];
	}
	if (target >= GL_MAP2_COLOR_4 && target < GL_MAP2_COLOR_4 + GLF_EVAL_KINDS) {
		*dims = 2;
		return components[target - GL_MAP2_COLOR_4];
	}
	return 0;
}

/* The maps, made with their initial values (order 1, domain 0 to 1, table 6.24's point) the first time; NULL if not */
static struct gl_eval *eval_of(struct gl_state *st)
{
	static const float initial[GLF_EVAL_KINDS][4] = {
		{ 1, 1, 1, 1 }, { 1 }, { 0, 0, 1 }, { 0 }, { 0, 0 }, { 0, 0, 0 }, { 0, 0, 0, 1 }, { 0, 0, 0 },
		{ 0, 0, 0, 1 },
	};
	if (st->eval)
		return st->eval;
	struct gl_eval *e = calloc(1, sizeof *e);
	if (!e) {
		glf_error(st, GL_OUT_OF_MEMORY);
		return NULL;
	}
	for (unsigned d = 0; d < 2; d++)
		for (unsigned k = 0; k < GLF_EVAL_KINDS; k++) {
			struct gl_map *m = &e->map[d][k];
			m->order[0] = m->order[1] = 1;
			m->domain[1] = m->domain[3] = 1;
			memcpy(m->points, initial[k], sizeof initial[k]);
		}
	st->eval = e;
	return e;
}

void glf_map(struct gl_state *st, unsigned dims, uint32_t target, const double domain[4], int32_t ustride,
	     int32_t uorder, int32_t vstride, int32_t vorder, const float *points)
{
	unsigned target_dims, k = glf_map_components(target, &target_dims);
	if (!k || target_dims != dims) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	if (dims == 1) {
		vstride = (int32_t)k;
		vorder = 1;
	}
	if ((float)domain[0] == (float)domain[1] || (dims == 2 && (float)domain[2] == (float)domain[3]) ||
	    uorder < 1 || uorder > GLF_EVAL_ORDER || vorder < 1 || vorder > GLF_EVAL_ORDER || ustride < (int32_t)k ||
	    vstride < (int32_t)k) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	if (st->active_texture) {               /* (as Mesa: evaluators are unit 0's, ARB_multitexture's F.2.13) */
		glf_error(st, GL_INVALID_OPERATION);
		return;
	}
	struct gl_eval *e = points ? eval_of(st) : NULL;
	if (!e)
		return;
	struct gl_map *m = &e->map[dims - 1][target - (dims == 1 ? GL_MAP1_COLOR_4 : GL_MAP2_COLOR_4)];
	m->order[0] = uorder;
	m->order[1] = vorder;
	for (int i = 0; i < 4; i++)
		m->domain[i] = dims == 1 && i >= 2 ? (float)(i - 2) : (float)domain[i];
	memcpy(m->points, points, (size_t)uorder * (size_t)vorder * k * sizeof *points);
}

void glf_map_grid1(struct gl_state *st, int32_t n, float u1, float u2)
{
	if (n < 1) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	st->grid1_n = n;
	st->grid1[0] = u1;
	st->grid1[1] = u2;
}

void glf_map_grid2(struct gl_state *st, int32_t un, float u1, float u2, int32_t vn, float v1, float v2)
{
	if (un < 1 || vn < 1) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	st->grid2_n[0] = un;
	st->grid2_n[1] = vn;
	st->grid2[0] = u1;
	st->grid2[1] = u2;
	st->grid2[2] = v1;
	st->grid2[3] = v2;
}

/*
 * The Bernstein polynomials of degree order - 1 at t into b (made a degree
 * at a time: b_i^n = (1 - t) b_i^(n-1) + t b_(i-1)^(n-1)), and their
 * derivatives into d, if wanted: n (b_(i-1)^(n-1) - b_i^(n-1))
 */
static void bernstein(int order, double t, double *b, double *d)
{
	int n = order - 1;
	b[0] = 1;
	if (d && n == 0)
		d[0] = 0;
	for (int deg = 1; deg <= n; deg++) {
		if (d && deg == n)
			for (int i = 0; i <= n; i++)
				d[i] = n * ((i > 0 ? b[i - 1] : 0) - (i < n ? b[i] : 0));
		b[deg] = t * b[deg - 1];
		for (int i = deg - 1; i > 0; i--)
			b[i] = (1 - t) * b[i] + t * b[i - 1];
		b[0] = (1 - t) * b[0];
	}
}

/*
 * A map's value at (u, v) (v: 0 for a 1D map's) into out, completed as
 * (0, 0, 0, 1); its partial derivatives in u' and v' into du and dv, if
 * wanted
 */
static void evaluate(const struct gl_map *m, unsigned k, float u, float v, float out[4], double du[4], double dv[4])
{
	double bu[GLF_EVAL_ORDER], bv[GLF_EVAL_ORDER], du_b[GLF_EVAL_ORDER], dv_b[GLF_EVAL_ORDER];
	double up = ((double)u - m->domain[0]) / ((double)m->domain[1] - m->domain[0]);
	double vp = ((double)v - m->domain[2]) / ((double)m->domain[3] - m->domain[2]);
	bernstein(m->order[0], up, bu, du ? du_b : NULL);
	bernstein(m->order[1], vp, bv, dv ? dv_b : NULL);
	double p[4] = { 0, 0, 0, 0 };
	if (du)
		memset(du, 0, 4 * sizeof *du);
	if (dv)
		memset(dv, 0, 4 * sizeof *dv);
	for (int j = 0; j < m->order[1]; j++)
		for (int i = 0; i < m->order[0]; i++) {
			const float *r = &m->points[((size_t)j * (size_t)m->order[0] + (size_t)i) * k];
			for (unsigned c = 0; c < k; c++) {
				p[c] += bu[i] * bv[j] * r[c];
				if (du)
					du[c] += du_b[i] * bv[j] * r[c];
				if (dv)
					dv[c] += bu[i] * dv_b[j] * r[c];
			}
		}
	for (unsigned c = 0; c < 4; c++)
		out[c] = c < k ? (float)p[c] : c == 3 ? 1.0f : 0.0f;
}

/* The enabled map of the kinds [first, last] (the last first: the most components), or none (-1) */
static int enabled_kind(const struct gl_state *st, unsigned dims, unsigned first, unsigned last)
{
	uint32_t base = dims == 1 ? GL_MAP1_COLOR_4 : GL_MAP2_COLOR_4;
	for (int k = (int)last; k >= (int)first; k--)
		if (glf_is_enabled(st, base + (uint32_t)k) == 1)
			return k;
	return -1;
}

void glf_eval_coord(struct gl_state *st, const struct gl_target *t, unsigned dims, float u, float v)
{
	int vertex = enabled_kind(st, dims, K_VERTEX3, K_VERTEX4);
	if (vertex < 0)
		return;                         /* (no vertex: nothing) */
	const struct gl_eval *e = eval_of(st);
	if (!e)
		return;
	const struct gl_map *maps = e->map[dims - 1];
	float color[4], normal[3], tex[4];
	memcpy(color, st->color, sizeof color);
	memcpy(normal, st->normal, sizeof normal);
	memcpy(tex, st->tex[0], sizeof tex);
	float out[4];
	if (enabled_kind(st, dims, K_COLOR, K_COLOR) >= 0) {
		evaluate(&maps[K_COLOR], 4, u, v, out, NULL, NULL);
		memcpy(st->color, out, sizeof st->color);
	}
	int tk = enabled_kind(st, dims, K_TEX1, K_TEX4);
	if (tk >= 0) {
		evaluate(&maps[tk], components[tk], u, v, out, NULL, NULL);
		memcpy(st->tex[0], out, sizeof out);
	}
	if (enabled_kind(st, dims, K_NORMAL, K_NORMAL) >= 0) {
		evaluate(&maps[K_NORMAL], 3, u, v, out, NULL, NULL);
		memcpy(st->normal, out, sizeof st->normal);
	}
	float p[4];
	if (dims == 2 && glf_is_enabled(st, GL_AUTO_NORMAL) == 1) {
		/*
		 * The surface's normal, dp/du x dp/dv normalized (of the point
		 * divided by w, for a 4D map: as Mesa, without the positive 1 / w^2);
		 * the derivatives in u and v, so a domain run backwards turns it
		 * round, where Mesa's are in u' and v'
		 */
		const struct gl_map *m = &maps[vertex];
		double du[4], dv[4];
		evaluate(m, components[vertex], u, v, p, du, dv);
		if (vertex == K_VERTEX4)
			for (int c = 0; c < 3; c++) {
				du[c] = du[c] * p[3] - du[3] * p[c];
				dv[c] = dv[c] * p[3] - dv[3] * p[c];
			}
		bool backwards = ((double)m->domain[1] - m->domain[0]) * ((double)m->domain[3] - m->domain[2]) < 0;
		double n[3] = { du[1] * dv[2] - du[2] * dv[1], du[2] * dv[0] - du[0] * dv[2],
				du[0] * dv[1] - du[1] * dv[0] };
		double len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		for (int c = 0; c < 3; c++)
			st->normal[c] = len > 0 ? (float)((backwards ? -n[c] : n[c]) / len) : 0.0f;
	} else {
		evaluate(&maps[vertex], components[vertex], u, v, p, NULL, NULL);
	}
	glf_vertex(st, t, p);
	memcpy(st->color, color, sizeof color);
	memcpy(st->normal, normal, sizeof normal);
	memcpy(st->tex[0], tex, sizeof tex);
}

/* Grid point i of n from a to b (b itself at n) */
static float grid(float a, float b, int32_t n, int32_t i)
{
	return i == n ? b : (float)((double)a + (double)i * ((double)b - a) / n);
}

void glf_eval_point(struct gl_state *st, const struct gl_target *t, unsigned dims, int32_t i, int32_t j)
{
	if (dims == 1)
		glf_eval_coord(st, t, 1, grid(st->grid1[0], st->grid1[1], st->grid1_n, i), 0);
	else
		glf_eval_coord(st, t, 2, grid(st->grid2[0], st->grid2[1], st->grid2_n[0], i),
			       grid(st->grid2[2], st->grid2[3], st->grid2_n[1], j));
}

void glf_eval_mesh(struct gl_state *st, const struct gl_target *t, unsigned dims, uint32_t mode, int32_t i1,
		   int32_t i2, int32_t j1, int32_t j2)
{
	if (mode != GL_POINT && mode != GL_LINE && (dims == 1 || mode != GL_FILL)) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	if (enabled_kind(st, dims, K_VERTEX3, K_VERTEX4) < 0)
		return;                         /* (as Mesa: not even glBegin and glEnd) */
	if (dims == 1) {
		glf_begin(st, mode == GL_POINT ? GL_POINTS : GL_LINE_STRIP);
		for (int32_t i = i1; i <= i2; i++)
			glf_eval_point(st, t, 1, i, 0);
		glf_end(st, t);
		return;
	}
	/* (OpenGL 1.2's 5.1: u along each row, rows of v; filled, each pair of rows a quad strip) */
	switch (mode) {
	case GL_POINT:
		glf_begin(st, GL_POINTS);
		for (int32_t j = j1; j <= j2; j++)
			for (int32_t i = i1; i <= i2; i++)
				glf_eval_point(st, t, 2, i, j);
		glf_end(st, t);
		break;
	case GL_LINE:
		for (int32_t j = j1; j <= j2; j++) {
			glf_begin(st, GL_LINE_STRIP);
			for (int32_t i = i1; i <= i2; i++)
				glf_eval_point(st, t, 2, i, j);
			glf_end(st, t);
		}
		for (int32_t i = i1; i <= i2; i++) {
			glf_begin(st, GL_LINE_STRIP);
			for (int32_t j = j1; j <= j2; j++)
				glf_eval_point(st, t, 2, i, j);
			glf_end(st, t);
		}
		break;
	default:
		for (int32_t j = j1; j < j2; j++) {
			glf_begin(st, GL_QUAD_STRIP);
			for (int32_t i = i1; i <= i2; i++) {
				glf_eval_point(st, t, 2, i, j);
				glf_eval_point(st, t, 2, i, j + 1);
			}
			glf_end(st, t);
		}
		break;
	}
}

unsigned glf_get_map(struct gl_state *st, uint32_t target, uint32_t query, double *v)
{
	unsigned dims, k = glf_map_components(target, &dims);
	if (!k || (query != GL_COEFF && query != GL_ORDER && query != GL_DOMAIN)) {
		glf_error(st, GL_INVALID_ENUM);
		return 0;
	}
	const struct gl_eval *e = eval_of(st);
	if (!e)
		return 0;
	const struct gl_map *m = &e->map[dims - 1][target - (dims == 1 ? GL_MAP1_COLOR_4 : GL_MAP2_COLOR_4)];
	switch (query) {
	case GL_ORDER:
		for (unsigned i = 0; i < dims; i++)
			v[i] = m->order[i];
		return dims;
	case GL_DOMAIN:
		for (unsigned i = 0; i < 2 * dims; i++)
			v[i] = m->domain[i];
		return 2 * dims;
	default: {                              /* (2D: u fastest, as glGetMap's page has it; Mesa: v fastest) */
		unsigned n = (unsigned)m->order[0] * (unsigned)m->order[1] * k;
		for (unsigned i = 0; i < n; i++)
			v[i] = m->points[i];
		return n;
	}
	}
}
