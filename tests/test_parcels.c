#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../src/be.h"
#include "../src/devtree.h"
#include "../src/parcels.h"
#include "test.h"

struct chain {
	uint8_t buf[4096];
	uint32_t end;                   /* where blob data goes next */
};

static void put_str(uint8_t *p, const char *s, size_t field)
{
	memset(p, 0, field);
	memcpy(p, s, strlen(s));
}

/* Writes a parcel at `at` (linking to `next`) with its blob records; returns where its blobs go. */
static uint32_t parcel(struct chain *c, uint32_t at, uint32_t next, const char *type, uint32_t flags,
		       const char *a, const char *b, uint32_t count)
{
	uint8_t *r = c->buf + at;
	put_be32(r, next);
	memcpy(r + 4, type, 4);
	put_be32(r + 8, 88 + 60 * count);
	put_be32(r + 12, flags);
	put_be32(r + 16, count);
	put_be32(r + 20, 60);
	put_str(r + 24, a, 32);
	put_str(r + 56, b, 32);
	return at + 88;
}

static void blob(struct chain *c, uint32_t at, const char *type, uint32_t flags, const char *name,
		 const void *data, uint32_t size, bool lzss, uint32_t unpacked)
{
	uint8_t *r = c->buf + at;
	memcpy(r, type, 4);
	put_be32(r + 4, flags);
	memcpy(r + 8, lzss ? "lzss" : "\0\0\0\0", 4);
	put_be32(r + 12, unpacked);
	put_be32(r + 20, size);
	put_be32(r + 24, c->end);
	put_str(r + 28, name, 32);
	memcpy(c->buf + c->end, data, size);
	c->end += (size + 3) & ~3u;
}

static const struct dt_prop *child_prop(const struct dt_node *n, const char *child, const char *prop)
{
	for (n = n->child; n; n = n->peer) {
		const struct dt_prop *p = dt_get(n, "name");
		if (p && strcmp((const char *)p->value, child) == 0)
			return prop ? dt_get(n, prop) : p;
	}
	return NULL;
}

void test_parcels(void)
{
	struct chain c;
	memset(&c, 0, sizeof c);
	memcpy(c.buf, "prcl\x01\x00\x00\x00", 8);
	put_be32(c.buf + 8, 0x14);
	put_be32(c.buf + 12, 0x14);
	c.end = 0x800;

	/* a new node, with a library stored LZSS-compressed (all literals) */
	static const uint8_t lib[] = { 0xFF, 'J', 'o', 'y', '!', 'p', 'e', 'f', 'f', 0x03, 'p', 'w' };
	uint32_t b = parcel(&c, 0x14, 0x200, "node", 0x20000, "", "", 2);
	blob(&c, b, "cstr", 0, "name", "AAPL,CodePrepare", 17, false, 17);
	blob(&c, b + 60, "nlib", 0x20094, "TestLib", lib, sizeof lib, true, 10);

	/* properties for the node whose device_type is via-cuda and that's compatible with cuda */
	b = parcel(&c, 0x200, 0x300, "prop", 0xC, "cuda", "via-cuda", 3);
	blob(&c, b, "ndrv", 0, "pef,AAPL,MacOS,PowerPC,register", "abc", 3, false, 3);
	blob(&c, b + 60, "cstr", 0x20, "code,AAPL,MacOS,name", "new", 4, false, 4);
	blob(&c, b + 120, "cstr", 0x100, "debug", "x", 2, false, 2);

	/* ... for an rtc under via-cuda: one deleted */
	b = parcel(&c, 0x300, 0x400, "prop", 0xA, "via-cuda", "rtc", 1);
	blob(&c, b, "cstr", 0x40, "compatible", "", 0, false, 0);

	/* the Toolbox image (a short one here) */
	b = parcel(&c, 0x400, 0, "rom ", 0, "Mac OS ROM Parcel", "", 1);
	blob(&c, b, "rom ", 5, "", "ROM!", 4, false, 4);

	struct parcels p;
	char err[128];
	CHECK(parcels_detect(c.buf, sizeof c.buf));
	CHECK(parcels_parse(c.buf, sizeof c.buf, &p, err, sizeof err) == 0);
	CHECK_EQ(p.count, 4);
	const struct parcel_blob *rom = parcels_find(&p, "rom ", "rom ");
	CHECK(rom && rom->size == 4 && memcmp(rom->data, "ROM!", 4) == 0);
	CHECK(parcels_find(&p, "prop", "nlib") == NULL);

	struct dt_node *root = dt_add_node(NULL, "device-tree");
	struct dt_node *cuda = dt_add_node(root, "via-cuda");
	dt_prop_str(cuda, "device_type", "via-cuda");
	dt_prop_str(cuda, "compatible", "cuda");
	dt_prop_str(cuda, "code,AAPL,MacOS,name", "old");
	struct dt_node *rtc = dt_add_node(cuda, "rtc");
	dt_prop_str(rtc, "device_type", "rtc");
	dt_prop_str(rtc, "compatible", "rtc");
	struct dt_node *other = dt_add_node(root, "other");
	dt_prop_str(other, "device_type", "serial");
	dt_prop_str(other, "compatible", "cuda");

	parcels_apply(&p, root);

	const struct dt_prop *nlib = child_prop(root, "AAPL,CodePrepare", "TestLib");
	CHECK(nlib && nlib->len == 10 && memcmp(nlib->value, "Joy!peff", 8) == 0);
	const struct dt_prop *pef = dt_get(cuda, "pef,AAPL,MacOS,PowerPC,register");
	CHECK(pef && pef->len == 3);
	CHECK(strcmp((const char *)dt_get(cuda, "code,AAPL,MacOS,name")->value, "old") == 0);
	CHECK(dt_get(cuda, "debug") == NULL);
	CHECK(dt_get(rtc, "compatible") == NULL && dt_get(rtc, "device_type") != NULL);
	CHECK(dt_get(other, "pef,AAPL,MacOS,PowerPC,register") == NULL);

	/* a link out of the chain is an error, not a crash */
	put_be32(c.buf + 0x400, 0xFFFFF0);
	struct parcels bad;
	CHECK(parcels_parse(c.buf, sizeof c.buf, &bad, err, sizeof err) != 0);

	dt_free(root);
	parcels_free(&p);
}
