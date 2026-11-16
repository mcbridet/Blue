/*
 * Blue's PippinPPC (pippin.h): the fragment (Apple's exports, found as the
 * Code Fragment Manager finds them; its imports; its file in the tools), and
 * its code run by the processor against a Mac OS of the test's own: a
 * Preferences folder, a few folders and files, Standard File's dialogs,
 * Gestalt, the trap table, resources; and its host calls (the AppleJack's).
 */
#include <string.h>

#include "../src/be.h"
#include "../src/pad.h"
#include "../src/pippin.h"
#include "../src/ppc.h"
#include "../src/rsrc.h"
#include "../src/tools.h"
#include "test.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static struct mem mem;
static struct ppc_cpu cpu;
static struct ndrv_layout layout;

#define TOC 0x10000u            /* the library's data */
#define CODE 0x200000u          /* its code */
#define CODE_SIZE 0x10000u
#define FAKES (CODE + 0x8000u)  /* each import's code: a host call (the test's Mac OS), then blr */
#define FAKE_TVECTORS 0x30000u
#define RETURN (CODE + CODE_SIZE - 16)  /* (where the calls return: a host call that stops the processor) */
#define STACK 0x1F0000u
#define ARGS 0x40000u           /* what the calls are passed */
#define HANDLE 0x50000u         /* a 'pipn' resource: its master pointer, then the resource */
#define PIPP_HANDLE 0x51000u    /* ... a 'pipp' one */
#define HOSTCALL_FAKE 0x3FEu
#define HOSTCALL_RETURN 0x3FFu

#define STARTUP (-1)            /* the startup disk's vRefNum */
#define OTHER_VOLUME (-2)
#define ROOT 2u
#define PREFERENCES 20u
#define LM_SF_SAVE_DISK 0x214u
#define LM_CUR_DIR_STORE 0x398u
#define LM_CUR_AP_NAME 0x910u
#define SHUTDOWN_ENTRY 0x1234ABCDu      /* _ShutDown's routine in the trap table, */
#define COUNT_ENTRY 0x2345BCDEu         /* ... CountADBs's */
#define GET_IND_ENTRY 0x3456CDEFu       /* ... and GetIndADB's */
#define LM_CUR_AP_REF_NUM 0x900u
#define APP_FILE 0x55                   /* the title's resource file */
#define CUR_FILE 0x66                   /* ... and the current one */
#define WORKING_DIR (-32700)            /* a working directory's reference */

/* ---- the test's Mac OS ------------------------------------------------------------- */

struct item {
	uint32_t parent;
	char name[64];
	uint32_t dir;                   /* (a folder's ID; 0: a file) */
	uint32_t data_physical, rsrc_physical;
};
static struct item items[16];
static unsigned nitems;
static uint32_t next_dir = 100;

static struct {
	unsigned calls[PIPPIN_IMPORTS];
	int16_t flushed[4];
	unsigned flushes;
	bool gestalt, gestalt_mine;
	uint32_t gestalt_value;
	uint32_t trap;                  /* _ShutDown's entry in the trap table */
	uint32_t count_trap, get_ind_trap;      /* ... CountADBs's and GetIndADB's */
	int16_t cur_file;               /* the current resource file */
	int16_t used[4];                /* ... as UseResFile made it */
	unsigned uses;
	bool pipp;                      /* Get1IndResource('pipp', 1) has one */
	int16_t pipp_from;              /* ... in the file current then */
	uint32_t pipp_size;
	unsigned host_calls;            /* the library's host calls: */
	uint32_t attached, detached;    /* ... the AppleJack's data they gave */
	uint32_t psn_at;
	int16_t sf_save_disk;           /* the low memory Standard File took */
	uint32_t cur_dir_store;
	uint32_t get_args[4], put_args[3];
	bool choose;                    /* the dialogs' answer: */
	int16_t chosen_vref;
	uint32_t chosen_dir;
	char chosen[64];
	bool pipn;                      /* GetResource('pipn', 1) has one */
	int16_t alert_item;
	char alert[256];
	int16_t wd_vref;                /* OpenWD's: the volume and folder */
	uint32_t wd_dir;
	unsigned released;
} os;

static void reset_os(void)
{
	memset(&os, 0, sizeof os);
	memset(items, 0, sizeof items);
	nitems = 0;
	next_dir = 100;
	os.trap = SHUTDOWN_ENTRY;
	os.count_trap = COUNT_ENTRY;
	os.get_ind_trap = GET_IND_ENTRY;
	os.cur_file = CUR_FILE;
	mem_w16(&mem, LM_CUR_AP_REF_NUM, APP_FILE);
	os.alert_item = 1;
	items[nitems++] = (struct item){ ROOT, "Preferences", PREFERENCES, 0, 0 };
	memcpy(mem_ptr(&mem, LM_CUR_AP_NAME), "\014marathon.ppc", 13);
	mem_w16(&mem, LM_SF_SAVE_DISK, 0);
	mem_w32(&mem, LM_CUR_DIR_STORE, 0);
}

static void pstring(uint32_t at, char *out)
{
	uint8_t n = mem_r8(&mem, at);
	memcpy(out, mem_ptr(&mem, at + 1), n);
	out[n] = '\0';
}

static struct item *lookup(uint32_t parent, const char *name)
{
	for (unsigned k = 0; k < nitems; k++)
		if (items[k].parent == parent && !strcmp(items[k].name, name))
			return &items[k];
	return NULL;
}

static struct item *folder(uint32_t parent, const char *name)
{
	struct item *i = lookup(parent, name);
	return i && i->dir ? i : NULL;
}

static void add_file(uint32_t parent, const char *name, uint32_t data, uint32_t rsrc)
{
	items[nitems] = (struct item){ parent, "", 0, data, rsrc };
	strcpy(items[nitems++].name, name);
}

static void reply(uint32_t r)
{
	memset(mem_ptr(&mem, r), 0, 88);
	if (!os.choose)
		return;
	mem_w8(&mem, r, 1);                                     /* sfGood */
	mem_w16(&mem, r + 6, (uint16_t)os.chosen_vref);         /* sfFile */
	mem_w32(&mem, r + 8, os.chosen_dir);
	mem_w8(&mem, r + 12, (uint8_t)strlen(os.chosen));
	memcpy(mem_ptr(&mem, r + 13), os.chosen, strlen(os.chosen));
}

/* An import's call: its result */
static uint32_t fake(unsigned imp, const uint32_t *a)
{
	char name[256];
	os.calls[imp]++;
	switch (imp) {
	case PIPPIN_FIND_FOLDER:
		CHECK_EQ(a[0], (uint32_t)-32768);
		if (a[1] != FOURCC('p', 'r', 'e', 'f'))
			return (uint32_t)-43;
		mem_w16(&mem, a[3], (uint16_t)STARTUP);
		mem_w32(&mem, a[4], PREFERENCES);
		return 0;
	case PIPPIN_PB_GET_CAT_INFO_SYNC: {
		uint32_t pb = a[0];
		CHECK_EQ(mem_r16(&mem, pb + 28), 0);            /* (by name) */
		CHECK_EQ(mem_r16(&mem, pb + 22), (uint16_t)STARTUP);
		pstring(mem_r32(&mem, pb + 18), name);
		struct item *i = lookup(mem_r32(&mem, pb + 48), name);
		if (!i)
			return (uint32_t)-43;
		mem_w8(&mem, pb + 30, i->dir ? 0x10 : 0);
		if (i->dir) {
			mem_w32(&mem, pb + 48, i->dir);
		} else {
			mem_w32(&mem, pb + 58, i->data_physical);
			mem_w32(&mem, pb + 68, i->rsrc_physical);
		}
		return 0;
	}
	case PIPPIN_DIR_CREATE:
		CHECK_EQ(a[0], (uint32_t)STARTUP);
		pstring(a[2], name);
		CHECK(!lookup(a[1], name));
		items[nitems] = (struct item){ a[1], "", next_dir, 0, 0 };
		strcpy(items[nitems++].name, name);
		mem_w32(&mem, a[3], next_dir++);
		return 0;
	case PIPPIN_FLUSH_VOL:
		CHECK_EQ(a[0], 0);
		if (os.flushes < 4)
			os.flushed[os.flushes] = (int16_t)a[1];
		os.flushes++;
		return 0;
	case PIPPIN_STANDARD_GET_FILE:
	case PIPPIN_STANDARD_PUT_FILE:
		os.sf_save_disk = (int16_t)mem_r16(&mem, LM_SF_SAVE_DISK);
		os.cur_dir_store = mem_r32(&mem, LM_CUR_DIR_STORE);
		if (imp == PIPPIN_STANDARD_GET_FILE)
			memcpy(os.get_args, a, sizeof os.get_args);
		else
			memcpy(os.put_args, a, sizeof os.put_args);
		reply(imp == PIPPIN_STANDARD_GET_FILE ? a[3] : a[2]);
		return 0;
	case PIPPIN_FSP_DELETE: {
		pstring(a[0] + 6, name);
		CHECK_EQ(mem_r16(&mem, a[0]), (uint16_t)STARTUP);
		struct item *i = lookup(mem_r32(&mem, a[0] + 2), name);
		if (!i)
			return (uint32_t)-43;
		*i = items[--nitems];
		return 0;
	}
	case PIPPIN_GET_RESOURCE:
		CHECK(a[0] == FOURCC('p', 'i', 'p', 'n') && a[1] == 1);
		return os.pipn ? HANDLE : 0;
	case PIPPIN_RELEASE_RESOURCE:
		CHECK(a[0] == HANDLE || a[0] == PIPP_HANDLE);
		os.released++;
		return 0;
	case PIPPIN_CUR_RES_FILE:
		return (uint32_t)(int32_t)os.cur_file;
	case PIPPIN_USE_RES_FILE:
		if (os.uses < 4)
			os.used[os.uses] = (int16_t)a[0];
		os.uses++;
		os.cur_file = (int16_t)a[0];
		return 0;
	case PIPPIN_GET1_IND_RESOURCE:
		CHECK(a[0] == FOURCC('p', 'i', 'p', 'p') && (int16_t)a[1] == 1);
		os.pipp_from = os.cur_file;
		return os.pipp ? PIPP_HANDLE : 0;
	case PIPPIN_GET_HANDLE_SIZE:
		CHECK_EQ(a[0], PIPP_HANDLE);
		return os.pipp_size;
	case PIPPIN_GET_CURRENT_PROCESS:
		os.psn_at = a[0];
		mem_w32(&mem, a[0], 0);
		mem_w32(&mem, a[0] + 4, 0x2B);
		return 0;
	case PIPPIN_BLOCK_MOVE:
	case PIPPIN_BLOCK_MOVE_DATA:
		memmove(mem_ptr(&mem, a[1]), mem_ptr(&mem, a[0]), a[2]);
		return 0;
	case PIPPIN_NEW_GESTALT_VALUE:
		if (a[0] != FOURCC('p', 'i', 'p', 'n'))
			return (uint32_t)-5551;
		if (os.gestalt)
			return (uint32_t)-5552;                  /* gestaltDupSelectorErr */
		os.gestalt = os.gestalt_mine = true;
		os.gestalt_value = a[1];
		return 0;
	case PIPPIN_DELETE_GESTALT_VALUE:
		CHECK(a[0] == FOURCC('p', 'i', 'p', 'n') && os.gestalt);
		os.gestalt = false;
		return 0;
	case PIPPIN_NGET_TRAP_ADDRESS:
		CHECK((a[0] == 0xA895 && a[1] == 1) || ((a[0] == 0xA077 || a[0] == 0xA078) && a[1] == 0));
		return a[0] == 0xA895 ? os.trap : a[0] == 0xA077 ? os.count_trap : os.get_ind_trap;
	case PIPPIN_NSET_TRAP_ADDRESS:
		CHECK((a[1] == 0xA895 && a[2] == 1) || ((a[1] == 0xA077 || a[1] == 0xA078) && a[2] == 0));
		*(a[1] == 0xA895 ? &os.trap : a[1] == 0xA077 ? &os.count_trap : &os.get_ind_trap) = a[0];
		return 0;
	case PIPPIN_OPEN_WD:
		CHECK_EQ(a[2], FOURCC('E', 'R', 'I', 'K'));
		os.wd_vref = (int16_t)a[0];
		os.wd_dir = a[1];
		mem_w16(&mem, a[3], (uint16_t)WORKING_DIR);
		return 0;
	case PIPPIN_STANDARD_ALERT:
		CHECK_EQ(a[0], 2);                              /* kAlertCautionAlert */
		pstring(a[1], os.alert);
		CHECK_EQ(mem_r32(&mem, a[3] + 10), 0xFFFFFFFFu);        /* "Cancel", */
		CHECK_EQ(mem_r16(&mem, a[3] + 18), 1);                  /* "Delete" the default button, */
		CHECK_EQ(mem_r16(&mem, a[3] + 20), 2);                  /* ... Cancel the cancel button */
		pstring(mem_r32(&mem, a[3] + 6), name);
		CHECK(!strcmp(name, "Delete"));
		mem_w16(&mem, a[4], (uint16_t)os.alert_item);
		return 0;
	default:
		CHECK(!"an import");
		return 0;
	}
}

/* ---- the library, run by the processor ----------------------------------------------- */

static void load_library(void)
{
	static uint8_t buf[PIPPIN_LIBRARY_MAX];
	size_t len = pippin_build(buf, sizeof buf, &layout);
	CHECK(len > 0);
	const uint8_t *code = buf + 40, *data = buf + 40 + 28;
	CHECK(be32(code + 12) < FAKES - CODE);
	mem_invalidate(&mem, CODE, CODE_SIZE);
	memcpy(mem_ptr(&mem, CODE), buf + be32(code + 20), be32(code + 12));
	memset(mem_ptr(&mem, TOC), 0, be32(data + 8));
	memcpy(mem_ptr(&mem, TOC), buf + be32(data + 20), be32(data + 12));
	for (unsigned k = 0; k < PIPPIN_FIRST_EXPORT + PIPPIN_EXPORTS; k++) {
		mem_w32(&mem, TOC + layout.entry[k], mem_r32(&mem, TOC + layout.entry[k]) + CODE);
		mem_w32(&mem, TOC + layout.entry[k] + 4, TOC);
	}
	for (unsigned k = 0; k < PIPPIN_IMPORTS; k++) {
		mem_w32(&mem, FAKE_TVECTORS + 8 * k, FAKES + 8 * k);
		mem_w32(&mem, FAKE_TVECTORS + 8 * k + 4, 0x7777);       /* (their own TOC) */
		mem_w32(&mem, FAKES + 8 * k, PPC_HOSTCALL_INSN(HOSTCALL_FAKE));
		mem_w32(&mem, FAKES + 8 * k + 4, 0x4E800020u);
		mem_w32(&mem, TOC + layout.import[k], FAKE_TVECTORS + 8 * k);
	}
	mem_w32(&mem, RETURN, PPC_HOSTCALL_INSN(HOSTCALL_RETURN));
}

/* A call by the processor to entry k with these arguments; its result */
static uint32_t call(unsigned entry, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
	const uint32_t args[5] = { a0, a1, a2, a3, a4 };
	uint32_t tvector = TOC + layout.entry[entry];
	ppc_init(&cpu, &mem);
	for (unsigned k = 0; k < 32; k++)
		cpu.gpr[k] = 0xDEAD0000u + k;   /* (nothing comes in that isn't passed) */
	cpu.gpr[1] = STACK;
	memcpy(&cpu.gpr[3], args, sizeof args);
	cpu.pc = mem_r32(&mem, tvector);
	cpu.gpr[2] = mem_r32(&mem, tvector + 4);
	cpu.lr = RETURN;
	uint32_t saved[19];
	memcpy(saved, &cpu.gpr[13], sizeof saved);
	for (int k = 0; k < 64; k++) {
		enum ppc_exit e = ppc_run(&cpu, 1000000);
		CHECK_EQ(e, PPC_EXIT_HOSTCALL);
		if (e != PPC_EXIT_HOSTCALL)
			return 0xBAD;
		if ((cpu.exit_insn & 0x03FFFFFFu) == HOSTCALL_TOOLS_PIPPIN) {
			/* (the library's own: its stub's, the entry in r11) */
			os.host_calls++;
			if (cpu.gpr[11] == PIPPIN_ENTRY_ATTACH) {
				os.attached = cpu.gpr[3];
				CHECK_EQ(cpu.gpr[4], os.psn_at);
			} else {
				CHECK_EQ(cpu.gpr[11], PIPPIN_ENTRY_DETACH);
				os.detached = cpu.gpr[3];
			}
			cpu.gpr[3] = 0;
			cpu.cr = (cpu.cr & 0x0FFFFFFFu) | 0x20000000u;
			cpu.pc += 4;
			continue;
		}
		if (cpu.pc == RETURN) {
			CHECK_EQ(cpu.gpr[1], STACK);                    /* the stack and nonvolatile registers kept */
			CHECK_EQ(cpu.gpr[2], TOC);
			CHECK(!memcmp(saved, &cpu.gpr[13], sizeof saved));
			return cpu.gpr[3];
		}
		CHECK(cpu.pc >= FAKES && cpu.pc < FAKES + 8 * PIPPIN_IMPORTS && (cpu.pc - FAKES) % 8 == 0);
		CHECK_EQ(cpu.gpr[2], 0x7777);                           /* (called through the import's TVector) */
		unsigned imp = (cpu.pc - FAKES) / 8;
		uint32_t a[8];
		memcpy(a, &cpu.gpr[3], sizeof a);
		uint32_t r = fake(imp, a);
		for (unsigned j = 3; j <= 12; j++)
			cpu.gpr[j] = 0xBEEF0000u + j;   /* (what calls may change) */
		cpu.gpr[0] = 0xBEEF0000u;
		cpu.gpr[3] = r;
		cpu.pc += 4;
	}
	CHECK(!"the call didn't return");
	return 0xBAD;
}

static unsigned export(const char *name)
{
	for (unsigned k = 0; k < PIPPIN_EXPORTS; k++)
		if (!strcmp(pippin_exports[k], name))
			return PIPPIN_FIRST_EXPORT + k;
	CHECK(!"an export");
	return 0;
}

#define CALL(name, ...) call(export(name), __VA_ARGS__)
#define INIT 0
#define TERM 1

/* ---- the tests ---------------------------------------------------------------------- */

/* Apple's twelve exports, by its hash table; its imports; its file among the tools */
static void test_fragment(void)
{
	static uint8_t buf[PIPPIN_LIBRARY_MAX];
	struct ndrv_layout l;
	size_t len = pippin_build(buf, sizeof buf, &l);
	CHECK(len > 0);
	CHECK_EQ(pippin_build(buf, 1024, &l), 0);
	len = pippin_build(buf, sizeof buf, &l);
	static const char *const apple[PIPPIN_EXPORTS] = {      /* (nlib 777's, in its order) */
		"HasFloppyDock", "PippinStandardGetFile", "PippinStandardPutFile", "InvokeSoftKeyboard",
		"SavePRAMtoFlash", "MakeWritesPermanent", "WriteNVExtraPRAM", "PippinFindFolder",
		"PippinStandardDeleteFile", "HasMountedFloppy", "GetDriveRefNum", "ReadNVExtraPRAM",
	};
	const uint8_t *ld = buf + be32(buf + 40 + 56 + 20);
	CHECK_EQ(be32(ld + 8), 1);                              /* initialization and termination routines */
	CHECK_EQ(be32(ld + 12), l.entry[INIT]);
	CHECK_EQ(be32(ld + 16), 1);
	CHECK_EQ(be32(ld + 20), l.entry[TERM]);
	CHECK_EQ(be32(ld + 24), 2);                             /* InterfaceLib, AppearanceLib */
	CHECK_EQ(be32(ld + 28), PIPPIN_IMPORTS);
	unsigned power = be32(ld + 48), nexports = be32(ld + 52);
	CHECK_EQ(nexports, PIPPIN_EXPORTS);
	const uint8_t *strings = ld + be32(ld + 40), *hash = ld + be32(ld + 44);
	const uint8_t *keys = hash + 4 * (1u << power), *syms = keys + 4 * nexports;
	for (unsigned k = 0; k < PIPPIN_EXPORTS; k++) {
		const char *name = apple[k];
		uint32_t key = pef_hash_word(name, strlen(name)), found = 0;
		uint32_t chain = be32(hash + 4 * ((key ^ key >> power) & ((1u << power) - 1)));
		for (uint32_t j = chain & 0x3FFFF; j < (chain & 0x3FFFF) + (chain >> 18); j++) {
			const uint8_t *sym = syms + 10 * j;
			if (be32(keys + 4 * j) != key || memcmp(strings + (be32(sym) & 0xFFFFFF), name, strlen(name)))
				continue;
			found++;
			CHECK_EQ(sym[0], 2);                    /* a TVector, of its entry */
			CHECK_EQ(be32(sym + 4), l.entry[export(name)]);
		}
		CHECK_EQ(found, 1);
	}
	/* (the host call in the stubs, which tells Blue's copy) */
	bool hostcall = false;
	for (size_t k = 0; k + 4 <= len; k += 4)
		hostcall |= be32(buf + k) == PPC_HOSTCALL_INSN(HOSTCALL_TOOLS_PIPPIN);
	CHECK(hostcall);

	struct hfs_file files[TOOLS_FILES];
	CHECK_EQ(tools_files(files), 0);
	const struct hfs_file *f = &files[TOOLS_PIPPIN];
	CHECK(!strcmp(f->name, "Blue Pippin Library"));
	CHECK(f->type == FOURCC('s', 'h', 'l', 'b') && f->creator == FOURCC('s', 'y', 'P', 'P'));
	CHECK(f->data_len == len && !memcmp(f->data, buf, len));
	const uint8_t *map = f->rsrc + be32(f->rsrc + 4), *types = map + be16(map + 24), *cfrg = NULL;
	for (unsigned t = 0; t <= be16(types); t++)
		if (be32(types + 2 + 8 * t) == FOURCC('c', 'f', 'r', 'g'))
			cfrg = f->rsrc + be32(f->rsrc) + (be32(types + be16(types + 2 + 8 * t + 6) + 4) & 0xFFFFFF) + 4;
	CHECK(cfrg && cfrg[32 + 22] == 0 && cfrg[32 + 42] == 9 && !memcmp(cfrg + 32 + 43, "PippinPPC", 9));
}

/* While a title runs: Gestalt's 'pipn', and _ShutDown patched by 68k code that returns for ShutDwnStart */
static void test_init(void)
{
	reset_os();
	load_library();
	CHECK_EQ(call(INIT, 0x4444, 0, 0, 0, 0), 0);
	CHECK(os.gestalt && os.gestalt_value == 0);
	uint32_t patch = os.trap;
	CHECK(patch != SHUTDOWN_ENTRY && patch >= TOC && patch < TOC + 0x1000);
	CHECK_EQ(os.calls[PIPPIN_BLOCK_MOVE], 2);               /* (which flushes the 68k emulator's cache: and the ADB's) */
	static const uint8_t code[] = { 0x0C, 0x6F, 0x00, 0x02, 0x00, 0x04, 0x62, 0x06, 0x20, 0x5F, 0x54, 0x8F, 0x4E, 0xD0,
					0x4E, 0xF9 };
	CHECK(!memcmp(mem_ptr(&mem, patch), code, sizeof code));
	CHECK_EQ(mem_r32(&mem, patch + 16), SHUTDOWN_ENTRY);
	call(TERM, 0, 0, 0, 0, 0);
	CHECK(!os.gestalt);
	CHECK_EQ(os.trap, SHUTDOWN_ENTRY);

	/* someone else's 'pipn', and patches over ours: left alone */
	reset_os();
	load_library();
	os.gestalt = true;
	os.gestalt_value = 7;
	CHECK_EQ(call(INIT, 0, 0, 0, 0, 0), 0);
	CHECK(os.gestalt && !os.gestalt_mine && os.gestalt_value == 7);
	os.trap = os.count_trap = os.get_ind_trap = 0x5555;
	call(TERM, 0, 0, 0, 0, 0);
	CHECK(os.gestalt);
	CHECK(os.trap == 0x5555 && os.count_trap == 0x5555 && os.get_ind_trap == 0x5555);
	CHECK_EQ(os.calls[PIPPIN_DELETE_GESTALT_VALUE], 0);
}

/* The AppleJack's mapping in its data (AJGlobalData), as a 'pipp' resource has it: a switch's */
static uint32_t aj_switch(uint32_t aj, unsigned k)
{
	return mem_r32(&mem, aj + PAD_AJ_MAP + 4 * k);
}

static uint32_t switch_word(const struct pad_switch *s)
{
	return (uint32_t)s->function << 24 | (uint32_t)s->modifiers << 16 | (uint32_t)s->key << 8 | s->chr;
}

/*
 * The 68k code of the ADB patches, run as a title calls them: enough of a
 * 68000 for it (and for nothing else). The routines patched over are the
 * test's: CountADBs says two devices, the keyboard and the mouse, and
 * GetIndADB gives those.
 */
static struct {
	uint32_t d[8], a[8], pc;
	bool n, z, v, c;
	unsigned steps;
} m68k;

static void m68k_push(uint32_t v)
{
	m68k.a[7] -= 4;
	mem_w32(&mem, m68k.a[7], v);
}

static uint32_t m68k_pop(void)
{
	uint32_t v = mem_r32(&mem, m68k.a[7]);
	m68k.a[7] += 4;
	return v;
}

/* Flags of a word's result, an addition's or a subtraction's (r = x + y, or x - y) */
static void m68k_flags(uint32_t x, uint32_t y, uint32_t r, bool sub)
{
	x &= 0xFFFF;
	y &= 0xFFFF;
	r &= 0xFFFF;
	m68k.n = r >> 15;
	m68k.z = r == 0;
	m68k.v = sub ? ((x ^ y) & (x ^ r)) >> 15 : (~(x ^ y) & (x ^ r)) >> 15;
	m68k.c = sub ? y > x : r < x;
}

static void set_word(uint32_t *reg, uint32_t v)
{
	*reg = (*reg & 0xFFFF0000u) | (v & 0xFFFF);
}

/* A call to the routine at `at` (D0 and A0 its registers); D0 after */
static uint32_t m68k_call(uint32_t at, uint32_t d0, uint32_t a0)
{
	memset(&m68k, 0, sizeof m68k);
	m68k.d[0] = d0;
	m68k.d[1] = 0x1111;
	m68k.a[0] = a0;
	m68k.a[7] = STACK - 0x1000;
	m68k_push(0xFFFFFFF0u);                         /* (where it returns) */
	m68k.pc = at;
	while (m68k.pc != 0xFFFFFFF0u && m68k.steps++ < 1000) {
		if (m68k.pc == COUNT_ENTRY) {
			m68k.d[0] = 2;
			m68k.pc = m68k_pop();
			continue;
		}
		if (m68k.pc == GET_IND_ENTRY) {
			int16_t i = (int16_t)m68k.d[0];
			if (i == 1 || i == 2) {
				mem_w8(&mem, m68k.a[0], i == 1 ? 2 : 3);        /* devType, origADBAddr */
				mem_w8(&mem, m68k.a[0] + 1, i == 1 ? 2 : 3);
				mem_w32(&mem, m68k.a[0] + 2, 0x4000);
				mem_w32(&mem, m68k.a[0] + 6, 0);
				m68k.d[0] = i == 1 ? 2 : 3;
			} else {
				m68k.d[0] = 0xFFFFFFFFu;
			}
			m68k.pc = m68k_pop();
			continue;
		}
		uint32_t pc = m68k.pc;
		uint16_t op = mem_r16(&mem, pc);
		uint32_t *d = m68k.d;
		m68k.pc += 2;
		switch (op) {
		case 0x48E7:                            /* movem.l d0-d1/a0,-(sp) */
			CHECK_EQ(mem_r16(&mem, pc + 2), 0xC080);
			m68k_push(m68k.a[0]);
			m68k_push(d[1]);
			m68k_push(d[0]);
			m68k.pc += 2;
			break;
		case 0x4CDF:                            /* movem.l (sp)+,d0-d1/a0 */
			CHECK_EQ(mem_r16(&mem, pc + 2), 0x0103);
			d[0] = m68k_pop();
			d[1] = m68k_pop();
			m68k.a[0] = m68k_pop();
			m68k.pc += 2;
			break;
		case 0x4EB9:                            /* jsr abs.l */
			m68k_push(pc + 6);
			m68k.pc = mem_r32(&mem, pc + 2);
			break;
		case 0x4EF9:                            /* jmp abs.l */
			m68k.pc = mem_r32(&mem, pc + 2);
			break;
		case 0x4E75:                            /* rts */
			m68k.pc = m68k_pop();
			break;
		case 0x2400:                            /* move.l d0,d2 */
			d[2] = d[0];
			break;
		case 0x4442: {                          /* neg.w d2 */
			uint32_t r = 0u - (d[2] & 0xFFFF);
			m68k_flags(0, d[2], r, true);
			set_word(&d[2], r);
			break;
		}
		case 0xD440: {                          /* add.w d0,d2 */
			uint32_t r = (d[2] & 0xFFFF) + (d[0] & 0xFFFF);
			m68k_flags(d[2], d[0], r, false);
			set_word(&d[2], r);
			break;
		}
		case 0xD079: {                          /* add.w abs.l,d0 */
			uint32_t y = mem_r16(&mem, mem_r32(&mem, pc + 2)), r = (d[0] & 0xFFFF) + y;
			m68k_flags(d[0], y, r, false);
			set_word(&d[0], r);
			m68k.pc += 4;
			break;
		}
		case 0xB479: {                          /* cmp.w abs.l,d2 */
			uint32_t y = mem_r16(&mem, mem_r32(&mem, pc + 2));
			m68k_flags(d[2], y, (d[2] & 0xFFFF) - y, true);
			m68k.pc += 4;
			break;
		}
		case 0x10FC:                            /* move.b #imm,(a0)+ */
			mem_w8(&mem, m68k.a[0]++, mem_r8(&mem, pc + 3));
			m68k.pc += 2;
			break;
		case 0x4298:                            /* clr.l (a0)+ */
			mem_w32(&mem, m68k.a[0], 0);
			m68k.a[0] += 4;
			break;
		case 0x5342:                            /* subq.w #1,d2 */
			set_word(&d[2], d[2] - 1);
			break;
		case 0x9042:                            /* sub.w d2,d0 */
			set_word(&d[0], d[0] - d[2]);
			break;
		case 0xED4A:                            /* lsl.w #6,d2 */
			set_word(&d[2], (d[2] & 0xFFFF) << 6);
			break;
		case 0x48C2:                            /* ext.l d2 */
			d[2] = (uint32_t)(int32_t)(int16_t)d[2];
			break;
		case 0xD4BC:                            /* add.l #imm,d2 */
			d[2] += mem_r32(&mem, pc + 2);
			m68k.pc += 4;
			break;
		case 0x2082:                            /* move.l d2,(a0) */
			mem_w32(&mem, m68k.a[0], d[2]);
			break;
		default:
			if ((op & 0xFF00) == 0x7000) {          /* moveq */
				d[(op >> 9) & 7] = (uint32_t)(int32_t)(int8_t)op;
				break;
			}
			if ((op & 0xFF00) == 0x6F00 || (op & 0xFF00) == 0x6200) {      /* ble.s, bhi.s */
				bool take = (op & 0xFF00) == 0x6F00 ? m68k.z || m68k.n != m68k.v : !m68k.c && !m68k.z;
				if (take)
					m68k.pc = pc + 2 + (uint32_t)(int32_t)(int8_t)op;
				break;
			}
			CHECK(!"a 68k instruction of the patches'");
			return 0xBAD;
		}
	}
	CHECK_EQ(m68k.pc, 0xFFFFFFF0u);
	CHECK_EQ(m68k.a[7], STACK - 0x1000);
	CHECK_EQ(m68k.d[1], 0x1111);            /* (as the trap dispatcher would keep it) */
	return m68k.d[0];
}

#define BLOCK (ARGS + 0x100)                    /* an ADBDataBlock */

/* The AppleJacks a title finds, as Apple's note and Super Marathon look: CountADBs down, original address 3 */
static unsigned find_applejacks(uint32_t count, uint32_t get_ind, uint32_t found[4], uint8_t addresses[4])
{
	unsigned n = 0;
	for (int16_t i = (int16_t)m68k_call(count, 0, 0); i > 0 && n < 4; i--) {
		memset(mem_ptr(&mem, BLOCK), 0xEE, 10);
		uint32_t address = m68k_call(get_ind, (uint16_t)i, BLOCK);
		uint32_t cursor = mem_r32(&mem, BLOCK + 6);
		if (mem_r8(&mem, BLOCK + 1) != 3 || !cursor)
			continue;
		uint32_t aj = mem_r32(&mem, cursor + 8);
		if (mem_r32(&mem, aj) == PAD_AJ_SIGNATURE) {
			CHECK_EQ(mem_r8(&mem, BLOCK), 0);               /* devType */
			CHECK_EQ(mem_r32(&mem, BLOCK + 2), 0);          /* dbServiceRtPtr */
			CHECK_EQ(mem_r8(&mem, cursor + 0x18), 2);       /* (a trackball) */
			addresses[n] = (uint8_t)address;
			found[n++] = aj;
		}
	}
	return n;
}

/*
 * The title's AppleJacks: their data (AJGlobalData) with the driver's own
 * mapping or the title's first 'pipp' resource's, found by the title as ADB
 * devices' (CountADBs and GetIndADB patched by 68k code): one, or two once
 * Blue says so; and Blue told
 */
static void test_applejack(void)
{
	reset_os();
	load_library();
	CHECK_EQ(call(INIT, 0, 0, 0, 0, 0), 0);
	CHECK_EQ(os.host_calls, 1);
	uint32_t aj = os.attached;
	CHECK(aj >= TOC && aj < TOC + 0x1000);
	for (unsigned j = 0; j < PIPPIN_APPLEJACKS; j++) {
		uint32_t at = aj + PAD_AJ_SIZE * j;
		CHECK_EQ(mem_r32(&mem, at), PAD_AJ_SIGNATURE);
		CHECK_EQ(mem_r32(&mem, at + PAD_AJ_STATES), 0xFFFFFFFFu);      /* (nothing down) */
		for (unsigned k = 0; k < PAD_AJ_SWITCHES; k++)
			CHECK_EQ(aj_switch(at, k), switch_word(&pad_applejack_defaults[k]));
	}
	CHECK_EQ(mem_r16(&mem, aj + PIPPIN_AJ_COUNT), 1);               /* (one, until Blue says two) */
	CHECK_EQ(mem_r32(&mem, os.psn_at + 4), 0x2B);                  /* (its process, for Blue) */
	/* the 'pipp' looked for in the title's file, and the current one put back */
	CHECK(os.uses == 2 && os.used[0] == APP_FILE && os.used[1] == CUR_FILE && os.pipp_from == APP_FILE);

	/* the patches, run: CountADBs the routine's count plus the AppleJacks; GetIndADB those (originally at 3) */
	uint32_t count = os.count_trap, get_ind = os.get_ind_trap;
	CHECK(count >= TOC && count < TOC + 0x1000 && get_ind == count + 14);
	CHECK_EQ(m68k_call(count, 0, 0), 3);
	uint32_t found[4];
	uint8_t address[4];
	CHECK_EQ(find_applejacks(count, get_ind, found, address), 1);
	CHECK(found[0] == aj && address[0] == PIPPIN_ADB_ADDRESS);
	CHECK_EQ(m68k_call(get_ind, 4, BLOCK), 0xFFFFFFFFu);           /* (no more: the routine's answer) */
	CHECK_EQ(m68k_call(get_ind, 2, BLOCK), 3);                      /* (the others: the routine's) */
	CHECK_EQ(m68k_call(get_ind, 0, BLOCK), 0xFFFFFFFFu);
	mem_w16(&mem, aj + PIPPIN_AJ_COUNT, 2);                         /* two pads */
	CHECK_EQ(m68k_call(count, 0, 0), 4);
	CHECK_EQ(find_applejacks(count, get_ind, found, address), 2);
	/* (found from the last: the second first, at 14, then the first, at 15) */
	CHECK(found[0] == aj + PAD_AJ_SIZE && address[0] == PIPPIN_ADB_ADDRESS - 1);
	CHECK(found[1] == aj && address[1] == PIPPIN_ADB_ADDRESS);
	CHECK_EQ(m68k_call(get_ind, 5, BLOCK), 0xFFFFFFFFu);
	CHECK(mem_r32(&mem, mem_r32(&mem, get_ind + 52) + 8) == aj);  /* (the first's record, then the second's) */
	call(TERM, 0, 0, 0, 0, 0);
	CHECK_EQ(os.host_calls, 2);
	CHECK_EQ(os.detached, aj);
	CHECK(os.count_trap == COUNT_ENTRY && os.get_ind_trap == GET_IND_ENTRY);

	/* the title's 'pipp' resource: its switches (and not the rest) */
	reset_os();
	load_library();
	os.pipp = true;
	os.pipp_size = PAD_AJ_SIZE;
	mem_w32(&mem, PIPP_HANDLE, PIPP_HANDLE + 16);
	uint8_t *pipp = mem_ptr(&mem, PIPP_HANDLE + 16);
	memset(pipp, 0x77, PAD_AJ_SIZE);
	put_be32(pipp, PAD_AJ_SIGNATURE);
	for (unsigned k = 0; k < PAD_AJ_SWITCHES; k++)
		put_be32(pipp + PAD_AJ_MAP + 4 * k, 0x04000000u | (k + 1) << 8 | 'a');
	CHECK_EQ(call(INIT, 0, 0, 0, 0, 0), 0);
	aj = os.attached;
	for (unsigned j = 0; j < PIPPIN_APPLEJACKS; j++) {
		uint32_t at = aj + PAD_AJ_SIZE * j;
		for (unsigned k = 0; k < PAD_AJ_SWITCHES; k++)
			CHECK_EQ(aj_switch(at, k), 0x04000000u | (k + 1) << 8 | 'a');
		CHECK_EQ(mem_r32(&mem, at + PAD_AJ_STATES), 0xFFFFFFFFu);
		CHECK_EQ(mem_r32(&mem, at + PAD_AJ_MAP + 4 * PAD_AJ_SWITCHES), 0);
	}
	CHECK_EQ(os.released, 1);
	/* ... one too short, or not one: the driver's */
	static const uint32_t sizes[2] = { 40, PAD_AJ_SIZE };
	for (unsigned t = 0; t < 2; t++) {
		reset_os();
		load_library();
		os.pipp = true;
		os.pipp_size = sizes[t];
		if (t)
			put_be32(pipp, FOURCC('P', 'I', 'P', 'P'));
		CHECK_EQ(call(INIT, 0, 0, 0, 0, 0), 0);
		CHECK_EQ(aj_switch(os.attached, 10), switch_word(&pad_applejack_defaults[10]));
		CHECK_EQ(aj_switch(os.attached + PAD_AJ_SIZE, 10), switch_word(&pad_applejack_defaults[10]));
		CHECK_EQ(os.released, 1);
	}
}

#define VREF (ARGS + 0)
#define DIRID (ARGS + 4)

static void test_find_folder(void)
{
	reset_os();
	load_library();
	mem_w16(&mem, VREF, 0x1111);
	mem_w32(&mem, DIRID, 0x2222);
	/* not there, and not to be made */
	CHECK_EQ(CALL("PippinFindFolder", 1, FOURCC('p', 'i', 'p', 'n'), 0xFFFFFF00u, VREF, DIRID), (uint32_t)-43);
	CHECK_EQ(mem_r16(&mem, VREF), 0x1111);
	CHECK_EQ(mem_r32(&mem, DIRID), 0x2222);
	CHECK_EQ(os.calls[PIPPIN_DIR_CREATE], 0);
	/* made: Preferences:Pippin:<the application> (a Boolean is its low byte) */
	CHECK_EQ(CALL("PippinFindFolder", 1, FOURCC('p', 'i', 'p', 'n'), 0x101, VREF, DIRID), 0);
	struct item *storage = folder(PREFERENCES, PIPPIN_FOLDER), *title = storage ? folder(storage->dir, "marathon.ppc") : NULL;
	CHECK(storage && title);
	if (!title)
		return;
	CHECK_EQ(mem_r16(&mem, VREF), (uint16_t)STARTUP);
	CHECK_EQ(mem_r32(&mem, DIRID), title->dir);
	CHECK_EQ(os.calls[PIPPIN_DIR_CREATE], 2);
	CHECK_EQ(os.released, 0);
	/* found again, whatever the volume and folder type */
	mem_w32(&mem, DIRID, 0);
	CHECK_EQ(CALL("PippinFindFolder", 0, FOURCC('p', 'r', 'e', 'f'), 1, VREF, DIRID), 0);
	CHECK_EQ(mem_r32(&mem, DIRID), title->dir);
	CHECK_EQ(os.calls[PIPPIN_DIR_CREATE], 2);

	/* named by the title's 'pipn' resource (63 characters at most) */
	os.pipn = true;
	mem_w32(&mem, HANDLE, HANDLE + 16);
	memset(mem_ptr(&mem, HANDLE + 16), 0, 12);
	memcpy(mem_ptr(&mem, HANDLE + 16 + 12), "\012Super Mara", 11);
	CHECK_EQ(CALL("PippinFindFolder", 1, FOURCC('p', 'i', 'p', 'n'), 1, VREF, DIRID), 0);
	CHECK(storage && folder(storage->dir, "Super Mara") && mem_r32(&mem, DIRID) == folder(storage->dir, "Super Mara")->dir);
	CHECK_EQ(os.released, 1);
	char longer[80];
	memset(longer, 'x', sizeof longer);
	mem_w8(&mem, HANDLE + 16 + 12, 70);
	memcpy(mem_ptr(&mem, HANDLE + 16 + 13), longer, 70);
	CHECK_EQ(CALL("PippinFindFolder", 1, FOURCC('p', 'i', 'p', 'n'), 1, VREF, DIRID), 0);
	longer[63] = '\0';
	CHECK(folder(storage->dir, longer) != NULL);
	/* ... an empty one, or one not in memory: the application's */
	mem_w8(&mem, HANDLE + 16 + 12, 0);
	CHECK_EQ(CALL("PippinFindFolder", 1, FOURCC('p', 'i', 'p', 'n'), 1, VREF, DIRID), 0);
	CHECK_EQ(mem_r32(&mem, DIRID), title->dir);
	mem_w32(&mem, HANDLE, 0);
	CHECK_EQ(CALL("PippinFindFolder", 1, FOURCC('p', 'i', 'p', 'n'), 1, VREF, DIRID), 0);
	CHECK_EQ(mem_r32(&mem, DIRID), title->dir);
	CHECK_EQ(os.released, 4);

	/* a file where the folder would be */
	reset_os();
	add_file(PREFERENCES, PIPPIN_FOLDER, 10, 0);
	CHECK_EQ(CALL("PippinFindFolder", 1, FOURCC('p', 'i', 'p', 'n'), 1, VREF, DIRID), (uint32_t)-48);
}

/* The dialogs start in the title's folder; MakeWritesPermanent flushes its volume and the last saved to */
static void test_dialogs(void)
{
	reset_os();
	load_library();
	CHECK_EQ(CALL("MakeWritesPermanent", 0, 0, 0, 0, 0), 0);
	CHECK(os.flushes == 1 && os.flushed[0] == STARTUP);

	CALL("PippinStandardGetFile", 0x1000, 1, ARGS + 0x100, ARGS + 0x200, 0);
	const struct item *title = folder(folder(PREFERENCES, PIPPIN_FOLDER)->dir, "marathon.ppc");
	CHECK(title != NULL);
	CHECK(os.get_args[0] == 0x1000 && os.get_args[1] == 1 && os.get_args[2] == ARGS + 0x100 &&
	      os.get_args[3] == ARGS + 0x200);
	CHECK_EQ(os.sf_save_disk, -STARTUP);
	CHECK_EQ(os.cur_dir_store, title ? title->dir : 0);
	CHECK_EQ(os.calls[PIPPIN_OPEN_WD], 0);                 /* (cancelled) */

	mem_w16(&mem, LM_SF_SAVE_DISK, 0);
	os.choose = true;
	os.chosen_vref = OTHER_VOLUME;
	os.chosen_dir = 77;
	strcpy(os.chosen, "Untitled Game");
	CALL("PippinStandardPutFile", ARGS + 0x300, ARGS + 0x400, ARGS + 0x500, 0, 0);
	CHECK(os.put_args[0] == ARGS + 0x300 && os.put_args[1] == ARGS + 0x400 && os.put_args[2] == ARGS + 0x500);
	CHECK_EQ(os.sf_save_disk, -STARTUP);
	CHECK_EQ(mem_r8(&mem, ARGS + 0x500), 1);
	/* the reply's vRefNum a working directory for the file's folder (for titles that take the folder from it) */
	CHECK(os.wd_vref == OTHER_VOLUME && os.wd_dir == 77);
	CHECK_EQ((int16_t)mem_r16(&mem, ARGS + 0x500 + 6), WORKING_DIR);
	CHECK_EQ(mem_r32(&mem, ARGS + 0x500 + 8), 77);
	os.chosen_dir = 78;
	CALL("PippinStandardGetFile", 0, 0xFFFF, 0, ARGS + 0x600, 0);
	CHECK(os.wd_vref == OTHER_VOLUME && os.wd_dir == 78);
	CHECK_EQ((int16_t)mem_r16(&mem, ARGS + 0x600 + 6), WORKING_DIR);
	os.flushes = 0;
	CHECK_EQ(CALL("MakeWritesPermanent", 0, 0, 0, 0, 0), 0);
	CHECK(os.flushes == 2 && os.flushed[0] == STARTUP && os.flushed[1] == OTHER_VOLUME);
}

static void test_delete(void)
{
	reset_os();
	load_library();
	const uint32_t bytes = ARGS;
	/* cancelled in the dialog */
	mem_w32(&mem, bytes, 5000);
	CHECK_EQ(CALL("PippinStandardDeleteFile", bytes, 0, 0, 0, 0), (uint32_t)-128);
	CHECK_EQ(mem_r32(&mem, bytes), 0);
	CHECK_EQ(os.calls[PIPPIN_STANDARD_ALERT], 0);
	CHECK(os.get_args[1] == 0xFFFFFFFFu && os.get_args[2] == 0);   /* (any type) */
	const struct item *title = folder(folder(PREFERENCES, PIPPIN_FOLDER)->dir, "marathon.ppc");
	CHECK(title != NULL);
	if (!title)
		return;
	CHECK_EQ(os.cur_dir_store, title->dir);
	uint32_t dir = title->dir;

	/* chosen, then not to be deleted */
	add_file(dir, "Level 3", 3000, 512);
	os.choose = true;
	os.chosen_vref = STARTUP;
	os.chosen_dir = dir;
	strcpy(os.chosen, "Level 3");
	os.alert_item = 2;
	mem_w32(&mem, bytes, 5000);
	CHECK_EQ(CALL("PippinStandardDeleteFile", bytes, 0, 0, 0, 0), (uint32_t)-128);
	CHECK(!strcmp(os.alert, "Delete \xD2Level 3\xD3?"));
	CHECK_EQ(mem_r32(&mem, bytes), 0);
	CHECK(lookup(dir, "Level 3") != NULL);
	CHECK_EQ(os.calls[PIPPIN_FSP_DELETE], 0);

	/* ... and deleted: the space it took */
	os.alert_item = 1;
	CHECK_EQ(CALL("PippinStandardDeleteFile", bytes, 0, 0, 0, 0), 0);
	CHECK_EQ(mem_r32(&mem, bytes), 3512);
	CHECK(lookup(dir, "Level 3") == NULL);
	/* (and without somewhere to say so) */
	add_file(dir, "Level 3", 1000, 0);
	CHECK_EQ(CALL("PippinStandardDeleteFile", 0, 0, 0, 0, 0), 0);
	CHECK(lookup(dir, "Level 3") == NULL);
	/* gone meanwhile */
	CHECK_EQ(CALL("PippinStandardDeleteFile", bytes, 0, 0, 0, 0), (uint32_t)-43);
	CHECK_EQ(mem_r32(&mem, bytes), 0);
}

static void test_others(void)
{
	reset_os();
	load_library();
	CHECK_EQ((int16_t)CALL("GetDriveRefNum", 1, 0, 0, 0, 0), STARTUP);       /* kPippinStorageDevice */
	CHECK_EQ((int16_t)CALL("GetDriveRefNum", 0, 0, 0, 0, 0), STARTUP);
	CHECK_EQ(CALL("GetDriveRefNum", 2, 0, 0, 0, 0), 0);                      /* kFloppyStorageDevice */
	CHECK_EQ(CALL("GetDriveRefNum", 0xFFFF0002u, 0, 0, 0, 0), 0);
	CHECK_EQ(CALL("HasFloppyDock", 0, 0, 0, 0, 0) & 0xFF, 0);
	mem_w16(&mem, ARGS, 0x1234);
	CHECK_EQ(CALL("HasMountedFloppy", ARGS, 0, 0, 0, 0) & 0xFF, 0);
	CHECK_EQ(mem_r16(&mem, ARGS), 0x1234);
	CHECK_EQ(CALL("SavePRAMtoFlash", 0, 0, 0, 0, 0), 0);
	CALL("InvokeSoftKeyboard", 0, 0, 0, 0, 0);
	memset(mem_ptr(&mem, ARGS), 0x55, 16);
	CHECK_EQ(CALL("WriteNVExtraPRAM", ARGS, 2, 12, 0, 0), 0);
	CHECK_EQ(CALL("ReadNVExtraPRAM", ARGS, 2, 12, 0, 0), 0);                 /* (buffer, count, offset) */
	CHECK(mem_r16(&mem, ARGS) == 0 && mem_r8(&mem, ARGS + 2) == 0x55);
	CHECK_EQ(CALL("ReadNVExtraPRAM", ARGS + 4, 0, 0, 0, 0), 0);
	CHECK_EQ(mem_r8(&mem, ARGS + 4), 0x55);
	CHECK_EQ(CALL("ReadNVExtraPRAM", ARGS + 4, 0xFFFF, 0, 0, 0), 0);
	CHECK_EQ(mem_r8(&mem, ARGS + 4), 0x55);
	for (unsigned k = 0; k < PIPPIN_IMPORTS; k++)
		if (k != PIPPIN_FIND_FOLDER)
			CHECK_EQ(os.calls[k], 0);
}

void test_pippin(void)
{
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		return;
	}
	mem_map(&mem, 0, 0x100000, MEM_RW);
	mem_map(&mem, 0x100000, 0x100000, MEM_RW);
	mem_map(&mem, CODE, CODE_SIZE, MEM_RWX);
	test_fragment();
	test_init();
	test_applejack();
	test_find_folder();
	test_dialogs();
	test_delete();
	test_others();
	mem_free(&mem);
}
