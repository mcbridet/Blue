/*
 * Builds resource forks (Inside Macintosh: More Macintosh Toolbox, "Resource
 * Manager"): a header, the resource data (each prefixed with its length),
 * and the resource map with its type list and reference lists. No names.
 */
#ifndef SYSHM_RSRC_H
#define SYSHM_RSRC_H

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

#endif
