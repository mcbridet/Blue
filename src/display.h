/*
 * Host display window and input, using SDL2 loaded at run time (no build
 * dependency; if the library or a display isn't available, the emulator
 * runs headless).
 */
#ifndef SYSHM_DISPLAY_H
#define SYSHM_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "ui.h"

struct display;

enum display_event_type {
	DISPLAY_EVENT_NONE,
	DISPLAY_EVENT_QUIT,
	DISPLAY_EVENT_KEY,              /* adb_key, down */
	DISPLAY_EVENT_MOUSE_MOVE,       /* x, y (guest screen pixels), dx, dy */
	DISPLAY_EVENT_MOUSE_BUTTON,     /* button, down */
	DISPLAY_EVENT_RESIZE,           /* x, y: the screen size that fills the window (resized by the user,
					 * or the status bar came or went in a window that kept its size) */
	DISPLAY_EVENT_EXPOSE,           /* the window needs drawing again (shown, or its size changed) */
	DISPLAY_EVENT_RELEASE_MOUSE,    /* the user pressed Control and Option (Ctrl and Alt) together */
	DISPLAY_EVENT_MENU,             /* the menu's to open or close (F12; or Esc or Resume, open) */
	DISPLAY_EVENT_MENU_ACTION,      /* something chosen in the menu: action (but Resume, Fullscreen, Status bar) */
};

struct display_event {
	enum display_event_type type;
	int x, y, dx, dy;
	int button;                     /* 0 = primary */
	bool down;
	uint8_t adb_key;                /* ADB key code */
	enum ui_action action;
};

/*
 * Returns NULL (with a message on stderr) if no window can be opened. The
 * window has one unit of its size (a point, on high-density displays) per
 * screen pixel and scales the screen to fill it. With `integer`, the screen
 * is only ever drawn at a whole multiple of its size, and the window's
 * natural size for a screen is the multiple nearest the display's scale.
 */
struct display *display_open(const char *title, unsigned width, unsigned height, bool integer);
void display_close(struct display *d);

/* Shows an RGB image (3 bytes per pixel); the window follows size changes. */
void display_present(struct display *d, const uint8_t *rgb, unsigned width, unsigned height);

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

/* Waits up to `ms` milliseconds for input; true if some is waiting (it isn't taken). */
bool display_wait(struct display *d, unsigned ms);

/*
 * Captures the host pointer (hidden, kept in the window, while the window
 * has the focus) so that mouse moves report only motion (dx, dy), or lets
 * it go. The window's title says how to let it go meanwhile.
 */
void display_capture_mouse(struct display *d, bool on);

/*
 * Opens or closes the menu over the screen (Dear ImGui's; with SDL older
 * than 2.0.18, there's none and the title says the guest's paused). While
 * it's open, input goes to it, and display_refresh() draws it again. It
 * toggles fullscreen and the status bar itself; the rest of what's chosen
 * comes back as DISPLAY_EVENT_MENU_ACTION.
 */
void display_menu(struct display *d, bool open);
void display_refresh(struct display *d);

/* What the menu shows of the machine: the mouse, and news (copied; NULL for none) */
void display_menu_state(struct display *d, enum ui_mouse mouse, const char *status);

/*
 * The status bar (with the menu's SDL only), below the screen: shown or
 * hidden (the menu toggles it too); what it shows, drawn when the window
 * next is (by display_present() or display_update()); and drawing the
 * window again if that's changed (and the bar is shown).
 */
void display_status_bar(struct display *d, bool on);
void display_status(struct display *d, const struct ui_status *s);
void display_update(struct display *d);

/* Returns false when there are no more events. */
bool display_poll(struct display *d, struct display_event *ev);

/* Maps a USB HID usage (SDL scancode) to an ADB key code; 0xFF if none. */
uint8_t display_hid_to_adb(unsigned usage);

#endif
