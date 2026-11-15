/*
 * OpenGLUtility (glu): OpenGL's utility library, its calls done on the
 * host (see opengl.h) as SGI's GLU does them (Apple's is SGI's 1.2): the
 * matrices (gluOrtho2D, gluPerspective, gluLookAt, gluPickMatrix),
 * gluProject and gluUnProject, images scaled (gluScaleImage) and mipmapped
 * (gluBuild1DMipmaps, gluBuild2DMipmaps), quadrics drawn (gluSphere,
 * gluCylinder, gluDisk, gluPartialDisk), and Apple's strings
 * (gluErrorString, gluGetString). Quadrics, tessellators and NURBS
 * renderers are Memory Manager blocks (NewPtrClear and DisposePtr, by
 * callouts); tessellators and NURBS renderers keep their properties but
 * draw nothing yet. Drawing is gl calls to the process's current context,
 * so a display list being made keeps them.
 */
#ifndef SYSHM_GLU_H
#define SYSHM_GLU_H

#include <stddef.h>
#include <stdint.h>

#include "ndrv.h"
#include "opengl.h"

/* The fragment's imports: OpenGLLibrary's glc_cc (data: where the process's current context is), InterfaceLib's */
enum { GLU_IMPORT_CURRENT, GLU_IMPORT_NEW_PTR_CLEAR, GLU_IMPORT_DISPOSE_PTR, GLU_IMPORTS };
extern const struct ndrv_import glu_imports[GLU_IMPORTS];

/* The fragment's data: the strings it gives */
const uint8_t *glu_data(size_t *len);

/*
 * A call: c's entry (opengl_table.h's OPENGL_UTILITY_FUNCTIONS order, its
 * arguments in a; or NDRV_ENTRY_RESUME after a callout), to x, the process's
 * current context (NULL: none). Finishes c, or makes a callout.
 */
void glu_call(struct opengl *o, struct opengl_context *x, struct ndrv_call *c, const union gl_arg *a);

#endif
