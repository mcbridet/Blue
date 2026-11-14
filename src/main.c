#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "disk.h"
#include "mem.h"
#include "nk.h"
#include "ppc.h"
#include "rom.h"
#include "tools.h"

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
		"  boot <file> [options]        start the ROM's 68k emulator under the native kernel\n"
		"      --ram <MB>               RAM size (default 128)\n"
		"      --window                 show the screen in a window (keyboard and mouse go to\n"
		"                               the guest); runs in real time until closed\n"
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
		"                               key HEX [down|up], resize W H, screenshot FILE\n"
		"      --max <n>                stop after n PowerPC instructions (default 500000000)\n"
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
		"                               drives are numbered in command-line order\n"
		"      --cdrom <image>          attach an image read-only\n"
		"      --tools                  attach the guest tools volume (see tools create)\n"
		"      --nvram <file>           keep NVRAM (PRAM, settings) in this file\n"
		"      --screenshot <file.png>  save the screen when the run ends\n"
		"      --dump-ram <file>        save guest RAM when the run ends\n"
		"      --log-hostcalls          log driver requests handled natively\n");
	fprintf(stderr,
		"  disk create <file> <size> [name]\n"
		"                               make a disk image holding an empty HFS volume\n"
		"                               (size in bytes, or with K, M or G)\n"
		"  tools create <file>          make the guest tools volume: drag its files onto\n"
		"                               the System Folder (\"syshm Enhancements\" lets\n"
		"                               the screen follow the window's size)\n"
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

static int cmd_rom(int argc, char **argv)
{
	if (argc < 2)
		return usage();

	const char *sub = argv[0], *path = argv[1];
	struct rom rom;
	char err[256];

	if (rom_load(path, &rom, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}

	int rc = 0;
	if (strcmp(sub, "info") == 0) {
		char nk[40];
		uint32_t stored = rom_stored_checksum(&rom), computed = rom_computed_checksum(&rom);

		rom_nanokernel_id(&rom, nk, sizeof nk);
		printf("file:          %s (%zu bytes)\n", path, rom.file_size);
		if (rom.format == ROM_FORMAT_BOOTINFO || rom.format == ROM_FORMAT_PARCELS) {
			printf("format:        NewWorld bootinfo, %s\n",
			       rom.format == ROM_FORMAT_PARCELS ? "parcels" : "LZSS Toolbox image");
			printf("compatible:    %s\n", rom.compatible);
			if (rom.elf_size)
				printf("trampoline:    ELF at 0x%X, %u bytes, entry 0x%08X\n",
				       rom.elf_offset, rom.elf_size, rom.elf_entry);
			if (rom.format == ROM_FORMAT_PARCELS) {
				const struct parcel_blob *v = NULL;
				for (unsigned k = 0; k < rom.parcels.count && !v; k++)
					for (unsigned j = 0; j < rom.parcels.list[k].count && !v; j++)
						if (strcmp(rom.parcels.list[k].blobs[j].name, "MacOSROMFile-version") == 0)
							v = &rom.parcels.list[k].blobs[j];
				printf("parcels:       %u at 0x%X, %u bytes (\"rom parcels\" lists them)\n",
				       rom.parcels.count, rom.lzss_offset, rom.lzss_size);
				if (v)
					printf("file version:  %.*s\n", (int)strnlen((const char *)v->data, v->size), v->data);
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
	char tools_path[256] = "";
	for (int k = 1; k < argc; k++) {
		if (strcmp(argv[k], "--ram") == 0 && k + 1 < argc) {
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
		} else if (strcmp(argv[k], "--tools") == 0 && !tools_path[0]) {
			if (opt.disks >= NK_MAX_DISKS) {
				fprintf(stderr, "at most %d disks\n", NK_MAX_DISKS);
				return 2;
			}
			const char *dir = getenv("TMPDIR");
			snprintf(tools_path, sizeof tools_path, "%s/syshm-tools-XXXXXX", dir ? dir : "/tmp");
			int fd = mkstemp(tools_path);
			if (fd < 0 || close(fd) || tools_create(tools_path)) {
				fprintf(stderr, "cannot make the tools volume in %s\n", dir ? dir : "/tmp");
				return 1;
			}
			opt.cdrom[opt.disks] = true;
			opt.disk[opt.disks++] = tools_path;
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
		} else {
			return usage();
		}
	}

	if (instruction_time)
		opt.realtime = false;
	if (opt.window && !max_given)
		opt.max_insns = UINT64_MAX;     /* run until the window is closed */

	struct rom rom;
	char err[256];
	if (rom_load(argv[0], &rom, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	int rc = nk_boot(&rom, &opt);
	rom_free(&rom);
	if (tools_path[0])
		unlink(tools_path);
	return rc;
}

static int cmd_tools(int argc, char **argv)
{
	if (argc != 2 || strcmp(argv[0], "create") != 0)
		return usage();
	return tools_create(argv[1]) ? 1 : 0;
}

static int cmd_disk(int argc, char **argv)
{
	if (argc < 3 || argc > 4 || strcmp(argv[0], "create") != 0)
		return usage();
	char *end;
	unsigned long long size = strtoull(argv[2], &end, 0);
	switch (*end) {
	case 'k': case 'K': size <<= 10; end++; break;
	case 'm': case 'M': size <<= 20; end++; break;
	case 'g': case 'G': size <<= 30; end++; break;
	}
	if (*end || size < (1u << 20)) {
		fprintf(stderr, "disk size must be at least 1M\n");
		return 2;
	}
	return disk_create(argv[1], size, argc > 3 ? argv[3] : "Macintosh HD") ? 1 : 0;
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
