#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cdimage.h"
#include "disk.h"
#include "hfsread.h"
#include "mem.h"
#include "nk.h"
#include "ppc.h"
#include "rom.h"
#include "share.h"
#include "tools.h"

#define TYPE_TBXI 0x74627869u           /* 'tbxi': the Mac OS ROM file's type */

static int usage(void)
{
	fprintf(stderr,
		"usage: syshm <command> [args]\n"
		"\n"
		"  rom info <file>              describe a Mac OS ROM file or 4 MB ROM image\n"
		"  rom config <file>            show the NanoKernel ConfigInfo and memory map\n"
		"  rom parcels <file>           list the parcels of a Mac OS 9.1-or-later ROM file\n"
		"  rom extract <file> <out>     write the decoded 4 MB Toolbox ROM image\n"
		"  rom trampoline <file> <out>  write the NewWorld Trampoline (ELF)\n"
		"                               (the file may be a disk or CD image: then the Mac OS\n"
		"                               ROM file in its System Folder, as boot would use it)\n"
		"  boot [options]               start Mac OS under the native kernel, from the Mac OS\n"
		"                               ROM file in the startup disk's System Folder (the first\n"
		"                               drive with one, as a New World Mac's firmware does)\n"
		"      --rom <file>             use this Mac OS ROM file or 4 MB ROM image instead\n"
		"      --ram <MB>               RAM size (default 128)\n"
		"      --window                 show the screen in a window (keyboard and mouse go to\n"
		"                               the guest); runs in real time until closed. Once the\n"
		"                               Finder is up, offers to install the guest tools on the\n"
		"                               startup disk if they're missing or another syshm's\n"
		"      --realtime               guest time follows the host clock\n"
		"      --instruction-time       with --window: guest time counts instructions, as\n"
		"                               without a window (scripted runs are reproducible)\n"
		"      --no-idle                in real time, don't sleep while the guest is idle\n"
		"      --no-fast-start          in real time, don't run guest time faster until the\n"
		"                               Finder is up\n"
		"      --no-sound               with --window: don't play the guest's sound (only\n"
		"                               played in real time)\n"
		"      --integer-scale          with --window: scale the screen by whole numbers only\n"
		"                               (crisp pixels; may leave a border)\n"
		"      --no-status-bar          with --window: start without the status bar (its\n"
		"                               lights and the processor's speed; F12's menu shows it)\n"
		"      --no-net                 in real time, no network behind the guest's Ethernet\n"
		"                               (with: NAT to the host's, DHCP on 10.0.2.0/24)\n"
		"      --no-gpu                 the guest tools' 3D engine draws in software (in real\n"
		"                               time it draws with the GPU, if there's OpenGL 3.3)\n"
		"      --interp                 interpret PowerPC code instead of translating it\n"
		"      --seconds <s>            stop after s seconds of host time\n"
		"      --fixed-time <unix time> start the guest clock here, not at the host's time\n"
		"                               (with instruction time, runs are reproducible)\n"
		"      --input <file>           scripted input, lines of \"<insn count> <command>\":\n"
		"                               move X Y, click X Y, dblclick X Y, down, up,\n"
		"                               motion DX DY (the mouse moved, as games read it),\n"
		"                               key HEX [down|up], resize W H, screenshot FILE,\n"
		"                               floppy insert FILE, floppy eject, cdrom insert FILE,\n"
		"                               cdrom eject,\n"
		"                               tools install|later (the answer to that offer)\n"
		"      --max <n>                stop after n PowerPC instructions (default 500000000)\n");
	fprintf(stderr,
		"      --io-ram                 back touched I/O pages with plain RAM and keep going\n"
		"      --trace                  log every instruction to stderr\n"
		"      --trace-from <n>         start logging after n instructions (implies --trace)\n"
		"      --trace-reg <r>          include GPR r in each log line (r24 = 68k pc)\n"
		"      --trace-range <lo:hi>    log only instructions at hex addresses lo..hi, with\n"
		"                               instruction counts, r3-r5 and lr (implies --trace)\n"
		"      --trace-insn <hex>       ... and only this instruction word\n"
		"      --watch <hex addr>       log every write to the page holding addr\n"
		"      --traps <n>              log the first n 68k A-line traps\n"
		"      --traps-after <n>        ... starting after n instructions\n"
		"      --disk <image>           attach a disk image (writable if the file is);\n"
		"                               drives are numbered, and tried for startup, in\n"
		"                               command-line order\n"
		"      --cdrom <image>          attach an image read-only, in a CD-ROM drive (whose\n"
		"                               disc can be swapped in a window: drop an image on it)\n"
		"      --cdrom-drive            an empty CD-ROM drive\n"
		"      --floppy <image>         a floppy image for the floppy drive (raw 400K-1440K, or\n"
		"                               Disk Copy 4.2); the first is in it at startup, the\n"
		"                               others go in, in turn, as Mac OS ejects the one in\n"
		"      --floppy-drive           a floppy drive, empty at startup (drop images on the\n"
		"                               window to put them in)\n"
		"      --share <folder>         show a host folder to Mac OS as a volume; what Mac OS\n"
		"                               changes on it is written back when it shuts down\n"
		"      --nvram <file>           keep NVRAM (PRAM, settings) in this file\n"
		"      --screenshot <file.png>  save the screen when the run ends\n"
		"      --dump-ram <file>        save guest RAM when the run ends\n"
		"      --log-hostcalls          log driver requests handled natively\n"
		"  disk toc <image>             list a CD image's tracks (a .cue sheet's, or one data\n"
		"                               track) as its CD-ROM drive gives them\n"
		"  disk create <file> <size> [name]\n"
		"                               make a disk image holding an empty HFS volume\n"
		"                               (size in bytes, or with K, M or G; 1M or more, or a\n"
		"                               floppy's: 400K, 720K, 800K or 1440K)\n"
		"  tools create <file>          make a volume of the guest tools, to install them by\n"
		"                               hand (attach it with --cdrom; drag its files onto\n"
		"                               the System Folder); in a window, syshm offers to\n"
		"                               install them itself\n"
		"  run-raw <file> <addr> [n]    load raw PowerPC code at hex <addr> in 64 MB of\n"
		"                               RAM, run up to n instructions, dump registers\n");
	return 2;
}

static int write_file(const char *path, const void *data, size_t size)
{
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(data, 1, size, f) != size) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		if (f)
			fclose(f);
		return 1;
	}
	return fclose(f) ? 1 : 0;
}

/* Whether a file is a ROM file (4 MB, or a bootinfo file) rather than a disk image; true if unreadable */
static bool is_rom_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	char head[11];
	if (!f)
		return true;            /* rom_load() says why */
	bool rom = fread(head, 1, sizeof head, f) == sizeof head && memcmp(head, "<CHRP-BOOT>", sizeof head) == 0;
	if (!rom && fseek(f, 0, SEEK_END) == 0)
		rom = ftell(f) == ROM_IMAGE_SIZE;
	fclose(f);
	return rom;
}

/*
 * Loads the Mac OS ROM file from the startup disk, as New World Macs'
 * firmware does: the first drive whose blessed System Folder holds a file of
 * type 'tbxi'. Says where it was in `from`, and which drive and partition in
 * `drive` and `part` (if not NULL). Returns 0, 1 if no drive has one, or -1
 * (having said why).
 */
static int load_startup_rom(const char *const *paths, unsigned count, struct rom *rom, char *from, size_t fromlen,
			    unsigned *drive, unsigned *part)
{
	struct hfsread_found found;
	char err[HFSREAD_PATH_MAX + 256], name[HFSREAD_PATH_MAX + 256];
	int rc = hfsread_system_file(paths, count, TYPE_TBXI, &found, err, sizeof err);
	if (rc <= 0) {
		if (rc < 0 && err[0])
			fprintf(stderr, "%s\n", err);
		return rc < 0 ? -1 : 1;
	}
	snprintf(name, sizeof name, "%s: \"%s\"", paths[found.drive], found.path);
	rc = rom_load_mem(found.data, found.size, name, rom, err, sizeof err);
	free(found.data);
	if (rc) {
		fprintf(stderr, "%s\n", err);
		return -1;
	}
	snprintf(from, fromlen, "\"%s\" from %s%s%s%s", found.path, paths[found.drive],
		 found.partition[0] ? ", partition \"" : "", found.partition, found.partition[0] ? "\"" : "");
	if (drive)
		*drive = found.drive;
	if (part)
		*part = found.part;
	return 0;
}

static int cmd_rom(int argc, char **argv)
{
	if (argc < 2)
		return usage();

	const char *sub = argv[0], *path = argv[1];
	struct rom rom;
	char err[256], from[HFSREAD_PATH_MAX + 256] = "";

	if (is_rom_file(path)) {
		if (rom_load(path, &rom, err, sizeof err)) {
			fprintf(stderr, "%s\n", err);
			return 1;
		}
	} else {
		int found = load_startup_rom(&path, 1, &rom, from, sizeof from, NULL, NULL);
		if (found > 0)
			fprintf(stderr, "%s: not a ROM file, nor a disk image with a Mac OS ROM file in its System Folder\n",
				path);
		if (found)
			return 1;
		if (strcmp(sub, "info") != 0)
			fprintf(stderr, "rom: %s\n", from);
	}

	int rc = 0;
	if (strcmp(sub, "info") == 0) {
		char nk[40], version[64];
		uint32_t stored = rom_stored_checksum(&rom), computed = rom_computed_checksum(&rom);

		rom_nanokernel_id(&rom, nk, sizeof nk);
		printf("file:          %s (%zu bytes)\n", from[0] ? from : path, rom.file_size);
		if (rom.format == ROM_FORMAT_BOOTINFO || rom.format == ROM_FORMAT_PARCELS) {
			printf("format:        NewWorld bootinfo, %s\n",
			       rom.format == ROM_FORMAT_PARCELS ? "parcels" : "LZSS Toolbox image");
			printf("compatible:    %s\n", rom.compatible);
			if (rom.elf_size)
				printf("trampoline:    ELF at 0x%X, %u bytes, entry 0x%08X\n",
				       rom.elf_offset, rom.elf_size, rom.elf_entry);
			if (rom.format == ROM_FORMAT_PARCELS) {
				printf("parcels:       %u at 0x%X, %u bytes (\"rom parcels\" lists them)\n",
				       rom.parcels.count, rom.lzss_offset, rom.lzss_size);
				if (rom_file_version(&rom, version, sizeof version))
					printf("file version:  %s\n", version);
			} else {
				printf("toolbox image: %u bytes compressed at 0x%X -> %u bytes\n",
				       rom.lzss_size, rom.lzss_offset, ROM_IMAGE_SIZE);
			}
		} else {
			printf("format:        raw 4 MB ROM image\n");
		}
		printf("checksum:      0x%08X (%s)\n", stored, stored == computed ? "ok" : "MISMATCH");
		printf("rom version:   0x%04X, release 0x%04X\n", rom_version(&rom), rom_release(&rom));
		printf("nanokernel id: \"%s\"\n", nk);
		printf("resource map:  0x%08X\n", rom_resource_map_offset(&rom));
		printf("trap tables:   0x%08X\n", rom_trap_table_offset(&rom));
	} else if (strcmp(sub, "config") == 0) {
		struct nk_config ci;
		if (nk_config_parse(&rom, &ci, err, sizeof err)) {
			fprintf(stderr, "%s: %s\n", path, err);
			rc = 1;
		} else {
			nk_config_dump(&rom, &ci, stdout);
		}
	} else if (strcmp(sub, "parcels") == 0) {
		if (rom.format != ROM_FORMAT_PARCELS)
			fprintf(stderr, "%s: no parcels in this file\n", path);
		for (unsigned k = 0; k < rom.parcels.count; k++) {
			const struct parcel *p = &rom.parcels.list[k];
			printf("'%s' flags 0x%05X", p->type, p->flags);
			if (p->a[0] || p->b[0])
				printf("  %s / %s", p->a, p->b);
			printf("\n");
			for (unsigned j = 0; j < p->count; j++) {
				const struct parcel_blob *b = &p->blobs[j];
				printf("    '%s' flags 0x%05X %8u bytes  %s", b->type, b->flags, b->size, b->name);
				if (strcmp(b->type, "cstr") == 0 || strcmp(b->type, "csta") == 0)
					for (uint32_t o = 0; o < b->size; o += (uint32_t)strnlen((const char *)b->data + o, b->size - o) + 1)
						printf(" \"%.*s\"", (int)strnlen((const char *)b->data + o, b->size - o), b->data + o);
				printf("\n");
			}
		}
		rc = rom.format != ROM_FORMAT_PARCELS;
	} else if (strcmp(sub, "extract") == 0 && argc >= 3) {
		rc = write_file(argv[2], rom.image, ROM_IMAGE_SIZE);
	} else if (strcmp(sub, "trampoline") == 0 && argc >= 3) {
		if (!rom.elf_size) {
			fprintf(stderr, "%s: no Trampoline in this file\n", path);
			rc = 1;
		} else {
			rc = write_file(argv[2], rom.file + rom.elf_offset, rom.elf_size);
		}
	} else {
		rc = usage();
	}

	rom_free(&rom);
	return rc;
}

static int cmd_run_raw(int argc, char **argv)
{
	if (argc < 2)
		return usage();

	const uint32_t ram_size = 64u << 20;
	uint32_t addr = (uint32_t)strtoul(argv[1], NULL, 16);
	uint64_t budget = argc >= 3 ? strtoull(argv[2], NULL, 0) : 100000000ull;

	FILE *f = fopen(argv[0], "rb");
	if (!f) {
		fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
		return 1;
	}

	struct mem mem;
	if (mem_init(&mem)) {
		fprintf(stderr, "cannot reserve guest address space\n");
		fclose(f);
		return 1;
	}
	mem_map(&mem, 0, ram_size, MEM_RWX);

	size_t n = 0;
	if (addr < ram_size)
		n = fread(mem_ptr(&mem, addr), 1, ram_size - addr, f);
	fclose(f);
	if (n == 0) {
		fprintf(stderr, "%s: nothing loaded at 0x%08X\n", argv[0], addr);
		mem_free(&mem);
		return 1;
	}

	struct ppc_cpu cpu;
	ppc_init(&cpu, &mem);
	cpu.pc = addr;
	cpu.gpr[1] = ram_size - 0x100;
	if (getenv("SYSHM_TRACE"))
		cpu.trace = stderr;

	enum ppc_exit e = ppc_run(&cpu, budget);
	printf("exit: %s at %08x (insn %08x", ppc_exit_name(e), cpu.pc, cpu.exit_insn);
	if (e == PPC_EXIT_DSI || e == PPC_EXIT_ISI)
		printf(", %s 0x%08x", cpu.fault_write ? "write" : "read", cpu.fault_addr);
	printf(")\n");
	ppc_dump(&cpu, stdout);

	mem_free(&mem);
	return 0;
}

static int cmd_boot(int argc, char **argv)
{
	if (argc < 1)
		return usage();

	struct nk_boot_options opt = {
		.ram_size = 128u << 20,
		.max_insns = 500000000ull,
		.trace_reg = -1,
	};
	bool max_given = false, instruction_time = false;
	const char *rom_path = NULL, *share_dir = NULL;
	for (int k = 0; k < argc; k++) {
		if (strcmp(argv[k], "--rom") == 0 && k + 1 < argc) {
			rom_path = argv[++k];
		} else if (strcmp(argv[k], "--ram") == 0 && k + 1 < argc) {
			unsigned long mb = strtoul(argv[++k], NULL, 0);
			if (mb < 8 || mb > 1024) {
				fprintf(stderr, "--ram must be between 8 and 1024 MB\n");
				return 2;
			}
			opt.ram_size = (uint32_t)mb << 20;
		} else if (strcmp(argv[k], "--max") == 0 && k + 1 < argc) {
			opt.max_insns = strtoull(argv[++k], NULL, 0);
			max_given = true;
		} else if (strcmp(argv[k], "--io-ram") == 0) {
			opt.io_as_ram = true;
		} else if (strcmp(argv[k], "--trace") == 0) {
			opt.trace = true;
		} else if (strcmp(argv[k], "--trace-from") == 0 && k + 1 < argc) {
			opt.trace = true;
			opt.trace_from = strtoull(argv[++k], NULL, 0);
		} else if (strcmp(argv[k], "--window") == 0) {
			opt.window = opt.realtime = true;
		} else if (strcmp(argv[k], "--input") == 0 && k + 1 < argc) {
			opt.input_script = argv[++k];
		} else if (strcmp(argv[k], "--fixed-time") == 0 && k + 1 < argc) {
			opt.fixed_time = strtoll(argv[++k], NULL, 0);
		} else if (strcmp(argv[k], "--interp") == 0) {
			opt.interp = true;
		} else if (strcmp(argv[k], "--no-idle") == 0) {
			opt.no_idle = true;
		} else if (strcmp(argv[k], "--no-fast-start") == 0) {
			opt.no_fast_start = true;
		} else if (strcmp(argv[k], "--no-sound") == 0) {
			opt.no_sound = true;
		} else if (strcmp(argv[k], "--integer-scale") == 0) {
			opt.integer_scale = true;
		} else if (strcmp(argv[k], "--no-status-bar") == 0) {
			opt.no_status_bar = true;
		} else if (strcmp(argv[k], "--no-net") == 0) {
			opt.no_net = true;
		} else if (strcmp(argv[k], "--no-gpu") == 0) {
			opt.no_gpu = true;
		} else if (strcmp(argv[k], "--seconds") == 0 && k + 1 < argc) {
			opt.max_seconds = strtod(argv[++k], NULL);
		} else if (strcmp(argv[k], "--realtime") == 0) {
			opt.realtime = true;
		} else if (strcmp(argv[k], "--instruction-time") == 0) {
			instruction_time = true;
		} else if (strcmp(argv[k], "--nvram") == 0 && k + 1 < argc) {
			opt.nvram = argv[++k];
		} else if (strcmp(argv[k], "--dump-ram") == 0 && k + 1 < argc) {
			opt.dump_ram = argv[++k];
		} else if ((strcmp(argv[k], "--disk") == 0 || strcmp(argv[k], "--cdrom") == 0) && k + 1 < argc) {
			if (opt.disks >= NK_MAX_DISKS) {
				fprintf(stderr, "at most %d disks\n", NK_MAX_DISKS);
				return 2;
			}
			opt.cdrom[opt.disks] = strcmp(argv[k], "--cdrom") == 0;
			opt.disk[opt.disks++] = argv[++k];
		} else if (strcmp(argv[k], "--cdrom-drive") == 0) {
			if (opt.disks >= NK_MAX_DISKS) {
				fprintf(stderr, "at most %d disks\n", NK_MAX_DISKS);
				return 2;
			}
			opt.cdrom[opt.disks] = true;
			opt.disk[opt.disks++] = NULL;
		} else if (strcmp(argv[k], "--floppy") == 0 && k + 1 < argc) {
			if (opt.floppies >= NK_MAX_FLOPPIES) {
				fprintf(stderr, "at most %d floppy images\n", NK_MAX_FLOPPIES);
				return 2;
			}
			opt.floppy[opt.floppies++] = argv[++k];
		} else if (strcmp(argv[k], "--floppy-drive") == 0) {
			opt.floppy_drive = true;
		} else if (strcmp(argv[k], "--share") == 0 && k + 1 < argc && !share_dir) {
			share_dir = argv[++k];
		} else if (strcmp(argv[k], "--screenshot") == 0 && k + 1 < argc) {
			opt.screenshot = argv[++k];
		} else if (strcmp(argv[k], "--log-hostcalls") == 0) {
			opt.log_hostcalls = true;
		} else if (strcmp(argv[k], "--traps-after") == 0 && k + 1 < argc) {
			opt.trap_log_from = strtoull(argv[++k], NULL, 0);
		} else if (strcmp(argv[k], "--traps") == 0 && k + 1 < argc) {
			opt.trap_log = strtoul(argv[++k], NULL, 0);
		} else if (strcmp(argv[k], "--watch") == 0 && k + 1 < argc) {
			opt.watch = (uint32_t)strtoul(argv[++k], NULL, 16);
		} else if (strcmp(argv[k], "--trace-range") == 0 && k + 1 < argc) {
			char *end;
			opt.trace = true;
			opt.trace_lo = (uint32_t)strtoul(argv[++k], &end, 16);
			opt.trace_hi = *end == ':' ? (uint32_t)strtoul(end + 1, NULL, 16) : 0;
			if (opt.trace_hi <= opt.trace_lo) {
				fprintf(stderr, "--trace-range wants LO:HI (hex)\n");
				return 2;
			}
		} else if (strcmp(argv[k], "--trace-insn") == 0 && k + 1 < argc) {
			opt.trace_insn = (uint32_t)strtoul(argv[++k], NULL, 16);
		} else if (strcmp(argv[k], "--trace-reg") == 0 && k + 1 < argc) {
			const char *r = argv[++k];
			opt.trace_reg = (int)strtol(r[0] == 'r' ? r + 1 : r, NULL, 10) & 31;
		} else if (argv[k][0] != '-') {
			fprintf(stderr, "syshm boot: \"%s\": a ROM file goes after --rom (without one, the startup "
				"disk's is used)\n", argv[k]);
			return 2;
		} else {
			return usage();
		}
	}

	if (instruction_time)
		opt.realtime = false;
	if (opt.window && !max_given)
		opt.max_insns = UINT64_MAX;     /* run until the window is closed */

	struct rom rom;
	char err[256], from[HFSREAD_PATH_MAX + 256], version[64];
	int found = 0;
	if (rom_path) {
		found = rom_load(rom_path, &rom, err, sizeof err);
		if (found)
			fprintf(stderr, "%s\n", err);
	} else {
		const char *paths[NK_MAX_DISKS];       /* (the drives with images: not an empty CD-ROM drive) */
		unsigned drive[NK_MAX_DISKS], images = 0;
		for (unsigned k = 0; k < opt.disks; k++)
			if (opt.disk[k]) {
				drive[images] = k;
				paths[images++] = opt.disk[k];
			}
		found = load_startup_rom(paths, images, &rom, from, sizeof from, &opt.rom_disk, &opt.rom_part);
		opt.rom_from_disk = !found;
		if (!found)
			opt.rom_disk = drive[opt.rom_disk];
		if (found > 0 && images)
			fprintf(stderr, "no drive has a System Folder with a Mac OS ROM file in it: give one with "
				"--rom FILE\n");
		else if (found > 0)
			fprintf(stderr, "no drives to start up from (the ROM file comes from the startup disk's "
				"System Folder): attach one with --disk or --cdrom, or give a ROM file with --rom FILE\n");
		if (!found && rom_file_version(&rom, version, sizeof version))
			printf("rom: %s (Mac OS ROM %s, %zu bytes)\n", from, version, rom.file_size);
		else if (!found)
			printf("rom: %s (Toolbox release 0x%04X, %zu bytes)\n", from, rom_release(&rom), rom.file_size);
	}
	struct share *share = NULL;
	if (!found && share_dir) {
		if (opt.disks >= NK_MAX_DISKS)
			fprintf(stderr, "at most %d disks, the shared folder's included\n", NK_MAX_DISKS);
		else
			share = share_open(share_dir, opt.fixed_time);
		if (!share) {
			rom_free(&rom);
			found = 1;
		}
	}
	if (found)
		return 1;
	opt.share = share ? share_disk(share) : NULL;
	int rc = nk_boot(&rom, &opt);
	rom_free(&rom);
	if (share_close(share))
		rc = 1;
	return rc;
}

static int cmd_tools(int argc, char **argv)
{
	if (argc != 2 || strcmp(argv[0], "create") != 0)
		return usage();
	return tools_create(argv[1]) ? 1 : 0;
}

static void print_msf(uint32_t lba, bool length)
{
	uint8_t m, s, f;
	cd_msf(length ? lba - CD_MSF_OFFSET : lba, &m, &s, &f);
	printf("%02u:%02u:%02u", m, s, f);
}

/* A CD image's table of contents, as the CD-ROM drive gives it */
static int cmd_toc(const char *path)
{
	char why[400];
	struct cdimage *c = cdimage_open(path, why, sizeof why);
	if (!c) {
		fprintf(stderr, "%s: %s\n", path, why);
		return 1;
	}
	printf("%s: %u track%s, %u session%s, lead-out at ", path, c->tracks, c->tracks == 1 ? "" : "s", c->sessions,
	       c->sessions == 1 ? "" : "s");
	print_msf(c->leadout, false);
	printf(" (LBA %u)\n", c->leadout);
	for (unsigned k = 0; k < c->tracks; k++) {
		const struct cd_track *t = &c->track[k];
		printf("  %2u  %-6s  ", t->number, t->mode ? (t->mode == 1 ? "mode 1" : "mode 2") : "audio");
		print_msf(t->start, false);
		printf("  LBA %6u  ", t->start);
		print_msf(t->end - t->start, true);
		if (t->start > t->pregap) {
			printf("  (pregap ");
			print_msf(t->start - t->pregap, true);
			printf(")");
		}
		if (c->sessions > 1)
			printf("  session %u", t->session);
		printf("%s\n", (int)k == c->data ? "  (Mac OS sees its data)" : "");
	}
	cdimage_close(c);
	return 0;
}

static int cmd_disk(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[0], "toc") == 0)
		return cmd_toc(argv[1]);
	if (argc < 3 || argc > 4 || strcmp(argv[0], "create") != 0)
		return usage();
	char *end;
	unsigned long long size = strtoull(argv[2], &end, 0);
	switch (*end) {
	case 'k': case 'K': size <<= 10; end++; break;
	case 'm': case 'M': size <<= 20; end++; break;
	case 'g': case 'G': size <<= 30; end++; break;
	}
	bool floppy = floppy_raw_size(size) && size <= 1440u << 10;
	if (*end || (size < (1u << 20) && !floppy)) {
		fprintf(stderr, "disk size must be at least 1M, or a floppy's (400K, 720K, 800K, 1440K)\n");
		return 2;
	}
	return disk_create(argv[1], size, argc > 3 ? argv[3] : floppy ? "Untitled" : "Macintosh HD") ? 1 : 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);       /* progress shows up even when redirected */
	if (argc < 2)
		return usage();
	if (strcmp(argv[1], "rom") == 0 && argc >= 3)
		return cmd_rom(argc - 2, argv + 2);
	if (strcmp(argv[1], "disk") == 0)
		return cmd_disk(argc - 2, argv + 2);
	if (strcmp(argv[1], "tools") == 0)
		return cmd_tools(argc - 2, argv + 2);
	if (strcmp(argv[1], "boot") == 0)
		return cmd_boot(argc - 2, argv + 2);
	if (strcmp(argv[1], "run-raw") == 0)
		return cmd_run_raw(argc - 2, argv + 2);
	return usage();
}
