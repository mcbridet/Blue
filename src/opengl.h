/*
 * The guest's OpenGL: syshm's own OpenGLLibrary (gl and agl), OpenGLMemory
 * (glm) and OpenGLUtility (glu), generated code fragments under Apple's
 * fragment names and with Apple's exports (opengl_table.h), whose calls are
 * host calls (see ndrv.h), or for most of gl's, records the library's own
 * code writes into a buffer in guest memory, done on the host at the next
 * host call (see opengl.c). The host keeps each context's state, its front
 * end (glfront.h) draws with the 3D backend (surface.h: the GPU or
 * raster.c), and a frame goes into the context's drawable in guest memory
 * (a window's or the screen's port, or an offscreen buffer) when it's
 * swapped. See docs/opengl-plan.md.
 *
 * A context (AGLContext) is a structure in the library's data laid out as
 * Apple's: its handle, then the dispatch table of its gl functions, which
 * take the handle first; glc_cc (exported) points at the current one.
 * Each process has its own copy of the library's data (an instance).
 */
#ifndef SYSHM_OPENGL_H
#define SYSHM_OPENGL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "glfront.h"
#include "gpu.h"
#include "mem.h"
#include "ndrv.h"
#include "surface.h"

#define OPENGL_CONTEXTS 8               /* per process */
#define OPENGL_INSTANCES 16             /* processes using the library at once */

struct opengl_instance;

struct opengl_context {
	bool used;
	struct opengl_instance *inst;
	uint32_t guest;                 /* its AGLContext */
	uint32_t format;                /* its pixel format's flags (PF_ in opengl.c) */
	unsigned depth_bits, stencil_bits, pixel_size;
	/* the drawable */
	int kind;                       /* (DRAWABLE_ in opengl.c) */
	uint32_t port;                  /* CGrafPtr */
	uint32_t base, row_bytes;       /* offscreen */
	int width, height;              /* of its buffers */
	int origin_x, origin_y;         /* where their (0, 0) is in the port's coordinates */
	bool attached;                  /* (viewport and scissor have been set to the drawable's size) */
	struct surface surface;
	bool has_surface;
	/* AGL's options */
	uint32_t enabled;               /* (bit k: AGL_ option opengl.c's AGL_OPTION k) */
	int32_t swap_rect[4], buffer_rect[4], swap_interval;
	struct gl_state st;
	unsigned long frames;
	/* a display list being made (glNewList; name 0: none): its mode, its entries so far (see opengl.c) */
	uint32_t list_name, list_mode;
	uint8_t *list;
	size_t list_len, list_cap;
	unsigned list_depth;            /* (lists being run: glCallList's nesting) */
	/*
	 * The bytes a call's pointer argument points to, given (a list's entry's,
	 * or an image OpenGLUtility made); NULL: the program's memory
	 */
	const uint8_t *given;
	size_t given_len;
};

struct opengl_instance {
	uint32_t toc;                   /* its data (0: none) */
	uint32_t agl_error;
	struct opengl_context ctx[OPENGL_CONTEXTS];
};

struct opengl {
	struct mem *mem;
	struct ndrv_layout library, memory, utility;    /* of each fragment */
	struct gpu *gpu;                /* (the caller's, as rave.h's) */
	bool gpu_asked;
	struct opengl_instance inst[OPENGL_INSTANCES];
	bool log;                       /* SYSHM_LOG_GL: every call */
	bool fps;                       /* SYSHM_GL_FPS: the frame rate every second */
	double fps_since, fps_drawing;
	unsigned fps_frames;
	unsigned char unimplemented[1024];      /* (said once each: OpenGLLibrary's functions) */
	uint64_t glu_told;                      /* (... OpenGLUtility's) */
	unsigned long calls, recorded, contexts, frames, gpu_frames;
	/* the last call swapped a frame to the screen (a port): its context's number from 1 (0: it didn't), its AGLContext */
	unsigned shown;
	uint32_t shown_id;
	unsigned long bad_records;              /* (records that made no sense: the rest of the buffer dropped) */
};

void opengl_init(struct opengl *o, struct mem *mem);
void opengl_free(struct opengl *o);

/* A call into each library (see ndrv.h): its function's entry, or the initialization or termination routine */
void opengl_library_call(struct opengl *o, struct ndrv_call *c);
void opengl_memory_call(struct opengl *o, struct ndrv_call *c);
void opengl_utility_call(struct opengl *o, struct ndrv_call *c);

/* The fragments; each returns its size (0 if `size` is too small: OPENGL_LIBRARY_MAX is enough for any) */
#define OPENGL_LIBRARY_MAX (256u << 10)
size_t opengl_build_library(uint8_t *out, size_t size, struct ndrv_layout *layout);
size_t opengl_build_memory(uint8_t *out, size_t size, struct ndrv_layout *layout);
size_t opengl_build_utility(uint8_t *out, size_t size, struct ndrv_layout *layout);

/* A call's argument (as opengl_table.h's types: integers as u or i, floats as f, doubles as d) */
union gl_arg {
	uint32_t u;
	int32_t i;
	float f;
	double d;
};

/*
 * For OpenGLUtility (glu.c): a gl call (fn: opengl_table.h's order) to a
 * context (NULL: none, nothing done), as the library's are made; or with
 * the bytes its pointer argument points to given (unpacked by `unpack`, if
 * it unpacks an image)
 */
uint32_t opengl_gl(struct opengl *o, struct opengl_context *x, unsigned fn, const union gl_arg *a);
uint32_t opengl_gl_given(struct opengl *o, struct opengl_context *x, unsigned fn, const union gl_arg *a,
			 const uint8_t *data, size_t len, const struct gl_pixel_store *unpack);
/*
 * The program's memory: len bytes at `at` there to be read, or written (any
 * code translated from them dropped); NULL if they aren't
 */
const uint8_t *opengl_in(struct opengl *o, uint32_t at, size_t len);
uint8_t *opengl_out(struct opengl *o, uint32_t at, size_t len);

/*
 * OpenGLLibrary's entries: its functions' (by name; -1: none), its
 * initialization and termination routines, and (from OPENGL_ENTRY_TABLE,
 * plus the function's) each gl function as a context's dispatch table
 * calls it, with the context's handle first. glc_cc is OPENGL_DATA_CURRENT
 * bytes into the fragment-specific data, the recording buffer's bounds
 * OPENGL_DATA_RECORD.
 */
int opengl_entry(const char *name);
#define OPENGL_FUNCTIONS 473u
#define OPENGL_ENTRY_INIT OPENGL_FUNCTIONS
#define OPENGL_ENTRY_TERM (OPENGL_FUNCTIONS + 1)
#define OPENGL_ENTRY_TABLE (OPENGL_FUNCTIONS + 3)
#define OPENGL_DATA_CURRENT 8u
#define OPENGL_DATA_RECORD 12u          /* (where the next record goes; then where the buffer ends) */

/*
 * How OpenGLLibrary records a call to gl function fn (its entry), if it
 * does: the record's size in bytes and its arguments' types as recorded
 * (opengl_table.h's, an array's elements in its place); 0 if it doesn't
 */
unsigned opengl_recorded(unsigned fn, const char **args);
#define OPENGL_RECORD_SIZE (64u << 10)  /* the buffer */

#endif
