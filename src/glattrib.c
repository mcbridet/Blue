#include "glfront.h"

#include <stdlib.h>
#include <string.h>

/*
 * glPushAttrib and glPopAttrib: a push keeps a copy of the state, and a
 * pop puts back the groups its mask named (OpenGL 1.2's tables 6.5 to
 * 6.21, with the extensions' state where they put it), texture bindings
 * and the bound textures' parameters through glBindTexture and
 * glTexParameter, so that what draws with them follows. glPushClientAttrib
 * and glPopClientAttrib: the pixel stores, the arrays.
 */

enum { A_CURRENT = 0x1, A_POINT = 0x2, A_LINE = 0x4, A_POLYGON = 0x8, A_POLYGON_STIPPLE = 0x10, A_PIXEL_MODE = 0x20,
       A_LIGHTING = 0x40,
       A_FOG = 0x80, A_DEPTH = 0x100, A_STENCIL = 0x400, A_VIEWPORT = 0x800, A_TRANSFORM = 0x1000,
       A_ENABLE = 0x2000, A_COLOR = 0x4000, A_HINT = 0x8000, A_LIST = 0x20000, A_TEXTURE = 0x40000,
       A_SCISSOR = 0x80000 };
enum { C_PIXEL_STORE = 0x1, C_VERTEX_ARRAY = 0x2 };

/* A texture's parameters (glTexParameter's), as they were */
struct tex_params {
	uint32_t name;
	uint32_t min_filter, mag_filter, wrap_s, wrap_t, wrap_r;
	float border[4], priority, min_lod, max_lod;
	int base_level, max_level;
};

struct gl_saved {
	uint32_t mask;
	struct gl_state st;
	struct tex_params bound[GLF_TEXTURE_UNITS][GLF_TARGETS];
};

struct gl_client_saved {
	uint32_t mask;
	struct gl_pixel_store pack, unpack;
	struct gl_array arrays[GLF_ARRAYS];
	unsigned client_active_texture;
};

void glf_push_attrib(struct gl_state *st, uint32_t mask)
{
	if (st->attrib_depth >= GLF_ATTRIB_DEPTH) {
		glf_error(st, GL_STACK_OVERFLOW);
		return;
	}
	struct gl_saved *s = malloc(sizeof *s);
	if (!s) {
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	s->mask = mask;
	s->st = *st;
	for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++)
		for (int k = 0; k < GLF_TARGETS; k++) {
			const struct gl_texture *t = st->units[u].bound[k];
			struct tex_params *p = &s->bound[u][k];
			p->name = t->name;
			p->min_filter = t->min_filter;
			p->mag_filter = t->mag_filter;
			p->wrap_s = t->wrap_s;
			p->wrap_t = t->wrap_t;
			p->wrap_r = t->wrap_r;
			memcpy(p->border, t->border, sizeof p->border);
			p->priority = t->priority;
			p->min_lod = t->min_lod;
			p->max_lod = t->max_lod;
			p->base_level = t->base_level;
			p->max_level = t->max_level;
		}
	st->attrib_stack[st->attrib_depth++] = s;
}

/* A capability as it was (for every unit, if it's a unit's) */
static void cap(struct gl_state *st, const struct gl_state *was, uint32_t name)
{
	int k = glf_cap(name);
	if (k >= 0)
		st->caps[k] = was->caps[k];
}

static void caps(struct gl_state *st, const struct gl_state *was, unsigned first, unsigned n)
{
	memcpy(st->caps + first, was->caps + first, n);
}

/* The textures bound as they were, with their parameters (the active unit's binding is per unit) */
static void textures(struct gl_state *st, const struct gl_saved *s)
{
	static const uint32_t targets[GLF_TARGETS] = { GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D };
	for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++) {
		st->active_texture = u;
		for (int k = 0; k < GLF_TARGETS; k++) {
			const struct tex_params *p = &s->bound[u][k];
			const struct gl_texture *named = p->name ? glf_texture(st, p->name) : NULL;
			if (named && named->target >= 0 && named->target != k)
				continue;               /* (deleted, and its name used again otherwise) */
			glf_bind_texture(st, targets[k], p->name);
			const struct gl_texture *t = st->units[u].bound[k];
			float v[4];
#define PARAM(field, pname) \
	if (t->field != p->field) { v[0] = (float)p->field; glf_tex_parameter(st, targets[k], pname, v); }
			PARAM(min_filter, 0x2801)
			PARAM(mag_filter, 0x2800)
			PARAM(wrap_s, 0x2802)
			PARAM(wrap_t, 0x2803)
			PARAM(wrap_r, 0x8072)
			PARAM(priority, 0x8066)
			PARAM(min_lod, 0x813A)
			PARAM(max_lod, 0x813B)
			PARAM(base_level, 0x813C)
			PARAM(max_level, 0x813D)
#undef PARAM
			if (memcmp(t->border, p->border, sizeof p->border))
				glf_tex_parameter(st, targets[k], 0x1004, p->border);
		}
	}
}

void glf_pop_attrib(struct gl_state *st)
{
	if (!st->attrib_depth) {
		glf_error(st, GL_STACK_UNDERFLOW);
		return;
	}
	struct gl_saved *s = st->attrib_stack[--st->attrib_depth];
	st->attrib_stack[st->attrib_depth] = NULL;
	const struct gl_state *o = &s->st;
	uint32_t m = s->mask;
	if (m & A_CURRENT) {
		memcpy(st->color, o->color, sizeof st->color);
		memcpy(st->secondary_color, o->secondary_color, sizeof st->secondary_color);
		memcpy(st->normal, o->normal, sizeof st->normal);
		memcpy(st->tex, o->tex, sizeof st->tex);
		st->fog_coord = o->fog_coord;
		st->edge_flag = o->edge_flag;
		st->rpos = o->rpos;
	}
	if (m & A_PIXEL_MODE)
		st->transfer = o->transfer;
	if (m & A_POINT) {
		st->point_size = o->point_size;
		cap(st, o, GL_POINT_SMOOTH);
	}
	if (m & A_LINE) {
		st->line_width = o->line_width;
		st->line_stipple = o->line_stipple;
		st->line_stipple_factor = o->line_stipple_factor;
		cap(st, o, GL_LINE_SMOOTH);
		cap(st, o, 0x0B24);                             /* GL_LINE_STIPPLE */
	}
	if (m & A_POLYGON) {
		st->cull_mode = o->cull_mode;
		st->front_face = o->front_face;
		memcpy(st->polygon_mode, o->polygon_mode, sizeof st->polygon_mode);
		st->offset_factor = o->offset_factor;
		st->offset_units = o->offset_units;
		cap(st, o, GL_CULL_FACE);
		cap(st, o, 0x0B41);                             /* GL_POLYGON_SMOOTH */
		cap(st, o, 0x0B42);                             /* GL_POLYGON_STIPPLE */
		caps(st, o, GLF_CAP_OFFSET_POINT, 3);
	}
	if (m & A_POLYGON_STIPPLE)
		memcpy(st->polygon_stipple, o->polygon_stipple, sizeof st->polygon_stipple);
	if (m & A_LIGHTING) {
		memcpy(st->lights, o->lights, sizeof st->lights);
		memcpy(st->material, o->material, sizeof st->material);
		memcpy(st->light_model_ambient, o->light_model_ambient, sizeof st->light_model_ambient);
		st->local_viewer = o->local_viewer;
		st->two_side = o->two_side;
		st->separate_specular = o->separate_specular;
		st->color_material_face = o->color_material_face;
		st->color_material_mode = o->color_material_mode;
		st->shade_model = o->shade_model;
		caps(st, o, GLF_CAP_LIGHTING, 1 + GLF_LIGHTS);
		cap(st, o, GL_COLOR_MATERIAL);
	}
	if (m & A_FOG) {
		st->fog_mode = o->fog_mode;
		st->fog_density = o->fog_density;
		st->fog_start = o->fog_start;
		st->fog_end = o->fog_end;
		memcpy(st->fog_color, o->fog_color, sizeof st->fog_color);
		cap(st, o, GL_FOG);
	}
	if (m & GL_DEPTH_BUFFER_BIT) {
		st->depth_func = o->depth_func;
		st->clear_depth = o->clear_depth;
		st->depth_mask = o->depth_mask;
		cap(st, o, GL_DEPTH_TEST);
	}
	if (m & GL_STENCIL_BUFFER_BIT) {
		st->stencil_func = o->stencil_func;
		st->stencil_ref = o->stencil_ref;
		st->stencil_mask = o->stencil_mask;
		st->stencil_fail = o->stencil_fail;
		st->stencil_zfail = o->stencil_zfail;
		st->stencil_zpass = o->stencil_zpass;
		st->clear_stencil = o->clear_stencil;
		st->stencil_write_mask = o->stencil_write_mask;
		cap(st, o, GL_STENCIL_TEST);
	}
	if (m & A_VIEWPORT) {
		glf_viewport(st, o->viewport[0], o->viewport[1], o->viewport[2], o->viewport[3]);
		glf_depth_range(st, o->depth_range[0], o->depth_range[1]);
	}
	if (m & A_TRANSFORM) {
		st->matrix_mode = o->matrix_mode;
		memcpy(st->clip_planes, o->clip_planes, sizeof st->clip_planes);
		caps(st, o, GLF_CAP_CLIP_PLANE0, GLF_CLIP_PLANES);
		cap(st, o, GL_NORMALIZE);
		cap(st, o, GL_RESCALE_NORMAL);
	}
	if (m & A_ENABLE)
		memcpy(st->caps, o->caps, sizeof st->caps);
	if (m & GL_COLOR_BUFFER_BIT) {
		st->alpha_func = o->alpha_func;
		st->alpha_ref = o->alpha_ref;
		st->blend_src = o->blend_src;
		st->blend_dst = o->blend_dst;
		st->blend_eq = o->blend_eq;
		memcpy(st->blend_color, o->blend_color, sizeof st->blend_color);
		memcpy(st->clear_color, o->clear_color, sizeof st->clear_color);
		memcpy(st->color_mask, o->color_mask, sizeof st->color_mask);
		cap(st, o, GL_ALPHA_TEST);
		cap(st, o, GL_BLEND);
		cap(st, o, GL_DITHER);
		cap(st, o, 0x0BF1);                             /* GL_INDEX_LOGIC_OP */
		cap(st, o, 0x0BF2);                             /* GL_COLOR_LOGIC_OP */
	}
	if (m & A_HINT)
		memcpy(st->hints, o->hints, sizeof st->hints);
	if (m & A_LIST)
		st->list_base = o->list_base;
	if (m & A_TEXTURE) {
		for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++) {
			st->units[u].env_mode = o->units[u].env_mode;
			memcpy(st->units[u].env_color, o->units[u].env_color, sizeof st->units[u].env_color);
		}
		memcpy(st->texgen, o->texgen, sizeof st->texgen);
		caps(st, o, GLF_CAP_TEXTURE_1D, 2);
		caps(st, o, GLF_CAP_TEXTURE_GEN_S, 4);
		cap(st, o, GL_TEXTURE_3D);
		textures(st, s);
		st->active_texture = o->active_texture;
	}
	if (m & A_SCISSOR) {
		memcpy(st->scissor, o->scissor, sizeof st->scissor);
		cap(st, o, GL_SCISSOR_TEST);
	}
	st->raster_stale = true;
	free(s);
}

void glf_push_client_attrib(struct gl_state *st, uint32_t mask)
{
	if (st->client_depth >= GLF_ATTRIB_DEPTH) {
		glf_error(st, GL_STACK_OVERFLOW);
		return;
	}
	struct gl_client_saved *s = malloc(sizeof *s);
	if (!s) {
		glf_error(st, GL_OUT_OF_MEMORY);
		return;
	}
	s->mask = mask;
	s->pack = st->pack;
	s->unpack = st->unpack;
	memcpy(s->arrays, st->arrays, sizeof s->arrays);
	s->client_active_texture = st->client_active_texture;
	st->client_stack[st->client_depth++] = s;
}

void glf_pop_client_attrib(struct gl_state *st)
{
	if (!st->client_depth) {
		glf_error(st, GL_STACK_UNDERFLOW);
		return;
	}
	struct gl_client_saved *s = st->client_stack[--st->client_depth];
	st->client_stack[st->client_depth] = NULL;
	if (s->mask & C_PIXEL_STORE) {
		st->pack = s->pack;
		st->unpack = s->unpack;
	}
	if (s->mask & C_VERTEX_ARRAY) {
		memcpy(st->arrays, s->arrays, sizeof st->arrays);
		st->client_active_texture = s->client_active_texture;
	}
	free(s);
}

void glf_free(struct gl_state *st)
{
	while (st->attrib_depth)
		free(st->attrib_stack[--st->attrib_depth]);
	while (st->client_depth)
		free(st->client_stack[--st->client_depth]);
}
