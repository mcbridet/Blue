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
#define EXAMPLE " boot --cdrom \"Mac OS 9.iso\" --window"

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

/* The window's background: Blue's desktop (the site's), a pattern of 2-pixel squares */
static void desktop(struct display *d, unsigned w, unsigned h)
{
	struct display_frame *f = display_frame_begin(d, w, h, 32, (size_t)w * 4);
	if (!f)
		return;
	f->palette.bpp = 32;
	for (unsigned y = 0; y < h; y++) {
		uint8_t *p = f->pixels + (size_t)y * w * 4;
		for (unsigned x = 0; x < w; x++, p += 4) {
			bool light = ((x ^ y) >> 1) & 1;
			p[0] = 0;               /* (the Mac's 32-bit pixels: xRGB, big-endian) */
			p[1] = light ? 0x0B : 0x00;
			p[2] = light ? 0x8E : 0x89;
			p[3] = light ? 0xAC : 0xA8;
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

struct welcome {
	char text[1024];
	char example[PATH_MAX + 64];
};

static bool prompt(struct display *d, const struct welcome *w, const char *copy)
{
	if (!display_prompt(d, w->text, "Quit", NULL))
		return false;
	display_prompt_alt(d, copy);
	return true;
}

static int window(void *arg)
{
	const struct welcome *w = arg;
	struct display *d = display_open("Blue", WIDTH, HEIGHT, false, DISPLAY_SCALE_OWN);
	if (!d)
		return -1;
	unsigned width = WIDTH, height = HEIGHT, frames = 3;
	display_follow(d, true);
	display_shortcuts(d, true);     /* (there's no Mac OS for Command-Q to go to) */
	desktop(d, width, height);
	if (!prompt(d, w, "Copy Command")) {
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
			else if (ev.type == DISPLAY_EVENT_MENU_ACTION && ev.action == UI_PROMPT_ALT) {
				display_set_clipboard(d, w->example);
				prompt(d, w, "Copied");
				frames = 3;
			} else if (ev.type == DISPLAY_EVENT_RESIZE && ev.x > 0 && ev.y > 0 && ev.x <= 8192 && ev.y <= 8192) {
				width = (unsigned)ev.x;
				height = (unsigned)ev.y;
				desktop(d, width, height);
				frames = 3;
			}
		}
	}
	display_close(d);
	return 0;
}

int welcome(void)
{
	static struct welcome w;
	char command[PATH_MAX + 8];
	bool moved;
	if (!welcome_command(command, sizeof command, &moved))
		snprintf(command, sizeof command, "blue");
	snprintf(w.example, sizeof w.example, "%s" EXAMPLE, command);
#ifdef __APPLE__
	const char *terminal = moved ? "copy Blue to Applications, then in Terminal" : "in Terminal";
#else
	const char *terminal = "in a terminal";
#endif
	snprintf(w.text, sizeof w.text,
		 "Blue is started from the command line for now; a launcher is coming. To start up "
		 "from a Mac OS CD image (8.5 to 9.2.2), %s:\n\n"
		 "%.500s\n\n"
		 "Without a command, it lists them all. The README tells how to install Mac OS on a "
		 "disk image.\n\n"
		 BLUE_ABOUT,
		 terminal, w.example);
	return display_main(window, &w);
}
