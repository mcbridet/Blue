/*
 * OpenGL's front end (glfront.h) on its own: its sines and matrices
 * against double-precision references, glBegin/glEnd's primitives
 * assembled into triangles, lines and points, flat shading's colors,
 * culling, clipping (the near and far planes, the guard band), the
 * viewport and scissor as the raster state's clip, errors, and pixels
 * drawn by raster.c. Textures: pixel transfer (formats, types, packed and
 * swapped, the pixel stores), objects and their errors, completeness, the
 * backend's copy made and retired, each texture environment mode for each
 * base format, the texture matrix and q, glReadPixels. Lighting's values
 * (each kind of light, two sides, separate specular, the color material),
 * generated texture coordinates, clip planes and fog; polygon modes and
 * offset; the stencil's state and clears; the attribute stacks; client
 * arrays (types, indices, bounds, interleaved); queries. Selection and
 * feedback where Mesa's depart from OpenGL 1.2's; evaluators' maps, errors,
 * values, normals and current values. The line stipple's counter, the
 * polygon stipple through the pixel store, logic ops' state, the
 * accumulation buffer.
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
	if (st.names)
		glf_detach(&st, NULL);
	CHECK(surface_init(&surf, W, H, NULL));
	glf_init(&st);
	CHECK(glf_attach(&st, NULL));
	glf_viewport(&st, 0, 0, W, H);
	glf_scissor(&st, 0, 0, W, H);
	target = (struct gl_target){ &surf, W, H, 24, 8, 8, 0 };
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

/* ---- textures ---------------------------------------------------------------------- */

enum { GL_UNSIGNED_SHORT_5_6_5 = 0x8363,
       GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033, GL_UNSIGNED_SHORT_1_5_5_5_REV = 0x8366, GL_UNSIGNED_INT_8_8_8_8 = 0x8035,
       GL_UNSIGNED_INT_8_8_8_8_REV = 0x8367, GL_TEXTURE_1D_T = 0x0DE0, GL_TEXTURE_2D_T = 0x0DE1 };

/* A 2D texture's level 0 texel (x, y) */
static uint32_t texel_at(uint32_t name, int x, int y)
{
	const struct gl_texture *t = name ? glf_texture(&st, name) : &st.defaults[GLF_2D];
	return t && t->level[0].texels ? t->level[0].texels[y * t->level[0].width + x] : 0xDEADBEEFu;
}

/* One texel of each format and type, as the texture keeps it (0xAARRGGBB) */
static uint32_t one(uint32_t internal, uint32_t format, uint32_t type, const uint8_t *bytes)
{
	glf_tex_image(&st, GL_TEXTURE_2D_T, 0, (int32_t)internal, 1, 1, 0, format, type, bytes);
	CHECK_EQ(error(), GL_NO_ERROR);
	return texel_at(1, 0, 0);
}

static void test_pixel_transfer(void)
{
	start();
	glf_bind_texture(&st, GL_TEXTURE_2D_T, 1);
	static const uint8_t rgba[4] = { 0x11, 0x22, 0x33, 0x44 };
	CHECK_EQ(one(GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, rgba), 0x44112233u);
	CHECK_EQ(one(GL_RGBA, GL_BGRA, GL_UNSIGNED_BYTE, rgba), 0x44332211u);      /* (B, G, R, A) */
	CHECK_EQ(one(GL_RGB, GL_RGBA, GL_UNSIGNED_BYTE, rgba), 0xFF112233u);           /* (RGB: alpha 1) */
	CHECK_EQ(one(GL_RGBA, GL_RGB, GL_UNSIGNED_BYTE, rgba), 0xFF112233u);
	CHECK_EQ(one(GL_RGBA, GL_BGR, GL_UNSIGNED_BYTE, rgba), 0xFF332211u);
	CHECK_EQ(one(GL_RGBA, 0x8000, GL_UNSIGNED_BYTE, rgba), 0x11443322u);           /* GL_ABGR_EXT */
	CHECK_EQ(one(GL_LUMINANCE, GL_RGBA, GL_UNSIGNED_BYTE, rgba), 0xFF111111u);     /* (luminance: red) */
	CHECK_EQ(one(GL_RGBA, GL_LUMINANCE, GL_UNSIGNED_BYTE, rgba), 0xFF111111u);
	CHECK_EQ(one(GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, rgba), 0x22111111u);
	CHECK_EQ(one(GL_ALPHA, GL_RGBA, GL_UNSIGNED_BYTE, rgba), 0x44000000u);
	CHECK_EQ(one(GL_INTENSITY, GL_RGBA, GL_UNSIGNED_BYTE, rgba), 0x11111111u);
	CHECK_EQ(one(GL_RGBA, GL_ALPHA, GL_UNSIGNED_BYTE, rgba), 0x11000000u);
	CHECK_EQ(one(4, GL_RGBA, GL_UNSIGNED_BYTE, rgba), 0x44112233u);               /* (1 to 4 components) */
	CHECK_EQ(one(0x8058, GL_RGBA, GL_UNSIGNED_BYTE, rgba), 0x44112233u);          /* GL_RGBA8 */
	/* packed: big-endian halfwords and words, fields from the top (_REV: the bottom) */
	static const uint8_t r565[2] = { 0xF8, 0x1F };                  /* red 31, green 0, blue 31 */
	CHECK_EQ(one(GL_RGB, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, r565), 0xFFFF00FFu);
	static const uint8_t r4444[2] = { 0x84, 0x2F };                 /* 8, 4, 2, 15 */
	CHECK_EQ(one(GL_RGBA, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, r4444), 0xFF884422u);
	static const uint8_t r1555[2] = { 0x80, 0x1F };                 /* (REV: B, the first, in the low bits; A on top) */
	CHECK_EQ(one(GL_RGBA, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, r1555), 0xFF0000FFu);
	CHECK_EQ(one(GL_RGBA, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, rgba), 0x44112233u);
	CHECK_EQ(one(GL_RGBA, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, rgba), 0x11223344u); /* (ARGB in a word) */
	/* other types: signed bytes, shorts, floats; swapped bytes */
	static const int8_t sb[4] = { -5, 0, 127, 63 };
	CHECK_EQ(one(GL_RGBA, GL_RGBA, GL_BYTE, (const uint8_t *)sb), 0x7F0001FFu);
	static const uint8_t us[8] = { 0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x12, 0x34 };
	CHECK_EQ(one(GL_RGBA, GL_RGBA, GL_UNSIGNED_SHORT, us), 0x12FF8000u);
	uint8_t fl[16];
	const float fv[4] = { 1.0f, 0.5f, -1.0f, 2.0f };
	for (int k = 0; k < 4; k++) {
		uint32_t b;
		memcpy(&b, &fv[k], 4);
		fl[4 * k] = (uint8_t)(b >> 24);
		fl[4 * k + 1] = (uint8_t)(b >> 16);
		fl[4 * k + 2] = (uint8_t)(b >> 8);
		fl[4 * k + 3] = (uint8_t)b;
	}
	CHECK_EQ(one(GL_RGBA, GL_RGBA, GL_FLOAT, fl), 0xFFFF8000u);
	glf_pixel_store(&st, 0x0CF0, 1);                                /* GL_UNPACK_SWAP_BYTES */
	CHECK_EQ(one(GL_RGBA, GL_RGBA, GL_UNSIGNED_SHORT, us), 0x34FF0000u);      /* (0x0080, 0x3412) */
	glf_pixel_store(&st, 0x0CF0, 0);

	/* the pixel stores: alignment, row length, skips */
	struct gl_pixel_store ps = { 0, 0, 0, 4, false, false, 0, 0 };
	CHECK_EQ(glf_image_bytes(&ps, GL_RGB, GL_UNSIGNED_BYTE, 3, 2), 12 + 9);        /* (rows of 9, padded to 12) */
	ps.image_height = 3;                    /* (3D images: 3 rows apart, after one) */
	ps.skip_images = 1;
	CHECK_EQ(glf_image_bytes_3d(&ps, GL_RGB, GL_UNSIGNED_BYTE, 3, 2, 2), 36 + 36 + 21);
	ps.image_height = ps.skip_images = 0;
	ps.alignment = 1;
	CHECK_EQ(glf_image_bytes(&ps, GL_RGB, GL_UNSIGNED_BYTE, 3, 2), 18);
	ps.row_length = 5;
	ps.skip_rows = 1;
	ps.skip_pixels = 2;
	CHECK_EQ(glf_image_bytes(&ps, GL_RGBA, GL_UNSIGNED_BYTE, 2, 2), 20 + 8 + 20 + 8);
	CHECK_EQ(glf_image_bytes(&ps, GL_RGB, 0x1234, 2, 2), 0);
	CHECK_EQ(glf_image_bytes(&ps, GL_RGBA, GL_UNSIGNED_SHORT_5_6_5, 2, 2), 0);     /* (565 is 3 components) */
	glf_pixel_store(&st, 0x0CF2, 4);                                /* GL_UNPACK_ROW_LENGTH */
	glf_pixel_store(&st, 0x0CF3, 1);                                /* ..._SKIP_ROWS */
	glf_pixel_store(&st, 0x0CF4, 1);                                /* ..._SKIP_PIXELS */
	glf_pixel_store(&st, 0x0CF5, 1);                                /* ..._ALIGNMENT */
	uint8_t img[4 * 3];
	for (int k = 0; k < 12; k++)
		img[k] = (uint8_t)k;
	glf_tex_image(&st, GL_TEXTURE_2D_T, 0, GL_LUMINANCE, 2, 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, img);
	CHECK(texel_at(1, 0, 0) == 0xFF050505u && texel_at(1, 1, 0) == 0xFF060606u &&
	      texel_at(1, 0, 1) == 0xFF090909u && texel_at(1, 1, 1) == 0xFF0A0A0Au);
	glf_pixel_store(&st, 0x0CF5, 3);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_pixel_store(&st, 0x1234, 1);
	CHECK_EQ(error(), GL_INVALID_ENUM);
}

static void test_texture_objects(void)
{
	start();
	uint32_t names[3];
	glf_gen_textures(&st, 3, names);
	CHECK(names[0] && names[1] != names[0] && names[2] != names[1]);
	CHECK(!glf_is_texture(&st, names[0]));                  /* (not until bound) */
	glf_bind_texture(&st, GL_TEXTURE_2D_T, names[0]);
	CHECK(glf_is_texture(&st, names[0]));
	glf_bind_texture(&st, GL_TEXTURE_1D_T, names[0]);       /* (a 2D texture) */
	CHECK_EQ(error(), GL_INVALID_OPERATION);
	glf_bind_texture(&st, 0x1234, names[0]);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	double v[4];
	CHECK_EQ(glf_get(&st, 0x8069, v), 1);                   /* GL_TEXTURE_BINDING_2D */
	CHECK(v[0] == names[0]);
	/* images: sizes powers of two (with a border, two more), levels within the largest */
	static uint8_t pix[64 * 64 * 4];
	glf_tex_image(&st, GL_TEXTURE_2D_T, 0, GL_RGBA, 6, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_tex_image(&st, GL_TEXTURE_2D_T, 0, GL_RGBA, 6, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, pix);
	CHECK_EQ(error(), GL_NO_ERROR);                         /* (4 x 2 and its border) */
	CHECK_EQ(glf_get_tex_level_parameter(&st, GL_TEXTURE_2D_T, 0, 0x1000, v), 1);
	CHECK(v[0] == 4);
	glf_tex_image(&st, GL_TEXTURE_2D_T, 11, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
	glf_tex_image(&st, GL_TEXTURE_2D_T, 0, 1234, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
	glf_tex_image(&st, GL_TEXTURE_2D_T, 0, GL_RGBA, 2048, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_tex_image(&st, GL_TEXTURE_2D_T, 0, GL_RGBA, 4, 4, 0, 0x1234, GL_UNSIGNED_BYTE, pix);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	glf_tex_image(&st, GL_TEXTURE_2D_T, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_6_5, pix);
	CHECK_EQ(error(), GL_INVALID_OPERATION);
	/* subimages within a level */
	glf_tex_image(&st, GL_TEXTURE_2D_T, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	static const uint8_t red[4 * 4] = { 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255 };
	glf_tex_sub_image(&st, GL_TEXTURE_2D_T, 0, 1, 2, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, red);
	CHECK_EQ(error(), GL_NO_ERROR);
	CHECK(texel_at(names[0], 1, 2) == 0xFFFF0000u && texel_at(names[0], 2, 3) == 0xFFFF0000u &&
	      texel_at(names[0], 0, 2) == 0 && texel_at(names[0], 1, 1) == 0);
	glf_tex_sub_image(&st, GL_TEXTURE_2D_T, 0, 3, 3, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, red);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_tex_sub_image(&st, GL_TEXTURE_2D_T, 1, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, red);
	CHECK_EQ(error(), GL_INVALID_OPERATION);                /* (no such level yet) */
	/* parameters */
	float p[4] = { (float)GL_CLAMP };
	glf_tex_parameter(&st, GL_TEXTURE_2D_T, GL_TEXTURE_WRAP_S, p);
	CHECK(glf_get_tex_parameter(&st, GL_TEXTURE_2D_T, GL_TEXTURE_WRAP_S, v) == 1 && v[0] == GL_CLAMP);
	p[0] = (float)GL_LINEAR_MIPMAP_LINEAR;
	glf_tex_parameter(&st, GL_TEXTURE_2D_T, GL_TEXTURE_MAG_FILTER, p);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	/* deleted: the binding goes back to the default */
	glf_delete_texture(&st, names[0]);
	CHECK(!glf_is_texture(&st, names[0]));
	CHECK(glf_get(&st, 0x8069, v) == 1 && v[0] == 0);
	glf_gen_textures(&st, 1, names);
	CHECK(names[0] != 0);
}

/* Draws with a texture: made complete, drawn, changed, drawn again */
static void test_texture_drawn(void)
{
	start();
	glf_bind_texture(&st, GL_TEXTURE_2D_T, 7);
	glf_enable(&st, GL_TEXTURE_2D, true);
	static uint8_t pix[8 * 8 * 4];
	glf_tex_image(&st, GL_TEXTURE_2D_T, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
	struct raster_texture *drawn = NULL;
	CHECK(!glf_unit_texture(&st, 0, &drawn));               /* (mipmapped by default: incomplete) */
	glf_tex_image(&st, GL_TEXTURE_2D_T, 1, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
	glf_tex_image(&st, GL_TEXTURE_2D_T, 2, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
	CHECK(!glf_unit_texture(&st, 0, &drawn));
	glf_tex_image(&st, GL_TEXTURE_2D_T, 3, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
	CHECK(glf_unit_texture(&st, 0, &drawn) && drawn && drawn->levels == 4);
	const struct gl_texture *t = glf_texture(&st, 7);
	CHECK(drawn->texels[0] == t->level[0].texels);          /* (the texels shared) */
	struct raster_texture *again = NULL;
	CHECK(glf_unit_texture(&st, 0, &again) && again == drawn);
	/* changed: the drawn one retired (its texels its own), a new one made */
	static const uint8_t white[4] = { 255, 255, 255, 255 };
	glf_tex_sub_image(&st, GL_TEXTURE_2D_T, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white);
	CHECK_EQ(st.names->nretired, 1);
	CHECK(st.names->retired[0] == drawn && drawn->texels[0][0] == 0);
	CHECK(glf_unit_texture(&st, 0, &again) && again != drawn && again->texels[0][0] == 0xFFFFFFFFu);
	glf_free_retired(st.names, NULL);
	CHECK_EQ(st.names->nretired, 0);
	/* without mipmaps: complete with a level 0 only, drawn with it alone */
	glf_bind_texture(&st, GL_TEXTURE_2D_T, 8);
	glf_tex_image(&st, GL_TEXTURE_2D_T, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
	float p[4] = { (float)GL_LINEAR };
	glf_tex_parameter(&st, GL_TEXTURE_2D_T, GL_TEXTURE_MIN_FILTER, p);
	CHECK(glf_unit_texture(&st, 0, &drawn) && drawn->levels == 1);
	/* the second unit: its own binding and enable */
	glf_active_texture(&st, GL_TEXTURE0_ARB + 1);
	CHECK(!glf_unit_texture(&st, 1, &drawn));
	glf_enable(&st, GL_TEXTURE_2D, true);
	CHECK(!glf_unit_texture(&st, 1, &drawn));               /* (its default texture has no image) */
	glf_bind_texture(&st, GL_TEXTURE_2D_T, 8);
	CHECK(glf_unit_texture(&st, 1, &drawn));
}

/* A quad over the whole target with texture coordinates 0..1, a vertex color, drawn by raster.c */
static void textured_quad(float r, float g, float b, float a)
{
	glf_begin(&st, GL_QUADS);
	static const float c[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	for (int k = 0; k < 4; k++) {
		st.color[0] = r;
		st.color[1] = g;
		st.color[2] = b;
		st.color[3] = a;
		st.tex[0][0] = c[k][0];
		st.tex[0][1] = c[k][1];
		vertex(c[k][0] * W, c[k][1] * H, 0);
	}
	glf_end(&st, &target);
	drawn();
}

static int channel(uint32_t p, int k)                   /* (k: 0 r, 1 g, 2 b, 3 a) */
{
	return (int)(p >> (k == 3 ? 24 : 16 - 8 * k) & 255);
}

/* Each environment mode for each base format, against OpenGL 1.2's table 3.18 (and GL_ADD's) */
static void test_texture_env(void)
{
	static const uint32_t bases[6] = { GL_ALPHA, GL_LUMINANCE, GL_LUMINANCE_ALPHA, GL_INTENSITY, GL_RGB, GL_RGBA };
	static const uint32_t modes[5] = { GL_REPLACE, GL_MODULATE, GL_DECAL, GL_BLEND, GL_ADD };
	const float cf[4] = { 0.8f, 0.4f, 0.2f, 0.6f }, cc[4] = { 0.1f, 0.9f, 0.5f, 0.3f };
	static const uint8_t tex[4] = { 0x40, 0x80, 0xC0, 0x20 };       /* (R, G, B, A: L and I are R) */
	for (unsigned m = 0; m < 5; m++)
		for (unsigned b = 0; b < 6; b++) {
			start();
			ortho();
			glf_bind_texture(&st, GL_TEXTURE_2D_T, 1);
			glf_tex_image(&st, GL_TEXTURE_2D_T, 0, (int32_t)bases[b], 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
			float p[4] = { (float)GL_NEAREST };
			glf_tex_parameter(&st, GL_TEXTURE_2D_T, GL_TEXTURE_MIN_FILTER, p);
			glf_enable(&st, GL_TEXTURE_2D, true);
			float mode[4] = { (float)modes[m] };
			glf_tex_env(&st, GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode);
			glf_tex_env(&st, GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, cc);
			CHECK_EQ(error(), GL_NO_ERROR);
			textured_quad(cf[0], cf[1], cf[2], cf[3]);
			/* the texture's components as the base format has them (Lt, It: red) */
			float t[4] = { tex[0] / 255.0f, tex[1] / 255.0f, tex[2] / 255.0f, tex[3] / 255.0f };
			float rgb[3], a;
			bool has_rgb = bases[b] != GL_ALPHA, has_a = bases[b] == GL_ALPHA || bases[b] == GL_LUMINANCE_ALPHA ||
								    bases[b] == GL_RGBA || bases[b] == GL_INTENSITY;
			float ct[3], at;
			for (int k = 0; k < 3; k++)
				ct[k] = bases[b] == GL_RGB || bases[b] == GL_RGBA ? t[k] : t[0];
			at = bases[b] == GL_INTENSITY ? t[0] : t[3];
			for (int k = 0; k < 3; k++) {
				float f = cf[k], c = ct[k];
				switch (modes[m]) {
				case GL_REPLACE: rgb[k] = has_rgb ? c : f; break;
				case GL_MODULATE: rgb[k] = has_rgb ? f * c : f; break;
				case GL_DECAL:
					rgb[k] = bases[b] == GL_RGB ? c : bases[b] == GL_RGBA ? f * (1 - at) + c * at : f;
					break;
				case GL_BLEND: rgb[k] = has_rgb ? f * (1 - c) + cc[k] * c : f; break;
				default: rgb[k] = has_rgb ? f + c : f; break;
				}
				rgb[k] = rgb[k] < 1 ? rgb[k] : 1;
			}
			switch (modes[m]) {
			case GL_REPLACE: a = has_a ? at : cf[3]; break;
			case GL_MODULATE: a = has_a ? cf[3] * at : cf[3]; break;
			case GL_DECAL: a = cf[3]; break;
			case GL_BLEND: a = bases[b] == GL_INTENSITY ? cf[3] * (1 - at) + cc[3] * at : has_a ? cf[3] * at : cf[3]; break;
			default: a = bases[b] == GL_INTENSITY ? cf[3] + at : has_a ? cf[3] * at : cf[3]; break;
			}
			a = a < 1 ? a : 1;
			uint32_t px = pixel(W / 2, H / 2);
			bool ok = true;
			for (int k = 0; k < 3; k++)
				ok &= abs(channel(px, k) - (int)(rgb[k] * 255 + 0.5f)) <= 1;
			ok &= abs(channel(px, 3) - (int)(a * 255 + 0.5f)) <= 1;
			if (!ok)
				fprintf(stderr, "mode 0x%x base 0x%x: %08x, expected %.3f %.3f %.3f %.3f\n", modes[m], bases[b],
					px, (double)rgb[0], (double)rgb[1], (double)rgb[2], (double)a);
			CHECK(ok);
		}
}

/* The texture matrix and q; repeat and clamp; glReadPixels */
static void test_texture_coords(void)
{
	start();
	ortho();
	static const uint8_t checker[2 * 2 * 4] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
	glf_bind_texture(&st, GL_TEXTURE_2D_T, 1);
	glf_tex_image(&st, GL_TEXTURE_2D_T, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, checker);
	float p[4] = { (float)GL_NEAREST };
	glf_tex_parameter(&st, GL_TEXTURE_2D_T, GL_TEXTURE_MIN_FILTER, p);
	glf_tex_parameter(&st, GL_TEXTURE_2D_T, GL_TEXTURE_MAG_FILTER, p);
	glf_enable(&st, GL_TEXTURE_2D, true);
	float mode[4] = { (float)GL_REPLACE };
	glf_tex_env(&st, GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode);
	textured_quad(1, 1, 1, 1);
	/* texture rows go up: row 0 at the bottom */
	CHECK(pixel(1, 1) == 0xFFFF0000u && pixel(W - 2, 1) == 0xFF00FF00u && pixel(1, H - 2) == 0xFF0000FFu &&
	      pixel(W - 2, H - 2) == 0xFFFFFFFFu);
	/* scaled by the texture matrix: repeated twice */
	glf_matrix_mode(&st, GL_TEXTURE);
	glf_scale(&st, 2, 2, 1);
	glf_matrix_mode(&st, GL_MODELVIEW);
	textured_quad(1, 1, 1, 1);
	CHECK(pixel(1, 1) == 0xFFFF0000u && pixel(W / 4 + 1, 1) == 0xFF00FF00u && pixel(W / 2 + 1, 1) == 0xFFFF0000u);
	/* clamped (GL_CLAMP: to the edge texels) */
	p[0] = (float)GL_CLAMP;
	glf_tex_parameter(&st, GL_TEXTURE_2D_T, GL_TEXTURE_WRAP_S, p);
	glf_tex_parameter(&st, GL_TEXTURE_2D_T, GL_TEXTURE_WRAP_T, p);
	textured_quad(1, 1, 1, 1);
	CHECK(pixel(W / 2 + 1, 1) == 0xFF00FF00u && pixel(W - 2, H - 2) == 0xFFFFFFFFu);
	/* q: (s, t, q) divided per pixel (the state's units projective) */
	glf_matrix_mode(&st, GL_TEXTURE);
	glf_identity(&st);
	glf_matrix_mode(&st, GL_MODELVIEW);
	glf_begin(&st, GL_TRIANGLES);
	st.tex[0][0] = 0.25f * 2;
	st.tex[0][1] = 0.25f * 2;
	st.tex[0][3] = 2;
	vertex(0.5f, 0.5f, 0);
	vertex(20.5f, 0.5f, 0);
	vertex(0.5f, 20.5f, 0);
	glf_end(&st, &target);
	CHECK(surf.batch[surf.rec].states[cmd(ncmds() - 1)->state].unit[0].projective);
	drawn();
	CHECK_EQ(pixel(2, 2), 0xFFFF0000u);                     /* (s/q = t/q = 0.25: the red texel) */
	st.tex[0][3] = 1;

	/* glReadPixels: from the bottom, packed as asked */
	uint8_t out[64];
	memset(out, 0xEE, sizeof out);
	glf_pixel_store(&st, 0x0D05, 1);                                /* GL_PACK_ALIGNMENT */
	glf_read_pixels(&st, &target, 0, 0, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
	CHECK(out[0] == 255 && out[1] == 0 && out[2] == 0 && out[3] == 255 && out[8] == 0xEE);
	glf_read_pixels(&st, &target, W - 1, H - 1, 1, 1, GL_BGRA, GL_UNSIGNED_BYTE, out);
	CHECK(out[0] == 255 && out[1] == 255 && out[2] == 255);
	glf_read_pixels(&st, &target, 0, 0, 1, 1, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, out);
	CHECK(out[0] == 0xF8 && out[1] == 0x00);
	glf_read_pixels(&st, &target, 0, 0, 1, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, out);
	CHECK_EQ(out[0], 255);
	glf_read_pixels(&st, &target, 0, 0, 1, 1, 0x1234, GL_UNSIGNED_BYTE, out);
	CHECK_EQ(error(), GL_INVALID_ENUM);
}

/* ---- lighting, fog, texture coordinate generation, clip planes ---------------------------------------- */

static void shade_at(float x, float y, float z, struct gl_vertex *v)
{
	const float obj[4] = { x, y, z, 1 };
	glf_shade(&st, obj, v);
}

static void test_lighting(void)
{
	start();
	struct gl_vertex v;
	glf_enable(&st, GL_LIGHTING, true);
	glf_enable(&st, GL_LIGHT0, true);
	/* the default light (white, from +z) on the default material: ambient 0.2 * 0.2, diffuse 0.8 * n.l */
	const float n[3] = { 0, 0.6f, 0.8f };
	memcpy(st.normal, n, sizeof n);
	shade_at(0, 0, 0, &v);
	CHECK(near(v.color[0][0], 0.04f + 0.8f * 0.8f, 1e-6f) && near(v.color[0][3], 1, 0));
	CHECK(near(v.secondary[0][0], 0, 0) && near(v.color[1][1], v.color[0][1], 0));
	/* a positional light, attenuated (constant 1, linear 0.5 at a distance of 2) and the spot it gives */
	glf_enable(&st, GL_LIGHT0, false);
	glf_enable(&st, GL_LIGHT0 + 1, true);
	const float white[4] = { 1, 1, 1, 1 }, pos[4] = { 0, 0, 2, 1 }, down[3] = { 0, 0, -1 }, half[1] = { 0.5f };
	const float cutoff[1] = { 30 }, two[1] = { 2 };
	glf_light(&st, GL_LIGHT0 + 1, GL_DIFFUSE, white);
	glf_light(&st, GL_LIGHT0 + 1, GL_POSITION, pos);
	glf_light(&st, GL_LIGHT0 + 1, GL_LINEAR_ATTENUATION, half);
	st.normal[1] = 0;
	st.normal[2] = 1;
	shade_at(0, 0, 0, &v);
	CHECK(near(v.color[0][0], 0.04f + 0.8f * 0.5f, 1e-6f));
	glf_light(&st, GL_LIGHT0 + 1, GL_SPOT_DIRECTION, down);
	glf_light(&st, GL_LIGHT0 + 1, GL_SPOT_CUTOFF, cutoff);
	glf_light(&st, GL_LIGHT0 + 1, GL_SPOT_EXPONENT, two);
	shade_at(0, 0, 0, &v);                                  /* (in the spot's middle: cos 0 = 1) */
	CHECK(near(v.color[0][0], 0.04f + 0.8f * 0.5f, 1e-6f));
	shade_at(2, 0, 0, &v);                                  /* (45 degrees off: outside 30) */
	CHECK(near(v.color[0][0], 0.04f, 1e-6f));
	/* two-sided: the back's material, the normal turned round (facing it: lit) */
	glf_enable(&st, GL_LIGHT0 + 1, false);
	glf_enable(&st, GL_LIGHT0, true);
	const float on[1] = { 1 }, green[4] = { 0, 1, 0, 1 };
	glf_light_model(&st, GL_LIGHT_MODEL_TWO_SIDE, on);
	glf_material(&st, GL_BACK, GL_DIFFUSE, green);
	st.normal[2] = -1;
	shade_at(0, 0, 0, &v);
	CHECK(near(v.color[0][0], 0.04f, 1e-6f) && v.color[1][1] == 1 && near(v.color[1][0], 0.04f, 1e-6f));
	/* specular, separate: in the secondary color */
	const float sep[1] = { (float)GL_SEPARATE_SPECULAR_COLOR }, shine[1] = { 10 };
	glf_material(&st, GL_FRONT, GL_SPECULAR, white);
	glf_material(&st, GL_FRONT, GL_SHININESS, shine);
	glf_light_model(&st, GL_LIGHT_MODEL_COLOR_CONTROL, sep);
	st.normal[2] = 1;
	shade_at(0, 0, 0, &v);                                  /* (n.h 1: all of it) */
	CHECK(near(v.secondary[0][0], 1, 1e-6f) && near(v.color[0][0], 0.84f, 1e-6f) && v.secondary[0][3] == 0);
	/* the color material tracking the current color */
	glf_color_material(&st, GL_FRONT, GL_DIFFUSE);
	glf_enable(&st, GL_COLOR_MATERIAL, true);
	color(0.5f, 0.25f, 0);
	double got[4];
	CHECK(glf_get_material(&st, GL_FRONT, GL_DIFFUSE, got) == 4 && got[0] == 0.5 && got[1] == 0.25);
	/* positions in eye coordinates, as the modelview was */
	glf_translate(&st, 1, 2, 3);
	const float origin[4] = { 0, 0, 0, 1 };
	glf_light(&st, GL_LIGHT0 + 3, GL_POSITION, origin);
	CHECK(glf_get_light(&st, GL_LIGHT0 + 3, GL_POSITION, got) == 4 && got[0] == 1 && got[1] == 2 && got[2] == 3);
	CHECK_EQ(error(), GL_NO_ERROR);
	/* errors */
	glf_light(&st, GL_LIGHT0 + 8, GL_DIFFUSE, white);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	const float wide[1] = { 100 }, shinier[1] = { 200 };
	glf_light(&st, GL_LIGHT0, GL_SPOT_CUTOFF, wide);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_material(&st, GL_FRONT, GL_SHININESS, shinier);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	CHECK_EQ(glf_get_material(&st, GL_FRONT_AND_BACK, GL_DIFFUSE, got), 0);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	glf_color_material(&st, GL_FRONT, GL_SHININESS);
	CHECK_EQ(error(), GL_INVALID_ENUM);
}

static void test_texgen_clip(void)
{
	start();
	struct gl_vertex v;
	/* object linear: by the plane in object coordinates */
	const double sp[4] = { 0.5, 0, 0, 0.25 };
	const double mode_object[1] = { GL_OBJECT_LINEAR }, mode_eye[1] = { GL_EYE_LINEAR }, sphere[1] = { GL_SPHERE_MAP };
	glf_tex_gen(&st, GL_S, GL_TEXTURE_GEN_MODE, mode_object);
	glf_tex_gen(&st, GL_S, 0x2501, sp);                     /* GL_OBJECT_PLANE */
	glf_enable(&st, GL_TEXTURE_GEN_S, true);
	shade_at(2, 7, 0, &v);
	CHECK(near(v.tex[0][0], 1.25f, 1e-6f) && v.tex[1][0] == 0);
	/* eye linear: the plane given now (the modelview a translation) */
	glf_translate(&st, 10, 0, 0);
	const double ep[4] = { 1, 0, 0, 0 };
	glf_tex_gen(&st, GL_S, GL_TEXTURE_GEN_MODE, mode_eye);
	glf_tex_gen(&st, GL_S, 0x2502, ep);                     /* GL_EYE_PLANE: (1, 0, 0, -10) */
	glf_identity(&st);
	shade_at(3, 0, 0, &v);                                  /* (eye x 3: 3 - 10) */
	CHECK(near(v.tex[0][0], -7, 1e-5f));
	/* the sphere map: straight back at the eye, the middle */
	glf_tex_gen(&st, GL_S, GL_TEXTURE_GEN_MODE, sphere);
	shade_at(0, 0, -1, &v);
	CHECK(near(v.tex[0][0], 0.5f, 1e-6f));
	glf_tex_gen(&st, GL_Q, GL_TEXTURE_GEN_MODE, sphere);   /* (s and t only) */
	CHECK_EQ(error(), GL_INVALID_ENUM);
	/* clip planes: distances in eye coordinates; nothing on disabled ones */
	const double plane[4] = { 1, 0, 0, -2 };
	glf_clip_plane(&st, GL_CLIP_PLANE0 + 2, plane);
	shade_at(5, 1, 1, &v);
	CHECK(v.user[2] == 0);
	glf_enable(&st, GL_CLIP_PLANE0 + 2, true);
	shade_at(5, 1, 1, &v);
	CHECK(near(v.user[2], 3, 1e-6f));
	/* a triangle cut by it (ortho: window coordinates), half its pixels gone */
	ortho();
	const double cut[4] = { 1, 0, 0, -20 };
	glf_clip_plane(&st, GL_CLIP_PLANE0 + 2, cut);
	color(1, 1, 1);
	glf_begin(&st, GL_TRIANGLES);
	vertex(4.3f, 4.2f, 0);
	vertex(40.6f, 4.2f, 0);
	vertex(4.3f, 40.7f, 0);
	glf_end(&st, &target);
	drawn();
	CHECK(pixel(19, 5) == 0 && pixel(21, 5) == 0xFFFFFFFFu);
	/* fog: the raster state's (the coordinate: eye-space |z|) */
	glf_enable(&st, GL_CLIP_PLANE0 + 2, false);
	glf_enable(&st, GL_FOG, true);
	const float linear[1] = { (float)GL_LINEAR }, end[1] = { 7 };
	glf_fog(&st, GL_FOG_MODE, linear);
	glf_fog(&st, GL_FOG_END, end);
	shade_at(0, 0, -3, &v);
	CHECK(v.fog == 3);
	glf_begin(&st, GL_POINTS);
	vertex(5.5f, 5.5f, 0);
	glf_end(&st, &target);
	const struct raster_state *s = &surf.batch[surf.rec].states[cmd(ncmds() - 1)->state];
	CHECK(s->fog_mode == RASTER_FOG_LINEAR && s->fog_coord && s->fog_end == 7);
	const float bad[1] = { 5 };
	glf_fog(&st, GL_FOG_MODE, bad);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	glf_fog(&st, 0x8450, bad);                              /* (no GL_EXT_fog_coord on Mac OS 9) */
	CHECK_EQ(error(), GL_INVALID_ENUM);
}

/* ---- polygon modes and offset, stencil --------------------------------------------------------- */

static unsigned commands(uint8_t kind)
{
	unsigned n = 0;
	for (unsigned k = 0; k < ncmds(); k++)
		n += cmd(k)->kind == kind;
	return n;
}

static void test_polygons(void)
{
	start();
	ortho();
	color(1, 1, 1);
	/* outlined: a quad's 4 edges (not its diagonal); a polygon's 5, its closing edge at glEnd */
	glf_polygon_mode(&st, GL_FRONT_AND_BACK, GL_LINE);
	glf_begin(&st, GL_QUADS);
	vertex(4.3f, 4.2f, 0);
	vertex(20.6f, 4.2f, 0);
	vertex(20.6f, 20.7f, 0);
	vertex(4.3f, 20.7f, 0);
	glf_end(&st, &target);
	CHECK(commands(RASTER_CMD_LINE) == 4 && commands(RASTER_CMD_TRIANGLE) == 0);
	glf_begin(&st, GL_POLYGON);
	vertex(30.3f, 4.2f, 0);
	vertex(40.6f, 4.2f, 0);
	vertex(44.6f, 12.7f, 0);
	st.edge_flag = false;                                   /* (the edge from this one isn't drawn) */
	vertex(37.6f, 20.7f, 0);
	st.edge_flag = true;
	vertex(28.3f, 12.7f, 0);
	glf_end(&st, &target);
	CHECK_EQ(commands(RASTER_CMD_LINE), 4 + 4);
	/* points: each vertex once */
	glf_polygon_mode(&st, GL_FRONT_AND_BACK, GL_POINT);
	glf_begin(&st, GL_QUADS);
	vertex(4.3f, 30.2f, 0);
	vertex(20.6f, 30.2f, 0);
	vertex(20.6f, 40.7f, 0);
	vertex(4.3f, 40.7f, 0);
	glf_end(&st, &target);
	CHECK_EQ(commands(RASTER_CMD_POINT), 4);
	/* cut by a clip plane: its edge along the plane isn't drawn */
	glf_polygon_mode(&st, GL_FRONT_AND_BACK, GL_LINE);
	const double plane[4] = { 1, 0, 0, -50 };
	glf_clip_plane(&st, GL_CLIP_PLANE0, plane);
	glf_enable(&st, GL_CLIP_PLANE0, true);
	unsigned before = commands(RASTER_CMD_LINE);
	glf_begin(&st, GL_TRIANGLES);
	vertex(40.3f, 30.2f, 0);
	vertex(60.6f, 30.2f, 0);
	vertex(60.6f, 46.7f, 0);
	glf_end(&st, &target);
	CHECK_EQ(commands(RASTER_CMD_LINE) - before, 3);
	glf_enable(&st, GL_CLIP_PLANE0, false);
	/* the back's mode for back faces only */
	glf_polygon_mode(&st, GL_FRONT, GL_FILL);
	before = commands(RASTER_CMD_TRIANGLE);
	glf_begin(&st, GL_TRIANGLES);
	vertex(4.3f, 4.2f, 0);
	vertex(4.3f, 20.7f, 0);                         /* (clockwise: a back face) */
	vertex(20.6f, 4.2f, 0);
	glf_end(&st, &target);
	CHECK_EQ(commands(RASTER_CMD_TRIANGLE), before);
	glf_polygon_mode(&st, GL_FRONT_AND_BACK, 0x1234);
	CHECK_EQ(error(), GL_INVALID_ENUM);

	/* offset: the depth slope times the factor, plus the units times 24 bits' resolution */
	start();
	ortho();
	glf_polygon_mode(&st, GL_FRONT_AND_BACK, GL_FILL);
	glf_enable(&st, GL_POLYGON_OFFSET_FILL, true);
	glf_polygon_offset(&st, 0, 1000);
	glf_begin(&st, GL_TRIANGLES);
	vertex(4.3f, 4.2f, 0);
	vertex(40.6f, 4.2f, 0);
	vertex(4.3f, 40.7f, 0);
	glf_end(&st, &target);
	CHECK(near(cmd(ncmds() - 1)->v[0].z, 0.5f + 1000.0f / 16777215, 1e-7f));
	glf_polygon_offset(&st, 2, 0);
	glf_begin(&st, GL_TRIANGLES);           /* (z from 0.5 to 0.25 over 32 pixels: a slope of 1/128) */
	vertex(4.3f, 4.2f, 0);
	vertex(36.3f, 4.2f, 0.5f);
	vertex(4.3f, 40.7f, 0);
	glf_end(&st, &target);
	CHECK(near(cmd(ncmds() - 1)->v[0].z, 0.5f + 2.0f / 128, 1e-6f));
}

static void test_stencil_state(void)
{
	start();
	ortho();
	glf_enable(&st, GL_STENCIL_TEST, true);
	glf_stencil_func(&st, GL_NOTEQUAL, 300, 0x3F);
	glf_stencil_op(&st, GL_INCR_WRAP_EXT, GL_INVERT, GL_REPLACE);
	glf_stencil_mask(&st, 0x1F0);
	glf_color_mask(&st, false, false, false, false);
	color(1, 1, 1);
	glf_begin(&st, GL_POINTS);
	vertex(5.5f, 5.5f, 0);
	glf_end(&st, &target);
	CHECK_EQ(ncmds(), 1);                   /* (all colors masked, but the stencil written: drawn) */
	const struct raster_state *s = &surf.batch[surf.rec].states[cmd(0)->state];
	CHECK(s->stencil_func == RASTER_Z_NE && s->stencil_ref == 255 && s->stencil_mask == 0x3F &&
	      s->stencil_write == 0xF0);
	CHECK(s->stencil_op[0] == RASTER_STENCIL_INCR_WRAP && s->stencil_op[1] == RASTER_STENCIL_INVERT &&
	      s->stencil_op[2] == RASTER_STENCIL_REPLACE);
	st.clear_stencil = 0x1A7;
	glf_clear(&st, &target, GL_STENCIL_BUFFER_BIT);
	CHECK(cmd(1)->kind == RASTER_CMD_CLEAR && cmd(1)->clear_stencil && cmd(1)->stencil == 0xA7 &&
	      !cmd(1)->clear_color);
	/* without stencil bits: no test, nothing cleared */
	struct gl_target none = target;
	none.stencil_bits = 0;
	glf_clear(&st, &none, GL_STENCIL_BUFFER_BIT);
	CHECK_EQ(ncmds(), 2);
	glf_stencil_op(&st, GL_KEEP, 0x1234, GL_KEEP);
	glf_stencil_func(&st, 0x1234, 0, 0);
	CHECK_EQ(error(), GL_INVALID_ENUM);
}

/* ---- the attribute stacks, client arrays, queries ------------------------------------------------- */

static void test_attrib(void)
{
	start();
	glf_bind_texture(&st, GL_TEXTURE_2D, 5);
	glf_push_attrib(&st, 0x40 | 0x2000 | 0x40000);  /* GL_LIGHTING_BIT | GL_ENABLE_BIT | GL_TEXTURE_BIT */
	glf_enable(&st, GL_LIGHTING, true);
	glf_enable(&st, GL_BLEND, true);
	glf_shade_model(&st, GL_FLAT);
	color(0.5f, 0.5f, 0.5f);                        /* (current: not pushed) */
	glf_bind_texture(&st, GL_TEXTURE_2D, 6);
	float linear[1] = { (float)GL_LINEAR };
	glf_tex_parameter(&st, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear);
	glf_bind_texture(&st, GL_TEXTURE_2D, 5);
	glf_tex_parameter(&st, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear);
	glf_bind_texture(&st, GL_TEXTURE_2D, 6);
	CHECK_EQ(st.attrib_depth, 1);
	glf_pop_attrib(&st);
	CHECK(!glf_is_enabled(&st, GL_LIGHTING) && !glf_is_enabled(&st, GL_BLEND) && st.shade_model == GL_SMOOTH);
	CHECK(st.color[0] == 0.5f);
	CHECK(st.units[0].bound[GLF_2D]->name == 5 && st.units[0].bound[GLF_2D]->min_filter == GL_NEAREST_MIPMAP_LINEAR);
	CHECK(glf_texture(&st, 6)->min_filter == GL_LINEAR);    /* (not bound when pushed: as it is) */
	/* the stack's depth */
	for (int k = 0; k < GLF_ATTRIB_DEPTH; k++)
		glf_push_attrib(&st, 0x1);
	CHECK_EQ(error(), GL_NO_ERROR);
	glf_push_attrib(&st, 0x1);
	CHECK_EQ(error(), GL_STACK_OVERFLOW);
	glf_free(&st);
	glf_pop_attrib(&st);
	CHECK_EQ(error(), GL_STACK_UNDERFLOW);
	/* the client's: the arrays as they were */
	glf_array_pointer(&st, GLF_ARRAY_VERTEX, 2, GL_SHORT, 8, 0x1000);
	glf_push_client_attrib(&st, 0x2);
	glf_array_pointer(&st, GLF_ARRAY_VERTEX, 3, GL_FLOAT, 0, 0x2000);
	glf_enable_client(&st, 0x8074, true);
	glf_pop_client_attrib(&st);
	CHECK(st.arrays[GLF_ARRAY_VERTEX].pointer == 0x1000 && st.arrays[GLF_ARRAY_VERTEX].size == 2 &&
	      !st.arrays[GLF_ARRAY_VERTEX].enabled);
	glf_pop_client_attrib(&st);
	CHECK_EQ(error(), GL_STACK_UNDERFLOW);
}

static void put_be(uint8_t *p, uint32_t w)
{
	p[0] = (uint8_t)(w >> 24);
	p[1] = (uint8_t)(w >> 16);
	p[2] = (uint8_t)(w >> 8);
	p[3] = (uint8_t)w;
}

static void put_float(uint8_t *p, float f)
{
	uint32_t w;
	memcpy(&w, &f, 4);
	put_be(p, w);
}

static void test_arrays(void)
{
	start();
	ortho();
	/* positions as floats, colors as unsigned bytes (to 0..1), both big-endian, as the program's memory has them */
	static uint8_t pos[4 * 2 * 4], col[4 * 4];
	static const float xy[4][2] = { { 4.3f, 4.2f }, { 40.6f, 4.2f }, { 4.3f, 40.7f }, { 40.6f, 40.7f } };
	for (int k = 0; k < 4; k++) {
		put_float(pos + 8 * k, xy[k][0]);
		put_float(pos + 8 * k + 4, xy[k][1]);
		col[4 * k] = 255;
		col[4 * k + 1] = (uint8_t)(51 * k);
		col[4 * k + 2] = 0;
		col[4 * k + 3] = 255;
	}
	CHECK(glf_enable_client(&st, 0x8074, true) && glf_enable_client(&st, 0x8076, true));
	CHECK(glf_is_enabled(&st, 0x8074) == 1 && glf_is_enabled(&st, 0x8075) == 0);
	glf_array_pointer(&st, GLF_ARRAY_VERTEX, 2, GL_FLOAT, 0, 0x1000);
	glf_array_pointer(&st, GLF_ARRAY_COLOR, 4, GL_UNSIGNED_BYTE, 0, 0x2000);
	CHECK_EQ(glf_array_bytes(&st.arrays[GLF_ARRAY_VERTEX], 4), 32);
	st.arrays[GLF_ARRAY_VERTEX] = (struct gl_array){ true, 2, 0, GL_FLOAT, 0x1000, pos, 0, 4 };
	st.arrays[GLF_ARRAY_COLOR] = (struct gl_array){ true, 4, 0, GL_UNSIGNED_BYTE, 0x2000, col, 0, 4 };
	glf_draw_arrays(&st, &target, GL_TRIANGLE_STRIP, 0, 4);
	CHECK(ncmds() == 2 && at(&cmd(1)->v[2], 40.6f, 40.7f));
	CHECK(near(cmd(1)->v[2].color[0][1], 0.6f, 1e-6f) && st.color[1] == 0.6f);     /* (and current) */
	/* elements: indices big-endian; one past what's there leaves the vertex out */
	static const uint8_t shorts[6] = { 0, 0, 0, 3, 0, 2 }, bytes[3] = { 0, 2, 9 };
	glf_draw_elements(&st, &target, GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, shorts);
	CHECK(ncmds() == 3 && at(&cmd(2)->v[1], 40.6f, 40.7f));
	glf_draw_elements(&st, &target, GL_TRIANGLES, 3, GL_UNSIGNED_BYTE, bytes);
	CHECK_EQ(ncmds(), 3);
	glf_draw_elements(&st, &target, GL_TRIANGLES, 3, GL_FLOAT, shorts);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	/* interleaved: where each array is, and the stride */
	glf_interleaved_arrays(&st, 0x2A29, 0, 0x4000);         /* GL_T2F_C4UB_V3F */
	CHECK(st.arrays[GLF_ARRAY_TEX0].pointer == 0x4000 && st.arrays[GLF_ARRAY_COLOR].pointer == 0x4008 &&
	      st.arrays[GLF_ARRAY_VERTEX].pointer == 0x400C && st.arrays[GLF_ARRAY_VERTEX].stride == 24 &&
	      st.arrays[GLF_ARRAY_COLOR].type == GL_UNSIGNED_BYTE && !st.arrays[GLF_ARRAY_NORMAL].enabled);
	/* errors */
	glf_array_pointer(&st, GLF_ARRAY_VERTEX, 5, GL_FLOAT, 0, 0);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_array_pointer(&st, GLF_ARRAY_VERTEX, 3, GL_UNSIGNED_BYTE, 0, 0);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	glf_array_pointer(&st, GLF_ARRAY_COLOR, 3, GL_FLOAT, -4, 0);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_unlock_arrays(&st);
	CHECK_EQ(error(), GL_INVALID_OPERATION);
	glf_lock_arrays(&st, 0, 4);
	glf_lock_arrays(&st, 0, 4);
	CHECK_EQ(error(), GL_INVALID_OPERATION);
	double v[4];
	CHECK(glf_get(&st, 0x81A9, v) == 1 && v[0] == 4);       /* GL_ARRAY_ELEMENT_LOCK_COUNT_EXT */
	CHECK(!glf_enable_client(&st, GL_LIGHTING, true));
}

static void test_queries(void)
{
	start();
	double v[16];
	static const struct {
		uint32_t pname;
		unsigned n;
		double first;
	} values[] = {
		{ 0x0B40, 2, GL_FILL },                 /* GL_POLYGON_MODE */
		{ 0x0B92, 1, GL_ALWAYS },               /* GL_STENCIL_FUNC */
		{ 0x0B94, 1, GL_KEEP },                 /* GL_STENCIL_FAIL */
		{ 0x0B98, 1, 4294967295.0 },            /* GL_STENCIL_WRITEMASK */
		{ GL_LIGHT_MODEL_AMBIENT, 4, 0.2f },
		{ GL_FOG_MODE, 1, GL_EXP },
		{ GL_FOG_DENSITY, 1, 1 },
		{ 0x0D31, 1, 8 },                       /* GL_MAX_LIGHTS */
		{ 0x0D32, 1, 6 },                       /* GL_MAX_CLIP_PLANES */
		{ 0x0D50, 1, 8 },                       /* GL_SUBPIXEL_BITS */
		{ 0x0D35, 1, GLF_ATTRIB_DEPTH },        /* GL_MAX_ATTRIB_STACK_DEPTH */
		{ 0x0BB0, 1, 0 },                       /* GL_ATTRIB_STACK_DEPTH */
		{ 0x807A, 1, 4 },                       /* GL_VERTEX_ARRAY_SIZE */
		{ 0x8089, 1, GL_FLOAT },                /* GL_TEXTURE_COORD_ARRAY_TYPE */
		{ 0x0C54, 1, GL_DONT_CARE },            /* GL_FOG_HINT */
		{ 0x0B55, 1, GL_FRONT_AND_BACK },       /* GL_COLOR_MATERIAL_FACE */
		{ 0x0C31, 1, 1 },                       /* GL_RGBA_MODE */
	};
	for (unsigned k = 0; k < sizeof values / sizeof values[0]; k++) {
		unsigned n = glf_get(&st, values[k].pname, v);
		if (n != values[k].n || v[0] != values[k].first)
			fprintf(stderr, "glfront: 0x%04X gives %u values, the first %g\n", values[k].pname, n, v[0]);
		CHECK(n == values[k].n && v[0] == values[k].first);
	}
	glf_enable(&st, GL_LIGHT0 + 3, true);
	CHECK(glf_is_enabled(&st, GL_LIGHT0 + 3) == 1 && glf_is_enabled(&st, GL_LIGHT0 + 2) == 0);
	glf_hint(&st, 0x85B1, GL_NICEST);                       /* GL_TRANSFORM_HINT_APPLE */
	CHECK(glf_get(&st, 0x85B1, v) == 1 && v[0] == GL_NICEST);
	glf_hint(&st, 0x1234, GL_NICEST);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	CHECK_EQ(glf_get(&st, 0x1234, v), 0);
}

/* ---- the raster position, pixel rectangles, the pixel transfer --------------------------------------- */

enum { GL_RGBA8_T = 0x8058, GL_PIXEL_MAP_I_TO_I = 0x0C70, GL_PIXEL_MAP_I_TO_R = 0x0C72, GL_PIXEL_MAP_R_TO_R = 0x0C76,
       GL_MAP_COLOR_T = 0x0D10, GL_INDEX_OFFSET_T = 0x0D13, GL_RED_SCALE_T = 0x0D14, GL_GREEN_BIAS_T = 0x0D19,
       GL_DEPTH_SCALE_T = 0x0D1E, GL_UNPACK_LSB_FIRST_T = 0x0CF1, GL_UNPACK_SKIP_PIXELS_T = 0x0CF4 };

static void raster_pos(float x, float y, float z)
{
	const float v[4] = { x, y, z, 1 };
	glf_raster_pos(&st, v);
}

static uint32_t argb(int r, int g, int b, int a)
{
	return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static void test_raster_pos(void)
{
	start();
	ortho();
	color(0.5f, 0.25f, 1);
	raster_pos(10.25f, 20.5f, 0.5f);
	const struct gl_raster_pos *r = &st.rpos;
	CHECK(r->valid && near(r->win[0], 10.25f, 1e-4f) && near(r->win[1], 20.5f, 1e-4f) && near(r->win[2], 0.25f, 1e-6f));
	CHECK(r->color[1] == 0.25f && near(r->distance, sqrtf(10.25f * 10.25f + 20.5f * 20.5f + 0.25f), 1e-4f));
	double v[4];
	CHECK(glf_get(&st, 0x0B07, v) == 4 && near((float)v[0], 10.25f, 1e-4f) && v[3] == 1);
	/* texture coordinates through the texture matrix */
	glf_matrix_mode(&st, GL_TEXTURE);
	glf_scale(&st, 2, 2, 2);
	glf_matrix_mode(&st, GL_MODELVIEW);
	st.tex[0][0] = 0.25f;
	raster_pos(10, 10, 0);
	CHECK(r->tex[0][0] == 0.5f);
	/* outside the view volume: not valid, and nothing drawn or moved from it */
	raster_pos(-5, 10, 0);
	CHECK(!r->valid);
	CHECK(glf_get(&st, 0x0B08, v) == 1 && v[0] == 0);
	static const uint8_t bits[4] = { 0xFF, 0, 0, 0 };
	glf_bitmap(&st, &target, 8, 1, 0, 0, 5, 5, bits);
	static const uint8_t rgba[4] = { 255, 0, 0, 255 };
	glf_draw_pixels(&st, &target, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	CHECK(ncmds() == 0 && r->win[0] == 10);
	CHECK_EQ(error(), GL_NO_ERROR);
}

static void test_draw_pixels(void)
{
	/* a zoomed image's texel (i, j) covers the pixels whose centers are in its rectangle (OpenGL 1.2's
	 * 3.6.4), from the raster position rounded to a pixel's corner; zoomed backwards, the other way */
	static uint8_t image[4 * 3 * 4];
	for (int k = 0; k < 12; k++) {
		image[4 * k] = (uint8_t)(20 * k + 10);
		image[4 * k + 1] = (uint8_t)k;
		image[4 * k + 2] = 0;
		image[4 * k + 3] = 255;
	}
	static const float zooms[2][2] = { { 1.6f, -1.7f }, { -1.5f, 1.3f } };
	static const unsigned covering[2] = { 6 * 5, 6 * 4 };
	for (int n = 0; n < 2; n++) {
		start();
		ortho();
		const float *z = zooms[n];
		glf_pixel_zoom(&st, z[0], z[1]);
		raster_pos(10.3f, 30.4f, 0);
		glf_draw_pixels(&st, &target, 4, 3, GL_RGBA, GL_UNSIGNED_BYTE, image);
		drawn();
		unsigned wrong = 0, covered = 0;
		for (int y = 18; y < 40; y++)
			for (int x = 0; x < 24; x++) {
				double i = floor((x + 0.5 - 10) / z[0]), j = floor((y + 0.5 - 30) / z[1]);
				bool in = i >= 0 && i < 4 && j >= 0 && j < 3;
				uint32_t want = in ? argb(20 * (int)(j * 4 + i) + 10, (int)(j * 4 + i), 0, 255) : 0;
				wrong += pixel(x, y) != want;
				covered += in;
			}
		CHECK(wrong == 0 && covered == covering[n]);
	}
	glf_pixel_zoom(&st, 1, 1);
	/* the transfer: scale and bias, then the maps (each component through its own) */
	start();
	ortho();
	float flip[2] = { 1, 0 }, ident[256], blue[4] = { 0.1f, 0.2f, 0.4f, 0.8f }, one[1] = { 1 };
	for (int k = 0; k < 256; k++)
		ident[k] = (float)k / 255;
	glf_pixel_map(&st, GL_PIXEL_MAP_R_TO_R, 2, flip);
	glf_pixel_map(&st, GL_PIXEL_MAP_R_TO_R + 1, 256, ident);
	glf_pixel_map(&st, GL_PIXEL_MAP_R_TO_R + 2, 4, blue);
	glf_pixel_map(&st, GL_PIXEL_MAP_R_TO_R + 3, 1, one);
	glf_pixel_transfer(&st, GL_MAP_COLOR_T, 1);
	glf_pixel_transfer(&st, GL_GREEN_BIAS_T, 0.25f);
	static const uint8_t px[4] = { 100, 37, 200, 0 };
	raster_pos(4.2f, 4.4f, 0);                              /* (drawn from (4, 4): rounded) */
	glf_draw_pixels(&st, &target, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	drawn();
	CHECK_EQ(pixel(4, 4), argb(255, 101, 102, 255));       /* (r 0.39: 1; g 37 + 64; b 0.78 of 3: 2) */
	float got[256];
	CHECK(glf_get_pixel_map(&st, GL_PIXEL_MAP_R_TO_R + 2, got) == 4 && got[3] == 0.8f);
	glf_pixel_map(&st, GL_PIXEL_MAP_I_TO_I, 3, ident);      /* (from indices: powers of two) */
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_pixel_map(&st, 0x0C7A, 1, ident);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	/* color indices: shifted and offset, then by the I_TO_ maps */
	glf_pixels_init(&st);
	const float ir[2] = { 0, 1 }, ig[2] = { 1, 0 }, ia[1] = { 1 };
	glf_pixel_map(&st, GL_PIXEL_MAP_I_TO_R, 2, ir);
	glf_pixel_map(&st, GL_PIXEL_MAP_I_TO_R + 1, 2, ig);
	glf_pixel_map(&st, GL_PIXEL_MAP_I_TO_R + 3, 1, ia);
	glf_pixel_transfer(&st, GL_INDEX_OFFSET_T, 1);
	static const uint8_t index[2] = { 0, 1 };
	raster_pos(10.4f, 3.6f, 0);
	glf_draw_pixels(&st, &target, 2, 1, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, index);
	drawn();
	CHECK(pixel(10, 4) == argb(255, 0, 0, 255) && pixel(11, 4) == argb(0, 255, 0, 255));
	glf_pixels_init(&st);
	/* errors */
	glf_draw_pixels(&st, &target, 1, 1, 0x1234, GL_UNSIGNED_BYTE, px);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	glf_draw_pixels(&st, &target, 1, 1, GL_RGB, 0x8033, px);        /* (4_4_4_4: four components) */
	CHECK_EQ(error(), GL_INVALID_OPERATION);
	glf_draw_pixels(&st, &target, -1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	struct gl_target none = target;
	none.depth_bits = 0;
	glf_draw_pixels(&st, &none, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, px);
	CHECK_EQ(error(), GL_INVALID_OPERATION);
	glf_draw_pixels(&st, &target, 1, 1, GL_RGBA, GL_BITMAP, px);
	CHECK_EQ(error(), GL_INVALID_ENUM);
}

static void test_bitmap(void)
{
	start();
	ortho();
	/* the raster color is the color when the raster position was set; least significant bits first, skipped */
	color(1, 0, 0);
	raster_pos(20.7f, 10.2f, 0);
	color(0, 1, 0);
	glf_pixel_store(&st, GL_UNPACK_LSB_FIRST_T, 1);
	glf_pixel_store(&st, GL_UNPACK_SKIP_PIXELS_T, 2);
	glf_pixel_store(&st, 0x0CF5, 1);                        /* GL_UNPACK_ALIGNMENT */
	static const uint8_t bits[2] = { 0x08, 0x10 };         /* (bits 2 on: 0, 1, 0; 0, 0, 1) */
	glf_bitmap(&st, &target, 3, 2, 1, 0, 4.5f, -1, bits);
	drawn();
	/* (its corner: floor(20.7 - 1), floor(10.2)) */
	CHECK(pixel(19, 10) == 0 && pixel(20, 10) == argb(255, 0, 0, 255) && pixel(21, 10) == 0);
	CHECK(pixel(19, 11) == 0 && pixel(20, 11) == 0 && pixel(21, 11) == argb(255, 0, 0, 255));
	CHECK(near(st.rpos.win[0], 25.2f, 1e-5f) && near(st.rpos.win[1], 9.2f, 1e-5f));
	glf_bitmap(&st, &target, 0, 0, 0, 0, 1, 1, NULL);       /* (nothing drawn: it still moves) */
	CHECK(near(st.rpos.win[0], 26.2f, 1e-5f) && ncmds() == 0);
	glf_bitmap(&st, &target, -1, 0, 0, 0, 1, 1, NULL);
	CHECK_EQ(error(), GL_INVALID_VALUE);
}

static void test_depth_stencil_pixels(void)
{
	start();
	ortho();
	st.clear_depth = 0.25;
	st.clear_stencil = 7;
	glf_clear(&st, &target, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	drawn();
	/* read: depth as floats and scaled shorts, through the depth scale; stencil through the offset, as bits */
	uint8_t d[8];
	glf_read_pixels(&st, &target, 3, 3, 2, 1, GL_DEPTH_COMPONENT, GL_FLOAT, d);
	CHECK(d[0] == 0x3E && d[1] == 0x80 && d[2] == 0 && d[4] == 0x3E);      /* (0.25, big-endian) */
	uint8_t be[2];
	glf_pixel_transfer(&st, GL_DEPTH_SCALE_T, 2);
	glf_read_pixels(&st, &target, 3, 3, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, be);
	CHECK(be[0] == 0x80 && be[1] == 0x00);                  /* (0.5 of 65535, big-endian) */
	glf_pixel_transfer(&st, GL_DEPTH_SCALE_T, 1);
	uint8_t s[2];
	glf_pixel_transfer(&st, GL_INDEX_OFFSET_T, 1);
	glf_read_pixels(&st, &target, 3, 3, 2, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, s);
	CHECK(s[0] == 8 && s[1] == 8);
	glf_pixel_transfer(&st, GL_INDEX_OFFSET_T, 0);
	uint8_t bit = 0xFF;
	glf_read_pixels(&st, &target, 3, 3, 2, 1, GL_STENCIL_INDEX, GL_BITMAP, &bit);
	CHECK_EQ(bit, 0xFF);                                    /* (7's low bit, twice: the rest left) */
	glf_read_pixels(&st, &target, 3, 3, 1, 1, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, s);
	CHECK_EQ(error(), GL_INVALID_OPERATION);
	/* drawn: depth (with the depth test: written, its color the raster's), stencil in the write mask */
	glf_enable(&st, GL_DEPTH_TEST, true);
	glf_depth_func(&st, GL_ALWAYS);
	color(0, 0, 1);
	raster_pos(10.2f, 10.3f, 0);
	const float depths[2] = { 0.5f, 0.75f };
	uint8_t dbe[8];
	for (int k = 0; k < 2; k++) {
		uint32_t w;
		memcpy(&w, &depths[k], 4);
		for (int b = 0; b < 4; b++)
			dbe[4 * k + b] = (uint8_t)(w >> (24 - 8 * b));
	}
	glf_draw_pixels(&st, &target, 2, 1, GL_DEPTH_COMPONENT, GL_FLOAT, dbe);
	glf_stencil_mask(&st, 0x0F);
	static const uint8_t stencil[2] = { 0x35, 0x3A };
	glf_draw_pixels(&st, &target, 2, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencil);
	drawn();
	size_t i = (size_t)(H - 1 - 10) * W + 10;
	CHECK(surf.target.depth[i] == 0.5f && surf.target.depth[i + 1] == 0.75f && pixel(10, 10) == argb(0, 0, 255, 255));
	CHECK(surf.target.stencil[i] == 0x05 && surf.target.stencil[i + 1] == 0x0A);
	/* copied */
	raster_pos(19.8f, 20.1f, 0);
	glf_copy_pixels(&st, &target, 10, 10, 2, 1, GL_DEPTH);
	glf_stencil_mask(&st, 0xFF);
	glf_copy_pixels(&st, &target, 10, 10, 2, 1, GL_STENCIL);
	drawn();
	size_t j = (size_t)(H - 1 - 20) * W + 20;
	CHECK(surf.target.depth[j] == 0.5f && surf.target.depth[j + 1] == 0.75f && surf.target.stencil[j] == 0x05);
	glf_enable(&st, GL_DEPTH_TEST, false);                  /* (a color copy's fragments have the raster's z) */
	glf_copy_pixels(&st, &target, 10, 10, 2, 1, GL_COLOR);
	drawn();
	CHECK(pixel(21, 20) == argb(0, 0, 255, 255) && surf.target.depth[j + 1] == 0.75f);
	glf_copy_pixels(&st, &target, 0, 0, 1, 1, 0x1234);
	CHECK_EQ(error(), GL_INVALID_ENUM);
}

static void test_copy_get_tex_image(void)
{
	start();
	ortho();
	static const uint8_t px[4] = { 10, 20, 30, 255 };
	for (int k = 0; k < 4; k++) {
		raster_pos(2.2f + (float)k, 3.3f, 0);
		glf_draw_pixels(&st, &target, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	}
	drawn();
	glf_bind_texture(&st, GL_TEXTURE_2D, 1);
	glf_copy_tex_image(&st, &target, GL_TEXTURE_2D, 0, GL_RGBA8_T, 1, 2, 4, 2, 0);
	CHECK_EQ(error(), GL_NO_ERROR);
	const struct gl_texture *t = glf_texture(&st, 1);
	CHECK(t->level[0].width == 4 && t->level[0].height == 2);
	CHECK(t->level[0].texels[4 + 1] == argb(10, 20, 30, 255) && t->level[0].texels[0] == 0);
	glf_copy_tex_sub_image(&st, &target, GL_TEXTURE_2D, 0, 0, 0, 2, 3, 1, 1);
	CHECK_EQ(t->level[0].texels[0], argb(10, 20, 30, 255));
	glf_copy_tex_image(&st, &target, GL_TEXTURE_2D, 0, GL_RGBA8_T, 0, 0, 3, 2, 0);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	/* glGetTexImage: a luminance texture as RGBA (L, 0, 0, 1) and as luminance */
	static const uint8_t lum[4] = { 9, 99, 199, 255 };
	glf_pixel_store(&st, 0x0CF5, 1);                        /* GL_UNPACK_ALIGNMENT */
	glf_pixel_store(&st, 0x0D05, 1);                        /* GL_PACK_ALIGNMENT */
	glf_tex_image(&st, GL_TEXTURE_2D, 0, GL_LUMINANCE, 2, 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
	CHECK_EQ(glf_tex_image_bytes(&st, GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE), 16);
	uint8_t out[16];
	glf_get_tex_image(&st, GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
	CHECK(out[4] == 99 && out[5] == 0 && out[6] == 0 && out[7] == 255);
	glf_get_tex_image(&st, GL_TEXTURE_2D, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, out);
	CHECK(out[0] == 9 && out[1] == 99 && out[2] == 199 && out[3] == 255);
	glf_get_tex_image(&st, GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, out);
	CHECK_EQ(error(), GL_INVALID_ENUM);
}

/* ---- selection and feedback --------------------------------------------------------------- */

static int32_t render_mode(uint32_t mode, const uint32_t **words, size_t *count)
{
	uint32_t buffer;
	return glf_render_mode(&st, mode, &buffer, words, count);
}

static float word_float(uint32_t w)
{
	float f;
	memcpy(&f, &w, 4);
	return f;
}

static void test_select_feedback(void)
{
	start();
	ortho();
	const uint32_t *w;
	size_t n;
	/* errors: a mode without its buffer, a bad mode or type, a buffer changed in its mode; the name stack's */
	CHECK_EQ(render_mode(GL_SELECT, &w, &n), 0);
	CHECK_EQ(error(), GL_INVALID_OPERATION);
	render_mode(0x1234, &w, &n);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	glf_feedback_buffer(&st, 10, 0x0605, 0x1000);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	glf_select_buffer(&st, 64, 0x2000);
	glf_load_name(&st, 1);                  /* (rendering: name stack calls ignored) */
	CHECK_EQ(error(), GL_NO_ERROR);
	CHECK_EQ(render_mode(GL_SELECT, &w, &n), 0);
	double v[4];
	CHECK(glf_get(&st, 0x0C40, v) == 1 && v[0] == GL_SELECT);
	glf_select_buffer(&st, 64, 0x2000);
	CHECK_EQ(error(), GL_INVALID_OPERATION);
	glf_load_name(&st, 1);
	CHECK_EQ(error(), GL_INVALID_OPERATION);
	glf_pop_name(&st);
	CHECK_EQ(error(), GL_STACK_UNDERFLOW);
	for (unsigned k = 0; k <= GLF_NAME_STACK; k++)
		glf_push_name(&st, k);
	CHECK_EQ(error(), GL_STACK_OVERFLOW);
	CHECK(glf_get(&st, 0x0D70, v) == 1 && v[0] == GLF_NAME_STACK);
	glf_init_names(&st);
	/* selecting: nothing drawn or cleared; a hit's record when the names change, and as the mode's left */
	glf_push_name(&st, 7);
	glf_begin(&st, GL_TRIANGLES);
	vertex(10, 10, 0.5f);
	vertex(20, 10, -0.5f);
	vertex(15, 20, 0);
	glf_end(&st, &target);
	glf_clear(&st, &target, GL_COLOR_BUFFER_BIT);
	CHECK_EQ(ncmds(), 0);
	glf_push_name(&st, 8);                  /* (a record: one name; nothing hit since: none) */
	glf_push_name(&st, 9);
	glf_raster_pos(&st, (const float[4]){ 30, 30, 0, 1 });
	glf_bitmap(&st, &target, 0, 0, 0, 0, 2, 0, NULL);
	CHECK_EQ(render_mode(GL_RENDER, &w, &n), 2);
	CHECK(n == 4 + 6 && w[0] == 1 && w[3] == 7 && w[4] == 3 && w[7] == 7 && w[8] == 8 && w[9] == 9);
	CHECK(w[1] == 0x3FFFFFFFu && w[2] == 0xBFFFFFFFu);      /* (depths 0.25 and 0.75, of 2^32 - 1) */
	CHECK(w[5] == 0x7FFFFFFFu && w[6] == 0x7FFFFFFFu);
	CHECK(near(st.rpos.win[0], 32, 1e-5f));                 /* (bitmaps still move it) */
	CHECK(glf_get(&st, 0x0D70, v) == 1 && v[0] == 0);       /* (the stack emptied) */
	CHECK(glf_get(&st, 0x0DF4, v) == 1 && v[0] == 64);
	uint32_t p;
	CHECK(glf_get_pointer(&st, 0x0DF3, &p) && p == 0x2000);
	/* overflow: -1, and what fitted */
	glf_select_buffer(&st, 2, 0x2000);
	render_mode(GL_SELECT, &w, &n);
	glf_push_name(&st, 1);
	glf_begin(&st, GL_POINTS);
	vertex(5, 5, 0);
	glf_end(&st, &target);
	CHECK(render_mode(GL_RENDER, &w, &n) == -1 && n == 2);

	/* feedback (OpenGL 1.2's 5.3, where Mesa's departs): lines reset the stipple at each strip's start
	 * and each separate line, polygons outlined at each polygon; flat shading's colors */
	glf_feedback_buffer(&st, 256, 0x0602, 0x3000);           /* GL_3D_COLOR */
	CHECK_EQ(render_mode(GL_FEEDBACK, &w, &n), 0);
	glf_begin(&st, GL_LINE_STRIP);
	vertex(1, 1, 0);
	vertex(5, 1, 0);
	vertex(5, 5, 0);
	glf_end(&st, &target);
	glf_begin(&st, GL_LINES);
	for (int k = 0; k < 4; k++)
		vertex(10 + (float)k, 2, 0);
	glf_end(&st, &target);
	glf_polygon_mode(&st, GL_FRONT_AND_BACK, GL_LINE);
	glf_begin(&st, GL_QUADS);
	vertex(20, 20, 0);
	vertex(30, 20, 0);
	vertex(30, 30, 0);
	vertex(20, 30, 0);
	glf_end(&st, &target);
	glf_polygon_mode(&st, GL_FRONT_AND_BACK, GL_FILL);
	glf_shade_model(&st, GL_FLAT);
	glf_begin(&st, GL_TRIANGLES);
	color(1, 0, 0);
	vertex(40, 40, 0);
	color(0, 1, 0);
	vertex(50, 40, 0);
	color(0, 0, 1);
	vertex(45, 46, 0);
	glf_end(&st, &target);
	glf_shade_model(&st, GL_SMOOTH);
	glf_pass_through(&st, 2.5f);
	int32_t values = render_mode(GL_RENDER, &w, &n);
	CHECK(values > 0 && (size_t)values == n && ncmds() == 0);
	uint32_t tokens[16];
	unsigned nt = 0;
	for (size_t i = 0; i < n && nt < 16;) {            /* (3D_COLOR: 7 values a vertex) */
		uint32_t tok = (uint32_t)word_float(w[i]);
		tokens[nt++] = tok;
		if (tok == GL_POLYGON_TOKEN) {
			CHECK_EQ(word_float(w[i + 1]), 3);
			for (int k = 0; k < 3; k++)             /* (each the last vertex's color) */
				CHECK(word_float(w[i + 2 + 7 * k + 3]) == 0 && word_float(w[i + 2 + 7 * k + 5]) == 1);
			i += 2 + 21;
		} else if (tok == GL_PASS_THROUGH_TOKEN) {
			CHECK_EQ(word_float(w[i + 1]), 2.5f);
			i += 2;
		} else {
			i += 1 + 14;
		}
	}
	static const uint32_t want[] = { GL_LINE_RESET_TOKEN, GL_LINE_TOKEN, GL_LINE_RESET_TOKEN, GL_LINE_RESET_TOKEN,
					 GL_LINE_RESET_TOKEN, GL_LINE_TOKEN, GL_LINE_TOKEN, GL_LINE_TOKEN,
					 GL_POLYGON_TOKEN, GL_PASS_THROUGH_TOKEN };
	CHECK_EQ(nt, sizeof want / sizeof want[0]);
	for (unsigned k = 0; k < nt && k < sizeof want / sizeof want[0]; k++)
		CHECK_EQ(tokens[k], want[k]);
	CHECK_EQ(error(), GL_NO_ERROR);
}

/* ---- evaluators -------------------------------------------------------------------------- */

enum { MAP1_COLOR_4 = 0x0D90, MAP1_NORMAL = 0x0D92, MAP1_VERTEX_3 = 0x0D97, MAP1_VERTEX_4, MAP2_COLOR_4 = 0x0DB0,
       MAP2_VERTEX_3 = 0x0DB7, AUTO_NORMAL = 0x0D80, Q_COEFF = 0x0A00, Q_ORDER, Q_DOMAIN, EVAL_BIT = 0x10000,
       FEEDBACK_3D_COLOR = 0x0602 };

/* What's fed back (GL_3D_COLOR) of points drawn by draw(): x, y, z, r, g, b, a each into p; how many */
static unsigned fed_back_points(void (*draw)(void), float p[][7], unsigned max)
{
	const uint32_t *w;
	size_t n;
	glf_feedback_buffer(&st, 1024, FEEDBACK_3D_COLOR, 0x3000);
	render_mode(GL_FEEDBACK, &w, &n);
	draw();
	render_mode(GL_RENDER, &w, &n);
	unsigned points = 0;
	for (size_t i = 0; i + 8 <= n && points < max; i += 8) {
		CHECK_EQ(word_float(w[i]), GL_POINT_TOKEN);
		for (int k = 0; k < 7; k++)
			p[points][k] = word_float(w[i + 1 + (size_t)k]);
		points++;
	}
	return points;
}

static void eval_curve(void)
{
	glf_begin(&st, GL_POINTS);
	glf_eval_coord(&st, &target, 1, 3, 0);
	glf_end(&st, &target);
	glf_eval_mesh(&st, &target, 1, GL_POINT, 0, 3, 0, 0);
}

static void eval_patch(void)
{
	glf_begin(&st, GL_POINTS);
	glf_eval_coord(&st, &target, 2, 0.5f, 0.5f);
	glf_end(&st, &target);
}

static void test_evaluators(void)
{
	start();
	ortho();
	double v[GLF_EVAL_ORDER * GLF_EVAL_ORDER * 4];
	static const double unit[4] = { 0, 1, 0, 1 };
	/* the maps and grids at first */
	CHECK(glf_get_map(&st, MAP1_VERTEX_3, Q_ORDER, v) == 1 && v[0] == 1);
	CHECK(glf_get_map(&st, MAP2_COLOR_4, Q_DOMAIN, v) == 4 && v[0] == 0 && v[1] == 1 && v[2] == 0 && v[3] == 1);
	CHECK(glf_get_map(&st, MAP2_COLOR_4, Q_COEFF, v) == 4 && v[0] == 1 && v[1] == 1 && v[2] == 1 && v[3] == 1);
	CHECK(glf_get_map(&st, MAP1_NORMAL, Q_COEFF, v) == 3 && v[0] == 0 && v[1] == 0 && v[2] == 1);
	CHECK(glf_get_map(&st, MAP1_VERTEX_4, Q_COEFF, v) == 4 && v[0] == 0 && v[2] == 0 && v[3] == 1);
	CHECK(glf_get(&st, 0x0DD0, v) == 2 && v[0] == 0 && v[1] == 1);         /* GL_MAP1_GRID_DOMAIN */
	CHECK(glf_get(&st, 0x0DD3, v) == 2 && v[0] == 1 && v[1] == 1);         /* GL_MAP2_GRID_SEGMENTS */
	CHECK(glf_get(&st, 0x0D30, v) == 1 && v[0] == GLF_EVAL_ORDER);
	CHECK_EQ(error(), GL_NO_ERROR);

	/* errors (the map as it was) */
	static const float none[GLF_EVAL_ORDER * GLF_EVAL_ORDER * 4];
	glf_map(&st, 1, MAP2_VERTEX_3, unit, 3, 2, 0, 1, none);                /* (a 2D map's target to glMap1) */
	CHECK_EQ(error(), GL_INVALID_ENUM);
	glf_map(&st, 1, 0x0D99, unit, 3, 2, 0, 1, none);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	glf_map(&st, 1, MAP1_VERTEX_3, (const double[4]){ 2, 2, 0, 1 }, 3, 2, 0, 1, none);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_map(&st, 2, MAP2_VERTEX_3, (const double[4]){ 0, 1, 3, 3 }, 3, 2, 6, 2, none);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_map(&st, 1, MAP1_VERTEX_3, unit, 3, 0, 0, 1, none);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_map(&st, 1, MAP1_VERTEX_3, unit, 3, GLF_EVAL_ORDER + 1, 0, 1, none);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_map(&st, 1, MAP1_VERTEX_3, unit, 2, 2, 0, 1, none);                /* (strides below the components) */
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_map(&st, 2, MAP2_VERTEX_3, unit, 3, 2, 2, 2, none);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_active_texture(&st, GL_TEXTURE0_ARB + 1);                           /* (as Mesa: unit 0's) */
	glf_map(&st, 1, MAP1_VERTEX_3, unit, 3, 2, 0, 1, none);
	CHECK_EQ(error(), GL_INVALID_OPERATION);
	glf_active_texture(&st, GL_TEXTURE0_ARB);
	glf_map_grid1(&st, 0, 0, 1);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_map_grid2(&st, 1, 0, 1, 0, 0, 1);
	CHECK_EQ(error(), GL_INVALID_VALUE);
	glf_eval_mesh(&st, &target, 1, GL_FILL, 0, 1, 0, 0);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	glf_eval_mesh(&st, &target, 2, 0x1B03, 0, 1, 0, 1);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	CHECK_EQ(glf_get_map(&st, MAP1_VERTEX_3, 0x0A03, v), 0);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	CHECK(glf_get_map(&st, MAP1_VERTEX_3, Q_ORDER, v) == 1 && v[0] == 1);
	CHECK(glf_get_map(&st, MAP2_VERTEX_3, Q_ORDER, v) == 2 && v[0] == 1 && v[1] == 1);

	/* a 2D map's control points as packed ([v][u]): glGetMap gives them u by u along each v (its page's order) */
	float packed[2 * 3 * 3];
	for (int k = 0; k < 18; k++)
		packed[k] = (float)(100 * (k / 9) + 10 * (k / 3 % 3) + k % 3);
	glf_map(&st, 2, MAP2_VERTEX_3, (const double[4]){ 0, 1, 2, 4 }, 3, 3, 9, 2, packed);
	CHECK_EQ(error(), GL_NO_ERROR);
	CHECK(glf_get_map(&st, MAP2_VERTEX_3, Q_ORDER, v) == 2 && v[0] == 3 && v[1] == 2);
	CHECK(glf_get_map(&st, MAP2_VERTEX_3, Q_DOMAIN, v) == 4 && v[1] == 1 && v[2] == 2 && v[3] == 4);
	CHECK_EQ(glf_get_map(&st, MAP2_VERTEX_3, Q_COEFF, v), 18);
	unsigned wrong = 0;
	for (int k = 0; k < 18; k++)
		wrong += v[k] != packed[k];
	CHECK_EQ(wrong, 0);

	/*
	 * A quadratic curve (domain 2 to 4), colored by a map: at u = 3, the
	 * points' (1/4, 1/2, 1/4) blend; a grid's last point u2's own; the
	 * current color, normal and texture coordinates as they were
	 */
	static const float curve[3 * 3] = { 10, 10, 0, 30, 40, 0, 50, 10, 0 }, ramp[2 * 4] = { 1, 0, 0, 1, 0, 0, 1, 1 };
	glf_map(&st, 1, MAP1_VERTEX_3, (const double[4]){ 2, 4 }, 3, 3, 0, 1, curve);
	glf_map(&st, 1, MAP1_COLOR_4, (const double[4]){ 2, 4 }, 4, 2, 0, 1, ramp);
	glf_enable(&st, MAP1_VERTEX_3, true);
	glf_enable(&st, MAP1_COLOR_4, true);
	glf_map_grid1(&st, 3, 2, 4);
	color(0.2f, 0.4f, 0.6f);
	static const float normal[3] = { 0.1f, 0.2f, 0.3f }, tex[4] = { 5, 6, 7, 8 };
	memcpy(st.normal, normal, sizeof normal);
	memcpy(st.tex[0], tex, sizeof tex);
	float p[8][7];
	CHECK_EQ(fed_back_points(eval_curve, p, 8), 5);
	CHECK(near(p[0][0], 30, 1e-4f) && near(p[0][1], 25, 1e-4f));
	CHECK(near(p[0][3], 0.5f, 1e-6f) && p[0][4] == 0 && near(p[0][5], 0.5f, 1e-6f) && p[0][6] == 1);
	CHECK(near(p[2][0], 210.0f / 9, 1e-4f) && near(p[2][1], 210.0f / 9, 1e-4f));
	CHECK(p[1][0] == 10 && p[1][1] == 10 && p[4][0] == 50 && p[4][1] == 10 && p[4][5] == 1);
	CHECK(st.color[0] == 0.2f && st.color[1] == 0.4f && st.color[2] == 0.6f && st.color[3] == 1);
	CHECK(!memcmp(st.normal, normal, sizeof normal) && !memcmp(st.tex[0], tex, sizeof tex));
	glf_enable(&st, MAP1_VERTEX_3, false);                                  /* (no vertex map: nothing) */
	CHECK_EQ(fed_back_points(eval_curve, p, 8), 0);
	glf_enable(&st, MAP1_VERTEX_4, true);                   /* (the 4D map: (0, 0, 0, 1) at first) */
	CHECK(fed_back_points(eval_curve, p, 8) == 5 && p[4][0] == 0 && p[4][1] == 0);
	glf_enable(&st, MAP1_VERTEX_4, false);
	glf_enable(&st, MAP1_COLOR_4, false);

	/*
	 * GL_AUTO_NORMAL: dp/du x dp/dv, normalized: lit from (0, 0, 1), a flat
	 * patch facing it is bright, and dark with its u domain run backwards
	 * (that turns dp/du round: Mesa's derivatives are in u', and don't)
	 */
	static const float flat[2 * 2 * 3] = { 0, 0, 0, 40, 0, 0, 0, 40, 0, 40, 40, 0 };
	static const float toward[4] = { 0, 0, 1, 0 }, white[4] = { 1, 1, 1, 1 }, black[4] = { 0, 0, 0, 1 };
	glf_enable(&st, GL_LIGHTING, true);
	glf_enable(&st, GL_LIGHT0, true);
	glf_light(&st, GL_LIGHT0, GL_POSITION, toward);
	glf_material(&st, GL_FRONT_AND_BACK, GL_DIFFUSE, white);
	glf_material(&st, GL_FRONT_AND_BACK, GL_AMBIENT, black);
	glf_enable(&st, MAP2_VERTEX_3, true);
	glf_enable(&st, AUTO_NORMAL, true);
	glf_map(&st, 2, MAP2_VERTEX_3, unit, 3, 2, 6, 2, flat);
	CHECK(fed_back_points(eval_patch, p, 8) == 1 && near(p[0][0], 20, 1e-4f) && p[0][3] > 0.99f);
	glf_map(&st, 2, MAP2_VERTEX_3, (const double[4]){ 1, 0, 0, 1 }, 3, 2, 6, 2, flat);
	CHECK(fed_back_points(eval_patch, p, 8) == 1 && near(p[0][0], 20, 1e-4f) && p[0][3] < 0.01f);
	glf_enable(&st, GL_LIGHTING, false);
	CHECK(!memcmp(st.normal, normal, sizeof normal));

	/* a filled mesh is quad strips (5.1's): outlined, each quad's four edges, no diagonals */
	const uint32_t *w;
	size_t n;
	glf_enable(&st, AUTO_NORMAL, false);
	glf_map(&st, 2, MAP2_VERTEX_3, unit, 3, 2, 6, 2, flat);
	glf_map_grid2(&st, 2, 0, 1, 1, 0, 1);
	glf_polygon_mode(&st, GL_FRONT_AND_BACK, GL_LINE);
	glf_feedback_buffer(&st, 1024, 0x0600, 0x3000);         /* GL_2D */
	render_mode(GL_FEEDBACK, &w, &n);
	glf_eval_mesh(&st, &target, 2, GL_FILL, 0, 2, 0, 1);
	render_mode(GL_RENDER, &w, &n);
	glf_polygon_mode(&st, GL_FRONT_AND_BACK, GL_FILL);
	unsigned lines = 0;
	for (size_t i = 0; i + 5 <= n; i += 5) {
		uint32_t tok = (uint32_t)word_float(w[i]);
		lines += tok == GL_LINE_TOKEN || tok == GL_LINE_RESET_TOKEN;
	}
	CHECK(lines == 8 && n == 8 * 5);

	/* GL_EVAL_BIT: the grids and the enables as they were, not the maps */
	glf_enable(&st, AUTO_NORMAL, true);
	glf_push_attrib(&st, EVAL_BIT);
	glf_map_grid2(&st, 5, -1, 2, 7, 3, 4);
	glf_enable(&st, AUTO_NORMAL, false);
	glf_enable(&st, MAP2_COLOR_4, true);
	glf_map(&st, 2, MAP2_VERTEX_3, unit, 3, 2, 6, 2, flat);
	glf_pop_attrib(&st);
	CHECK(glf_get(&st, 0x0DD2, v) == 4 && v[0] == 0 && v[1] == 1);         /* GL_MAP2_GRID_DOMAIN */
	CHECK(glf_get(&st, 0x0DD3, v) == 2 && v[0] == 2 && v[1] == 1);         /* GL_MAP2_GRID_SEGMENTS */
	CHECK(glf_get(&st, 0x0DD1, v) == 1 && v[0] == 3);                      /* GL_MAP1_GRID_SEGMENTS */
	CHECK(glf_is_enabled(&st, AUTO_NORMAL) == 1 && glf_is_enabled(&st, MAP2_COLOR_4) == 0);
	CHECK(glf_get_map(&st, MAP2_VERTEX_3, Q_DOMAIN, v) == 4 && v[0] == 0 && v[1] == 1);
	CHECK_EQ(error(), GL_NO_ERROR);
}

/*
 * Line stipple through glBegin and glEnd: a strip's counter going on from
 * segment to segment, separate lines starting it again; the polygon stipple
 * through the pixel store (packed back the same); logic ops' state
 */
static void test_stipple_logic(void)
{
	start();
	ortho();
	glf_clear(&st, &target, GL_COLOR_BUFFER_BIT);
	glf_enable(&st, GL_LINE_STIPPLE, true);
	glf_line_stipple(&st, 1, 0x00FF);
	color(1, 1, 1);
	glf_begin(&st, GL_LINE_STRIP);          /* (row 2: 10 pixels, then 10 more) */
	vertex(1.5f, 2.5f, 0);
	vertex(11.5f, 2.5f, 0);
	vertex(21.5f, 2.5f, 0);
	glf_end(&st, &target);
	glf_begin(&st, GL_LINES);               /* (row 5: two of 10, each from bit 0) */
	vertex(1.5f, 5.5f, 0);
	vertex(11.5f, 5.5f, 0);
	vertex(21.5f, 5.5f, 0);
	vertex(31.5f, 5.5f, 0);
	glf_end(&st, &target);
	drawn();
	for (int x = 1; x < 21; x++)
		CHECK_EQ(pixel(x, 2) != 0, x - 1 < 8 || x - 1 >= 16);
	for (int x = 1; x < 31; x++)            /* (1 to 10, 21 to 30: the second from bit 0 again) */
		CHECK_EQ(pixel(x, 5) != 0, (x <= 10 || x >= 21) && (x - 1) % 10 < 8);
	glf_enable(&st, GL_LINE_STIPPLE, false);

	/* the polygon stipple: rows of bits from the bottom, each unpacked as glBitmap's (here LSB first, rows of 8) */
	uint8_t bits[32 * 8], back[32 * 8];
	for (int k = 0; k < 32 * 8; k++)
		bits[k] = (uint8_t)(k * 37 + 11);
	glf_pixel_store(&st, 0x0CF1, 1);                        /* GL_UNPACK_LSB_FIRST */
	glf_pixel_store(&st, 0x0CF5, 8);                        /* GL_UNPACK_ALIGNMENT */
	CHECK_EQ(glf_polygon_stipple_bytes(&st.unpack), 32 * 8);
	glf_polygon_stipple(&st, bits);
	CHECK_EQ(st.polygon_stipple[1] >> 31, (uint32_t)(bits[8] & 1));
	CHECK_EQ(st.polygon_stipple[1] >> 24 & 1, (uint32_t)(bits[8] >> 7 & 1));
	glf_pixel_store(&st, 0x0D01, 1);                        /* GL_PACK_LSB_FIRST */
	glf_pixel_store(&st, 0x0D05, 8);
	memset(back, 0, sizeof back);
	glf_get_polygon_stipple(&st, back);
	for (int j = 0; j < 32; j++)
		CHECK(!memcmp(back + 8 * j, bits + 8 * j, 4));
	glf_pixel_store(&st, 0x0CF1, 0);
	glf_pixel_store(&st, 0x0CF5, 4);
	glf_pixel_store(&st, 0x0D01, 0);
	glf_pixel_store(&st, 0x0D05, 4);

	/* logic ops: the mode, its errors, pushed and popped with the color buffer's bits */
	double v[4];
	CHECK_EQ(glf_get(&st, 0x0BF0, v), 1);                   /* GL_LOGIC_OP_MODE */
	CHECK_EQ(v[0], GL_COPY);
	glf_logic_op(&st, 0x1506);                              /* GL_XOR */
	glf_logic_op(&st, 0x1510);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	glf_push_attrib(&st, GL_COLOR_BUFFER_BIT);
	glf_logic_op(&st, GL_SET);
	glf_enable(&st, GL_COLOR_LOGIC_OP, true);
	glf_pop_attrib(&st);
	CHECK_EQ(glf_get(&st, 0x0BF0, v), 1);
	CHECK_EQ(v[0], 0x1506);
	CHECK_EQ(glf_is_enabled(&st, GL_COLOR_LOGIC_OP), 0);
	glf_enable(&st, GL_COLOR_LOGIC_OP, true);
	CHECK_EQ(glf_raster_state(&st, &target)->blend, RASTER_BLEND_LOGIC);
	CHECK_EQ(glf_raster_state(&st, &target)->logic_op, RASTER_LOGIC_XOR);
	glf_logic_op(&st, GL_COPY);                             /* (copying: no blending) */
	CHECK_EQ(glf_raster_state(&st, &target)->blend, RASTER_BLEND_NONE);
	glf_enable(&st, GL_COLOR_LOGIC_OP, false);
}

/*
 * The accumulation buffer: loaded, accumulated, scaled, moved, returned
 * (clamped, through the color mask); its clear and the scissor box; a
 * drawable without one; 8 bits' steps
 */
static void test_accum(void)
{
	start();
	ortho();
	glf_accum(&st, &target, GL_LOAD, 1);
	CHECK_EQ(error(), GL_INVALID_OPERATION);
	target.accum_bits = 16;
	glf_accum(&st, &target, 0x0105, 1);
	CHECK_EQ(error(), GL_INVALID_ENUM);
	const float a[4] = { 0.2f, 0.4f, 0.6f, 1 }, b[4] = { 0.6f, 0.2f, 0, 1 };
	memcpy(st.clear_color, a, sizeof a);
	glf_clear(&st, &target, GL_COLOR_BUFFER_BIT);
	drawn();
	glf_accum(&st, &target, GL_LOAD, 1);
	memcpy(st.clear_color, b, sizeof b);
	glf_clear(&st, &target, GL_COLOR_BUFFER_BIT);
	drawn();
	glf_accum(&st, &target, GL_ACCUM, 0.5f);               /* 0.5, 0.5, 0.6 */
	glf_accum(&st, &target, GL_MULT, 0.5f);                /* 0.25, 0.25, 0.3 */
	glf_accum(&st, &target, GL_ADD, 0.1f);                 /* 0.35, 0.35, 0.4 */
	glf_accum(&st, &target, GL_RETURN, 2);                 /* 0.7, 0.7, 0.8 (alpha 1, clamped) */
	drawn();
	CHECK_EQ(pixel(10, 10), 0xFFB3B3CCu);
	/* the clear value clamped, cleared within the scissor box, returned through the color mask */
	glf_clear_accum(&st, 2, -0.25f, 0.5f, 0);
	double v[4];
	CHECK_EQ(glf_get(&st, 0x0B80, v), 4);                   /* GL_ACCUM_CLEAR_VALUE */
	CHECK(v[0] == 1 && v[1] == -0.25 && v[2] == 0.5 && v[3] == 0);
	glf_scissor(&st, 0, 0, 8, 8);
	glf_enable(&st, GL_SCISSOR_TEST, true);
	glf_clear(&st, &target, GL_ACCUM_BUFFER_BIT);
	glf_enable(&st, GL_SCISSOR_TEST, false);
	glf_color_mask(&st, true, true, false, true);
	glf_accum(&st, &target, GL_RETURN, 1);
	glf_color_mask(&st, true, true, true, true);
	drawn();
	CHECK_EQ(pixel(4, 4), 0x00FF00CCu);                     /* (red 1, green 0 (-0.25), blue kept, alpha 0) */
	CHECK_EQ(pixel(10, 10), 0x995959CCu);                   /* (0.35, 0.35, (blue kept), 0.6 returned once) */
	/* 8 bits: to the nearest of 127 steps */
	target.accum_bits = 8;
	free(st.accum);
	st.accum = NULL;
	glf_clear_accum(&st, 0.3f, 0.3f, 0.3f, 1);
	glf_clear(&st, &target, GL_ACCUM_BUFFER_BIT);
	CHECK_EQ(st.accum[0], (int16_t)(38.0f / 127 * 32767 + 0.5f));
	target.accum_bits = 0;
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
	test_pixel_transfer();
	test_texture_objects();
	test_texture_drawn();
	test_texture_env();
	test_texture_coords();
	test_lighting();
	test_texgen_clip();
	test_polygons();
	test_stencil_state();
	test_attrib();
	test_arrays();
	test_queries();
	test_raster_pos();
	test_draw_pixels();
	test_bitmap();
	test_depth_stencil_pixels();
	test_copy_get_tex_image();
	test_select_feedback();
	test_evaluators();
	test_stipple_logic();
	test_accum();
	surface_free(&surf);
	glf_detach(&st, NULL);
}
