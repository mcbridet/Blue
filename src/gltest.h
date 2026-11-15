/*
 * The OpenGL test application, "syshm GL Test": a generated application
 * whose logic runs on the host (see ndrv.h). It opens a window and draws a
 * scene in it through the guest's OpenGLLibrary, by callouts to what it
 * imports from there (so through the Code Fragment Manager's linking, as a
 * game would), and draws it again at each update event. For checking the
 * libraries by screenshots, in instruction time: `syshm tools create` puts
 * it on the tools volume with SYSHM_TOOLS_GL_TEST=1 (put it in Startup
 * Items to run it), SYSHM_GL_TEST_SCENE names the scene and
 * SYSHM_GL_TEST_DEPTH the screen's depth it sets first (16 or 32).
 */
#ifndef SYSHM_GLTEST_H
#define SYSHM_GLTEST_H

#include <stddef.h>
#include <stdint.h>

#include "mem.h"
#include "ndrv.h"

struct gltest {
	struct ndrv_layout layout;
	const struct gltest_step *steps;        /* the scene's */
	unsigned nsteps;
	unsigned depth;                         /* the screen's (SYSHM_GL_TEST_DEPTH) */
	unsigned long frames;
};

void gltest_init(struct gltest *t);

/* Runs it for one host call (see ndrv.h) */
void gltest_call(struct gltest *t, struct ndrv_call *c);

/* The application's fragment; returns its size (0 if `size` is too small) */
size_t gltest_build(uint8_t *out, size_t size, struct ndrv_layout *layout);

#endif
