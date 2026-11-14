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
	if (pread(d->fd, b, sizeof b, 0) != (ssize_t)sizeof b || be16(b) != 0x4552)
		return;
	unsigned bs = be16(b + 2);
	if (bs != 512 && bs != 1024 && bs != 2048 && bs != 4096)
		bs = 512;
	if (pread(d->fd, b, sizeof b, 512) == (ssize_t)sizeof b && be16(b) == 0x504D)
		bs = 512;
	unsigned count = 1;
	for (unsigned k = 1; k <= count && k <= 64; k++) {
		if (pread(d->fd, b, sizeof b, (off_t)k * bs) != (ssize_t)sizeof b || be16(b) != 0x504D)
			break;
		count = be32(b + 4);
		char type[33] = { 0 }, name[33] = { 0 };
		memcpy(type, b + 48, 32);
		memcpy(name, b + 16, 32);
		if (strcmp(type, "Apple_HFS") == 0)
			add_partition(d, (uint64_t)be32(b + 8) * bs, (uint64_t)be32(b + 12) * bs, name, k);
	}
}

int disk_open(struct disk *d, struct mem *mem, const char *path, bool read_only, bool cdrom)
{
	memset(d, 0, sizeof *d);
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

bool disk_bootable(const struct disk *d, unsigned part)
{
	uint8_t sig[2];
	return part < d->partitions && read_at(d, sig, 2, d->part[part].offset) == 2 && be16(sig) == 0x4C4B;
}

void disk_close(struct disk *d)
{
	if (d->fd >= 0) {
		if (!d->read_only)
			fsync(d->fd);
		close(d->fd);
	}
	d->fd = -1;
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
		.device_name = "syshm,disk",
		.driver_name = ".syshmDisk",
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
	struct disk_partition *p = &d->part[k];
	if (mode & POS_MODE_WIDE)
		pos = mem_r64(m, pb + PB_POS_OFFSET);
	else if ((mode & 3) == FS_FROM_START || !dce)
		pos = mem_r32(m, pb + PB_POS_OFFSET);
	else    /* the Device Manager has already resolved the mark into dCtlPosition */
		pos = mem_r32(m, dce + DCE_POSITION);

	if (d->log)
		printf("disk: %s %u bytes at %llu on drive %u (mode %04X) -> %08X\n", write ? "write" : "read",
		       count, (unsigned long long)pos, p->drive_num, mode, buf);
	if (pos <= MDB_OFFSET && pos + count > MDB_OFFSET)
		d->part[k].mounted = true;      /* MountVol reads the master directory block */
	if (write && d->read_only)
		return WRITE_PROTECTED_ERR;
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

static int32_t driver_io(struct disk *d, uint32_t toc, uint32_t contents, uint32_t code)
{
	switch (code) {
	case NDRV_INITIALIZE:
	case NDRV_REPLACE:      /* DriverInitInfo: refNum, RegEntryID */
		if (d->boot_gestalt)
			memcpy(mem_ptr(d->mem, toc + d->layout.extra + DATA_REG_ENTRY), mem_ptr(d->mem, contents + 2), 16);
		add_drives(d, toc, mem_r16(d->mem, contents));
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
		return control(d, contents);
	case NDRV_STATUS:
		return status(d, contents, toc);
	default:
		return PARAM_ERR;
	}
}

static int32_t driver_io_logged(struct disk *d, uint32_t toc, uint32_t contents, uint32_t code, uint32_t kind)
{
	d->calls++;
	int32_t err = driver_io(d, toc, contents, code);
	if (d->log && code != NDRV_READ && code != NDRV_WRITE) {
		static const char *const names[] = { "open", "close", "read", "write", "control", "status",
						     "kill", "initialize", "finalize", "replace", "superseded" };
		printf("disk: %s", code < 11 ? names[code] : "?");
		if (code == NDRV_CONTROL || code == NDRV_STATUS)
			printf(" %u (drive %d)", mem_r16(d->mem, contents + PB_CS_CODE),
			       (int16_t)mem_r16(d->mem, contents + PB_VREF_NUM));
		if (code == NDRV_STATUS && mem_r16(d->mem, contents + PB_CS_CODE) == 43)
			printf(" '%.4s'", (const char *)mem_ptr(d->mem, contents + PB_CS_PARAM));
		printf(" (kind %u) -> %d\n", kind, err);
	} else if (d->log && err) {
		printf("disk: -> %d\n", err);
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
	for (unsigned k = 0; k < d->partitions; k++)
		if (d->part[k].drive_num && !d->part[k].mounted)
			return false;
	return true;
}

/* Posts the event for the next drive (from state word 1) that needs one; false when none is left. */
static bool post_next_drive(struct disk *d, struct ndrv_call *c)
{
	unsigned k = ndrv_state(c, 1);
	while (k < d->partitions && (!d->part[k].drive_num || d->part[k].mounted || c->tb < d->part[k].next_post))
		k++;
	if (k >= d->partitions)
		return false;
	ndrv_set_state(c, 0, STEP_POSTING);
	ndrv_set_state(c, 1, k + 1);
	ndrv_callout(c, ndrv_import(c, &d->layout, IMP_POST_EVENT), DISK_INSERTED_EVENT, d->part[k].drive_num, 0, 0);
	return true;
}

void disk_driver_call(struct disk *d, struct ndrv_call *c, uint32_t timebase_hz)
{
	switch (c->entry) {
	case 0: {                       /* DoDriverIO(spaceID, cmdID, contents, code, kind) */
		uint32_t code = c->arg[3];
		int32_t status = driver_io_logged(d, c->toc, c->arg[2], code, c->arg[4]);
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
			printf("disk: disk-inserted event for drive %u -> %d\n", d->part[k].drive_num,
			       (int16_t)c->arg[0]);
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
