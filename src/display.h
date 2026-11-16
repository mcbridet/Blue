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
	DISPLAY_EVENT_QUIT,
	DISPLAY_EVENT_KEY,              /* adb_key, down */
	DISPLAY_EVENT_MOUSE_MOVE,       /* x, y (guest screen pixels), dx, dy */
	DISPLAY_EVENT_MOUSE_BUTTON,     /* button, down */
	DISPLAY_EVENT_RESIZE,           /* x, y: the screen size that fills the window (resized by the user,
					 * or the status bar came or went in a window that kept its size) */
	DISPLAY_EVENT_RELEASE_MOUSE,    /* the user pressed Control and Option (Ctrl and Alt) together */
	DISPLAY_EVENT_MENU,             /* the menu's to open or close (F12; or Esc or Resume, open) */
	DISPLAY_EVENT_MENU_ACTION,      /* something chosen in the menu: action (but Resume, Fullscreen, Status bar) */
	DISPLAY_EVENT_DROP_FILE,        /* a file was dropped on the window: path (until the next event) */
	DISPLAY_EVENT_CLIPBOARD,        /* the host's clipboard may have changed (or the window got the focus) */
};

struct display_event {
	enum display_event_type type;
	int x, y, dx, dy;
	int button;                     /* 0 = primary */
	bool down;
	uint8_t adb_key;                /* ADB key code */
	enum ui_action action;
	const char *path;
};

/*
 * Returns NULL (with a message on stderr) if no window can be opened (or
 * display_main() isn't running the main thread). The window has one unit of
 * its size (a point, on high-density displays) per screen pixel and scales
 * the screen to fill it. With `integer`, the screen is only ever drawn at a
 * whole multiple of its size, and the window's natural size for a screen is
 * the multiple nearest the display's scale.
 */
struct display *display_open(const char *title, unsigned width, unsigned height, bool integer);
void display_close(struct display *d);

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
 * each screen pixel stays a sharp, evenly sized square.
 */
struct display_view {
	unsigned screen_w, screen_h;
	int x, y, w, h;                 /* where it's drawn, in the window's pixels */
	unsigned prescale;              /* 0: drawn directly; else drawn at this multiple first */
};
struct display_view display_view(unsigned screen_w, unsigned screen_h, int out_w, int out_h, bool integer);

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
 * toggles fullscreen and the status bar itself; the rest of what's chosen
 * comes back as DISPLAY_EVENT_MENU_ACTION.
 */
void display_menu(struct display *d, bool open);

/* What the menu shows of the machine: the mouse, and news (copied; NULL for none) */
void display_menu_state(struct display *d, enum ui_mouse mouse, const char *status);

/* ... and of the drives with removable media (copied; NULL: there are none) */
void display_drives_state(struct display *d, const struct ui_drives *drives);

/* ... and Install Guest Tools...: whether it's there, and why it's disabled (copied; NULL: it isn't) */
void display_tools_state(struct display *d, bool shown, const char *why_not);

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
