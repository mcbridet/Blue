/*
 * A drawing surface for the guest's 3D APIs: color, depth and stencil
 * buffers on the host (a raster_target), drawn by recorded commands in batches, by
 * the GPU (gpu.c) or the software rasterizer (raster.c), whose pixels are
 * then presented into guest memory: a QuickDraw pixmap (the screen's, a
 * GWorld's) or a buffer, clipped to QuickDraw regions.
 */
#ifndef SYSHM_SURFACE_H
#define SYSHM_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu.h"
#include "mem.h"
#include "raster.h"

#define SURFACE_BATCH 512               /* commands drawn together in the background */

struct surface {
	struct raster_target target;
	struct gpu *gpu;                /* drawn by the GPU, into gpu_target (NULL: by raster.c) */
	struct gpu_target *gpu_target;
	struct surface_batch {
		struct raster_cmd *cmds;
		unsigned ncmds, cmds_cap;
		struct raster_state *states;
		unsigned nstates, states_cap;
	} batch[2];
	unsigned rec;                   /* the batch being recorded (the other may be drawing) */
};

/*
 * Buffers of this size, cleared to 0, drawn by the GPU if there's one;
 * false if there's no memory for them. If the GPU can't make its copy,
 * gpu becomes NULL and the surface is drawn in software.
 */
bool surface_init(struct surface *s, int width, int height, struct gpu *gpu);
void surface_free(struct surface *s);

/*
 * A command to record with this state (the rest of it zeroed: kind and
 * state set), or NULL if there's no memory. A full batch is drawn in the
 * background first. The state's textures must stay as they are until the
 * surface is finished.
 */
struct raster_cmd *surface_cmd(struct surface *s, const struct raster_state *state, uint8_t kind);

/* Whether anything is recorded and not drawn yet */
bool surface_pending(const struct surface *s);

/* Whether anything recorded since it was last finished may still use its textures */
bool surface_busy(const struct surface *s);

/*
 * Everything recorded drawn, and the pixels in target.color. False if the
 * GPU failed: what it drew is lost, and the surface goes on in software
 * (surface_lose_gpu() has been done).
 */
bool surface_finish(struct surface *s);

/*
 * After surface_finish(): the depth and stencil buffers on the host too (the
 * GPU's read back, if it draws). False if that fails (then it's lost).
 */
bool surface_read_depth(struct surface *s);

/* Waits for drawing in the background (textures it uses may then go) */
void surface_wait(struct surface *s);

/*
 * The host time (ns) every surface's caller has spent waiting for its
 * backend (to take a batch, or to finish), and reading frames back from the
 * GPU, so far: for profiling (see opengl.h's SYSHM_GL_PROFILE)
 */
void surface_times(uint64_t *wait_ns, uint64_t *read_ns);

/* The GPU's copy given up: the surface is drawn in software from now on (its pixels as they were last read) */
void surface_lose_gpu(struct surface *s);

/*
 * Where presented pixels go in guest memory: a pixmap whose pixel (0, 0)
 * is at base, `bits` 16 (1-5-5-5) or 32 (x-8-8-8) a pixel, big-endian, of
 * which [left, right) x [top, bottom) may be written; the surface's pixel
 * (0, 0) at (x, y) there. Up to two QuickDraw regions (RgnHandles, 0 for
 * none), whose coordinates are the surface's plus (region_x, region_y),
 * limit it further.
 */
struct surface_dest {
	uint32_t base, row_bytes;
	unsigned bits;
	bool alpha;                     /* 32 bits a pixel: a-8-8-8 */
	int left, top, right, bottom;
	int x, y;
	uint32_t region[2];
	int region_x, region_y;
};

/* The surface's pixels [l, r) x [t, b) into the destination (after surface_finish()) */
void surface_present(const struct surface *s, struct mem *m, const struct surface_dest *d, int l, int t, int r,
		     int b);

#endif
