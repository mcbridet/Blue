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
	snprintf(path, len, "%s/syshm-test-XXXXXX", dir ? dir : "/tmp");
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
	CHECK_EQ(d.part[1].offset, 4096 * 512);
	CHECK_EQ(d.part[1].size, (8u << 20) - 4096 * 512);
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
	disk_close(&d);
	unlink(path);
}

void test_hfs(void)
{
	test_format();
	test_partition_map();
}
