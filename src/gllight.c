#include "glfront.h"

/* (the same results on every host: no fused multiply-adds where some have them; GCC fuses none in ISO C) */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

#include <math.h>
#include <string.h>

/*
 * Lighting as OpenGL 1.2's section 2.13.1 has it, per vertex in eye
 * coordinates (as the Rage 128's OpenGL did it: on the processor, the chip
 * had no transform engine); fog by the eye-space |z| (Mac OS 9 has no fog
 * coordinates: its OpenGLLibrary exports no glFogCoord); texture
 * coordinates generated in object or eye space, by
 * the sphere map, or as the eye-space normal or reflection vector
 * (GL_NV_texgen_reflection's); user clip planes as distances in eye space. Powers are
 * libm's in doubles (rounded to floats: the same on every host but in
 * cases far rarer than one in a million), cosines the front end's own.
 */

static void set4(float v[4], float a, float b, float c, float d)
{
	v[0] = a;
	v[1] = b;
	v[2] = c;
	v[3] = d;
}

void glf_lighting_init(struct gl_state *st)
{
	for (unsigned k = 0; k < GLF_LIGHTS; k++) {
		struct gl_light *l = &st->lights[k];
		set4(l->ambient, 0, 0, 0, 1);
		if (k == 0) {
			set4(l->diffuse, 1, 1, 1, 1);
			set4(l->specular, 1, 1, 1, 1);
		} else {
			set4(l->diffuse, 0, 0, 0, 1);
			set4(l->specular, 0, 0, 0, 1);
		}
		set4(l->position, 0, 0, 1, 0);
		l->spot_direction[2] = -1;
		l->spot_cutoff = 180;
		l->attenuation[0] = 1;
	}
	for (int f = 0; f < 2; f++) {
		struct gl_material *m = &st->material[f];
		set4(m->ambient, 0.2f, 0.2f, 0.2f, 1);
		set4(m->diffuse, 0.8f, 0.8f, 0.8f, 1);
		set4(m->specular, 0, 0, 0, 1);
		set4(m->emission, 0, 0, 0, 1);
	}
	set4(st->light_model_ambient, 0.2f, 0.2f, 0.2f, 1);
	st->color_material_face = GL_FRONT_AND_BACK;
	st->color_material_mode = GL_AMBIENT_AND_DIFFUSE;
	st->fog_mode = GL_EXP;
	st->fog_density = 1;
	st->fog_end = 1;
	for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++)
		for (int c = 0; c < 4; c++) {
			struct gl_texgen *g = &st->texgen[u][c];
			g->mode = GL_EYE_LINEAR;
			g->object[c < 2 ? c : 3] = c < 2 ? 1 : 0;       /* (s: x, t: y; r and q: none) */
			g->eye[c < 2 ? c : 3] = c < 2 ? 1 : 0;
		}
}

/* ---- matrices ----------------------------------------------------------------------------- */

static const float *modelview(const struct gl_state *st)
{
	return st->modelview.m[st->modelview.depth - 1];
}

/* The inverse of a 4x4 matrix (column-major); false if it has none */
static bool invert(const float m[16], float out[16])
{
	float inv[16];
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
		 m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
		 m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
		 m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
		  m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
		 m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
		 m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
		 m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
		  m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
		 m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
		 m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
		  m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
		  m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
		 m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
		 m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
		  m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
		  m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (!(det != 0) || !isfinite(det))
		return false;
	for (int k = 0; k < 16; k++)
		out[k] = inv[k] / det;
	return true;
}

/* A plane given in object coordinates now, in eye coordinates: p times the modelview's inverse */
static void eye_plane(const struct gl_state *st, const double p[4], float out[4])
{
	float inv[16];
	if (!invert(modelview(st), inv)) {
		for (int k = 0; k < 4; k++)
			out[k] = (float)p[k];
		return;
	}
	for (int c = 0; c < 4; c++)
		out[c] = (float)p[0] * inv[4 * c] + (float)p[1] * inv[4 * c + 1] + (float)p[2] * inv[4 * c + 2] +
			 (float)p[3] * inv[4 * c + 3];
}

/* Normals into eye space: by the modelview's upper 3x3, inverted and transposed (OpenGL 1.2's 2.10.3) */
static void normal_matrix(struct gl_state *st)
{
	if (!st->normal_stale)
		return;
	st->normal_stale = false;
	const float *m = modelview(st);
	/* (the 3x3: a[row][column] = m[column * 4 + row]) */
	float a00 = m[0], a01 = m[4], a02 = m[8], a10 = m[1], a11 = m[5], a12 = m[9], a20 = m[2], a21 = m[6],
	      a22 = m[10];
	float c00 = a11 * a22 - a12 * a21, c01 = a12 * a20 - a10 * a22, c02 = a10 * a21 - a11 * a20;
	float det = a00 * c00 + a01 * c01 + a02 * c02;
	float *n = st->normal_matrix;
	if (!(det != 0) || !isfinite(det)) {
		memset(n, 0, 9 * sizeof *n);
		st->rescale = 1;
		return;
	}
	/* inverse[i][j] = cofactor[j][i] / det; normals go by its transpose: n'_j = sum_i n_i inverse[i][j] */
	float inv[3][3] = {
		{ c00 / det, (a02 * a21 - a01 * a22) / det, (a01 * a12 - a02 * a11) / det },
		{ c01 / det, (a00 * a22 - a02 * a20) / det, (a02 * a10 - a00 * a12) / det },
		{ c02 / det, (a01 * a20 - a00 * a21) / det, (a00 * a11 - a01 * a10) / det },
	};
	for (int j = 0; j < 3; j++)
		for (int i = 0; i < 3; i++)
			n[3 * j + i] = inv[i][j];               /* (n'_j = sum over i of n[3j + i] * n_i) */
	float r = sqrtf(inv[2][0] * inv[2][0] + inv[2][1] * inv[2][1] + inv[2][2] * inv[2][2]);
	st->rescale = r > 0 ? 1 / r : 1;
}

/* ---- the state ------------------------------------------------------------------------------- */

static bool is_color(uint32_t pname)
{
	return pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR;
}

void glf_light(struct gl_state *st, uint32_t light, uint32_t pname, const float *v)
{
	if (light - GL_LIGHT0 >= GLF_LIGHTS) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	struct gl_light *l = &st->lights[light - GL_LIGHT0];
	const float *m = modelview(st);
	switch (pname) {
	case GL_AMBIENT: memcpy(l->ambient, v, 4 * sizeof *v); break;
	case GL_DIFFUSE: memcpy(l->diffuse, v, 4 * sizeof *v); break;
	case GL_SPECULAR: memcpy(l->specular, v, 4 * sizeof *v); break;
	case GL_POSITION:
		for (int i = 0; i < 4; i++)
			l->position[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
		break;
	case GL_SPOT_DIRECTION:
		for (int i = 0; i < 3; i++)
			l->spot_direction[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2];
		break;
	case GL_SPOT_EXPONENT:
		if (!(v[0] >= 0 && v[0] <= 128)) {
			glf_error(st, GL_INVALID_VALUE);
			return;
		}
		l->spot_exponent = v[0];
		break;
	case GL_SPOT_CUTOFF:
		if (!((v[0] >= 0 && v[0] <= 90) || v[0] == 180)) {
			glf_error(st, GL_INVALID_VALUE);
			return;
		}
		l->spot_cutoff = v[0];
		break;
	case GL_CONSTANT_ATTENUATION:
	case GL_LINEAR_ATTENUATION:
	case GL_QUADRATIC_ATTENUATION:
		if (!(v[0] >= 0)) {
			glf_error(st, GL_INVALID_VALUE);
			return;
		}
		l->attenuation[pname - GL_CONSTANT_ATTENUATION] = v[0];
		break;
	default:
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
}

void glf_light_model(struct gl_state *st, uint32_t pname, const float *v)
{
	switch (pname) {
	case GL_LIGHT_MODEL_AMBIENT: memcpy(st->light_model_ambient, v, 4 * sizeof *v); break;
	case GL_LIGHT_MODEL_LOCAL_VIEWER: st->local_viewer = v[0] != 0; break;
	case GL_LIGHT_MODEL_TWO_SIDE: st->two_side = v[0] != 0; break;
	case GL_LIGHT_MODEL_SPECULAR_VECTOR_APPLE: st->specular_vector = v[0] != 0; break;
	case GL_LIGHT_MODEL_COLOR_CONTROL:
		if (v[0] != GL_SINGLE_COLOR && v[0] != GL_SEPARATE_SPECULAR_COLOR) {
			glf_error(st, GL_INVALID_ENUM);
			return;
		}
		st->separate_specular = v[0] == GL_SEPARATE_SPECULAR_COLOR;
		st->raster_stale = true;
		break;
	default:
		glf_error(st, GL_INVALID_ENUM);
		break;
	}
}

static bool is_face(uint32_t face)
{
	return face == GL_FRONT || face == GL_BACK || face == GL_FRONT_AND_BACK;
}

/* A material's property for a face (both: each) */
static void set_material(struct gl_state *st, uint32_t face, uint32_t pname, const float *v)
{
	for (int f = 0; f < 2; f++) {
		if (face != GL_FRONT_AND_BACK && face != (f ? GL_BACK : GL_FRONT))
			continue;
		struct gl_material *m = &st->material[f];
		switch (pname) {
		case GL_AMBIENT: memcpy(m->ambient, v, 4 * sizeof *v); break;
		case GL_DIFFUSE: memcpy(m->diffuse, v, 4 * sizeof *v); break;
		case GL_AMBIENT_AND_DIFFUSE:
			memcpy(m->ambient, v, 4 * sizeof *v);
			memcpy(m->diffuse, v, 4 * sizeof *v);
			break;
		case GL_SPECULAR: memcpy(m->specular, v, 4 * sizeof *v); break;
		case GL_EMISSION: memcpy(m->emission, v, 4 * sizeof *v); break;
		case GL_SHININESS: m->shininess = v[0]; break;
		default: break;                 /* (color indexes: no color index mode) */
		}
	}
}

void glf_material(struct gl_state *st, uint32_t face, uint32_t pname, const float *v)
{
	bool known = is_color(pname) || pname == GL_EMISSION || pname == GL_AMBIENT_AND_DIFFUSE ||
		     pname == GL_SHININESS || pname == GL_COLOR_INDEXES;
	if (!is_face(face) || !known) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	if (pname == GL_SHININESS && !(v[0] >= 0 && v[0] <= 128)) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	set_material(st, face, pname, v);
}

/* While GL_COLOR_MATERIAL is on, the material's property it names is the current color */
static void track_color(struct gl_state *st)
{
	if (st->caps[GLF_CAP_COLOR_MATERIAL])
		set_material(st, st->color_material_face, st->color_material_mode, st->color);
}

void glf_color_material(struct gl_state *st, uint32_t face, uint32_t mode)
{
	if (!is_face(face) || !(is_color(mode) || mode == GL_EMISSION || mode == GL_AMBIENT_AND_DIFFUSE)) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	st->color_material_face = face;
	st->color_material_mode = mode;
}

void glf_fog(struct gl_state *st, uint32_t pname, const float *v)
{
	switch (pname) {
	case GL_FOG_MODE:
		if (v[0] != GL_LINEAR && v[0] != GL_EXP && v[0] != GL_EXP2) {
			glf_error(st, GL_INVALID_ENUM);
			return;
		}
		st->fog_mode = (uint32_t)v[0];
		break;
	case GL_FOG_DENSITY:
		if (!(v[0] >= 0)) {
			glf_error(st, GL_INVALID_VALUE);
			return;
		}
		st->fog_density = v[0];
		break;
	case GL_FOG_START: st->fog_start = v[0]; break;
	case GL_FOG_END: st->fog_end = v[0]; break;
	case GL_FOG_COLOR:
		for (int k = 0; k < 4; k++)
			st->fog_color[k] = v[k] < 0 ? 0 : v[k] > 1 ? 1 : v[k];
		break;
	case GL_FOG_INDEX: break;
	default:
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	st->raster_stale = true;
}

void glf_tex_gen(struct gl_state *st, uint32_t coord, uint32_t pname, const double *v)
{
	if (coord - GL_S >= 4) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	struct gl_texgen *g = &st->texgen[st->active_texture][coord - GL_S];
	switch (pname) {
	case GL_TEXTURE_GEN_MODE: {
		uint32_t mode = (uint32_t)v[0];
		bool reflection = mode == GL_NORMAL_MAP_NV || mode == GL_REFLECTION_MAP_NV;
		if ((mode != GL_EYE_LINEAR && mode != GL_OBJECT_LINEAR && mode != GL_SPHERE_MAP && !reflection) ||
		    v[0] != mode || (mode == GL_SPHERE_MAP && coord > GL_T) || (reflection && coord == GL_Q)) {
			glf_error(st, GL_INVALID_ENUM);
			return;
		}
		g->mode = mode;
		break;
	}
	case GL_OBJECT_PLANE:
		for (int k = 0; k < 4; k++)
			g->object[k] = (float)v[k];
		break;
	case GL_EYE_PLANE: eye_plane(st, v, g->eye); break;
	default: glf_error(st, GL_INVALID_ENUM); break;
	}
}

void glf_clip_plane(struct gl_state *st, uint32_t plane, const double eq[4])
{
	if (plane - GL_CLIP_PLANE0 >= GLF_CLIP_PLANES) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	eye_plane(st, eq, st->clip_planes[plane - GL_CLIP_PLANE0]);
}

/* ---- queries -------------------------------------------------------------------------------- */

static unsigned values(double *v, const float *f, unsigned n)
{
	for (unsigned k = 0; k < n; k++)
		v[k] = f[k];
	return n;
}

unsigned glf_get_light(struct gl_state *st, uint32_t light, uint32_t pname, double *v)
{
	if (light - GL_LIGHT0 >= GLF_LIGHTS) {
		glf_error(st, GL_INVALID_ENUM);
		return 0;
	}
	const struct gl_light *l = &st->lights[light - GL_LIGHT0];
	switch (pname) {
	case GL_AMBIENT: return values(v, l->ambient, 4);
	case GL_DIFFUSE: return values(v, l->diffuse, 4);
	case GL_SPECULAR: return values(v, l->specular, 4);
	case GL_POSITION: return values(v, l->position, 4);
	case GL_SPOT_DIRECTION: return values(v, l->spot_direction, 3);
	case GL_SPOT_EXPONENT: return values(v, &l->spot_exponent, 1);
	case GL_SPOT_CUTOFF: return values(v, &l->spot_cutoff, 1);
	case GL_CONSTANT_ATTENUATION:
	case GL_LINEAR_ATTENUATION:
	case GL_QUADRATIC_ATTENUATION: return values(v, &l->attenuation[pname - GL_CONSTANT_ATTENUATION], 1);
	default: glf_error(st, GL_INVALID_ENUM); return 0;
	}
}

unsigned glf_get_material(struct gl_state *st, uint32_t face, uint32_t pname, double *v)
{
	if (face != GL_FRONT && face != GL_BACK) {
		glf_error(st, GL_INVALID_ENUM);
		return 0;
	}
	track_color(st);
	const struct gl_material *m = &st->material[face == GL_BACK];
	switch (pname) {
	case GL_AMBIENT: return values(v, m->ambient, 4);
	case GL_DIFFUSE: return values(v, m->diffuse, 4);
	case GL_SPECULAR: return values(v, m->specular, 4);
	case GL_EMISSION: return values(v, m->emission, 4);
	case GL_SHININESS: return values(v, &m->shininess, 1);
	case GL_COLOR_INDEXES: v[0] = 0; v[1] = v[2] = 1; return 3;
	default: glf_error(st, GL_INVALID_ENUM); return 0;
	}
}

unsigned glf_get_tex_gen(struct gl_state *st, uint32_t coord, uint32_t pname, double *v)
{
	if (coord - GL_S >= 4) {
		glf_error(st, GL_INVALID_ENUM);
		return 0;
	}
	const struct gl_texgen *g = &st->texgen[st->active_texture][coord - GL_S];
	switch (pname) {
	case GL_TEXTURE_GEN_MODE: v[0] = g->mode; return 1;
	case GL_OBJECT_PLANE: return values(v, g->object, 4);
	case GL_EYE_PLANE: return values(v, g->eye, 4);
	default: glf_error(st, GL_INVALID_ENUM); return 0;
	}
}

unsigned glf_get_clip_plane(struct gl_state *st, uint32_t plane, double *v)
{
	if (plane - GL_CLIP_PLANE0 >= GLF_CLIP_PLANES) {
		glf_error(st, GL_INVALID_ENUM);
		return 0;
	}
	return values(v, st->clip_planes[plane - GL_CLIP_PLANE0], 4);
}

enum { GL_COLOR_MATERIAL_FACE = 0x0B55, GL_COLOR_MATERIAL_PARAMETER, GL_MAX_LIGHTS = 0x0D31,
       GL_MAX_CLIP_PLANES = 0x0D32 };

unsigned glf_get_lighting(struct gl_state *st, uint32_t pname, double *v)
{
	switch (pname) {
	case GL_LIGHT_MODEL_AMBIENT: return values(v, st->light_model_ambient, 4);
	case GL_LIGHT_MODEL_LOCAL_VIEWER: v[0] = st->local_viewer; return 1;
	case GL_LIGHT_MODEL_TWO_SIDE: v[0] = st->two_side; return 1;
	case GL_LIGHT_MODEL_SPECULAR_VECTOR_APPLE: v[0] = st->specular_vector; return 1;
	case GL_LIGHT_MODEL_COLOR_CONTROL: v[0] = st->separate_specular ? GL_SEPARATE_SPECULAR_COLOR : GL_SINGLE_COLOR;
		return 1;
	case GL_COLOR_MATERIAL_FACE: v[0] = st->color_material_face; return 1;
	case GL_COLOR_MATERIAL_PARAMETER: v[0] = st->color_material_mode; return 1;
	case GL_MAX_LIGHTS: v[0] = GLF_LIGHTS; return 1;
	case GL_MAX_CLIP_PLANES: v[0] = GLF_CLIP_PLANES; return 1;
	case GL_FOG_MODE: v[0] = st->fog_mode; return 1;
	case GL_FOG_DENSITY: v[0] = st->fog_density; return 1;
	case GL_FOG_START: v[0] = st->fog_start; return 1;
	case GL_FOG_END: v[0] = st->fog_end; return 1;
	case GL_FOG_COLOR: return values(v, st->fog_color, 4);
	case GL_FOG_INDEX: v[0] = 0; return 1;
	default: return 0;
	}
}

/* ---- vertices ---------------------------------------------------------------------------------- */

static float dot3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void normalize(float v[3])
{
	float len = sqrtf(dot3(v, v));
	if (len > 0)
		for (int k = 0; k < 3; k++)
			v[k] /= len;
}

static void cross3(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static float clampf(float v)
{
	return v > 0 ? (v < 1 ? v : 1) : 0;
}

/* x^y (x >= 0) */
static float power(float x, float y)
{
	return y == 0 ? 1 : x > 0 ? (float)pow(x, y) : 0;
}

/*
 * A face's lit colors at a vertex (eye coordinates; its normal n, for the
 * face): primary and secondary (all specular in the secondary, if separate)
 */
static void light_face(const struct gl_state *st, const struct gl_material *m, const float eye[4], const float n[3],
		       float primary[4], float secondary[4])
{
	float c[3], spec[3] = { 0, 0, 0 };
	for (int k = 0; k < 3; k++)
		c[k] = m->emission[k] + m->ambient[k] * st->light_model_ambient[k];
	for (unsigned i = 0; i < GLF_LIGHTS; i++) {
		if (!st->caps[GLF_CAP_LIGHT0 + i])
			continue;
		const struct gl_light *l = &st->lights[i];
		float vp[3], att = 1;
		if (l->position[3] != 0) {
			for (int k = 0; k < 3; k++)
				vp[k] = l->position[k] / l->position[3] - eye[k];
			float d = sqrtf(dot3(vp, vp));
			if (d > 0)
				for (int k = 0; k < 3; k++)
					vp[k] /= d;
			float denom = l->attenuation[0] + l->attenuation[1] * d + l->attenuation[2] * d * d;
			att = denom > 0 ? 1 / denom : 1;
		} else {
			memcpy(vp, l->position, 3 * sizeof *vp);
			normalize(vp);
		}
		float spot = 1;
		if (l->spot_cutoff != 180) {
			float s[3];
			memcpy(s, l->spot_direction, sizeof s);
			normalize(s);
			float cosine = -dot3(vp, s);
			double sd, cd;
			glf_sincos(l->spot_cutoff, &sd, &cd);
			spot = cosine >= (float)cd ? power(cosine, l->spot_exponent) : 0;
		}
		float f = att * spot;
		if (!(f > 0))
			continue;
		float ndotl = dot3(n, vp);
		for (int k = 0; k < 3; k++)
			c[k] += f * m->ambient[k] * l->ambient[k];
		if (!(ndotl > 0))
			continue;
		for (int k = 0; k < 3; k++)
			c[k] += f * ndotl * m->diffuse[k] * l->diffuse[k];
		float e[3] = { 0, 0, 1 };              /* (toward the eye) */
		if (st->local_viewer) {
			e[0] = -eye[0];
			e[1] = -eye[1];
			e[2] = -eye[2];
			normalize(e);
		}
		float ndoth;
		if (st->specular_vector) {     /* (GL_APPLE_specular_vector: the light's reflection, 2 n x (n x l) + l) */
			float nl[3], r[3];
			cross3(n, vp, nl);
			cross3(n, nl, r);
			for (int k = 0; k < 3; k++)
				r[k] = 2 * r[k] + vp[k];
			ndoth = dot3(r, e);
		} else {
			float h[3];
			for (int k = 0; k < 3; k++)
				h[k] = vp[k] + e[k];
			normalize(h);
			ndoth = dot3(n, h);
		}
		if (ndoth > 0) {
			float p = f * power(ndoth, m->shininess);
			for (int k = 0; k < 3; k++)
				spec[k] += p * m->specular[k] * l->specular[k];
		}
	}
	for (int k = 0; k < 3; k++) {
		if (st->separate_specular) {
			primary[k] = clampf(c[k]);
			secondary[k] = clampf(spec[k]);
		} else {
			primary[k] = clampf(c[k] + spec[k]);
			secondary[k] = 0;
		}
	}
	primary[3] = clampf(m->diffuse[3]);
	secondary[3] = 0;
}

void glf_shade(struct gl_state *st, const float obj[4], struct gl_vertex *v)
{
	unsigned unit_gen = 0;                  /* (bit 4u + c: unit u generates coordinate c) */
	bool sphere = false, eye_gen = false;   /* (sphere: the sphere map's or the reflection modes') */
	for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++)
		for (unsigned c = 0; c < 4; c++)
			if (st->caps[GLF_CAP_TEXTURE_GEN_S + c] >> u & 1) {
				uint32_t mode = st->texgen[u][c].mode;
				unit_gen |= 1u << (4 * u + c);
				sphere |= mode == GL_SPHERE_MAP || mode == GL_NORMAL_MAP_NV || mode == GL_REFLECTION_MAP_NV;
				eye_gen |= mode == GL_EYE_LINEAR;
			}
	bool lighting = st->caps[GLF_CAP_LIGHTING];
	bool fog = st->caps[GLF_CAP_FOG];
	unsigned planes = 0;
	for (unsigned k = 0; k < GLF_CLIP_PLANES; k++)
		planes |= st->caps[GLF_CAP_CLIP_PLANE0 + k] ? 1u << k : 0;
	float eye[4] = { 0, 0, 0, 1 }, n[3] = { 0, 0, 1 };
	if (lighting || fog || sphere || eye_gen || planes) {
		const float *m = modelview(st);
		for (int i = 0; i < 4; i++)
			eye[i] = m[i] * obj[0] + m[4 + i] * obj[1] + m[8 + i] * obj[2] + m[12 + i] * obj[3];
	}
	if (lighting || sphere) {
		normal_matrix(st);
		const float *nm = st->normal_matrix, *cn = st->normal;
		for (int j = 0; j < 3; j++)
			n[j] = nm[3 * j] * cn[0] + nm[3 * j + 1] * cn[1] + nm[3 * j + 2] * cn[2];
		if (st->caps[GLF_CAP_NORMALIZE])
			normalize(n);
		else if (st->caps[GLF_CAP_RESCALE_NORMAL])
			for (int j = 0; j < 3; j++)
				n[j] *= st->rescale;
	}

	/* colors */
	if (lighting) {
		track_color(st);
		light_face(st, &st->material[0], eye, n, v->color[0], v->secondary[0]);
		if (st->two_side) {
			float back[3] = { -n[0], -n[1], -n[2] };
			light_face(st, &st->material[1], eye, back, v->color[1], v->secondary[1]);
		} else {
			memcpy(v->color[1], v->color[0], sizeof v->color[0]);
			memcpy(v->secondary[1], v->secondary[0], sizeof v->secondary[0]);
		}
	} else {
		for (int i = 0; i < 4; i++) {
			v->color[0][i] = v->color[1][i] = clampf(st->color[i]);
			v->secondary[0][i] = v->secondary[1][i] = i < 3 ? clampf(st->secondary_color[i]) : 0;
		}
	}
	v->fog = fog ? fabsf(eye[2]) : 0;
	for (unsigned k = 0; k < GLF_CLIP_PLANES; k++)
		v->user[k] = planes >> k & 1 ? dot3(st->clip_planes[k], eye) + st->clip_planes[k][3] * eye[3] : 0;

	/* texture coordinates: generated, then through the texture matrix */
	if (st->texmat_stale) {
		static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++)
			st->texmat_identity[u] = !memcmp(st->texture[u].m[st->texture[u].depth - 1], id, sizeof id);
		st->texmat_stale = false;
	}
	float sphere_st[2] = { 0, 0 }, r[3] = { 0, 0, 0 };
	if (sphere) {
		float u[3] = { eye[0], eye[1], eye[2] };
		normalize(u);
		float d = dot3(n, u);
		for (int k = 0; k < 3; k++)
			r[k] = u[k] - 2 * n[k] * d;
		float mm = 2 * sqrtf(r[0] * r[0] + r[1] * r[1] + (r[2] + 1) * (r[2] + 1));
		sphere_st[0] = mm > 0 ? r[0] / mm + 0.5f : 0.5f;
		sphere_st[1] = mm > 0 ? r[1] / mm + 0.5f : 0.5f;
	}
	for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++) {
		float c[4];
		memcpy(c, st->tex[u], sizeof c);
		for (unsigned k = 0; k < 4; k++) {
			if (!(unit_gen >> (4 * u + k) & 1))
				continue;
			const struct gl_texgen *g = &st->texgen[u][k];
			switch (g->mode) {
			case GL_OBJECT_LINEAR:
				c[k] = g->object[0] * obj[0] + g->object[1] * obj[1] + g->object[2] * obj[2] +
				       g->object[3] * obj[3];
				break;
			case GL_EYE_LINEAR:
				c[k] = g->eye[0] * eye[0] + g->eye[1] * eye[1] + g->eye[2] * eye[2] + g->eye[3] * eye[3];
				break;
			case GL_NORMAL_MAP_NV: c[k] = n[k < 3 ? k : 2]; break;
			case GL_REFLECTION_MAP_NV: c[k] = r[k < 3 ? k : 2]; break;
			default: c[k] = sphere_st[k & 1]; break;
			}
		}
		const float *tm = st->texture[u].m[st->texture[u].depth - 1];
		if (st->texmat_identity[u])
			memcpy(v->tex[u], c, sizeof v->tex[u]);
		else
			for (int i = 0; i < 4; i++)
				v->tex[u][i] = tm[i] * c[0] + tm[4 + i] * c[1] + tm[8 + i] * c[2] + tm[12 + i] * c[3];
	}
	v->edge = st->edge_flag;
}
