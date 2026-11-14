/*
 * Floppy disk images, for the floppy drive (disk.h's removable drive): raw
 * images of 400K, 720K, 800K, 1440K or 2880K disks, and Disk Copy 4.2
 * images (an 84-byte header, the disk's blocks, then their tags; also
 * inside MacBinary), told apart by their contents and size, not their
 * names. Compressed and other kinds of images are only recognized, to be
 * refused with a reason. Also the drive's icons.
 */
#ifndef SYSHM_FLOPPY_H
#define SYSHM_FLOPPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum floppy_kind { FLOPPY_RAW, FLOPPY_DC42 };

struct floppy_image {
	enum floppy_kind kind;
	uint64_t data_offset, data_size;        /* the disk's blocks, in the file */
	uint64_t header_offset;                 /* DC42: its header (0, or 128 inside MacBinary) */
	uint64_t tag_offset, tag_size;          /* ... and the tags after the blocks */
	bool checksums_ok;                      /* ... whose checksums match */
	char volume[64];                        /* what's on it, for messages: HFS "Name", FAT, blank, ... */
};

/* Looks at an open image; returns 0, or -1 with the reason it isn't taken in `why`. */
int floppy_probe(int fd, struct floppy_image *img, char *why, size_t len);

/* ... the same for a file */
int floppy_check(const char *path, char *why, size_t len);

/* What an open image is if it's one syshm doesn't take, a CD's either (compressed, an archive, ...); NULL if not */
const char *floppy_other_image(int fd);

/* Whether a raw image of this many bytes is a floppy's */
bool floppy_raw_size(uint64_t bytes);

/* Disk Copy 4.2's checksum of `len` bytes (a whole number of words), going on from `sum` */
uint32_t floppy_dc42_sum(uint32_t sum, const uint8_t *p, size_t len);

/* Works the checksums of a DC42 image out again from the file, and puts them in its header; 0 on success */
int floppy_dc42_update(int fd, const struct floppy_image *img);

/* Erases a disk (Format): its blocks, and a DC42 image's tags, become zeros; 0 on success */
int floppy_erase(int fd, const struct floppy_image *img);

/*
 * The drive's icons, as the driver hands them out: 'ICN#' data (icon and
 * mask, FLOPPY_ICN_SIZE bytes) for the disk and the drive (Control 22 and
 * 21), and icon families ('icns' data, FLOPPY_ICNS_SIZE bytes, with
 * 'ICN#', 'icl8', 'ics#' and 'ics8') for DriverGestalt 'mics' and 'dics'.
 */
#define FLOPPY_ICN_SIZE 256u
#define FLOPPY_ICNS_SIZE (8u + 8u + 256u + 8u + 1024u + 8u + 64u + 8u + 256u)
void floppy_icons(uint8_t *media_icn, uint8_t *drive_icn, uint8_t *media_icns, uint8_t *drive_icns);

/* An icon family from drawings in characters (see floppy.c), 32 and 16 pixels square: other drives' icons */
void floppy_icon_family(const char *const *art32, const char *const *art16, uint8_t *icn, uint8_t *icns);

#endif
