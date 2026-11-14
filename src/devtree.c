#include "devtree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "be.h"

static void *xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);
	if (!p) {
		fprintf(stderr, "devtree: out of memory\n");
		exit(1);
	}
	return p;
}

struct dt_node *dt_add_node(struct dt_node *parent, const char *name)
{
	struct dt_node *n = xcalloc(1, sizeof *n);
	n->parent = parent;
	if (parent) {
		if (parent->last_child)
			parent->last_child->peer = n;
		else
			parent->child = n;
		parent->last_child = n;
	}
	dt_prop_str(n, "name", name);
	return n;
}

void dt_free(struct dt_node *root)
{
	if (!root)
		return;
	for (struct dt_node *c = root->child, *next; c; c = next) {
		next = c->peer;
		dt_free(c);
	}
	for (struct dt_prop *p = root->props, *next; p; p = next) {
		next = p->next;
		free(p->value);
		free(p);
	}
	free(root);
}

void dt_prop(struct dt_node *n, const char *name, const void *value, uint32_t len)
{
	struct dt_prop *p;
	for (p = n->props; p; p = p->next)
		if (strcmp(p->name, name) == 0)
			break;
	if (!p) {
		p = xcalloc(1, sizeof *p);
		snprintf(p->name, sizeof p->name, "%s", name);
		if (n->last_prop)
			n->last_prop->next = p;
		else
			n->props = p;
		n->last_prop = p;
	}
	free(p->value);
	p->value = xcalloc(1, len);
	if (len)
		memcpy(p->value, value, len);
	p->len = len;
}

void dt_prop_str(struct dt_node *n, const char *name, const char *s)
{
	dt_prop(n, name, s, (uint32_t)strlen(s) + 1);
}

void dt_prop_remove(struct dt_node *n, const char *name)
{
	for (struct dt_prop **pp = &n->props, *prev = NULL; *pp; prev = *pp, pp = &(*pp)->next) {
		struct dt_prop *p = *pp;
		if (strcmp(p->name, name) != 0)
			continue;
		*pp = p->next;
		if (n->last_prop == p)
			n->last_prop = prev;
		free(p->value);
		free(p);
		return;
	}
}

void dt_prop_u32(struct dt_node *n, const char *name, uint32_t v)
{
	dt_prop_cells(n, name, &v, 1);
}

void dt_prop_cells(struct dt_node *n, const char *name, const uint32_t *cells, unsigned count)
{
	uint8_t *buf = xcalloc(count, 4);
	for (unsigned k = 0; k < count; k++)
		put_be32(buf + 4 * k, cells[k]);
	dt_prop(n, name, buf, 4 * count);
	free(buf);
}

const struct dt_prop *dt_get(const struct dt_node *n, const char *name)
{
	for (const struct dt_prop *p = n->props; p; p = p->next)
		if (strcmp(p->name, name) == 0)
			return p;
	return NULL;
}

struct flat {
	uint8_t *out;
	size_t size, props_end;
	unsigned entries;
	int error;
};

/* Writes `n` at entry `index` and its subtree after it; returns the next free entry. */
static unsigned flatten(struct flat *f, const struct dt_node *n, unsigned index)
{
	if (index >= DT_MAX_ENTRIES) {
		f->error = 1;
		return index;
	}
	uint8_t *entry = f->out + DT_ENTRY_SIZE * index;
	unsigned next = index + 1;

	if (n->props) {
		put_be32(entry + 8, (uint32_t)(f->props_end - DT_ENTRY_SIZE * index));
		for (const struct dt_prop *p = n->props; p; p = p->next) {
			size_t rec = 40 + ((p->len + 3u) & ~3u);
			if (f->props_end + rec > f->size) {
				f->error = 1;
				return next;
			}
			uint8_t *r = f->out + f->props_end;
			memset(r, 0, rec);
			put_be32(r, p->next ? (uint32_t)rec : 0);
			memcpy(r + 4, p->name, strnlen(p->name, DT_PROP_NAME_SIZE));
			put_be32(r + 36, p->len);
			if (p->len)
				memcpy(r + 40, p->value, p->len);
			f->props_end += rec;
		}
	}
	if (n->child) {
		put_be32(entry + 4, DT_ENTRY_SIZE * (next - index));
		next = flatten(f, n->child, next);
	}
	if (n->peer && n->parent) {
		put_be32(entry + 0, DT_ENTRY_SIZE * (next - index));
		next = flatten(f, n->peer, next);
	}
	return next;
}

size_t dt_flatten(const struct dt_node *root, uint8_t *out, size_t size)
{
	size_t table = DT_MAX_ENTRIES * DT_ENTRY_SIZE;
	if (size < table)
		return 0;
	memset(out, 0, size);
	struct flat f = { out, size, table, 0, 0 };
	flatten(&f, root, 0);
	return f.error ? 0 : f.props_end;
}
