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
 * - GL_CLAMP clamps to the edge texels, as GL_CLAMP_TO_EDGE (the chip had
 *   no texture borders, and its drivers took GL_CLAMP so); texture images
 *   keep 8 bits a component whatever internal format is asked for.
 *
 * Textures (gltex.c) are objects of a namespace their contexts share, each
 * with its levels' texels on the host; what the backend draws with is a
 * raster_texture made from them when they're first drawn after a change
 * (sharing their texels), and retired when they change again, to be freed
 * once nothing recorded can use it (glf_free_retired()).
 */
#ifndef SYSHM_GLFRONT_H
#define SYSHM_GLFRONT_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu.h"
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
enum { GL_TEXTURE_ENV_MODE = 0x2200, GL_TEXTURE_ENV_COLOR = 0x2201, GL_TEXTURE_ENV = 0x2300 };
enum { GL_MODULATE = 0x2100, GL_DECAL = 0x2101, GL_ADD = 0x0104, GL_REPLACE = 0x1E01 };   /* (and GL_BLEND) */
enum { GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T };
enum { GL_NEAREST = 0x2600, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_LINEAR_MIPMAP_NEAREST,
       GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR };
enum { GL_CLAMP = 0x2900, GL_REPEAT, GL_CLAMP_TO_EDGE = 0x812F };
enum { GL_UNSIGNED_BYTE = 0x1401 };
enum { GL_ALPHA = 0x1906, GL_RGB, GL_RGBA, GL_LUMINANCE, GL_LUMINANCE_ALPHA, GL_INTENSITY = 0x8049,
       GL_BGR = 0x80E0, GL_BGRA = 0x80E1 };
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
#define GLF_MAX_TEXTURE_SIZE 1024
#define GLF_TEXTURE_LEVELS 11                   /* (1024 down to 1) */
_Static_assert(GLF_TEXTURE_LEVELS <= RASTER_MAX_LEVELS, "raster.h's levels");

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
       GLF_CAP_DITHER, GLF_CAP_TEXTURE_1D, GLF_CAP_TEXTURE_2D, GLF_CAPS = 80 };

/* A texture object (1D or 2D): its levels, its parameters */
enum { GLF_1D, GLF_2D, GLF_TARGETS };
struct gl_level {
	int width, height;                      /* (0: not specified) */
	uint32_t internal, base;                /* as given; its base format (GL_ALPHA ... GL_RGBA) */
	uint32_t *texels;                       /* 0xAARRGGBB, row 0 at t = 0 (see gltex.c for each base format) */
};
struct gl_texture {
	uint32_t name;
	int target;                             /* GLF_1D or GLF_2D (-1: not bound yet) */
	struct gl_level level[GLF_TEXTURE_LEVELS];
	uint32_t min_filter, mag_filter, wrap_s, wrap_t;
	float border[4], priority, min_lod, max_lod;
	int base_level, max_level;
	struct raster_texture *drawn;           /* what's drawn with (its texels are the levels'), or NULL */
	unsigned serial;                        /* (changes when drawing with it would) */
	struct gl_texture *next;                /* (in its namespace's chain) */
};

/* The texture objects contexts share (aglCreateContext's share context) */
#define GLF_NAME_CHAINS 256
struct gl_names {
	unsigned users;
	struct gl_texture *chain[GLF_NAME_CHAINS];
	uint32_t next_name;                     /* glGenTextures' next */
	struct raster_texture **retired;        /* to be freed when nothing recorded can use them */
	unsigned nretired, retired_cap;
};

/* glPixelStore's, for unpacking (images from the program) or packing (to it) */
struct gl_pixel_store {
	int32_t row_length, skip_rows, skip_pixels, alignment;
	bool swap_bytes, lsb_first;
};

/* A texture unit's */
struct gl_unit {
	struct gl_texture *bound[GLF_TARGETS];
	uint32_t env_mode;
	float env_color[4];                     /* r, g, b, a */
};

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
	/* textures */
	struct gl_names *names;                 /* (the context's: see glf_attach()) */
	struct gl_texture defaults[GLF_TARGETS];        /* texture 0 */
	struct gl_unit units[GLF_TEXTURE_UNITS];
	struct gl_pixel_store pack, unpack;
	unsigned client_active_texture;
	/* matrices */
	uint32_t matrix_mode;
	unsigned active_texture;
	struct gl_matrices modelview, projection, texture[GLF_TEXTURE_UNITS];
	float mvp[16];                          /* projection times modelview */
	bool mvp_stale;
	bool texmat_identity[GLF_TEXTURE_UNITS], texmat_stale;
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
	bool raster_stale, draw_none, textured;
	int raster_width, raster_height;
	const struct gl_texture *raster_tex[GLF_TEXTURE_UNITS];        /* (the textures it has, and their serials) */
	unsigned raster_serial[GLF_TEXTURE_UNITS];
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

/*
 * A context's texture objects: a namespace of its own (share NULL), or the
 * share context's; false if there's no memory. glf_detach() lets go of
 * them (and the context's own default textures): the namespace is freed
 * with the last context, which mustn't have drawing recorded. gpu: what
 * drew them, if anything.
 */
bool glf_attach(struct gl_state *st, struct gl_state *share);
void glf_detach(struct gl_state *st, struct gpu *gpu);

/* aglCopyContext: the state, but not the textures (bindings to the defaults are to the destination's own) */
void glf_copy(struct gl_state *to, const struct gl_state *from);

/* Frees the backend textures retired (when nothing recorded anywhere can use them) */
void glf_free_retired(struct gl_names *n, struct gpu *gpu);

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

/* ---- textures (gltex.c) ---- */

/* (glf_init()'s part: the default textures, units, pixel stores) */
void glf_textures_init(struct gl_state *st);

/* glGenTextures' names (n of them, into out); glDeleteTextures; glIsTexture; glBindTexture */
void glf_gen_textures(struct gl_state *st, int32_t n, uint32_t *out);
void glf_delete_texture(struct gl_state *st, uint32_t name);
bool glf_is_texture(struct gl_state *st, uint32_t name);
const struct gl_texture *glf_texture(struct gl_state *st, uint32_t name);      /* (NULL: none of that name) */
void glf_bind_texture(struct gl_state *st, uint32_t target, uint32_t name);
/* A texture deleted (in another context of the namespace): bindings to it go back to the defaults */
void glf_unbind(struct gl_state *st, const struct gl_texture *t);

/* glPixelStore (false: not a parameter of it) */
void glf_pixel_store(struct gl_state *st, uint32_t pname, int32_t v);

/*
 * The bytes of the program's memory an image (or a part, for glReadPixels)
 * of this size, format and type takes as packed or unpacked by `ps`; 0 if
 * the format or type isn't one OpenGL takes (or they don't go together)
 */
size_t glf_image_bytes(const struct gl_pixel_store *ps, uint32_t format, uint32_t type, int32_t w, int32_t h);

/*
 * glTexImage1D and 2D (h 1 for 1D) and glTexSubImage: the image from
 * `pixels` (the program's memory, big-endian, glf_image_bytes() of it; NULL:
 * none, for glTexImage)
 */
void glf_tex_image(struct gl_state *st, uint32_t target, int32_t level, int32_t internal, int32_t w, int32_t h,
		   int32_t border, uint32_t format, uint32_t type, const uint8_t *pixels);
void glf_tex_sub_image(struct gl_state *st, uint32_t target, int32_t level, int32_t x, int32_t y, int32_t w,
		       int32_t h, uint32_t format, uint32_t type, const uint8_t *pixels);

/* glTexParameter (values: as many as pname takes) and glTexEnv; glGetTexParameter, glGetTexEnv, glGetTexLevelParameter */
void glf_tex_parameter(struct gl_state *st, uint32_t target, uint32_t pname, const float *v);
void glf_tex_env(struct gl_state *st, uint32_t target, uint32_t pname, const float *v);
unsigned glf_get_tex_parameter(struct gl_state *st, uint32_t target, uint32_t pname, double *v);
unsigned glf_get_tex_env(struct gl_state *st, uint32_t target, uint32_t pname, double *v);
unsigned glf_get_tex_level_parameter(struct gl_state *st, uint32_t target, int32_t level, uint32_t pname, double *v);

/*
 * glReadPixels: [x, x + w) x [y, y + h) (OpenGL's: from the bottom) of the
 * target's pixels (finished) into `out` (the program's memory)
 */
void glf_read_pixels(struct gl_state *st, const struct gl_target *t, int32_t x, int32_t y, int32_t w, int32_t h,
		     uint32_t format, uint32_t type, uint8_t *out);

/*
 * For drawing: a unit's texture, if it's enabled and complete (its
 * raster_texture made if need be), else NULL
 */
struct gl_texture *glf_unit_texture(struct gl_state *st, unsigned unit, struct raster_texture **drawn);

#endif
