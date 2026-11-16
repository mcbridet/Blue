#include "pippin.h"

#include <string.h>

#include "be.h"
#include "pad.h"
#include "tools.h"

const char *const pippin_exports[PIPPIN_EXPORTS] = {
	/* (in the order of their selectors of the Pippin's trap $AA6F: 0, 1, 6, 7, 9 to 16) */
	"SavePRAMtoFlash", "MakeWritesPermanent", "ReadNVExtraPRAM", "WriteNVExtraPRAM", "InvokeSoftKeyboard",
	"PippinFindFolder", "PippinStandardGetFile", "PippinStandardPutFile", "PippinStandardDeleteFile",
	"HasFloppyDock", "HasMountedFloppy", "GetDriveRefNum",
};

const struct ndrv_import pippin_imports[PIPPIN_IMPORTS] = {
	[PIPPIN_FIND_FOLDER] = { "InterfaceLib", "FindFolder" },
	[PIPPIN_PB_GET_CAT_INFO_SYNC] = { "InterfaceLib", "PBGetCatInfoSync" },
	[PIPPIN_DIR_CREATE] = { "InterfaceLib", "DirCreate" },
	[PIPPIN_FLUSH_VOL] = { "InterfaceLib", "FlushVol" },
	[PIPPIN_STANDARD_GET_FILE] = { "InterfaceLib", "StandardGetFile" },
	[PIPPIN_STANDARD_PUT_FILE] = { "InterfaceLib", "StandardPutFile" },
	[PIPPIN_FSP_DELETE] = { "InterfaceLib", "FSpDelete" },
	[PIPPIN_GET_RESOURCE] = { "InterfaceLib", "GetResource" },
	[PIPPIN_RELEASE_RESOURCE] = { "InterfaceLib", "ReleaseResource" },
	[PIPPIN_BLOCK_MOVE] = { "InterfaceLib", "BlockMove" },
	[PIPPIN_BLOCK_MOVE_DATA] = { "InterfaceLib", "BlockMoveData" },
	[PIPPIN_NEW_GESTALT_VALUE] = { "InterfaceLib", "NewGestaltValue" },
	[PIPPIN_DELETE_GESTALT_VALUE] = { "InterfaceLib", "DeleteGestaltValue" },
	[PIPPIN_NGET_TRAP_ADDRESS] = { "InterfaceLib", "NGetTrapAddress" },
	[PIPPIN_NSET_TRAP_ADDRESS] = { "InterfaceLib", "NSetTrapAddress" },
	[PIPPIN_OPEN_WD] = { "InterfaceLib", "OpenWD" },
	[PIPPIN_CUR_RES_FILE] = { "InterfaceLib", "CurResFile" },
	[PIPPIN_USE_RES_FILE] = { "InterfaceLib", "UseResFile" },
	[PIPPIN_GET1_IND_RESOURCE] = { "InterfaceLib", "Get1IndResource" },
	[PIPPIN_GET_HANDLE_SIZE] = { "InterfaceLib", "GetHandleSize" },
	[PIPPIN_GET_CURRENT_PROCESS] = { "InterfaceLib", "GetCurrentProcess" },
	[PIPPIN_STANDARD_ALERT] = { "AppearanceLib", "StandardAlert" },
};

/* Entries: initialization and termination, then the exports (as pippin_exports), then the host calls */
enum {
	E_INIT, E_TERM, E_SAVE_PRAM, E_MAKE_WRITES_PERMANENT, E_READ_NV, E_WRITE_NV, E_INVOKE_SOFT_KEYBOARD,
	E_FIND_FOLDER, E_GET_FILE, E_PUT_FILE, E_DELETE_FILE, E_HAS_FLOPPY_DOCK, E_HAS_MOUNTED_FLOPPY,
	E_GET_DRIVE_REF_NUM, E_ATTACH, E_DETACH, E_COUNT
};
_Static_assert(E_ATTACH == PIPPIN_FIRST_EXPORT + PIPPIN_EXPORTS && E_ATTACH == PIPPIN_ENTRY_ATTACH &&
	       E_DETACH == PIPPIN_ENTRY_DETACH, "an entry for each export, then the host calls");

/*
 * The library's data (after the imports and entry TVectors): strings, and
 * the _ShutDown patch, 68k code: ShutDwnPower and ShutDwnStart (selectors
 * 1 and 2, the word above the return address) return at once, the others
 * go on to the routine patched over.
 */
#define FOLDER_NAME "\006" PIPPIN_FOLDER
#define DELETE_NAME "\006Delete"
#define PREFIX "Delete \xD2"                    /* Delete “name”? (Mac OS Roman) */
#define SUFFIX "\xD3?"
#define EXPLAIN "It will be deleted at once, to make room on the disk."
enum {
	D_FOLDER = 0,                           /* Str255s, and the alert's message around the file's name */
	D_DELETE = 8,
	D_PREFIX = 16,
	D_SUFFIX = 24,
	D_EXPLAIN = 28,
	D_TEMPLATE = 84,                        /* the patch, with the routine patched over */
	D_PATCH = 104,                          /* ... copied here (by BlockMove, which flushes the 68k emulator's cache) */
	D_ORIGINAL = 124,                       /* the routine patched over */
	D_LAST_VREF = 128,                      /* the volume PippinStandardPutFile's last file went to (0: none) */
	D_GESTALT = 130,                        /* the 'pipn' Gestalt value is ours */
	D_PSN = 132,                            /* the title's ProcessSerialNumber */
	D_ORIGINAL_COUNT = 140,                 /* CountADBs and GetIndADB, patched over */
	D_ORIGINAL_GET_IND = 144,
	D_CURSOR = 148,                         /* the AppleJack's cursor device record (its refCon: the AJGlobalData) */
	D_AJ = D_CURSOR + 64,                   /* the AppleJack's AJGlobalData */
	D_ADB_TEMPLATE = D_AJ + PAD_AJ_SIZE,    /* the two patches, with the routines patched over ... */
	D_ADB_PATCHES = D_ADB_TEMPLATE + 60,    /* ... copied here */
	D_SIZE = D_ADB_PATCHES + 60,
};
#define PREFIX_LEN (sizeof PREFIX - 1)
#define SUFFIX_LEN (sizeof SUFFIX - 1)
_Static_assert(sizeof FOLDER_NAME <= D_DELETE - D_FOLDER && sizeof DELETE_NAME <= D_PREFIX - D_DELETE &&
	       PREFIX_LEN <= D_SUFFIX - D_PREFIX && SUFFIX_LEN <= D_EXPLAIN - D_SUFFIX &&
	       sizeof EXPLAIN <= D_TEMPLATE - D_EXPLAIN, "the strings fit");
static const uint8_t patch_template[] = {
	0x0C, 0x6F, 0x00, 0x02, 0x00, 0x04,     /* cmpi.w #2,4(sp)              (the selector) */
	0x62, 0x06,                             /* bhi.s other */
	0x20, 0x5F,                             /* movea.l (sp)+,a0 */
	0x54, 0x8F,                             /* addq.l #2,sp */
	0x4E, 0xD0,                             /* jmp (a0) */
	0x4E, 0xF9, 0, 0, 0, 0,                 /* other: jmp original */
};
#define PATCH_SIZE sizeof patch_template
#define PATCH_ORIGINAL 16                       /* (the jmp's address, in it) */
_Static_assert(PATCH_SIZE <= D_PATCH - D_TEMPLATE && PATCH_SIZE <= D_ORIGINAL - D_PATCH, "the patch fits");

/*
 * CountADBs and GetIndADB, patched (OS traps: D0 the count, or the index
 * and the result; A0 the ADBDataBlock): one more device, the AppleJack
 */
static const uint8_t adb_template[] = {
	/* CountADBs */
	0x4E, 0xB9, 0, 0, 0, 0,                 /* jsr CountADBs */
	0x52, 0x40,                             /* addq.w #1,d0 */
	0x4E, 0x75,                             /* rts */
	0x4E, 0x71,                             /* (nop) */
	/* GetIndADB */
	0x48, 0xE7, 0xC0, 0x80,                 /* movem.l d0-d1/a0,-(sp)       (the index, the trap, the block) */
	0x4E, 0xB9, 0, 0, 0, 0,                 /* jsr CountADBs */
	0x24, 0x00,                             /* move.l d0,d2 */
	0x4C, 0xDF, 0x01, 0x03,                 /* movem.l (sp)+,d0-d1/a0 */
	0x52, 0x42,                             /* addq.w #1,d2 */
	0xB0, 0x42,                             /* cmp.w d2,d0 */
	0x67, 0x06,                             /* beq.s applejack */
	0x4E, 0xF9, 0, 0, 0, 0,                 /* jmp GetIndADB */
	0x10, 0xFC, 0x00, 0x00,                 /* applejack: move.b #0,(a0)+   (devType) */
	0x10, 0xFC, 0x00, 0x03,                 /* move.b #3,(a0)+              (origADBAddr) */
	0x42, 0x98,                             /* clr.l (a0)+                  (dbServiceRtPtr) */
	0x20, 0xBC, 0, 0, 0, 0,                 /* move.l #cursor,(a0)          (dbDataAreaAddr) */
	0x70, PIPPIN_ADB_ADDRESS,               /* moveq #15,d0 */
	0x4E, 0x75,                             /* rts */
};
#define ADB_GET_IND 12                          /* (GetIndADB's patch, in it) */
#define ADB_COUNT_AT 2                          /* (the addresses in it) */
#define ADB_COUNT_AT_2 (ADB_GET_IND + 6)
#define ADB_GET_IND_AT (ADB_GET_IND + 24)
#define ADB_CURSOR_AT (ADB_GET_IND + 40)
#define COUNT_ADBS_TRAP 0xA077u
#define GET_IND_ADB_TRAP 0xA078u
#define OS_TRAP 0u                              /* kOSTrapType */
#define LM_CUR_AP_REF_NUM 0x900                 /* the current application's resource file */
#define CURSOR_REF_CON 8                        /* (in a cursor device record) */
#define CURSOR_DEV_CLASS 0x18
#define CLASS_TRACKBALL 2
_Static_assert(sizeof adb_template <= D_ADB_PATCHES - D_ADB_TEMPLATE && sizeof adb_template <= D_SIZE - D_ADB_PATCHES,
	       "the patches fit");

#define SHUTDOWN_TRAP 0xA895u
#define TOOLBOX_TRAP 1u                         /* kToolboxTrapType */
#define ON_SYSTEM_DISK (-32768)                 /* kOnSystemDisk */
#define PREFERENCES_HI 0x7072                   /* 'pref' */
#define PREFERENCES_LO 0x6566
#define PIPN_HI 0x7069                          /* 'pipn' */
#define PIPN_LO 0x706E
#define ERIK_HI 0x4552                          /* 'ERIK': Standard File's working directories */
#define ERIK_LO 0x494B
#define FLOPPY_DEVICE 2                         /* kFloppyStorageDevice */
#define FNF_ERR (-43)
#define DUP_FN_ERR (-48)
#define USER_CANCELED_ERR (-128)
#define CAUTION_ALERT 2                         /* kAlertCautionAlert */
#define ALERT_OK 1                              /* kAlertStdAlertOKButton (here "Delete", the default) */
#define ALERT_CANCEL 2                          /* kAlertStdAlertCancelButton (it can't be the default too: paramErr) */
#define LM_SF_SAVE_DISK 0x214                   /* low memory: Standard File's volume (-vRefNum), */
#define LM_CUR_DIR_STORE 0x398                  /* ... its directory */
#define LM_CUR_AP_NAME 0x910                    /* the current application's name (Str31) */

/* CInfoPBRec, StandardFileReply, AlertStdAlertParamRec */
#define PB_NAME_PTR 18
#define PB_VREF_NUM 22
#define PB_ATTRIB 30
#define PB_DIR_ID 48
#define PB_DATA_PHYSICAL 58
#define PB_RSRC_PHYSICAL 68
#define PB_WORDS 27                             /* (108 bytes) */
#define ATTRIB_DIRECTORY 0x10
#define REPLY_GOOD 0
#define REPLY_FILE 6                            /* an FSSpec: vRefNum, parID, name */
#define ALERT_DEFAULT_TEXT 6
#define ALERT_CANCEL_TEXT 10
#define ALERT_DEFAULT_BUTTON 18
#define ALERT_CANCEL_BUTTON 20

/* The stack frame of the routines that make calls: linkage, parameters, then these; r26-r31 saved above */
#define FRAME 0x280
#define F_PB 64                                 /* CInfoPBRec */
#define F_NAME 176                              /* Str255 */
#define F_LOC 432                               /* a folder: vRefNum (a short), then its dirID */
#define F_REPLY 440                             /* StandardFileReply (88 bytes) */
#define F_ALERT 528                             /* AlertStdAlertParamRec (24 bytes) */
#define F_ITEM 552                              /* the alert's item hit */
_Static_assert(F_ITEM + 2 <= FRAME - 24, "the frame holds them");

/* ---- PowerPC code ------------------------------------------------------------------- */

enum label {
	L_SUBFOLDER, L_STORAGE, L_TITLE, L_DEFAULT_DIR, L_WORKING_DIR,  /* the routines below */
	L_SUB_FILE, L_SUB_NOT_FOUND, L_SUB_RETURN, L_STO_RETURN, L_TIT_APP, L_TIT_RELEASE, L_TIT_SHORT,
	L_TIT_APP_SHORT, L_TIT_FOLDER, L_TIT_RETURN, L_DEF_RETURN, L_FF_RETURN, L_GD_RETURN, L_MW_LAST,
	L_MW_RETURN, L_PF_RETURN, L_DF_ASK, L_DF_STORE, L_DF_RETURN, L_RN_DONE, L_IN_PATCH, L_TE_PATCH,
	L_TE_RETURN, L_WD_RETURN, L_IN_NO_PIPP, L_IN_RELEASE, L_TE_ADB, L_TE_GET_IND, L_COUNT
};
#define MAX_FIXUPS 64

struct gen {
	uint32_t *w;
	size_t n, room;
	size_t label[L_COUNT];
	struct { size_t at; enum label to; } fix[MAX_FIXUPS];
	unsigned fixes;
	const struct ndrv_layout *layout;
};

static void emit(struct gen *g, uint32_t insn)
{
	if (g->n < g->room)
		g->w[g->n] = insn;
	g->n++;
}

static void label(struct gen *g, enum label l)
{
	g->label[l] = g->n;
}

/* A branch (b, bl: I-form; bc: B-form) to a label, its displacement filled in by resolve() */
static void branch(struct gen *g, uint32_t insn, enum label to)
{
	if (g->fixes < MAX_FIXUPS) {
		g->fix[g->fixes].at = g->n;
		g->fix[g->fixes].to = to;
	}
	g->fixes++;
	emit(g, insn);
}

static bool resolve(struct gen *g)
{
	if (g->fixes > MAX_FIXUPS || g->n > g->room)
		return false;
	for (unsigned k = 0; k < g->fixes; k++) {
		if (g->label[g->fix[k].to] == SIZE_MAX)
			return false;
		int64_t disp = 4 * ((int64_t)g->label[g->fix[k].to] - (int64_t)g->fix[k].at);
		uint32_t *w = &g->w[g->fix[k].at];
		if (*w >> 26 == 18)
			*w |= (uint32_t)disp & 0x03FFFFFCu;
		else if (disp >= -0x8000 && disp < 0x8000)
			*w |= (uint32_t)disp & 0xFFFCu;
		else
			return false;
	}
	return true;
}

static uint32_t d_form(unsigned op, unsigned rt, unsigned ra, int32_t d)
{
	return (uint32_t)op << 26 | rt << 21 | ra << 16 | ((uint32_t)d & 0xFFFFu);
}

#define LI(rt, v) d_form(14, rt, 0, v)
#define LIS(rt, v) d_form(15, rt, 0, v)
#define ADDI(rt, ra, v) d_form(14, rt, ra, v)
#define ORI(ra, rs, v) d_form(24, rs, ra, v)
#define ANDI_(ra, rs, v) d_form(28, rs, ra, v)
#define CMPWI(ra, v) d_form(11, 0, ra, v)
#define CMPLWI(ra, v) d_form(10, 0, ra, v)
#define LWZ(rt, d, ra) d_form(32, rt, ra, d)
#define LBZ(rt, d, ra) d_form(34, rt, ra, d)
#define STW(rs, d, ra) d_form(36, rs, ra, d)
#define STWU(rs, d, ra) d_form(37, rs, ra, d)
#define STB(rs, d, ra) d_form(38, rs, ra, d)
#define STBU(rs, d, ra) d_form(39, rs, ra, d)
#define LHZ(rt, d, ra) d_form(40, rt, ra, d)
#define LHA(rt, d, ra) d_form(42, rt, ra, d)
#define STH(rs, d, ra) d_form(44, rs, ra, d)
#define LMW(rt, d, ra) d_form(46, rt, ra, d)
#define STMW(rs, d, ra) d_form(47, rs, ra, d)
#define X31(rs, ra, rb, xo, rc) (31u << 26 | (rs) << 21 | (ra) << 16 | (rb) << 11 | (xo) << 1 | (rc))
#define MR(ra, rs) X31(rs, ra, rs, 444u, 0u)
#define MR_(ra, rs) X31(rs, ra, rs, 444u, 1u)
#define EXTSH(ra, rs) X31(rs, ra, 0u, 922u, 0u)
#define EXTSH_(ra, rs) X31(rs, ra, 0u, 922u, 1u)
#define NEG(rt, ra) X31(rt, ra, 0u, 104u, 0u)
#define ADD(rt, ra, rb) X31(rt, ra, rb, 266u, 0u)
#define CMPW(ra, rb) X31(0u, ra, rb, 0u, 0u)
#define CLRLWI(ra, rs, n) (21u << 26 | (rs) << 21 | (ra) << 16 | (n) << 6 | 31u << 1)
#define SRWI(ra, rs, n) (21u << 26 | (rs) << 21 | (ra) << 16 | (32u - (n)) << 11 | (n) << 6 | 31u << 1)
#define MFLR_R0 0x7C0802A6u
#define MTLR_R0 0x7C0803A6u
#define MTCTR(rs) (0x7C0903A6u | (rs) << 21)
#define BCTRL 0x4E800421u
#define BLR 0x4E800020u
#define B 0x48000000u
#define BL 0x48000001u
#define BEQ 0x41820000u                         /* (cr0) */
#define BNE 0x40820000u
#define BLE 0x40810000u
#define BLT 0x41800000u
#define BDNZ_BACK 0x4200FFFCu                   /* bdnz to the instruction before */

/* Where the library's data (D_) is, from r2 */
static int32_t data(const struct gen *g, unsigned at)
{
	return (int32_t)(g->layout->extra + at);
}

/* A call to an import, as compilers' glue makes it (the TOC saved in our frame's linkage area) */
static void call(struct gen *g, unsigned imp)
{
	emit(g, LWZ(12, (int32_t)g->layout->import[imp], 2));
	emit(g, STW(2, 20, 1));
	emit(g, LWZ(0, 0, 12));
	emit(g, LWZ(2, 4, 12));
	emit(g, MTCTR(0));
	emit(g, BCTRL);
	emit(g, LWZ(2, 20, 1));
}

static void prologue(struct gen *g)
{
	emit(g, MFLR_R0);
	emit(g, STMW(26, -24, 1));
	emit(g, STW(0, 8, 1));
	emit(g, STWU(1, -FRAME, 1));
}

static void epilogue(struct gen *g)
{
	emit(g, ADDI(1, 1, FRAME));
	emit(g, LWZ(0, 8, 1));
	emit(g, MTLR_R0);
	emit(g, LMW(26, -24, 1));
	emit(g, BLR);
}

/* A host call: a call to an entry's stub (before our code, in the code section) */
static void host(struct gen *g, unsigned entry)
{
	int32_t disp = (int32_t)(NDRV_STUB_SIZE * entry) - (int32_t)(ndrv_code_offset(E_COUNT) + 4 * g->n);
	emit(g, BL | ((uint32_t)disp & 0x03FFFFFCu));
}

/* A register's 32 bits into the data at `at`, a halfword at a time (68k code's addresses are on halfwords) */
static void store_halves(struct gen *g, unsigned reg, unsigned at)
{
	emit(g, SRWI(0, reg, 16));
	emit(g, STH(0, data(g, at), 2));
	emit(g, STH(reg, data(g, at + 2), 2));
}

/* NGetTrapAddress(trap, kOSTrapType) */
static void get_os_trap(struct gen *g, uint16_t trap)
{
	emit(g, LI(3, 0));
	emit(g, ORI(3, 3, trap));
	emit(g, LI(4, OS_TRAP));
	call(g, PIPPIN_NGET_TRAP_ADDRESS);
}

/* NSetTrapAddress(the data at `at`, trap, kOSTrapType) */
static void set_os_trap(struct gen *g, unsigned at, uint16_t trap, bool address)
{
	if (address)
		emit(g, ADDI(3, 2, data(g, at)));
	else
		emit(g, LWZ(3, data(g, at), 2));
	emit(g, LI(4, 0));
	emit(g, ORI(4, 4, trap));
	emit(g, LI(5, OS_TRAP));
	call(g, PIPPIN_NSET_TRAP_ADDRESS);
}

/* The routine patched over put back, unless it was never patched or has been patched over since */
static void unpatch_os_trap(struct gen *g, uint16_t trap, unsigned original, unsigned patch, enum label skip)
{
	emit(g, LWZ(0, data(g, original), 2));
	emit(g, CMPWI(0, 0));
	branch(g, BEQ, skip);
	get_os_trap(g, trap);
	emit(g, ADDI(0, 2, data(g, patch)));
	emit(g, CMPW(3, 0));
	branch(g, BNE, skip);
	set_os_trap(g, original, trap, false);
}

static void clear_pb(struct gen *g)
{
	emit(g, LI(0, 0));
	emit(g, LI(9, PB_WORDS));
	emit(g, MTCTR(9));
	emit(g, ADDI(9, 1, F_PB - 4));
	emit(g, STWU(0, 4, 9));
	emit(g, BDNZ_BACK);
}

/* FindFolder(kOnSystemDisk, 'pref', create, F_LOC, F_LOC + 4): the result in CR0 */
static void find_preferences(struct gen *g, bool create)
{
	emit(g, LI(3, ON_SYSTEM_DISK));
	emit(g, LIS(4, PREFERENCES_HI));
	emit(g, ORI(4, 4, PREFERENCES_LO));
	emit(g, LI(5, create));
	emit(g, ADDI(6, 1, F_LOC));
	emit(g, ADDI(7, 1, F_LOC + 4));
	call(g, PIPPIN_FIND_FOLDER);
	emit(g, EXTSH_(3, 3));
}

static void subroutines(struct gen *g)
{
	/* subfolder(vRefNum, parent, name, create, long *dirID): the folder of that name in the parent, made if
	 * create; OSErr */
	label(g, L_SUBFOLDER);
	prologue(g);
	emit(g, MR(27, 3));
	emit(g, MR(28, 4));
	emit(g, MR(29, 5));
	emit(g, MR(30, 6));
	emit(g, MR(31, 7));
	clear_pb(g);
	emit(g, STW(29, F_PB + PB_NAME_PTR, 1));
	emit(g, STH(27, F_PB + PB_VREF_NUM, 1));
	emit(g, STW(28, F_PB + PB_DIR_ID, 1));
	emit(g, ADDI(3, 1, F_PB));
	call(g, PIPPIN_PB_GET_CAT_INFO_SYNC);
	emit(g, EXTSH_(3, 3));
	branch(g, BNE, L_SUB_NOT_FOUND);
	emit(g, LBZ(0, F_PB + PB_ATTRIB, 1));
	emit(g, ANDI_(0, 0, ATTRIB_DIRECTORY));
	branch(g, BEQ, L_SUB_FILE);
	emit(g, LWZ(0, F_PB + PB_DIR_ID, 1));
	emit(g, STW(0, 0, 31));
	emit(g, LI(3, 0));
	branch(g, B, L_SUB_RETURN);
	label(g, L_SUB_FILE);                   /* (a file of that name) */
	emit(g, LI(3, DUP_FN_ERR));
	branch(g, B, L_SUB_RETURN);
	label(g, L_SUB_NOT_FOUND);
	emit(g, CMPWI(3, FNF_ERR));
	branch(g, BNE, L_SUB_RETURN);
	emit(g, CMPWI(30, 0));
	branch(g, BEQ, L_SUB_RETURN);
	emit(g, MR(3, 27));
	emit(g, MR(4, 28));
	emit(g, MR(5, 29));
	emit(g, MR(6, 31));
	call(g, PIPPIN_DIR_CREATE);
	emit(g, EXTSH(3, 3));
	label(g, L_SUB_RETURN);
	epilogue(g);

	/* storage(create, out): the folder of the titles' folders, out = { short vRefNum; long dirID } */
	label(g, L_STORAGE);
	prologue(g);
	emit(g, MR(30, 3));
	emit(g, MR(31, 4));
	emit(g, LI(3, ON_SYSTEM_DISK));
	emit(g, LIS(4, PREFERENCES_HI));
	emit(g, ORI(4, 4, PREFERENCES_LO));
	emit(g, MR(5, 30));
	emit(g, MR(6, 31));
	emit(g, ADDI(7, 31, 4));
	call(g, PIPPIN_FIND_FOLDER);
	emit(g, EXTSH_(3, 3));
	branch(g, BNE, L_STO_RETURN);
	emit(g, LHA(3, 0, 31));
	emit(g, LWZ(4, 4, 31));
	emit(g, ADDI(5, 2, data(g, D_FOLDER)));
	emit(g, MR(6, 30));
	emit(g, ADDI(7, 31, 4));
	branch(g, BL, L_SUBFOLDER);
	label(g, L_STO_RETURN);
	epilogue(g);

	/* title(create, out): the title's folder in it, named by its 'pipn' 1 (at 12) or the application */
	label(g, L_TITLE);
	prologue(g);
	emit(g, MR(30, 3));
	emit(g, MR(31, 4));
	branch(g, BL, L_STORAGE);
	emit(g, CMPWI(3, 0));
	branch(g, BNE, L_TIT_RETURN);
	emit(g, LIS(3, PIPN_HI));
	emit(g, ORI(3, 3, PIPN_LO));
	emit(g, LI(4, 1));
	call(g, PIPPIN_GET_RESOURCE);
	emit(g, MR_(29, 3));
	branch(g, BEQ, L_TIT_APP);
	emit(g, LWZ(4, 0, 29));
	emit(g, CMPWI(4, 0));
	branch(g, BEQ, L_TIT_RELEASE);
	emit(g, LBZ(5, 12, 4));
	emit(g, CMPLWI(5, 63));
	branch(g, BLE, L_TIT_SHORT);
	emit(g, LI(5, 63));
	label(g, L_TIT_SHORT);
	emit(g, CMPWI(5, 0));
	branch(g, BEQ, L_TIT_RELEASE);
	emit(g, STB(5, F_NAME, 1));
	emit(g, ADDI(3, 4, 13));
	emit(g, ADDI(4, 1, F_NAME + 1));
	call(g, PIPPIN_BLOCK_MOVE_DATA);
	emit(g, MR(3, 29));
	call(g, PIPPIN_RELEASE_RESOURCE);
	branch(g, B, L_TIT_FOLDER);
	label(g, L_TIT_RELEASE);
	emit(g, MR(3, 29));
	call(g, PIPPIN_RELEASE_RESOURCE);
	label(g, L_TIT_APP);
	emit(g, LBZ(5, LM_CUR_AP_NAME, 0));
	emit(g, CMPLWI(5, 31));
	branch(g, BLE, L_TIT_APP_SHORT);
	emit(g, LI(5, 31));
	label(g, L_TIT_APP_SHORT);
	emit(g, STB(5, F_NAME, 1));
	emit(g, LI(3, LM_CUR_AP_NAME + 1));
	emit(g, ADDI(4, 1, F_NAME + 1));
	call(g, PIPPIN_BLOCK_MOVE_DATA);
	label(g, L_TIT_FOLDER);
	emit(g, LHA(3, 0, 31));
	emit(g, LWZ(4, 4, 31));
	emit(g, ADDI(5, 1, F_NAME));
	emit(g, MR(6, 30));
	emit(g, ADDI(7, 31, 4));
	branch(g, BL, L_SUBFOLDER);
	label(g, L_TIT_RETURN);
	epilogue(g);

	/* default_dir(): Standard File's next dialog starts in the title's folder (made if need be) */
	label(g, L_DEFAULT_DIR);
	prologue(g);
	emit(g, LI(3, 1));
	emit(g, ADDI(4, 1, F_LOC));
	branch(g, BL, L_TITLE);
	emit(g, CMPWI(3, 0));
	branch(g, BNE, L_DEF_RETURN);
	emit(g, LHA(0, F_LOC, 1));
	emit(g, NEG(0, 0));
	emit(g, STH(0, LM_SF_SAVE_DISK, 0));
	emit(g, LWZ(0, F_LOC + 4, 1));
	emit(g, STW(0, LM_CUR_DIR_STORE, 0));
	label(g, L_DEF_RETURN);
	epilogue(g);

	/* working_dir(StandardFileReply *): a file chosen, its vRefNum a working directory for its folder */
	label(g, L_WORKING_DIR);
	prologue(g);
	emit(g, MR(31, 3));
	emit(g, LBZ(0, REPLY_GOOD, 31));
	emit(g, CMPWI(0, 0));
	branch(g, BEQ, L_WD_RETURN);
	emit(g, LHA(3, REPLY_FILE, 31));
	emit(g, LWZ(4, REPLY_FILE + 2, 31));
	emit(g, LIS(5, ERIK_HI));
	emit(g, ORI(5, 5, ERIK_LO));
	emit(g, ADDI(6, 1, F_LOC));
	call(g, PIPPIN_OPEN_WD);
	emit(g, EXTSH_(3, 3));
	branch(g, BNE, L_WD_RETURN);
	emit(g, LHZ(0, F_LOC, 1));
	emit(g, STH(0, REPLY_FILE, 31));
	label(g, L_WD_RETURN);
	epilogue(g);
}

/* Each entry's code; `at` gets where it starts (in words from the first) */
static void entries(struct gen *g, size_t at[E_COUNT])
{
	/* OSErr initialization(CFragInitBlock *): Gestalt's 'pipn', and the _ShutDown patch */
	at[E_INIT] = g->n;
	prologue(g);
	emit(g, LIS(3, PIPN_HI));
	emit(g, ORI(3, 3, PIPN_LO));
	emit(g, LI(4, 0));
	call(g, PIPPIN_NEW_GESTALT_VALUE);
	emit(g, EXTSH_(3, 3));
	branch(g, BNE, L_IN_PATCH);             /* (someone else's: left alone) */
	emit(g, LI(0, 1));
	emit(g, STB(0, data(g, D_GESTALT), 2));
	label(g, L_IN_PATCH);
	emit(g, LI(3, 0));
	emit(g, ORI(3, 3, SHUTDOWN_TRAP));
	emit(g, LI(4, TOOLBOX_TRAP));
	call(g, PIPPIN_NGET_TRAP_ADDRESS);
	emit(g, STW(3, data(g, D_ORIGINAL), 2));
	emit(g, STW(3, data(g, D_TEMPLATE + PATCH_ORIGINAL), 2));
	emit(g, ADDI(3, 2, data(g, D_TEMPLATE)));
	emit(g, ADDI(4, 2, data(g, D_PATCH)));
	emit(g, LI(5, PATCH_SIZE));
	call(g, PIPPIN_BLOCK_MOVE);
	emit(g, ADDI(3, 2, data(g, D_PATCH)));
	emit(g, LI(4, 0));
	emit(g, ORI(4, 4, SHUTDOWN_TRAP));
	emit(g, LI(5, TOOLBOX_TRAP));
	call(g, PIPPIN_NSET_TRAP_ADDRESS);

	/* the AppleJack's mapping: the title's first 'pipp' resource, if it has one (else the driver's own, there) */
	call(g, PIPPIN_CUR_RES_FILE);
	emit(g, EXTSH(31, 3));
	emit(g, LHA(3, LM_CUR_AP_REF_NUM, 0));
	call(g, PIPPIN_USE_RES_FILE);
	emit(g, LIS(3, PAD_AJ_SIGNATURE >> 16));
	emit(g, ORI(3, 3, PAD_AJ_SIGNATURE & 0xFFFF));
	emit(g, LI(4, 1));
	call(g, PIPPIN_GET1_IND_RESOURCE);
	emit(g, MR(30, 3));
	emit(g, MR(3, 31));
	call(g, PIPPIN_USE_RES_FILE);
	emit(g, CMPWI(30, 0));
	branch(g, BEQ, L_IN_NO_PIPP);
	emit(g, MR(3, 30));
	call(g, PIPPIN_GET_HANDLE_SIZE);
	emit(g, CMPWI(3, PAD_AJ_MAP + 4 * PAD_AJ_SWITCHES));
	branch(g, BLT, L_IN_RELEASE);
	emit(g, LWZ(4, 0, 30));
	emit(g, CMPWI(4, 0));
	branch(g, BEQ, L_IN_RELEASE);
	emit(g, LWZ(0, 0, 4));
	emit(g, LIS(5, PAD_AJ_SIGNATURE >> 16));
	emit(g, ORI(5, 5, PAD_AJ_SIGNATURE & 0xFFFF));
	emit(g, CMPW(0, 5));
	branch(g, BNE, L_IN_RELEASE);
	emit(g, ADDI(3, 4, PAD_AJ_MAP));
	emit(g, ADDI(4, 2, data(g, D_AJ + PAD_AJ_MAP)));
	emit(g, LI(5, 4 * PAD_AJ_SWITCHES));
	call(g, PIPPIN_BLOCK_MOVE_DATA);
	label(g, L_IN_RELEASE);
	emit(g, MR(3, 30));
	call(g, PIPPIN_RELEASE_RESOURCE);
	label(g, L_IN_NO_PIPP);
	emit(g, ADDI(3, 2, data(g, D_PSN)));
	call(g, PIPPIN_GET_CURRENT_PROCESS);

	/* the AppleJack on the ADB: CountADBs and GetIndADB patched, the cursor device's refCon set */
	get_os_trap(g, COUNT_ADBS_TRAP);
	emit(g, STW(3, data(g, D_ORIGINAL_COUNT), 2));
	store_halves(g, 3, D_ADB_TEMPLATE + ADB_COUNT_AT);
	store_halves(g, 3, D_ADB_TEMPLATE + ADB_COUNT_AT_2);
	get_os_trap(g, GET_IND_ADB_TRAP);
	emit(g, STW(3, data(g, D_ORIGINAL_GET_IND), 2));
	store_halves(g, 3, D_ADB_TEMPLATE + ADB_GET_IND_AT);
	emit(g, ADDI(3, 2, data(g, D_CURSOR)));
	store_halves(g, 3, D_ADB_TEMPLATE + ADB_CURSOR_AT);
	emit(g, ADDI(0, 2, data(g, D_AJ)));
	emit(g, STW(0, data(g, D_CURSOR + CURSOR_REF_CON), 2));
	emit(g, ADDI(3, 2, data(g, D_ADB_TEMPLATE)));
	emit(g, ADDI(4, 2, data(g, D_ADB_PATCHES)));
	emit(g, LI(5, sizeof adb_template));
	call(g, PIPPIN_BLOCK_MOVE);
	set_os_trap(g, D_ADB_PATCHES, COUNT_ADBS_TRAP, true);
	set_os_trap(g, D_ADB_PATCHES + ADB_GET_IND, GET_IND_ADB_TRAP, true);

	/* and Blue told where the AppleJack's data is */
	emit(g, ADDI(3, 2, data(g, D_AJ)));
	emit(g, ADDI(4, 2, data(g, D_PSN)));
	host(g, E_ATTACH);
	emit(g, LI(3, 0));
	epilogue(g);

	/* void termination(void): all undone */
	at[E_TERM] = g->n;
	prologue(g);
	emit(g, ADDI(3, 2, data(g, D_AJ)));
	host(g, E_DETACH);
	emit(g, LBZ(0, data(g, D_GESTALT), 2));
	emit(g, CMPWI(0, 0));
	branch(g, BEQ, L_TE_PATCH);
	emit(g, LIS(3, PIPN_HI));
	emit(g, ORI(3, 3, PIPN_LO));
	call(g, PIPPIN_DELETE_GESTALT_VALUE);
	emit(g, LI(0, 0));
	emit(g, STB(0, data(g, D_GESTALT), 2));
	label(g, L_TE_PATCH);
	emit(g, LWZ(0, data(g, D_ORIGINAL), 2));
	emit(g, CMPWI(0, 0));
	branch(g, BEQ, L_TE_ADB);
	emit(g, LI(3, 0));
	emit(g, ORI(3, 3, SHUTDOWN_TRAP));
	emit(g, LI(4, TOOLBOX_TRAP));
	call(g, PIPPIN_NGET_TRAP_ADDRESS);
	emit(g, ADDI(0, 2, data(g, D_PATCH)));
	emit(g, CMPW(3, 0));
	branch(g, BNE, L_TE_ADB);               /* (patched over since: left alone) */
	emit(g, LWZ(3, data(g, D_ORIGINAL), 2));
	emit(g, LI(4, 0));
	emit(g, ORI(4, 4, SHUTDOWN_TRAP));
	emit(g, LI(5, TOOLBOX_TRAP));
	call(g, PIPPIN_NSET_TRAP_ADDRESS);
	label(g, L_TE_ADB);
	unpatch_os_trap(g, COUNT_ADBS_TRAP, D_ORIGINAL_COUNT, D_ADB_PATCHES, L_TE_GET_IND);
	label(g, L_TE_GET_IND);
	unpatch_os_trap(g, GET_IND_ADB_TRAP, D_ORIGINAL_GET_IND, D_ADB_PATCHES + ADB_GET_IND, L_TE_RETURN);
	label(g, L_TE_RETURN);
	epilogue(g);

	/* OSErr SavePRAMtoFlash(void), OSErr WriteNVExtraPRAM(void *, short count, short offset) */
	at[E_SAVE_PRAM] = at[E_WRITE_NV] = g->n;
	emit(g, LI(3, 0));
	emit(g, BLR);

	/* OSErr MakeWritesPermanent(void) */
	at[E_MAKE_WRITES_PERMANENT] = g->n;
	prologue(g);
	find_preferences(g, true);
	branch(g, BNE, L_MW_LAST);
	emit(g, LI(3, 0));
	emit(g, LHA(4, F_LOC, 1));
	call(g, PIPPIN_FLUSH_VOL);
	label(g, L_MW_LAST);
	emit(g, LHA(4, data(g, D_LAST_VREF), 2));
	emit(g, CMPWI(4, 0));
	branch(g, BEQ, L_MW_RETURN);
	emit(g, LI(3, 0));
	call(g, PIPPIN_FLUSH_VOL);
	label(g, L_MW_RETURN);
	emit(g, LI(3, 0));
	epilogue(g);

	/* OSErr ReadNVExtraPRAM(void *buffer, short count, short offset): zeros */
	at[E_READ_NV] = g->n;
	emit(g, EXTSH_(4, 4));
	branch(g, BLE, L_RN_DONE);
	emit(g, CMPWI(3, 0));
	branch(g, BEQ, L_RN_DONE);
	emit(g, MTCTR(4));
	emit(g, ADDI(3, 3, -1));
	emit(g, LI(0, 0));
	emit(g, STBU(0, 1, 3));
	emit(g, BDNZ_BACK);
	label(g, L_RN_DONE);
	emit(g, LI(3, 0));
	emit(g, BLR);

	/* void InvokeSoftKeyboard(void) */
	at[E_INVOKE_SOFT_KEYBOARD] = g->n;
	emit(g, BLR);

	/* OSErr PippinFindFolder(short vRefNum, OSType folderType, Boolean create, short *foundVRefNum,
	 * long *foundDirID): the title's folder, whatever the volume and type asked for */
	at[E_FIND_FOLDER] = g->n;
	prologue(g);
	emit(g, MR(31, 6));
	emit(g, MR(30, 7));
	emit(g, CLRLWI(3, 5, 24));
	emit(g, ADDI(4, 1, F_LOC));
	branch(g, BL, L_TITLE);
	emit(g, CMPWI(3, 0));
	branch(g, BNE, L_FF_RETURN);
	emit(g, LHZ(0, F_LOC, 1));
	emit(g, STH(0, 0, 31));
	emit(g, LWZ(0, F_LOC + 4, 1));
	emit(g, STW(0, 0, 30));
	emit(g, LI(3, 0));
	label(g, L_FF_RETURN);
	epilogue(g);

	/* void PippinStandardGetFile(FileFilterUPP, short numTypes, ConstSFTypeListPtr, StandardFileReply *) */
	at[E_GET_FILE] = g->n;
	prologue(g);
	emit(g, MR(31, 3));
	emit(g, MR(30, 4));
	emit(g, MR(29, 5));
	emit(g, MR(28, 6));
	branch(g, BL, L_DEFAULT_DIR);
	emit(g, MR(3, 31));
	emit(g, MR(4, 30));
	emit(g, MR(5, 29));
	emit(g, MR(6, 28));
	call(g, PIPPIN_STANDARD_GET_FILE);
	emit(g, MR(3, 28));
	branch(g, BL, L_WORKING_DIR);
	epilogue(g);

	/* void PippinStandardPutFile(ConstStr255Param prompt, ConstStr255Param defaultName, StandardFileReply *) */
	at[E_PUT_FILE] = g->n;
	prologue(g);
	emit(g, MR(31, 3));
	emit(g, MR(30, 4));
	emit(g, MR(29, 5));
	branch(g, BL, L_DEFAULT_DIR);
	emit(g, MR(3, 31));
	emit(g, MR(4, 30));
	emit(g, MR(5, 29));
	call(g, PIPPIN_STANDARD_PUT_FILE);
	emit(g, LBZ(0, REPLY_GOOD, 29));
	emit(g, CMPWI(0, 0));
	branch(g, BEQ, L_PF_RETURN);
	emit(g, LHZ(0, REPLY_FILE, 29));
	emit(g, STH(0, data(g, D_LAST_VREF), 2));
	emit(g, MR(3, 29));
	branch(g, BL, L_WORKING_DIR);
	label(g, L_PF_RETURN);
	epilogue(g);

	/* OSErr PippinStandardDeleteFile(long *bytesRequested): a file chosen, and deleted if the alert says so;
	 * its space in *bytesRequested (0 if none) */
	at[E_DELETE_FILE] = g->n;
	prologue(g);
	emit(g, MR(31, 3));
	emit(g, LI(30, 0));                     /* (bytes deleted) */
	emit(g, LI(29, USER_CANCELED_ERR));     /* (the result) */
	emit(g, LI(28, 0));                     /* (the file's space) */
	branch(g, BL, L_DEFAULT_DIR);
	emit(g, LI(3, 0));
	emit(g, LI(4, -1));                     /* (any type) */
	emit(g, LI(5, 0));
	emit(g, ADDI(6, 1, F_REPLY));
	call(g, PIPPIN_STANDARD_GET_FILE);
	emit(g, LBZ(0, F_REPLY + REPLY_GOOD, 1));
	emit(g, CMPWI(0, 0));
	branch(g, BEQ, L_DF_STORE);
	clear_pb(g);
	emit(g, ADDI(0, 1, F_REPLY + REPLY_FILE + 6));
	emit(g, STW(0, F_PB + PB_NAME_PTR, 1));
	emit(g, LHZ(0, F_REPLY + REPLY_FILE, 1));
	emit(g, STH(0, F_PB + PB_VREF_NUM, 1));
	emit(g, LWZ(0, F_REPLY + REPLY_FILE + 2, 1));
	emit(g, STW(0, F_PB + PB_DIR_ID, 1));
	emit(g, ADDI(3, 1, F_PB));
	call(g, PIPPIN_PB_GET_CAT_INFO_SYNC);
	emit(g, EXTSH_(3, 3));
	branch(g, BNE, L_DF_ASK);
	emit(g, LWZ(28, F_PB + PB_DATA_PHYSICAL, 1));
	emit(g, LWZ(0, F_PB + PB_RSRC_PHYSICAL, 1));
	emit(g, ADD(28, 28, 0));
	label(g, L_DF_ASK);                     /* Delete “name”? */
	emit(g, ADDI(3, 2, data(g, D_PREFIX)));
	emit(g, ADDI(4, 1, F_NAME + 1));
	emit(g, LI(5, PREFIX_LEN));
	call(g, PIPPIN_BLOCK_MOVE_DATA);
	emit(g, LBZ(27, F_REPLY + REPLY_FILE + 6, 1));
	emit(g, ADDI(3, 1, F_REPLY + REPLY_FILE + 7));
	emit(g, ADDI(4, 1, F_NAME + 1 + PREFIX_LEN));
	emit(g, MR(5, 27));
	call(g, PIPPIN_BLOCK_MOVE_DATA);
	emit(g, ADDI(3, 2, data(g, D_SUFFIX)));
	emit(g, ADDI(4, 1, F_NAME + 1 + PREFIX_LEN));
	emit(g, ADD(4, 4, 27));
	emit(g, LI(5, SUFFIX_LEN));
	call(g, PIPPIN_BLOCK_MOVE_DATA);
	emit(g, ADDI(0, 27, PREFIX_LEN + SUFFIX_LEN));
	emit(g, STB(0, F_NAME, 1));
	emit(g, LI(0, 0));
	for (int32_t k = 0; k < 24; k += 4)
		emit(g, STW(0, F_ALERT + k, 1));
	emit(g, ADDI(0, 2, data(g, D_DELETE)));
	emit(g, STW(0, F_ALERT + ALERT_DEFAULT_TEXT, 1));
	emit(g, LI(0, -1));                     /* (kAlertDefaultCancelText) */
	emit(g, STW(0, F_ALERT + ALERT_CANCEL_TEXT, 1));
	emit(g, LI(0, ALERT_OK));
	emit(g, STH(0, F_ALERT + ALERT_DEFAULT_BUTTON, 1));
	emit(g, LI(0, ALERT_CANCEL));
	emit(g, STH(0, F_ALERT + ALERT_CANCEL_BUTTON, 1));
	emit(g, LI(3, CAUTION_ALERT));
	emit(g, ADDI(4, 1, F_NAME));
	emit(g, ADDI(5, 2, data(g, D_EXPLAIN)));
	emit(g, ADDI(6, 1, F_ALERT));
	emit(g, ADDI(7, 1, F_ITEM));
	call(g, PIPPIN_STANDARD_ALERT);
	emit(g, EXTSH_(3, 3));
	branch(g, BNE, L_DF_STORE);
	emit(g, LHA(0, F_ITEM, 1));
	emit(g, CMPWI(0, ALERT_OK));
	branch(g, BNE, L_DF_STORE);
	emit(g, ADDI(3, 1, F_REPLY + REPLY_FILE));
	call(g, PIPPIN_FSP_DELETE);
	emit(g, EXTSH_(29, 3));
	branch(g, BNE, L_DF_STORE);
	emit(g, MR(30, 28));
	label(g, L_DF_STORE);
	emit(g, CMPWI(31, 0));
	branch(g, BEQ, L_DF_RETURN);
	emit(g, STW(30, 0, 31));
	label(g, L_DF_RETURN);
	emit(g, MR(3, 29));
	epilogue(g);

	/* Boolean HasFloppyDock(void), Boolean HasMountedFloppy(short *) */
	at[E_HAS_FLOPPY_DOCK] = at[E_HAS_MOUNTED_FLOPPY] = g->n;
	emit(g, LI(3, 0));
	emit(g, BLR);

	/* short GetDriveRefNum(short drive) */
	at[E_GET_DRIVE_REF_NUM] = g->n;
	prologue(g);
	emit(g, EXTSH(3, 3));
	emit(g, LI(31, 0));
	emit(g, CMPWI(3, FLOPPY_DEVICE));
	branch(g, BEQ, L_GD_RETURN);
	find_preferences(g, true);
	branch(g, BNE, L_GD_RETURN);
	emit(g, LHA(31, F_LOC, 1));
	label(g, L_GD_RETURN);
	emit(g, MR(3, 31));
	epilogue(g);
}

/* ---- the fragment ------------------------------------------------------------------- */

#define CODE_WORDS 1536

size_t pippin_build(uint8_t *out, size_t size, struct ndrv_layout *layout)
{
	static struct ndrv_export exports[PIPPIN_EXPORTS];
	static uint32_t code[CODE_WORDS], entry_code[E_COUNT];
	static uint8_t extra[D_SIZE];
	for (unsigned k = 0; k < PIPPIN_EXPORTS; k++)
		exports[k] = (struct ndrv_export){ .name = pippin_exports[k], .entry = PIPPIN_FIRST_EXPORT + k };
	memset(extra, 0, sizeof extra);
	memcpy(extra + D_FOLDER, FOLDER_NAME, sizeof FOLDER_NAME - 1);
	memcpy(extra + D_DELETE, DELETE_NAME, sizeof DELETE_NAME - 1);
	memcpy(extra + D_PREFIX, PREFIX, PREFIX_LEN);
	memcpy(extra + D_SUFFIX, SUFFIX, SUFFIX_LEN);
	extra[D_EXPLAIN] = (uint8_t)(sizeof EXPLAIN - 1);
	memcpy(extra + D_EXPLAIN + 1, EXPLAIN, sizeof EXPLAIN - 1);
	memcpy(extra + D_TEMPLATE, patch_template, PATCH_SIZE);
	extra[D_CURSOR + CURSOR_DEV_CLASS] = CLASS_TRACKBALL;
	put_be32(extra + D_AJ, PAD_AJ_SIGNATURE);
	put_be32(extra + D_AJ + PAD_AJ_STATES, 0xFFFFFFFFu);    /* (no switch down) */
	for (unsigned k = 0; k < PAD_AJ_SWITCHES; k++) {
		const struct pad_switch *s = &pad_applejack_defaults[k];
		uint8_t *at = extra + D_AJ + PAD_AJ_MAP + 4 * k;
		at[0] = s->function;
		at[1] = s->modifiers;
		at[2] = s->key;
		at[3] = s->chr;
	}
	memcpy(extra + D_ADB_TEMPLATE, adb_template, sizeof adb_template);
	struct ndrv_spec spec = {
		.kind = FRAG_LIBRARY,
		.entries = E_COUNT,
		.init_entry = E_INIT,
		.term_entry = E_TERM,
		.exports = exports,
		.export_count = PIPPIN_EXPORTS,
		.hostcall = HOSTCALL_TOOLS_PIPPIN,
		.imports = pippin_imports,
		.import_count = PIPPIN_IMPORTS,
		.extra = extra,
		.extra_len = sizeof extra,
	};
	/* the layout first (the data's doesn't depend on the code), then the code */
	if (!ndrv_build(&spec, out, size, layout))
		return 0;
	struct gen g = { .w = code, .room = CODE_WORDS, .layout = layout };
	for (unsigned k = 0; k < L_COUNT; k++)
		g.label[k] = SIZE_MAX;
	size_t at[E_COUNT] = { 0 };
	subroutines(&g);
	entries(&g, at);
	if (!resolve(&g))
		return 0;
	for (unsigned k = 0; k < E_COUNT; k++)
		entry_code[k] = k == E_ATTACH || k == E_DETACH ? NDRV_STUB : ndrv_code_offset(E_COUNT) + 4 * (uint32_t)at[k];
	spec.code = code;
	spec.code_words = g.n;
	spec.entry_code = entry_code;
	return ndrv_build(&spec, out, size, layout);
}
