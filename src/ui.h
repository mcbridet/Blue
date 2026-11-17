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

#include "pad.h"

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
#define UI_USB_PORTS 2                  /* the Mac's USB ports */
#define UI_USB_DEVICES 12               /* the host's USB devices listed */
#define UI_SCALES 5                     /* the window's scales: Auto, 1x to 4x */

/* What the user chose in the menu (or a prompt's buttons) */
enum ui_action { UI_NONE, UI_RESUME, UI_RESTART, UI_SHUT_DOWN, UI_SCREENSHOT, UI_FULLSCREEN, UI_STATUS_BAR,
		 UI_CAPTURE_MOUSE, UI_RELEASE_MOUSE, UI_TURN_OFF, UI_PROMPT_YES, UI_PROMPT_NO,
		 UI_PROMPT_ALT,                 /* (a prompt's third button, on the left: Don't Ask Again) */
		 UI_INSTALL_TOOLS,              /* (the offer to install the guest tools, asked for) */
		 UI_SHARE_REFRESH,              /* (the shared folder's volume put away, written back, offered again) */
		 UI_PADS,                       /* (game controllers as the keyboard and mouse, or not) */
		 UI_MIC,                        /* (sound input from the host's microphone, or silence) */
		 UI_PAD_EDIT,                   /* (a change to a mapping of them: ui_pad_edit()) */
		 UI_PAD_MAPPINGS,               /* (the menu's own: its mappings shown) */
		 UI_MODEM_HANG_UP,              /* (the modem's call ended) */
		 UI_EJECT,                      /* (+ the drive's number in the menu's list) */
		 UI_EJECT_LAST = UI_EJECT + UI_DRIVES - 1,
		 UI_INSERT,                     /* (likewise: an image chosen with the host's file chooser) */
		 UI_INSERT_LAST = UI_INSERT + UI_DRIVES - 1,
		 UI_SCALE,                      /* (+ the scale chosen: 0 Auto, else display pixels to a screen pixel) */
		 UI_SCALE_LAST = UI_SCALE + UI_SCALES - 1,
		 UI_USB_REFRESH,                /* (the host's USB devices listed again) */
		 UI_USB_DISCONNECT,             /* (+ the Mac's port: 0 or 1) */
		 UI_USB_DISCONNECT_LAST = UI_USB_DISCONNECT + UI_USB_PORTS - 1,
		 UI_USB_CONNECT,                /* (+ the host's device's number in the menu's list) */
		 UI_USB_CONNECT_LAST = UI_USB_CONNECT + UI_USB_DEVICES - 1 };

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
	/* the shared folder's volume, if Mac OS can put it away during the run ("": none) */
	char share[64];                 /* its name */
	char share_why_not[240];        /* why Refresh is disabled ("": it isn't) */
	bool share_changed;             /* the folder has changed on the host since the volume was made */
};

/* The game controllers, as the menu shows them */
struct ui_usb_device {
	char name[96];                  /* the maker's and product's names, or its IDs */
	char about[64];                 /* "05ac:0201, full speed" */
	char why_not[200];              /* why it can't be connected ("": it can) */
};

struct ui_usb {
	char port[UI_USB_PORTS][112];   /* what's plugged into the Mac's ports ("": nothing) */
	unsigned count;                 /* the host's devices (hubs left out) */
	struct ui_usb_device dev[UI_USB_DEVICES];
	char why_none[240];             /* why there are no host devices to take ("": there are) */
};

#define UI_PAD_TYPES (PAD_MAX + 1)
#define UI_PAD_ID 64

struct ui_pad_type {
	char id[UI_PAD_ID];             /* ("": all of them; else a type's, settings.h's) */
	char name[PAD_NAME];            /* "All controllers", or a pad's */
	bool own;                       /* (a type's) it has a mapping of its own, else the one for all */
	struct pad_map map;             /* the mapping it has */
};

struct ui_pads {
	unsigned count;                 /* connected */
	char names[200];                /* ... their names */
	bool on;                        /* mapped to the keyboard and mouse (the menu's checkbox) */
	char mapping[200];              /* ... how ("as an AppleJack, by marathon.ppc's mapping") */
	/* Blue's mappings, for all of them and for the types plugged in, and where they're kept (or why not) */
	unsigned types;
	struct ui_pad_type type[UI_PAD_TYPES];
	char kept[1100];
};

/* A change to a mapping (UI_PAD_EDIT): to the type with this id, ui_pad_type's */
enum ui_pad_change {
	UI_PAD_SET,                     /* a control does `action` */
	UI_PAD_STICK,                   /* a stick (`control`: 0 left, 1 right) is `mode` (PAD_STICK_*) */
	UI_PAD_OWN,                     /* the type has a mapping of its own (`mode` 1: the one for all, copied) or not */
	UI_PAD_RESTORE,                 /* the mapping is Blue's default */
};

struct ui_pad_edit {
	char id[UI_PAD_ID];
	enum ui_pad_change change;
	unsigned control, mode;
	struct pad_switch action;
};

/* The status bar's lights (the microphone's only while it's open) */
enum ui_led { UI_LED_CPU, UI_LED_DISK, UI_LED_CDROM, UI_LED_FLOPPY, UI_LED_NET, UI_LED_MIC, UI_LEDS };

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
	const struct ui_pads *pads;     /* the game controllers, or NULL if the window can't have any */
	const char *mic;                /* the microphone: what it's doing ("in use"), or NULL (not shown) */
	bool mic_on;                    /* ... sound input on (the menu's checkbox; off: Mac OS records silence) */
	/* the menu's Install Guest Tools... (`tools`), and why it's disabled (or NULL) */
	bool tools;
	const char *tools_why_not;
	/* the window's scale chosen (0: Auto), and Auto's (display pixels to a screen pixel) */
	unsigned scale, scale_auto;
	/* the modem: what it's doing ("Modem: connected to host:port"; NULL: there's none), with a call to hang up */
	const char *modem;
	bool modem_call;
	const struct ui_usb *usb;       /* USB: the Mac's ports and the host's devices, or NULL (not shown) */
};

#define UI_STATUS_BAR_HEIGHT 24         /* in the window's units */

struct ui *ui_open(const struct ui_renderer *r);
void ui_close(struct ui *u);

/* Input, in the window's units (points on high-density displays) */
void ui_mouse_move(struct ui *u, float x, float y);
void ui_mouse_button(struct ui *u, int button, bool down);      /* (0: primary) */
void ui_mouse_wheel(struct ui *u, float dx, float dy);
void ui_key(struct ui *u, enum ui_key key, bool down);    /* (Enter: the default button; Space: the focused one) */

/* Whether the menu waits for a key to give a controller's button (any key then: ui_capture_key(), an ADB key's) */
bool ui_capturing(const struct ui *u);
void ui_capture_key(struct ui *u, uint8_t adb_key, bool down);

/* The change UI_PAD_EDIT says was made (after ui_draw()) */
void ui_pad_edit(const struct ui *u, struct ui_pad_edit *out);

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
