#include "tools.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
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

/* ---- syshm Helper ---------------------------------------------------------- */

enum { IMP_WAIT_NEXT_EVENT, IMP_GET_MAIN_DEVICE, IMP_DM_SET_DISPLAY_MODE, IMP_COUNT };

static const struct ndrv_import helper_imports[IMP_COUNT] = {
	[IMP_WAIT_NEXT_EVENT] = { "InterfaceLib", "WaitNextEvent" },
	[IMP_GET_MAIN_DEVICE] = { "InterfaceLib", "GetMainDevice" },
	[IMP_DM_SET_DISPLAY_MODE] = { "InterfaceLib", "DMSetDisplayMode" },
};

/* Its data (after the entry TVector) */
#define DATA_EVENT 0u                   /* EventRecord */
#define DATA_DEPTH 16u                  /* depth mode, in and out of DMSetDisplayMode */
#define DATA_SIZE 24u

static size_t build_helper(uint8_t *out, size_t size, struct ndrv_layout *layout)
{
	static const uint8_t data[DATA_SIZE];
	const struct ndrv_spec spec = {
		.kind = FRAG_APPLICATION,
		.entries = 1,
		.hostcall = HOSTCALL_TOOLS_HELPER,
		.imports = helper_imports,
		.import_count = IMP_COUNT,
		.extra = data,
		.extra_len = sizeof data,
	};
	return ndrv_build(&spec, out, size, layout);
}

void helper_init(struct helper *h)
{
	static uint8_t scratch[4096];
	memset(h, 0, sizeof *h);
	build_helper(scratch, sizeof scratch, &h->layout);
}

#define EVERY_EVENT 0xFFFFu
#define SLEEP_TICKS 10u                 /* a sixth of a second between looks */
#define HIGH_LEVEL_EVENT 23u

enum { STEP_WAITED = 1, STEP_GOT_DEVICE, STEP_SWITCHED };

static void wait_next_event(struct helper *h, struct ndrv_call *c)
{
	ndrv_set_state(c, 0, STEP_WAITED);
	ndrv_callout(c, ndrv_import(c, &h->layout, IMP_WAIT_NEXT_EVENT), EVERY_EVENT,
		     c->toc + h->layout.extra + DATA_EVENT, SLEEP_TICKS, 0);
}

void helper_call(struct helper *h, struct ndrv_call *c, struct video *v)
{
	struct mem *m = c->mem;
	uint32_t data = c->toc + h->layout.extra;

	if (c->entry == 0) {                    /* main() */
		h->running = true;
		h->launches++;
		wait_next_event(h, c);
		return;
	}
	uint32_t result = c->arg[0];            /* resuming after a callout */
	switch (ndrv_state(c, 0)) {
	case STEP_WAITED: {
		uint32_t ev = data + DATA_EVENT, mode, depth;
		if (mem_r16(m, ev) == HIGH_LEVEL_EVENT && mem_r32(m, ev + 2) == FOURCC('a', 'e', 'v', 't') &&
		    mem_r32(m, ev + 10) == FOURCC('q', 'u', 'i', 't')) {
			h->running = false;     /* the Process Manager asks us to quit (shutting down) */
			ndrv_finish(c, 0);
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
	case STEP_GOT_DEVICE:
		/* DMSetDisplayMode(device, mode, &depthMode, reserved, displayState) */
		mem_w32(m, data + DATA_DEPTH, ndrv_state(c, 2));
		ndrv_set_state(c, 0, STEP_SWITCHED);
		ndrv_callout(c, ndrv_import(c, &h->layout, IMP_DM_SET_DISPLAY_MODE), result, ndrv_state(c, 1),
			     data + DATA_DEPTH, 0);
		c->out[4] = 0;
		return;
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
	default:
		ndrv_finish(c, 0);
		return;
	}
}

/* ---- URL Access stub ------------------------------------------------------------ */

#define URL_ACCESS_NOT_AVAILABLE (-30787)       /* kURLAccessNotAvailableError */

static const struct ndrv_export url_stub_exports[] = {
	{ .name = "URLSimpleDownload", .entry = 0 },
};

static size_t build_url_stub(uint8_t *out, size_t size)
{
	const struct ndrv_spec spec = {
		.kind = FRAG_LIBRARY,
		.entries = 1,
		.init_entry = -1,
		.exports = url_stub_exports,
		.export_count = sizeof url_stub_exports / sizeof url_stub_exports[0],
		.hostcall = HOSTCALL_TOOLS_URL_STUB,
	};
	struct ndrv_layout layout;
	return ndrv_build(&spec, out, size, &layout);
}

void url_stub_call(struct ndrv_call *c)
{
	ndrv_finish(c, (uint32_t)URL_ACCESS_NOT_AVAILABLE);   /* URLSimpleDownload(): no network here */
}

/* ---- the volume -------------------------------------------------------------- */

int tools_create(const char *path)
{
	static uint8_t helper[8192], helper_rsrc[1024], stub[4096], stub_rsrc[1024], engine[8192], engine_rsrc[1024];
	struct ndrv_layout layout;
	size_t helper_len = build_helper(helper, sizeof helper, &layout);
	size_t stub_len = build_url_stub(stub, sizeof stub);
	size_t engine_len = rave_build_engine(engine, sizeof engine, &layout);

	uint8_t cfrg[128], size_res[10];
	size_t cfrg_len = make_cfrg(cfrg, CFRG_APPLICATION, 32u << 10, "syshm Helper");
	put_be16(size_res, SIZE_CAN_BACKGROUND | SIZE_ONLY_BACKGROUND | SIZE_32_BIT | SIZE_HIGH_LEVEL_EVENTS);
	put_be32(size_res + 2, 96u << 10);      /* preferred and minimum partition */
	put_be32(size_res + 6, 96u << 10);
	const struct rsrc res[] = {
		{ FOURCC('S', 'I', 'Z', 'E'), -1, size_res, sizeof size_res },
		{ FOURCC('c', 'f', 'r', 'g'), 0, cfrg, (uint32_t)cfrg_len },
	};
	size_t rsrc_len = rsrc_build(res, 2, helper_rsrc, sizeof helper_rsrc);

	uint8_t stub_cfrg[128];
	size_t stub_cfrg_len = make_cfrg(stub_cfrg, CFRG_LIBRARY, 0, "URLAccessLib");
	const struct rsrc stub_res[] = { { FOURCC('c', 'f', 'r', 'g'), 0, stub_cfrg, (uint32_t)stub_cfrg_len } };
	size_t stub_rsrc_len = rsrc_build(stub_res, 1, stub_rsrc, sizeof stub_rsrc);

	/* RAVE loads 'shlb' files of creator 'tnsl' from the Extensions folder */
	uint8_t engine_cfrg[128];
	size_t engine_cfrg_len = make_cfrg(engine_cfrg, CFRG_LIBRARY, 0, "syshm 3D Engine");
	const struct rsrc engine_res[] = { { FOURCC('c', 'f', 'r', 'g'), 0, engine_cfrg, (uint32_t)engine_cfrg_len } };
	size_t engine_rsrc_len = rsrc_build(engine_res, 1, engine_rsrc, sizeof engine_rsrc);
	if (!helper_len || !rsrc_len || !stub_len || !stub_rsrc_len || !engine_len || !engine_rsrc_len)
		return -1;

	const struct hfs_file files[] = {
		{ "syshm Helper", FOURCC('a', 'p', 'p', 'e'), FOURCC('s', 'y', 'H', 'p'), 0, 40, 60,
		  helper, helper_rsrc, (uint32_t)helper_len, (uint32_t)rsrc_len },
		{ "URL Access stub", FOURCC('s', 'h', 'l', 'b'), FOURCC('s', 'y', 'H', 'p'), 0, 40, 160,
		  stub, stub_rsrc, (uint32_t)stub_len, (uint32_t)stub_rsrc_len },
		{ "syshm 3D Engine", FOURCC('s', 'h', 'l', 'b'), FOURCC('t', 'n', 's', 'l'), 0, 40, 260,
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
