/*
 * Host display window and input, using SDL2 loaded at run time (no build
 * dependency; if the library or a display isn't available, the emulator
 * runs headless).
 *
 * The window runs on the process's main thread (SDL wants its window,
 * events and drawing there on macOS), and the emulated machine on a thread
 * of its own (display_main()): the machine hands over what the window shows
 * (its screen, a frame at a time, the pointer, the status bar, the menu's
 * and prompts' contents) and takes the window's input from a queue, and the
 * window converts, draws and presents without stopping it. The functions
 * below but display_main() and the pure ones are the machine's, called
 * from its thread.
 */
#ifndef BLUE_DISPLAY_H
#define BLUE_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui.h"
#include "video.h"

struct display;

/*
 * Runs body(arg) on a thread of its own, the machine's, while this thread
 * (the main one) runs the window it opens, if it opens one; returns what
 * body returned.
 */
int display_main(int (*body)(void *), void *arg);

enum display_event_type {
	DISPLAY_EVENT_NONE,
	DISPLAY_EVENT_QUIT,             /* quit now: Blue was told to (SIGINT, SIGTERM) */
	DISPLAY_EVENT_CLOSE,            /* the user asks to quit: the window's close button (or the window
					 * manager's Close), Quit Blue in the menu bar or the Dock, logging out
					 * of macOS (the machine may ask first) */
	DISPLAY_EVENT_KEY,              /* adb_key, down */
	DISPLAY_EVENT_MOUSE_MOVE,       /* x, y (guest screen pixels), dx, dy */
	DISPLAY_EVENT_MOUSE_BUTTON,     /* button, down */
	DISPLAY_EVENT_RESIZE,           /* x, y: the screen size that fills the window at its scale (resized by
					 * the user, the scale or the display's density changed, or the status bar
					 * came or went in a window that kept its size) */
	DISPLAY_EVENT_RELEASE_MOUSE,    /* the user pressed Control and Option (Ctrl and Alt) together */
	DISPLAY_EVENT_MENU,             /* the menu's to open or close (F12; or Esc or Resume, open) */
	DISPLAY_EVENT_MENU_ACTION,      /* something chosen in the menu: action (but Resume, Fullscreen, Status bar);
					 * a drive's Insert...: path, the image chosen (NULL: the host has no file
					 * chooser), until the next event */
	DISPLAY_EVENT_DROP_FILE,        /* a file was dropped on the window: path (until the next event) */
	DISPLAY_EVENT_CLIPBOARD,        /* the host's clipboard may have changed (or the window got the focus) */
	DISPLAY_EVENT_WHEEL,            /* the scroll wheel: dx, dy (right and up, as the host has it), in
					 * 1/WHEEL_LINE lines (wheel.h) */
	/* game controllers (SDL's), each by Blue's number for it, pad (0 to PAD_MAX - 1) */
	DISPLAY_EVENT_PAD_ADDED,        /* pad, path: its name (until the next event), guid: SDL's GUID ("": none);
					 * value 1 if it's a virtual one (display_pad_virtual()) */
	DISPLAY_EVENT_PAD_REMOVED,      /* pad */
	DISPLAY_EVENT_PAD_BUTTON,       /* pad, button (pad.h's, SDL's), down */
	DISPLAY_EVENT_PAD_AXIS,         /* pad, button: the axis (pad.h's), value (-32768 to 32767; triggers 0 up) */
};

struct display_event {
	enum display_event_type type;
	int x, y, dx, dy;
	int button;                     /* 0 = primary, 1 middle, 2 secondary */
	bool down;
	uint8_t adb_key;                /* ADB key code */
	enum ui_action action;          /* (UI_PAD_EDIT: the type's id in path, the change in button, the control in
					 * value, the stick's way or the action, as a 'pipp' SwitchData's word, in x) */
	const char *path;
	int pad, value;
	char guid[33];
};

/*
 * Returns NULL if no window can be opened (no display, SDL2 missing, or
 * display_main() isn't running the main thread: display_failure() says
 * why). The screen is shown at a
 * scale, `scale` whole pixels of the display to each of its pixels
 * (DISPLAY_SCALE_AUTO: Auto, display_auto_scale() for the display the
 * window is on; the menu changes it): the window takes the screen's size
 * at it (within the display), and a window resized asks for the screen its
 * own size in pixels over the scale (DISPLAY_EVENT_RESIZE). A screen of
 * that size is drawn at that scale; others are scaled to fill the window,
 * keeping their shape, and with `integer` by whole numbers only.
 */
#define DISPLAY_SCALE_AUTO 0
#define DISPLAY_SCALE_MAX 4
#define DISPLAY_SCALE_OWN (DISPLAY_SCALE_MAX + 1)       /* the display's own: a screen pixel to a unit, as the menu's */
struct display *display_open(const char *title, unsigned width, unsigned height, bool integer, unsigned scale);
const char *display_failure(void);
void display_close(struct display *d);

/*
 * Command-key shortcuts: all Mac OS's (as a window opens; see menubar.h),
 * or on macOS Command-Q and Command-H the menu bar's, quitting and hiding
 * Blue (the window shown without Mac OS)
 */
void display_shortcuts(struct display *d, bool host_keys);

/*
 * Whether the screen follows the window's size (the guest tools' Blue
 * Enhancements runs, which switches to the size asked for): then a new
 * scale changes the screen; otherwise the window
 */
void display_follow(struct display *d, bool on);

/*
 * Auto: the whole number of a display's pixels to a screen pixel (1 to
 * DISPLAY_SCALE_MAX) that brings the screen nearest, in proportion, to 80
 * pixels per inch, as a 1999 Mac's: from the display's pixels per inch, or
 * if that isn't known (0, or out of reason) its own scale, `backing`
 * pixels to the window's unit, rounded.
 */
unsigned display_auto_scale(double ppi, double backing);

/*
 * The screen a window w x h pixels (above the status bar) shows at a
 * scale: its pixels over the scale, rounded down (video_fit_size() rounds
 * that to what the driver takes)
 */
void display_screen_for(int w, int h, unsigned scale, int *sw, int *sh);

/*
 * The screen, as the window has it: a copy of the framebuffer in its own
 * format (rows of `row_bytes` bytes, and the palette that converts them
 * for 8 bits a pixel and fewer), which the machine brings up to date from
 * display_frame_begin() to display_frame_end(), marking the rows it
 * changes; meanwhile the window doesn't look at it. The window then shows
 * the rows marked (all of it if the size changed). The copy keeps what was
 * in it, unless the size or row length changed (`kept` false: then it's
 * all to be written).
 */
struct display_frame {
	uint8_t *pixels;
	uint8_t *changed;               /* a byte for each row: set when it has changed */
	bool kept;
	struct video_palette palette;
};
struct display_frame *display_frame_begin(struct display *d, unsigned width, unsigned height, unsigned bpp,
					  size_t row_bytes);
void display_frame_end(struct display *d);

/*
 * The pointer, drawn over the screen (Mac OS's hardware cursor): where its
 * image's top left is in screen pixels, and its pixels (0xFFRRGGBB, or
 * clear, or inverting the screen), copied when `image` changes. NULL: none.
 */
#define DISPLAY_CURSOR_MAX 16
#define DISPLAY_CURSOR_CLEAR 0x00000000u
#define DISPLAY_CURSOR_INVERT 0x00FFFFFFu
struct display_cursor {
	bool visible;
	int x, y;
	unsigned width, height;         /* (at most DISPLAY_CURSOR_MAX) */
	const uint32_t *pixels;
	unsigned long image;
};
void display_cursor(struct display *d, const struct display_cursor *c);

/*
 * Where a screen goes in a window out_w x out_h pixels in size: as large
 * as fits keeping its shape (with `integer`, a whole multiple of its size),
 * centred. Whole multiples are drawn exactly (nearest neighbour); other
 * sizes are drawn at the whole multiple above, then filtered down, so that
 * each screen pixel stays a sharp, evenly sized square. A screen the
 * window's own at `scale` (display_screen_for(), the width to 8 pixels) is
 * drawn at exactly that, with a border if need be.
 */
struct display_view {
	unsigned screen_w, screen_h;
	int x, y, w, h;                 /* where it's drawn, in the window's pixels */
	unsigned prescale;              /* 0: drawn directly; else drawn at this multiple first */
};
struct display_view display_view(unsigned screen_w, unsigned screen_h, int out_w, int out_h, bool integer,
				 unsigned scale);

/* The screen pixel at (px, py) in the window's pixels (off the screen in its borders). */
void display_view_point(const struct display_view *v, double px, double py, int *x, int *y);

/* Waits up to `ns` nanoseconds for input; true if some is waiting (it isn't taken). */
bool display_wait(struct display *d, uint64_t ns);

/*
 * Captures the host pointer (hidden, kept in the window, while the window
 * has the focus) so that mouse moves report only motion (dx, dy), or lets
 * it go. The window's title says how to let it go meanwhile.
 */
void display_capture_mouse(struct display *d, bool on);

/*
 * Opens or closes the menu over the screen (Dear ImGui's; with SDL older
 * than 2.0.18, there's none and the title says the guest's paused). While
 * it's open, input goes to it, and the window draws it as it changes. It
 * toggles fullscreen and the status bar and sets the scale itself; the rest
 * of what's chosen comes back as DISPLAY_EVENT_MENU_ACTION.
 */
void display_menu(struct display *d, bool open);

/* What the menu shows of the machine: the mouse, and news (copied; NULL for none) */
void display_menu_state(struct display *d, enum ui_mouse mouse, const char *status);

/* ... and of the drives with removable media (copied; NULL: there are none) */
void display_drives_state(struct display *d, const struct ui_drives *drives);

/* ... and Install Guest Tools...: whether it's there, and why it's disabled (copied; NULL: it isn't) */
void display_tools_state(struct display *d, bool shown, const char *why_not);

/* ... and the game controllers (copied) */
void display_pads_state(struct display *d, const struct ui_pads *pads);

/* ... and the modem: what it's doing (copied; NULL: there's none), and whether it has a call to hang up */
void display_modem_state(struct display *d, const char *text, bool call);

/*
 * Game controllers: SDL's (2.0.14 and later) virtual ones, for scripted
 * runs in the window, which then come and go and send their buttons and
 * axes as a real one would: false if SDL can't make them. Virtual pad n
 * (0 to PAD_MAX - 1) is plugged in (`value` 1) or out (0), or button or
 * axis `which` (pad.h's) is set to `value` (a button's 0 or 1; an axis's
 * -32768 to 32767, a trigger's from 0).
 */
enum display_pad_op { DISPLAY_PAD_PLUG, DISPLAY_PAD_BUTTON, DISPLAY_PAD_AXIS };
bool display_pad_virtual(struct display *d, enum display_pad_op op, unsigned n, unsigned which, int value);

/*
 * Shows a question or news over the screen, with one or two buttons (`no`
 * may be NULL; all are copied), until one is chosen (DISPLAY_EVENT_MENU_ACTION,
 * UI_PROMPT_YES or UI_PROMPT_NO; Esc is no) or it's taken away (text NULL).
 * Meanwhile input goes to it, but the guest runs on. False if it can't be
 * shown (no menu).
 */
bool display_prompt(struct display *d, const char *text, const char *yes, const char *no);

/* A third button for the question shown, on the left (UI_PROMPT_ALT): Don't Ask Again */
void display_prompt_alt(struct display *d, const char *label);

/*
 * The status bar (with the menu's SDL only), below the screen: shown or
 * hidden (the menu toggles it too); and what it shows.
 */
void display_status_bar(struct display *d, bool on);
void display_status(struct display *d, const struct ui_status *s);

/*
 * The host clipboard's text (UTF-8, to free(); NULL if SDL can't say), and
 * setting it (the window's thread does both, as SDL requires).
 */
char *display_clipboard(struct display *d);
void display_set_clipboard(struct display *d, const char *text);

/* Returns false when there are no more events. */
bool display_poll(struct display *d, struct display_event *ev);

/* Maps a USB HID usage (SDL scancode) to an ADB key code; 0xFF if none. */
uint8_t display_hid_to_adb(unsigned usage);

/*
 * BLUE_WINDOW_PROFILE: the host time drawing and presenting have taken so
 * far on the window's thread, in nanoseconds of the host clock and of that
 * thread's processor time, by part; the frames it took (with changes) and
 * how many times it drew the window
 */
enum { DISPLAY_TIME_CONVERT, DISPLAY_TIME_UPLOAD, DISPLAY_TIME_DRAW, DISPLAY_TIME_PRESENT, DISPLAY_TIME_EVENTS,
       DISPLAY_TIMES };
struct display_times {
	unsigned long frames;
	unsigned long draws;
	uint64_t ns[DISPLAY_TIMES], cpu_ns[DISPLAY_TIMES];
	uint64_t latency_ns, latency_max_ns;    /* from frames' ends (display_frame_end()) to their presenting */
};
void display_times(struct display *d, struct display_times *t);

#endif
