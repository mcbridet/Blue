/*
 * The guest tools' 3D engine: a QuickDraw 3D RAVE drawing engine whose
 * methods are host calls (see ndrv.h). The guest side is a generated code
 * fragment in the Extensions folder (file type 'shlb', creator 'tnsl', as
 * RAVE looks for); its initialization routine registers it with
 * QARegisterEngine. Draw contexts, textures and bitmaps live on the host,
 * drawn by raster.c into host buffers, or by the GPU (gpu.c) and read back
 * into them, and a frame is copied into the context's device (the screen,
 * or a pixmap in guest memory) when it ends.
 *
 * It reports ATI's vendor ID: games of the time (Bugdom, for one) take a
 * RAVE engine for hardware acceleration only if it is ATI's.
 */
#ifndef BLUE_RAVE_H
#define BLUE_RAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu.h"
#include "mem.h"
#include "ndrv.h"
#include "raster.h"

#define RAVE_MAX_CONTEXTS 16
#define RAVE_TAGS 160                   /* state tags kept (RAVE's go up to 153) */

/* A triangle held back to be drawn sorted by depth when the frame ends */
struct rave_deferred {
	struct raster_vertex v[3];
	unsigned state;                 /* index into the context's dstates */
	bool textured;
	float distance;
	unsigned order;
};

struct rave_context {
	bool used;
	uint32_t guest;                 /* its TQADrawContext */
	uint32_t device_type;
	uint32_t device[5];             /* TQAPlatformDevice: the GDHandle, or the memory device */
	int32_t left, top, right, bottom;       /* its rect (global coordinates for GDevices) */
	uint32_t clip;                  /* RgnHandle, or 0 */
	uint32_t flags;                 /* kQAContext_ */
	struct raster_target target;
	struct gpu *gpu;                /* drawn by the GPU, into gpu_target (NULL: by raster.c) */
	struct gpu_target *gpu_target;
	uint32_t ints[RAVE_TAGS];
	float floats[RAVE_TAGS];
	uint32_t texture;               /* kQATag_Texture */
	bool ati_multitexture;          /* ATI's second texture (see rave.c) is on */
	uint32_t texture2;              /* ... and is this */
	uint32_t notice[5][2];          /* notice methods: TVector, refCon */
	struct raster_vertex *verts;    /* submitted for triangle meshes */
	unsigned nverts, verts_cap;
	bool verts_textured;
	/*
	 * The frame's drawing so far, in batches: a full one is drawn in the
	 * background while the next is recorded; the rest when the frame ends.
	 */
	struct rave_batch {
		struct raster_cmd *cmds;
		unsigned ncmds, cmds_cap;
		struct raster_state *states;
		unsigned nstates, states_cap;
	} batch[2];
	unsigned rec;                   /* the batch being recorded */
	bool clear_recorded;            /* it starts with renderStart's clear (see render_abort()) */
	struct rave_deferred *deferred; /* transparent triangles (kQATag_ZSortedHint) */
	unsigned ndeferred, deferred_cap;
	struct raster_state *dstates;
	unsigned ndstates, dstates_cap;
	unsigned long frames;
};

struct rave_image {
	bool used, bitmap;
	bool flip;                      /* its rows were turned over when loaded */
	struct raster_texture *tex;     /* (its own allocation: recorded drawing points at it) */
};

struct rave {
	struct mem *mem;
	struct ndrv_layout layout;
	/*
	 * The GPU backend, if the host has one (the caller's, set after
	 * rave_init()): asked whether it's usable when the first context is
	 * made, and given up for good if it fails.
	 */
	struct gpu *gpu;
	bool gpu_asked;
	struct rave_context ctx[RAVE_MAX_CONTEXTS];
	struct rave_image *images;
	unsigned nimages;
	struct raster_texture **doomed; /* deleted while drawing still recorded may use them */
	unsigned ndoomed, doomed_cap;
	bool log;
	bool fps;                       /* print the frame rate every second (BLUE_RAVE_FPS) */
	double fps_since, fps_raster;
	unsigned fps_frames;
	bool registered;
	unsigned long calls, contexts, frames, triangles, textures, backfacing;
	unsigned long gpu_frames;       /* (of frames) drawn by the GPU */
};

void rave_init(struct rave *r, struct mem *mem);
void rave_free(struct rave *r);

/*
 * Runs one call into the engine (see ndrv.h). Its entries: the engine
 * methods (TQAEngineMethodTag) from RAVE_ENTRY_ENGINE, the draw methods
 * (TQADrawMethodTag) from RAVE_ENTRY_DRAW, with the method's arguments.
 */
void rave_call(struct rave *r, struct ndrv_call *c);

#define RAVE_ENTRY_ENGINE 2u
#define RAVE_ENTRY_DRAW 20u
#define RAVE_ENTRY_ATI_TEXTURE_UPDATE 55u       /* ATI's extension: (flags, pixel type, image, texture) */

/* The engine's code fragment; returns its size (0 if `size` is too small) */
size_t rave_build_engine(uint8_t *out, size_t size, struct ndrv_layout *layout);

#endif
