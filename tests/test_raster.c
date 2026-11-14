/*
 * The 3D engine's rasterizer: coverage (the top-left rule), depth testing,
 * texture orientation, and drawing recorded commands on helper threads,
 * at once or in background batches, which must give exactly the pixels of
 * drawing them one at a time.
 */
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
	return (struct raster_target){ W, H, color, depth };
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
	v.r = r;
	v.g = g;
	v.b = b;
	v.a = 1;
	for (int k = 0; k < 3; k++)
		v.kd[k] = 1;
	return v;
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
	struct raster_texture tx = { 1, { 2 }, { 2 }, { texels }, 0 };
	struct raster_target t = target(color_a, depth_a);
	raster_clear(&t, 0, 0, W, H, true, 0, true, 1);
	struct raster_state s = plain();
	s.texture = &tx;
	struct raster_vertex v[4] = { vertex(0, 0, 0, 1, 1, 1), vertex(8, 0, 0, 1, 1, 1), vertex(8, 8, 0, 1, 1, 1),
				      vertex(0, 8, 0, 1, 1, 1) };
	v[1].uw = v[2].uw = 1;
	v[2].vw = v[3].vw = 1;
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
	struct raster_texture tx = { 1, { 16 }, { 16 }, { texels }, 0 };

	struct raster_state states[STATES];
	for (int k = 0; k < STATES; k++) {
		states[k] = plain();
		states[k].z_func = k % 3 ? RASTER_Z_LT : RASTER_Z_LE;
		states[k].z_write = k != 4;
		states[k].blend = (unsigned)k % 3;
		states[k].blend_src = RASTER_F_SRC_ALPHA;
		states[k].blend_dst = RASTER_F_ONE_MINUS_SRC_ALPHA;
		states[k].texture = &tx;
		states[k].texture_op = k & 1 ? RASTER_TEX_MODULATE : 0;
		states[k].bilinear = k & 2;
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
			c->v[v].a = frand(0, 1);
			c->v[v].invw = frand(0.3f, 2);
			c->v[v].uw = frand(-1, 2) * c->v[v].invw;
			c->v[v].vw = frand(-1, 2) * c->v[v].invw;
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

void test_raster(void)
{
	test_coverage();
	test_depth();
	test_texture();
	test_threads();
}
