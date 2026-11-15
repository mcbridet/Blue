/*
 * A shared folder (`--share DIR`): a host folder shown to Mac OS as an HFS
 * volume of its own, named after it, with its files and subfolders. The
 * volume's metadata (MDB, bitmap, catalog) is made in memory at the start;
 * its files' forks are read from the host's files when Mac OS reads them.
 * Mac OS's writes go to an overlay (a sparse temporary file), never to the
 * folder during the run. At the end, if Mac OS put the volume away cleanly
 * (shut down), what it changed is written back to the folder: new, changed,
 * renamed, moved and deleted files and folders, with resource forks and
 * Finder info as the host keeps them (macOS: the file's own resource fork
 * and com.apple.FinderInfo; elsewhere AppleDouble "._" files). Host files
 * that Mac OS deleted or replaced are moved to DIR/.blue-recovery/<date>
 * rather than destroyed; if Mac OS didn't put the volume away, nothing is
 * written back and the volume is saved there as a disk image instead.
 */
#ifndef BLUE_SHARE_H
#define BLUE_SHARE_H

#include "disk.h"

#define SHARE_RECOVERY ".blue-recovery"

struct share;

/*
 * Builds the volume for a folder (saying what it leaves out, and why it
 * fails). `start` is where the guest's clock starts (Unix time; 0: the
 * host's, now): no date on the volume is later. NULL on failure.
 */
struct share *share_open(const char *dir, int64_t start);

/* The drive's blocks: base and overlay */
const struct disk_backend *share_disk(struct share *s);

/*
 * Writes back what Mac OS changed (or keeps it for recovery), says what was
 * done, and frees the share. Returns 0, or -1 if something couldn't be
 * written back (it is then in the recovery folder).
 */
int share_close(struct share *s);

#endif
