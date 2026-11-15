#include "glfront.h"

#include <stdlib.h>
#include <string.h>

/*
 * Display lists' names and contents, in the namespace the contexts sharing
 * textures share too. What a list holds is the libraries' (opengl.c's
 * entries); here it's bytes.
 */

#define MAX_RANGE 65536                 /* (glGenLists' at once: more is no names) */

static struct gl_list *find(struct gl_names *n, uint32_t name)
{
	for (struct gl_list *l = n->lists[name % GLF_NAME_CHAINS]; l; l = l->next)
		if (l->name == name)
			return l;
	return NULL;
}

uint32_t glf_gen_lists(struct gl_state *st, int32_t range)
{
	struct gl_names *n = st->names;
	if (range < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return 0;
	}
	if (!range || range > MAX_RANGE)
		return 0;
	/* the first free run of names from the last one given (from 1 again past the top) */
	uint32_t first = n->next_list ? n->next_list : 1;
	bool wrapped = first == 1;
	for (;;) {
		if (first > UINT32_MAX - (uint32_t)range + 1) {
			if (wrapped)
				return 0;
			wrapped = true;
			first = 1;
			continue;
		}
		uint32_t k = 0;
		while (k < (uint32_t)range && !find(n, first + k))
			k++;
		if (k == (uint32_t)range)
			break;
		first += k + 1;
	}
	for (uint32_t k = 0; k < (uint32_t)range; k++) {
		struct gl_list *l = calloc(1, sizeof *l);
		if (!l) {
			glf_delete_lists(st, first, (int32_t)k);
			glf_error(st, GL_OUT_OF_MEMORY);
			return 0;
		}
		l->name = first + k;
		l->next = n->lists[l->name % GLF_NAME_CHAINS];
		n->lists[l->name % GLF_NAME_CHAINS] = l;
	}
	n->next_list = first + (uint32_t)range;
	return first;
}

void glf_delete_lists(struct gl_state *st, uint32_t first, int32_t range)
{
	struct gl_names *n = st->names;
	if (range < 0) {
		glf_error(st, GL_INVALID_VALUE);
		return;
	}
	for (uint32_t k = 0; k < (uint32_t)range && first + k >= first; k++) {
		uint32_t name = first + k;
		struct gl_list **p = &n->lists[name % GLF_NAME_CHAINS];
		while (*p && (*p)->name != name)
			p = &(*p)->next;
		if (!name || !*p)
			continue;
		struct gl_list *l = *p;
		*p = l->next;
		free(l->data);
		free(l);
	}
}

bool glf_is_list(struct gl_state *st, uint32_t name)
{
	return name && find(st->names, name);
}

const struct gl_list *glf_list(struct gl_state *st, uint32_t name)
{
	return name ? find(st->names, name) : NULL;
}

bool glf_define_list(struct gl_state *st, uint32_t name, uint8_t *data, size_t len)
{
	struct gl_names *n = st->names;
	struct gl_list *l = find(n, name);
	if (!l) {
		l = calloc(1, sizeof *l);
		if (!l) {
			free(data);
			return false;
		}
		l->name = name;
		l->next = n->lists[name % GLF_NAME_CHAINS];
		n->lists[name % GLF_NAME_CHAINS] = l;
	}
	free(l->data);
	l->data = data;
	l->len = len;
	return true;
}

void glf_free_lists(struct gl_names *n)
{
	for (unsigned c = 0; c < GLF_NAME_CHAINS; c++)
		while (n->lists[c]) {
			struct gl_list *l = n->lists[c];
			n->lists[c] = l->next;
			free(l->data);
			free(l);
		}
}
