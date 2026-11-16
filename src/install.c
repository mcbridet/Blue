#include "install.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "be.h"
#include "disk.h"
#include "hfs.h"
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
#define MAX_FORK 0x40000u               /* (a bigger fork isn't one of ours) */
#define MAX_FOLDERS 64u
#define VOLUME_LOCKED 0x8080u           /* volume attributes: locked by hardware, by software */

/* What each tool's code calls (installed copies keep these numbers, see tools.h) */
static const uint32_t tool_hostcall[TOOLS_FILES] = { HOSTCALL_TOOLS_HELPER, HOSTCALL_TOOLS_RAVE, HOSTCALL_TOOLS_GL,
						     HOSTCALL_TOOLS_GLM, HOSTCALL_TOOLS_GLU, HOSTCALL_TOOLS_PIPPIN };

/* Apple's OpenGL libraries (see install.h), and the folders the check knows */
static const char *const apple_names[INSTALL_APPLE_GL] = { "OpenGLLibrary", "OpenGLMemory", "OpenGLUtility" };
#define EXTENSIONS_NAME "Extensions"
#define DISABLED_NAME "Extensions (Disabled)"

/* ---- Blue's preferences file -------------------------------------------------------- */

#define PREFS_TYPE FOURCC('p', 'r', 'e', 'f')
#define PREFS_CREATOR FOURCC('s', 'y', 'H', 'p')       /* (Blue Enhancements') */
#define PREFS_HEADER "Blue Preferences"
#define PREFS_OFFER "Offer the guest tools: "

size_t install_prefs_make(char *out, size_t len, bool never)
{
	int n = snprintf(out, len, PREFS_HEADER "\r" PREFS_OFFER "%s\r", never ? "no" : "yes");
	return n > 0 && (size_t)n < len ? (size_t)n : 0;
}

bool install_prefs_parse(const uint8_t *data, size_t len, bool *never)
{
	const size_t header = sizeof PREFS_HEADER - 1, offer = sizeof PREFS_OFFER - 1;
	*never = false;
	if (len < header + 1 || memcmp(data, PREFS_HEADER, header) != 0 || (data[header] != '\r' && data[header] != '\n'))
		return false;
	for (size_t at = header + 1; at < len;) {
		size_t end = at;
		while (end < len && data[end] != '\r' && data[end] != '\n')
			end++;
		if (end - at == offer + 2 && memcmp(data + at, PREFS_OFFER, offer) == 0 &&
		    memcmp(data + at + offer, "no", 2) == 0)
			*never = true;
		at = end + 1;
	}
	return true;
}

static bool is_gl(unsigned t)
{
	return t >= TOOLS_GL_FIRST && t < TOOLS_GL_FIRST + TOOLS_GL_FILES;
}

/* Whether a tool is to be installed or updated, if it isn't current: not Blue's OpenGL when Apple's go back */
static bool wanted(const struct install_check *ck, unsigned t)
{
	return !((ck->apple_restore || ck->apple_kept) && is_gl(t));
}

struct look {
	const struct hfsread_volume *v;
	struct install_check *ck;
	const struct hfs_file *files;
	uint8_t *buf;                   /* MAX_FORK bytes */
	struct folder { uint32_t id; char name[HFSREAD_NAME_MAX]; } *sub;      /* the System Folder's folders */
	unsigned subs;
	uint32_t last;                  /* the highest of their IDs */
};

#define WALK_STOP 1                     /* (what a walk's function returns to stop it early) */

/* (records go by the ID of the folder they're in, so the System Folder's own come together) */
static int add_folder(void *ctx, const struct hfsread_file *f)
{
	struct look *l = ctx;
	if (f->parent > l->ck->system_dir)
		return WALK_STOP;
	if (f->folder && f->parent == l->ck->system_dir && l->subs < MAX_FOLDERS) {
		l->sub[l->subs].id = f->id;
		memcpy(l->sub[l->subs].name, f->name, sizeof f->name);
		l->last = f->id > l->last ? f->id : l->last;
		l->subs++;
	}
	return 0;
}

/* Whether a file is this tool: 0 not, 1 another version's, 2 this version's (both forks the same) */
static int which_copy(struct look *l, const struct hfsread_file *e, unsigned t)
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

static int look_at_file(void *ctx, const struct hfsread_file *e)
{
	struct look *l = ctx;
	if (e->parent > l->last)
		return WALK_STOP;
	unsigned in = 0;
	while (in < l->subs && l->sub[in].id != e->parent)
		in++;
	if (!e->folder && in < l->subs && e->type == PREFS_TYPE && e->creator == PREFS_CREATOR &&
	    !strcmp(e->name, INSTALL_PREFS_NAME) && e->data.size <= INSTALL_PREFS_MAX && !l->ck->prefs_found &&
	    hfsread_read(l->v, &e->data, 0, l->buf, (size_t)e->data.size) == 0 &&
	    install_prefs_parse(l->buf, (size_t)e->data.size, &l->ck->never)) {
		l->ck->prefs_found = true;
		l->ck->prefs_dir = e->parent;
	}
	if (!e->folder && in < l->subs && e->type == FOURCC('s', 'h', 'l', 'b') &&
	    e->creator != l->files[TOOLS_GL_FIRST].creator) {          /* (Apple's OpenGL libraries) */
		int where = !strcmp(l->sub[in].name, EXTENSIONS_NAME) ? 0 : !strcmp(l->sub[in].name, DISABLED_NAME) ? 1 : -1;
		for (unsigned k = 0; where >= 0 && k < INSTALL_APPLE_GL; k++)
			if (!strcmp(e->name, apple_names[k])) {
				l->ck->apple[k].found[where] = true;
				l->ck->apple[k].dir[where] = e->parent;
			}
	}
	for (unsigned t = 0; t < TOOLS_FILES && !e->folder && in < l->subs; t++) {
		struct install_copy *c = &l->ck->tool[t];
		if (e->type != l->files[t].type || e->creator != l->files[t].creator || c->state == INSTALL_CURRENT)
			continue;
		int copy = which_copy(l, e, t);
		if (!copy || (copy == 1 && c->state == INSTALL_DIFFERS))
			continue;               /* (of several, the current one, or else the first) */
		c->state = copy == 2 ? INSTALL_CURRENT : INSTALL_DIFFERS;
		c->dir = e->parent;
		c->id = e->id;
		memcpy(c->folder, l->sub[in].name, sizeof c->folder);
	}
	return 0;
}

/* What's to be done with Apple's OpenGL libraries (see install.h) */
static void apple_state(struct install_check *ck)
{
	bool ours_enabled = true, ours = false, on = false, off = false;
	for (unsigned t = TOOLS_GL_FIRST; t < TOOLS_GL_FIRST + TOOLS_GL_FILES; t++) {
		ours |= ck->tool[t].state != INSTALL_MISSING;
		ours_enabled &= ck->tool[t].state != INSTALL_MISSING && !strcmp(ck->tool[t].folder, EXTENSIONS_NAME);
	}
	for (unsigned k = 0; k < INSTALL_APPLE_GL; k++) {
		on |= ck->apple[k].found[0];
		off |= ck->apple[k].found[1];
	}
	ck->apple_disable = ours_enabled && on;
	ck->apple_restore = !ours && !on && off;
}

int install_check(const char *path, unsigned part, struct install_check *ck, char *err, size_t errlen)
{
	struct hfs_file files[TOOLS_FILES];
	struct look l = { .ck = ck, .files = files };
	struct hfsread_volume v;
	struct disk d;
	struct disk_reader reader = { &d, part };

	memset(ck, 0, sizeof *ck);
	if (tools_files(files))
		return fail(err, errlen, "cannot make the tools");
	if (disk_open(&d, NULL, path, true, cdimage_has_toc(path)))      /* (any image, read-only) */
		return fail(err, errlen, "%s: cannot open it", path);
	int rc = -1;
	l.v = &v;
	l.buf = malloc(MAX_FORK);
	l.sub = malloc(MAX_FOLDERS * sizeof *l.sub);
	if (!l.buf || !l.sub) {
		fail(err, errlen, "out of memory");
	} else if (part >= d.partitions || hfsread_open_with(&v, disk_read, &reader, d.part[part].size)) {
		fail(err, errlen, "%s: no HFS volume there", path);
	} else if (!v.blessed) {
		fail(err, errlen, "%s: no System Folder", path);
	} else {
		ck->system_dir = v.blessed;
		ck->locked = (v.attributes & VOLUME_LOCKED) != 0;
		int walked = hfsread_path(&v, ROOT_ID, ck->volume, sizeof ck->volume) ||
			     hfsread_path(&v, v.blessed, ck->system, sizeof ck->system) ? -1 : hfsread_walk(&v, add_folder, &l);
		if (walked >= 0 && l.subs)
			walked = hfsread_walk(&v, look_at_file, &l);
		apple_state(ck);
		rc = walked < 0 ? fail(err, errlen, "%s: its catalog is damaged", path) : 0;
	}
	free(l.buf);
	free(l.sub);
	disk_close(&d);
	return rc;
}

bool install_needed(const struct install_check *ck)
{
	for (unsigned t = 0; t < TOOLS_FILES; t++)
		if (ck->tool[t].state != INSTALL_CURRENT && wanted(ck, t))
			return true;
	return ck->apple_disable || ck->apple_restore;
}

/* ---- telling the user ------------------------------------------------------------- */

static const char *tool_name(unsigned t)
{
	static const char *const names[TOOLS_FILES] = { "Blue Enhancements", "Blue 3D Engine", "Blue OpenGL Library",
							"Blue OpenGL Memory", "Blue OpenGL Utility", "Blue Pippin Library" };
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
#define APPLE_GOES " Apple's OpenGL libraries go to \"" DISABLED_NAME "\": Blue's take their place."

static void say_found(struct install *in)
{
	const struct install_check *ck = &in->check;
	char line[1024] = "";
	for (unsigned t = 0; t < TOOLS_FILES; t++) {
		const struct install_copy *c = &ck->tool[t];
		add(line, sizeof line, "%s%s %s", joint(t, TOOLS_FILES), tool_name(t),
		    c->state == INSTALL_MISSING ? "isn't installed" :
		    c->state == INSTALL_DIFFERS ? "differs from this version's" : "is current");
		if (c->state != INSTALL_MISSING)
			add(line, sizeof line, " (in \"%s\")", c->folder);
	}
	bool on = false, off = false;
	for (unsigned k = 0; k < INSTALL_APPLE_GL; k++) {
		on |= ck->apple[k].found[0];
		off |= ck->apple[k].found[1];
	}
	if (on || off)
		add(line, sizeof line, "; Apple's OpenGL libraries are in %s%s%s", on ? "\"" EXTENSIONS_NAME "\"" : "",
		    on && off ? " and " : "", off ? "\"" DISABLED_NAME "\"" : "");
	if (ck->apple_kept)
		add(line, sizeof line, " (put back in this run: Blue's aren't offered)");
	if (ck->prefs_found)
		add(line, sizeof line, "; " INSTALL_PREFS_NAME " says %s", ck->never ? "not to offer them" : "to offer them");
	printf("tools: on \"%s\", %s\n", ck->volume, line);
	in->never = "Don't Ask Again";

	unsigned installed = 0, todo = 0;
	bool gl_todo = false;
	for (unsigned t = 0; t < TOOLS_FILES; t++) {
		installed += ck->tool[t].state != INSTALL_MISSING;
		todo += ck->tool[t].state != INSTALL_CURRENT && wanted(ck, t);
		gl_todo |= is_gl(t) && ck->tool[t].state == INSTALL_MISSING && wanted(ck, t);
	}
	const char *apple = on && gl_todo ? APPLE_GOES : "";
	if (!installed) {
		snprintf(in->question, sizeof in->question,
			 "Blue's guest tools aren't installed on \"%s\". " ABOUT "%s", ck->volume, apple);
		in->yes = "Install and Restart";
		return;
	}
	if (!todo && ck->apple_restore) {
		snprintf(in->question, sizeof in->question, "Blue's OpenGL libraries aren't on \"%s\", and Apple's are in "
			 "\"" DISABLED_NAME "\": put Apple's back in \"" EXTENSIONS_NAME "\"?", ck->volume);
		in->yes = "Put Back and Restart";
		return;
	}
	if (!todo) {
		snprintf(in->question, sizeof in->question, "Apple's OpenGL libraries are in \"" EXTENSIONS_NAME "\" on "
			 "\"%s\" with Blue's: move Apple's to \"" DISABLED_NAME "\"?", ck->volume);
		in->yes = "Move and Restart";
		return;
	}
	char what[512] = "";
	for (unsigned t = 0, k = 0; t < TOOLS_FILES; t++)
		if (ck->tool[t].state != INSTALL_CURRENT && wanted(ck, t))
			add(what, sizeof what, "%s%s %s", joint(k++, todo), tool_name(t),
			    ck->tool[t].state == INSTALL_MISSING ? "isn't installed" : "is from another version of Blue");
	snprintf(in->question, sizeof in->question, "Blue's guest tools on \"%s\" need updating: %s.%s%s", ck->volume,
		 what, ck->apple_restore ? " Apple's OpenGL libraries go back to \"" EXTENSIONS_NAME "\"." : "",
		 ck->apple_disable ? " Apple's OpenGL libraries go to \"" DISABLED_NAME "\"." : apple);
	in->yes = "Update and Restart";
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
	R_OLD = 0x04D0,                 /* ... the copy it replaces */
	R_NEWNAME = 0x0570,             /* (Str63) */
	R_FINFO = 0x05B0,
	R_PROCESS = 0x05C0,             /* the Process Manager's process serial number, */
	R_ADDRESS = 0x05C8,             /* ... the Restart event's address (an AEDesc), */
	R_EVENT = 0x05D0,               /* ... the event, */
	R_REPLY = 0x05D8,               /* ... and its reply (none) */
	R_PREFS = 0x0600,               /* an FSSpec: Blue's preferences file */
	R_FRAGMENT = 0x1000,            /* the installer's container */
	R_BUFFER = 0x4000,              /* a fork, for FSWrite */
};
_Static_assert(R_BUFFER + MAX_FORK <= INSTALL_REGION_SIZE, "room for a fork of any tool's");
#define RD_PROC 20u                     /* the routine record's procDescriptor */
#define PB_SIZE 128u
#define STATUS_PENDING 1u
#define STATUS_STARTED 2u

enum {
	I_FIND_FOLDER, I_HGET_VINFO, I_GET_CAT_INFO, I_FSP_DELETE, I_FSP_CREATE, I_FSP_OPEN_DF, I_FSP_OPEN_RF,
	I_FS_WRITE, I_FS_CLOSE, I_FSP_SET_FINFO, I_FSP_RENAME, I_CAT_MOVE, I_FLUSH_VOL, I_AE_CREATE_DESC,
	I_AE_CREATE_APPLE_EVENT, I_AE_SEND, I_AE_DISPOSE_DESC, I_COUNT,
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
	[I_CAT_MOVE] = { "InterfaceLib", "PBCatMoveSync" },
	[I_FLUSH_VOL] = { "InterfaceLib", "FlushVol" },
	[I_AE_CREATE_DESC] = { "InterfaceLib", "AECreateDesc" },
	[I_AE_CREATE_APPLE_EVENT] = { "InterfaceLib", "AECreateAppleEvent" },
	[I_AE_SEND] = { "InterfaceLib", "AESend" },
	[I_AE_DISPOSE_DESC] = { "InterfaceLib", "AEDisposeDesc" },
};

static uint32_t at(uint32_t offset)
{
	return INSTALL_REGION + offset;
}

/*
 * The 68k code the idle trap's entry in the trap table points at (once): it
 * saves the registers, puts the entry back, prepares the installer and calls
 * its main routine, closes the connection, and goes on to the trap's routine.
 */
static size_t make_bootstrap(uint8_t *p, uint32_t entry, uint32_t routine, uint32_t fragment_len)
{
	uint8_t *q = p, *bne;
#define W(v) (put_be16(q, (uint16_t)(v)), q += 2)
#define L(v) (put_be32(q, (uint32_t)(v)), q += 4)
	W(0x48E7); W(0xFFFE);                           /* movem.l d0-d7/a0-a6,-(sp) */
	W(0x23FC); L(routine); L(entry);                /* move.l #routine,entry */
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

/* The trap table's entry for a trap (OS traps' table at 0x400, the Toolbox's at 0xE00) */
static uint32_t trap_entry(uint16_t trap)
{
	return trap & 0x0800u ? 0x0E00u + 4u * (trap & 0x3FFu) : 0x0400u + 4u * (trap & 0xFFu);
}

/* Points the idle trap's entry at the bootstrap, for its dispatch under way. */
static bool inject(struct install *in, struct mem *m, uint16_t trap)
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
	uint32_t entry = trap_entry(trap), routine = mem_r32(m, entry);
	if (!len) {
		snprintf(in->news, sizeof in->news, "couldn't make the installer");
		finish_news(in);
		return false;
	}
	memset(head, 0, sizeof head);
	make_bootstrap(head + R_CODE, entry, routine, (uint32_t)len);
	make_routine_descriptor(head + R_RD);
	put_be32(head + R_STATUS, STATUS_PENDING);
	put_pstring(head + R_NAME, "Blue Installer", 64);
	mem_map(m, INSTALL_REGION, INSTALL_REGION_SIZE, MEM_RWX);
	mem_load(m, at(0), head, sizeof head);
	mem_load(m, at(R_FRAGMENT), fragment, (uint32_t)len);
	uint8_t to[4];
	put_be32(to, at(R_CODE));
	mem_load(m, entry, to, 4);
	in->trap_entry = entry;
	in->trap_routine = routine;
	in->fragment_len = (uint32_t)len;
	in->phase = INSTALL_INJECTED;
	if (in->job == INSTALL_JOB_TOOLS)
		printf("tools: installing%s (Mac OS writes the files)\n", in->retried ? " again" : "");
	return true;
}

/* At a later idle call: the bootstrap ran (or not), but the installer didn't start */
static void injected(struct install *in, struct mem *m)
{
	uint32_t status = mem_r32(m, at(R_STATUS));
	if (status == STATUS_STARTED || status == 0)
		return;                 /* (still preparing it, or about to call it) */
	if (status == STATUS_PENDING) {
		if (mem_r32(m, in->trap_entry) == at(R_CODE)) {
			uint8_t to[4];
			put_be32(to, in->trap_routine);
			mem_load(m, in->trap_entry, to, 4);
		}
		snprintf(in->news, sizeof in->news, "couldn't install the guest tools: Mac OS didn't run the installer");
	} else {
		char msg[256];
		const uint8_t *s = mem_ptr(m, at(R_ERRMSG));
		hfs_to_utf8(s + 1, s[0], msg, sizeof msg);
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
#define START_WAIT_TB(hz) ((hz) * 60)           /* ... one being launched: to start, then quit */
#define QUIT_AFTER_TB(hz) ((hz) / 10)           /* ... and then for its process to go */
#define INJECT_WAIT_TB(hz) ((hz) * 10)          /* for the idle trap's call */

void install_init(struct install *in)
{
	memset(in, 0, sizeof *in);
}

void install_begin(struct install *in, const char *path, unsigned part, bool read_only)
{
	char err[HFSREAD_PATH_MAX + 128];
	bool apple_kept = in->check.apple_kept;
	install_init(in);
	in->on = true;
	in->check.apple_kept = apple_kept;
	if (read_only) {
		printf("tools: the startup disk (%s) is read-only: the guest tools aren't offered\n", path);
		return;
	}
	int checked = install_check(path, part, &in->check, err, sizeof err);
	in->check.apple_kept = apple_kept;
	if (checked) {
		printf("tools: %s\n", err);
		return;
	}
	say_found(in);
	if (in->check.locked) {
		printf("tools: \"%s\" is locked: the guest tools aren't offered\n", in->check.volume);
		return;
	}
	in->checked = true;
	if (install_needed(&in->check) && in->check.never)
		printf("tools: not offered (" INSTALL_PREFS_NAME ": Don't Ask Again; Blue's menu, F12, offers them)\n");
	else if (install_needed(&in->check))
		in->phase = INSTALL_WAIT_FINDER;
}

/*
 * The Finder is up once it has run (been the current application, at an
 * idle call or in between), and the guest has then idled for FINDER_QUIET
 * without disk transfers (or FINDER_LONGEST has passed).
 */
void install_finder(struct install *in, uint64_t tb)
{
	if (!in->finder_tb)
		in->finder_tb = in->quiet_tb = tb;
}

static bool finder_up(struct install *in, const struct install_idle *w)
{
	if (w->finder)
		install_finder(in, w->tb);
	if (!in->finder_tb)
		return false;
	if (w->disk_bytes != in->disk || w->tb - in->idle_tb > FINDER_BUSY_TB(w->tb_hz))
		in->quiet_tb = w->tb;
	in->disk = w->disk_bytes;
	in->idle_tb = w->tb;
	return w->tb - in->quiet_tb >= FINDER_QUIET_TB(w->tb_hz) || w->tb - in->finder_tb >= FINDER_LONGEST_TB(w->tb_hz);
}

/*
 * Enhancements' file is in use while it runs, so a copy that's to be
 * replaced quits first (it does at its next WaitNextEvent, or at its first,
 * one that's being launched). Not asked again if it has quit for this
 * install already: then its process just hadn't gone.
 */
static void ask_to_quit(struct install *in, struct helper *h)
{
	if (!in->quit_asked || h->quits == in->quits || h->running) {
		h->quit = true;
		in->quits = h->quits;
		in->quit_running = h->running;
	}
	in->quit_asked = true;
	in->quit_tb = in->stopped_tb = in->inject_tb = 0;
	in->phase = INSTALL_QUITTING;
}

/* Before the installer goes in: a running Enhancements that's to be replaced quits first (true if asked to) */
static bool quit_first(struct install *in, struct helper *h)
{
	if (in->job != INSTALL_JOB_TOOLS || in->quit_asked || in->check.tool[TOOLS_ENHANCEMENTS].state != INSTALL_DIFFERS ||
	    !h->running)
		return false;
	ask_to_quit(in, h);
	printf("tools: asking Blue Enhancements to quit, to replace it\n");
	return true;
}

bool install_idle(struct install *in, const struct install_idle *w)
{
	struct helper *h = w->helper;
	if (in->phase == INSTALL_WAIT_FINDER && finder_up(in, w)) {
		in->phase = INSTALL_ASKING;
		if (in->answered) {
			install_answer(in, in->answer, h);
		} else {
			in->ask = true;
			printf("tools: the Finder is up: asking (%s?)\n", in->yes);
		}
	}
	if (in->phase == INSTALL_INJECT && quit_first(in, h))
		return false;           /* (one that started after the answer) */
	if (in->phase == INSTALL_BUSY)
		ask_to_quit(in, h);     /* (the installer found it in use: see install_call()) */
	if (in->phase == INSTALL_QUITTING) {
		if (!in->quit_tb)
			in->quit_tb = w->tb;
		if (h->quits != in->quits && !h->running && !in->stopped_tb)
			in->stopped_tb = w->tb;
		if ((in->stopped_tb && w->tb - in->stopped_tb >= QUIT_AFTER_TB(w->tb_hz)) ||
		    w->tb - in->quit_tb >= (in->quit_running ? QUIT_WAIT_TB(w->tb_hz) : START_WAIT_TB(w->tb_hz)))
			in->phase = INSTALL_INJECT;
	}
	if (in->phase == INSTALL_INJECT) {
		if (!in->inject_tb)
			in->inject_tb = w->tb;
		if (w->trap) {
			in->drive = w->drive;
			return inject(in, w->mem, w->trap);
		}
		if (w->tb - in->inject_tb >= INJECT_WAIT_TB(w->tb_hz)) {
			snprintf(in->news, sizeof in->news, "couldn't install the guest tools: Mac OS idles without a call "
				 "Blue knows (the Process Manager's _SynchIdleTime, or Mac OS 9's in its place)");
			finish_news(in);
		}
	}
	if (in->phase == INSTALL_INJECTED)
		injected(in, w->mem);
	return false;
}

void install_answer(struct install *in, enum install_answer a, struct helper *h)
{
	switch (in->phase) {
	case INSTALL_OFF:
		if (in->on)
			printf(in->checked && install_needed(&in->check) ? "tools: not offered (Don't Ask Again)\n" :
			       "tools: nothing to install\n");
		break;
	case INSTALL_WAIT_FINDER:
		in->answer = a;
		in->answered = true;
		break;
	case INSTALL_ASKING:
		in->ask = false;
		in->job = a == INSTALL_NEVER ? INSTALL_JOB_NEVER : INSTALL_JOB_TOOLS;
		if (a == INSTALL_NOT_NOW) {
			in->phase = INSTALL_DONE;
			printf("tools: not now\n");
		} else if (a == INSTALL_NEVER) {
			in->phase = INSTALL_INJECT;
			printf("tools: don't ask again (Mac OS writes " INSTALL_PREFS_NAME ")\n");
		} else if (!quit_first(in, h)) {
			in->phase = INSTALL_INJECT;
		}
		break;
	default:
		break;
	}
}

const char *install_unofferable(const struct install *in)
{
	if (!in->checked)
		return "There's no startup disk Blue can install them on (a writable disk image with a System Folder)";
	if (in->phase >= INSTALL_BUSY && in->phase <= INSTALL_RUNNING)
		return "Mac OS is installing them";
	if (in->phase == INSTALL_DONE && in->restarting)
		return "Mac OS is restarting";
	if (!install_needed(&in->check))
		return "They're installed, and this version's";
	return NULL;
}

void install_offer(struct install *in)
{
	const char *why = install_unofferable(in);
	if (why) {
		printf("tools: not offered: %s\n", why);
		return;
	}
	in->phase = INSTALL_ASKING;
	in->answered = false;
	in->ask = true;
	in->quit_asked = in->retried = false;
	in->quit_tb = in->stopped_tb = in->inject_tb = 0;
	printf("tools: asking (%s?)\n", in->yes);
}

/* ---- the installer ------------------------------------------------------------------ */

#define K_ON_SYSTEM_DISK 0xFFFF8000u    /* (-32768) */
#define K_EXTENSION_FOLDER FOURCC('e', 'x', 't', 'n')
#define K_EXTENSION_DISABLED_FOLDER FOURCC('e', 'x', 't', 'D')
#define K_PREFERENCES_FOLDER FOURCC('p', 'r', 'e', 'f')
#define FS_RD_WR_PERM 3u
#define SM_SYSTEM_SCRIPT 0xFFFFFFFFu    /* (-1) */
#define FNF_ERR (-43)
#define FBSY_ERR (-47)
#define WRONG_DISK 1                    /* (not an OSErr: the System Folder isn't the one checked) */
#define TEMP_NAME "Blue installing"
#define TEMP_TYPE FOURCC('T', 'E', 'M', 'P')    /* (until it's whole: nothing is started from it) */
#define TEMP_CREATOR FOURCC('s', 'y', 'H', 'p')

/* HParamBlockRec and CInfoPBRec fields */
#define PB_NAME_PTR 18u
#define PB_VREF_NUM 22u
#define PB_NEW_NAME 28u                 /* (CMovePBRec) */
#define PB_NEW_DIR_ID 36u
#define PB_DIR_INDEX 28u
#define PB_ATTRIB 30u
#define PB_VATRB 38u
#define PB_DIR_ID 48u
#define PB_VFNDR_INFO 90u
#define PB_VDRV_INFO 66u
#define ATTRIB_DIRECTORY 0x10u

/*
 * The Restart event (kCoreEventClass, kAERestart), sent to the Process
 * Manager (the system process), which restarts as Special > Restart does:
 * each program is asked to quit, and can ask to save its work first, or stop
 * the restart. (The Finder ignores this one; its own is 'fndr' 'rest'.)
 */
#define TYPE_PROCESS_SERIAL_NUMBER FOURCC('p', 's', 'n', ' ')
#define K_SYSTEM_PROCESS 1u
#define K_CORE_EVENT_CLASS FOURCC('a', 'e', 'v', 't')
#define K_AE_RESTART FOURCC('r', 'e', 's', 't')
#define AUTO_GENERATE_RETURN_ID 0xFFFFFFFFu             /* (-1) */
#define AE_NO_REPLY 0x01u
#define AE_ALWAYS_INTERACT 0x30u
#define AE_CAN_SWITCH_LAYER 0x40u
#define AE_DEFAULT_TIMEOUT 0xFFFFFFFFu                  /* (-1) */
#define AE_DESC_SIZE 8u

enum {
	S_FIND_FOLDER, S_VOLUME, S_EXT_NAME, S_SCAN, S_DELETE_TEMP, S_CREATE, S_OPEN_DF, S_WRITE_DF, S_CLOSE_DF,
	S_OPEN_RF, S_WRITE_RF, S_CLOSE_RF, S_SET_INFO, S_DELETE_OLD, S_RENAME, S_CLEANUP_CLOSE, S_CLEANUP_DELETE, S_BUSY,
	S_DISABLED_FOLDER, S_DISABLED_NAME, S_APPLE_MOVE, S_FLUSH, S_RESTART_ADDRESS, S_RESTART_EVENT,
	S_RESTART_SEND, S_DISPOSE_EVENT, S_DISPOSE_ADDRESS,
	S_PREFS_FOLDER, S_PREFS_NAME, S_PREFS_DELETE, S_PREFS_CREATE, S_PREFS_OPEN, S_PREFS_WRITE, S_PREFS_CLOSE,
	S_PREFS_FLUSH, S_PREFS_FORGET,
};
_Static_assert(TOOLS_ENHANCEMENTS == 0, "Enhancements goes first: when it's found in use, the installer starts over");

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

static void put_name_spec(struct mem *m, uint32_t spec, int16_t vref, uint32_t dir, const char *name)
{
	uint8_t pname[64];
	put_pstring(pname, name, sizeof pname);
	put_fsspec(m, spec, vref, dir, pname);
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
	hfs_to_utf8(s + 1, s[0], out, len);
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

static void report(struct install *in, struct ndrv_call *c);
static void next_tool(struct install *in, struct ndrv_call *c);

/* The folder of one of Blue's tools once it's been written (or not): 0 if it isn't there */
static uint32_t tool_dir(const struct install *in, unsigned t)
{
	return in->result[t].done ? in->result[t].dir : in->check.tool[t].state != INSTALL_MISSING ? in->check.tool[t].dir : 0;
}

/* The next of Apple's OpenGL libraries to move (see install.h), or on to what's after them */
static void apple_next(struct install *in, struct ndrv_call *c)
{
	for (; in->apple < INSTALL_APPLE_GL; in->apple++) {
		const struct install_apple *a = &in->check.apple[in->apple];
		uint32_t from, to;
		if (in->apple_disabling && a->found[0] && a->dir[0] == in->ext_dir) {
			from = in->ext_dir;
			to = in->disabled_dir;
		} else if (in->apple_restoring && a->found[1] && !a->found[0] && a->dir[1] != in->ext_dir) {
			from = a->dir[1];
			to = in->ext_dir;
		} else {
			continue;
		}
		uint32_t pb = param_block(in, c->mem);
		put_pstring(mem_ptr(c->mem, at(R_NAMEBUF)), apple_names[in->apple], 256);
		mem_w32(c->mem, pb + PB_DIR_ID, from);
		mem_w32(c->mem, pb + PB_NEW_DIR_ID, to);
		mem_w32(c->mem, pb + PB_NEW_NAME, 0);
		callout(in, c, S_APPLE_MOVE, I_CAT_MOVE, pb, 0, 0, 0);
		return;
	}
	next_tool(in, c);
}

/* Blue's tools written: Apple's OpenGL libraries to Extensions (Disabled) if Blue's are in Extensions, or back */
static void apple_begin(struct install *in, struct ndrv_call *c)
{
	bool enabled = true, ours = false;
	for (unsigned t = TOOLS_GL_FIRST; t < TOOLS_GL_FIRST + TOOLS_GL_FILES; t++) {
		enabled &= tool_dir(in, t) == in->ext_dir;
		ours |= tool_dir(in, t) != 0;
	}
	in->apple_done = true;
	in->apple = 0;
	in->apple_restoring = !ours && in->check.apple_restore;
	in->apple_disabling = false;
	for (unsigned k = 0; k < INSTALL_APPLE_GL; k++)
		in->apple_disabling |= enabled && in->check.apple[k].found[0] && in->check.apple[k].dir[0] == in->ext_dir;
	if (in->apple_disabling) {
		callout(in, c, S_DISABLED_FOLDER, I_FIND_FOLDER, K_ON_SYSTEM_DISK, K_EXTENSION_DISABLED_FOLDER, 1,
			at(R_VREF));
		c->out[4] = at(R_DIRID);
		return;
	}
	apple_next(in, c);
}

/* Whether everything asked for was done: then Mac OS restarts */
static bool all_done(const struct install *in)
{
	bool any = false;
	for (unsigned t = 0; t < TOOLS_FILES; t++)
		any |= in->result[t].done;
	for (unsigned k = 0; k < INSTALL_APPLE_GL; k++) {
		if (in->apple_moved[k].error)
			return false;
		any |= in->apple_moved[k].moved;
	}
	return any && !in->error;
}

static void next_tool(struct install *in, struct ndrv_call *c)
{
	while (in->tool < TOOLS_FILES &&
	       (in->check.tool[in->tool].state == INSTALL_CURRENT || !wanted(&in->check, in->tool)))
		in->tool++;
	in->replacing = in->temp_made = in->temp_open = in->old_deleted = false;
	if (in->tool == TOOLS_FILES && !in->apple_done) {
		apple_begin(in, c);
		return;
	}
	if (in->tool == TOOLS_FILES) {
		/* all in place: Blue's preferences forgotten (the offer comes again when needed), on the disk, then
		 * a restart, as Special > Restart (programs can save their work) */
		if (all_done(in) && in->check.prefs_found && !in->prefs_forgotten) {
			in->prefs_forgotten = true;
			put_name_spec(c->mem, at(R_PREFS), in->vref, in->check.prefs_dir, INSTALL_PREFS_NAME);
			callout(in, c, S_PREFS_FORGET, I_FSP_DELETE, at(R_PREFS), 0, 0, 0);
		} else if (all_done(in)) {
			callout(in, c, S_FLUSH, I_FLUSH_VOL, 0, (uint32_t)(int32_t)in->vref, 0, 0);
		} else {
			report(in, c);
		}
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
static void stop(struct install *in, struct ndrv_call *c, int16_t err, const char *doing)
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
	next_tool(in, c);
}

/* Don't Ask Again couldn't be saved */
static void prefs_stop(struct install *in, struct ndrv_call *c, int16_t err, const char *doing)
{
	in->prefs_error = err;
	in->prefs_doing = doing;
	report(in, c);
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

void install_call(struct install *in, struct ndrv_call *c)
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
		in->error = in->restart_error = in->prefs_error = 0;
		in->restarting = in->prefs_forgotten = false;
		memset(in->result, 0, sizeof in->result);
		in->apple_done = true;          /* (not until the tools are written) */
		memset(in->apple_moved, 0, sizeof in->apple_moved);
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
			stop(in, c, err, "finding the Extensions folder");
			return;
		}
		in->vref = (int16_t)mem_r16(m, at(R_VREF));
		in->ext_dir = mem_r32(m, at(R_DIRID));
		param_block(in, m);
		callout(in, c, S_VOLUME, I_HGET_VINFO, pb, 0, 0, 0);
		return;
	case S_VOLUME:
		if (err)
			stop(in, c, err, "looking at the startup disk");
		else if (mem_r16(m, pb + PB_VATRB) & VOLUME_LOCKED)
			stop(in, c, -44, "looking at the startup disk");
		else if (mem_r32(m, pb + PB_VFNDR_INFO) != in->check.system_dir || mem_r16(m, pb + PB_VDRV_INFO) != in->drive)
			stop(in, c, WRONG_DISK, NULL);
		if (in->error)
			return;
		if (in->job == INSTALL_JOB_NEVER) {
			callout(in, c, S_PREFS_FOLDER, I_FIND_FOLDER, K_ON_SYSTEM_DISK, K_PREFERENCES_FOLDER, 1, at(R_VREF));
			c->out[4] = at(R_DIRID);
			return;
		}
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
		in->apple_done = false;
		next_tool(in, c);
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
			stop(in, c, err, "looking for it");
		} else if (!(mem_r8(m, pb + PB_ATTRIB) & ATTRIB_DIRECTORY) &&
			   mem_r32(m, pb + PB_DIR_ID) == in->check.tool[in->tool].id) {
			in->replacing = true;
			in->dir = in->scan_dir;
			put_fsspec(m, at(R_OLD), in->vref, in->dir, mem_ptr(m, at(R_NAMEBUF)));
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
			stop(in, c, err, "making a file");
			return;
		}
		in->temp_made = true;
		callout(in, c, S_OPEN_DF, I_FSP_OPEN_DF, at(R_TEMP), FS_RD_WR_PERM, at(R_REFNUM), 0);
		return;
	case S_OPEN_DF:
	case S_OPEN_RF:
		if (err) {
			stop(in, c, err, "writing it");
			return;
		}
		in->refnum = (int16_t)mem_r16(m, at(R_REFNUM));
		in->temp_open = true;
		write_fork(in, c, in->step == S_OPEN_RF);
		return;
	case S_WRITE_DF:
	case S_WRITE_RF:
		if (err) {
			stop(in, c, err, "writing it");
			return;
		}
		callout(in, c, in->step == S_WRITE_DF ? S_CLOSE_DF : S_CLOSE_RF, I_FS_CLOSE, (uint32_t)(int32_t)in->refnum,
			0, 0, 0);
		return;
	case S_CLOSE_DF:
		in->temp_open = false;
		if (err) {
			stop(in, c, err, "writing it");
			return;
		}
		callout(in, c, S_OPEN_RF, I_FSP_OPEN_RF, at(R_TEMP), FS_RD_WR_PERM, at(R_REFNUM), 0);
		return;
	case S_CLOSE_RF:
		in->temp_open = false;
		if (err) {
			stop(in, c, err, "writing it");
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
			stop(in, c, err, "writing it");
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
		if (err == FBSY_ERR && in->tool == TOOLS_ENHANCEMENTS && !in->retried) {
			/* running after all (being launched, it hasn't started yet): the installer comes back once it has
			 * quit, and starts over (nothing else is done yet) */
			in->retried = true;
			in->temp_made = false;
			printf("tools: Blue Enhancements is in use: asking it to quit, then trying again\n");
			callout(in, c, S_BUSY, I_FSP_DELETE, at(R_TEMP), 0, 0, 0);
			return;
		}
		if (err) {
			stop(in, c, err, "replacing the old copy");
			return;
		}
		in->old_deleted = true;
		memcpy(mem_ptr(m, at(R_NEWNAME)), mem_ptr(m, at(R_OLD) + 6), 64);    /* (it keeps its name) */
		callout(in, c, S_RENAME, I_FSP_RENAME, at(R_TEMP), at(R_NEWNAME), 0, 0);
		return;
	case S_RENAME: {
		if (err) {
			stop(in, c, err, "naming it");
			return;
		}
		unsigned t = in->tool;
		in->result[t].done = true;
		in->result[t].replaced = in->replacing;
		in->result[t].dir = in->dir;
		memcpy(in->result[t].folder, in->dir == in->ext_dir ? in->ext_name : in->check.tool[t].folder,
		       sizeof in->result[t].folder);
		roman_name(m, at(R_NEWNAME), in->result[t].old_name, sizeof in->result[t].old_name);
		in->temp_made = false;
		in->tool++;
		next_tool(in, c);
		return;
	}
	case S_CLEANUP_CLOSE:
	case S_CLEANUP_DELETE:
		stop(in, c, in->error, in->error_doing);
		return;
	case S_BUSY:
		in->phase = INSTALL_BUSY;
		ndrv_finish(c, 0);
		return;
	case S_DISABLED_FOLDER:
		if (err) {
			for (unsigned k = 0; k < INSTALL_APPLE_GL; k++)
				if (in->check.apple[k].found[0] && in->check.apple[k].dir[0] == in->ext_dir)
					in->apple_moved[k].error = err;
			in->apple = INSTALL_APPLE_GL;
			apple_next(in, c);
			return;
		}
		in->disabled_dir = mem_r32(m, at(R_DIRID));
		param_block(in, m);
		mem_w16(m, pb + PB_DIR_INDEX, 0xFFFF);
		mem_w32(m, pb + PB_DIR_ID, in->disabled_dir);
		callout(in, c, S_DISABLED_NAME, I_GET_CAT_INFO, pb, 0, 0, 0);
		return;
	case S_DISABLED_NAME:
		if (err)
			snprintf(in->disabled_name, sizeof in->disabled_name, DISABLED_NAME);
		else
			roman_name(m, at(R_NAMEBUF), in->disabled_name, sizeof in->disabled_name);
		apple_next(in, c);
		return;
	case S_APPLE_MOVE:
		in->apple_moved[in->apple].moved = !err;
		in->apple_moved[in->apple].error = err;
		in->apple++;
		apple_next(in, c);
		return;
	case S_FLUSH:
		if (err) {
			in->restart_error = err;
			report(in, c);
			return;
		}
		memset(mem_ptr(m, at(R_ADDRESS)), 0, 3 * AE_DESC_SIZE);        /* (null descriptors until made) */
		mem_w32(m, at(R_PROCESS), 0);
		mem_w32(m, at(R_PROCESS) + 4, K_SYSTEM_PROCESS);
		callout(in, c, S_RESTART_ADDRESS, I_AE_CREATE_DESC, TYPE_PROCESS_SERIAL_NUMBER, at(R_PROCESS), 8,
			at(R_ADDRESS));
		return;
	case S_RESTART_ADDRESS:
		if (err) {
			in->restart_error = err;
			report(in, c);
			return;
		}
		callout(in, c, S_RESTART_EVENT, I_AE_CREATE_APPLE_EVENT, K_CORE_EVENT_CLASS, K_AE_RESTART, at(R_ADDRESS),
			AUTO_GENERATE_RETURN_ID);
		c->out[4] = 0;                  /* kAnyTransactionID */
		c->out[5] = at(R_EVENT);
		return;
	case S_RESTART_EVENT:
		if (err) {
			in->restart_error = err;
			callout(in, c, S_DISPOSE_ADDRESS, I_AE_DISPOSE_DESC, at(R_ADDRESS), 0, 0, 0);
			return;
		}
		/* (normal priority, the default timeout, no idle or filter routine) */
		callout(in, c, S_RESTART_SEND, I_AE_SEND, at(R_EVENT), at(R_REPLY),
			AE_NO_REPLY | AE_ALWAYS_INTERACT | AE_CAN_SWITCH_LAYER, 0);
		c->out[4] = AE_DEFAULT_TIMEOUT;
		return;
	case S_RESTART_SEND:
		in->restart_error = err;
		in->restarting = !err;
		callout(in, c, S_DISPOSE_EVENT, I_AE_DISPOSE_DESC, at(R_EVENT), 0, 0, 0);
		return;
	case S_DISPOSE_EVENT:
		callout(in, c, S_DISPOSE_ADDRESS, I_AE_DISPOSE_DESC, at(R_ADDRESS), 0, 0, 0);
		return;
	case S_DISPOSE_ADDRESS:
		report(in, c);
		return;
	case S_PREFS_FORGET:            /* (the tools are in place: a failure here only means Don't Ask Again stays) */
		if (!err || err == FNF_ERR)
			in->check.prefs_found = in->check.never = false;
		next_tool(in, c);
		return;
	case S_PREFS_FOLDER:
		if (err) {
			prefs_stop(in, c, err, "finding the Preferences folder");
			return;
		}
		in->vref = (int16_t)mem_r16(m, at(R_VREF));
		in->prefs_dir = mem_r32(m, at(R_DIRID));
		param_block(in, m);
		mem_w16(m, pb + PB_DIR_INDEX, 0xFFFF);
		mem_w32(m, pb + PB_DIR_ID, in->prefs_dir);
		callout(in, c, S_PREFS_NAME, I_GET_CAT_INFO, pb, 0, 0, 0);
		return;
	case S_PREFS_NAME:
		if (err)
			snprintf(in->prefs_folder, sizeof in->prefs_folder, "Preferences");
		else
			roman_name(m, at(R_NAMEBUF), in->prefs_folder, sizeof in->prefs_folder);
		put_name_spec(m, at(R_PREFS), in->vref, in->prefs_dir, INSTALL_PREFS_NAME);
		callout(in, c, S_PREFS_DELETE, I_FSP_DELETE, at(R_PREFS), 0, 0, 0);    /* (an older one) */
		return;
	case S_PREFS_DELETE:
		if (err && err != FNF_ERR) {
			prefs_stop(in, c, err, "replacing the old " INSTALL_PREFS_NAME);
			return;
		}
		callout(in, c, S_PREFS_CREATE, I_FSP_CREATE, at(R_PREFS), PREFS_CREATOR, PREFS_TYPE, SM_SYSTEM_SCRIPT);
		return;
	case S_PREFS_CREATE:
		if (err) {
			prefs_stop(in, c, err, "making " INSTALL_PREFS_NAME);
			return;
		}
		callout(in, c, S_PREFS_OPEN, I_FSP_OPEN_DF, at(R_PREFS), FS_RD_WR_PERM, at(R_REFNUM), 0);
		return;
	case S_PREFS_OPEN: {
		if (err) {
			prefs_stop(in, c, err, "writing " INSTALL_PREFS_NAME);
			return;
		}
		char text[INSTALL_PREFS_MAX];
		size_t len = install_prefs_make(text, sizeof text, true);
		in->refnum = (int16_t)mem_r16(m, at(R_REFNUM));
		mem_load(m, at(R_BUFFER), text, (uint32_t)len);
		mem_w32(m, at(R_COUNT), (uint32_t)len);
		callout(in, c, S_PREFS_WRITE, I_FS_WRITE, (uint32_t)(int32_t)in->refnum, at(R_COUNT), at(R_BUFFER), 0);
		return;
	}
	case S_PREFS_WRITE:
		in->prefs_error = err;
		in->prefs_doing = "writing " INSTALL_PREFS_NAME;
		callout(in, c, S_PREFS_CLOSE, I_FS_CLOSE, (uint32_t)(int32_t)in->refnum, 0, 0, 0);
		return;
	case S_PREFS_CLOSE:
		if (err || in->prefs_error) {
			prefs_stop(in, c, in->prefs_error ? in->prefs_error : err, "writing " INSTALL_PREFS_NAME);
			return;
		}
		callout(in, c, S_PREFS_FLUSH, I_FLUSH_VOL, 0, (uint32_t)(int32_t)in->vref, 0, 0);
		return;
	case S_PREFS_FLUSH:
		if (err) {
			prefs_stop(in, c, err, "writing " INSTALL_PREFS_NAME);
			return;
		}
		in->check.prefs_found = in->check.never = true;
		in->check.prefs_dir = in->prefs_dir;
		report(in, c);
		return;
	default:
		ndrv_finish(c, 0);
		return;
	}
}

/* What became of Don't Ask Again, said once the installer has finished */
static void report_never(struct install *in, struct ndrv_call *c)
{
	char buf[64];
	const char *const how = " To install them later, choose Install Guest Tools... in Blue's menu (F12).";
	in->news[0] = '\0';
	if (in->error == WRONG_DISK)
		add(in->news, sizeof in->news, "couldn't save Don't Ask Again: Mac OS didn't start up from \"%s\".",
		    in->check.system);
	else if (in->error || in->prefs_error)
		add(in->news, sizeof in->news, "couldn't save Don't Ask Again (%s: %s), so Blue will offer its guest "
		    "tools again next time.", in->error ? in->error_doing : in->prefs_doing,
		    error_text(in->error ? in->error : in->prefs_error, buf, sizeof buf));
	else
		add(in->news, sizeof in->news, "Blue won't offer its guest tools on \"%s\" again (" INSTALL_PREFS_NAME
		    " in \"%s:%s\" says so).%s", in->check.volume, in->check.system, in->prefs_folder, how);
	finish_news(in);
	ndrv_finish(c, 0);
}

/* What was done, said once the installer has finished */
static void report(struct install *in, struct ndrv_call *c)
{
	char *s = in->news, buf[64];
	size_t len = sizeof in->news;
	const struct install_result *r = in->result;
	if (in->job == INSTALL_JOB_NEVER) {
		report_never(in, c);
		return;
	}
	s[0] = '\0';
	/* each tool done, with the others done alike (in the same folder, as they're named) */
	bool said[TOOLS_FILES] = { false };
	for (unsigned t = 0, k = 0; t < TOOLS_FILES; t++) {
		if (!r[t].done || said[t])
			continue;
		bool plain = !strcmp(r[t].old_name, tool_name(t));
		unsigned n = 0;
		for (unsigned u = t; u < TOOLS_FILES; u++)
			n += r[u].done && !said[u] && (u == t || (plain && r[u].replaced == r[t].replaced &&
								  r[u].dir == r[t].dir && !strcmp(r[u].old_name, tool_name(u))));
		add(s, len, "%s%s ", k++ ? " and " : "", r[t].replaced ? "updated" : "installed");
		for (unsigned u = t, j = 0; u < TOOLS_FILES; u++) {
			if (!r[u].done || said[u] || (u != t && (!plain || r[u].replaced != r[t].replaced ||
								 r[u].dir != r[t].dir || strcmp(r[u].old_name, tool_name(u)))))
				continue;
			add(s, len, "%s%s", joint(j++, n), tool_name(u));
			if (!plain)
				add(s, len, " (\"%s\")", r[u].old_name);
			said[u] = true;
		}
		add(s, len, " in \"%s:%s\"", in->check.system, r[t].folder);
	}
	/* Apple's OpenGL libraries */
	unsigned moved = 0;
	for (unsigned k = 0; k < INSTALL_APPLE_GL; k++)
		moved += in->apple_moved[k].moved;
	in->check.apple_kept |= moved && in->apple_restoring;  /* (the user's choice: see install_begin()) */
	if (moved) {
		add(s, len, "%s%s Apple's ", s[0] ? "; " : "", in->apple_restoring ? "put" : "moved");
		for (unsigned k = 0, j = 0; k < INSTALL_APPLE_GL; k++)
			if (in->apple_moved[k].moved)
				add(s, len, "%s%s", joint(j++, moved), apple_names[k]);
		add(s, len, " %s \"%s:%s\"", in->apple_restoring ? "back in" : "to", in->check.system,
		    in->apple_restoring ? in->ext_name : in->disabled_name);
	}
	for (unsigned k = 0; k < INSTALL_APPLE_GL; k++)
		if (in->apple_moved[k].error)
			add(s, len, "%scouldn't move Apple's %s: %s", s[0] ? "; " : "", apple_names[k],
			    error_text(in->apple_moved[k].error, buf, sizeof buf));
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
	if (in->restart_error)
		add(s, len, "%scouldn't restart Mac OS: %s", s[0] ? "; " : "", error_text(in->restart_error, buf, sizeof buf));
	add(s, len, ".");
	const struct install_result *e = &r[TOOLS_ENHANCEMENTS];
	if (e->done && e->dir != in->ext_dir)
		add(s, len, " It stays in \"%s\": Mac OS starts Blue Enhancements only from \"%s\".", e->folder,
		    in->ext_name);
	if (in->restarting)
		add(s, len, " Mac OS is restarting.");
	else if (in->restart_error)
		add(s, len, " Restart Mac OS to use them.");
	else if (in->quit_asked || (e->done && e->dir == in->ext_dir))
		add(s, len, " Blue Enhancements starts with Mac OS next time.");
	finish_news(in);
	ndrv_finish(c, 0);
}
