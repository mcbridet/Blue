/*
 * What Blue shows when it's opened with nothing to do and no terminal to
 * say so in (Blue.app or the AppImage double-clicked): a window saying
 * that Blue is started from the command line for now, with the command
 * for this copy of it, until Quit.
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

#endif
