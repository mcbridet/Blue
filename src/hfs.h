/*
 * Creates HFS volumes, laid out the way Mac OS's Disk Initialization
 * package does it (Inside Macintosh: Files, "Data Organization on Volumes"):
 * boot blocks, master directory block, volume bitmap, then the extents
 * overflow and catalog B-trees, and the alternate MDB near the end. A
 * volume may start out with files and folders, each fork in one extent.
 */
#ifndef SYSHM_HFS_H
#define SYSHM_HFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HFS_MAX_NAME 27                 /* a volume's name */
#define HFS_MAX_FILE_NAME 31
#define HFS_FIRST_ID 16u                /* the first catalog node ID for files and folders (the root's is 2) */
#define HFS_UNMOUNTED 0x0100u           /* MDB attributes: the volume was put away cleanly */
#define HFS_MAX_FORK 0x7FFFFFFFu

struct hfs_file {
	const char *name;               /* 1-31 characters of Mac OS Roman, no colons */
	uint32_t type, creator;
	uint16_t finder_flags;
	int16_t v, h;                   /* icon position in the folder's window */
	const uint8_t *data, *rsrc;     /* the forks */
	uint32_t data_len, rsrc_len;
};

/*
 * Formats `size` bytes of the open file `fd` (from offset 0), with `nfiles`
 * files in the root folder. Returns 0 on success.
 */
int hfs_format(int fd, uint64_t size, const char *name, uint32_t mac_time, const struct hfs_file *files,
	       unsigned nfiles);

/* An entry of a volume being laid out: a folder or a file */
struct hfs_entry {
	uint8_t name[32];               /* a Pascal string of Mac OS Roman: 1-31 characters (the root's, the
					 * volume's name: 1-27), no colons */
	unsigned parent;                /* the folder it's in, an earlier entry (the root's: 0, itself) */
	bool folder;
	uint8_t finder[32];             /* FInfo and FXInfo (DInfo and DXInfo for a folder) */
	uint32_t created, modified;     /* Mac OS times */
	uint32_t data_len, rsrc_len;    /* a file's forks, at most HFS_MAX_FORK bytes */
	uint32_t id;                    /* catalog node ID, at least HFS_FIRST_ID; 0: hfs_build() gives it the
					 * next after the highest given, in order (entry k: 15 + k if none is) */
	/* set by hfs_build(): */
	uint32_t data_start, rsrc_start; /* each fork's first allocation block (one extent each; 0 if empty) */
};

/* A laid-out volume: its metadata, and where the forks' data goes */
struct hfs_layout {
	uint64_t size;                  /* bytes */
	uint32_t block_size;            /* allocation block size */
	uint64_t first_block;           /* the offset of allocation block 0 */
	uint8_t *meta;                  /* the volume's first meta_len bytes (malloc'd): boot blocks (none), the
					 * MDB, the bitmap and the B-trees */
	size_t meta_len;
	uint8_t mdb[512];               /* the MDB, as at 1024 and at size - 1024 (the alternate) */
	uint32_t blocks, free_blocks;
};

/*
 * Lays out a volume holding entries[0..n-1], entry 0 being the root folder:
 * `size` bytes, or with size 0 as small as the entries allow with `free`
 * bytes free. Entry k's data fork goes at first_block + data_start *
 * block_size, and likewise its resource fork. Returns 0, or -1 if two
 * names in a folder are the same to Mac OS, a name is invalid, or it can't
 * all fit (HFS has at most 65535 allocation blocks). hfs_layout_free()
 * frees `meta`.
 */
int hfs_build(struct hfs_layout *l, struct hfs_entry *entries, unsigned n, uint64_t size, uint64_t free_bytes,
	      uint32_t mac_time);
void hfs_layout_free(struct hfs_layout *l);

/*
 * The catalog's order of names (Pascal strings' characters): Mac OS's
 * RelString without regard to case, but with regard to diacritical marks.
 */
int hfs_compare_names(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen);

/* Mac OS Roman characters 0x80-0xFF in Unicode */
extern const uint16_t hfs_mac_roman[128];

/*
 * A name in UTF-8 (precomposed or decomposed) in Mac OS Roman: characters
 * it lacks become '_' (combining marks it can't combine are dropped).
 * Writes at most `max` characters, and returns how many there were in all.
 */
size_t hfs_from_utf8(const char *s, uint8_t *out, size_t max);

/* Mac OS Roman in UTF-8 (precomposed), NUL-terminated as far as `len` allows. Returns its length. */
size_t hfs_to_utf8(const uint8_t *s, size_t n, char *out, size_t len);

/* Seconds since 1904-01-01 in local time, as Mac OS counts dates ($SOURCE_DATE_EPOCH if set). */
uint32_t hfs_mac_time_now(void);

/* A Unix time as Mac OS counts it (local time, from 1904), and back */
uint32_t hfs_mac_time(int64_t unix_time);
int64_t hfs_unix_time(uint32_t mac_time);

#endif
