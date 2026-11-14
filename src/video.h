/*
 * Native display driver. Mac OS talks to it through a generated NDRV whose
 * DoDriverIO is a host call (see ndrv.h); the framebuffer is ordinary guest
 * memory that QuickDraw draws into directly, and the host reads it back to
 * show (or save) the screen.
 */
#ifndef SYSHM_VIDEO_H
#define SYSHM_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem.h"
#include "ndrv.h"

#define VIDEO_DEPTHS 6                  /* 1, 2, 4, 8, 16, 32 bits per pixel */
#define VIDEO_CUSTOM_MODES 2            /* sized to the host window on request (see below) */
#define VIDEO_FIRST_DEPTH_MODE 0x80u

struct video_mode {
	uint16_t width, height;
	uint32_t timing;                /* Apple timing constant */
};

/*
 * The hardware cursor: Mac OS hands the driver the pointer's image and
 * where it goes, for the window to draw over the screen (instead of
 * drawing it into the framebuffer itself).
 */
#define VIDEO_CURSOR_SIZE 16            /* (Mac OS cursors are 16 x 16 pixels) */
#define VIDEO_CURSOR_CLEAR 0x00000000u  /* pixels: the screen shows through */
#define VIDEO_CURSOR_INVERT 0x00FFFFFFu /* ... inverted; others are 0xFFRRGGBB */

struct video_cursor {
	bool set;                       /* Mac OS has handed the driver an image (that it could take) */
	bool visible;
	int32_t x, y;                   /* the image's top left, in screen pixels */
	unsigned width, height;
	uint32_t pixels[VIDEO_CURSOR_SIZE * VIDEO_CURSOR_SIZE];        /* rows of `width` */
	unsigned long images;           /* counts the images handed over */
};

struct video {
	struct mem *mem;
	uint32_t fb_base, fb_size;
	unsigned mode;                  /* index into video_modes */
	unsigned depth;                 /* index into the depth list */
	unsigned preferred_mode, preferred_depth;
	uint8_t clut[256][3];
	bool luminance_mapping;
	bool vbl_disabled;
	uint8_t sync;
	uint32_t power_state;
	unsigned open_count;
	unsigned long calls, mode_changes;
	struct video_mode custom[VIDEO_CUSTOM_MODES];   /* after the fixed modes */
	int request;                    /* custom mode the helper should switch to, or -1 */
	int switching;                  /* custom mode the helper is switching to, or -1 */
	bool log;                       /* print each request */
	struct ndrv_layout layout;      /* of the generated driver */
	uint32_t vbl_service;           /* Video Services Library VBL interrupt service */
	uint64_t vbl_period;            /* AbsoluteTime units between VBLs */
	unsigned long vbls;
	bool hw_cursor;                 /* offer Mac OS a hardware cursor (set before building the driver) */
	struct video_cursor cursor;
};

extern const struct video_mode video_modes[];
extern const unsigned video_mode_count;

void video_init(struct video *v, struct mem *mem, uint32_t fb_base, uint32_t fb_size);

/* Builds the driver fragment; returns its size or 0. */
size_t video_build_ndrv(struct video *v, uint8_t *out, size_t size, uint32_t hostcall);

/*
 * Runs the driver for one host call: DoDriverIO (entry 0) or the VBL timer
 * (entry 1), or resumes one after a callout. `timebase_hz` sets the VBL rate.
 */
void video_driver_call(struct video *v, struct ndrv_call *c, uint32_t timebase_hz);

unsigned video_width(const struct video *v);
unsigned video_height(const struct video *v);

/*
 * The host window is now `width` x `height` (in guest pixels): one of the
 * custom modes takes that size (within what the framebuffer holds), for the
 * guest tools' helper to switch to with the Display Manager. The two custom
 * modes alternate, so each request is a change of mode. False if the
 * screen is that size already.
 */
bool video_request_size(struct video *v, unsigned width, unsigned height);

/*
 * The helper's side: the display mode ID and depth mode to switch to, if a
 * request is pending; then video_request_done() when the switch is over.
 */
bool video_take_request(struct video *v, uint32_t *mode_id, uint32_t *depth_mode);
void video_request_done(struct video *v);
unsigned video_bpp(const struct video *v);
unsigned video_row_bytes(const struct video *v);

/* Converts the visible screen to RGB (width * height * 3 bytes). */
void video_to_rgb(const struct video *v, uint8_t *rgb);

/* ... and draws the hardware cursor over it, if it's shown */
void video_draw_cursor(const struct video *v, uint8_t *rgb);

/*
 * Saves the screen as a PNG (with the hardware cursor drawn over it, where
 * it's shown).
 */
int video_screenshot(const struct video *v, const char *path);

/*
 * For the window: the screen as 32-bit pixels, 0xFFRRGGBB (SDL's
 * ARGB8888), the same colours as video_to_rgb()'s. A palette holds what
 * converting takes from the depth and CLUT; video_convert() converts
 * `rows` rows of `width` pixels, rows src_pitch bytes apart in the
 * framebuffer's format and dst_pitch bytes apart in `dst`.
 */
struct video_palette {
	unsigned bpp;
	uint32_t color[256];            /* each CLUT entry (8 bits per pixel and fewer) */
	uint32_t bytes[256][8];         /* ... and the pixels each byte holds (fewer than 8) */
};
void video_palette(const struct video *v, struct video_palette *p);
void video_convert(const struct video_palette *p, const uint8_t *src, size_t src_pitch, unsigned width,
		   unsigned rows, uint32_t *dst, size_t dst_pitch);

/*
 * The rows that differ between two copies of the framebuffer (rows
 * `row_bytes` long): up to `max` bands of them, top to bottom, those fewer
 * than VIDEO_ROWS_GAP rows apart (or past the last) taken as one. Returns
 * how many.
 */
#define VIDEO_ROWS_GAP 32u
struct video_rows {
	unsigned from, to;              /* rows from .. to - 1 */
};
unsigned video_changed_rows(const uint8_t *shown, const uint8_t *fb, size_t row_bytes, unsigned rows,
			    struct video_rows *bands, unsigned max);

#endif
