/*
 * The 3D backend's software rasterizer: triangles, lines and points in
 * screen space into a color and depth buffer, with what the guest's 3D APIs
 * ask of hardware (QuickDraw 3D RAVE, OpenGL's fixed function, each through
 * a layer of its own): Gouraud shading, perspective-correct texturing from
 * up to RASTER_UNITS textures, combined in stages with the vertices' colors,
 * fog, alpha test, stencil, z functions, blending and color masks. Everything is
 * plain C on host floats, without fused multiply-adds, so the same input
 * gives the same pixels on every host.
 *
 * Coordinates are pixels, x right and y down, with pixel (i, j) covering
 * [i, i+1) x [j, j+1) and sampled at its center; edges follow the top-left
 * rule. Colors are 0xAARRGGBB, and colors in states a, r, g, b.
 */
#ifndef BLUE_RASTER_H
#define BLUE_RASTER_H

#include <stdbool.h>
#include <stdint.h>

#define RASTER_MAX_LEVELS 13
#define RASTER_UNITS 2                  /* texture units */
#define RASTER_COLORS 3                 /* colors per vertex */
#define RASTER_STAGES 4                 /* combining stages */

/*
 * A texture: a 2D one, or a 3D one if its levels have depths (each level's
 * width x height texels a slice, `depth` of them, slice k at r = k / depth)
 */
struct raster_texture {
	unsigned levels;
	unsigned width[RASTER_MAX_LEVELS], height[RASTER_MAX_LEVELS];
	unsigned depth[RASTER_MAX_LEVELS];      /* (0: a 2D texture) */
	uint32_t *texels[RASTER_MAX_LEVELS];    /* row k at t = k / height (bitmaps: rows top to bottom) */
	uint32_t gpu;                           /* the GPU backend's copy (gpu.c's; 0: none yet) */
};

/*
 * A vertex. Everything after x and y is interpolated linearly in screen
 * space, so what is to be perspective-correct comes divided by w: texture
 * coordinates as s/w, t/w and q/w (divided per pixel by q/w, or by 1/w for
 * units whose q is 1), and the fog coordinate as f/w.
 */
struct raster_vertex {
	float x, y, z, invw;                    /* pixels; depth; 1/w */
	float color[RASTER_COLORS][4];          /* r, g, b, a: 0 is the primary color, the stages use the others */
	float fog;                              /* fog coordinate / w (see fog_coord) */
	float tex[RASTER_UNITS][3];             /* each unit's s/w, t/w, q/w */
	float texr[RASTER_UNITS];               /* ... and r/w (3D textures') */
};

/* Values as RAVE's (kQAZFunction_ etc.) */
enum { RASTER_Z_NONE, RASTER_Z_LT, RASTER_Z_EQ, RASTER_Z_LE, RASTER_Z_GT, RASTER_Z_NE, RASTER_Z_GE,
       RASTER_Z_TRUE, RASTER_Z_FALSE };
enum { RASTER_BLEND_PREMULTIPLY, RASTER_BLEND_INTERPOLATE, RASTER_BLEND_FACTORS, RASTER_BLEND_NONE,
       RASTER_BLEND_LOGIC };
/* Logic operations on the color and the target's (as OpenGL's GL_CLEAR to GL_SET, in their order) */
enum { RASTER_LOGIC_CLEAR, RASTER_LOGIC_AND, RASTER_LOGIC_AND_REVERSE, RASTER_LOGIC_COPY, RASTER_LOGIC_AND_INVERTED,
       RASTER_LOGIC_NOOP, RASTER_LOGIC_XOR, RASTER_LOGIC_OR, RASTER_LOGIC_NOR, RASTER_LOGIC_EQUIV, RASTER_LOGIC_INVERT,
       RASTER_LOGIC_OR_REVERSE, RASTER_LOGIC_COPY_INVERTED, RASTER_LOGIC_OR_INVERTED, RASTER_LOGIC_NAND,
       RASTER_LOGIC_SET };
/* Blend factors (as OpenGL's GL_ZERO, GL_ONE, GL_SRC_COLOR...; the constant ones from blend_color) */
enum { RASTER_F_ZERO, RASTER_F_ONE, RASTER_F_SRC_COLOR, RASTER_F_ONE_MINUS_SRC_COLOR, RASTER_F_SRC_ALPHA,
       RASTER_F_ONE_MINUS_SRC_ALPHA, RASTER_F_DST_ALPHA, RASTER_F_ONE_MINUS_DST_ALPHA, RASTER_F_DST_COLOR,
       RASTER_F_ONE_MINUS_DST_COLOR, RASTER_F_SRC_ALPHA_SATURATE, RASTER_F_CONSTANT_COLOR,
       RASTER_F_ONE_MINUS_CONSTANT_COLOR, RASTER_F_CONSTANT_ALPHA, RASTER_F_ONE_MINUS_CONSTANT_ALPHA };
/* How the factored source and destination make the color (min and max take no factors) */
enum { RASTER_EQ_ADD, RASTER_EQ_SUBTRACT, RASTER_EQ_REVERSE_SUBTRACT, RASTER_EQ_MIN, RASTER_EQ_MAX };
/* Fog: by the fragment's alpha (RAVE's), or linear, exp(-density * c) or exp(-(density * c)^2) */
enum { RASTER_FOG_NONE, RASTER_FOG_ALPHA, RASTER_FOG_LINEAR, RASTER_FOG_EXP, RASTER_FOG_EXP2 };
enum { RASTER_ALPHA_NONE, RASTER_ALPHA_LT, RASTER_ALPHA_EQ, RASTER_ALPHA_LE, RASTER_ALPHA_GT,
       RASTER_ALPHA_NE, RASTER_ALPHA_GE, RASTER_ALPHA_TRUE };
/* What a stencil value becomes (as OpenGL's GL_KEEP...; the wrapping ones as GL_EXT_stencil_wrap's) */
enum { RASTER_STENCIL_KEEP, RASTER_STENCIL_ZERO, RASTER_STENCIL_REPLACE, RASTER_STENCIL_INCR, RASTER_STENCIL_DECR,
       RASTER_STENCIL_INVERT, RASTER_STENCIL_INCR_WRAP, RASTER_STENCIL_DECR_WRAP };

/*
 * Texture filters (as OpenGL's; raster.c takes one mipmap level per
 * triangle, so *_LINEAR as *_NEAREST, and a 3D texture's by s and t alone)
 */
enum { RASTER_NEAREST, RASTER_LINEAR, RASTER_NEAREST_MIPMAP_NEAREST, RASTER_LINEAR_MIPMAP_NEAREST,
       RASTER_NEAREST_MIPMAP_LINEAR, RASTER_LINEAR_MIPMAP_LINEAR };

struct raster_unit {
	const struct raster_texture *texture;
	uint8_t min_filter, mag_filter;
	bool clamp_s, clamp_t, clamp_r;         /* to the edge texels (else repeating) */
	bool projective;                        /* coordinates divided by the unit's q/w (else by 1/w: q is 1) */
	float anisotropy;                       /* the most the GPU may use, where it has some (1 or 0: none) */
	float lod_bias;                         /* added to triangles' and lines' levels of detail */
};

/*
 * A combining stage: r, g and b, and a, each from an operation on up to
 * three arguments (as OpenGL's GL_COMBINE); `previous` is the stage
 * before's result, or color 0 for the first.
 */
enum { RASTER_OP_REPLACE, RASTER_OP_MODULATE, RASTER_OP_ADD, RASTER_OP_ADD_SIGNED, RASTER_OP_INTERPOLATE,
       RASTER_OP_SUBTRACT };
/* An argument: a source, with RASTER_ALPHA its alpha in every channel, with RASTER_INVERT one minus that */
enum { RASTER_PREVIOUS, RASTER_COLOR0, RASTER_COLOR1, RASTER_COLOR2, RASTER_TEXTURE0,
       RASTER_CONSTANT = RASTER_TEXTURE0 + RASTER_UNITS, RASTER_SOURCES };
#define RASTER_ALPHA 0x10u
#define RASTER_INVERT 0x20u

struct raster_stage {
	uint8_t op[2];                          /* for r, g and b; for a */
	uint8_t arg[2][3];                      /* op(arg[0], arg[1], arg[2]): REPLACE arg 0, MODULATE 0 * 1, */
						/* ADD 0 + 1, ADD_SIGNED 0 + 1 - 0.5, INTERPOLATE 0 * 2 + 1 * (1 - 2), */
						/* SUBTRACT 0 - 1 */
	uint8_t shift[2];                       /* the result times 1 << shift, */
	bool clamp[2];                          /* ... then to 0..1 */
	float constant[4];                      /* a, r, g, b */
};

struct raster_state {
	unsigned z_func;
	bool z_write;
	bool perspective_z;                     /* test 1/w instead of z */
	/* Textured primitives: the units' textures, combined by the stages (others are just color 0) */
	struct raster_unit unit[RASTER_UNITS];
	unsigned stages;
	struct raster_stage stage[RASTER_STAGES];
	unsigned fog_mode;
	bool fog_coord;                         /* by the vertices' fog coordinates (else by w) */
	float fog_color[4];                     /* a, r, g, b */
	float fog_start, fog_end, fog_density;
	unsigned alpha_test;
	float alpha_ref;
	/*
	 * The stencil test (RASTER_Z_NONE: none): passes if the reference's bits
	 * in stencil_mask compare so with the target's; then each of the
	 * target's stencil bits in stencil_write becomes the operation's for the
	 * stencil test failing, the z test failing, or both passing. With it, the
	 * tests are OpenGL's in its order: alpha, stencil, z.
	 */
	unsigned stencil_func;
	uint8_t stencil_ref, stencil_mask, stencil_write;
	uint8_t stencil_op[3];
	/*
	 * Fragments whose alpha is 0 or less are drawn too (as OpenGL draws
	 * them); otherwise they leave depth alone, and color too unless they add
	 * some (as hardware RAVE engines skip transparent texels).
	 */
	bool all_fragments;
	unsigned blend;
	unsigned blend_src, blend_dst;          /* (RASTER_BLEND_FACTORS) */
	unsigned blend_eq;
	float blend_color[4];                   /* a, r, g, b */
	unsigned logic_op;                      /* (RASTER_BLEND_LOGIC: the color's bits with the target's) */
	uint32_t color_keep;                    /* bits of the target's colors left as they are (0: none) */
	float width;                            /* of points and lines */
	int clip_left, clip_top, clip_right, clip_bottom;      /* pixels drawn: [left, right) x [top, bottom) */
	/*
	 * Points and lines as OpenGL draws them (else as RAVE's: a square of
	 * `width` at each step along a line): see raster_gl_fragments().
	 * Antialiased (smooth), each fragment's alpha is times its coverage.
	 * Stippled, a line's fragment k is drawn if bit (stipple_counter + k) /
	 * factor % 16 of the pattern is set (the counter: the command's
	 * `stipple`).
	 */
	bool gl_points;
	bool smooth;
	bool line_stippled;
	uint16_t line_stipple, line_stipple_factor;
	/*
	 * Triangles stippled: a pixel is drawn if bit 31 - x % 32 of row
	 * (height - 1 - y) % 32 is set (as OpenGL's, its rows going up from the
	 * bottom of the target)
	 */
	bool polygon_stippled;
	uint32_t polygon_stipple[32];
};

/* RAVE's texture operations (see raster_rave_stages()) */
enum { RASTER_TEX_MODULATE = 1, RASTER_TEX_HIGHLIGHT = 2, RASTER_TEX_DECAL = 4 };

/*
 * The stages for QuickDraw 3D RAVE's texture operations: unit 0's texture
 * decaled over color 0 (keeping its alpha), or with its alpha times color
 * 0's; then times color 2 (kd: RASTER_TEX_MODULATE); then times unit 1's
 * texture if it has one (ATI's multitexturing), or else plus color 1 (ks:
 * RASTER_TEX_HIGHLIGHT).
 */
void raster_rave_stages(struct raster_state *s, unsigned ops);

/* The units (bit k: unit k) and colors (bit k: color k) a state's stages use */
unsigned raster_units_used(const struct raster_state *s);
unsigned raster_colors_used(const struct raster_state *s);

/*
 * Whether a primitive recorded as textured is drawn with the stages: they
 * aren't if there are none, or a unit they use has no texture.
 */
bool raster_textured(const struct raster_state *s, bool textured);

struct raster_target {
	int width, height;
	uint32_t *color;
	float *depth;
	uint8_t *stencil;                       /* (NULL: none; then the stencil test passes) */
};

/* Depth that means "farthest" in the state's z mode */
float raster_far(const struct raster_state *s);

void raster_clear(struct raster_target *t, int left, int top, int right, int bottom,
		  bool color, uint32_t argb, bool depth, float z);

void raster_triangle(struct raster_target *t, const struct raster_state *s, const struct raster_vertex *v0,
		     const struct raster_vertex *v1, const struct raster_vertex *v2, bool textured);
void raster_line(struct raster_target *t, const struct raster_state *s, const struct raster_vertex *v0,
		 const struct raster_vertex *v1);
void raster_point(struct raster_target *t, const struct raster_state *s, const struct raster_vertex *v);

/* An image drawn unscaled at (x, y), depth z (bitmaps) */
void raster_bitmap(struct raster_target *t, const struct raster_state *s, const struct raster_texture *image,
		   float x, float y, float z);

/*
 * Drawing recorded to be done later, all together. RASTER_CMD_PIXELS: an
 * image whose texel (i, j) covers the pixels whose centers are in
 * [x + i zx, x + (i + 1) zx) x [y + j zy, y + (j + 1) zy) (either way round:
 * the zoom may be negative), from (x, y) = (v[0].x, v[0].y), (zx, zy) =
 * (v[1].x, v[1].y): each such pixel a fragment of the texel's color
 * (RASTER_PIXELS_COLOR), or of v[0].color[0] where the texel isn't 0
 * (_MASK) or at the depth whose float bits the texel is (_DEPTH), at depth
 * v[0].z otherwise; through the state's tests, blending and masks but not
 * textured or fogged. Or each such pixel's stencil set to the texel's low
 * 8 bits, in the state's stencil_write (_STENCIL: no tests).
 */
enum { RASTER_CMD_TRIANGLE, RASTER_CMD_LINE, RASTER_CMD_POINT, RASTER_CMD_BITMAP, RASTER_CMD_CLEAR,
       RASTER_CMD_PIXELS };
enum { RASTER_PIXELS_COLOR, RASTER_PIXELS_MASK, RASTER_PIXELS_DEPTH, RASTER_PIXELS_STENCIL };

struct raster_cmd {
	uint8_t kind;
	bool textured;                          /* triangles (see raster_textured()) */
	bool clear_color, clear_depth;          /* clears: rect's rows within the clip's, to argb (but */
						/* for color_keep's bits) and z, */
	bool clear_stencil;                     /* ... and the stencil's bits in stencil_write to `stencil` */
	uint8_t stencil;
	uint8_t pixels;                         /* (RASTER_CMD_PIXELS: RASTER_PIXELS_...) */
	unsigned state;                         /* index into the states given with the commands */
	struct raster_vertex v[3];              /* (bitmaps: at v[0].x, y, z) */
	const struct raster_texture *image;     /* bitmaps, pixels */
	int rect[4];                            /* left, top, right, bottom */
	uint32_t argb;
	float z;
	unsigned stipple;                       /* (OpenGL's lines: the stipple counter at their first fragment) */
};

/*
 * The fragments of an OpenGL point or line (a command whose state has
 * gl_points), within [left, right) x [top, bottom): fn(ctx, x, y, t,
 * cover) for each, in order along a line, with t where along it its values
 * are (0 at v[0], 1 at v[1]; points' 0) and cover its coverage (1 unless
 * smooth). Points are squares of the width rounded (at least 1), centered as
 * OpenGL 1.2 has it; lines take the pixels whose diamonds they leave (the
 * diamond-exit rule, ties broken as OpenGL's perturbed endpoints break
 * them), or columns or rows of the width rounded where wider; smooth, the
 * pixels a disk of the width, or a rectangle of the width along the line,
 * covers some of, by the area covered. Stippled lines' fragments are left
 * out as their pattern has it. Returns how far a line moves the stipple
 * counter on (its fragments along it; fn may be NULL).
 */
typedef void raster_fragment_fn(void *ctx, int x, int y, float t, float cover);
unsigned raster_gl_fragments(const struct raster_state *s, const struct raster_cmd *c, int left, int top, int right,
			     int bottom, raster_fragment_fn *fn, void *ctx);

/* A vertex's values (but x and y) t of the way from a's to b's, as raster_gl_fragments()'s fragments have them */
void raster_lerp_vertex(struct raster_vertex *out, const struct raster_vertex *a, const struct raster_vertex *b,
			float t);

/*
 * A level of detail for a unit's texture along a line (log2 of texels per
 * pixel; if it doesn't change, far below any bias: magnified)
 */
float raster_line_lod(const struct raster_unit *u, unsigned unit, const struct raster_vertex *v0,
		      const struct raster_vertex *v1);

/*
 * Does the commands in order. Threads (BLUE_RASTER_THREADS in all, the
 * caller's included: by default 3, at most 8) each take bands of rows and
 * do every command there, so each pixel sees the commands in order and the
 * result is the same as doing them one at a time.
 */
void raster_run(struct raster_target *t, const struct raster_state *states, const struct raster_cmd *cmds,
		unsigned n);

/*
 * The same in the background: raster_start() returns at once (after
 * waiting for the commands started before), and the target, states and
 * commands must stay as they are until raster_wait(), which returns once
 * they're done (helping with them). One thread calls these.
 */
void raster_start(struct raster_target *t, const struct raster_state *states, const struct raster_cmd *cmds,
		  unsigned n);
void raster_wait(void);

#endif
