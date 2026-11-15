#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "../src/be.h"
#include "../src/disk.h"
#include "../src/hfs.h"
#include "test.h"

static int temp_file(char *path, size_t len)
{
	const char *dir = getenv("TMPDIR");
	snprintf(path, len, "%s/blue-test-XXXXXX", dir ? dir : "/tmp");
	return mkstemp(path);
}

static void test_format(void)
{
	char path[256];
	int fd = temp_file(path, sizeof path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	const uint64_t size = 20u << 20;
	CHECK(ftruncate(fd, (off_t)size) == 0);
	CHECK(hfs_format(fd, size, "", 0, NULL, 0) != 0);                /* names: 1-27 chars, no colons */
	CHECK(hfs_format(fd, size, "a:b", 0, NULL, 0) != 0);
	CHECK_EQ(hfs_format(fd, size, "Test Disk", 0x12345678u, NULL, 0), 0);

	uint8_t mdb[512], alt[512], node[512];
	CHECK(pread(fd, mdb, 512, 1024) == 512);
	CHECK(pread(fd, alt, 512, (off_t)(size - 1024)) == 512);
	CHECK(memcmp(mdb, alt, 512) == 0);
	CHECK_EQ(be16(mdb), 0x4244);
	CHECK_EQ(be32(mdb + 2), 0x12345678u);
	CHECK_EQ(be32(mdb + 20), 512);                          /* 20 MB: 512-byte allocation blocks */
	CHECK_EQ(mdb[36], 9);
	CHECK(memcmp(mdb + 37, "Test Disk", 9) == 0);
	CHECK_EQ(be32(mdb + 30), 16);                           /* next catalog node ID */

	/* catalog: header says one leaf (node 1) with two records */
	uint32_t al_bl_st = be16(mdb + 28), al_siz = be32(mdb + 20);
	off_t cat = (off_t)al_bl_st * 512 + (off_t)be16(mdb + 150) * al_siz;
	CHECK(pread(fd, node, 512, cat) == 512);
	CHECK_EQ(node[8], 1);
	CHECK_EQ(be32(node + 14 + 2), 1);                       /* root */
	CHECK_EQ(be32(node + 14 + 6), 2);                       /* leaf records */
	CHECK_EQ(be16(node + 14 + 20), 37);
	CHECK_EQ(node[248], 0xC0);                              /* nodes 0 and 1 in use */
	CHECK(pread(fd, node, 512, cat + 512) == 512);
	CHECK_EQ(node[8], 0xFF);
	CHECK_EQ(be16(node + 10), 2);
	const uint8_t *rec = node + be16(node + 510);
	CHECK_EQ(be32(rec + 2), 1);                             /* parent of the root folder */
	CHECK(memcmp(rec + 7, "Test Disk", 9) == 0);
	CHECK_EQ(rec[(7 + 9 + 1) & ~1], 1);                     /* folder record */
	CHECK_EQ(be32(rec + ((7 + 9 + 1) & ~1) + 6), 2);        /* directory ID 2 */

	/* bitmap: exactly the B-tree files' blocks are allocated */
	uint8_t bitmap[512];
	CHECK(pread(fd, bitmap, 512, 1536) == 512);
	unsigned used = 0;
	for (int k = 0; k < 512 * 8; k++)
		used += bitmap[k / 8] >> (7 - k % 8) & 1;
	CHECK_EQ(used, be16(mdb + 18) - be16(mdb + 34));
	close(fd);
	unlink(path);
}

static void put_entry(uint8_t *b, uint32_t count, uint32_t start, uint32_t blocks, const char *name, const char *type)
{
	memset(b, 0, 512);
	put_be16(b, 0x504D);
	put_be32(b + 4, count);
	put_be32(b + 8, start);
	put_be32(b + 12, blocks);
	strcpy((char *)b + 16, name);
	strcpy((char *)b + 48, type);
}

static void test_partition_map(void)
{
	char path[256];
	int fd = temp_file(path, sizeof path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(ftruncate(fd, 8u << 20) == 0);
	uint8_t b[512] = { 0 };
	put_be16(b, 0x4552);
	put_be16(b + 2, 512);
	CHECK(pwrite(fd, b, 512, 0) == 512);
	put_entry(b, 4, 1, 63, "Apple", "Apple_partition_map");
	CHECK(pwrite(fd, b, 512, 512) == 512);
	put_entry(b, 4, 64, 2048, "Macintosh", "Apple_HFS");
	CHECK(pwrite(fd, b, 512, 1024) == 512);
	put_entry(b, 4, 2112, 100, "Extra", "Apple_Free");
	CHECK(pwrite(fd, b, 512, 1536) == 512);
	put_entry(b, 4, 4096, 99999, "Second", "Apple_HFS");     /* runs past the end: clipped */
	CHECK(pwrite(fd, b, 512, 2048) == 512);
	close(fd);

	struct mem mem = { 0 };
	struct disk d;
	CHECK_EQ(disk_open(&d, &mem, path, true, false), 0);
	CHECK_EQ(d.partitions, 2);
	CHECK_EQ(d.part[0].offset, 64 * 512);
	CHECK_EQ(d.part[0].size, 2048 * 512);
	CHECK(strcmp(d.part[0].name, "Macintosh") == 0);
	CHECK_EQ(d.part[0].map_entry, 2);
	CHECK_EQ(d.part[1].offset, 4096 * 512);
	CHECK_EQ(d.part[1].size, (8u << 20) - 4096 * 512);
	CHECK_EQ(d.part[1].map_entry, 4);
	disk_close(&d);

	/* a CD's: the descriptor says 2048-byte blocks, but the entries are in 512-byte ones */
	fd = open(path, O_RDWR);
	memset(b, 0, sizeof b);
	put_be16(b, 0x4552);
	put_be16(b + 2, 2048);
	CHECK(pwrite(fd, b, 512, 0) == 512);
	close(fd);
	CHECK_EQ(disk_open(&d, &mem, path, true, false), 0);
	CHECK_EQ(d.partitions, 2);
	CHECK_EQ(d.part[0].offset, 64 * 512);
	CHECK_EQ(d.part[0].size, 2048 * 512);
	disk_close(&d);

	/* entries in 2048-byte blocks */
	fd = open(path, O_RDWR);
	memset(b, 0, sizeof b);
	for (unsigned k = 1; k < 8; k++)
		CHECK(pwrite(fd, b, 512, 512 * k) == 512);
	put_entry(b, 2, 1, 15, "Apple", "Apple_partition_map");
	CHECK(pwrite(fd, b, 512, 2048) == 512);
	put_entry(b, 2, 16, 512, "Macintosh", "Apple_HFS");
	CHECK(pwrite(fd, b, 512, 4096) == 512);
	close(fd);
	CHECK_EQ(disk_open(&d, &mem, path, true, false), 0);
	CHECK_EQ(d.partitions, 1);
	CHECK_EQ(d.part[0].offset, 16 * 2048);
	CHECK_EQ(d.part[0].size, 512 * 2048);
	disk_close(&d);

	/* without a map, the whole file is one drive */
	fd = open(path, O_RDWR);
	memset(b, 0, sizeof b);
	CHECK(pwrite(fd, b, 512, 0) == 512);
	close(fd);
	CHECK_EQ(disk_open(&d, &mem, path, true, false), 0);
	CHECK_EQ(d.partitions, 1);
	CHECK_EQ(d.part[0].offset, 0);
	CHECK_EQ(d.part[0].size, 8u << 20);
	CHECK_EQ(d.part[0].map_entry, 0);
	disk_close(&d);
	unlink(path);
}

/* A DriverGestalt status call (immediate) for a drive: its status, and the response's words in r */
static int32_t gestalt(struct disk *d, struct ndrv_call *c, uint32_t pb, uint16_t drive, uint32_t sel, uint32_t r[4])
{
	mem_w16(c->mem, pb + 22, drive);
	mem_w16(c->mem, pb + 26, 43);
	mem_w32(c->mem, pb + 28, sel);
	c->entry = 0;
	c->arg[2] = pb;
	c->arg[3] = NDRV_STATUS;
	c->arg[4] = NDRV_KIND_IMMEDIATE;
	disk_driver_call(d, c, 1000);
	for (unsigned k = 0; k < 4; k++)
		r[k] = mem_r32(c->mem, pb + 32 + 4 * k);
	CHECK(c->done);
	return (int32_t)c->result;
}

/*
 * For ROM files with parcels: which partitions have boot blocks, and the
 * DriverGestalt answers that tie the drives to the disk's node and the
 * device tree's bootpath (see disk.c).
 */
static void test_boot_gestalt(void)
{
	char path[256];
	int fd = temp_file(path, sizeof path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(ftruncate(fd, 4u << 20) == 0);
	uint8_t b[512] = { 0 };
	put_be16(b, 0x4552);
	put_be16(b + 2, 512);
	CHECK(pwrite(fd, b, 512, 0) == 512);
	put_entry(b, 3, 1, 63, "Apple", "Apple_partition_map");
	CHECK(pwrite(fd, b, 512, 512) == 512);
	put_entry(b, 3, 64, 2048, "First", "Apple_HFS");
	CHECK(pwrite(fd, b, 512, 1024) == 512);
	put_entry(b, 3, 4096, 2048, "Second", "Apple_HFS");
	CHECK(pwrite(fd, b, 512, 1536) == 512);
	CHECK(pwrite(fd, "LK", 2, 4096 * 512) == 2);
	close(fd);

	struct mem mem;
	struct disk d;
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		unlink(path);
		return;
	}
	mem_map(&mem, 0, 0x20000, MEM_RW);
	CHECK_EQ(disk_open(&d, &mem, path, true, false), 0);
	CHECK_EQ(d.partitions, 2);
	CHECK(!disk_bootable(&d, 0));
	CHECK(disk_bootable(&d, 1));
	CHECK(!disk_bootable(&d, 2));

	d.boot_gestalt = true;
	d.unit = 3;
	uint8_t ndrv[2048];
	CHECK(disk_build_ndrv(&d, ndrv, sizeof ndrv, 0) > 0);
	const uint32_t pb = 0x1F000;
	struct ndrv_call c = { .mem = &mem, .toc = 0x10000, .frame = 0x1E000 };
	mem_w16(&mem, pb, 0xFFD0);                              /* Initialize: DriverInitInfo */
	for (unsigned k = 0; k < 16; k++)
		mem_w8(&mem, pb + 2 + k, (uint8_t)(0xA0 + k));  /* (the node's RegEntryID) */
	c.arg[2] = pb;
	c.arg[3] = NDRV_INITIALIZE;
	c.arg[4] = NDRV_KIND_IMMEDIATE;
	disk_driver_call(&d, &c, 1000);
	CHECK_EQ(d.part[0].drive_num, 8);
	CHECK_EQ(d.part[1].drive_num, 9);

	uint32_t r[4];
	CHECK_EQ(gestalt(&d, &c, pb, 9, 0x6E6D7267u, r), 0);    /* 'nmrg': the RegEntryID's address */
	CHECK(mem_ok(&mem, r[0], 16, MEM_R) && mem_r8(&mem, r[0]) == 0xA0 && mem_r8(&mem, r[0] + 15) == 0xAF);
	CHECK_EQ(gestalt(&d, &c, pb, 9, 0x696E7466u, r), 0);    /* 'intf' */
	CHECK_EQ(r[0], 0x73637369u);                            /* 'scsi' */
	CHECK_EQ(gestalt(&d, &c, pb, 9, 0x64767266u, r), 0);    /* 'dvrf': target 3 */
	CHECK_EQ(r[0], 0x0300);
	CHECK_EQ(gestalt(&d, &c, pb, 9, 0x6F667074u, r), 0);    /* 'ofpt': map entry 3, from 0 */
	CHECK_EQ(r[0], 2);
	CHECK_EQ(r[1], 2);
	CHECK_EQ(r[2], 4096);
	CHECK_EQ(r[3], 2048);
	CHECK_EQ(gestalt(&d, &c, pb, 8, 0x6F667074u, r), 0);
	CHECK_EQ(r[1], 1);
	CHECK_EQ(r[2], 64);
	CHECK_EQ(gestalt(&d, &c, pb, 7, 0x6F667074u, r), -56);  /* (no such drive) */
	d.boot_gestalt = false;
	CHECK_EQ(gestalt(&d, &c, pb, 9, 0x6E6D7267u, r), -18);  /* not for earlier ROMs */
	disk_close(&d);
	mem_free(&mem);
	unlink(path);
}

void test_hfs(void)
{
	test_format();
	test_partition_map();
	test_boot_gestalt();
}
