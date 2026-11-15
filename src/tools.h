/*
 * The guest tools: a "syshm Tools" volume of files to put in the System
 * Folder, for what only code inside Mac OS can do. Like the drivers, they
 * are generated code fragments whose logic runs in the emulator through
 * host calls (see ndrv.h).
 *
 *   syshm Enhancements  a background-only application, which Mac OS launches
 *                       from the Extensions folder at startup. When the host
 *                       window is resized, it switches the display to a mode
 *                       of that size (video_request_size()) with the Display
 *                       Manager. With a clipboard to share (clip.h), it
 *                       patches the Scrap Manager's ZeroScrap and PutScrap,
 *                       and filters events, for every application.
 *   syshm 3D Engine     a QuickDraw 3D RAVE drawing engine (rave.h).
 *   syshm OpenGL ...    OpenGLLibrary, OpenGLMemory and OpenGLUtility
 *                       (opengl.h), Mac OS 9's OpenGL; on Mac OS 9 they
 *                       take Apple's place (see install.h).
 */
#ifndef SYSHM_TOOLS_H
#define SYSHM_TOOLS_H

#include <stdbool.h>
#include <stdint.h>

#include "clip.h"
#include "hfs.h"
#include "ndrv.h"
#include "video.h"

/* (installed copies have these numbers in their code; 0x101 is free) */
#define HOSTCALL_TOOLS_HELPER 0x100u
#define HOSTCALL_TOOLS_RAVE 0x102u
#define HOSTCALL_TOOLS_GL 0x104u        /* OpenGLLibrary */
#define HOSTCALL_TOOLS_GLM 0x105u       /* OpenGLMemory */
#define HOSTCALL_TOOLS_GLU 0x106u       /* OpenGLUtility */
#define HOSTCALL_TOOLS_GL_TEST 0x107u   /* the OpenGL test application (gltest.h) */
#define TOOLS_VOLUME_SIZE (2u << 20)

/* The tools' files, in the volume's order */
enum { TOOLS_ENHANCEMENTS, TOOLS_3D_ENGINE, TOOLS_GL_LIBRARY, TOOLS_GL_MEMORY, TOOLS_GL_UTILITY, TOOLS_FILES };
#define TOOLS_GL_FIRST TOOLS_GL_LIBRARY
#define TOOLS_GL_FILES 3

/*
 * The files as the tools volume holds them: names, type and creator, Finder
 * flags and forks (in static buffers, the same at every call). Returns 0, or
 * -1 if they can't be made.
 */
int tools_files(struct hfs_file files[TOOLS_FILES]);

/*
 * Writes the tools volume (a disk image) to `path`. Returns 0 on success.
 * SYSHM_TOOLS_APPL=1 makes syshm Enhancements there an application the
 * Finder opens (an 'appe' only runs from Extensions), to try it from a CD's
 * system; SYSHM_TOOLS_GL_TEST=1 adds the OpenGL test application (gltest.h).
 */
int tools_create(const char *path);

struct helper {
	struct ndrv_layout layout;      /* of the helper's fragment */
	uint32_t v1_extra;              /* ... and where version 1's data is */
	bool running;
	bool quit;                      /* to quit when it next looks (see install.h) */
	bool old;                       /* an older copy has run (see current()) */
	bool patched;                   /* its Scrap Manager patches are in place */
	uint32_t patched_toc;           /* ... by the copy with this data */
	bool delivering;                /* a patch is putting the host's text in the scrap */
	unsigned long serial;           /* ... this one (clip.h) */
	uint32_t handle, size;          /* ... from this buffer (a handle), this big */
	unsigned long launches, switches, failures;
	int32_t last_error;             /* of the last mode switch that failed */
	unsigned long scrap_calls;      /* to the patches */
};

void helper_init(struct helper *h);

/* Runs the helper for one host call: its main routine, a patch, or a resumption after a callout. */
void helper_call(struct helper *h, struct ndrv_call *c, struct video *v, struct clip *clip);

#endif
