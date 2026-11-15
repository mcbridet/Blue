#include "gltest.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "be.h"
#include "tools.h"

/* What it calls: the Toolbox to start and for a window and events; OpenGLLibrary; OpenGLUtility */
#define TOOLBOX_IMPORTS(X) X(InitGraf) X(InitFonts) X(InitWindows) X(InitMenus) X(TEInit) X(InitDialogs) \
	X(InitCursor) X(GetMainDevice) X(SetDepth) X(NewCWindow) X(SetPort) X(WaitNextEvent) X(BeginUpdate) \
	X(EndUpdate)
#define GL_IMPORTS(X) X(aglChoosePixelFormat) X(aglCreateContext) X(aglSetDrawable) X(aglSetCurrentContext) \
	X(aglSwapBuffers) X(glClearColor) X(glClear) X(glScissor) X(glEnable) X(glDisable) X(glGetString) \
	X(glGetError) X(glViewport) X(glClearDepth) X(glDepthFunc) X(glShadeModel) X(glMatrixMode) \
	X(glLoadIdentity) X(glFrustum) X(glTranslatef) X(glRotatef) X(glPushMatrix) X(glPopMatrix) X(glBegin) \
	X(glEnd) X(glColor3f) X(glColor3ub) X(glVertex3f) X(glVertex3fv) X(glClearStencil) X(glLightfv) \
	X(glMaterialfv) X(glMaterialf) X(glFogi) X(glFogf) X(glFogfv) X(glEnableClientState) \
	X(glDisableClientState) X(glVertexPointer) X(glNormalPointer) X(glDrawElements) X(glStencilFunc) \
	X(glStencilOp) X(glColorMask) X(glNewList) X(glEndList) X(glCallList) X(glRasterPos2f) X(glDrawPixels) \
	X(glBitmap) X(glCopyPixels) X(glPixelZoom)
#define GLU_IMPORTS(X) X(gluNewQuadric) X(gluDeleteQuadric) X(gluQuadricDrawStyle) X(gluQuadricNormals) \
	X(gluSphere) X(gluDisk) X(gluPerspective) X(gluLookAt) X(gluOrtho2D)

enum {
#define X(name) I_##name,
	TOOLBOX_IMPORTS(X) GL_IMPORTS(X) GLU_IMPORTS(X)
#undef X
	I_COUNT
};

static const struct ndrv_import imports[I_COUNT] = {
#define X(name) { "InterfaceLib", #name },
	TOOLBOX_IMPORTS(X)
#undef X
#define X(name) { "OpenGLLibrary", #name },
	GL_IMPORTS(X)
#undef X
#define X(name) { "OpenGLUtility", #name },
	GLU_IMPORTS(X)
#undef X
};

static const char *const import_names[I_COUNT] = {
#define X(name) #name,
	TOOLBOX_IMPORTS(X) GL_IMPORTS(X) GLU_IMPORTS(X)
#undef X
};

/*
 * Its data (after the entry's TVector): QuickDraw's globals (thePort last),
 * an event record, the window's rectangle and title, the pixel format's
 * attributes, a vertex for glVertex3fv, the variables (the window, the
 * pixel format, the context, a quadric), then "lit"'s vectors (a light's
 * position and color, a material's colors, the fog's color) and a mesh's
 * arrays (positions, normals, indices), "lists"' colors, "pixels"' image
 * (16 x 16 RGBA) and glyph (16 x 16 bits, in rows of 4 bytes)
 */
#define MESH_X 7
#define MESH_Z 5
#define MESH_INDICES ((MESH_X - 1) * (MESH_Z - 1) * 6)
enum { V_NONE, V_WINDOW, V_FORMAT, V_CONTEXT, V_DEVICE, V_QUADRIC, V_COUNT };
enum { D_QD = 0, D_THE_PORT = 202, D_EVENT = 208, D_RECT = 224, D_TITLE = 232, D_ATTRIBS = 264, D_VERTEX = 316,
       D_VARS = 328, D_LIGHT_POSITION = D_VARS + 4 * V_COUNT, D_LIGHT_COLOR = D_LIGHT_POSITION + 16,
       D_DIFFUSE = D_LIGHT_COLOR + 16, D_SPECULAR = D_DIFFUSE + 16, D_FOG_COLOR = D_SPECULAR + 16,
       D_POSITIONS = D_FOG_COLOR + 16, D_NORMALS = D_POSITIONS + 12 * MESH_X * MESH_Z,
       D_INDICES = D_NORMALS + 12 * MESH_X * MESH_Z, D_RED = D_INDICES + 2 * MESH_INDICES, D_GREEN = D_RED + 16,
       D_IMAGE = D_GREEN + 16, D_GLYPH = D_IMAGE + 16 * 16 * 4, D_SIZE = D_GLYPH + 16 * 4 };
#define WINDOW_LEFT 40
#define WINDOW_TOP 60
#define WINDOW_WIDTH 320
#define WINDOW_HEIGHT 240

/* AGL's and OpenGL's */
#define AGL_RGBA 4
#define AGL_DOUBLEBUFFER 5
#define AGL_DEPTH_SIZE 12
#define AGL_STENCIL_SIZE 13
#define AGL_ACCELERATED 73
#define GL_DEPTH_BUFFER_BIT 0x0100u
#define GL_COLOR_BUFFER_BIT 0x4000u
#define GL_SCISSOR_TEST 0x0C11u
#define GL_DEPTH_TEST 0x0B71u
#define GL_LESS 0x0201u
#define GL_FLAT 0x1D00u
#define GL_SMOOTH 0x1D01u
#define GL_MODELVIEW 0x1700u
#define GL_PROJECTION 0x1701u
#define GL_TRIANGLES 4u
#define GL_TRIANGLE_STRIP 5u
#define GL_TRIANGLE_FAN 6u
#define GL_QUADS 7u
#define GL_VENDOR 0x1F00u
#define GL_RENDERER 0x1F01u
#define GL_VERSION 0x1F02u
#define GL_STENCIL_BUFFER_BIT 0x0400u
#define GL_LIGHTING 0x0B50u
#define GL_LIGHT0 0x4000u
#define GL_NORMALIZE 0x0BA1u
#define GL_FOG 0x0B60u
#define GL_STENCIL_TEST 0x0B90u
#define GL_POSITION 0x1203u
#define GL_DIFFUSE 0x1201u
#define GL_SPECULAR 0x1202u
#define GL_SHININESS 0x1601u
#define GL_FRONT 0x0404u
#define GL_FOG_MODE 0x0B65u
#define GL_FOG_START 0x0B63u
#define GL_FOG_END 0x0B64u
#define GL_FOG_COLOR 0x0B66u
#define GL_LINEAR 0x2601u
#define GL_VERTEX_ARRAY 0x8074u
#define GL_NORMAL_ARRAY 0x8075u
#define GL_FLOAT 0x1406u
#define GL_UNSIGNED_SHORT 0x1403u
#define GL_ALWAYS 0x0207u
#define GL_EQUAL 0x0202u
#define GL_KEEP 0x1E00u
#define GL_REPLACE 0x1E01u
#define GL_COMPILE 0x1300u
#define GL_RGBA 0x1908u
#define GL_UNSIGNED_BYTE 0x1401u
#define GL_COLOR 0x1800u
#define GLU_FLAT 100001u
#define GLU_LINE 100011u
#define GLU_FILL 100012u

#define UPDATE_EVENT 6u
#define EVERY_EVENT 0xFFFFu

static uint32_t float_bits(float f)
{
	uint32_t w;
	memcpy(&w, &f, 4);
	return w;
}

size_t gltest_build(uint8_t *out, size_t size, struct ndrv_layout *layout)
{
	static uint8_t data[D_SIZE];
	put_be16(data + D_RECT, WINDOW_TOP);
	put_be16(data + D_RECT + 2, WINDOW_LEFT);
	put_be16(data + D_RECT + 4, WINDOW_TOP + WINDOW_HEIGHT);
	put_be16(data + D_RECT + 6, WINDOW_LEFT + WINDOW_WIDTH);
	data[D_TITLE] = 7;
	memcpy(data + D_TITLE + 1, "GL Test", 7);
	static const uint32_t attribs[] = { AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 16, AGL_STENCIL_SIZE, 8,
					    AGL_ACCELERATED, 0 };
	_Static_assert(D_ATTRIBS + sizeof attribs <= D_VERTEX, "room for the attributes");
	for (unsigned k = 0; k < sizeof attribs / sizeof attribs[0]; k++)
		put_be32(data + D_ATTRIBS + 4 * k, attribs[k]);
	static const uint32_t vertex[3] = { 0x3FB33333u, 0x3F99999Au, 0 };     /* (1.4, 1.2, 0) */
	for (unsigned k = 0; k < 3; k++)
		put_be32(data + D_VERTEX + 4 * k, vertex[k]);
	/* "lit": its vectors; a mesh, a bump on a floor (its normals by sqrt alone: the same on every host) */
	static const float vectors[5][4] = { { 0.4f, 1, 0.6f, 0 }, { 1, 0.95f, 0.85f, 1 }, { 0.3f, 0.55f, 0.9f, 1 },
					     { 0.8f, 0.8f, 0.8f, 1 }, { 0.5f, 0.6f, 0.7f, 1 } };
	for (unsigned k = 0; k < 5; k++)
		for (unsigned j = 0; j < 4; j++)
			put_be32(data + D_LIGHT_POSITION + 16 * k + 4 * j, float_bits(vectors[k][j]));
	static const float colors[2][4] = { { 0.9f, 0.25f, 0.2f, 1 }, { 0.3f, 0.85f, 0.35f, 1 } };
	for (unsigned k = 0; k < 2; k++)
		for (unsigned j = 0; j < 4; j++)
			put_be32(data + D_RED + 16 * k + 4 * j, float_bits(colors[k][j]));
	/* "pixels": an image of four colors' gradients, a glyph (an arrow) */
	for (unsigned j = 0; j < 16; j++)
		for (unsigned i = 0; i < 16; i++) {
			uint8_t *p = data + D_IMAGE + 4 * (16 * j + i);
			bool left = i < 8, low = j < 8;
			p[0] = (uint8_t)(left ? 40 + 12 * j : 230);
			p[1] = (uint8_t)(low ? 60 + 10 * i : 200 - 8 * i);
			p[2] = (uint8_t)(left == low ? 220 : 30 + 6 * (i + j));
			p[3] = 255;
		}
	static const uint16_t glyph[16] = { 0x0180, 0x03C0, 0x07E0, 0x0FF0, 0x1FF8, 0x3FFC, 0x7FFE, 0xFFFF,
					    0x03C0, 0x03C0, 0x03C0, 0x03C0, 0x03C0, 0x03C0, 0x03C0, 0x03C0 };
	for (unsigned j = 0; j < 16; j++)               /* (rows from the bottom: the arrow points down) */
		put_be16(data + D_GLYPH + 4 * j, glyph[j]);
	for (int z = 0; z < MESH_Z; z++)
		for (int x = 0; x < MESH_X; x++) {
			float px = -1.5f + 0.5f * (float)x, pz = -1 + 0.5f * (float)z;
			float ax = 1 - px * px / 2.25f, az = 1 - pz * pz;
			float py = 0.6f * ax * az;
			float n[3] = { 0.6f * 2 * px / 2.25f * az, 1, 0.6f * ax * 2 * pz };     /* (-dy/dx, 1, -dy/dz) */
			float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			const float p[3] = { px, py, pz };
			unsigned at = (unsigned)(z * MESH_X + x) * 12;
			for (unsigned c = 0; c < 3; c++) {
				put_be32(data + D_POSITIONS + at + 4 * c, float_bits(p[c]));
				put_be32(data + D_NORMALS + at + 4 * c, float_bits(n[c] / len));
			}
		}
	for (int z = 0, k = 0; z + 1 < MESH_Z; z++)
		for (int x = 0; x + 1 < MESH_X; x++) {
			int a = z * MESH_X + x, b = a + MESH_X, c = b + 1, d = a + 1;      /* (counterclockwise from above) */
			const int tri[6] = { a, b, c, a, c, d };
			for (int t = 0; t < 6; t++, k++)
				put_be16(data + D_INDICES + 2 * (unsigned)k, (uint16_t)tri[t]);
		}
	const struct ndrv_spec spec = {
		.kind = FRAG_APPLICATION, .entries = 1, .hostcall = HOSTCALL_TOOLS_GL_TEST, .imports = imports,
		.import_count = I_COUNT, .extra = data, .extra_len = sizeof data,
	};
	return ndrv_build(&spec, out, size, layout);
}

/* ---- scenes --------------------------------------------------------------------- */

/*
 * A call: an import, its words (variables, addresses in the data, or
 * values) and floating-point arguments (floats rounded to single precision,
 * as a compiled caller passes them, or doubles)
 */
struct gltest_step {
	uint8_t import;
	uint8_t words, doubles;
	uint8_t result;                 /* the variable it goes to (V_NONE: none) */
	bool show;                      /* print it: a C string */
	uint32_t w[8];
	double f[9];
	bool single;
};

#define VAR(k) (0xF0000000u | (k))
#define DATA(offset) (0xE0000000u | (offset))

#define CALL(name, ...) { I_##name, sizeof (uint32_t[]){ __VA_ARGS__ } / sizeof(uint32_t), 0, V_NONE, false, \
			  { __VA_ARGS__ }, { 0 }, false }
#define CALL0(name) { I_##name, 0, 0, V_NONE, false, { 0 }, { 0 }, false }
#define CALL_INTO(var, name, ...) { I_##name, sizeof (uint32_t[]){ __VA_ARGS__ } / sizeof(uint32_t), 0, var, \
				    false, { __VA_ARGS__ }, { 0 }, false }
#define CALL_SHOW(name, ...) { I_##name, sizeof (uint32_t[]){ __VA_ARGS__ } / sizeof(uint32_t), 0, V_NONE, true, \
			       { __VA_ARGS__ }, { 0 }, false }
#define CALL_F(name, ...) { I_##name, 0, sizeof (double[]){ __VA_ARGS__ } / sizeof(double), V_NONE, false, \
			    { 0 }, { __VA_ARGS__ }, true }
#define CALL_D(name, ...) { I_##name, 0, sizeof (double[]){ __VA_ARGS__ } / sizeof(double), V_NONE, false, \
			    { 0 }, { __VA_ARGS__ }, false }

/* Starting: the Toolbox, the screen's depth (SYSHM_GL_TEST_DEPTH, 32 if not), a window, a context drawing in it */
#define DEPTH 0xD0000000u               /* (the depth, in a word) */
static const struct gltest_step start[] = {
	CALL(InitGraf, DATA(D_THE_PORT)),
	CALL0(InitFonts),
	CALL0(InitWindows),
	CALL0(InitMenus),
	CALL0(TEInit),
	CALL(InitDialogs, 0),
	CALL0(InitCursor),
	CALL_INTO(V_DEVICE, GetMainDevice, 0),
	CALL(SetDepth, VAR(V_DEVICE), DEPTH, 0, 0),
	CALL_INTO(V_WINDOW, NewCWindow, 0, DATA(D_RECT), DATA(D_TITLE), 1, 0, 0xFFFFFFFFu, 0, 0),
	CALL(SetPort, VAR(V_WINDOW)),
	CALL_INTO(V_FORMAT, aglChoosePixelFormat, 0, 0, DATA(D_ATTRIBS)),
	CALL_INTO(V_CONTEXT, aglCreateContext, VAR(V_FORMAT), 0),
	CALL(aglSetDrawable, VAR(V_CONTEXT), VAR(V_WINDOW)),
	CALL(aglSetCurrentContext, VAR(V_CONTEXT)),
	CALL_SHOW(glGetString, GL_VENDOR),
	CALL_SHOW(glGetString, GL_RENDERER),
	CALL_SHOW(glGetString, GL_VERSION),
};

/*
 * "clear": the window cleared to a blue, then (scissored) an orange
 * rectangle 20 pixels in from its bottom left
 */
static const struct gltest_step scene_clear[] = {
	CALL_F(glClearColor, 0.25, 0.5, 0.75, 1.0),
	CALL(glClear, GL_COLOR_BUFFER_BIT),
	CALL(glEnable, GL_SCISSOR_TEST),
	CALL(glScissor, 20, 20, 100, 60),
	CALL_F(glClearColor, 1.0, 0.5, 0.0, 1.0),
	CALL(glClear, GL_COLOR_BUFFER_BIT),
	CALL(glDisable, GL_SCISSOR_TEST),
	CALL(aglSwapBuffers, VAR(V_CONTEXT)),
};

/*
 * "triangle": in perspective (a frustum 4:3, near 1, far 10), a floor
 * from behind the near plane into the distance (a strip, clipped by that
 * plane), then turned 25 degrees about y: a triangle of red, green and
 * blue, and a yellow quad tilted through it (depth tested: the quad in
 * front below where they cross, behind above), and a flat-shaded fan
 */
static const struct gltest_step scene_triangle[] = {
	CALL_F(glClearColor, 0.1, 0.1, 0.2, 1.0),
	CALL_D(glClearDepth, 1.0),
	CALL(glClear, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT),
	CALL(glEnable, GL_DEPTH_TEST),
	CALL(glDepthFunc, GL_LESS),
	CALL(glMatrixMode, GL_PROJECTION),
	CALL0(glLoadIdentity),
	CALL_D(glFrustum, -1.0, 1.0, -0.75, 0.75, 1.0, 10.0),
	CALL(glMatrixMode, GL_MODELVIEW),
	CALL0(glLoadIdentity),
	CALL_F(glTranslatef, 0.0, 0.0, -3.0),
	CALL(glBegin, GL_TRIANGLE_STRIP),
	CALL_F(glColor3f, 0.6, 0.6, 0.6),
	CALL_F(glVertex3f, -3.0, -1.0, 2.5),
	CALL_F(glVertex3f, 3.0, -1.0, 2.5),
	CALL_F(glColor3f, 0.1, 0.3, 0.1),
	CALL_F(glVertex3f, -3.0, -1.0, -6.0),
	CALL_F(glVertex3f, 3.0, -1.0, -6.0),
	CALL0(glEnd),
	CALL0(glPushMatrix),
	CALL_F(glRotatef, 25.0, 0.0, 1.0, 0.0),
	CALL(glBegin, GL_TRIANGLES),
	CALL(glColor3ub, 255, 0, 0),
	CALL_F(glVertex3f, -1.0, -0.8, 0.0),
	CALL(glColor3ub, 0, 255, 0),
	CALL_F(glVertex3f, 1.0, -0.8, 0.0),
	CALL(glColor3ub, 0, 0, 255),
	CALL_F(glVertex3f, 0.0, 0.9, 0.0),
	CALL0(glEnd),
	CALL(glBegin, GL_QUADS),
	CALL_F(glColor3f, 1.0, 0.9, 0.1),
	CALL_F(glVertex3f, -1.2, -0.3, 0.6),
	CALL_F(glVertex3f, 1.2, -0.3, 0.6),
	CALL_F(glVertex3f, 1.2, 0.3, -0.6),
	CALL_F(glVertex3f, -1.2, 0.3, -0.6),
	CALL0(glEnd),
	CALL0(glPopMatrix),
	CALL(glShadeModel, GL_FLAT),
	CALL(glBegin, GL_TRIANGLE_FAN),
	CALL_F(glVertex3f, 1.6, 0.6, 0.0),
	CALL_F(glColor3f, 1.0, 0.0, 1.0),
	CALL_F(glVertex3f, 2.2, 0.6, 0.0),
	CALL_F(glColor3f, 0.0, 1.0, 1.0),
	CALL_F(glVertex3f, 2.0, 1.1, 0.0),
	CALL_F(glColor3f, 1.0, 1.0, 1.0),
	CALL(glVertex3fv, DATA(D_VERTEX) + 0),
	CALL0(glEnd),
	CALL(glShadeModel, GL_SMOOTH),
	CALL(aglSwapBuffers, VAR(V_CONTEXT)),
};

/*
 * "lit": in perspective, a bump on a floor (vertex and normal arrays drawn
 * by glDrawElements), lit by a light from above, its material shiny, fogged
 * into the distance; then a yellow quad drawn only where a triangle was put
 * in the stencil (its colors masked)
 */
static const struct gltest_step scene_lit[] = {
	CALL_F(glClearColor, 0.5, 0.6, 0.7, 1.0),
	CALL_D(glClearDepth, 1.0),
	CALL(glClearStencil, 0),
	CALL(glClear, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT),
	CALL(glEnable, GL_DEPTH_TEST),
	CALL(glMatrixMode, GL_PROJECTION),
	CALL0(glLoadIdentity),
	CALL_D(glFrustum, -1.0, 1.0, -0.75, 0.75, 1.0, 10.0),
	CALL(glMatrixMode, GL_MODELVIEW),
	CALL0(glLoadIdentity),
	CALL_F(glTranslatef, 0.0, -0.4, -3.5),
	CALL_F(glRotatef, 25.0, 1.0, 0.0, 0.0),
	CALL(glEnable, GL_LIGHTING),
	CALL(glEnable, GL_LIGHT0),
	CALL(glEnable, GL_NORMALIZE),
	CALL(glLightfv, GL_LIGHT0, GL_POSITION, DATA(D_LIGHT_POSITION)),
	CALL(glLightfv, GL_LIGHT0, GL_DIFFUSE, DATA(D_LIGHT_COLOR)),
	CALL(glMaterialfv, GL_FRONT, GL_DIFFUSE, DATA(D_DIFFUSE)),
	CALL(glMaterialfv, GL_FRONT, GL_SPECULAR, DATA(D_SPECULAR)),
	{ I_glMaterialf, 2, 1, V_NONE, false, { GL_FRONT, GL_SHININESS }, { 20.0 }, true },
	CALL(glEnable, GL_FOG),
	CALL(glFogi, GL_FOG_MODE, GL_LINEAR),
	{ I_glFogf, 1, 1, V_NONE, false, { GL_FOG_START }, { 3.0 }, true },
	{ I_glFogf, 1, 1, V_NONE, false, { GL_FOG_END }, { 6.0 }, true },
	CALL(glFogfv, GL_FOG_COLOR, DATA(D_FOG_COLOR)),
	CALL(glEnableClientState, GL_VERTEX_ARRAY),
	CALL(glEnableClientState, GL_NORMAL_ARRAY),
	CALL(glVertexPointer, 3, GL_FLOAT, 0, DATA(D_POSITIONS)),
	CALL(glNormalPointer, GL_FLOAT, 0, DATA(D_NORMALS)),
	CALL(glDrawElements, GL_TRIANGLES, MESH_INDICES, GL_UNSIGNED_SHORT, DATA(D_INDICES)),
	CALL(glDisableClientState, GL_VERTEX_ARRAY),
	CALL(glDisableClientState, GL_NORMAL_ARRAY),
	CALL(glDisable, GL_LIGHTING),
	CALL(glDisable, GL_FOG),
	CALL(glDisable, GL_DEPTH_TEST),
	CALL(glEnable, GL_STENCIL_TEST),
	CALL(glStencilFunc, GL_ALWAYS, 1, 0xFF),
	CALL(glStencilOp, GL_KEEP, GL_KEEP, GL_REPLACE),
	CALL(glColorMask, 0, 0, 0, 0),
	CALL0(glLoadIdentity),
	CALL(glBegin, GL_TRIANGLES),
	CALL_F(glVertex3f, 0.5, 0.2, -2.0),
	CALL_F(glVertex3f, 1.4, 0.3, -2.0),
	CALL_F(glVertex3f, 0.9, 1.2, -2.0),
	CALL0(glEnd),
	CALL(glColorMask, 1, 1, 1, 1),
	CALL(glStencilFunc, GL_EQUAL, 1, 0xFF),
	CALL(glStencilOp, GL_KEEP, GL_KEEP, GL_KEEP),
	CALL_F(glColor3f, 1.0, 0.85, 0.1),
	CALL(glBegin, GL_QUADS),
	CALL_F(glVertex3f, 0.6, 0.4, -2.0),
	CALL_F(glVertex3f, 1.6, 0.4, -2.0),
	CALL_F(glVertex3f, 1.6, 1.3, -2.0),
	CALL_F(glVertex3f, 0.6, 1.3, -2.0),
	CALL0(glEnd),
	CALL(glDisable, GL_STENCIL_TEST),
	CALL(aglSwapBuffers, VAR(V_CONTEXT)),
};

/*
 * A call with words and floating-point arguments both: the words where
 * the PowerPC calling convention puts them (a double skips two, a float
 * one), the floating-point values in f1 on
 */
#define CALL_WF(name, single, words, ...) { I_##name, sizeof (uint32_t[])words / sizeof(uint32_t), \
					    sizeof (double[]){ __VA_ARGS__ } / sizeof(double), V_NONE, false, words, \
					    { __VA_ARGS__ }, single }
#define W(...) { __VA_ARGS__ }

/*
 * "lists": through OpenGLUtility, a perspective and a viewpoint, a
 * quadric; display lists of it (a sphere; a sphere as lines; a ring
 * beneath a call to the first), each called lit in its own color or
 * plain, then the quadric gone
 */
static const struct gltest_step scene_lists[] = {
	CALL_F(glClearColor, 0.15, 0.15, 0.2, 1.0),
	CALL_D(glClearDepth, 1.0),
	CALL(glClear, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT),
	CALL(glEnable, GL_DEPTH_TEST),
	CALL(glMatrixMode, GL_PROJECTION),
	CALL0(glLoadIdentity),
	CALL_D(gluPerspective, 45.0, 4.0 / 3, 1.0, 20.0),
	CALL(glMatrixMode, GL_MODELVIEW),
	CALL0(glLoadIdentity),
	CALL_D(gluLookAt, 0.2, 1.6, 5.2, 0.0, -0.1, 0.0, 0.0, 1.0, 0.0),
	CALL(glEnable, GL_LIGHTING),
	CALL(glEnable, GL_LIGHT0),
	CALL(glLightfv, GL_LIGHT0, GL_POSITION, DATA(D_LIGHT_POSITION)),
	CALL(glLightfv, GL_LIGHT0, GL_DIFFUSE, DATA(D_LIGHT_COLOR)),
	{ I_gluNewQuadric, 0, 0, V_QUADRIC, false, { 0 }, { 0 }, false },
	CALL(glNewList, 1, GL_COMPILE),
	CALL_WF(gluSphere, false, W(VAR(V_QUADRIC), 0, 0, 24, 16), 0.6),
	CALL0(glEndList),
	CALL(gluQuadricDrawStyle, VAR(V_QUADRIC), GLU_LINE),
	CALL(glNewList, 2, GL_COMPILE),
	CALL_WF(gluSphere, false, W(VAR(V_QUADRIC), 0, 0, 12, 8), 0.6),
	CALL0(glEndList),
	CALL(gluQuadricDrawStyle, VAR(V_QUADRIC), GLU_FILL),
	CALL(gluQuadricNormals, VAR(V_QUADRIC), GLU_FLAT),
	CALL(glNewList, 3, GL_COMPILE),
	CALL0(glPushMatrix),
	CALL_F(glTranslatef, 0.0, -0.75, 0.0),
	CALL_F(glRotatef, -90.0, 1.0, 0.0, 0.0),
	CALL_WF(gluDisk, false, W(VAR(V_QUADRIC), 0, 0, 0, 0, 20, 3), 0.35, 1.1),
	CALL0(glPopMatrix),
	CALL(glCallList, 1),
	CALL0(glEndList),
	CALL(glMaterialfv, GL_FRONT, GL_DIFFUSE, DATA(D_DIFFUSE)),
	CALL0(glPushMatrix),
	CALL_F(glTranslatef, -1.7, 0.1, 0.0),
	CALL(glCallList, 1),
	CALL0(glPopMatrix),
	CALL(glMaterialfv, GL_FRONT, GL_DIFFUSE, DATA(D_RED)),
	CALL(glCallList, 3),
	CALL(glMaterialfv, GL_FRONT, GL_DIFFUSE, DATA(D_GREEN)),
	CALL0(glPushMatrix),
	CALL_F(glTranslatef, 1.7, 0.1, 0.0),
	CALL_F(glRotatef, 30.0, 0.0, 1.0, 0.0),
	CALL(glCallList, 1),
	CALL0(glPopMatrix),
	CALL(glDisable, GL_LIGHTING),
	CALL_F(glColor3f, 1.0, 0.9, 0.3),
	CALL0(glPushMatrix),
	CALL_F(glTranslatef, 0.0, 1.35, -1.0),
	CALL(glCallList, 2),
	CALL0(glPopMatrix),
	CALL(gluDeleteQuadric, VAR(V_QUADRIC)),
	CALL(aglSwapBuffers, VAR(V_CONTEXT)),
};

/*
 * "pixels": in window coordinates (gluOrtho2D), an image drawn four times
 * its size, a glyph drawn by glBitmap four times along (each moving the
 * raster position on), the image copied by glCopyPixels, half its size
 * the other way up
 */
static const struct gltest_step scene_pixels[] = {
	CALL_F(glClearColor, 0.2, 0.2, 0.25, 1.0),
	CALL(glClear, GL_COLOR_BUFFER_BIT),
	CALL(glMatrixMode, GL_PROJECTION),
	CALL0(glLoadIdentity),
	CALL_D(gluOrtho2D, 0.0, 320.0, 0.0, 240.0),
	CALL(glMatrixMode, GL_MODELVIEW),
	CALL0(glLoadIdentity),
	CALL_F(glRasterPos2f, 20.3, 30.2),
	CALL_F(glPixelZoom, 4.0, 4.0),
	CALL(glDrawPixels, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, DATA(D_IMAGE)),
	CALL_F(glPixelZoom, 1.0, 1.0),
	CALL_F(glColor3f, 1.0, 0.85, 0.2),
	CALL_F(glRasterPos2f, 120.3, 40.2),
	CALL_WF(glBitmap, true, W(16, 16, 0, 0, 0, 0, DATA(D_GLYPH)), 0.0, 0.0, 20.0, 0.0),
	CALL_WF(glBitmap, true, W(16, 16, 0, 0, 0, 0, DATA(D_GLYPH)), 0.0, 0.0, 20.0, 6.0),
	CALL_WF(glBitmap, true, W(16, 16, 0, 0, 0, 0, DATA(D_GLYPH)), 0.0, 0.0, 20.0, 12.0),
	CALL_WF(glBitmap, true, W(16, 16, 0, 0, 0, 0, DATA(D_GLYPH)), 0.0, 0.0, 20.0, 0.0),
	CALL_F(glRasterPos2f, 200.3, 180.2),
	CALL_F(glPixelZoom, 0.5, -0.5),
	CALL(glCopyPixels, 20, 30, 64, 64, GL_COLOR),
	CALL_F(glPixelZoom, 1.0, 1.0),
	CALL(aglSwapBuffers, VAR(V_CONTEXT)),
};

static const struct {
	const char *name;
	const struct gltest_step *steps;
	unsigned n;
} scenes[] = {
	{ "clear", scene_clear, sizeof scene_clear / sizeof scene_clear[0] },
	{ "triangle", scene_triangle, sizeof scene_triangle / sizeof scene_triangle[0] },
	{ "lit", scene_lit, sizeof scene_lit / sizeof scene_lit[0] },
	{ "lists", scene_lists, sizeof scene_lists / sizeof scene_lists[0] },
	{ "pixels", scene_pixels, sizeof scene_pixels / sizeof scene_pixels[0] },
};

void gltest_init(struct gltest *t)
{
	static uint8_t scratch[32768];
	memset(t, 0, sizeof *t);
	gltest_build(scratch, sizeof scratch, &t->layout);
	const char *want = getenv("SYSHM_GL_TEST_SCENE"), *depth = getenv("SYSHM_GL_TEST_DEPTH");
	t->depth = depth && atoi(depth) == 16 ? 16 : 32;
	t->steps = scenes[0].steps;
	t->nsteps = scenes[0].n;
	for (unsigned k = 0; want && k < sizeof scenes / sizeof scenes[0]; k++)
		if (!strcmp(scenes[k].name, want)) {
			t->steps = scenes[k].steps;
			t->nsteps = scenes[k].n;
		}
}

/* ---- running ---------------------------------------------------------------------- */

/* State words: what it's doing, the step, and what comes after drawing */
enum { S_PHASE, S_STEP, S_AFTER };
enum { P_START, P_DRAW, P_WAIT, P_BEGIN, P_END };

static uint32_t word(const struct gltest *t, const struct ndrv_call *c, uint32_t w)
{
	uint32_t data = c->toc + t->layout.extra;
	if ((w & 0xFFFFFF00u) == 0xF0000000u)
		return mem_r32(c->mem, data + D_VARS + 4 * (w & 0xFF));
	if ((w & 0xFFFF0000u) == 0xE0000000u)
		return data + (w & 0xFFFF);
	if (w == DEPTH)
		return t->depth;
	return w;
}

static void call(struct gltest *t, struct ndrv_call *c, const struct gltest_step *s)
{
	ndrv_callout(c, ndrv_import(c, &t->layout, s->import), 0, 0, 0, 0);
	for (unsigned k = 0; k < s->words; k++)
		c->out[k] = word(t, c, s->w[k]);
	for (unsigned k = 0; k < s->doubles; k++)
		ndrv_callout_double(c, k, s->single ? (double)(float)s->f[k] : s->f[k]);
}

static void step(struct gltest *t, struct ndrv_call *c, unsigned phase, unsigned k, unsigned after)
{
	ndrv_set_state(c, S_PHASE, phase);
	ndrv_set_state(c, S_STEP, k);
	ndrv_set_state(c, S_AFTER, after);
	uint32_t window = word(t, c, VAR(V_WINDOW)), data = c->toc + t->layout.extra;
	switch (phase) {
	case P_START:
		call(t, c, &start[k]);
		return;
	case P_DRAW:
		call(t, c, &t->steps[k]);
		return;
	case P_WAIT:
		ndrv_callout(c, ndrv_import(c, &t->layout, I_WaitNextEvent), EVERY_EVENT, data + D_EVENT, 30, 0);
		return;
	default:
		ndrv_callout(c, ndrv_import(c, &t->layout, phase == P_BEGIN ? I_BeginUpdate : I_EndUpdate), window, 0,
			     0, 0);
		return;
	}
}

void gltest_call(struct gltest *t, struct ndrv_call *c)
{
	struct mem *m = c->mem;
	uint32_t data = c->toc + t->layout.extra;
	if (!mem_ok(m, data, D_SIZE, MEM_RW)) {
		ndrv_finish(c, 0);
		return;
	}
	if (c->entry == 0) {            /* main */
		step(t, c, P_START, 0, 0);
		return;
	}
	uint32_t r = c->arg[0], phase = ndrv_state(c, S_PHASE), k = ndrv_state(c, S_STEP);
	uint32_t after = ndrv_state(c, S_AFTER);
	const struct gltest_step *done = phase == P_START && k < sizeof start / sizeof start[0] ? &start[k] :
					 phase == P_DRAW && k < t->nsteps ? &t->steps[k] : NULL;
	if (done && done->result)
		mem_w32(m, data + D_VARS + 4 * done->result, r);
	if (done && done->show) {
		char s[256] = "";
		for (unsigned i = 0; i + 1 < sizeof s && mem_ok(m, r + i, 1, MEM_R) && mem_r8(m, r + i); i++)
			s[i] = (char)mem_r8(m, r + i);
		printf("gltest: %s(0x%X) = \"%s\"\n", import_names[done->import], word(t, c, done->w[0]), s);
		fflush(stdout);
	}
	switch (phase) {
	case P_START:
		if (k + 1 < sizeof start / sizeof start[0])
			step(t, c, P_START, k + 1, 0);
		else
			step(t, c, P_DRAW, 0, P_WAIT);
		return;
	case P_DRAW:
		if (k + 1 < t->nsteps) {
			step(t, c, P_DRAW, k + 1, after);
			return;
		}
		t->frames++;
		step(t, c, after, 0, 0);
		return;
	case P_WAIT:
		if (mem_r16(m, data + D_EVENT) == UPDATE_EVENT &&
		    mem_r32(m, data + D_EVENT + 2) == word(t, c, VAR(V_WINDOW)))
			step(t, c, P_BEGIN, 0, 0);
		else
			step(t, c, P_WAIT, 0, 0);
		return;
	case P_BEGIN:
		step(t, c, P_DRAW, 0, P_END);
		return;
	default:
		step(t, c, P_WAIT, 0, 0);
		return;
	}
}
