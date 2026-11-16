/*
 * The window's menu (F12), prompts and status bar, made with Dear ImGui
 * (third_party/imgui) and drawn as Mac OS 8's Platinum, in Chicago (font.h),
 * with the SDL2 renderer display.c loads at run time: this takes the SDL
 * functions it draws with from there, so it needs none at build time either.
 */
#ifndef BLUE_UI_H
#define BLUE_UI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ui;

struct ui_rect {                        /* (an SDL_Rect) */
	int x, y, w, h;
};

/* The SDL2 renderer and the functions (SDL 2.0.18 and later) the menu draws with */
struct ui_renderer {
	void *renderer;
	void *(*CreateTexture)(void *renderer, uint32_t format, int access, int w, int h);
	void (*DestroyTexture)(void *texture);
	int (*UpdateTexture)(void *texture, const struct ui_rect *rect, const void *pixels, int pitch);
	int (*SetTextureBlendMode)(void *texture, int mode);
	int (*SetTextureScaleMode)(void *texture, int mode);
	int (*RenderSetScale)(void *renderer, float x, float y);
	int (*RenderSetClipRect)(void *renderer, const struct ui_rect *rect);
	int (*RenderGeometryRaw)(void *renderer, void *texture, const float *xy, int xy_stride, const void *color,
				 int color_stride, const float *uv, int uv_stride, int vertices, const void *indices,
				 int count, int index_size);
};

enum ui_key { UI_KEY_TAB, UI_KEY_LEFT, UI_KEY_RIGHT, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_SPACE, UI_KEY_ENTER };

#define UI_DRIVES 5                     /* drives with removable media: the floppy drive, CD-ROM drives */
#define UI_SCALES 5                     /* the window's scales: Auto, 1x to 4x */

/* What the user chose in the menu (or a prompt's buttons) */
enum ui_action { UI_NONE, UI_RESUME, UI_RESTART, UI_SHUT_DOWN, UI_SCREENSHOT, UI_FULLSCREEN, UI_STATUS_BAR,
		 UI_CAPTURE_MOUSE, UI_RELEASE_MOUSE, UI_TURN_OFF, UI_PROMPT_YES, UI_PROMPT_NO,
		 UI_PROMPT_ALT,                 /* (a prompt's third button, on the left: Don't Ask Again) */
		 UI_INSTALL_TOOLS,              /* (the offer to install the guest tools, asked for) */
		 UI_EJECT,                      /* (+ the drive's number in the menu's list) */
		 UI_EJECT_LAST = UI_EJECT + UI_DRIVES - 1,
		 UI_INSERT,                     /* (likewise: an image chosen with the host's file chooser) */
		 UI_INSERT_LAST = UI_INSERT + UI_DRIVES - 1,
		 UI_SCALE,                      /* (+ the scale chosen: 0 Auto, else display pixels to a screen pixel) */
		 UI_SCALE_LAST = UI_SCALE + UI_SCALES - 1 };

/* What becomes of the mouse after the menu */
enum ui_mouse {
	UI_MOUSE_FOLLOWS,               /* the Mac's cursor shows: it follows the pointer */
	UI_MOUSE_CAPTURED,              /* the Mac hides its cursor: the pointer is captured */
	UI_MOUSE_RELEASED,              /* ... but the user let it go */
};

/* The drives with removable media, as the menu shows them */
struct ui_drive {
	char name[24];                  /* "Floppy drive", "CD-ROM drive 2" */
	char in[64];                    /* the image in it ("": it's empty) */
	bool locked;                    /* ... read-only (CDs always are: not said) */
	bool in_use;                    /* ... mounted in Mac OS, which is then the one to eject it */
};

struct ui_drives {
	unsigned count;
	struct ui_drive drive[UI_DRIVES];
	char next[200];                 /* the floppy images that go in as Mac OS ejects the one in ("": none) */
};

/* The status bar's lights */
enum ui_led { UI_LED_CPU, UI_LED_DISK, UI_LED_CDROM, UI_LED_FLOPPY, UI_LED_NET, UI_LEDS };

struct ui_light {
	uint8_t level;                  /* how brightly it's lit: 0 (off) to 255 */
	uint8_t nap;                    /* ... and softly blue, the processor's while it naps: 0 to 255 */
	bool write;                     /* amber (written to) rather than green */
	bool absent;                    /* there's no such device: it's dimmed */
	bool hidden;                    /* ... not even shown (the floppy drive's, without one) */
};

/* What the status bar shows */
struct ui_status {
	struct ui_light light[UI_LEDS];
	uint32_t mhz;                   /* the processor's speed (millions of instructions a second): 0 not known */
	/* bytes a second each device read (or received) and wrote (or sent); a CD-ROM's only reads */
	uint64_t in[UI_LEDS], out[UI_LEDS];
};

/* What's drawn */
struct ui_state {
	bool menu;                      /* the menu is open */
	float status_bar;               /* the status bar's height, at the bottom of the window (0: none) */
	struct ui_status bar;
	/* what the menu shows */
	bool fullscreen;
	enum ui_mouse mouse;
	const char *news;               /* (where a screenshot went, say), or NULL */
	/* a question or news over the screen (the guest runs on), or NULL: its buttons (`no`, `alt` may be NULL) */
	const char *prompt, *prompt_yes, *prompt_no, *prompt_alt;
	const struct ui_drives *drives; /* the drives with removable media, or NULL without any */
	/* the menu's Install Guest Tools... (`tools`), and why it's disabled (or NULL) */
	bool tools;
	const char *tools_why_not;
	/* the window's scale chosen (0: Auto), and Auto's (display pixels to a screen pixel) */
	unsigned scale, scale_auto;
};

#define UI_STATUS_BAR_HEIGHT 24         /* in the window's units */

struct ui *ui_open(const struct ui_renderer *r);
void ui_close(struct ui *u);

/* Input, in the window's units (points on high-density displays) */
void ui_mouse_move(struct ui *u, float x, float y);
void ui_mouse_button(struct ui *u, int button, bool down);      /* (0: primary) */
void ui_mouse_wheel(struct ui *u, float dx, float dy);
void ui_key(struct ui *u, enum ui_key key, bool down);    /* (Enter: the default button; Space: the focused one) */

/*
 * Draws the status bar, a prompt and the menu (whichever are shown) over what
 * the renderer has drawn so far, in a window of w x h units at `density`
 * pixels each, `seconds` after the last time.
 */
enum ui_action ui_draw(struct ui *u, const struct ui_state *s, int w, int h, float density, double seconds);

#ifdef __cplusplus
}
#endif

#endif
