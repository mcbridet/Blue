#include "glfront.h"

/* (the same results on every host: no fused multiply-adds where some have them; GCC fuses none in ISO C) */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

#include <stdlib.h>
#include <string.h>

#define MAX_WORDS (1u << 24)            /* (kept of a bigger buffer: the rest counted, as if written) */

enum { GL_2D = 0x0600, GL_3D, GL_3D_COLOR, GL_3D_COLOR_TEXTURE, GL_4D_COLOR_TEXTURE };

/* A word of the mode's records: kept while the program's buffer has room, counted anyway */
static void put(struct gl_state *st, uint32_t w)
{
	struct gl_select *s = &st->select;
	size_t size = (size_t)(s->mode == GL_SELECT ? s->select_size : s->feedback_size);
	if (s->count < size && s->count < MAX_WORDS) {
		if (s->count == s->cap) {
			size_t cap = s->cap ? 2 * s->cap : 256;
			cap = cap < size ? cap : size;
			uint32_t *p = realloc(s->words, cap * sizeof *p);
			if (!p) {
				glf_error(st, GL_OUT_OF_MEMORY);
				s->count++;
				return;
			}
			s->words = p;
			s->cap = cap;
		}
		s->words[s->count] = w;
	}
	s->count++;
}

static void put_float(struct gl_state *st, float f)
{
	uint32_t w;
	memcpy(&w, &f, 4);
	put(st, w);
}

/* A window depth as a hit record has it: 0 to 2^32 - 1 */
static uint32_t z_word(float z)
{
	double v = (double)z * 4294967295.0;
	return v > 0 ? (v < 4294967295.0 ? (uint32_t)v : 0xFFFFFFFFu) : 0;
}

/* The hit since the last record, if there was one, as a record: the names, the least and greatest depth, the names */
static void write_hit(struct gl_state *st)
{
	struct gl_select *s = &st->select;
	if (!s->hit)
		return;
	put(st, s->depth);
	put(st, z_word(s->zmin));
	put(st, z_word(s->zmax));
	for (unsigned k = 0; k < s->depth; k++)
		put(st, s->names[k]);
	s->hits++;
	s->hit = false;
}

void glf_select_buffer(struct gl_state *st, int32_t size, uint32_t buffer)
{
	if (st->select.mode == GL_SELECT) {
		glf_error(st, GL_INVALID_OPERATION);
		return;
	}
	if (size < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	st->select.select_size = size;
	st->select.select_buffer = buffer;
}

void glf_feedback_buffer(struct gl_state *st, int32_t size, uint32_t type, uint32_t buffer)
{
	if (st->select.mode == GL_FEEDBACK) {
		glf_error(st, GL_INVALID_OPERATION);
		return;
	}
	if (size < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	if (type < GL_2D || type > GL_4D_COLOR_TEXTURE) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	st->select.feedback_size = size;
	st->select.feedback_type = type;
	st->select.feedback_buffer = buffer;
}

int32_t glf_render_mode(struct gl_state *st, uint32_t mode, uint32_t *buffer, const uint32_t **words, size_t *count)
{
	struct gl_select *s = &st->select;
	*count = 0;
	*words = NULL;
	if (mode != GL_RENDER && mode != GL_SELECT && mode != GL_FEEDBACK) {
		glf_error(st, GL_INVALID_ENUM);
		return 0;
	}
	if ((mode == GL_SELECT && !s->select_size) || (mode == GL_FEEDBACK && !s->feedback_size)) {
		glf_error(st, GL_INVALID_OPERATION);   /* (no glSelectBuffer, or glFeedbackBuffer, yet: as Mesa) */
		return 0;
	}
	int32_t result = 0;
	if (s->mode != GL_RENDER) {
		bool selecting = s->mode == GL_SELECT;
		if (selecting)
			write_hit(st);
		size_t size = (size_t)(selecting ? s->select_size : s->feedback_size);
		result = s->count > size ? -1 : selecting ? (int32_t)s->hits : (int32_t)s->count;
		*buffer = selecting ? s->select_buffer : s->feedback_buffer;
		*words = s->words;
		*count = s->count < size ? s->count : size;
		*count = *count < s->cap ? *count : s->cap;
		if (selecting)
			s->depth = 0;
	}
	s->mode = mode;
	s->count = 0;
	s->hits = 0;
	s->hit = false;
	s->line_reset = true;
	return result;
}

void glf_init_names(struct gl_state *st)
{
	if (st->select.mode != GL_SELECT)
		return;
	write_hit(st);
	st->select.depth = 0;
}

void glf_load_name(struct gl_state *st, uint32_t name)
{
	struct gl_select *s = &st->select;
	if (s->mode != GL_SELECT)
		return;
	if (!s->depth) {
		glf_error(st, GL_INVALID_OPERATION);
		return;
	}
	write_hit(st);
	s->names[s->depth - 1] = name;
}

void glf_push_name(struct gl_state *st, uint32_t name)
{
	struct gl_select *s = &st->select;
	if (s->mode != GL_SELECT)
		return;
	write_hit(st);
	if (s->depth >= GLF_NAME_STACK) {
		glf_error(st, GL_STACK_OVERFLOW);
		return;
	}
	s->names[s->depth++] = name;
}

void glf_pop_name(struct gl_state *st)
{
	struct gl_select *s = &st->select;
	if (s->mode != GL_SELECT)
		return;
	write_hit(st);
	if (!s->depth) {
		glf_error(st, GL_STACK_UNDERFLOW);
		return;
	}
	s->depth--;
}

void glf_pass_through(struct gl_state *st, float token)
{
	if (st->select.mode != GL_FEEDBACK)
		return;
	put_float(st, GL_PASS_THROUGH_TOKEN);
	put_float(st, token);
}

void glf_select_primitive(struct gl_state *st, uint32_t token, const struct glf_fb_vertex *v, unsigned n)
{
	struct gl_select *s = &st->select;
	if (s->mode == GL_SELECT) {
		for (unsigned k = 0; k < n; k++) {
			float z = v[k].win[2];
			if (!s->hit || z < s->zmin)
				s->zmin = z;
			if (!s->hit || z > s->zmax)
				s->zmax = z;
			s->hit = true;
		}
		return;
	}
	if (s->mode != GL_FEEDBACK)
		return;
	uint32_t type = s->feedback_type;
	put_float(st, (float)token);
	if (token == GL_POLYGON_TOKEN)
		put_float(st, (float)n);
	for (unsigned k = 0; k < n; k++) {
		put_float(st, v[k].win[0]);
		put_float(st, v[k].win[1]);
		if (type == GL_2D)
			continue;
		put_float(st, v[k].win[2]);
		if (type == GL_4D_COLOR_TEXTURE)
			put_float(st, v[k].win[3]);
		if (type == GL_3D)
			continue;
		for (int c = 0; c < 4; c++)
			put_float(st, v[k].color[c]);
		if (type == GL_3D_COLOR)
			continue;
		for (int c = 0; c < 4; c++)
			put_float(st, v[k].tex[c]);
	}
}
