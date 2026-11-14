#include <stdbool.h>
#include <string.h>

#include "../src/be.h"
#include "../src/bootinfo.h"
#include "../src/devtree.h"
#include "../src/mem.h"
#include "../src/nvram.h"
#include "test.h"

/* A property of a node in the flattened tree: its value and length, or NULL. */
static const uint8_t *flat_prop(const uint8_t *entry, const char *name, uint32_t *len)
{
	if (!be32(entry + 8))
		return NULL;
	for (const uint8_t *p = entry + be32(entry + 8);; p += be32(p)) {
		if (strncmp((const char *)p + 4, name, DT_PROP_NAME_SIZE) == 0) {
			*len = be32(p + 36);
			return p + 40;
		}
		if (!be32(p))
			return NULL;
	}
}

static bool named(const uint8_t *entry, const char *name)
{
	uint32_t len;
	const uint8_t *v = flat_prop(entry, "name", &len);
	return v && len == strlen(name) + 1 && memcmp(v, name, len) == 0;
}

/* The node with this name (whose parent, `up` at this level, has that name unless it's NULL), or NULL. */
static const uint8_t *flat_find(const uint8_t *entry, const uint8_t *up, const char *name, const char *parent)
{
	for (; entry; entry = be32(entry) ? entry + be32(entry) : NULL) {
		if (named(entry, name) && (!parent || (up && named(up, parent))))
			return entry;
		const uint8_t *found = be32(entry + 4) ? flat_find(entry + be32(entry + 4), entry, name, parent) : NULL;
		if (found)
			return found;
	}
	return NULL;
}

static uint32_t flat_u32(const uint8_t *entry, const char *name, unsigned cell)
{
	uint32_t len;
	const uint8_t *v = flat_prop(entry, name, &len);
	return v && len >= 4 * (cell + 1) ? be32(v + 4 * cell) : 0xDEADBEEFu;
}

void test_nvram(void)
{
	/* byte k at +16k, whatever the low four address bits */
	uint8_t bytes[8192];
	struct nvram n;
	for (unsigned k = 0; k < sizeof bytes; k++)
		bytes[k] = (uint8_t)(k * 7);
	nvram_init(&n, bytes, sizeof bytes);
	CHECK_EQ(nvram_mmio_ops.read(&n, 0x1400 * NVRAM_STRIDE, 1), (uint8_t)(0x1400 * 7));
	CHECK_EQ(nvram_mmio_ops.read(&n, 0x1401 * NVRAM_STRIDE + 5, 1), (uint8_t)(0x1401 * 7));
	nvram_mmio_ops.write(&n, 0x1FFF * NVRAM_STRIDE, 1, 0xA5);
	nvram_mmio_ops.write(&n, 3 * NVRAM_STRIDE + 15, 1, 0x5A);
	CHECK_EQ(bytes[0x1FFF], 0xA5);
	CHECK_EQ(bytes[3], 0x5A);
	CHECK_EQ(bytes[4], (uint8_t)(4 * 7));
	CHECK_EQ(nvram_mmio_ops.read(&n, MACIO_NVRAM_SIZE + 3 * NVRAM_STRIDE, 1), 0x5A);       /* (wraps) */
	/* wider accesses: each byte from its own address */
	nvram_mmio_ops.write(&n, 0x10 * NVRAM_STRIDE + 14, 4, 0x11223344u);
	CHECK_EQ(bytes[0x10], 0x22);
	CHECK_EQ(bytes[0x11], 0x44);
	CHECK_EQ(nvram_mmio_ops.read(&n, 0x10 * NVRAM_STRIDE + 15, 2), 0x2244);
	CHECK_EQ(n.reads, 4);
	CHECK_EQ(n.writes, 3);

	/* the device tree has the iMac's nvram node for ROM files with parcels only (and /chosen) */
	struct mem mem;
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		return;
	}
	mem_map(&mem, BOOTINFO_BASE, BOOTINFO_SIZE, MEM_RW);
	struct parcels none = { 0 };
	struct bootinfo_machine hw = { .ram_size = 32u << 20, .timebase_hz = 16600000, .disks = 2,
				       .boot = true, .boot_disk = 1, .boot_map_entry = 3 };
	struct bootinfo bi;
	CHECK(bootinfo_build(&mem, &bi, &hw) == 0);
	const uint8_t *tree = mem_ptr(&mem, bi.device_tree);
	CHECK(flat_find(tree, NULL, "via-cuda", "mac-io") != NULL);
	CHECK(flat_find(tree, NULL, "nvram", NULL) == NULL);
	CHECK(flat_find(tree, NULL, "chosen", NULL) == NULL);

	hw.parcels = &none;
	CHECK(bootinfo_build(&mem, &bi, &hw) == 0);
	tree = mem_ptr(&mem, bi.device_tree);
	const uint8_t *node = flat_find(tree, NULL, "nvram", "mac-io");
	CHECK(node != NULL);
	if (node) {
		uint32_t len;
		const uint8_t *type = flat_prop(node, "device_type", &len);
		CHECK(type && len == 6 && memcmp(type, "nvram", 6) == 0);
		CHECK_EQ(flat_u32(node, "reg", 0), MACIO_NVRAM - MACIO_BASE);
		CHECK_EQ(flat_u32(node, "reg", 1), MACIO_NVRAM_SIZE);
		CHECK_EQ(flat_u32(node, "#bytes", 0), sizeof bytes);
		CHECK_EQ(flat_u32(node, "AAPL,address", 0), MACIO_NVRAM);
	}
	/* where Open Firmware booted from: the second disk (PCI device 3), map entry 3 */
	node = flat_find(tree, NULL, "chosen", NULL);
	CHECK(node != NULL);
	if (node) {
		uint32_t len;
		const uint8_t *path = flat_prop(node, "bootpath", &len);
		static const char want[] = "/pci/syshm,disk@3:3,\\\\:tbxi";
		CHECK(path && len == sizeof want && memcmp(path, want, len) == 0);
	}
	CHECK(flat_find(tree, NULL, "syshm,disk", "pci") != NULL);

	/*
	 * NanoKernel 2's HWInfo (0x0108): CascadeInfo at +0xA8, no cascaded
	 * interrupt controllers; and a parcel's lanLib for bmac+ (Mac OS
	 * 9.2.2's) leaves the firmware's in the Ethernet node
	 */
	uint8_t hwinfo[0xC0];
	memset(hwinfo, 0xAA, sizeof hwinfo);
	bootinfo_hwinfo(&bi, hwinfo);
	CHECK_EQ(be32(hwinfo + 0x70), 0x486E666Fu);
	CHECK_EQ(be32(hwinfo + 0xA0), 0);
	CHECK_EQ(be32(hwinfo + 0xA8), bi.cascade_info);
	CHECK(bi.cascade_info != 0 && be32(mem_ptr(&mem, bi.cascade_info)) == 0);
	static uint8_t ours[] = { 1, 2, 3, 4 }, theirs[] = { 9, 9 };
	struct parcel_blob lanlib = { .type = "shlb", .flags = 6, .name = "lanLib,AAPL,MacOS,PowerPC",
				      .data = theirs, .size = sizeof theirs };
	struct parcel bmac = { .type = "prop", .flags = PARCEL_MATCH_TYPE | PARCEL_MATCH_COMPATIBLE,
			       .a = "bmac+", .b = "network", .blobs = &lanlib, .count = 1 };
	struct parcels some = { &bmac, 1 };
	hw.parcels = &some;
	hw.lanlib = ours;
	hw.lanlib_size = sizeof ours;
	CHECK(bootinfo_build(&mem, &bi, &hw) == 0);
	CHECK_EQ(bi.parcel_props, 1);
	node = flat_find(mem_ptr(&mem, bi.device_tree), NULL, "ethernet", "mac-io");
	CHECK(node != NULL);
	if (node) {
		uint32_t len;
		const uint8_t *lib = flat_prop(node, "lanLib,AAPL,MacOS,PowerPC", &len);
		CHECK(lib && len == sizeof ours && memcmp(lib, ours, len) == 0);
	}

	/* earlier ROMs: HWInfo is 0xA0 bytes */
	hw.parcels = NULL;
	CHECK(bootinfo_build(&mem, &bi, &hw) == 0);
	CHECK_EQ(bi.cascade_info, 0);
	memset(hwinfo, 0xAA, sizeof hwinfo);
	bootinfo_hwinfo(&bi, hwinfo);
	CHECK_EQ(hwinfo[0x9F], 0);
	CHECK_EQ(hwinfo[0xA0], 0xAA);
	mem_free(&mem);
}
