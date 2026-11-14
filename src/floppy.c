#include "floppy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "be.h"

#define K 1024u
#define DC42_HEADER 84u
#define DC42_DATA_SIZE 0x40u
#define DC42_TAG_SIZE 0x44u
#define DC42_DATA_SUM 0x48u
#define DC42_TAG_SUM 0x4Cu
#define DC42_MAGIC 0x52u                /* the private word: 0x0100 */
#define DC42_TAG_SKIP 12u               /* the first block's tags aren't in the tag checksum */
#define MACBINARY_HEADER 128u
#define MAX_FLOPPY (2880u * K)
#define CHUNK (64u * K)

bool floppy_raw_size(uint64_t bytes)
{
	return bytes == 400 * K || bytes == 720 * K || bytes == 800 * K || bytes == 1440 * K || bytes == 2880 * K;
}

uint32_t floppy_dc42_sum(uint32_t sum, const uint8_t *p, size_t len)
{
	for (size_t k = 0; k + 1 < len; k += 2) {
		sum += be16(p + k);
		sum = sum >> 1 | sum << 31;
	}
	return sum;
}

static int fail(char *why, size_t len, const char *what)
{
	snprintf(why, len, "%s", what);
	return -1;
}

/* The checksum of `size` bytes of the file from `at` (both even) */
static int file_sum(int fd, uint64_t at, uint64_t size, uint32_t *sum)
{
	uint8_t buf[CHUNK];
	*sum = 0;
	while (size) {
		size_t n = size < CHUNK ? (size_t)size : CHUNK;
		if (pread(fd, buf, n, (off_t)at) != (ssize_t)n)
			return -1;
		*sum = floppy_dc42_sum(*sum, buf, n);
		at += n;
		size -= n;
	}
	return 0;
}

static int dc42_sums(int fd, const struct floppy_image *img, uint32_t *data, uint32_t *tags)
{
	*tags = 0;
	if (file_sum(fd, img->data_offset, img->data_size, data))
		return -1;
	if (img->tag_size > DC42_TAG_SKIP)
		return file_sum(fd, img->tag_offset + DC42_TAG_SKIP, img->tag_size - DC42_TAG_SKIP, tags);
	return 0;
}

int floppy_dc42_update(int fd, const struct floppy_image *img)
{
	uint8_t sums[8];
	uint32_t data, tags;
	if (img->kind != FLOPPY_DC42 || dc42_sums(fd, img, &data, &tags))
		return -1;
	put_be32(sums, data);
	put_be32(sums + 4, tags);
	return pwrite(fd, sums, 8, (off_t)(img->header_offset + DC42_DATA_SUM)) == 8 ? 0 : -1;
}

int floppy_erase(int fd, const struct floppy_image *img)
{
	static const uint8_t zero[CHUNK];
	uint64_t ranges[2][2] = { { img->data_offset, img->data_size }, { img->tag_offset, img->tag_size } };
	for (int r = 0; r < (img->kind == FLOPPY_DC42 ? 2 : 1); r++)
		for (uint64_t at = 0; at < ranges[r][1]; at += CHUNK) {
			size_t n = ranges[r][1] - at < CHUNK ? (size_t)(ranges[r][1] - at) : CHUNK;
			if (pwrite(fd, zero, n, (off_t)(ranges[r][0] + at)) != (ssize_t)n)
				return -1;
		}
	return img->kind == FLOPPY_DC42 ? floppy_dc42_update(fd, img) : 0;
}

/* A Disk Copy 4.2 image's header at `at`, in a file (or MacBinary data fork) of `room` bytes from there */
static bool dc42_header(const uint8_t *h, uint64_t at, uint64_t room, struct floppy_image *img)
{
	uint64_t data = be32(h + DC42_DATA_SIZE), tags = be32(h + DC42_TAG_SIZE);
	if (h[0] > 63 || be16(h + DC42_MAGIC) != 0x0100 || !data || data % 512 || data > MAX_FLOPPY ||
	    tags > data || DC42_HEADER + data + tags > room)
		return false;
	img->kind = FLOPPY_DC42;
	img->header_offset = at;
	img->data_offset = at + DC42_HEADER;
	img->data_size = data;
	img->tag_offset = img->data_offset + data;
	img->tag_size = tags;
	return true;
}

/* What's on the disk, for messages; -1 (with why) for what Mac OS can't use at all */
static int describe_volume(int fd, struct floppy_image *img, char *why, size_t len)
{
	uint8_t boot[512], mdb[512];
	if (pread(fd, boot, 512, (off_t)img->data_offset) != 512 ||
	    pread(fd, mdb, 512, (off_t)img->data_offset + 1024) != 512)
		return fail(why, len, "cannot read it");
	unsigned sig = be16(mdb);
	if (sig == 0x4244) {                    /* 'BD' */
		unsigned n = mdb[36] < 27 ? mdb[36] : 27;
		snprintf(img->volume, sizeof img->volume, "%s \"%.*s\"", be16(mdb + 124) == 0x482B ? "HFS+" : "HFS",
			 (int)n, (const char *)mdb + 37);
	} else if (sig == 0x482B || sig == 0x4858) {
		snprintf(img->volume, sizeof img->volume, "HFS+");
	} else if (sig == 0xD2D7) {
		return fail(why, len, "an MFS disk (the first Macs' file system): Mac OS 8.5 and later can't read "
			    "MFS disks (System 6 and earlier can)");
	} else if ((boot[0] == 0xEB || boot[0] == 0xE9) && boot[11] == 0 && boot[12] == 2) {
		char label[12] = "";
		if (boot[38] == 0x29) {
			memcpy(label, boot + 43, 11);
			for (int k = 10; k >= 0 && label[k] == ' '; k--)
				label[k] = '\0';
			if (strcmp(label, "NO NAME") == 0)
				label[0] = '\0';
		}
		snprintf(img->volume, sizeof img->volume, label[0] ? "FAT \"%s\"" : "FAT", label);
	} else {
		bool blank = true;
		for (unsigned k = 0; k < 512 && blank; k++)
			blank = !boot[k] && !mdb[k];
		snprintf(img->volume, sizeof img->volume, blank ? "blank" : "not HFS or FAT");
	}
	return 0;
}

int floppy_probe(int fd, struct floppy_image *img, char *why, size_t len)
{
	struct stat st;
	uint8_t h[MACBINARY_HEADER], tail[4];
	memset(img, 0, sizeof *img);
	if (fstat(fd, &st) || !S_ISREG(st.st_mode))
		return fail(why, len, "not a file");
	uint64_t size = (uint64_t)st.st_size;
	if (size < sizeof h || pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h)
		return fail(why, len, "too small to be a floppy image");

	const char *other = NULL;
	if (h[0] == 0x1F && h[1] == 0x8B)
		other = "gzip-compressed";
	else if (!memcmp(h, "PK\3\4", 4))
		other = "a ZIP archive";
	else if (!memcmp(h, "BZh", 3))
		other = "bzip2-compressed";
	else if (!memcmp(h, "SIT!", 4) || !memcmp(h, "StuffIt", 7))
		other = "a StuffIt archive";
	else if (!memcmp(h, "(This file must be converted with BinHex", 40))
		other = "BinHex-encoded";
	else if (!memcmp(h, "2IMG", 4))
		other = "an Apple II 2IMG image";
	else if (h[0] <= 2 && (h[1] == 1 || h[1] == 2 || h[1] == 3 || (h[1] >= 0x10 && h[1] <= 0x12)) &&
		 (be16(h + 2) == 400 || be16(h + 2) == 720 || be16(h + 2) == 800 || be16(h + 2) == 1440))
		other = "a DART image";
	else if (size >= 512 && pread(fd, tail, 4, (off_t)(size - 512)) == 4 && !memcmp(tail, "koly", 4))
		other = "a UDIF image (.dmg)";
	if (other) {
		snprintf(why, len, "%s: syshm takes raw floppy images (400K, 720K, 800K, 1440K) and Disk Copy 4.2 "
			 "images; convert it to one of those first", other);
		return -1;
	}

	/* MacBinary: a DC42 image in its data fork */
	if (!h[0] && h[1] >= 1 && h[1] <= 63 && !h[74] && !h[82] && be32(h + 83) <= size - MACBINARY_HEADER) {
		uint8_t dc[DC42_HEADER];
		if (pread(fd, dc, sizeof dc, MACBINARY_HEADER) != (ssize_t)sizeof dc ||
		    !dc42_header(dc, MACBINARY_HEADER, be32(h + 83), img)) {
			snprintf(why, len, "a MacBinary file of type '%.4s', not a Disk Copy 4.2 image in one",
				 (const char *)h + 65);
			return -1;
		}
	} else if (!dc42_header(h, 0, size, img) || DC42_HEADER + img->data_size + img->tag_size != size) {
		if (size > MAX_FLOPPY) {
			snprintf(why, len, "too big for a floppy (%llu MB): disk and CD images go on syshm's command "
				 "line (--disk, --cdrom)", (unsigned long long)(size >> 20));
			return -1;
		}
		if (!floppy_raw_size(size)) {
			snprintf(why, len, "%llu bytes is no floppy's size: syshm takes raw images of 400K, 720K, "
				 "800K, 1440K or 2880K disks, and Disk Copy 4.2 images", (unsigned long long)size);
			return -1;
		}
		memset(img, 0, sizeof *img);
		img->kind = FLOPPY_RAW;
		img->data_size = size;
	}
	if (img->kind == FLOPPY_DC42) {
		uint8_t sums[8];
		uint32_t data, tags;
		img->checksums_ok = pread(fd, sums, 8, (off_t)(img->header_offset + DC42_DATA_SUM)) == 8 &&
				    !dc42_sums(fd, img, &data, &tags) && be32(sums) == data && be32(sums + 4) == tags;
	}
	return describe_volume(fd, img, why, len);
}

int floppy_check(const char *path, char *why, size_t len)
{
	struct floppy_image img;
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		snprintf(why, len, "%s", strerror(errno));
		return -1;
	}
	int rc = floppy_probe(fd, &img, why, len);
	close(fd);
	return rc;
}

/*
 * The icons, drawn in characters: '.' is outside (the mask), the rest the
 * colors of the Mac's standard 8-bit palette below, black or white in the
 * 1-bit icon.
 */
static const struct {
	char c;
	uint8_t index;                  /* in the standard palette */
	bool black;                     /* in the 1-bit icon */
} colors[] = {
	{ 'K', 255, true },             /* black */
	{ 'D', 253, true },             /* 222222 */
	{ 'b', 252, false },            /* 444444 */
	{ 'h', 251, false },            /* 555555 */
	{ 'G', 249, false },            /* 888888 */
	{ 'M', 248, false },            /* AAAAAA */
	{ 'g', 247, false },            /* BBBBBB */
	{ 'L', 246, false },            /* DDDDDD */
	{ 'W', 0, false },              /* white */
	{ 'Z', 191, true },             /* 00CC00 */
};

static const char *const disk32[32] = {
	"................................",
	"................................",
	"..KKKKKKKKKKKKKKKKKKKKKKKKK.....",
	"..KhhhhhhKLLLLLLLLLLLLKhhhhK....",
	"..KhbbbbbKLMMMMMMDDDDMKbbbbbK...",
	"..KhbbbbbKLMMMMMMDDDDMKbbbbbbK..",
	"..KhbbbbbKLMMMMMMDDDDMKbbbbbbK..",
	"..KhbbbbbKLMMMMMMDDDDMKbbbbbbK..",
	"..KhbbbbbKLMMMMMMDDDDMKbbbbbbK..",
	"..KhbbbbbKLMMMMMMDDDDMKbbbbbbK..",
	"..KhbbbbbKLMMMMMMDDDDMKbbbbbbK..",
	"..KhbbbbbKLMMMMMMMMMMMKbbbbbbK..",
	"..KhbbbbbKLMMMMMMMMMMMKbbbbbbK..",
	"..KhbbbbbKKKKKKKKKKKKKKbbbbbbK..",
	"..KhbbbbbbbbbbbbbbbbbbbbbbbbbK..",
	"..KhbbbbbbbbbbbbbbbbbbbbbbbbbK..",
	"..KhbbbbbbbbbbbbbbbbbbbbbbbbbK..",
	"..KhbbKKKKKKKKKKKKKKKKKKKKbbbK..",
	"..KhbbKWWWWWWWWWWWWWWWWWWKbbbK..",
	"..KhbbKWWWWWWWWWWWWWWWWWWKbbbK..",
	"..KhbbKWWWWWWWWWWWWWWWWWWKbbbK..",
	"..KhbbKWWggggggggggggggWWKbbbK..",
	"..KhbbKWWWWWWWWWWWWWWWWWWKbbbK..",
	"..KhbbKWWWWWWWWWWWWWWWWWWKbbbK..",
	"..KhbbKWWggggggggggggggWWKbbbK..",
	"..KhbbKWWWWWWWWWWWWWWWWWWKbbbK..",
	"..KhbbKWWWWWWWWWWWWWWWWWWKbKKK..",
	"..KhbbKWWWWWWWWWWWWWWWWWWKbKKK..",
	"..KhbbKWWWWWWWWWWWWWWWWWWKbbbK..",
	"..KKKKKKKKKKKKKKKKKKKKKKKKKKKK..",
	"................................",
	"................................",
};

static const char *const disk16[16] = {
	"................",
	".KKKKKKKKKKKK...",
	".KbbKLMMDDKbbK..",
	".KbbKLMMDDKbbbK.",
	".KbbKLMMDDKbbbK.",
	".KbbKKKKKKKbbbK.",
	".KbbbbbbbbbbbbK.",
	".KbKKKKKKKKKKbK.",
	".KbKWWWWWWWWKbK.",
	".KbKWWWWWWWWKbK.",
	".KbKWggggggWKbK.",
	".KbKWWWWWWWWKbK.",
	".KbKWggggggWKbK.",
	".KbKWWWWWWWWKbK.",
	".KKKKKKKKKKKKKK.",
	"................",
};

static const char *const drive32[32] = {
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
	".KWWWWWWWWWWWWWWWWWWWWWWWWWWWWK.",
	".KWLLLLLLLLLLLLLLLLLLLLLLLLLLGK.",
	".KWLLLLLLLLLLLLLLLLLLLLLLLLLLGK.",
	".KWLLLLLLLLLLLLLLLLLLLLLLLLLLGK.",
	".KWLLLLLLLLLLLLLLLLLLLLLLLLLLGK.",
	".KWLLLLKKKKKKKKKKKKKKKKKKLLLLGK.",
	".KWLLLLKDDDDDDDDDDDDDDDDKLLLLGK.",
	".KWLLLLWWWWWWWWWWWWWWWWWWLLLLGK.",
	".KWLLLLLLLLLLLLLLLLLLLLLLLLLLGK.",
	".KWLZZLLLLLLLLLLLLLLLLGGGGLLLGK.",
	".KWLLLLLLLLLLLLLLLLLLLGGGGLLLGK.",
	".KWLLLLLLLLLLLLLLLLLLLLLLLLLLGK.",
	".KWLLLLLLLLLLLLLLLLLLLLLLLLLLGK.",
	".KGGGGGGGGGGGGGGGGGGGGGGGGGGGGK.",
	".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
};

static const char *const drive16[16] = {
	"................",
	"................",
	"................",
	"................",
	"................",
	".KKKKKKKKKKKKKK.",
	".KWWWWWWWWWWWWK.",
	".KWLKKKKKKKKLGK.",
	".KWLKDDDDDDKLGK.",
	".KWLLLLLLLLLLGK.",
	".KWZLLLLLLGGLGK.",
	".KGGGGGGGGGGGGK.",
	".KKKKKKKKKKKKKK.",
	"................",
	"................",
	"................",
};

/* A drawing as a 1-bit icon and mask ('ICN#', 'ics#': rows of n bits) and 8-bit pixels ('icl8', 'ics8') */
static void draw(const char *const *art, unsigned n, uint8_t *bw, uint8_t *pixels)
{
	unsigned row = n / 8;
	memset(bw, 0, 2 * row * n);
	for (unsigned y = 0; y < n; y++)
		for (unsigned x = 0; x < n; x++) {
			char c = art[y][x];
			pixels[y * n + x] = 0;
			if (c == '.')
				continue;
			bw[row * n + y * row + x / 8] |= (uint8_t)(0x80u >> (x % 8));         /* mask */
			for (size_t k = 0; k < sizeof colors / sizeof colors[0]; k++)
				if (colors[k].c == c) {
					pixels[y * n + x] = colors[k].index;
					if (colors[k].black)
						bw[y * row + x / 8] |= (uint8_t)(0x80u >> (x % 8));
				}
		}
}

static uint8_t *icns_element(uint8_t *p, const char *type, const uint8_t *data, uint32_t len)
{
	memcpy(p, type, 4);
	put_be32(p + 4, 8 + len);
	memcpy(p + 8, data, len);
	return p + 8 + len;
}

static void icon_family(const char *const *art32, const char *const *art16, uint8_t *icn, uint8_t *icns)
{
	uint8_t icl8[1024], ics[64], ics8[256];
	draw(art32, 32, icn, icl8);
	draw(art16, 16, ics, ics8);
	memcpy(icns, "icns", 4);
	put_be32(icns + 4, FLOPPY_ICNS_SIZE);
	uint8_t *p = icns_element(icns + 8, "ICN#", icn, FLOPPY_ICN_SIZE);
	p = icns_element(p, "icl8", icl8, sizeof icl8);
	p = icns_element(p, "ics#", ics, sizeof ics);
	icns_element(p, "ics8", ics8, sizeof ics8);
}

void floppy_icons(uint8_t *media_icn, uint8_t *drive_icn, uint8_t *media_icns, uint8_t *drive_icns)
{
	icon_family(disk32, disk16, media_icn, media_icns);
	icon_family(drive32, drive16, drive_icn, drive_icns);
}
