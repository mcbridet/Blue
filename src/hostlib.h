/*
 * The libraries Blue loads at run time, so that it needs none to build or
 * to start: SDL2 for the window and sound, libslirp for the network,
 * libusb-1.0 for the host's USB devices passed through to Mac OS. Each
 * is looked for first among Blue's own copies of them, beside the
 * executable (../Frameworks from its directory: Blue.app's
 * Contents/Frameworks; ../lib elsewhere: an AppImage's usr/lib), then as
 * the system finds it (and on macOS in Homebrew's and MacPorts'
 * directories, which bare names aren't looked for in). BLUE_LOG_LIBS=1
 * says which file each came from; BLUE_LIBUSB names a libusb to use
 * instead (a test's stand-in for it).
 */
#ifndef BLUE_HOSTLIB_H
#define BLUE_HOSTLIB_H

#include <stdbool.h>
#include <stddef.h>

enum hostlib { HOSTLIB_SDL2, HOSTLIB_SLIRP, HOSTLIB_USB, HOSTLIBS };

/* dlopen()'s handle for it, or NULL (dlerror() says why) */
void *hostlib_open(enum hostlib which);

/* Its version and the file it comes from, or that it isn't found, in a line (for --version) */
void hostlib_about(enum hostlib which, char *out, size_t size);

/* The executable's path, links resolved; false if it can't be found */
bool hostlib_executable(char *out, size_t size);

#endif
