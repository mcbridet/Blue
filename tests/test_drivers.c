#include <string.h>

#include "../src/adb.h"
#include "../src/be.h"
#include "../src/devtree.h"
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

void test_drivers(void)
{
	test_hash();
	test_container();
	test_flatten();
	test_adb_bus();
}
