#include "nk.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "audio.h"
#include "awacs.h"
#include "be.h"
#include "bootinfo.h"
#include "cuda.h"
#include "dbdma.h"
#include "disk.h"
#include "display.h"
#include "gpu.h"
#include "jit.h"
#include "macio.h"
#include "mmio.h"
#include "mp.h"
#include "ndrv.h"
#include "net.h"
#include "nvram.h"
#include "ohci.h"
#include "pci.h"
#include "perf.h"
#include "ppc_fp.h"
#include "rave.h"
#include "rtas.h"
#include "scc.h"
#include "tools.h"
#include "via.h"
#include "video.h"

/* Mac OS low-memory globals */
#define LOWMEM_VIA 0x01D4u

/* ConfigInfo field offsets */
enum {
	CI_MAC68K_ROM_OFFSET = 0x034,
	CI_MAC68K_ROM_SIZE = 0x038,
	CI_EMULATOR_CODE_OFFSET = 0x054,
	CI_EMULATOR_CODE_SIZE = 0x058,
	CI_OPCODE_TABLE_OFFSET = 0x05C,
	CI_OPCODE_TABLE_SIZE = 0x060,
	CI_BOOTSTRAP_VERSION = 0x064,
	CI_BOOT_VERSION_OFFSET = 0x074,
	CI_ECB_OFFSET = 0x078,
	CI_IPL_VALUE_OFFSET = 0x07C,
	CI_EMULATOR_ENTRY_OFFSET = 0x080,
	CI_KERNEL_TRAP_TABLE_OFFSET = 0x084,
	CI_TEST_INT_MASK = 0x088,
	CI_CLEAR_INT_MASK = 0x08C,
	CI_POST_INT_MASK = 0x090,
	CI_LA_INTERRUPT_CTL = 0x094,
	CI_LA_INFO_RECORD = 0x09C,
	CI_LA_KERNEL_DATA = 0x0A0,
	CI_LA_EMULATOR_DATA = 0x0A4,
	CI_LA_DISPATCH_TABLE = 0x0A8,
	CI_LA_EMULATOR_CODE = 0x0AC,
	CI_MAC_LOWMEM_INIT_OFFSET = 0x0B0,
	CI_PAGE_MAP_INIT_SIZE = 0x0B8,
	CI_PAGE_MAP_INIT_OFFSET = 0x0BC,
	CI_PAGE_MAP_IRP_OFFSET = 0x0C0,
	CI_PAGE_MAP_KDP_OFFSET = 0x0C4,
	CI_PAGE_MAP_EDP_OFFSET = 0x0C8,
	CI_SEGMAP32_USR = 0x14C,
	CI_BAT_RANGE = 0x2CC,
	CI_BATMAP32_USR = 0x350,
	CI_PA_RELOCATED_LOWMEM = 0x360,
	CI_LA_OPEN_FIRMWARE = 0x36C,
	CI_LA_HARDWARE_PRIV = 0x374,
	CI_SIZE = 0x380,
};

/* PMDT attribute bits (low 12 bits of the second word) */
enum {
	PATTR_NOT_PTE = 0x800,
	PATTR_PTE_SINGLE = 0x400,
	PATTR_PTE_REL = 0x200,      /* RPN is ConfigInfo-relative */
	PTE_W = 0x040,              /* write-through */
	PTE_I = 0x020,              /* cache inhibited */
	PTE_PP = 0x003,
};

/*
 * Pages mapped write-through and read-only (PP 3), as the ROM is: the
 * NanoKernel drops stores to them (counting them in NanoKernelInfo's
 * QuietWriteCount) instead of raising an exception. Mac OS 8.5's own ROM
 * file has a 'thng' resource in it (the DAV audio component's), whose
 * description the Component Manager copies onto itself when it registers it.
 */
static bool quiet_attr(uint32_t attr)
{
	return (attr & (PTE_W | PTE_PP)) == (PTE_W | PTE_PP);
}

/* Kernel Data Page layout: PowerPC info records sit just below 0xFC0. */
enum {
	KDP_EMU_TRAP_TABLE = 0x648,     /* the emulator's kernel call table (EmuTrapTableLogical) */
	KDP_NK_INFO = 0x940,            /* (v2's longer record, where v1 kept its page map) */
	KDP_PROC_STATE = 0xB00,
	KDP_HW_INFO = 0xB80,
	KDP_SYS_INFO = 0xC20,
	KDP_DIAG_INFO = 0xD60,
	KDP_PROC_INFO = 0xF60,
	KDP_INFO_PTRS = 0xFC0,
};

/*
 * The NanoKernel version we report (NanoKernelInfo's). Mac OS 8.6 and later
 * carry a v2 kernel (resource 'krnl') and put it in place of an older one,
 * by way of supervisor mode, which there's none of here; a kernel at least
 * as new they leave alone. So we report the last version there was, and
 * NanoKernelInfo's v2 fields say there's no blue task, so that
 * Multiprocessing Services don't count on the kernel's.
 */
#define NK_INFO_VERSION 0x0228u
#define NK_INFO_LENGTH 0x160u
#define NKI_QUIET_WRITES 0x0D0u         /* NanoKernelInfo's QuietWriteCount (see quiet_attr()) */

/* NanoKernelInfo's version 2 fields */
enum {
	NKI_BLUE_PROCESS = 0x100,
	NKI_BLUE_TASK = 0x104,
	NKI_PAGE_QUEUE = 0x108,         /* (page faults for the blue task to handle) */
	NKI_TASK_COUNT = 0x10C,
};

/* Context block (EmulatorData at ECBOffset, and native contexts); 64-bit slots, value in the low word */
enum {
	CB_INTER_FLAGS = 0x000,
	CB_INTER_ENABLES = 0x004,
	CB_INTRA_FLAGS = 0x040,
	CB_INTRA_ENABLES = 0x040 + 0x04,
	CB_INTRA_HANDLER = 0x040 + 0x08 + 4,
	CB_INTRA_HANDLER_ARG = 0x040 + 0x10 + 4,
	CB_INTRA_HANDLER_RETURN = 0x040 + 0x18 + 4,
	CB_FAULT_SRC_PC = 0x080 + 4,
	CB_FAULT_SRC_LR = 0x088 + 4,
	CB_FAULT_SRC_R3 = 0x090 + 4,
	CB_FAULT_SRC_R4 = 0x098 + 4,
	CB_PRIORITY_SHIFTY = 0x0CC,     /* (NanoKernel 2's: see blue_boost()) */
	CB_XER = 0x0D0 + 4,
	CB_CR = 0x0D8 + 4,
	CB_FPSCR = 0x0E0 + 4,
	CB_LR = 0x0E8 + 4,
	CB_CTR = 0x0F0 + 4,
	CB_PC = 0x0F8 + 4,
	CB_GPR = 0x100,         /* r0..r31, 8 bytes each, value in the low word */
	CB_FPR = 0x200,         /* f0..f31, 8 bytes each */
	CB_SIZE = 0x300,
};

/* NanoKernel exception codes (high byte of the context flags) */
enum {
	EC_NO_EXCEPTION = 0,            /* an external interrupt ended the alternate context */
	EC_SYSTEM_CALL = 1,
	EC_TRAP_INSTR = 2,
	EC_INVALID_INSTR = 4,
	EC_PRIVILEGED_INSTR = 5,
	EC_INST_INVALID_ADDRESS = 10,
	EC_DATA_INVALID_ADDRESS = 18,
	EC_DATA_WRITE_VIOLATION = 21,
};

#define TWI_KCALL_MASK 0xFFFF0000u
#define TWI_KCALL 0x0FFF0000u           /* twi 31,r31,N */
#define USER_MSR 0x0000D032u            /* EE PR ME IR DR RI, as the NanoKernel sets it */

static uint32_t ci32(const struct rom *rom, uint32_t off)
{
	return be32(rom->image + NK_CONFIGINFO_OFFSET + off);
}

int nk_config_parse(const struct rom *rom, struct nk_config *ci, char *err, size_t errlen)
{
	memset(ci, 0, sizeof *ci);

	memcpy(ci->bootstrap_version, rom->image + NK_CONFIGINFO_OFFSET + CI_BOOTSTRAP_VERSION, 16);
	for (int k = 0; k < 16; k++)
		if ((unsigned char)ci->bootstrap_version[k] < 0x20 || (unsigned char)ci->bootstrap_version[k] >= 0x7F)
			ci->bootstrap_version[k] = '\0';
	if (strncmp(ci->bootstrap_version, "NewWorld", 8) != 0 && strncmp(ci->bootstrap_version, "Boot ", 5) != 0) {
		snprintf(err, errlen, "no NanoKernel ConfigInfo at 0x%X", NK_CONFIGINFO_OFFSET);
		return -1;
	}

	ci->mac68k_rom_offset = (int32_t)ci32(rom, CI_MAC68K_ROM_OFFSET);
	ci->mac68k_rom_size = ci32(rom, CI_MAC68K_ROM_SIZE);
	ci->emulator_code_offset = (int32_t)ci32(rom, CI_EMULATOR_CODE_OFFSET);
	ci->emulator_code_size = ci32(rom, CI_EMULATOR_CODE_SIZE);
	ci->opcode_table_offset = (int32_t)ci32(rom, CI_OPCODE_TABLE_OFFSET);
	ci->opcode_table_size = ci32(rom, CI_OPCODE_TABLE_SIZE);
	ci->boot_version_offset = ci32(rom, CI_BOOT_VERSION_OFFSET);
	ci->ecb_offset = ci32(rom, CI_ECB_OFFSET);
	ci->ipl_value_offset = ci32(rom, CI_IPL_VALUE_OFFSET);
	ci->emulator_entry_offset = ci32(rom, CI_EMULATOR_ENTRY_OFFSET);
	ci->kernel_trap_table_offset = ci32(rom, CI_KERNEL_TRAP_TABLE_OFFSET);
	ci->la_interrupt_ctl = ci32(rom, CI_LA_INTERRUPT_CTL);
	ci->la_info_record = ci32(rom, CI_LA_INFO_RECORD);
	ci->la_kernel_data = ci32(rom, CI_LA_KERNEL_DATA);
	ci->la_emulator_data = ci32(rom, CI_LA_EMULATOR_DATA);
	ci->la_dispatch_table = ci32(rom, CI_LA_DISPATCH_TABLE);
	ci->la_emulator_code = ci32(rom, CI_LA_EMULATOR_CODE);
	ci->mac_lowmem_init_offset = ci32(rom, CI_MAC_LOWMEM_INIT_OFFSET);
	ci->page_map_init_size = ci32(rom, CI_PAGE_MAP_INIT_SIZE);
	ci->page_map_init_offset = ci32(rom, CI_PAGE_MAP_INIT_OFFSET);
	ci->page_map_irp_offset = ci32(rom, CI_PAGE_MAP_IRP_OFFSET);
	ci->page_map_kdp_offset = ci32(rom, CI_PAGE_MAP_KDP_OFFSET);
	ci->page_map_edp_offset = ci32(rom, CI_PAGE_MAP_EDP_OFFSET);
	for (int s = 0; s < 16; s++) {
		ci->segmap_usr[s][0] = ci32(rom, CI_SEGMAP32_USR + 8 * s);
		ci->segmap_usr[s][1] = ci32(rom, CI_SEGMAP32_USR + 8 * s + 4);
		ci->bat_range[s][0] = ci32(rom, CI_BAT_RANGE + 8 * s);
		ci->bat_range[s][1] = ci32(rom, CI_BAT_RANGE + 8 * s + 4);
	}
	ci->batmap_usr = ci32(rom, CI_BATMAP32_USR);
	ci->pa_relocated_lowmem = ci32(rom, CI_PA_RELOCATED_LOWMEM);
	ci->la_open_firmware = ci32(rom, CI_LA_OPEN_FIRMWARE);
	ci->la_hardware_priv = ci32(rom, CI_LA_HARDWARE_PRIV);

	uint32_t pm_end = ci->page_map_init_offset + ci->page_map_init_size;
	if (pm_end < ci->page_map_init_offset || NK_CONFIGINFO_OFFSET + pm_end > ROM_IMAGE_SIZE ||
	    NK_CONFIGINFO_OFFSET + ci->mac_lowmem_init_offset >= ROM_IMAGE_SIZE) {
		snprintf(err, errlen, "ConfigInfo tables lie outside the ROM image");
		return -1;
	}
	return 0;
}

/* ConfigInfo-relative offset -> Toolbox image offset, or -1 if outside the image. */
static int64_t ci_rel_to_rom(int32_t rel)
{
	int64_t off = (int64_t)NK_CONFIGINFO_OFFSET + rel;
	return (off < 0 || off >= ROM_IMAGE_SIZE) ? -1 : off;
}

static int add_region(struct nk_region *out, int n, int max, struct nk_region r)
{
	if (n < max)
		out[n] = r;
	return n + 1;
}

/*
 * ROM files from Mac OS 9.1 on leave the address space out of ConfigInfo
 * (the Trampoline in the same file hands the kernel a page map instead).
 * It's laid out here as earlier ROMs describe it, at the logical addresses
 * ConfigInfo still gives.
 */
static int default_regions(const struct nk_config *ci, struct nk_region *out, int max)
{
	int64_t code = ci_rel_to_rom(ci->emulator_code_offset), table = ci_rel_to_rom(ci->opcode_table_offset);
	if (code < 0 || code + ci->emulator_code_size > ROM_IMAGE_SIZE ||
	    table < 0 || table + ci->opcode_table_size > ROM_IMAGE_SIZE)
		return 0;
	const struct nk_region r[] = {
		{ .kind = NK_REGION_ROM, .logical = ROM_BASE, .size = ROM_IMAGE_SIZE, .quiet = true },
		/* the emulator's code and dispatch table, in the megabyte of the image around them */
		{ .kind = NK_REGION_ROM, .logical = ci->la_emulator_code & 0xFFF00000u, .size = 0x100000,
		  .rom_offset = (uint32_t)code & 0xFFF00000u },
		{ .kind = NK_REGION_ROM, .logical = ci->la_emulator_code, .size = ci->emulator_code_size,
		  .rom_offset = (uint32_t)code },
		{ .kind = NK_REGION_ROM, .logical = ci->la_dispatch_table, .size = ci->opcode_table_size,
		  .rom_offset = (uint32_t)table },
		{ .kind = NK_REGION_INFO_RECORD, .logical = ci->la_info_record, .size = MEM_PAGE_SIZE, .writable = true },
		{ .kind = NK_REGION_KERNEL_DATA, .logical = ci->la_kernel_data, .size = MEM_PAGE_SIZE },
		{ .kind = NK_REGION_EMULATOR_DATA, .logical = ci->la_emulator_data, .size = MEM_PAGE_SIZE, .writable = true },
		{ .kind = NK_REGION_IO, .logical = ci->la_interrupt_ctl, .size = MEM_PAGE_SIZE,
		  .physical = ci->la_interrupt_ctl, .writable = true },
		{ .kind = NK_REGION_PHYS, .logical = ci->la_open_firmware, .size = BOOTINFO_SIZE,
		  .physical = 0x00400000u, .writable = true },
		{ .kind = NK_REGION_PHYS, .logical = 0x60000000u | ROM_PHYS_BASE, .size = MEM_PAGE_SIZE,
		  .physical = ROM_PHYS_BASE },
		{ .kind = NK_REGION_IO, .logical = 0xF0000000u, .size = 0x0F010000u, .physical = 0xF0000000u,
		  .writable = true },
	};
	int n = 0;
	for (size_t k = 0; k < sizeof r / sizeof r[0]; k++) {
		n = add_region(out, n, max, r[k]);
		if (n <= max)
			out[n - 1].source = "auto";
	}
	return n < max ? n : max;
}

int nk_user_regions(const struct rom *rom, const struct nk_config *ci, struct nk_region *out, int max)
{
	int n = 0;

	if (!ci->page_map_init_size && !ci->batmap_usr)
		return default_regions(ci, out, max);

	/* BATs: four instruction and four data BAT indices, one nibble each */
	for (int k = 0; k < 8; k++) {
		unsigned idx = (ci->batmap_usr >> (28 - 4 * k)) & 0xF;
		uint32_t u = ci->bat_range[idx][0], l = ci->bat_range[idx][1];
		if (!(u & 3))
			continue;
		uint32_t logical = u & 0xFFFE0000u;
		uint32_t size = (((u >> 2) & 0x7FF) + 1) << 17;
		bool dup = false;
		for (int j = 0; j < n && j < max; j++)
			dup |= out[j].logical == logical && out[j].size == size;
		if (dup)
			continue;
		struct nk_region r = { .logical = logical, .size = size, .writable = (l & 3) == 2, .quiet = quiet_attr(l),
				       .source = "BAT" };
		if (l & PATTR_PTE_REL) {
			int64_t off = ci_rel_to_rom((int32_t)(l & 0xFFFFF000u));
			if (off < 0 || off + size > ROM_IMAGE_SIZE)
				continue;
			r.kind = NK_REGION_ROM;
			r.rom_offset = (uint32_t)off;
		} else {
			r.kind = (l & PTE_I) ? NK_REGION_IO : NK_REGION_PHYS;
			r.physical = l & 0xFFFE0000u;
		}
		n = add_region(out, n, max, r);
	}

	/* PMDTs reachable from the user segment map */
	const uint8_t *pm = rom->image + NK_CONFIGINFO_OFFSET + ci->page_map_init_offset;
	for (uint32_t seg = 0; seg < 16; seg++) {
		for (uint32_t o = ci->segmap_usr[seg][0]; o + 8 <= ci->page_map_init_size; o += 8) {
			uint16_t idx = be16(pm + o), cnt = be16(pm + o + 2);
			uint32_t w2 = be32(pm + o + 4), attr = w2 & 0xFFF;
			if (idx == 0 && cnt == 0xFFFF)
				break;  /* whole-segment entry ends the list */

			struct nk_region r = {
				.logical = seg << 28 | (uint32_t)idx << 12,
				.size = (attr & PATTR_PTE_SINGLE) ? MEM_PAGE_SIZE : ((uint32_t)cnt + 1) << 12,
				.writable = (attr & 3) != 3,
				.quiet = quiet_attr(attr),
				.source = "PMDT",
			};
			if (o == ci->page_map_irp_offset) {
				r.kind = NK_REGION_INFO_RECORD;
			} else if (o == ci->page_map_kdp_offset) {
				r.kind = NK_REGION_KERNEL_DATA;
			} else if (o == ci->page_map_edp_offset) {
				r.kind = NK_REGION_EMULATOR_DATA;
			} else if (attr & PATTR_NOT_PTE) {
				continue;
			} else if (attr & PATTR_PTE_REL) {
				int64_t off = ci_rel_to_rom((int32_t)(w2 & 0xFFFFF000u));
				if (off < 0 || off + r.size > ROM_IMAGE_SIZE)
					continue;
				r.kind = NK_REGION_ROM;
				r.rom_offset = (uint32_t)off;
			} else {
				r.kind = (attr & PTE_I) ? NK_REGION_IO : NK_REGION_PHYS;
				r.physical = w2 & 0xFFFFF000u;
			}
			n = add_region(out, n, max, r);
		}
	}
	return n < max ? n : max;
}

static const char *region_kind_name(enum nk_region_kind k)
{
	switch (k) {
	case NK_REGION_ROM:           return "rom";
	case NK_REGION_INFO_RECORD:   return "info records";
	case NK_REGION_KERNEL_DATA:   return "kernel data";
	case NK_REGION_EMULATOR_DATA: return "emulator data";
	case NK_REGION_IO:            return "i/o";
	case NK_REGION_PHYS:          return "physical";
	}
	return "?";
}

void nk_config_dump(const struct rom *rom, const struct nk_config *ci, FILE *f)
{
	fprintf(f, "bootstrap version:  \"%s\"\n", ci->bootstrap_version);
	fprintf(f, "emulator code:      0x%08X (rom 0x%06X, %u bytes), entry +0x%X, kernel traps +0x%X\n",
		ci->la_emulator_code, NK_CONFIGINFO_OFFSET + ci->emulator_code_offset, ci->emulator_code_size,
		ci->emulator_entry_offset, ci->kernel_trap_table_offset);
	fprintf(f, "dispatch table:     0x%08X (rom 0x%06X, %u bytes)\n",
		ci->la_dispatch_table, NK_CONFIGINFO_OFFSET + ci->opcode_table_offset, ci->opcode_table_size);
	fprintf(f, "emulator data:      0x%08X (ECB +0x%X, IPL +0x%X, boot version +0x%X)\n",
		ci->la_emulator_data, ci->ecb_offset, ci->ipl_value_offset, ci->boot_version_offset);
	fprintf(f, "kernel data:        0x%08X\n", ci->la_kernel_data);
	fprintf(f, "info records:       0x%08X\n", ci->la_info_record);
	fprintf(f, "interrupt control:  0x%08X\n", ci->la_interrupt_ctl);
	fprintf(f, "open firmware:      0x%08X\n", ci->la_open_firmware);
	fprintf(f, "hardware priv:      0x%08X\n", ci->la_hardware_priv);

	struct nk_region regions[NK_MAX_REGIONS];
	int n = nk_user_regions(rom, ci, regions, NK_MAX_REGIONS);
	fprintf(f, "\nuser address space:\n");
	for (int k = 0; k < n; k++) {
		const struct nk_region *r = &regions[k];
		fprintf(f, "  %08X-%08X  %-4s %-2s  %-13s", r->logical, r->logical + r->size - 1,
			r->source, r->writable ? "rw" : "ro", region_kind_name(r->kind));
		if (r->kind == NK_REGION_ROM)
			fprintf(f, "  rom 0x%06X", r->rom_offset);
		else if (r->kind == NK_REGION_IO || r->kind == NK_REGION_PHYS)
			fprintf(f, "  phys 0x%08X", r->physical);
		fprintf(f, "\n");
	}

	fprintf(f, "\nlow memory init:\n");
	for (uint32_t o = ci->mac_lowmem_init_offset; NK_CONFIGINFO_OFFSET + o + 8 <= ROM_IMAGE_SIZE; o += 8) {
		uint32_t addr = ci32(rom, o);
		if (addr == 0)
			break;
		fprintf(f, "  [0x%04X] = 0x%08X\n", addr, ci32(rom, o + 4));
	}
}

const char *nk_kcall_name(unsigned n)
{
	static const char *const names[16] = {
		"ReturnFromException", "RunAlternateContext", "ResetSystem", "VMDispatch",
		"PrioritizeInterrupts", "PowerDispatch", "RTASDispatch", "CacheDispatch",
		"MPDispatch", NULL, NULL, NULL,
		"CallAdapterProcPPC", NULL, "CallAdapterProc68k", "SystemCrash",
	};
	return n < 16 && names[n] ? names[n] : "unknown";
}

/* ---- boot ---------------------------------------------------------------- */

#define TIMEBASE_HZ 16600000u
#define SYNC_INSNS 20000u               /* instructions between device updates */
#define CPU_PVR 0x00080301u             /* PowerPC 750 */
#define CPU_HZ 266000000u
#define BUS_HZ 66000000u

/* Framebuffer: plain RAM in PCI memory space, described by the display node */
#define FB_BASE 0x82000000u
#define FB_SIZE 0x01000000u

/* Host call selectors (low 26 bits of the primary-opcode-6 instruction) */
enum {
	HOSTCALL_VIDEO_DRIVER = 1,      /* the display NDRV */
	HOSTCALL_DISK_DRIVER,           /* the block device NDRVs: one selector each */
};

struct machine {
	const struct rom *rom;
	struct nk_config ci;
	struct mem mem;
	struct ppc_cpu cpu;
	struct bootinfo bootinfo;
	struct rtas rtas;
	struct nvram nvram;             /* RTAS's NVRAM as a device (ROM files with parcels; see nvram.h) */
	struct mmio_bus bus;
	struct macio macio;
	struct dbdma dbdma;
	struct awacs awacs;
	struct audio *audio;            /* host sound output, if open */
	FILE *wav;                      /* sound capture (SYSHM_SOUND_WAV), if any */
	uint32_t wav_frames, wav_rate;
	struct via via;
	struct cuda cuda;
	struct ohci ohci;               /* USB */
	struct pci_bus pci;             /* configuration space */
	struct guest_clock clock;
	struct scc scc;
	struct video video;
	struct helper helper;           /* the guest tools' helper application */
	struct rave rave;               /* ... and their 3D engine */
	struct gpu *gpu;                /* ... which draws with this, if the host can (see nk_boot()) */
	/* SYSHM_PERF: the host's counters for this thread, printed every second */
	bool perf_on;
	struct perf perf;
	/* instruction time at another rate (the script's "clock"): this many million per guest second */
	unsigned clock_mips;
	uint64_t clock_tb, clock_icount;
	uint64_t perf_ns, perf_cycles, perf_insns, perf_icount, perf_idle_ns, perf_disk_read, perf_disk_written;
	unsigned want_w, want_h;        /* screen size the window asks for (0: none) */
	uint64_t want_ns;               /* host time it last changed (0: from a script, at once) */
	struct display *display;        /* host window, if open */
	uint8_t *rgb;                   /* screen converted for the window */
	uint8_t *shown;                 /* framebuffer bytes as last shown */
	uint8_t shown_clut[256][3];
	unsigned shown_w, shown_h, shown_bpp;
	uint64_t shown_ns;
	bool quit;                      /* the window was closed */
	bool redraw;                    /* the window needs drawing even if the screen is the same */
	bool mouse_captured;            /* host motion goes to the ADB mouse (see follow_cursor()) */
	bool mouse_released;            /* ... but the user let the pointer go (Control-Option) */
	bool paused;                    /* the menu is open (F12): the guest doesn't run */
	bool restart, shut_down;        /* ... and this was chosen in it */
	char menu_status[480];          /* news it shows (where a screenshot went) */
	/* the status bar (see status_lights()) */
	uint64_t led_count[UI_LEDS], led_written[UI_LEDS];      /* devices' counts at the last frame */
	uint64_t led_until[UI_LEDS], led_amber_until[UI_LEDS];  /* host time their lights stay lit until */
	uint64_t led_ns, led_idle_ns;   /* host time, and idle_ns, at the last frame (0: none since a pause) */
	float cpu_busy;                 /* share of the time the guest hasn't napped (smoothed) */
	uint64_t busy_ns;               /* host time it hasn't, since ... */
	uint64_t speed_ns;              /* ... the speed and rates were last worked out (0: start again) */
	uint64_t run_ns, run_insns;     /* host time in sampled ppc_run() calls, and the instructions they ran */
	uint32_t run_sample;            /* (picks them) */
	uint32_t speed_mhz;
	uint64_t rate_from_in[UI_LEDS], rate_from_out[UI_LEDS];  /* devices' bytes in and out then */
	uint64_t rate_in[UI_LEDS], rate_out[UI_LEDS];            /* ... and a second since */
	uint64_t cursor_hidden_ns;      /* host time Mac OS's cursor was hidden (0: it's shown) */
	bool scripted;                  /* input comes from a script (coordinates assume its screen) */
	bool realtime;                  /* guest time follows the host clock */
	bool idle_sleep;                /* ... and the host sleeps while the guest idles */
	uint64_t start_ns;              /* host time at boot */
	uint64_t lead_tb;               /* how far guest time is ahead of the host clock (see pass_guest_time) */
	uint64_t next_frame_ns;         /* when the window is next updated */
	unsigned long idle_calls;       /* _SynchIdleTime and PowerDispatch naps */
	uint64_t idle_ns;               /* host time spent sleeping for them */
	uint32_t recompiled_traps;      /* the 68k emulator's trap entry for recompiled code, or 0 */
	/* fast start (see fast_start_begin()) */
	bool fast_start;
	bool fast_idled;                /* ... the guest has idled since it began */
	bool fast_finder;               /* ... the Finder has run */
	uint64_t fast_begin_tb, fast_begin_ns, fast_last_ns;
	uint64_t fast_finder_tb;        /* guest time the Finder first ran */
	uint64_t fast_pause_ns;         /* host time until which input holds it back (UINT64_MAX: held down) */
	uint64_t fast_keys[2];          /* ADB keys down, before the Finder runs */
	bool fast_button;               /* ... and the mouse button */
	uint64_t fast_disk;             /* bytes the disks had moved at the last idle call */
	uint64_t fast_idle_tb;          /* guest time of the last idle call */
	uint64_t fast_quiet_tb;         /* guest time since which the guest has idled without disk transfers */
	uint8_t display_ndrv[2048];
	struct disk disk[NK_MAX_DISKS];
	unsigned disks;
	uint8_t disk_ndrv[NK_MAX_DISKS][2048];
	struct net net;                 /* Ethernet */
	uint8_t lanlib[4096];
	uint64_t net_poll_ns;           /* host time the host network was last serviced */
	uint32_t level_mask[2][8];      /* 68k interrupt level of each source */
	uint32_t pending[2];            /* last pending sources reported in HWInfo */
	unsigned ipl;                   /* last 68k interrupt level posted to the emulator */
	bool dev_dirty;                 /* devices may have changed since they were last updated */
	uint64_t dev_next_tb;           /* ... or will by themselves at this timebase (device_next_tb()) */
	uint64_t mmio_until;            /* device_hook() takes accesses before this instruction count only */
	unsigned long dev_accesses, dev_inline;         /* device register accesses, and those device_hook() did */
	bool preempt;                   /* end the alternate context for a posted interrupt */
	bool watch_writable;            /* whether the --watch page is writable (see watch_protect()) */
	uint32_t ram_size;
	uint32_t logical_pages;          /* RAM pages visible to Mac OS (shrinks with VMAllocateMemory) */
	uint32_t context;               /* logical address of the running context block */
	uint32_t system_cb;             /* the emulator's ("System") context block */
	uint32_t native_cb;             /* most recent alternate (native PowerPC) context block */
	bool system;                    /* is the System context running? */
	uint32_t flags;                 /* context flags; exception code in the top byte */
	uint32_t enables;               /* exception enables of the running context */
	unsigned long exceptions[32];
	uint8_t kdp[MEM_PAGE_SIZE];     /* kernel's copy of the KernelData page */
	bool mp_kernel;                 /* the ROM wants Multiprocessing Services from the kernel (see init_info_records()) */
	struct mp mp;                   /* ... which are these */
};

/*
 * The InfoRecord page is a user-writable alias of KernelData (one physical page
 * on real hardware). The kernel's copy is pushed to both views with kdp_sync().
 * The guest's view is read-only here: a store to it faults, the kernel makes it
 * with the page writable and pulls the change in with kdp_pull().
 */
static void kdp_sync(struct machine *m)
{
	mem_load(&m->mem, m->ci.la_kernel_data, m->kdp, MEM_PAGE_SIZE);
	mem_load(&m->mem, m->ci.la_info_record, m->kdp, MEM_PAGE_SIZE);
}

static void kdp_pull(struct machine *m)
{
	if (memcmp(m->kdp, mem_ptr(&m->mem, m->ci.la_info_record), MEM_PAGE_SIZE) != 0) {
		memcpy(m->kdp, mem_ptr(&m->mem, m->ci.la_info_record), MEM_PAGE_SIZE);
		mem_load(&m->mem, m->ci.la_kernel_data, m->kdp, MEM_PAGE_SIZE);
	}
}

static void kdp_info_ptr(struct machine *m, uint32_t slot, uint32_t rec, uint16_t ver, uint16_t len)
{
	put_be32(m->kdp + slot, m->ci.la_info_record + rec);
	put_be16(m->kdp + slot + 4, ver);
	put_be16(m->kdp + slot + 6, len);
}

static void init_info_records(struct machine *m)
{
	uint8_t *k = m->kdp;

	memset(k, 0, sizeof m->kdp);

	/* Pointer table at InfoRecord+0xFC0; versions/lengths as NanoKernel v1 reports them */
	put_be32(k + KDP_INFO_PTRS, m->ci.la_info_record + KDP_INFO_PTRS);
	kdp_info_ptr(m, 0xFC8, KDP_PROC_STATE, 0x0100, 0x80);
	kdp_info_ptr(m, 0xFD0, KDP_HW_INFO, 0x0103, 0xA0);
	kdp_info_ptr(m, 0xFD8, KDP_PROC_INFO, 0x0101, 0x60);
	kdp_info_ptr(m, 0xFE0, KDP_NK_INFO, NK_INFO_VERSION, NK_INFO_LENGTH);
	kdp_info_ptr(m, 0xFE8, KDP_DIAG_INFO, 0x0100, 0x100);
	kdp_info_ptr(m, 0xFF0, KDP_SYS_INFO, 0x0105, 0x140);
	kdp_info_ptr(m, 0xFF8, KDP_PROC_INFO, 0x0101, 0x60);
	put_be32(k + KDP_EMU_TRAP_TABLE, m->ci.la_emulator_code + m->ci.kernel_trap_table_offset);

	/*
	 * ROM files from Mac OS 9.1 on start Multiprocessing Services
	 * themselves and want a NanoKernel 2 underneath: one that runs Mac OS
	 * as its "blue" task, in the blue process (see mp.h). The ROM checks
	 * that the blue process has ID 1, the first one a NanoKernel 2 makes.
	 */
	if (m->mp_kernel) {
		uint8_t *nki = k + KDP_NK_INFO;
		put_be32(nki + NKI_BLUE_PROCESS, m->mp.blue_process);
		put_be32(nki + NKI_BLUE_TASK, m->mp.blue_task);
		put_be32(nki + NKI_PAGE_QUEUE, m->mp.page_queue);
		put_be32(nki + NKI_TASK_COUNT, m->mp.tasks);
	}

	/* ProcessorInfo: a 266 MHz PowerPC 750 on a 66 MHz bus */
	uint8_t *pi = k + KDP_PROC_INFO;
	put_be32(pi + 0x00, CPU_PVR);
	put_be32(pi + 0x04, CPU_HZ);
	put_be32(pi + 0x08, BUS_HZ);
	put_be32(pi + 0x0C, TIMEBASE_HZ);       /* decrementer clock */
	put_be32(pi + 0x10, MEM_PAGE_SIZE);
	put_be32(pi + 0x14, 32768);             /* D-cache */
	put_be32(pi + 0x18, 32768);             /* I-cache */
	put_be16(pi + 0x1C, 32);                /* coherency block */
	put_be16(pi + 0x1E, 32);                /* reservation granule */
	put_be16(pi + 0x22, 32);                /* I-cache line */
	put_be16(pi + 0x24, 32);                /* D-cache line */
	put_be16(pi + 0x26, 32);                /* dcbt block */
	put_be16(pi + 0x28, 32);                /* I-cache block */
	put_be16(pi + 0x2A, 32);                /* D-cache block */
	put_be16(pi + 0x2C, 8);                 /* I-cache associativity */
	put_be16(pi + 0x2E, 8);                 /* D-cache associativity */
	put_be16(pi + 0x30, 128);               /* TLB entries */
	put_be16(pi + 0x32, 2);                 /* TLB associativity */
	put_be32(pi + 0x48, 7);                 /* ProcessorFlags: L2CR, PLRU L1, TAU */

	/* SystemInfo: one bank of RAM at 0, no L2 */
	uint8_t *si = k + KDP_SYS_INFO;
	put_be32(si + 0x00, m->ram_size);       /* physical */
	put_be32(si + 0x04, m->ram_size);       /* usable */
	put_be32(si + 0x08, m->ram_size);       /* logical */
	put_be32(si + 0x0C, 0x00100000);        /* hash table size */
	put_be16(si + 0x18, 1);                 /* L2 combined (none) */
	put_be32(si + 0x30, 0);                 /* bank 0 start */
	put_be32(si + 0x34, m->ram_size);       /* bank 0 size */
	put_be32(si + 0x100, MACIO_INTCTL);     /* IntCntrBaseAddr */

	/* HWInfo: built by our Trampoline replacement */
	bootinfo_hwinfo(&m->bootinfo, k + KDP_HW_INFO);
}

/*
 * Played sound: the codec's volume applied, then to the host and the
 * capture file. Nothing here feeds back into the guest, which is paced by
 * guest time only.
 */
static void play_sound(void *ctx, const uint8_t *frames, unsigned count, unsigned rate)
{
	struct machine *m = ctx;
	unsigned gl, gr;
	awacs_gain(&m->awacs, &gl, &gr);
	uint8_t out[256 * 4], wav[256 * 4];
	while (count) {
		unsigned n = count < 256 ? count : 256;
		for (unsigned k = 0; k < n; k++) {
			int l = (int16_t)be16(frames + 4 * k) * (int)gl / 65536;
			int r = (int16_t)be16(frames + 4 * k + 2) * (int)gr / 65536;
			put_be16(out + 4 * k, (uint16_t)l);
			put_be16(out + 4 * k + 2, (uint16_t)r);
			put_le32(wav + 4 * k, (uint32_t)(uint16_t)l | (uint32_t)(uint16_t)r << 16);
		}
		if (m->audio)
			audio_play(m->audio, out, n, rate);
		if (m->wav) {
			m->wav_rate = m->wav_rate ? m->wav_rate : rate;
			m->wav_frames += (uint32_t)fwrite(wav, 4, n, m->wav);
		}
		frames += 4 * n;
		count -= n;
	}
}

static void wav_header(uint8_t h[44], uint32_t frames, uint32_t rate)
{
	memcpy(h, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0", 24);
	put_le32(h + 4, 36 + frames * 4);
	put_le32(h + 24, rate);
	put_le32(h + 28, rate * 4);
	put_le32(h + 32, 4u | 16u << 16);       /* block align 4, 16 bits */
	memcpy(h + 36, "data", 4);
	put_le32(h + 40, frames * 4);
}

/* DMA channels pulse their interrupt sources; Mac I/O latches them */
static void dma_interrupt(void *ctx, unsigned source)
{
	macio_pulse(ctx, source);
}

/*
 * Generated code reaches guest memory through host page protection (see
 * jit.c), and host pages may be 16 KB (Asahi): a 4 KB guest page that can't
 * be read, in the same 16 KB as one that can, would make every access to
 * that one take the slow way. The ROM maps some one-page regions so (the
 * kernel's and the emulator's data, the info records); the pages around
 * them read as zeros here instead of faulting, unless they're I/O. This is
 * done on every host, so that the machine is the same everywhere.
 */
#define FILL_BLOCK_PAGES 4u

static void fill_page_holes(struct machine *m)
{
	struct nk_region regions[NK_MAX_REGIONS];
	int n = nk_user_regions(m->rom, &m->ci, regions, NK_MAX_REGIONS);
	for (uint32_t pg = 0; pg < (1u << (32 - MEM_PAGE_SHIFT)); pg += FILL_BLOCK_PAGES) {
		unsigned readable = 0;
		for (unsigned k = 0; k < FILL_BLOCK_PAGES; k++)
			readable += m->mem.pageflags[pg + k] & MEM_R;
		if (!readable || readable == FILL_BLOCK_PAGES)
			continue;
		for (unsigned k = 0; k < FILL_BLOCK_PAGES; k++) {
			uint32_t addr = (pg + k) << MEM_PAGE_SHIFT;
			bool io = mmio_find(&m->bus, addr) != NULL;
			for (int r = 0; r < n && !io; r++)
				io = regions[r].kind == NK_REGION_IO && addr - regions[r].logical < regions[r].size;
			if (!(m->mem.pageflags[pg + k] & MEM_R) && !io) {
				memset(mem_ptr(&m->mem, addr), 0, MEM_PAGE_SIZE);
				mem_map(&m->mem, addr, MEM_PAGE_SIZE, MEM_R);
			}
		}
	}
}

static int setup_machine(struct machine *m, const struct nk_boot_options *opt)
{
	const struct nk_config *ci = &m->ci;

	if (mem_init(&m->mem)) {
		fprintf(stderr, "cannot reserve the guest address space\n");
		return -1;
	}

	/* RAM: logical = physical, starting at 0 */
	mem_map(&m->mem, 0, m->ram_size, MEM_RWX);

	struct nk_region regions[NK_MAX_REGIONS];
	int n = nk_user_regions(m->rom, ci, regions, NK_MAX_REGIONS);
	for (int k = 0; k < n; k++) {
		const struct nk_region *r = &regions[k];
		switch (r->kind) {
		case NK_REGION_ROM:
			mem_load(&m->mem, r->logical, m->rom->image + r->rom_offset, r->size);
			mem_map(&m->mem, r->logical, r->size, MEM_RX);
			break;
		case NK_REGION_INFO_RECORD:     /* user-writable view (e.g. HWInfo.CompletedInts) */
			mem_map(&m->mem, r->logical, r->size, MEM_RW);
			break;
		case NK_REGION_KERNEL_DATA:
			mem_map(&m->mem, r->logical, r->size, MEM_R);
			break;
		case NK_REGION_EMULATOR_DATA:
			mem_map(&m->mem, r->logical, r->size, MEM_RW);
			break;
		case NK_REGION_IO:
			/* left unmapped: accesses show up as DSI exits */
			break;
		case NK_REGION_PHYS:
			if (r->logical == ci->la_open_firmware && r->size == BOOTINFO_SIZE) {
				/* where the Trampoline leaves its tables; ours go here too */
				mem_map(&m->mem, r->logical, r->size, MEM_RW);
			} else if (r->physical >= ROM_PHYS_BASE && r->physical - ROM_PHYS_BASE < ROM_IMAGE_SIZE &&
				   r->size <= ROM_IMAGE_SIZE - (r->physical - ROM_PHYS_BASE)) {
				/* a view of the ROM through its physical copy */
				mem_load(&m->mem, r->logical, m->rom->image + (r->physical - ROM_PHYS_BASE), r->size);
				mem_map(&m->mem, r->logical, r->size, MEM_RX);
			}
			break;
		}
	}

	/* Display: framebuffer RAM and the driver Mac OS finds in the device tree */
	memset(mem_ptr(&m->mem, FB_BASE), 0, FB_SIZE);
	mem_map(&m->mem, FB_BASE, FB_SIZE, MEM_RW);
	video_init(&m->video, &m->mem, FB_BASE, FB_SIZE);
	helper_init(&m->helper);
	rave_init(&m->rave, &m->mem);
	m->rave.gpu = m->gpu;
	m->rave.log = getenv("SYSHM_LOG_RAVE") != NULL;
	m->rave.fps = getenv("SYSHM_RAVE_FPS") != NULL;
	if (getenv("SYSHM_PERF") && !m->perf_on) {
		m->perf_on = perf_open(&m->perf);
		if (!m->perf_on)
			fprintf(stderr, "SYSHM_PERF: no processor counters here\n");
	}
	m->video.log = opt->log_hostcalls;
	size_t ndrv_size = video_build_ndrv(&m->video, m->display_ndrv, sizeof m->display_ndrv, HOSTCALL_VIDEO_DRIVER);

	size_t disk_ndrv_size[NK_MAX_DISKS] = { 0 };
	for (unsigned k = 0; k < opt->disks && k < NK_MAX_DISKS; k++) {
		struct disk *d = &m->disk[k];
		if (disk_open(d, &m->mem, opt->disk[k], opt->cdrom[k], opt->cdrom[k]))
			return -1;
		m->disks = k + 1;
		d->log = opt->log_hostcalls;
		disk_ndrv_size[k] = disk_build_ndrv(d, m->disk_ndrv[k], sizeof m->disk_ndrv[k],
						    HOSTCALL_DISK_DRIVER + k);
		for (unsigned p = 0; p < d->partitions; p++)
			printf("disk %u: %s%s, %llu MB at %llu%s%s\n", k, opt->disk[k],
			       d->read_only ? " (read-only)" : "", (unsigned long long)(d->part[p].size >> 20),
			       (unsigned long long)d->part[p].offset, d->part[p].name[0] ? ", partition " : "",
			       d->part[p].name);
	}

	static const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };   /* (locally administered) */
	net_init(&m->net, &m->mem, mac, TIMEBASE_HZ);
	m->net.log = getenv("SYSHM_LOG_NET") != NULL;
	size_t lanlib_size = net_build_lanlib(&m->net, m->lanlib, sizeof m->lanlib);
	if (((opt->realtime && !opt->no_net) || getenv("SYSHM_NET")) && net_start(&m->net))
		printf("net: Ethernet behind NAT (DHCP 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3)\n");

	struct bootinfo_machine hw = {
		.ram_size = m->ram_size,
		.pvr = CPU_PVR,
		.cpu_hz = CPU_HZ,
		.bus_hz = BUS_HZ,
		.timebase_hz = TIMEBASE_HZ,
		.fb_base = FB_BASE,
		.fb_size = FB_SIZE,
		.fb_width = video_width(&m->video),
		.fb_height = video_height(&m->video),
		.fb_depth = video_bpp(&m->video),
		.fb_row_bytes = video_row_bytes(&m->video),
		.display_ndrv = m->display_ndrv,
		.display_ndrv_size = ndrv_size,
		.disks = m->disks,
		.lanlib = m->lanlib,
		.lanlib_size = lanlib_size,
		.parcels = m->rom->format == ROM_FORMAT_PARCELS ? &m->rom->parcels : NULL,
	};
	memcpy(hw.mac, mac, 6);
	for (unsigned k = 0; k < m->disks; k++) {
		hw.disk_ndrv[k] = m->disk_ndrv[k];
		hw.disk_ndrv_size[k] = disk_ndrv_size[k];
	}
	if (!ndrv_size || bootinfo_build(&m->mem, &m->bootinfo, &hw))
		return -1;
	if (hw.parcels)
		printf("rom: %u parcels, %u device-tree properties from them (tree %u KB)\n", hw.parcels->count,
		       m->bootinfo.parcel_props, m->bootinfo.device_tree_size >> 10);
	bootinfo_level_masks(m->level_mask);
	rtas_init(&m->rtas);
	m->clock = (struct guest_clock){ opt->fixed_time, &m->cpu.tb, TIMEBASE_HZ };
	m->rtas.clock = &m->clock;
	if (opt->nvram && rtas_nvram_load(&m->rtas, opt->nvram) < 0) {
		fprintf(stderr, "%s: not a %u-byte NVRAM image\n", opt->nvram, RTAS_NVRAM_SIZE);
		return -1;
	}

	/* Devices (HAL side of the I/O the ROM was told about) */
	macio_init(&m->macio);
	via_init(&m->via);
	cuda_init(&m->cuda, &m->via);
	m->cuda.clock = &m->clock;
	mmio_add(&m->bus, MACIO_BASE, MACIO_PAGE_SIZE, &macio_mmio_ops, &m->macio, "mac-io");
	mmio_add(&m->bus, MACIO_VIA1, VIA_SIZE, &via_mmio_ops, &m->via, "via1");
	scc_init(&m->scc);
	mmio_add(&m->bus, MACIO_SCC, SCC_SIZE, &scc_mmio_ops, &m->scc, "scc");
	dbdma_init(&m->dbdma, &m->mem, m->ram_size);
	m->dbdma.bus = &m->bus;
	m->dbdma.interrupt = dma_interrupt;
	m->dbdma.interrupt_ctx = &m->macio;
	mmio_add(&m->bus, MACIO_DBDMA, DBDMA_SIZE, &dbdma_mmio_ops, &m->dbdma, "dbdma");
	awacs_init(&m->awacs, &m->dbdma, TIMEBASE_HZ);
	m->awacs.log = getenv("SYSHM_LOG_SOUND") != NULL;
	m->awacs.sink = play_sound;
	m->awacs.sink_ctx = m;
	if (!m->wav && getenv("SYSHM_SOUND_WAV") && (m->wav = fopen(getenv("SYSHM_SOUND_WAV"), "wb"))) {
		uint8_t h[44];
		wav_header(h, 0, 44100);
		fwrite(h, 1, sizeof h, m->wav);
	}
	mmio_add(&m->bus, MACIO_SOUND, AWACS_SIZE, &awacs_mmio_ops, &m->awacs, "sound");
	ohci_init(&m->ohci, &m->mem);
	m->ohci.log = getenv("SYSHM_LOG_USB") != NULL;
	mmio_add(&m->bus, USB_BASE, OHCI_SIZE, &ohci_mmio_ops, &m->ohci, "usb");

	/* PCI configuration space: the functions the device tree lists */
	pci_init(&m->pci);
	m->pci.log = getenv("SYSHM_LOG_PCI") != NULL;
	pci_add(&m->pci, 0x00, PCI_VENDOR_MOTOROLA, PCI_DEVICE_GRACKLE, PCI_CLASS_HOST_BRIDGE, 0x40, 0, 0);
	pci_add(&m->pci, PCI_DEVFN_DISPLAY, PCI_VENDOR_SYSHM, PCI_DEVICE_DISPLAY, PCI_CLASS_DISPLAY, 1, FB_BASE, FB_SIZE);
	for (unsigned k = 0; k < m->disks; k++)
		pci_add(&m->pci, (uint8_t)(PCI_DEVFN_DISK + 8 * k), PCI_VENDOR_SYSHM, PCI_DEVICE_DISK, PCI_CLASS_DISK, 1, 0, 0);
	pci_add(&m->pci, PCI_DEVFN_USB, OHCI_PCI_VENDOR, OHCI_PCI_DEVICE, OHCI_PCI_CLASS, OHCI_PCI_REVISION, USB_BASE, OHCI_SIZE);
	pci_add(&m->pci, PCI_DEVFN_MACIO, PCI_VENDOR_APPLE, PCI_DEVICE_HEATHROW, PCI_CLASS_MACIO, 0, MACIO_BASE, MACIO_SIZE);
	mmio_add(&m->bus, PCI_CONFIG_ADDR, PCI_CONFIG_SIZE, &pci_addr_mmio_ops, &m->pci, "pci config address");
	mmio_add(&m->bus, PCI_CONFIG_DATA, PCI_CONFIG_SIZE, &pci_data_mmio_ops, &m->pci, "pci config data");
	m->rtas.pci = &m->pci;
	m->mp_kernel = m->rom->format == ROM_FORMAT_PARCELS;
	if (m->mp_kernel) {
		nvram_init(&m->nvram, m->rtas.nvram, RTAS_NVRAM_SIZE);
		mmio_add(&m->bus, MACIO_NVRAM, MACIO_NVRAM_SIZE, &nvram_mmio_ops, &m->nvram, "nvram");
		m->mp.log = getenv("SYSHM_LOG_MP") != NULL;
		mp_init(&m->mp, TIMEBASE_HZ);
		/* the areas a NanoKernel 2 makes from the page map: Mac OS's RAM, then the rest */
		struct nk_region regions[NK_MAX_REGIONS];
		int n = nk_user_regions(m->rom, &m->ci, regions, NK_MAX_REGIONS);
		mp_add_area(&m->mp, 0, m->ram_size, MP_AREA_READ_WRITE, true);
		for (int k = 0; k < n; k++)
			mp_add_area(&m->mp, regions[k].logical, regions[k].size,
				    (regions[k].writable ? MP_AREA_READ_WRITE : MP_AREA_READ_ONLY) |
				    (regions[k].kind == NK_REGION_IO ? MP_AREA_IO : 0), false);
		/* a new task's entry point returns into the kernel's data page, which can't run: that ends it (see nk_boot()) */
		m->mp.task_return = ci->la_kernel_data;
		m->mp.emulator_data = mem_ptr(&m->mem, ci->la_emulator_data);
		m->mp.ecb = ci->ecb_offset;
		/*
		 * NanoKernel 2 starts its free page list with 18 pages and one per 8 MB
		 * of RAM, from outside Mac OS's (for its own pool, and the page
		 * ager's reserve). syshm's kernel needs none of them.
		 */
		m->mp.free_pages = 18 + (m->ram_size >> MEM_PAGE_SHIFT) / 2048;
	}
	init_info_records(m);
	kdp_sync(m);

	/* Low memory: zero the first 8 KB, then apply ConfigInfo's address/value list */
	memset(mem_ptr(&m->mem, 0), 0, 0x2000);
	for (uint32_t o = ci->mac_lowmem_init_offset; NK_CONFIGINFO_OFFSET + o + 8 <= ROM_IMAGE_SIZE; o += 8) {
		uint32_t addr = ci32(m->rom, o);
		if (addr == 0)
			break;
		if (addr + 4 <= m->ram_size)
			mem_w32(&m->mem, addr, ci32(m->rom, o + 4));
	}
	/* The Trampoline points the VIA low-memory global at this machine's VIA1. */
	mem_w32(&m->mem, LOWMEM_VIA, MACIO_VIA1);
	fill_page_holes(m);

	/* EmulatorData: bootstrap version string and the emulator's context block,
	 * prepared as if returning from an exception into the emulator's entry point */
	uint32_t edp = ci->la_emulator_data;
	uint32_t ecb = edp + ci->ecb_offset;
	uint32_t entry = ci->la_emulator_code + ci->emulator_entry_offset;
	uint32_t traps = ci->la_emulator_code + ci->kernel_trap_table_offset;

	mem_load(&m->mem, edp + ci->boot_version_offset,
		 m->rom->image + NK_CONFIGINFO_OFFSET + CI_BOOTSTRAP_VERSION, 16);
	mem_w32(&m->mem, edp + ci->ipl_value_offset, 0x80000000u);
	mem_w32(&m->mem, ecb + CB_FAULT_SRC_PC, entry);
	mem_w32(&m->mem, ecb + CB_FAULT_SRC_R3, edp);
	mem_w32(&m->mem, ecb + CB_FAULT_SRC_R4, ci->la_dispatch_table);
	mem_w32(&m->mem, ecb + CB_INTRA_HANDLER_RETURN, traps);

	ppc_init(&m->cpu, &m->mem);
	if (!opt->interp && !(m->cpu.jit = jit_create(&m->mem)))
		fprintf(stderr, "no JIT on this host: interpreting\n");
	m->cpu.pc = entry;
	m->cpu.gpr[3] = edp;
	m->cpu.gpr[4] = ci->la_dispatch_table;
	m->cpu.trace_reg = opt->trace_reg;
	m->cpu.trace_lo = opt->trace_lo;
	m->cpu.trace_hi = opt->trace_hi;
	m->cpu.trace_insn = opt->trace_insn;
	m->context = m->system_cb = ecb;
	m->system = true;
	return 0;
}

static uint64_t host_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Timebase ticks in `ns` nanoseconds of host time. */
static uint64_t ns_to_tb(uint64_t ns)
{
	return ns / 1000000000u * TIMEBASE_HZ + ns % 1000000000u * TIMEBASE_HZ / 1000000000u;
}

static uint64_t tb_to_ns(uint64_t tb)
{
	return tb / TIMEBASE_HZ * 1000000000u + tb % TIMEBASE_HZ * 1000000000u / TIMEBASE_HZ;
}

/*
 * Moves guest time on by `ticks` at once, as if the processor had waited
 * that long. In real time, guest time then runs ahead of the host clock;
 * idle_wait() makes it up.
 */
static void pass_guest_time(struct machine *m, uint64_t ticks)
{
	m->cpu.tb += ticks;
	if (m->clock_mips)
		m->clock_tb += ticks;
	if (m->realtime)
		m->lead_tb += ticks;
	m->dev_dirty = true;
}

#define IDLE_MAX_NS 50000000u          /* longest sleep while the guest idles (50 ms) */
#define SOUND_WAKE_NS 4000000u          /* ... and while sound plays (4 ms) */
#define NET_WAKE_NS 4000000u            /* ... and with a host network (4 ms) */
#define NET_POLL_NS 1000000u            /* the host network is serviced every millisecond while running */
#define TRAP_SYNCH_IDLE_TIME 0xABF7u    /* the Process Manager's "nothing to run" call */
#define PPC_CRSET_CR1LT 0x4C842242u     /* crset cr1lt */
#define PPC_LHA_R4_R24 0xA8980000u      /* lha r4,0(r24) */

/*
 * The 68k emulator runs _SynchIdleTime through its dispatch table entry
 * for the trap word when it interprets it, but code its dynamic recompiler
 * has translated calls a trap entry of its own for every A-line trap, with
 * r24 pointing at the trap word. That entry starts like the table's A-line
 * entries (with the load of the emulator global they all begin with) after
 * a crset, and then loads the trap word. 0 if this emulator has no such
 * entry.
 */
static uint32_t recompiled_trap_entry(const struct machine *m)
{
	const struct nk_config *ci = &m->ci;
	uint32_t first = mem_r32(&m->mem, ci->la_dispatch_table + 8u * TRAP_SYNCH_IDLE_TIME);
	for (uint32_t a = ci->la_emulator_code + 4; a + 8 <= ci->la_emulator_code + ci->emulator_code_size; a += 4)
		if (mem_r32(&m->mem, a) == first && mem_r32(&m->mem, a + 4) == PPC_LHA_R4_R24 &&
		    mem_r32(&m->mem, a - 4) == PPC_CRSET_CR1LT)
			return a - 4;
	return 0;
}

/*
 * Fast start: in real time, from startup until the Finder is up, guest
 * time runs FAST_START_SPEED times as fast as the host clock. Startup waits
 * out timeouts by the guest's clock (AppleTalk on Ethernet looking for
 * other machines, Apple Remote Access making sure of its name, USB, DHCP),
 * mostly in busy loops that don't look idle, and so they pass in a
 * fraction of the time; meanwhile the guest sees a processor that much
 * slower. The Finder is up once it has run (the Process Manager names the
 * current application in CurApName, which stays unset until the first one
 * runs) and the guest has then idled for FAST_START_QUIET of guest time
 * without disk transfers or busy stretches: the Process Manager first idles
 * once the Finder runs, and idles all the time once nothing has anything
 * left to do. (Network traffic doesn't count: what's left may be looking
 * for printers in the background.) Input ends it too once the Finder has
 * run; before, it only holds it back for a while (see fast_start_input()).
 * FAST_START_MAX of guest time after the Finder has
 * run ends it as well (the Mac OS Setup Assistant never idles), and
 * FAST_START_EARLY_MAX from the start if the Finder never runs (startup
 * itself may wait out minutes of guest time first). Guest time is then
 * ahead of the host clock by what it gained, which idle_wait() makes up as
 * it does for disk transfers. Not while sound plays.
 */
#define FAST_START_SPEED 10u
#define FAST_START_QUIET_TB (TIMEBASE_HZ * 3ull / 2)   /* 1.5 s */
#define FAST_START_BUSY_TB (TIMEBASE_HZ / 4ull)         /* longer between idle calls: busy, not idle */
#define FAST_START_MAX_TB (TIMEBASE_HZ * 120ull)        /* after the Finder has run */
#define FAST_START_EARLY_MAX_TB (TIMEBASE_HZ * 600ull)  /* before */

#define LM_FINDER_NAME 0x02E0u          /* Str15: the Finder's name, from the boot blocks */
#define LM_DOUBLE_TIME 0x02F0u          /* long: the double-click time, in ticks */
#define LM_CUR_AP_NAME 0x0910u          /* Str31: the current application's */

static void fast_start_begin(struct machine *m)
{
	m->fast_start = true;
	m->fast_idled = false;
	m->fast_finder = false;
	m->fast_pause_ns = 0;
	m->fast_keys[0] = m->fast_keys[1] = 0;
	m->fast_button = false;
	m->fast_begin_tb = m->cpu.tb;
	m->fast_begin_ns = m->fast_last_ns = host_ns();
}

static void fast_start_end(struct machine *m, const char *why)
{
	if (!m->fast_start)
		return;
	m->fast_start = false;
	printf("fast start: over (%s) after %.1f s, at %.1f s of guest time, %.1f s ahead of the host clock\n", why,
	       (double)(host_ns() - m->fast_begin_ns) / 1e9, (double)(m->cpu.tb - m->fast_begin_tb) / TIMEBASE_HZ,
	       (double)m->lead_tb / TIMEBASE_HZ);
}

/* Whether the Finder is the current application */
static bool finder_current(const struct machine *m)
{
	if (!mem_ok(&m->mem, LM_FINDER_NAME, 16, MEM_R) || !mem_ok(&m->mem, LM_CUR_AP_NAME, 32, MEM_R))
		return false;
	const uint8_t *finder = mem_ptr(&m->mem, LM_FINDER_NAME), *cur = mem_ptr(&m->mem, LM_CUR_AP_NAME);
	return finder[0] && finder[0] <= 15 && cur[0] == finder[0] && !memcmp(cur + 1, finder + 1, finder[0]);
}

/* At each sync point: guest time gains on the host clock, unless input holds it back */
static void fast_start_step(struct machine *m, uint64_t now)
{
	uint64_t d = now - m->fast_last_ns;
	m->fast_last_ns = now;
	if (!awacs_playing(&m->awacs) && now >= m->fast_pause_ns)
		m->lead_tb += ns_to_tb(d) * (FAST_START_SPEED - 1);
	if (!m->fast_finder && finder_current(m)) {
		m->fast_finder = true;
		m->fast_finder_tb = m->cpu.tb;
	}
	if (m->fast_finder ? m->cpu.tb - m->fast_finder_tb >= FAST_START_MAX_TB :
			     m->cpu.tb - m->fast_begin_tb >= FAST_START_EARLY_MAX_TB)
		fast_start_end(m, m->fast_finder ? "time limit" : "time limit, without the Finder");
}

/* At each idle call: whether the Finder is up */
static void fast_start_idle(struct machine *m)
{
	uint64_t disk = 0;
	for (unsigned k = 0; k < m->disks; k++)
		disk += m->disk[k].bytes_read + m->disk[k].bytes_written;
	bool busy = m->cpu.tb - m->fast_idle_tb > FAST_START_BUSY_TB;  /* (since the last idle call) */
	if (!m->fast_idled || busy || disk != m->fast_disk || !m->fast_finder) {
		m->fast_idled = true;
		m->fast_disk = disk;
		m->fast_quiet_tb = m->cpu.tb;
	} else if (m->cpu.tb - m->fast_quiet_tb >= FAST_START_QUIET_TB) {
		fast_start_end(m, "the Finder is up");
	}
	m->fast_idle_tb = m->cpu.tb;
}

/* The guest's double-click time, in host ns (Mac OS's default if it's unset) */
static uint64_t double_click_ns(const struct machine *m)
{
	uint32_t ticks = mem_ok(&m->mem, LM_DOUBLE_TIME, 4, MEM_R) ? mem_r32(&m->mem, LM_DOUBLE_TIME) : 0;
	if (!ticks || ticks > 255)
		ticks = 32;
	return ticks * 1000000000ull / 60;     /* (about 60 ticks a second) */
}

/*
 * Input: a key (its ADB code) or the mouse button (-1) went down or up.
 * Someone is there, and guest time keeps the host clock's pace for them,
 * since Mac OS times clicks, double-clicks and key repeat in its own ticks.
 * Once the Finder has run, for good. Before (a dialog during startup, such
 * as the one after a crash, or keys held down), only while a key or the
 * button is down and for the double-click time after the last is let go
 * (a second click then still makes a double-click), and then startup goes
 * on faster.
 */
static void fast_start_input(struct machine *m, int key, bool down)
{
	if (!m->fast_start)
		return;
	if (m->fast_finder) {
		fast_start_end(m, "input");
		return;
	}
	if (key < 0)
		m->fast_button = down;
	else if (down)
		m->fast_keys[key >> 6 & 1] |= 1ull << (key & 63);
	else
		m->fast_keys[key >> 6 & 1] &= ~(1ull << (key & 63));
	uint64_t now = host_ns();
	if (now >= m->fast_pause_ns)
		printf("fast start: held back by input (the Finder hasn't run yet)\n");
	bool held = m->fast_button || m->fast_keys[0] || m->fast_keys[1];
	m->fast_pause_ns = held ? UINT64_MAX : now + double_click_ns(m);
}

/*
 * The guest has nothing to do until an interrupt arrives (the Process
 * Manager found no process to run, or the system asked to nap), or until
 * guest time reaches `deadline` (UINT64_MAX: none). In real time, sleep
 * until the next device event: the next VIA timer interrupt, the next
 * window frame, or host input, whichever is first. Interrupts only come
 * from the VIA timers and from Cuda, which only talks after input. With
 * Multiprocessing Services, a timer's, timeout's or delay's end counts too.
 *
 * Guest time that disk transfers put ahead of the host clock is made up
 * here, while nothing shows it: guest time runs at 3/4 speed while the guest
 * idles, unless sound plays.
 */
#define IDLE_CATCH_UP 4u                /* (a quarter of each sleep) */

/* Host time at which guest time reaches `ticks` of a `hz` clock (now, if it has already). */
static uint64_t host_ns_at(const struct machine *m, uint64_t ticks, uint64_t hz)
{
	uint64_t at = m->start_ns + ticks / hz * 1000000000u + ticks % hz * 1000000000u / hz;
	uint64_t lead = tb_to_ns(m->lead_tb);
	return at > lead ? at - lead : 0;
}

static void idle_wait(struct machine *m, uint64_t deadline)
{
	m->idle_calls++;
	if (m->fast_start)
		fast_start_idle(m);
	if (!m->idle_sleep || m->ipl)
		return;
	if (m->mp_kernel && mp_next_due(&m->mp) < deadline)
		deadline = mp_next_due(&m->mp);
	uint64_t now = host_ns(), until = now + IDLE_MAX_NS;
	uint64_t ev = via_next_event(&m->via);
	if (ev != UINT64_MAX && host_ns_at(m, ev, VIA_HZ) < until)
		until = host_ns_at(m, ev, VIA_HZ);
	ev = ohci_next_event(&m->ohci);
	if (ev != UINT64_MAX && host_ns_at(m, ev, OHCI_BIT_HZ) < until)
		until = host_ns_at(m, ev, OHCI_BIT_HZ);
	if (deadline != UINT64_MAX && host_ns_at(m, deadline, TIMEBASE_HZ) < until)
		until = host_ns_at(m, deadline, TIMEBASE_HZ);
	bool sound = awacs_playing(&m->awacs), fast = m->fast_start && !sound && now >= m->fast_pause_ns;
	if (fast && until > now)        /* (see fast_start_step()) */
		until = now + (until - now) / FAST_START_SPEED;
	if (m->display && m->next_frame_ns < until)
		until = m->next_frame_ns;
	if (sound && now + SOUND_WAKE_NS < until)
		until = now + SOUND_WAKE_NS;   /* (the sound DMA moves only as time is seen to pass) */
	if (m->net.backend && now + NET_WAKE_NS < until)
		until = now + NET_WAKE_NS;     /* (to see the host network's traffic) */
	if (until <= now + 20000)       /* not worth a system call */
		return;
	if (m->display && until - now >= 1000000) {
		display_wait(m->display, (unsigned)((until - now) / 1000000));
	} else {
		struct timespec ts = { 0, (long)(until - now) };
		nanosleep(&ts, NULL);
	}
	uint64_t slept = host_ns() - now;
	m->idle_ns += slept;
	if (!m->fast_start && !sound) {
		uint64_t catch_up = ns_to_tb(slept / IDLE_CATCH_UP);
		m->lead_tb -= catch_up < m->lead_tb ? catch_up : m->lead_tb;
	}
}

/* Low-memory mouse globals (Points are v, h) */
#define LM_MTEMP 0x0828u
#define LM_RAW_MOUSE 0x082Cu
#define LM_CRSR_NEW 0x08CEu
#define LM_CRSR_COUPLE 0x08CFu
#define LM_CRSR_STATE 0x08D0u           /* HideCursor's nesting: 0 when the cursor is shown */

#define CAPTURE_AFTER_NS 300000000u

/*
 * Puts the cursor where the host pointer is, the way SheepShaver and
 * Basilisk II do it: set the raw mouse position and flag it as new.
 */
static void move_mouse_to(struct machine *m, int x, int y)
{
	int w = (int)video_width(&m->video), h = (int)video_height(&m->video);
	x = x < 0 ? 0 : x >= w ? w - 1 : x;
	y = y < 0 ? 0 : y >= h ? h - 1 : y;
	mem_w16(&m->mem, LM_MTEMP, (uint16_t)y);
	mem_w16(&m->mem, LM_MTEMP + 2, (uint16_t)x);
	mem_w16(&m->mem, LM_RAW_MOUSE, (uint16_t)y);
	mem_w16(&m->mem, LM_RAW_MOUSE + 2, (uint16_t)x);
	mem_w8(&m->mem, LM_CRSR_NEW, mem_r8(&m->mem, LM_CRSR_COUPLE));
}

/*
 * Games hide Mac OS's cursor and read the mouse's motion (InputSprocket
 * reads it from the ADB mouse), which putting the cursor where the host
 * pointer is doesn't give them. So once the cursor has been hidden a while
 * (not just around some drawing), the host pointer is captured and its
 * motion goes to the ADB mouse, as a real mouse's would; when the cursor
 * comes back, so does the pointer. Control-Option lets it go until a click
 * in the window (or the cursor showing again).
 */
static void follow_cursor(struct machine *m, uint64_t now)
{
	bool hidden = (int16_t)mem_r16(&m->mem, LM_CRSR_STATE) < 0;
	if (!hidden) {
		m->cursor_hidden_ns = 0;
		m->mouse_released = false;
	} else if (!m->cursor_hidden_ns) {
		m->cursor_hidden_ns = now;
	}
	bool capture = hidden && now - m->cursor_hidden_ns >= CAPTURE_AFTER_NS && !m->scripted && !m->mouse_released &&
		       !m->paused;
	if (capture != m->mouse_captured) {
		m->mouse_captured = capture;
		display_capture_mouse(m->display, capture);
		printf("mouse: %s\n", capture ? "captured (the guest hid its cursor; Control-Option releases it)" :
		       m->mouse_released ? "released (click in the window to capture it again)" : "released");
		fflush(stdout);
	}
}

static void display_input(struct machine *m, uint64_t now);
static void stay_paused(struct machine *m);

/* What the menu shows: whether the mouse will be captured when the guest resumes, and news */
static void menu_state(struct machine *m)
{
	enum ui_mouse mouse = !m->cursor_hidden_ns ? UI_MOUSE_FOLLOWS :
			      m->mouse_released ? UI_MOUSE_RELEASED : UI_MOUSE_CAPTURED;
	display_menu_state(m->display, mouse, m->menu_status);
}

/* The screen as a PNG on the Desktop (or here), named for when it was taken */
static void screenshot(struct machine *m)
{
	const char *home = getenv("HOME");
	char dir[256] = ".", stamp[64], path[400];
	struct stat st;
	if (home) {
		snprintf(dir, sizeof dir, "%s/Desktop", home);
		if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
			snprintf(dir, sizeof dir, ".");
	}
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(stamp, sizeof stamp, "%Y-%m-%d at %H.%M.%S", &tm);
	snprintf(path, sizeof path, "%s/syshm %s.png", dir, stamp);
	for (int n = 2; stat(path, &st) == 0 && n < 100; n++)
		snprintf(path, sizeof path, "%s/syshm %s (%d).png", dir, stamp, n);
	bool home_dir = home && strncmp(path, home, strlen(home)) == 0;
	if (video_screenshot(&m->video, path)) {
		snprintf(m->menu_status, sizeof m->menu_status, "Couldn't save %s%s", home_dir ? "~" : "",
			 path + (home_dir ? strlen(home) : 0));
		fprintf(stderr, "cannot write %s\n", path);
	} else {
		snprintf(m->menu_status, sizeof m->menu_status, "Saved %s%s", home_dir ? "~" : "",
			 path + (home_dir ? strlen(home) : 0));
		printf("screen saved to %s\n", path);
	}
}

/* What's chosen in the menu (the rest of it: Resume closes it, and fullscreen is the window's) */
static void menu_action(struct machine *m, enum ui_action action, uint64_t now)
{
	switch (action) {
	case UI_RESTART:
	case UI_SHUT_DOWN:
		if (action == UI_RESTART)
			m->restart = true;
		else
			m->shut_down = m->quit = true;
		m->paused = false;
		display_menu(m->display, false);
		follow_cursor(m, now);
		break;
	case UI_SCREENSHOT:
		screenshot(m);
		break;
	case UI_CAPTURE_MOUSE:
	case UI_RELEASE_MOUSE:
		m->mouse_released = action == UI_RELEASE_MOUSE;
		break;
	default:
		break;
	}
	menu_state(m);
}

/*
 * The status bar's lights. A device's lights for LED_HOLD_NS whenever it
 * moved data since the last frame (amber if it wrote), so that a single
 * request shows; a CD drive being asked whether there's a disc doesn't
 * count. The processor's is as bright as the share of the time the guest
 * didn't nap (without idle sleep the host never naps, and it's always lit),
 * and softly blue for the rest, while it naps.
 * Its speed is that of the instructions it ran: how many million a second
 * of the time spent running them (not drawing, on devices or napping),
 * timed in a random one of every RUN_SAMPLE runs (a clock read costs as
 * much as a short run) and worked out about once a second, if the guest
 * was busy enough for it to mean something; otherwise it stands. What the
 * devices read and wrote a second is worked out then too. Paused, nothing's
 * lit or moving.
 */
#define LED_HOLD_NS 100000000u
#define SPEED_EVERY_NS 1000000000u
#define SPEED_BUSY_NS 100000000u        /* (busy for less of a second than this: the speed stands) */
#define RUN_SAMPLE 32u

static void status_lights(struct machine *m, uint64_t now, bool paused)
{
	struct ui_status s;
	memset(&s, 0, sizeof s);
	/* bytes each device has read (or received) and written (or sent) */
	uint64_t in[UI_LEDS] = { 0 }, out[UI_LEDS] = { 0 };
	bool present[UI_LEDS] = { [UI_LED_CPU] = true, [UI_LED_NET] = m->net.backend != NULL };
	for (unsigned k = 0; k < m->disks; k++) {
		enum ui_led led = m->disk[k].cdrom ? UI_LED_CDROM : UI_LED_DISK;
		in[led] += m->disk[k].bytes_read;
		out[led] += m->disk[k].bytes_written;
		present[led] = true;
	}
	in[UI_LED_NET] = m->net.bytes_received;
	out[UI_LED_NET] = m->net.bytes_sent;
	for (int k = UI_LED_DISK; k < UI_LEDS; k++) {
		if (in[k] + out[k] != m->led_count[k])
			m->led_until[k] = now + LED_HOLD_NS;
		/* (the network's stays green: receiving, it sends all the while too) */
		if (out[k] != m->led_written[k] && k != UI_LED_NET)
			m->led_amber_until[k] = now + LED_HOLD_NS;
		m->led_count[k] = in[k] + out[k];
		m->led_written[k] = out[k];
		s.light[k].level = now < m->led_until[k] && !paused ? 255 : 0;
		s.light[k].write = now < m->led_amber_until[k];
		s.light[k].absent = !present[k];
	}

	if (paused) {
		m->cpu_busy = 0;
		m->led_ns = 0;
	} else if (m->led_ns && now > m->led_ns) {
		uint64_t slept = m->idle_sleep ? m->idle_ns - m->led_idle_ns : 0, d = now - m->led_ns;
		uint64_t busy = slept < d ? d - slept : 0;
		m->busy_ns += busy;
		m->cpu_busy += ((float)busy / (float)d - m->cpu_busy) / 4;
		m->led_ns = now;
	} else {
		m->led_ns = now;
	}
	m->led_idle_ns = m->idle_ns;
	/* (in sixteenths, not to draw the window again for every flicker); the rest of the time, it naps */
	unsigned busy16 = (unsigned)(m->cpu_busy * 16 + 0.5f);
	s.light[UI_LED_CPU].level = (uint8_t)(busy16 * 255 / 16);
	if (m->idle_sleep && !paused)
		s.light[UI_LED_CPU].nap = (uint8_t)((16 - busy16) * 255 / 16);

	if (paused || !m->speed_ns || now - m->speed_ns >= SPEED_EVERY_NS) {
		if (paused) {
			memset(m->rate_in, 0, sizeof m->rate_in);
			memset(m->rate_out, 0, sizeof m->rate_out);
		} else if (m->speed_ns) {
			double secs = (double)(now - m->speed_ns) / 1e9;
			for (int k = 0; k < UI_LEDS; k++) {
				uint64_t i = in[k] - m->rate_from_in[k], o = out[k] - m->rate_from_out[k];
				m->rate_in[k] = in[k] >= m->rate_from_in[k] ? (uint64_t)((double)i / secs + 0.5) : 0;
				m->rate_out[k] = out[k] >= m->rate_from_out[k] ? (uint64_t)((double)o / secs + 0.5) : 0;
			}
			if (m->busy_ns >= SPEED_BUSY_NS && m->run_ns)
				m->speed_mhz = (uint32_t)(m->run_insns * 1000u / m->run_ns);
		}
		m->speed_ns = paused ? 0 : now;         /* (0: the next frame starts again) */
		m->busy_ns = m->run_ns = m->run_insns = 0;
		memcpy(m->rate_from_in, in, sizeof in);
		memcpy(m->rate_from_out, out, sizeof out);
	}
	memcpy(s.in, m->rate_in, sizeof s.in);
	memcpy(s.out, m->rate_out, sizeof s.out);
	s.mhz = m->speed_mhz;
	display_status(m->display, &s);
}

/*
 * Shows the screen in the window if it has changed (and once a second
 * regardless), and feeds the window's input to the guest; while the menu is
 * open, the guest stays paused here.
 */
static void display_frame(struct machine *m, uint64_t now)
{
	unsigned w = video_width(&m->video), h = video_height(&m->video), bpp = video_bpp(&m->video);
	size_t len = (size_t)video_row_bytes(&m->video) * h;
	const uint8_t *fb = mem_ptr(&m->mem, m->video.fb_base);
	if (len > m->video.fb_size)
		len = m->video.fb_size;
	status_lights(m, now, false);   /* (drawn with the screen, if that's drawn) */
	if (m->redraw || w != m->shown_w || h != m->shown_h || bpp != m->shown_bpp ||
	    now - m->shown_ns >= 1000000000u || memcmp(m->shown_clut, m->video.clut, sizeof m->shown_clut) != 0 ||
	    memcmp(m->shown, fb, len) != 0) {
		m->redraw = false;
		memcpy(m->shown, fb, len);
		memcpy(m->shown_clut, m->video.clut, sizeof m->shown_clut);
		m->shown_w = w;
		m->shown_h = h;
		m->shown_bpp = bpp;
		m->shown_ns = now;
		video_to_rgb(&m->video, m->rgb);
		display_present(m->display, m->rgb, w, h);
	}
	display_update(m->display);
	display_input(m, now);
	if (m->paused)
		stay_paused(m);
}

/*
 * The menu (F12) pauses the guest: nothing but the menu runs until it's
 * closed. In real time, guest time doesn't count the pause (so the Mac's
 * clock is that much behind afterwards).
 */
static void stay_paused(struct machine *m)
{
	uint64_t from = host_ns();
	status_lights(m, from, true);
	while (m->paused && !m->quit) {
		display_refresh(m->display);
		display_wait(m->display, 1000 / 60);
		display_input(m, host_ns());
	}
	m->start_ns += host_ns() - from;
}

/* The window's input, for the guest (or the menu) */
static void display_input(struct machine *m, uint64_t now)
{
	struct display_event ev;
	follow_cursor(m, now);
	while (display_poll(m->display, &ev)) {
		switch (ev.type) {
		case DISPLAY_EVENT_QUIT:
			m->quit = true;
			break;
		case DISPLAY_EVENT_MENU:
			m->paused = !m->paused;
			follow_cursor(m, now);          /* (paused: the pointer goes) */
			m->menu_status[0] = '\0';
			menu_state(m);
			display_menu(m->display, m->paused);
			printf("%s\n", m->paused ? "paused (F12 resumes)" : "resumed");
			fflush(stdout);
			follow_cursor(m, now);          /* (resumed: it may be captured again) */
			break;
		case DISPLAY_EVENT_MENU_ACTION:
			menu_action(m, ev.action, now);
			break;
		/* (a script drives scripted runs: the host's input would change what they do) */
		case DISPLAY_EVENT_KEY:
			if (!m->scripted) {
				adb_key(&m->cuda.adb, ev.adb_key, ev.down);
				fast_start_input(m, ev.adb_key & 0x7F, ev.down);
			}
			break;
		case DISPLAY_EVENT_MOUSE_MOVE:
			if (m->scripted)
				break;
			if (m->mouse_captured)
				adb_mouse_move(&m->cuda.adb, ev.dx, ev.dy);
			else
				move_mouse_to(m, ev.x, ev.y);
			break;
		case DISPLAY_EVENT_MOUSE_BUTTON:
			if (ev.button != 0 || m->scripted)
				break;
			if (m->mouse_released && ev.down) {
				/* a click takes the pointer back (and isn't the guest's, if that captures it) */
				m->mouse_released = false;
				follow_cursor(m, now);
				if (m->mouse_captured)
					break;
			}
			if (!m->mouse_captured)
				move_mouse_to(m, ev.x, ev.y);
			adb_mouse_button(&m->cuda.adb, ev.down);
			fast_start_input(m, -1, ev.down);
			break;
		case DISPLAY_EVENT_RELEASE_MOUSE:
			if (m->mouse_captured) {
				m->mouse_released = true;
				follow_cursor(m, now);
			}
			break;
		case DISPLAY_EVENT_EXPOSE:
			m->redraw = true;
			break;
		case DISPLAY_EVENT_RESIZE:      /* see follow_window(); scripted runs keep their screen */
			m->redraw = true;
			if (ev.x > 0 && ev.y > 0 && !m->scripted) {
				m->want_w = (unsigned)ev.x;
				m->want_h = (unsigned)ev.y;
				m->want_ns = host_ns();
			}
			break;
		default:
			break;
		}
	}
}

/* Scripted input (for headless runs and tests) */
struct script_event {
	uint64_t at;                    /* instruction count, or host ns for a timed script */
	char cmd[16];
	int a, b;
	char arg[256];
};

struct script {
	struct script_event *ev;
	size_t count, next;
	bool timed;                     /* times are host seconds ("12.5s"), for real-time runs */
};

static int script_cmp(const void *x, const void *y)
{
	const struct script_event *a = x, *b = y;
	return a->at < b->at ? -1 : a->at > b->at;
}

static int script_load(struct script *s, const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		perror(path);
		return -1;
	}
	char line[512];
	size_t cap = 0;
	bool counted = false;
	while (fgets(line, sizeof line, f)) {
		struct script_event e = { 0 };
		double at;
		char unit[2] = "";
		if (line[0] == '#')
			continue;
		if (sscanf(line, "%lfs %15s", &at, e.cmd) == 2) {
			unit[0] = 's';
			e.at = (uint64_t)(at * 1e9);
		} else if (sscanf(line, "%lf %15s", &at, e.cmd) == 2) {
			e.at = (uint64_t)at;
		} else {
			continue;
		}
		if (unit[0])
			s->timed = true;
		else
			counted = true;
		const char *rest = strstr(line, e.cmd) + strlen(e.cmd);
		if (strcmp(e.cmd, "screenshot") == 0)
			sscanf(rest, " %255s", e.arg);
		else if (strcmp(e.cmd, "key") == 0)
			sscanf(rest, " %x %255s", (unsigned *)&e.a, e.arg);
		else
			sscanf(rest, " %d %d", &e.a, &e.b);
		if (strcmp(e.cmd, "click") == 0 || strcmp(e.cmd, "dblclick") == 0) {
			/* move, then press once the cursor has followed (a few VBLs); presses are
			 * 0.2 s and 0.1 s apart in instruction time (16.6 million per second) */
			bool dbl = strcmp(e.cmd, "dblclick") == 0;
			static const uint64_t insns[4] = { 5000000u, 8000000u, 10000000u, 13000000u };
			uint64_t times[4];
			for (unsigned k = 0; k < 4; k++)       /* (in seconds: the same, at 16.6 million per second) */
				times[k] = unit[0] ? insns[k] * 1000000000u / TIMEBASE_HZ : insns[k];
			if (s->count + 5 > cap) {
				cap = cap ? 2 * cap + 5 : 16;
				s->ev = realloc(s->ev, cap * sizeof *s->ev);
			}
			strcpy(e.cmd, "move");
			s->ev[s->count++] = e;
			for (unsigned k = 0; k < (dbl ? 4u : 2u); k++) {
				struct script_event b = { .at = e.at + times[k] };
				strcpy(b.cmd, k % 2 ? "up" : "down");
				s->ev[s->count++] = b;
			}
			continue;
		}
		if (s->count + 1 > cap) {
			cap = cap ? 2 * cap : 16;
			s->ev = realloc(s->ev, cap * sizeof *s->ev);
		}
		s->ev[s->count++] = e;
	}
	fclose(f);
	if (counted && s->timed) {
		fprintf(stderr, "%s: times are either all instruction counts or all seconds\n", path);
		return -1;
	}
	if (s->count)
		qsort(s->ev, s->count, sizeof *s->ev, script_cmp);
	return 0;
}

static void script_run(struct machine *m, struct script *s)
{
	uint64_t now = s->timed ? host_ns() - m->start_ns : m->cpu.icount;
	while (s->next < s->count && s->ev[s->next].at <= now) {
		const struct script_event *e = &s->ev[s->next++];
		if (strcmp(e->cmd, "move") == 0) {
			move_mouse_to(m, e->a, e->b);
		} else if (strcmp(e->cmd, "motion") == 0) {    /* as the mouse moved, captured (see follow_cursor()) */
			adb_mouse_move(&m->cuda.adb, e->a, e->b);
		} else if (strcmp(e->cmd, "press") == 0) {
			move_mouse_to(m, e->a, e->b);
			adb_mouse_button(&m->cuda.adb, true);
			fast_start_input(m, -1, true);
		} else if (strcmp(e->cmd, "down") == 0) {
			adb_mouse_button(&m->cuda.adb, true);
			fast_start_input(m, -1, true);
		} else if (strcmp(e->cmd, "up") == 0) {
			adb_mouse_button(&m->cuda.adb, false);
			fast_start_input(m, -1, false);
		} else if (strcmp(e->cmd, "key") == 0) {
			bool down = strcmp(e->arg, "up") != 0;
			adb_key(&m->cuda.adb, (uint8_t)e->a, down);
			fast_start_input(m, e->a & 0x7F, down);
			if (!e->arg[0]) {       /* no direction: press and release */
				adb_key(&m->cuda.adb, (uint8_t)e->a, false);
				fast_start_input(m, e->a & 0x7F, false);
			}
		} else if (strcmp(e->cmd, "clock") == 0) {
			/*
			 * From here on, guest time passes at a guest processor speed of
			 * this many million instructions per second (instead of one
			 * timebase tick each), so that what guest time paces (sound,
			 * timer tasks, a game's frame timing) costs what it does in real
			 * time at that speed, while runs stay exactly reproducible.
			 */
			if (!m->realtime && e->a > 0) {
				m->clock_mips = (unsigned)e->a;
				m->clock_tb = m->cpu.tb;
				m->clock_icount = m->cpu.icount;
				m->cpu.tb_step = 0;
			}
		} else if (strcmp(e->cmd, "resize") == 0) {    /* as if the window were resized */
			if (e->a > 0 && e->b > 0) {
				m->want_w = (unsigned)e->a;
				m->want_h = (unsigned)e->b;
				m->want_ns = 0;
			}
		} else if (strcmp(e->cmd, "screenshot") == 0) {
			if (video_screenshot(&m->video, e->arg))
				fprintf(stderr, "cannot write %s\n", e->arg);
			else
				printf("screen saved to %s at instruction %llu\n", e->arg,
				       (unsigned long long)m->cpu.icount);
		} else {
			fprintf(stderr, "input script: unknown command '%s'\n", e->cmd);
		}
	}
}

/*
 * The screen follows the window's size when the guest tools' helper runs
 * (it switches modes when it next looks); otherwise the window scales it.
 * A size is asked for once the window has kept it for a moment: while it's
 * being dragged, the window scales the screen (a mode switch, with the
 * Finder laying out the desktop again, is no quick thing). Each size is
 * asked for once, so a mode chosen in Mac OS afterwards stands (and the
 * window follows it).
 */
#define RESIZE_SETTLE_NS 300000000u

static void follow_window(struct machine *m)
{
	if (!m->want_w || !m->helper.running || m->video.switching >= 0 ||
	    (m->want_ns && host_ns() - m->want_ns < RESIZE_SETTLE_NS))
		return;
	video_request_size(&m->video, m->want_w, m->want_h);
	m->want_w = m->want_h = 0;
}

/* Device time: VIA ticks from the timebase; DMA channels that had more to do continue. */
static void update_device_time(struct machine *m)
{
	via_set_time(&m->via, m->cpu.tb * VIA_HZ / TIMEBASE_HZ);
	ohci_set_time(&m->ohci, m->cpu.tb / TIMEBASE_HZ * OHCI_BIT_HZ + m->cpu.tb % TIMEBASE_HZ * OHCI_BIT_HZ / TIMEBASE_HZ);
	if (m->dbdma.poll)
		dbdma_poll(&m->dbdma);
	awacs_set_time(&m->awacs, m->cpu.tb);
}

/*
 * The first timebase at which devices change by themselves: an enabled VIA
 * timer runs out or a USB frame with an enabled interrupt starts, or now if
 * sound DMA moves or a DMA channel is to continue.
 * Until then, and unless something touched them, updating the devices and
 * interrupts would change nothing, so exits skip it.
 */
static uint64_t device_next_tb(const struct machine *m)
{
	if (m->dbdma.poll || awacs_moving(&m->awacs))
		return 0;
	uint64_t next = UINT64_MAX, ev = via_next_event(&m->via), usb = ohci_next_event(&m->ohci);
	if (ev < UINT64_MAX / TIMEBASE_HZ)
		next = (ev * TIMEBASE_HZ + VIA_HZ - 1) / VIA_HZ;        /* (the first whose VIA time reaches it) */
	if (usb != UINT64_MAX) {
		usb = usb / OHCI_BIT_HZ * TIMEBASE_HZ + (usb % OHCI_BIT_HZ * TIMEBASE_HZ + OHCI_BIT_HZ - 1) / OHCI_BIT_HZ;
		if (usb < next)
			next = usb;
	}
	return next;
}

/*
 * Multiprocessing Services' tasks (mp_kernel) take turns on the processor:
 * the current one's registers are in m->cpu, the others' saved in m->mp
 * (see switch_task()). The blue task's NanoKernel 1 state (its running
 * context, flags and enables) stays in m while another task runs, and
 * nothing changes it then but interrupts posted to its System context.
 */
static bool blue_running(const struct machine *m)
{
	return !m->mp_kernel || m->mp.current == m->mp.blue_task;
}

/* Where the System context's CR is while it runs (NULL: in its context block) */
static uint32_t *system_cr(struct machine *m)
{
	if (!m->system)
		return NULL;
	return blue_running(m) ? &m->cpu.cr : &mp_regs(&m->mp, m->mp.blue_task)->cr;
}

/*
 * The priority NanoKernel 2 gives the blue task, while it runs, for its 68k
 * interrupt mask, which the emulator keeps in its context's r25
 * (SchFiddlePriorityShifty): critical with all interrupts masked (7),
 * latency-protected at or above the emulator's level (the low bits of its
 * context block's PriorityShifty word; 2 if they're 0), else as it is.
 * Boosted, or interrupted (update_interrupts(), post_software_interrupt()),
 * the word gets SHIFTY_YIELD, which has the emulator yield (MPYieldWithHint,
 * hint 1) when it next unmasks all interrupts; once it has yielded or napped
 * (SHIFTY_YIELDED), the next evaluation leaves its priority as it is.
 */
#define SHIFTY_YIELD 0x100u
#define SHIFTY_YIELDED 0x80000000u
#define SHIFTY_CLEARED 0x300u           /* (when it yields or naps) */

static void set_shifty(struct machine *m, uint32_t set, uint32_t clear)
{
	uint32_t at = m->system_cb + CB_PRIORITY_SHIFTY;
	mem_w32(&m->mem, at, (mem_r32(&m->mem, at) & ~clear) | set);
}

static unsigned blue_boost(struct machine *m)
{
	if (!blue_running(m))
		return MP_AS_IS;
	uint32_t shifty = mem_r32(&m->mem, m->system_cb + CB_PRIORITY_SHIFTY);
	if (shifty & SHIFTY_YIELDED) {
		set_shifty(m, 0, SHIFTY_YIELDED);
		return MP_AS_IS;
	}
	uint32_t r25 = m->system ? m->cpu.gpr[25] : mem_r32(&m->mem, m->system_cb + CB_GPR + 8 * 25 + 4);
	unsigned mask = r25 & 7, level = shifty & 0xF, boost;
	if (mask == 7)
		boost = MP_CRITICAL;
	else if (mask >= (level ? level : 2))
		boost = MP_LATENCY_PROTECTED;
	else
		return MP_AS_IS;
	set_shifty(m, SHIFTY_YIELD, 0);
	return boost;
}

/*
 * Delivers external interrupts the way the NanoKernel's Heathrow handler
 * does: pending = mask & levels per bank, reported in HWInfo.PendingInts,
 * where latched pulses (DMA channels) are acknowledged once posted and stay
 * until PrioritizeInterrupts, which Mac OS calls once it has handled them
 * (Mac OS 9's interrupt sets never clear them in the controller themselves);
 * the 68k level is the highest whose sources are pending; it is posted as
 * 0x8000 | level at EmulatorData+IplValueOffset, with ConfigInfo's Post mask
 * ORed into the System context's CR (Clear mask ANDed when nothing is left).
 *
 * Posting is edge-like: only when the level changes, new sources become
 * pending, or `force` (PrioritizeInterrupts). The emulator clears the CR bits
 * itself while the 68k interrupt mask blocks the level, and asks again later.
 *
 * Posting while native code (the alternate context) runs sets `preempt`:
 * the NanoKernel then ends that context with exception 0 so the emulator can
 * take the interrupt, and the emulator resumes it afterwards. (While a task
 * other than the blue one runs, that waits until the blue task is back,
 * which with Multiprocessing Services is next: see mp_interrupt(), and
 * blue_boost() for how it gives up being critical.)
 *
 * Returns whether anything changed (what's reported pending, or posted).
 */
static bool update_interrupts(struct machine *m, bool force)
{
	macio_set_line(&m->macio, IRQ_VIA1, via_irq(&m->via));
	macio_set_line(&m->macio, IRQ_USB, ohci_irq(&m->ohci));
	if (force)
		macio_release(&m->macio);

	uint32_t p0 = macio_pending(&m->macio, 0), p1 = macio_pending(&m->macio, 1);
	bool new_sources = (p0 & ~m->pending[0]) || (p1 & ~m->pending[1]);
	bool changed = p0 != m->pending[0] || p1 != m->pending[1];
	if (changed) {
		m->pending[0] = p0;
		m->pending[1] = p1;
		put_be32(m->kdp + KDP_HW_INFO + 0x28, p0);      /* PendingInts */
		put_be32(m->kdp + KDP_HW_INFO + 0x2C, p1);
		kdp_sync(m);
	}

	unsigned level = 0;
	for (unsigned l = 7; l >= 1 && !level && (p0 | p1); l--)
		if ((p0 & m->level_mask[0][l]) || (p1 & m->level_mask[1][l]))
			level = l;

	bool post = level && (force || new_sources || level != m->ipl);
	bool clear = !level && m->ipl;
	m->ipl = level;
	if (!post && !clear)
		return changed;
	if (post)
		macio_take(&m->macio);

	mem_w16(&m->mem, m->ci.la_emulator_data + m->ci.ipl_value_offset, (uint16_t)(0x8000u | level));
	uint32_t *live = system_cr(m);
	uint32_t cr = live ? *live : mem_r32(&m->mem, m->system_cb + CB_CR);
	if (post)
		cr |= ci32(m->rom, CI_POST_INT_MASK);
	else
		cr &= ci32(m->rom, CI_CLEAR_INT_MASK);
	if (live)
		*live = cr;
	else
		mem_w32(&m->mem, m->system_cb + CB_CR, cr);
	if (post && !m->system)
		m->preempt = true;
	if (post && m->mp_kernel) {
		mp_interrupt(&m->mp, m->cpu.tb);
		set_shifty(m, SHIFTY_YIELD, 0);
	}
	return true;
}

/*
 * A device register access: the load or store that faulted (decoded in
 * `a`) is emulated, with device time brought up to date for it, and
 * interrupts after it. Returns whether those changed.
 */
static bool device_access(struct machine *m, const struct mmio_access *a)
{
	update_device_time(m);
	mmio_perform(a, &m->cpu);
	bool changed = update_interrupts(m, false);
	m->dev_next_tb = device_next_tb(m);     /* (all brought up to date) */
	m->dev_accesses++;
	return changed;
}

/* Why a run stops after a device register access, if it does */
static const char *device_stop(const struct machine *m)
{
	return m->cuda.power_off ? "guest powered off (Cuda)" : m->cuda.reset ? "guest restarted (Cuda)" : NULL;
}

/*
 * Device registers without leaving ppc_run() (the processor's mmio hook):
 * the hook does what the loop in nk_boot() does at a device register's DSI
 * exit, the same way (devices brought up to date as at any exit, then as
 * for the access), so long as that's all the loop would do. Should there be
 * more (interrupts that change, a Multiprocessing Services timer due, a
 * preemption, a stop), the run stops right after the access for the loop
 * to do the rest (PPC_EXIT_MMIO), as it would after the access at that exit.
 *
 * Not on the last instruction before the loop's next stop (mmio_until): an
 * exit there leaves the device sync point to come for later (the loop
 * counts it past), so that access still makes its exit; nor while the
 * loop sets guest time at every exit (the script's "clock").
 */
static int device_hook(void *ctx, struct ppc_cpu *cpu)
{
	struct machine *m = ctx;
	struct mmio_access a;
	if (cpu->icount + 1 >= m->mmio_until || !mmio_decode(&m->bus, cpu, &a))
		return PPC_MMIO_NO;
	bool changed = false;
	if (m->dev_dirty || cpu->tb >= m->dev_next_tb) {        /* (as at every exit) */
		update_device_time(m);
		changed = update_interrupts(m, false);
		m->dev_dirty = false;
		m->dev_next_tb = device_next_tb(m);
	}
	changed |= device_access(m, &a);
	m->dev_inline++;
	if (changed || m->preempt || device_stop(m) || m->mp.swi || m->mp.yield ||
	    (m->mp_kernel && mp_due(&m->mp, cpu->tb)))
		return PPC_MMIO_STOP;
	return PPC_MMIO_DONE;
}

/*
 * A software interrupt from Multiprocessing Services (see mp.c's
 * raise_swi()) is posted to the blue task's emulator as NanoKernel 2 posts
 * it when the task next runs: ConfigInfo's Post mask, less the bits its
 * Clear mask clears (those that say an external interrupt is pending), is
 * ORed into the System context's CR, and native code running is ended as
 * for an external interrupt. The emulator then takes the highest level
 * waiting above its 68k interrupt mask (vector 0x58, the event group
 * pushed), and looks again whenever its mask is set.
 */
static void post_software_interrupt(struct machine *m)
{
	m->mp.swi = false;
	uint32_t bits = ci32(m->rom, CI_POST_INT_MASK) & ci32(m->rom, CI_CLEAR_INT_MASK);
	uint32_t *live = system_cr(m);
	if (live)
		*live |= bits;
	else
		mem_w32(&m->mem, m->system_cb + CB_CR, mem_r32(&m->mem, m->system_cb + CB_CR) | bits);
	if (!m->system)
		m->preempt = true;
	set_shifty(m, SHIFTY_YIELD, 0);
}

static uint32_t trap_table(const struct machine *m)
{
	return m->ci.la_emulator_code + m->ci.kernel_trap_table_offset;
}

static void save_context(struct machine *m, uint32_t cb, uint32_t pc)
{
	struct mem *mem = &m->mem;
	const struct ppc_cpu *cpu = &m->cpu;

	mem_w32(mem, cb + CB_INTER_FLAGS, m->flags);
	mem_w32(mem, cb + CB_INTER_ENABLES, m->enables);
	mem_w32(mem, cb + CB_PC, pc);
	mem_w32(mem, cb + CB_LR, cpu->lr);
	mem_w32(mem, cb + CB_CR, cpu->cr);
	mem_w32(mem, cb + CB_XER, cpu->xer);
	mem_w32(mem, cb + CB_CTR, cpu->ctr);
	mem_w32(mem, cb + CB_FPSCR, cpu->fpscr);
	uint8_t *p = mem_ptr(mem, cb);          /* (one base pointer: the loops vectorize) */
	for (int r = 0; r < 32; r++)
		put_be32(p + CB_GPR + 8 * r + 4, cpu->gpr[r]);
	for (int r = 0; r < 32; r++)
		put_be64(p + CB_FPR + 8 * r, cpu->fpr[r]);
}

static void load_context(struct machine *m, uint32_t cb)
{
	const struct mem *mem = &m->mem;
	struct ppc_cpu *cpu = &m->cpu;

	m->flags = mem_r32(mem, cb + CB_INTER_FLAGS);
	m->enables = mem_r32(mem, cb + CB_INTER_ENABLES);
	cpu->pc = mem_r32(mem, cb + CB_PC);
	cpu->lr = mem_r32(mem, cb + CB_LR);
	cpu->cr = mem_r32(mem, cb + CB_CR);
	cpu->xer = mem_r32(mem, cb + CB_XER);
	cpu->ctr = mem_r32(mem, cb + CB_CTR);
	ppc_fp_set_fpscr(cpu, mem_r32(mem, cb + CB_FPSCR));     /* (as by mtfsf) */
	const uint8_t *p = mem_ptr(mem, cb);
	for (int r = 0; r < 32; r++)
		cpu->gpr[r] = be32(p + CB_GPR + 8 * r + 4);
	for (int r = 0; r < 32; r++)
		cpu->fpr[r] = be64(p + CB_FPR + 8 * r);
	cpu->resv_valid = false;
}

/* Saves the running context (to resume at resume_pc) and runs the one at `to`. */
static const char *switch_context(struct machine *m, uint32_t to, uint32_t resume_pc)
{
	if (!mem_ok(&m->mem, to, CB_SIZE, MEM_RW))
		return "context block not in writable memory";
	save_context(m, m->context, resume_pc);
	load_context(m, to);
	m->context = to;
	m->system = to == m->system_cb;
	return NULL;
}

/*
 * Raises NanoKernel exception `code` for the instruction at `pc`. A context
 * that has the exception enabled handles it itself; otherwise the alternate
 * context ends and the System context resumes.
 */
static const char *raise_exception(struct machine *m, unsigned code, uint32_t pc)
{
	struct ppc_cpu *cpu = &m->cpu;
	uint32_t cb = m->context;

	m->exceptions[code & 31]++;
	if ((m->enables << code) & 0x80000000u) {
		mem_w32(&m->mem, cb + CB_FAULT_SRC_PC, pc);
		mem_w32(&m->mem, cb + CB_FAULT_SRC_LR, cpu->lr);
		mem_w32(&m->mem, cb + CB_FAULT_SRC_R3, cpu->gpr[3]);
		mem_w32(&m->mem, cb + CB_FAULT_SRC_R4, cpu->gpr[4]);
		m->flags = (uint32_t)code << 24 | (m->flags & 0x00FFFFFFu);
		mem_w32(&m->mem, cb + CB_INTRA_FLAGS, m->flags);
		mem_w32(&m->mem, cb + CB_INTRA_ENABLES, m->enables);
		m->enables = 0;         /* exceptions in the handler go to the System context */
		m->flags &= 0xFFFF0007u;
		cpu->pc = mem_r32(&m->mem, cb + CB_INTRA_HANDLER);
		cpu->gpr[4] = mem_r32(&m->mem, cb + CB_INTRA_HANDLER_ARG);
		cpu->gpr[3] = m->system ? m->system_cb : m->native_cb;
		cpu->lr = trap_table(m);
		return NULL;
	}
	if (m->system)
		return "exception in the System context with exceptions disabled";
	m->flags = (uint32_t)code << 24 | (m->flags & 0x00FFFFFFu);
	return switch_context(m, m->system_cb, pc);
}

/* Trap logging stops at the dispatch table's A-line entries, and at the recompiler's trap entry */
static void cpu_set_trap_break(struct machine *m)
{
	m->cpu.break_lo = m->ci.la_dispatch_table + 0xA000u * 8;
	m->cpu.break_hi = m->cpu.break_lo + 0x1000u * 8;
	if (m->recompiled_traps)
		m->cpu.break_pc = m->recompiled_traps;
}

static void print_pstring(const struct mem *mem, uint32_t addr)
{
	if (!addr || !mem_ok(mem, addr, 1, MEM_R))
		return;
	unsigned len = mem_r8(mem, addr);
	printf(" \"");
	for (unsigned k = 1; k <= len && mem_ok(mem, addr + k, 1, MEM_R); k++) {
		uint8_t ch = mem_r8(mem, addr + k);
		putchar(ch >= 0x20 && ch < 0x7F ? ch : '?');
	}
	printf("\"");
}

/*
 * --watch: guest stores to the watched page exit while guest code runs, but
 * the kernel's own writes there (disk reads, saved contexts) must still go
 * through, so that watching doesn't change what the guest does.
 */
static void watch_protect(struct machine *m, uint32_t addr, bool on)
{
	uint32_t page = addr & ~(MEM_PAGE_SIZE - 1);
	uint8_t flags = m->mem.pageflags[page >> MEM_PAGE_SHIFT];
	if (on) {
		m->watch_writable = (flags & MEM_W) != 0;
		if (m->watch_writable)
			mem_map(&m->mem, page, MEM_PAGE_SIZE, flags & ~MEM_W);
	} else if (m->watch_writable) {
		mem_map(&m->mem, page, MEM_PAGE_SIZE, flags | MEM_W);
	}
}

/* Logs a 68k A-line trap; the emulator holds d0-d7 in r8-r15 and a0-a6 in r16-r22. */
/* `pc` is the 68k pc after the trap word */
static void log_trap(const struct machine *m, uint16_t trap, uint32_t pc)
{
	const struct ppc_cpu *cpu = &m->cpu;
	printf("trap %04X  68k pc %08X  d0 %08X d1 %08X a0 %08X a1 %08X  @%llu",
	       trap, pc, cpu->gpr[8], cpu->gpr[9], cpu->gpr[16], cpu->gpr[17],
	       (unsigned long long)cpu->icount);
	if ((trap & 0xF8FF) == 0xA000 && mem_ok(&m->mem, cpu->gpr[16] + 18, 4, MEM_R))  /* _Open */
		print_pstring(&m->mem, mem_r32(&m->mem, cpu->gpr[16] + 18));
	printf("\n");
}

/* Logs the first few exceptions of each kind, then raises it. */
static const char *exception(struct machine *m, unsigned code, uint32_t pc)
{
	if (!blue_running(m)) {
		printf("exception %u in task 0x%08X at pc 0x%08X (insn 0x%08X, addr 0x%08X, lr 0x%08X) @%llu\n",
		       code, m->mp.current, m->cpu.pc, m->cpu.exit_insn, m->cpu.fault_addr, m->cpu.lr,
		       (unsigned long long)m->cpu.icount);
		return "exception in a task other than the blue one (not handled yet)";
	}
	if (m->exceptions[code & 31] < 4)
		printf("exception %u in %s context at pc 0x%08X (insn 0x%08X, addr 0x%08X, r24 0x%08X, lr 0x%08X, "
		       "ctr 0x%08X, r12 0x%08X) @%llu\n",
		       code, m->system ? "system" : "native", m->cpu.pc, m->cpu.exit_insn, m->cpu.fault_addr,
		       m->cpu.gpr[24], m->cpu.lr, m->cpu.ctr, m->cpu.gpr[12], (unsigned long long)m->cpu.icount);
	return raise_exception(m, code, pc);
}

/* Whether stores to addr are dropped (see quiet_attr()); BATs come first among the regions, as in translation. */
static bool quiet_page(struct machine *m, uint32_t addr)
{
	if ((m->mem.pageflags[addr >> MEM_PAGE_SHIFT] & (MEM_R | MEM_W)) != MEM_R)
		return false;
	struct nk_region regions[NK_MAX_REGIONS];
	int n = nk_user_regions(m->rom, &m->ci, regions, NK_MAX_REGIONS);
	for (int k = 0; k < n; k++)
		if (addr - regions[k].logical < regions[k].size)
			return regions[k].quiet;
	return false;
}

/*
 * Makes the store that faulted on a quiet page with its pages writable, then
 * puts them back as they were: the instruction does all else it does (its
 * base register, bytes on other pages) as under the NanoKernel's MemRetry,
 * which skips the accesses to quiet pages. False if it faulted otherwise.
 */
static bool quiet_store(struct machine *m)
{
	struct ppc_cpu *cpu = &m->cpu;
	uint32_t page[2] = { cpu->fault_addr & ~(MEM_PAGE_SIZE - 1) };
	page[1] = page[0] + MEM_PAGE_SIZE;      /* (a store is 128 bytes at most) */
	bool quiet[2];
	uint8_t saved[2][MEM_PAGE_SIZE];
	for (int k = 0; k < 2; k++) {
		quiet[k] = quiet_page(m, page[k]);
		if (quiet[k]) {
			memcpy(saved[k], mem_ptr(&m->mem, page[k]), MEM_PAGE_SIZE);
			m->mem.pageflags[page[k] >> MEM_PAGE_SHIFT] |= MEM_W;
		}
	}
	enum ppc_exit e = ppc_interp(cpu, 1);
	for (int k = 0; k < 2; k++) {
		if (quiet[k]) {
			m->mem.pageflags[page[k] >> MEM_PAGE_SHIFT] &= (uint8_t)~MEM_W;
			memcpy(mem_ptr(&m->mem, page[k]), saved[k], MEM_PAGE_SIZE);
		}
	}
	if (e != PPC_EXIT_BUDGET)
		return false;
	uint8_t *count = m->kdp + KDP_NK_INFO + NKI_QUIET_WRITES;
	put_be32(count, be32(count) + 1);
	kdp_sync(m);
	return true;
}

static bool is_mfmsr(uint32_t insn)
{
	return (insn & 0xFC0007FEu) == 0x7C0000A6u;
}

/*
 * KCall 0. r3 is the handler's verdict: 0 resumes the interrupted code with
 * PC, LR, r3 and r4 restored from the context block; 1 propagates the
 * exception to the System context; 32-255 ends the alternate context with that
 * code (this is how native code returns to the 68k emulator).
 */
static const char *kcall_return_from_exception(struct machine *m)
{
	struct ppc_cpu *cpu = &m->cpu;
	uint32_t cb = m->context;
	uint32_t verdict = cpu->gpr[3];

	if (verdict >= 2 && !(verdict >= 32 && verdict < 256))
		return raise_exception(m, EC_TRAP_INSTR, cpu->pc);
	if (verdict >= 1 && m->system)
		return "ReturnFromException: System context cannot leave itself";

	if (verdict >= 32) {
		m->flags = verdict << 24 | (m->flags & 0x00FFFFFFu);
		return switch_context(m, m->system_cb, cpu->lr);
	}

	m->flags = (m->flags & 0x00FF0000u) | (mem_r32(&m->mem, cb + CB_INTRA_FLAGS) & 0xFF00FFFFu);
	m->enables = mem_r32(&m->mem, cb + CB_INTRA_ENABLES);
	cpu->pc = mem_r32(&m->mem, cb + CB_FAULT_SRC_PC);
	cpu->lr = mem_r32(&m->mem, cb + CB_FAULT_SRC_LR);
	cpu->gpr[3] = mem_r32(&m->mem, cb + CB_FAULT_SRC_R3);
	cpu->gpr[4] = mem_r32(&m->mem, cb + CB_FAULT_SRC_R4);
	if (verdict == 1)       /* propagate: the System context sees the original exception */
		return switch_context(m, m->system_cb, cpu->pc);
	return NULL;
}

/*
 * 68k Page Descriptor bits (68040 format; bit 0 = least significant)
 * as the NanoKernel's VM interface reports them.
 */
#define M68PD_RESIDENT 0x001u
#define M68PD_INITED 0x002u

#define VM_SEGMENT_PAGES 0x10000u
#define VM_MAX_VIRTUAL_PAGES (4 * VM_SEGMENT_PAGES)     /* VM lives in segments 0-3 */

static void set_logical_ram(struct machine *m, uint32_t pages)
{
	m->logical_pages = pages;
	put_be32(m->kdp + KDP_SYS_INFO + 0x04, pages << MEM_PAGE_SHIFT);   /* usable */
	put_be32(m->kdp + KDP_SYS_INFO + 0x08, pages << MEM_PAGE_SHIFT);   /* logical */
	kdp_sync(m);
}

/*
 * KCall 3, the 68k VM Manager interface (FE0A trap): selector in r3, args in r4-r6,
 * result in r3. VM is always off here: every RAM page is resident and logical
 * addresses equal "physical" ones.
 */
static const char *kcall_vm_dispatch(struct machine *m)
{
	struct ppc_cpu *cpu = &m->cpu;
	uint32_t sel = cpu->gpr[3], page = cpu->gpr[4], arg2 = cpu->gpr[5];
	uint32_t *r3 = &cpu->gpr[3];

	switch (sel) {
	case 0:                 /* VMInit: turning VM on is not supported */
		*r3 = 0xFFFFFFFFu;
		break;
	case 1: case 2: case 13: case 14: case 15: case 16: case 22:
		break;          /* UnInit, FinalInit, Reload, cache flushes, LRU: nothing to do */
	case 3:                 /* IsResident */
	case 5:                 /* IsInited */
	case 4:                 /* IsUnmodified */
		*r3 = 1;
		break;
	case 6:                 /* ShouldClean */
		*r3 = 0;
		break;
	case 7: case 8: case 9: case 23:        /* page-state changes only matter with VM on */
	case 17: case 18: case 20: case 24:     /* cacheability and PTE changes */
		*r3 = 0;
		break;
	case 10:                /* GetPhysicalPage */
		*r3 = page;
		break;
	case 11:                /* GetPhysicalAddress: unimplemented in the real kernel too */
		*r3 = 0xFFFFFFFFu;
		break;
	case 12: {              /* ExchangePages */
		uint32_t a = page << MEM_PAGE_SHIFT, b = arg2 << MEM_PAGE_SHIFT;
		if (!mem_ok(&m->mem, a, MEM_PAGE_SIZE, MEM_RW) || !mem_ok(&m->mem, b, MEM_PAGE_SIZE, MEM_RW)) {
			*r3 = 0xFFFFFFFFu;
			break;
		}
		uint8_t tmp[MEM_PAGE_SIZE];
		mem_invalidate(&m->mem, a, MEM_PAGE_SIZE);
		mem_invalidate(&m->mem, b, MEM_PAGE_SIZE);
		memcpy(tmp, mem_ptr(&m->mem, a), MEM_PAGE_SIZE);
		memcpy(mem_ptr(&m->mem, a), mem_ptr(&m->mem, b), MEM_PAGE_SIZE);
		memcpy(mem_ptr(&m->mem, b), tmp, MEM_PAGE_SIZE);
		break;
	}
	case 19:                /* GetPTEntryGivenPage */
		*r3 = page << MEM_PAGE_SHIFT | M68PD_INITED | M68PD_RESIDENT;
		break;
	case 21:                /* PTest: why did this page fault? */
		*r3 = (page >> MEM_PAGE_SHIFT) >= m->logical_pages ? 1u << 14 : 1u << 15;
		break;
	case 25: {              /* AllocateMemory(first_page, page_count, align_mask) */
		uint32_t count = arg2;
		if ((int32_t)count <= 0 || page < VM_MAX_VIRTUAL_PAGES || page >= 0x100000u ||
		    count > 0x100000u - page || count >= m->logical_pages / 2) {
			*r3 = 0xFFFFFFFFu;
			break;
		}
		uint32_t addr = page << MEM_PAGE_SHIFT, size = count << MEM_PAGE_SHIFT;
		for (uint32_t a = addr; a - addr < size; a += MEM_PAGE_SIZE)
			if (mem_ok(&m->mem, a, 1, MEM_R)) {
				*r3 = 0xFFFFFFFFu;  /* already in use */
				return NULL;
			}
		memset(mem_ptr(&m->mem, addr), 0, size);
		mem_map(&m->mem, addr, size, MEM_RWX);
		/* the real kernel steals these pages from the top of RAM */
		set_logical_ram(m, m->logical_pages - count);
		*r3 = 1;
		break;
	}
	default:
		*r3 = 0xFFFFFFFFu;
		break;
	}
	return NULL;
}

/*
 * Disk transfers take guest time, at the speeds of the drives of the day.
 * Without it, a program's launch or a file copy takes no time at all, and
 * Mac OS, which counts a process's processor time in whole ticks, can see a
 * busy process as never having run: the Mac OS 9 installer then waits
 * forever for its engine to start. A transfer moves guest time on by at most
 * a tick, so that a big one doesn't jump past what interrupts keep going,
 * such as sound.
 */
#define HD_BYTES_PER_SECOND 20000000u   /* a hard disk around 2000 */
#define CD_BYTES_PER_SECOND 4000000u    /* a 24x to 32x CD-ROM drive */

/*
 * A host call from a generated driver (see ndrv.h): run the native driver
 * one step, then either finish (CR0[EQ] set, result in r3) or have the stub
 * call a Mac OS routine (TVector in r12, arguments in r3..r10).
 */
static const char *host_call(struct machine *m, uint32_t selector)
{
	struct ppc_cpu *cpu = &m->cpu;
	struct ndrv_call c = {
		.mem = &m->mem,
		.entry = cpu->gpr[11],
		.toc = cpu->gpr[2],
		.frame = cpu->gpr[1],
		.tb = cpu->tb,
		.f1 = cpu->fpr[1],
	};
	memcpy(c.arg, &cpu->gpr[3], sizeof c.arg);
	if (!mem_ok(&m->mem, c.frame, 80, MEM_RW))
		return "driver stack frame not in writable memory";

	switch (selector) {
	case HOSTCALL_VIDEO_DRIVER:
		video_driver_call(&m->video, &c, TIMEBASE_HZ);
		break;
	case HOSTCALL_TOOLS_HELPER:
		helper_call(&m->helper, &c, &m->video);
		break;
	case HOSTCALL_TOOLS_RAVE:
		rave_call(&m->rave, &c);
		break;
	case HOSTCALL_LANLIB:
		net_call(&m->net, &c);
		break;
	default: {
		if (selector - HOSTCALL_DISK_DRIVER >= m->disks)
			return "unknown host call";
		struct disk *d = &m->disk[selector - HOSTCALL_DISK_DRIVER];
		uint64_t moved = d->bytes_read + d->bytes_written;
		disk_driver_call(d, &c, TIMEBASE_HZ);
		moved = d->bytes_read + d->bytes_written - moved;
		if (moved) {
			uint64_t ticks = moved * TIMEBASE_HZ / (d->cdrom ? CD_BYTES_PER_SECOND : HD_BYTES_PER_SECOND);
			pass_guest_time(m, ticks < TIMEBASE_HZ / 60 ? ticks : TIMEBASE_HZ / 60);
		}
		break;
	}
	}

	if (c.done) {
		cpu->gpr[3] = c.result;
		if (c.set_f1)
			cpu->fpr[1] = c.f1_out;
		cpu->cr = (cpu->cr & 0x0FFFFFFFu) | 0x20000000u;
	} else {
		memcpy(&cpu->gpr[3], c.out, sizeof c.out);
		cpu->gpr[12] = c.tvector;
		cpu->cr &= 0x0FFFFFFFu;
	}
	cpu->pc += 4;
	return NULL;
}

/* Returns NULL to keep running, or a reason to stop. */
/* SYSHM_PERF: every second of host time, how fast the guest ran and how the host spent this thread's time */
static void perf_report(struct machine *m)
{
	uint64_t now = host_ns(), cycles, insns;
	if (now - m->perf_ns < 1000000000u || !perf_read(&m->perf, &cycles, &insns))
		return;
	uint64_t disk_read = 0, disk_written = 0;
	for (unsigned k = 0; k < m->disks; k++) {
		disk_read += m->disk[k].bytes_read;
		disk_written += m->disk[k].bytes_written;
	}
	if (m->perf_ns) {
		double s = (double)(now - m->perf_ns) * 1e-9, guest = (double)(m->cpu.icount - m->perf_icount);
		double c = (double)(cycles - m->perf_cycles), i = (double)(insns - m->perf_insns);
		printf("perf: %.0f million guest instructions/s; host: %.2f GHz, %.2f instructions/cycle, "
		       "%.1f instructions and %.1f cycles per guest instruction", guest / s * 1e-6, c / s * 1e-9,
		       i / (c ? c : 1), i / (guest ? guest : 1), c / (guest ? guest : 1));
		if (m->idle_ns != m->perf_idle_ns)
			printf("; idle %.0f%%", (double)(m->idle_ns - m->perf_idle_ns) * 1e-9 / s * 100);
		if (disk_read != m->perf_disk_read || disk_written != m->perf_disk_written)
			printf("; disks: %.1f MB/s read, %.1f MB/s written", (double)(disk_read - m->perf_disk_read) / s * 1e-6,
			       (double)(disk_written - m->perf_disk_written) / s * 1e-6);
		printf("\n");
		fflush(stdout);
	}
	m->perf_ns = now;
	m->perf_cycles = cycles;
	m->perf_insns = insns;
	m->perf_icount = m->cpu.icount;
	m->perf_idle_ns = m->idle_ns;
	m->perf_disk_read = disk_read;
	m->perf_disk_written = disk_written;
}

/*
 * MPDispatch: the calls Multiprocessing Services make to NanoKernel 2 (call
 * number in r0), through `sc` or kernel call 8. As syshm's kernel reports
 * itself as 2.28 (see NK_INFO_VERSION), Mac OS 8.6 and later make some;
 * ROM files from Mac OS 9.1 on, for which syshm reports a blue task too
 * (mp_kernel), make more (see mp.h). Call 55 is the one 8.6's Process
 * Manager (its ProcessMgrSupport fragment) makes when nothing needs to
 * run, to sleep until the next process wake time (r3:r4, an AbsoluteTime;
 * the result is ignored). The processor naps until then, or until an
 * interrupt, as for PowerDispatch (see blue_naps()). With a NanoKernel 2's
 * blue task (mp_kernel), MPYieldWithHint (13), and a nap with no interrupt
 * pending, mark the emulator's PriorityShifty word as having yielded (see
 * blue_boost()).
 */
#define MP_YIELD_WITH_HINT 13u
#define MP_DELAY_UNTIL 55u

/* Their names, after the reverse-engineered NanoKernel 2 (github.com/elliotnunn/powermac-rom) */
static const char *mp_call_name(unsigned n)
{
	static const char *const names[134] = {
		[2] = "MPRegisterCpuPlugin", [3] = "MPProcessors", [4] = "MPCreateProcess",
		[5] = "MPTerminateProcess", [6] = "MPDeleteProcess", [11] = "MPIsTaskBlue",
		[13] = "MPYieldWithHint", [14] = "MPSetTaskWeight", [15] = "MPCreateQueue",
		[16] = "MPDeleteQueue", [17] = "MPNotifyQueue", [18] = "MPWaitOnQueue", [19] = "MPQueryQueue",
		[20] = "MPCreateSemaphore", [21] = "MPDeleteSemaphore", [22] = "MPSignalSemaphore",
		[23] = "MPWaitOnSemaphore", [24] = "MPQuerySemaphore", [25] = "MPCreateCriticalRegion",
		[26] = "MPDeleteCriticalRegion", [27] = "MPEnterCriticalRegion", [28] = "MPExitCriticalRegion",
		[29] = "MPQueryCriticalRegion", [30] = "NKSetTimerNotify", [31] = "MPArmTimer",
		[32] = "MPCancelTimer", [33] = "MPDelayUntil", [37] = "MPGetNextID", [39] = "MPSetQueueReserve",
		[40] = "NKCreateTimer", [41] = "NKDeleteTimer", [42] = "MPCreateProcessor",
		[43] = "MPDeleteProcessor", [44] = "MPStartScheduling", [45] = "MPStopScheduling",
		[46] = "MPCpuPlugin", [49] = "MPCreateEvent", [50] = "MPDeleteEvent", [51] = "MPSetEvent",
		[52] = "MPWaitForEvent", [53] = "MPQueryEvent", [54] = "MPSetSWIEvent", [55] = "MPDelayUntilSys",
		[56] = "MPSetExceptionHandler", [57] = "MPThrowException", [62] = "MPGetNextCpuID",
		[64] = "MPCreateNotification", [65] = "MPDeleteNotification",
		[66] = "MPModifyNotification", [67] = "MPCauseNotification", [68] = "MPGetPageSizeClasses",
		[69] = "MPGetPageSize", [70] = "MPCreateAddressSpace", [71] = "MPDeleteAddressSpace",
		[72] = "MPCreateArea", [73] = "MPCreateAliasArea", [74] = "MPDeleteArea", [75] = "MPSetAreaSize",
		[76] = "MPSetAreaAccess", [77] = "MPSetAreaBackingProvider", [79] = "MPGetNextAreaID",
		[80] = "MPGetAreaFromAddress", [81] = "MPGetPhysicalAddress", [82] = "MPRegisterAger",
		[83] = "MPGetFreePage", [84] = "MPPutFreePage", [85] = "MPMapPage", [86] = "MPUnmapPages",
		[87] = "MPLockPages", [88] = "MPUnlockPages", [89] = "MPHoldPages", [90] = "MPUnholdPages",
		[91] = "MPGetPageAttributes", [92] = "MPSetPageAttributes", [93] = "MPGetPageAge",
		[94] = "MPSetPageAge", [95] = "MPFindVictimPage", [96] = "MPDebugPutString",
		[97] = "MPDebugPutHex", [98] = "MPGetPhysicalExtent", [99] = "MPSetBlueProcessID",
		[100] = "MPGetFreePageCount", [101] = "MPGetUnheldPageCount", [102] = "MPGetKernelStateSize",
		[103] = "MPGetKernelState", [104] = "MPRegisterThermalHandler", [105] = "MPRegisterPMFHandler",
		[106] = "MPMarkPMFTask", [107] = "NKLocateInfoRecord", [108] = "NKSetPrInfoClockRates",
		[114] = "MPSetTaskCPU", [116] = "MPGetNextIDOwnedByProcess", [117] = "MPCurrentAddressSpace",
		[118] = "MPHomeAddressSpace", [119] = "MPSetTaskAddressSpace", [121] = "MPRegisterExternalHandler",
		[122] = "KCPropogateExternalInterrupt", [123] = "MPGetAreaAccess", [124] = "MPDebugPutDec",
		[125] = "MPGetNextAreaFromAddress", [126] = "KCSetTaskType", [127] = "MPMakePhysicallyContiguous",
		[129] = "MPGetPageHoldLockCounts", [130] = "MPSetSubPageAccess", [131] = "NKSetClockStep",
		[132] = "NKSetClockDriftCorrection",
	};
	return n < 134 && names[n] ? names[n] : "unnamed";
}

/*
 * Puts task `to` (mp_next()'s) on the processor in place of the current
 * one: their registers change places (the blue task's NanoKernel 1 state
 * stays where it is: see blue_running()). If it's the blue task, napping
 * with nothing else to run, the rest of its nap is idle.
 */
static void switch_task(struct machine *m, uint32_t to)
{
	struct ppc_cpu *cpu = &m->cpu;
	if (to == m->mp.blue_task && mp_napping(&m->mp))
		idle_wait(m, mp_lookup(&m->mp, to, MP_TASK)->task.deadline);
	if (to == m->mp.current) {
		mp_switch(&m->mp, to, cpu->tb);
		return;
	}
	struct mp_regs *out = mp_regs(&m->mp, m->mp.current), *in = mp_regs(&m->mp, to);
	if (m->mp.log)
		printf("mp: task 0x%08X -> 0x%08X (pc 0x%08X -> 0x%08X) @%llu\n", m->mp.current, to, cpu->pc, in->pc,
		       (unsigned long long)cpu->icount);
	if (out) {
		memcpy(out->gpr, cpu->gpr, sizeof out->gpr);
		memcpy(out->fpr, cpu->fpr, sizeof out->fpr);
		out->cr = cpu->cr;
		out->xer = cpu->xer;
		out->lr = cpu->lr;
		out->ctr = cpu->ctr;
		out->pc = cpu->pc;
		out->fpscr = cpu->fpscr;
	}
	memcpy(cpu->gpr, in->gpr, sizeof cpu->gpr);
	memcpy(cpu->fpr, in->fpr, sizeof cpu->fpr);
	cpu->cr = in->cr;
	cpu->xer = in->xer;
	cpu->lr = in->lr;
	cpu->ctr = in->ctr;
	cpu->pc = in->pc;
	ppc_fp_set_fpscr(cpu, in->fpscr);
	cpu->resv_valid = false;
	mp_switch(&m->mp, to, cpu->tb);
}

/*
 * The blue task naps until guest time `until` (UINT64_MAX: an interrupt):
 * Multiprocessing Services' tasks that are ready run meanwhile (see
 * mp_nap()), else the processor idles. Not while an interrupt waits for
 * it.
 */
static void blue_naps(struct machine *m, uint64_t until)
{
	if (m->mp_kernel && blue_running(m) && !m->ipl && mp_nap(&m->mp, until) != MP_NAP_IDLE)
		return;
	idle_wait(m, until);
}

/*
 * The CPU plugin, for the machine's processors (starting others, their
 * clocks, power and temperature): Mac OS registers one with
 * MPRegisterCpuPlugin (call 2: coherence group r3, 0 the machine's; the
 * plugin's page-aligned code r4 and size r5; its descriptor r6, in it: a
 * table of stacks by processor +0, its routine count +0x1C, their
 * transition vectors +0x20), and calls its routines with MPCpuPlugin
 * (call 46: selector r3, arguments r4-r10). NanoKernel 2 runs one there and
 * then (SIGP), in supervisor mode with interrupts off, on the processor's
 * stack with r3 its index (0), returning to the emulator's
 * ReturnFromException trap; its r3 and r4 are the call's results, the
 * caller's other registers stay. An exception in it ends it with
 * MP_CPU_PLUGIN_FAULT_ERR (here: anything but a trap, a device register's
 * access or mfmsr, so supervisor-only registers too).
 */
#define MP_REGISTER_CPU_PLUGIN 2u
#define MP_CPU_PLUGIN 46u
#define CPU_PLUGIN_ROUTINES 64u                 /* (at most) */
#define CPU_PLUGIN_INSNS 10000000u              /* (a routine that runs longer has hung) */
#define SUPERVISOR_MSR (USER_MSR & ~0xC000u)    /* (EE and PR off) */

static int32_t cpu_plugin_register(struct machine *m)
{
	const uint32_t *r = m->cpu.gpr;
	struct mp *mp = &m->mp;
	if ((r[4] | r[5]) & (MEM_PAGE_SIZE - 1) || !r[5])
		return MP_INSUFFICIENT_RESOURCES_ERR;
	if (r[3])                       /* (there's no coherence group but the machine's) */
		return MP_INVALID_ID_ERR;
	if (!r[6]) {
		mp->plugin.desc = 0;
		return MP_INVALID_ID_ERR;
	}
	if (r[6] - r[4] >= r[5] || !mem_ok(&m->mem, r[6], 0x20, MEM_R))
		return MP_INSUFFICIENT_RESOURCES_ERR;
	if (mp->plugin.desc)
		return MP_INVALID_ID_ERR;
	mp->plugin.base = r[4];
	mp->plugin.size = r[5];
	mp->plugin.desc = r[6];
	mp->plugin.routines = mem_r32(&m->mem, r[6] + 0x1C);
	return mp->plugin.routines > CPU_PLUGIN_ROUTINES ? MP_INSUFFICIENT_RESOURCES_ERR : MP_NO_ERR;
}

/* Runs the plugin's routine `selector` to its end; returns its r3 (r4 in *r4), or MP_CPU_PLUGIN_FAULT_ERR. */
static int32_t cpu_plugin_run(struct machine *m, uint32_t selector, uint32_t *r4)
{
	struct ppc_cpu *cpu = &m->cpu;
	struct mem *mem = &m->mem;
	uint32_t desc = m->mp.plugin.desc, tv = desc + 0x20 + 4 * selector;
	if (!mem_ok(mem, tv, 4, MEM_R) || !mem_ok(mem, desc, 4, MEM_R) || !mem_ok(mem, mem_r32(mem, desc), 4, MEM_R))
		return MP_CPU_PLUGIN_FAULT_ERR;
	tv = mem_r32(mem, tv);
	if (!mem_ok(mem, tv, 8, MEM_R))
		return MP_CPU_PLUGIN_FAULT_ERR;
	cpu->pc = mem_r32(mem, tv);
	cpu->gpr[2] = mem_r32(mem, tv + 4);
	cpu->gpr[1] = mem_r32(mem, mem_r32(mem, desc));
	cpu->gpr[3] = 0;
	cpu->lr = trap_table(m);
	for (uint64_t end = cpu->icount + CPU_PLUGIN_INSNS; cpu->icount < end;) {
		enum ppc_exit e = ppc_run(cpu, end - cpu->icount);
		if (e == PPC_EXIT_TRAP) {
			*r4 = cpu->gpr[4];
			return (int32_t)cpu->gpr[3];
		} else if (e == PPC_EXIT_PRIV && is_mfmsr(cpu->exit_insn)) {
			cpu->gpr[(cpu->exit_insn >> 21) & 31] = SUPERVISOR_MSR;
			cpu->pc += 4;
		} else if (e == PPC_EXIT_DSI && mmio_find(&m->bus, cpu->fault_addr)) {
			update_device_time(m);
			if (mmio_emulate(&m->bus, cpu) != MMIO_DONE)
				break;
		} else if (e == PPC_EXIT_BREAK) {
			cpu->break_skip = true;
		} else if (e != PPC_EXIT_BUDGET) {
			if (m->mp.log)
				printf("mp: CPU plugin routine %u: exit %s at pc 0x%08X (insn 0x%08X)\n", selector,
				       ppc_exit_name(e), cpu->pc, cpu->exit_insn);
			break;
		}
	}
	*r4 = cpu->gpr[4];
	return MP_CPU_PLUGIN_FAULT_ERR;
}

static int32_t cpu_plugin_call(struct machine *m)
{
	struct ppc_cpu *cpu = &m->cpu;
	uint32_t selector = cpu->gpr[3], r4 = cpu->gpr[4];
	if (!m->mp.plugin.desc)
		return MP_NO_CPU_PLUGIN_ERR;
	if (selector >= m->mp.plugin.routines)
		return MP_CPU_PLUGIN_SELECTOR_ERR;
	struct mp_regs caller;
	memcpy(caller.gpr, cpu->gpr, sizeof caller.gpr);
	memcpy(caller.fpr, cpu->fpr, sizeof caller.fpr);
	caller.cr = cpu->cr;
	caller.xer = cpu->xer;
	caller.lr = cpu->lr;
	caller.ctr = cpu->ctr;
	caller.fpscr = cpu->fpscr;
	int32_t result = cpu_plugin_run(m, selector, &r4);
	memcpy(cpu->gpr, caller.gpr, sizeof cpu->gpr);
	memcpy(cpu->fpr, caller.fpr, sizeof cpu->fpr);
	cpu->cr = caller.cr;
	cpu->xer = caller.xer;
	cpu->lr = caller.lr;
	cpu->ctr = caller.ctr;
	ppc_fp_set_fpscr(cpu, caller.fpscr);
	cpu->resv_valid = false;
	cpu->gpr[4] = r4;
	return result;
}

/* Makes call r0; the caller resumes at next_pc once it's done (a task that blocks in it makes it again). */
static const char *kcall_mp_dispatch(struct machine *m, uint32_t next_pc)
{
	struct ppc_cpu *cpu = &m->cpu;
	bool yields = cpu->gpr[0] == MP_YIELD_WITH_HINT || (cpu->gpr[0] == MP_DELAY_UNTIL && !m->ipl);
	if (m->mp_kernel && yields && blue_running(m))
		set_shifty(m, SHIFTY_YIELDED, SHIFTY_CLEARED);
	if (cpu->gpr[0] == MP_DELAY_UNTIL && blue_running(m)) {
		blue_naps(m, (uint64_t)cpu->gpr[3] << 32 | cpu->gpr[4]);
		cpu->gpr[3] = 0;                /* noErr */
		cpu->pc = next_pc;
		return NULL;
	}
	if (m->mp_kernel && (cpu->gpr[0] == MP_REGISTER_CPU_PLUGIN || cpu->gpr[0] == MP_CPU_PLUGIN)) {
		uint32_t r3 = cpu->gpr[3], r4 = cpu->gpr[4], r5 = cpu->gpr[5];
		int32_t status = cpu->gpr[0] == MP_CPU_PLUGIN ? cpu_plugin_call(m) : cpu_plugin_register(m);
		if (m->mp.log)
			printf("mp: call %u (r3=0x%08X r4=0x%08X r5=0x%08X) -> %d\n", cpu->gpr[0], r3, r4, r5, status);
		cpu->gpr[3] = (uint32_t)status;
		cpu->pc = next_pc;
		return NULL;
	}
	if (!m->mp_kernel || !mp_call(&m->mp, cpu, &m->mem)) {
		printf("MPDispatch: call %u (%s; r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X) at pc 0x%08X, lr 0x%08X\n",
		       cpu->gpr[0], mp_call_name(cpu->gpr[0]), cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6],
		       cpu->pc, cpu->lr);
		return "unimplemented Multiprocessing Services call";
	}
	uint8_t *count = m->kdp + KDP_NK_INFO + NKI_TASK_COUNT;
	if (be32(count) != m->mp.tasks) {
		put_be32(count, m->mp.tasks);
		kdp_sync(m);
	}
	if (mp_lookup(&m->mp, m->mp.current, MP_TASK)->task.state != MP_TASK_BLOCKED)
		cpu->pc = next_pc;
	return NULL;
}

/*
 * `sc` under NanoKernel 2: r0 -1 tests quickly whether the current task is
 * the blue one (r0 its flag, 0x00200000, or 0; CR0 EQ if it isn't); -2
 * too, with the task's word for Multiprocessing Services (call 12's) in r0.
 * The rest are Multiprocessing Services calls.
 */
#define NK2_FLAG_BLUE 0x00200000u

static const char *system_call(struct machine *m)
{
	struct ppc_cpu *cpu = &m->cpu;
	int32_t n = (int32_t)cpu->gpr[0];
	if (n != -1 && n != -2)
		return kcall_mp_dispatch(m, cpu->pc + 4);
	bool blue = blue_running(m);
	if (n == -1)
		cpu->gpr[0] = blue ? NK2_FLAG_BLUE : 0;
	else
		cpu->gpr[0] = mp_lookup(&m->mp, m->mp.current, MP_TASK)->task.label;
	cpu->cr = (cpu->cr & 0x0FFFFFFFu) | (blue ? CR_GT : CR_EQ) << 28 | (cpu->xer & XER_SO ? CR_SO : 0) << 28;
	cpu->pc += 4;
	return NULL;
}

/* Calls made through the emulator's trap table return to LR; others to the next instruction. */
static uint32_t kcall_return_pc(const struct machine *m)
{
	return m->cpu.pc - trap_table(m) < 16 * 4 ? m->cpu.lr : m->cpu.pc + 4;
}

static const char *kernel_call(struct machine *m, unsigned n)
{
	struct ppc_cpu *cpu = &m->cpu;
	uint32_t table = trap_table(m);
	const char *why;

	if (n != 8 && !blue_running(m))
		return "kernel call from a task other than the blue one";

	if (n == 0)
		return kcall_return_from_exception(m);

	if (n == 1) {   /* RunAlternateContext(cb r3, interrupt test mask r4) */
		uint32_t cb = cpu->gpr[3];
		if (cpu->gpr[4] & cpu->cr) {    /* an interrupt is pending: let the emulator take it first */
			cpu->pc = cpu->lr;
			return NULL;
		}
		m->native_cb = cb;
		mem_w32(&m->mem, cb + CB_INTRA_HANDLER_RETURN, table);
		m->flags &= 0x00FFFFFFu;
		return switch_context(m, cb, cpu->lr);
	}

	switch (n) {
	case 2:     /* ResetSystem (68k RESET): the kernel re-initializes itself and resumes */
		break;
	case 4:     /* PrioritizeInterrupts: re-evaluate pending interrupts */
		update_device_time(m);
		update_interrupts(m, true);
		break;
	case 3:     /* VMDispatch (68k FE0A) */
		why = kcall_vm_dispatch(m);
		if (why)
			return why;
		break;
	case 5:     /* PowerDispatch(selector r3, CR interrupt mask r4): 0-7 doze/nap/sleep, 8 suspend, 9 ICCR */
		if (cpu->gpr[3] > 11) {
			cpu->gpr[3] = 0xFFFFFFFFu;
			break;
		}
		if (cpu->gpr[3] <= 7 && !(cpu->gpr[4] & cpu->cr))
			blue_naps(m, UINT64_MAX);       /* until an interrupt: there's no processor state to lose */
		cpu->gpr[3] = 0;
		break;
	case 6:     /* RTASDispatch: r3 = argument block; returns 0, or -1 if unusable */
		cpu->gpr[3] = rtas_call(&m->rtas, &m->mem, cpu->gpr[3]) ? 0 : 0xFFFFFFFFu;
		if (m->rtas.power_off)
			return "guest powered off";
		if (m->rtas.reboot)
			return "guest restarted";
		break;
	case 8:     /* MPDispatch: Multiprocessing Services, NanoKernel 2's (call r0) */
		return kcall_mp_dispatch(m, kcall_return_pc(m));
	default:
		return "unimplemented kernel call";
	}

	cpu->pc = kcall_return_pc(m);
	return NULL;
}

/*
 * Builds the machine (m holds only the ROM, the RAM size and the host-side
 * state of a restart) and leaves the 68k emulator about to start at the ROM's
 * entry.
 */
static int start_machine(struct machine *m, const struct nk_boot_options *opt)
{
	char err[256];

	m->logical_pages = m->ram_size >> MEM_PAGE_SHIFT;
	if (nk_config_parse(m->rom, &m->ci, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return -1;
	}
	if (setup_machine(m, opt))
		return -1;
	{
		uint32_t irp = m->ci.la_info_record & ~(MEM_PAGE_SIZE - 1);     /* see kdp_pull() */
		mem_map(&m->mem, irp, MEM_PAGE_SIZE, m->mem.pageflags[irp >> MEM_PAGE_SHIFT] & ~MEM_W);
	}
	if (m->realtime || m->clock_mips)
		m->cpu.tb_step = 0;     /* the host clock or the instruction count sets the timebase between chunks */
	/* the emulator dispatches 68k opcode X through dispatch_table + 8 * X; see also recompiled_trap_entry() */
	m->cpu.hook_pc = m->ci.la_dispatch_table + 8u * TRAP_SYNCH_IDLE_TIME;
	m->cpu.mmio = getenv("SYSHM_DEVICE_EXITS") ? NULL : device_hook;      /* (for comparisons: each access exits) */
	m->cpu.mmio_ctx = m;
	uint32_t entry = recompiled_trap_entry(m);
	m->recompiled_traps = entry;
	if (entry) {
		m->cpu.hook_match_pc = entry;
		m->cpu.hook_match_reg = 24;
		m->cpu.hook_match_value = TRAP_SYNCH_IDLE_TIME;
	}
	return 0;
}

/* Frees what start_machine() set up (not the window, sound output or WAV file). */
static void stop_machine(struct machine *m)
{
	for (unsigned k = 0; k < m->disks; k++)
		disk_close(&m->disk[k]);
	rave_free(&m->rave);
	net_free(&m->net);
	jit_free(m->cpu.jit);
	mem_free(&m->mem);
}

/*
 * The guest asked to restart (RTAS system-reboot, or Cuda's reset): start
 * over from the ROM with memory and devices reset, as a real Mac does,
 * keeping what survives a restart there (NVRAM) and what the host side
 * counts across it: guest time, the instruction count (which input scripts
 * and --max go by), statistics, and the window and sound output.
 */
static int restart_machine(struct machine *m, const struct nk_boot_options *opt)
{
	static struct machine old;

	old = *m;
	stop_machine(m);
	memset(m, 0, sizeof *m);
	m->rom = old.rom;
	m->ram_size = old.ram_size;
	m->audio = old.audio;
	m->wav = old.wav;
	m->wav_frames = old.wav_frames;
	m->wav_rate = old.wav_rate;
	m->perf_on = old.perf_on;
	m->perf = old.perf;
	m->clock_mips = old.clock_mips;
	m->clock_tb = old.clock_tb;
	m->clock_icount = old.clock_icount;
	m->perf_ns = old.perf_ns;
	m->perf_cycles = old.perf_cycles;
	m->perf_insns = old.perf_insns;
	m->perf_icount = old.perf_icount;
	m->perf_idle_ns = old.perf_idle_ns;
	m->want_w = old.want_w;
	m->want_h = old.want_h;
	m->want_ns = old.want_ns;
	m->display = old.display;
	m->gpu = old.gpu;
	m->rgb = old.rgb;
	m->shown = old.shown;
	m->quit = old.quit;
	m->mouse_captured = old.mouse_captured;
	m->mouse_released = old.mouse_released;
	m->redraw = true;
	m->scripted = old.scripted;
	m->realtime = old.realtime;
	m->idle_sleep = old.idle_sleep;
	m->start_ns = old.start_ns;
	m->next_frame_ns = old.next_frame_ns;
	m->idle_calls = old.idle_calls;
	m->idle_ns = old.idle_ns;
	m->dev_accesses = old.dev_accesses;
	m->dev_inline = old.dev_inline;
	m->net_poll_ns = old.net_poll_ns;
	memcpy(m->exceptions, old.exceptions, sizeof m->exceptions);
	if (start_machine(m, opt))
		return -1;
	memcpy(m->rtas.nvram, old.rtas.nvram, sizeof m->rtas.nvram);
	m->cpu.icount = old.cpu.icount;
	m->cpu.tb = old.cpu.tb;
	if (m->realtime)        /* (the guest reads the clock again: the lead guest time had is moot) */
		m->cpu.tb = ns_to_tb(host_ns() - m->start_ns);
	return 0;
}

/*
 * The 3D engine draws with the GPU in real time, if the host has hardware
 * OpenGL; runs in instruction time draw in software, so that they are the
 * same on every host. SYSHM_GPU=1 has the GPU draw in any run (and takes
 * software OpenGL too); SYSHM_GPU=0 or --no-gpu, never.
 */
static struct gpu *open_gpu(const struct nk_boot_options *opt)
{
	const char *env = getenv("SYSHM_GPU");
	bool forced = env && strcmp(env, "1") == 0;
	if (opt->no_gpu || (env && strcmp(env, "0") == 0) || (!forced && !opt->realtime))
		return NULL;
	return gpu_open(forced);
}

int nk_boot(const struct rom *rom, const struct nk_boot_options *opt)
{
	static struct machine m;

	memset(&m, 0, sizeof m);
	m.rom = rom;
	m.ram_size = opt->ram_size;
	m.realtime = opt->realtime;
	m.gpu = open_gpu(opt);          /* (getting ready in the background) */
	if (start_machine(&m, opt))
		return 1;
	bool trap_log_armed = false;

	printf("boot: emulator entry 0x%08X, emulator data 0x%08X, dispatch table 0x%08X, %u MB RAM\n",
	       m.cpu.pc, m.cpu.gpr[3], m.cpu.gpr[4], m.ram_size >> 20);

	struct script script = { 0 };
	if (opt->input_script && script_load(&script, opt->input_script))
		return 1;
	m.scripted = opt->input_script != NULL;

	m.start_ns = host_ns();
	m.idle_sleep = opt->realtime && !opt->no_idle;
	if (opt->realtime && !opt->no_fast_start)
		fast_start_begin(&m);
	if (opt->window) {
		m.rgb = malloc((size_t)FB_SIZE / 4 * 3 + 3);
		m.shown = malloc(FB_SIZE);
		if (m.rgb && m.shown)
			m.display = display_open("syshm", video_width(&m.video), video_height(&m.video),
						 opt->integer_scale);
		if (m.display && !opt->no_status_bar)
			display_status_bar(m.display, true);
		if (m.display && opt->realtime && !opt->no_sound)
			m.audio = audio_open();         /* (in instruction time, sound would be too fast or slow) */
	}

	struct ppc_cpu *cpu = &m.cpu;
	const char *why = NULL;
	unsigned io_pages = 0;
	unsigned long kcalls[16] = { 0 };
	unsigned long dev_stops = 0;
	unsigned long traps_logged = 0;
	bool sync = true;               /* at a device sync point (see below) */

	/*
	 * Devices, interrupts and input are serviced at fixed instruction counts
	 * (every SYNC_INSNS) and at real exits to the kernel, never at exits made
	 * only for debugging (trap logging, trace start, write watches), so those
	 * don't change what the guest does.
	 */
	while (!why) {
		if (m.preempt && blue_running(&m)) {
			m.preempt = false;
			if (!m.system && (why = raise_exception(&m, EC_NO_EXCEPTION, cpu->pc)))
				break;
		}
		if (sync && (opt->realtime || m.display)) {
			uint64_t now = host_ns();
			if (m.fast_start)
				fast_start_step(&m, now);
			uint64_t tb = ns_to_tb(now - m.start_ns) + m.lead_tb;
			if (opt->realtime && tb > cpu->tb)
				cpu->tb = tb;
			if (m.display && now >= m.next_frame_ns) {
				display_frame(&m, now);
				m.next_frame_ns = now + 1000000000u / 60;
				if (m.quit) {
					why = m.shut_down ? "shut down from the menu" : "window closed";
					break;
				}
				if (m.restart) {
					printf("restart: from the menu at instruction %llu\n", (unsigned long long)cpu->icount);
					if (restart_machine(&m, opt))
						return 1;
					if (opt->realtime && !opt->no_fast_start)
						fast_start_begin(&m);
					trap_log_armed = false;
					sync = true;
					continue;
				}
			}
		}
		if (opt->trap_log && !trap_log_armed && cpu->icount >= opt->trap_log_from) {
			/* the emulator dispatches 68k opcode X through dispatch_table + 8 * X */
			cpu_set_trap_break(&m);
			trap_log_armed = true;
		}
		if (sync) {
			script_run(&m, &script);
			follow_window(&m);
			cuda_poll(&m.cuda, cpu->tb / (TIMEBASE_HZ / 1000));
			m.dev_dirty = true;
			if (m.net.backend) {    /* (every millisecond is plenty, and polling costs) */
				uint64_t now = host_ns();
				if (now - m.net_poll_ns >= NET_POLL_NS) {
					m.net_poll_ns = now;
					net_poll(&m.net, 0);
				}
			}
			if (m.net.kick) {       /* (frames came, here or while idle) */
				m.net.kick = false;
				macio_pulse(&m.macio, IRQ_DMA_ENET_RX);
			}
			if (m.perf_on)
				perf_report(&m);
		}

		uint64_t left = opt->max_insns - cpu->icount;
		if (left == 0) {
			why = "instruction limit reached";
			break;
		}
		if (sync && opt->max_seconds > 0 && host_ns() - m.start_ns >= opt->max_seconds * 1e9) {
			why = "time limit reached";
			break;
		}
		uint64_t next_sync = (cpu->icount / SYNC_INSNS + 1) * SYNC_INSNS;
		uint64_t chunk = next_sync - cpu->icount < left ? next_sync - cpu->icount : left;
		if (opt->trace && !cpu->trace) {
			if (cpu->icount >= opt->trace_from)
				cpu->trace = stderr;
			else if (opt->trace_from - cpu->icount < chunk)
				chunk = opt->trace_from - cpu->icount;
		}
		if (m.clock_mips) {
			uint64_t d = cpu->icount - m.clock_icount, rate = (uint64_t)m.clock_mips * 1000000u;
			cpu->tb = m.clock_tb + d / rate * TIMEBASE_HZ + d % rate * TIMEBASE_HZ / rate;
		}
		if (opt->watch)
			watch_protect(&m, opt->watch, true);
		/* (a random one in RUN_SAMPLE timed, for the status bar's speed: see status_lights()) */
		m.run_sample = m.run_sample * 1664525u + 1013904223u;
		bool timed = m.display && m.run_sample < UINT32_MAX / RUN_SAMPLE;
		uint64_t run_from = timed ? host_ns() : 0, insns_from = cpu->icount;
		m.mmio_until = m.clock_mips ? 0 : cpu->icount + chunk;         /* (see device_hook()) */
		if (opt->trap_log && !trap_log_armed && opt->trap_log_from < m.mmio_until)
			m.mmio_until = opt->trap_log_from;
		enum ppc_exit e = ppc_run(cpu, chunk);
		m.mmio_until = 0;
		if (timed) {
			m.run_ns += host_ns() - run_from;
			m.run_insns += cpu->icount - insns_from;
		}
		if (opt->watch)
			watch_protect(&m, opt->watch, false);
		sync = cpu->icount >= next_sync;
		bool info_write = e == PPC_EXIT_DSI && cpu->fault_write &&
				  (cpu->fault_addr >> MEM_PAGE_SHIFT) == (m.ci.la_info_record >> MEM_PAGE_SHIFT);
		bool quiet_write = e == PPC_EXIT_DSI && cpu->fault_write && !info_write &&
				   (quiet_page(&m, cpu->fault_addr) || quiet_page(&m, cpu->fault_addr + 127));
		bool debug_exit = e == PPC_EXIT_BREAK || (e == PPC_EXIT_BUDGET && !sync) ||
				  ((info_write || quiet_write) && !sync) ||
				  (e == PPC_EXIT_HOOK && !sync && !opt->realtime) ||
				  (e == PPC_EXIT_DSI && opt->watch && cpu->fault_write &&
				   (cpu->fault_addr >> MEM_PAGE_SHIFT) == (opt->watch >> MEM_PAGE_SHIFT));
		if (!debug_exit && e != PPC_EXIT_MMIO && (sync || m.dev_dirty || cpu->tb >= m.dev_next_tb)) {
			update_device_time(&m);
			update_interrupts(&m, false);
			m.dev_dirty = false;
			m.dev_next_tb = device_next_tb(&m);
		}
		bool dev_clean = e == PPC_EXIT_BUDGET;  /* (whether handling the exit leaves devices alone) */

		switch (e) {
		case PPC_EXIT_BUDGET:
			break;
		case PPC_EXIT_MMIO:     /* device_hook() did an access and stopped after it, as below */
			dev_stops++;
			dev_clean = true;
			why = device_stop(&m);
			break;
		case PPC_EXIT_TRAP:
			if ((cpu->exit_insn & TWI_KCALL_MASK) == TWI_KCALL) {
				unsigned n = cpu->exit_insn & 0xFFFF;
				kcalls[n < 16 ? n : 15]++;
				dev_clean = n == 0 || n == 1 || n == 3; /* ReturnFromException, RunAlternateContext, VMDispatch */
				why = kernel_call(&m, n);
				if (why && !m.rtas.reboot)
					printf("kernel call %u (%s) at pc 0x%08X, r3=0x%08X r4=0x%08X r5=0x%08X\n",
					       n, nk_kcall_name(n), cpu->pc, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
			} else {
				why = exception(&m, EC_TRAP_INSTR, cpu->pc);
			}
			break;
		case PPC_EXIT_SC:
			if (m.mp_kernel)        /* NanoKernel 2's way to Multiprocessing Services */
				why = system_call(&m);
			else
				why = exception(&m, EC_SYSTEM_CALL, cpu->pc + 4);
			break;
		case PPC_EXIT_HOSTCALL:
			why = host_call(&m, cpu->exit_insn & 0x03FFFFFFu);
			break;
		case PPC_EXIT_HOOK:
			blue_naps(&m, UINT64_MAX);
			if (opt->realtime)
				sync = true;    /* catch up with the host clock now */
			break;
		case PPC_EXIT_BREAK: {
			uint32_t off = cpu->pc - cpu->break_lo, at = cpu->gpr[24];
			if (cpu->pc == cpu->break_pc && mem_ok(&m.mem, at, 2, MEM_R) && traps_logged < opt->trap_log) {
				log_trap(&m, mem_r16(&m.mem, at), at + 2);      /* (r24 points at the trap word here) */
				traps_logged++;
			} else if (cpu->pc != cpu->break_pc && off % 8 == 0 && traps_logged < opt->trap_log) {
				log_trap(&m, (uint16_t)(0xA000 + off / 8), at);
				traps_logged++;
			}
			if (traps_logged >= opt->trap_log) {
				cpu->break_lo = cpu->break_hi = 0;
				cpu->break_pc = PPC_NO_HOOK;
			}
			cpu->break_skip = true;
			break;
		}
		case PPC_EXIT_ILLEGAL:
			why = exception(&m, EC_INVALID_INSTR, cpu->pc);
			break;
		case PPC_EXIT_ISI:
			if (!blue_running(&m) && cpu->pc == m.mp.task_return) {
				if (m.mp.log)
					printf("mp: task 0x%08X returned 0x%08X\n", m.mp.current, cpu->gpr[3]);
				mp_terminate(&m.mp, m.mp.current, cpu->gpr[3]);
				break;
			}
			why = exception(&m, EC_INST_INVALID_ADDRESS, cpu->pc);
			break;
		case PPC_EXIT_PRIV:
			if (is_mfmsr(cpu->exit_insn)) {
				cpu->gpr[(cpu->exit_insn >> 21) & 31] = USER_MSR;
				cpu->pc += 4;
				break;
			}
			why = exception(&m, EC_PRIVILEGED_INSTR, cpu->pc);
			break;
		case PPC_EXIT_DSI:
			if (info_write) {
				uint8_t *flags = &m.mem.pageflags[m.ci.la_info_record >> MEM_PAGE_SHIFT];
				*flags |= MEM_W;
				enum ppc_exit e1 = ppc_interp(cpu, 1);
				*flags &= (uint8_t)~MEM_W;
				kdp_pull(&m);
				if (e1 != PPC_EXIT_BUDGET) {
					why = "fault storing to the info records";
					break;
				}
				if (!sync && cpu->icount >= next_sync) {        /* the store ended the chunk */
					sync = true;
					update_device_time(&m);
					update_interrupts(&m, false);
				}
				break;
			}
			if (quiet_write && quiet_store(&m)) {
				if (!sync && cpu->icount >= next_sync) {        /* the store ended the chunk */
					sync = true;
					update_device_time(&m);
					update_interrupts(&m, false);
				}
				break;
			}
			if (opt->watch && cpu->fault_write &&
			    (cpu->fault_addr >> MEM_PAGE_SHIFT) == (opt->watch >> MEM_PAGE_SHIFT)) {
				/* step the store (the page is writable again outside ppc_run) */
				uint32_t a = cpu->fault_addr & ~3u, before = 0;
				if (mem_ok(&m.mem, a, 4, MEM_R))
					before = mem_r32(&m.mem, a);
				uint32_t pc = cpu->pc, r24 = cpu->gpr[24];
				ppc_run(cpu, 1);
				printf("watch: insn %llu pc 0x%08X lr 0x%08X (r24 0x%08X) wrote 0x%08X: %08X -> %08X\n",
				       (unsigned long long)cpu->icount, pc, cpu->lr, r24, cpu->fault_addr, before, mem_r32(&m.mem, a));
				if (!sync && cpu->icount >= next_sync) {        /* the store ended the chunk */
					sync = true;
					update_device_time(&m);
					update_interrupts(&m, false);
				}
				break;
			}
			if (mmio_find(&m.bus, cpu->fault_addr)) {
				struct mmio_access a;
				if (!mmio_decode(&m.bus, cpu, &a)) {
					update_device_time(&m);
					why = "unsupported instruction form for a device register";
					break;
				}
				device_access(&m, &a);
				dev_clean = true;
				why = device_stop(&m);
				break;
			}
			if (opt->io_as_ram && cpu->fault_addr >= 0x80000000u &&
			    !mem_ok(&m.mem, cpu->fault_addr, 1, MEM_R)) {
				uint32_t page = cpu->fault_addr & ~(MEM_PAGE_SIZE - 1);
				printf("i/o: page 0x%08X first %s by pc 0x%08X\n",
				       page, cpu->fault_write ? "written" : "read", cpu->pc);
				memset(mem_ptr(&m.mem, page), 0, MEM_PAGE_SIZE);
				mem_map(&m.mem, page, MEM_PAGE_SIZE, MEM_RW);
				io_pages++;
				break;
			}
			why = exception(&m, (cpu->fault_write && mem_ok(&m.mem, cpu->fault_addr, 1, MEM_R))
					? EC_DATA_WRITE_VIOLATION : EC_DATA_INVALID_ADDRESS, cpu->pc);
			break;
		default:
			why = ppc_exit_name(e);
			break;
		}
		if (m.mp_kernel && !why && !debug_exit && mp_due(&m.mp, cpu->tb))
			mp_tick(&m.mp, cpu->tb, mp_fires(&m.mp, cpu->tb) ? blue_boost(&m) : MP_AS_IS);
		if (m.mp.swi)
			post_software_interrupt(&m);
		if (m.mp.yield && !why)         /* (Multiprocessing Services: the task to run may change) */
			switch_task(&m, mp_next(&m.mp, blue_boost(&m)));
		if (!dev_clean)
			m.dev_dirty = true;
		if (why && (m.rtas.reboot || m.cuda.reset)) {
			printf("restart: %s at instruction %llu\n", why, (unsigned long long)cpu->icount);
			if (restart_machine(&m, opt))
				return 1;
			if (opt->realtime && !opt->no_fast_start)
				fast_start_begin(&m);
			why = NULL;
			trap_log_armed = false;
			sync = true;
		}
	}

	if (blue_running(&m))
		printf("stopped: %s (%s context)\n", why, m.system ? "system" : "native");
	else
		printf("stopped: %s (task 0x%08X)\n", why, m.mp.current);
	printf("insn 0x%08X at pc 0x%08X, last fault address 0x%08X; %u i/o pages touched\n",
	       cpu->exit_insn, cpu->pc, cpu->fault_addr, io_pages);
	printf("exceptions:");
	for (unsigned c = 0; c < 32; c++)
		if (m.exceptions[c])
			printf(" %u=%lu", c, m.exceptions[c]);
	printf("\n");
	if (be32(m.kdp + KDP_NK_INFO + NKI_QUIET_WRITES))
		printf("quiet writes: %u (stores to the ROM, dropped)\n", be32(m.kdp + KDP_NK_INFO + NKI_QUIET_WRITES));
	printf("kernel calls:");
	for (unsigned n = 0; n < 16; n++)
		if (kcalls[n])
			printf(" %s=%lu", nk_kcall_name(n), kcalls[n]);
	printf("\ndevice registers: %lu accesses, %lu without leaving the run (%lu of them stopping it after)",
	       m.dev_accesses, m.dev_inline, dev_stops);
	printf("\ncuda packets: %lu, interrupt level %u, pending %08X %08X, via ifr %02X ier %02X",
	       m.cuda.packets, m.ipl, m.pending[0], m.pending[1], m.via.ifr, m.via.ier);
	printf("\nadb: %lu commands, autopoll %s (devices %04X, %u ms), keyboard @%u handler %u, mouse @%u handler %u",
	       m.cuda.adb.commands, m.cuda.autopoll ? "on" : "off", m.cuda.autopoll_devices, m.cuda.autopoll_ms,
	       m.cuda.adb.kbd.addr, m.cuda.adb.kbd.handler, m.cuda.adb.mouse.addr, m.cuda.adb.mouse.handler);
	printf("\nvideo: %lu driver calls, %u opens, %ux%u %u-bit, %lu mode changes, %lu VBLs",
	       m.video.calls, m.video.open_count, video_width(&m.video), video_height(&m.video),
	       video_bpp(&m.video), m.video.mode_changes, m.video.vbls);
	for (unsigned k = 0; k < m.disks; k++)
		printf("\ndisk %u: %lu driver calls, drive %u, %lu reads (%llu KB), %lu writes (%llu KB)", k,
		       m.disk[k].calls, m.disk[k].part[0].drive_num, m.disk[k].reads,
		       (unsigned long long)(m.disk[k].bytes_read >> 10), m.disk[k].writes,
		       (unsigned long long)(m.disk[k].bytes_written >> 10));
	if (m.helper.launches)
		printf("\nenhancements: %s, %lu mode switches, %lu failed (last error %d)",
		       m.helper.running ? "running" : "quit", m.helper.switches, m.helper.failures, m.helper.last_error);
	if (m.rave.calls)
		printf("\n3d engine: %lu calls, %lu contexts, %lu textures, %lu frames, %lu triangles (%lu back-facing)",
		       m.rave.calls, m.rave.contexts, m.rave.textures, m.rave.frames, m.rave.triangles, m.rave.backfacing);
	if (m.rave.gpu_frames)
		printf("; %lu frames drawn with %s", m.rave.gpu_frames, gpu_renderer(m.gpu));
	if (m.awacs.codec_writes)
		printf("\nsound: %lu codec writes, %u Hz, %lu frames played, %lu recorded",
		       m.awacs.codec_writes, awacs_rate(&m.awacs), m.awacs.frames_played, m.awacs.frames_recorded);
	if (m.audio)
		printf("\naudio: %lu frames dropped, %lu frames of silence added",
		       audio_dropped(m.audio), audio_padded(m.audio));
	for (unsigned n = 0; n < DBDMA_CHANNELS; n++)
		if (m.dbdma.ch[n].commands || m.dbdma.ch[n].status)
			printf("\ndma %u: %lu commands, %lu interrupts, status %04X", n, m.dbdma.ch[n].commands,
			       m.dbdma.ch[n].interrupts, m.dbdma.ch[n].status);
	printf("\n");
	if (m.cpu.jit)
		jit_print_stats(m.cpu.jit, stdout);
	if (m.net.sent || m.net.received)
		printf("net: %lu frames sent, %lu received, %lu dropped\n", m.net.sent, m.net.received, m.net.dropped);
	uint64_t cycles, insns;
	if (m.perf_on && perf_read(&m.perf, &cycles, &insns) && m.cpu.icount)
		printf("perf total: %.3f billion host cycles; %.2f instructions and %.3f cycles per guest instruction\n",
		       cycles * 1e-9, (double)insns / (double)m.cpu.icount, (double)cycles / (double)m.cpu.icount);
	printf("idle: %lu calls, host slept %.1f s of %.1f s", m.idle_calls, m.idle_ns / 1e9,
	       (host_ns() - m.start_ns) / 1e9);
	printf("\nrtas calls:");
	for (unsigned t = 0; t < 16; t++)
		if (m.rtas.calls[t])
			printf(" %s=%lu", rtas_name(t), m.rtas.calls[t]);
	printf("\n");
	if (m.mp_kernel)
		printf("nvram: %lu reads, %lu writes\n", m.nvram.reads, m.nvram.writes);
	ppc_dump(cpu, stdout);
	if (opt->screenshot) {
		if (video_screenshot(&m.video, opt->screenshot))
			fprintf(stderr, "cannot write %s\n", opt->screenshot);
		else
			printf("screen saved to %s\n", opt->screenshot);
	}

	if (opt->nvram && rtas_nvram_save(&m.rtas, opt->nvram))
		fprintf(stderr, "cannot write %s\n", opt->nvram);
	if (opt->dump_ram) {
		FILE *f = fopen(opt->dump_ram, "wb");
		if (!f || fwrite(mem_ptr(&m.mem, 0), 1, m.ram_size, f) != m.ram_size)
			fprintf(stderr, "cannot write %s\n", opt->dump_ram);
		if (f)
			fclose(f);
	}
	stop_machine(&m);
	gpu_close(m.gpu);
	if (m.wav) {
		uint8_t h[44];
		wav_header(h, m.wav_frames, m.wav_rate ? m.wav_rate : 44100);
		fseek(m.wav, 0, SEEK_SET);
		fwrite(h, 1, sizeof h, m.wav);
		fclose(m.wav);
	}
	audio_close(m.audio);
	display_close(m.display);
	free(m.rgb);
	free(m.shown);
	free(script.ev);
	return 0;
}