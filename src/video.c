#include "video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ndrv.h"
#include "png.h"

/* OSErr values */
#define NO_ERR 0
#define CONTROL_ERR (-17)
#define STATUS_ERR (-18)
#define PARAM_ERR (-50)

/* CntrlParam fields */
#define PB_CS_CODE 26
#define PB_CS_PARAM 28

/* Display mode IDs with special meanings */
#define MODE_ID_CURRENT 0x00000000u
#define MODE_ID_FIND_FIRST 0xFFFFFFFEu
#define MODE_ID_NO_MORE 0xFFFFFFFDu

#define DEVICE_CLUT 0u
#define DEVICE_DIRECT 2u

#define CONNECT_VGA 10u
#define CONNECT_ALL_MODES_VALID 0x1u
#define CONNECT_ALL_MODES_SAFE 0x2u

#define TIMING_VALID 0x1u
#define TIMING_SAFE 0x2u
#define TIMING_DEFAULT 0x4u
#define TIMING_FORMAT_DECL 0x6465636Cu    /* 'decl' */

#define GESTALT_VERSION 0x76657273u       /* 'vers' */
#define DRIVER_VERSION 0x01008000u        /* 1.0 final */

const struct video_mode video_modes[] = {
	{ 640, 480, 150 },              /* timingVESA_640x480_60hz */
	{ 800, 600, 182 },              /* timingVESA_800x600_60hz */
	{ 1024, 768, 190 },             /* timingVESA_1024x768_60hz */
	{ 1152, 870, 220 },             /* timingApple_1152x870_75hz */
	{ 1280, 1024, 260 },            /* timingVESA_1280x1024_60hz */
};
const unsigned video_mode_count = sizeof video_modes / sizeof video_modes[0];

#define MODE_COUNT (video_mode_count + VIDEO_CUSTOM_MODES)
#define CUSTOM_TIMING 42u                 /* timingApple_FixedRateLCD: any size the panel likes */
#define MIN_WIDTH 512u
#define MIN_HEIGHT 384u

static const uint8_t depth_bits[VIDEO_DEPTHS] = { 1, 2, 4, 8, 16, 32 };

#define DEFAULT_MODE 1
#define DEFAULT_DEPTH 3

void video_init(struct video *v, struct mem *mem, uint32_t fb_base, uint32_t fb_size)
{
	memset(v, 0, sizeof *v);
	v->mem = mem;
	v->fb_base = fb_base;
	v->fb_size = fb_size;
	v->mode = v->preferred_mode = DEFAULT_MODE;
	v->depth = v->preferred_depth = DEFAULT_DEPTH;
	v->power_state = 3;
	v->request = v->switching = -1;
	for (unsigned k = 0; k < VIDEO_CUSTOM_MODES; k++)
		v->custom[k] = (struct video_mode){ 640, 480, CUSTOM_TIMING };
	for (int k = 0; k < 256; k++)
		v->clut[k][0] = v->clut[k][1] = v->clut[k][2] = (uint8_t)(255 - k);
}

/* Driver data (after the description): see video_build_ndrv */
#define EXTRA_GAMMA 0u                  /* GammaTbl, 12 + 256 bytes */
#define EXTRA_REG_ENTRY 272u            /* RegEntryID of our device node */
#define EXTRA_SERVICE 288u              /* InterruptServiceIDType */
#define EXTRA_TIMER 292u                /* TimerID */
#define EXTRA_NOW 296u                  /* AbsoluteTime from UpTime */
#define EXTRA_EXPIRATION 304u           /* AbsoluteTime for SetInterruptTimer */
#define EXTRA_SIZE 312u

enum { IMP_IO_COMPLETE, IMP_SET_TIMER, IMP_UPTIME, IMP_VSL_NEW_SERVICE, IMP_VSL_DO_SERVICE, IMP_COUNT };

#define VBL_SERVICE_TYPE 0x76626C20u      /* 'vbl ' */

size_t video_build_ndrv(struct video *v, uint8_t *out, size_t size, uint32_t hostcall)
{
	static const struct ndrv_import imports[IMP_COUNT] = {
		[IMP_IO_COMPLETE] = { "DriverServicesLib", "IOCommandIsComplete" },
		[IMP_SET_TIMER] = { "DriverServicesLib", "SetInterruptTimer" },
		[IMP_UPTIME] = { "DriverServicesLib", "UpTime" },
		[IMP_VSL_NEW_SERVICE] = { "VideoServicesLib", "VSLNewInterruptService" },
		[IMP_VSL_DO_SERVICE] = { "VideoServicesLib", "VSLDoInterruptService" },
	};

	/* A linear gamma table (GammaTbl) for GetGamma to point at */
	uint8_t extra[EXTRA_SIZE] = { 0 };
	uint8_t *gamma = extra + EXTRA_GAMMA;
	gamma[7] = 1;                   /* gChanCnt */
	gamma[8] = 0x01;                /* gDataCnt = 256 */
	gamma[11] = 8;                  /* gDataWidth */
	for (int k = 0; k < 256; k++)
		gamma[12 + k] = (uint8_t)k;

	struct ndrv_spec spec = {
		.device_name = "display",
		.driver_name = ".Display_Video_syshm",
		.runtime_options = NDRV_OPENED_UPON_LOAD | NDRV_UNDER_EXPERT_CONTROL,
		.category = 0x6E647276u,        /* 'ndrv' */
		.type = 0x76696465u,            /* 'vide' */
		.version = DRIVER_VERSION,
		.hostcall = hostcall,
		.imports = imports,
		.import_count = IMP_COUNT,
		.extra = extra,
		.extra_len = sizeof extra,
	};
	return ndrv_build(&spec, out, size, &v->layout);
}

/* Mode index (ID - 1): the fixed modes, then the custom ones */
static const struct video_mode *mode_at(const struct video *v, unsigned mode)
{
	return mode < video_mode_count ? &video_modes[mode] : &v->custom[mode - video_mode_count];
}

unsigned video_width(const struct video *v) { return mode_at(v, v->mode)->width; }
unsigned video_height(const struct video *v) { return mode_at(v, v->mode)->height; }
unsigned video_bpp(const struct video *v) { return depth_bits[v->depth]; }

static unsigned row_bytes_for(const struct video *v, unsigned mode, unsigned depth)
{
	return mode_at(v, mode)->width * depth_bits[depth] / 8;
}

unsigned video_row_bytes(const struct video *v)
{
	return row_bytes_for(v, v->mode, v->depth);
}

static bool mode_fits(const struct video *v, unsigned mode, unsigned depth)
{
	return (uint64_t)row_bytes_for(v, mode, depth) * mode_at(v, mode)->height <= v->fb_size;
}

/* Fixed modes are always there; a custom mode only while it's the screen's, or about to be */
static bool custom_in_use(const struct video *v, unsigned mode)
{
	return mode < video_mode_count || mode == v->mode ||
	       (v->request >= 0 && mode == video_mode_count + (unsigned)v->request) ||
	       (v->switching >= 0 && mode == video_mode_count + (unsigned)v->switching);
}

bool video_request_size(struct video *v, unsigned width, unsigned height)
{
	width &= ~7u;                           /* whole bytes at 1 bit per pixel, and then some */
	if (width < MIN_WIDTH)
		width = MIN_WIDTH;
	if (height < MIN_HEIGHT)
		height = MIN_HEIGHT;
	while ((uint64_t)width * height * 4 > v->fb_size) {     /* fits at 32 bits per pixel */
		width = (width * 15 / 16) & ~7u;
		height = height * 15 / 16;
	}
	if (width == video_width(v) && height == video_height(v)) {
		v->request = -1;
		return false;
	}
	if (v->switching >= 0)
		return false;                   /* (the helper asks again when it's done) */
	unsigned slot = v->mode == video_mode_count ? 1 : 0;   /* the custom mode not in use */
	v->custom[slot] = (struct video_mode){ (uint16_t)width, (uint16_t)height, CUSTOM_TIMING };
	v->request = (int)slot;
	return true;
}

bool video_take_request(struct video *v, uint32_t *mode_id, uint32_t *depth_mode)
{
	if (v->request < 0 || v->switching >= 0)
		return false;
	*mode_id = video_mode_count + (unsigned)v->request + 1;
	*depth_mode = VIDEO_FIRST_DEPTH_MODE + v->depth;
	v->switching = v->request;      /* (valid until the switch is over) */
	v->request = -1;
	return true;
}

void video_request_done(struct video *v)
{
	v->switching = -1;
}

/* Depth mode (0x80-based) to depth index; -1 if invalid. */
static int depth_index(uint32_t depth_mode)
{
	uint32_t d = depth_mode - VIDEO_FIRST_DEPTH_MODE;
	return d < VIDEO_DEPTHS ? (int)d : -1;
}

/* Display mode ID (1-based) to mode index; -1 if invalid. */
static int mode_index(uint32_t id)
{
	return id >= 1 && id <= MODE_COUNT ? (int)id - 1 : -1;
}

static void gray_page(struct video *v)
{
	unsigned w = video_width(v), h = video_height(v), bpp = video_bpp(v), rb = video_row_bytes(v);
	uint32_t white = bpp <= 8 ? 0 : bpp == 16 ? 0x7FFF : 0xFFFFFF;
	uint32_t black = bpp <= 8 ? (1u << bpp) - 1 : 0;
	uint8_t *fb = mem_ptr(v->mem, v->fb_base);

	for (unsigned y = 0; y < h; y++) {
		uint8_t *row = fb + (size_t)y * rb;
		memset(row, 0, rb);
		for (unsigned x = 0; x < w; x++) {
			uint32_t px = ((x ^ y) & 1) ? black : white;
			switch (bpp) {
			case 32: put_be32(row + 4 * x, px); break;
			case 16: put_be16(row + 2 * x, (uint16_t)px); break;
			default: {
				unsigned bit = x * bpp;
				row[bit / 8] |= (uint8_t)(px << (8 - bpp - bit % 8));
				break;
			}
			}
		}
	}
}

/* ColorSpec tables: {value, red, green, blue} 16-bit each. */
static int32_t set_entries(struct video *v, uint32_t rec)
{
	uint32_t table = mem_r32(v->mem, rec);
	int16_t start = (int16_t)mem_r16(v->mem, rec + 4);
	int16_t count = (int16_t)mem_r16(v->mem, rec + 6);
	if (!table || count < 0 || count > 255)
		return PARAM_ERR;
	for (int k = 0; k <= count; k++) {
		uint32_t cs = table + 8 * (uint32_t)k;
		unsigned index = start == -1 ? mem_r16(v->mem, cs) & 0xFF : (unsigned)(start + k) & 0xFF;
		uint8_t r = mem_r8(v->mem, cs + 2), g = mem_r8(v->mem, cs + 4), b = mem_r8(v->mem, cs + 6);
		if (v->luminance_mapping)
			r = g = b = (uint8_t)((r * 30 + g * 59 + b * 11) / 100);
		v->clut[index][0] = r;
		v->clut[index][1] = g;
		v->clut[index][2] = b;
	}
	return NO_ERR;
}

static int32_t get_entries(struct video *v, uint32_t rec)
{
	uint32_t table = mem_r32(v->mem, rec);
	int16_t start = (int16_t)mem_r16(v->mem, rec + 4);
	int16_t count = (int16_t)mem_r16(v->mem, rec + 6);
	if (!table || count < 0 || count > 255)
		return PARAM_ERR;
	for (int k = 0; k <= count; k++) {
		uint32_t cs = table + 8 * (uint32_t)k;
		unsigned index = start == -1 ? mem_r16(v->mem, cs) & 0xFF : (unsigned)(start + k) & 0xFF;
		if (start != -1)
			mem_w16(v->mem, cs, (uint16_t)index);
		for (int c = 0; c < 3; c++)
			mem_w16(v->mem, cs + 2 + 2 * (uint32_t)c, (uint16_t)(v->clut[index][c] * 0x101));
	}
	return NO_ERR;
}

static void set_mode(struct video *v, unsigned mode, unsigned depth)
{
	if (mode != v->mode || depth != v->depth)
		v->mode_changes++;
	v->mode = mode;
	v->depth = depth;
}

static int32_t control(struct video *v, uint32_t pb, uint32_t toc)
{
	struct mem *m = v->mem;
	unsigned code = mem_r16(m, pb + PB_CS_CODE);
	uint32_t p = mem_r32(m, pb + PB_CS_PARAM);
	int d, md;
	(void)toc;

	switch (code) {
	case 1:         /* KillIO */
		return NO_ERR;
	case 2:         /* SetMode (VDPageInfo) */
		d = depth_index(mem_r16(m, p));
		if (d < 0 || mem_r16(m, p + 6) != 0 || !mode_fits(v, v->mode, (unsigned)d))
			return PARAM_ERR;
		set_mode(v, v->mode, (unsigned)d);
		mem_w32(m, p + 8, v->fb_base);
		return NO_ERR;
	case 3:         /* SetEntries (VDSetEntryRecord) */
	case 8:         /* DirectSetEntries */
		return set_entries(v, p);
	case 4:         /* SetGamma: only the linear table is supported */
		return NO_ERR;
	case 5:         /* GrayPage (VDPageInfo) */
		if (mem_r16(m, p + 6) != 0)
			return PARAM_ERR;
		gray_page(v);
		return NO_ERR;
	case 6:         /* SetGray (VDGrayRecord) */
		v->luminance_mapping = mem_r8(m, p) != 0;
		return NO_ERR;
	case 7:         /* SetInterrupt (VDFlagRecord): 0 enables, 1 disables */
		v->vbl_disabled = mem_r8(m, p) != 0;
		return NO_ERR;
	case 10:        /* SwitchMode (VDSwitchInfoRec) */
		d = depth_index(mem_r16(m, p));
		md = mode_index(mem_r32(m, p + 2));
		if (d < 0 || md < 0 || mem_r16(m, p + 6) != 0 || !mode_fits(v, (unsigned)md, (unsigned)d))
			return PARAM_ERR;
		set_mode(v, (unsigned)md, (unsigned)d);
		mem_w32(m, p + 8, v->fb_base);
		return NO_ERR;
	case 11:        /* SetSync (VDSyncInfoRec) */
		v->sync = mem_r8(m, p);
		return NO_ERR;
	case 16:        /* SavePreferredConfiguration (VDSwitchInfoRec) */
		d = depth_index(mem_r16(m, p));
		md = mode_index(mem_r32(m, p + 2));
		if (d < 0 || md < 0)
			return PARAM_ERR;
		v->preferred_depth = (unsigned)d;
		v->preferred_mode = (unsigned)md;
		return NO_ERR;
	default:        /* Reset, SetDefaultMode, hardware cursor, power, ... */
		return CONTROL_ERR;
	}
}

static int32_t status(struct video *v, uint32_t pb, uint32_t toc)
{
	struct mem *m = v->mem;
	unsigned code = mem_r16(m, pb + PB_CS_CODE);
	uint32_t p = mem_r32(m, pb + PB_CS_PARAM);
	int d, md;

	switch (code) {
	case 2:         /* GetMode (VDPageInfo) */
		mem_w16(m, p, (uint16_t)(VIDEO_FIRST_DEPTH_MODE + v->depth));
		mem_w16(m, p + 6, 0);
		mem_w32(m, p + 8, v->fb_base);
		return NO_ERR;
	case 3:         /* GetEntries */
		return get_entries(v, p);
	case 4:         /* GetPages */
		mem_w16(m, p + 6, 1);
		return NO_ERR;
	case 5:         /* GetBaseAddr */
		if (mem_r16(m, p + 6) != 0)
			return PARAM_ERR;
		mem_w32(m, p + 8, v->fb_base);
		return NO_ERR;
	case 6:         /* GetGray */
		mem_w8(m, p, v->luminance_mapping);
		return NO_ERR;
	case 7:         /* GetInterrupt */
		mem_w8(m, p, v->vbl_disabled);
		return NO_ERR;
	case 8:         /* GetGamma */
		mem_w32(m, p, toc + v->layout.extra + EXTRA_GAMMA);
		return NO_ERR;
	case 10:        /* GetCurMode (VDSwitchInfoRec) */
		mem_w16(m, p, (uint16_t)(VIDEO_FIRST_DEPTH_MODE + v->depth));
		mem_w32(m, p + 2, v->mode + 1);
		mem_w16(m, p + 6, 0);
		mem_w32(m, p + 8, v->fb_base);
		return NO_ERR;
	case 11:        /* GetSync (VDSyncInfoRec); 0xFF asks for capabilities: none */
		mem_w8(m, p, mem_r8(m, p) == 0xFF ? 0 : v->sync);
		return NO_ERR;
	case 12:        /* GetConnection (VDDisplayConnectInfoRec) */
		mem_w16(m, p, CONNECT_VGA);
		mem_w8(m, p + 2, 0);
		mem_w8(m, p + 3, 0);
		mem_w32(m, p + 4, 0);           /* modes say for themselves whether they're valid */
		mem_w32(m, p + 8, 0);
		return NO_ERR;
	case 13:        /* GetModeTiming (VDTimingInfoRec) */
		md = mode_index(mem_r32(m, p));
		if (md < 0)
			return PARAM_ERR;
		mem_w32(m, p + 8, TIMING_FORMAT_DECL);
		mem_w32(m, p + 12, mode_at(v, (unsigned)md)->timing);
		mem_w32(m, p + 16, !custom_in_use(v, (unsigned)md) ? 0 : TIMING_VALID | TIMING_SAFE |
			(md == (int)v->preferred_mode ? TIMING_DEFAULT : 0));
		return NO_ERR;
	case 16:        /* GetPreferredConfiguration (VDSwitchInfoRec) */
		mem_w16(m, p, (uint16_t)(VIDEO_FIRST_DEPTH_MODE + v->preferred_depth));
		mem_w32(m, p + 2, v->preferred_mode + 1);
		return NO_ERR;
	case 17: {      /* GetNextResolution (VDResolutionInfoRec) */
		uint32_t prev = mem_r32(m, p);
		uint32_t next;
		if (prev == MODE_ID_FIND_FIRST)
			next = 1;
		else if (prev == MODE_ID_CURRENT)
			next = v->mode + 1;
		else if (mode_index(prev) >= 0)
			next = prev + 1;
		else
			return PARAM_ERR;
		if (next > MODE_COUNT) {
			mem_w32(m, p + 4, MODE_ID_NO_MORE);
			return NO_ERR;
		}
		const struct video_mode *vm = mode_at(v, next - 1);
		unsigned max_depth = 0;
		for (unsigned k = 0; k < VIDEO_DEPTHS; k++)
			if (mode_fits(v, next - 1, k))
				max_depth = k;
		mem_w32(m, p + 4, next);
		mem_w32(m, p + 8, vm->width);
		mem_w32(m, p + 12, vm->height);
		mem_w32(m, p + 16, 60u << 16);
		mem_w16(m, p + 20, (uint16_t)(VIDEO_FIRST_DEPTH_MODE + max_depth));
		return NO_ERR;
	}
	case 18: {      /* GetVideoParameters (VDVideoParametersInfoRec) */
		md = mode_index(mem_r32(m, p));
		d = depth_index(mem_r16(m, p + 4));
		if (md < 0 || d < 0 || !mode_fits(v, (unsigned)md, (unsigned)d))
			return PARAM_ERR;
		uint32_t vp = mem_r32(m, p + 6);
		unsigned bpp = depth_bits[d];
		mem_w32(m, p + 10, 1);          /* pages */
		mem_w32(m, p + 14, bpp <= 8 ? DEVICE_CLUT : DEVICE_DIRECT);
		memset(mem_ptr(m, vp), 0, 42);
		mem_w16(m, vp + 4, (uint16_t)row_bytes_for(v, (unsigned)md, (unsigned)d));
		mem_w16(m, vp + 10, mode_at(v, (unsigned)md)->height);  /* bounds bottom */
		mem_w16(m, vp + 12, mode_at(v, (unsigned)md)->width);   /* bounds right */
		mem_w32(m, vp + 22, 72u << 16);                 /* hRes */
		mem_w32(m, vp + 26, 72u << 16);                 /* vRes */
		mem_w16(m, vp + 30, bpp <= 8 ? 0 : 16);         /* pixelType: chunky / RGBDirect */
		mem_w16(m, vp + 32, (uint16_t)bpp);
		mem_w16(m, vp + 34, bpp <= 8 ? 1 : 3);          /* components */
		mem_w16(m, vp + 36, bpp <= 8 ? bpp : bpp == 16 ? 5 : 8);
		return NO_ERR;
	}
	case 29:        /* GetClutBehavior: kSetClutAtSetEntries */
		mem_w32(m, p, 0);
		return NO_ERR;
	case 43:        /* DriverGestalt: selector and response are inline in csParam */
		if (mem_r32(m, pb + PB_CS_PARAM) == GESTALT_VERSION) {
			mem_w32(m, pb + PB_CS_PARAM + 4, DRIVER_VERSION);
			return NO_ERR;
		}
		return STATUS_ERR;
	default:        /* GetDefaultMode, gamma lists, hardware cursor, power, ... */
		return STATUS_ERR;
	}
}

static int32_t driver_io(struct video *v, uint32_t toc, uint32_t contents, uint32_t code)
{
	switch (code) {
	case NDRV_INITIALIZE:
	case NDRV_REPLACE:
	case NDRV_FINALIZE:
	case NDRV_SUPERSEDED:
	case NDRV_KILL_IO:
		return NO_ERR;
	case NDRV_OPEN:
		v->open_count++;
		return NO_ERR;
	case NDRV_CLOSE:
		if (v->open_count)
			v->open_count--;
		return NO_ERR;
	case NDRV_CONTROL:
		return control(v, contents, toc);
	case NDRV_STATUS:
		return status(v, contents, toc);
	default:
		return PARAM_ERR;
	}
}

void video_to_rgb(const struct video *v, uint8_t *rgb)
{
	unsigned w = video_width(v), h = video_height(v), bpp = video_bpp(v), rb = video_row_bytes(v);
	const uint8_t *fb = mem_ptr(v->mem, v->fb_base);

	for (unsigned y = 0; y < h; y++) {
		const uint8_t *row = fb + (size_t)y * rb;
		uint8_t *out = rgb + (size_t)y * w * 3;
		for (unsigned x = 0; x < w; x++, out += 3) {
			if (bpp == 32) {
				out[0] = row[4 * x + 1];
				out[1] = row[4 * x + 2];
				out[2] = row[4 * x + 3];
			} else if (bpp == 16) {
				uint16_t px = be16(row + 2 * x);
				unsigned r = px >> 10 & 31, g = px >> 5 & 31, b = px & 31;
				out[0] = (uint8_t)(r << 3 | r >> 2);
				out[1] = (uint8_t)(g << 3 | g >> 2);
				out[2] = (uint8_t)(b << 3 | b >> 2);
			} else {
				unsigned bit = x * bpp;
				unsigned index = (row[bit / 8] >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
				memcpy(out, v->clut[index], 3);
			}
		}
	}
}

static int32_t driver_io_logged(struct video *v, uint32_t toc, uint32_t contents, uint32_t code, uint32_t kind)
{
	v->calls++;
	int32_t err = driver_io(v, toc, contents, code);
	if (v->log) {
		static const char *const names[] = { "open", "close", "read", "write", "control", "status",
						     "kill", "initialize", "finalize", "replace", "superseded" };
		printf("video: %s", code < 11 ? names[code] : "?");
		if (code == NDRV_CONTROL || code == NDRV_STATUS)
			printf(" %u", mem_r16(v->mem, contents + PB_CS_CODE));
		printf(" (kind %u) -> %d\n", kind, err);
	}
	return err;
}

/* Steps of the driver's flows (state word 0) */
enum {
	STEP_OPEN_SERVICE = 1,          /* VSLNewInterruptService returned */
	STEP_OPEN_UPTIME,               /* UpTime returned; arm the first VBL timer */
	STEP_OPEN_TIMER,                /* SetInterruptTimer returned */
	STEP_VBL_SERVICE,               /* VSLDoInterruptService returned */
	STEP_VBL_UPTIME,
	STEP_VBL_TIMER,
};

/* Arms the VBL timer one period after the UpTime result in EXTRA_NOW. */
static void arm_timer(struct video *v, struct ndrv_call *c, uint32_t next_step)
{
	struct mem *m = v->mem;
	uint32_t data = c->toc + v->layout.extra;
	uint64_t now = mem_r64(m, data + EXTRA_NOW);
	mem_w64(m, data + EXTRA_EXPIRATION, now + v->vbl_period);
	ndrv_set_state(c, 0, next_step);
	ndrv_callout(c, ndrv_import(c, &v->layout, IMP_SET_TIMER), data + EXTRA_EXPIRATION,
		     c->toc + v->layout.entry[1], 0, data + EXTRA_TIMER);
}

static void uptime(struct video *v, struct ndrv_call *c, uint32_t next_step)
{
	ndrv_set_state(c, 0, next_step);
	ndrv_callout(c, ndrv_import(c, &v->layout, IMP_UPTIME), c->toc + v->layout.extra + EXTRA_NOW, 0, 0, 0);
}

void video_driver_call(struct video *v, struct ndrv_call *c, uint32_t timebase_hz)
{
	struct mem *m = v->mem;
	uint32_t data = c->toc + v->layout.extra;

	v->vbl_period = timebase_hz / 60;
	if (c->entry == 0) {            /* DoDriverIO(spaceID, cmdID, contents, code, kind) */
		uint32_t cmd_id = c->arg[1], contents = c->arg[2], code = c->arg[3], kind = c->arg[4];
		if (code == NDRV_INITIALIZE || code == NDRV_REPLACE)    /* DriverInitInfo: refNum, RegEntryID */
			memcpy(mem_ptr(m, data + EXTRA_REG_ENTRY), mem_ptr(m, contents + 2), 16);
		int32_t status = driver_io_logged(v, c->toc, contents, code, kind);
		if (code == NDRV_OPEN && status == 0 && !v->vbl_service) {
			/* VBL interrupts come from a timer, as there is no hardware to raise them */
			ndrv_set_state(c, 0, STEP_OPEN_SERVICE);
			ndrv_set_state(c, 1, (uint32_t)status);
			ndrv_set_state(c, 2, cmd_id);
			ndrv_set_state(c, 3, kind);
			ndrv_callout(c, ndrv_import(c, &v->layout, IMP_VSL_NEW_SERVICE), data + EXTRA_REG_ENTRY,
				     VBL_SERVICE_TYPE, data + EXTRA_SERVICE, 0);
			return;
		}
		ndrv_complete(c, &v->layout, IMP_IO_COMPLETE, cmd_id, kind, status);
		return;
	}
	if (c->entry == 1) {            /* VBL timer (secondary interrupt handler) */
		v->vbls++;
		if (v->vbl_service && !v->vbl_disabled) {
			ndrv_set_state(c, 0, STEP_VBL_SERVICE);
			ndrv_callout(c, ndrv_import(c, &v->layout, IMP_VSL_DO_SERVICE), v->vbl_service, 0, 0, 0);
		} else {
			uptime(v, c, STEP_VBL_UPTIME);
		}
		return;
	}

	uint32_t result = c->arg[0];    /* resuming after a callout */
	switch (ndrv_state(c, 0)) {
	case STEP_OPEN_SERVICE:
		if (result == 0) {
			v->vbl_service = mem_r32(m, data + EXTRA_SERVICE);
			uptime(v, c, STEP_OPEN_UPTIME);
			return;
		}
		if (v->log)
			printf("video: VSLNewInterruptService failed (%d)\n", (int32_t)result);
		ndrv_complete(c, &v->layout, IMP_IO_COMPLETE, ndrv_state(c, 2), ndrv_state(c, 3), (int32_t)ndrv_state(c, 1));
		return;
	case STEP_OPEN_UPTIME:
		if (v->log)
			printf("video: UpTime %016llX, timebase %016llX\n",
			       (unsigned long long)mem_r64(m, data + EXTRA_NOW), (unsigned long long)c->tb);
		arm_timer(v, c, STEP_OPEN_TIMER);
		return;
	case STEP_OPEN_TIMER:
		if (result != 0 && v->log)
			printf("video: SetInterruptTimer failed (%d)\n", (int32_t)result);
		ndrv_complete(c, &v->layout, IMP_IO_COMPLETE, ndrv_state(c, 2), ndrv_state(c, 3), (int32_t)ndrv_state(c, 1));
		return;
	case STEP_VBL_SERVICE:
		uptime(v, c, STEP_VBL_UPTIME);
		return;
	case STEP_VBL_UPTIME:
		arm_timer(v, c, STEP_VBL_TIMER);
		return;
	case STEP_VBL_TIMER:
		ndrv_finish(c, 0);
		return;
	case NDRV_STEP_COMPLETING:
	default:
		ndrv_finish(c, result);
		return;
	}
}

int video_screenshot(const struct video *v, const char *path)
{
	unsigned w = video_width(v), h = video_height(v);
	uint8_t *rgb = malloc((size_t)w * h * 3);
	if (!rgb)
		return -1;
	video_to_rgb(v, rgb);
	int err = png_write_rgb(path, rgb, w, h);
	free(rgb);
	return err;
}
