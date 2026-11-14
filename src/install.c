#include "install.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "be.h"
#include "disk.h"
#include "ppc.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
	return -1;
}

/* ---- the check ------------------------------------------------------------------ */

#define ROOT_ID 2u
#define MAX_FORK 0x10000u               /* (a bigger fork isn't one of ours) */
#define MAX_FOLDERS 64u
#define VOLUME_LOCKED 0x8080u           /* volume attributes: locked by hardware, by software */

/* What each tool's code calls (installed copies keep these numbers, see tools.h) */
static const uint32_t tool_hostcall[TOOLS_FILES] = { HOSTCALL_TOOLS_HELPER, HOSTCALL_TOOLS_RAVE };

struct look {
	const struct hfsread_volume *v;
	struct install_check *ck;
	const struct hfs_file *files;
	uint8_t *buf;                   /* MAX_FORK bytes */
	struct folder { uint32_t id; char name[HFSREAD_NAME_MAX]; } *sub;
	unsigned subs;
	const struct folder *in;        /* the folder being looked through */
};

static int add_folder(void *ctx, const struct hfsread_entry *e)
{
	struct look *l = ctx;
	if (e->folder && l->subs < MAX_FOLDERS) {
		l->sub[l->subs].id = e->id;
		memcpy(l->sub[l->subs].name, e->name, sizeof e->name);
		l->subs++;
	}
	return 0;
}

/* Whether a file is this tool: 0 not, 1 another syshm's, 2 this syshm's (both forks the same) */
static int which_copy(struct look *l, const struct hfsread_entry *e, unsigned t)
{
	const struct hfs_file *f = &l->files[t];
	size_t size = (size_t)e->data.size;
	if (size < 16 || e->data.size > MAX_FORK || e->rsrc.size > MAX_FORK ||
	    hfsread_read(l->v, &e->data, 0, l->buf, size) || memcmp(l->buf, "Joy!peffpwpc", 12) != 0)
		return 0;
	bool calls = false;
	for (size_t k = 0; k + 4 <= size && !calls; k += 4)
		calls = be32(l->buf + k) == PPC_HOSTCALL_INSN(tool_hostcall[t]);
	if (!calls)
		return 0;
	if (size != f->data_len || memcmp(l->buf, f->data, size) != 0 || e->rsrc.size != f->rsrc_len)
		return 1;
	return hfsread_read(l->v, &e->rsrc, 0, l->buf, f->rsrc_len) == 0 && memcmp(l->buf, f->rsrc, f->rsrc_len) == 0 ? 2 : 1;
}

static int look_at_file(void *ctx, const struct hfsread_entry *e)
{
	struct look *l = ctx;
	for (unsigned t = 0; t < TOOLS_FILES && !e->folder; t++) {
		struct install_copy *c = &l->ck->tool[t];
		if (e->type != l->files[t].type || e->creator != l->files[t].creator || c->state == INSTALL_CURRENT)
			continue;
		int copy = which_copy(l, e, t);
		if (!copy || (copy == 1 && c->state == INSTALL_DIFFERS))
			continue;               /* (of several, the current one, or else the first) */
		c->state = copy == 2 ? INSTALL_CURRENT : INSTALL_DIFFERS;
		c->dir = l->in->id;
		c->id = e->id;
		memcpy(c->folder, l->in->name, sizeof c->folder);
	}
	return 0;
}

int install_check(const char *path, unsigned part, struct install_check *ck, char *err, size_t errlen)
{
	struct hfs_file files[TOOLS_FILES];
	struct look l = { .ck = ck, .files = files };
	struct hfsread_volume v;
	struct disk d;

	memset(ck, 0, sizeof *ck);
	if (tools_files(files))
		return fail(err, errlen, "cannot make the tools");
	if (disk_open(&d, NULL, path, true, true))
		return fail(err, errlen, "%s: cannot open it", path);
	int rc = -1;
	l.v = &v;
	l.buf = malloc(MAX_FORK);
	l.sub = malloc(MAX_FOLDERS * sizeof *l.sub);
	if (!l.buf || !l.sub) {
		fail(err, errlen, "out of memory");
	} else if (part >= d.partitions || hfsread_open(&v, d.fd, d.part[part].offset, d.part[part].size)) {
		fail(err, errlen, "%s: no HFS volume there", path);
	} else if (!v.blessed) {
		fail(err, errlen, "%s: no System Folder", path);
	} else if (hfsread_path(&v, ROOT_ID, ck->volume, sizeof ck->volume) ||
		   hfsread_path(&v, v.blessed, ck->system, sizeof ck->system) || hfsread_list(&v, v.blessed, add_folder, &l)) {
		fail(err, errlen, "%s: its catalog is damaged", path);
	} else {
		uint8_t h[16];
		uint64_t header = (v.plus ? v.offset : d.part[part].offset) + 1024;   /* (the MDB, or the HFS+ volume header) */
		if (pread(d.fd, h, sizeof h, (off_t)header) == (ssize_t)sizeof h)
			ck->locked = ((v.plus ? be32(h + 4) : be16(h + 10)) & VOLUME_LOCKED) != 0;
		ck->system_dir = v.blessed;
		rc = 0;
		for (unsigned k = 0; k < l.subs && rc == 0; k++) {
			l.in = &l.sub[k];
			if (hfsread_list(&v, l.sub[k].id, look_at_file, &l))
				rc = fail(err, errlen, "%s: its catalog is damaged", path);
		}
	}
	free(l.buf);
	free(l.sub);
	disk_close(&d);
	return rc;
}

bool install_needed(const struct install_check *ck)
{
	for (unsigned t = 0; t < TOOLS_FILES; t++)
		if (ck->tool[t].state != INSTALL_CURRENT)
			return true;
	return false;
}

/* ---- telling the user ------------------------------------------------------------- */

static const char *tool_name(unsigned t)
{
	static const char *const names[TOOLS_FILES] = { "syshm Enhancements", "syshm 3D Engine" };
	return names[t];
}

/* Appends to a sentence */
static void add(char *s, size_t len, const char *fmt, ...)
{
	size_t at = strlen(s);
	va_list ap;
	if (at >= len)
		return;
	va_start(ap, fmt);
	vsnprintf(s + at, len - at, fmt, ap);
	va_end(ap);
}

/* ", " or " and " before item k of n */
static const char *joint(unsigned k, unsigned n)
{
	return k == 0 ? "" : k + 1 == n ? " and " : ", ";
}

#define ABOUT "They make the screen follow the window, share the clipboard and accelerate 3D."

static void say_found(struct install *in)
{
	const struct install_check *ck = &in->check;
	char line[1024] = "";
	for (unsigned t = 0; t < TOOLS_FILES; t++) {
		const struct install_copy *c = &ck->tool[t];
		add(line, sizeof line, "%s%s %s", joint(t, TOOLS_FILES), tool_name(t),
		    c->state == INSTALL_MISSING ? "isn't installed" :
		    c->state == INSTALL_DIFFERS ? "differs from this syshm's" : "is current");
		if (c->state != INSTALL_MISSING)
			add(line, sizeof line, " (in \"%s\")", c->folder);
	}
	printf("tools: on \"%s\", %s\n", ck->volume, line);

	unsigned installed = 0, todo = 0;
	for (unsigned t = 0; t < TOOLS_FILES; t++) {
		installed += ck->tool[t].state != INSTALL_MISSING;
		todo += ck->tool[t].state != INSTALL_CURRENT;
	}
	if (!installed) {
		snprintf(in->question, sizeof in->question,
			 "syshm's guest tools aren't installed on \"%s\". " ABOUT, ck->volume);
		in->yes = "Install";
		return;
	}
	char what[512] = "";
	for (unsigned t = 0, k = 0; t < TOOLS_FILES; t++)
		if (ck->tool[t].state != INSTALL_CURRENT)
			add(what, sizeof what, "%s%s %s", joint(k++, todo), tool_name(t),
			    ck->tool[t].state == INSTALL_MISSING ? "isn't installed" : "is from another version of syshm");
	snprintf(in->question, sizeof in->question, "syshm's guest tools on \"%s\" need updating: %s. " ABOUT,
		 ck->volume, what);
	in->yes = "Update";
}

static const char *error_text(int16_t err, char *buf, size_t len)
{
	switch (err) {
	case -34: return "the disk is full";
	case -44: case -46: return "the disk is locked";
	case -45: return "the file is locked";
	case -47: return "it's in use";
	case -48: return "something of that name is in the way";
	case -54: case -5000: return "it isn't allowed";
	case -108: return "there isn't enough memory";
	case -120: return "its folder has gone";
	default:
		snprintf(buf, len, "error %d", err);
		return buf;
	}
}

/* ---- the bootstrap ---------------------------------------------------------------- */

/* The Toolbox trap table's entry for _SynchIdleTime */
#define TRAP_SYNCH_IDLE_TIME 0xABF7u
#define TRAP_ENTRY (0x0E00u + 4u * (TRAP_SYNCH_IDLE_TIME & 0x3FFu))
#define TRAP_CODE_FRAGMENT_DISPATCH 0xAA5Au
#define CFRAG_GET_MEM_FRAGMENT 3u
#define CFRAG_CLOSE_CONNECTION 4u
#define CFRAG_PRIVATE_COPY 5u           /* kPrivateCFragCopy */

/* The region: the bootstrap and what the installer works with */
enum {
	R_CODE = 0x0000,                /* the bootstrap (68k) */
	R_RD = 0x0100,                  /* the routine descriptor of the installer's main routine */
	R_CONN = 0x0140,                /* its connection ID */
	R_MAIN = 0x0144,                /* its main routine (a TVector) */
	R_STATUS = 0x0148,              /* the bootstrap's progress (STATUS_*, then GetMemFragment's result) */
	R_NAME = 0x0150,                /* the fragment's name (Str63) */
	R_ERRMSG = 0x0190,              /* GetMemFragment's error message (Str255) */
	R_VREF = 0x02A0,                /* FindFolder's results */
	R_DIRID = 0x02A4,
	R_REFNUM = 0x02A8,              /* an open fork's */
	R_COUNT = 0x02AC,               /* FSWrite's */
	R_PB = 0x0300,                  /* a parameter block (HParamBlockRec, CInfoPBRec) */
	R_NAMEBUF = 0x0380,             /* a name (Str255) */
	R_TEMP = 0x0480,                /* FSSpecs: the temporary file, */
	R_OLD = 0x04D0,                 /* ... the copy it replaces, */
	R_APP = 0x0520,                 /* ... syshm Enhancements */
	R_NEWNAME = 0x0570,             /* (Str63) */
	R_FINFO = 0x05B0,
	R_LAUNCH = 0x05C0,              /* LaunchParamBlockRec */
	R_FRAGMENT = 0x1000,            /* the installer's container */
	R_BUFFER = 0x4000,              /* a fork, for FSWrite */
};
#define RD_PROC 20u                     /* the routine record's procDescriptor */
#define PB_SIZE 128u
#define STATUS_PENDING 1u
#define STATUS_STARTED 2u

enum {
	I_FIND_FOLDER, I_HGET_VINFO, I_GET_CAT_INFO, I_FSP_DELETE, I_FSP_CREATE, I_FSP_OPEN_DF, I_FSP_OPEN_RF,
	I_FS_WRITE, I_FS_CLOSE, I_FSP_SET_FINFO, I_FSP_RENAME, I_LAUNCH, I_COUNT,
};

static const struct ndrv_import installer_imports[I_COUNT] = {
	[I_FIND_FOLDER] = { "InterfaceLib", "FindFolder" },
	[I_HGET_VINFO] = { "InterfaceLib", "PBHGetVInfoSync" },
	[I_GET_CAT_INFO] = { "InterfaceLib", "PBGetCatInfoSync" },
	[I_FSP_DELETE] = { "InterfaceLib", "FSpDelete" },
	[I_FSP_CREATE] = { "InterfaceLib", "FSpCreate" },
	[I_FSP_OPEN_DF] = { "InterfaceLib", "FSpOpenDF" },
	[I_FSP_OPEN_RF] = { "InterfaceLib", "FSpOpenRF" },
	[I_FS_WRITE] = { "InterfaceLib", "FSWrite" },
	[I_FS_CLOSE] = { "InterfaceLib", "FSClose" },
	[I_FSP_SET_FINFO] = { "InterfaceLib", "FSpSetFInfo" },
	[I_FSP_RENAME] = { "InterfaceLib", "FSpRename" },
	[I_LAUNCH] = { "InterfaceLib", "LaunchApplication" },
};

static uint32_t at(uint32_t offset)
{
	return INSTALL_REGION + offset;
}

/*
 * The 68k code _SynchIdleTime's trap table entry points at (once): it saves
 * the registers, puts the entry back, prepares the installer and calls its
 * main routine, closes the connection, and goes on to the trap's routine.
 */
static size_t make_bootstrap(uint8_t *p, uint32_t routine, uint32_t fragment_len)
{
	uint8_t *q = p, *bne;
#define W(v) (put_be16(q, (uint16_t)(v)), q += 2)
#define L(v) (put_be32(q, (uint32_t)(v)), q += 4)
	W(0x48E7); W(0xFFFE);                           /* movem.l d0-d7/a0-a6,-(sp) */
	W(0x23FC); L(routine); L(TRAP_ENTRY);           /* move.l #routine,TRAP_ENTRY */
	W(0x23FC); L(STATUS_STARTED); L(at(R_STATUS));  /* move.l #STATUS_STARTED,status */
	W(0x4267);                                      /* clr.w -(sp): GetMemFragment( */
	W(0x4879); L(at(R_FRAGMENT));                   /*   pea memAddr, */
	W(0x2F3C); L(fragment_len);                     /*   move.l #memLength,-(sp), */
	W(0x4879); L(at(R_NAME));                       /*   pea fragName, */
	W(0x2F3C); L(CFRAG_PRIVATE_COPY);               /*   move.l #loadFlags,-(sp), */
	W(0x4879); L(at(R_CONN));                       /*   pea connID, */
	W(0x4879); L(at(R_MAIN));                       /*   pea mainAddr, */
	W(0x4879); L(at(R_ERRMSG));                     /*   pea errMessage) */
	W(0x3F3C); W(CFRAG_GET_MEM_FRAGMENT);           /* move.w #selector,-(sp) */
	W(TRAP_CODE_FRAGMENT_DISPATCH);
	W(0x301F);                                      /* move.w (sp)+,d0 */
	W(0x48C0);                                      /* ext.l d0 */
	W(0x23C0); L(at(R_STATUS));                     /* move.l d0,status */
	bne = q;
	W(0x6600);                                      /* bne.s done */
	W(0x23F9); L(at(R_MAIN)); L(at(R_RD + RD_PROC));       /* move.l mainAddr,procDescriptor */
	W(0x4EB9); L(at(R_RD));                         /* jsr main's routine descriptor */
	W(0x4267);                                      /* clr.w -(sp): CloseConnection( */
	W(0x4879); L(at(R_CONN));                       /*   pea connID) */
	W(0x3F3C); W(CFRAG_CLOSE_CONNECTION);
	W(TRAP_CODE_FRAGMENT_DISPATCH);
	W(0x544F);                                      /* addq.w #2,sp */
	bne[1] = (uint8_t)(q - (bne + 2));
	W(0x4CDF); W(0x7FFF);                           /* done: movem.l (sp)+,d0-d7/a0-a6 */
	W(0x4EF9); L(routine);                          /* jmp routine */
#undef W
#undef L
	return (size_t)(q - p);
}

/* A routine descriptor for a native routine taking and returning nothing (its TVector filled in later) */
static void make_routine_descriptor(uint8_t *p)
{
	memset(p, 0, 32);
	put_be16(p, 0xAAFE);                    /* _MixedModeMagic */
	p[2] = 7;                               /* kRoutineDescriptorVersion; one routine: */
	p[17] = 1;                              /* kPowerPCISA (procInfo 0: kPascalStackBased, nothing passed) */
}

static void put_pstring(uint8_t *p, const char *s, size_t max)
{
	size_t n = strlen(s);
	n = n < max - 1 ? n : max - 1;
	p[0] = (uint8_t)n;
	memcpy(p + 1, s, n);
}

static void finish_news(struct install *in)
{
	in->phase = INSTALL_DONE;
	in->news_ready = true;
	printf("tools: %s\n", in->news);
}

/* Points _SynchIdleTime's entry at the bootstrap, for the trap being dispatched now. */
static bool inject(struct install *in, struct mem *m)
{
	static uint8_t fragment[R_BUFFER - R_FRAGMENT], head[R_FRAGMENT];
	const struct ndrv_spec spec = {
		.kind = FRAG_APPLICATION,
		.entries = 1,
		.hostcall = HOSTCALL_TOOLS_INSTALL,
		.imports = installer_imports,
		.import_count = I_COUNT,
	};
	size_t len = ndrv_build(&spec, fragment, sizeof fragment, &in->layout);
	uint32_t routine = mem_r32(m, TRAP_ENTRY);
	if (!len) {
		snprintf(in->news, sizeof in->news, "couldn't make the installer");
		finish_news(in);
		return false;
	}
	memset(head, 0, sizeof head);
	make_bootstrap(head + R_CODE, routine, (uint32_t)len);
	make_routine_descriptor(head + R_RD);
	put_be32(head + R_STATUS, STATUS_PENDING);
	put_pstring(head + R_NAME, "syshm Installer", 64);
	mem_map(m, INSTALL_REGION, INSTALL_REGION_SIZE, MEM_RWX);
	mem_load(m, at(0), head, sizeof head);
	mem_load(m, at(R_FRAGMENT), fragment, (uint32_t)len);
	uint8_t entry[4];
	put_be32(entry, at(R_CODE));
	mem_load(m, TRAP_ENTRY, entry, 4);
	in->trap_routine = routine;
	in->fragment_len = (uint32_t)len;
	in->phase = INSTALL_INJECTED;
	printf("tools: installing (Mac OS writes the files)\n");
	return true;
}

/* At a later idle call: the bootstrap ran (or not), but the installer didn't start */
static void injected(struct install *in, struct mem *m)
{
	uint32_t status = mem_r32(m, at(R_STATUS));
	if (status == STATUS_STARTED || status == 0)
		return;                 /* (still preparing it, or about to call it) */
	if (status == STATUS_PENDING) {
		if (mem_r32(m, TRAP_ENTRY) == at(R_CODE)) {
			uint8_t entry[4];
			put_be32(entry, in->trap_routine);
			mem_load(m, TRAP_ENTRY, entry, 4);
		}
		snprintf(in->news, sizeof in->news, "couldn't install the guest tools: Mac OS didn't run the installer");
	} else {
		char msg[256];
		const uint8_t *s = mem_ptr(m, at(R_ERRMSG));
		hfsread_roman_utf8(s + 1, s[0], msg, sizeof msg);
		snprintf(in->news, sizeof in->news,
			 "couldn't install the guest tools: the Code Fragment Manager couldn't prepare the installer "
			 "(error %d%s%s)", (int32_t)status, msg[0] ? ", " : "", msg);
	}
	finish_news(in);
}

/* ---- once Mac OS runs --------------------------------------------------------------- */

#define FINDER_QUIET_TB(hz) ((hz) * 1)          /* idling this long without disk transfers: up */
#define FINDER_BUSY_TB(hz) ((hz) / 4)           /* (longer between idle calls: busy) */
#define FINDER_LONGEST_TB(hz) ((hz) * 15)       /* up anyway, this long after it first ran */
#define QUIT_WAIT_TB(hz) ((hz) * 5)             /* for Enhancements to quit */
#define QUIT_AFTER_TB(hz) ((hz) / 10)           /* ... and then for its process to go */

void install_init(struct install *in)
{
	memset(in, 0, sizeof *in);
}

void install_begin(struct install *in, const char *path, unsigned part, bool read_only)
{
	char err[HFSREAD_PATH_MAX + 128];
	install_init(in);
	in->on = true;
	if (read_only) {
		printf("tools: the startup disk (%s) is read-only: the guest tools aren't offered\n", path);
		return;
	}
	if (install_check(path, part, &in->check, err, sizeof err)) {
		printf("tools: %s\n", err);
		return;
	}
	say_found(in);
	if (in->check.locked)
		printf("tools: \"%s\" is locked: the guest tools aren't offered\n", in->check.volume);
	else if (install_needed(&in->check))
		in->phase = INSTALL_WAIT_FINDER;
}

/*
 * The Finder is up once it has run (been the current application at an
 * idle call), and the guest has then idled for FINDER_QUIET without disk
 * transfers (or FINDER_LONGEST has passed).
 */
static bool finder_up(struct install *in, const struct install_idle *w)
{
	if (w->finder && !in->finder_tb)
		in->finder_tb = in->quiet_tb = w->tb;
	if (!in->finder_tb)
		return false;
	if (w->disk_bytes != in->disk || w->tb - in->idle_tb > FINDER_BUSY_TB(w->tb_hz))
		in->quiet_tb = w->tb;
	in->disk = w->disk_bytes;
	in->idle_tb = w->tb;
	return w->tb - in->quiet_tb >= FINDER_QUIET_TB(w->tb_hz) || w->tb - in->finder_tb >= FINDER_LONGEST_TB(w->tb_hz);
}

bool install_idle(struct install *in, const struct install_idle *w)
{
	if (in->phase == INSTALL_WAIT_FINDER && finder_up(in, w)) {
		in->phase = INSTALL_ASKING;
		if (in->answer) {
			install_answer(in, in->answer > 0, w->helper);
		} else {
			in->ask = true;
			printf("tools: the Finder is up: asking (%s?)\n", in->yes);
		}
	}
	if (in->phase == INSTALL_QUITTING) {
		if (!in->quit_tb)
			in->quit_tb = w->tb;
		if (!w->helper->running && !in->stopped_tb)
			in->stopped_tb = w->tb;
		if ((in->stopped_tb && w->tb - in->stopped_tb >= QUIT_AFTER_TB(w->tb_hz)) ||
		    w->tb - in->quit_tb >= QUIT_WAIT_TB(w->tb_hz))
			in->phase = INSTALL_INJECT;
	}
	if (in->phase == INSTALL_INJECT) {
		in->drive = w->drive;
		return inject(in, w->mem);
	}
	if (in->phase == INSTALL_INJECTED)
		injected(in, w->mem);
	return false;
}

void install_answer(struct install *in, bool yes, struct helper *h)
{
	switch (in->phase) {
	case INSTALL_OFF:
		if (in->on)
			printf("tools: nothing to install\n");
		break;
	case INSTALL_WAIT_FINDER:
		in->answer = yes ? 1 : -1;
		break;
	case INSTALL_ASKING:
		in->ask = false;
		if (!yes) {
			in->phase = INSTALL_DONE;
			printf("tools: not now\n");
		} else if (in->check.tool[TOOLS_ENHANCEMENTS].state == INSTALL_DIFFERS && h->running) {
			h->quit = true;         /* (to replace it, it has to have quit) */
			in->quit_asked = true;
			in->phase = INSTALL_QUITTING;
			printf("tools: asking syshm Enhancements to quit, to replace it\n");
		} else {
			in->phase = INSTALL_INJECT;
		}
		break;
	default:
		break;
	}
}

/* ---- the installer ------------------------------------------------------------------ */

#define K_ON_SYSTEM_DISK 0xFFFF8000u    /* (-32768) */
#define K_EXTENSION_FOLDER FOURCC('e', 'x', 't', 'n')
#define FS_RD_WR_PERM 3u
#define SM_SYSTEM_SCRIPT 0xFFFFFFFFu    /* (-1) */
#define FNF_ERR (-43)
#define WRONG_DISK 1                    /* (not an OSErr: the System Folder isn't the one checked) */
#define TEMP_NAME "syshm installing"
#define TEMP_TYPE FOURCC('T', 'E', 'M', 'P')    /* (until it's whole: nothing is started from it) */
#define TEMP_CREATOR FOURCC('s', 'y', 'H', 'p')

/* HParamBlockRec and CInfoPBRec fields */
#define PB_NAME_PTR 18u
#define PB_VREF_NUM 22u
#define PB_DIR_INDEX 28u
#define PB_ATTRIB 30u
#define PB_VATRB 38u
#define PB_DIR_ID 48u
#define PB_VFNDR_INFO 90u
#define PB_VDRV_INFO 66u
#define ATTRIB_DIRECTORY 0x10u

enum {
	S_FIND_FOLDER, S_VOLUME, S_EXT_NAME, S_SCAN, S_DELETE_TEMP, S_CREATE, S_OPEN_DF, S_WRITE_DF, S_CLOSE_DF,
	S_OPEN_RF, S_WRITE_RF, S_CLOSE_RF, S_SET_INFO, S_DELETE_OLD, S_RENAME, S_LAUNCH, S_CLEANUP_CLOSE,
	S_CLEANUP_DELETE,
};

static void callout(struct install *in, struct ndrv_call *c, unsigned step, unsigned imp, uint32_t a0, uint32_t a1,
		    uint32_t a2, uint32_t a3)
{
	in->step = step;
	ndrv_callout(c, ndrv_import(c, &in->layout, imp), a0, a1, a2, a3);
	memset(c->out + 4, 0, 4 * sizeof c->out[0]);
}

static void put_fsspec(struct mem *m, uint32_t spec, int16_t vref, uint32_t dir, const uint8_t *pname)
{
	memset(mem_ptr(m, spec), 0, 70);
	mem_w16(m, spec, (uint16_t)vref);
	mem_w32(m, spec + 2, dir);
	memcpy(mem_ptr(m, spec + 6), pname, (size_t)pname[0] + 1);
}

/* The parameter block, cleared, with the name buffer (emptied) and the volume */
static uint32_t param_block(struct install *in, struct mem *m)
{
	memset(mem_ptr(m, at(R_PB)), 0, PB_SIZE);
	mem_w8(m, at(R_NAMEBUF), 0);
	mem_w32(m, at(R_PB) + PB_NAME_PTR, at(R_NAMEBUF));
	mem_w16(m, at(R_PB) + PB_VREF_NUM, (uint16_t)in->vref);
	return at(R_PB);
}

static void roman_name(struct mem *m, uint32_t pstring, char *out, size_t len)
{
	const uint8_t *s = mem_ptr(m, pstring);
	hfsread_roman_utf8(s + 1, s[0], out, len);
}

/* The next of the folder's items, looking for the copy to replace (by its file ID) */
static void scan(struct install *in, struct ndrv_call *c)
{
	uint32_t pb = param_block(in, c->mem);
	mem_w16(c->mem, pb + PB_DIR_INDEX, (uint16_t)in->scan_index);
	mem_w32(c->mem, pb + PB_DIR_ID, in->scan_dir);
	callout(in, c, S_SCAN, I_GET_CAT_INFO, pb, 0, 0, 0);
}

static void start_temp(struct install *in, struct ndrv_call *c)
{
	uint8_t name[64];
	put_pstring(name, TEMP_NAME, sizeof name);
	put_fsspec(c->mem, at(R_TEMP), in->vref, in->dir, name);
	callout(in, c, S_DELETE_TEMP, I_FSP_DELETE, at(R_TEMP), 0, 0, 0);       /* (one left over) */
}

static void report(struct install *in, struct ndrv_call *c, struct helper *h);

static void next_tool(struct install *in, struct ndrv_call *c, struct helper *h)
{
	while (in->tool < TOOLS_FILES && in->check.tool[in->tool].state == INSTALL_CURRENT)
		in->tool++;
	in->replacing = in->temp_made = in->temp_open = in->old_deleted = false;
	if (in->tool == TOOLS_FILES) {
		/* Enhancements starts: the new one, or the old one again if it quit for nothing */
		in->relaunch = !in->result[TOOLS_ENHANCEMENTS].done && in->quit_asked && in->old_found;
		const bool start = in->relaunch ? in->old_dir == in->ext_dir :
				   in->result[TOOLS_ENHANCEMENTS].done && in->result[TOOLS_ENHANCEMENTS].dir == in->ext_dir;
		if (start && !h->running) {
			uint32_t pb = at(R_LAUNCH);
			memset(mem_ptr(c->mem, pb), 0, 44);
			mem_w16(c->mem, pb + 6, 0x4C43);        /* extendedBlock */
			mem_w32(c->mem, pb + 8, 32);            /* extendedBlockLen */
			mem_w16(c->mem, pb + 14, 0x4000 | 0x0800 | 0x0200);    /* launchContinue, NoFileFlags, DontSwitch */
			mem_w32(c->mem, pb + 16, at(R_APP));
			in->launch_tried = true;
			callout(in, c, S_LAUNCH, I_LAUNCH, pb, 0, 0, 0);
			return;
		}
		report(in, c, h);
		return;
	}
	const struct install_copy *cp = &in->check.tool[in->tool];
	if (cp->state == INSTALL_DIFFERS) {
		in->scan_dir = cp->dir;
		in->scan_index = 1;
		in->scanned_ext = cp->dir == in->ext_dir;
		scan(in, c);
		return;
	}
	in->dir = in->ext_dir;
	start_temp(in, c);
}

/* Something failed: no temporary file is left behind (unless it's all there is of the tool now) */
static void stop(struct install *in, struct ndrv_call *c, struct helper *h, int16_t err, const char *doing)
{
	if (!in->error) {
		in->error = err;
		in->error_doing = doing;
		in->error_tool = in->tool;
	}
	if (in->temp_open) {
		in->temp_open = false;
		callout(in, c, S_CLEANUP_CLOSE, I_FS_CLOSE, (uint32_t)(int32_t)in->refnum, 0, 0, 0);
		return;
	}
	if (in->temp_made && !in->old_deleted) {
		in->temp_made = false;
		callout(in, c, S_CLEANUP_DELETE, I_FSP_DELETE, at(R_TEMP), 0, 0, 0);
		return;
	}
	in->tool = TOOLS_FILES;
	next_tool(in, c, h);
}

/* Writes a fork of the tool into the open temporary file */
static void write_fork(struct install *in, struct ndrv_call *c, bool rsrc)
{
	struct hfs_file files[TOOLS_FILES];
	tools_files(files);
	const struct hfs_file *f = &files[in->tool];
	uint32_t len = rsrc ? f->rsrc_len : f->data_len;
	mem_load(c->mem, at(R_BUFFER), rsrc ? f->rsrc : f->data, len);
	mem_w32(c->mem, at(R_COUNT), len);
	callout(in, c, rsrc ? S_WRITE_RF : S_WRITE_DF, I_FS_WRITE, (uint32_t)(int32_t)in->refnum, at(R_COUNT),
		at(R_BUFFER), 0);
}

void install_call(struct install *in, struct ndrv_call *c, struct helper *h)
{
	struct mem *m = c->mem;
	struct hfs_file files[TOOLS_FILES];

	if (c->entry == 0) {                    /* main() */
		if (in->phase != INSTALL_INJECTED) {
			ndrv_finish(c, 0);
			return;
		}
		in->phase = INSTALL_RUNNING;
		in->tool = TOOLS_FILES;         /* (none yet) */
		in->error = 0;
		memset(in->result, 0, sizeof in->result);
		callout(in, c, S_FIND_FOLDER, I_FIND_FOLDER, K_ON_SYSTEM_DISK, K_EXTENSION_FOLDER, 1, at(R_VREF));
		c->out[4] = at(R_DIRID);
		return;
	}
	if (in->phase != INSTALL_RUNNING) {
		ndrv_finish(c, 0);
		return;
	}
	int16_t err = (int16_t)c->arg[0];
	uint32_t pb = at(R_PB);
	switch (in->step) {
	case S_FIND_FOLDER:
		if (err) {
			stop(in, c, h, err, "finding the Extensions folder");
			return;
		}
		in->vref = (int16_t)mem_r16(m, at(R_VREF));
		in->ext_dir = mem_r32(m, at(R_DIRID));
		param_block(in, m);
		callout(in, c, S_VOLUME, I_HGET_VINFO, pb, 0, 0, 0);
		return;
	case S_VOLUME:
		if (err)
			stop(in, c, h, err, "looking at the startup disk");
		else if (mem_r16(m, pb + PB_VATRB) & VOLUME_LOCKED)
			stop(in, c, h, -44, "looking at the startup disk");
		else if (mem_r32(m, pb + PB_VFNDR_INFO) != in->check.system_dir || mem_r16(m, pb + PB_VDRV_INFO) != in->drive)
			stop(in, c, h, WRONG_DISK, NULL);
		if (in->error)
			return;
		param_block(in, m);
		mem_w16(m, pb + PB_DIR_INDEX, 0xFFFF);  /* (-1: the folder itself) */
		mem_w32(m, pb + PB_DIR_ID, in->ext_dir);
		callout(in, c, S_EXT_NAME, I_GET_CAT_INFO, pb, 0, 0, 0);
		return;
	case S_EXT_NAME:
		if (err)
			snprintf(in->ext_name, sizeof in->ext_name, "Extensions");
		else
			roman_name(m, at(R_NAMEBUF), in->ext_name, sizeof in->ext_name);
		in->tool = 0;
		next_tool(in, c, h);
		return;
	case S_SCAN:
		if (err == FNF_ERR && !in->scanned_ext) {       /* not there: in the Extensions folder now? */
			in->scanned_ext = true;
			in->scan_dir = in->ext_dir;
			in->scan_index = 1;
			scan(in, c);
		} else if (err == FNF_ERR) {                    /* gone: a new copy, then */
			in->dir = in->ext_dir;
			start_temp(in, c);
		} else if (err) {
			stop(in, c, h, err, "looking for it");
		} else if (!(mem_r8(m, pb + PB_ATTRIB) & ATTRIB_DIRECTORY) &&
			   mem_r32(m, pb + PB_DIR_ID) == in->check.tool[in->tool].id) {
			in->replacing = true;
			in->dir = in->scan_dir;
			put_fsspec(m, at(R_OLD), in->vref, in->dir, mem_ptr(m, at(R_NAMEBUF)));
			if (in->tool == TOOLS_ENHANCEMENTS) {
				in->old_found = true;
				in->old_dir = in->dir;
				put_fsspec(m, at(R_APP), in->vref, in->dir, mem_ptr(m, at(R_NAMEBUF)));
			}
			start_temp(in, c);
		} else {
			in->scan_index++;
			scan(in, c);
		}
		return;
	case S_DELETE_TEMP:
		callout(in, c, S_CREATE, I_FSP_CREATE, at(R_TEMP), TEMP_CREATOR, TEMP_TYPE, SM_SYSTEM_SCRIPT);
		return;
	case S_CREATE:
		if (err) {
			stop(in, c, h, err, "making a file");
			return;
		}
		in->temp_made = true;
		callout(in, c, S_OPEN_DF, I_FSP_OPEN_DF, at(R_TEMP), FS_RD_WR_PERM, at(R_REFNUM), 0);
		return;
	case S_OPEN_DF:
	case S_OPEN_RF:
		if (err) {
			stop(in, c, h, err, "writing it");
			return;
		}
		in->refnum = (int16_t)mem_r16(m, at(R_REFNUM));
		in->temp_open = true;
		write_fork(in, c, in->step == S_OPEN_RF);
		return;
	case S_WRITE_DF:
	case S_WRITE_RF:
		if (err) {
			stop(in, c, h, err, "writing it");
			return;
		}
		callout(in, c, in->step == S_WRITE_DF ? S_CLOSE_DF : S_CLOSE_RF, I_FS_CLOSE, (uint32_t)(int32_t)in->refnum,
			0, 0, 0);
		return;
	case S_CLOSE_DF:
		in->temp_open = false;
		if (err) {
			stop(in, c, h, err, "writing it");
			return;
		}
		callout(in, c, S_OPEN_RF, I_FSP_OPEN_RF, at(R_TEMP), FS_RD_WR_PERM, at(R_REFNUM), 0);
		return;
	case S_CLOSE_RF:
		in->temp_open = false;
		if (err) {
			stop(in, c, h, err, "writing it");
			return;
		}
		tools_files(files);
		memset(mem_ptr(m, at(R_FINFO)), 0, 16);         /* (not placed yet: the Finder places it) */
		mem_w32(m, at(R_FINFO), files[in->tool].type);
		mem_w32(m, at(R_FINFO) + 4, files[in->tool].creator);
		mem_w16(m, at(R_FINFO) + 8, files[in->tool].finder_flags);
		callout(in, c, S_SET_INFO, I_FSP_SET_FINFO, at(R_TEMP), at(R_FINFO), 0, 0);
		return;
	case S_SET_INFO:
		if (err) {
			stop(in, c, h, err, "writing it");
			return;
		}
		if (in->replacing) {
			callout(in, c, S_DELETE_OLD, I_FSP_DELETE, at(R_OLD), 0, 0, 0);
		} else {
			put_pstring(mem_ptr(m, at(R_NEWNAME)), tool_name(in->tool), 64);
			callout(in, c, S_RENAME, I_FSP_RENAME, at(R_TEMP), at(R_NEWNAME), 0, 0);
		}
		return;
	case S_DELETE_OLD:
		if (err) {
			stop(in, c, h, err, "replacing the old copy");
			return;
		}
		in->old_deleted = true;
		memcpy(mem_ptr(m, at(R_NEWNAME)), mem_ptr(m, at(R_OLD) + 6), 64);    /* (it keeps its name) */
		callout(in, c, S_RENAME, I_FSP_RENAME, at(R_TEMP), at(R_NEWNAME), 0, 0);
		return;
	case S_RENAME: {
		if (err) {
			stop(in, c, h, err, "naming it");
			return;
		}
		unsigned t = in->tool;
		in->result[t].done = true;
		in->result[t].replaced = in->replacing;
		in->result[t].dir = in->dir;
		memcpy(in->result[t].folder, in->dir == in->ext_dir ? in->ext_name : in->check.tool[t].folder,
		       sizeof in->result[t].folder);
		roman_name(m, at(R_NEWNAME), in->result[t].old_name, sizeof in->result[t].old_name);
		if (t == TOOLS_ENHANCEMENTS)
			put_fsspec(m, at(R_APP), in->vref, in->dir, mem_ptr(m, at(R_NEWNAME)));
		in->temp_made = false;
		in->tool++;
		next_tool(in, c, h);
		return;
	}
	case S_LAUNCH:
		in->launch_error = err;
		report(in, c, h);
		return;
	case S_CLEANUP_CLOSE:
	case S_CLEANUP_DELETE:
		stop(in, c, h, in->error, in->error_doing);
		return;
	default:
		ndrv_finish(c, 0);
		return;
	}
}

/* What was done, said once the installer has finished */
static void report(struct install *in, struct ndrv_call *c, struct helper *h)
{
	char *s = in->news, buf[64];
	size_t len = sizeof in->news;
	const struct install_result *r = in->result;
	s[0] = '\0';
	if (r[0].done && r[1].done && r[0].replaced == r[1].replaced && r[0].dir == r[1].dir &&
	    !strcmp(r[0].old_name, tool_name(0)) && !strcmp(r[1].old_name, tool_name(1))) {
		add(s, len, "%s %s and %s in \"%s:%s\"", r[0].replaced ? "updated" : "installed", tool_name(0), tool_name(1),
		    in->check.system, r[0].folder);
	} else {
		for (unsigned t = 0, k = 0; t < TOOLS_FILES; t++) {
			if (!r[t].done)
				continue;
			add(s, len, "%s%s %s", k++ ? " and " : "", r[t].replaced ? "updated" : "installed", tool_name(t));
			if (strcmp(r[t].old_name, tool_name(t)) != 0)
				add(s, len, " (\"%s\")", r[t].old_name);
			add(s, len, " in \"%s:%s\"", in->check.system, r[t].folder);
		}
	}
	if (in->error) {
		unsigned t = in->error_tool;
		add(s, len, "%scouldn't %s %s", s[0] ? "; " : "",
		    t < TOOLS_FILES && in->check.tool[t].state == INSTALL_DIFFERS ? "update" : "install",
		    t < TOOLS_FILES ? tool_name(t) : "the guest tools");
		if (in->error == WRONG_DISK)
			add(s, len, ": Mac OS didn't start up from \"%s\"", in->check.system);
		else
			add(s, len, " (%s): %s", in->error_doing, error_text(in->error, buf, sizeof buf));
	}
	add(s, len, ".");
	if (in->relaunch && in->launch_tried && !in->launch_error)
		add(s, len, " The old syshm Enhancements is running again.");
	else if (in->quit_asked && !r[TOOLS_ENHANCEMENTS].done)
		add(s, len, " syshm Enhancements starts with Mac OS next time.");
	if (r[TOOLS_ENHANCEMENTS].done) {
		if (in->launch_tried && !in->launch_error)
			add(s, len, " syshm Enhancements is running.");
		else if (in->launch_tried)
			add(s, len, " syshm Enhancements starts with Mac OS next time (it couldn't start now: %s).",
			    error_text(in->launch_error, buf, sizeof buf));
		else if (r[TOOLS_ENHANCEMENTS].dir != in->ext_dir)
			add(s, len, " Mac OS starts syshm Enhancements only from \"%s\".", in->ext_name);
		else if (h->running)
			add(s, len, " (Another copy of syshm Enhancements is running.)");
	}
	if (r[TOOLS_3D_ENGINE].done)
		add(s, len, " Programs opened from now on find the 3D engine.");
	finish_news(in);
	ndrv_finish(c, 0);
}
