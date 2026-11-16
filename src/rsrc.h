/*
 * Builds resource forks (Inside Macintosh: More Macintosh Toolbox, "Resource
 * Manager"): a header, the resource data (each prefixed with its length),
 * and the resource map with its type list and reference lists. No names.
 * And finds resources in one.
 */
#ifndef BLUE_RSRC_H
#define BLUE_RSRC_H

#include <stddef.h>
#include <stdint.h>

struct rsrc {
	uint32_t type;
	int16_t id;
	const uint8_t *data;
	uint32_t len;
};

/* Resources of one type must be adjacent. Returns the fork's size, or 0 if `size` is too small. */
size_t rsrc_build(const struct rsrc *r, unsigned n, uint8_t *out, size_t size);

/* The index'th resource of a type in a fork of `len` bytes (its data points into the fork): 0, or -1 if none */
int rsrc_find(const uint8_t *fork, size_t len, uint32_t type, unsigned index, struct rsrc *out);

#endif
