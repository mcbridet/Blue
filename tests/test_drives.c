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
	snprintf(path, len, "%s/blue-test-XXXXXX", dir ? dir : "/tmp");
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
	/* it answers as a CD-ROM drive, with its icons, keeping its disc at a restart */
	CHECK_EQ(gestalt(&d, mem, drive, "devt"), 0);
	CHECK_EQ(mem_r32(mem, PB + 32), be32("cdrm"));
	CHECK_EQ(gestalt(&d, mem, drive, "ejec"), 0);
	CHECK_EQ(mem_r32(mem, PB + 32), 1);
	CHECK_EQ(gestalt(&d, mem, drive, "mics"), 0);
	CHECK_EQ(mem_r32(mem, mem_r32(mem, PB + 32)), be32("icns"));
	static uint8_t icn[2][256], icns[2][FLOPPY_ICNS_SIZE];
	cdrom_icons(icn[0], icn[1], icns[0], icns[1]);
	CHECK_EQ(io(&d, mem, NDRV_CONTROL, 22, drive), 0);
	CHECK(!memcmp(mem_ptr(mem, mem_r32(mem, PB + 28)), icn[0], 256));
	CHECK_EQ(io(&d, mem, NDRV_CONTROL, 21, drive), 0);
	CHECK(!memcmp(mem_ptr(mem, mem_r32(mem, PB + 28)), icn[1], 256));
	uint32_t where = mem_r32(mem, PB + 28) + 256;
	CHECK(!memcmp(mem_ptr(mem, where), "\022syshm CD-ROM drive", 19));
	CHECK_EQ(io(&d, mem, NDRV_CONTROL, 23, drive), 0);
	CHECK_EQ(mem_r32(mem, PB + 28), 0xB01);
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

/* ---- a CD-ROM drive's calls (cdrom.c), with a cue sheet's disc ---------------- */

#define BUF 0x30000u

static int32_t cd_io(struct disk *d, struct mem *mem, uint32_t code, uint16_t cs_code, uint16_t drive, uint64_t tb,
		     const uint16_t *param, unsigned n)
{
	struct ndrv_call c = { .mem = mem, .toc = TOC, .frame = FRAME, .tb = tb };
	memset(mem_ptr(mem, PB + 28), 0, 22);
	for (unsigned k = 0; k < n; k++)
		mem_w16(mem, PB + 28 + 2 * k, param[k]);
	mem_w16(mem, PB + 22, drive);
	mem_w16(mem, PB + 26, cs_code);
	c.arg[2] = PB;
	c.arg[3] = code;
	c.arg[4] = NDRV_KIND_IMMEDIATE;
	disk_driver_call(d, &c, 1000);
	for (int k = 0; !c.done && k < 4; k++) {
		c.entry = NDRV_ENTRY_RESUME;
		c.arg[0] = 0;
		disk_driver_call(d, &c, 1000);
	}
	CHECK(c.done);
	return (int32_t)c.result;
}

#define CONTROL(code, tb, ...) cd_io(&d, mem, NDRV_CONTROL, code, 8, tb, (const uint16_t[]){ __VA_ARGS__ }, \
				     sizeof (uint16_t[]){ __VA_ARGS__ } / 2)
#define STATUS(code, tb, ...) cd_io(&d, mem, NDRV_STATUS, code, 8, tb, (const uint16_t[]){ __VA_ARGS__ }, \
				    sizeof (uint16_t[]){ __VA_ARGS__ } / 2)

static uint16_t param(struct mem *mem, unsigned k)
{
	return mem_r16(mem, PB + 28 + 2 * k);
}

/* Track n's samples: left counts the frames of its file, right is n * 1000 */
static void put_track(FILE *f, unsigned n, unsigned sectors, unsigned *frame)
{
	for (unsigned k = 0; k < sectors * 588; k++, (*frame)++) {
		uint16_t s[2] = { (uint16_t)(*frame & 0x7FFF), (uint16_t)(n * 1000) };
		uint8_t b[4] = { (uint8_t)s[0], (uint8_t)(s[0] >> 8), (uint8_t)s[1], (uint8_t)(s[1] >> 8) };
		fwrite(b, 1, 4, f);
	}
}

/*
 * A fixed CD-ROM drive with a cue sheet's disc: an HFS data track (1024
 * sectors) and two audio tracks of 20 sectors, the first after a 2 s pregap
 * not in its file: LBA 0, 1174, 1194, the lead-out at 1214.
 */
static void test_cd_calls(struct mem *mem)
{
	char data[256], audio[256], cue[300], text[1024], why[400];
	make_image(data, sizeof data, 2u << 20, "CD Data");
	temp_path(audio, sizeof audio);
	FILE *f = fopen(audio, "wb");
	unsigned frame = 0;
	put_track(f, 2, 20, &frame);
	put_track(f, 3, 20, &frame);
	fclose(f);
	temp_path(cue, sizeof cue - 4);
	unlink(cue);
	strcat(cue, ".cue");
	snprintf(text, sizeof text, "FILE \"%s\" BINARY\n TRACK 01 MODE1/2048\n  INDEX 01 00:00:00\n"
		 "FILE \"%s\" BINARY\n TRACK 02 AUDIO\n  PREGAP 00:02:00\n  INDEX 01 00:00:00\n"
		 " TRACK 03 AUDIO\n  INDEX 01 00:00:20\n", data, audio);
	f = fopen(cue, "w");
	fputs(text, f);
	fclose(f);

	static struct disk d;
	CHECK_EQ(disk_open(&d, mem, cue, true, true), 0);
	CHECK(d.cdrom && d.read_only && !d.removable && d.disc && d.backend);
	CHECK_EQ(d.partitions, 1);
	CHECK_EQ(d.part[0].size, 2u << 20);
	CHECK(disk_check_cd(cue, why, sizeof why) == 0);
	load_driver(&d, mem);
	mem_w32(mem, 0x308 + 2, 0);
	mem_w32(mem, 0x308 + 6, 0);
	mem_w16(mem, PB, 0xFFCF);
	CHECK_EQ(io(&d, mem, NDRV_INITIALIZE, 0, 0), 0);
	CHECK_EQ(d.part[0].drive_num, 8);
	CHECK(d.cd != NULL);

	/* the data track: the volume, read through the disc */
	CHECK_EQ(read_block(&d, mem, 8, 2), 0);
	CHECK_EQ(mem_r16(mem, 0x30000), 0x4244);
	/* a CD-ROM drive, its disc in place (DriveStatus 1, as Apple's driver), of so many blocks */
	CHECK_EQ(gestalt(&d, mem, 8, "devt"), 0);
	CHECK_EQ(mem_r32(mem, PB + 32), be32("cdrm"));
	CHECK_EQ(gestalt(&d, mem, 8, "minf"), 0);
	CHECK_EQ(mem_r32(mem, PB + 32), (2u << 20) / 512);
	CHECK_EQ(mem_r16(mem, PB + 40), 129);
	CHECK_EQ(io(&d, mem, NDRV_STATUS, 8, 8), 0);
	CHECK_EQ(mem_r8(mem, PB + 31), 1);
	CHECK_EQ(in_place(mem, &d, 0), 8);                      /* (the drive queue's flags: not ejectable) */
	CHECK_EQ(io(&d, mem, NDRV_CONTROL, 7, 8), -17);
	CHECK_EQ(STATUS(120, 0, 0), 0);                         /* ReturnDeviceIdent */
	CHECK_EQ(STATUS(96, 0, 0), 0);                          /* GetDriveType: a multi-speed drive */
	CHECK_EQ(param(mem, 0), 3);

	/* ReadTOC: first and last tracks, the lead-out, track starts, the whole table, sessions */
	CHECK_EQ(CONTROL(100, 0, 1), 0);
	CHECK_EQ(param(mem, 0), 0x0103);
	CHECK_EQ(CONTROL(100, 0, 2), 0);                        /* 1214 + 150 = 00:18:14 */
	CHECK_EQ(param(mem, 0), 0x0018);
	CHECK_EQ(param(mem, 1), 0x1400);
	CHECK_EQ(CONTROL(100, 0, 3, BUF >> 16, BUF & 0xFFFF, 8, 0x0200), 0);   /* (two entries' room, from 2) */
	CHECK_EQ(mem_r32(mem, BUF), 0x00001749u);               /* audio, 1174 + 150: 00:17:49 */
	CHECK_EQ(mem_r32(mem, BUF + 4), 0x00001769u);           /* 1194 + 150: 00:17:69 */
	CHECK_EQ(CONTROL(100, 0, 4, BUF >> 16, BUF & 0xFFFF), 0);
	CHECK_EQ(mem_r8(mem, BUF + 2), 0xA0);
	CHECK_EQ(mem_r8(mem, BUF + 3), 0x01);
	CHECK_EQ(mem_r8(mem, BUF + 8), 0x03);                   /* A1: the last track */
	CHECK_EQ(mem_r8(mem, BUF + 16), 0x04);                  /* track 1: data */
	CHECK_EQ(mem_r8(mem, BUF + 22), 0x02);
	CHECK_EQ(CONTROL(100, 0, 5), 0);
	CHECK_EQ(param(mem, 1), 1);
	CHECK_EQ(CONTROL(100, 0, 9), -50);

	/* AudioPlay: where to stop (the end of track 3), then from track 2 */
	CHECK_EQ(CONTROL(104, 0, 2, 0, 0x03, 1, 9), 0);
	CHECK_EQ(CONTROL(104, 1000, 2, 0, 0x02, 0, 9), 0);
	CHECK_EQ(CONTROL(107, 1200, 0), 0);                     /* AudioStatus: playing, stereo, 0.2 s in */
	CHECK_EQ(param(mem, 0), 0x0009);
	CHECK_EQ(param(mem, 1), 0x0000);                        /* audio; 1174 + 15 + 150 = 00:17:64 */
	CHECK_EQ(param(mem, 2), 0x1764);
	CHECK_EQ(CONTROL(101, 1200, 0), 0);                     /* ReadTheQSubcode: track 2, index 1, 00:00:15 in */
	CHECK_EQ(param(mem, 0), 0x0002);
	CHECK_EQ(param(mem, 1), 0x0100);
	CHECK_EQ(param(mem, 2), 0x0015);
	CHECK_EQ(CONTROL(105, 1300, 0, 1), 0);                  /* paused, the position kept */
	CHECK_EQ(CONTROL(107, 5000, 0), 0);
	CHECK_EQ(param(mem, 0) >> 8, 1);
	CHECK_EQ(param(mem, 2), 0x1771);                        /* (22.5 sectors in: 22) */
	CHECK_EQ(CONTROL(105, 5000, 0, 0), 0);
	CHECK_EQ(CONTROL(107, 6000, 0), 0);                     /* (to the end of track 3 by then) */
	CHECK_EQ(param(mem, 0), 0x0309);                        /* done */
	CHECK_EQ(CONTROL(106, 6000, 0, 0, 0), 0);               /* AudioStop at once */
	CHECK_EQ(CONTROL(107, 6000, 0), 0);
	CHECK_EQ(param(mem, 0) >> 8, 5);
	CHECK_EQ(CONTROL(104, 6000, 0, 0, 10, 0, 9), -50);      /* (not on the data track) */
	CHECK_EQ(CONTROL(106, 6000, 2, 0, 0x00), 0);            /* (track 0, as AppleCD Audio Player gives: none) */
	CHECK_EQ(CONTROL(103, 6000, 1, 0, 0x1849, 0, 9), -50);  /* (past the lead-out) */
	CHECK_EQ(CONTROL(103, 6000, 1, 0, 0x1760, 0, 9), 0);    /* AudioTrackSearch: holds at 00:17:60 */
	CHECK_EQ(CONTROL(107, 7000, 0), 0);
	CHECK_EQ(param(mem, 0) >> 8, 1);
	CHECK_EQ(param(mem, 2), 0x1760);
	CHECK_EQ(CONTROL(108, 7000, 2, 0, 0x02, 0), 0);         /* AudioScan forward from track 2 */
	CHECK_EQ(CONTROL(107, 7100, 0), 0);
	CHECK_EQ(param(mem, 0) >> 8, 0);
	CHECK_EQ(CONTROL(106, 7100, 0, 0, 0), 0);

	/* the drive's volume and play mode */
	CHECK_EQ(CONTROL(109, 7100, 0x80FF), 0);
	CHECK_EQ(CONTROL(112, 7100, 0), 0);
	CHECK_EQ(param(mem, 0), 0x80FF);
	CHECK_EQ(d.cd->audio.volume[0], 0x80);

	/* ReadAudio: samples (little-endian), with Q, the subchannel alone; not of the data track */
	CHECK_EQ(CONTROL(115, 7100, 0, BUF >> 16, BUF & 0xFFFF, 0, 0, 1194, 2), 0);
	CHECK_EQ(mem_r8(mem, BUF), (20 * 588) & 0xFF);          /* track 3's first frame: the file's 11760th */
	CHECK_EQ(mem_r8(mem, BUF + 1), (20 * 588) >> 8);
	CHECK_EQ(mem_r16(mem, BUF + 2), 0xB80B);                /* 3000, little-endian */
	CHECK_EQ(mem_r8(mem, BUF + 2352 + 4), (20 * 588 + 589) & 0xFF);
	CHECK_EQ(CONTROL(115, 7100, 1, BUF >> 16, BUF & 0xFFFF, 2, 0, 0x03, 1), 0);
	CHECK_EQ(mem_r8(mem, BUF + 2352), 0x01);                /* Q: control 0, ADR 1, track 3, index 1 */
	CHECK_EQ(mem_r8(mem, BUF + 2353), 0x03);
	CHECK_EQ(mem_r8(mem, BUF + 2354), 0x01);
	CHECK_EQ(CONTROL(115, 7100, 3, BUF >> 16, BUF & 0xFFFF, 0, 0, 1180, 1), 0);
	CHECK_EQ(mem_r8(mem, BUF) & 0xC0, 0);                   /* P off, Q's first bit 0 */
	CHECK_EQ(CONTROL(115, 7100, 0, BUF >> 16, BUF & 0xFFFF, 0, 0, 5, 1), -50);

	/* the play list: track 3 then 2, played by the driver, entry by entry */
	mem_w8(mem, BUF + 0x100, 0x03);
	mem_w8(mem, BUF + 0x101, 0x02);
	CHECK_EQ(CONTROL(122, 8000, 2, BUF >> 16, (BUF + 0x100) & 0xFFFF), 0);
	CHECK_EQ(CONTROL(125, 8000, 0x0102), 0);                /* repeat, programmed */
	CHECK_EQ(CONTROL(126, 8000, 0), 0);
	CHECK_EQ(param(mem, 0), 0x0102);
	CHECK_EQ(CONTROL(123, 8000, 0, BUF >> 16, (BUF + 0x200) & 0xFFFF), 0);
	CHECK_EQ(param(mem, 0), 2);
	CHECK_EQ(mem_r16(mem, BUF + 0x200), 0x0302);
	CHECK_EQ(CONTROL(124, 8000, 0), 0);
	CHECK_EQ(param(mem, 0), 0xFFFF);                        /* (none playing) */
	CHECK_EQ(CONTROL(104, 8000, 3, 0, 0x00, 0, 9), 0);      /* from entry 0 */
	CHECK_EQ(CONTROL(124, 8100, 0), 0);
	CHECK_EQ(param(mem, 0), 0);
	CHECK_EQ(CONTROL(101, 8100, 0), 0);
	CHECK_EQ(param(mem, 0), 0x0003);
	CHECK_EQ(CONTROL(124, 8300, 0), 0);                     /* (track 3 lasts 267 ms) */
	CHECK_EQ(param(mem, 0), 1);
	CHECK_EQ(CONTROL(101, 8300, 0), 0);
	CHECK_EQ(param(mem, 0), 0x0002);
	CHECK_EQ(CONTROL(124, 8600, 0), 0);                     /* (and over again) */
	CHECK_EQ(param(mem, 0), 0);
	CHECK_EQ(CONTROL(106, 8600, 3, 0, 0x01), 0);            /* AudioStop after entry 1: once more through */
	CHECK_EQ(CONTROL(124, 8900, 0), 0);
	CHECK_EQ(param(mem, 0), 1);
	CHECK_EQ(CONTROL(107, 9200, 0), 0);
	CHECK_EQ(param(mem, 0) >> 8, 3);
	mem_w8(mem, BUF + 0x101, 0x07);
	CHECK_EQ(CONTROL(122, 8600, 2, BUF >> 16, (BUF + 0x100) & 0xFFFF), -50);       /* (no track 7) */
	CHECK_EQ(CONTROL(106, 8600, 0, 0, 0), 0);
	disk_close(&d);

	/* a swappable drive: the disc taken out while playing is silent at once; none: off line */
	CHECK_EQ(disk_open_cdrom(&d, mem, cue), 0);
	load_driver(&d, mem);
	mem_w32(mem, 0x308 + 2, 0);
	mem_w32(mem, 0x308 + 6, 0);
	CHECK_EQ(io(&d, mem, NDRV_INITIALIZE, 0, 0), 0);
	CHECK_EQ(CONTROL(104, 0, 2, 0, 0x02, 0, 9), 0);
	cdaudio_run(&d.cd->audio, 100);
	CHECK(cdaudio_active(&d.cd->audio));
	disk_eject(&d);
	CHECK(!cdaudio_active(&d.cd->audio));
	CHECK_EQ(CONTROL(100, 200, 1), -65);
	CHECK_EQ(CONTROL(109, 200, 0xFFFF), 0);                 /* (the volume needs no disc) */
	disk_close(&d);

	/* a disc of audio alone: its drive reads zeros (Audio CD Access mounts it) */
	snprintf(text, sizeof text, "FILE \"%s\" BINARY\n TRACK 01 AUDIO\n  INDEX 01 00:00:00\n", audio);
	f = fopen(cue, "w");
	fputs(text, f);
	fclose(f);
	CHECK_EQ(disk_open(&d, mem, cue, true, true), 0);
	CHECK_EQ(d.part[0].size, 40u * 2048);
	load_driver(&d, mem);
	mem_w32(mem, 0x308 + 2, 0);
	mem_w32(mem, 0x308 + 6, 0);
	CHECK_EQ(io(&d, mem, NDRV_INITIALIZE, 0, 0), 0);
	mem_w16(mem, 0x30000, 0x4244);
	CHECK_EQ(read_block(&d, mem, 8, 2), 0);
	CHECK_EQ(mem_r16(mem, 0x30000), 0);
	disk_close(&d);
	unlink(cue);
	unlink(audio);
	unlink(data);
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

	/* a floppy's size to the floppy drive, anything else (a cue sheet too) to the first empty CD-ROM drive */
	char cue[300];
	snprintf(cue, sizeof cue, "%s.cue", cd);
	FILE *sheet = fopen(cue, "w");
	fprintf(sheet, "FILE \"%s\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n", cd);
	fclose(sheet);
	CHECK(drives_pick(&dr, flop, DRIVES_ANY, why, sizeof why) == &f);
	CHECK(drives_pick(&dr, cd, DRIVES_ANY, why, sizeof why) == &c1);
	CHECK(drives_pick(&dr, cue, DRIVES_ANY, why, sizeof why) == &c1);
	unlink(cue);
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
	test_cd_calls(&mem);
	test_routing(&mem);
	mem_free(&mem);
}
