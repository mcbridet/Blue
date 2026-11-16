/*
 * What Blue shows when it has no Mac to show: opened with nothing to do
 * and no terminal to say so in (Blue.app or the AppImage double-clicked),
 * a window saying that Blue is started from the command line for now, with
 * the command for this copy of it; and when `boot` finds no Mac OS startup
 * disk, why, over a black screen (the Mac's, with nothing to start). Each
 * until Quit, with a Copy Command button where there's a command to copy.
 */
#ifndef BLUE_WELCOME_H
#define BLUE_WELCOME_H

#include <stdbool.h>
#include <stddef.h>

/* The exit status; -1 if no window could be opened */
int welcome(void);

/*
 * How to run this copy of Blue from a terminal: its path, quoted if need
 * be (the AppImage's, in one; ~ for the home directory where it can be).
 * On macOS, if it runs from somewhere read-only (its disk image, or where
 * macOS moved it to run once), where it would be in Applications, and
 * `moved` set. False if the executable can't be found.
 */
bool welcome_command(char *out, size_t size, bool *moved);

/* A word for the shell: as it is if it needs no quoting, else in single quotes */
void welcome_quote(const char *word, char *out, size_t size);

/* What `boot` was given, with no startup disk among it */
struct welcome_boot {
	const char *blue;               /* the command for Blue (welcome_command()'s) */
	int argc;                       /* boot's arguments */
	char *const *argv;
	bool images;                    /* disk or CD images were given */
	const char *pippin;             /* a Pippin disc's volume name (startup_pippin()), or NULL */
	const char *pippin_image;       /* ... its image, one of argv */
};

/*
 * What to say: a Pippin disc's own words (its titles run inside Mac OS:
 * start Blue with a Mac OS disk as well), with the command that adds one
 * to boot's arguments (`command`; the disc in a CD-ROM drive), or that
 * there's no startup disk, and why, with a pointer to the README's Quick
 * start (`command` empty).
 */
void welcome_no_startup(const struct welcome_boot *b, char *text, size_t text_len, char *command, size_t command_len);

/*
 * Shows that text over a black screen, with Copy Command if there's a
 * command (NULL: none), until Quit. Returns 0, or -1 if no window could
 * be opened.
 */
int welcome_notice(const char *text, const char *command);

#endif
