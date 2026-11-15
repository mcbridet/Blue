/*
 * The GPU backend against raster.c: where both must agree exactly
 * (coverage of whole-pixel geometry, depth, texture orientation, clears,
 * points, lines and bitmaps, copies, reading back), and within a tolerance
 * on a random scene (edges and filtering may round differently). Skipped
 * without OpenGL, unless SYSHM_TEST_GPU=1.
 */
#include <stdlib.h>
#include <string.h>

#include "../src/gpu.h"
#include "../src/raster.h"
#include "test.h"

#define W 64
#define H 48

static uint32_t cpu_color[W * H], gpu_color[W * H];
static float cpu_depth[W * H];

static struct raster_state plain(void)
{
	struct raster_state s;
	memset(&s, 0, sizeof s);
	s.z_func = RASTER_Z_NONE;
	s.blend = RASTER_BLEND_PREMULTIPLY;
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

static struct raster_cmd clear_cmd(uint32_t argb, float z)
{
	struct raster_cmd c;
	memset(&c, 0, sizeof c);
	c.kind = RASTER_CMD_CLEAR;
	c.rect[2] = W;
	c.rect[3] = H;
	c.clear_color = c.clear_depth = true;
	c.argb = argb;
	c.z = z;
	return c;
}

static struct raster_cmd tri_cmd(unsigned state, struct raster_vertex a, struct raster_vertex b, struct raster_vertex c,
				 bool textured)
{
	struct raster_cmd cmd;
	memset(&cmd, 0, sizeof cmd);
	cmd.kind = RASTER_CMD_TRIANGLE;
	cmd.state = state;
	cmd.textured = textured;
	cmd.v[0] = a;
	cmd.v[1] = b;
	cmd.v[2] = c;
	return cmd;
}

/* The commands drawn by raster.c into cpu_color */
static void draw_cpu(const struct raster_state *states, const struct raster_cmd *cmds, unsigned n)
{
	struct raster_target t = { W, H, cpu_color, cpu_depth };
	raster_run(&t, states, cmds, n);
}

/* ... and by the GPU, into gpu_color */
static void draw_gpu(struct gpu *g, const struct raster_state *states, const struct raster_cmd *cmds, unsigned n)
{
	struct gpu_target *t = gpu_target_new(g, W, H);
	CHECK(t != NULL);
	gpu_start(g, t, states, cmds, n);
	CHECK(gpu_read(g, t, gpu_color));
	gpu_target_free(g, t);
}

static unsigned channel_diff(uint32_t a, uint32_t b)
{
	unsigned worst = 0;
	for (int sh = 0; sh < 32; sh += 8) {
		int d = (int)(a >> sh & 255) - (int)(b >> sh & 255);
		unsigned ad = (unsigned)(d < 0 ? -d : d);
		worst = ad > worst ? ad : worst;
	}
	return worst;
}

/* Pixels differing by more than `tolerance` in some channel */
static unsigned differing(unsigned tolerance)
{
	unsigned n = 0;
	for (unsigned k = 0; k < W * H; k++)
		n += channel_diff(cpu_color[k], gpu_color[k]) > tolerance;
	return n;
}

static void both(struct gpu *g, const struct raster_state *states, const struct raster_cmd *cmds, unsigned n)
{
	draw_cpu(states, cmds, n);
	draw_gpu(g, states, cmds, n);
}

/*
 * Triangles reaching past the target's edges (as clipped OpenGL triangles
 * do, within the guard band) are interpolated as raster.c does, wherever
 * the GPU cuts them
 */
static void test_past_edges(struct gpu *g)
{
	static const float far[][2] = { { -96, 84 }, { -40, 60 }, { 20, -50 }, { 200, 90 } };
	struct raster_state s[1] = { plain() };
	for (unsigned k = 0; k < sizeof far / sizeof far[0]; k++) {
		struct raster_vertex a = vertex(175.06f, 84, 0.25f, 0.85f, 0.49f, 0.48f);
		struct raster_vertex b = vertex(32, 21.3f, 0.75f, 0.1f, 0.3f, 0.1f);
		struct raster_vertex c = vertex(far[k][0], far[k][1], 0.5f, 0.57f, 0.58f, 0.57f);
		struct raster_cmd cmds[2] = { clear_cmd(0, 1), tri_cmd(0, a, b, c, false) };
		both(g, s, cmds, 2);
		CHECK(differing(2) <= 2);
	}
}

/* Two triangles sharing an edge cover each pixel of their square exactly once */
static void test_coverage(struct gpu *g)
{
	struct raster_state s[1] = { plain() };
	s[0].blend = RASTER_BLEND_FACTORS;
	s[0].blend_src = RASTER_F_ONE;
	s[0].blend_dst = RASTER_F_ONE;
	float one = 1.0f / 255;
	struct raster_vertex a = vertex(2, 3, 0, one, 0, 0), b = vertex(12, 3, 0, one, 0, 0);
	struct raster_vertex c = vertex(12, 13, 0, one, 0, 0), d = vertex(2, 13, 0, one, 0, 0);
	struct raster_cmd cmds[3] = { clear_cmd(0, 1), tri_cmd(0, a, b, c, false), tri_cmd(0, a, c, d, false) };
	draw_gpu(g, s, cmds, 3);
	unsigned wrong = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			wrong += (gpu_color[y * W + x] >> 16 & 255) != (x >= 2 && x < 12 && y >= 3 && y < 13);
	CHECK_EQ(wrong, 0);
}

/* The nearer triangle wins whichever is drawn first, in both depth modes */
static void test_depth(struct gpu *g)
{
	for (int perspective = 0; perspective < 2; perspective++)
		for (int order = 0; order < 2; order++) {
			struct raster_state s[1] = { plain() };
			s[0].z_func = RASTER_Z_LT;
			s[0].z_write = true;
			s[0].perspective_z = perspective;
			struct raster_vertex n0 = vertex(0, 0, 0.2f, 0, 1, 0), n1 = vertex(40, 0, 0.2f, 0, 1, 0);
			struct raster_vertex n2 = vertex(0, 40, 0.2f, 0, 1, 0);
			struct raster_vertex f0 = vertex(0, 0, 0.8f, 1, 0, 0), f1 = vertex(40, 0, 0.8f, 1, 0, 0);
			struct raster_vertex f2 = vertex(0, 40, 0.8f, 1, 0, 0);
			/* (in perspective z, nearer is a larger 1/w) */
			n0.invw = n1.invw = n2.invw = perspective ? 2.0f : 1;
			f0.invw = f1.invw = f2.invw = perspective ? 0.5f : 1;
			struct raster_cmd near = tri_cmd(0, n0, n1, n2, false), far = tri_cmd(0, f0, f1, f2, false);
			struct raster_cmd cmds[3] = { clear_cmd(0, raster_far(&s[0])), order ? far : near,
						      order ? near : far };
			draw_gpu(g, s, cmds, 3);
			CHECK_EQ(gpu_color[5 * W + 5], 0xFF00FF00u);
			CHECK_EQ(gpu_color[45 * W + 60], 0);
		}
}

/* Texel row k is at v = k / height, column j at u = j / width (nearest, no mipmaps) */
static void test_texture(struct gpu *g)
{
	static uint32_t texels[4] = { 0xFF000011u, 0xFF000022u, 0xFF000033u, 0xFF000044u };
	struct raster_texture tx = { .levels = 1, .width = { 2 }, .height = { 2 }, .texels = { texels } };
	struct raster_state s[1] = { plain() };
	texture(&s[0], &tx, 0, false);
	struct raster_vertex v[4] = { vertex(0, 0, 0, 1, 1, 1), vertex(8, 0, 0, 1, 1, 1), vertex(8, 8, 0, 1, 1, 1),
				      vertex(0, 8, 0, 1, 1, 1) };
	v[1].tex[0][0] = v[2].tex[0][0] = 1;
	v[2].tex[0][1] = v[3].tex[0][1] = 1;
	struct raster_cmd cmds[3] = { clear_cmd(0, 1), tri_cmd(0, v[0], v[1], v[2], true),
				      tri_cmd(0, v[0], v[2], v[3], true) };
	draw_gpu(g, s, cmds, 3);
	CHECK_EQ(gpu_color[1 * W + 1], texels[0]);
	CHECK_EQ(gpu_color[1 * W + 6], texels[1]);
	CHECK_EQ(gpu_color[6 * W + 1], texels[2]);
	CHECK_EQ(gpu_color[6 * W + 6], texels[3]);
	gpu_texture_free(g, &tx);
	CHECK_EQ(tx.gpu, 0);
}

/* Clears of a rectangle within a clip; points, lines and bitmaps: pixel for pixel as raster.c */
static void test_exact(struct gpu *g)
{
	static uint32_t texels[5 * 3];
	for (unsigned k = 0; k < 5 * 3; k++)
		texels[k] = k % 4 ? 0xFF000000u | k * 0x110F07u : 0;    /* (some clear) */
	struct raster_texture image = { .levels = 1, .width = { 5 }, .height = { 3 }, .texels = { texels } };
	struct raster_state s[3] = { plain(), plain(), plain() };
	s[1].clip_top = 10;
	s[1].clip_bottom = 30;
	s[2].width = 3;
	s[2].z_func = RASTER_Z_LE;
	s[2].z_write = true;
	struct raster_cmd cmds[7];
	cmds[0] = clear_cmd(0xFF102030u, 1);
	cmds[1] = clear_cmd(0xFF405060u, 0.5f);
	cmds[1].state = 1;
	cmds[1].rect[0] = 5;
	cmds[1].rect[1] = 2;
	cmds[1].rect[2] = 40;
	cmds[1].rect[3] = 45;
	memset(&cmds[2], 0, 5 * sizeof cmds[2]);
	cmds[2].kind = RASTER_CMD_POINT;
	cmds[2].state = 2;
	cmds[2].v[0] = vertex(20.3f, 5.6f, 0.4f, 0, 1, 0);
	cmds[3].kind = RASTER_CMD_POINT;
	cmds[3].v[0] = vertex(50.5f, 40.5f, 0, 1, 1, 0);
	cmds[4].kind = RASTER_CMD_LINE;
	cmds[4].v[0] = vertex(3.2f, 44.1f, 0, 1, 0, 0);
	cmds[4].v[1] = vertex(60.7f, 20.4f, 0, 1, 0, 0);
	cmds[5].kind = RASTER_CMD_LINE;
	cmds[5].state = 2;
	cmds[5].v[0] = vertex(10, 10, 0.3f, 0, 0, 1);
	cmds[5].v[1] = vertex(10.5f, 35, 0.3f, 0, 0, 1);
	cmds[6].kind = RASTER_CMD_BITMAP;
	cmds[6].image = &image;
	cmds[6].v[0] = vertex(30.6f, 32.2f, 0, 0, 0, 0);
	both(g, s, cmds, 7);
	CHECK_EQ(differing(0), 0);
	gpu_texture_free(g, &image);
}

/* One target's color and depth copied into another */
static void test_copy(struct gpu *g)
{
	struct raster_state s[1] = { plain() };
	s[0].z_func = RASTER_Z_LT;
	s[0].z_write = true;
	struct raster_cmd first[2] = { clear_cmd(0xFF000000u, 1),
				       tri_cmd(0, vertex(0, 0, 0.5f, 1, 0, 0), vertex(W, 0, 0.5f, 1, 0, 0),
					       vertex(0, H, 0.5f, 1, 0, 0), false) };
	/* (then a far triangle over all of it: only the half the copied depth leaves shows) */
	struct raster_cmd second[1] = { tri_cmd(0, vertex(-1, -1, 0.7f, 0, 0, 1), vertex(3 * W, -1, 0.7f, 0, 0, 1),
						vertex(-1, 3 * H, 0.7f, 0, 0, 1), false) };
	struct gpu_target *a = gpu_target_new(g, W, H), *b = gpu_target_new(g, W, H);
	CHECK(a && b);
	gpu_start(g, a, s, first, 2);
	gpu_copy(g, b, a);
	gpu_start(g, b, s, second, 1);
	CHECK(gpu_read(g, b, gpu_color));
	CHECK_EQ(gpu_color[2 * W + 2], 0xFFFF0000u);
	CHECK_EQ(gpu_color[(H - 2) * W + W - 2], 0xFF0000FFu);
	gpu_target_free(g, a);
	gpu_target_free(g, b);
}

static uint64_t rng = 88172645463325252ull;

static float frand(float lo, float hi)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return lo + (hi - lo) * (float)(rng >> 40) / (float)(1 << 24);
}

/*
 * test_raster.c's random scene (blend modes, fog, filtering, texture ops,
 * clips, clears, lines), in several batches as rave.c draws them: alike
 * but for pixels at triangle edges and filtering's rounding.
 */
static void test_random(struct gpu *g)
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
			states[k].clip_top = 10;
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
	struct raster_target cpu = { W, H, cpu_color, cpu_depth };
	raster_clear(&cpu, 0, 0, W, H, true, 0xFF000000u, true, 1);
	draw_cpu(states, cmds, CMDS);
	struct gpu_target *t = gpu_target_new(g, W, H);
	struct raster_cmd start = clear_cmd(0xFF000000u, 1);
	struct raster_state any = plain();
	gpu_start(g, t, &any, &start, 1);
	for (unsigned k = 0; k < CMDS; k += 128)
		gpu_start(g, t, states, cmds + k, CMDS - k < 128 ? CMDS - k : 128);
	CHECK(gpu_read(g, t, gpu_color));
	gpu_target_free(g, t);
	unsigned off3 = differing(3), off32 = differing(32);
	if (getenv("SYSHM_GPU_DEBUG"))
		printf("gpu: random scene: %u of %u pixels off by more than 3, %u by more than 32\n", off3, W * H,
		       off32);
	CHECK(off3 <= W * H * 4 / 100);
	CHECK(off32 <= W * H * 2 / 100);
	gpu_texture_free(g, &tx);
}

/*
 * ATI's multitexturing: a second texture modulating the first, at the
 * coordinates in ks: exactly as raster.c with an even second texture, and
 * within filtering's rounding with an uneven one.
 */
static void test_multitexture(struct gpu *g)
{
	static uint32_t base[4] = { 0xFF0000CCu, 0xFF00CC00u, 0xFFCC0000u, 0xFFCCCCCCu };
	static uint32_t even[4] = { 0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u };
	static uint32_t uneven[4 * 4];
	for (unsigned k = 0; k < 16; k++)
		uneven[k] = 0xFF000000u | (k * 16 + 15) * 0x010101u;
	struct raster_texture tx = { .levels = 1, .width = { 2 }, .height = { 2 }, .texels = { base } };
	struct raster_texture lm = { .levels = 1, .width = { 2 }, .height = { 2 }, .texels = { even } };
	struct raster_texture lm2 = { .levels = 1, .width = { 4 }, .height = { 4 }, .texels = { uneven } };
	for (int pass = 0; pass < 2; pass++) {
		struct raster_state s[1] = { plain() };
		s[0].unit[1].texture = pass ? &lm2 : &lm;
		s[0].unit[1].min_filter = RASTER_LINEAR_MIPMAP_NEAREST;
		s[0].unit[1].mag_filter = RASTER_LINEAR;
		s[0].unit[1].clamp_s = s[0].unit[1].clamp_t = s[0].unit[1].projective = true;
		texture(&s[0], &tx, 0, false);
		struct raster_vertex v[4] = { vertex(0, 0, 0, 1, 1, 1), vertex(40, 0, 0, 1, 1, 1),
					      vertex(40, 40, 0, 1, 1, 1), vertex(0, 40, 0, 1, 1, 1) };
		for (int k = 0; k < 4; k++) {
			v[k].invw = v[k].tex[1][2] = pass ? 0.5f + 0.25f * (float)k : 1;  /* (perspective: within tolerance) */
			v[k].tex[0][0] = v[k].tex[1][0] = (k == 1 || k == 2 ? 1.0f : 0.0f) * v[k].invw;
			v[k].tex[0][1] = v[k].tex[1][1] = (k >= 2 ? 1.0f : 0.0f) * v[k].invw;
		}
		struct raster_cmd cmds[3] = { clear_cmd(0, 1), tri_cmd(0, v[0], v[1], v[2], true),
					      tri_cmd(0, v[0], v[2], v[3], true) };
		both(g, s, cmds, 3);
		if (pass) {
			CHECK(differing(3) <= 40);          /* (edges, bilinear rounding) */
		} else {
			CHECK_EQ(differing(1), 0);
			for (int k = 0; k < W * H && getenv("SYSHM_GPU_DEBUG"); k++)
				if (channel_diff(cpu_color[k], gpu_color[k]) > 1)
					printf("gpu: multitexture (%d,%d): %08x against %08x\n", k % W, k / W, gpu_color[k],
					       cpu_color[k]);
			CHECK_EQ(cpu_color[3 * W + 3], 0xFF000066u);    /* 0xCC times 0x80 / 0xFF */
		}
	}
	gpu_texture_free(g, &tx);
	gpu_texture_free(g, &lm);
	gpu_texture_free(g, &lm2);
}

/* Every blend factor as source and destination (some drivers refuse some) */
static void test_blend_factors(struct gpu *g)
{
	struct raster_cmd cmds[1 + 11 * 11];
	struct raster_state states[11 * 11];
	cmds[0] = clear_cmd(0x80402010u, 1);
	for (unsigned k = 0; k < 11 * 11; k++) {
		states[k] = plain();
		states[k].blend = RASTER_BLEND_FACTORS;
		states[k].blend_src = k / 11;
		states[k].blend_dst = k % 11;
		struct raster_vertex v = vertex((float)(k % 11) * 5, (float)(k / 11) * 4, 0, 0.6f, 0.4f, 0.2f);
		v.color[0][3] = 0.4f;
		struct raster_vertex v1 = v, v2 = v;
		v1.x += 5;
		v2.y += 4;
		cmds[1 + k] = tri_cmd(k, v, v1, v2, false);
	}
	draw_cpu(states, cmds, 1 + 11 * 11);
	draw_gpu(g, states, cmds, 1 + 11 * 11);
	CHECK(!gpu_failed(g));
	CHECK(differing(2) <= W * H / 50);
}

/*
 * What OpenGL and Glide add, each as an 8 x 8 square side by side: the
 * stages' operations and arguments, blend equations and the constant
 * color, no blending, color masks (in drawing and clears), the fog
 * coordinate, fragments without alpha drawn; then a projective unit and a
 * texture magnified and minified with different filters. As raster.c, but
 * for a unit in the last place.
 */
static void test_pipeline(struct gpu *g)
{
	enum { N = 15 };
	static uint32_t texel = 0x80402010u, big[16 * 16];
	for (unsigned k = 0; k < 16 * 16; k++)
		big[k] = 0xFF000000u | (k * 0x10101u * 17u);
	struct raster_texture tx = { .levels = 1, .width = { 1 }, .height = { 1 }, .texels = { &texel } };
	struct raster_texture bx = { .levels = 1, .width = { 16 }, .height = { 16 }, .texels = { big } };
	const unsigned T = RASTER_TEXTURE0, C0 = RASTER_COLOR0, C1 = RASTER_COLOR0 + 1, K = RASTER_CONSTANT,
		       P = RASTER_PREVIOUS, A = RASTER_ALPHA, I = RASTER_INVERT;
	static const uint8_t ops[7][8] = {      /* rgb: op, args; a: op, args */
		{ RASTER_OP_MODULATE, T, C0, 0, RASTER_OP_MODULATE, T, C0, 0 },
		{ RASTER_OP_ADD, T, C0, 0, RASTER_OP_MODULATE, T, C0, 0 },
		{ RASTER_OP_INTERPOLATE, K, C0, T, RASTER_OP_MODULATE, T, C0, 0 },
		{ RASTER_OP_SUBTRACT, C0, T, 0, RASTER_OP_REPLACE, C0, 0, 0 },
		{ RASTER_OP_ADD_SIGNED, T, C0, 0, RASTER_OP_REPLACE, T | I, 0, 0 },
		{ RASTER_OP_MODULATE, T, C0 | A | I, 0, RASTER_OP_REPLACE, K, 0, 0 },
		{ RASTER_OP_INTERPOLATE, P, C1, C0 | A, RASTER_OP_ADD, P, C1, 0 },
	};
	struct raster_state s[N];
	struct raster_cmd cmds[1 + 2 * N + 1];
	cmds[0] = clear_cmd(0x80604020u, 1);
	for (unsigned k = 0; k < N; k++) {
		s[k] = plain();
		s[k].blend = RASTER_BLEND_NONE;
		struct raster_vertex v = vertex((float)(k % 8) * 8, (float)(k / 8) * 8, 0.5f, 0.75f, 0.5f, 0.125f);
		v.color[0][3] = k < 7 ? 0.75f : 0.25f;
		const float c1[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
		memcpy(v.color[1], c1, sizeof c1);
		if (k < 7) {
			s[k].unit[0].texture = &tx;
			s[k].stages = k == 6 ? 2 : 1;
			struct raster_stage *st = &s[k].stage[s[k].stages - 1];
			st->op[0] = ops[k][0];
			st->op[1] = ops[k][4];
			for (int n = 0; n < 3; n++) {
				st->arg[0][n] = ops[k][1 + n];
				st->arg[1][n] = ops[k][5 + n];
			}
			st->clamp[0] = st->clamp[1] = true;
			st->shift[0] = k == 4;
			const float constant[4] = { 0.6f, 0.9f, 0.5f, 0.25f };
			memcpy(st->constant, constant, sizeof constant);
			if (k == 6) {
				s[k].stage[0].arg[0][0] = s[k].stage[0].arg[1][0] = T;
				s[k].stage[0].clamp[0] = s[k].stage[0].clamp[1] = true;
			}
		} else if (k < 12) {
			static const unsigned eq[5] = { RASTER_EQ_SUBTRACT, RASTER_EQ_REVERSE_SUBTRACT, RASTER_EQ_MIN,
							RASTER_EQ_MAX, RASTER_EQ_ADD };
			s[k].blend = RASTER_BLEND_FACTORS;
			s[k].blend_src = k == 11 ? RASTER_F_CONSTANT_COLOR : RASTER_F_ONE;
			s[k].blend_dst = k == 11 ? RASTER_F_ONE_MINUS_CONSTANT_ALPHA : RASTER_F_ONE;
			s[k].blend_eq = eq[k - 7];
			const float constant[4] = { 0.5f, 0.25f, 1, 0 };
			memcpy(s[k].blend_color, constant, sizeof constant);
		} else if (k == 12) {
			s[k].color_keep = 0xFF00FF00u;
		} else if (k == 13) {
			s[k].fog_mode = RASTER_FOG_LINEAR;
			s[k].fog_coord = true;
			s[k].fog_end = 10;
			s[k].fog_color[1] = 1;
			v.invw = 0.5f;
			v.fog = 5 * v.invw;
		} else {
			s[k].all_fragments = true;
			s[k].z_func = RASTER_Z_LT;
			s[k].z_write = true;
			v.color[0][3] = 0;
		}
		struct raster_vertex q[4] = { v, v, v, v };
		q[1].x = q[2].x = v.x + 8;
		q[2].y = q[3].y = v.y + 8;
		cmds[1 + 2 * k] = tri_cmd(k, q[0], q[1], q[2], true);
		cmds[2 + 2 * k] = tri_cmd(k, q[0], q[2], q[3], true);
	}
	cmds[1 + 2 * N] = clear_cmd(0x11223344u, 1);
	cmds[1 + 2 * N].state = 12;
	cmds[1 + 2 * N].rect[0] = 40;
	cmds[1 + 2 * N].rect[1] = 20;
	cmds[1 + 2 * N].clear_depth = false;
	draw_cpu(s, cmds, 2 + 2 * N);
	draw_gpu(g, s, cmds, 2 + 2 * N);
	CHECK(!gpu_failed(g));
	for (int k = 0; k < W * H && getenv("SYSHM_GPU_DEBUG"); k++)
		if (channel_diff(cpu_color[k], gpu_color[k]) > 1)
			printf("gpu: pipeline (%d,%d): %08x against %08x\n", k % W, k / W, gpu_color[k], cpu_color[k]);
	CHECK_EQ(differing(1), 0);
	CHECK_EQ(cpu_color[45 * W + 60], 0x80224044u);

	/*
	 * A projective unit (q / w across the square, nearest texels: exact), and
	 * a texture minified (linear) and magnified (nearest)
	 */
	struct raster_state p[2] = { plain(), plain() };
	p[0].unit[0].texture = p[1].unit[0].texture = &bx;
	p[0].unit[0].projective = true;
	p[1].unit[0].min_filter = RASTER_LINEAR;
	p[0].stages = p[1].stages = 1;
	p[0].stage[0].arg[0][0] = p[0].stage[0].arg[1][0] = T;
	p[1].stage[0] = p[0].stage[0];
	struct raster_vertex v[4] = { vertex(0, 0, 0, 1, 1, 1), vertex(32, 0, 0, 1, 1, 1), vertex(32, 32, 0, 1, 1, 1),
				      vertex(0, 32, 0, 1, 1, 1) };
	for (int k = 0; k < 4; k++) {
		float q = 1 + (float)k * 0.5f;
		v[k].tex[0][0] = (k == 1 || k == 2 ? 1.0f : 0.0f) * q;
		v[k].tex[0][1] = (k >= 2 ? 1.0f : 0.0f) * q;
		v[k].tex[0][2] = q;
	}
	struct raster_vertex m[3] = { vertex(40, 0, 0, 1, 1, 1), vertex(46, 0, 0, 1, 1, 1), vertex(40, 6, 0, 1, 1, 1) };
	m[1].tex[0][0] = m[2].tex[0][1] = 4;    /* (4 texels a pixel: linear, as raster.c sees it per triangle) */
	struct raster_vertex z[3] = { vertex(40, 20, 0, 1, 1, 1), vertex(60, 20, 0, 1, 1, 1),
				      vertex(40, 40, 0, 1, 1, 1) };
	z[1].tex[0][0] = z[2].tex[0][1] = 0.45f;   /* (2.8 pixels a texel: no pixel center on a texel's edge) */
	struct raster_cmd more[5] = { clear_cmd(0xFF000000u, 1), tri_cmd(0, v[0], v[1], v[2], true),
				      tri_cmd(0, v[0], v[2], v[3], true), tri_cmd(1, m[0], m[1], m[2], true),
				      tri_cmd(1, z[0], z[1], z[2], true) };
	both(g, p, more, 5);
	if (getenv("SYSHM_GPU_DEBUG"))
		printf("gpu: projective and filtered: %u pixels off by more than 1, %u by more than 40\n", differing(1),
		       differing(40));
	for (int k = 0; k < W * H && getenv("SYSHM_GPU_DEBUG"); k++)
		if (channel_diff(cpu_color[k], gpu_color[k]) > 1)
			printf("gpu: (%d,%d): %08x against %08x\n", k % W, k / W, gpu_color[k], cpu_color[k]);
	CHECK(differing(3) <= 8);               /* (texel edges under perspective, filtering's rounding) */
	CHECK_EQ(differing(40), 0);
	gpu_texture_free(g, &tx);
	gpu_texture_free(g, &bx);
}

/* A failed job: everything after it is refused, and freeing still works */
static void test_failure(void)
{
	setenv("SYSHM_GPU_FAIL", "3", 1);
	struct gpu *g = gpu_open(true);
	unsetenv("SYSHM_GPU_FAIL");
	if (!g || !gpu_ready(g, NULL)) {
		gpu_close(g);
		return;
	}
	struct raster_state s[1] = { plain() };
	struct raster_cmd cmds[1] = { clear_cmd(0xFF00FF00u, 1) };
	struct gpu_target *t = gpu_target_new(g, W, H);         /* job 1 */
	CHECK(t != NULL);
	gpu_start(g, t, s, cmds, 1);                            /* 2 */
	CHECK(gpu_read(g, t, gpu_color));                       /* 3: fails */
	CHECK(gpu_failed(g));
	CHECK(gpu_target_new(g, W, H) == NULL);
	gpu_target_free(g, t);
	gpu_close(g);
}

void test_gpu(void)
{
	struct gpu *g = gpu_open(true);
	const char *why = NULL;
	if (!g || !gpu_ready(g, &why)) {
		const char *need = getenv("SYSHM_TEST_GPU");
		printf("gpu    skipped (%s)\n", why ? why : "no thread");
		CHECK(!(need && strcmp(need, "1") == 0));
		gpu_close(g);
		return;
	}
	test_coverage(g);
	test_past_edges(g);
	test_depth(g);
	test_texture(g);
	test_exact(g);
	test_copy(g);
	test_random(g);
	test_multitexture(g);
	test_blend_factors(g);
	test_pipeline(g);
	CHECK(!gpu_failed(g));
	gpu_close(g);
	test_failure();
}
