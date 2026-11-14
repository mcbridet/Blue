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
 *                       Manager.
 *   syshm 3D Engine     a QuickDraw 3D RAVE drawing engine (rave.h).
 */
#ifndef SYSHM_TOOLS_H
#define SYSHM_TOOLS_H

#include <stdbool.h>
#include <stdint.h>

#include "ndrv.h"
#include "video.h"

/* (installed copies have these numbers in their code; 0x101 is free) */
#define HOSTCALL_TOOLS_HELPER 0x100u
#define HOSTCALL_TOOLS_RAVE 0x102u
#define TOOLS_VOLUME_SIZE (2u << 20)

/* Writes the tools volume (a disk image) to `path`. Returns 0 on success. */
int tools_create(const char *path);

struct helper {
	struct ndrv_layout layout;      /* of the helper's fragment */
	bool running;
	unsigned long launches, switches, failures;
	int32_t last_error;             /* of the last mode switch that failed */
};

void helper_init(struct helper *h);

/* Runs the helper for one host call: its main routine, or a resumption after a callout. */
void helper_call(struct helper *h, struct ndrv_call *c, struct video *v);

#endif
