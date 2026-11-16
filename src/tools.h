/*
 * The guest tools: a "Blue Tools" volume of files to put in the System
 * Folder, for what only code inside Mac OS can do. Like the drivers, they
 * are generated code fragments whose logic runs in the emulator through
 * host calls (see ndrv.h).
 *
 *   Blue Enhancements   a background-only application, which Mac OS launches
 *                       from the Extensions folder at startup. When the host
 *                       window is resized, it switches the display to a mode
 *                       of that size (video_request_size()) with the Display
 *                       Manager. With a clipboard to share (clip.h), it
 *                       patches the Scrap Manager's ZeroScrap and PutScrap,
 *                       and filters events, for every application.
 *   Blue 3D Engine      a QuickDraw 3D RAVE drawing engine (rave.h).
 *   Blue OpenGL ...     OpenGLLibrary, OpenGLMemory and OpenGLUtility
 *                       (opengl.h), Mac OS 9's OpenGL; on Mac OS 9 they
 *                       take Apple's place (see install.h).
 *   Blue Pippin Library PippinPPC (pippin.h), the Bandai Pippin's library,
 *                       for Pippin titles that link to it; its code is its
 *                       own, calling Mac OS (its host calls only tell Blue
 *                       of the title's AppleJack, for game controllers).
 *   Blue Game           an InputSprocket driver (isp.h): the host's game
 *     Controllers       controllers, for games that read them through Apple
 *                       Game Sprockets' InputSprocket.
 */
#ifndef BLUE_TOOLS_H
#define BLUE_TOOLS_H

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
#define HOSTCALL_TOOLS_PIPPIN 0x108u    /* the Pippin library's (pippin.h) */
#define HOSTCALL_TOOLS_ISP 0x109u       /* the InputSprocket driver's (isp.h) */
#define TOOLS_VOLUME_SIZE (2u << 20)

/* The tools' files, in the volume's order */
enum {
	TOOLS_ENHANCEMENTS, TOOLS_3D_ENGINE, TOOLS_GL_LIBRARY, TOOLS_GL_MEMORY, TOOLS_GL_UTILITY, TOOLS_PIPPIN, TOOLS_ISP,
	TOOLS_FILES
};
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
 * BLUE_TOOLS_APPL=1 makes Blue Enhancements there an application the
 * Finder opens (an 'appe' only runs from Extensions), to try it from a CD's
 * system; BLUE_TOOLS_GL_TEST=1 adds the OpenGL test application (gltest.h).
 */
int tools_create(const char *path);

struct helper {
	struct ndrv_layout layout;      /* of the helper's fragment */
	uint32_t v1_extra;              /* ... and where version 1's data is */
	bool running;
	bool quit;                      /* to quit when it next looks (see install.h) */
	unsigned long quits;            /* times it has quit (asked by Blue or by the Process Manager) */
	bool old;                       /* an older copy has run (see current()) */
	bool patched;                   /* its Scrap Manager patches are in place */
	uint32_t patched_toc;           /* ... by the copy with this data */
	bool delivering;                /* a patch is putting the host's text in the scrap */
	unsigned long serial;           /* ... this one (clip.h) */
	uint32_t handle, size;          /* ... from this buffer (a handle), this big */
	unsigned long launches, switches, failures;
	int32_t last_error;             /* of the last mode switch that failed */
	unsigned long scrap_calls;      /* to the patches */
	/* which process is in front, asked for as it looks round (while Blue wants to know: a Pippin title runs) */
	bool want_front;
	bool front_known;
	uint32_t front[2];              /* (its ProcessSerialNumber) */
};

void helper_init(struct helper *h);

/* Runs the helper for one host call: its main routine, a patch, or a resumption after a callout. */
void helper_call(struct helper *h, struct ndrv_call *c, struct video *v, struct clip *clip);

#endif
