/*
 * Native block storage: a host file presented to Mac OS as one or more
 * drives. Like the display, Mac OS loads a generated NDRV for it from the
 * device tree; DoDriverIO is a host call, and on Initialize the driver puts
 * its drives in the drive queue so the Start Manager and File Manager can use
 * them. A file with an Apple partition map gives one drive per HFS
 * partition (as Apple's own disk drivers do); otherwise the whole file is
 * one drive.
 *
 * The floppy drive is one too, with removable media (see
 * disk_open_removable()): images (floppy.h) go in and come out while the
 * machine runs, and Mac OS ejects them through the driver.
 */
#ifndef SYSHM_DISK_H
#define SYSHM_DISK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "floppy.h"
#include "mem.h"
#include "ndrv.h"

#define DISK_MAX_PARTITIONS 8

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
	/*
	 * The floppy drive (`removable`): one drive, whose image is in it
	 * (`media`, host side) or not. The driver's timer tells Mac OS
	 * (`shown`, in the drive's flags), and posts a disk-inserted event, only
	 * once applications run, so that a floppy is never the startup disk.
	 */
	bool removable;
	bool media, shown;
	unsigned media_gen, shown_gen;  /* (an image put in after another before Mac OS saw that one go) */
	struct floppy_image image;
	char media_path[1024];
	bool dirty;                     /* written since last flushed (and a DC42 image's checksums worked out) */
	uint64_t written_tb;            /* ... last, at this timebase */
	unsigned long ejects;           /* by Mac OS */
};

/* Opens the backing file and reads its partition map; returns 0 on success. */
int disk_open(struct disk *d, struct mem *mem, const char *path, bool read_only, bool cdrom);
void disk_close(struct disk *d);

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

/*
 * The floppy drive, empty: a drive whose disk comes and goes. Its driver
 * (disk_build_ndrv()) is for a node named "syshm,floppy", answers as a
 * floppy drive (icons, format list, DriverGestalt 'devt' 'flop'), and
 * ejects when Mac OS asks (Put Away, Eject), which empties the drive.
 */
void disk_open_removable(struct disk *d, struct mem *mem);

/*
 * Puts an image in the drive (taking out the one in it, if any): read-only
 * if the file can't be written. Returns 0, or -1 with the reason in `why`
 * (the drive is left as it was).
 */
int disk_insert(struct disk *d, const char *path, char *why, size_t len);

/* Takes the image out, host side (what was written is flushed); Mac OS learns of it from the driver's timer. */
void disk_eject(struct disk *d);

/* Writes what's pending: a DC42 image's checksums, and the file to its disk. */
void disk_flush(struct disk *d);

/* Whether Mac OS has a volume on the drive mounted (in its volume queue), so that only it can eject it */
bool disk_in_use(const struct disk *d);

#endif
