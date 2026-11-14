#include "parcels.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "be.h"
#include "lzss.h"

#define CHAIN_FIRST 12u                 /* offset of the first parcel's offset */
#define PARCEL_SIZE 88u                 /* link, type, header size, flags, blob count, blob size, a, b */
#define BLOB_SIZE 60u                   /* type, flags, compression, unpacked size, checksum, size, offset, name */
#define MAX_PARCELS 1024u
#define MAX_BLOB (16u << 20)

static int fail(char *err, size_t errlen, const char *msg, uint32_t at)
{
	snprintf(err, errlen, "parcels: %s at 0x%X", msg, at);
	return -1;
}

static void copy_str(char *dst, const uint8_t *src, size_t n)
{
	size_t len = strnlen((const char *)src, n);
	memcpy(dst, src, len);
	dst[len] = '\0';
}

int parcels_detect(const uint8_t *data, size_t size)
{
	return size >= CHAIN_FIRST + 4 && memcmp(data, "prcl", 4) == 0;
}

static int parse_blob(const uint8_t *data, size_t size, uint32_t at, struct parcel_blob *b, char *err, size_t errlen)
{
	const uint8_t *r = data + at;
	copy_str(b->type, r, 4);
	b->flags = be32(r + 4);
	bool lzss = memcmp(r + 8, "lzss", 4) == 0;
	uint32_t unpacked = be32(r + 12), packed = be32(r + 20), ptr = be32(r + 24);
	copy_str(b->name, r + 28, 32);

	if (ptr > size || packed > size - ptr)
		return fail(err, errlen, "blob data outside the chain", at);
	b->size = lzss ? unpacked : packed;
	if (b->size > MAX_BLOB)
		return fail(err, errlen, "blob too big", at);
	b->data = malloc(b->size ? b->size : 1);
	if (!b->data)
		return fail(err, errlen, "out of memory", at);
	if (!lzss)
		memcpy(b->data, data + ptr, packed);
	else if (lzss_decode(data + ptr, packed, b->data, b->size) != b->size)
		return fail(err, errlen, "blob decompresses short", at);
	return 0;
}

int parcels_parse(const uint8_t *data, size_t size, struct parcels *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof *out);
	if (!parcels_detect(data, size))
		return fail(err, errlen, "no 'prcl' magic", 0);

	for (uint32_t at = be32(data + CHAIN_FIRST); at; at = be32(data + at)) {
		if (out->count >= MAX_PARCELS || at > size || size - at < PARCEL_SIZE) {
			parcels_free(out);
			return fail(err, errlen, "bad parcel link", at);
		}
		const uint8_t *r = data + at;
		uint32_t header = be32(r + 8), count = be32(r + 16), blob_size = be32(r + 20);
		if (blob_size < BLOB_SIZE || header < PARCEL_SIZE || header > size - at ||
		    count > (header - PARCEL_SIZE) / blob_size) {
			parcels_free(out);
			return fail(err, errlen, "bad parcel header", at);
		}

		struct parcel *grown = realloc(out->list, (out->count + 1) * sizeof *grown);
		if (!grown) {
			parcels_free(out);
			return fail(err, errlen, "out of memory", at);
		}
		out->list = grown;
		struct parcel *p = &out->list[out->count++];
		memset(p, 0, sizeof *p);
		copy_str(p->type, r + 4, 4);
		p->flags = be32(r + 12);
		copy_str(p->a, r + 24, 32);
		copy_str(p->b, r + 56, 32);
		p->blobs = calloc(count ? count : 1, sizeof *p->blobs);
		if (!p->blobs) {
			parcels_free(out);
			return fail(err, errlen, "out of memory", at);
		}
		for (uint32_t k = 0; k < count; k++) {
			p->count = k + 1;
			if (parse_blob(data, size, at + PARCEL_SIZE + k * blob_size, &p->blobs[k], err, errlen)) {
				parcels_free(out);
				return -1;
			}
		}
	}
	return 0;
}

void parcels_free(struct parcels *p)
{
	for (unsigned k = 0; k < p->count; k++) {
		for (unsigned j = 0; j < p->list[k].count; j++)
			free(p->list[k].blobs[j].data);
		free(p->list[k].blobs);
	}
	free(p->list);
	memset(p, 0, sizeof *p);
}

const struct parcel_blob *parcels_find(const struct parcels *p, const char *parcel_type, const char *blob_type)
{
	for (unsigned k = 0; k < p->count; k++)
		for (unsigned j = 0; strcmp(p->list[k].type, parcel_type) == 0 && j < p->list[k].count; j++)
			if (strcmp(p->list[k].blobs[j].type, blob_type) == 0)
				return &p->list[k].blobs[j];
	return NULL;
}

/* ---- the device tree ----------------------------------------------------- */

static bool prop_is(const struct dt_node *n, const char *name, const char *s)
{
	const struct dt_prop *p = dt_get(n, name);
	if (!p)
		return false;
	size_t len = strnlen((const char *)p->value, p->len);
	return len == strlen(s) && memcmp(p->value, s, len) == 0;
}

static bool compatible_lists(const struct dt_node *n, const char *s)
{
	const struct dt_prop *p = dt_get(n, "compatible");
	for (uint32_t o = 0; p && o < p->len;) {
		size_t len = strnlen((const char *)p->value + o, p->len - o);
		if (len == strlen(s) && memcmp(p->value + o, s, len) == 0)
			return true;
		o += (uint32_t)len + 1;
	}
	return false;
}

static bool matches(const struct parcel *pc, const struct dt_node *n)
{
	const uint32_t by = PARCEL_MATCH_COMPATIBLE | PARCEL_MATCH_PARENT | PARCEL_MATCH_NAME;
	if ((pc->flags & PARCEL_MATCH_TYPE) && !prop_is(n, "device_type", pc->b))
		return false;
	if (!(pc->flags & by))
		return (pc->flags & PARCEL_MATCH_TYPE) != 0;
	return ((pc->flags & PARCEL_MATCH_COMPATIBLE) && compatible_lists(n, pc->a)) ||
	       ((pc->flags & PARCEL_MATCH_PARENT) && n->parent && prop_is(n->parent, "name", pc->a)) ||
	       ((pc->flags & PARCEL_MATCH_NAME) && prop_is(n, "name", pc->a));
}

struct apply {
	struct dt_node *special[16];
	bool used[64];                  /* the current parcel's BLOB_ONCE blobs, once applied */
	bool matched;
	unsigned props;
};

/* Puts a parcel's blobs in node n (or in the special nodes they name). */
static void apply_blobs(struct apply *a, const struct parcel *pc, struct dt_node *n)
{
	for (unsigned k = 0; k < pc->count; k++) {
		const struct parcel_blob *b = &pc->blobs[k];
		struct dt_node *target = (b->flags & BLOB_IN_SPECIAL) ? a->special[(b->flags & BLOB_SPECIAL) >> 16] : n;
		bool *used = k < 64 ? &a->used[k] : NULL;
		if (!target || (b->flags & BLOB_DEBUG_ONLY) || (used && *used))
			continue;
		if (used && (b->flags & BLOB_ONCE))
			*used = true;
		if (b->flags & BLOB_DELETE) {
			dt_prop_remove(target, b->name);
			a->props++;
		} else if (!(b->flags & BLOB_KEEP_EXISTING) || !dt_get(target, b->name)) {
			dt_prop(target, b->name, b->data, b->size);
			a->props++;
		}
	}
}

static void apply_prop(struct apply *a, const struct parcel *pc, struct dt_node *n)
{
	for (; n; n = n->peer) {
		if (a->matched && (pc->flags & PARCEL_ONCE))
			return;
		if (matches(pc, n)) {
			a->matched = true;
			apply_blobs(a, pc, n);
		}
		apply_prop(a, pc, n->child);
	}
}

unsigned parcels_apply(const struct parcels *p, struct dt_node *root)
{
	struct apply a;
	memset(&a, 0, sizeof a);
	for (unsigned k = 0; k < p->count; k++) {
		const struct parcel *pc = &p->list[k];
		memset(a.used, 0, sizeof a.used);
		a.matched = false;
		if (strcmp(pc->type, "node") == 0) {
			struct dt_node *n = dt_add_node(root, "");      /* (its name is one of its blobs) */
			if (pc->flags & PARCEL_SPECIAL)
				a.special[(pc->flags & PARCEL_SPECIAL) >> 16] = n;
			apply_blobs(&a, pc, n);
		} else if (strcmp(pc->type, "prop") == 0 && !(pc->flags & PARCEL_BOOT_DISK_ONLY)) {
			apply_prop(&a, pc, root);
		}
	}
	return a.props;
}
