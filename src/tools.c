#include "tools.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "be.h"
#include "gltest.h"
#include "hfs.h"
#include "opengl.h"
#include "isp.h"
#include "pippin.h"
#include "rave.h"
#include "rsrc.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* 'cfrg' usage */
#define CFRG_LIBRARY 0
#define CFRG_APPLICATION 1

/* The Finder's custom icons (Finder flags, resource ID), and the icons: finder_icons[] */
#define HAS_CUSTOM_ICON 0x0400u
#define IS_INVISIBLE 0x4000u
#define CUSTOM_ICON_ID (-16455)
#define ICON_RESOURCES 6
#include "finder_icons.inc"

/* 'SIZE' flags */
#define SIZE_CAN_BACKGROUND 0x1000u
#define SIZE_ONLY_BACKGROUND 0x0400u
#define SIZE_32_BIT 0x0080u
#define SIZE_HIGH_LEVEL_EVENTS 0x0040u

/* A 'cfrg' resource with one PowerPC member: the whole data fork, of this version. Returns its size. */
static size_t make_cfrg(uint8_t *p, unsigned usage, uint32_t stack, const char *name, uint32_t version)
{
	size_t n = strlen(name), member = (43 + n + 3) & ~(size_t)3;
	memset(p, 0, 32 + member);
	put_be16(p + 10, 1);                    /* version */
	put_be16(p + 30, 1);                    /* members */
	uint8_t *m = p + 32;
	memcpy(m, "pwpc", 4);
	put_be32(m + 8, version);               /* (current; the oldest compatible definition: 0) */
	put_be32(m + 16, usage == CFRG_APPLICATION ? stack : 0);
	m[22] = (uint8_t)usage;
	m[23] = 1;                              /* in the data fork (offset 0, length 0: all of it) */
	put_be16(m + 40, (uint16_t)member);
	m[42] = (uint8_t)n;
	memcpy(m + 43, name, n);
	return 32 + member;
}

/* ---- Blue Enhancements ----------------------------------------------------- */

enum {
	IMP_WAIT_NEXT_EVENT, IMP_GET_MAIN_DEVICE, IMP_DM_SET_DISPLAY_MODE, IMP_V1_COUNT,
	IMP_CALL_UNIVERSAL_PROC = IMP_V1_COUNT, IMP_TEMP_NEW_HANDLE, IMP_HLOCK, IMP_DISPOSE_HANDLE,
	IMP_GET_CURRENT_PROCESS, IMP_GET_FRONT_PROCESS, IMP_SAME_PROCESS, IMP_POST_EVENT, IMP_V2_COUNT,
	IMP_FIND_WINDOW = IMP_V2_COUNT, IMP_TEST_CONTROL, IMP_COUNT
};

static const struct ndrv_import helper_imports[IMP_COUNT] = {
	[IMP_WAIT_NEXT_EVENT] = { "InterfaceLib", "WaitNextEvent" },
	[IMP_GET_MAIN_DEVICE] = { "InterfaceLib", "GetMainDevice" },
	[IMP_DM_SET_DISPLAY_MODE] = { "InterfaceLib", "DMSetDisplayMode" },
	[IMP_CALL_UNIVERSAL_PROC] = { "InterfaceLib", "CallUniversalProc" },
	[IMP_TEMP_NEW_HANDLE] = { "InterfaceLib", "TempNewHandle" },
	[IMP_HLOCK] = { "InterfaceLib", "HLock" },
	[IMP_DISPOSE_HANDLE] = { "InterfaceLib", "DisposeHandle" },
	[IMP_GET_CURRENT_PROCESS] = { "InterfaceLib", "GetCurrentProcess" },
	[IMP_GET_FRONT_PROCESS] = { "InterfaceLib", "GetFrontProcess" },
	[IMP_SAME_PROCESS] = { "InterfaceLib", "SameProcess" },
	[IMP_POST_EVENT] = { "InterfaceLib", "PostEvent" },
	[IMP_FIND_WINDOW] = { "InterfaceLib", "FindWindow" },
	[IMP_TEST_CONTROL] = { "InterfaceLib", "TestControl" },
};

/*
 * Entry points: main, then its patches: two traps, a GetNextEvent filter
 * (the low-memory hook jGNEFilter), which is called by a few instructions of
 * 68k code (see patch()) in each application just before it gets an event,
 * and another trap.
 */
enum { E_MAIN, E_ZERO_SCRAP, E_PUT_SCRAP, E_EVENT_FILTER, E_V2_COUNT, E_TRACK_CONTROL = E_V2_COUNT, E_COUNT };
#define PATCHES (E_COUNT - 1)
#define PATCH(e) ((e) - E_ZERO_SCRAP)
static const struct {
	uint16_t trap;                  /* (0: the filter) */
	uint32_t proc_info;             /* Pascal: the result's size, then each parameter's */
} patches[PATCHES] = {
	{ 0xA9FC, 0x30 },               /* long ZeroScrap(void) */
	{ 0xA9FE, 0xFF0 },              /* long PutScrap(long length, ResType type, const void *source) */
	{ 0, 0xC0 },                    /* void filter(EventRecord *) */
	{ 0xA968, 0xFE0 },              /* short TrackControl(ControlHandle, Point, ControlActionUPP) */
};
#define TOOL_TRAP_TABLE 0x0E00u         /* in low memory: 1024 entries */
#define LM_JGNE_FILTER 0x029Au

/*
 * Its data (after the entry TVectors): what it is and its version (which
 * goes up when the fragment changes, so that one installed by an older
 * version is told apart: see copy_version()), then the rest.
 */
#define HELPER_MAGIC 0x73794870u        /* 'syHp' */
#define HELPER_VERSION 3u
#define DATA_EVENT 8u                   /* EventRecord */
#define DATA_DEPTH 24u                  /* depth mode, in and out of DMSetDisplayMode */
#define DATA_PSN 28u                    /* two ProcessSerialNumbers, */
#define DATA_SAME 44u                   /* ... whether they're the same process */
#define DATA_ERR 46u                    /* TempNewHandle's result code */
#define DATA_ORIGINAL 48u               /* the trap addresses and filter patched over */
#define DATA_DESCRIPTOR (DATA_ORIGINAL + 4u * PATCHES)  /* the patches' routine descriptors */
#define DESCRIPTOR_SIZE 32u
#define DATA_FILTER (DATA_DESCRIPTOR + DESCRIPTOR_SIZE * PATCHES)       /* the filter's 68k code */
#define FILTER_SIZE 24u
#define DATA_WINDOW (DATA_FILTER + FILTER_SIZE) /* FindWindow's window */
#define DATA_TRACKER (DATA_WINDOW + 4u) /* the process calling TrackControl */
#define DATA_SIZE (DATA_TRACKER + 8u)

/* Version 1 (before the clipboard): the first three imports, main only, and this data */
#define V1_DATA_EVENT 0u
#define V1_DATA_DEPTH 16u
#define V1_DATA_SIZE 24u

/* (version 2, before scrolling: the imports and entries before IMP_V2_COUNT and E_V2_COUNT, and the data's start) */
static size_t build_helper(uint8_t *out, size_t size, struct ndrv_layout *layout, unsigned version)
{
	static const uint8_t data[DATA_SIZE] = { 's', 'y', 'H', 'p', 0, 0, 0, HELPER_VERSION };
	static const uint8_t v1_data[V1_DATA_SIZE];
	const struct ndrv_spec spec = {
		.kind = FRAG_APPLICATION,
		.entries = version == 1 ? 1 : version == 2 ? E_V2_COUNT : E_COUNT,
		.hostcall = HOSTCALL_TOOLS_HELPER,
		.imports = helper_imports,
		.import_count = version == 1 ? IMP_V1_COUNT : version == 2 ? IMP_V2_COUNT : IMP_COUNT,
		.extra = version == 1 ? v1_data : data,
		.extra_len = version == 1 ? sizeof v1_data : sizeof data,
	};
	return ndrv_build(&spec, out, size, layout);
}

void helper_init(struct helper *h)
{
	static uint8_t scratch[4096];
	struct ndrv_layout old;
	memset(h, 0, sizeof *h);
	build_helper(scratch, sizeof scratch, &h->layout, HELPER_VERSION);
	build_helper(scratch, sizeof scratch, &old, 1);
	h->v1_extra = old.extra;
	build_helper(scratch, sizeof scratch, &old, 2);
	h->v2_extra = old.extra;
}

#define EVERY_EVENT 0xFFFFu
#define SLEEP_TICKS 10u                 /* a sixth of a second between looks */
#define HIGH_LEVEL_EVENT 23u
#define OS_EVENT 15u
#define RESUME_CONVERT_CLIPBOARD 0x01000003u    /* suspendResumeMessage, resumeFlag, convertClipboardFlag */

/* Steps (state word 0) */
enum {
	STEP_WAITED = 1, STEP_GOT_DEVICE, STEP_SWITCHED, STEP_POSTED,
	STEP_ORIGINAL,                          /* a patch: the routine patched over returned */
	STEP_CURRENT, STEP_FRONT, STEP_SAME,    /* ... the host's text for the scrap: is this the front process? */
	STEP_ALLOCATED, STEP_LOCKED, STEP_ZEROED, STEP_PUT_TEXT, STEP_PUT_UTXT, STEP_DISPOSED,
	STEP_FRONT_ASKED,                       /* main: which process is in front (helper.want_front) */
	STEP_W_CURRENT, STEP_W_FRONT, STEP_W_SAME,      /* the wheel's lines: is this the front process? */
	STEP_W_FOUND,                           /* ... the window under the pointer */
	STEP_W_TESTED,                          /* ... a scroll bar's arrows, each end tested */
	STEP_T_TRACKER,                         /* TrackControl, given the wheel's click: the process calling it */
	STEP_T_ACTION,                          /* ... the program's action procedure called, a line at a time */
};
/* The patches' other state words: which one, and its parameters */
enum { S_ENTRY = 1, S_PARAM };

/* Which version of Blue's the copy making this call is (an older one doesn't have its data where this one has) */
static unsigned copy_version(const struct helper *h, const struct ndrv_call *c)
{
	uint32_t data = c->toc + h->layout.extra, v2 = c->toc + h->v2_extra;
	if (mem_ok(c->mem, data, 8, MEM_R) && mem_r32(c->mem, data) == HELPER_MAGIC &&
	    mem_r32(c->mem, data + 4) >= HELPER_VERSION)
		return HELPER_VERSION;
	if (mem_ok(c->mem, v2, 8, MEM_R) && mem_r32(c->mem, v2) == HELPER_MAGIC && mem_r32(c->mem, v2 + 4) == 2)
		return 2;
	return 1;
}

static bool current(const struct helper *h, const struct ndrv_call *c)
{
	return copy_version(h, c) == HELPER_VERSION;
}

static uint32_t helper_data(const struct helper *h, const struct ndrv_call *c)
{
	unsigned v = copy_version(h, c);
	return c->toc + (v == HELPER_VERSION ? h->layout.extra : v == 2 ? h->v2_extra : h->v1_extra);
}

/* An older copy's: where its event record and depth are (version 2's are where this one's are, in its data) */
static uint32_t old_event(const struct helper *h, const struct ndrv_call *c)
{
	return helper_data(h, c) + (copy_version(h, c) == 1 ? V1_DATA_EVENT : DATA_EVENT);
}

static uint32_t old_depth(const struct helper *h, const struct ndrv_call *c)
{
	return helper_data(h, c) + (copy_version(h, c) == 1 ? V1_DATA_DEPTH : DATA_DEPTH);
}

static bool guest_range(struct mem *m, uint32_t addr, uint32_t len, unsigned need)
{
	if (!len)
		return true;
	if (addr + len - 1 < addr)
		return false;
	for (uint32_t p = addr >> MEM_PAGE_SHIFT; p <= (addr + len - 1) >> MEM_PAGE_SHIFT; p++) {
		if ((m->pageflags[p] & need) != need)
			return false;
		if ((need & MEM_W) && (m->pageflags[p] & MEM_CODE))
			mem_invalidate(m, p << MEM_PAGE_SHIFT, MEM_PAGE_SIZE);
	}
	return true;
}

static void wait_next_event(struct helper *h, struct ndrv_call *c)
{
	ndrv_set_state(c, 0, STEP_WAITED);
	ndrv_callout(c, ndrv_import(c, &h->layout, IMP_WAIT_NEXT_EVENT), EVERY_EVENT, old_event(h, c), SLEEP_TICKS, 0);
}

/*
 * The patches go straight into the trap table and jGNEFilter, as an
 * extension's do at startup: the Process Manager takes those an application
 * makes with NSetTrapAddress for its own, in force only while it runs. Each
 * is a routine descriptor for one of our entry points (the routine patched
 * over is called with CallUniversalProc); the filter's 68k code calls its
 * descriptor with the event record (in A1), then the filter before it.
 */
static bool patch(struct helper *h, struct ndrv_call *c, bool on)
{
	struct mem *m = c->mem;
	uint32_t data = helper_data(h, c);
	if (!guest_range(m, 0, TOOL_TRAP_TABLE + 4 * 1024, MEM_R | MEM_W) ||
	    !guest_range(m, data + DATA_ORIGINAL, DATA_SIZE - DATA_ORIGINAL, MEM_R | MEM_W))
		return false;
	for (unsigned k = 0; k < PATCHES; k++) {
		uint32_t at = patches[k].trap ? TOOL_TRAP_TABLE + 4 * (patches[k].trap & 0x3FFu) : LM_JGNE_FILTER;
		uint32_t d = data + DATA_DESCRIPTOR + DESCRIPTOR_SIZE * k;
		uint32_t ours = patches[k].trap ? d : data + DATA_FILTER;
		if (!on) {
			/* (unless something has patched over this since) */
			if (mem_r32(m, at) == ours)
				mem_w32(m, at, mem_r32(m, data + DATA_ORIGINAL + 4 * k));
			continue;
		}
		/* a RoutineDescriptor: _MixedModeMagic, version 7, one PowerPC routine, our entry's TVector */
		memset(mem_ptr(m, d), 0, DESCRIPTOR_SIZE);
		mem_w16(m, d, 0xAAFE);
		mem_w8(m, d + 2, 7);
		mem_w32(m, d + 12, patches[k].proc_info);
		mem_w8(m, d + 17, 1);
		mem_w32(m, d + 20, c->toc + h->layout.entry[E_ZERO_SCRAP + k]);
		uint32_t before = mem_r32(m, at);
		mem_w32(m, data + DATA_ORIGINAL + 4 * k, before);
		if (!patches[k].trap) {
			static const uint8_t filter[FILTER_SIZE] = {
				0x48, 0xE7, 0xE0, 0xC0,         /* movem.l d0-d2/a0-a1,-(sp) */
				0x2F, 0x09,                     /* move.l a1,-(sp) */
				0x4E, 0xB9, 0, 0, 0, 0,         /* jsr descriptor */
				0x4C, 0xDF, 0x03, 0x07,         /* movem.l (sp)+,d0-d2/a0-a1 */
				0x2F, 0x3C, 0, 0, 0, 0,         /* move.l #before,-(sp) */
				0x4E, 0x75,                     /* rts (to it) */
			};
			uint8_t *f = mem_ptr(m, data + DATA_FILTER);
			memcpy(f, filter, sizeof filter);
			put_be32(f + 8, d);
			if (before)
				put_be32(f + 18, before);
			else
				put_be16(f + 16, 0x4E75);       /* rts (none before) */
		}
		mem_w32(m, at, ours);
	}
	return true;
}

/*
 * main(): an older copy gets on with what it knows, at its own data's
 * addresses. This one patches the Scrap Manager, TrackControl and filters
 * events, for every application, if there's a clipboard to share or a
 * wheel to scroll with.
 */
static void helper_main(struct helper *h, struct ndrv_call *c, struct clip *clip, struct wheel *w)
{
	h->running = true;
	h->launches++;
	unsigned v = copy_version(h, c);
	if (v != HELPER_VERSION) {
		h->old = true;
		printf("enhancements: the guest's Blue Enhancements is older than this version of Blue (no %s): "
		       "update it (Blue offers to, in a window), or by hand from a volume made with \"blue tools create\"\n",
		       v == 1 ? "clipboard or scrolling" : "scrolling");
		fflush(stdout);
	} else if ((clip->enabled || w->enabled) && !h->patched && patch(h, c, true)) {
		h->patched = true;
		h->patched_toc = c->toc;
		clip->listening = clip->enabled;
	}
	wait_next_event(h, c);
}

/* A patch's call to the routine it patched over, with its parameters (the filter just returns) */
static void call_original(struct helper *h, struct ndrv_call *c)
{
	unsigned k = PATCH(ndrv_state(c, S_ENTRY));
	if (!patches[k].trap) {
		ndrv_finish(c, 0);
		return;
	}
	ndrv_set_state(c, 0, STEP_ORIGINAL);
	ndrv_callout(c, ndrv_import(c, &h->layout, IMP_CALL_UNIVERSAL_PROC),
		     mem_r32(c->mem, helper_data(h, c) + DATA_ORIGINAL + 4 * k), patches[k].proc_info,
		     ndrv_state(c, S_PARAM), ndrv_state(c, S_PARAM + 1));
	c->out[4] = ndrv_state(c, S_PARAM + 2);
}

/* ... and to ZeroScrap's or PutScrap's (to put the host's text in the scrap) */
static void call_scrap(struct helper *h, struct ndrv_call *c, unsigned entry, uint32_t len, uint32_t type,
		       uint32_t from)
{
	unsigned k = PATCH(entry);
	ndrv_callout(c, ndrv_import(c, &h->layout, IMP_CALL_UNIVERSAL_PROC),
		     mem_r32(c->mem, helper_data(h, c) + DATA_ORIGINAL + 4 * k), patches[k].proc_info, len, type);
	c->out[4] = from;
}

static bool wheel_event(struct helper *h, struct ndrv_call *c, struct wheel *w);

/*
 * The patches. ZeroScrap and PutScrap tell the host what applications put
 * in the scrap. Each application has a scrap of its own (which the Process
 * Manager hands on when another comes to the front), and some read it in
 * low memory rather than with GetScrap: so the host's newer text, if any,
 * goes into the front application's scrap before it gets its next event.
 * The wheel's lines go to the front application then too, as a click on a
 * scroll bar's arrow, which TrackControl's patch holds down for them (see
 * wheel_event()).
 */
static void patch_call(struct helper *h, struct ndrv_call *c, struct clip *clip, struct wheel *w)
{
	h->scrap_calls += c->entry == E_ZERO_SCRAP || c->entry == E_PUT_SCRAP;
	ndrv_set_state(c, S_ENTRY, c->entry);
	for (unsigned k = 0; k < 3; k++)
		ndrv_set_state(c, S_PARAM + k, c->arg[k]);
	if (c->entry == E_ZERO_SCRAP)
		clip_mac_zero(clip);
	if (c->entry == E_EVENT_FILTER && wheel_event(h, c, w))
		return;
	if (c->entry == E_EVENT_FILTER && clip->pending && !h->delivering && !h->scrolling) {
		h->delivering = true;
		ndrv_set_state(c, 0, STEP_CURRENT);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_GET_CURRENT_PROCESS), helper_data(h, c) + DATA_PSN,
			     0, 0, 0);
		return;
	}
	if (c->entry == E_TRACK_CONTROL && h->click.armed && c->arg[0] == h->click.control) {
		ndrv_set_state(c, 0, STEP_T_TRACKER);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_GET_CURRENT_PROCESS), helper_data(h, c) + DATA_TRACKER,
			     0, 0, 0);
		return;
	}
	call_original(h, c);
}

/* Putting the host's text in the scrap: a step after a callout */
static void deliver(struct helper *h, struct ndrv_call *c, struct clip *clip, uint32_t result)
{
	struct mem *m = c->mem;
	uint32_t data = helper_data(h, c), step = ndrv_state(c, 0), at = 0;
	if (step >= STEP_LOCKED && step <= STEP_PUT_TEXT && mem_ok(m, h->handle, 4, MEM_R))
		at = mem_r32(m, h->handle);
	/* (the host's text may change meanwhile: then this one is dropped, and the next look puts that) */
	bool same_text = clip->pending && clip->serial == h->serial;
	switch (step) {
	case STEP_CURRENT:
		ndrv_set_state(c, 0, STEP_FRONT);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_GET_FRONT_PROCESS), data + DATA_PSN + 8, 0, 0, 0);
		return;
	case STEP_FRONT:
		ndrv_set_state(c, 0, STEP_SAME);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_SAME_PROCESS), data + DATA_PSN, data + DATA_PSN + 8,
			     data + DATA_SAME, 0);
		return;
	case STEP_SAME:
		if (result || !mem_r8(m, data + DATA_SAME) || !clip->pending)
			break;
		h->serial = clip->serial;
		h->size = (uint32_t)(clip->text_len > clip->utxt_len ? clip->text_len : clip->utxt_len);
		ndrv_set_state(c, 0, STEP_ALLOCATED);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_TEMP_NEW_HANDLE), h->size ? h->size : 1, data + DATA_ERR,
			     0, 0);
		return;
	case STEP_ALLOCATED:
		if (!result) {
			printf("clipboard: no memory for the host's text (%u KB) in Mac OS\n", h->size >> 10);
			fflush(stdout);
			clip->pending = false;
			break;
		}
		h->handle = result;
		ndrv_set_state(c, 0, STEP_LOCKED);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_HLOCK), result, 0, 0, 0);
		return;
	case STEP_LOCKED:
		if (!same_text || !at || !guest_range(m, at, h->size, MEM_W))
			goto dispose;
		ndrv_set_state(c, 0, STEP_ZEROED);
		call_scrap(h, c, E_ZERO_SCRAP, 0, 0, 0);
		return;
	case STEP_ZEROED:
		if (!same_text)
			goto dispose;
		memcpy(mem_ptr(m, at), clip->text, clip->text_len);
		ndrv_set_state(c, 0, STEP_PUT_TEXT);
		call_scrap(h, c, E_PUT_SCRAP, (uint32_t)clip->text_len, CLIP_TEXT, at);
		return;
	case STEP_PUT_TEXT:
		if (result) {
			printf("clipboard: Mac OS has no room for the host's text (error %d)\n", (int)(int32_t)result);
			fflush(stdout);
			clip->pending = false;
			goto dispose;
		}
		if (!same_text)
			goto dispose;
		clip_delivered(clip);
		memcpy(mem_ptr(m, at), clip->utxt, clip->utxt_len);
		ndrv_set_state(c, 0, STEP_PUT_UTXT);
		call_scrap(h, c, E_PUT_SCRAP, (uint32_t)clip->utxt_len, CLIP_UTXT, at);
		return;
	case STEP_PUT_UTXT:
	dispose:
		ndrv_set_state(c, 0, STEP_DISPOSED);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_DISPOSE_HANDLE), h->handle, 0, 0, 0);
		return;
	default:
		break;
	}
	h->delivering = false;
	call_original(h, c);
}

/* ---- the wheel -------------------------------------------------------------- */

/*
 * The wheel's lines scroll the window under the pointer, in the program in
 * front, as period tools did it (USB Overdrive's, say): by its scroll bar
 * (up and down, or across for a wheel turned sideways), as a click on the
 * arrow at one end would. A key wakes the program (WHEEL_WAKE_KEY); as it
 * gets that event (or a null event) the filter finds the window
 * (FindWindow), its scroll bars that way, and the arrows' parts at their
 * ends (TestControl), and makes the event a mouse-down on the arrow; the
 * program's own handling of it calls TrackControl, whose patch, seeing
 * the click, calls the action procedure the program gives it once for each
 * line (as holding the button down on the arrow would) instead of tracking
 * the mouse, and says the arrow was clicked. The program's procedure (its
 * own, or the control's) runs in its own click handling, then, with
 * whatever that sets up. A program with no action procedure gets a line a
 * click. Without a scroll bar under the pointer the lines are the arrow
 * keys (wheel.h); a scroll bar with nowhere to go takes them.
 */
#define LM_MOUSE 0x0830u                /* the pointer, in global coordinates (v, h) */
#define LM_WINDOW_LIST 0x09D6u          /* the current application's windows, front first */
#define LM_CUR_AP_NAME 0x0910u
#define NULL_EVENT 0
#define MOUSE_DOWN 1
#define KEY_DOWN 3
#define KEY_UP 4
#define IN_CONTENT 3                    /* FindWindow's */
#define UP_BUTTON 20                    /* kControlUpButtonPart (a horizontal scroll bar's left arrow) */
#define DOWN_BUTTON 21
#define ACTION_PROC_INFO 0x2C0u         /* pascal void (ControlHandle, short) */
#define NO_PROC(p) ((p) == 0 || (p) == 0xFFFFFFFFu)     /* (TrackControl's: none, or the control's own) */
/* (EventRecord, WindowRecord, CGrafPort, PixMap and ControlRecord) */
#define EV_MESSAGE 2
#define EV_WHERE 10
#define EV_MODIFIERS 14
#define W_VISIBLE 110
#define W_HILITED 111
#define W_CONTROLS 140
#define W_NEXT 144
#define PORT_PIXMAP 2
#define PORT_VERSION 6
#define PORT_BOUNDS 8
#define PIXMAP_BOUNDS 6
#define C_NEXT 0
#define C_RECT 8
#define C_VISIBLE 16
#define C_HILITE 17
#define C_MIN 20
#define C_MAX 22
#define C_ACTION 32

static int16_t r16s(struct mem *m, uint32_t at)
{
	return (int16_t)mem_r16(m, at);
}

static void scroll_log(const struct helper *h, struct mem *m, const char *fmt, ...)
{
	if (!h->log_wheel)
		return;
	const uint8_t *name = mem_ptr(m, LM_CUR_AP_NAME);
	printf("wheel: %.*s: ", name[0] < 31 ? name[0] : 31, name + 1);
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static const char *way(unsigned axis, int32_t lines)
{
	return axis ? (lines > 0 ? "up" : "down") : (lines > 0 ? "right" : "left");
}

/*
 * The program's active window (in its list, visible and highlighted, as
 * the one in front is): where its port's coordinates are from the screen's
 */
static bool scroll_window(struct mem *m, uint32_t window, int16_t *dv, int16_t *dh)
{
	uint32_t w = mem_ok(m, LM_WINDOW_LIST, 4, MEM_R) ? mem_r32(m, LM_WINDOW_LIST) : 0;
	for (unsigned n = 0; w && w != window && n < 256 && mem_ok(m, w, W_NEXT + 4, MEM_R); n++)
		w = mem_r32(m, w + W_NEXT);
	if (!w || w != window || !mem_ok(m, w, W_NEXT + 4, MEM_R) || !mem_r8(m, w + W_VISIBLE) ||
	    !mem_r8(m, w + W_HILITED))
		return false;
	uint32_t bounds = w + PORT_BOUNDS;
	if ((mem_r16(m, w + PORT_VERSION) & 0xC000) == 0xC000) {        /* (a CGrafPort: its PixMap's) */
		uint32_t pm = mem_r32(m, w + PORT_PIXMAP);
		if (!mem_ok(m, pm, 4, MEM_R) || !mem_ok(m, mem_r32(m, pm), PIXMAP_BOUNDS + 8, MEM_R))
			return false;
		bounds = mem_r32(m, pm) + PIXMAP_BOUNDS;
	}
	*dv = r16s(m, bounds);
	*dh = r16s(m, bounds + 2);
	return true;
}

/*
 * The window's scroll bars one way (axis 1: up and down) that might be the
 * one to scroll, by shape (long, and as wide as a scroll bar is: Mac OS's
 * are 16, Appearance's small ones 11) and with somewhere to scroll to: the
 * one under the pointer first, then those beside it (to its right first),
 * then the rest; their arrows are tested later. Returns how many there are
 * (`shaped`: whether there were any of that shape, scrollable or not).
 */
static unsigned scroll_bars(struct helper *h, struct mem *m, uint32_t window, int16_t v, int16_t x, unsigned axis)
{
	int32_t score[HELPER_BARS];
	unsigned n = 0;
	h->shaped = false;
	uint32_t handle = mem_r32(m, window + W_CONTROLS);
	for (unsigned k = 0; handle && k < 128 && mem_ok(m, handle, 4, MEM_R); k++) {
		uint32_t ctl = mem_r32(m, handle);
		if (!mem_ok(m, ctl, C_ACTION + 4, MEM_R))
			break;
		int32_t top = r16s(m, ctl + C_RECT), left = r16s(m, ctl + C_RECT + 2);
		int32_t bottom = r16s(m, ctl + C_RECT + 4), right = r16s(m, ctl + C_RECT + 6);
		int32_t across = axis ? right - left : bottom - top, along = axis ? bottom - top : right - left;
		int32_t at = axis ? v : x, beside = axis ? x : v;
		int32_t from = axis ? top : left, to = axis ? bottom : right, side = axis ? left : top;
		int32_t far = axis ? right : bottom;
		uint32_t next = mem_r32(m, ctl + C_NEXT);
		if (!mem_r8(m, ctl + C_VISIBLE) || across < 10 || across > 24 || along < 2 * across) {
			handle = next;
			continue;
		}
		h->shaped = true;
		if (mem_r8(m, ctl + C_HILITE) == 255 || r16s(m, ctl + C_MAX) <= r16s(m, ctl + C_MIN)) {
			handle = next;
			continue;
		}
		int32_t s = at >= from && at < to ? (beside >= side && beside < far ? 0 :
						     beside < side ? 1 + side - beside : 20000 + beside - far) :
						    40000 + (at < from ? from - at : at - to);
		unsigned j = n < HELPER_BARS ? n++ : HELPER_BARS;
		while (j > 0 && score[j - 1] > s) {
			if (j < HELPER_BARS) {
				score[j] = score[j - 1];
				h->bar[j] = h->bar[j - 1];
			}
			j--;
		}
		if (j < HELPER_BARS) {
			score[j] = s;
			h->bar[j] = handle;
		}
		handle = next;
	}
	return n;
}

/*
 * Where a scroll bar's arrow might be, in its window's coordinates (v, h):
 * the up (or left) one at the start, or else the second from the end, as
 * Mac OS 9's Smart Scrolling puts them both together; the down one at the
 * end, or else the second from the start. An arrow is as long as the bar
 * is wide.
 */
#define ARROW_PLACES 2

static uint32_t arrow_point(struct mem *m, uint32_t handle, unsigned axis, unsigned part, unsigned place)
{
	uint32_t ctl = mem_r32(m, handle);
	int32_t top = r16s(m, ctl + C_RECT), left = r16s(m, ctl + C_RECT + 2);
	int32_t bottom = r16s(m, ctl + C_RECT + 4), right = r16s(m, ctl + C_RECT + 6);
	int32_t from = axis ? top : left, to = axis ? bottom : right, across = axis ? right - left : bottom - top;
	bool end = (part == DOWN_BUTTON) == (place == 0);
	int32_t at = end ? to - across / 2 - (place ? across : 0) : from + across / 2 + (place ? across : 0);
	int32_t mid = axis ? (left + right) / 2 : (top + bottom) / 2;
	return (uint32_t)(uint16_t)(axis ? at : mid) << 16 | (uint16_t)(axis ? mid : at);
}

/* Done looking: the event goes on to the program (as it is, or as the click) */
static void scroll_finish(struct helper *h, struct ndrv_call *c)
{
	h->scrolling = false;
	call_original(h, c);
}

static void scroll_axis(struct helper *h, struct ndrv_call *c, struct wheel *w);

/* The arrow's next place, or the next scroll bar's first, to be tested (or, with none left, what becomes of the lines) */
static void scroll_test(struct helper *h, struct ndrv_call *c, struct wheel *w)
{
	if (h->bar_at >= h->bars) {
		int32_t lines = h->lines[h->axis];
		/* (a scroll bar with nowhere to go: nothing; no scroll bar: the arrow keys) */
		if (h->shaped) {
			scroll_log(h, c->mem, "%d lines %s: nowhere to scroll", lines < 0 ? -lines : lines, way(h->axis, lines));
		} else {
			scroll_log(h, c->mem, "%d lines %s: no scroll bar under the pointer (the arrow keys)",
				   lines < 0 ? -lines : lines, way(h->axis, lines));
			wheel_unscrolled(w, h->axis ? 0 : lines, h->axis ? lines : 0);
		}
		h->lines[h->axis] = 0;
		scroll_axis(h, c, w);
		return;
	}
	if (h->place == ARROW_PLACES) {
		h->place = 0;
		h->bar_at++;
		scroll_test(h, c, w);
		return;
	}
	ndrv_set_state(c, 0, STEP_W_TESTED);
	ndrv_callout(c, ndrv_import(c, &h->layout, IMP_TEST_CONTROL), h->bar[h->bar_at],
		     arrow_point(c->mem, h->bar[h->bar_at], h->axis, h->part, h->place), 0, 0);
}

/* The next way to scroll (up and down first), or done */
static void scroll_axis(struct helper *h, struct ndrv_call *c, struct wheel *w)
{
	struct mem *m = c->mem;
	h->axis = h->lines[1] ? 1 : 0;
	if (!h->lines[h->axis]) {
		scroll_finish(h, c);
		return;
	}
	int16_t dv, dh;
	if (!scroll_window(m, h->window, &dv, &dh)) {
		scroll_log(h, m, "the window under the pointer isn't the one in front: nowhere to scroll");
		h->lines[0] = h->lines[1] = 0;
		scroll_finish(h, c);
		return;
	}
	h->bars = scroll_bars(h, m, h->window, (int16_t)((int16_t)(h->point >> 16) + dv),
			      (int16_t)((int16_t)h->point + dh), h->axis);
	h->bar_at = 0;
	h->place = 0;
	h->part = (h->lines[h->axis] > 0) == (h->axis == 1) ? UP_BUTTON : DOWN_BUTTON;
	scroll_test(h, c, w);
}

/*
 * A scroll bar found: the event becomes a click on the arrow (in the
 * screen's coordinates), which TrackControl's patch takes; the other way's
 * lines wait for the next
 */
static void scroll_click(struct helper *h, struct ndrv_call *c, struct wheel *w)
{
	struct mem *m = c->mem;
	uint32_t handle = h->bar[h->bar_at], ctl = mem_r32(m, handle), data = helper_data(h, c);
	int32_t lines = h->lines[h->axis];
	int16_t dv, dh;
	scroll_window(m, h->window, &dv, &dh);
	h->click = (struct helper_click){
		.armed = true,
		.psn = { mem_r32(m, data + DATA_PSN), mem_r32(m, data + DATA_PSN + 4) },
		.control = handle,
		.part = h->part,
		.axis = h->axis,
		.lines = lines,
		.again = h->retry,
	};
	h->retry = false;
	uint32_t at = arrow_point(m, handle, h->axis, h->part, h->place);
	mem_w16(m, h->event, MOUSE_DOWN);
	mem_w32(m, h->event + EV_MESSAGE, 0);
	mem_w16(m, h->event + EV_WHERE, (uint16_t)((int16_t)(at >> 16) - dv));
	mem_w16(m, h->event + EV_WHERE + 2, (uint16_t)((int16_t)at - dh));
	mem_w16(m, h->event + EV_MODIFIERS, 0);         /* (the button down, no keys) */
	scroll_log(h, m, "%d lines %s: a click on the scroll bar (%d,%d,%d,%d)'s %s arrow", lines < 0 ? -lines : lines,
		   way(h->axis, lines), r16s(m, ctl + C_RECT), r16s(m, ctl + C_RECT + 2), r16s(m, ctl + C_RECT + 4),
		   r16s(m, ctl + C_RECT + 6), h->click.part == UP_BUTTON ? (h->axis ? "up" : "left") :
		   (h->axis ? "down" : "right"));
	h->lines[h->axis] = 0;
	wheel_again(w, h->lines[0], h->lines[1]);
	h->lines[0] = h->lines[1] = 0;
	scroll_finish(h, c);
}

/*
 * The filter: the key that woke the program for the wheel is taken away
 * (a null event, then); a click the program didn't take to TrackControl is
 * forgotten (its lines tried once more); and lines waiting go to the
 * program in front, in a null event's place. True if it's on that.
 */
static bool wheel_event(struct helper *h, struct ndrv_call *c, struct wheel *w)
{
	struct mem *m = c->mem;
	uint32_t ev = c->arg[0];
	if (!mem_ok(m, ev, 16, MEM_RW))
		return false;
	uint16_t what = mem_r16(m, ev);
	if ((what == KEY_DOWN || what == KEY_UP) && mem_r8(m, ev + EV_MESSAGE + 2) == WHEEL_WAKE_KEY) {
		if (what == KEY_DOWN)
			wheel_woken(w);
		mem_w16(m, ev, NULL_EVENT);
		mem_w32(m, ev + EV_MESSAGE, 0);
	}
	if (h->click.armed && !h->scrolling) {
		h->click.armed = false;
		scroll_log(h, m, "the program didn't track its click on the scroll bar%s",
			   h->click.again ? "" : " (once more)");
		if (!h->click.again) {
			wheel_again(w, h->click.axis ? 0 : h->click.lines, h->click.axis ? h->click.lines : 0);
			h->retry = true;
		}
	}
	if (mem_r16(m, ev) != NULL_EVENT || (!w->lines[0] && !w->lines[1]) || h->delivering || h->scrolling)
		return false;
	h->scrolling = true;
	h->event = ev;
	ndrv_set_state(c, 0, STEP_W_CURRENT);
	ndrv_callout(c, ndrv_import(c, &h->layout, IMP_GET_CURRENT_PROCESS), helper_data(h, c) + DATA_PSN, 0, 0, 0);
	return true;
}

/* Scrolling: a step after a callout */
static void scroll(struct helper *h, struct ndrv_call *c, struct wheel *w, uint32_t result)
{
	struct mem *m = c->mem;
	uint32_t data = helper_data(h, c), step = ndrv_state(c, 0);
	switch (step) {
	case STEP_W_CURRENT:
		ndrv_set_state(c, 0, STEP_W_FRONT);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_GET_FRONT_PROCESS), data + DATA_PSN + 8, 0, 0, 0);
		return;
	case STEP_W_FRONT:
		ndrv_set_state(c, 0, STEP_W_SAME);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_SAME_PROCESS), data + DATA_PSN, data + DATA_PSN + 8,
			     data + DATA_SAME, 0);
		return;
	case STEP_W_SAME:
		/* (another program's: the lines wait for the one in front, which gets events too) */
		if (result || !mem_r8(m, data + DATA_SAME) || !mem_ok(m, LM_MOUSE, 4, MEM_R) ||
		    !wheel_take(w, WHEEL_MAX, &h->lines[0], &h->lines[1], c->tb)) {
			scroll_finish(h, c);
			return;
		}
		h->point = mem_r32(m, LM_MOUSE);
		mem_w32(m, data + DATA_WINDOW, 0);
		ndrv_set_state(c, 0, STEP_W_FOUND);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_FIND_WINDOW), h->point, data + DATA_WINDOW, 0, 0);
		return;
	case STEP_W_FOUND:
		h->window = mem_r32(m, data + DATA_WINDOW);
		if ((int16_t)result != IN_CONTENT || !h->window) {
			scroll_log(h, m, "the pointer isn't in a window's contents (FindWindow: %d): the arrow keys",
				   (int16_t)result);
			wheel_unscrolled(w, h->lines[0], h->lines[1]);
			h->lines[0] = h->lines[1] = 0;
			scroll_finish(h, c);
			return;
		}
		scroll_axis(h, c, w);
		return;
	case STEP_W_TESTED:
		if ((int16_t)result != (int16_t)h->part) {
			h->place++;
			scroll_test(h, c, w);
			return;
		}
		scroll_click(h, c, w);
		return;
	default:
		scroll_finish(h, c);
		return;
	}
}

/*
 * TrackControl, called with the wheel's click on the control (in the
 * program it went to): the arrow held down for the lines, the program's
 * action procedure (or the control's) called for each, and the arrow said
 * to have been clicked; without a procedure, a line (the rest wait for
 * another click)
 */
static void track_click(struct helper *h, struct ndrv_call *c, struct wheel *w, uint32_t result)
{
	struct mem *m = c->mem;
	uint32_t data = helper_data(h, c), step = ndrv_state(c, 0);
	struct helper_click *k = &h->click;
	if (step == STEP_T_TRACKER) {
		if (result || !k->armed || mem_r32(m, data + DATA_TRACKER) != k->psn[0] ||
		    mem_r32(m, data + DATA_TRACKER + 4) != k->psn[1]) {
			call_original(h, c);
			return;
		}
		k->armed = false;
		uint32_t proc = ndrv_state(c, S_PARAM + 2);
		if (proc == 0xFFFFFFFFu && mem_ok(m, mem_r32(m, k->control), C_ACTION + 4, MEM_R))
			proc = mem_r32(m, mem_r32(m, k->control) + C_ACTION);
		k->proc = NO_PROC(proc) ? 0 : proc;
		k->count = (unsigned)(k->lines < 0 ? -k->lines : k->lines);
		if (!k->proc) {
			int32_t rest = k->lines > 0 ? k->lines - 1 : k->lines + 1;
			wheel_again(w, k->axis ? 0 : rest, k->axis ? rest : 0);
			k->count = 1;
		}
		w->scrolled += k->count;
		scroll_log(h, m, "TrackControl: %u %s, %s", k->count, k->count == 1 ? "line" : "lines",
			   k->proc ? "by the program's action procedure" : "by the program (no action procedure)");
		if (!k->proc) {
			ndrv_finish(c, k->part);
			return;
		}
	}
	if (k->count) {
		k->count--;
		ndrv_set_state(c, 0, STEP_T_ACTION);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_CALL_UNIVERSAL_PROC), k->proc, ACTION_PROC_INFO,
			     k->control, k->part);
		return;
	}
	ndrv_finish(c, k->part);
}

static void look(struct helper *h, struct ndrv_call *c, struct video *v, struct clip *clip, bool old)
{
	uint32_t mode, depth;
	if (!old && h->patched && clip_telling(clip)) {
		/* a resume event for the front application, saying the clipboard has changed */
		clip->told = clip->serial;
		ndrv_set_state(c, 0, STEP_POSTED);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_POST_EVENT), OS_EVENT, RESUME_CONVERT_CLIPBOARD, 0, 0);
		return;
	}
	if (video_take_request(v, &mode, &depth)) {
		ndrv_set_state(c, 1, mode);
		ndrv_set_state(c, 2, depth);
		ndrv_set_state(c, 0, STEP_GOT_DEVICE);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_GET_MAIN_DEVICE), 0, 0, 0, 0);
		return;
	}
	wait_next_event(h, c);
}

void helper_call(struct helper *h, struct ndrv_call *c, struct video *v, struct clip *clip, struct wheel *w)
{
	struct mem *m = c->mem;
	uint32_t data = helper_data(h, c);
	bool old = !current(h, c);

	if (c->entry == E_MAIN) {
		helper_main(h, c, clip, w);
		return;
	}
	if (c->entry != NDRV_ENTRY_RESUME) {
		if (c->entry < E_COUNT && !old)
			patch_call(h, c, clip, w);
		else
			ndrv_finish(c, 0);
		return;
	}
	uint32_t result = c->arg[0];            /* resuming after a callout */
	uint32_t step = ndrv_state(c, 0);
	switch (step) {
	case STEP_WAITED: {
		uint32_t ev = old_event(h, c);
		if (h->quit || (mem_r16(m, ev) == HIGH_LEVEL_EVENT && mem_r32(m, ev + 2) == FOURCC('a', 'e', 'v', 't') &&
				mem_r32(m, ev + 10) == FOURCC('q', 'u', 'i', 't'))) {
			/* the Process Manager asks us to quit (shutting down), or Blue does (to replace us):
			 * the patches go first */
			h->quit = false;
			if (h->patched && c->toc == h->patched_toc) {
				patch(h, c, false);
				h->patched = false;
				clip->listening = false;
			}
			h->running = false;
			h->quits++;
			ndrv_finish(c, 0);
			return;
		}
		if (!old && h->want_front) {
			/* (into the second ProcessSerialNumber's place: the scrap's delivery uses it only between
			 * callouts that let no other process run, as this one doesn't) */
			ndrv_set_state(c, 0, STEP_FRONT_ASKED);
			ndrv_callout(c, ndrv_import(c, &h->layout, IMP_GET_FRONT_PROCESS), data + DATA_PSN + 8, 0, 0, 0);
			return;
		}
		look(h, c, v, clip, old);
		return;
	}
	case STEP_FRONT_ASKED:
		h->front_known = result == 0;
		h->front[0] = mem_r32(m, data + DATA_PSN + 8);
		h->front[1] = mem_r32(m, data + DATA_PSN + 12);
		look(h, c, v, clip, old);
		return;
	case STEP_GOT_DEVICE: {
		/* DMSetDisplayMode(device, mode, &depthMode, reserved, displayState) */
		uint32_t depth_at = old_depth(h, c);
		mem_w32(m, depth_at, ndrv_state(c, 2));
		ndrv_set_state(c, 0, STEP_SWITCHED);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_DM_SET_DISPLAY_MODE), result, ndrv_state(c, 1),
			     depth_at, 0);
		c->out[4] = 0;
		return;
	}
	case STEP_SWITCHED:
		video_request_done(v);
		if (result) {
			h->failures++;
			h->last_error = (int32_t)(int16_t)result;
		} else {
			h->switches++;
		}
		wait_next_event(h, c);
		return;
	case STEP_POSTED:
		wait_next_event(h, c);
		return;
	case STEP_ORIGINAL: {
		uint32_t len = ndrv_state(c, S_PARAM), from = ndrv_state(c, S_PARAM + 2);
		if (ndrv_state(c, S_ENTRY) == E_PUT_SCRAP && result == 0 &&
		    (len > CLIP_MAX_BYTES || guest_range(m, from, len, MEM_R)))
			clip_mac_put(clip, ndrv_state(c, S_PARAM + 1), mem_ptr(m, from), len);
		ndrv_finish(c, result);
		return;
	}
	case STEP_T_TRACKER:
	case STEP_T_ACTION:
		track_click(h, c, w, result);
		return;
	default:
		if (step >= STEP_CURRENT && step <= STEP_DISPOSED) {
			deliver(h, c, clip, result);
			return;
		}
		if (step >= STEP_W_CURRENT && step <= STEP_W_TESTED) {
			scroll(h, c, w, result);
			return;
		}
		ndrv_finish(c, 0);
		return;
	}
}

/* ---- the volume -------------------------------------------------------------- */

/* A Finder icon (FINDER_ICON_*) as a file's custom icon, its resources in r[0..ICON_RESOURCES-1] */
static void icon_resources(struct rsrc *r, unsigned icon)
{
	const struct finder_icon *f = &finder_icons[icon];
	r[0] = (struct rsrc){ FOURCC('I', 'C', 'N', '#'), CUSTOM_ICON_ID, f->icn, sizeof f->icn };
	r[1] = (struct rsrc){ FOURCC('i', 'c', 'l', '8'), CUSTOM_ICON_ID, f->icl8, sizeof f->icl8 };
	r[2] = (struct rsrc){ FOURCC('i', 'c', 'l', '4'), CUSTOM_ICON_ID, f->icl4, sizeof f->icl4 };
	r[3] = (struct rsrc){ FOURCC('i', 'c', 's', '#'), CUSTOM_ICON_ID, f->ics, sizeof f->ics };
	r[4] = (struct rsrc){ FOURCC('i', 'c', 's', '8'), CUSTOM_ICON_ID, f->ics8, sizeof f->ics8 };
	r[5] = (struct rsrc){ FOURCC('i', 'c', 's', '4'), CUSTOM_ICON_ID, f->ics4, sizeof f->ics4 };
}

static int gl_files(struct hfs_file files[TOOLS_GL_FILES]);
static int pippin_file(struct hfs_file *file);
static int isp_file(struct hfs_file *file);

int tools_files(struct hfs_file files[TOOLS_FILES])
{
	static uint8_t helper[8192], helper_rsrc[4096], engine[8192], engine_rsrc[4096];
	struct ndrv_layout layout;
	size_t helper_len = build_helper(helper, sizeof helper, &layout, HELPER_VERSION);
	size_t engine_len = rave_build_engine(engine, sizeof engine, &layout);

	uint8_t cfrg[128], size_res[10];
	size_t cfrg_len = make_cfrg(cfrg, CFRG_APPLICATION, 32u << 10, "Blue Enhancements", 0);
	put_be16(size_res, SIZE_CAN_BACKGROUND | SIZE_ONLY_BACKGROUND | SIZE_32_BIT | SIZE_HIGH_LEVEL_EVENTS);
	put_be32(size_res + 2, 96u << 10);      /* preferred and minimum partition */
	put_be32(size_res + 6, 96u << 10);
	struct rsrc res[2 + ICON_RESOURCES] = {
		{ FOURCC('S', 'I', 'Z', 'E'), -1, size_res, sizeof size_res },
		{ FOURCC('c', 'f', 'r', 'g'), 0, cfrg, (uint32_t)cfrg_len },
	};
	icon_resources(res + 2, FINDER_ICON_ENHANCEMENTS);
	size_t rsrc_len = rsrc_build(res, 2 + ICON_RESOURCES, helper_rsrc, sizeof helper_rsrc);

	/* RAVE loads 'shlb' files of creator 'tnsl' from the Extensions folder */
	uint8_t engine_cfrg[128];
	size_t engine_cfrg_len = make_cfrg(engine_cfrg, CFRG_LIBRARY, 0, "Blue 3D Engine", 0);
	struct rsrc engine_res[1 + ICON_RESOURCES] = {
		{ FOURCC('c', 'f', 'r', 'g'), 0, engine_cfrg, (uint32_t)engine_cfrg_len } };
	icon_resources(engine_res + 1, FINDER_ICON_ENGINE);
	size_t engine_rsrc_len = rsrc_build(engine_res, 1 + ICON_RESOURCES, engine_rsrc, sizeof engine_rsrc);
	if (!helper_len || !rsrc_len || !engine_len || !engine_rsrc_len)
		return -1;

	files[TOOLS_ENHANCEMENTS] = (struct hfs_file){
		"Blue Enhancements", FOURCC('a', 'p', 'p', 'e'), FOURCC('s', 'y', 'H', 'p'), HAS_CUSTOM_ICON, 40, 60,
		helper, helper_rsrc, (uint32_t)helper_len, (uint32_t)rsrc_len };
	files[TOOLS_3D_ENGINE] = (struct hfs_file){
		"Blue 3D Engine", FOURCC('s', 'h', 'l', 'b'), FOURCC('t', 'n', 's', 'l'), HAS_CUSTOM_ICON, 40, 160,
		engine, engine_rsrc, (uint32_t)engine_len, (uint32_t)engine_rsrc_len };
	if (gl_files(files + TOOLS_GL_FIRST))
		return -1;
	return pippin_file(&files[TOOLS_PIPPIN]) || isp_file(&files[TOOLS_ISP]) ? -1 : 0;
}

/* The Pippin library (pippin.h), under the fragment name of the Pippin's */
static int pippin_file(struct hfs_file *file)
{
	static uint8_t frag[PIPPIN_LIBRARY_MAX], rsrc[4096];
	struct ndrv_layout layout;
	size_t len = pippin_build(frag, sizeof frag, &layout);
	uint8_t cfrg[128];
	size_t cfrg_len = make_cfrg(cfrg, CFRG_LIBRARY, 0, "PippinPPC", 0);
	struct rsrc res[1 + ICON_RESOURCES] = { { FOURCC('c', 'f', 'r', 'g'), 0, cfrg, (uint32_t)cfrg_len } };
	icon_resources(res + 1, FINDER_ICON_PIPPIN);
	size_t rsrc_len = rsrc_build(res, 1 + ICON_RESOURCES, rsrc, sizeof rsrc);
	if (!len || !rsrc_len)
		return -1;
	*file = (struct hfs_file){ "Blue Pippin Library", FOURCC('s', 'h', 'l', 'b'), FOURCC('s', 'y', 'P', 'P'),
				   HAS_CUSTOM_ICON, 40, 260, frag, rsrc, (uint32_t)len, (uint32_t)rsrc_len };
	return 0;
}

/* The InputSprocket driver (isp.h): InputSprocket loads the shared libraries of creator 'insp' in Extensions */
static int isp_file(struct hfs_file *file)
{
	static uint8_t frag[ISP_LIBRARY_MAX], rsrc[4096];
	struct ndrv_layout layout;
	size_t len = isp_build(frag, sizeof frag, &layout);
	uint8_t cfrg[128];
	size_t cfrg_len = make_cfrg(cfrg, CFRG_LIBRARY, 0, "Blue Game Controllers", 0);
	struct rsrc res[1 + ICON_RESOURCES] = { { FOURCC('c', 'f', 'r', 'g'), 0, cfrg, (uint32_t)cfrg_len } };
	icon_resources(res + 1, FINDER_ICON_GAMEPAD);
	size_t rsrc_len = rsrc_build(res, 1 + ICON_RESOURCES, rsrc, sizeof rsrc);
	if (!len || !rsrc_len)
		return -1;
	*file = (struct hfs_file){ "Blue Game Controllers", FOURCC('s', 'h', 'l', 'b'), FOURCC('i', 'n', 's', 'p'),
				   HAS_CUSTOM_ICON, 240, 60, frag, rsrc, (uint32_t)len, (uint32_t)rsrc_len };
	return 0;
}

/* The OpenGL libraries (opengl.h), as tools_files() */
static int gl_files(struct hfs_file files[TOOLS_GL_FILES])
{
	static uint8_t frag[TOOLS_GL_FILES][OPENGL_LIBRARY_MAX], rsrc[TOOLS_GL_FILES][4096];
	static const char *const names[TOOLS_GL_FILES] = { "Blue OpenGL Library", "Blue OpenGL Memory",
							   "Blue OpenGL Utility" };
	static const char *const fragments[TOOLS_GL_FILES] = { "OpenGLLibrary", "OpenGLMemory", "OpenGLUtility" };
	size_t (*const build[TOOLS_GL_FILES])(uint8_t *, size_t, struct ndrv_layout *) = {
		opengl_build_library, opengl_build_memory, opengl_build_utility };
	for (unsigned k = 0; k < TOOLS_GL_FILES; k++) {
		struct ndrv_layout layout;
		size_t len = build[k](frag[k], sizeof frag[k], &layout);
		uint8_t cfrg[128];
		/* (as Apple's: current version 1, any version from 0 compatible) */
		size_t cfrg_len = make_cfrg(cfrg, CFRG_LIBRARY, 0, fragments[k], 1);
		struct rsrc res[1 + ICON_RESOURCES] = { { FOURCC('c', 'f', 'r', 'g'), 0, cfrg, (uint32_t)cfrg_len } };
		icon_resources(res + 1, FINDER_ICON_OPENGL);
		size_t rsrc_len = rsrc_build(res, 1 + ICON_RESOURCES, rsrc[k], sizeof rsrc[k]);
		if (!len || !rsrc_len)
			return -1;
		files[k] = (struct hfs_file){ names[k], FOURCC('s', 'h', 'l', 'b'), FOURCC('s', 'y', 'G', 'L'), HAS_CUSTOM_ICON, 140,
					      (int16_t)(60 + 100 * k), frag[k], rsrc[k], (uint32_t)len, (uint32_t)rsrc_len };
	}
	return 0;
}

/* The OpenGL test application (gltest.h) */
static int gl_test_file(struct hfs_file *file)
{
	static uint8_t frag[16384], rsrc[1024];
	struct ndrv_layout layout;
	size_t len = gltest_build(frag, sizeof frag, &layout);
	uint8_t cfrg[128], size_res[10];
	size_t cfrg_len = make_cfrg(cfrg, CFRG_APPLICATION, 32u << 10, "Blue GL Test", 0);
	put_be16(size_res, SIZE_CAN_BACKGROUND | SIZE_32_BIT);
	put_be32(size_res + 2, 512u << 10);
	put_be32(size_res + 6, 512u << 10);
	const struct rsrc res[] = {
		{ FOURCC('S', 'I', 'Z', 'E'), -1, size_res, sizeof size_res },
		{ FOURCC('c', 'f', 'r', 'g'), 0, cfrg, (uint32_t)cfrg_len },
	};
	size_t rsrc_len = rsrc_build(res, 2, rsrc, sizeof rsrc);
	if (!len || !rsrc_len)
		return -1;
	*file = (struct hfs_file){ "Blue GL Test", FOURCC('A', 'P', 'P', 'L'), FOURCC('s', 'y', 'G', 'T'), 0, 240,
				   160, frag, rsrc, (uint32_t)len, (uint32_t)rsrc_len };
	return 0;
}

/* The volume's own icon: an invisible file "Icon\r" holding it (see hfs_format()) */
static int volume_icon_file(struct hfs_file *file)
{
	static uint8_t rsrc[4096];
	struct rsrc res[ICON_RESOURCES];
	icon_resources(res, FINDER_ICON_VOLUME);
	size_t rsrc_len = rsrc_build(res, ICON_RESOURCES, rsrc, sizeof rsrc);
	if (!rsrc_len)
		return -1;
	*file = (struct hfs_file){ HFS_VOLUME_ICON, FOURCC('i', 'c', 'o', 'n'), FOURCC('M', 'A', 'C', 'S'), IS_INVISIBLE, 0, 0,
				   NULL, rsrc, 0, (uint32_t)rsrc_len };
	return 0;
}

int tools_create(const char *path)
{
	struct hfs_file files[TOOLS_FILES + 2];
	unsigned n = TOOLS_FILES;
	if (tools_files(files) || volume_icon_file(&files[n++]))
		return -1;
	const char *test = getenv("BLUE_TOOLS_GL_TEST");
	if (test && strcmp(test, "0") && gl_test_file(&files[n++]))
		return -1;
	const char *appl = getenv("BLUE_TOOLS_APPL");           /* (see tools.h) */
	if (appl && strcmp(appl, "0"))
		files[TOOLS_ENHANCEMENTS].type = FOURCC('A', 'P', 'P', 'L');
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -1;
	}
	int err = ftruncate(fd, TOOLS_VOLUME_SIZE) ? -1 :
		  hfs_format(fd, TOOLS_VOLUME_SIZE, "Blue Tools", hfs_mac_time_now(), files, n);
	if (close(fd))
		err = -1;
	if (err)
		fprintf(stderr, "%s: cannot write the tools volume\n", path);
	return err;
}
