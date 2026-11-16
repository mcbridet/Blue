#include "welcome.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#include "display.h"
#include "hostlib.h"
#include "version.h"

#define WIDTH 600u
#define HEIGHT 360u
#define EXAMPLE " boot --cdrom \"Mac OS 9.iso\""

void welcome_quote(const char *word, char *out, size_t size)
{
	bool plain = *word != 0;
	for (const char *c = word; *c && plain; c++)
		plain = isalnum((unsigned char)*c) || strchr("/._+,:@%-", *c);
	if (plain) {
		snprintf(out, size, "%s", word);
		return;
	}
	size_t n = 0;
	if (size < 3) {
		if (size)
			*out = 0;
		return;
	}
	out[n++] = '\'';
	for (const char *c = word; *c; c++) {
		const char *put = *c == '\'' ? "'\\''" : (char[]){ *c, 0 };
		size_t len = strlen(put);
		if (n + len + 2 > size)
			break;
		memcpy(out + n, put, len);
		n += len;
	}
	out[n++] = '\'';
	out[n] = 0;
}

bool welcome_command(char *out, size_t size, bool *moved)
{
	char path[PATH_MAX];
	*moved = false;
	const char *appimage = getenv("APPIMAGE");
	if (appimage && *appimage && strlen(appimage) < sizeof path)
		strcpy(path, appimage);
	else if (!hostlib_executable(path, sizeof path))
		return false;
#ifdef __APPLE__
	/* (its disk image, or where Gatekeeper put an app not yet moved, to run it once) */
	struct statvfs fs;
	const char *app = strstr(path, ".app/Contents/MacOS/");
	if (app && (strstr(path, "/AppTranslocation/") || (statvfs(path, &fs) == 0 && (fs.f_flag & ST_RDONLY)))) {
		const char *name = app;
		while (name > path && name[-1] != '/')
			name--;
		char in_apps[PATH_MAX];
		snprintf(in_apps, sizeof in_apps, "/Applications/%s", name);
		strcpy(path, in_apps);
		*moved = true;
	}
#endif
	const char *home = getenv("HOME");
	size_t n = home ? strlen(home) : 0;
	if (n > 1 && strncmp(path, home, n) == 0 && path[n] == '/') {
		char rest[PATH_MAX + 8];
		welcome_quote(path + n + 1, rest, sizeof rest);
		if (strcmp(rest, path + n + 1) == 0) {
			snprintf(out, size, "~/%s", rest);
			return true;
		}
	}
	welcome_quote(path, out, size);
	return true;
}

/* The window's background: Blue's desktop (the site's), a pattern of 2-pixel squares; or the Mac's screen, black */
static void background(struct display *d, unsigned w, unsigned h, bool black)
{
	struct display_frame *f = display_frame_begin(d, w, h, 32, (size_t)w * 4);
	if (!f)
		return;
	f->palette.bpp = 32;
	for (unsigned y = 0; y < h; y++) {
		uint8_t *p = f->pixels + (size_t)y * w * 4;
		for (unsigned x = 0; x < w; x++, p += 4) {
			bool light = !black && ((x ^ y) >> 1) & 1;
			p[0] = 0;               /* (the Mac's 32-bit pixels: xRGB, big-endian) */
			p[1] = black ? 0 : light ? 0x0B : 0x00;
			p[2] = black ? 0 : light ? 0x8E : 0x89;
			p[3] = black ? 0 : light ? 0xAC : 0xA8;
		}
		f->changed[y] = 1;
	}
	display_frame_end(d);
}

/* The same frame again: the window draws a prompt as frames come (Dear ImGui sizes a box in its first) */
static void again(struct display *d, unsigned w, unsigned h)
{
	if (display_frame_begin(d, w, h, 32, (size_t)w * 4))
		display_frame_end(d);
}

struct notice {
	const char *text;
	const char *command;            /* what Copy Command copies (NULL: there's no such button) */
	bool black;                     /* over a black screen (else Blue's desktop) */
	unsigned width, height;
};

static bool prompt(struct display *d, const struct notice *n, const char *copy)
{
	if (!display_prompt(d, n->text, "Quit", NULL))
		return false;
	if (n->command)
		display_prompt_alt(d, copy);
	return true;
}

static int window(void *arg)
{
	const struct notice *n = arg;
	struct display *d = display_open("Blue", n->width, n->height, false, DISPLAY_SCALE_OWN);
	if (!d)
		return -1;
	unsigned width = n->width, height = n->height, frames = 3;
	display_follow(d, true);
	display_shortcuts(d, true);     /* (there's no Mac OS for Command-Q to go to) */
	background(d, width, height, n->black);
	if (!prompt(d, n, "Copy Command")) {
		display_close(d);
		return -1;
	}
	for (bool done = false; !done;) {
		display_wait(d, frames ? 30000000u : 1000000000u);
		if (frames) {
			again(d, width, height);
			frames--;
		}
		struct display_event ev;
		while (display_poll(d, &ev)) {
			if (ev.type == DISPLAY_EVENT_QUIT || ev.type == DISPLAY_EVENT_CLOSE ||
			    (ev.type == DISPLAY_EVENT_MENU_ACTION && ev.action == UI_PROMPT_YES))
				done = true;
			else if (ev.type == DISPLAY_EVENT_MENU_ACTION && ev.action == UI_PROMPT_ALT && n->command) {
				display_set_clipboard(d, n->command);
				prompt(d, n, "Copied");
				frames = 3;
			} else if (ev.type == DISPLAY_EVENT_RESIZE && ev.x > 0 && ev.y > 0 && ev.x <= 8192 && ev.y <= 8192) {
				width = (unsigned)ev.x;
				height = (unsigned)ev.y;
				background(d, width, height, n->black);
				frames = 3;
			}
		}
	}
	display_close(d);
	return 0;
}

int welcome(void)
{
	static char text[1024], example[PATH_MAX + 64];
	char command[PATH_MAX + 8];
	bool moved;
	if (!welcome_command(command, sizeof command, &moved))
		snprintf(command, sizeof command, "blue");
	snprintf(example, sizeof example, "%s" EXAMPLE, command);
#ifdef __APPLE__
	const char *terminal = moved ? "copy Blue to Applications, then in Terminal" : "in Terminal";
#else
	const char *terminal = "in a terminal";
#endif
	snprintf(text, sizeof text,
		 "Blue is started from the command line for now; a launcher is coming. To start up "
		 "from a Mac OS CD image (8.5 to 9.2.2), %s:\n\n"
		 "%.500s\n\n"
		 "Without a command, it lists them all. The README tells how to install Mac OS on a "
		 "disk image.\n\n"
		 BLUE_ABOUT,
		 terminal, example);
	static struct notice n = { text, example, false, WIDTH, HEIGHT };
	return display_main(window, &n);
}

/* Appends a word to a command line, quoted for the shell if need be (`raw`: as it is) */
static void append(char *out, size_t len, const char *word, bool raw)
{
	char quoted[PATH_MAX + 64];
	size_t at = strlen(out);
	if (raw)
		snprintf(quoted, sizeof quoted, "%s", word);
	else
		welcome_quote(word, quoted, sizeof quoted);
	snprintf(out + at, len > at ? len - at : 0, "%s%s", at ? " " : "", quoted);
}

void welcome_no_startup(const struct welcome_boot *b, char *text, size_t text_len, char *command, size_t command_len)
{
	command[0] = '\0';
	if (b->pippin) {
		/* boot's arguments, with a Mac OS disk before the drives given, and the disc as a CD */
		bool added = false;
		append(command, command_len, b->blue, true);
		append(command, command_len, "boot", true);
		for (int k = 0; k < b->argc; k++) {
			const char *a = b->argv[k];
			bool drive = !strcmp(a, "--disk") || !strcmp(a, "--cdrom") || !strcmp(a, "--cdrom-drive");
			if (drive && !added) {
				append(command, command_len, "--disk <your Mac OS disk>", true);
				added = true;
			}
			if (!strcmp(a, "--disk") && k + 1 < b->argc && b->argv[k + 1] == b->pippin_image)
				a = "--cdrom";
			append(command, command_len, a, a[0] == '-' && a[1] == '-');
		}
		if (!added)
			append(command, command_len, "--disk <your Mac OS disk>", true);
		snprintf(text, text_len,
			 "\"%.200s\" is a Pippin disc. Pippin titles run inside Mac OS: start Blue with your Mac OS "
			 "8.5 to 9.2.2 disk as well, and the disc will be in its CD drive:\n\n%s", b->pippin, command);
	} else if (b->images) {
		snprintf(text, text_len,
			 "No Mac OS startup disk: none of the disks or CDs has a System Folder with a Mac OS ROM "
			 "file.\n\nStart Blue with a Mac OS CD (8.5 to 9.2.2), or a disk Mac OS is installed on (or "
			 "give a ROM file with --rom FILE). The README's Quick start tells how: "
			 "https://github.com/mcbridet/Blue#quick-start");
	} else {
		snprintf(text, text_len,
			 "No Mac OS startup disk: Blue was given no disks or CDs to start up from.\n\nStart Blue "
			 "with a Mac OS CD (8.5 to 9.2.2) with --cdrom, or a disk Mac OS is installed on with "
			 "--disk. The README's Quick start tells how: https://github.com/mcbridet/Blue#quick-start");
	}
}

int welcome_notice(const char *text, const char *command)
{
	static struct notice n;
	n = (struct notice){ text, command, true, 640, 480 };
	return display_main(window, &n);
}
