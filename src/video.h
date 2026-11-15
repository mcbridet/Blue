/*
 * Native display driver. Mac OS talks to it through a generated NDRV whose
 * DoDriverIO is a host call (see ndrv.h); the framebuffer is ordinary guest
 * memory that QuickDraw draws into directly, and the host reads it back to
 * show (or save) the screen.
 *
 * The display interrupts at each vertical blank, as the iMac's ATI chip
 * does: VIDEO_VBL_HZ times a second of guest time, on the timebase. Each
 * vertical blank latches the chip's VBL status, which raises its interrupt
 * line while the interrupt is enabled, until the driver acknowledges it.
 * The driver's handler for it, in the display node's interrupt set, does
 * that and queues a secondary interrupt handler, which runs Mac OS's VBL
 * tasks for the screen (the Video Services Library's VBL service).
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
#define VIDEO_VBL_HZ 60u
#define VIDEO_FRAME_SOURCES 16          /* 3D drawing contexts paced by its refresh (see video_frame_shown()) */

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
	bool vbl_disabled;              /* Mac OS turned VBL interrupts off (SetInterrupt) */
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
	/* the chip's vertical blank (see above) */
	uint32_t timebase_hz;
	uint64_t vbl_count;             /* vertical blanks since timebase 0 */
	bool vbl_status;                /* ... one since the driver last acknowledged one */
	bool vbl_enabled;               /* ... which interrupts (the driver's to set) */
	bool vbl_queued;                /* the handler queued the secondary handler, which hasn't run yet */
	unsigned long vbls;             /* VBL interrupts the driver took */
	/* frames drawn for the screen by 3D drawing contexts (see video_frame_shown()) */
	uint32_t frame_id[VIDEO_FRAME_SOURCES];
	uint64_t frame_tb[VIDEO_FRAME_SOURCES];
	bool hw_cursor;                 /* offer Mac OS a hardware cursor (set before building the driver) */
	struct video_cursor cursor;
};

extern const struct video_mode video_modes[];
extern const unsigned video_mode_count;

/* `timebase_hz`: the timebase's rate, which vertical blanks are timed by */
void video_init(struct video *v, struct mem *mem, uint32_t fb_base, uint32_t fb_size, uint32_t timebase_hz);

/* Builds the driver fragment; returns its size or 0. */
size_t video_build_ndrv(struct video *v, uint8_t *out, size_t size, uint32_t hostcall);

/*
 * Runs the driver for one host call: DoDriverIO (entry 0), the VBL
 * interrupt's handler (entry 1) or the secondary handler it queues (entry
 * 2), or resumes one after a callout.
 */
void video_driver_call(struct video *v, struct ndrv_call *c);

/*
 * The vertical blank, as a device: video_set_time() brings it up to the
 * timebase `tb` (latching the VBL status if one has passed);
 * video_next_vbl() is the timebase at which the interrupt line next
 * rises, UINT64_MAX if nothing would change it (the interrupt is off, or
 * already raised).
 */
void video_set_time(struct video *v, uint64_t tb);
uint64_t video_next_vbl(const struct video *v);

/*
 * A frame for the screen from a 3D drawing context (`id`, in slot `source`
 * of VIDEO_FRAME_SOURCES), finished at timebase `tb`: the timebase at which
 * the display shows it, at most one a vertical blank. That's a vertical
 * blank interval after the context's previous one, or `tb` if that's later
 * (or it's the context's first; the guest waits for the rest: see nk.c).
 */
uint64_t video_frame_shown(struct video *v, unsigned source, uint32_t id, uint64_t tb);

/* Whether the display's interrupt line is raised */
static inline bool video_irq(const struct video *v)
{
	return v->vbl_enabled && v->vbl_status;
}

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
