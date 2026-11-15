#include <string.h>

#include "../src/adb.h"
#include "../src/be.h"
#include "../src/devtree.h"
#include "../src/mem.h"
#include "../src/ndrv.h"
#include "test.h"

static void test_hash(void)
{
	/* export keys found in the ROM's own NDRVs */
	CHECK_EQ(pef_hash_word("TheDriverDescription", 20), 0x0014BDE0);
	CHECK_EQ(pef_hash_word("DoDriverIO", 10), 0x000AD1FD);
	CHECK_EQ(pef_hash_word("ThePluginDispatchTable", 22), 0x00163D51);
}

static void test_container(void)
{
	static const struct ndrv_import imports[] = {
		{ "DriverServicesLib", "IOCommandIsComplete" },
		{ "DriverServicesLib", "UpTime" },
		{ "VideoServicesLib", "VSLDoInterruptService" },
	};
	static const uint8_t extra[5] = { 1, 2, 3, 4, 5 };
	struct ndrv_spec spec = {
		.device_name = "display", .driver_name = ".Test", .runtime_options = 6,
		.category = 0x6E647276u, .type = 0x76696465u, .version = 0x01008000u, .hostcall = 7,
		.imports = imports, .import_count = 3, .extra = extra, .extra_len = sizeof extra,
	};
	static uint8_t buf[4096];
	struct ndrv_layout l;
	size_t n = ndrv_build(&spec, buf, sizeof buf, &l);
	CHECK(n > 0);
	CHECK(ndrv_build(&spec, buf, 100, &l) == 0);
	n = ndrv_build(&spec, buf, sizeof buf, &l);
	CHECK(memcmp(buf, "Joy!peffpwpc", 12) == 0);
	CHECK_EQ(be16(buf + 32), 3);

	/* data section: imports, then 8-byte aligned entry TVectors */
	CHECK_EQ(l.import[2], 8);
	CHECK_EQ(l.entry[0], 16);
	CHECK_EQ(l.entry[1], 24);
	const uint8_t *code = buf + be32(buf + 40 + 20);
	const uint8_t *data = buf + be32(buf + 40 + 28 + 20);
	CHECK_EQ(be32(code + 0x20), 0x18000007u);               /* the host call */
	CHECK_EQ(be32(data + l.entry[1]), 8);                   /* entry 1 code offset */
	CHECK(memcmp(data + l.entry[1] + 8, "mtej", 4) == 0);   /* description follows */
	CHECK(memcmp(data + l.extra, extra, sizeof extra) == 0);

	/* loader: two libraries, three imports, relocations ImportRun(3), +4, TVector8 x2 */
	const uint8_t *ld = buf + be32(buf + 40 + 56 + 20);
	CHECK_EQ(be32(ld + 24), 2);
	CHECK_EQ(be32(ld + 28), 3);
	const uint8_t *strings = ld + be32(ld + 40);
	CHECK(strcmp((const char *)strings + be32(ld + 56 + 24), "VideoServicesLib") == 0);
	CHECK_EQ(be32(ld + 56 + 24 + 16), 2);                   /* its first import */
	const uint8_t *relocs = ld + be32(ld + 36);
	CHECK_EQ(be16(relocs), 0x4A02);
	CHECK_EQ(be16(relocs + 2), 0x8003);
	CHECK_EQ(be16(relocs + 4), 0x4601);
}

static void test_flatten(void)
{
	struct dt_node *root = dt_add_node(NULL, "device-tree");
	struct dt_node *a = dt_add_node(root, "a");
	struct dt_node *b = dt_add_node(root, "b");
	dt_add_node(a, "a1");
	dt_prop_u32(b, "reg", 0x1234);
	dt_prop_u32(b, "reg", 0x5678);          /* replaces */

	static uint8_t out[0x8000];
	size_t n = dt_flatten(root, out, sizeof out);
	CHECK(n > DT_MAX_ENTRIES * DT_ENTRY_SIZE);

	/* preorder: root 0, a 1, a1 2, b 3 */
	CHECK_EQ(be32(out + 4), 12);            /* root's child: entry 1 */
	CHECK_EQ(be32(out + 12 + 4), 12);       /* a's child: entry 2 */
	CHECK_EQ(be32(out + 12), 24);           /* a's peer: entry 3 */
	CHECK_EQ(be32(out + 24), 0);            /* a1 has no peer */
	CHECK_EQ(be32(out + 0), 0);             /* the root has none either */

	const uint8_t *p = out + 36 + be32(out + 36 + 8);       /* b's properties */
	CHECK(strcmp((const char *)p + 4, "name") == 0);
	CHECK(memcmp(p + 40, "b", 2) == 0);
	p += be32(p);
	CHECK(strcmp((const char *)p + 4, "reg") == 0);
	CHECK_EQ(be32(p + 36), 4);
	CHECK_EQ(be32(p + 40), 0x5678);
	CHECK_EQ(be32(p), 0);                   /* last */

	CHECK(dt_flatten(root, out, 1000) == 0);
	dt_free(root);
}

static void test_adb_bus(void)
{
	struct adb_bus b;
	uint8_t r[8];
	adb_init(&b);

	CHECK_EQ(adb_command(&b, 0x2F, NULL, 0, r), 2);         /* Talk R3, keyboard */
	CHECK_EQ(r[0] & 0x0F, 2);
	CHECK_EQ(r[1], 2);
	CHECK_EQ(adb_command(&b, 0x5F, NULL, 0, r), -1);        /* nobody at 5 */
	CHECK_EQ(adb_command(&b, 0x2C, NULL, 0, r), -1);        /* no keys yet */

	static const uint8_t move[2] = { 0x09, 0xFE };          /* move the keyboard to 9 */
	CHECK_EQ(adb_command(&b, 0x2B, move, 2, r), 0);
	CHECK_EQ(adb_command(&b, 0x2F, NULL, 0, r), -1);
	CHECK_EQ(adb_command(&b, 0x9F, NULL, 0, r), 2);
	static const uint8_t handler[2] = { 0x09, 0x03 };
	adb_command(&b, 0x9B, handler, 2, r);
	adb_command(&b, 0x9F, NULL, 0, r);
	CHECK_EQ(r[1], 3);

	adb_key(&b, 0x24, true);
	adb_key(&b, 0x24, false);
	adb_key(&b, 0x31, true);
	CHECK_EQ(adb_pending(&b, 1u << 9), 9);
	CHECK_EQ(adb_command(&b, 0x9C, NULL, 0, r), 2);
	CHECK_EQ(r[0], 0x24);
	CHECK_EQ(r[1], 0xA4);
	CHECK_EQ(adb_command(&b, 0x9C, NULL, 0, r), 2);
	CHECK_EQ(r[0], 0x31);
	CHECK_EQ(r[1], 0xFF);

	adb_command(&b, 0x00, NULL, 0, r);                      /* bus reset: back to 2 */
	CHECK_EQ(b.kbd.addr, 2);

	/* the power key: 7F 7F pressed, FF FF released, never split across reports */
	adb_key(&b, 0x31, true);
	adb_power_key(&b, true);
	adb_power_key(&b, false);
	CHECK_EQ(adb_command(&b, 0x2C, NULL, 0, r), 2);
	CHECK_EQ(r[0], 0x31);
	CHECK_EQ(r[1], 0xFF);
	CHECK_EQ(adb_command(&b, 0x2C, NULL, 0, r), 2);
	CHECK_EQ(r[0], 0x7F);
	CHECK_EQ(r[1], 0x7F);
	CHECK_EQ(adb_command(&b, 0x2C, NULL, 0, r), 2);
	CHECK_EQ(r[0], 0xFF);
	CHECK_EQ(r[1], 0xFF);
	CHECK_EQ(adb_command(&b, 0x2C, NULL, 0, r), -1);

	/* mouse: a quick click is two reports; motion is clamped per report */
	adb_mouse_button(&b, true);
	adb_mouse_button(&b, false);
	adb_mouse_move(&b, 100, -3);
	CHECK_EQ(adb_command(&b, 0x3C, NULL, 0, r), 2);
	CHECK_EQ(r[0], 0x7D);                                   /* down, dy -3 */
	CHECK_EQ(r[1], 0x80 | 63);
	CHECK_EQ(adb_command(&b, 0x3C, NULL, 0, r), 2);
	CHECK_EQ(r[0], 0x80);                                   /* up */
	CHECK_EQ(r[1], 0x80 | 37);
	CHECK_EQ(adb_command(&b, 0x3C, NULL, 0, r), -1);
}

/*
 * A host call's arguments where the calling convention puts them: words in
 * r3 to r10 and then the caller's parameter area (past the stub's frame),
 * floats and doubles in f1 to f13, each skipping one or two words
 */
static void test_args(void)
{
	static struct mem m;
	CHECK_EQ(mem_init(&m), 0);
	mem_map(&m, 0, 0x10000, MEM_RW);
	const uint32_t frame = 0x8000, params = frame + NDRV_FRAME_SIZE + 24;
	struct ndrv_call c = { .mem = &m, .frame = frame };

	/* glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels) */
	for (unsigned k = 0; k < 8; k++)
		c.arg[k] = 100 + k;
	mem_w32(&m, params + 4 * 8, 0x4000);
	struct ndrv_args a = ndrv_args(&c);
	for (unsigned k = 0; k < 8; k++)
		CHECK_EQ(ndrv_next_word(&a), 100 + k);
	CHECK_EQ(ndrv_next_word(&a), 0x4000);

	/* glBitmap(width, height, xorig, yorig, xmove, ymove, bitmap): the floats in f1 to f4, bitmap in r9 */
	for (unsigned k = 0; k < 4; k++) {
		double d = 0.5 + k;
		memcpy(&c.f[k], &d, 8);
	}
	a = ndrv_args(&c);
	CHECK_EQ(ndrv_next_word(&a), 100);
	CHECK_EQ(ndrv_next_word(&a), 101);
	for (unsigned k = 0; k < 4; k++) {
		float f = 0.5f + (float)k;
		uint32_t b;
		memcpy(&b, &f, 4);
		CHECK_EQ(ndrv_next_float(&a), b);
	}
	CHECK_EQ(ndrv_next_word(&a), 106);

	/* doubles take two words each; past f13, they're in the parameter area */
	a = ndrv_args(&c);
	for (unsigned k = 0; k < 13; k++)
		ndrv_next_double(&a);
	CHECK_EQ(a.word, 26);
	mem_w32(&m, params + 4 * 26, 0x3FF00000u);
	mem_w32(&m, params + 4 * 27, 1);
	CHECK_EQ(ndrv_next_double(&a), 0x3FF0000000000001ull);
	CHECK_EQ(ndrv_arg(&c, 3), 103);
	mem_free(&m);
}

void test_drivers(void)
{
	test_hash();
	test_container();
	test_args();
	test_flatten();
	test_adb_bus();
}
