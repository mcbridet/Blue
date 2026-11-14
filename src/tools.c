#include "tools.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "be.h"
#include "hfs.h"
#include "rave.h"
#include "rsrc.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* 'cfrg' usage */
#define CFRG_LIBRARY 0
#define CFRG_APPLICATION 1

/* 'SIZE' flags */
#define SIZE_CAN_BACKGROUND 0x1000u
#define SIZE_ONLY_BACKGROUND 0x0400u
#define SIZE_32_BIT 0x0080u
#define SIZE_HIGH_LEVEL_EVENTS 0x0040u

/* A 'cfrg' resource with one PowerPC member: the whole data fork. Returns its size. */
static size_t make_cfrg(uint8_t *p, unsigned usage, uint32_t stack, const char *name)
{
	size_t n = strlen(name), member = (43 + n + 3) & ~(size_t)3;
	memset(p, 0, 32 + member);
	put_be16(p + 10, 1);                    /* version */
	put_be16(p + 30, 1);                    /* members */
	uint8_t *m = p + 32;
	memcpy(m, "pwpc", 4);
	put_be32(m + 16, usage == CFRG_APPLICATION ? stack : 0);
	m[22] = (uint8_t)usage;
	m[23] = 1;                              /* in the data fork (offset 0, length 0: all of it) */
	put_be16(m + 40, (uint16_t)member);
	m[42] = (uint8_t)n;
	memcpy(m + 43, name, n);
	return 32 + member;
}

/* ---- syshm Enhancements ---------------------------------------------------- */

enum {
	IMP_WAIT_NEXT_EVENT, IMP_GET_MAIN_DEVICE, IMP_DM_SET_DISPLAY_MODE, IMP_V1_COUNT,
	IMP_CALL_UNIVERSAL_PROC = IMP_V1_COUNT, IMP_TEMP_NEW_HANDLE, IMP_HLOCK, IMP_DISPOSE_HANDLE,
	IMP_GET_CURRENT_PROCESS, IMP_GET_FRONT_PROCESS, IMP_SAME_PROCESS, IMP_POST_EVENT, IMP_COUNT
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
};

/*
 * Entry points: main, then its patches: two traps, and a GetNextEvent filter
 * (the low-memory hook jGNEFilter), which is called by a few instructions of
 * 68k code (see patch()) in each application just before it gets an event.
 */
enum { E_MAIN, E_ZERO_SCRAP, E_PUT_SCRAP, E_EVENT_FILTER, E_COUNT };
#define PATCHES (E_COUNT - 1)
#define PATCH(e) ((e) - E_ZERO_SCRAP)
static const struct {
	uint16_t trap;                  /* (0: the filter) */
	uint32_t proc_info;             /* Pascal: the result's size, then each parameter's */
} patches[PATCHES] = {
	{ 0xA9FC, 0x30 },               /* long ZeroScrap(void) */
	{ 0xA9FE, 0xFF0 },              /* long PutScrap(long length, ResType type, const void *source) */
	{ 0, 0xC0 },                    /* void filter(EventRecord *) */
};
#define TOOL_TRAP_TABLE 0x0E00u         /* in low memory: 1024 entries */
#define LM_JGNE_FILTER 0x029Au

/*
 * Its data (after the entry TVectors): what it is and its version (which
 * goes up when the fragment changes, so that one installed by an older
 * syshm is told apart: see current()), then the rest.
 */
#define HELPER_MAGIC 0x73794870u        /* 'syHp' */
#define HELPER_VERSION 2u
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
#define DATA_SIZE (DATA_FILTER + FILTER_SIZE)

/* Version 1 (before the clipboard): the first three imports, main only, and this data */
#define V1_DATA_EVENT 0u
#define V1_DATA_DEPTH 16u
#define V1_DATA_SIZE 24u

static size_t build_helper(uint8_t *out, size_t size, struct ndrv_layout *layout, bool v1)
{
	static const uint8_t data[DATA_SIZE] = { 's', 'y', 'H', 'p', 0, 0, 0, HELPER_VERSION };
	static const uint8_t v1_data[V1_DATA_SIZE];
	const struct ndrv_spec spec = {
		.kind = FRAG_APPLICATION,
		.entries = v1 ? 1 : E_COUNT,
		.hostcall = HOSTCALL_TOOLS_HELPER,
		.imports = helper_imports,
		.import_count = v1 ? IMP_V1_COUNT : IMP_COUNT,
		.extra = v1 ? v1_data : data,
		.extra_len = v1 ? sizeof v1_data : sizeof data,
	};
	return ndrv_build(&spec, out, size, layout);
}

void helper_init(struct helper *h)
{
	static uint8_t scratch[4096];
	struct ndrv_layout v1;
	memset(h, 0, sizeof *h);
	build_helper(scratch, sizeof scratch, &h->layout, false);
	build_helper(scratch, sizeof scratch, &v1, true);
	h->v1_extra = v1.extra;
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
};
/* The patches' other state words: which one, and its parameters */
enum { S_ENTRY = 1, S_PARAM };

/* Whether the copy making this call is this syshm's: one without the version where this one has it is older */
static bool current(const struct helper *h, const struct ndrv_call *c)
{
	uint32_t data = c->toc + h->layout.extra;
	return mem_ok(c->mem, data, 8, MEM_R) && mem_r32(c->mem, data) == HELPER_MAGIC &&
	       mem_r32(c->mem, data + 4) >= HELPER_VERSION;
}

static uint32_t helper_data(const struct helper *h, const struct ndrv_call *c)
{
	return c->toc + (current(h, c) ? h->layout.extra : h->v1_extra);
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
	ndrv_callout(c, ndrv_import(c, &h->layout, IMP_WAIT_NEXT_EVENT), EVERY_EVENT,
		     helper_data(h, c) + (current(h, c) ? DATA_EVENT : V1_DATA_EVENT), SLEEP_TICKS, 0);
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
 * addresses. This one patches the Scrap Manager and filters events, for
 * every application, if there's a clipboard to share.
 */
static void helper_main(struct helper *h, struct ndrv_call *c, struct clip *clip)
{
	h->running = true;
	h->launches++;
	if (!current(h, c)) {
		h->old = true;
		printf("enhancements: the guest's syshm Enhancements is older than this syshm: install it again from "
		       "the syshm Tools volume (--tools) into the System Folder, and restart\n");
		fflush(stdout);
	} else if (clip->enabled && !h->patched && patch(h, c, true)) {
		h->patched = true;
		h->patched_toc = c->toc;
		clip->listening = true;
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

/*
 * The patches. ZeroScrap and PutScrap tell the host what applications put
 * in the scrap. Each application has a scrap of its own (which the Process
 * Manager hands on when another comes to the front), and some read it in
 * low memory rather than with GetScrap: so the host's newer text, if any,
 * goes into the front application's scrap before it gets its next event.
 */
static void patch_call(struct helper *h, struct ndrv_call *c, struct clip *clip)
{
	h->scrap_calls += c->entry != E_EVENT_FILTER;
	ndrv_set_state(c, S_ENTRY, c->entry);
	for (unsigned k = 0; k < 3; k++)
		ndrv_set_state(c, S_PARAM + k, c->arg[k]);
	if (c->entry == E_ZERO_SCRAP)
		clip_mac_zero(clip);
	if (c->entry == E_EVENT_FILTER && clip->pending && !h->delivering) {
		h->delivering = true;
		ndrv_set_state(c, 0, STEP_CURRENT);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_GET_CURRENT_PROCESS), helper_data(h, c) + DATA_PSN,
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

void helper_call(struct helper *h, struct ndrv_call *c, struct video *v, struct clip *clip)
{
	struct mem *m = c->mem;
	uint32_t data = helper_data(h, c);
	bool old = !current(h, c);

	if (c->entry == E_MAIN) {
		helper_main(h, c, clip);
		return;
	}
	if (c->entry != NDRV_ENTRY_RESUME) {
		if (c->entry < E_COUNT && !old)
			patch_call(h, c, clip);
		else
			ndrv_finish(c, 0);
		return;
	}
	uint32_t result = c->arg[0];            /* resuming after a callout */
	uint32_t step = ndrv_state(c, 0);
	switch (step) {
	case STEP_WAITED: {
		uint32_t ev = data + (old ? V1_DATA_EVENT : DATA_EVENT), mode, depth;
		if (mem_r16(m, ev) == HIGH_LEVEL_EVENT && mem_r32(m, ev + 2) == FOURCC('a', 'e', 'v', 't') &&
		    mem_r32(m, ev + 10) == FOURCC('q', 'u', 'i', 't')) {
			/* the Process Manager asks us to quit (shutting down): the patches go first */
			if (h->patched && c->toc == h->patched_toc) {
				patch(h, c, false);
				h->patched = false;
				clip->listening = false;
			}
			h->running = false;
			ndrv_finish(c, 0);
			return;
		}
		if (!old && h->patched && clip_telling(clip)) {
			/* a resume event for the front application, saying the clipboard has changed */
			clip->told = clip->serial;
			ndrv_set_state(c, 0, STEP_POSTED);
			ndrv_callout(c, ndrv_import(c, &h->layout, IMP_POST_EVENT), OS_EVENT, RESUME_CONVERT_CLIPBOARD,
				     0, 0);
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
		return;
	}
	case STEP_GOT_DEVICE: {
		/* DMSetDisplayMode(device, mode, &depthMode, reserved, displayState) */
		uint32_t depth_at = data + (old ? V1_DATA_DEPTH : DATA_DEPTH);
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
	default:
		if (step >= STEP_CURRENT && step <= STEP_DISPOSED) {
			deliver(h, c, clip, result);
			return;
		}
		ndrv_finish(c, 0);
		return;
	}
}

/* ---- the volume -------------------------------------------------------------- */

int tools_create(const char *path)
{
	static uint8_t helper[8192], helper_rsrc[1024], engine[8192], engine_rsrc[1024];
	struct ndrv_layout layout;
	size_t helper_len = build_helper(helper, sizeof helper, &layout, false);
	size_t engine_len = rave_build_engine(engine, sizeof engine, &layout);

	uint8_t cfrg[128], size_res[10];
	size_t cfrg_len = make_cfrg(cfrg, CFRG_APPLICATION, 32u << 10, "syshm Enhancements");
	put_be16(size_res, SIZE_CAN_BACKGROUND | SIZE_ONLY_BACKGROUND | SIZE_32_BIT | SIZE_HIGH_LEVEL_EVENTS);
	put_be32(size_res + 2, 96u << 10);      /* preferred and minimum partition */
	put_be32(size_res + 6, 96u << 10);
	const struct rsrc res[] = {
		{ FOURCC('S', 'I', 'Z', 'E'), -1, size_res, sizeof size_res },
		{ FOURCC('c', 'f', 'r', 'g'), 0, cfrg, (uint32_t)cfrg_len },
	};
	size_t rsrc_len = rsrc_build(res, 2, helper_rsrc, sizeof helper_rsrc);

	/* RAVE loads 'shlb' files of creator 'tnsl' from the Extensions folder */
	uint8_t engine_cfrg[128];
	size_t engine_cfrg_len = make_cfrg(engine_cfrg, CFRG_LIBRARY, 0, "syshm 3D Engine");
	const struct rsrc engine_res[] = { { FOURCC('c', 'f', 'r', 'g'), 0, engine_cfrg, (uint32_t)engine_cfrg_len } };
	size_t engine_rsrc_len = rsrc_build(engine_res, 1, engine_rsrc, sizeof engine_rsrc);
	if (!helper_len || !rsrc_len || !engine_len || !engine_rsrc_len)
		return -1;

	const char *appl = getenv("SYSHM_TOOLS_APPL");          /* (see tools.h) */
	uint32_t helper_type = appl && strcmp(appl, "0") ? FOURCC('A', 'P', 'P', 'L') : FOURCC('a', 'p', 'p', 'e');
	const struct hfs_file files[] = {
		{ "syshm Enhancements", helper_type, FOURCC('s', 'y', 'H', 'p'), 0, 40, 60,
		  helper, helper_rsrc, (uint32_t)helper_len, (uint32_t)rsrc_len },
		{ "syshm 3D Engine", FOURCC('s', 'h', 'l', 'b'), FOURCC('t', 'n', 's', 'l'), 0, 40, 160,
		  engine, engine_rsrc, (uint32_t)engine_len, (uint32_t)engine_rsrc_len },
	};
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -1;
	}
	int err = ftruncate(fd, TOOLS_VOLUME_SIZE) ? -1 :
		  hfs_format(fd, TOOLS_VOLUME_SIZE, "syshm Tools", hfs_mac_time_now(), files,
			     sizeof files / sizeof files[0]);
	if (close(fd))
		err = -1;
	if (err)
		fprintf(stderr, "%s: cannot write the tools volume\n", path);
	return err;
}
