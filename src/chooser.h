/*
 * The host's own file chooser, for the window's menu (a drive's Insert...):
 * on macOS an NSOpenPanel (chooser_mac.m), elsewhere zenity's or kdialog's,
 * whichever is on the PATH. Neither the window's thread, which opens it,
 * nor the machine's waits for it: the window looks again (chooser_poll())
 * when it's woken.
 */
#ifndef BLUE_CHOOSER_H
#define BLUE_CHOOSER_H

#include <stdbool.h>

struct chooser;

enum chooser_state {
	CHOOSER_WAITING,
	CHOOSER_CHOSEN,                 /* a file */
	CHOOSER_CANCELLED,
	CHOOSER_FAILED,                 /* (it couldn't be shown, or went wrong) */
};

/*
 * Opens a chooser for a file, saying what for (`message`), on the window's
 * thread; `wake(arg)` is called, from any thread, once it's done. NULL if
 * the host has none.
 */
struct chooser *chooser_open(const char *message, void (*wake)(void *), void *arg);

/* Whether it's done; if so (not CHOOSER_WAITING) it's gone, and *path is the file chosen (to free) or NULL */
enum chooser_state chooser_poll(struct chooser *c, char **path);

/* Closes it unfinished (the window is going): `wake` isn't called after */
void chooser_close(struct chooser *c);

/* zenity's or kdialog's (chooser_open()'s but on macOS): NULL if neither is on the PATH */
struct chooser *chooser_spawn(const char *message, void (*wake)(void *), void *arg);

/* (for chooser_mac.m: a chooser, waiting; and its outcome, `path` to be freed with it) */
struct chooser *chooser_new(void (*wake)(void *), void *arg, void *panel);
void chooser_done(struct chooser *c, enum chooser_state state, char *path);
void chooser_cancel_panel(void *panel);

#endif
