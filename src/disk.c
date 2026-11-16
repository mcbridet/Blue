#include "disk.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "be.h"
#include "hfs.h"
#include "ndrv.h"

/* OSErr values */
#define NO_ERR 0
#define CONTROL_ERR (-17)
#define STATUS_ERR (-18)
#define IO_ERR (-36)
#define WRITE_PROTECTED_ERR (-44)
#define PARAM_ERR (-50)
#define NO_SUCH_DRIVE_ERR (-56)
#define OFFLINE_ERR (-65)

/* Low-memory globals */
#define LM_UTABLE_BASE 0x011Cu          /* unit table: DCE handles */
#define LM_UNIT_COUNT 0x01D2u
#define LM_DRVQ_HDR 0x0308u             /* drive queue header: flags, head, tail */

/* IOParam / XIOParam fields */
#define PB_VREF_NUM 22                  /* drive number for driver calls */
#define PB_REF_NUM 24
#define PB_CS_CODE 26
#define PB_CS_PARAM 28
#define PB_BUFFER 32
#define PB_REQ_COUNT 36
#define PB_ACT_COUNT 40
#define PB_POS_MODE 44
#define PB_POS_OFFSET 46
#define POS_MODE_WIDE 0x0100u
#define FS_FROM_START 1

#define DCE_POSITION 16

/* Driver data: per drive, the drive status flags followed by its DrvQEl; then the timer */
#define DRIVE_DATA 20u
#define DRIVE_FLAGS 0u
#define DRIVE_QUEUE_ELEMENT 4u
#define DATA_EXPIRATION (DRIVE_DATA * DISK_MAX_PARTITIONS)     /* AbsoluteTime */
#define DATA_TIMER (DATA_EXPIRATION + 8)                        /* TimerID */
#define DATA_SIZE (DATA_TIMER + 4)
#define DATA_REG_ENTRY DATA_SIZE                                /* (with boot_gestalt) the node's RegEntryID */
#define DATA_SIZE_REG_ENTRY (DATA_REG_ENTRY + 16)
/* The floppy drive's and CD-ROM drives': their icons and where they are, which the driver hands out pointers to */
#define MEDIA_ICN DATA_SIZE_REG_ENTRY                           /* 'ICN#' (Control 22) */
#define DRIVE_ICN (MEDIA_ICN + FLOPPY_ICN_SIZE)                 /* 'ICN#' (Control 21) ... */
#define DRIVE_WHERE (DRIVE_ICN + FLOPPY_ICN_SIZE)               /* ... and where it is: Str31 */
#define MEDIA_ICNS (DRIVE_WHERE + 32)                           /* 'icns' data ('mics') */
#define DRIVE_ICNS (MEDIA_ICNS + FLOPPY_ICNS_SIZE)              /* ('dics') */
#define DATA_SIZE_ICONS (DRIVE_ICNS + FLOPPY_ICNS_SIZE)

enum { IMP_IO_COMPLETE, IMP_POST_EVENT, IMP_SET_TIMER, IMP_COUNT };

#define DISK_INSERTED_EVENT 7
#define MDB_OFFSET 1024u

/* Steps (state word 0) */
#define STEP_ARMING 0x41524D31u         /* 'ARM1': SetInterruptTimer returned, finish Initialize */
#define STEP_POSTING 0x504F5354u        /* 'POST': PostEvent returned */
#define STEP_REARMING 0x41524D32u       /* 'ARM2': SetInterruptTimer returned, timer done */

#define DRIVER_VERSION 0x01008000u
#define FIRST_DRIVE_NUMBER 8

static uint32_t os_type(const char *s)
{
	return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 | (uint8_t)s[3];
}

static void add_partition(struct disk *d, uint64_t offset, uint64_t size, const char *name, unsigned map_entry)
{
	if (d->partitions >= DISK_MAX_PARTITIONS || offset >= d->size)
		return;
	struct disk_partition *p = &d->part[d->partitions++];
	p->offset = offset;
	p->size = size < d->size - offset ? size : d->size - offset;
	p->map_entry = map_entry;
	snprintf(p->name, sizeof p->name, "%s", name);
}

static ssize_t read_at(const struct disk *d, void *buf, size_t len, uint64_t at)
{
	if (d->backend)
		return d->backend->read(d->backend->ctx, buf, len, at) ? -1 : (ssize_t)len;
	return pread(d->fd, buf, len, (off_t)at);
}

static ssize_t write_at(const struct disk *d, const void *buf, size_t len, uint64_t at)
{
	if (d->backend)
		return d->backend->write(d->backend->ctx, buf, len, at) ? -1 : (ssize_t)len;
	return pwrite(d->fd, buf, len, (off_t)at);
}

/*
 * Apple partition map: driver descriptor 'ER' in block 0, entries 'PM' from
 * block 1. The entries are normally 512-byte blocks counting 512-byte blocks,
 * also on CDs whose descriptor says 2048; otherwise in the descriptor's.
 */
static void read_partition_map(struct disk *d)
{
	uint8_t b[512];
	if (d->size < 1024 || read_at(d, b, sizeof b, 0) != (ssize_t)sizeof b || be16(b) != 0x4552)
		return;
	unsigned bs = be16(b + 2);
	if (bs != 512 && bs != 1024 && bs != 2048 && bs != 4096)
		bs = 512;
	if (read_at(d, b, sizeof b, 512) == (ssize_t)sizeof b && be16(b) == 0x504D)
		bs = 512;
	unsigned count = 1;
	for (unsigned k = 1; k <= count && k <= 64; k++) {
		if ((uint64_t)(k + 1) * bs > d->size || read_at(d, b, sizeof b, (uint64_t)k * bs) != (ssize_t)sizeof b ||
		    be16(b) != 0x504D)
			break;
		count = be32(b + 4);
		char type[33] = { 0 }, name[33] = { 0 };
		memcpy(type, b + 48, 32);
		memcpy(name, b + 16, 32);
		if (strcmp(type, "Apple_HFS") == 0)
			add_partition(d, (uint64_t)be32(b + 8) * bs, (uint64_t)be32(b + 12) * bs, name, k);
	}
}

static int cd_image(const char *path, struct disk *img, char *why, size_t len);
static void take_image(struct disk *d, struct disk *img);

int disk_open(struct disk *d, struct mem *mem, const char *path, bool read_only, bool cdrom)
{
	memset(d, 0, sizeof *d);
	if (cdrom) {                    /* (a fixed CD drive) */
		char why[400];
		struct disk img;
		if (cd_image(path, &img, why, sizeof why)) {
			fprintf(stderr, "%s: %s\n", path, why);
			return -1;
		}
		d->fd = -1;
		take_image(d, &img);
		d->partitions = img.partitions;
		memcpy(d->part, img.part, sizeof d->part);
		d->mem = mem;
		d->path = path;
		d->read_only = d->cdrom = true;
		return 0;
	}
	d->mem = mem;
	d->path = path;
	d->read_only = read_only;
	d->cdrom = cdrom;
	d->fd = open(path, read_only ? O_RDONLY : O_RDWR);
	if (d->fd < 0 && !read_only && (errno == EACCES || errno == EROFS)) {
		fprintf(stderr, "%s: not writable, attaching it read-only\n", path);
		d->read_only = true;
		d->fd = open(path, O_RDONLY);
	}
	if (d->fd < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -1;
	}
	struct stat st;
	if (fstat(d->fd, &st) || st.st_size < 1024) {
		fprintf(stderr, "%s: not a usable disk image\n", path);
		close(d->fd);
		d->fd = -1;
		return -1;
	}
	d->size = (uint64_t)st.st_size & ~(uint64_t)511;
	read_partition_map(d);
	if (!d->partitions)
		add_partition(d, 0, d->size, "", 0);
	return 0;
}

int disk_open_backend(struct disk *d, struct mem *mem, const struct disk_backend *b)
{
	memset(d, 0, sizeof *d);
	d->mem = mem;
	d->path = b->name;
	d->fd = -1;
	d->backend = b;
	d->size = b->size & ~(uint64_t)511;
	if (d->size < 1024) {
		fprintf(stderr, "%s: not a usable volume\n", b->name);
		return -1;
	}
	add_partition(d, 0, d->size, "", 0);
	return 0;
}

int disk_read(void *reader, void *buf, size_t len, uint64_t at)
{
	const struct disk_reader *r = reader;
	const struct disk_partition *p = &r->d->part[r->part];
	if (at > p->size || len > p->size - at)
		return -1;
	return read_at(r->d, buf, len, p->offset + at) == (ssize_t)len ? 0 : -1;
}

bool disk_bootable(const struct disk *d, unsigned part)
{
	uint8_t sig[2];
	return part < d->partitions && read_at(d, sig, 2, d->part[part].offset) == 2 && be16(sig) == 0x4C4B;
}

void disk_close(struct disk *d)
{
	if (d->fd >= 0) {
		if (d->dirty)
			disk_flush(d);
		else if (!d->read_only)
			fsync(d->fd);
		close(d->fd);
	}
	d->fd = -1;
	d->media = false;
	cdimage_close(d->disc);
	d->disc = NULL;
	cdrom_free(d->cd);
	d->cd = NULL;
}

int disk_create(const char *path, uint64_t size, const char *volume_name)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -1;
	}
	size &= ~(uint64_t)511;
	int err = ftruncate(fd, (off_t)size);
	if (err)
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
	else if ((err = hfs_format(fd, size, volume_name, hfs_mac_time_now(), NULL, 0)))
		fprintf(stderr, "%s: cannot make an HFS volume of this size and name\n", path);
	close(fd);
	if (err)
		unlink(path);
	return err ? -1 : 0;
}

size_t disk_build_ndrv(struct disk *d, uint8_t *out, size_t size, uint32_t hostcall)
{
	static const struct ndrv_import imports[IMP_COUNT] = {
		[IMP_IO_COMPLETE] = { "DriverServicesLib", "IOCommandIsComplete" },
		[IMP_POST_EVENT] = { "InterfaceLib", "PostEvent" },
		[IMP_SET_TIMER] = { "DriverServicesLib", "SetInterruptTimer" },
	};
	static const uint8_t zero[DATA_SIZE_REG_ENTRY];
	struct ndrv_spec spec = {
		.device_name = "blue,disk",
		.driver_name = ".BlueDisk",
		.runtime_options = NDRV_LOADED_UPON_DISCOVERY | NDRV_OPENED_UPON_LOAD,
		.category = 0x6E647276u,        /* 'ndrv' */
		.type = 0x626C6F6Bu,            /* 'blok' */
		.version = DRIVER_VERSION,
		.hostcall = hostcall,
		.imports = imports,
		.import_count = IMP_COUNT,
		.extra = zero,
		.extra_len = d->boot_gestalt ? DATA_SIZE_REG_ENTRY : DATA_SIZE,
	};
	uint8_t icons[DATA_SIZE_ICONS];
	if (d->floppy || d->cdrom) {
		const char *where = d->floppy ? "Blue floppy drive" : "Blue CD-ROM drive";
		memset(icons, 0, sizeof icons);
		(d->floppy ? floppy_icons : cdrom_icons)(icons + MEDIA_ICN, icons + DRIVE_ICN, icons + MEDIA_ICNS,
							  icons + DRIVE_ICNS);
		icons[DRIVE_WHERE] = (uint8_t)strlen(where);
		memcpy(icons + DRIVE_WHERE + 1, where, strlen(where));
		spec.extra = icons;
		spec.extra_len = sizeof icons;
	}
	if (d->floppy) {
		spec.device_name = "blue,floppy";
		spec.driver_name = ".BlueFloppy";
	} else if (d->cdrom) {
		spec.driver_name = ".AppleCD";  /* (programs open the CD-ROM driver by its name) */
	}
	return ndrv_build(&spec, out, size, &d->layout);
}

static bool drive_number_used(struct mem *m, uint16_t num)
{
	unsigned guard = 0;
	for (uint32_t q = mem_r32(m, LM_DRVQ_HDR + 2); q && guard < 256; q = mem_r32(m, q), guard++)
		if (mem_r16(m, q + 6) == num)
			return true;
	return false;
}

/* Puts each drive in the drive queue, as AddDrive does (Enqueue at the tail). */
static void add_drives(struct disk *d, uint32_t toc, uint16_t ref_num)
{
	struct mem *m = d->mem;
	uint16_t num = FIRST_DRIVE_NUMBER;

	d->ref_num = ref_num;
	for (unsigned k = 0; k < d->partitions; k++) {
		struct disk_partition *p = &d->part[k];
		uint32_t data = toc + d->layout.extra + DRIVE_DATA * k;
		uint32_t el = data + DRIVE_QUEUE_ELEMENT;
		if (p->drive_num)
			continue;               /* already queued (Replace after Initialize) */
		while (drive_number_used(m, num))
			num++;

		uint64_t blocks = p->size / 512;
		if (blocks > 0xFFFFFFFFu)
			blocks = 0xFFFFFFFFu;
		mem_w8(m, data + DRIVE_FLAGS + 0, d->read_only ? 0x80 : 0);    /* write protected */
		mem_w8(m, data + DRIVE_FLAGS + 1, 8);                           /* non-ejectable disk in place */
		mem_w8(m, data + DRIVE_FLAGS + 2, 1);                           /* installed */
		mem_w8(m, data + DRIVE_FLAGS + 3, 0);
		mem_w32(m, el + 0, 0);                  /* qLink */
		mem_w16(m, el + 4, 1);                  /* qType 1: dQDrvSz2 holds the high word */
		mem_w16(m, el + 6, num);
		mem_w16(m, el + 8, ref_num);
		mem_w16(m, el + 10, 0);                 /* HFS */
		mem_w16(m, el + 12, (uint16_t)blocks);
		mem_w16(m, el + 14, (uint16_t)(blocks >> 16));

		uint32_t tail = mem_r32(m, LM_DRVQ_HDR + 6);
		if (mem_r32(m, LM_DRVQ_HDR + 2) == 0 || !tail)
			mem_w32(m, LM_DRVQ_HDR + 2, el);
		else
			mem_w32(m, tail, el);
		mem_w32(m, LM_DRVQ_HDR + 6, el);
		p->drive_num = num;
	}
}

/* The partition a request is for, by the drive number in ioVRefNum. */
static int partition_of(const struct disk *d, uint32_t pb)
{
	int16_t drive = (int16_t)mem_r16(d->mem, pb + PB_VREF_NUM);
	for (unsigned k = 0; k < d->partitions; k++)
		if (d->part[k].drive_num == drive)
			return (int)k;
	return d->partitions == 1 ? 0 : -1;
}

static uint32_t dce_for(const struct disk *d, uint16_t ref_num)
{
	int16_t ref = (int16_t)ref_num;
	if (ref >= 0)
		return 0;
	uint32_t unit = (uint32_t)(-ref - 1);
	if (unit >= mem_r16(d->mem, LM_UNIT_COUNT))
		return 0;
	uint32_t handle = mem_r32(d->mem, mem_r32(d->mem, LM_UTABLE_BASE) + 4 * unit);
	return handle ? mem_r32(d->mem, handle) : 0;
}

static bool guest_range_ok(const struct mem *m, uint32_t addr, uint32_t len, unsigned need)
{
	if (len == 0)
		return true;
	if (addr + len - 1 < addr)
		return false;
	for (uint32_t a = addr & ~(MEM_PAGE_SIZE - 1); ; a += MEM_PAGE_SIZE) {
		if (!mem_ok(m, a, 1, need))
			return false;
		if (a >= ((addr + len - 1) & ~(MEM_PAGE_SIZE - 1)))
			return true;
	}
}

static bool in_place(const struct disk *d, unsigned k);

static int32_t transfer(struct disk *d, uint32_t pb, bool write)
{
	struct mem *m = d->mem;
	uint32_t buf = mem_r32(m, pb + PB_BUFFER);
	uint32_t count = mem_r32(m, pb + PB_REQ_COUNT);
	uint16_t mode = mem_r16(m, pb + PB_POS_MODE);
	uint32_t dce = dce_for(d, mem_r16(m, pb + PB_REF_NUM));
	int k = partition_of(d, pb);
	uint64_t pos;

	mem_w32(m, pb + PB_ACT_COUNT, 0);
	if (k < 0)
		return NO_SUCH_DRIVE_ERR;
	if (d->removable && !in_place(d, (unsigned)k))
		return OFFLINE_ERR;     /* (no disk in the drive, as far as Mac OS knows) */
	struct disk_partition *p = &d->part[k];
	if (mode & POS_MODE_WIDE)
		pos = mem_r64(m, pb + PB_POS_OFFSET);
	else if ((mode & 3) == FS_FROM_START || !dce)
		pos = mem_r32(m, pb + PB_POS_OFFSET);
	else    /* the Device Manager has already resolved the mark into dCtlPosition */
		pos = mem_r32(m, dce + DCE_POSITION);

	if (d->log)
		printf("%s: %s %u bytes at %llu on drive %u (mode %04X) -> %08X\n", d->floppy ? "floppy" : "disk",
		       write ? "write" : "read", count, (unsigned long long)pos, p->drive_num, mode, buf);
	if (pos <= MDB_OFFSET && pos + count > MDB_OFFSET)
		d->part[k].mounted = true;      /* MountVol reads the master directory block */
	if (write && d->read_only)
		return WRITE_PROTECTED_ERR;
	if (d->cd) {
		d->cd->last_read = (uint32_t)pos;
		d->cd->read_once = true;
	}
	if (pos > p->size || count > p->size - pos)
		return PARAM_ERR;
	if (!guest_range_ok(m, buf, count, write ? MEM_R : MEM_W))
		return PARAM_ERR;

	uint64_t at = p->offset + pos;
	if (!write)
		mem_invalidate(m, buf, count);  /* reads may load code over translated code */
	ssize_t n = write ? write_at(d, mem_ptr(m, buf), count, at) : read_at(d, mem_ptr(m, buf), count, at);
	if (n != (ssize_t)count) {
		fprintf(stderr, "%s: %s error at %llu\n", d->path, write ? "write" : "read", (unsigned long long)at);
		return IO_ERR;
	}

	if (write) {
		d->writes++;
		d->bytes_written += count;
		if (d->floppy)
			d->dirty = true;
	} else {
		d->reads++;
		d->bytes_read += count;
	}
	mem_w32(m, pb + PB_ACT_COUNT, count);
	if (!(mode & POS_MODE_WIDE))
		mem_w32(m, pb + PB_POS_OFFSET, (uint32_t)(pos + count));
	if (dce)
		mem_w32(m, dce + DCE_POSITION, (uint32_t)(pos + count));
	return NO_ERR;
}

static int32_t control(struct disk *d, uint32_t pb)
{
	switch (mem_r16(d->mem, pb + PB_CS_CODE)) {
	case 1:         /* KillIO */
	case 5:         /* Verify */
		return NO_ERR;
	case 6:         /* Format: nothing to do below the file system */
		return d->read_only ? WRITE_PROTECTED_ERR : NO_ERR;
	default:        /* Eject (drives are fixed), icons, drive info, ... */
		return CONTROL_ERR;
	}
}

static int32_t status(struct disk *d, uint32_t pb, uint32_t toc)
{
	struct mem *m = d->mem;
	uint32_t p = pb + PB_CS_PARAM;

	switch (mem_r16(m, pb + PB_CS_CODE)) {
	case 8: {       /* DriveStatus: DrvSts */
		int k = partition_of(d, pb);
		if (k < 0 || !d->part[k].drive_num)
			return k < 0 ? NO_SUCH_DRIVE_ERR : STATUS_ERR;
		uint32_t data = toc + d->layout.extra + DRIVE_DATA * (uint32_t)k;
		memset(mem_ptr(m, p), 0, 22);
		memcpy(mem_ptr(m, p + 2), mem_ptr(m, data + DRIVE_FLAGS), 4);
		memcpy(mem_ptr(m, p + 6), mem_ptr(m, data + DRIVE_QUEUE_ELEMENT), 12);
		return NO_ERR;
	}
	case 43: {      /* DriverGestalt: selector, then the response, inline */
		uint32_t sel = mem_r32(m, p);
		if (sel == os_type("vers")) {
			mem_w32(m, p + 4, DRIVER_VERSION);
		} else if (sel == os_type("devt")) {
			mem_w32(m, p + 4, os_type("disk"));
		} else if (sel == os_type("sync") || sel == os_type("wide")) {
			mem_w32(m, p + 4, 0x01000000u);         /* Boolean true */
		} else if (sel == os_type("vmop")) {
			/* kAllowVMNone: no virtual memory backing store on these drives
			 * (the kernel doesn't page; give the guest more RAM instead) */
			mem_w32(m, p + 4, 0);
		} else if (d->boot_gestalt && sel == os_type("nmrg")) {
			/*
			 * Mac OS 9.1's StartLib takes a drive for the one Open Firmware
			 * booted from when /chosen's bootpath leads to the node 'nmrg'
			 * points to, ends in the drive's unit address, and names the
			 * partition 'ofpt' gives. It knows units only from 'intf' and
			 * 'dvrf' for SCSI (the target ID), ATA, USB and FireWire, not a
			 * PCI card's; so the drives are a SCSI target whose ID is the
			 * node's PCI device number, the bootpath's last unit address.
			 */
			mem_w32(m, p + 4, toc + d->layout.extra + DATA_REG_ENTRY);     /* RegEntryID * */
		} else if (d->boot_gestalt && sel == os_type("intf")) {
			mem_w32(m, p + 4, os_type("scsi"));
		} else if (d->boot_gestalt && sel == os_type("dvrf")) {
			mem_w32(m, p + 4, d->unit << 8);        /* a DeviceIdent: bus 0, target, LUN 0 */
		} else if (d->boot_gestalt && sel == os_type("ofpt")) {
			int k = partition_of(d, pb);
			if (k < 0)
				return NO_SUCH_DRIVE_ERR;
			const struct disk_partition *part = &d->part[k];
			bool mapped = part->map_entry > 0;
			mem_w32(m, p + 4, mapped ? 2 : 4);      /* kOFBootSpecifiedPartition, kOFBootNotPartitioned */
			mem_w32(m, p + 8, mapped ? part->map_entry - 1 : 0);   /* (counted from 0) */
			mem_w32(m, p + 12, (uint32_t)(part->offset / 512));
			mem_w32(m, p + 16, (uint32_t)(part->size / 512));
		} else {
			return STATUS_ERR;
		}
		return NO_ERR;
	}
	default:
		return STATUS_ERR;
	}
}

static void show_media(struct disk *d, uint32_t data);
static int32_t floppy_control(struct disk *d, uint32_t pb, uint32_t toc);
static int32_t floppy_status(struct disk *d, uint32_t pb, uint32_t toc);
static int32_t cdrom_control(struct disk *d, uint32_t pb, uint32_t toc, uint64_t tb);
static int32_t cdrom_status(struct disk *d, uint32_t pb, uint32_t toc, uint64_t tb);

static int32_t driver_io(struct disk *d, uint32_t toc, uint32_t contents, uint32_t code, uint64_t tb)
{
	switch (code) {
	case NDRV_INITIALIZE:
	case NDRV_REPLACE:      /* DriverInitInfo: refNum, RegEntryID */
		if (d->boot_gestalt)
			memcpy(mem_ptr(d->mem, toc + d->layout.extra + DATA_REG_ENTRY), mem_ptr(d->mem, contents + 2), 16);
		add_drives(d, toc, mem_r16(d->mem, contents));
		if (d->removable)
			show_media(d, toc + d->layout.extra);
		return NO_ERR;
	case NDRV_FINALIZE:
	case NDRV_SUPERSEDED:
	case NDRV_OPEN:
	case NDRV_CLOSE:
	case NDRV_KILL_IO:
		return NO_ERR;
	case NDRV_READ:
		return transfer(d, contents, false);
	case NDRV_WRITE:
		return transfer(d, contents, true);
	case NDRV_CONTROL:
		return d->floppy ? floppy_control(d, contents, toc) :
		       d->cdrom ? cdrom_control(d, contents, toc, tb) : control(d, contents);
	case NDRV_STATUS:
		return d->floppy ? floppy_status(d, contents, toc) :
		       d->cdrom ? cdrom_status(d, contents, toc, tb) : status(d, contents, toc);
	default:
		return PARAM_ERR;
	}
}

static int32_t driver_io_logged(struct disk *d, uint32_t toc, uint32_t contents, uint32_t code, uint32_t kind,
				uint64_t tb)
{
	d->calls++;
	int32_t err = driver_io(d, toc, contents, code, tb);
	if (d->log && code != NDRV_READ && code != NDRV_WRITE) {
		static const char *const names[] = { "open", "close", "read", "write", "control", "status",
						     "kill", "initialize", "finalize", "replace", "superseded" };
		printf("%s: %s", d->floppy ? "floppy" : "disk", code < 11 ? names[code] : "?");
		if (code == NDRV_CONTROL || code == NDRV_STATUS)
			printf(" %u (drive %d)", mem_r16(d->mem, contents + PB_CS_CODE),
			       (int16_t)mem_r16(d->mem, contents + PB_VREF_NUM));
		if (code == NDRV_STATUS && mem_r16(d->mem, contents + PB_CS_CODE) == 43)
			printf(" '%.4s'", (const char *)mem_ptr(d->mem, contents + PB_CS_PARAM));
		printf(" (kind %u) -> %d\n", kind, err);
	} else if (d->log && err) {
		printf("%s: -> %d\n", d->floppy ? "floppy" : "disk", err);
	}
	return err;
}

/*
 * Disk-inserted events: disk drivers post one per drive so that the Event
 * Manager mounts the volume when an application next asks for events. Ours
 * is loaded before events are enabled, so a timer (once a second) posts for
 * every drive whose volume hasn't been mounted yet, until it is.
 */
#define POST_RETRY_SECONDS 10           /* after a post the system accepted */

static void arm_timer(struct disk *d, struct ndrv_call *c, uint32_t timebase_hz, uint32_t step)
{
	uint32_t data = c->toc + d->layout.extra;
	mem_w64(d->mem, data + DATA_EXPIRATION, c->tb + timebase_hz);
	ndrv_set_state(c, 0, step);
	ndrv_callout(c, ndrv_import(c, &d->layout, IMP_SET_TIMER), data + DATA_EXPIRATION,
		     c->toc + d->layout.entry[1], 0, data + DATA_TIMER);
}

static bool all_mounted(const struct disk *d)
{
	if (d->removable)
		return false;           /* (its timer goes on: discs come and go) */
	for (unsigned k = 0; k < d->partitions; k++)
		if (d->part[k].drive_num && !d->part[k].mounted)
			return false;
	return true;
}

static bool volume_on(const struct disk *d, unsigned k);

/* Posts the event for the next drive (from state word 1) that needs one; false when none is left. */
static bool post_next_drive(struct disk *d, struct ndrv_call *c)
{
	unsigned k = ndrv_state(c, 1);
	for (unsigned j = k; d->cdrom && j < d->partitions; j++)
		if (!d->part[j].mounted && volume_on(d, j))
			d->part[j].mounted = true;      /* (other file systems than HFS: ISO 9660, Audio CD) */
	while (k < d->partitions && (!d->part[k].drive_num || d->part[k].mounted || c->tb < d->part[k].next_post ||
				     (d->removable && !in_place(d, k))))
		k++;
	if (k >= d->partitions)
		return false;
	ndrv_set_state(c, 0, STEP_POSTING);
	ndrv_set_state(c, 1, k + 1);
	ndrv_callout(c, ndrv_import(c, &d->layout, IMP_POST_EVENT), DISK_INSERTED_EVENT, d->part[k].drive_num, 0, 0);
	return true;
}

static void media_poll(struct disk *d, struct ndrv_call *c, uint32_t timebase_hz);

void disk_driver_call(struct disk *d, struct ndrv_call *c, uint32_t timebase_hz)
{
	switch (c->entry) {
	case 0: {                       /* DoDriverIO(spaceID, cmdID, contents, code, kind) */
		uint32_t code = c->arg[3];
		if (d->cdrom && !d->cd && (d->cd = cdrom_new(timebase_hz, d->unit << 8))) {
			d->cd->log = d->log;
			cdrom_disc(d->cd, d->disc);
		}
		int32_t status = driver_io_logged(d, c->toc, c->arg[2], code, c->arg[4], c->tb);
		if (code == NDRV_WRITE)
			d->written_tb = c->tb;
		if (code == NDRV_INITIALIZE && status == 0) {
			ndrv_set_state(c, 2, c->arg[1]);
			ndrv_set_state(c, 3, c->arg[4]);
			arm_timer(d, c, timebase_hz, STEP_ARMING);
			return;
		}
		ndrv_complete(c, &d->layout, IMP_IO_COMPLETE, c->arg[1], c->arg[4], status);
		return;
	}
	case 1:                         /* timer (secondary interrupt handler) */
		if (d->removable)
			media_poll(d, c, timebase_hz);
		ndrv_set_state(c, 1, 0);
		if (!post_next_drive(d, c)) {
			if (all_mounted(d))
				ndrv_finish(c, 0);
			else
				arm_timer(d, c, timebase_hz, STEP_REARMING);
		}
		return;
	}

	switch (ndrv_state(c, 0)) {     /* resuming after a callout */
	case STEP_ARMING:
		ndrv_complete(c, &d->layout, IMP_IO_COMPLETE, ndrv_state(c, 2), ndrv_state(c, 3), 0);
		return;
	case STEP_POSTING: {
		unsigned k = ndrv_state(c, 1) - 1;
		if (c->arg[0] == 0)     /* accepted: give the system time to mount it */
			d->part[k].next_post = c->tb + (uint64_t)POST_RETRY_SECONDS * timebase_hz;
		if (d->log)
			printf("%s: disk-inserted event for drive %u -> %d\n", d->floppy ? "floppy" : "disk",
			       d->part[k].drive_num, (int16_t)c->arg[0]);
		if (!post_next_drive(d, c))
			arm_timer(d, c, timebase_hz, STEP_REARMING);
		return;
	}
	case STEP_REARMING:
		ndrv_finish(c, 0);
		return;
	default:                        /* IOCommandIsComplete returned */
		ndrv_finish(c, c->arg[0]);
		return;
	}
}

/* ---- removable media: the floppy drive and CD-ROM drives --------------------- */

#define LM_VCB_QHDR 0x0356u             /* volume queue header: flags, head, tail */
#define VCB_DRV_NUM 72                  /* (0 while the volume is off line) */
#define LM_CUR_AP_NAME 0x0910u          /* the current application's name: empty until one runs */

#define FORMAT_TSS 0x80u                /* format list: tracks, sides and sectors are given */
#define FORMAT_CURRENT 0x40u            /* ... the disk in the drive has this format */
#define FORMAT_HIGH_DENSITY 0x10u
/*
 * DriverGestalt 'ejec': a CD stays in at a restart and at shutdown, as on
 * an iMac. Both bits: 8.5's, 8.6's and 9.1's ShutDown Managers test the
 * first at a restart (as Apple's headers name them), 9.2.2's (its ROM's
 * and its System's) the second. The floppy drive doesn't answer, so Mac OS
 * ejects its disk then, as from a Mac's floppy drive.
 */
#define RESTART_DONT_EJECT 1u
#define SHUT_DOWN_DONT_EJECT 2u
#define MEDIA_CDROM 129                 /* DriverGestalt 'minf' */
#define MEDIA_OVERWRITABLE 136
#define MEDIA_NONE (-1)
#define DRIVE_INFO_SUPERDRIVE 4u        /* Control 23: an internal 400K/800K/720K/1440K drive */
#define DRIVE_INFO_CDROM 0x0B01u        /* ... "unspecified", "external", "SCSI", removable, secondary */

static const char *drive_kind(const struct disk *d)
{
	return d->floppy ? "floppy" : "cdrom";
}

void disk_open_floppy(struct disk *d, struct mem *mem)
{
	memset(d, 0, sizeof *d);
	d->mem = mem;
	d->fd = -1;
	d->path = d->media_path;
	d->removable = d->floppy = true;
	d->partitions = 1;
}

/*
 * An image for a CD-ROM drive, opened read-only: its partitions and its disc
 * (in a disk struct of its own), or -1 and why. A cue sheet's data comes
 * through its disc (`backend` set: take_image() points it at the drive's).
 */
static int cd_image(const char *path, struct disk *img, char *why, size_t len)
{
	struct stat st;
	memset(img, 0, sizeof *img);
	img->fd = -1;
	if (cdimage_is_cue(path)) {
		const struct cdimage *c = img->disc = cdimage_open(path, why, len);
		if (!c)
			return -1;
		img->size = c->data >= 0 ? c->data_size & ~(uint64_t)511 : (uint64_t)c->leadout * CD_DATA;
		img->disc_data = (struct disk_backend){ path, img->disc, img->size, cdimage_read_data, NULL };
		img->backend = &img->disc_data;
		if (c->data >= 0)
			read_partition_map(img);
		if (!img->partitions)
			add_partition(img, 0, img->size, "", 0);
		return 0;
	}
	img->fd = open(path, O_RDONLY);
	if (img->fd < 0) {
		snprintf(why, len, "%s", strerror(errno));
		return -1;
	}
	const char *other = floppy_other_image(img->fd);
	if (fstat(img->fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 2048 || other) {
		if (other && strstr(other, "2352"))
			snprintf(why, len, "%s: give Blue its .cue sheet, which says where its tracks are", other);
		else if (other)
			snprintf(why, len, "%s: CD-ROM drives take raw images (.iso, .toast, .cdr, .img; a UDIF .dmg "
				 "converts to one with hdiutil convert -format UDTO) and .cue sheets", other);
		else
			snprintf(why, len, "not a disc image");
		close(img->fd);
		img->fd = -1;
		return -1;
	}
	img->size = (uint64_t)st.st_size & ~(uint64_t)511;
	read_partition_map(img);
	if (!img->partitions)
		add_partition(img, 0, img->size, "", 0);
	if (!(img->disc = cdimage_open(path, why, len))) {      /* (its table of contents: one data track) */
		close(img->fd);
		img->fd = -1;
		return -1;
	}
	return 0;
}

/* The image (from cd_image()) goes in the drive: its file or its disc's data, and its table of contents */
static void take_image(struct disk *d, struct disk *img)
{
	d->fd = img->fd;
	d->size = img->size;
	d->disc = img->disc;
	d->disc_data = img->disc_data;
	d->backend = img->backend ? &d->disc_data : NULL;
	if (d->cd)
		cdrom_disc(d->cd, d->disc);
}

int disk_check_cd(const char *path, char *why, size_t len)
{
	struct disk img;
	if (cd_image(path, &img, why, len))
		return -1;
	if (img.fd >= 0)
		close(img.fd);
	cdimage_close(img.disc);
	return 0;
}

int disk_open_cdrom(struct disk *d, struct mem *mem, const char *path)
{
	char why[400];
	struct disk img;
	memset(d, 0, sizeof *d);
	d->mem = mem;
	d->fd = -1;
	d->path = d->media_path;
	d->removable = d->cdrom = d->read_only = true;
	d->partitions = 1;
	if (!path)
		return 0;
	if (cd_image(path, &img, why, sizeof why)) {
		fprintf(stderr, "%s: %s\n", path, why);
		return -1;
	}
	d->partitions = img.partitions;         /* (a drive for each of them, from now on) */
	if (img.fd >= 0)
		close(img.fd);
	cdimage_close(img.disc);
	if (disk_insert(d, path, why, sizeof why)) {
		fprintf(stderr, "%s: %s\n", path, why);
		return -1;
	}
	d->shown = true;                        /* (in at startup: a startup disk it may be) */
	d->shown_gen = d->media_gen;
	return 0;
}

int disk_insert(struct disk *d, const char *path, char *why, size_t len)
{
	if (!d->floppy) {
		struct disk img;
		if (cd_image(path, &img, why, len))
			return -1;
		disk_eject(d);
		take_image(d, &img);
		d->media_parts = img.partitions < d->partitions ? img.partitions : d->partitions;
		for (unsigned k = 0; k < d->partitions; k++) {
			struct disk_partition *p = &d->part[k], *q = &img.part[k];
			p->offset = k < d->media_parts ? q->offset : 0;
			p->size = k < d->media_parts ? q->size : 0;
			p->map_entry = k < d->media_parts ? q->map_entry : 0;
			snprintf(p->name, sizeof p->name, "%s", k < d->media_parts ? q->name : "");
		}
		if (img.partitions > d->partitions)
			fprintf(stderr, "%s: %u of its %u partitions go in (the drive has as many as its disc at "
				"startup had)\n", path, d->partitions, img.partitions);
		snprintf(d->media_path, sizeof d->media_path, "%s", path);
		d->media = true;
		d->media_gen++;
		return 0;
	}
	bool read_only = false;
	int fd = open(path, O_RDWR);
	if (fd < 0 && (errno == EACCES || errno == EROFS || errno == EPERM)) {
		read_only = true;       /* (EPERM: locked in the Finder) */
		fd = open(path, O_RDONLY);
	}
	if (fd < 0) {
		snprintf(why, len, "%s", strerror(errno));
		return -1;
	}
	struct floppy_image image;
	if (floppy_probe(fd, &image, why, len)) {
		close(fd);
		return -1;
	}
	disk_eject(d);
	d->fd = fd;
	d->read_only = read_only;
	d->image = image;
	d->size = image.data_size;
	d->part[0].offset = image.data_offset;
	d->part[0].size = image.data_size;
	d->media_parts = 1;
	snprintf(d->media_path, sizeof d->media_path, "%s", path);
	d->media = true;
	d->media_gen++;
	return 0;
}

void disk_flush(struct disk *d)
{
	if (d->fd < 0)
		return;
	if (d->dirty && d->image.kind == FLOPPY_DC42 && floppy_dc42_update(d->fd, &d->image))
		fprintf(stderr, "%s: cannot update its Disk Copy checksums\n", d->path);
	d->dirty = false;
	if (!d->read_only)
		fsync(d->fd);
}

void disk_eject(struct disk *d)
{
	if (!d->media)
		return;
	disk_flush(d);
	if (d->fd >= 0)
		close(d->fd);
	d->fd = -1;
	d->media = false;
	if (d->cd)
		cdrom_disc(d->cd, NULL);
	cdimage_close(d->disc);
	d->disc = NULL;
	d->backend = NULL;
}

/* Whether Mac OS has a volume mounted on drive k of the disk's (k past them: on any) */
static bool volume_on(const struct disk *d, unsigned k)
{
	const struct mem *m = d->mem;
	unsigned guard = 0;
	if (!mem_ok(m, LM_VCB_QHDR, 10, MEM_R))
		return false;
	for (uint32_t q = mem_r32(m, LM_VCB_QHDR + 2); q && guard < 256; q = mem_r32(m, q), guard++) {
		if (!mem_ok(m, q, VCB_DRV_NUM + 2, MEM_R))
			break;
		uint16_t drive = mem_r16(m, q + VCB_DRV_NUM);
		for (unsigned j = 0; j < d->partitions; j++)
			if (drive && drive == d->part[j].drive_num && (j == k || k >= d->partitions))
				return true;
	}
	return false;
}

bool disk_in_use(const struct disk *d)
{
	return volume_on(d, d->partitions);
}

/* Whether Mac OS knows of the disc in the drive (and it's still the one it knows of) */
static bool media_shown(const struct disk *d)
{
	return d->media && d->shown && d->shown_gen == d->media_gen;
}

/* ... and that drive k has one of its volumes (it may have fewer partitions, or Mac OS ejected that one) */
static bool in_place(const struct disk *d, unsigned k)
{
	return media_shown(d) && k < d->media_parts && !d->part[k].ejected && d->part[k].size;
}

/* The drives' flags and sizes as Mac OS sees them: a disc in it (ejectable; locked or not), or none */
static void show_media(struct disk *d, uint32_t data)
{
	struct mem *m = d->mem;
	for (unsigned k = 0; k < d->partitions; k++) {
		uint32_t at = data + DRIVE_DATA * k;
		bool in = in_place(d, k);
		uint64_t blocks = in ? d->part[k].size / 512 : 0;
		if (blocks > 0xFFFFFFFFu)
			blocks = 0xFFFFFFFFu;
		mem_w8(m, at + DRIVE_FLAGS + 0, in && d->read_only ? 0x80 : 0);
		mem_w8(m, at + DRIVE_FLAGS + 1, in ? 1 : 0);
		mem_w8(m, at + DRIVE_FLAGS + 3, d->floppy ? 0xFF : 0);         /* (two-sided) */
		mem_w16(m, at + DRIVE_QUEUE_ELEMENT + 12, (uint16_t)blocks);
		mem_w16(m, at + DRIVE_QUEUE_ELEMENT + 14, (uint16_t)(blocks >> 16));
	}
}

/*
 * The timer's look at the drive: Mac OS learns that a disc went in (once
 * applications run: before, it could become the startup disk) or came out,
 * and the timer then posts disk-inserted events. What was written is
 * flushed once writes have stopped for a second.
 */
static void media_poll(struct disk *d, struct ndrv_call *c, uint32_t timebase_hz)
{
	/* (until an application runs, the name holds whatever startup left there, 0xFF bytes with 8.5) */
	unsigned app = mem_ok(d->mem, LM_CUR_AP_NAME, 1, MEM_R) ? mem_r8(d->mem, LM_CUR_AP_NAME) : 0;
	bool ready = app >= 1 && app <= 31;
	if (d->shown && (!d->media || d->shown_gen != d->media_gen)) {
		d->shown = false;
		show_media(d, c->toc + d->layout.extra);
		if (d->log)
			printf("%s: drive %u is empty\n", drive_kind(d), d->part[0].drive_num);
	} else if (!d->shown && d->media && ready) {
		d->shown = true;
		d->shown_gen = d->media_gen;
		for (unsigned k = 0; k < d->partitions; k++) {
			d->part[k].mounted = d->part[k].ejected = false;
			d->part[k].next_post = 0;
		}
		show_media(d, c->toc + d->layout.extra);
		if (d->log)
			printf("%s: drive %u has a disc in it (the current application: \"%.*s\")\n", drive_kind(d),
			       d->part[0].drive_num, (int)app, (const char *)mem_ptr(d->mem, LM_CUR_AP_NAME + 1));
	}
	if (d->dirty && c->tb - d->written_tb >= timebase_hz)
		disk_flush(d);
}

/*
 * Mac OS ejects a drive (Control 7), after taking its volume off line or
 * unmounting it: the disc comes out once none of its volumes is in use
 * (a CD with several partitions: the last of them), and the drives are
 * empty.
 */
static int32_t eject(struct disk *d, uint32_t pb, uint32_t data)
{
	int k = partition_of(d, pb);
	if (k < 0)
		return NO_SUCH_DRIVE_ERR;
	if (in_place(d, (unsigned)k)) {
		d->part[k].ejected = true;
		if (!disk_in_use(d)) {
			printf("%s: Mac OS ejected %s\n", drive_kind(d), d->path);
			disk_eject(d);
			d->ejects++;
			d->shown = false;
		}
	}
	show_media(d, data);
	return NO_ERR;
}

/* The disc in a CD-ROM drive, as far as Mac OS knows (NULL: none) */
static const struct cdimage *disc_in(const struct disk *d)
{
	return !d->removable || media_shown(d) ? d->disc : NULL;
}

/* A CD-ROM drive: Apple's CD-ROM driver's calls (cdrom.c), its icons, ejecting its disc, the rest as the disks */
static int32_t cdrom_control(struct disk *d, uint32_t pb, uint32_t toc, uint64_t tb)
{
	uint32_t data = toc + d->layout.extra, p = pb + PB_CS_PARAM;
	int32_t err;
	switch (mem_r16(d->mem, pb + PB_CS_CODE)) {
	case 7:         /* Eject: a disc that can be swapped */
		return d->removable ? eject(d, pb, data) : CONTROL_ERR;
	case 21:        /* the drive's icon, and where it is (for Get Info) */
		mem_w32(d->mem, p, data + DRIVE_ICN);
		return NO_ERR;
	case 22:        /* the disc's icon */
		mem_w32(d->mem, p, data + MEDIA_ICN);
		return NO_ERR;
	case 23:        /* drive info, as Apple's CD-ROM driver gives it */
		mem_w32(d->mem, p, DRIVE_INFO_CDROM);
		return NO_ERR;
	}
	if (d->cd && cdrom_control_call(d->cd, disc_in(d), d->mem, pb, tb, &err))
		return err;
	return control(d, pb);
}

static int32_t cdrom_status(struct disk *d, uint32_t pb, uint32_t toc, uint64_t tb)
{
	struct mem *m = d->mem;
	uint32_t data = toc + d->layout.extra, p = pb + PB_CS_PARAM;
	int32_t err;
	if (mem_r16(m, pb + PB_CS_CODE) == 43) {        /* DriverGestalt: as a CD-ROM drive */
		uint32_t sel = mem_r32(m, p);
		if (sel == os_type("devt")) {
			mem_w32(m, p + 4, os_type("cdrm"));
		} else if (sel == os_type("intf")) {
			mem_w32(m, p + 4, os_type("scsi"));     /* (as with boot_gestalt: see status()) */
		} else if (sel == os_type("dvrf")) {
			mem_w32(m, p + 4, d->unit << 8);
		} else if (sel == os_type("ejec")) {
			mem_w32(m, p + 4, RESTART_DONT_EJECT | SHUT_DOWN_DONT_EJECT);
		} else if (sel == os_type("mics")) {
			mem_w32(m, p + 4, data + MEDIA_ICNS);
		} else if (sel == os_type("dics")) {
			mem_w32(m, p + 4, data + DRIVE_ICNS);
		} else if (sel == os_type("mnam")) {
			mem_w32(m, p + 4, data + DRIVE_WHERE);
		} else if (sel == os_type("minf")) {   /* the disc's size (512-byte blocks), and that it's a CD */
			int k = partition_of(d, pb);
			bool in = k >= 0 && disc_in(d) && (!d->removable || in_place(d, (unsigned)k));
			mem_w32(m, p + 4, in ? (uint32_t)(d->part[k].size / 512) : 0);
			mem_w32(m, p + 8, 512);
			mem_w16(m, p + 12, (uint16_t)(in ? MEDIA_CDROM : MEDIA_NONE));
		} else {
			return status(d, pb, toc);
		}
		return NO_ERR;
	}
	if (mem_r16(m, pb + PB_CS_CODE) == 8) {         /* DriveStatus: the disc in place is 1, as Apple's driver has it */
		int32_t err = status(d, pb, toc);
		int k = partition_of(d, pb);
		if (!err)
			mem_w8(m, p + 3, k >= 0 && disc_in(d) && (!d->removable || in_place(d, (unsigned)k)) ? 1 : 0);
		return err;
	}
	if (d->cd && cdrom_status_call(d->cd, disc_in(d), m, pb, tb, &err))
		return err;
	return status(d, pb, toc);
}

static int32_t floppy_control(struct disk *d, uint32_t pb, uint32_t toc)
{
	struct mem *m = d->mem;
	uint32_t data = toc + d->layout.extra, p = pb + PB_CS_PARAM;
	bool in = media_shown(d);

	switch (mem_r16(m, pb + PB_CS_CODE)) {
	case 1:         /* KillIO */
	case 8:         /* tag buffer (tags aren't kept) */
	case 9:         /* track cache */
		return NO_ERR;
	case 5:         /* Verify */
		return in ? NO_ERR : OFFLINE_ERR;
	case 6:         /* Format: the disk is erased (Mac OS then writes a new volume on it) */
		if (!in)
			return OFFLINE_ERR;
		if (d->read_only)
			return WRITE_PROTECTED_ERR;
		if (floppy_erase(d->fd, &d->image)) {
			fprintf(stderr, "%s: cannot erase it\n", d->path);
			return IO_ERR;
		}
		d->dirty = true;
		d->writes++;
		d->bytes_written += d->image.data_size;
		printf("floppy: Mac OS formatted %s\n", d->path);
		return NO_ERR;
	case 7:         /* Eject */
		return eject(d, pb, data);
	case 21:        /* the drive's icon, and where it is (for Get Info) */
		mem_w32(m, p, data + DRIVE_ICN);
		return NO_ERR;
	case 22:        /* the disk's icon */
		mem_w32(m, p, data + MEDIA_ICN);
		return NO_ERR;
	case 23:        /* drive info */
		mem_w32(m, p, DRIVE_INFO_SUPERDRIVE);
		return NO_ERR;
	case 43:        /* DriverConfigure */
		if (mem_r32(m, p) != os_type("flus"))
			return CONTROL_ERR;
		disk_flush(d);
		return NO_ERR;
	default:
		return CONTROL_ERR;
	}
}

/* The format list's entry for a disk of this many blocks: flags, sectors per track, tracks */
static void disk_format(uint32_t blocks, uint8_t *flags, uint8_t *sectors, uint16_t *tracks)
{
	static const struct { uint32_t blocks; uint8_t flags, sectors; } formats[] = {
		{ 800, FORMAT_TSS | 1, 0 },                             /* 400K GCR (sectors vary by zone) */
		{ 1600, FORMAT_TSS | 2, 0 },                            /* 800K GCR */
		{ 1440, FORMAT_TSS | 2, 9 },                            /* 720K MFM */
		{ 2880, FORMAT_TSS | FORMAT_HIGH_DENSITY | 2, 18 },     /* 1440K MFM */
		{ 5760, FORMAT_TSS | FORMAT_HIGH_DENSITY | 2, 36 },     /* 2880K MFM */
	};
	*flags = FORMAT_CURRENT;
	*sectors = 0;
	*tracks = 0;
	for (size_t k = 0; k < sizeof formats / sizeof formats[0]; k++)
		if (formats[k].blocks == blocks) {
			*flags |= formats[k].flags;
			*sectors = formats[k].sectors;
			*tracks = 80;
		}
}

static int32_t floppy_status(struct disk *d, uint32_t pb, uint32_t toc)
{
	struct mem *m = d->mem;
	uint32_t data = toc + d->layout.extra, p = pb + PB_CS_PARAM;
	bool in = media_shown(d);
	uint32_t blocks = in ? (uint32_t)(d->part[0].size / 512) : 0;

	switch (mem_r16(m, pb + PB_CS_CODE)) {
	case 6: {       /* format list: max entries, the list; just the disk's own format */
		uint32_t list = mem_r32(m, p + 2);
		uint8_t flags, sectors;
		uint16_t tracks;
		if (!in)
			return OFFLINE_ERR;
		if (mem_r16(m, p) < 1 || !guest_range_ok(m, list, 8, MEM_W))
			return PARAM_ERR;
		disk_format(blocks, &flags, &sectors, &tracks);
		mem_w32(m, list, blocks);
		mem_w8(m, list + 4, flags);
		mem_w8(m, list + 5, sectors);
		mem_w16(m, list + 6, tracks);
		mem_w16(m, p, 1);
		return NO_ERR;
	}
	case 43: {      /* DriverGestalt: as a floppy drive (other selectors as the disks) */
		uint32_t sel = mem_r32(m, p);
		if (sel == os_type("devt")) {
			mem_w32(m, p + 4, os_type("flop"));
		} else if (sel == os_type("minf")) {
			mem_w32(m, p + 4, blocks);
			mem_w32(m, p + 8, 512);
			mem_w16(m, p + 12, (uint16_t)(in ? MEDIA_OVERWRITABLE : MEDIA_NONE));
		} else if (sel == os_type("mics")) {
			mem_w32(m, p + 4, data + MEDIA_ICNS);
		} else if (sel == os_type("dics")) {
			mem_w32(m, p + 4, data + DRIVE_ICNS);
		} else if (sel == os_type("mnam")) {
			mem_w32(m, p + 4, data + DRIVE_WHERE);
		} else if (sel == os_type("flus")) {
			mem_w8(m, p + 4, 1);                    /* can flush (DriverConfigure 'flus') */
			mem_w8(m, p + 5, d->dirty);             /* ... and has something to */
			mem_w16(m, p + 6, 0);
		} else {
			return status(d, pb, toc);
		}
		return NO_ERR;
	}
	default:
		return status(d, pb, toc);
	}
}
