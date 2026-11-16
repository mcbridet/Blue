#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/be.h"
#include "../src/disk.h"
#include "../src/floppy.h"
#include "../src/hfs.h"
#include "test.h"

#define K 1024u

static int temp_file(char *path, size_t len)
{
	const char *dir = getenv("TMPDIR");
	snprintf(path, len, "%s/blue-test-XXXXXX", dir ? dir : "/tmp");
	return mkstemp(path);
}

/* Disk Copy 4.2's checksum, as its documentation gives it */
static uint32_t reference_sum(const uint8_t *p, size_t len)
{
	uint32_t sum = 0;
	for (size_t k = 0; k < len; k += 2) {
		sum += (uint32_t)(p[k] << 8 | p[k + 1]);
		sum = (sum >> 1) | ((sum & 1) << 31);
	}
	return sum;
}

/* A probe of `size` bytes holding `head` at the start (and `at1024` at 1024); 0 or -1, the reason in why */
static int probe_bytes(uint64_t size, const void *head, size_t head_len, const void *at1024, size_t len1024,
		       struct floppy_image *img, char *why, size_t why_len)
{
	char path[256];
	int fd = temp_file(path, sizeof path);
	if (fd < 0)
		return -2;
	int rc = ftruncate(fd, (off_t)size);
	if (head_len)
		rc |= pwrite(fd, head, head_len, 0) != (ssize_t)head_len;
	if (len1024)
		rc |= pwrite(fd, at1024, len1024, 1024) != (ssize_t)len1024;
	if (!rc)
		rc = floppy_probe(fd, img, why, why_len);
	close(fd);
	unlink(path);
	return rc;
}

static void test_raw(void)
{
	struct floppy_image img;
	char why[400];
	const uint64_t sizes[] = { 400 * K, 720 * K, 800 * K, 1440 * K, 2880 * K };
	for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
		CHECK_EQ(probe_bytes(sizes[k], NULL, 0, NULL, 0, &img, why, sizeof why), 0);
		CHECK_EQ(img.kind, FLOPPY_RAW);
		CHECK_EQ(img.data_offset, 0);
		CHECK_EQ(img.data_size, sizes[k]);
		CHECK(strcmp(img.volume, "blank") == 0);
	}
	CHECK_EQ(probe_bytes(1000 * K, NULL, 0, NULL, 0, &img, why, sizeof why), -1);
	CHECK(strstr(why, "1024000 bytes") != NULL);
	CHECK_EQ(probe_bytes(2 << 20, NULL, 0, NULL, 0, &img, why, sizeof why), -1);

	/* what's on it */
	uint8_t mdb[64] = { 0x42, 0x44 };
	mdb[36] = 5;
	memcpy(mdb + 37, "Tools", 5);
	CHECK_EQ(probe_bytes(1440 * K, NULL, 0, mdb, sizeof mdb, &img, why, sizeof why), 0);
	CHECK(strcmp(img.volume, "HFS \"Tools\"") == 0);
	const uint8_t mfs[2] = { 0xD2, 0xD7 };
	CHECK_EQ(probe_bytes(400 * K, NULL, 0, mfs, 2, &img, why, sizeof why), -1);
	CHECK(strstr(why, "MFS") != NULL);
	uint8_t fat[64] = { 0xEB, 0x3C, 0x90 };
	fat[12] = 2;                            /* 512 bytes a sector */
	fat[38] = 0x29;
	memcpy(fat + 43, "PCDISK     ", 11);
	CHECK_EQ(probe_bytes(1440 * K, fat, sizeof fat, NULL, 0, &img, why, sizeof why), 0);
	CHECK(strcmp(img.volume, "FAT \"PCDISK\"") == 0);
	const uint8_t mixed[4] = { 0x12, 0x34, 0x56, 0x78 };
	CHECK_EQ(probe_bytes(800 * K, mixed, 4, NULL, 0, &img, why, sizeof why), 0);
	CHECK(strcmp(img.volume, "not HFS or FAT") == 0);

	/* compressed and other images: refused, saying what they are */
	const uint8_t gz[4] = { 0x1F, 0x8B, 8, 0 }, dart[4] = { 0x01, 0x01, 0x03, 0x20 };
	CHECK_EQ(probe_bytes(1440 * K, gz, 4, NULL, 0, &img, why, sizeof why), -1);
	CHECK(strstr(why, "gzip") != NULL && strstr(why, "Disk Copy 4.2") != NULL);
	CHECK_EQ(probe_bytes(500 * K, dart, 4, NULL, 0, &img, why, sizeof why), -1);
	CHECK(strstr(why, "DART") != NULL);
	CHECK_EQ(probe_bytes(1440 * K, "StuffIt (c)1997", 15, NULL, 0, &img, why, sizeof why), -1);
	CHECK(strstr(why, "StuffIt") != NULL);
	CHECK_EQ(probe_bytes(1440 * K, "PK\3\4", 4, NULL, 0, &img, why, sizeof why), -1);

	char path[256];
	int fd = temp_file(path, sizeof path);
	CHECK(fd >= 0 && ftruncate(fd, 1440 * K + 512) == 0 && pwrite(fd, "koly", 4, 1440 * K) == 4);
	CHECK_EQ(floppy_probe(fd, &img, why, sizeof why), -1);
	CHECK(strstr(why, "UDIF") != NULL);
	close(fd);
	unlink(path);
	CHECK(floppy_raw_size(1440 * K) && !floppy_raw_size(1440 * K + 512));
}

/* A Disk Copy 4.2 image of an 800K disk at `at` in the file (with tags, data and tags in a pattern) */
static int make_dc42(const char *path, uint64_t at, uint8_t *data, uint8_t *tags, bool good_sums)
{
	const uint32_t size = 800 * K, tag_size = 1600 * 12;
	for (uint32_t k = 0; k < size; k++)
		data[k] = (uint8_t)(k * 7 + k / 512);
	for (uint32_t k = 0; k < tag_size; k++)
		tags[k] = (uint8_t)(k * 13 + 1);
	uint8_t h[84] = { 0 };
	h[0] = 9;
	memcpy(h + 1, "Test Disk", 9);
	put_be32(h + 0x40, size);
	put_be32(h + 0x44, tag_size);
	put_be32(h + 0x48, reference_sum(data, size) ^ !good_sums);
	put_be32(h + 0x4C, reference_sum(tags + 12, tag_size - 12));
	h[0x50] = 1;
	h[0x51] = 0x22;
	put_be16(h + 0x52, 0x0100);
	int fd = open(path, O_RDWR | O_TRUNC);
	if (fd < 0)
		return -1;
	int rc = pwrite(fd, h, 84, (off_t)at) != 84 || pwrite(fd, data, size, (off_t)at + 84) != (ssize_t)size ||
		 pwrite(fd, tags, tag_size, (off_t)(at + 84 + size)) != (ssize_t)tag_size;
	close(fd);
	return rc ? -1 : 0;
}

static void test_dc42(void)
{
	static uint8_t data[800 * K], tags[1600 * 12], back[800 * K];
	struct floppy_image img;
	char path[256], why[400];
	int fd = temp_file(path, sizeof path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	close(fd);

	CHECK_EQ(make_dc42(path, 0, data, tags, true), 0);
	fd = open(path, O_RDWR);
	CHECK_EQ(floppy_probe(fd, &img, why, sizeof why), 0);
	CHECK_EQ(img.kind, FLOPPY_DC42);
	CHECK_EQ(img.header_offset, 0);
	CHECK_EQ(img.data_offset, 84);
	CHECK_EQ(img.data_size, 800 * K);
	CHECK_EQ(img.tag_offset, 84 + 800 * K);
	CHECK_EQ(img.tag_size, 1600 * 12);
	CHECK(img.checksums_ok);
	CHECK_EQ(floppy_dc42_sum(0, data, 800 * K), reference_sum(data, 800 * K));

	/* a write, then the checksums worked out again */
	uint8_t block[512];
	memset(block, 0xA5, sizeof block);
	CHECK(pwrite(fd, block, 512, 84 + 512 * 33) == 512);
	memcpy(data + 512 * 33, block, 512);
	CHECK_EQ(floppy_dc42_update(fd, &img), 0);
	uint8_t sums[8];
	CHECK(pread(fd, sums, 8, 0x48) == 8);
	CHECK_EQ(be32(sums), reference_sum(data, 800 * K));
	CHECK_EQ(be32(sums + 4), reference_sum(tags + 12, 1600 * 12 - 12));
	CHECK_EQ(floppy_probe(fd, &img, why, sizeof why), 0);
	CHECK(img.checksums_ok);

	/* erased: zeros, tags too, and their checksums */
	CHECK_EQ(floppy_erase(fd, &img), 0);
	CHECK(pread(fd, back, 800 * K, 84) == 800 * K);
	CHECK_EQ(reference_sum(back, 800 * K), 0);
	CHECK(pread(fd, sums, 8, 0x48) == 8);
	CHECK_EQ(be32(sums), 0);
	CHECK_EQ(be32(sums + 4), 0);
	close(fd);

	/* bad checksums: taken, but said */
	CHECK_EQ(make_dc42(path, 0, data, tags, false), 0);
	fd = open(path, O_RDONLY);
	CHECK_EQ(floppy_probe(fd, &img, why, sizeof why), 0);
	CHECK(!img.checksums_ok);
	close(fd);

	/* inside MacBinary */
	CHECK_EQ(make_dc42(path, 128, data, tags, true), 0);
	fd = open(path, O_RDWR);
	uint8_t mb[128] = { 0 };
	mb[1] = 4;
	memcpy(mb + 2, "Test", 4);
	memcpy(mb + 65, "dImgdCpy", 8);
	put_be32(mb + 83, 84 + 800 * K + 1600 * 12);
	CHECK(pwrite(fd, mb, 128, 0) == 128);
	CHECK_EQ(floppy_probe(fd, &img, why, sizeof why), 0);
	CHECK_EQ(img.kind, FLOPPY_DC42);
	CHECK_EQ(img.header_offset, 128);
	CHECK_EQ(img.data_offset, 128 + 84);
	CHECK(img.checksums_ok);
	memcpy(mb + 65, "TEXTttxt", 8);                         /* (something else in MacBinary) */
	CHECK(pwrite(fd, mb, 128, 0) == 128 && pwrite(fd, "\0\0", 2, 128 + 0x52) == 2);
	CHECK_EQ(floppy_probe(fd, &img, why, sizeof why), -1);
	CHECK(strstr(why, "'TEXT'") != NULL);
	close(fd);
	unlink(path);
}

static void test_icons(void)
{
	static uint8_t media[FLOPPY_ICN_SIZE], drive[FLOPPY_ICN_SIZE], media_icns[FLOPPY_ICNS_SIZE],
		drive_icns[FLOPPY_ICNS_SIZE];
	floppy_icons(media, drive, media_icns, drive_icns);
	CHECK(memcmp(media_icns, "icns", 4) == 0);
	CHECK_EQ(be32(media_icns + 4), FLOPPY_ICNS_SIZE);
	CHECK(memcmp(media_icns + 8, "ICN#", 4) == 0);
	CHECK_EQ(be32(media_icns + 12), 8 + 256);
	CHECK(memcmp(media_icns + 16, media, 256) == 0);
	/* the elements add up to the whole */
	uint32_t at = 8, n = 0;
	while (at + 8 <= FLOPPY_ICNS_SIZE && n < 8) {
		at += be32(media_icns + at + 4);
		n++;
	}
	CHECK_EQ(at, FLOPPY_ICNS_SIZE);
	CHECK_EQ(n, 4);
	/* the disk's mask is its outline, filled; its 1-bit icon is inside the mask */
	CHECK_EQ(be32(media + 128 + 4 * 2), 0x3FFFFFE0u);      /* row 2 (the corner's cut) */
	CHECK_EQ(be32(media + 128 + 4 * 15), 0x3FFFFFFCu);
	CHECK_EQ(be32(media + 128), 0);
	for (unsigned k = 0; k < 128; k++)
		CHECK_EQ(media[k] & ~media[128 + k], 0);
	CHECK(memcmp(media, drive, 256) != 0);
}

/* ---- the driver ---- */

#define TOC 0x10000u
#define PB 0x1F000u
#define FRAME 0x1E000u
#define LIST 0x1F800u
#define POST_EVENT 0x00ABCDE0u          /* (the imports' TVectors, as if the Code Fragment Manager had them) */
#define SET_TIMER 0x00ABCDF0u
#define IO_DONE 0x00ABCE00u

static void load_driver(struct disk *d, struct mem *mem)
{
	static uint8_t ndrv[8192];
	size_t size = disk_build_ndrv(d, ndrv, sizeof ndrv, 0);
	CHECK(size > 0);
	/* the data section as the Code Fragment Manager would place it (at TOC), and the imports */
	uint32_t data_off = be32(ndrv + 40 + 28 + 20), data_len = be32(ndrv + 40 + 28 + 16);
	mem_load(mem, TOC, ndrv + data_off, data_len);
	mem_w32(mem, TOC + d->layout.import[0], IO_DONE);
	mem_w32(mem, TOC + d->layout.import[1], POST_EVENT);
	mem_w32(mem, TOC + d->layout.import[2], SET_TIMER);
}

/* A DoDriverIO call (immediate; through its callouts, which all return 0): its status */
static int32_t io(struct disk *d, struct mem *mem, uint32_t code, uint16_t cs_code, uint16_t drive)
{
	struct ndrv_call c = { .mem = mem, .toc = TOC, .frame = FRAME };
	mem_w16(mem, PB + 22, drive);
	mem_w16(mem, PB + 26, cs_code);
	c.arg[2] = PB;
	c.arg[3] = code;
	c.arg[4] = NDRV_KIND_IMMEDIATE;
	disk_driver_call(d, &c, 1000);
	for (int n = 0; !c.done && n < 4; n++) {
		c.entry = NDRV_ENTRY_RESUME;
		c.arg[0] = 0;
		disk_driver_call(d, &c, 1000);
	}
	CHECK(c.done);
	return (int32_t)c.result;
}

static int32_t gestalt(struct disk *d, struct mem *mem, uint16_t drive, const char *sel)
{
	mem_w32(mem, PB + 28, be32(sel));
	return io(d, mem, NDRV_STATUS, 43, drive);
}

/* The timer's run; returns the drive whose disk-inserted event it posted, or 0 */
static unsigned timer(struct disk *d, struct mem *mem, uint64_t tb)
{
	struct ndrv_call c = { .mem = mem, .toc = TOC, .frame = FRAME, .entry = 1, .tb = tb };
	unsigned posted = 0;
	disk_driver_call(d, &c, 1000);
	for (int n = 0; !c.done && n < 4; n++) {
		if (c.tvector == POST_EVENT) {
			CHECK_EQ(c.out[0], 7);  /* diskEvt */
			posted = c.out[1];
		} else {
			CHECK_EQ(c.tvector, SET_TIMER);
		}
		c.entry = NDRV_ENTRY_RESUME;
		c.arg[0] = 0;
		disk_driver_call(d, &c, 1000);
	}
	CHECK(c.done);
	return posted;
}

static int32_t read_block(struct disk *d, struct mem *mem, uint16_t drive, uint32_t block, uint32_t buf, bool write)
{
	mem_w32(mem, PB + 32, buf);
	mem_w32(mem, PB + 36, 512);
	mem_w16(mem, PB + 44, 1);
	mem_w32(mem, PB + 46, block * 512);
	return io(d, mem, write ? NDRV_WRITE : NDRV_READ, 0, drive);
}

static void test_driver(void)
{
	static uint8_t data[800 * K], tags[1600 * 12];
	char path[256], why[400];
	int fd = temp_file(path, sizeof path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	close(fd);
	CHECK_EQ(make_dc42(path, 0, data, tags, true), 0);

	struct mem mem;
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		unlink(path);
		return;
	}
	mem_map(&mem, 0, 0x40000, MEM_RW);
	memset(mem_ptr(&mem, 0), 0, 0x40000);
	static struct disk d;
	disk_open_floppy(&d, &mem);
	load_driver(&d, &mem);

	/* Initialize: the drive is queued, empty */
	mem_w16(&mem, PB, 0xFFD0);
	CHECK_EQ(io(&d, &mem, NDRV_INITIALIZE, 0, 0), 0);
	const uint16_t drive = d.part[0].drive_num;
	CHECK_EQ(drive, 8);
	uint32_t el = mem_r32(&mem, 0x308 + 2);                 /* the drive queue's head */
	CHECK_EQ(mem_r16(&mem, el + 6), drive);
	CHECK_EQ(mem_r8(&mem, el - 3), 0);                      /* no disk in place */
	CHECK_EQ(mem_r8(&mem, el - 2), 1);                      /* installed */
	CHECK_EQ(mem_r16(&mem, el + 12), 0);
	CHECK_EQ(io(&d, &mem, NDRV_STATUS, 8, drive), 0);       /* DriveStatus: the same */
	CHECK_EQ(mem_r8(&mem, PB + 28 + 3), 0);
	CHECK_EQ(mem_r16(&mem, PB + 28 + 12), drive);

	/* answers as a floppy drive */
	CHECK_EQ(gestalt(&d, &mem, drive, "devt"), 0);
	CHECK_EQ(mem_r32(&mem, PB + 32), be32("flop"));
	CHECK_EQ(gestalt(&d, &mem, drive, "ejec"), -18);        /* (Mac OS ejects its disk at a restart) */
	CHECK_EQ(gestalt(&d, &mem, drive, "minf"), 0);
	CHECK_EQ(mem_r16(&mem, PB + 40), 0xFFFF);               /* no media */
	CHECK_EQ(gestalt(&d, &mem, drive, "vmop"), 0);
	CHECK_EQ(mem_r32(&mem, PB + 32), 0);
	CHECK_EQ(gestalt(&d, &mem, drive, "nmrg"), -18);
	static uint8_t media[FLOPPY_ICN_SIZE], drive_icn[FLOPPY_ICN_SIZE], media_icns[FLOPPY_ICNS_SIZE],
		drive_icns[FLOPPY_ICNS_SIZE];
	floppy_icons(media, drive_icn, media_icns, drive_icns);
	CHECK_EQ(gestalt(&d, &mem, drive, "mics"), 0);
	CHECK(memcmp(mem_ptr(&mem, mem_r32(&mem, PB + 32)), media_icns, FLOPPY_ICNS_SIZE) == 0);
	CHECK_EQ(gestalt(&d, &mem, drive, "dics"), 0);
	CHECK(memcmp(mem_ptr(&mem, mem_r32(&mem, PB + 32)), drive_icns, FLOPPY_ICNS_SIZE) == 0);
	CHECK_EQ(io(&d, &mem, NDRV_CONTROL, 22, drive), 0);    /* media icon */
	CHECK(memcmp(mem_ptr(&mem, mem_r32(&mem, PB + 28)), media, FLOPPY_ICN_SIZE) == 0);
	CHECK_EQ(io(&d, &mem, NDRV_CONTROL, 21, drive), 0);    /* drive icon, then where it is */
	uint32_t icon = mem_r32(&mem, PB + 28);
	CHECK(memcmp(mem_ptr(&mem, icon), drive_icn, FLOPPY_ICN_SIZE) == 0);
	CHECK(mem_r8(&mem, icon + 256) > 0 && memcmp(mem_ptr(&mem, icon + 257), "Blue floppy drive", 17) == 0);
	CHECK_EQ(io(&d, &mem, NDRV_CONTROL, 23, drive), 0);    /* drive info: a SuperDrive */
	CHECK_EQ(mem_r32(&mem, PB + 28), 4);
	CHECK_EQ(read_block(&d, &mem, drive, 0, 0x30000, false), -65);         /* (off line) */

	/* a disk goes in: Mac OS hears of it once applications run */
	CHECK_EQ(disk_insert(&d, path, why, sizeof why), 0);
	CHECK(d.media && !d.shown);
	CHECK_EQ(timer(&d, &mem, 1000), 0);
	memset(mem_ptr(&mem, 0x910), 0xFF, 32);                 /* (what startup leaves there) */
	CHECK_EQ(timer(&d, &mem, 2000), 0);
	CHECK_EQ(mem_r8(&mem, el - 3), 0);
	memcpy(mem_ptr(&mem, 0x910), "\6Finder", 7);
	CHECK_EQ(timer(&d, &mem, 3000), drive);
	CHECK_EQ(mem_r8(&mem, el - 3), 1);                      /* in place */
	CHECK_EQ(mem_r8(&mem, el - 4), 0);                      /* not locked */
	CHECK_EQ(mem_r16(&mem, el + 12), 1600);
	CHECK_EQ(timer(&d, &mem, 4000), 0);                     /* (posted again only after 10 s) */

	/* read: the disk's blocks, after the DC42 header */
	CHECK_EQ(read_block(&d, &mem, drive, 2, 0x30000, false), 0);
	CHECK(memcmp(mem_ptr(&mem, 0x30000), data + 1024, 512) == 0);
	CHECK_EQ(timer(&d, &mem, 9000), 0);                     /* (mounted: no more events) */
	CHECK_EQ(timer(&d, &mem, 20000), 0);

	/* the format list: its own */
	mem_w16(&mem, PB + 28, 4);
	mem_w32(&mem, PB + 30, LIST);
	CHECK_EQ(io(&d, &mem, NDRV_STATUS, 6, drive), 0);
	CHECK_EQ(mem_r16(&mem, PB + 28), 1);
	CHECK_EQ(mem_r32(&mem, LIST), 1600);
	CHECK_EQ(mem_r8(&mem, LIST + 4), 0xC2);                 /* given, current, two sides */
	CHECK_EQ(mem_r16(&mem, LIST + 6), 80);
	CHECK_EQ(gestalt(&d, &mem, drive, "minf"), 0);
	CHECK_EQ(mem_r32(&mem, PB + 32), 1600);
	CHECK_EQ(mem_r16(&mem, PB + 40), 136);

	/* a write: flushed with the checksums a second after (or when asked, or ejected) */
	memset(mem_ptr(&mem, 0x30000), 0x5A, 512);
	CHECK_EQ(read_block(&d, &mem, drive, 40, 0x30000, true), 0);
	memset(data + 40 * 512, 0x5A, 512);
	CHECK(d.dirty);
	CHECK_EQ(gestalt(&d, &mem, drive, "flus"), 0);
	CHECK_EQ(mem_r8(&mem, PB + 32), 1);
	CHECK_EQ(mem_r8(&mem, PB + 33), 1);
	CHECK_EQ(timer(&d, &mem, 21000), 0);
	CHECK(!d.dirty);
	uint8_t sums[8];
	fd = open(path, O_RDONLY);
	CHECK(pread(fd, sums, 8, 0x48) == 8);
	CHECK_EQ(be32(sums), reference_sum(data, 800 * K));
	close(fd);

	/* the host can't take it out while Mac OS has it mounted */
	put_be32(mem_ptr(&mem, 0x356 + 2), 0x20000);           /* a volume: its VCB */
	mem_w32(&mem, 0x20000, 0);
	mem_w16(&mem, 0x20000 + 72, drive);
	CHECK(disk_in_use(&d));
	mem_w16(&mem, 0x20000 + 72, 0);                         /* (off line) */
	CHECK(!disk_in_use(&d));

	/* Mac OS ejects it: the drive's empty */
	CHECK_EQ(io(&d, &mem, NDRV_CONTROL, 7, drive), 0);
	CHECK(!d.media && !d.shown && d.fd < 0);
	CHECK_EQ(d.ejects, 1);
	CHECK_EQ(mem_r8(&mem, el - 3), 0);
	CHECK_EQ(mem_r16(&mem, el + 12), 0);
	CHECK_EQ(read_block(&d, &mem, drive, 2, 0x30000, false), -65);
	CHECK_EQ(io(&d, &mem, NDRV_STATUS, 6, drive), -65);

	/* a locked image: write-protected (root can write to anything, though) */
	bool root = geteuid() == 0;
	CHECK_EQ(chmod(path, 0444), 0);
	CHECK_EQ(disk_insert(&d, path, why, sizeof why), 0);
	CHECK(d.read_only || root);
	CHECK_EQ(timer(&d, &mem, 30000), drive);
	if (!root) {
		CHECK_EQ(mem_r8(&mem, el - 4), 0x80);
		CHECK_EQ(read_block(&d, &mem, drive, 40, 0x30000, true), -44);
		CHECK_EQ(io(&d, &mem, NDRV_CONTROL, 6, drive), -44);    /* (Format) */
	}

	/* another image, put in by the host while Mac OS wasn't using the first: out, then in */
	char path2[256];
	fd = temp_file(path2, sizeof path2);
	CHECK(fd >= 0 && ftruncate(fd, 1440 * K) == 0);
	close(fd);
	CHECK_EQ(disk_insert(&d, path2, why, sizeof why), 0);
	CHECK_EQ(read_block(&d, &mem, drive, 2, 0x30000, false), -65);
	CHECK_EQ(timer(&d, &mem, 31000), 0);
	CHECK_EQ(mem_r8(&mem, el - 3), 0);
	CHECK_EQ(timer(&d, &mem, 32000), drive);
	CHECK_EQ(mem_r8(&mem, el - 3), 1);
	CHECK_EQ(mem_r8(&mem, el - 4), 0);
	CHECK_EQ(mem_r16(&mem, el + 12), 2880);

	/* Format erases it */
	fd = open(path2, O_RDWR);
	CHECK(pwrite(fd, "data", 4, 4096) == 4);
	close(fd);
	CHECK_EQ(io(&d, &mem, NDRV_CONTROL, 6, drive), 0);
	char back[4] = "xxx";
	fd = open(path2, O_RDONLY);
	CHECK(pread(fd, back, 4, 4096) == 4 && !memcmp(back, "\0\0\0\0", 4));
	close(fd);

	/* not a floppy image: refused, the drive left as it was */
	CHECK_EQ(disk_insert(&d, "/nonexistent/blue-test", why, sizeof why), -1);
	CHECK(d.media && strcmp(d.media_path, path2) == 0);
	disk_close(&d);
	CHECK(!d.media);
	mem_free(&mem);
	chmod(path, 0644);
	unlink(path);
	unlink(path2);
}

/* `disk create` makes floppies too: an HFS volume that fills the disk */
static void test_blank(void)
{
	char path[256];
	int fd = temp_file(path, sizeof path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	close(fd);
	unlink(path);
	CHECK_EQ(disk_create(path, 1440 * K, "Untitled"), 0);
	struct floppy_image img;
	char why[400];
	fd = open(path, O_RDONLY);
	CHECK_EQ(floppy_probe(fd, &img, why, sizeof why), 0);
	CHECK(strcmp(img.volume, "HFS \"Untitled\"") == 0);
	close(fd);
	unlink(path);
	CHECK_EQ(disk_create(path, 400 * K, "Small"), 0);
	fd = open(path, O_RDONLY);
	CHECK_EQ(floppy_probe(fd, &img, why, sizeof why), 0);
	CHECK(strcmp(img.volume, "HFS \"Small\"") == 0);
	close(fd);
	unlink(path);
}

void test_floppy(void)
{
	test_raw();
	test_dc42();
	test_icons();
	test_driver();
	test_blank();
}
