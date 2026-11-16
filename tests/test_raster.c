/*
 * The 3D engine's rasterizer: coverage (the top-left rule, exact on edges
 * triangles share), depth testing, texture orientation, and drawing
 * recorded commands on helper threads, at once or in background batches,
 * which must give exactly the pixels of drawing them one at a time.
 * OpenGL's points and lines (as Mesa's llvmpipe draws them, but for its
 * ties), their stipple and coverage; logic ops; the polygon stipple; the
 * levels a LOD bias and range pick.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../src/raster.h"
#include "test.h"

#define W 64
#define H 48

static uint32_t color_a[W * H], color_b[W * H], color_c[W * H];
static float depth_a[W * H], depth_b[W * H], depth_c[W * H];

static struct raster_target target(uint32_t *color, float *depth)
{
	return (struct raster_target){ W, H, color, depth, NULL };
}

static struct raster_state plain(void)
{
	struct raster_state s;
	memset(&s, 0, sizeof s);
	s.z_func = RASTER_Z_NONE;
	s.blend = RASTER_BLEND_PREMULTIPLY;
	s.fog_mode = RASTER_FOG_NONE;
	s.alpha_test = RASTER_ALPHA_NONE;
	s.width = 1;
	s.clip_right = W;
	s.clip_bottom = H;
	return s;
}

static struct raster_vertex vertex(float x, float y, float z, float r, float g, float b)
{
	struct raster_vertex v;
	memset(&v, 0, sizeof v);
	v.x = x;
	v.y = y;
	v.z = z;
	v.invw = 1;
	v.color[0][0] = r;
	v.color[0][1] = g;
	v.color[0][2] = b;
	v.color[0][3] = 1;
	for (int k = 0; k < 3; k++)
		v.color[2][k] = 1;              /* (kd) */
	return v;
}

/* A texture drawn as RAVE draws it (raster_rave_stages()) */
static void texture(struct raster_state *s, const struct raster_texture *tx, unsigned ops, bool bilinear)
{
	s->unit[0].texture = tx;
	s->unit[0].min_filter = bilinear ? RASTER_LINEAR_MIPMAP_NEAREST : RASTER_NEAREST_MIPMAP_NEAREST;
	s->unit[0].mag_filter = bilinear ? RASTER_LINEAR : RASTER_NEAREST;
	raster_rave_stages(s, ops);
}

/* Two triangles sharing an edge cover each pixel of their square exactly once */
static void test_coverage(void)
{
	struct raster_target t = target(color_a, depth_a);
	raster_clear(&t, 0, 0, W, H, true, 0, true, 1);
	struct raster_state s = plain();
	s.blend = RASTER_BLEND_FACTORS;         /* adding up: a pixel drawn twice would show */
	s.blend_src = RASTER_F_ONE;
	s.blend_dst = RASTER_F_ONE;
	float one = 1.0f / 255;
	struct raster_vertex a = vertex(2, 3, 0, one, 0, 0), b = vertex(12, 3, 0, one, 0, 0);
	struct raster_vertex c = vertex(12, 13, 0, one, 0, 0), d = vertex(2, 13, 0, one, 0, 0);
	raster_triangle(&t, &s, &a, &b, &c, false);
	raster_triangle(&t, &s, &a, &c, &d, false);
	unsigned wrong = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			unsigned red = color_a[y * W + x] >> 16 & 255;
			wrong += red != (x >= 2 && x < 12 && y >= 3 && y < 13);
		}
	CHECK_EQ(wrong, 0);
}

/*
 * A mesh of triangles sharing edges, many of them through pixels' centers
 * (vertices at .3, .5, a third...), wound either way: each pixel inside is
 * covered exactly once (no cracks, no pixel drawn twice)
 */
static void test_shared_edges(void)
{
	struct raster_target t = target(color_a, depth_a);
	raster_clear(&t, 0, 0, W, H, true, 0, true, 1);
	struct raster_state s = plain();
	s.blend = RASTER_BLEND_FACTORS;
	s.blend_src = RASTER_F_ONE;
	s.blend_dst = RASTER_F_ONE;
	float one = 1.0f / 255;
	enum { NX = 10, NY = 8 };
	static const float offsets[] = { 0, 0.3f, 0.5f, -0.5f, 1.0f / 3, 0.25f, -0.2f, 0.7f, -1.0f / 3 };
	struct raster_vertex v[NY + 1][NX + 1];
	unsigned pick = 0;
	for (int j = 0; j <= NY; j++)
		for (int i = 0; i <= NX; i++) {
			float x = 2 + 6.0f * (float)i, y = 3 + 5.0f * (float)j;
			if (i > 0 && i < NX && j > 0 && j < NY) {       /* (the outside stays a rectangle) */
				x += offsets[pick++ % 9];
				y += offsets[(pick += 4) % 9];
			}
			v[j][i] = vertex(x, y, 0, one, 0, 0);
		}
	for (int j = 0; j < NY; j++)
		for (int i = 0; i < NX; i++) {
			const struct raster_vertex *a = &v[j][i], *b = &v[j][i + 1], *c = &v[j + 1][i + 1], *d = &v[j + 1][i];
			if ((i + j) & 1) {
				raster_triangle(&t, &s, a, b, c, false);
				raster_triangle(&t, &s, c, d, a, false);
			} else {
				raster_triangle(&t, &s, b, a, d, false);        /* (the other diagonal, the other way round) */
				raster_triangle(&t, &s, b, d, c, false);
			}
		}
	unsigned wrong = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			unsigned red = color_a[y * W + x] >> 16 & 255;
			wrong += red != (x >= 2 && x < 2 + 6 * NX && y >= 3 && y < 3 + 5 * NY);
		}
	CHECK_EQ(wrong, 0);
}

/* The nearer triangle wins whichever is drawn first */
static void test_depth(void)
{
	struct raster_target t = target(color_a, depth_a);
	struct raster_state s = plain();
	s.z_func = RASTER_Z_LT;
	s.z_write = true;
	for (int order = 0; order < 2; order++) {
		raster_clear(&t, 0, 0, W, H, true, 0, true, 1);
		struct raster_vertex n0 = vertex(0, 0, 0.2f, 0, 1, 0), n1 = vertex(40, 0, 0.2f, 0, 1, 0);
		struct raster_vertex n2 = vertex(0, 40, 0.2f, 0, 1, 0);
		struct raster_vertex f0 = vertex(0, 0, 0.8f, 1, 0, 0), f1 = vertex(40, 0, 0.8f, 1, 0, 0);
		struct raster_vertex f2 = vertex(0, 40, 0.8f, 1, 0, 0);
		if (order) {
			raster_triangle(&t, &s, &f0, &f1, &f2, false);
			raster_triangle(&t, &s, &n0, &n1, &n2, false);
		} else {
			raster_triangle(&t, &s, &n0, &n1, &n2, false);
			raster_triangle(&t, &s, &f0, &f1, &f2, false);
		}
		CHECK_EQ(color_a[5 * W + 5], 0xFF00FF00u);
		CHECK(depth_a[5 * W + 5] == 0.2f);
		CHECK_EQ(color_a[45 * W + 60], 0);
	}
}

/* Texel row k is at v = k / height, column j at u = j / width */
static void test_texture(void)
{
	static uint32_t texels[4] = { 0xFF000011u, 0xFF000022u, 0xFF000033u, 0xFF000044u };
	struct raster_texture tx = { .levels = 1, .width = { 2 }, .height = { 2 }, .texels = { texels } };
	struct raster_target t = target(color_a, depth_a);
	raster_clear(&t, 0, 0, W, H, true, 0, true, 1);
	struct raster_state s = plain();
	texture(&s, &tx, 0, false);
	struct raster_vertex v[4] = { vertex(0, 0, 0, 1, 1, 1), vertex(8, 0, 0, 1, 1, 1), vertex(8, 8, 0, 1, 1, 1),
				      vertex(0, 8, 0, 1, 1, 1) };
	v[1].tex[0][0] = v[2].tex[0][0] = 1;
	v[2].tex[0][1] = v[3].tex[0][1] = 1;
	raster_triangle(&t, &s, &v[0], &v[1], &v[2], true);
	raster_triangle(&t, &s, &v[0], &v[2], &v[3], true);
	CHECK_EQ(color_a[1 * W + 1], texels[0]);
	CHECK_EQ(color_a[1 * W + 6], texels[1]);
	CHECK_EQ(color_a[6 * W + 1], texels[2]);
	CHECK_EQ(color_a[6 * W + 6], texels[3]);
}

static uint64_t rng = 88172645463325252ull;

static float frand(float lo, float hi)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return lo + (hi - lo) * (float)(rng >> 40) / (float)(1 << 24);
}

/* Commands drawn on threads (at once, or in batches in the background) match drawing them one by one */
static void test_threads(void)
{
	enum { STATES = 6, CMDS = 400 };
	static uint32_t texels[16 * 16];
	for (unsigned k = 0; k < 16 * 16; k++)
		texels[k] = 0xFF000000u | (uint32_t)(k * 2654435761u >> 8);
	struct raster_texture tx = { .levels = 1, .width = { 16 }, .height = { 16 }, .texels = { texels } };

	struct raster_state states[STATES];
	for (int k = 0; k < STATES; k++) {
		states[k] = plain();
		states[k].z_func = k % 3 ? RASTER_Z_LT : RASTER_Z_LE;
		states[k].z_write = k != 4;
		states[k].blend = (unsigned)k % 3;
		states[k].blend_src = RASTER_F_SRC_ALPHA;
		states[k].blend_dst = RASTER_F_ONE_MINUS_SRC_ALPHA;
		texture(&states[k], &tx, k & 1 ? RASTER_TEX_MODULATE : 0, k & 2);
		states[k].fog_mode = k == 5 ? RASTER_FOG_LINEAR : RASTER_FOG_NONE;
		states[k].fog_start = 0.5f;
		states[k].fog_end = 3;
		states[k].fog_color[1] = 0.25f;
		if (k == 3)
			states[k].clip_top = 10;        /* (a context's clip) */
	}
	static struct raster_cmd cmds[CMDS];
	memset(cmds, 0, sizeof cmds);
	for (int k = 0; k < CMDS; k++) {
		struct raster_cmd *c = &cmds[k];
		c->kind = k % 97 == 0 ? RASTER_CMD_CLEAR : k % 31 == 0 ? RASTER_CMD_LINE : RASTER_CMD_TRIANGLE;
		c->state = (unsigned)k % STATES;
		c->textured = k & 1;
		for (int v = 0; v < 3; v++) {
			c->v[v] = vertex(frand(-10, W + 10), frand(-10, H + 10), frand(0, 1), frand(0, 1), frand(0, 1),
					 frand(0, 1));
			c->v[v].color[0][3] = frand(0, 1);
			c->v[v].invw = frand(0.3f, 2);
			c->v[v].tex[0][0] = frand(-1, 2) * c->v[v].invw;
			c->v[v].tex[0][1] = frand(-1, 2) * c->v[v].invw;
		}
		c->rect[0] = 5;
		c->rect[1] = 7;
		c->rect[2] = 40;
		c->rect[3] = 30;
		c->clear_color = true;
		c->clear_depth = k & 2;
		c->argb = 0xFF102030u;
		c->z = 1;
	}

	struct raster_target a = target(color_a, depth_a), b = target(color_b, depth_b), c = target(color_c, depth_c);
	raster_clear(&a, 0, 0, W, H, true, 0xFF000000u, true, 1);
	raster_clear(&b, 0, 0, W, H, true, 0xFF000000u, true, 1);
	raster_clear(&c, 0, 0, W, H, true, 0xFF000000u, true, 1);
	for (int k = 0; k < CMDS; k++) {        /* one at a time */
		const struct raster_cmd *cmd = &cmds[k];
		const struct raster_state *s = &states[cmd->state];
		if (cmd->kind == RASTER_CMD_TRIANGLE)
			raster_triangle(&a, s, &cmd->v[0], &cmd->v[1], &cmd->v[2], cmd->textured);
		else if (cmd->kind == RASTER_CMD_LINE)
			raster_line(&a, s, &cmd->v[0], &cmd->v[1]);
		else {
			int top = cmd->rect[1] > s->clip_top ? cmd->rect[1] : s->clip_top;
			raster_clear(&a, cmd->rect[0], top, cmd->rect[2], cmd->rect[3], cmd->clear_color, cmd->argb,
				     cmd->clear_depth, cmd->z);
		}
	}
	raster_run(&b, states, cmds, CMDS);
	raster_start(&c, states, cmds, CMDS / 3);
	raster_start(&c, states, cmds + CMDS / 3, CMDS - CMDS / 3);     /* (waits for the first) */
	raster_wait();
	CHECK(!memcmp(color_a, color_b, sizeof color_a) && !memcmp(depth_a, depth_b, sizeof depth_a));
	CHECK(!memcmp(color_a, color_c, sizeof color_a) && !memcmp(depth_a, depth_c, sizeof depth_a));
	unsigned drawn = 0;
	for (int k = 0; k < W * H; k++)
		drawn += color_a[k] != 0xFF000000u;
	CHECK(drawn > W * H / 2);
}

/* A packed color as raster.c packs one */
static uint32_t argb(float a, float r, float g, float b)
{
	const float c[4] = { a, r, g, b };
	uint32_t p = 0;
	for (int k = 0; k < 4; k++)
		p = p << 8 | (uint32_t)((c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k]) * 255 + 0.5f);
	return p;
}

/* Every channel within one (the test's own arithmetic may round differently) */
static bool near(uint32_t a, uint32_t b)
{
	for (int sh = 0; sh < 32; sh += 8) {
		int d = (int)(a >> sh & 255) - (int)(b >> sh & 255);
		if (d < -1 || d > 1)
			return false;
	}
	return true;
}

/* An 8 x 8 square of a state's fragments over a target cleared to `under`: the pixel in its middle */
static uint32_t square(const struct raster_state *s, const struct raster_vertex *v, uint32_t under, bool textured)
{
	struct raster_target t = target(color_a, depth_a);
	raster_clear(&t, 0, 0, W, H, true, under, true, 1);
	struct raster_vertex q[4] = { *v, *v, *v, *v };
	q[1].x = q[2].x = 8;
	q[2].y = q[3].y = 8;
	raster_triangle(&t, s, &q[0], &q[1], &q[2], textured);
	raster_triangle(&t, s, &q[0], &q[2], &q[3], textured);
	return color_a[4 * W + 4];
}

static struct raster_stage stage(unsigned op_rgb, unsigned r0, unsigned r1, unsigned r2, unsigned op_a, unsigned a0,
				 unsigned a1, unsigned a2)
{
	struct raster_stage st;
	memset(&st, 0, sizeof st);
	st.op[0] = (uint8_t)op_rgb;
	st.op[1] = (uint8_t)op_a;
	st.arg[0][0] = (uint8_t)r0;
	st.arg[0][1] = (uint8_t)r1;
	st.arg[0][2] = (uint8_t)r2;
	st.arg[1][0] = (uint8_t)a0;
	st.arg[1][1] = (uint8_t)a1;
	st.arg[1][2] = (uint8_t)a2;
	st.clamp[0] = st.clamp[1] = true;
	st.constant[0] = 0.6f;          /* a, r, g, b */
	st.constant[1] = 0.9f;
	st.constant[2] = 0.5f;
	st.constant[3] = 0.25f;
	return st;
}

/* Combining stages (as OpenGL's texture environments): each operation and kind of argument */
static void test_stages(void)
{
	static uint32_t texel = 0x80402010u;
	struct raster_texture tx = { .levels = 1, .width = { 1 }, .height = { 1 }, .texels = { &texel } };
	const float ta = 128.0f / 255, tr = 64.0f / 255, tg = 32.0f / 255, tb = 16.0f / 255;
	struct raster_vertex v = vertex(0, 0, 0.5f, 0.5f, 0.25f, 1);
	v.color[0][3] = 0.75f;
	const float c1[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
	memcpy(v.color[1], c1, sizeof c1);
	const unsigned T = RASTER_TEXTURE0, C0 = RASTER_COLOR0, C1 = RASTER_COLOR0 + 1, K = RASTER_CONSTANT,
		       P = RASTER_PREVIOUS, A = RASTER_ALPHA, I = RASTER_INVERT;
	struct {
		struct raster_stage st[2];
		unsigned n;
		uint32_t want;
	} cases[] = {
		{ { stage(RASTER_OP_MODULATE, T, C0, 0, RASTER_OP_MODULATE, T, C0, 0) }, 1,
		  argb(ta * 0.75f, tr * 0.5f, tg * 0.25f, tb * 1) },
		{ { stage(RASTER_OP_ADD, T, C0, 0, RASTER_OP_MODULATE, T, C0, 0) }, 1,
		  argb(ta * 0.75f, tr + 0.5f, tg + 0.25f, tb + 1) },
		{ { stage(RASTER_OP_INTERPOLATE, K, C0, T, RASTER_OP_MODULATE, T, C0, 0) }, 1,      /* GL_BLEND */
		  argb(ta * 0.75f, 0.9f * tr + 0.5f * (1 - tr), 0.5f * tg + 0.25f * (1 - tg), 0.25f * tb + 1 * (1 - tb)) },
		{ { stage(RASTER_OP_SUBTRACT, C0, T, 0, RASTER_OP_REPLACE, C0, 0, 0) }, 1,
		  argb(0.75f, 0.5f - tr, 0.25f - tg, 1 - tb) },
		{ { stage(RASTER_OP_ADD_SIGNED, T, C0, 0, RASTER_OP_REPLACE, T | I, 0, 0) }, 1,
		  argb(1 - ta, (tr + 0.5f - 0.5f) * 2, (tg + 0.25f - 0.5f) * 2, (tb + 1 - 0.5f) * 2) },
		{ { stage(RASTER_OP_MODULATE, T, C0 | A | I, 0, RASTER_OP_REPLACE, K, 0, 0) }, 1,
		  argb(0.6f, tr * 0.25f, tg * 0.25f, tb * 0.25f) },
		{ { stage(RASTER_OP_REPLACE, T, 0, 0, RASTER_OP_REPLACE, T, 0, 0),
		    stage(RASTER_OP_INTERPOLATE, P, C1, C0 | A, RASTER_OP_ADD, P, C1, 0) }, 2,
		  argb(ta + 0.4f, tr * 0.75f + 0.1f * 0.25f, tg * 0.75f + 0.2f * 0.25f, tb * 0.75f + 0.3f * 0.25f) },
	};
	cases[4].st[0].shift[0] = 1;
	for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		struct raster_state s = plain();
		s.unit[0].texture = &tx;
		s.stages = cases[k].n;
		memcpy(s.stage, cases[k].st, sizeof cases[k].st);
		CHECK_EQ(raster_units_used(&s), 1);
		uint32_t got = square(&s, &v, 0, true);
		if (!near(got, cases[k].want))
			fprintf(stderr, "stages, case %u: %08x, expected %08x\n", k, got, cases[k].want);
		CHECK(near(got, cases[k].want));
		CHECK(near(square(&s, &v, 0, false), argb(0.75f, 0.5f, 0.25f, 1)));    /* (untextured: color 0) */
	}
	struct raster_state s = plain();
	s.stages = 1;
	s.stage[0] = cases[0].st[0];
	CHECK(!raster_textured(&s, true));                      /* (no texture: none) */
}

/* Blend equations, constant colors, no blending, color masks (drawn and cleared), the fog coordinate */
static void test_fragments(void)
{
	const uint32_t under = 0x80604020u;
	const float ua = 128.0f / 255, ur = 96.0f / 255, ug = 64.0f / 255, ub = 32.0f / 255;
	struct raster_vertex v = vertex(0, 0, 0.5f, 0.75f, 0.5f, 0.125f);
	v.color[0][3] = 0.25f;
	struct raster_state s = plain();
	s.blend = RASTER_BLEND_FACTORS;
	s.blend_src = s.blend_dst = RASTER_F_ONE;
	s.blend_eq = RASTER_EQ_SUBTRACT;
	CHECK(near(square(&s, &v, under, false), argb(0.25f - ua, 0.75f - ur, 0.5f - ug, 0.125f - ub)));
	s.blend_eq = RASTER_EQ_REVERSE_SUBTRACT;
	CHECK(near(square(&s, &v, under, false), argb(ua - 0.25f, ur - 0.75f, ug - 0.5f, ub - 0.125f)));
	s.blend_eq = RASTER_EQ_MIN;
	CHECK(near(square(&s, &v, under, false), argb(0.25f, ur, ug, 0.125f)));
	s.blend_eq = RASTER_EQ_MAX;
	CHECK(near(square(&s, &v, under, false), argb(ua, 0.75f, 0.5f, ub)));
	s.blend_eq = RASTER_EQ_ADD;
	s.blend_src = RASTER_F_CONSTANT_COLOR;
	s.blend_dst = RASTER_F_ONE_MINUS_CONSTANT_ALPHA;
	const float k[4] = { 0.5f, 0.25f, 1, 0 };
	memcpy(s.blend_color, k, sizeof k);
	CHECK(near(square(&s, &v, under, false), argb(0.25f * 0.5f + ua * 0.5f, 0.75f * 0.25f + ur * 0.5f,
						      0.5f + ug * 0.5f, ub * 0.5f)));
	s = plain();
	s.blend = RASTER_BLEND_NONE;
	CHECK(near(square(&s, &v, under, false), argb(0.25f, 0.75f, 0.5f, 0.125f)));
	s.color_keep = 0xFF00FF00u;
	CHECK(near(square(&s, &v, under, false), (argb(0.25f, 0.75f, 0.5f, 0.125f) & 0x00FF00FFu) | (under & 0xFF00FF00u)));
	struct raster_target t = target(color_a, depth_a);
	raster_clear(&t, 0, 0, W, H, true, under, true, 1);
	struct raster_cmd clear;
	memset(&clear, 0, sizeof clear);
	clear.kind = RASTER_CMD_CLEAR;
	clear.rect[2] = W;
	clear.rect[3] = H;
	clear.clear_color = true;
	clear.argb = 0x11223344u;
	raster_run(&t, &s, &clear, 1);
	CHECK_EQ(color_a[5 * W + 5], 0x80224044u);

	/* fog by the fog coordinate (f / w, divided by 1/w per pixel): halfway to the fog's color */
	s = plain();
	s.fog_mode = RASTER_FOG_LINEAR;
	s.fog_coord = true;
	s.fog_start = 0;
	s.fog_end = 10;
	s.fog_color[1] = 1;
	v = vertex(0, 0, 0.5f, 0, 0, 0);
	v.invw = 0.5f;
	v.fog = 5 * v.invw;
	CHECK(near(square(&s, &v, 0, false), argb(1, 0.5f, 0, 0)));

	/* fragments without alpha: drawn, depth too, only when all fragments are */
	s = plain();
	s.z_func = RASTER_Z_LT;
	s.z_write = true;
	v = vertex(0, 0, 0.25f, 0, 0, 0);
	v.color[0][3] = 0;
	square(&s, &v, under, false);
	CHECK(depth_a[4 * W + 4] == 1);
	s.all_fragments = true;
	s.blend = RASTER_BLEND_NONE;
	CHECK_EQ(square(&s, &v, under, false), 0);
	CHECK(depth_a[4 * W + 4] == 0.25f);
}

/*
 * The stencil test and its operations, in OpenGL's order (the alpha test
 * first, the z test after), the write mask, clears of it
 */
static uint8_t stencil_a[W * H];

static void stencil_square(struct raster_target *t, const struct raster_state *s, float x0, float y0, float x1, float y1,
		   float z, float alpha)
{
	struct raster_vertex a = vertex(x0, y0, z, 1, 1, 1), b = vertex(x1, y0, z, 1, 1, 1);
	struct raster_vertex c = vertex(x1, y1, z, 1, 1, 1), d = vertex(x0, y1, z, 1, 1, 1);
	a.color[0][3] = b.color[0][3] = c.color[0][3] = d.color[0][3] = alpha;
	raster_triangle(t, s, &a, &b, &c, false);
	raster_triangle(t, s, &a, &c, &d, false);
}

static void test_stencil(void)
{
	struct raster_target t = target(color_a, depth_a);
	t.stencil = stencil_a;
	raster_clear(&t, 0, 0, W, H, true, 0, true, 1);
	memset(stencil_a, 0, sizeof stencil_a);
	struct raster_state s = plain();
	s.blend = RASTER_BLEND_NONE;
	s.all_fragments = true;
	s.color_keep = 0xFFFFFFFFu;             /* (the stencil alone) */
	s.stencil_func = RASTER_Z_TRUE;
	s.stencil_ref = 1;
	s.stencil_mask = s.stencil_write = 0xFF;
	s.stencil_op[2] = RASTER_STENCIL_REPLACE;
	stencil_square(&t, &s, 2, 2, 12, 12, 0.5f, 1);
	CHECK(stencil_a[5 * W + 5] == 1 && stencil_a[5 * W + 15] == 0);
	/* counting, then inverting the low bits */
	s.stencil_op[2] = RASTER_STENCIL_INCR;
	stencil_square(&t, &s, 8, 8, 20, 20, 0.5f, 1);
	CHECK(stencil_a[10 * W + 10] == 2 && stencil_a[15 * W + 15] == 1 && stencil_a[5 * W + 5] == 1);
	s.stencil_write = 0x0F;
	s.stencil_op[2] = RASTER_STENCIL_INVERT;
	stencil_square(&t, &s, 0, 0, 30, 30, 0.5f, 1);
	CHECK(stencil_a[10 * W + 10] == 0x0D && stencil_a[15 * W + 15] == 0x0E && stencil_a[25 * W + 25] == 0x0F);
	/* drawn where it's equal to the reference in the mask's bits */
	s.stencil_write = 0xFF;
	s.stencil_op[2] = RASTER_STENCIL_KEEP;
	s.stencil_func = RASTER_Z_EQ;
	s.stencil_ref = 0x3D;                   /* (0xD in the mask) */
	s.stencil_mask = 0x0F;
	s.color_keep = 0;
	stencil_square(&t, &s, 0, 0, 30, 30, 0.5f, 1);
	CHECK(color_a[10 * W + 10] == 0xFFFFFFFFu && color_a[15 * W + 15] == 0 && color_a[5 * W + 5] == 0);
	/* the alpha test first: what it discards leaves the stencil alone */
	s.stencil_func = RASTER_Z_TRUE;
	s.stencil_mask = 0xFF;
	s.stencil_ref = 7;
	s.stencil_op[2] = RASTER_STENCIL_REPLACE;
	s.alpha_test = RASTER_ALPHA_GT;
	s.alpha_ref = 0.5f;
	stencil_square(&t, &s, 32, 2, 42, 12, 0.5f, 0.25f);
	CHECK_EQ(stencil_a[5 * W + 35], 0);
	stencil_square(&t, &s, 32, 2, 42, 12, 0.5f, 0.75f);
	CHECK_EQ(stencil_a[5 * W + 35], 7);
	s.alpha_test = RASTER_ALPHA_NONE;
	/* the z test after: its failing operation, or its passing one */
	s.z_func = RASTER_Z_LT;
	s.z_write = true;
	s.stencil_op[0] = RASTER_STENCIL_ZERO;
	s.stencil_op[1] = RASTER_STENCIL_INCR;
	s.stencil_op[2] = RASTER_STENCIL_DECR;
	stencil_square(&t, &s, 32, 14, 42, 24, 0.25f, 1);       /* (passes: 0 decremented stays 0) */
	CHECK_EQ(stencil_a[20 * W + 35], 0);
	stencil_square(&t, &s, 32, 14, 42, 24, 0.75f, 1);       /* (farther: fails) */
	CHECK_EQ(stencil_a[20 * W + 35], 1);
	s.stencil_func = RASTER_Z_FALSE;                /* (the stencil test failing: zero) */
	stencil_square(&t, &s, 32, 14, 42, 24, 0.1f, 1);
	CHECK(stencil_a[20 * W + 35] == 0 && depth_a[20 * W + 35] == 0.25f);
	/* wrapping, and not */
	s.z_func = RASTER_Z_NONE;
	s.stencil_func = RASTER_Z_TRUE;
	s.stencil_op[2] = RASTER_STENCIL_DECR_WRAP;
	stencil_square(&t, &s, 44, 2, 54, 12, 0.5f, 1);
	CHECK_EQ(stencil_a[5 * W + 50], 255);
	s.stencil_op[2] = RASTER_STENCIL_INCR;
	stencil_square(&t, &s, 44, 2, 54, 12, 0.5f, 1);
	CHECK_EQ(stencil_a[5 * W + 50], 255);
	s.stencil_op[2] = RASTER_STENCIL_INCR_WRAP;
	stencil_square(&t, &s, 44, 2, 54, 12, 0.5f, 1);
	CHECK_EQ(stencil_a[5 * W + 50], 0);
	/* cleared, in the write mask's bits */
	struct raster_cmd clear = { .kind = RASTER_CMD_CLEAR, .clear_stencil = true, .stencil = 0xA5,
				    .rect = { 0, 0, W, H } };
	s.stencil_write = 0xF0;
	stencil_a[0] = 0x0F;
	raster_run(&t, &s, &clear, 1);
	CHECK(stencil_a[0] == 0xAF && stencil_a[5 * W + 50] == 0xA0);
}

/* ---- OpenGL's points and lines, logic ops, the polygon stipple ---- */

/* A state drawing OpenGL's points and lines, replacing what's there */
static struct raster_state gl_state(float width)
{
	struct raster_state s = plain();
	s.blend = RASTER_BLEND_NONE;
	s.all_fragments = true;
	s.gl_points = true;
	s.width = width;
	return s;
}

/* A vertex at window (x, y): OpenGL's y going up from the target's bottom */
static struct raster_vertex win(float x, float y)
{
	return vertex(x, (float)H - y, 0, 1, 1, 1);
}

/* The pixels lit (window coordinates: x + 1000 y), sorted, from the bottom left */
static unsigned lit(const uint32_t *color, unsigned *out, unsigned max)
{
	unsigned n = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			if (color[(H - 1 - y) * W + x] && n < max)
				out[n++] = (unsigned)(x + 1000 * y);
	return n;
}

static unsigned draw_lit(const struct raster_state *s, const struct raster_cmd *c, unsigned *out, unsigned max)
{
	struct raster_target t = target(color_a, depth_a);
	raster_clear(&t, 0, 0, W, H, true, 0, true, 1);
	raster_run(&t, s, c, 1);
	return lit(color_a, out, max);
}

static struct raster_cmd line_cmd(float xa, float ya, float xb, float yb)
{
	struct raster_cmd c;
	memset(&c, 0, sizeof c);
	c.kind = RASTER_CMD_LINE;
	c.v[0] = win(xa, ya);
	c.v[1] = win(xb, yb);
	return c;
}

/*
 * Lines by the diamond-exit rule, as Mesa's llvmpipe draws them (but for
 * its ties): ends in diamonds and out, on pixels' edges (the perturbed
 * endpoints going down and left), every way; wide ones; aliased points
 */
static void test_gl_lines(void)
{
	static const struct {
		float a[4];
		unsigned n, first, last;        /* (fragments; the first and last in lit()'s order) */
	} lines[] = {
		{ { 2, 2, 12, 2 }, 10, 1002, 1011 },            /* (on a pixel edge: the row below) */
		{ { 2.5f, 4.5f, 12.5f, 4.5f }, 10, 4002, 4011 }, /* (center to center: the last left out) */
		{ { 15, 2, 15, 12 }, 10, 2014, 11014 },         /* (on an edge: the column left) */
		{ { 30, 2, 26, 6 }, 4, 2029, 5026 },
		{ { 40, 2, 30, 7 }, 10, 2038, 6031 },
		{ { 2.3f, 11.7f, 13.6f, 15.2f }, 11, 11002, 14012 },   /* (ends in the last pixel's diamond) */
		{ { 35.4f, 10.4f, 35.6f, 10.6f }, 0, 0, 0 },    /* (within one diamond) */
		{ { 33.5f, 10.5f, 33.5f, 10.5f }, 0, 0, 0 },
	};
	struct raster_state s = gl_state(1);
	unsigned px[256];
	for (unsigned k = 0; k < sizeof lines / sizeof lines[0]; k++) {
		struct raster_cmd c = line_cmd(lines[k].a[0], lines[k].a[1], lines[k].a[2], lines[k].a[3]);
		unsigned n = draw_lit(&s, &c, px, 256);
		CHECK_EQ(n, lines[k].n);
		CHECK_EQ(raster_gl_fragments(&s, &c, 0, 0, 0, 0, NULL, NULL), lines[k].n);
		if (n && n == lines[k].n) {
			CHECK_EQ(px[0], lines[k].first);
			CHECK_EQ(px[n - 1], lines[k].last);
		}
	}
	/* wide: columns of 3 (2: from the width-1 pixel up), rows of 3 for a y-major line */
	s = gl_state(3);
	struct raster_cmd c = line_cmd(2.3f, 3.4f, 14.6f, 7.2f);
	CHECK_EQ(draw_lit(&s, &c, px, 256), 36);
	CHECK_EQ(px[0], 2002);
	CHECK_EQ(px[35], 7013);
	c = line_cmd(20.2f, 2.3f, 23.6f, 14.8f);
	CHECK_EQ(draw_lit(&s, &c, px, 256), 36);
	CHECK_EQ(px[0], 2019);
	s = gl_state(2);
	c = line_cmd(30.2f, 3.4f, 44.6f, 9.2f);
	CHECK_EQ(draw_lit(&s, &c, px, 256), 28);
	CHECK_EQ(px[0], 3030);
	CHECK_EQ(px[27], 9043);
	/* points: n x n, rounded, centered on the pixel (odd) or the corner (even) */
	static const struct {
		float size, x, y;
		unsigned first, last;
	} points[] = { { 1, 3.3f, 3.6f, 3003, 3003 }, { 2, 9.5f, 3.5f, 3009, 4010 }, { 3, 15.5f, 3.5f, 2014, 4016 },
		       { 2.4f, 21.2f, 3.7f, 3020, 4021 }, { 2.6f, 27.2f, 3.7f, 2026, 4028 }, { 4, 34, 4, 2032, 5035 } };
	for (unsigned k = 0; k < sizeof points / sizeof points[0]; k++) {
		s = gl_state(points[k].size);
		memset(&c, 0, sizeof c);
		c.kind = RASTER_CMD_POINT;
		c.v[0] = win(points[k].x, points[k].y);
		unsigned n = draw_lit(&s, &c, px, 256), side = (unsigned)(points[k].size + 0.5f);
		CHECK_EQ(n, side * side);
		CHECK_EQ(px[0], points[k].first);
		CHECK_EQ(px[n - 1], points[k].last);
	}
}

/*
 * Stippled lines: fragment k of a line drawn if bit (counter + k) / factor %
 * 16 is set; a strip going on from where its last segment got to
 */
static void test_gl_stipple(void)
{
	struct raster_state s = gl_state(1);
	s.line_stippled = true;
	s.line_stipple = 0x00FF;
	s.line_stipple_factor = 2;
	struct raster_cmd c = line_cmd(1.5f, 2.5f, 41.5f, 2.5f);
	unsigned px[256], n = draw_lit(&s, &c, px, 256);
	CHECK_EQ(n, 24);                        /* (0-15, 32-39: of 40) */
	CHECK_EQ(px[15], 2016);
	CHECK_EQ(px[16], 2033);
	c.stipple = 20;                         /* (a strip's later segment: 12 off, 16 on, 12 off) */
	n = draw_lit(&s, &c, px, 256);
	CHECK_EQ(n, 16);
	CHECK_EQ(px[0], 2013);
	CHECK_EQ(px[15], 2028);
	CHECK_EQ(raster_gl_fragments(&s, &c, 0, 0, 0, 0, NULL, NULL), 40);
}

/* Smooth: a pixel's coverage the area of it the disk or rectangle covers (all of it inside) */
struct sum {
	double cover;
	unsigned n, inside;
};

static void add_cover(void *ctx, int x, int y, float t, float cover)
{
	(void)x;
	(void)y;
	(void)t;
	struct sum *s = ctx;
	s->cover += cover;
	s->n++;
	s->inside += cover == 1;
}

static void test_gl_smooth(void)
{
	struct raster_state s = gl_state(9);
	s.smooth = true;
	struct raster_cmd c;
	memset(&c, 0, sizeof c);
	c.kind = RASTER_CMD_POINT;
	c.v[0] = win(20.3f, 20.6f);
	struct sum sum = { 0, 0, 0 };
	raster_gl_fragments(&s, &c, 0, 0, W, H, add_cover, &sum);
	CHECK(sum.cover > 63.6 - 0.2 && sum.cover < 63.6 + 0.2);       /* (pi 4.5^2, to 1/256 a pixel) */
	CHECK(sum.inside >= 44 && sum.n < 100);
	s.width = 2;
	c = line_cmd(3.3f, 4.6f, 30.7f, 22.1f);
	memset(&sum, 0, sizeof sum);
	raster_gl_fragments(&s, &c, 0, 0, W, H, add_cover, &sum);
	double len = sqrt(27.4 * 27.4 + 17.5 * 17.5);
	CHECK(sum.cover > 2 * len - 1e-3 && sum.cover < 2 * len + 1e-3);       /* (exact: the rectangle's area) */
	/* drawn: alpha times the coverage (blending by it) */
	struct raster_target t = target(color_a, depth_a);
	raster_clear(&t, 0, 0, W, H, true, 0xFF000000u, true, 1);
	s.blend = RASTER_BLEND_FACTORS;
	s.blend_src = RASTER_F_SRC_ALPHA;
	s.blend_dst = RASTER_F_ONE_MINUS_SRC_ALPHA;
	raster_run(&t, &s, &c, 1);
	unsigned partial = 0, full = 0;
	for (int k = 0; k < W * H; k++) {
		unsigned g = color_a[k] >> 8 & 255;
		partial += g > 0 && g < 255;
		full += g == 255;
	}
	CHECK(partial > 40 && full > 20);
}

/* Logic ops: the color's bits with the target's, then the color mask */
static void test_logic(void)
{
	/* (0x3C2444F8 with 0x0CAA3BC5) */
	static const uint32_t want[16] = { 0x00000000, 0x0C2000C0, 0x30044438, 0x3C2444F8, 0x008A3B05, 0x0CAA3BC5,
					    0x308E7F3D, 0x3CAE7FFD, 0xC3518002, 0xCF7180C2, 0xF355C43A, 0xFF75C4FA,
					    0xC3DBBB07, 0xCFFBBBC7, 0xF3DFFF3F, 0xFFFFFFFF };
	for (unsigned op = 0; op < 16; op++) {
		struct raster_target t = target(color_a, depth_a);
		raster_clear(&t, 0, 0, W, H, true, 0x0CAA3BC5u, true, 1);
		struct raster_state s = gl_state(1);
		s.blend = RASTER_BLEND_LOGIC;
		s.logic_op = op;
		struct raster_vertex v[3] = { vertex(0, 0, 0, 0x24 / 255.0f, 0x44 / 255.0f, 0xF8 / 255.0f),
					      vertex(8, 0, 0, 0x24 / 255.0f, 0x44 / 255.0f, 0xF8 / 255.0f),
					      vertex(0, 8, 0, 0x24 / 255.0f, 0x44 / 255.0f, 0xF8 / 255.0f) };
		for (int k = 0; k < 3; k++)
			v[k].color[0][3] = 0x3C / 255.0f;
		raster_triangle(&t, &s, &v[0], &v[1], &v[2], false);
		CHECK_EQ(color_a[W + 1], want[op]);
		raster_clear(&t, 0, 0, W, H, true, 0x0CAA3BC5u, true, 1);
		s.color_keep = 0xFF00FF00u;             /* (alpha and green masked) */
		raster_triangle(&t, &s, &v[0], &v[1], &v[2], false);
		CHECK_EQ(color_a[W + 1], (want[op] & 0x00FF00FFu) | 0x0C003B00u);
	}
}

/* The polygon stipple: bit 31 - x % 32 of row (height - 1 - y) % 32 */
static void test_polygon_stipple(void)
{
	struct raster_target t = target(color_a, depth_a);
	raster_clear(&t, 0, 0, W, H, true, 0, true, 1);
	struct raster_state s = gl_state(1);
	s.polygon_stippled = true;
	for (int k = 0; k < 32; k++)
		s.polygon_stipple[k] = k == 3 ? 0x80000001u : k == 5 ? 0xFFFFFFFFu : 0;
	struct raster_vertex v[4] = { vertex(0, 0, 0, 1, 1, 1), vertex(W, 0, 0, 1, 1, 1), vertex(W, H, 0, 1, 1, 1),
				      vertex(0, H, 0, 1, 1, 1) };
	raster_triangle(&t, &s, &v[0], &v[1], &v[2], false);
	raster_triangle(&t, &s, &v[0], &v[2], &v[3], false);
	unsigned px[512], n = lit(color_a, px, 512);
	CHECK_EQ(n, 2 * 4 + 64 * 2);            /* (rows 3 and 35: 4 pixels; rows 5 and 37: all) */
	CHECK_EQ(px[0], 3000);
	CHECK_EQ(px[1], 3031);
	CHECK_EQ(px[2], 3032);
	CHECK_EQ(px[4], 5000);
}

/* ---- GL_EXT_texture_lod_bias ---- */

/* A 32 x 32 texture whose 6 levels are each a color of their own */
static const uint32_t level_color[6] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u, 0xFF00FFFFu,
					 0xFFFF00FFu };
static uint32_t level_texels[32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1];

static struct raster_texture levels_texture(void)
{
	struct raster_texture tx = { .levels = 6 };
	uint32_t *p = level_texels;
	for (unsigned k = 0; k < 6; k++) {
		tx.width[k] = tx.height[k] = 32u >> k;
		tx.texels[k] = p;
		for (unsigned i = 0; i < tx.width[k] * tx.height[k]; i++)
			*p++ = level_color[k];
	}
	return tx;
}

/* A state drawing unit 0's texture as it is, nearest texels of the nearest level, biased */
static struct raster_state biased(const struct raster_texture *tx, float bias)
{
	struct raster_state s = plain();
	s.blend = RASTER_BLEND_NONE;
	s.all_fragments = true;
	s.gl_points = true;
	s.unit[0].texture = tx;
	s.unit[0].min_filter = RASTER_NEAREST_MIPMAP_NEAREST;
	s.unit[0].mag_filter = RASTER_NEAREST;
	s.lod[0].bias = bias;
	s.stages = 1;
	s.stage[0].arg[0][0] = s.stage[0].arg[1][0] = RASTER_TEXTURE0;
	return s;
}

/* A square at (x, y), `size` pixels, the whole texture across it (or one texel's place: constant) */
static void textured_square(const struct raster_state *s, float x, float y, float size, bool constant)
{
	struct raster_target t = target(color_a, depth_a);
	struct raster_vertex v[4] = { vertex(x, y, 0, 1, 1, 1), vertex(x + size, y, 0, 1, 1, 1),
				      vertex(x + size, y + size, 0, 1, 1, 1), vertex(x, y + size, 0, 1, 1, 1) };
	for (int k = 0; k < 4; k++) {
		v[k].tex[0][0] = constant ? 0.3f : (k == 1 || k == 2 ? 1.0f : 0.0f);
		v[k].tex[0][1] = constant ? 0.6f : (k >= 2 ? 1.0f : 0.0f);
		v[k].tex[0][2] = 1;
	}
	raster_triangle(&t, s, &v[0], &v[1], &v[2], true);
	raster_triangle(&t, s, &v[0], &v[2], &v[3], true);
}

/*
 * The bias added to the level of detail before the level is chosen (and
 * whether it's magnified): 32 texels over 12 pixels is 1.415; where the
 * coordinates don't change (and for points) it's magnified, biased or not
 */
static void test_lod_bias(void)
{
	struct raster_texture tx = levels_texture();
	static const struct {
		float bias;
		unsigned level;
	} cases[] = { { 0, 1 }, { 1, 2 }, { 2.3f, 4 }, { -1, 0 }, { -3, 0 }, { 0.2f, 2 }, { 20, 5 } };
	for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		struct raster_state s = biased(&tx, cases[k].bias);
		raster_clear(&(struct raster_target){ W, H, color_a, depth_a, NULL }, 0, 0, W, H, true, 0, true, 1);
		textured_square(&s, 4, 4, 12, false);
		CHECK_EQ(color_a[10 * W + 10], level_color[cases[k].level]);
		textured_square(&s, 30, 4, 12, true);
		CHECK_EQ(color_a[10 * W + 36], level_color[0]);
	}
	/* a line: 32 texels along 24 pixels, 0.415 (minified, level 0), and 2 more */
	for (int b = 0; b < 2; b++) {
		struct raster_state s = biased(&tx, b ? 2.0f : 0.0f);
		struct raster_target t = target(color_a, depth_a);
		raster_clear(&t, 0, 0, W, H, true, 0, true, 1);
		struct raster_vertex v[2] = { vertex(4, 30.5f, 0, 1, 1, 1), vertex(28, 30.5f, 0, 1, 1, 1) };
		v[0].tex[0][1] = v[1].tex[0][1] = 0.5f;
		v[1].tex[0][0] = 1;
		v[0].tex[0][2] = v[1].tex[0][2] = 1;
		struct raster_cmd c;
		memset(&c, 0, sizeof c);
		c.kind = RASTER_CMD_LINE;
		c.textured = true;
		c.v[0] = v[0];
		c.v[1] = v[1];
		raster_run(&t, &s, &c, 1);
		CHECK_EQ(color_a[30 * W + 16], level_color[b ? 2 : 0]);
		/* a point: magnified */
		c.kind = RASTER_CMD_POINT;
		c.v[0] = vertex(40.5f, 30.5f, 0, 1, 1, 1);
		raster_run(&t, &s, &c, 1);
		CHECK_EQ(color_a[30 * W + 40], level_color[0]);
	}
}

/*
 * GL_TEXTURE_MIN_LOD and _MAX_LOD: the level of detail (biased) clamped to
 * them, where the coordinates don't change and for points too (from below)
 */
static void test_lod_range(void)
{
	struct raster_texture tx = levels_texture();
	static const struct {
		float bias, min, max;
		unsigned level, constant;               /* (a 12 pixel square's level, an unchanging one's) */
	} cases[] = { { 0, 2.7f, 1000, 3, 3 }, { 0, -1000, 0.3f, 0, 0 }, { 2, -1000, 1.3f, 1, 0 },
		      { 0, 1.2f, 1.2f, 1, 1 }, { 0, 4.6f, 1000, 5, 5 }, { -3, 0.8f, 2, 1, 1 } };
	for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		struct raster_state s = biased(&tx, cases[k].bias);
		s.lod[0].clamped = true;
		s.lod[0].min = cases[k].min;
		s.lod[0].max = cases[k].max;
		struct raster_target t = target(color_a, depth_a);
		raster_clear(&t, 0, 0, W, H, true, 0, true, 1);
		textured_square(&s, 4, 4, 12, false);
		CHECK_EQ(color_a[10 * W + 10], level_color[cases[k].level]);
		textured_square(&s, 30, 4, 12, true);
		CHECK_EQ(color_a[10 * W + 36], level_color[cases[k].constant]);
		struct raster_cmd c;                    /* (a point: as the unchanging square) */
		memset(&c, 0, sizeof c);
		c.kind = RASTER_CMD_POINT;
		c.textured = true;
		c.v[0] = vertex(40.5f, 30.5f, 0, 1, 1, 1);
		c.v[0].tex[0][2] = 1;
		raster_run(&t, &s, &c, 1);
		CHECK_EQ(color_a[30 * W + 40], level_color[cases[k].constant]);
		c.kind = RASTER_CMD_LINE;               /* (a line: 0.415, clamped) */
		c.v[0] = vertex(4, 30.5f, 0, 1, 1, 1);
		c.v[1] = vertex(28, 30.5f, 0, 1, 1, 1);
		c.v[0].tex[0][1] = c.v[1].tex[0][1] = 0.5f;
		c.v[1].tex[0][0] = 1;
		c.v[0].tex[0][2] = c.v[1].tex[0][2] = 1;
		raster_run(&t, &s, &c, 1);
		float l = 0.415f + cases[k].bias;
		l = l < cases[k].min ? cases[k].min : l > cases[k].max ? cases[k].max : l;
		unsigned want = l > 0 ? (unsigned)(l + 0.5f) : 0;
		CHECK_EQ(color_a[30 * W + 16], level_color[want < 5 ? want : 5]);
	}
}

void test_raster(void)
{
	test_gl_lines();
	test_gl_stipple();
	test_gl_smooth();
	test_logic();
	test_polygon_stipple();
	test_lod_bias();
	test_lod_range();
	test_coverage();
	test_shared_edges();
	test_depth();
	test_texture();
	test_threads();
	test_stages();
	test_fragments();
	test_stencil();
}
