/*
 * The device tree handed to Mac OS. Open Firmware normally builds it and the
 * Trampoline flattens a copy for the ROM (NKHWInfo.DeviceTreeBase), from
 * which the Name Registry is populated. We build the tree natively and write
 * the same flattened format:
 *
 *   DT_MAX_ENTRIES node entries {peer, child, props}, each an offset relative
 *   to the entry itself (0 = none), followed by property records
 *   {next (relative, 0 = last), name[32], length, value padded to 4 bytes}.
 */
#ifndef BLUE_DEVTREE_H
#define BLUE_DEVTREE_H

#include <stddef.h>
#include <stdint.h>

#define DT_MAX_ENTRIES 1000u
#define DT_ENTRY_SIZE 12u
#define DT_PROP_NAME_SIZE 32u

struct dt_prop {
	char name[DT_PROP_NAME_SIZE];
	uint8_t *value;
	uint32_t len;
	struct dt_prop *next;
};

struct dt_node {
	struct dt_node *parent, *child, *last_child, *peer;
	struct dt_prop *props, *last_prop;
};

/* Creates a node (with its "name" property); parent may be NULL for the root. */
struct dt_node *dt_add_node(struct dt_node *parent, const char *name);
void dt_free(struct dt_node *root);

/* Sets a property, replacing any existing one of the same name. */
void dt_prop(struct dt_node *n, const char *name, const void *value, uint32_t len);
void dt_prop_str(struct dt_node *n, const char *name, const char *s);          /* with the NUL */
void dt_prop_u32(struct dt_node *n, const char *name, uint32_t v);
void dt_prop_cells(struct dt_node *n, const char *name, const uint32_t *cells, unsigned count);
#define dt_prop_empty(n, name) dt_prop((n), (name), NULL, 0)
void dt_prop_remove(struct dt_node *n, const char *name);

const struct dt_prop *dt_get(const struct dt_node *n, const char *name);

/* Writes the flattened copy; returns the bytes used or 0 if it doesn't fit. */
size_t dt_flatten(const struct dt_node *root, uint8_t *out, size_t size);

#endif
