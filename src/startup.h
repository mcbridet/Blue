/*
 * The startup disk, as a New World Mac's firmware picks it, and the Mac OS
 * ROM file in its blessed System Folder (a file of type 'tbxi'), which Blue
 * starts: the drive and partition Open Firmware's boot-device names (in
 * NVRAM, where Mac OS 9.1 and later's Startup Disk control panel puts it:
 * see ofnvram.h), if it's attached and has one; otherwise the first drive
 * that has one, in command-line order (an image's partitions in map order).
 */
#ifndef BLUE_STARTUP_H
#define BLUE_STARTUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hfsread.h"
#include "rom.h"

struct startup {
	unsigned drive, part;           /* where the ROM file came from: the drive (its index), its partition */
	bool chosen;                    /* ... the one boot-device names */
	char from[HFSREAD_PATH_MAX + 256];      /* "\"Vol:System Folder:Mac OS ROM\" from IMAGE, partition \"NAME\"" */
};

/*
 * The drive boot-device names in `nvram` (its index among Blue's drives, as
 * their PCI device numbers go) and its partition's map entry (0: none).
 * False if boot-device isn't set, or isn't one of Blue's drives.
 */
bool startup_choice(const uint8_t *nvram, size_t size, unsigned *drive, unsigned *map_entry);

/*
 * Loads the ROM file from the startup disk: `paths` are the drives' images
 * in command-line order (NULL for an empty drive), `nvram` NVRAM's bytes (or
 * NULL). Returns 0, 1 if no drive has a ROM file, or -1 (having said why).
 * `note` says what became of boot-device's choice when it wasn't taken
 * (else it's empty).
 */
int startup_rom(const char *const *paths, unsigned count, const uint8_t *nvram, size_t size, struct rom *rom,
		struct startup *s, char *note, size_t notelen);

/*
 * A Bandai Pippin's disc among the drives (which, with no Mac OS ROM file,
 * has nothing to start: a Pippin's ROM is the console's): a volume with a
 * PippinAuthenticationFile at its root, or whose blessed System Folder's
 * system is the Pippin's (it has the "Pippin" enabler, type 'gbly'). True
 * if there's one, with the first one's drive and volume name (UTF-8).
 */
bool startup_pippin(const char *const *paths, unsigned count, unsigned *drive, char *name, size_t len);

#endif
