/*
 * The guest tools and what they're made with: resource forks, HFS volumes
 * with files, application fragments, the display's custom modes and the
 * helper's mode switching (driven here as the guest would drive it).
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "../src/be.h"
#include "../src/hfs.h"
#include "../src/rsrc.h"
#include "../src/tools.h"
#include "../src/video.h"
#include "test.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* A resource of the given type and ID in a fork, or NULL (with its length in *len) */
static const uint8_t *find_resource(const uint8_t *fork, uint32_t type, int16_t id, uint32_t *len)
{
	const uint8_t *data = fork + be32(fork), *map = fork + be32(fork + 4);
	const uint8_t *types = map + be16(map + 24);
	for (unsigned t = 0; t <= be16(types); t++) {
		const uint8_t *e = types + 2 + 8 * t;
		if (be32(e) != type)
			continue;
		for (unsigned k = 0; k <= be16(e + 4); k++) {
			const uint8_t *ref = types + be16(e + 6) + 12 * k;
			if ((int16_t)be16(ref) == id) {
				const uint8_t *r = data + (be32(ref + 4) & 0xFFFFFF);
				*len = be32(r);
				return r + 4;
			}
		}
	}
	return NULL;
}

static void test_rsrc(void)
{
	static const uint8_t a[3] = { 1, 2, 3 }, b[5] = { 4, 5, 6, 7, 8 }, c[1] = { 9 };
	const struct rsrc r[] = {
		{ FOURCC('S', 'I', 'Z', 'E'), -1, a, sizeof a },
		{ FOURCC('c', 'f', 'r', 'g'), 0, b, sizeof b },
		{ FOURCC('c', 'f', 'r', 'g'), 1, c, sizeof c },
	};
	uint8_t fork[512];
	CHECK_EQ(rsrc_build(r, 3, fork, 100), 0);
	size_t n = rsrc_build(r, 3, fork, sizeof fork);
	CHECK(n > 256);
	uint32_t len = 0;
	const uint8_t *p = find_resource(fork, FOURCC('c', 'f', 'r', 'g'), 1, &len);
	CHECK(p && len == 1 && p[0] == 9);
	p = find_resource(fork, FOURCC('S', 'I', 'Z', 'E'), -1, &len);
	CHECK(p && len == 3 && !memcmp(p, a, 3));
	CHECK(!find_resource(fork, FOURCC('c', 'f', 'r', 'g'), 2, &len));
	CHECK_EQ(be16(fork + be32(fork + 4) + 28), 1);  /* two types */
}

static int temp_file(char *path, size_t len)
{
	const char *dir = getenv("TMPDIR");
	snprintf(path, len, "%s/syshm-test-XXXXXX", dir ? dir : "/tmp");
	return mkstemp(path);
}

/* Enough files for several catalog leaves under an index node */
static void test_hfs_files(void)
{
	char path[256];
	int fd = temp_file(path, sizeof path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	static char names[12][32];
	static uint8_t data[12][700];
	struct hfs_file files[12];
	for (unsigned k = 0; k < 12; k++) {
		static const char *const fruit[3] = { "apple %u", "Banana %u", "cherry %u" };  /* (case matters not) */
		snprintf(names[k], sizeof names[k], fruit[k % 3], 12 - k);
		memset(data[k], (int)k + 1, sizeof data[k]);
		files[k] = (struct hfs_file){ names[k], FOURCC('T', 'E', 'X', 'T'), FOURCC('t', 't', 'x', 't'), 0, 0, 0,
					      data[k], data[k], 600 + k, k };
	}
	const uint64_t size = 2u << 20;
	CHECK(ftruncate(fd, (off_t)size) == 0);
	struct hfs_file bad = files[0];
	bad.name = "a:b";
	CHECK(hfs_format(fd, size, "Files", 0, &bad, 1) != 0);
	struct hfs_file twice[2] = { files[0], files[0] };
	CHECK(hfs_format(fd, size, "Files", 0, twice, 2) != 0);
	CHECK_EQ(hfs_format(fd, size, "Files", 0x12345678u, files, 12), 0);

	uint8_t mdb[512], node[512];
	CHECK(pread(fd, mdb, 512, 1024) == 512);
	CHECK_EQ(be16(mdb + 12), 12);                           /* files in the root */
	CHECK_EQ(be32(mdb + 84), 12);                           /* files on the volume */
	CHECK_EQ(be32(mdb + 30), 16 + 12);                      /* next catalog node ID */
	uint32_t al_bl_st = be16(mdb + 28), al_siz = be32(mdb + 20);
	off_t cat = (off_t)al_bl_st * 512 + (off_t)be16(mdb + 150) * al_siz;
	CHECK(pread(fd, node, 512, cat) == 512);
	CHECK_EQ(be16(node + 14), 2);                           /* depth: an index node over the leaves */
	CHECK_EQ(be32(node + 14 + 6), 14);                      /* leaf records */
	uint32_t leaf = be32(node + 14 + 10), last = be32(node + 14 + 14), root = be32(node + 14 + 2);
	CHECK_EQ(leaf, 1);
	CHECK(last > 1);
	CHECK_EQ(root, last + 1);

	/* walk the leaves: records in key order, and each file's data where its extent says */
	char prev[32] = "";
	unsigned records = 0, found = 0;
	while (leaf) {
		CHECK(pread(fd, node, 512, cat + (off_t)leaf * 512) == 512);
		CHECK_EQ(node[8], 0xFF);
		for (unsigned r = 0; r < be16(node + 10); r++, records++) {
			const uint8_t *rec = node + be16(node + 510 - 2 * r);
			if (be32(rec + 2) != 2 || !rec[6])
				continue;
			char name[32] = { 0 };
			memcpy(name, rec + 7, rec[6]);
			CHECK(strcasecmp(prev, name) < 0);
			strcpy(prev, name);
			const uint8_t *f = rec + ((7 + rec[6] + 1) & ~1);
			CHECK_EQ(f[0], 2);
			unsigned k = 0;
			while (k < 12 && strcmp(names[k], name))
				k++;
			CHECK(k < 12);
			if (k == 12)
				continue;
			found++;
			CHECK_EQ(be32(f + 26), 600 + k);               /* data fork length */
			CHECK_EQ(be32(f + 36), k);                      /* resource fork length */
			uint8_t buf[700];
			CHECK(pread(fd, buf, 600 + k, (off_t)al_bl_st * 512 + (off_t)be16(f + 74) * al_siz) ==
			      (ssize_t)(600 + k));
			CHECK(!memcmp(buf, data[k], 600 + k));
		}
		leaf = be32(node);
	}
	CHECK_EQ(records, 14);
	CHECK_EQ(found, 12);
	close(fd);
	unlink(path);
}

static void test_fragments(void)
{
	static const struct ndrv_import imports[] = { { "InterfaceLib", "WaitNextEvent" } };
	for (int kind = FRAG_APPLICATION; kind <= FRAG_LIBRARY; kind++) {
		struct ndrv_spec spec = { .kind = (enum frag_kind)kind, .entries = 3, .hostcall = 9,
					  .imports = imports, .import_count = 1 };
		static uint8_t buf[4096];
		struct ndrv_layout l;
		CHECK(ndrv_build(&spec, buf, sizeof buf, &l) > 0);
		const uint8_t *code = buf + be32(buf + 40 + 20);
		const uint8_t *ld = buf + be32(buf + 40 + 56 + 20);
		CHECK_EQ(be32(code + 16), 0x39600002u);                 /* entry 2: li r11,2 */
		CHECK_EQ(be32(code + 20), 0x48000004u);                 /* ... b common */
		CHECK_EQ(be32(code + 24 + 16), 0x18000009u);            /* the host call */
		CHECK_EQ(be32(ld + (kind == FRAG_APPLICATION ? 0 : 8)), 1);     /* main or init: data section */
		CHECK_EQ(be32(ld + (kind == FRAG_APPLICATION ? 4 : 12)), l.entry[0]);
		CHECK_EQ(be32(ld + (kind == FRAG_APPLICATION ? 8 : 0)), 0xFFFFFFFFu);
		CHECK_EQ(be32(ld + 52), 0);                             /* no exports */
		CHECK_EQ(l.extra, l.entry[2] + 8);                      /* no driver description */
	}
}

/* A library's named exports: TVectors of their entries, found through the hash chain */
static void test_exports(void)
{
	static const struct ndrv_export exports[] = { { .name = "First", .entry = 1 }, { .name = "SecondOne", .entry = 0 } };
	struct ndrv_spec spec = { .kind = FRAG_LIBRARY, .entries = 2, .init_entry = -1, .hostcall = 3,
				  .exports = exports, .export_count = 2 };
	static uint8_t buf[4096];
	struct ndrv_layout l;
	CHECK(ndrv_build(&spec, buf, sizeof buf, &l) > 0);
	const uint8_t *ld = buf + be32(buf + 40 + 56 + 20);
	CHECK_EQ(be32(ld + 8), 0xFFFFFFFFu);                    /* no init routine */
	CHECK_EQ(be32(ld + 24), 0);                             /* no imports */
	CHECK_EQ(be32(ld + 52), 2);
	const uint8_t *strings = ld + be32(ld + 40), *hash = ld + be32(ld + 44);
	CHECK_EQ(be32(hash), 2u << 18);                         /* one chain, both exports */
	const uint8_t *keys = hash + 4, *syms = keys + 8;
	for (unsigned k = 0; k < 2; k++) {
		size_t n = strlen(exports[k].name);
		CHECK_EQ(be32(keys + 4 * k), pef_hash_word(exports[k].name, n));
		CHECK_EQ(syms[10 * k], 2);                      /* TVector */
		CHECK(!memcmp(strings + (be32(syms + 10 * k) & 0xFFFFFF), exports[k].name, n));
		CHECK_EQ(be32(syms + 10 * k + 4), l.entry[exports[k].entry]);
		CHECK_EQ(be16(syms + 10 * k + 8), 1);           /* in the data section */
	}
	const uint8_t *relocs = ld + be32(ld + 36);
	CHECK_EQ(be16(relocs), 0x4601);                         /* just the two TVectors */
}

static void test_volume(void)
{
	char path[256];
	int fd = temp_file(path, sizeof path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	close(fd);
	CHECK_EQ(tools_create(path), 0);
	fd = open(path, O_RDONLY);
	uint8_t mdb[512], node[512];
	CHECK(pread(fd, mdb, 512, 1024) == 512);
	CHECK(!memcmp(mdb + 37, "syshm Tools", 11));
	uint32_t al_bl_st = be16(mdb + 28), al_siz = be32(mdb + 20);
	off_t cat = (off_t)al_bl_st * 512 + (off_t)be16(mdb + 150) * al_siz;
	CHECK(pread(fd, node, 512, cat + 512) == 512);
	const uint8_t *rec = NULL;                              /* syshm Enhancements' catalog record */
	for (unsigned k = 0; k < be16(node + 10) && !rec; k++) {
		const uint8_t *r = node + be16(node + 510 - 2 * k);
		if (r[6] == 18 && !memcmp(r + 7, "syshm Enhancements", 18))
			rec = r;
	}
	CHECK(rec != NULL);
	if (!rec) {
		close(fd);
		return;
	}
	const uint8_t *f = rec + ((7 + 18 + 1) & ~1);
	CHECK_EQ(be32(f + 4), FOURCC('a', 'p', 'p', 'e'));
	uint8_t pef[16], fork[1024];
	CHECK(pread(fd, pef, 16, (off_t)al_bl_st * 512 + (off_t)be16(f + 74) * al_siz) == 16);
	CHECK(!memcmp(pef, "Joy!peffpwpc", 12));
	CHECK_EQ(be16(mdb + 12), 2);                            /* and the 3D engine */
	uint32_t rlen = be32(f + 36), len = 0;
	CHECK(rlen <= sizeof fork);
	CHECK(pread(fd, fork, rlen, (off_t)al_bl_st * 512 + (off_t)be16(f + 86) * al_siz) == (ssize_t)rlen);
	const uint8_t *cfrg = find_resource(fork, FOURCC('c', 'f', 'r', 'g'), 0, &len);
	CHECK(cfrg && !memcmp(cfrg + 32, "pwpc", 4) && cfrg[32 + 22] == 1 && cfrg[32 + 23] == 1);
	CHECK(cfrg && !memcmp(cfrg + 32 + 43, "syshm Enhancements", 18));
	const uint8_t *size = find_resource(fork, FOURCC('S', 'I', 'Z', 'E'), -1, &len);
	CHECK(size && (be16(size) & 0x0400));                  /* background only */
	close(fd);
	unlink(path);
}

/* Guest memory for driver and helper calls */
#define TOC 0x10000u
#define FRAME 0x20000u
#define PB 0x30000u
#define PARAM 0x30100u
#define TVECTORS 0x40000u
#define FB 0x1000000u
#define FB_SIZE (8u << 20)

static struct mem mem;
static struct video video;

static void driver(struct video *v, unsigned code, unsigned cs, uint32_t param, int32_t want)
{
	struct ndrv_call c = { .mem = &mem, .entry = 0, .toc = TOC, .frame = FRAME };
	c.arg[2] = PB;
	c.arg[3] = code;
	c.arg[4] = NDRV_KIND_IMMEDIATE;
	mem_w16(&mem, PB + 26, (uint16_t)cs);
	mem_w32(&mem, PB + 28, param);
	video_driver_call(v, &c);
	CHECK(c.done);
	CHECK_EQ(c.result, (uint32_t)want);
}

static uint32_t timing_flags(struct video *v, uint32_t mode_id)
{
	mem_w32(&mem, PARAM, mode_id);
	driver(v, NDRV_STATUS, 13, PARAM, 0);
	return mem_r32(&mem, PARAM + 16);
}

static void test_custom_modes(void)
{
	struct video *v = &video;
	video_init(v, &mem, FB, FB_SIZE, 16600000);
	unsigned fixed = video_mode_count;
	uint32_t id, depth;

	CHECK(!video_take_request(v, &id, &depth));
	CHECK(!video_request_size(v, 800, 600));                /* the screen's size already */
	CHECK(video_request_size(v, 1003, 701));
	CHECK_EQ(v->custom[0].width, 1000);                     /* whole bytes even at 1 bit per pixel */
	CHECK_EQ(v->custom[0].height, 701);
	CHECK(timing_flags(v, fixed + 1) & 1);                  /* valid while requested ... */
	CHECK_EQ(timing_flags(v, fixed + 2), 0);                /* ... the other one isn't */
	CHECK(video_take_request(v, &id, &depth));
	CHECK_EQ(id, fixed + 1);
	CHECK_EQ(depth, 0x83);                                  /* 8 bits per pixel */
	CHECK(!video_take_request(v, &id, &depth));
	CHECK(timing_flags(v, fixed + 1) & 1);                  /* ... and while switching */
	CHECK(!video_request_size(v, 900, 600));                /* (one switch at a time) */

	/* the Display Manager switches (VDSwitchInfoRec: depth, mode ID, page, base) */
	mem_w16(&mem, PARAM, (uint16_t)depth);
	mem_w32(&mem, PARAM + 2, id);
	mem_w16(&mem, PARAM + 6, 0);
	driver(v, NDRV_CONTROL, 10, PARAM, 0);
	video_request_done(v);
	CHECK_EQ(video_width(v), 1000);
	CHECK_EQ(video_height(v), 701);

	/* GetVideoParameters for it: row bytes and bounds */
	mem_w32(&mem, PARAM, id);
	mem_w16(&mem, PARAM + 4, 0x85);                         /* 32 bits per pixel */
	mem_w32(&mem, PARAM + 6, PARAM + 64);
	driver(v, NDRV_STATUS, 18, PARAM, 0);
	CHECK_EQ(mem_r16(&mem, PARAM + 64 + 4), 4000);
	CHECK_EQ(mem_r16(&mem, PARAM + 64 + 10), 701);
	CHECK_EQ(mem_r16(&mem, PARAM + 64 + 12), 1000);

	/* the next request takes the other custom mode; sizes are kept sensible */
	CHECK(video_request_size(v, 100, 100));
	CHECK(video_take_request(v, &id, &depth));
	CHECK_EQ(id, fixed + 2);
	CHECK_EQ(v->custom[1].width, 512);
	CHECK_EQ(v->custom[1].height, 384);
	video_request_done(v);
	CHECK(video_request_size(v, 8000, 6000));
	CHECK((uint64_t)v->custom[1].width * v->custom[1].height * 4 <= FB_SIZE);
	CHECK_EQ(v->custom[1].width % 8, 0);
	CHECK_EQ(timing_flags(v, fixed + 1), 1 | 2);            /* the current one */

	/* GetNextResolution lists every mode, custom ones included */
	unsigned listed = 0;
	mem_w32(&mem, PARAM, 0xFFFFFFFEu);
	for (;;) {
		driver(v, NDRV_STATUS, 17, PARAM, 0);
		if (mem_r32(&mem, PARAM + 4) == 0xFFFFFFFDu)
			break;
		listed++;
		mem_w32(&mem, PARAM, mem_r32(&mem, PARAM + 4));
	}
	CHECK_EQ(listed, fixed + VIDEO_CUSTOM_MODES);
}

/*
 * The helper as the guest drives it: main, then resumptions after each
 * callout; and its patches, called meanwhile by applications (on other stacks)
 */
static void helper_step(struct helper *h, struct clip *clip, struct ndrv_call *c, uint32_t entry, uint32_t result)
{
	*c = (struct ndrv_call){ .mem = &mem, .entry = entry, .toc = TOC, .frame = FRAME };
	c->arg[0] = result;
	helper_call(h, c, &video, clip);
}

static void patch_step(struct helper *h, struct clip *clip, struct ndrv_call *c, uint32_t entry, uint32_t arg0)
{
	*c = (struct ndrv_call){ .mem = &mem, .entry = entry, .toc = TOC, .frame = FRAME + 0x100 };
	c->arg[0] = arg0;
	helper_call(h, c, &video, clip);
}

/* Its imports' TVectors (TVECTORS + 8 * k), and its data: this syshm's copy, or an older one */
enum { IMP_WNE, IMP_GET_MAIN_DEVICE, IMP_DM_SET_DISPLAY_MODE, IMP_CALL_UNIVERSAL_PROC, IMP_TEMP_NEW_HANDLE, IMP_HLOCK,
       IMP_DISPOSE_HANDLE, IMP_GET_CURRENT_PROCESS, IMP_GET_FRONT_PROCESS, IMP_SAME_PROCESS, IMP_POST_EVENT };
#define IMPORT(k) (TVECTORS + 8u * (k))

static void helper_load(struct helper *h, bool current)
{
	helper_init(h);
	memset(mem_ptr(&mem, TOC), 0, 0x1000);
	for (unsigned k = 0; k < NDRV_MAX_IMPORTS && (k == 0 || h->layout.import[k]); k++)
		mem_w32(&mem, TOC + h->layout.import[k], IMPORT(k));
	if (current) {                                          /* 'syHp', version 2 */
		mem_w32(&mem, TOC + h->layout.extra, FOURCC('s', 'y', 'H', 'p'));
		mem_w32(&mem, TOC + h->layout.extra + 4, 2);
	}
}

static void quit(struct helper *h, struct clip *clip, uint32_t event)
{
	struct ndrv_call c;
	mem_w16(&mem, event, 23);
	mem_w32(&mem, event + 2, FOURCC('a', 'e', 'v', 't'));
	mem_w32(&mem, event + 10, FOURCC('q', 'u', 'i', 't'));
	helper_step(h, clip, &c, NDRV_ENTRY_RESUME, 1);
	CHECK(c.done && !h->running);
}

static void test_helper(void)
{
	struct helper h;
	struct clip clip = { 0 };
	helper_load(&h, true);
	video_init(&video, &mem, FB, FB_SIZE, 16600000);
	const uint32_t event = TOC + h.layout.extra + 8;
	struct ndrv_call c;

	helper_step(&h, &clip, &c, 0, 0);
	CHECK(h.running && !h.old && !h.patched && !c.done);
	CHECK_EQ(c.tvector, IMPORT(IMP_WNE));                   /* WaitNextEvent(everyEvent, &event, sleep, NULL) */
	CHECK_EQ(c.out[0], 0xFFFF);
	CHECK_EQ(c.out[1], event);
	memset(mem_ptr(&mem, event), 0, 16);
	helper_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK_EQ(c.tvector, IMPORT(IMP_WNE));                   /* nothing to do: wait again */

	video_request_size(&video, 1024, 640);
	helper_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK_EQ(c.tvector, IMPORT(IMP_GET_MAIN_DEVICE));
	helper_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0x5000);
	CHECK_EQ(c.tvector, IMPORT(IMP_DM_SET_DISPLAY_MODE));   /* DMSetDisplayMode(gd, mode, &depth, 0, NULL) */
	CHECK_EQ(c.out[0], 0x5000);
	CHECK_EQ(c.out[1], video_mode_count + 1);
	CHECK_EQ(mem_r32(&mem, c.out[2]), 0x83);
	CHECK_EQ(c.out[3], 0);
	CHECK_EQ(c.out[4], 0);
	helper_step(&h, &clip, &c, NDRV_ENTRY_RESUME, (uint32_t)-50);  /* paramErr */
	CHECK_EQ(c.tvector, IMPORT(IMP_WNE));
	CHECK_EQ(h.failures, 1);
	CHECK_EQ(h.last_error, -50);
	CHECK_EQ(video.switching, -1);

	quit(&h, &clip, event);
}

/* One installed by an older syshm: it switches modes as it did, at its data's addresses */
static void test_helper_v1(void)
{
	struct helper h;
	struct clip clip = { .enabled = true };
	helper_load(&h, false);
	video_init(&video, &mem, FB, FB_SIZE, 16600000);
	const uint32_t event = TOC + h.v1_extra;
	struct ndrv_call c;

	CHECK_EQ(h.v1_extra, 24);                               /* (three imports and main: as version 1 had it) */
	mem_w32(&mem, 0xE00 + 4 * 0x1FC, 0x1111);
	helper_step(&h, &clip, &c, 0, 0);                       /* (says to install it again) */
	CHECK(h.running && h.old && !h.patched && !clip.listening);
	CHECK_EQ(mem_r32(&mem, 0xE00 + 4 * 0x1FC), 0x1111);
	CHECK_EQ(c.tvector, IMPORT(IMP_WNE));
	CHECK_EQ(c.out[1], event);
	memset(mem_ptr(&mem, event), 0, 16);
	video_request_size(&video, 800, 512);
	helper_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK_EQ(c.tvector, IMPORT(IMP_GET_MAIN_DEVICE));
	helper_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0x5000);
	CHECK_EQ(c.tvector, IMPORT(IMP_DM_SET_DISPLAY_MODE));
	CHECK_EQ(c.out[2], TOC + h.v1_extra + 16);
	helper_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK_EQ(h.switches, 1);
	quit(&h, &clip, event);
	clip_free(&clip);
}

/* A Mixed Mode routine descriptor for one PowerPC routine */
static void check_descriptor(uint32_t rd, uint32_t proc_info, uint32_t tvector)
{
	CHECK_EQ(mem_r16(&mem, rd), 0xAAFE);
	CHECK_EQ(mem_r8(&mem, rd + 2), 7);
	CHECK_EQ(mem_r16(&mem, rd + 10), 0);                    /* one routine */
	CHECK_EQ(mem_r32(&mem, rd + 12), proc_info);
	CHECK_EQ(mem_r8(&mem, rd + 17), 1);                     /* PowerPC */
	CHECK_EQ(mem_r32(&mem, rd + 20), tvector);
}

#define ZERO_SCRAP_ENTRY (0xE00 + 4 * 0x1FC)
#define PUT_SCRAP_ENTRY (0xE00 + 4 * 0x1FE)
#define JGNE_FILTER 0x29A
#define SCRAP_TEXT 0x50000u
#define TEMP 0x60000u                                           /* a handle's master pointer, then its block */

static void test_helper_clipboard(void)
{
	struct helper h;
	struct clip clip = { .enabled = true };
	helper_load(&h, true);
	video_init(&video, &mem, FB, FB_SIZE, 16600000);
	const uint32_t event = TOC + h.layout.extra + 8;
	struct ndrv_call c;

	/* main: patches ZeroScrap, PutScrap and the GetNextEvent filter */
	mem_w32(&mem, ZERO_SCRAP_ENTRY, 0x1111);
	mem_w32(&mem, PUT_SCRAP_ENTRY, 0x2222);
	mem_w32(&mem, JGNE_FILTER, 0x3333);
	helper_step(&h, &clip, &c, 0, 0);
	CHECK(h.patched && clip.listening);
	CHECK_EQ(c.tvector, IMPORT(IMP_WNE));
	check_descriptor(mem_r32(&mem, ZERO_SCRAP_ENTRY), 0x30, TOC + h.layout.entry[1]);
	check_descriptor(mem_r32(&mem, PUT_SCRAP_ENTRY), 0xFF0, TOC + h.layout.entry[2]);
	uint32_t filter = mem_r32(&mem, JGNE_FILTER);
	CHECK_EQ(mem_r32(&mem, filter), 0x48E7E0C0);           /* movem.l d0-d2/a0-a1,-(sp) */
	CHECK_EQ(mem_r16(&mem, filter + 6), 0x4EB9);            /* jsr ... */
	check_descriptor(mem_r32(&mem, filter + 8), 0xC0, TOC + h.layout.entry[3]);
	CHECK_EQ(mem_r16(&mem, filter + 16), 0x2F3C);           /* ... then the filter before it */
	CHECK_EQ(mem_r32(&mem, filter + 18), 0x3333);

	/* an application's copy: ZeroScrap, PutScrap, each calling the routine patched over */
	patch_step(&h, &clip, &c, 1, 0);
	CHECK_EQ(c.tvector, IMPORT(IMP_CALL_UNIVERSAL_PROC));
	CHECK(c.out[0] == 0x1111 && c.out[1] == 0x30);
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK(c.done && c.result == 0);
	memcpy(mem_ptr(&mem, SCRAP_TEXT), "caf\x8E\rok", 7);
	c = (struct ndrv_call){ .mem = &mem, .entry = 2, .toc = TOC, .frame = FRAME + 0x100,
				.arg = { 7, FOURCC('T', 'E', 'X', 'T'), SCRAP_TEXT } };
	helper_call(&h, &c, &video, &clip);
	CHECK_EQ(c.tvector, IMPORT(IMP_CALL_UNIVERSAL_PROC));
	CHECK(c.out[0] == 0x2222 && c.out[1] == 0xFF0 && c.out[2] == 7 && c.out[4] == SCRAP_TEXT);
	CHECK(!clip.got);
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK(c.done && c.result == 0);
	const char *got = clip_take(&clip);
	CHECK(got && strcmp(got, "caf\xC3\xA9\nok") == 0);
	CHECK_EQ(h.scrap_calls, 2);

	/* the host's text: a resume event for the front application, saying the clipboard has changed */
	clip_host_text(&clip, "na\xC3\xAFve");
	memset(mem_ptr(&mem, event), 0, 16);
	helper_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK_EQ(c.tvector, IMPORT(IMP_POST_EVENT));
	CHECK(c.out[0] == 15 && c.out[1] == 0x01000003);
	CHECK(!clip_telling(&clip));
	helper_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK_EQ(c.tvector, IMPORT(IMP_WNE));

	/* the filter in a background application: not for it */
	patch_step(&h, &clip, &c, 3, event);
	CHECK_EQ(c.tvector, IMPORT(IMP_GET_CURRENT_PROCESS));
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK_EQ(c.tvector, IMPORT(IMP_GET_FRONT_PROCESS));
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK_EQ(c.tvector, IMPORT(IMP_SAME_PROCESS));
	uint32_t same = c.out[2];
	mem_w8(&mem, same, 0);
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK(c.done && clip.pending);

	/* ... and in the front one: into its scrap, as 'TEXT' and 'utxt' */
	patch_step(&h, &clip, &c, 3, event);
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	mem_w8(&mem, same, 1);
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK_EQ(c.tvector, IMPORT(IMP_TEMP_NEW_HANDLE));
	CHECK_EQ(c.out[0], 10);                                 /* (room for either) */
	mem_w32(&mem, TEMP, TEMP + 16);
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, TEMP);
	CHECK(c.tvector == IMPORT(IMP_HLOCK) && c.out[0] == TEMP);
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK(c.tvector == IMPORT(IMP_CALL_UNIVERSAL_PROC) && c.out[0] == 0x1111);      /* ZeroScrap */
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK(c.out[0] == 0x2222 && c.out[2] == 5 && c.out[3] == FOURCC('T', 'E', 'X', 'T') && c.out[4] == TEMP + 16);
	CHECK(!memcmp(mem_ptr(&mem, TEMP + 16), "na\x95ve", 5));
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK(!clip.pending && clip.to_mac == 1);
	CHECK(c.out[0] == 0x2222 && c.out[2] == 10 && c.out[3] == FOURCC('u', 't', 'x', 't'));
	CHECK(!memcmp(mem_ptr(&mem, TEMP + 16), "\0n\0a\0\xEF\0v\0e", 10));
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK(c.tvector == IMPORT(IMP_DISPOSE_HANDLE) && c.out[0] == TEMP);
	patch_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK(c.done && !h.delivering);
	patch_step(&h, &clip, &c, 3, event);                   /* (nothing more to put there) */
	CHECK(c.done);

	/* quitting (asked by syshm, to be replaced): the patches go */
	memset(mem_ptr(&mem, event), 0, 16);
	h.quit = true;
	helper_step(&h, &clip, &c, NDRV_ENTRY_RESUME, 0);
	CHECK(c.done && !h.running && !h.quit && !h.patched);
	CHECK_EQ(mem_r32(&mem, ZERO_SCRAP_ENTRY), 0x1111);
	CHECK_EQ(mem_r32(&mem, PUT_SCRAP_ENTRY), 0x2222);
	CHECK_EQ(mem_r32(&mem, JGNE_FILTER), 0x3333);
	CHECK(!clip.listening);
	clip_free(&clip);
}

void test_tools(void)
{
	test_rsrc();
	test_hfs_files();
	test_fragments();
	test_exports();
	test_volume();
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		return;
	}
	mem_map(&mem, 0, 0x100000, MEM_RW);
	mem_map(&mem, FB, FB_SIZE, MEM_RW);
	test_custom_modes();
	test_helper();
	test_helper_v1();
	test_helper_clipboard();
	mem_free(&mem);
}
