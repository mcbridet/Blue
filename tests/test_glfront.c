/*
 * OpenGL's front end (glfront.h) on its own: its sines and matrices
 * against double-precision references, glBegin/glEnd's primitives
 * assembled into triangles, lines and points, flat shading's colors,
 * culling, clipping (the near and far planes, the guard band), the
 * viewport and scissor as the raster state's clip, errors, and pixels
 * drawn by raster.c.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../src/glfront.h"
#include "test.h"

#define W 64
#define H 48

static struct surface surf;
static struct gl_state st;
static struct gl_target target;

static void start(void)
{
	if (surf.target.color)
		surface_free(&surf);
	CHECK(surface_init(&surf, W, H, NULL));
	glf_init(&st);
	glf_viewport(&st, 0, 0, W, H);
	glf_scissor(&st, 0, 0, W, H);
	target = (struct gl_target){ &surf, W, H, 24, 8 };
}

/* Object coordinates as window coordinates (x right, y up, z 0 in the middle of the depth range) */
static void ortho(void)
{
	glf_matrix_mode(&st, GL_PROJECTION);
	glf_identity(&st);
	glf_ortho(&st, 0, W, 0, H, -1, 1);
	glf_matrix_mode(&st, GL_MODELVIEW);
	glf_identity(&st);
}

static unsigned ncmds(void)
{
	return surf.batch[surf.rec].ncmds;
}

static const struct raster_cmd *cmd(unsigned k)
{
	return &surf.batch[surf.rec].cmds[k];
}

/* The commands drawn and gone */
static void drawn(void)
{
	surface_finish(&surf);
}

static void vertex(float x, float y, float z)
{
	const float v[4] = { x, y, z, 1 };
	glf_vertex(&st, &target, v);
}

static void color(float r, float g, float b)
{
	st.color[0] = r;
	st.color[1] = g;
	st.color[2] = b;
	st.color[3] = 1;
}

static bool near(float a, float b, float tolerance)
{
	return fabsf(a - b) <= tolerance;
}

/* A raster vertex at window (x, y) (y up) */
static bool at(const struct raster_vertex *v, float x, float y)
{
	return near(v->x, x, 1e-4f) && near(v->y, (float)H - y, 1e-4f);
}

static uint32_t error(void)
{
	uint32_t e = st.error;
	st.error = GL_NO_ERROR;
	return e;
}

static void test_sincos(void)
{
	const double pi = 3.14159265358979323846;
	double worst = 0;
	for (int k = -1500; k <= 1500; k++) {
		double deg = k * 0.73 + 0.011, s, c;
		glf_sincos(deg, &s, &c);
		double es = fabs(s - sin(deg * pi / 180)), ec = fabs(c - cos(deg * pi / 180));
		worst = es > worst ? es : worst;
		worst = ec > worst ? ec : worst;
	}
	CHECK(worst < 1e-14);           /* (the angle's own rounding in radians is about 1e-16 times it) */
	static const struct {
		double deg, s, c;
	} exact[] = { { 0, 0, 1 }, { 90, 1, 0 }, { 180, 0, -1 }, { 270, -1, 0 }, { 360, 0, 1 }, { -90, -1, 0 },
		      { 450, 1, 0 } };
	for (unsigned k = 0; k < sizeof exact / sizeof exact[0]; k++) {
		double s, c;
		glf_sincos(exact[k].deg, &s, &c);
		CHECK(s == exact[k].s && c == exact[k].c);
	}
}

/* m (column-major floats) against a reference in doubles */
static bool same_matrix(const float *m, const double *ref)
{
	for (int k = 0; k < 16; k++)
		if (fabs(m[k] - ref[k]) > 1e-6 * (1 + fabs(ref[k])))
			return false;
	return true;
}

static void test_matrices(void)
{
	start();
	/* glFrustum and glOrtho */
	glf_matrix_mode(&st, GL_PROJECTION);
	glf_frustum(&st, -1, 2, -0.75, 0.5, 1.5, 20);
	double l = -1, r = 2, b = -0.75, t = 0.5, n = 1.5, f = 20;
	const double frustum[16] = { 2 * n / (r - l), 0, 0, 0, 0, 2 * n / (t - b), 0, 0, (r + l) / (r - l),
				     (t + b) / (t - b), -(f + n) / (f - n), -1, 0, 0, -2 * f * n / (f - n), 0 };
	CHECK(same_matrix(glf_top(&st), frustum));
	glf_frustum(&st, -1, 1, -1, 1, 0, 10);                  /* (near 0: an error, nothing changes) */
	CHECK_EQ(error(), GL_INVALID_VALUE);
	CHECK(same_matrix(glf_top(&st), frustum));
	glf_identity(&st);
	glf_ortho(&st, 0, 640, 0, 480, -1, 1);
	const double ortho_ref[16] = { 2.0 / 640, 0, 0, 0, 0, 2.0 / 480, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1 };
	CHECK(same_matrix(glf_top(&st), ortho_ref));
	glf_ortho(&st, 1, 1, 0, 1, 0, 1);
	CHECK_EQ(error(), GL_INVALID_VALUE);

	/* glRotate: about a normalized axis, as Rodrigues' formula */
	glf_matrix_mode(&st, GL_MODELVIEW);
	glf_rotate(&st, 30, 1, 2, 3);
	double len = sqrt(14.0), x = 1 / len, y = 2 / len, z = 3 / len;
	double s = sin(30 * 3.14159265358979323846 / 180), c = cos(30 * 3.14159265358979323846 / 180);
	const double rot[16] = { x * x * (1 - c) + c, y * x * (1 - c) + z * s, x * z * (1 - c) - y * s, 0,
				 x * y * (1 - c) - z * s, y * y * (1 - c) + c, y * z * (1 - c) + x * s, 0,
				 x * z * (1 - c) + y * s, y * z * (1 - c) - x * s, z * z * (1 - c) + c, 0, 0, 0, 0, 1 };
	CHECK(same_matrix(glf_top(&st), rot));
	glf_identity(&st);
	glf_rotate(&st, 90, 0, 0, 2);                           /* (a quarter turn: exact) */
	const float quarter[16] = { 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	CHECK(!memcmp(glf_top(&st), quarter, sizeof quarter));
	glf_rotate(&st, 45, 0, 0, 0);                           /* (no axis: no change) */
	CHECK(!memcmp(glf_top(&st), quarter, sizeof quarter));

	/* products in OpenGL's order: translate then scale moves (1, 1) to t + s */
	glf_identity(&st);
	glf_translate(&st, 5, 6, 7);
	glf_scale(&st, 2, 3, 4);
	const double ts[16] = { 2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 5, 6, 7, 1 };
	CHECK(same_matrix(glf_top(&st), ts));
	double v[16];
	CHECK_EQ(glf_get(&st, 0x0BA6, v), 16);                  /* GL_MODELVIEW_MATRIX */
	CHECK(v[0] == 2 && v[12] == 5 && v[14] == 7);

	/* the stacks */
	glf_push(&st);
	glf_identity(&st);
	CHECK_EQ(glf_get(&st, 0x0BA3, v), 1);                   /* GL_MODELVIEW_STACK_DEPTH */
	CHECK(v[0] == 2);
	glf_pop(&st);
	CHECK(same_matrix(glf_top(&st), ts));
	glf_pop(&st);
	CHECK_EQ(error(), GL_STACK_UNDERFLOW);
	for (int k = 0; k < GLF_MODELVIEW_DEPTH - 1; k++)
		glf_push(&st);
	CHECK_EQ(error(), GL_NO_ERROR);
	glf_push(&st);
	CHECK_EQ(error(), GL_STACK_OVERFLOW);
	glf_matrix_mode(&st, GL_PROJECTION);
	for (int k = 0; k < GLF_PROJECTION_DEPTH; k++)
		glf_push(&st);
	CHECK_EQ(error(), GL_STACK_OVERFLOW);
	glf_matrix_mode(&st, 0x1234);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	/* a texture unit's own stack */
	glf_matrix_mode(&st, GL_TEXTURE);
	glf_active_texture(&st, GL_TEXTURE0_ARB + 1);
	glf_scale(&st, 2, 2, 2);
	glf_active_texture(&st, GL_TEXTURE0_ARB);
	CHECK(glf_top(&st)[0] == 1 && st.texture[1].m[0][0] == 2);
	glf_active_texture(&st, GL_TEXTURE0_ARB + GLF_TEXTURE_UNITS);
	CHECK_EQ(error(), GL_INVALID_ENUM);
}

/* Each mode's primitives: how many of each, in what order */
static void test_assembly(void)
{
	static const struct {
		uint32_t mode;
		unsigned vertices, cmds;
		uint8_t kind;
	} modes[] = {
		{ GL_POINTS, 3, 3, RASTER_CMD_POINT }, { GL_LINES, 5, 2, RASTER_CMD_LINE },
		{ GL_LINE_STRIP, 4, 3, RASTER_CMD_LINE }, { GL_LINE_LOOP, 4, 4, RASTER_CMD_LINE },
		{ GL_TRIANGLES, 7, 2, RASTER_CMD_TRIANGLE }, { GL_TRIANGLE_STRIP, 5, 3, RASTER_CMD_TRIANGLE },
		{ GL_TRIANGLE_FAN, 5, 3, RASTER_CMD_TRIANGLE }, { GL_QUADS, 9, 4, RASTER_CMD_TRIANGLE },
		{ GL_QUAD_STRIP, 7, 4, RASTER_CMD_TRIANGLE }, { GL_POLYGON, 5, 3, RASTER_CMD_TRIANGLE },
	};
	/* vertices around a circle, counterclockwise (strips zigzag: inner, outer) */
	for (unsigned m = 0; m < sizeof modes / sizeof modes[0]; m++) {
		start();
		ortho();
		glf_enable(&st, GL_CULL_FACE, true);            /* (back faces: none here) */
		glf_begin(&st, modes[m].mode);
		for (unsigned k = 0; k < modes[m].vertices; k++) {
			double s, c;
			glf_sincos(40.0 * k, &s, &c);
			bool strip = modes[m].mode == GL_TRIANGLE_STRIP || modes[m].mode == GL_QUAD_STRIP;
			float radius = strip ? (k & 1 ? 20.0f : 8.0f) : 20.0f;
			if (strip)
				glf_sincos(40.0 * (k / 2), &s, &c);
			vertex(32.3f + radius * (float)c, 24.2f + radius * (float)s, 0);
		}
		glf_end(&st, &target);
		CHECK_EQ(error(), GL_NO_ERROR);
		CHECK_EQ(ncmds(), modes[m].cmds);
		for (unsigned k = 0; k < ncmds(); k++)
			CHECK_EQ(cmd(k)->kind, modes[m].kind);
	}

	/* a quad's triangles: (v0, v1, v3), (v1, v2, v3) */
	start();
	ortho();
	glf_begin(&st, GL_QUADS);
	vertex(10.5f, 10.5f, 0);
	vertex(30.5f, 10.5f, 0);
	vertex(30.5f, 30.5f, 0);
	vertex(10.5f, 30.5f, 0);
	glf_end(&st, &target);
	CHECK_EQ(ncmds(), 2);
	CHECK(at(&cmd(0)->v[0], 10.5f, 10.5f) && at(&cmd(0)->v[1], 30.5f, 10.5f) && at(&cmd(0)->v[2], 10.5f, 30.5f));
	CHECK(at(&cmd(1)->v[0], 30.5f, 10.5f) && at(&cmd(1)->v[1], 30.5f, 30.5f) && at(&cmd(1)->v[2], 10.5f, 30.5f));
	/* a strip's odd triangles turned round: all face the same way (this one's clockwise: back facing) */
	start();
	ortho();
	glf_enable(&st, GL_CULL_FACE, true);
	glf_begin(&st, GL_TRIANGLE_STRIP);
	for (int k = 0; k < 6; k++)
		vertex(5.5f + 8 * (float)(k / 2), k & 1 ? 30.5f : 10.5f, 0);
	glf_end(&st, &target);
	CHECK_EQ(ncmds(), 0);

	/* errors */
	start();
	glf_begin(&st, GL_POLYGON + 1);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	glf_end(&st, &target);
	CHECK_EQ(error(), GL_INVALID_OPERATION);
	CHECK(!glf_enable(&st, 0x1234, true));
	CHECK_EQ(glf_is_enabled(&st, 0x1234), -1);
	CHECK_EQ(glf_is_enabled(&st, GL_DITHER), 1);            /* (on at first) */
	glf_enable(&st, GL_TEXTURE_2D, true);                   /* (per texture unit) */
	glf_active_texture(&st, GL_TEXTURE0_ARB + 1);
	CHECK_EQ(glf_is_enabled(&st, GL_TEXTURE_2D), 0);
	glf_active_texture(&st, GL_TEXTURE0_ARB);
	CHECK_EQ(glf_is_enabled(&st, GL_TEXTURE_2D), 1);
	glf_blend_func(&st, GL_ONE, GL_SRC_ALPHA_SATURATE);
	glf_depth_func(&st, GL_ALWAYS + 1);
	glf_cull_face(&st, GL_CCW);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	glf_point_size(&st, 0);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_viewport(&st, 0, 0, -1, 5);
	CHECK_EQ(error(), GL_INVALID_VALUE);
}

/* Flat shading: each primitive in the color of the vertex OpenGL says gives it */
static void test_flat(void)
{
	static const struct {
		uint32_t mode;
		unsigned vertices;
		uint8_t by[4];          /* each primitive's: the vertex whose color it has */
	} modes[] = {
		{ GL_TRIANGLES, 3, { 2 } }, { GL_TRIANGLE_STRIP, 4, { 2, 3 } }, { GL_TRIANGLE_FAN, 4, { 2, 3 } },
		{ GL_QUADS, 4, { 3, 3 } }, { GL_QUAD_STRIP, 4, { 3, 3 } }, { GL_POLYGON, 4, { 0, 0 } },
		{ GL_LINES, 2, { 1 } }, { GL_LINE_STRIP, 3, { 1, 2 } }, { GL_LINE_LOOP, 3, { 1, 2, 0 } },
	};
	static const float square[4][2] = { { 10.5f, 10.5f }, { 30.5f, 10.5f }, { 30.5f, 30.5f }, { 10.5f, 30.5f } };
	static const float zigzag[4][2] = { { 10.5f, 10.5f }, { 10.5f, 30.5f }, { 30.5f, 10.5f }, { 30.5f, 30.5f } };
	for (unsigned m = 0; m < sizeof modes / sizeof modes[0]; m++) {
		start();
		ortho();
		glf_shade_model(&st, GL_FLAT);
		bool zig = modes[m].mode == GL_TRIANGLE_STRIP || modes[m].mode == GL_QUAD_STRIP;
		glf_begin(&st, modes[m].mode);
		for (unsigned k = 0; k < modes[m].vertices; k++) {
			color(0.1f * (float)(k + 1), 0, 0);
			vertex((zig ? zigzag : square)[k][0], (zig ? zigzag : square)[k][1], 0);
		}
		glf_end(&st, &target);
		for (unsigned k = 0; k < ncmds(); k++) {
			float want = 0.1f * (float)(modes[m].by[k] + 1);
			unsigned n = cmd(k)->kind == RASTER_CMD_LINE ? 2 : 3;
			for (unsigned v = 0; v < n; v++)
				CHECK(cmd(k)->v[v].color[0][0] == want);
		}
	}
	/* smooth: each vertex its own */
	start();
	ortho();
	glf_begin(&st, GL_TRIANGLES);
	for (unsigned k = 0; k < 3; k++) {
		color(0, 0.25f * (float)k, 2);                  /* (clamped) */
		vertex(square[k][0], square[k][1], 0);
	}
	glf_end(&st, &target);
	CHECK_EQ(ncmds(), 1);
	for (unsigned k = 0; k < 3; k++)
		CHECK(cmd(0)->v[k].color[0][1] == 0.25f * (float)k && cmd(0)->v[k].color[0][2] == 1);
}

static void triangle(float x0, float y0, float x1, float y1, float x2, float y2)
{
	glf_begin(&st, GL_TRIANGLES);
	vertex(x0, y0, 0);
	vertex(x1, y1, 0);
	vertex(x2, y2, 0);
	glf_end(&st, &target);
}

static void test_cull(void)
{
	static const struct {
		uint32_t front, cull;
		bool ccw_drawn, cw_drawn;
	} cases[] = {
		{ GL_CCW, GL_BACK, true, false }, { GL_CCW, GL_FRONT, false, true }, { GL_CW, GL_BACK, false, true },
		{ GL_CW, GL_FRONT, true, false }, { GL_CCW, GL_FRONT_AND_BACK, false, false },
	};
	for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		start();
		ortho();
		glf_enable(&st, GL_CULL_FACE, true);
		glf_front_face(&st, cases[k].front);
		glf_cull_face(&st, cases[k].cull);
		triangle(10.5f, 10.5f, 30.5f, 10.5f, 20.5f, 30.5f);    /* counterclockwise */
		CHECK_EQ(ncmds(), cases[k].ccw_drawn);
		triangle(10.5f, 10.5f, 20.5f, 30.5f, 30.5f, 10.5f);    /* clockwise */
		CHECK_EQ(ncmds(), cases[k].ccw_drawn + cases[k].cw_drawn);
		glf_begin(&st, GL_LINES);                       /* (lines aren't culled) */
		vertex(1.5f, 1.5f, 0);
		vertex(9.5f, 1.5f, 0);
		glf_end(&st, &target);
		CHECK_EQ(ncmds(), cases[k].ccw_drawn + cases[k].cw_drawn + 1u);
	}
	/* no culling: both; nothing of no area */
	start();
	ortho();
	triangle(10.5f, 10.5f, 30.5f, 10.5f, 20.5f, 30.5f);
	triangle(10.5f, 10.5f, 20.5f, 30.5f, 30.5f, 10.5f);
	triangle(10.5f, 10.5f, 20.5f, 20.5f, 30.5f, 30.5f);
	CHECK_EQ(ncmds(), 2);
}

static void perspective(void)
{
	glf_matrix_mode(&st, GL_PROJECTION);
	glf_identity(&st);
	glf_frustum(&st, -1, 1, -0.75, 0.75, 1, 10);
	glf_matrix_mode(&st, GL_MODELVIEW);
	glf_identity(&st);
}

/* Every recorded vertex within the depth range and in front of the eye; the commands' count */
static unsigned all_in_range(void)
{
	for (unsigned k = 0; k < ncmds(); k++) {
		unsigned n = cmd(k)->kind == RASTER_CMD_TRIANGLE ? 3 : cmd(k)->kind == RASTER_CMD_LINE ? 2 : 1;
		for (unsigned v = 0; v < n; v++) {
			const struct raster_vertex *r = &cmd(k)->v[v];
			CHECK(r->z >= -1e-6f && r->z <= 1 + 1e-6f && r->invw > 0 && isfinite(r->x) && isfinite(r->y));
		}
	}
	return ncmds();
}

static void test_clipping(void)
{
	/* the near plane: a triangle from in front of it to behind the eye */
	start();
	perspective();
	glf_begin(&st, GL_TRIANGLES);
	vertex(-1, -0.5f, 2);
	vertex(1, -0.5f, 2);
	vertex(0.5f, 0.5f, -5);
	glf_end(&st, &target);
	CHECK(all_in_range() >= 1);
	start();
	perspective();
	glf_begin(&st, GL_TRIANGLES);
	vertex(-1, -0.5f, -0.5f);                               /* (all between the eye and the near plane) */
	vertex(1, -0.5f, -0.5f);
	vertex(0, 0.5f, -0.9f);
	glf_end(&st, &target);
	CHECK_EQ(ncmds(), 0);
	/* the far plane */
	start();
	perspective();
	glf_begin(&st, GL_TRIANGLES);
	vertex(-1, -0.5f, -2);
	vertex(1, -0.5f, -2);
	vertex(0, 0.5f, -30);
	glf_end(&st, &target);
	CHECK(all_in_range() >= 1);
	/* a line through the near plane, a point behind it */
	start();
	perspective();
	glf_begin(&st, GL_LINES);
	vertex(0, 0, 1);
	vertex(0.1f, 0.1f, -3);
	glf_end(&st, &target);
	glf_begin(&st, GL_POINTS);
	vertex(0, 0, -0.5f);
	vertex(0, 0, -3);
	glf_end(&st, &target);
	CHECK_EQ(all_in_range(), 2);
	CHECK_EQ(cmd(1)->kind, RASTER_CMD_POINT);

	/* the guard band: within it, a triangle as given; past it, cut there; wholly past it, nothing */
	start();
	ortho();
	triangle(10.5f, 10.5f, 1.5f * W, 10.5f, 20.5f, 30.5f);
	CHECK_EQ(ncmds(), 1);
	CHECK(at(&cmd(0)->v[1], 1.5f * W, 10.5f));
	start();
	ortho();
	triangle(10.5f, 10.5f, 40.0f * W, 10.5f, 20.5f, 30.5f);
	CHECK(ncmds() >= 1);
	float edge = W * 0.5f + GLF_GUARD * W * 0.5f;           /* (x where the band ends) */
	for (unsigned k = 0; k < ncmds(); k++)
		for (int v = 0; v < 3; v++)
			CHECK(cmd(k)->v[v].x <= edge + 1e-3f);
	start();
	ortho();
	triangle(-10.0f * W, 10.5f, -9.0f * W, 10.5f, -9.5f * W, 30.5f);
	triangle(10.5f, -10.0f * H, 30.5f, -10.0f * H, 20.5f, -9.0f * H);
	CHECK_EQ(ncmds(), 0);

	/* the viewport and scissor box: the raster state's clip */
	start();
	ortho();
	glf_viewport(&st, 8, 4, 32, 24);
	glf_scissor(&st, 0, 10, W, 100);
	triangle(10.5f, 10.5f, 30.5f, 10.5f, 20.5f, 30.5f);
	CHECK_EQ(surf.batch[surf.rec].states[cmd(0)->state].clip_left, 8);
	CHECK_EQ(surf.batch[surf.rec].states[cmd(0)->state].clip_right, 40);
	CHECK_EQ(surf.batch[surf.rec].states[cmd(0)->state].clip_top, H - 28);
	CHECK_EQ(surf.batch[surf.rec].states[cmd(0)->state].clip_bottom, H - 4);
	glf_enable(&st, GL_SCISSOR_TEST, true);
	triangle(10.5f, 10.5f, 30.5f, 10.5f, 20.5f, 30.5f);
	CHECK_EQ(surf.batch[surf.rec].states[cmd(1)->state].clip_bottom, H - 10);
}

static uint32_t pixel(int x, int y)            /* (y up) */
{
	return surf.target.color[(H - 1 - y) * W + x];
}

static bool rgb_near(uint32_t p, int r, int g, int b)
{
	int d[3] = { (int)(p >> 16 & 255) - r, (int)(p >> 8 & 255) - g, (int)(p & 255) - b };
	return abs(d[0]) <= 1 && abs(d[1]) <= 1 && abs(d[2]) <= 1;
}

/* Pixels drawn by raster.c: coverage, colors, depth, masks, alpha */
static void test_pixels(void)
{
	start();
	ortho();
	const float clear[4] = { 0, 0, 0, 0 };
	memcpy(st.clear_color, clear, sizeof clear);
	glf_clear(&st, &target, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	color(1, 0, 0);
	triangle(4.2f, 4.3f, 40.6f, 4.3f, 4.2f, 40.7f);
	drawn();
	CHECK_EQ(pixel(4, 4), 0xFFFF0000u);                     /* (pixels whose centers are inside) */
	CHECK_EQ(pixel(39, 4), 0xFFFF0000u);
	CHECK_EQ(pixel(40, 4), 0);
	CHECK_EQ(pixel(3, 4), 0);
	CHECK_EQ(pixel(4, 3), 0);
	CHECK_EQ(pixel(30, 30), 0);

	/* depth: the nearer wins, whichever comes first (z up is nearer: glOrtho's near is -1) */
	glf_enable(&st, GL_DEPTH_TEST, true);
	for (int order = 0; order < 2; order++) {
		glf_clear(&st, &target, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		for (int k = 0; k < 2; k++) {
			bool nearer = k != order;
			color(nearer, !nearer, 0);
			glf_begin(&st, GL_QUADS);
			float z = nearer ? 0.5f : -0.5f;
			vertex(8.3f + 10 * (float)k, 8.3f, z);
			vertex(40.3f + 10 * (float)k, 8.3f, z);
			vertex(40.3f + 10 * (float)k, 40.3f, z);
			vertex(8.3f + 10 * (float)k, 40.3f, z);
			glf_end(&st, &target);
		}
		drawn();
		CHECK_EQ(pixel(10, 10), order ? 0xFFFF0000u : 0xFF00FF00u);
		CHECK_EQ(pixel(30, 30), 0xFFFF0000u);
		CHECK_EQ(pixel(45, 30), order ? 0xFF00FF00u : 0xFFFF0000u);
	}
	/* no depth bits: no test */
	target.depth_bits = 0;
	glf_clear(&st, &target, GL_COLOR_BUFFER_BIT);
	color(0, 0, 1);
	triangle(4.2f, 4.3f, 40.6f, 4.3f, 4.2f, 40.7f);
	target.depth_bits = 24;
	drawn();
	CHECK_EQ(pixel(5, 5), 0xFF0000FFu);
	glf_enable(&st, GL_DEPTH_TEST, false);

	/* smooth colors: linear across the screen, the vertices' at the vertices */
	glf_clear(&st, &target, GL_COLOR_BUFFER_BIT);
	glf_begin(&st, GL_TRIANGLES);
	color(1, 0, 0);
	vertex(0, 0, 0);
	color(0, 1, 0);
	vertex(60, 0, 0);
	color(0, 0, 1);
	vertex(0, 40, 0);
	glf_end(&st, &target);
	drawn();
	CHECK(rgb_near(pixel(0, 0), 250, 2, 3));               /* (at pixels' centers: x + 0.5, y + 0.5) */
	CHECK(rgb_near(pixel(30, 0), 122, 130, 3));
	CHECK(rgb_near(pixel(0, 20), 122, 2, 131));

	/* masks; alpha test; blending */
	glf_clear(&st, &target, GL_COLOR_BUFFER_BIT);
	glf_color_mask(&st, false, true, true, true);
	color(1, 1, 1);
	triangle(4.2f, 4.3f, 40.6f, 4.3f, 4.2f, 40.7f);
	glf_color_mask(&st, true, true, true, true);
	glf_enable(&st, GL_ALPHA_TEST, true);
	glf_alpha_func(&st, GL_GREATER, 0.5f);
	st.color[3] = 0.25f;
	triangle(30.2f, 30.3f, 60.6f, 30.3f, 30.2f, 46.7f);     /* (fails the test) */
	glf_alpha_func(&st, GL_NEVER, 0);
	st.color[3] = 1;
	triangle(30.2f, 30.3f, 60.6f, 30.3f, 30.2f, 46.7f);     /* (never passes: not even recorded) */
	glf_enable(&st, GL_ALPHA_TEST, false);
	glf_enable(&st, GL_BLEND, true);
	glf_blend_func(&st, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	color(1, 0, 0);
	st.color[3] = 0.5f;
	triangle(4.2f, 4.3f, 20.6f, 4.3f, 4.2f, 20.7f);
	drawn();
	CHECK_EQ(pixel(30, 5), 0xFF00FFFFu);
	CHECK_EQ(pixel(35, 35), 0);
	CHECK_EQ(pixel(5, 5), 0xBF808080u);                     /* (red at half over cyan; alpha 0.5 + 1 * 0.5) */
}

void test_glfront(void)
{
	test_sincos();
	test_matrices();
	test_assembly();
	test_flat();
	test_cull();
	test_clipping();
	test_pixels();
	surface_free(&surf);
}
