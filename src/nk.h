/*
 * NanoKernel v1 interface definitions: the ConfigInfo record the ROM supplies,
 * and the kernel/emulator data the native kernel must provide in its place.
 * Offsets follow Apple's PPCInfoRecordsPriv.h (Darwin, APSL) and the
 * reverse-engineered NanoKernel sources (github.com/elliotnunn/NanoKernel).
 */
#ifndef BLUE_NK_H
#define BLUE_NK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "disk.h"
#include "mem.h"
#include "ppc.h"
#include "rom.h"

struct share;

/* ConfigInfo lives at a fixed offset in the 4 MB Toolbox image. */
#define NK_CONFIGINFO_OFFSET 0x30D000u

struct nk_config {
	int32_t mac68k_rom_offset;          /* offsets are relative to ConfigInfo */
	uint32_t mac68k_rom_size;
	int32_t emulator_code_offset;
	uint32_t emulator_code_size;
	int32_t opcode_table_offset;
	uint32_t opcode_table_size;
	char bootstrap_version[17];
	uint32_t boot_version_offset;       /* offsets within EmulatorData */
	uint32_t ecb_offset;
	uint32_t ipl_value_offset;
	uint32_t emulator_entry_offset;     /* offsets within emulator code */
	uint32_t kernel_trap_table_offset;
	uint32_t la_interrupt_ctl;
	uint32_t la_info_record;
	uint32_t la_kernel_data;
	uint32_t la_emulator_data;
	uint32_t la_dispatch_table;
	uint32_t la_emulator_code;
	uint32_t mac_lowmem_init_offset;
	uint32_t page_map_init_size;
	uint32_t page_map_init_offset;
	uint32_t page_map_irp_offset;
	uint32_t page_map_kdp_offset;
	uint32_t page_map_edp_offset;
	uint32_t segmap_usr[16][2];         /* PageMap offset, segment register */
	uint32_t bat_range[16][2];          /* BATU, BATL templates */
	uint32_t batmap_usr;                /* eight 4-bit BAT range indices */
	uint32_t pa_relocated_lowmem;
	uint32_t la_open_firmware;
	uint32_t la_hardware_priv;
};

/* One logical range of the user address space described by ConfigInfo. */
enum nk_region_kind {
	NK_REGION_ROM,          /* backed by the Toolbox image at rom_offset */
	NK_REGION_INFO_RECORD,  /* InfoRecord page (alias of KernelData) */
	NK_REGION_KERNEL_DATA,
	NK_REGION_EMULATOR_DATA,
	NK_REGION_IO,           /* cache-inhibited physical I/O */
	NK_REGION_PHYS,         /* other physical memory */
};

struct nk_region {
	enum nk_region_kind kind;
	uint32_t logical, size;
	uint32_t rom_offset;    /* NK_REGION_ROM */
	uint32_t physical;      /* NK_REGION_IO / NK_REGION_PHYS */
	bool writable;
	bool quiet;             /* read-only, and stores are dropped (write-through and PP 3: the ROM) */
	const char *source;     /* "BAT" or "PMDT" */
};

#define NK_MAX_REGIONS 64

int nk_config_parse(const struct rom *rom, struct nk_config *ci, char *err, size_t errlen);
int nk_user_regions(const struct rom *rom, const struct nk_config *ci,
		    struct nk_region *out, int max);
void nk_config_dump(const struct rom *rom, const struct nk_config *ci, FILE *f);

/* NanoKernel calls: the emulator executes `twi 31,r31,N` from its trap table. */
const char *nk_kcall_name(unsigned n);

struct nk_boot_options {
	uint32_t ram_size;
	uint64_t max_insns;
	bool trace;
	uint64_t trace_from;    /* start tracing after this many instructions */
	int trace_reg;          /* GPR to include in trace lines, or -1 */
	uint32_t trace_lo, trace_hi;    /* if trace_hi: trace only pcs in [lo, hi) ... */
	uint32_t trace_insn;            /* ... and only this instruction word, if not 0 */
	bool io_as_ram;         /* back first-touched I/O pages with RAM and keep going */
	uint32_t watch;         /* log writes to the 4 KB page containing this address (0: off) */
	unsigned long trap_log; /* log this many 68k A-line traps (0: off) */
	uint64_t trap_log_from; /* ... starting at this instruction count */
	bool log_hostcalls;     /* log calls from the generated drivers */
	const char *screenshot; /* save the screen here (PNG) when the run ends */
#define NK_MAX_DISKS 4
	const char *disk[NK_MAX_DISKS];         /* disk images, in drive order (NULL: an empty CD-ROM drive) */
	bool cdrom[NK_MAX_DISKS];               /* ... attached read-only as CDs (see cd_swap in nk.c) */
	unsigned disks;
	struct share *share;    /* a shared folder, its drive after the disks (counts towards the 4) */
	bool rom_from_disk;     /* the ROM file came from a drive's System Folder: ... */
	unsigned rom_disk, rom_part;    /* ... this drive's, this partition (the startup disk) */
	bool rom_chosen;        /* ... which Open Firmware's boot-device names (see startup.h) */
#define NK_MAX_FLOPPIES 16
	const char *floppy[NK_MAX_FLOPPIES];    /* floppy images (floppy.h): the first is in the drive at startup */
	unsigned floppies;
	bool floppy_drive;      /* a floppy drive, even without images (see disk.h) */
	const char *dump_ram;   /* write guest RAM here when the run ends */
	const char *nvram;      /* NVRAM file: loaded at start (if present), saved at the end */
	bool window;            /* show the screen in a host window and take input from it */
	bool realtime;          /* guest time follows the host clock */
	bool no_idle;           /* keep running flat out while the guest idles */
	bool no_fast_start;     /* in real time, guest time doesn't run faster until the Finder is up */
	bool interp;            /* interpret only (no JIT) */
	bool no_sound;          /* don't play sound on the host (it plays to nowhere) */
	bool no_mic;            /* sound input records silence (the host's microphone is never opened) */
	const char *mic_file;   /* a WAV file standing in for the microphone (over and over), or NULL */
	bool integer_scale;     /* the window scales a screen not its own by whole numbers only */
	unsigned scale;         /* the window's scale: display pixels to a screen pixel (0: Auto, display.h) */
	bool no_status_bar;     /* the window starts without its status bar */
	bool no_net;            /* no host network behind the guest's Ethernet (real time has one) */
	bool no_gpu;            /* the 3D engine never draws with the host's GPU (real time does, if it can) */
	double max_seconds;     /* stop after this much host time (0: no limit) */
	int64_t fixed_time;     /* real-time clock starts here (Unix time) instead of the host's */
	const char *input_script;       /* scripted input: "<insns> <command> [args]" lines */
};

int nk_boot(const struct rom *rom, const struct nk_boot_options *opt);

#endif
