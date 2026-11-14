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
int video_screenshot(const struct video *v, const char *path);

#endif
