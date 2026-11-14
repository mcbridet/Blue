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
	v.r = r;
	v.g = g;
	v.b = b;
	v.a = 1;
	for (int k = 0; k < 3; k++)
		v.kd[k] = 1;
	return v;
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

/* The commands drawn by raster.c (one at a time, as run_band() would) into cpu_color */
static void draw_cpu(const struct raster_state *states, const struct raster_cmd *cmds, unsigned n)
{
	struct raster_target t = { W, H, cpu_color, cpu_depth };
	for (unsigned k = 0; k < n; k++) {
		const struct raster_cmd *c = &cmds[k];
		const struct raster_state *s = &states[c->state];
		switch (c->kind) {
		case RASTER_CMD_TRIANGLE:
			raster_triangle(&t, s, &c->v[0], &c->v[1], &c->v[2], c->textured);
			break;
		case RASTER_CMD_LINE:
			raster_line(&t, s, &c->v[0], &c->v[1]);
			break;
		case RASTER_CMD_POINT:
			raster_point(&t, s, &c->v[0]);
			break;
		case RASTER_CMD_BITMAP:
			raster_bitmap(&t, s, c->image, c->v[0].x, c->v[0].y, c->v[0].z);
			break;
		case RASTER_CMD_CLEAR: {
			int top = c->rect[1] > s->clip_top ? c->rect[1] : s->clip_top;
			int bottom = c->rect[3] < s->clip_bottom ? c->rect[3] : s->clip_bottom;
			raster_clear(&t, c->rect[0], top, c->rect[2], bottom, c->clear_color, c->argb, c->clear_depth,
				     c->z);
			break;
		}
		}
	}
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
	s[0].texture = &tx;
	struct raster_vertex v[4] = { vertex(0, 0, 0, 1, 1, 1), vertex(8, 0, 0, 1, 1, 1), vertex(8, 8, 0, 1, 1, 1),
				      vertex(0, 8, 0, 1, 1, 1) };
	v[1].uw = v[2].uw = 1;
	v[2].vw = v[3].vw = 1;
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
		states[k].texture = &tx;
		states[k].texture_op = k & 1 ? RASTER_TEX_MODULATE : 0;
		states[k].bilinear = k & 2;
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
		s[0].texture = &tx;
		s[0].texture2 = pass ? &lm2 : &lm;
		struct raster_vertex v[4] = { vertex(0, 0, 0, 1, 1, 1), vertex(40, 0, 0, 1, 1, 1),
					      vertex(40, 40, 0, 1, 1, 1), vertex(0, 40, 0, 1, 1, 1) };
		for (int k = 0; k < 4; k++) {
			v[k].uw = k == 1 || k == 2;
			v[k].vw = k >= 2;
			v[k].invw = v[k].ks[2] = pass ? 0.5f + 0.25f * (float)k : 1;  /* (perspective: within tolerance) */
			v[k].uw *= v[k].invw;
			v[k].vw *= v[k].invw;
			v[k].ks[0] = (k == 1 || k == 2 ? 1.0f : 0.0f) * v[k].invw;
			v[k].ks[1] = (k >= 2 ? 1.0f : 0.0f) * v[k].invw;
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
		v.a = 0.4f;
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
	test_depth(g);
	test_texture(g);
	test_exact(g);
	test_copy(g);
	test_random(g);
	test_multitexture(g);
	test_blend_factors(g);
	CHECK(!gpu_failed(g));
	gpu_close(g);
	test_failure();
}
