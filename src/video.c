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

/* Hardware cursor calls (Control and Status share the numbers) */
#define CSC_SET_HARDWARE_CURSOR 22
#define CSC_DRAW_HARDWARE_CURSOR 23
#define CSC_SUPPORTS_HARDWARE_CURSOR 22
#define CSC_GET_HARDWARE_CURSOR_DRAW_STATE 23

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

void video_init(struct video *v, struct mem *mem, uint32_t fb_base, uint32_t fb_size, uint32_t timebase_hz)
{
	memset(v, 0, sizeof *v);
	v->mem = mem;
	v->fb_base = fb_base;
	v->fb_size = fb_size;
	v->timebase_hz = timebase_hz;
	v->mode = v->preferred_mode = DEFAULT_MODE;
	v->depth = v->preferred_depth = DEFAULT_DEPTH;
	v->power_state = 3;
	v->request = v->switching = -1;
	for (unsigned k = 0; k < VIDEO_CUSTOM_MODES; k++)
		v->custom[k] = (struct video_mode){ 640, 480, CUSTOM_TIMING };
	for (int k = 0; k < 256; k++)
		v->clut[k][0] = v->clut[k][1] = v->clut[k][2] = (uint8_t)(255 - k);
}

void video_set_time(struct video *v, uint64_t tb)
{
	uint64_t hz = v->timebase_hz;
	uint64_t count = tb / hz * VIDEO_VBL_HZ + tb % hz * VIDEO_VBL_HZ / hz + 1;     /* (the first at timebase 0) */
	if (count > v->vbl_count) {
		v->vbl_count = count;
		v->vbl_status = true;
	}
}

uint64_t video_next_vbl(const struct video *v)
{
	if (!v->vbl_enabled || v->vbl_status)
		return UINT64_MAX;
	uint64_t n = v->vbl_count, hz = v->timebase_hz;        /* (vertical blank n: n * hz / 60, rounded up) */
	return n / VIDEO_VBL_HZ * hz + (n % VIDEO_VBL_HZ * hz + VIDEO_VBL_HZ - 1) / VIDEO_VBL_HZ;
}

uint64_t video_frame_shown(struct video *v, unsigned source, uint32_t id, uint64_t tb)
{
	if (source >= VIDEO_FRAME_SOURCES)
		return tb;
	uint64_t due = v->frame_id[source] == id ? v->frame_tb[source] + v->timebase_hz / VIDEO_VBL_HZ : 0;
	v->frame_id[source] = id;
	v->frame_tb[source] = due > tb ? due : tb;
	return v->frame_tb[source];
}

/* The chip's VBL interrupt on or off; turned on, a vertical blank from before is acknowledged first */
static void enable_vbl(struct video *v, bool on)
{
	if (on && !v->vbl_enabled)
		v->vbl_status = false;
	v->vbl_enabled = on;
}

/* Driver data (after the description): see video_build_ndrv */
#define EXTRA_GAMMA 0u                  /* GammaTbl, 12 + 256 bytes */
#define EXTRA_REG_ENTRY 272u            /* RegEntryID of our device node */
#define EXTRA_SERVICE 288u              /* InterruptServiceIDType */
#define EXTRA_IST_SIZE 292u             /* RegistryPropertyGet's size (in and out) */
#define EXTRA_IST 296u                  /* the node's driver-ist: (set, member) of each interrupt, 3 at most */
#define EXTRA_FUNCS 320u                /* the VBL member's refCon, handler, enabler, disabler */
#define EXTRA_IST_NAME 336u             /* "driver-ist" */
#define EXTRA_SIZE 348u
/* ... and with the hardware cursor: what VSLPrepareCursorForHardwareCursor converts it to, and into */
#define EXTRA_CURSOR_DESC 348u          /* HardwareCursorDescriptorRec, 100 bytes */
#define EXTRA_CURSOR_INFO 448u          /* HardwareCursorInfoRec, 44 bytes */
#define EXTRA_CURSOR_ENCODINGS 492u     /* colorEncodings: a word for each of CURSOR_COLORS */
#define EXTRA_CURSOR_CTAB 1508u         /* ColorTable for colorMap: 8 bytes, then a ColorSpec for each */
#define EXTRA_CURSOR_IMAGE 3548u        /* the cursor: a byte a pixel */
#define EXTRA_CURSOR_SIZE (EXTRA_CURSOR_IMAGE + VIDEO_CURSOR_SIZE * VIDEO_CURSOR_SIZE)

/*
 * The cursor format: 8 bits a pixel, CURSOR_COLORS colours from colorMap
 * (the pixel value is the colour's index), and two more values for
 * transparent and inverting pixels. (The Video Services Library of Mac OS
 * 8.5 to 9.1 converts to indexed formats of 2, 4 or 8 bits only.)
 */
#define CURSOR_COLORS 254u
#define CURSOR_TRANSPARENT 254u
#define CURSOR_INVERTING 255u
#define TRANSPARENT_ENCODED_PIXEL 0x1u  /* supportedSpecialEncodings */
#define INVERTING_ENCODED_PIXEL 0x4u

enum { IMP_IO_COMPLETE, IMP_GET_INT, IMP_INSTALL_INT, IMP_QUEUE_SECONDARY, IMP_PROPERTY_GET, IMP_VSL_NEW_SERVICE,
       IMP_VSL_DO_SERVICE, IMP_VSL_PREPARE_CURSOR, IMP_COUNT };

/* The driver's entry points: DoDriverIO, the VBL interrupt's handler and the secondary handler it queues */
enum { ENTRY_DRIVER_IO, ENTRY_VBL_INTERRUPT, ENTRY_VBL_SECONDARY, ENTRY_COUNT };

#define VBL_SERVICE_TYPE 0x76626C20u      /* 'vbl ' */
#define ISR_IS_COMPLETE 0u                /* an interrupt handler's results */
#define ISR_IS_NOT_COMPLETE 0xFFFFFFFFu

size_t video_build_ndrv(struct video *v, uint8_t *out, size_t size, uint32_t hostcall)
{
	static const struct ndrv_import imports[IMP_COUNT] = {
		[IMP_IO_COMPLETE] = { "DriverServicesLib", "IOCommandIsComplete" },
		[IMP_GET_INT] = { "DriverServicesLib", "GetInterruptFunctions" },
		[IMP_INSTALL_INT] = { "DriverServicesLib", "InstallInterruptFunctions" },
		[IMP_QUEUE_SECONDARY] = { "DriverServicesLib", "QueueSecondaryInterruptHandler" },
		[IMP_PROPERTY_GET] = { "NameRegistryLib", "RegistryPropertyGet" },
		[IMP_VSL_NEW_SERVICE] = { "VideoServicesLib", "VSLNewInterruptService" },
		[IMP_VSL_DO_SERVICE] = { "VideoServicesLib", "VSLDoInterruptService" },
		[IMP_VSL_PREPARE_CURSOR] = { "VideoServicesLib", "VSLPrepareCursorForHardwareCursor" },
	};

	/* A linear gamma table (GammaTbl) for GetGamma to point at */
	uint8_t extra[EXTRA_CURSOR_SIZE] = { 0 };
	uint8_t *gamma = extra + EXTRA_GAMMA;
	gamma[7] = 1;                   /* gChanCnt */
	gamma[8] = 0x01;                /* gDataCnt = 256 */
	gamma[11] = 8;                  /* gDataWidth */
	for (int k = 0; k < 256; k++)
		gamma[12 + k] = (uint8_t)k;
	memcpy(extra + EXTRA_IST_NAME, "driver-ist", sizeof "driver-ist");

	/* HardwareCursorDescriptorRec (colorEncodings is filled in when the driver runs) */
	uint8_t *desc = extra + EXTRA_CURSOR_DESC;
	put_be16(desc, 1);                      /* majorVersion */
	put_be32(desc + 4, VIDEO_CURSOR_SIZE);  /* height */
	put_be32(desc + 8, VIDEO_CURSOR_SIZE);  /* width */
	put_be32(desc + 12, 8);                 /* bitDepth */
	put_be32(desc + 20, CURSOR_COLORS);     /* numColors */
	put_be32(desc + 32, TRANSPARENT_ENCODED_PIXEL | INVERTING_ENCODED_PIXEL);
	put_be32(desc + 36, CURSOR_TRANSPARENT);        /* specialEncodings */
	put_be32(desc + 40, CURSOR_INVERTING);
	for (unsigned k = 0; k < CURSOR_COLORS; k++)
		put_be32(extra + EXTRA_CURSOR_ENCODINGS + 4 * k, k);

	struct ndrv_spec spec = {
		.device_name = "display",
		.driver_name = ".Display_Video_Blue",
		.runtime_options = NDRV_OPENED_UPON_LOAD | NDRV_UNDER_EXPERT_CONTROL,
		.category = 0x6E647276u,        /* 'ndrv' */
		.type = 0x76696465u,            /* 'vide' */
		.version = DRIVER_VERSION,
		.hostcall = hostcall,
		.entries = ENTRY_COUNT,
		.imports = imports,
		/* (without the hardware cursor, the driver is as it was before there was one) */
		.import_count = v->hw_cursor ? IMP_COUNT : IMP_VSL_PREPARE_CURSOR,
		.extra = extra,
		.extra_len = v->hw_cursor ? EXTRA_CURSOR_SIZE : EXTRA_SIZE,
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

void video_mode_now(const struct video *v, uint32_t *mode_id, uint32_t *depth_mode)
{
	*mode_id = v->mode + 1;
	*depth_mode = VIDEO_FIRST_DEPTH_MODE + v->depth;
}

bool video_mode_for(const struct video *v, unsigned width, unsigned height, unsigned bits, uint32_t *mode_id,
		    uint32_t *depth_mode)
{
	unsigned depth = 0;
	while (depth < VIDEO_DEPTHS && depth_bits[depth] != bits)
		depth++;
	for (unsigned mode = 0; depth < VIDEO_DEPTHS && mode < video_mode_count + VIDEO_CUSTOM_MODES; mode++) {
		const struct video_mode *vm = mode_at(v, mode);
		if (vm->width == width && vm->height == height && (mode < video_mode_count || mode == v->mode) &&
		    mode_fits(v, mode, depth)) {
			*mode_id = mode + 1;
			*depth_mode = VIDEO_FIRST_DEPTH_MODE + depth;
			return true;
		}
	}
	return false;
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
	case 7:         /* SetInterrupt (VDFlagRecord): 0 enables VBL interrupts, 1 disables them */
		v->vbl_disabled = mem_r8(m, p) != 0;
		if (v->vbl_service)
			enable_vbl(v, !v->vbl_disabled);
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
	case CSC_DRAW_HARDWARE_CURSOR:  /* VDDrawHardwareCursorRec: x, y, visible */
		if (!v->hw_cursor)
			return CONTROL_ERR;
		v->cursor.x = (int32_t)mem_r32(m, p);
		v->cursor.y = (int32_t)mem_r32(m, p + 4);
		v->cursor.visible = mem_r32(m, p + 8) != 0;
		return NO_ERR;
	default:        /* Reset, SetDefaultMode, power, ... (SetHardwareCursor: see video_driver_call()) */
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
	case CSC_SUPPORTS_HARDWARE_CURSOR:      /* VDSupportsHardwareCursorRec */
		if (!v->hw_cursor)
			return STATUS_ERR;
		mem_w32(m, p, 1);
		return NO_ERR;
	case CSC_GET_HARDWARE_CURSOR_DRAW_STATE:        /* VDHardwareCursorDrawStateRec: x, y, visible, set */
		if (!v->hw_cursor)
			return STATUS_ERR;
		mem_w32(m, p, (uint32_t)v->cursor.x);
		mem_w32(m, p + 4, (uint32_t)v->cursor.y);
		mem_w32(m, p + 8, v->cursor.visible);
		mem_w32(m, p + 12, v->cursor.set);
		return NO_ERR;
	case 43:        /* DriverGestalt: selector and response are inline in csParam */
		if (mem_r32(m, pb + PB_CS_PARAM) == GESTALT_VERSION) {
			mem_w32(m, pb + PB_CS_PARAM + 4, DRIVER_VERSION);
			return NO_ERR;
		}
		return STATUS_ERR;
	default:        /* GetDefaultMode, gamma lists, power, ... */
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

void video_palette(const struct video *v, struct video_palette *p)
{
	unsigned bpp = video_bpp(v), colors = bpp <= 8 ? 1u << bpp : 0;
	p->bpp = bpp;
	for (unsigned k = 0; k < colors; k++)
		p->color[k] = 0xFF000000u | (uint32_t)v->clut[k][0] << 16 | (uint32_t)v->clut[k][1] << 8 | v->clut[k][2];
	if (bpp >= 8)
		return;
	for (unsigned b = 0; b < 256; b++)
		for (unsigned k = 0; k < 8 / bpp; k++)
			p->bytes[b][k] = p->color[(b >> (8 - bpp - k * bpp)) & (colors - 1)];
}

/* (loops simple enough for the compiler to do several pixels at a time) */
static void convert_32(const uint8_t *in, uint32_t *out, unsigned width)
{
	for (unsigned x = 0; x < width; x++)
		out[x] = be32(in + 4 * x) | 0xFF000000u;
}

/* 5-bit channels widened as r << 3 | r >> 2, all three at once */
static void convert_16(const uint8_t *in, uint32_t *out, unsigned width)
{
	for (unsigned x = 0; x < width; x++) {
		uint32_t px = be16(in + 2 * x);
		uint32_t c = (px & 0x7C00) << 9 | (px & 0x03E0) << 6 | (px & 0x001F) << 3;
		out[x] = 0xFF000000u | c | (c >> 5 & 0x070707u);
	}
}

void video_convert(const struct video_palette *p, const uint8_t *src, size_t src_pitch, unsigned width,
		   unsigned rows, uint32_t *dst, size_t dst_pitch)
{
	unsigned per = p->bpp < 8 ? 8 / p->bpp : 1, bytes = width / per, rest = width % per;
	for (unsigned y = 0; y < rows; y++) {
		const uint8_t *in = src + y * src_pitch;
		uint32_t *out = (uint32_t *)(void *)((uint8_t *)dst + y * dst_pitch);
		if (p->bpp == 32) {
			convert_32(in, out, width);
		} else if (p->bpp == 16) {
			convert_16(in, out, width);
		} else if (p->bpp == 8) {
			for (unsigned x = 0; x < width; x++)
				out[x] = p->color[in[x]];
		} else {
			for (unsigned b = 0; b < bytes; b++)
				memcpy(out + b * per, p->bytes[in[b]], per * sizeof *out);
			if (rest)
				memcpy(out + bytes * per, p->bytes[in[bytes]], rest * sizeof *out);
		}
	}
}

unsigned video_changed_rows(const uint8_t *shown, const uint8_t *fb, size_t row_bytes, unsigned rows,
			    struct video_rows *bands, unsigned max)
{
	unsigned n = 0;
	for (unsigned y = 0; y < rows; y++) {
		if (memcmp(shown + y * row_bytes, fb + y * row_bytes, row_bytes) == 0)
			continue;
		if (n && (y - bands[n - 1].to < VIDEO_ROWS_GAP || n == max))
			bands[n - 1].to = y + 1;
		else if (max)
			bands[n++] = (struct video_rows){ y, y + 1 };
	}
	return n;
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
	STEP_OPEN_IST = 1,              /* RegistryPropertyGet returned the node's driver-ist */
	STEP_OPEN_FUNCTIONS,            /* GetInterruptFunctions returned the VBL member's */
	STEP_OPEN_INSTALL,              /* InstallInterruptFunctions returned */
	STEP_OPEN_SERVICE,              /* VSLNewInterruptService returned */
	STEP_OPEN_ENABLE,               /* the member's enabler returned */
	STEP_VBL_QUEUED,                /* QueueSecondaryInterruptHandler returned */
	STEP_VBL_SERVICE,               /* VSLDoInterruptService returned */
	STEP_CURSOR_PREPARED,           /* VSLPrepareCursorForHardwareCursor returned */
};

/*
 * SetHardwareCursor: the Video Services Library converts Mac OS's cursor
 * (csCursorRef) to the driver's format, if it can; if it can't (too many
 * colours), SetHardwareCursor fails, and Mac OS draws that cursor itself.
 * Mac OS 8.5 to 9.1 call SetHardwareCursor with each new cursor (without
 * asking SupportsHardwareCursor first), then DrawHardwareCursor as it moves.
 */
static void prepare_cursor(struct video *v, struct ndrv_call *c, uint32_t cursor_ref)
{
	struct mem *m = v->mem;
	uint32_t data = c->toc + v->layout.extra;
	uint32_t info = data + EXTRA_CURSOR_INFO;
	mem_w32(m, data + EXTRA_CURSOR_DESC + 24, data + EXTRA_CURSOR_ENCODINGS);
	memset(mem_ptr(m, info), 0, 44);        /* HardwareCursorInfoRec */
	mem_w16(m, info, 1);                    /* majorVersion */
	mem_w32(m, info + 12, data + EXTRA_CURSOR_CTAB);        /* colorMap */
	mem_w32(m, info + 16, data + EXTRA_CURSOR_IMAGE);       /* hardwareCursor */
	ndrv_callout(c, ndrv_import(c, &v->layout, IMP_VSL_PREPARE_CURSOR), cursor_ref, data + EXTRA_CURSOR_DESC,
		     info, 0);
}

/* ... and takes it if it came (`ok`, the library's Boolean) */
static int32_t cursor_prepared(struct video *v, struct ndrv_call *c, uint32_t ok)
{
	struct mem *m = v->mem;
	uint32_t data = c->toc + v->layout.extra;
	uint32_t info = data + EXTRA_CURSOR_INFO, ctab = data + EXTRA_CURSOR_CTAB + 8;
	unsigned w = mem_r32(m, info + 8), h = mem_r32(m, info + 4);
	if (!(ok & 0xFF) || !w || !h || w > VIDEO_CURSOR_SIZE || h > VIDEO_CURSOR_SIZE) {
		v->cursor.set = v->cursor.visible = false;
		return CONTROL_ERR;
	}
	struct video_cursor *cur = &v->cursor;
	uint32_t pixels[VIDEO_CURSOR_SIZE * VIDEO_CURSOR_SIZE];
	for (unsigned y = 0; y < h; y++) {
		for (unsigned x = 0; x < w; x++) {
			unsigned px = mem_r8(m, data + EXTRA_CURSOR_IMAGE + y * VIDEO_CURSOR_SIZE + x);
			uint32_t cs = ctab + 8 * px;
			pixels[y * w + x] = px == CURSOR_TRANSPARENT ? VIDEO_CURSOR_CLEAR :
					    px == CURSOR_INVERTING ? VIDEO_CURSOR_INVERT :
					    0xFF000000u | (uint32_t)mem_r8(m, cs + 2) << 16 |
					    (uint32_t)mem_r8(m, cs + 4) << 8 | mem_r8(m, cs + 6);
		}
	}
	/* (Mac OS hands over the same one again and again) */
	if (!cur->set || w != cur->width || h != cur->height || memcmp(pixels, cur->pixels, w * h * sizeof *pixels)) {
		memcpy(cur->pixels, pixels, w * h * sizeof *pixels);
		cur->width = w;
		cur->height = h;
		cur->images++;
	}
	cur->set = true;
	return NO_ERR;
}

/*
 * VBL interrupts, set up at Open as a PCI card's driver does it: our handler
 * in the display node's interrupt set (its driver-ist property names the
 * set and member), keeping the set's refCon, enabler and disabler; then the
 * Video Services Library's VBL service, which Mac OS hangs the screen's VBL
 * tasks on; then the chip's interrupt and the set's member enabled. Each
 * step runs after the last one's callout returns. Without an interrupt set,
 * there are no VBL interrupts (and no VBL service, which would wait for
 * them).
 */
static void open_step(struct video *v, struct ndrv_call *c)
{
	struct mem *m = v->mem;
	uint32_t data = c->toc + v->layout.extra;
	uint32_t set = mem_r32(m, data + EXTRA_IST), member = mem_r32(m, data + EXTRA_IST + 4);
	uint32_t refcon = mem_r32(m, data + EXTRA_FUNCS), enabler = mem_r32(m, data + EXTRA_FUNCS + 8);
	uint32_t step = ndrv_state(c, 0), status = c->arg[0];

	switch (step) {
	case STEP_OPEN_IST:
		if (status || mem_r32(m, data + EXTRA_IST_SIZE) < 8)
			break;
		ndrv_set_state(c, 0, STEP_OPEN_FUNCTIONS);
		ndrv_callout(c, ndrv_import(c, &v->layout, IMP_GET_INT), set, member, data + EXTRA_FUNCS,
			     data + EXTRA_FUNCS + 4);
		c->out[4] = data + EXTRA_FUNCS + 8;
		c->out[5] = data + EXTRA_FUNCS + 12;
		return;
	case STEP_OPEN_FUNCTIONS:
		if (status)
			break;
		ndrv_set_state(c, 0, STEP_OPEN_INSTALL);
		ndrv_callout(c, ndrv_import(c, &v->layout, IMP_INSTALL_INT), set, member, refcon,
			     c->toc + v->layout.entry[ENTRY_VBL_INTERRUPT]);
		c->out[4] = enabler;
		c->out[5] = mem_r32(m, data + EXTRA_FUNCS + 12);
		return;
	case STEP_OPEN_INSTALL:
		if (status)
			break;
		ndrv_set_state(c, 0, STEP_OPEN_SERVICE);
		ndrv_callout(c, ndrv_import(c, &v->layout, IMP_VSL_NEW_SERVICE), data + EXTRA_REG_ENTRY,
			     VBL_SERVICE_TYPE, data + EXTRA_SERVICE, 0);
		return;
	case STEP_OPEN_SERVICE:
		if (status)
			break;
		v->vbl_service = mem_r32(m, data + EXTRA_SERVICE);
		enable_vbl(v, !v->vbl_disabled);
		ndrv_set_state(c, 0, STEP_OPEN_ENABLE);
		if (enabler) {
			ndrv_callout(c, enabler, set, member, refcon, 0);
			return;
		}
		break;
	default:                        /* STEP_OPEN_ENABLE */
		break;
	}
	if (v->log && v->vbl_service)
		printf("video: VBL interrupts from interrupt set %08X, member %u\n", set, member);
	else if (v->log)
		printf("video: no VBL interrupts (step %u, status %d)\n", step, (int32_t)status);
	ndrv_complete(c, &v->layout, IMP_IO_COMPLETE, ndrv_state(c, 2), ndrv_state(c, 3), (int32_t)ndrv_state(c, 1));
}

void video_driver_call(struct video *v, struct ndrv_call *c)
{
	struct mem *m = v->mem;
	uint32_t data = c->toc + v->layout.extra;

	if (c->entry == ENTRY_DRIVER_IO) {      /* DoDriverIO(spaceID, cmdID, contents, code, kind) */
		uint32_t cmd_id = c->arg[1], contents = c->arg[2], code = c->arg[3], kind = c->arg[4];
		if (code == NDRV_INITIALIZE || code == NDRV_REPLACE)    /* DriverInitInfo: refNum, RegEntryID */
			memcpy(mem_ptr(m, data + EXTRA_REG_ENTRY), mem_ptr(m, contents + 2), 16);
		if (code == NDRV_CONTROL && mem_r16(m, contents + PB_CS_CODE) == CSC_SET_HARDWARE_CURSOR && v->hw_cursor) {
			v->calls++;
			ndrv_set_state(c, 0, STEP_CURSOR_PREPARED);
			ndrv_set_state(c, 2, cmd_id);
			ndrv_set_state(c, 3, kind);
			prepare_cursor(v, c, mem_r32(m, mem_r32(m, contents + PB_CS_PARAM)));
			return;
		}
		int32_t status = driver_io_logged(v, c->toc, contents, code, kind);
		if (code == NDRV_OPEN && status == 0 && !v->vbl_service) {
			ndrv_set_state(c, 0, STEP_OPEN_IST);
			ndrv_set_state(c, 1, (uint32_t)status);
			ndrv_set_state(c, 2, cmd_id);
			ndrv_set_state(c, 3, kind);
			mem_w32(m, data + EXTRA_IST_SIZE, 24);
			ndrv_callout(c, ndrv_import(c, &v->layout, IMP_PROPERTY_GET), data + EXTRA_REG_ENTRY,
				     data + EXTRA_IST_NAME, data + EXTRA_IST, data + EXTRA_IST_SIZE);
			return;
		}
		ndrv_complete(c, &v->layout, IMP_IO_COMPLETE, cmd_id, kind, status);
		return;
	}
	if (c->entry == ENTRY_VBL_INTERRUPT) {  /* (set, member, refCon, count), at hardware interrupt level */
		if (!v->vbl_status) {
			ndrv_finish(c, ISR_IS_NOT_COMPLETE);
			return;
		}
		v->vbl_status = false;          /* acknowledged */
		v->vbls++;
		if (v->vbl_queued || !v->vbl_service) {
			ndrv_finish(c, ISR_IS_COMPLETE);
			return;
		}
		v->vbl_queued = true;
		ndrv_set_state(c, 0, STEP_VBL_QUEUED);
		ndrv_callout(c, ndrv_import(c, &v->layout, IMP_QUEUE_SECONDARY),
			     c->toc + v->layout.entry[ENTRY_VBL_SECONDARY], 0, 0, 0);
		return;
	}
	if (c->entry == ENTRY_VBL_SECONDARY) {  /* the VBL tasks, at secondary interrupt level */
		v->vbl_queued = false;
		if (v->vbl_service && !v->vbl_disabled) {
			ndrv_set_state(c, 0, STEP_VBL_SERVICE);
			ndrv_callout(c, ndrv_import(c, &v->layout, IMP_VSL_DO_SERVICE), v->vbl_service, 0, 0, 0);
			return;
		}
		ndrv_finish(c, 0);
		return;
	}

	uint32_t result = c->arg[0];    /* resuming after a callout */
	switch (ndrv_state(c, 0)) {
	case STEP_OPEN_IST:
	case STEP_OPEN_FUNCTIONS:
	case STEP_OPEN_INSTALL:
	case STEP_OPEN_SERVICE:
	case STEP_OPEN_ENABLE:
		open_step(v, c);
		return;
	case STEP_VBL_QUEUED:
		if (result)
			v->vbl_queued = false;
		ndrv_finish(c, ISR_IS_COMPLETE);
		return;
	case STEP_VBL_SERVICE:
		ndrv_finish(c, 0);
		return;
	case STEP_CURSOR_PREPARED: {
		int32_t status = cursor_prepared(v, c, result);
		if (v->log)
			printf("video: control %u (kind %u) -> %d\n", CSC_SET_HARDWARE_CURSOR, ndrv_state(c, 3), status);
		ndrv_complete(c, &v->layout, IMP_IO_COMPLETE, ndrv_state(c, 2), ndrv_state(c, 3), status);
		return;
	}
	case NDRV_STEP_COMPLETING:
	default:
		ndrv_finish(c, result);
		return;
	}
}

void video_draw_cursor(const struct video *v, uint8_t *rgb)
{
	const struct video_cursor *cur = &v->cursor;
	int w = (int)video_width(v), h = (int)video_height(v);
	if (!v->hw_cursor || !cur->set || !cur->visible)
		return;
	for (int y = 0; y < (int)cur->height; y++) {
		for (int x = 0; x < (int)cur->width; x++) {
			int sx = cur->x + x, sy = cur->y + y;
			uint32_t px = cur->pixels[y * (int)cur->width + x];
			if (sx < 0 || sy < 0 || sx >= w || sy >= h || px == VIDEO_CURSOR_CLEAR)
				continue;
			uint8_t *out = rgb + ((size_t)sy * (size_t)w + (size_t)sx) * 3;
			for (int c = 0; c < 3; c++)
				out[c] = px == VIDEO_CURSOR_INVERT ? (uint8_t)~out[c] : (uint8_t)(px >> (16 - 8 * c));
		}
	}
}

int video_screenshot(const struct video *v, const char *path)
{
	unsigned w = video_width(v), h = video_height(v);
	uint8_t *rgb = malloc((size_t)w * h * 3);
	if (!rgb)
		return -1;
	video_to_rgb(v, rgb);
	video_draw_cursor(v, rgb);
	int err = png_write_rgb(path, rgb, w, h);
	free(rgb);
	return err;
}
