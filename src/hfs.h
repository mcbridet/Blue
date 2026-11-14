/*
 * Creates an HFS volume, laid out the way Mac OS's Disk Initialization
 * package does it (Inside Macintosh: Files, "Data Organization on Volumes"):
 * boot blocks, master directory block, volume bitmap, then the extents
 * overflow and catalog B-trees, and the alternate MDB near the end. The
 * volume may start out with files in its root folder.
 */
#ifndef SYSHM_HFS_H
#define SYSHM_HFS_H

#include <stdint.h>

#define HFS_MAX_NAME 27

struct hfs_file {
	const char *name;               /* 1-31 letters, digits and spaces */
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

/* Seconds since 1904-01-01 in local time, as Mac OS counts dates ($SOURCE_DATE_EPOCH if set). */
uint32_t hfs_mac_time_now(void);

#endif
