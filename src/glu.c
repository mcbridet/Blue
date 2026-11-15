#include "glu.h"

/* (the same results on every host: no fused multiply-adds where some have them; GCC fuses none in ISO C) */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "be.h"
#include "opengl_table.h"

/* gl's functions and glu's, by name (opengl_table.h's order) */
enum {
#define X(name, result, args, offset) G_##name,
	OPENGL_LIBRARY_FUNCTIONS(X)
#undef X
};
enum {
#define X(name, result, args, offset) U_##name,
	OPENGL_UTILITY_FUNCTIONS(X)
#undef X
	U_FUNCTIONS
};
static const char *const names[U_FUNCTIONS] = {
#define X(name, result, args, offset) #name,
	OPENGL_UTILITY_FUNCTIONS(X)
#undef X
};

/* GLU's numbers (glu.h) */
enum { GLU_SMOOTH = 100000, GLU_FLAT, GLU_NONE };
enum { GLU_POINT = 100010, GLU_LINE, GLU_FILL, GLU_SILHOUETTE };
enum { GLU_OUTSIDE = 100020, GLU_INSIDE };
enum { GLU_ERROR = 100103 };
enum { GLU_TESS_WINDING_ODD = 100130, GLU_TESS_WINDING_ABS_GEQ_TWO = 100134 };
enum { GLU_TESS_WINDING_RULE = 100140, GLU_TESS_BOUNDARY_ONLY, GLU_TESS_TOLERANCE };
enum { GLU_TESS_ERROR1 = 100151, GLU_TESS_ERRORS = 6 };
enum { GLU_AUTO_LOAD_MATRIX = 100200, GLU_CULLING, GLU_PARAMETRIC_TOLERANCE, GLU_SAMPLING_TOLERANCE,
       GLU_DISPLAY_MODE, GLU_SAMPLING_METHOD, GLU_U_STEP, GLU_V_STEP, NURBS_PROPERTIES = 8 };
enum { GLU_PATH_LENGTH = 100215 };
enum { GLU_NURBS_ERROR1 = 100251, GLU_NURBS_ERRORS = 37 };
enum { GLU_VERSION = 100800, GLU_EXTENSIONS };
enum { GLU_INVALID_ENUM = 100900, GLU_INVALID_VALUE, GLU_OUT_OF_MEMORY };

const struct ndrv_import glu_imports[GLU_IMPORTS] = {
	[GLU_IMPORT_CURRENT] = { "OpenGLLibrary", "glc_cc" },
	[GLU_IMPORT_NEW_PTR_CLEAR] = { "InterfaceLib", "NewPtrClear" },
	[GLU_IMPORT_DISPOSE_PTR] = { "InterfaceLib", "DisposePtr" },
};

/* ---- the fragment's data: its strings ------------------------------------------------ */

/* As Apple's: its version, and gluErrorString()'s (SGI's, its NURBS errors' as SGI's GLU 1.2 put them) */
static const char version[] = "1.2 MacOS 8.0";
static const char extensions[] = "GL_EXT_texture";
static const char *const gl_errors[] = { "invalid enumerant", "invalid value", "invalid operation", "stack overflow",
					 "stack underflow", "out of memory" };
static const char *const nurbs_errors[GLU_NURBS_ERRORS] = {
	"spline order un-supported", "too few knots", "valid knot range is empty", "decreasing knot sequence knot",
	"knot multiplicity greater than order of spline", "endcurve() must follow bgncurve()",
	"bgncurve() must precede endcurve()", "missing or extra geometric data", "can't draw pwlcurves",
	"missing or extra domain data", "missing or extra domain data", "endtrim() must precede endsurface()",
	"bgnsurface() must precede endsurface()", "curve of improper type passed as trim curve",
	"bgnsurface() must precede bgntrim()", "endtrim() must follow bgntrim()", "bgntrim() must precede endtrim()",
	"invalid or missing trim curve", "bgntrim() must precede pwlcurve()", "pwlcurve referenced twice",
	"pwlcurve and nurbscurve mixed", "improper usage of trim data type", "nurbscurve referenced twice",
	"nurbscurve and pwlcurve mixed", "nurbssurface referenced twice", "invalid property",
	"endsurface() must follow bgnsurface()", "intersecting or misoriented trim curves", "intersecting trim curves",
	"UNUSED", "unconnected trim curves", "unknown knot error", "negative vertex count encountered",
	"negative byte-stride encounteed", "unknown type descriptor", "null control point reference",
	"duplicate point on pwlcurve",
};
static const char *const tess_errors[GLU_TESS_ERRORS] = {
	"gluTessBeginPolygon() must precede a gluTessEndPolygon()",
	"gluTessBeginContour() must precede a gluTessEndContour()",
	"gluTessEndPolygon() must follow a gluTessBeginPolygon()",
	"gluTessEndContour() must follow a gluTessBeginContour()", "a coordinate is too large",
	"need combine callback",
};

static struct {
	uint8_t data[2048];
	size_t len;
	uint32_t version, extensions, no_error, gl_error[6], nurbs_error[GLU_NURBS_ERRORS], tess_error[GLU_TESS_ERRORS];
} strings;

static uint32_t put_string(const char *s)
{
	size_t n = strlen(s) + 1;
	uint32_t at = (uint32_t)strings.len;
	if (strings.len + n <= sizeof strings.data) {
		memcpy(strings.data + strings.len, s, n);
		strings.len += n;
	}
	return at;
}

static void make_strings(void)
{
	if (!strings.len) {
		strings.version = put_string(version);
		strings.extensions = put_string(extensions);
		strings.no_error = put_string("no error");
		for (unsigned k = 0; k < 6; k++)
			strings.gl_error[k] = put_string(gl_errors[k]);
		for (unsigned k = 0; k < GLU_NURBS_ERRORS; k++)
			strings.nurbs_error[k] = put_string(nurbs_errors[k]);
		for (unsigned k = 0; k < GLU_TESS_ERRORS; k++)
			strings.tess_error[k] = put_string(tess_errors[k]);
	}
}

const uint8_t *glu_data(size_t *len)
{
	make_strings();
	*len = strings.len;
	return strings.data;
}

/* gluErrorString's string's offset in the data; false if there's none */
static bool error_string(uint32_t e, uint32_t *at)
{
	make_strings();
	switch (e) {
	case GL_NO_ERROR: *at = strings.no_error; return true;
	case GLU_INVALID_ENUM: *at = strings.gl_error[0]; return true;
	case GLU_INVALID_VALUE: *at = strings.gl_error[1]; return true;
	case GLU_OUT_OF_MEMORY: *at = strings.gl_error[5]; return true;
	default: break;
	}
	if (e >= GL_INVALID_ENUM && e <= GL_OUT_OF_MEMORY)
		*at = strings.gl_error[e - GL_INVALID_ENUM];
	else if (e >= GLU_NURBS_ERROR1 && e < GLU_NURBS_ERROR1 + GLU_NURBS_ERRORS)
		*at = strings.nurbs_error[e - GLU_NURBS_ERROR1];
	else if (e >= GLU_TESS_ERROR1 && e < GLU_TESS_ERROR1 + GLU_TESS_ERRORS)
		*at = strings.tess_error[e - GLU_TESS_ERROR1];
	else
		return false;
	return true;
}

/* ---- objects ------------------------------------------------------------------------- */

/*
 * Quadrics, tessellators and NURBS renderers: blocks the program's
 * NewPtrClear made, a magic word first. A quadric's words: its draw style,
 * normals, orientation, whether it's textured, its error callback (a
 * TVector). A tessellator's: its winding rule, whether boundaries only, its
 * tolerance (a double). A NURBS renderer's: its properties (floats, from
 * GLU_AUTO_LOAD_MATRIX's).
 */
#define QUADRIC_MAGIC 0x73795175u       /* 'syQu' */
#define TESS_MAGIC 0x73795473u          /* 'syTs' */
#define NURBS_MAGIC 0x73794E75u         /* 'syNu' */
enum { Q_STYLE = 4, Q_NORMALS = 8, Q_ORIENTATION = 12, Q_TEXTURE = 16, Q_ERROR = 20, QUADRIC_SIZE = 24 };
enum { T_WINDING = 4, T_BOUNDARY = 8, T_TOLERANCE = 16, TESS_SIZE = 24 };
enum { N_PROPERTY = 4, NURBS_SIZE = N_PROPERTY + 4 * NURBS_PROPERTIES };

/* What a call waits for (state word 0; 1: what's being made) */
enum { STEP_NONE, STEP_NEW, STEP_DONE };

/* An object of this kind's address (0 if `at` isn't one) */
static uint32_t object(struct opengl *o, uint32_t at, uint32_t magic, uint32_t size)
{
	const uint8_t *p = at ? opengl_in(o, at, size) : NULL;
	return p && be32(p) == magic ? at : 0;
}

static void new_object(struct opengl *o, struct ndrv_call *c, uint32_t magic, uint32_t size)
{
	ndrv_set_state(c, 0, STEP_NEW);
	ndrv_set_state(c, 1, magic);
	ndrv_callout(c, ndrv_import(c, &o->utility, GLU_IMPORT_NEW_PTR_CLEAR), size, 0, 0, 0);
}

/* A new block made an object (NewPtrClear's zeros, then the defaults) */
static void made(struct opengl *o, uint32_t at, uint32_t magic)
{
	static const float nurbs[NURBS_PROPERTIES] = { 1, 0, 0.5f, 50, GLU_FILL, GLU_PATH_LENGTH, 100, 100 };
	uint32_t size = magic == QUADRIC_MAGIC ? QUADRIC_SIZE : magic == TESS_MAGIC ? TESS_SIZE : NURBS_SIZE;
	uint8_t *p = opengl_out(o, at, size);
	if (!p)
		return;
	put_be32(p, magic);
	if (magic == QUADRIC_MAGIC) {
		put_be32(p + Q_STYLE, GLU_FILL);
		put_be32(p + Q_NORMALS, GLU_SMOOTH);
		put_be32(p + Q_ORIENTATION, GLU_OUTSIDE);
	} else if (magic == TESS_MAGIC) {
		put_be32(p + T_WINDING, GLU_TESS_WINDING_ODD);
	} else {
		for (unsigned k = 0; k < NURBS_PROPERTIES; k++) {
			uint32_t w;
			memcpy(&w, &nurbs[k], 4);
			put_be32(p + N_PROPERTY + 4 * k, w);
		}
	}
}

static void delete_object(struct opengl *o, struct ndrv_call *c, uint32_t at, uint32_t magic, uint32_t size)
{
	if (!object(o, at, magic, size)) {
		ndrv_finish(c, 0);
		return;
	}
	put_be32(opengl_out(o, at, 4), 0);
	ndrv_set_state(c, 0, STEP_DONE);
	ndrv_callout(c, ndrv_import(c, &o->utility, GLU_IMPORT_DISPOSE_PTR), at, 0, 0, 0);
}

/* A word of an object (one object() found) */
static uint32_t word(struct opengl *o, uint32_t at, unsigned offset)
{
	return be32(opengl_in(o, at + offset, 4));
}

static void set_word(struct opengl *o, uint32_t at, unsigned offset, uint32_t v)
{
	put_be32(opengl_out(o, at + offset, 4), v);
}

/* ---- the gl calls glu makes ---------------------------------------------------------- */

struct draw {
	struct opengl *o;
	struct opengl_context *x;
};

static void begin(const struct draw *d, uint32_t mode)
{
	union gl_arg a[1];
	a[0].u = mode;
	opengl_gl(d->o, d->x, G_glBegin, a);
}

static void end(const struct draw *d)
{
	union gl_arg a[1] = { { 0 } };
	opengl_gl(d->o, d->x, G_glEnd, a);
}

static void call3f(const struct draw *d, unsigned fn, float x, float y, float z)
{
	union gl_arg a[3];
	a[0].f = x;
	a[1].f = y;
	a[2].f = z;
	opengl_gl(d->o, d->x, fn, a);
}

static void vertex(const struct draw *d, float x, float y, float z)
{
	call3f(d, G_glVertex3f, x, y, z);
}

static void normal(const struct draw *d, float x, float y, float z)
{
	call3f(d, G_glNormal3f, x, y, z);
}

static void texcoord(const struct draw *d, float s, float t)
{
	union gl_arg a[2];
	a[0].f = s;
	a[1].f = t;
	opengl_gl(d->o, d->x, G_glTexCoord2f, a);
}

static void mult_matrixd(const struct draw *d, const double m[16])
{
	union gl_arg a[16];
	for (int k = 0; k < 16; k++)
		a[k].d = m[k];
	opengl_gl(d->o, d->x, G_glMultMatrixd, a);
}

/* ---- matrices ------------------------------------------------------------------------ */

static void perspective(const struct draw *d, double fovy, double aspect, double near, double far)
{
	double s, c, dz = far - near;
	glf_sincos(fovy / 2, &s, &c);
	if (dz == 0 || s == 0 || aspect == 0)
		return;
	double cot = c / s, m[16] = { 0 };
	m[0] = cot / aspect;
	m[5] = cot;
	m[10] = -(far + near) / dz;
	m[11] = -1;
	m[14] = -2 * near * far / dz;
	mult_matrixd(d, m);
}

static void normalize(float v[3])
{
	float r = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (r == 0)
		return;
	v[0] /= r;
	v[1] /= r;
	v[2] /= r;
}

static void cross(const float a[3], const float b[3], float r[3])
{
	r[0] = a[1] * b[2] - a[2] * b[1];
	r[1] = a[2] * b[0] - a[0] * b[2];
	r[2] = a[0] * b[1] - a[1] * b[0];
}

static void look_at(const struct draw *d, const union gl_arg *a)
{
	float forward[3], side[3], up[3];
	for (int k = 0; k < 3; k++) {
		forward[k] = (float)(a[3 + k].d - a[k].d);
		up[k] = (float)a[6 + k].d;
	}
	normalize(forward);
	cross(forward, up, side);
	normalize(side);
	cross(side, forward, up);
	union gl_arg m[16];
	for (int k = 0; k < 16; k++)
		m[k].f = k % 5 ? 0 : 1;
	for (int k = 0; k < 3; k++) {
		m[4 * k].f = side[k];
		m[4 * k + 1].f = up[k];
		m[4 * k + 2].f = -forward[k];
	}
	opengl_gl(d->o, d->x, G_glMultMatrixf, m);
	union gl_arg t[3];
	for (int k = 0; k < 3; k++)
		t[k].d = -a[k].d;
	opengl_gl(d->o, d->x, G_glTranslated, t);
}

/* Big-endian doubles or integers from the program's memory; false if they aren't there */
static bool doubles_in(struct opengl *o, uint32_t at, double *v, unsigned n)
{
	const uint8_t *p = opengl_in(o, at, 8 * (size_t)n);
	if (!p)
		return false;
	for (unsigned k = 0; k < n; k++) {
		uint64_t b = be64(p + 8 * k);
		memcpy(&v[k], &b, 8);
	}
	return true;
}

static bool ints_in(struct opengl *o, uint32_t at, int32_t *v, unsigned n)
{
	const uint8_t *p = opengl_in(o, at, 4 * (size_t)n);
	if (!p)
		return false;
	for (unsigned k = 0; k < n; k++)
		v[k] = (int32_t)be32(p + 4 * k);
	return true;
}

static void double_out(struct opengl *o, uint32_t at, double v)
{
	uint8_t *p = opengl_out(o, at, 8);
	uint64_t b;
	memcpy(&b, &v, 8);
	if (p)
		put_be64(p, b);
}

static void pick_matrix(const struct draw *d, const union gl_arg *a)
{
	int32_t vp[4];
	double x = a[0].d, y = a[1].d, dx = a[2].d, dy = a[3].d;
	if (dx <= 0 || dy <= 0 || !ints_in(d->o, a[4].u, vp, 4))
		return;
	call3f(d, G_glTranslatef, (float)((vp[2] - 2 * (x - vp[0])) / dx), (float)((vp[3] - 2 * (y - vp[1])) / dy), 0);
	call3f(d, G_glScalef, (float)(vp[2] / dx), (float)(vp[3] / dy), 1);
}

/* out = m in (m's columns first, as OpenGL's) */
static void mult_vec(const double m[16], const double in[4], double out[4])
{
	for (int i = 0; i < 4; i++)
		out[i] = in[0] * m[i] + in[1] * m[4 + i] + in[2] * m[8 + i] + in[3] * m[12 + i];
}

/* gluProject (false: unproject: gluUnProject); GL_TRUE or GL_FALSE */
static uint32_t project(struct opengl *o, const union gl_arg *a, bool unproject)
{
	double model[16], proj[16], in[4] = { a[0].d, a[1].d, a[2].d, 1 }, out[4];
	int32_t vp[4];
	if (!doubles_in(o, a[3].u, model, 16) || !doubles_in(o, a[4].u, proj, 16) || !ints_in(o, a[5].u, vp, 4))
		return 0;
	if (!unproject) {
		mult_vec(model, in, out);
		mult_vec(proj, out, in);
		if (in[3] == 0)
			return 0;
		for (int k = 0; k < 3; k++)
			in[k] = in[k] / in[3] * 0.5 + 0.5;
		out[0] = in[0] * vp[2] + vp[0];
		out[1] = in[1] * vp[3] + vp[1];
		out[2] = in[2];
	} else {
		/* (model x proj's inverse, by cofactors, as SGI's) */
		double m[16], inv[16];
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++)
				m[4 * i + j] = model[4 * i] * proj[j] + model[4 * i + 1] * proj[4 + j] +
					       model[4 * i + 2] * proj[8 + j] + model[4 * i + 3] * proj[12 + j];
		inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
			 m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
		inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
			 m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
		inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
			 m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
		inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
			  m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
		inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
			 m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
		inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
			 m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
		inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
			 m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
		inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
			  m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
		inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
			 m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
		inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
			 m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
		inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
			  m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
		inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
			  m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
		inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
			 m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
		inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
			 m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
		inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
			  m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
		inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
			  m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
		double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
		if (det == 0)
			return 0;
		for (int k = 0; k < 16; k++)
			inv[k] *= 1.0 / det;
		in[0] = (in[0] - vp[0]) / vp[2] * 2 - 1;
		in[1] = (in[1] - vp[1]) / vp[3] * 2 - 1;
		in[2] = in[2] * 2 - 1;
		mult_vec(inv, in, out);
		if (out[3] == 0)
			return 0;
		for (int k = 0; k < 3; k++)
			out[k] /= out[3];
	}
	for (int k = 0; k < 3; k++)
		double_out(o, a[6 + k].u, out[k]);
	return 1;
}

/* ---- images ---------------------------------------------------------------------------- */

/*
 * As SGI's GLU 1.2 scales them: 16 bits a component (an 8-bit one times
 * 257), each pixel the mean of the box it covers in the image (repeating
 * past its edges, when enlarged), rounded; halved by the mean of 2 x 2
 * exactly. Images are R, G, B, A.
 */
static void scale(int win, int hin, const uint16_t *in, int wout, int hout, uint16_t *out)
{
	if (win == wout * 2 && hin == hout * 2) {
		for (int i = 0; i < hout; i++)
			for (int j = 0; j < wout; j++)
				for (int k = 0; k < 4; k++) {
					const uint16_t *t = in + ((size_t)(2 * i) * (size_t)win + (size_t)(2 * j)) * 4 + k;
					size_t row = (size_t)win * 4;
					out[((size_t)i * (size_t)wout + (size_t)j) * 4 + k] =
						(uint16_t)((t[0] + t[4] + t[row] + t[row + 4] + 2) / 4);
				}
		return;
	}
	float convy = (float)hin / (float)hout, convx = (float)win / (float)wout;
	float halfconvx = convx / 2, halfconvy = convy / 2;
	for (int i = 0; i < hout; i++) {
		float y = (float)(convy * (i + 0.5)), lowy, highy;
		if (hin > hout) {
			highy = y + halfconvy;
			lowy = y - halfconvy;
		} else {
			highy = (float)(y + 0.5);
			lowy = (float)(y - 0.5);
		}
		for (int j = 0; j < wout; j++) {
			float x = (float)(convx * (j + 0.5)), lowx, highx;
			if (win > wout) {
				highx = x + halfconvx;
				lowx = x - halfconvx;
			} else {
				highx = (float)(x + 0.5);
				lowx = (float)(x - 0.5);
			}
			float totals[4] = { 0, 0, 0, 0 }, area = 0;
			float yy = lowy;
			int yint = (int)floorf(yy);
			while (yy < highy) {
				int yindex = (yint + hin) % hin;
				float ypercent = highy < (float)(yint + 1) ? highy - yy : (float)(yint + 1) - yy;
				float xx = lowx;
				int xint = (int)floorf(xx);
				while (xx < highx) {
					int xindex = (xint + win) % win;
					float xpercent = highx < (float)(xint + 1) ? highx - xx : (float)(xint + 1) - xx;
					float percent = xpercent * ypercent;
					area += percent;
					const uint16_t *p = in + ((size_t)yindex * (size_t)win + (size_t)xindex) * 4;
					for (int k = 0; k < 4; k++)
						totals[k] += (float)p[k] * percent;
					xint++;
					xx = (float)xint;
				}
				yint++;
				yy = (float)yint;
			}
			for (int k = 0; k < 4; k++)
				out[((size_t)i * (size_t)wout + (size_t)j) * 4 + k] = (uint16_t)((totals[k] + 0.5) / area);
		}
	}
}

/* SGI's nearestPower(): the power of two nearest v (3 x 2^k going up) */
static int nearest_power(int v)
{
	int p = 1;
	for (unsigned u = (unsigned)v; u > 1; u >>= 1) {
		if (u == 3)
			return p * 4;
		p *= 2;
	}
	return p;
}

static int log2_of(int v)
{
	int k = 0;
	while (v > 1) {
		v >>= 1;
		k++;
	}
	return k;
}

/* The pixel stores and transfer images are read through: the context's, else the defaults */
static const struct gl_state *state_of(const struct opengl_context *x)
{
	static struct gl_state defaults;
	static bool made_defaults;
	if (x)
		return &x->st;
	if (!made_defaults) {
		glf_init(&defaults);
		made_defaults = true;
	}
	return &defaults;
}

#define MAX_PIXELS (1u << 24)

/* An image in the program's memory, as 16-bit R, G, B, A (no pixel transfer: gl does it); NULL if it can't be */
static uint16_t *image_in(struct opengl *o, const struct gl_state *st, uint32_t format, uint32_t type, int32_t w,
			  int32_t h, uint32_t at)
{
	size_t n = (size_t)w * (size_t)h;
	const uint8_t *src = n <= MAX_PIXELS ? opengl_in(o, at, glf_image_bytes(&st->unpack, format, type, w, h)) : NULL;
	uint8_t *rgba = src ? malloc(n * 4) : NULL;
	uint16_t *wide = rgba ? malloc(n * 8) : NULL;
	if (wide && !glf_unpack_rgba(st, &st->unpack, format, type, w, h, src, rgba, false)) {
		free(wide);
		wide = NULL;
	}
	for (size_t k = 0; wide && k < n * 4; k++)
		wide[k] = (uint16_t)(rgba[k] * 257);
	free(rgba);
	return wide;
}

static bool color_format(uint32_t format)
{
	return format != GL_COLOR_INDEX && format != GL_STENCIL_INDEX && format != GL_DEPTH_COMPONENT;
}

static uint32_t scale_image(struct opengl *o, struct opengl_context *x, const union gl_arg *a)
{
	uint32_t format = a[0].u, type_in = a[3].u, type_out = a[7].u;
	int32_t win = a[1].i, hin = a[2].i, wout = a[5].i, hout = a[6].i;
	if (!win || !hin || !wout || !hout)
		return 0;
	if (win < 0 || hin < 0 || wout < 0 || hout < 0)
		return GLU_INVALID_VALUE;
	if (!color_format(format) || glf_pixel_check(format, type_in) || glf_pixel_check(format, type_out))
		return GLU_INVALID_ENUM;        /* (indices, stencil and depth images not taken) */
	const struct gl_state *st = state_of(x);
	size_t n = (size_t)wout * (size_t)hout;
	uint8_t *dst = n <= MAX_PIXELS ? opengl_out(o, a[8].u, glf_image_bytes(&st->pack, format, type_out, wout, hout))
				       : NULL;
	uint16_t *in = dst ? image_in(o, st, format, type_in, win, hin, a[4].u) : NULL;
	if (!in)
		return 0;                       /* (not there to be read or written: nothing done) */
	uint16_t *out = malloc(n * 8);
	uint8_t *rgba = malloc(n * 4);
	uint32_t r = GLU_OUT_OF_MEMORY;
	if (out && rgba) {
		scale(win, hin, in, wout, hout, out);
		bool lum = format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA;   /* (packed as R + G + B) */
		for (size_t k = 0; k < n * 4; k++)
			rgba[k] = lum && (k & 3) && (k & 3) < 3 ? 0 : (uint8_t)(out[k] >> 8);
		glf_pack_rgba(st, format, type_out, wout, hout, rgba, dst);
		r = 0;
	}
	free(in);
	free(out);
	free(rgba);
	return r;
}

/* A level's 16-bit image to glTexImage1D or 2D (as SGI's, with gl's pixel transfer) */
static void tex_level(const struct draw *d, bool two_d, uint32_t target, int32_t level, int32_t internal, int w,
		      int h, const uint16_t *img)
{
	static const struct gl_pixel_store packed = { .alignment = 2 };
	size_t n = (size_t)w * (size_t)h * 4;
	uint8_t *be = malloc(n * 2);
	if (!be)
		return;
	for (size_t k = 0; k < n; k++)
		put_be16(be + 2 * k, img[k]);
	union gl_arg a[9];
	int j = 0;
	a[j++].u = target;
	a[j++].i = level;
	a[j++].i = internal;
	a[j++].i = w;
	if (two_d)
		a[j++].i = h;
	a[j++].i = 0;
	a[j++].u = GL_RGBA;
	a[j++].u = GL_UNSIGNED_SHORT;
	a[j++].u = 1;                           /* (given: see opengl_gl_given()) */
	opengl_gl_given(d->o, d->x, two_d ? G_glTexImage2D : G_glTexImage1D, a, be, n * 2, &packed);
	free(be);
}

/*
 * gluBuild1DMipmaps and gluBuild2DMipmaps: the image scaled to the nearest
 * powers of two (no more than gl's largest), then halved down to 1 x 1, each
 * a level; the first as given if it's the size already. (SGI's GLU 1.3,
 * Mesa's libGLU, makes 2D ones from 8-bit images by another filter: within
 * a level of these where there's no scaling, apart where there is.)
 */
static uint32_t build_mipmaps(const struct draw *d, bool two_d, const union gl_arg *a)
{
	uint32_t target = a[0].u, format = a[two_d ? 4 : 3].u, type = a[two_d ? 5 : 4].u, data = a[two_d ? 6 : 5].u;
	int32_t internal = a[1].i, w = a[2].i, h = two_d ? a[3].i : 1;
	if (glf_pixel_check(format, type) || format == GL_STENCIL_INDEX || format == GL_DEPTH_COMPONENT)
		return GLU_INVALID_ENUM;
	if (w < 1 || h < 1)
		return GLU_INVALID_VALUE;
	if (!d->x)
		return 0;
	int nw = nearest_power(w), nh = nearest_power(h);
	nw = nw < GLF_MAX_TEXTURE_SIZE ? nw : GLF_MAX_TEXTURE_SIZE;
	nh = nh < GLF_MAX_TEXTURE_SIZE ? nh : GLF_MAX_TEXTURE_SIZE;
	int levels = log2_of(nw) > log2_of(nh) ? log2_of(nw) : log2_of(nh);
	uint16_t *img = image_in(d->o, &d->x->st, format, type, w, h, data);
	if (!img)
		return 0;                       /* (not there to be read) */
	int iw = w, ih = h;
	uint32_t r = 0;
	for (int level = 0; level <= levels; level++) {
		if (iw != nw || ih != nh) {
			uint16_t *scaled = malloc((size_t)nw * (size_t)nh * 8);
			if (!scaled) {
				r = GLU_OUT_OF_MEMORY;
				break;
			}
			scale(iw, ih, img, nw, nh, scaled);
			free(img);
			img = scaled;
			iw = nw;
			ih = nh;
		}
		if (level == 0 && w == nw && h == nh) {
			union gl_arg t[9];
			int j = 0;
			t[j++].u = target;
			t[j++].i = 0;
			t[j++].i = internal;
			t[j++].i = w;
			if (two_d)
				t[j++].i = h;
			t[j++].i = 0;
			t[j++].u = format;
			t[j++].u = type;
			t[j++].u = data;
			opengl_gl(d->o, d->x, two_d ? G_glTexImage2D : G_glTexImage1D, t);
		} else {
			tex_level(d, two_d, target, level, internal, nw, nh, img);
		}
		nw = nw > 1 ? nw / 2 : 1;
		nh = nh > 1 ? nh / 2 : 1;
	}
	free(img);
	return r;
}

/*
 * ---- quadrics -------------------------------------------------------------------------
 *
 * The gl calls SGI's GLU makes, in its order (its quad.c's), but where it
 * reads normals it didn't work out (flat ones for points and silhouettes):
 * the smooth ones there.
 */

#define CACHE_SIZE 240

struct quadric {
	uint32_t style, normals, orientation, callback;
	bool texture;
};

static bool quadric_of(struct opengl *o, uint32_t at, struct quadric *q)
{
	if (!object(o, at, QUADRIC_MAGIC, QUADRIC_SIZE))
		return false;
	q->style = word(o, at, Q_STYLE);
	q->normals = word(o, at, Q_NORMALS);
	q->orientation = word(o, at, Q_ORIENTATION);
	q->texture = word(o, at, Q_TEXTURE) != 0;
	q->callback = word(o, at, Q_ERROR);
	return true;
}

/* sin and cos of an angle in degrees, as floats */
static void sin_cos(double degrees, float *s, float *c)
{
	double sd, cd;
	glf_sincos(degrees, &sd, &cd);
	*s = (float)sd;
	*c = (float)cd;
}

static void cylinder(const struct draw *d, const struct quadric *q, double base, double top, double height,
		     int slices, int stacks)
{
	float sin1[CACHE_SIZE], cos1[CACHE_SIZE], sin2[CACHE_SIZE], cos2[CACHE_SIZE], sin3[CACHE_SIZE],
	      cos3[CACHE_SIZE];
	float delta = (float)(base - top), length = (float)sqrt(delta * delta + height * height);
	float znormal = delta / length, ratio = (float)(height / length);
	bool outside = q->orientation == GLU_OUTSIDE, tex = q->texture;
	float sign = outside ? 1.0f : -1.0f;
	for (int i = 0; i < slices; i++) {
		sin_cos(360.0 * i / slices, &sin1[i], &cos1[i]);
		sin2[i] = sign * ratio * sin1[i];
		cos2[i] = sign * ratio * cos1[i];
		float s, c;
		sin_cos(360.0 * (i - 0.5) / slices, &s, &c);
		sin3[i] = sign * ratio * s;
		cos3[i] = sign * ratio * c;
	}
	sin1[slices] = sin1[0];
	cos1[slices] = cos1[0];
	sin2[slices] = sin2[0];
	cos2[slices] = cos2[0];
	sin3[slices] = sin3[0];
	cos3[slices] = cos3[0];
	/* the normal at slice i (flat: the face's before it) */
	const float *ns = q->normals == GLU_FLAT ? sin3 : sin2, *nc = q->normals == GLU_FLAT ? cos3 : cos2;
	bool normals = q->normals == GLU_FLAT || q->normals == GLU_SMOOTH;
	switch (q->style) {
	case GLU_FILL:
		for (int j = 0; j < stacks; j++) {
			float zlow = (float)(j * height / stacks), zhigh = (float)((j + 1) * height / stacks);
			float rlow = (float)(base - delta * ((float)j / (float)stacks));
			float rhigh = (float)(base - delta * ((float)(j + 1) / (float)stacks));
			begin(d, GL_QUAD_STRIP);
			for (int i = 0; i <= slices; i++) {
				if (normals)
					normal(d, ns[i], nc[i], znormal);
				float s = 1 - (float)i / (float)slices;
				float t0 = (float)j / (float)stacks, t1 = (float)(j + 1) / (float)stacks;
				for (int k = 0; k < 2; k++) {
					bool low = (k == 0) == outside;
					if (tex)
						texcoord(d, s, low ? t0 : t1);
					float r = low ? rlow : rhigh;
					vertex(d, r * sin1[i], r * cos1[i], low ? zlow : zhigh);
				}
			}
			end(d);
		}
		break;
	case GLU_POINT:
		begin(d, GL_POINTS);
		for (int i = 0; i < slices; i++) {
			if (normals)
				normal(d, sin2[i], cos2[i], znormal);
			for (int j = 0; j <= stacks; j++) {
				float z = (float)(j * height / stacks);
				float r = (float)(base - delta * ((float)j / (float)stacks));
				if (tex)
					texcoord(d, 1 - (float)i / (float)slices, (float)j / (float)stacks);
				vertex(d, r * sin1[i], r * cos1[i], z);
			}
		}
		end(d);
		break;
	case GLU_LINE:
	case GLU_SILHOUETTE:
		/* the stacks' circles: lines' between the ends first, then (as silhouettes' only) the ends' */
		for (int n = q->style == GLU_LINE ? 1 : stacks; n < stacks + 2; n++) {
			int j = n < stacks ? n : n == stacks ? 0 : stacks;
			float z = (float)(j * height / stacks);
			float r = (float)(base - delta * ((float)j / (float)stacks));
			begin(d, GL_LINE_STRIP);
			for (int i = 0; i <= slices; i++) {
				if (normals)
					normal(d, ns[i], nc[i], znormal);
				if (tex)
					texcoord(d, 1 - (float)i / (float)slices, (float)j / (float)stacks);
				vertex(d, r * sin1[i], r * cos1[i], z);
			}
			end(d);
		}
		for (int i = 0; i < slices; i++) {
			if (normals)
				normal(d, sin2[i], cos2[i], 0);
			begin(d, GL_LINE_STRIP);
			for (int j = 0; j <= stacks; j++) {
				float z = (float)(j * height / stacks);
				float r = (float)(base - delta * ((float)j / (float)stacks));
				if (tex)
					texcoord(d, 1 - (float)i / (float)slices, (float)j / (float)stacks);
				vertex(d, r * sin1[i], r * cos1[i], z);
			}
			end(d);
		}
		break;
	default:
		break;
	}
}

static void sphere(const struct draw *d, const struct quadric *q, double radius, int slices, int stacks)
{
	float sin1a[CACHE_SIZE], cos1a[CACHE_SIZE], sin3a[CACHE_SIZE], cos3a[CACHE_SIZE];
	float sin1b[CACHE_SIZE], cos1b[CACHE_SIZE], sin2b[CACHE_SIZE], cos2b[CACHE_SIZE], sin3b[CACHE_SIZE],
	      cos3b[CACHE_SIZE];
	bool outside = q->orientation == GLU_OUTSIDE, tex = q->texture;
	bool smooth = q->normals == GLU_SMOOTH, flat = q->normals == GLU_FLAT;
	float sign = outside ? 1.0f : -1.0f;
	for (int i = 0; i < slices; i++) {
		sin_cos(360.0 * i / slices, &sin1a[i], &cos1a[i]);
		sin_cos(360.0 * (i - 0.5) / slices, &sin3a[i], &cos3a[i]);
	}
	for (int j = 0; j <= stacks; j++) {
		float s, c;
		sin_cos(180.0 * j / stacks, &s, &c);
		sin2b[j] = sign * s;
		cos2b[j] = sign * c;
		sin1b[j] = (float)(radius * s);
		cos1b[j] = (float)(radius * c);
		sin_cos(180.0 * (j - 0.5) / stacks, &s, &c);
		sin3b[j] = sign * s;
		cos3b[j] = sign * c;
	}
	sin1b[0] = 0;                           /* (the poles: points) */
	sin1b[stacks] = 0;
	sin1a[slices] = sin1a[0];
	cos1a[slices] = cos1a[0];
	sin3a[slices] = sin3a[0];
	cos3a[slices] = cos3a[0];
	const float *sin2a = sin1a, *cos2a = cos1a;
	switch (q->style) {
	case GLU_FILL: {
		int start = 0, finish = stacks;
		if (!tex) {
			/* the ends as fans (textured, their apexes' coordinates differ for each slice: strips) */
			start = 1;
			finish = stacks - 1;
			for (int e = 0; e < 2; e++) {
				int ring = e ? stacks - 1 : 1;
				float sr = sin1b[ring], zr = cos1b[ring], s3 = 0, c3 = 0;
				if (flat) {
					s3 = sin3b[e ? stacks : 1];
					c3 = cos3b[e ? stacks : 1];
				} else if (smooth) {
					s3 = sin2b[ring];
					c3 = cos2b[ring];
					/* (SGI's takes the bottom's normal's slice by its stack: one past its slices
					 * if there are more stacks, where sin2b's 0 makes it no matter) */
					int pole = e ? stacks : 0, slice = pole <= slices ? pole : 0;
					normal(d, sin2a[slice] * sin2b[pole], cos2a[slice] * sin2b[pole], cos2b[pole]);
				}
				begin(d, GL_TRIANGLE_FAN);
				vertex(d, 0, 0, (float)(e ? -radius : radius));
				/* (around the top backwards from outside, the bottom forwards) */
				bool backwards = (e == 0) == outside;
				for (int n = 0; n <= slices; n++) {
					int i = backwards ? slices - n : n;
					if (smooth)
						normal(d, sin2a[i] * s3, cos2a[i] * s3, c3);
					else if (flat && !backwards)
						normal(d, sin3a[i] * s3, cos3a[i] * s3, c3);
					else if (flat && i != slices)
						normal(d, sin3a[i + 1] * s3, cos3a[i + 1] * s3, c3);
					vertex(d, sr * sin1a[i], sr * cos1a[i], zr);
				}
				end(d);
			}
		}
		for (int j = start; j < finish; j++) {
			float zlow = cos1b[j], zhigh = cos1b[j + 1], s1 = sin1b[j], s2 = sin1b[j + 1];
			float s3 = 0, c3 = 0, s4 = 0, c4 = 0;
			if (flat) {
				s4 = sin3b[j + 1];
				c4 = cos3b[j + 1];
			} else if (smooth) {
				int hi = outside ? j + 1 : j, lo = outside ? j : j + 1;
				s3 = sin2b[hi];
				c3 = cos2b[hi];
				s4 = sin2b[lo];
				c4 = cos2b[lo];
			}
			begin(d, GL_QUAD_STRIP);
			for (int i = 0; i <= slices; i++) {
				float s = 1 - (float)i / (float)slices;
				if (smooth)
					normal(d, sin2a[i] * s3, cos2a[i] * s3, c3);
				if (outside) {
					if (tex)
						texcoord(d, s, 1 - (float)(j + 1) / (float)stacks);
					vertex(d, s2 * sin1a[i], s2 * cos1a[i], zhigh);
				} else {
					if (tex)
						texcoord(d, s, 1 - (float)j / (float)stacks);
					vertex(d, s1 * sin1a[i], s1 * cos1a[i], zlow);
				}
				if (smooth)
					normal(d, sin2a[i] * s4, cos2a[i] * s4, c4);
				else if (flat)
					normal(d, sin3a[i] * s4, cos3a[i] * s4, c4);
				if (outside) {
					if (tex)
						texcoord(d, s, 1 - (float)j / (float)stacks);
					vertex(d, s1 * sin1a[i], s1 * cos1a[i], zlow);
				} else {
					if (tex)
						texcoord(d, s, 1 - (float)(j + 1) / (float)stacks);
					vertex(d, s2 * sin1a[i], s2 * cos1a[i], zhigh);
				}
			}
			end(d);
		}
		break;
	}
	case GLU_POINT:
		begin(d, GL_POINTS);
		for (int j = 0; j <= stacks; j++)
			for (int i = 0; i < slices; i++) {
				if (smooth || flat)
					normal(d, sin2a[i] * sin2b[j], cos2a[i] * sin2b[j], cos2b[j]);
				if (tex)
					texcoord(d, 1 - (float)i / (float)slices, 1 - (float)j / (float)stacks);
				vertex(d, sin1b[j] * sin1a[i], sin1b[j] * cos1a[i], cos1b[j]);
			}
		end(d);
		break;
	case GLU_LINE:
	case GLU_SILHOUETTE:
		for (int j = 1; j < stacks; j++) {
			begin(d, GL_LINE_STRIP);
			for (int i = 0; i <= slices; i++) {
				if (flat)
					normal(d, sin3a[i] * sin2b[j], cos3a[i] * sin2b[j], cos2b[j]);
				else if (smooth)
					normal(d, sin2a[i] * sin2b[j], cos2a[i] * sin2b[j], cos2b[j]);
				if (tex)
					texcoord(d, 1 - (float)i / (float)slices, 1 - (float)j / (float)stacks);
				vertex(d, sin1b[j] * sin1a[i], sin1b[j] * cos1a[i], cos1b[j]);
			}
			end(d);
		}
		for (int i = 0; i < slices; i++) {
			begin(d, GL_LINE_STRIP);
			for (int j = 0; j <= stacks; j++) {
				if (flat)
					normal(d, sin2a[i] * sin3b[j], cos2a[i] * sin3b[j], cos3b[j]);
				else if (smooth)
					normal(d, sin2a[i] * sin2b[j], cos2a[i] * sin2b[j], cos2b[j]);
				if (tex)
					texcoord(d, 1 - (float)i / (float)slices, 1 - (float)j / (float)stacks);
				vertex(d, sin1a[i] * sin1b[j], cos1a[i] * sin1b[j], cos1b[j]);
			}
			end(d);
		}
		break;
	default:
		break;
	}
}

/* gluPartialDisk (gluDisk: all of it): loops from the outer radius in, slices from the start angle around */
static void partial_disk(const struct draw *d, const struct quadric *q, double inner, double outer, int slices,
			 int loops, double start, double sweep)
{
	float sinc[CACHE_SIZE], cosc[CACHE_SIZE];
	bool outside = q->orientation == GLU_OUTSIDE, tex = q->texture;
	if (sweep < -360.0)
		sweep = 360.0;
	if (sweep > 360.0)
		sweep = 360.0;
	if (sweep < 0) {
		start += sweep;
		sweep = -sweep;
	}
	int slices2 = sweep == 360.0 ? slices : slices + 1;
	float delta = (float)(outer - inner);
	for (int i = 0; i <= slices; i++)
		sin_cos(start + sweep * i / slices, &sinc[i], &cosc[i]);
	if (sweep == 360.0) {
		sinc[slices] = sinc[0];
		cosc[slices] = cosc[0];
	}
	if (q->normals == GLU_FLAT || q->normals == GLU_SMOOTH)
		normal(d, 0, 0, outside ? 1.0f : -1.0f);
	/* loop j's radius, its texture coordinates' radius */
#define RADIUS(j) ((float)(outer - delta * ((float)(j) / (float)loops)))
#define TEXR(r) ((float)((r) / outer / 2))
	switch (q->style) {
	case GLU_FILL: {
		int finish = loops;
		if (inner == 0.0) {
			finish = loops - 1;
			begin(d, GL_TRIANGLE_FAN);
			if (tex)
				texcoord(d, 0.5f, 0.5f);
			vertex(d, 0, 0, 0);
			float r = RADIUS(loops - 1), t = tex ? TEXR(r) : 0;
			for (int n = 0; n <= slices; n++) {
				int i = outside ? slices - n : n;
				if (tex)
					texcoord(d, t * sinc[i] + 0.5f, t * cosc[i] + 0.5f);
				vertex(d, r * sinc[i], r * cosc[i], 0);
			}
			end(d);
		}
		for (int j = 0; j < finish; j++) {
			float rlow = RADIUS(j), rhigh = RADIUS(j + 1);
			float tlow = tex ? TEXR(rlow) : 0, thigh = tex ? TEXR(rhigh) : 0;
			begin(d, GL_QUAD_STRIP);
			for (int i = 0; i <= slices; i++)
				for (int k = 0; k < 2; k++) {
					bool low = (k == 0) == outside;
					float r = low ? rlow : rhigh, t = low ? tlow : thigh;
					if (tex)
						texcoord(d, t * sinc[i] + 0.5f, t * cosc[i] + 0.5f);
					vertex(d, r * sinc[i], r * cosc[i], 0);
				}
			end(d);
		}
		break;
	}
	case GLU_POINT:
		begin(d, GL_POINTS);
		for (int i = 0; i < slices2; i++)
			for (int j = 0; j <= loops; j++) {
				float r = RADIUS(j);
				if (tex) {
					float t = TEXR(r);
					texcoord(d, t * sinc[i] + 0.5f, t * cosc[i] + 0.5f);
				}
				vertex(d, r * sinc[i], r * cosc[i], 0);
			}
		end(d);
		break;
	case GLU_LINE:
		if (inner == outer) {
			begin(d, GL_LINE_STRIP);
			for (int i = 0; i <= slices; i++) {
				if (tex)
					texcoord(d, sinc[i] / 2 + 0.5f, cosc[i] / 2 + 0.5f);
				vertex(d, (float)(inner * sinc[i]), (float)(inner * cosc[i]), 0);
			}
			end(d);
			break;
		}
		for (int j = 0; j <= loops; j++) {
			float r = RADIUS(j), t = tex ? TEXR(r) : 0;
			begin(d, GL_LINE_STRIP);
			for (int i = 0; i <= slices; i++) {
				if (tex)
					texcoord(d, t * sinc[i] + 0.5f, t * cosc[i] + 0.5f);
				vertex(d, r * sinc[i], r * cosc[i], 0);
			}
			end(d);
		}
		for (int i = 0; i < slices2; i++) {
			begin(d, GL_LINE_STRIP);
			for (int j = 0; j <= loops; j++) {
				float r = RADIUS(j);
				if (tex) {
					float t = TEXR(r);
					texcoord(d, t * sinc[i] + 0.5f, t * cosc[i] + 0.5f);
				}
				vertex(d, r * sinc[i], r * cosc[i], 0);
			}
			end(d);
		}
		break;
	case GLU_SILHOUETTE:
		if (sweep < 360.0)
			for (int i = 0; i <= slices; i += slices) {
				begin(d, GL_LINE_STRIP);
				for (int j = 0; j <= loops; j++) {
					float r = RADIUS(j);
					if (tex) {
						float t = TEXR(r);
						texcoord(d, t * sinc[i] + 0.5f, t * cosc[i] + 0.5f);
					}
					vertex(d, r * sinc[i], r * cosc[i], 0);
				}
				end(d);
			}
		for (int j = 0; j <= loops; j += loops) {
			float r = RADIUS(j), t = tex ? TEXR(r) : 0;
			begin(d, GL_LINE_STRIP);
			for (int i = 0; i <= slices; i++) {
				if (tex)
					texcoord(d, t * sinc[i] + 0.5f, t * cosc[i] + 0.5f);
				vertex(d, r * sinc[i], r * cosc[i], 0);
			}
			end(d);
			if (inner == outer)
				break;
		}
		break;
	default:
		break;
	}
#undef RADIUS
#undef TEXR
}

/* A quadric's error: its callback called with it (a callout), if it has one */
static void quadric_error(struct ndrv_call *c, const struct quadric *q, uint32_t e)
{
	if (!q->callback) {
		ndrv_finish(c, 0);
		return;
	}
	ndrv_set_state(c, 0, STEP_DONE);
	ndrv_callout(c, q->callback, e, 0, 0, 0);
}

/* A quadric drawn (or its error); finishes c or makes a callout */
static void draw_quadric(const struct draw *d, struct ndrv_call *c, unsigned fn, const union gl_arg *a)
{
	struct quadric q;
	if (!quadric_of(d->o, a[0].u, &q)) {
		ndrv_finish(c, 0);
		return;
	}
	bool bad;
	switch (fn) {
	case U_gluSphere: {
		int slices = a[2].i < CACHE_SIZE ? a[2].i : CACHE_SIZE - 1;
		int stacks = a[3].i < CACHE_SIZE ? a[3].i : CACHE_SIZE - 1;
		bad = slices < 2 || stacks < 1 || a[1].d < 0;
		if (!bad && d->x)
			sphere(d, &q, a[1].d, slices, stacks);
		break;
	}
	case U_gluCylinder: {
		int slices = a[4].i < CACHE_SIZE ? a[4].i : CACHE_SIZE - 1, stacks = a[5].i;
		double delta = a[1].d - a[2].d;
		bad = slices < 2 || stacks < 1 || a[1].d < 0 || a[2].d < 0 || a[3].d < 0 ||
		      (float)sqrt(delta * delta + a[3].d * a[3].d) == 0;
		if (!bad && d->x)
			cylinder(d, &q, a[1].d, a[2].d, a[3].d, slices, stacks);
		break;
	}
	default: {                              /* (gluDisk, gluPartialDisk) */
		bool partial = fn == U_gluPartialDisk;
		int slices = a[3].i < CACHE_SIZE ? a[3].i : CACHE_SIZE - 1, loops = a[4].i;
		bad = slices < 2 || loops < 1 || a[2].d <= 0 || a[1].d < 0 || a[1].d > a[2].d;
		if (!bad && d->x)
			partial_disk(d, &q, a[1].d, a[2].d, slices, loops, partial ? a[5].d : 0, partial ? a[6].d : 360);
		break;
	}
	}
	if (bad)
		quadric_error(c, &q, GLU_INVALID_VALUE);
	else
		ndrv_finish(c, 0);
}

/* gluQuadricDrawStyle, Normals, Orientation: the value set, or an error */
static void quadric_set(struct opengl *o, struct ndrv_call *c, unsigned fn, const union gl_arg *a)
{
	struct quadric q;
	if (!quadric_of(o, a[0].u, &q)) {
		ndrv_finish(c, 0);
		return;
	}
	uint32_t v = a[1].u, offset;
	bool ok;
	switch (fn) {
	case U_gluQuadricDrawStyle: offset = Q_STYLE; ok = v >= GLU_POINT && v <= GLU_SILHOUETTE; break;
	case U_gluQuadricNormals: offset = Q_NORMALS; ok = v >= GLU_SMOOTH && v <= GLU_NONE; break;
	default: offset = Q_ORIENTATION; ok = v == GLU_OUTSIDE || v == GLU_INSIDE; break;
	}
	if (!ok) {
		quadric_error(c, &q, GLU_INVALID_ENUM);
		return;
	}
	set_word(o, a[0].u, offset, v);
	ndrv_finish(c, 0);
}

/* ---- tessellators and NURBS renderers: their properties ----------------------------------- */

static void tess_property(struct opengl *o, const union gl_arg *a)
{
	uint32_t t = object(o, a[0].u, TESS_MAGIC, TESS_SIZE);
	double v = a[2].d;
	if (!t)
		return;
	switch (a[1].u) {
	case GLU_TESS_WINDING_RULE:
		if (v == floor(v) && v >= GLU_TESS_WINDING_ODD && v <= GLU_TESS_WINDING_ABS_GEQ_TWO)
			set_word(o, t, T_WINDING, (uint32_t)v);
		break;
	case GLU_TESS_BOUNDARY_ONLY: set_word(o, t, T_BOUNDARY, v != 0); break;
	case GLU_TESS_TOLERANCE:
		if (v >= 0 && v <= 1) {
			uint64_t b;
			memcpy(&b, &v, 8);
			put_be64(opengl_out(o, t + T_TOLERANCE, 8), b);
		}
		break;
	default: break;
	}
}

static void get_tess_property(struct opengl *o, const union gl_arg *a)
{
	uint32_t t = object(o, a[0].u, TESS_MAGIC, TESS_SIZE);
	double v = 0;
	if (t && a[1].u == GLU_TESS_WINDING_RULE)
		v = word(o, t, T_WINDING);
	else if (t && a[1].u == GLU_TESS_BOUNDARY_ONLY)
		v = word(o, t, T_BOUNDARY);
	else if (t && a[1].u == GLU_TESS_TOLERANCE)
		doubles_in(o, t + T_TOLERANCE, &v, 1);
	double_out(o, a[2].u, v);
}

static void nurbs_property(struct opengl *o, const union gl_arg *a, bool get)
{
	uint32_t n = object(o, a[0].u, NURBS_MAGIC, NURBS_SIZE), k = a[1].u - GLU_AUTO_LOAD_MATRIX;
	if (!n || k >= NURBS_PROPERTIES)
		return;
	if (get) {
		uint8_t *p = opengl_out(o, a[2].u, 4);
		if (p)
			put_be32(p, word(o, n, N_PROPERTY + 4 * k));
	} else {
		set_word(o, n, N_PROPERTY + 4 * k, a[2].u);
	}
}

/* ---- calls ------------------------------------------------------------------------------- */

void glu_call(struct opengl *o, struct opengl_context *x, struct ndrv_call *c, const union gl_arg *a)
{
	if (c->entry == NDRV_ENTRY_RESUME) {
		uint32_t r = c->arg[0];
		if (ndrv_state(c, 0) == STEP_NEW) {
			if (r)
				made(o, r, ndrv_state(c, 1));
			ndrv_finish(c, r);
		} else {
			ndrv_finish(c, 0);
		}
		return;
	}
	if (c->entry >= U_FUNCTIONS) {
		ndrv_finish(c, 0);
		return;
	}
	const struct draw d = { o, x };
	uint32_t r = 0, base = c->toc + o->utility.extra;
	unsigned fn = c->entry;
	switch (fn) {
	/* matrices, projections */
	case U_gluOrtho2D: {
		const union gl_arg b[6] = { a[0], a[1], a[2], a[3], { .d = -1 }, { .d = 1 } };
		if (x)
			opengl_gl(o, x, G_glOrtho, b);
		break;
	}
	case U_gluPerspective:
		if (x)
			perspective(&d, a[0].d, a[1].d, a[2].d, a[3].d);
		break;
	case U_gluLookAt:
		if (x)
			look_at(&d, a);
		break;
	case U_gluPickMatrix:
		if (x)
			pick_matrix(&d, a);
		break;
	case U_gluProject: r = project(o, a, false); break;
	case U_gluUnProject: r = project(o, a, true); break;
	/* images */
	case U_gluScaleImage: r = scale_image(o, x, a); break;
	case U_gluBuild1DMipmaps: r = build_mipmaps(&d, false, a); break;
	case U_gluBuild2DMipmaps: r = build_mipmaps(&d, true, a); break;
	/* strings */
	case U_gluErrorString: {
		uint32_t s;
		r = error_string(a[0].u, &s) ? base + s : 0;
		break;
	}
	case U_gluGetString:
		make_strings();
		r = a[0].u == GLU_VERSION ? base + strings.version :
		    a[0].u == GLU_EXTENSIONS ? base + strings.extensions : 0;
		break;
	/* quadrics */
	case U_gluNewQuadric: new_object(o, c, QUADRIC_MAGIC, QUADRIC_SIZE); return;
	case U_gluDeleteQuadric: delete_object(o, c, a[0].u, QUADRIC_MAGIC, QUADRIC_SIZE); return;
	case U_gluQuadricDrawStyle: case U_gluQuadricNormals: case U_gluQuadricOrientation:
		quadric_set(o, c, fn, a);
		return;
	case U_gluQuadricTexture:
		if (object(o, a[0].u, QUADRIC_MAGIC, QUADRIC_SIZE))
			set_word(o, a[0].u, Q_TEXTURE, a[1].u != 0);
		break;
	case U_gluQuadricCallback:
		if (object(o, a[0].u, QUADRIC_MAGIC, QUADRIC_SIZE) && a[1].u == GLU_ERROR)
			set_word(o, a[0].u, Q_ERROR, a[2].u);
		break;
	case U_gluSphere: case U_gluCylinder: case U_gluDisk: case U_gluPartialDisk:
		draw_quadric(&d, c, fn, a);
		return;
	/* tessellators, NURBS renderers */
	case U_gluNewTess: new_object(o, c, TESS_MAGIC, TESS_SIZE); return;
	case U_gluDeleteTess: delete_object(o, c, a[0].u, TESS_MAGIC, TESS_SIZE); return;
	case U_gluNewNurbsRenderer: new_object(o, c, NURBS_MAGIC, NURBS_SIZE); return;
	case U_gluDeleteNurbsRenderer: delete_object(o, c, a[0].u, NURBS_MAGIC, NURBS_SIZE); return;
	case U_gluTessProperty: tess_property(o, a); break;
	case U_gluGetTessProperty: get_tess_property(o, a); break;
	case U_gluNurbsProperty: nurbs_property(o, a, false); break;
	case U_gluGetNurbsProperty: nurbs_property(o, a, true); break;
	case U_gluTessCallback: case U_gluTessNormal: case U_gluTessBeginContour: case U_gluTessEndContour:
	case U_gluTessBeginPolygon: case U_gluNurbsCallback: case U_gluLoadSamplingMatrices: case U_gluBeginCurve:
	case U_gluBeginSurface: case U_gluBeginTrim: case U_gluEndTrim: case U_gluBeginPolygon:
		break;                          /* (accepted: what they'd start draws nothing) */
	default:                                /* (what tessellates or draws NURBS) */
		if (!(o->glu_told >> fn & 1)) {
			o->glu_told |= 1ull << fn;
			printf("opengl: %s isn't done yet\n", names[fn]);
			fflush(stdout);
		}
		break;
	}
	ndrv_finish(c, r);
}
