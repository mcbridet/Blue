/*
 * The 3D engine's rasterizer: triangles, lines and points into a color and
 * depth buffer, with what QuickDraw 3D RAVE asks of a drawing engine:
 * Gouraud shading, perspective-correct texture mapping (nearest or
 * bilinear, mipmaps), the texture operations, z functions, transparency
 * blending, fog and alpha test. Everything is plain C on host floats, so
 * the same input gives the same pixels on every host.
 *
 * Coordinates are pixels, x right and y down, with pixel (i, j) covering
 * [i, i+1) x [j, j+1) and sampled at its center; edges follow the top-left
 * rule. Colors are 0xAARRGGBB.
 */
#ifndef SYSHM_RASTER_H
#define SYSHM_RASTER_H

#include <stdbool.h>
#include <stdint.h>

#define RASTER_MAX_LEVELS 13

struct raster_texture {
	unsigned levels;
	unsigned width[RASTER_MAX_LEVELS], height[RASTER_MAX_LEVELS];
	uint32_t *texels[RASTER_MAX_LEVELS];    /* row k at v = k / height (bitmaps: rows top to bottom) */
	uint32_t gpu;                           /* the GPU backend's copy (gpu.c's; 0: none yet) */
};

struct raster_vertex {
	float x, y, z, invw;
	float r, g, b, a;
	float uw, vw;                           /* u / w, v / w */
	float kd[3], ks[3];                     /* texture color scale and highlight */
};

/* Values as RAVE's (kQAZFunction_ etc.) */
enum { RASTER_Z_NONE, RASTER_Z_LT, RASTER_Z_EQ, RASTER_Z_LE, RASTER_Z_GT, RASTER_Z_NE, RASTER_Z_GE,
       RASTER_Z_TRUE, RASTER_Z_FALSE };
enum { RASTER_TEX_MODULATE = 1, RASTER_TEX_HIGHLIGHT = 2, RASTER_TEX_DECAL = 4 };
enum { RASTER_BLEND_PREMULTIPLY, RASTER_BLEND_INTERPOLATE, RASTER_BLEND_FACTORS };
/* Blend factors (as OpenGL's GL_ZERO, GL_ONE, GL_SRC_COLOR...) */
enum { RASTER_F_ZERO, RASTER_F_ONE, RASTER_F_SRC_COLOR, RASTER_F_ONE_MINUS_SRC_COLOR, RASTER_F_SRC_ALPHA,
       RASTER_F_ONE_MINUS_SRC_ALPHA, RASTER_F_DST_ALPHA, RASTER_F_ONE_MINUS_DST_ALPHA, RASTER_F_DST_COLOR,
       RASTER_F_ONE_MINUS_DST_COLOR, RASTER_F_SRC_ALPHA_SATURATE };
enum { RASTER_FOG_NONE, RASTER_FOG_ALPHA, RASTER_FOG_LINEAR, RASTER_FOG_EXP, RASTER_FOG_EXP2 };
enum { RASTER_ALPHA_NONE, RASTER_ALPHA_LT, RASTER_ALPHA_EQ, RASTER_ALPHA_LE, RASTER_ALPHA_GT,
       RASTER_ALPHA_NE, RASTER_ALPHA_GE, RASTER_ALPHA_TRUE };

struct raster_state {
	unsigned z_func;
	bool z_write;
	bool perspective_z;                     /* test 1/w instead of z */
	const struct raster_texture *texture;   /* (for textured primitives) */
	/*
	 * A second texture modulating the first (ATI's multitexturing): its
	 * coordinates are the vertices' ks (u/w, v/w, 1/w), which then aren't a
	 * highlight. Filtered bilinearly, clamped at its edges.
	 */
	const struct raster_texture *texture2;
	unsigned texture_op;
	bool bilinear;
	bool trilinear;                         /* blend mipmap levels too (the GPU; raster.c picks one per triangle) */
	bool clamp_u, clamp_v;
	unsigned blend;
	unsigned blend_src, blend_dst;          /* (RASTER_BLEND_FACTORS) */
	unsigned fog_mode;
	float fog_color[4];                     /* a, r, g, b */
	float fog_start, fog_end, fog_density;  /* in w; exponential fog: exp(-(density * w / end)^n) */
	unsigned alpha_test;
	float alpha_ref;
	float width;                            /* of points and lines */
	int clip_left, clip_top, clip_right, clip_bottom;      /* pixels drawn: [left, right) x [top, bottom) */
};

struct raster_target {
	int width, height;
	uint32_t *color;
	float *depth;
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

/* Drawing recorded to be done later, all together */
enum { RASTER_CMD_TRIANGLE, RASTER_CMD_LINE, RASTER_CMD_POINT, RASTER_CMD_BITMAP, RASTER_CMD_CLEAR };

struct raster_cmd {
	uint8_t kind;
	bool textured;                          /* triangles */
	bool clear_color, clear_depth;          /* clears: of rect, to argb and z */
	unsigned state;                         /* index into the states given with the commands */
	struct raster_vertex v[3];              /* (bitmaps: at v[0].x, y, z) */
	const struct raster_texture *image;     /* bitmaps */
	int rect[4];                            /* left, top, right, bottom */
	uint32_t argb;
	float z;
};

/*
 * Does the commands in order. Threads (SYSHM_RASTER_THREADS in all, the
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
