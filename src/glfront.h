/*
 * OpenGL's fixed-function front end, for the guest's OpenGL (opengl.h):
 * the GL state drawing depends on, the matrix stacks, current attributes,
 * glBegin/glEnd's primitives assembled into points, lines and triangles,
 * transformed to clip coordinates, clipped, divided by w and mapped through
 * the viewport and depth range, culled, and handed to the 3D backend
 * (surface.h) as screen-space raster commands with the state that draws
 * them. Built without fused multiply-adds (as raster.c), and with sines and
 * cosines of its own, so its arithmetic is the same on every host.
 *
 * Where OpenGL leaves room, it does as ATI's Rage 128 OpenGL did, as far as
 * that's known:
 *
 * - Colors are interpolated linearly in screen space: the Rage 128's setup
 *   engine corrected texture coordinates for perspective, not colors
 *   (OpenGL allows either; the RAVE engine's are the same).
 * - Primitives are clipped against the near and far planes, and against
 *   the others only past a guard band (GLF_GUARD times the view volume);
 *   the rest of the clipping is the viewport's pixels. So a triangle
 *   partly off the screen is drawn from its own vertices, colors and all,
 *   as hardware with a guard band drew it.
 * - Quadrilaterals are split as (v0, v1, v3) and (v1, v2, v3), polygons
 *   fanned from their first vertex; each triangle is culled by its own
 *   facing.
 */
#ifndef SYSHM_GLFRONT_H
#define SYSHM_GLFRONT_H

#include <stdbool.h>
#include <stdint.h>

#include "raster.h"
#include "surface.h"

/* OpenGL's numbers (gl.h's) that the front end and the libraries take */
enum { GL_NO_ERROR, GL_INVALID_ENUM = 0x0500, GL_INVALID_VALUE, GL_INVALID_OPERATION, GL_STACK_OVERFLOW,
       GL_STACK_UNDERFLOW, GL_OUT_OF_MEMORY };
enum { GL_POINTS, GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN,
       GL_QUADS, GL_QUAD_STRIP, GL_POLYGON };
enum { GL_NEVER = 0x0200, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
enum { GL_ZERO, GL_ONE, GL_SRC_COLOR = 0x0300, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
       GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE,
       GL_CONSTANT_COLOR = 0x8001, GL_ONE_MINUS_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA };
enum { GL_FUNC_ADD = 0x8006, GL_MIN, GL_MAX, GL_FUNC_SUBTRACT = 0x800A, GL_FUNC_REVERSE_SUBTRACT };
enum { GL_FRONT = 0x0404, GL_BACK, GL_FRONT_AND_BACK = 0x0408, GL_CW = 0x0900, GL_CCW };
enum { GL_FLAT = 0x1D00, GL_SMOOTH };
enum { GL_MODELVIEW = 0x1700, GL_PROJECTION, GL_TEXTURE };
enum { GL_TEXTURE0_ARB = 0x84C0 };
#define GL_DEPTH_BUFFER_BIT 0x0100u
#define GL_ACCUM_BUFFER_BIT 0x0200u
#define GL_STENCIL_BUFFER_BIT 0x0400u
#define GL_COLOR_BUFFER_BIT 0x4000u
/* glEnable's capabilities the front end draws by (the rest are kept: see glf_enable()) */
enum { GL_POINT_SMOOTH = 0x0B10, GL_LINE_SMOOTH = 0x0B20, GL_CULL_FACE = 0x0B44, GL_LIGHTING = 0x0B50,
       GL_FOG = 0x0B60, GL_DEPTH_TEST = 0x0B71, GL_ALPHA_TEST = 0x0BC0, GL_DITHER = 0x0BD0, GL_BLEND = 0x0BE2,
       GL_SCISSOR_TEST = 0x0C11, GL_TEXTURE_1D = 0x0DE0, GL_TEXTURE_2D = 0x0DE1 };

#define GLF_GUARD 4.0f                  /* the guard band, in view volumes (see above) */
#define GLF_MAX_SIZE 4096               /* viewports, drawables */
#define GLF_MODELVIEW_DEPTH 32          /* matrix stacks' depths */
#define GLF_PROJECTION_DEPTH 8
#define GLF_TEXTURE_DEPTH 8
#define GLF_TEXTURE_UNITS RASTER_UNITS

struct gl_matrices {
	float m[GLF_MODELVIEW_DEPTH][16];       /* column-major, as OpenGL's; the top is m[depth - 1] */
	unsigned depth, max;
};

/* A vertex on its way: clip coordinates, then what's interpolated */
struct gl_vertex {
	float clip[4];
	float color[4];                         /* r, g, b, a (clamped) */
	float tex[GLF_TEXTURE_UNITS][4];
};

/* Indexes of the capabilities in gl_state.caps (glfront.c's table has their names, these first) */
enum { GLF_CAP_DEPTH_TEST, GLF_CAP_CULL_FACE, GLF_CAP_BLEND, GLF_CAP_ALPHA_TEST, GLF_CAP_SCISSOR_TEST,
       GLF_CAP_DITHER, GLF_CAPS = 80 };

struct gl_state {
	uint32_t error;                         /* the first since glGetError() */
	uint8_t caps[GLF_CAPS];                 /* enabled (per texture unit ones: bit k for unit k) */
	/* clearing */
	float clear_color[4];                   /* r, g, b, a */
	double clear_depth;
	/* the window */
	int32_t viewport[4], scissor[4];
	double depth_range[2];
	float vp_scale[3], vp_offset[3];        /* the viewport transform: window = ndc * scale + offset */
	bool color_mask[4];                     /* r, g, b, a */
	bool depth_mask;
	/* per fragment, per primitive */
	uint32_t depth_func, blend_src, blend_dst, blend_eq, alpha_func, cull_mode, front_face, shade_model;
	float blend_color[4];                   /* r, g, b, a */
	float alpha_ref;
	float point_size, line_width;
	/* matrices */
	uint32_t matrix_mode;
	unsigned active_texture;
	struct gl_matrices modelview, projection, texture[GLF_TEXTURE_UNITS];
	float mvp[16];                          /* projection times modelview */
	bool mvp_stale;
	/* current attributes */
	float color[4], secondary_color[4], normal[3], tex[GLF_TEXTURE_UNITS][4], fog_coord;
	bool edge_flag;
	/* between glBegin and glEnd */
	bool inside;
	uint32_t mode;
	unsigned count;                         /* vertices since glBegin */
	struct gl_vertex vb[3];                 /* (what the primitive so far needs) */
	/* the raster state drawing takes, made again when the GL state changes */
	struct raster_state raster;
	bool raster_stale, draw_none;
	int raster_width, raster_height;
	unsigned long primitives;               /* triangles, lines and points handed to the backend */
};

/* Where a context's drawing goes: its buffers (NULL: nowhere), their size, the pixel format's bits */
struct gl_target {
	struct surface *surface;
	int width, height;
	unsigned depth_bits, alpha_bits;
};

/* OpenGL's initial state; the viewport and scissor box are set when there's a drawable (glf_viewport()) */
void glf_init(struct gl_state *st);

/* An error, as glGetError() gives the first */
void glf_error(struct gl_state *st, uint32_t e);

/* The raster state needs making again (the drawable changed) */
static inline void glf_changed(struct gl_state *st)
{
	st->raster_stale = true;
}

/* glEnable and glDisable; false for what isn't a capability. glIsEnabled: -1 for that. */
bool glf_enable(struct gl_state *st, uint32_t cap, bool on);
int glf_is_enabled(const struct gl_state *st, uint32_t cap);

/* State: as gl* (each checks its arguments, and sets an error if they're wrong) */
void glf_viewport(struct gl_state *st, int32_t x, int32_t y, int32_t w, int32_t h);
void glf_scissor(struct gl_state *st, int32_t x, int32_t y, int32_t w, int32_t h);
void glf_depth_range(struct gl_state *st, double n, double f);
void glf_depth_func(struct gl_state *st, uint32_t func);
void glf_depth_mask(struct gl_state *st, bool on);
void glf_color_mask(struct gl_state *st, bool r, bool g, bool b, bool a);
void glf_blend_func(struct gl_state *st, uint32_t src, uint32_t dst);
void glf_blend_equation(struct gl_state *st, uint32_t eq);
void glf_blend_color(struct gl_state *st, float r, float g, float b, float a);
void glf_alpha_func(struct gl_state *st, uint32_t func, float ref);
void glf_cull_face(struct gl_state *st, uint32_t mode);
void glf_front_face(struct gl_state *st, uint32_t mode);
void glf_shade_model(struct gl_state *st, uint32_t mode);
void glf_point_size(struct gl_state *st, float size);
void glf_line_width(struct gl_state *st, float width);
void glf_active_texture(struct gl_state *st, uint32_t unit);

/* Matrices: glMatrixMode, glLoadIdentity, glLoadMatrix, glMultMatrix (column-major), and the rest */
void glf_matrix_mode(struct gl_state *st, uint32_t mode);
void glf_load(struct gl_state *st, const float m[16]);
void glf_mult(struct gl_state *st, const float m[16]);
void glf_identity(struct gl_state *st);
void glf_translate(struct gl_state *st, float x, float y, float z);
void glf_scale(struct gl_state *st, float x, float y, float z);
void glf_rotate(struct gl_state *st, float angle, float x, float y, float z);
void glf_frustum(struct gl_state *st, double l, double r, double b, double t, double n, double f);
void glf_ortho(struct gl_state *st, double l, double r, double b, double t, double n, double f);
void glf_push(struct gl_state *st);
void glf_pop(struct gl_state *st);
/* The matrix glMatrixMode's stack has on top */
float *glf_top(struct gl_state *st);

/* sin and cos of an angle in degrees: the same on every host */
void glf_sincos(double degrees, double *s, double *c);

/* glBegin; a vertex (object coordinates x, y, z, w) with the current attributes; glEnd */
void glf_begin(struct gl_state *st, uint32_t mode);
void glf_vertex(struct gl_state *st, const struct gl_target *t, const float v[4]);
void glf_end(struct gl_state *st, const struct gl_target *t);

/* glClear (GL_COLOR_BUFFER_BIT and GL_DEPTH_BUFFER_BIT; the others are accepted) */
void glf_clear(struct gl_state *st, const struct gl_target *t, uint32_t mask);

/* A state's values (glGetDoublev's: up to 16); 0 for what the front end doesn't keep */
unsigned glf_get(struct gl_state *st, uint32_t pname, double *v);

#endif
