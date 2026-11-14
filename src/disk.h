/*
 * Native block storage: a host file presented to Mac OS as one or more
 * drives. Like the display, Mac OS loads a generated NDRV for it from the
 * device tree; DoDriverIO is a host call, and on Initialize the driver puts
 * its drives in the drive queue so the Start Manager and File Manager can use
 * them. A file with an Apple partition map gives one drive per HFS
 * partition (as Apple's own disk drivers do); otherwise the whole file is
 * one drive.
 */
#ifndef SYSHM_DISK_H
#define SYSHM_DISK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem.h"
#include "ndrv.h"

#define DISK_MAX_PARTITIONS 8

/* A drive whose blocks come from code rather than a file (a shared folder): one writable volume */
struct disk_backend {
	const char *name;               /* for messages */
	void *ctx;
	uint64_t size;                  /* bytes */
	int (*read)(void *ctx, void *buf, size_t len, uint64_t at);             /* 0, or -1 */
	int (*write)(void *ctx, const void *buf, size_t len, uint64_t at);
};

struct disk_partition {
	uint64_t offset, size;          /* bytes */
	unsigned map_entry;             /* its entry in the partition map (from 1), 0 without a map */
	char name[33];
	uint16_t drive_num;             /* 0 until queued */
	bool mounted;                   /* the File Manager has read its MDB */
	uint64_t next_post;             /* timebase of the next disk-inserted event, 0: now */
};

struct disk {
	struct mem *mem;
	const char *path;
	int fd;
	const struct disk_backend *backend;     /* instead of the file, if set */
	uint64_t size;                  /* bytes */
	bool read_only;
	bool cdrom;                     /* report as a CD-ROM */
	struct disk_partition part[DISK_MAX_PARTITIONS];
	unsigned partitions;
	uint16_t ref_num;
	unsigned long calls, reads, writes;
	uint64_t bytes_read, bytes_written;
	bool log;
	/*
	 * For ROM files with parcels: the DriverGestalt answers that tie the
	 * drives to the node in the device tree ('nmrg', 'intf', 'dvrf', 'ofpt'),
	 * by which Mac OS 9.1's StartLib finds the drive /chosen's bootpath names
	 * (see status()). `unit` is the node's unit address, its PCI device.
	 */
	bool boot_gestalt;
	unsigned unit;
	struct ndrv_layout layout;
};

/* Opens the backing file and reads its partition map; returns 0 on success. */
int disk_open(struct disk *d, struct mem *mem, const char *path, bool read_only, bool cdrom);
void disk_close(struct disk *d);

/* Attaches a drive whose blocks come from `b`; returns 0 on success. */
int disk_open_backend(struct disk *d, struct mem *mem, const struct disk_backend *b);

/* Whether a partition starts with boot blocks (what the Start Manager looks for) */
bool disk_bootable(const struct disk *d, unsigned part);

/* Creates a (sparse) image of `size` bytes holding an empty HFS volume; returns 0 on success. */
int disk_create(const char *path, uint64_t size, const char *volume_name);

size_t disk_build_ndrv(struct disk *d, uint8_t *out, size_t size, uint32_t hostcall);

/*
 * Runs the driver for one host call: DoDriverIO (entry 0), its timer (entry
 * 1), or their resumption after a callout. `timebase_hz` paces the timer.
 */
void disk_driver_call(struct disk *d, struct ndrv_call *c, uint32_t timebase_hz);

#endif
