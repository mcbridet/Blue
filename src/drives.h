/*
 * Drives with removable media, host side (see disk.h): which drive an
 * image goes in (dropped on the window, or put in by an input script),
 * their names, and what the window's menu shows of them; and the shared
 * folder's drive, whose volume is written back and made again as Mac OS
 * puts it away (see share.h). nk.c puts images in and says what came of
 * it.
 */
#ifndef BLUE_DRIVES_H
#define BLUE_DRIVES_H

#include <stdbool.h>
#include <stddef.h>

#include "disk.h"
#include "ui.h"

#define DRIVES_MAX_CDS 4

struct share;

struct drives {
	struct disk *floppy;            /* the floppy drive (NULL: none) */
	struct disk *cd[DRIVES_MAX_CDS];        /* the CD-ROM drives whose discs can be swapped, in order */
	unsigned cds;
	struct disk *share;             /* the shared folder's drive, if its disk comes and goes (NULL: none) */
	struct share *folder;           /* ... and the folder */
};

/* What an image is to go in: whatever it is (a floppy's size or not), or as asked */
enum drives_for { DRIVES_ANY, DRIVES_FLOPPY, DRIVES_CD };

/*
 * The drive an image goes in: images of a floppy's size (2880K or less, a
 * compressed one's unpacked) in the floppy drive, others (and images with
 * tracks, cue sheets and their like) in a CD-ROM drive, an empty one or else one
 * whose disc Mac OS isn't using. NULL, with why in `why`, if it can't go
 * in (no such drive, Mac OS using the disc in it, not an image it takes).
 */
struct disk *drives_pick(const struct drives *dr, const char *path, enum drives_for kind, char *why, size_t len);

/* ... or in drive `d` (the menu's Insert...): d, or NULL with why (not an image it takes, Mac OS using its disc) */
struct disk *drives_pick_in(const struct drives *dr, struct disk *d, const char *path, char *why, size_t len);

/* A drive's name in messages: "floppy drive", "CD-ROM drive", "CD-ROM drive 2" */
const char *drives_name(const struct drives *dr, const struct disk *d, char *buf, size_t len);

/*
 * What the menu shows: each drive (the floppy drive first) with its disc
 * and whether Mac OS uses it, and the floppy images that go in next; the
 * shared folder's volume, whether it can be refreshed (not while Mac OS
 * has files open on it), and whether the folder has changed. order[k] is
 * the menu's drive k (for its Eject buttons).
 */
void drives_menu(const struct drives *dr, const char *const *queue, unsigned queued, struct ui_drives *out,
		 struct disk *order[UI_DRIVES]);

/*
 * Mac OS ejected the shared folder's volume (Put Away, Eject): if it put
 * it away first (unmounted it: it doesn't have it off line), what it
 * changed is written back, the volume made again from the folder as it is
 * now (`now`: the guest's clock, Unix time), and in the drive again as a
 * new disk. If it didn't, nothing is written back and the same disk goes
 * back in. True if it was written back.
 */
bool drives_share_ejected(struct drives *dr, int64_t now);

/*
 * The shared folder's volume made again while Mac OS doesn't have it
 * mounted (it was put away without being ejected, it hasn't been mounted
 * yet, or it couldn't be made before): written back first if Mac OS put it
 * away. False, `why` saying why, if Mac OS has it (or has it off line).
 */
bool drives_share_remake(struct drives *dr, int64_t now, char *why, size_t len);

#endif
