#include "glfront.h"

/* (the same results on every host: no fused multiply-adds where some have them; GCC fuses none in ISO C) */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

#include <string.h>

#include "be.h"

/*
 * Client arrays: what glVertexPointer and the rest say of each (in the
 * program's memory, big-endian), drawn element by element as
 * glArrayElement would be between glBegin and glEnd: each enabled array
 * sets its current attribute, the vertex array last makes the vertex.
 * glLockArraysEXT's range is kept (compiled arrays are the same arrays).
 */

enum { GL_VERTEX_ARRAY = 0x8074, GL_NORMAL_ARRAY, GL_COLOR_ARRAY, GL_INDEX_ARRAY, GL_TEXTURE_COORD_ARRAY,
       GL_EDGE_FLAG_ARRAY, GL_SECONDARY_COLOR_ARRAY_EXT = 0x845E };

static const uint32_t array_caps[GLF_ARRAY_TEX0 + 1] = {
	[GLF_ARRAY_VERTEX] = GL_VERTEX_ARRAY, [GLF_ARRAY_NORMAL] = GL_NORMAL_ARRAY, [GLF_ARRAY_COLOR] = GL_COLOR_ARRAY,
	[GLF_ARRAY_SECONDARY] = GL_SECONDARY_COLOR_ARRAY_EXT, [GLF_ARRAY_EDGE] = GL_EDGE_FLAG_ARRAY, [GLF_ARRAY_INDEX] = GL_INDEX_ARRAY,
	[GLF_ARRAY_TEX0] = GL_TEXTURE_COORD_ARRAY,
};

void glf_arrays_init(struct gl_state *st)
{
	static const int32_t sizes[GLF_ARRAY_TEX0] = { 4, 3, 4, 3, 1, 1 };
	for (unsigned k = 0; k < GLF_ARRAYS; k++) {
		struct gl_array *a = &st->arrays[k];
		memset(a, 0, sizeof *a);
		a->size = k < GLF_ARRAY_TEX0 ? sizes[k] : 4;
		a->type = k == GLF_ARRAY_EDGE ? GL_UNSIGNED_BYTE : GL_FLOAT;
	}
}

/* The array a capability names (the client active unit's for texture coordinates), or -1 */
static int array_of(const struct gl_state *st, uint32_t cap)
{
	for (unsigned k = 0; k <= GLF_ARRAY_TEX0; k++)
		if (array_caps[k] == cap)
			return k == GLF_ARRAY_TEX0 ? (int)(k + st->client_active_texture) : (int)k;
	return -1;
}

int glf_array_enabled(const struct gl_state *st, uint32_t cap)
{
	int k = array_of(st, cap);
	return k < 0 ? -1 : st->arrays[k].enabled;
}

bool glf_enable_client(struct gl_state *st, uint32_t cap, bool on)
{
	int k = array_of(st, cap);
	if (k < 0)
		return false;
	st->arrays[k].enabled = on;
	return true;
}

static unsigned type_size(uint32_t type)
{
	switch (type) {
	case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
	case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
	case GL_DOUBLE: return 8;
	default: return 4;
	}
}

/* The sizes (bit n: n values) and types (bit k: GL_BYTE + k; bit 10: GL_DOUBLE) an array takes */
static const struct {
	unsigned sizes, types;
} takes[GLF_ARRAY_TEX0 + 1] = {
	[GLF_ARRAY_VERTEX] = { 1u << 2 | 1u << 3 | 1u << 4, 1u << 2 | 1u << 4 | 1u << 6 | 1u << 10 },
	[GLF_ARRAY_NORMAL] = { 1u << 3, 1u << 0 | 1u << 2 | 1u << 4 | 1u << 6 | 1u << 10 },
	[GLF_ARRAY_COLOR] = { 1u << 3 | 1u << 4, 0x7Fu | 1u << 10 },
	[GLF_ARRAY_SECONDARY] = { 1u << 3, 0x7Fu | 1u << 10 },
	[GLF_ARRAY_EDGE] = { 1u << 1, 1u << 1 },
	[GLF_ARRAY_INDEX] = { 1u << 1, 1u << 1 | 1u << 2 | 1u << 4 | 1u << 6 | 1u << 10 },
	[GLF_ARRAY_TEX0] = { 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4, 1u << 2 | 1u << 4 | 1u << 6 | 1u << 10 },
};

void glf_array_pointer(struct gl_state *st, unsigned array, int32_t size, uint32_t type, int32_t stride,
		       uint32_t pointer)
{
	const unsigned kind = array < GLF_ARRAY_TEX0 ? array : GLF_ARRAY_TEX0;
	if (array == GLF_ARRAY_TEX0)
		array += st->client_active_texture;
	if (stride < 0 || size < 1 || size > 4 || !(takes[kind].sizes >> size & 1)) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	if (type - GL_BYTE > 10 || !(takes[kind].types >> (type - GL_BYTE) & 1)) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	struct gl_array *a = &st->arrays[array];
	a->size = size;
	a->type = type;
	a->stride = stride;
	a->pointer = pointer;
}

uint32_t glf_array_stride(const struct gl_array *a)
{
	return a->stride ? (uint32_t)a->stride : (uint32_t)a->size * type_size(a->type);
}

uint64_t glf_array_bytes(const struct gl_array *a, uint32_t n)
{
	return n ? (uint64_t)(n - 1) * glf_array_stride(a) + (uint64_t)a->size * type_size(a->type) : 0;
}

/* glInterleavedArrays' formats (OpenGL 1.2's table 2.5): sizes, the color's type, offsets in floats, the stride */
enum { GL_V2F = 0x2A20, GL_T4F_C4F_N3F_V4F = 0x2A2D };

void glf_interleaved_arrays(struct gl_state *st, uint32_t format, int32_t stride, uint32_t pointer)
{
	static const struct {
		uint8_t tex, color, normal, vertex;     /* sizes (0: none) */
		bool ubyte;                             /* the color's: 4 unsigned bytes (else floats) */
		uint8_t pc, pn, pv, s;                  /* offsets, the stride: in 4 bytes */
	} formats[GL_T4F_C4F_N3F_V4F - GL_V2F + 1] = {
		{ 0, 0, 0, 2, false, 0, 0, 0, 2 },      /* GL_V2F */
		{ 0, 0, 0, 3, false, 0, 0, 0, 3 },      /* GL_V3F */
		{ 0, 4, 0, 2, true, 0, 0, 1, 3 },       /* GL_C4UB_V2F */
		{ 0, 4, 0, 3, true, 0, 0, 1, 4 },       /* GL_C4UB_V3F */
		{ 0, 3, 0, 3, false, 0, 0, 3, 6 },      /* GL_C3F_V3F */
		{ 0, 0, 3, 3, false, 0, 0, 3, 6 },      /* GL_N3F_V3F */
		{ 0, 4, 3, 3, false, 0, 4, 7, 10 },     /* GL_C4F_N3F_V3F */
		{ 2, 0, 0, 3, false, 0, 0, 2, 5 },      /* GL_T2F_V3F */
		{ 4, 0, 0, 4, false, 0, 0, 4, 8 },      /* GL_T4F_V4F */
		{ 2, 4, 0, 3, true, 2, 0, 3, 6 },       /* GL_T2F_C4UB_V3F */
		{ 2, 3, 0, 3, false, 2, 0, 5, 8 },      /* GL_T2F_C3F_V3F */
		{ 2, 0, 3, 3, false, 0, 2, 5, 8 },      /* GL_T2F_N3F_V3F */
		{ 2, 4, 3, 3, false, 2, 6, 9, 12 },     /* GL_T2F_C4F_N3F_V3F */
		{ 4, 4, 3, 4, false, 4, 8, 11, 15 },    /* GL_T4F_C4F_N3F_V4F */
	};
	if (format < GL_V2F || format > GL_T4F_C4F_N3F_V4F) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	if (stride < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	const unsigned k = format - GL_V2F;
	int32_t s = stride ? stride : 4 * formats[k].s;
	struct gl_array *a = st->arrays;
	a[GLF_ARRAY_EDGE].enabled = a[GLF_ARRAY_INDEX].enabled = false;
	struct gl_array *tex = &a[GLF_ARRAY_TEX0 + st->client_active_texture];
	tex->enabled = formats[k].tex != 0;
	if (tex->enabled)
		*tex = (struct gl_array){ true, formats[k].tex, s, GL_FLOAT, pointer, NULL, 0, 0 };
	a[GLF_ARRAY_COLOR].enabled = formats[k].color != 0;
	if (formats[k].color)
		a[GLF_ARRAY_COLOR] = (struct gl_array){ true, formats[k].color, s,
							formats[k].ubyte ? GL_UNSIGNED_BYTE : GL_FLOAT,
							pointer + 4u * formats[k].pc, NULL, 0, 0 };
	a[GLF_ARRAY_NORMAL].enabled = formats[k].normal != 0;
	if (formats[k].normal)
		a[GLF_ARRAY_NORMAL] = (struct gl_array){ true, 3, s, GL_FLOAT, pointer + 4u * formats[k].pn, NULL, 0,
							 0 };
	a[GLF_ARRAY_VERTEX] = (struct gl_array){ true, formats[k].vertex, s, GL_FLOAT, pointer + 4u * formats[k].pv,
						 NULL, 0, 0 };
}

void glf_lock_arrays(struct gl_state *st, int32_t first, int32_t count)
{
	if (first < 0 || count <= 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	if (st->lock_count) {
		glf_error(st, GL_INVALID_OPERATION);
		return;
	}
	st->lock_first = first;
	st->lock_count = count;
}

void glf_unlock_arrays(struct gl_state *st)
{
	if (!st->lock_count) {
		glf_error(st, GL_INVALID_OPERATION);
		return;
	}
	st->lock_first = st->lock_count = 0;
}

/* ---- queries ------------------------------------------------------------------------------- */

unsigned glf_get_arrays(struct gl_state *st, uint32_t pname, double *v)
{
	const struct gl_array *a = st->arrays, *tex = &a[GLF_ARRAY_TEX0 + st->client_active_texture];
	switch (pname) {
	case 0x807A: v[0] = a[GLF_ARRAY_VERTEX].size; return 1;        /* GL_VERTEX_ARRAY_SIZE */
	case 0x807B: v[0] = a[GLF_ARRAY_VERTEX].type; return 1;
	case 0x807C: v[0] = a[GLF_ARRAY_VERTEX].stride; return 1;
	case 0x807E: v[0] = a[GLF_ARRAY_NORMAL].type; return 1;        /* GL_NORMAL_ARRAY_TYPE */
	case 0x807F: v[0] = a[GLF_ARRAY_NORMAL].stride; return 1;
	case 0x8081: v[0] = a[GLF_ARRAY_COLOR].size; return 1;         /* GL_COLOR_ARRAY_SIZE */
	case 0x8082: v[0] = a[GLF_ARRAY_COLOR].type; return 1;
	case 0x8083: v[0] = a[GLF_ARRAY_COLOR].stride; return 1;
	case 0x8085: v[0] = a[GLF_ARRAY_INDEX].type; return 1;         /* GL_INDEX_ARRAY_TYPE */
	case 0x8086: v[0] = a[GLF_ARRAY_INDEX].stride; return 1;
	case 0x8088: v[0] = tex->size; return 1;                        /* GL_TEXTURE_COORD_ARRAY_SIZE */
	case 0x8089: v[0] = tex->type; return 1;
	case 0x808A: v[0] = tex->stride; return 1;
	case 0x808C: v[0] = a[GLF_ARRAY_EDGE].stride; return 1;        /* GL_EDGE_FLAG_ARRAY_STRIDE */
	case 0x845A: v[0] = a[GLF_ARRAY_SECONDARY].size; return 1;     /* GL_SECONDARY_COLOR_ARRAY_SIZE_EXT */
	case 0x845B: v[0] = a[GLF_ARRAY_SECONDARY].type; return 1;
	case 0x845C: v[0] = a[GLF_ARRAY_SECONDARY].stride; return 1;
	case 0x81A8: v[0] = st->lock_first; return 1;                   /* GL_ARRAY_ELEMENT_LOCK_FIRST_EXT */
	case 0x81A9: v[0] = st->lock_count; return 1;
	default: return 0;
	}
}

bool glf_get_pointer(const struct gl_state *st, uint32_t pname, uint32_t *p)
{
	static const struct {
		uint32_t pname;
		unsigned array;
	} pointers[] = {
		{ 0x808E, GLF_ARRAY_VERTEX }, { 0x808F, GLF_ARRAY_NORMAL }, { 0x8090, GLF_ARRAY_COLOR },
		{ 0x8091, GLF_ARRAY_INDEX }, { 0x8092, GLF_ARRAY_TEX0 }, { 0x8093, GLF_ARRAY_EDGE },
		{ 0x845D, GLF_ARRAY_SECONDARY },
	};
	for (unsigned k = 0; k < sizeof pointers / sizeof pointers[0]; k++)
		if (pointers[k].pname == pname) {
			unsigned array = pointers[k].array;
			*p = st->arrays[array == GLF_ARRAY_TEX0 ? array + st->client_active_texture : array].pointer;
			return true;
		}
	if (pname == 0x0DF0 || pname == 0x0DF3) {       /* GL_FEEDBACK_BUFFER_POINTER, GL_SELECTION_BUFFER_POINTER */
		*p = pname == 0x0DF0 ? st->select.feedback_buffer : st->select.select_buffer;
		return true;
	}
	return false;
}

/* ---- drawing ------------------------------------------------------------------------------- */

/* A value of an array's type at p, as a float: normalized (colors', normals') as OpenGL 1.2's table 2.6 says */
static float value(const uint8_t *p, uint32_t type, bool normalized)
{
	switch (type) {
	case GL_BYTE: {
		int32_t c = (int8_t)p[0];
		return normalized ? (float)((2.0 * c + 1) / 255) : (float)c;
	}
	case GL_UNSIGNED_BYTE: return normalized ? (float)(p[0] / 255.0) : (float)p[0];
	case GL_SHORT: {
		int32_t c = (int16_t)be16(p);
		return normalized ? (float)((2.0 * c + 1) / 65535) : (float)c;
	}
	case GL_UNSIGNED_SHORT: return normalized ? (float)(be16(p) / 65535.0) : (float)be16(p);
	case GL_INT: {
		int32_t c = (int32_t)be32(p);
		return normalized ? (float)((2.0 * c + 1) / 4294967295.0) : (float)c;
	}
	case GL_UNSIGNED_INT: return normalized ? (float)(be32(p) / 4294967295.0) : (float)be32(p);
	case GL_DOUBLE: {
		uint64_t b = be64(p);
		double d;
		memcpy(&d, &b, sizeof d);
		return (float)d;
	}
	default: {
		uint32_t b = be32(p);
		float f;
		memcpy(&f, &b, sizeof f);
		return f;
	}
	}
}

/* Whether an enabled array has element i there to be read */
static bool has(const struct gl_array *a, uint32_t i)
{
	return a->enabled && a->data && i - a->first < a->count;
}

/* Element i of an array into v (the array's size of them: v has room; the rest as v has them) */
static void fetch(const struct gl_array *a, uint32_t i, bool normalized, float *v)
{
	const uint8_t *p = a->data + (size_t)(i - a->first) * glf_array_stride(a);
	unsigned size = type_size(a->type);
	for (int k = 0; k < a->size && k < 4; k++)
		v[k] = value(p + (size_t)k * size, a->type, normalized);
}

void glf_array_values(const struct gl_state *st, int32_t i, struct gl_element *el)
{
	static const bool normalized[GLF_ARRAY_TEX0] = { [GLF_ARRAY_NORMAL] = true, [GLF_ARRAY_COLOR] = true,
							 [GLF_ARRAY_SECONDARY] = true };
	el->has = 0;
	if (i < 0)
		return;
	const struct gl_array *a = st->arrays;
	uint32_t e = (uint32_t)i;
	for (unsigned k = 0; k < GLF_ARRAYS; k++) {
		float *v = el->v[k];
		v[0] = v[1] = v[2] = 0;
		v[3] = 1;
		if (k == GLF_ARRAY_INDEX || !has(&a[k], e))
			continue;
		el->has |= 1u << k;
		if (k == GLF_ARRAY_EDGE)
			v[0] = a[k].data[(size_t)(e - a[k].first) * glf_array_stride(&a[k])] != 0;
		else
			fetch(&a[k], e, k < GLF_ARRAY_TEX0 && normalized[k], v);
	}
}

void glf_array_element(struct gl_state *st, const struct gl_target *t, int32_t i)
{
	struct gl_element e;
	glf_array_values(st, i, &e);
	if (e.has >> GLF_ARRAY_EDGE & 1)
		st->edge_flag = e.v[GLF_ARRAY_EDGE][0] != 0;
	for (unsigned u = 0; u < GLF_TEXTURE_UNITS; u++)
		if (e.has >> (GLF_ARRAY_TEX0 + u) & 1)
			memcpy(st->tex[u], e.v[GLF_ARRAY_TEX0 + u], sizeof st->tex[u]);
	if (e.has >> GLF_ARRAY_COLOR & 1)
		memcpy(st->color, e.v[GLF_ARRAY_COLOR], sizeof st->color);
	if (e.has >> GLF_ARRAY_SECONDARY & 1)
		memcpy(st->secondary_color, e.v[GLF_ARRAY_SECONDARY], 3 * sizeof *st->secondary_color);
	if (e.has >> GLF_ARRAY_NORMAL & 1)
		memcpy(st->normal, e.v[GLF_ARRAY_NORMAL], sizeof st->normal);
	if (e.has >> GLF_ARRAY_VERTEX & 1)
		glf_vertex(st, t, e.v[GLF_ARRAY_VERTEX]);
}

void glf_draw_arrays(struct gl_state *st, const struct gl_target *t, uint32_t mode, int32_t first, int32_t count)
{
	if (mode > GL_POLYGON) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	if (count < 0 || first < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	glf_begin(st, mode);
	for (int32_t k = 0; k < count; k++)
		glf_array_element(st, t, first + k);
	glf_end(st, t);
}

unsigned glf_index_size(uint32_t type)
{
	return type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 0;
}

void glf_draw_elements(struct gl_state *st, const struct gl_target *t, uint32_t mode, int32_t count, uint32_t type,
		       const uint8_t *indices)
{
	unsigned size = glf_index_size(type);
	if (mode > GL_POLYGON || !size) {
		glf_error(st, GL_INVALID_ENUM);
		return;
	}
	if (count < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	glf_begin(st, mode);
	for (int32_t k = 0; k < count && indices; k++) {
		const uint8_t *p = indices + (size_t)k * size;
		uint32_t i = size == 1 ? p[0] : size == 2 ? be16(p) : be32(p);
		if (i <= INT32_MAX)
			glf_array_element(st, t, (int32_t)i);
	}
	glf_end(st, t);
}
