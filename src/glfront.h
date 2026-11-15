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
       GL_SCISSOR_TEST = 0x0C11, GL_TEXTURE_1D = 0x0DE0, GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE_3D = 0x806F,
       GL_TEXTURE_GEN_S = 0x0C60,
       GL_LIGHT0 = 0x4000, GL_NORMALIZE = 0x0BA1, GL_RESCALE_NORMAL = 0x803A, GL_COLOR_MATERIAL = 0x0B57,
       GL_COLOR_SUM_EXT = 0x8458, GL_CLIP_PLANE0 = 0x3000, GL_POLYGON_OFFSET_POINT = 0x2A01,
       GL_POLYGON_OFFSET_LINE = 0x2A02, GL_POLYGON_OFFSET_FILL = 0x8037, GL_STENCIL_TEST = 0x0B90 };
/* Lighting, fog, texture coordinate generation */
enum { GL_AMBIENT = 0x1200, GL_DIFFUSE, GL_SPECULAR, GL_POSITION, GL_SPOT_DIRECTION, GL_SPOT_EXPONENT,
       GL_SPOT_CUTOFF, GL_CONSTANT_ATTENUATION, GL_LINEAR_ATTENUATION, GL_QUADRATIC_ATTENUATION };
enum { GL_EMISSION = 0x1600, GL_SHININESS, GL_AMBIENT_AND_DIFFUSE, GL_COLOR_INDEXES };
enum { GL_LIGHT_MODEL_LOCAL_VIEWER = 0x0B51, GL_LIGHT_MODEL_TWO_SIDE, GL_LIGHT_MODEL_AMBIENT,
       GL_LIGHT_MODEL_COLOR_CONTROL = 0x81F8, GL_SINGLE_COLOR, GL_SEPARATE_SPECULAR_COLOR };
enum { GL_FOG_INDEX = 0x0B61, GL_FOG_DENSITY, GL_FOG_START, GL_FOG_END, GL_FOG_MODE, GL_FOG_COLOR,
       GL_EXP = 0x0800, GL_EXP2 };
enum { GL_S = 0x2000, GL_T, GL_R, GL_Q, GL_TEXTURE_GEN_MODE = 0x2500, GL_OBJECT_PLANE, GL_EYE_PLANE,
       GL_EYE_LINEAR = 0x2400, GL_OBJECT_LINEAR, GL_SPHERE_MAP, GL_NORMAL_MAP_NV = 0x8511, GL_REFLECTION_MAP_NV };
/* Polygons, stencil, hints */
enum { GL_POINT = 0x1B00, GL_LINE, GL_FILL };
enum { GL_DONT_CARE = 0x1100, GL_FASTEST, GL_NICEST };
enum { GL_KEEP = 0x1E00, GL_INCR = 0x1E02, GL_DECR, GL_INVERT = 0x150A, GL_INCR_WRAP_EXT = 0x8507,
       GL_DECR_WRAP_EXT };
/* Types of arrays' and indices' values */
enum { GL_BYTE = 0x1400, GL_SHORT = 0x1402, GL_UNSIGNED_SHORT, GL_INT, GL_UNSIGNED_INT, GL_FLOAT,
       GL_DOUBLE = 0x140A };
/* Pixels */
enum { GL_COLOR_INDEX = 0x1900, GL_STENCIL_INDEX, GL_DEPTH_COMPONENT, GL_RED, GL_GREEN, GL_BLUE, GL_BITMAP = 0x1A00,
       GL_COLOR = 0x1800, GL_DEPTH, GL_STENCIL };

#define GLF_GUARD 4.0f                  /* the guard band, in view volumes (see above) */
#define GLF_MAX_SIZE 4096               /* viewports, drawables */
#define GLF_MODELVIEW_DEPTH 32          /* matrix stacks' depths */
#define GLF_PROJECTION_DEPTH 8
#define GLF_TEXTURE_DEPTH 8
#define GLF_TEXTURE_UNITS RASTER_UNITS
#define GLF_MAX_TEXTURE_SIZE 1024
#define GLF_TEXTURE_LEVELS 11                   /* (1024 down to 1) */
#define GLF_MAX_3D_TEXTURE_SIZE 128             /* (3D textures': 8 levels) */
#define GLF_LIGHTS 8
#define GLF_CLIP_PLANES 6
#define GLF_ATTRIB_DEPTH 16                     /* glPushAttrib's stack, and glPushClientAttrib's */
#define GLF_MAX_ELEMENTS 65536                  /* (GL_MAX_ELEMENTS_VERTICES and _INDICES: advice) */
#define GLF_PIXEL_MAP 256                       /* (GL_MAX_PIXEL_MAP_TABLE) */
_Static_assert(GLF_TEXTURE_LEVELS <= RASTER_MAX_LEVELS, "raster.h's levels");

struct gl_matrices {
	float m[GLF_MODELVIEW_DEPTH][16];       /* column-major, as OpenGL's; the top is m[depth - 1] */
	unsigned depth, max;
};

/* A vertex on its way: clip coordinates, then what's interpolated (all floats: clipping interpolates them) */
struct gl_vertex {
	float clip[4];
	float color[2][4];                      /* r, g, b, a (clamped): the front's, the back's */
	float secondary[2][4];                  /* (lighting's specular, or the secondary color) */
	float tex[GLF_TEXTURE_UNITS][4];
	float fog;                              /* the fog coordinate (eye-space |z|) */
	float user[GLF_CLIP_PLANES];            /* distances from the user clip planes (enabled ones) */
	float edge;                             /* (polygon modes: the edge from this vertex to the next is drawn) */
};

/*
 * Indexes of the capabilities in gl_state.caps (glfront.c's table has their
 * names, these first; per texture unit ones have bit k for unit k)
 */
enum { GLF_CAP_DEPTH_TEST, GLF_CAP_CULL_FACE, GLF_CAP_BLEND, GLF_CAP_ALPHA_TEST, GLF_CAP_SCISSOR_TEST,
       GLF_CAP_DITHER, GLF_CAP_TEXTURE_1D, GLF_CAP_TEXTURE_2D, GLF_CAP_TEXTURE_GEN_S,     /* (s, t, r, q) */
       GLF_CAP_LIGHTING = GLF_CAP_TEXTURE_GEN_S + 4, GLF_CAP_LIGHT0, GLF_CAP_NORMALIZE = GLF_CAP_LIGHT0 + GLF_LIGHTS,
       GLF_CAP_RESCALE_NORMAL, GLF_CAP_COLOR_MATERIAL, GLF_CAP_FOG, GLF_CAP_COLOR_SUM, GLF_CAP_CLIP_PLANE0,
       GLF_CAP_OFFSET_POINT = GLF_CAP_CLIP_PLANE0 + GLF_CLIP_PLANES, GLF_CAP_OFFSET_LINE, GLF_CAP_OFFSET_FILL,
       GLF_CAP_STENCIL_TEST, GLF_CAP_TEXTURE_3D, GLF_CAP_NAMED, GLF_CAPS = 80 };

/* A texture object (1D, 2D or 3D): its levels, its parameters */
enum { GLF_1D, GLF_2D, GLF_3D, GLF_TARGETS };
struct gl_level {
	int width, height, depth;               /* (0: not specified; depth 1 but in 3D textures) */
	uint32_t internal, base;                /* as given; its base format (GL_ALPHA ... GL_RGBA) */
	uint32_t *texels;                       /* 0xAARRGGBB, row 0 at t = 0, slice 0 at r = 0 (see gltex.c) */
};
struct gl_texture {
	uint32_t name;
	int target;                             /* GLF_1D, GLF_2D or GLF_3D (-1: not bound yet) */
	struct gl_level level[GLF_TEXTURE_LEVELS];
	uint32_t min_filter, mag_filter, wrap_s, wrap_t, wrap_r;
	float border[4], priority, min_lod, max_lod;
	int base_level, max_level;
	struct raster_texture *drawn;           /* what's drawn with (its texels are the levels'), or NULL */
	unsigned serial;                        /* (changes when drawing with it would) */
	struct gl_texture *next;                /* (in its namespace's chain) */
};

/* A display list: what it holds is the libraries' (opengl.c's entries) */
struct gl_list {
	uint32_t name;
	uint8_t *data;
	size_t len;
	struct gl_list *next;                   /* (in its namespace's chain) */
};

/* The texture objects and display lists contexts share (aglCreateContext's share context) */
#define GLF_NAME_CHAINS 256
struct gl_names {
	unsigned users;
	struct gl_texture *chain[GLF_NAME_CHAINS];
	uint32_t next_name;                     /* glGenTextures' next */
	struct gl_list *lists[GLF_NAME_CHAINS];
	uint32_t next_list;                     /* (where glGenLists looks first) */
	struct raster_texture **retired;        /* to be freed when nothing recorded can use them */
	unsigned nretired, retired_cap;
};

/* glPixelStore's, for unpacking (images from the program) or packing (to it) */
struct gl_pixel_store {
	int32_t row_length, skip_rows, skip_pixels, alignment;
	bool swap_bytes, lsb_first;
	int32_t image_height, skip_images;      /* (3D images') */
};

/* A texture unit's */
struct gl_unit {
	struct gl_texture *bound[GLF_TARGETS];
	uint32_t env_mode;
	float env_color[4];                     /* r, g, b, a */
};

/* Lighting (gllight.c): a light, in eye coordinates as it was given; a face's material */
struct gl_light {
	float ambient[4], diffuse[4], specular[4];
	float position[4], spot_direction[3];
	float spot_exponent, spot_cutoff;
	float attenuation[3];                   /* constant, linear, quadratic */
};
struct gl_material {
	float ambient[4], diffuse[4], specular[4], emission[4];
	float shininess;
};

/* A texture coordinate's generation (s, t, r or q of a unit): GL_EYE_LINEAR... and its planes */
struct gl_texgen {
	uint32_t mode;
	float object[4], eye[4];                /* (the eye plane in eye coordinates, as it was given) */
};

/* A client array (glVertexPointer's and the rest): where it is in the program's memory, and how */
enum { GLF_ARRAY_VERTEX, GLF_ARRAY_NORMAL, GLF_ARRAY_COLOR, GLF_ARRAY_SECONDARY, GLF_ARRAY_EDGE, GLF_ARRAY_INDEX,
       GLF_ARRAY_TEX0, GLF_ARRAYS = GLF_ARRAY_TEX0 + GLF_TEXTURE_UNITS };
struct gl_array {
	bool enabled;
	int32_t size, stride;                   /* (the stride as given: 0 for packed) */
	uint32_t type, pointer;
	/* (for drawing: elements [first, first + count) on the host, element `first` at data; NULL: none) */
	const uint8_t *data;
	uint32_t first, count;
};

/*
 * The raster position (glRasterPos): window coordinates (x, y in pixels up
 * from the bottom left, z 0..1) and clip w; whether it's valid; the color,
 * texture coordinates and distance it was given
 */
struct gl_raster_pos {
	float win[4];
	bool valid;
	float color[4], secondary[4], tex[GLF_TEXTURE_UNITS][4], distance;
};

/* The pixel transfer (glPixelTransfer) and zoom (glPixelZoom); the pixel maps (glPixelMap) */
struct gl_transfer {
	bool map_color, map_stencil;
	int32_t index_shift, index_offset;
	float scale[4], bias[4];                /* r, g, b, a */
	float depth_scale, depth_bias;
	float zoom[2];
};
enum { GLF_MAP_I_TO_I, GLF_MAP_S_TO_S, GLF_MAP_I_TO_R, GLF_MAP_I_TO_G, GLF_MAP_I_TO_B, GLF_MAP_I_TO_A,
       GLF_MAP_R_TO_R, GLF_MAP_G_TO_G, GLF_MAP_B_TO_B, GLF_MAP_A_TO_A, GLF_MAPS };
struct gl_pixel_map {
	unsigned size;
	float v[GLF_PIXEL_MAP];
};

struct gl_saved;                                /* (glPushAttrib's) */
struct gl_client_saved;

/*
 * Evaluators (OpenGL 1.2's 5.1, gleval.c): each kind's one- and
 * two-dimensional map (in GL_MAP1_COLOR_4's order: color, index, normal,
 * texture coordinates of 1 to 4 components, vertex of 3 and 4), its order
 * in u and v (1 in v for a 1D map), domain and control points, [v][u][k];
 * made when first needed. The grids are in gl_state.
 */
#define GLF_EVAL_ORDER 8
#define GLF_EVAL_KINDS 9
struct gl_map {
	int32_t order[2];
	float domain[4];                        /* u1, u2, v1, v2 */
	float points[GLF_EVAL_ORDER * GLF_EVAL_ORDER * 4];
};
struct gl_eval {
	struct gl_map map[2][GLF_EVAL_KINDS];   /* 1D, 2D */
};

/*
 * Selection and feedback (OpenGL 1.2's 5.2 and 5.3, glselect.c): the render
 * mode, the program's buffers as given (opengl.c writes the records into
 * them when glRenderMode leaves the mode), the records so far (words:
 * integers when selecting, floats' bits for feedback; those past the
 * buffer's size counted, not kept), the name stack and the hit so far
 */
#define GLF_NAME_STACK 64
struct gl_select {
	uint32_t mode;                          /* GL_RENDER, GL_SELECT or GL_FEEDBACK */
	uint32_t select_buffer, feedback_buffer;
	int32_t select_size, feedback_size;     /* (words) */
	uint32_t feedback_type;
	uint32_t *words;
	size_t count, cap;
	uint32_t names[GLF_NAME_STACK];
	unsigned depth;
	bool hit;
	float zmin, zmax;
	uint32_t hits;
	bool line_reset;                        /* (the next line fed back resets the stipple: GL_LINE_RESET_TOKEN) */
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
	uint32_t polygon_mode[2];               /* the front's, the back's */
	float offset_factor, offset_units;
	int32_t line_stipple_factor;
	uint16_t line_stipple;
	uint8_t polygon_stipple[128];           /* (kept: neither stipple is drawn) */
	/* stencil */
	uint32_t stencil_func, stencil_fail, stencil_zfail, stencil_zpass;
	int32_t stencil_ref, clear_stencil;
	uint32_t stencil_mask, stencil_write_mask;
	/* hints: perspective correction, points, lines, polygons, fog, clip volume clipping, transform (Apple's) */
	uint32_t hints[7];
	/* lighting, fog (gllight.c) */
	struct gl_light lights[GLF_LIGHTS];
	struct gl_material material[2];         /* the front's, the back's */
	float light_model_ambient[4];
	bool local_viewer, two_side, separate_specular;
	uint32_t color_material_face, color_material_mode;
	uint32_t fog_mode;
	float fog_density, fog_start, fog_end, fog_color[4];
	/* texture coordinate generation, user clip planes (eye coordinates) */
	struct gl_texgen texgen[GLF_TEXTURE_UNITS][4];
	float clip_planes[GLF_CLIP_PLANES][4];
	/* client arrays (glarray.c) */
	struct gl_array arrays[GLF_ARRAYS];
	int32_t lock_first, lock_count;         /* glLockArraysEXT's */
	uint32_t list_base;                     /* glListBase's */
	/* the raster position, the pixel transfer, the pixel maps (glpixel.c) */
	struct gl_raster_pos rpos;
	struct gl_transfer transfer;
	struct gl_pixel_map maps[GLF_MAPS];
	/* glPushAttrib's and glPushClientAttrib's stacks */
	struct gl_saved *attrib_stack[GLF_ATTRIB_DEPTH];
	struct gl_client_saved *client_stack[GLF_ATTRIB_DEPTH];
	unsigned attrib_depth, client_depth;
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
	float normal_matrix[9], rescale;        /* (the modelview's upper 3x3 inverted, for normals; its scale) */
	bool normal_stale;
	bool texmat_identity[GLF_TEXTURE_UNITS], texmat_stale;
	/* current attributes */
	float color[4], secondary_color[4], normal[3], tex[GLF_TEXTURE_UNITS][4], fog_coord;
	bool edge_flag;
	/* between glBegin and glEnd */
	bool inside;
	uint32_t mode;
	unsigned count;                         /* vertices since glBegin */
	struct gl_vertex vb[3];                 /* (what the primitive so far needs) */
	int polygon_face;                       /* (the last triangle's: 0 front, 1 back, -1 not drawn; and */
	float polygon_offset;                   /* its offset: for a polygon's closing edge in polygon modes) */
	/* the raster state drawing takes, made again when the GL state changes */
	struct raster_state raster;
	bool raster_stale, draw_none, textured;
	int raster_width, raster_height;
	const struct gl_texture *raster_tex[GLF_TEXTURE_UNITS];        /* (the textures it has, and their serials) */
	unsigned raster_serial[GLF_TEXTURE_UNITS];
	unsigned long primitives;               /* triangles, lines and points handed to the backend */
	struct gl_select select;
	/* evaluators: the maps (NULL: none yet, the initial ones), the grids (glMapGrid's) */
	struct gl_eval *eval;
	int32_t grid1_n, grid2_n[2];
	float grid1[2], grid2[4];
};

/* Where a context's drawing goes: its buffers (NULL: nowhere), their size, the pixel format's bits */
struct gl_target {
	struct surface *surface;
	int width, height;
	unsigned depth_bits, alpha_bits, stencil_bits;
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
/* A capability's index in gl_state.caps (-1: not one) */
int glf_cap(uint32_t cap);

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
void glf_polygon_mode(struct gl_state *st, uint32_t face, uint32_t mode);
void glf_polygon_offset(struct gl_state *st, float factor, float units);
void glf_line_stipple(struct gl_state *st, int32_t factor, uint16_t pattern);
void glf_hint(struct gl_state *st, uint32_t target, uint32_t mode);
void glf_stencil_func(struct gl_state *st, uint32_t func, int32_t ref, uint32_t mask);
void glf_stencil_op(struct gl_state *st, uint32_t fail, uint32_t zfail, uint32_t zpass);
void glf_stencil_mask(struct gl_state *st, uint32_t mask);

/*
 * glPushAttrib and glPopAttrib; glPushClientAttrib and glPopClientAttrib.
 * glf_free() lets go of what they keep (and glf_detach() does it too).
 */
void glf_push_attrib(struct gl_state *st, uint32_t mask);
void glf_pop_attrib(struct gl_state *st);
void glf_push_client_attrib(struct gl_state *st, uint32_t mask);
void glf_pop_client_attrib(struct gl_state *st);
void glf_free(struct gl_state *st);

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

/* glClear (GL_COLOR_BUFFER_BIT, GL_DEPTH_BUFFER_BIT and GL_STENCIL_BUFFER_BIT; the accumulation buffer's is taken) */
void glf_clear(struct gl_state *st, const struct gl_target *t, uint32_t mask);

/* A state's values (glGetDoublev's: up to 16); 0 for what the front end doesn't keep */
unsigned glf_get(struct gl_state *st, uint32_t pname, double *v);

/* ---- lighting, fog, texture coordinate generation, user clip planes (gllight.c) ---- */

/* (glf_init()'s part) */
void glf_lighting_init(struct gl_state *st);

/*
 * glLight, glLightModel, glMaterial, glColorMaterial, glFog: v has as many
 * values as pname takes (integers given as GL converts them: colors to
 * -1..1, the rest as they are)
 */
void glf_light(struct gl_state *st, uint32_t light, uint32_t pname, const float *v);
void glf_light_model(struct gl_state *st, uint32_t pname, const float *v);
void glf_material(struct gl_state *st, uint32_t face, uint32_t pname, const float *v);
void glf_color_material(struct gl_state *st, uint32_t face, uint32_t mode);
void glf_fog(struct gl_state *st, uint32_t pname, const float *v);
/* glTexGen (for the active texture unit) and glClipPlane */
void glf_tex_gen(struct gl_state *st, uint32_t coord, uint32_t pname, const double *v);
void glf_clip_plane(struct gl_state *st, uint32_t plane, const double eq[4]);
/* glGetLight, glGetMaterial, glGetTexGen, glGetClipPlane: the values, how many (0: an error, set) */
unsigned glf_get_light(struct gl_state *st, uint32_t light, uint32_t pname, double *v);
unsigned glf_get_material(struct gl_state *st, uint32_t face, uint32_t pname, double *v);
unsigned glf_get_tex_gen(struct gl_state *st, uint32_t coord, uint32_t pname, double *v);
unsigned glf_get_clip_plane(struct gl_state *st, uint32_t plane, double *v);
/* glf_get()'s for lighting's and fog's state (0: not theirs) */
unsigned glf_get_lighting(struct gl_state *st, uint32_t pname, double *v);

/*
 * A vertex's eye-space work: its colors (lit, or the current ones), fog
 * coordinate, texture coordinates (generated, then through the texture
 * matrices) and clip distances, from its object coordinates and the
 * current attributes; clip already set.
 */
void glf_shade(struct gl_state *st, const float obj[4], struct gl_vertex *v);

/* ---- client arrays, glDrawArrays and glDrawElements (glarray.c) ---- */

/* (glf_init()'s part) */
void glf_arrays_init(struct gl_state *st);

/* glVertexPointer and the rest (array: GLF_ARRAY_..., the client active unit's for texture coordinates) */
void glf_array_pointer(struct gl_state *st, unsigned array, int32_t size, uint32_t type, int32_t stride,
		       uint32_t pointer);
/* glEnableClientState and glDisableClientState; false for what isn't an array. Is it enabled (-1: not an array)? */
bool glf_enable_client(struct gl_state *st, uint32_t cap, bool on);
int glf_array_enabled(const struct gl_state *st, uint32_t cap);
void glf_interleaved_arrays(struct gl_state *st, uint32_t format, int32_t stride, uint32_t pointer);
void glf_lock_arrays(struct gl_state *st, int32_t first, int32_t count);
void glf_unlock_arrays(struct gl_state *st);
/* An array's (GLF_ARRAY_...) stride, as drawn; and the bytes elements [0, n) take from its pointer */
uint32_t glf_array_stride(const struct gl_array *a);
uint64_t glf_array_bytes(const struct gl_array *a, uint32_t n);
/* glf_get()'s for the arrays' state (0: not theirs); glGetPointerv's (false: not an array's) */
unsigned glf_get_arrays(struct gl_state *st, uint32_t pname, double *v);
bool glf_get_pointer(const struct gl_state *st, uint32_t pname, uint32_t *p);

/*
 * Drawing from the arrays: each enabled one's data must be set (to the
 * bytes on the host of the elements that can be read: see gl_array).
 * Elements outside them leave that array's attribute as it is.
 * glArrayElement; glDrawArrays; glDrawElements and glDrawRangeElements
 * (the indices big-endian, as type).
 */
void glf_array_element(struct gl_state *st, const struct gl_target *t, int32_t i);
void glf_draw_arrays(struct gl_state *st, const struct gl_target *t, uint32_t mode, int32_t first, int32_t count);
void glf_draw_elements(struct gl_state *st, const struct gl_target *t, uint32_t mode, int32_t count, uint32_t type,
		       const uint8_t *indices);
/* The bytes an index of this type takes (0: not a type of index) */
unsigned glf_index_size(uint32_t type);

/*
 * The values element i has in the enabled arrays, as glArrayElement would
 * set them (bit GLF_ARRAY_k of `has`: array k has one; the rest of each
 * vector as glArrayElement leaves it: 0, 0, 0, 1)
 */
struct gl_element {
	unsigned has;
	float v[GLF_ARRAYS][4];
};
void glf_array_values(const struct gl_state *st, int32_t i, struct gl_element *e);

/* ---- display lists (gllist.c): their names and contents ---- */

/*
 * glGenLists (the first of `range` names, each an empty list; 0 if there
 * are none), glDeleteLists, glIsList; a list (NULL: none of that name);
 * what a list holds from now on (data from malloc(), the list's then;
 * false if there's no memory, data freed); every list freed with the
 * namespace (glf_detach()'s).
 */
uint32_t glf_gen_lists(struct gl_state *st, int32_t range);
void glf_delete_lists(struct gl_state *st, uint32_t first, int32_t range);
bool glf_is_list(struct gl_state *st, uint32_t name);
const struct gl_list *glf_list(struct gl_state *st, uint32_t name);
bool glf_define_list(struct gl_state *st, uint32_t name, uint8_t *data, size_t len);
void glf_free_lists(struct gl_names *n);

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

/*
 * glTexImage1D and 2D (h 1 for 1D) and glTexSubImage: the image from
 * `pixels` (the program's memory, big-endian, glf_image_bytes() of it; NULL:
 * none, for glTexImage)
 */
void glf_tex_image(struct gl_state *st, uint32_t target, int32_t level, int32_t internal, int32_t w, int32_t h,
		   int32_t border, uint32_t format, uint32_t type, const uint8_t *pixels);
void glf_tex_sub_image(struct gl_state *st, uint32_t target, int32_t level, int32_t x, int32_t y, int32_t w,
		       int32_t h, uint32_t format, uint32_t type, const uint8_t *pixels);
/* glTexImage3D and glTexSubImage3D (pixels: glf_image_bytes_3d() of them), glCopyTexSubImage3D */
void glf_tex_image_3d(struct gl_state *st, uint32_t target, int32_t level, int32_t internal, int32_t w, int32_t h,
		      int32_t d, int32_t border, uint32_t format, uint32_t type, const uint8_t *pixels);
void glf_tex_sub_image_3d(struct gl_state *st, uint32_t target, int32_t level, int32_t x, int32_t y, int32_t z,
			  int32_t w, int32_t h, int32_t d, uint32_t format, uint32_t type, const uint8_t *pixels);
void glf_copy_tex_sub_image_3d(struct gl_state *st, const struct gl_target *t, uint32_t target, int32_t level,
			       int32_t xoffset, int32_t yoffset, int32_t zoffset, int32_t x, int32_t y, int32_t w,
			       int32_t h);

/* glTexParameter (values: as many as pname takes) and glTexEnv; glGetTexParameter, glGetTexEnv, glGetTexLevelParameter */
void glf_tex_parameter(struct gl_state *st, uint32_t target, uint32_t pname, const float *v);
void glf_tex_env(struct gl_state *st, uint32_t target, uint32_t pname, const float *v);
unsigned glf_get_tex_parameter(struct gl_state *st, uint32_t target, uint32_t pname, double *v);
unsigned glf_get_tex_env(struct gl_state *st, uint32_t target, uint32_t pname, double *v);
unsigned glf_get_tex_level_parameter(struct gl_state *st, uint32_t target, int32_t level, uint32_t pname, double *v);

/*
 * For drawing: a unit's texture, if it's enabled and complete (its
 * raster_texture made if need be), else NULL
 */
struct gl_texture *glf_unit_texture(struct gl_state *st, unsigned unit, struct raster_texture **drawn);

/* A backend texture retired at once (freed by glf_free_retired()) */
void glf_retire(struct gl_names *n, struct raster_texture *r);

/*
 * glCopyTexImage1D and 2D (h 1 for 1D), glCopyTexSubImage1D and 2D: from
 * the target's pixels at (x, y) (finished), through the pixel transfer
 */
void glf_copy_tex_image(struct gl_state *st, const struct gl_target *t, uint32_t target, int32_t level,
			int32_t internal, int32_t x, int32_t y, int32_t w, int32_t h, int32_t border);
void glf_copy_tex_sub_image(struct gl_state *st, const struct gl_target *t, uint32_t target, int32_t level,
			    int32_t xoffset, int32_t yoffset, int32_t x, int32_t y, int32_t w, int32_t h);
/* glGetTexImage: the level into `out` (the program's memory, as glf_tex_image_bytes() says it takes) */
void glf_get_tex_image(struct gl_state *st, uint32_t target, int32_t level, uint32_t format, uint32_t type,
		       uint8_t *out);
size_t glf_tex_image_bytes(struct gl_state *st, uint32_t target, int32_t level, uint32_t format, uint32_t type);

/* ---- pixels (glpixel.c) ---- */

/* (glf_init()'s part: the raster position, the pixel transfer and maps) */
void glf_pixels_init(struct gl_state *st);

/* glPixelStore, glPixelTransfer, glPixelZoom, glPixelMap (v as floats: colors 0..1), glGetPixelMap (how many) */
void glf_pixel_store(struct gl_state *st, uint32_t pname, int32_t v);
void glf_pixel_transfer(struct gl_state *st, uint32_t pname, float v);
void glf_pixel_zoom(struct gl_state *st, float x, float y);
void glf_pixel_map(struct gl_state *st, uint32_t map, int32_t size, const float *v);
unsigned glf_get_pixel_map(struct gl_state *st, uint32_t map, float *v);
/* glf_get()'s for the pixel transfer, maps and zoom and the raster position (0: not theirs) */
unsigned glf_get_pixels(struct gl_state *st, uint32_t pname, double *v);

/* Whether a format and type go together (GL_NO_ERROR), or the error they make */
uint32_t glf_pixel_check(uint32_t format, uint32_t type);

/*
 * The bytes of the program's memory an image (or a part, for glReadPixels)
 * of this size, format and type takes as packed or unpacked by `ps`; 0 if
 * the format or type isn't one OpenGL takes (or they don't go together)
 */
size_t glf_image_bytes(const struct gl_pixel_store *ps, uint32_t format, uint32_t type, int32_t w, int32_t h);
/* The same for a 3D image (d images of w x h, image_height rows apart, skip_images of them first); one's stride */
size_t glf_image_bytes_3d(const struct gl_pixel_store *ps, uint32_t format, uint32_t type, int32_t w, int32_t h,
			  int32_t d);
size_t glf_image_stride(const struct gl_pixel_store *ps, uint32_t format, uint32_t type, int32_t w, int32_t h);

/*
 * An image (from `src`, as `ps` unpacks it; a color format, or color
 * indices) as R, G, B, A bytes (w x h, rows from the first), through the
 * pixel transfer if `transfer` (color indices always through their maps);
 * false if the format and type aren't such an image's
 */
bool glf_unpack_rgba(const struct gl_state *st, const struct gl_pixel_store *ps, uint32_t format, uint32_t type,
		     int32_t w, int32_t h, const uint8_t *src, uint8_t *rgba, bool transfer);
/* R, G, B, A bytes (w x h) into `out` as the pack store packs a color format and type (no pixel transfer) */
void glf_pack_rgba(const struct gl_state *st, uint32_t format, uint32_t type, int32_t w, int32_t h,
		   const uint8_t *rgba, uint8_t *out);

/*
 * glReadPixels: [x, x + w) x [y, y + h) (OpenGL's: from the bottom) of the
 * target's pixels (finished; depth and stencil read too, for them) into
 * `out` (the program's memory)
 */
void glf_read_pixels(struct gl_state *st, const struct gl_target *t, int32_t x, int32_t y, int32_t w, int32_t h,
		     uint32_t format, uint32_t type, uint8_t *out);
/* The same target's colors as R, G, B, A bytes (w x h), through the pixel transfer (glCopyTexImage's) */
void glf_read_rgba(const struct gl_state *st, const struct gl_target *t, int32_t x, int32_t y, int32_t w, int32_t h,
		   uint8_t *rgba);

/*
 * glRasterPos (glfront.c's); glBitmap (bits as unpacked: NULL, none drawn),
 * glDrawPixels, glCopyPixels (the target finished, and its depth and
 * stencil read: see glf_read_pixels())
 */
void glf_raster_pos(struct gl_state *st, const float obj[4]);
void glf_bitmap(struct gl_state *st, const struct gl_target *t, int32_t w, int32_t h, float xorig, float yorig,
		float xmove, float ymove, const uint8_t *bits);
void glf_draw_pixels(struct gl_state *st, const struct gl_target *t, int32_t w, int32_t h, uint32_t format,
		     uint32_t type, const uint8_t *pixels);
void glf_copy_pixels(struct gl_state *st, const struct gl_target *t, int32_t x, int32_t y, int32_t w, int32_t h,
		     uint32_t type);

/* The raster state drawing in the target takes now (glfront.c's) */
const struct raster_state *glf_raster_state(struct gl_state *st, const struct gl_target *t);

/* ---- evaluators (gleval.c) ---- */

/* A map's target's components (0: not a map's target): 1D's or 2D's (*dims) */
unsigned glf_map_components(uint32_t target, unsigned *dims);

/*
 * glMap1 and glMap2 (dims): the domain (u1, u2, v1, v2), the strides and
 * orders as given (for the errors), the control points packed ([v][u][k],
 * as many as the orders take; NULL: not there, nothing done but the errors)
 */
void glf_map(struct gl_state *st, unsigned dims, uint32_t target, const double domain[4], int32_t ustride,
	     int32_t uorder, int32_t vstride, int32_t vorder, const float *points);

/* glMapGrid1 and 2 */
void glf_map_grid1(struct gl_state *st, int32_t n, float u1, float u2);
void glf_map_grid2(struct gl_state *st, int32_t un, float u1, float u2, int32_t vn, float v1, float v2);

/* glEvalCoord1 and 2 (dims), glEvalPoint1 and 2, glEvalMesh1 and 2 */
void glf_eval_coord(struct gl_state *st, const struct gl_target *t, unsigned dims, float u, float v);
void glf_eval_point(struct gl_state *st, const struct gl_target *t, unsigned dims, int32_t i, int32_t j);
void glf_eval_mesh(struct gl_state *st, const struct gl_target *t, unsigned dims, uint32_t mode, int32_t i1,
		   int32_t i2, int32_t j1, int32_t j2);

/*
 * glGetMap: a map's coefficients, orders or domain (GL_COEFF, GL_ORDER,
 * GL_DOMAIN) into v (room for GLF_EVAL_ORDER^2 x 4); how many (0: an error)
 */
unsigned glf_get_map(struct gl_state *st, uint32_t target, uint32_t query, double *v);

/* ---- selection and feedback (glselect.c) ---- */

enum { GL_RENDER = 0x1C00, GL_FEEDBACK, GL_SELECT };
enum { GL_PASS_THROUGH_TOKEN = 0x0700, GL_POINT_TOKEN, GL_LINE_TOKEN, GL_POLYGON_TOKEN, GL_BITMAP_TOKEN,
       GL_DRAW_PIXEL_TOKEN, GL_COPY_PIXEL_TOKEN, GL_LINE_RESET_TOKEN };

/* glSelectBuffer, glFeedbackBuffer (buffer: the program's address, kept as given) */
void glf_select_buffer(struct gl_state *st, int32_t size, uint32_t buffer);
void glf_feedback_buffer(struct gl_state *st, int32_t size, uint32_t type, uint32_t buffer);

/*
 * glRenderMode: the result for the mode left (hits or values; -1 if they
 * overflowed the buffer), and the records to go into the program's buffer
 * (*buffer; *words, *count of them, the front end's until the next call)
 */
int32_t glf_render_mode(struct gl_state *st, uint32_t mode, uint32_t *buffer, const uint32_t **words, size_t *count);

/* The name stack; glPassThrough */
void glf_init_names(struct gl_state *st);
void glf_load_name(struct gl_state *st, uint32_t name);
void glf_push_name(struct gl_state *st, uint32_t name);
void glf_pop_name(struct gl_state *st);
void glf_pass_through(struct gl_state *st, float token);

/* A vertex as feedback gives it: window coordinates (y up) and clip w, its color, unit 0's texture coordinates */
struct glf_fb_vertex {
	float win[4];
	float color[4];
	float tex[4];
};

/*
 * Instead of drawing (the render mode isn't GL_RENDER): a primitive, clipped
 * (a point, a line or a polygon's token, or a raster position's: n 1), a
 * hit, or fed back
 */
void glf_select_primitive(struct gl_state *st, uint32_t token, const struct glf_fb_vertex *v, unsigned n);

/* Whether drawing goes to selection or feedback instead */
static inline bool glf_selecting(const struct gl_state *st)
{
	return st->select.mode != GL_RENDER;
}

#endif
