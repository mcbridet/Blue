#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/be.h"
#include "../src/disk.h"
#include "../src/drives.h"
#include "test.h"

#define K 1024u
#define TOC 0x10000u
#define PB 0x1F000u
#define FRAME 0x1E000u
#define POST_EVENT 0x00ABCDE0u          /* (the imports' TVectors, as if the Code Fragment Manager had them) */
#define SET_TIMER 0x00ABCDF0u
#define IO_DONE 0x00ABCE00u
#define VCB 0x20000u

static void temp_path(char *path, size_t len)
{
	const char *dir = getenv("TMPDIR");
	snprintf(path, len, "%s/syshm-test-XXXXXX", dir ? dir : "/tmp");
	int fd = mkstemp(path);
	if (fd >= 0)
		close(fd);
}

/* An image of `size` bytes, an HFS volume in it */
static void make_image(char *path, size_t len, uint64_t size, const char *name)
{
	temp_path(path, len);
	unlink(path);
	CHECK_EQ(disk_create(path, size, name), 0);
}

/* An image with an Apple partition map and two HFS partitions of 1 MB, each a volume */
static void make_partitioned(char *path, size_t len)
{
	temp_path(path, len);
	int fd = open(path, O_RDWR);
	uint8_t b[512] = { 0 };
	CHECK(ftruncate(fd, 4u << 20) == 0);
	put_be16(b, 0x4552);
	put_be16(b + 2, 512);
	CHECK(pwrite(fd, b, 512, 0) == 512);
	for (unsigned k = 0; k < 2; k++) {
		memset(b, 0, sizeof b);
		put_be16(b, 0x504D);
		put_be32(b + 4, 2);
		put_be32(b + 8, 64 + 2048 * k);
		put_be32(b + 12, 2048);
		snprintf((char *)b + 16, 32, "Part %u", k + 1);
		strcpy((char *)b + 48, "Apple_HFS");
		CHECK(pwrite(fd, b, 512, 512 * (k + 1)) == 512);
	}
	close(fd);
}

static void load_driver(struct disk *d, struct mem *mem)
{
	static uint8_t ndrv[8192];
	size_t size = disk_build_ndrv(d, ndrv, sizeof ndrv, 0);
	CHECK(size > 0);
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

/* The timer's run; returns the drives whose disk-inserted events it posted, a bit each (from drive 8) */
static unsigned timer(struct disk *d, struct mem *mem, uint64_t tb)
{
	struct ndrv_call c = { .mem = mem, .toc = TOC, .frame = FRAME, .entry = 1, .tb = tb };
	unsigned posted = 0;
	disk_driver_call(d, &c, 1000);
	for (int n = 0; !c.done && n < 8; n++) {
		if (c.tvector == POST_EVENT) {
			CHECK_EQ(c.out[0], 7);
			posted |= 1u << (c.out[1] - 8);
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

static int32_t read_block(struct disk *d, struct mem *mem, uint16_t drive, uint32_t block)
{
	mem_w32(mem, PB + 32, 0x30000);
	mem_w32(mem, PB + 36, 512);
	mem_w16(mem, PB + 44, 1);
	mem_w32(mem, PB + 46, block * 512);
	return io(d, mem, NDRV_READ, 0, drive);
}

/* Mac OS's volume queue: a VCB for each drive given (0: none), as if they were mounted */
static void volumes(struct mem *mem, uint16_t a, uint16_t b)
{
	mem_w32(mem, 0x356 + 2, a || b ? VCB : 0);
	mem_w32(mem, VCB, b ? VCB + 0x100 : 0);
	mem_w16(mem, VCB + 72, a ? a : b);
	mem_w32(mem, VCB + 0x100, 0);
	mem_w16(mem, VCB + 0x100 + 72, b);
}

/* The drive's flags (the 4 bytes before its queue element) and size, for drive k of the disk */
static uint8_t in_place(struct mem *mem, const struct disk *d, unsigned k)
{
	return mem_r8(mem, TOC + d->layout.extra + 20 * k + 1);
}

static uint32_t blocks(struct mem *mem, const struct disk *d, unsigned k)
{
	uint32_t el = TOC + d->layout.extra + 20 * k + 4;
	return mem_r16(mem, el + 12) | (uint32_t)mem_r16(mem, el + 14) << 16;
}

/* A swappable CD-ROM drive: its disc in at startup, Mac OS's Eject, another put in */
static void test_cdrom(struct mem *mem)
{
	char one[256], two[256], parts[256], why[400];
	make_image(one, sizeof one, 4u << 20, "Disc One");
	make_image(two, sizeof two, 6u << 20, "Disc Two");
	make_partitioned(parts, sizeof parts);
	static struct disk d;

	CHECK_EQ(disk_open_cdrom(&d, mem, one), 0);
	CHECK(d.removable && d.cdrom && d.read_only && !d.floppy && d.media && d.shown);
	CHECK_EQ(d.partitions, 1);
	load_driver(&d, mem);
	mem_w16(mem, PB, 0xFFD0);
	CHECK_EQ(io(&d, mem, NDRV_INITIALIZE, 0, 0), 0);
	const uint16_t drive = d.part[0].drive_num;
	CHECK_EQ(drive, 8);
	/* in at startup: shown at once (the startup disk it may be), ejectable, write-protected */
	CHECK_EQ(in_place(mem, &d, 0), 1);
	CHECK_EQ(mem_r8(mem, TOC + d.layout.extra), 0x80);
	CHECK_EQ(blocks(mem, &d, 0), (4u << 20) / 512);
	CHECK_EQ(timer(&d, mem, 1000), 1);                      /* (applications or not) */
	CHECK_EQ(read_block(&d, mem, drive, 2), 0);              /* (mounted) */
	/* it answers as the disks do, but for keeping its disc at a restart */
	CHECK_EQ(gestalt(&d, mem, drive, "devt"), 0);
	CHECK_EQ(mem_r32(mem, PB + 32), be32("disk"));
	CHECK_EQ(gestalt(&d, mem, drive, "ejec"), 0);
	CHECK_EQ(mem_r32(mem, PB + 32), 1);
	CHECK_EQ(gestalt(&d, mem, drive, "mics"), -18);
	CHECK_EQ(io(&d, mem, NDRV_CONTROL, 22, drive), -17);
	CHECK_EQ(io(&d, mem, NDRV_CONTROL, 23, drive), -17);
	CHECK(disk_bootable(&d, 0) == false);

	/* Mac OS ejects it (its volume put away): the drive's empty, its timer goes on */
	volumes(mem, 0, 0);
	CHECK_EQ(io(&d, mem, NDRV_CONTROL, 7, drive), 0);
	CHECK(!d.media && d.fd < 0 && d.ejects == 1);
	CHECK_EQ(in_place(mem, &d, 0), 0);
	CHECK_EQ(blocks(mem, &d, 0), 0);
	CHECK_EQ(read_block(&d, mem, drive, 2), -65);
	CHECK_EQ(timer(&d, mem, 2000), 0);

	/* another goes in: shown once an application runs, then its event */
	memset(mem_ptr(mem, 0x910), 0xFF, 32);
	CHECK_EQ(disk_insert(&d, two, why, sizeof why), 0);
	CHECK(d.read_only);
	CHECK_EQ(timer(&d, mem, 3000), 0);
	memcpy(mem_ptr(mem, 0x910), "\6Finder", 7);
	CHECK_EQ(timer(&d, mem, 4000), 1);
	CHECK_EQ(in_place(mem, &d, 0), 1);
	CHECK_EQ(blocks(mem, &d, 0), (6u << 20) / 512);
	CHECK_EQ(read_block(&d, mem, drive, 2), 0);
	volumes(mem, drive, 0);                                 /* (mounted: Mac OS's to eject) */
	CHECK(disk_in_use(&d));
	volumes(mem, 0, 0);
	CHECK_EQ(io(&d, mem, NDRV_CONTROL, 7, drive), 0);
	CHECK(!d.media);
	disk_close(&d);

	/* a disc with two partitions: two drives; one ejected while the other's in use, the disc stays */
	CHECK_EQ(disk_open_cdrom(&d, mem, parts), 0);
	CHECK_EQ(d.partitions, 2);
	CHECK_EQ(d.media_parts, 2);
	load_driver(&d, mem);
	mem_w32(mem, 0x308 + 2, 0);                             /* (a new drive queue) */
	mem_w32(mem, 0x308 + 6, 0);
	CHECK_EQ(io(&d, mem, NDRV_INITIALIZE, 0, 0), 0);
	CHECK_EQ(d.part[0].drive_num, 8);
	CHECK_EQ(d.part[1].drive_num, 9);
	CHECK_EQ(in_place(mem, &d, 0), 1);
	CHECK_EQ(in_place(mem, &d, 1), 1);
	CHECK_EQ(blocks(mem, &d, 1), 2048);
	CHECK_EQ(timer(&d, mem, 5000), 3);
	volumes(mem, 9, 0);
	CHECK_EQ(io(&d, mem, NDRV_CONTROL, 7, 8), 0);
	CHECK(d.media);
	CHECK_EQ(in_place(mem, &d, 0), 0);
	CHECK_EQ(in_place(mem, &d, 1), 1);
	CHECK_EQ(read_block(&d, mem, 8, 2), -65);
	CHECK_EQ(read_block(&d, mem, 9, 2), 0);
	volumes(mem, 0, 0);
	CHECK_EQ(io(&d, mem, NDRV_CONTROL, 7, 9), 0);
	CHECK(!d.media);
	/* a disc of one partition in it: the second drive stays empty */
	CHECK_EQ(disk_insert(&d, one, why, sizeof why), 0);
	CHECK_EQ(d.media_parts, 1);
	CHECK_EQ(timer(&d, mem, 6000), 1);
	CHECK_EQ(in_place(mem, &d, 0), 1);
	CHECK_EQ(in_place(mem, &d, 1), 0);
	CHECK_EQ(read_block(&d, mem, 9, 2), -65);
	disk_close(&d);

	/* an empty drive at startup: one drive, nothing in it */
	CHECK_EQ(disk_open_cdrom(&d, mem, NULL), 0);
	CHECK(!d.media && d.partitions == 1);
	load_driver(&d, mem);
	mem_w32(mem, 0x308 + 2, 0);
	mem_w32(mem, 0x308 + 6, 0);
	CHECK_EQ(io(&d, mem, NDRV_INITIALIZE, 0, 0), 0);
	CHECK_EQ(in_place(mem, &d, 0), 0);
	CHECK_EQ(timer(&d, mem, 7000), 0);
	CHECK_EQ(disk_insert(&d, parts, why, sizeof why), 0);
	CHECK_EQ(d.media_parts, 1);                             /* (as many as it has drives) */
	CHECK_EQ(timer(&d, mem, 8000), 1);
	disk_close(&d);

	/* what CD-ROM drives don't take */
	char bad[256];
	temp_path(bad, sizeof bad);
	int fd = open(bad, O_RDWR);
	static const uint8_t sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
	CHECK(ftruncate(fd, 4u << 20) == 0 && pwrite(fd, sync, sizeof sync, 0) == (ssize_t)sizeof sync);
	CHECK_EQ(disk_check_cd(bad, why, sizeof why), -1);
	CHECK(strstr(why, "2352") != NULL);
	CHECK(pwrite(fd, "\x1F\x8B", 2, 0) == 2);
	CHECK_EQ(disk_check_cd(bad, why, sizeof why), -1);
	CHECK(strstr(why, "gzip") != NULL);
	CHECK(pwrite(fd, "\0\0", 2, 0) == 2 && pwrite(fd, "koly", 4, (4u << 20) - 512) == 4);
	CHECK_EQ(disk_check_cd(bad, why, sizeof why), -1);
	CHECK(strstr(why, "UDIF") != NULL && strstr(why, "hdiutil") != NULL);
	CHECK(ftruncate(fd, 1000) == 0);
	CHECK_EQ(disk_check_cd(bad, why, sizeof why), -1);
	close(fd);
	CHECK_EQ(disk_check_cd(one, why, sizeof why), 0);
	CHECK_EQ(disk_open_cdrom(&d, mem, bad), -1);
	unlink(bad);
	unlink(one);
	unlink(two);
	unlink(parts);
}

/* Where images go, and what the menu shows */
static void test_routing(struct mem *mem)
{
	char flop[256], flop2[256], cd[256], why[400];
	make_image(flop, sizeof flop, 1440 * K, "Floppy");
	make_image(flop2, sizeof flop2, 800 * K, "Floppy Two");
	make_image(cd, sizeof cd, 4u << 20, "Disc");
	static struct disk f, c1, c2;
	disk_open_floppy(&f, mem);
	CHECK_EQ(disk_open_cdrom(&c1, mem, NULL), 0);
	CHECK_EQ(disk_open_cdrom(&c2, mem, NULL), 0);
	f.part[0].drive_num = 8;
	c1.part[0].drive_num = 9;
	c2.part[0].drive_num = 10;
	struct drives dr = { .floppy = &f, .cd = { &c1, &c2 }, .cds = 2 };
	volumes(mem, 0, 0);

	/* a floppy's size to the floppy drive, anything else to the first empty CD-ROM drive */
	CHECK(drives_pick(&dr, flop, DRIVES_ANY, why, sizeof why) == &f);
	CHECK(drives_pick(&dr, cd, DRIVES_ANY, why, sizeof why) == &c1);
	CHECK(drives_pick(&dr, flop, DRIVES_CD, why, sizeof why) == &c1);     /* (as asked) */
	CHECK(drives_pick(&dr, cd, DRIVES_FLOPPY, why, sizeof why) == NULL);
	CHECK(strstr(why, "too big for a floppy") != NULL);
	CHECK_EQ(disk_insert(&c1, cd, why, sizeof why), 0);
	CHECK(drives_pick(&dr, cd, DRIVES_ANY, why, sizeof why) == &c2);      /* (an empty one first) */
	CHECK_EQ(disk_insert(&c2, cd, why, sizeof why), 0);
	CHECK(drives_pick(&dr, cd, DRIVES_ANY, why, sizeof why) == &c1);      /* (else one not in use) */
	volumes(mem, 9, 0);
	CHECK(drives_pick(&dr, cd, DRIVES_ANY, why, sizeof why) == &c2);
	volumes(mem, 9, 10);
	CHECK(drives_pick(&dr, cd, DRIVES_ANY, why, sizeof why) == NULL);
	CHECK(strstr(why, "The CD-ROM drives have discs Mac OS is using") != NULL);
	CHECK_EQ(disk_insert(&f, flop, why, sizeof why), 0);
	volumes(mem, 8, 0);
	CHECK(drives_pick(&dr, flop2, DRIVES_ANY, why, sizeof why) == NULL);
	CHECK(strstr(why, "The floppy drive has") != NULL);
	volumes(mem, 0, 0);
	CHECK(drives_pick(&dr, flop2, DRIVES_ANY, why, sizeof why) == &f);    /* (in, but not in use) */

	/* the menu: each drive, what's in it, whether Mac OS uses it; the floppies that follow */
	struct ui_drives ui;
	struct disk *order[UI_DRIVES];
	const char *queue[2] = { flop2, "/some/where/Disk 3.img" };
	volumes(mem, 10, 0);
	drives_menu(&dr, queue, 2, &ui, order);
	CHECK_EQ(ui.count, 3);
	CHECK(order[0] == &f && order[1] == &c1 && order[2] == &c2);
	CHECK(strcmp(ui.drive[0].name, "Floppy drive") == 0);
	CHECK(strcmp(ui.drive[1].name, "CD-ROM drive 1") == 0);
	CHECK(strcmp(ui.drive[2].name, "CD-ROM drive 2") == 0);
	CHECK(strcmp(ui.drive[1].in, strrchr(cd, '/') + 1) == 0);
	CHECK(!ui.drive[0].in_use && !ui.drive[1].in_use && ui.drive[2].in_use);
	CHECK(!ui.drive[1].locked);                             /* (a CD's always is: not said) */
	CHECK(strstr(ui.next, "Disk 3.img") != NULL && strstr(ui.next, ", ") != NULL);
	disk_eject(&c1);
	drives_menu(&dr, NULL, 0, &ui, order);
	CHECK(!ui.drive[1].in[0] && !ui.next[0]);

	/* one CD-ROM drive, no floppy drive */
	struct drives one = { .cd = { &c2 }, .cds = 1 };
	CHECK(drives_pick(&one, flop, DRIVES_ANY, why, sizeof why) == NULL);
	CHECK(strstr(why, "no floppy drive") != NULL);
	CHECK(drives_pick(&one, cd, DRIVES_ANY, why, sizeof why) == NULL);    /* (in use) */
	CHECK(strstr(why, "The CD-ROM drive has") != NULL);
	drives_menu(&one, NULL, 0, &ui, order);
	CHECK(ui.count == 1 && strcmp(ui.drive[0].name, "CD-ROM drive") == 0);
	struct drives none = { 0 };
	CHECK(drives_pick(&none, cd, DRIVES_ANY, why, sizeof why) == NULL);
	CHECK(strstr(why, "no CD-ROM drive") != NULL);
	disk_close(&f);
	disk_close(&c1);
	disk_close(&c2);
	unlink(flop);
	unlink(flop2);
	unlink(cd);
}

void test_drives(void)
{
	struct mem mem;
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		return;
	}
	mem_map(&mem, 0, 0x40000, MEM_RW);
	memset(mem_ptr(&mem, 0), 0, 0x40000);
	test_cdrom(&mem);
	test_routing(&mem);
	mem_free(&mem);
}
