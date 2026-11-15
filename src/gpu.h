/*
 * The 3D backend's GPU side: draws the rasterizer's recorded commands
 * (raster.h) with OpenGL 3.3 core in an off-screen context of its own,
 * owned by a thread of its own, and reads frames back into host buffers for
 * the guest's 3D APIs to present. Pixels match raster.c's but for rounding
 * at triangle edges and in filtering, and mipmapping, which the GPU does per
 * pixel (trilinear with the *_MIPMAP_LINEAR filters). Each way of drawing (a
 * state's stages, above all) gets a program of its own, made the first time
 * it's needed. See docs/gpu-plan.md.
 *
 * One thread (the emulator's) calls these. Every call but gpu_open() and
 * gpu_ready() first waits for the job the previous one started.
 */
#ifndef SYSHM_GPU_H
#define SYSHM_GPU_H

#include <stdbool.h>
#include <stdint.h>

#include "raster.h"

struct gpu;
struct gpu_target;

/*
 * Starts the GPU thread, which opens an OpenGL context and checks it
 * (version, limits, shaders and a self-test against raster.c) in the
 * background. Unless `allow_software`, a software OpenGL renderer is
 * refused. Returns NULL only if the thread can't be started.
 */
struct gpu *gpu_open(bool allow_software);

/* Waits for that; false (with the reason in *why, if why isn't NULL) if the GPU can't be used */
bool gpu_ready(struct gpu *g, const char **why);

/* "OpenGL 4.1 Metal - 91.7 (Apple M5)", once ready */
const char *gpu_renderer(const struct gpu *g);

/* An OpenGL error or a failed job since it was ready: nothing more is drawn; draw in software */
bool gpu_failed(const struct gpu *g);

void gpu_close(struct gpu *g);          /* (NULL is fine) */

/* A color and depth buffer of this size, cleared to 0; NULL if it can't be made (then gpu_failed()) */
struct gpu_target *gpu_target_new(struct gpu *g, int width, int height);
void gpu_target_free(struct gpu *g, struct gpu_target *t);

/* Copies one target's color and depth into another of the same size */
void gpu_copy(struct gpu *g, struct gpu_target *to, struct gpu_target *from);

/*
 * As raster_start(): draws the commands into the target in the background.
 * The states and commands, and the textures they use, must stay as they are
 * until gpu_wait() or the next call into the backend returns.
 */
void gpu_start(struct gpu *g, struct gpu_target *t, const struct raster_state *states,
	       const struct raster_cmd *cmds, unsigned n);
void gpu_wait(struct gpu *g);

/*
 * The target's pixels, once everything started on it is drawn, into `color`
 * (width x height words, 0xAARRGGBB, rows top to bottom, as a
 * raster_target's). False if the GPU has failed (`color` is then left alone).
 */
bool gpu_read(struct gpu *g, struct gpu_target *t, uint32_t *color);

/*
 * The same for its depth (as raster.c keeps it, for states without
 * perspective_z) and stencil (NULL: not read)
 */
bool gpu_read_depth(struct gpu *g, struct gpu_target *t, float *depth, uint8_t *stencil);

/*
 * The GPU thread's host time in drawing jobs and in reading back so far,
 * and the textures it's made and their texels' bytes: for profiling (NULL
 * g: 0s)
 */
void gpu_times(struct gpu *g, uint64_t *draw_ns, uint64_t *read_ns, uint64_t *uploads, uint64_t *upload_bytes);

/* Forgets a texture's GPU copy (tex->gpu), before raster.c's copy is freed */
void gpu_texture_free(struct gpu *g, struct raster_texture *tex);

#endif
