/*
 * Blue Pippin Library: Blue's own PippinPPC, the shared library of the
 * Bandai Pippin's system software (its "Pippin" enabler's 'nlib' 777), with
 * Apple's exports, for Pippin titles that link to it. Its code is the
 * library's own and calls Mac OS through InterfaceLib (and AppearanceLib);
 * it makes no host calls (the host call in the builder's stubs, which no
 * export uses, only identifies Blue's copy: see install.c).
 *
 * On a Pippin each call is a selector of the enabler's trap $AA6F (Pippin.h,
 * Pippin SDK 1.1); here they map onto Mac OS 8.5 to 9.2.2:
 *
 *   PippinFindFolder          finds (or makes) the title's folder: on a
 *                             Pippin at the root of its 120 KB of flash or of
 *                             a floppy, here in a folder "Pippin" in the
 *                             Preferences folder; named as the Pippin names it,
 *                             by the title's 'pipn' 1 resource (a Pascal
 *                             string at offset 12), else the application's name
 *   PippinStandardGetFile     StandardGetFile, PippinStandardPutFile
 *   PippinStandardPutFile     StandardPutFile, starting in the title's folder;
 *                             the reply's vRefNum a working directory for the
 *                             file's folder (Standard File's, 'ERIK'), so that
 *                             titles that ignore its parID (Marathon's
 *                             vRefNum and directory 0) find the file there
 *   PippinStandardDeleteFile  a file chosen with StandardGetFile (from the
 *                             title's folder), deleted once a caution alert
 *                             has been answered Delete; the space it took in
 *                             *bytesRequested
 *   MakeWritesPermanent       FlushVol of the Preferences folder's volume, and
 *                             of where PippinStandardPutFile's last file went
 *   GetDriveRefNum            the Preferences folder's volume (kPippinStorage-
 *                             Device), 0 for the floppy (there's no dock)
 *   HasFloppyDock, HasMountedFloppy                       false
 *   InvokeSoftKeyboard, SavePRAMtoFlash, WriteNVExtraPRAM nothing (noErr)
 *   ReadNVExtraPRAM           zeros
 *
 * While a title that links to it runs (from the library's initialization to
 * its termination), Gestalt answers 'pipn' (gestaltPippinAttr, 0): titles
 * check it, and some won't run without it ("this game is only for the
 * Pippin"). And the title's own patch of _ShutDown makes ShutDwnPower and
 * ShutDwnStart return at once: a Pippin turns off when its title quits,
 * which here would shut Mac OS down. (An application's patches are its own:
 * the Process Manager swaps them in and out with it.)
 */
#ifndef BLUE_PIPPIN_H
#define BLUE_PIPPIN_H

#include <stddef.h>
#include <stdint.h>

#include "ndrv.h"

#define PIPPIN_EXPORTS 12
#define PIPPIN_FOLDER "Pippin"          /* in the Preferences folder: the titles' folders */
#define PIPPIN_LIBRARY_MAX (16u << 10)

/* Apple's exports, in its order; export k is entry PIPPIN_FIRST_EXPORT + k */
extern const char *const pippin_exports[PIPPIN_EXPORTS];
#define PIPPIN_FIRST_EXPORT 2           /* (entries 0 and 1: initialization and termination) */

/* The library's imports (InterfaceLib's, then AppearanceLib's), in its order */
enum {
	PIPPIN_FIND_FOLDER, PIPPIN_PB_GET_CAT_INFO_SYNC, PIPPIN_DIR_CREATE, PIPPIN_FLUSH_VOL, PIPPIN_STANDARD_GET_FILE,
	PIPPIN_STANDARD_PUT_FILE, PIPPIN_FSP_DELETE, PIPPIN_GET_RESOURCE, PIPPIN_RELEASE_RESOURCE, PIPPIN_BLOCK_MOVE,
	PIPPIN_BLOCK_MOVE_DATA, PIPPIN_NEW_GESTALT_VALUE, PIPPIN_DELETE_GESTALT_VALUE, PIPPIN_NGET_TRAP_ADDRESS,
	PIPPIN_NSET_TRAP_ADDRESS, PIPPIN_OPEN_WD, PIPPIN_STANDARD_ALERT, PIPPIN_IMPORTS
};
extern const struct ndrv_import pippin_imports[PIPPIN_IMPORTS];

/* The fragment; returns its size (0 if `size` is too small) */
size_t pippin_build(uint8_t *out, size_t size, struct ndrv_layout *layout);

#endif
