/*
 * Drives with removable media, host side (see disk.h): which drive an
 * image goes in (dropped on the window, or put in by an input script),
 * their names, and what the window's menu shows of them. nk.c puts images
 * in and says what came of it.
 */
#ifndef SYSHM_DRIVES_H
#define SYSHM_DRIVES_H

#include <stdbool.h>
#include <stddef.h>

#include "disk.h"
#include "ui.h"

#define DRIVES_MAX_CDS 4

struct drives {
	struct disk *floppy;            /* the floppy drive (NULL: none) */
	struct disk *cd[DRIVES_MAX_CDS];        /* the CD-ROM drives whose discs can be swapped, in order */
	unsigned cds;
};

/* What an image is to go in: whatever it is (a floppy's size or not), or as asked */
enum drives_for { DRIVES_ANY, DRIVES_FLOPPY, DRIVES_CD };

/*
 * The drive an image goes in: images of a floppy's size (2880K or less) in
 * the floppy drive, others (and cue sheets) in a CD-ROM drive, an empty one or else one
 * whose disc Mac OS isn't using. NULL, with why in `why`, if it can't go
 * in (no such drive, Mac OS using the disc in it, not an image it takes).
 */
struct disk *drives_pick(const struct drives *dr, const char *path, enum drives_for kind, char *why, size_t len);

/* A drive's name in messages: "floppy drive", "CD-ROM drive", "CD-ROM drive 2" */
const char *drives_name(const struct drives *dr, const struct disk *d, char *buf, size_t len);

/*
 * What the menu shows: each drive (the floppy drive first) with its disc
 * and whether Mac OS uses it, and the floppy images that go in next.
 * order[k] is the menu's drive k (for its Eject buttons).
 */
void drives_menu(const struct drives *dr, const char *const *queue, unsigned queued, struct ui_drives *out,
		 struct disk *order[UI_DRIVES]);

#endif
