#include "settings.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILE_NAME "settings.ini"
#define SECTION "controller"

bool settings_default_path(char *out, size_t size)
{
	const char *home = getenv("HOME");
#ifdef __APPLE__
	if (!home || home[0] != '/')
		return false;
	return (size_t)snprintf(out, size, "%s/Library/Application Support/Blue/" FILE_NAME, home) < size;
#else
	const char *config = getenv("XDG_CONFIG_HOME");
	if (config && config[0] == '/')
		return (size_t)snprintf(out, size, "%s/blue/" FILE_NAME, config) < size;
	if (!home || home[0] != '/')
		return false;
	return (size_t)snprintf(out, size, "%s/.config/blue/" FILE_NAME, home) < size;
#endif
}

void settings_init(struct settings *s)
{
	memset(s, 0, sizeof *s);
	s->all = pad_default_map;
}

static char *trim(char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	size_t n = strlen(p);
	while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r' || p[n - 1] == '\n'))
		p[--n] = '\0';
	return p;
}

/* A pad type's entry, by its ID (made if `make`; NULL if there's no room) */
static struct settings_pad *pad_entry(struct settings *s, const char *id, bool make)
{
	for (unsigned k = 0; k < s->pads; k++)
		if (!strcasecmp(s->pad[k].id, id))
			return &s->pad[k];
	if (!make || s->pads == SETTINGS_PADS || !id[0] || strlen(id) >= SETTINGS_ID)
		return NULL;
	struct settings_pad *p = &s->pad[s->pads++];
	memset(p, 0, sizeof *p);
	snprintf(p->id, sizeof p->id, "%s", id);
	p->map = pad_default_map;
	return p;
}

static void warn_once(char *warn, size_t size, unsigned line, const char *why)
{
	if (warn && size && !warn[0])
		snprintf(warn, size, "line %u: %s", line, why);
}

/* A line of a controller section: "name = value" */
static void pad_line(struct pad_map *map, char *pad_name, char *line, unsigned n, char *warn, size_t warn_size)
{
	char *eq = strchr(line, '=');
	if (!eq) {
		warn_once(warn, warn_size, n, "not \"name = value\"");
		return;
	}
	*eq = '\0';
	char *name = trim(line), *value = trim(eq + 1);
	for (char *c = name; *c; c++)
		*c = (char)tolower((unsigned char)*c);
	if (!strcmp(name, "name")) {
		if (pad_name)
			snprintf(pad_name, PAD_NAME, "%s", value);
		return;
	}
	if (!strcmp(name, "leftstickmode") || !strcmp(name, "rightstickmode")) {
		unsigned stick = name[0] == 'r';
		if (!strcasecmp(value, "keys"))
			map->stick[stick] = PAD_STICK_KEYS;
		else if (!strcasecmp(value, "mouse"))
			map->stick[stick] = PAD_STICK_MOUSE;
		else
			warn_once(warn, warn_size, n, "a stick is Keys or Mouse");
		return;
	}
	int c = pad_control_named(name);
	struct pad_switch a;
	if (c < 0)
		warn_once(warn, warn_size, n, "not a controller's button, stick or trigger");
	else if (!pad_action_parse(value, &a))
		warn_once(warn, warn_size, n, "not a key, a click or Nothing");
	else
		map->control[c] = a;
}

/* Keeps a line of a section Blue doesn't know, as it was */
static void keep(struct settings *s, size_t *kept, const char *raw, unsigned n, char *warn, size_t warn_size)
{
	size_t len = strlen(raw);
	while (len && (raw[len - 1] == '\r' || raw[len - 1] == '\n'))
		len--;
	if (*kept + len + 2 > sizeof s->other) {
		warn_once(warn, warn_size, n, "too much Blue doesn't know");
		return;
	}
	memcpy(s->other + *kept, raw, len);
	*kept += len;
	s->other[(*kept)++] = '\n';
	s->other[*kept] = '\0';
}

void settings_parse(struct settings *s, const char *text, char *warn, size_t warn_size)
{
	if (warn && warn_size)
		warn[0] = '\0';
	struct pad_map *map = NULL;     /* (in a controller section: its mapping, and the pad's name) */
	char *pad_name = NULL;
	bool other = false;             /* ... in another section: its lines are kept */
	size_t kept = strlen(s->other);
	unsigned n = 0;
	for (const char *at = text; *at;) {
		const char *end = strchr(at, '\n');
		size_t len = end ? (size_t)(end - at) : strlen(at);
		char raw[512], buf[512];
		n++;
		if (len >= sizeof raw) {
			warn_once(warn, warn_size, n, "too long");
			at += len + (end != NULL);
			continue;
		}
		memcpy(raw, at, len);
		raw[len] = '\0';
		at += len + (end != NULL);
		memcpy(buf, raw, len + 1);
		char *line = trim(buf);
		if (line[0] == '[') {
			char *close = strchr(line, ']');
			if (!close) {
				warn_once(warn, warn_size, n, "a section without its ]");
				map = NULL;
				other = false;
				continue;
			}
			*close = '\0';
			char *inside = trim(line + 1);
			size_t sn = strlen(SECTION);
			map = NULL;
			pad_name = NULL;
			other = false;
			if (!strncasecmp(inside, SECTION, sn) && (!inside[sn] || inside[sn] == ' ')) {
				char *id = trim(inside + sn);
				struct settings_pad *p = id[0] ? pad_entry(s, id, true) : NULL;
				if (id[0] && !p) {
					warn_once(warn, warn_size, n, "too many controllers, or too long a name");
					continue;
				}
				map = p ? &p->map : &s->all;
				pad_name = p ? p->name : NULL;
				continue;
			}
			other = true;
		}
		if (other) {
			keep(s, &kept, raw, n, warn, warn_size);
			continue;
		}
		if (!line[0] || line[0] == '#' || line[0] == ';')
			continue;
		if (!map) {
			warn_once(warn, warn_size, n, "not in a [section]");
			continue;
		}
		pad_line(map, pad_name, line, n, warn, warn_size);
	}
}

/* A mapping's lines */
static size_t format_map(const struct pad_map *map, char *out, size_t size)
{
	size_t n = 0;
	for (unsigned c = 0; c < PAD_CONTROLS; c++) {
		char what[64];
		if (c == PAD_STICK_UP || c == PAD_RSTICK_UP) {
			bool left = c == PAD_STICK_UP;
			n += (size_t)snprintf(out + (n < size ? n : size), n < size ? size - n : 0, "%sstickmode = %s\n",
					      left ? "left" : "right",
					      map->stick[!left] == PAD_STICK_MOUSE ? "Mouse" : "Keys");
		}
		pad_action_name(&map->control[c], what, sizeof what);
		n += (size_t)snprintf(out + (n < size ? n : size), n < size ? size - n : 0, "%s = %s\n",
				      pad_control_name(c), what);
	}
	return n;
}

size_t settings_format(const struct settings *s, char *out, size_t size)
{
	size_t n = 0;
#define PUT(...) (n += (size_t)snprintf(out + (n < size ? n : size), n < size ? size - n : 0, __VA_ARGS__))
	PUT("# Blue's settings, written by Blue as they're changed in its menu (F12): see its\n"
	    "# README, \"Keeping your settings\". Lines are \"name = value\" under a [section];\n"
	    "# a line starting with # is a comment.\n");
	size_t other = strlen(s->other);
	while (other && s->other[other - 1] == '\n')
		other--;
	if (other)
		PUT("\n%.*s\n", (int)other, s->other);
	PUT("\n[" SECTION "]\n"
	    "# Game controllers as the keyboard and mouse: what each button, stick and\n"
	    "# trigger does, for all controllers without a mapping of their own below\n");
	n += format_map(&s->all, out + (n < size ? n : size), n < size ? size - n : 0);
	for (unsigned k = 0; k < s->pads; k++) {
		PUT("\n[" SECTION " %s]\n", s->pad[k].id);
		if (s->pad[k].name[0])
			PUT("name = %s\n", s->pad[k].name);
		n += format_map(&s->pad[k].map, out + (n < size ? n : size), n < size ? size - n : 0);
	}
#undef PUT
	if (size)
		out[n < size ? n : size - 1] = '\0';
	return n;
}

int settings_load(struct settings *s, const char *path, char *err, size_t err_size)
{
	settings_init(s);
	if (err && err_size)
		err[0] = '\0';
	FILE *f = fopen(path, "rb");
	if (!f) {
		if (errno == ENOENT)
			return 0;
		snprintf(err, err_size, "%s: %s", path, strerror(errno));
		return -1;
	}
	size_t cap = 64u << 10, len = 0;
	char *text = malloc(cap + 1);
	size_t got;
	while (text && (got = fread(text + len, 1, cap - len, f)) > 0) {
		len += got;
		if (len == cap) {
			char *more = cap < (4u << 20) ? realloc(text, 2 * cap + 1) : NULL;
			if (!more) {
				free(text);
				text = NULL;
				break;
			}
			text = more;
			cap *= 2;
		}
	}
	bool bad = ferror(f) || !text;
	fclose(f);
	if (bad) {
		free(text);
		snprintf(err, err_size, "%s: cannot read it", path);
		return -1;
	}
	text[len] = '\0';
	char warn[160];
	settings_parse(s, text, warn, sizeof warn);
	free(text);
	if (warn[0])
		snprintf(err, err_size, "%s, %s (left out)", path, warn);
	return 0;
}

/* The folders above `path`, made if need be */
static int make_folders(const char *path)
{
	char dir[4096];
	if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir)
		return -1;
	char *slash = strrchr(dir, '/');
	if (!slash || slash == dir)
		return 0;
	*slash = '\0';
	for (char *p = dir + 1; ; p++) {
		if (*p == '/' || !*p) {
			char c = *p;
			*p = '\0';
			if (mkdir(dir, 0755) && errno != EEXIST)
				return -1;
			*p = c;
			if (!c)
				break;
		}
	}
	return 0;
}

int settings_save(const struct settings *s, const char *path, char *err, size_t err_size)
{
	size_t len = settings_format(s, NULL, 0);
	char *text = malloc(len + 1), tmp[4096];
	if (!text) {
		snprintf(err, err_size, "%s: no memory", path);
		return -1;
	}
	settings_format(s, text, len + 1);
	int fd = -1;
	if (make_folders(path) || snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp ||
	    (fd = mkstemp(tmp)) < 0) {
		snprintf(err, err_size, "%s: %s", path, strerror(errno));
		free(text);
		return -1;
	}
	bool ok = true;
	for (size_t at = 0; ok && at < len;) {
		ssize_t w = write(fd, text + at, len - at);
		if (w < 0 && errno == EINTR)
			continue;
		ok = w > 0;
		at += w > 0 ? (size_t)w : 0;
	}
	int why = errno;
	ok = ok && fchmod(fd, 0644) == 0 && fsync(fd) == 0;
	why = ok ? 0 : why ? why : errno;
	if (close(fd) && ok) {
		ok = false;
		why = errno;
	}
	free(text);
	if (ok && rename(tmp, path)) {
		ok = false;
		why = errno;
	}
	if (!ok) {
		unlink(tmp);
		snprintf(err, err_size, "%s: %s", path, strerror(why ? why : EIO));
		return -1;
	}
	return 0;
}

const struct pad_map *settings_pad_map(const struct settings *s, const char *guid, const char *name, bool *own)
{
	const struct settings_pad *p = NULL;
	for (unsigned pass = 0; pass < 2 && !p; pass++) {
		const char *id = pass ? name : guid;
		for (unsigned k = 0; id && id[0] && k < s->pads && !p; k++)
			if (!strcasecmp(s->pad[k].id, id))
				p = &s->pad[k];
	}
	if (own)
		*own = p != NULL;
	return p ? &p->map : &s->all;
}

bool settings_set_pad_map(struct settings *s, const char *guid, const char *name, const struct pad_map *map)
{
	const char *id = guid && guid[0] ? guid : name ? name : "";
	if (!map) {
		/* (by either: a name's entry, say, made before SDL gave a GUID) */
		for (unsigned pass = 0; pass < 2; pass++) {
			const char *by = pass ? name : guid;
			struct settings_pad *p;
			while (by && by[0] && (p = pad_entry(s, by, false)))
				*p = s->pad[--s->pads];
		}
		return true;
	}
	struct settings_pad *p = pad_entry(s, id, true);
	if (!p)
		return false;
	if (name && name[0])
		snprintf(p->name, sizeof p->name, "%s", name);
	p->map = *map;
	return true;
}
