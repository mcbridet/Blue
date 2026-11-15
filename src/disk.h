/*
 * Native block storage: a host file presented to Mac OS as one or more
 * drives. Like the display, Mac OS loads a generated NDRV for it from the
 * device tree; DoDriverIO is a host call, and on Initialize the driver puts
 * its drives in the drive queue so the Start Manager and File Manager can use
 * them. A file with an Apple partition map gives one drive per HFS
 * partition (as Apple's own disk drivers do); otherwise the whole file is
 * one drive.
 *
 * Drives with removable media are too, the floppy drive (see
 * disk_open_floppy()) and CD-ROM drives whose discs can be swapped (see
 * disk_open_cdrom()): images go in and come out while the machine runs,
 * and Mac OS ejects them through the driver.
 *
 * CD-ROM drives (`cdrom`, swappable or not) answer as Apple's CD-ROM driver
 * does (cdrom.h): DriverGestalt 'devt' 'cdrm', the disc's table of contents,
 * playing and reading audio, a CD-ROM drive's icons. Their images are plain
 * (2048-byte sectors) or cue sheets (cdimage.h), whose data track Mac OS
 * mounts; a disc of audio alone has a drive whose reads fail.
 */
#ifndef BLUE_DISK_H
#define BLUE_DISK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdimage.h"
#include "cdrom.h"
#include "floppy.h"
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
	bool ejected;                   /* (removable) Mac OS ejected this drive, the disc staying for another's
					 * volume in use */
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
	/*
	 * Removable media (`removable`): the floppy drive (`floppy`, one drive),
	 * and swappable CD-ROM drives (one drive per partition of the disc in
	 * at startup, at least one). The image is in it (`media`, host side) or
	 * not. The driver's timer tells Mac OS (`shown`, in the drives' flags)
	 * and posts disk-inserted events; of an image put in while the machine
	 * runs, only once applications run, so that it is never the startup disk
	 * (a CD in at startup may be).
	 */
	bool removable, floppy;
	bool media, shown;
	unsigned media_gen, shown_gen;  /* (an image put in after another before Mac OS saw that one go) */
	unsigned media_parts;           /* the drives the image has partitions for (the others are empty) */
	struct floppy_image image;
	char media_path[1024];
	/* a CD-ROM drive's: the disc's tracks (its data through disc_data for a cue sheet), the drive's state */
	struct cdimage *disc;
	struct disk_backend disc_data;
	struct cdrom *cd;               /* (made at the driver's first call) */
	bool dirty;                     /* written since last flushed (and a DC42 image's checksums worked out) */
	uint64_t written_tb;            /* ... last, at this timebase */
	unsigned long ejects;           /* by Mac OS */
};

/*
 * Opens the backing file and reads its partition map; returns 0 on success.
 * A CD (`cdrom`, then read-only) may be a cue sheet too.
 */
int disk_open(struct disk *d, struct mem *mem, const char *path, bool read_only, bool cdrom);

/*
 * Reads `len` bytes at `at` of a partition's (a `struct disk_reader`):
 * the image's data, whatever it's in (for hfsread_open_with()). 0, or -1.
 */
struct disk_reader {
	const struct disk *d;
	unsigned part;
};
int disk_read(void *reader, void *buf, size_t len, uint64_t at);
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

/*
 * The floppy drive, empty: a drive whose disk comes and goes. Its driver
 * (disk_build_ndrv()) is for a node named "syshm,floppy", answers as a
 * floppy drive (icons, format list, DriverGestalt 'devt' 'flop'), and
 * ejects when Mac OS asks (Put Away, Eject), which empties the drive.
 */
void disk_open_floppy(struct disk *d, struct mem *mem);

/*
 * A CD-ROM drive whose discs can be swapped, with the image at `path` in
 * it (NULL: empty), shown to Mac OS at once. It answers as the disks do
 * (it looks like one), but says its disc is ejectable, and Mac OS ejects it
 * through the driver; the disc comes out once none of its volumes is in use.
 * Returns 0, or -1 (having said why).
 */
int disk_open_cdrom(struct disk *d, struct mem *mem, const char *path);

/*
 * Puts an image in the drive (taking out the one in it, if any): read-only
 * if the file can't be written (a CD always). Returns 0, or -1 with the
 * reason in `why` (the drive is left as it was).
 */
int disk_insert(struct disk *d, const char *path, char *why, size_t len);

/* Whether an image would go in a CD-ROM drive: 0, or -1 with the reason in `why` */
int disk_check_cd(const char *path, char *why, size_t len);

/* Takes the image out, host side (what was written is flushed); Mac OS learns of it from the driver's timer. */
void disk_eject(struct disk *d);

/* Writes what's pending: a DC42 image's checksums, and the file to its disk. */
void disk_flush(struct disk *d);

/* Whether Mac OS has a volume on the drive (one of its drives) mounted, so that only it can eject it */
bool disk_in_use(const struct disk *d);

#endif
