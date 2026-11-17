#define _GNU_SOURCE                     /* dladdr() */
#include "hostlib.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef __APPLE__
#define BUNDLED_DIR "Frameworks"
#else
#define BUNDLED_DIR "lib"
#endif

struct hostlib_names {
	const char *name;               /* (for messages) */
	const char *symbol;             /* one of its functions, to find the file by */
	const char *bundled;            /* Blue's copy's file name */
	const char *const *system;      /* ... and the system's, in the order they're tried */
	size_t count;
};

/* (bare names aren't looked for in Homebrew's or MacPorts' directories on macOS) */
static const char *const sdl2_names[] = { "libSDL2-2.0.so.0", "libSDL2.so", "libSDL2-2.0.dylib",
					  "/opt/homebrew/lib/libSDL2-2.0.dylib", "/opt/local/lib/libSDL2-2.0.dylib",
					  "SDL2.framework/SDL2" };
static const char *const slirp_names[] = { "libslirp.so.0", "libslirp.dylib", "/opt/homebrew/lib/libslirp.dylib",
					   "/opt/local/lib/libslirp.dylib" };

static const char *const usb_names[] = { "libusb-1.0.so.0", "libusb-1.0.0.dylib", "/opt/homebrew/lib/libusb-1.0.0.dylib",
					 "/usr/local/lib/libusb-1.0.0.dylib", "/opt/local/lib/libusb-1.0.0.dylib" };

static const struct hostlib_names libs[HOSTLIBS] = {
#ifdef __APPLE__
	[HOSTLIB_SDL2] = { "SDL2", "SDL_Init", "libSDL2-2.0.0.dylib", sdl2_names, sizeof sdl2_names / sizeof *sdl2_names },
	[HOSTLIB_SLIRP] = { "libslirp", "slirp_new", "libslirp.0.dylib", slirp_names,
			    sizeof slirp_names / sizeof *slirp_names },
	[HOSTLIB_USB] = { "libusb", "libusb_init", "libusb-1.0.0.dylib", usb_names, sizeof usb_names / sizeof *usb_names },
#else
	[HOSTLIB_SDL2] = { "SDL2", "SDL_Init", "libSDL2-2.0.so.0", sdl2_names, sizeof sdl2_names / sizeof *sdl2_names },
	[HOSTLIB_SLIRP] = { "libslirp", "slirp_new", "libslirp.so.0", slirp_names,
			    sizeof slirp_names / sizeof *slirp_names },
	[HOSTLIB_USB] = { "libusb", "libusb_init", "libusb-1.0.so.0", usb_names, sizeof usb_names / sizeof *usb_names },
#endif
};

bool hostlib_executable(char *out, size_t size)
{
	char path[PATH_MAX];
#ifdef __APPLE__
	uint32_t n = sizeof path;
	if (_NSGetExecutablePath(path, &n) != 0)
		return false;
#else
	ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
	if (n <= 0)
		return false;
	path[n] = 0;
#endif
	char real[PATH_MAX];
	if (!realpath(path, real) || strlen(real) >= size)
		return false;
	strcpy(out, real);
	return true;
}

/* Blue's own copy: ../Frameworks or ../lib from the executable's directory */
static void *open_bundled(const struct hostlib_names *l)
{
	char exe[PATH_MAX], path[PATH_MAX + 64];
	if (!hostlib_executable(exe, sizeof exe))
		return NULL;
	char *slash = strrchr(exe, '/');
	if (!slash)
		return NULL;
	*slash = 0;
	char dir[PATH_MAX + 16];
	snprintf(dir, sizeof dir, "%s/../" BUNDLED_DIR, exe);
	char real[PATH_MAX];
	if (!realpath(dir, real))
		return NULL;
	snprintf(path, sizeof path, "%s/%s", real, l->bundled);
	return access(path, F_OK) == 0 ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
}

/* The file a library was loaded from ("": not known) */
static const char *file_of(void *lib, const struct hostlib_names *l)
{
	Dl_info info;
	void *sym = dlsym(lib, l->symbol);
	return sym && dladdr(sym, &info) && info.dli_fname ? info.dli_fname : "";
}

void *hostlib_open(enum hostlib which)
{
	const struct hostlib_names *l = &libs[which];
	void *lib = which == HOSTLIB_USB && getenv("BLUE_LIBUSB") ? dlopen(getenv("BLUE_LIBUSB"), RTLD_NOW | RTLD_LOCAL) :
		    open_bundled(l);
	for (size_t k = 0; k < l->count && !lib; k++)
		lib = dlopen(l->system[k], RTLD_NOW | RTLD_LOCAL);
	if (lib && getenv("BLUE_LOG_LIBS"))
		fprintf(stderr, "libs: %s from %s\n", l->name, file_of(lib, l));
	return lib;
}

void hostlib_about(enum hostlib which, char *out, size_t size)
{
	const struct hostlib_names *l = &libs[which];
	void *lib = hostlib_open(which);
	if (!lib) {
		snprintf(out, size, "%s: not found (%s)", l->name,
			 which == HOSTLIB_SDL2 ? "no window or sound" : which == HOSTLIB_SLIRP ? "no network" :
			 "no USB devices from the host");
		return;
	}
	char version[32] = "";
	if (which == HOSTLIB_SDL2) {
		void (*get)(uint8_t v[3]);
		*(void **)&get = dlsym(lib, "SDL_GetVersion");
		uint8_t v[3];                   /* (an SDL_version) */
		if (get) {
			get(v);
			snprintf(version, sizeof version, " %u.%u.%u", v[0], v[1], v[2]);
		}
	} else if (which == HOSTLIB_SLIRP) {
		const char *(*get)(void);
		*(void **)&get = dlsym(lib, "slirp_version_string");
		if (get)
			snprintf(version, sizeof version, " %s", get());
	} else {
		const uint16_t *(*get)(void);  /* (a struct libusb_version: major, minor, micro, nano, ...) */
		*(void **)&get = dlsym(lib, "libusb_get_version");
		const uint16_t *v = get ? get() : NULL;
		if (v)
			snprintf(version, sizeof version, " %u.%u.%u", v[0], v[1], v[2]);
	}
	snprintf(out, size, "%s%s: %s", l->name, version, file_of(lib, l));
	dlclose(lib);
}
