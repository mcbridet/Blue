/*
 * Blue's settings file: what the user chose in the window's menu, kept
 * between runs. It's text, in the platform's place for such things:
 *
 *   macOS   ~/Library/Application Support/Blue/settings.ini
 *   Linux   $XDG_CONFIG_HOME/blue/settings.ini (~/.config/blue/settings.ini)
 *
 * Lines are "name = value" under a [section] (names and sections ignore
 * case; # starts a comment line). For now it holds the game controllers'
 * mappings (pad.h): [controller] the one for all pads, and [controller ID]
 * a pad type's own, ID its SDL GUID (32 hex digits) or, without one, its
 * name; a section's "name" is the pad's, and each control's line what it
 * does ("a = Return", "leftstickmode = Mouse": see pad_action_parse()). A
 * control without a line keeps Blue's default. Sections and lines Blue
 * doesn't know are kept as they are when it writes the file.
 *
 * Only runs in a window, in real time and without an input script, use the
 * file unless told which (--settings FILE, or none), so that other runs
 * come out the same wherever they're made. It's written whole each time
 * something changes: to a new file beside it, then renamed over it.
 */
#ifndef BLUE_SETTINGS_H
#define BLUE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#include "pad.h"

#define SETTINGS_PADS 32                /* pad types with mappings of their own */
#define SETTINGS_ID 64
#define SETTINGS_OTHER (16u << 10)      /* what Blue doesn't know, kept */

struct settings_pad {
	char id[SETTINGS_ID];           /* SDL's GUID, or the pad's name */
	char name[PAD_NAME];
	struct pad_map map;
};

struct settings {
	struct pad_map all;             /* the mapping for pads without their own */
	unsigned pads;
	struct settings_pad pad[SETTINGS_PADS];
	char other[SETTINGS_OTHER];     /* other sections, as they were */
};

/* Where the file is, by default (false: no home to put it in) */
bool settings_default_path(char *out, size_t size);

void settings_init(struct settings *s);

/*
 * Reads settings from text (over Blue's defaults); lines it can't make sense
 * of are said in `warn` (one, the first: "" if none) and left out.
 */
void settings_parse(struct settings *s, const char *text, char *warn, size_t warn_size);

/* ... and writes them: the text's length (as snprintf, so a longer one didn't fit) */
size_t settings_format(const struct settings *s, char *out, size_t size);

/*
 * The file: read (a missing one is Blue's defaults: 0), or written (its
 * folders made if need be): 0, or -1 with why in `err`
 */
int settings_load(struct settings *s, const char *path, char *err, size_t err_size);
int settings_save(const struct settings *s, const char *path, char *err, size_t err_size);

/* A pad type's mapping, by its GUID ("" or NULL: none) or else its name: its own (`own` true), or the one for all */
const struct pad_map *settings_pad_map(const struct settings *s, const char *guid, const char *name, bool *own);

/* ... set (a mapping of its own: `map`; NULL: none, it takes the one for all); false if there's no room */
bool settings_set_pad_map(struct settings *s, const char *guid, const char *name, const struct pad_map *map);

#endif
