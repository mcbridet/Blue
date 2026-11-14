/*
 * Reads HFS and HFS+ volumes (Inside Macintosh: Files, "Data Organization on
 * Volumes"; Technical Note TN1150), HFS+ also inside an HFS wrapper: enough
 * to find a file of a given type in a folder and read its forks. That's how
 * New World Macs' firmware finds the "Mac OS ROM" file: the first file of
 * type 'tbxi' in the startup disk's blessed System Folder. Only the blocks
 * needed are read (pread()).
 */
#ifndef SYSHM_HFSREAD_H
#define SYSHM_HFSREAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HFSREAD_NAME_MAX 768            /* a name in UTF-8 (255 UTF-16 units), with its NUL */
#define HFSREAD_PATH_MAX 1024

struct hfsread_extent {
	uint32_t start, count;          /* allocation blocks */
};

struct hfsread_fork {
	uint32_t id;                    /* the file's ID */
	uint8_t type;                   /* 0 the data fork, 0xFF the resource fork */
	uint64_t size;                  /* bytes */
	struct hfsread_extent ext[8];   /* the first extents (3 on HFS); the rest are in the extents file */
};

struct hfsread_tree {
	struct hfsread_fork fork;
	uint32_t node_size, root, nodes;
	uint16_t max_key;
	bool variable_keys;             /* index nodes' keys are as long as they say, not max_key */
};

struct hfsread_volume {
	int fd;
	uint64_t offset;                /* of allocation block 0 in the file */
	uint32_t block_size, blocks;
	bool plus;                      /* HFS+ (or HFSX) */
	bool wrapped;                   /* ... inside an HFS wrapper */
	uint32_t blessed;               /* the blessed System Folder's ID (Finder info word 0), 0: none */
	struct hfsread_tree catalog, extents;
};

struct hfsread_file {
	char name[HFSREAD_NAME_MAX];    /* UTF-8 */
	uint32_t id, type, creator;
	struct hfsread_fork data, rsrc;
};

/* Opens the volume at `offset` (`size` bytes) in the open file `fd`. Returns 0, or -1 if it isn't one. */
int hfsread_open(struct hfsread_volume *v, int fd, uint64_t offset, uint64_t size);

/* The first file (in catalog order) of `type` in a folder: 1 found, 0 none, -1 the catalog is damaged. */
int hfsread_find(const struct hfsread_volume *v, uint32_t folder, uint32_t type, struct hfsread_file *f);

/* A folder's path from the volume's name, "Volume:Folder:...". Returns 0, or -1. */
int hfsread_path(const struct hfsread_volume *v, uint32_t folder, char *buf, size_t len);

/* Reads `len` bytes of a fork from `pos`. Returns 0, or -1 (past its end, damaged, or an I/O error). */
int hfsread_read(const struct hfsread_volume *v, const struct hfsread_fork *fork, uint64_t pos, void *buf,
		 size_t len);

/* A file found in a drive's System Folder */
struct hfsread_found {
	unsigned drive;                 /* which of the images */
	unsigned part;                  /* ... and which of its partitions (disk.h's order) */
	char partition[33];             /* its partition's name; "" for an image without a partition map */
	char path[HFSREAD_PATH_MAX];    /* "Volume:System Folder:Name" */
	uint8_t *data;                  /* the data fork (malloc'd) */
	size_t size;
};

/*
 * Looks at disk images as drives, in order (an image with an Apple partition
 * map is a drive per HFS partition, in map order), for the first volume whose
 * blessed System Folder holds a file of `type`, and reads that file's data
 * fork. Images are only opened read-only. Returns 1 if found, 0 if no volume
 * has one, -1 if an image can't be opened (disk_open() says why; err is
 * empty) or the file can't be read (err says why).
 */
int hfsread_system_file(const char *const *paths, unsigned count, uint32_t type, struct hfsread_found *found,
			char *err, size_t errlen);

/* A file or folder in a folder (see hfsread_list()) */
struct hfsread_entry {
	char name[HFSREAD_NAME_MAX];    /* UTF-8 */
	bool folder;
	uint32_t id;                    /* the file's or folder's ID */
	uint32_t type, creator;         /* (files: their Finder info) */
	uint16_t finder_flags;
	struct hfsread_fork data, rsrc;
};

/*
 * Calls `each` for every file and folder in a folder, in catalog order,
 * until it returns something other than 0. Returns that, 0 when done, or
 * -1 if the catalog is damaged.
 */
int hfsread_list(const struct hfsread_volume *v, uint32_t folder, int (*each)(void *ctx, const struct hfsread_entry *e),
		 void *ctx);

/* A name in Mac OS Roman (as HFS keeps them) in UTF-8 */
void hfsread_roman_utf8(const uint8_t *s, size_t n, char *out, size_t len);

#endif
