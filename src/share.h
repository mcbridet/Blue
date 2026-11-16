/*
 * A shared folder (`--share DIR`): a host folder shown to Mac OS as an HFS
 * volume of its own, named after it, with its files and subfolders. The
 * volume's metadata (MDB, bitmap, catalog) is made in memory at the start;
 * its files' forks are read from the host's files when Mac OS reads them.
 * Mac OS's writes go to an overlay (a sparse temporary file), never to the
 * folder while Mac OS has the volume. Once Mac OS has put the volume away
 * cleanly (Put Away, Eject, Shut Down), what it changed is written back to
 * the folder: new, changed, renamed, moved and deleted files and folders,
 * with resource forks and Finder info as the host keeps them (macOS: the
 * file's own resource fork and com.apple.FinderInfo; elsewhere AppleDouble
 * "._" files). Host files that Mac OS deleted or replaced are moved to
 * DIR/.blue-recovery/<date> rather than destroyed; if Mac OS didn't put the
 * volume away by the end of the run, nothing is written back and the
 * volume is saved there as a disk image instead. Written back during the
 * run (Mac OS ejected it), the volume is made again from the folder as it
 * is then, to be offered to Mac OS as a new disk.
 */
#ifndef BLUE_SHARE_H
#define BLUE_SHARE_H

#include <stdbool.h>

#include "disk.h"

#define SHARE_RECOVERY ".blue-recovery"

struct share;

/*
 * Builds the volume for a folder (saying what it leaves out, and why it
 * fails). `start` is where the guest's clock starts (Unix time; 0: the
 * host's, now): no date on the volume is later. NULL on failure.
 */
struct share *share_open(const char *dir, int64_t start);

/* The drive's blocks: base and overlay (NULL while there's no volume: see share_rebuild()) */
const struct disk_backend *share_disk(struct share *s);

/* The volume's name (UTF-8), and the folder */
const char *share_name(const struct share *s);
const char *share_folder(const struct share *s);

/* Whether Mac OS has put the volume away: its MDB written, saying it was unmounted cleanly */
bool share_put_away(struct share *s);

/*
 * Mac OS has put the volume away during the run: writes back what it
 * changed, as share_close() does, and says what was done. Not again until
 * the volume is made again (nor at share_close()). Nothing is done unless
 * share_put_away(). Returns 0, or -1 if something couldn't be written back
 * (it is then in the recovery folder).
 */
int share_write_back(struct share *s);

/*
 * Makes the volume again from the folder as it is now (`start`: the
 * guest's clock now, as for share_open()), as new: what Mac OS wrote on the
 * old one is gone (written back first: share_write_back()). Returns 0, or
 * -1 if it can't be made (the folder's gone, say): then there's no volume.
 */
int share_rebuild(struct share *s, int64_t start);

/*
 * Whether the folder has changed since the volume was made: a file Mac OS
 * would see came, went or changed (looked at now; what Mac OS doesn't see,
 * such as dot files, doesn't count).
 */
bool share_host_changed(const struct share *s);

/* The drive's icons (where its disk comes and goes), as floppy_icons() gives a floppy drive's */
void share_icons(uint8_t *media_icn, uint8_t *drive_icn, uint8_t *media_icns, uint8_t *drive_icns);

/*
 * Writes back what Mac OS changed (or keeps it for recovery), unless that
 * was done already (share_write_back()), says what was done, and frees the
 * share. Returns 0, or -1 if something couldn't be written back (it is then
 * in the recovery folder).
 */
int share_close(struct share *s);

#endif
