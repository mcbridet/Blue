/*
 * Where Blue runs from and what it says without a command: the executable
 * found, the command shown for it (quoted for the shell, the AppImage's
 * path in one, ~ for the home directory), and the libraries' line in
 * --version.
 */
#include <stdlib.h>
#include <string.h>

#include "../src/hostlib.h"
#include "../src/welcome.h"
#include "test.h"

static void quoted(const char *word, const char *expected)
{
	char out[256];
	welcome_quote(word, out, sizeof out);
	if (strcmp(out, expected) != 0) {
		fprintf(stderr, "%s:%d: welcome_quote(%s) = %s, expected %s\n", __FILE__, __LINE__, word, out, expected);
		test_failures++;
	}
}

/* The command shown, with APPIMAGE and HOME set so */
static void command(const char *appimage, const char *home, const char *expected)
{
	char *old_appimage = getenv("APPIMAGE") ? strdup(getenv("APPIMAGE")) : NULL;
	char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("APPIMAGE", appimage, 1);
	setenv("HOME", home, 1);
	char out[512];
	bool moved = true;
	CHECK(welcome_command(out, sizeof out, &moved));
	CHECK(!moved);
	if (strcmp(out, expected) != 0) {
		fprintf(stderr, "%s:%d: command for %s = %s, expected %s\n", __FILE__, __LINE__, appimage, out, expected);
		test_failures++;
	}
	if (old_appimage)
		setenv("APPIMAGE", old_appimage, 1);
	else
		unsetenv("APPIMAGE");
	if (old_home)
		setenv("HOME", old_home, 1);
	else
		unsetenv("HOME");
	free(old_appimage);
	free(old_home);
}

void test_welcome(void)
{
	quoted("/Applications/Blue.app/Contents/MacOS/blue", "/Applications/Blue.app/Contents/MacOS/blue");
	quoted("./Blue-x86_64.AppImage", "./Blue-x86_64.AppImage");
	quoted("/Volumes/My Disk/Blue.app", "'/Volumes/My Disk/Blue.app'");
	quoted("/home/o'brien/Blue-aarch64.AppImage", "'/home/o'\\''brien/Blue-aarch64.AppImage'");
	quoted("~/x", "'~/x'");
	quoted("", "''");

	command("/home/ann/Downloads/Blue-x86_64.AppImage", "/home/ann", "~/Downloads/Blue-x86_64.AppImage");
	command("/home/ann/My Apps/Blue-x86_64.AppImage", "/home/ann", "'/home/ann/My Apps/Blue-x86_64.AppImage'");
	command("/opt/Blue-aarch64.AppImage", "/home/ann", "/opt/Blue-aarch64.AppImage");
	command("/home/annie/Blue-x86_64.AppImage", "/home/ann", "/home/annie/Blue-x86_64.AppImage");

	char exe[1024];
	CHECK(hostlib_executable(exe, sizeof exe));
	CHECK(exe[0] == '/');
	const char *leaf = strrchr(exe, '/');
	CHECK(leaf && strcmp(leaf, "/blue-tests") == 0);

	/* (found or not, a line naming it: SDL2 and libslirp aren't needed to build or test) */
	char about[1200];
	hostlib_about(HOSTLIB_SDL2, about, sizeof about);
	CHECK(strncmp(about, "SDL2", 4) == 0);
	hostlib_about(HOSTLIB_SLIRP, about, sizeof about);
	CHECK(strncmp(about, "libslirp", 8) == 0);
}
