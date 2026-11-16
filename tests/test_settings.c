/*
 * Blue's settings file (settings.h): the mappings of the game controllers
 * read and written (and what Blue doesn't know kept), a pad type's found by
 * its GUID or its name, the default for the rest; the file written whole,
 * to a new file then renamed, in folders made for it; where it goes.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/settings.h"
#include "test.h"

static const char text[] =
	"# someone's comment\n"
	"[Launcher]\n"
	"  window = 800x600   # (Blue doesn't know it)\n"
	"\n"
	"[controller]\n"
	"a = Space\n"
	"B = command-Q\n"
	"leftstickmode = mouse\n"
	"\n"
	"[controller 030000005e040000e002000003090000]\n"
	"name = Xbox Wireless Controller\n"
	"x = Control-Click\n"
	"rightstickmode = Keys\n"
	"rightstickup = W\n"
	"\n"
	"[controller Test Pad]\n"
	"start = Return\n";

static void test_parse(void)
{
	struct settings *s = malloc(sizeof *s);
	char warn[160];
	settings_init(s);
	CHECK(pad_maps_equal(&s->all, &pad_default_map));
	settings_parse(s, text, warn, sizeof warn);
	CHECK(!warn[0]);
	CHECK(s->all.control[PAD_A].key == 0x31 && s->all.control[PAD_B].modifiers == PAD_MOD_COMMAND);
	CHECK(s->all.stick[0] == PAD_STICK_MOUSE && s->all.stick[1] == PAD_STICK_MOUSE);
	CHECK(s->all.control[PAD_Y].key == pad_default_map.control[PAD_Y].key);        /* (no line: Blue's) */
	CHECK_EQ(s->pads, 2);

	/* a type's own: by its GUID (in any case), else its name; the rest, the one for all */
	bool own;
	const struct pad_map *m = settings_pad_map(s, "030000005E040000E002000003090000", "Some Pad", &own);
	CHECK(own && m->control[PAD_X].function == PAD_DO_MOUSE && m->control[PAD_X].modifiers == PAD_MOD_CONTROL);
	CHECK(m->stick[1] == PAD_STICK_KEYS && m->control[PAD_RSTICK_UP].key == 0x0D);
	CHECK(m->control[PAD_A].key == pad_default_map.control[PAD_A].key);    /* (its own: Blue's, not all's) */
	CHECK(!strcmp(s->pad[0].name, "Xbox Wireless Controller"));
	m = settings_pad_map(s, NULL, "Test Pad", &own);
	CHECK(own && m->control[PAD_START].key == 0x24);
	m = settings_pad_map(s, "", "test pad", &own);
	CHECK(own);
	m = settings_pad_map(s, "0300000000000000", "Other", &own);
	CHECK(!own && m == &s->all);

	/* written and read again: the same, and what Blue doesn't know kept */
	size_t len = settings_format(s, NULL, 0);
	char *out = malloc(len + 1);
	CHECK_EQ(settings_format(s, out, len + 1), len);
	CHECK(strstr(out, "[Launcher]\n  window = 800x600   # (Blue doesn't know it)\n") != NULL);
	CHECK(strstr(out, "\n[controller]\n") && strstr(out, "\nb = Command-Q\n") &&
	      strstr(out, "\nleftstickmode = Mouse\n") && strstr(out, "\nrightstickleft = Left Arrow\n"));
	CHECK(strstr(out, "\n[controller 030000005e040000e002000003090000]\nname = Xbox Wireless Controller\n"));
	CHECK(!strstr(out, "someone's comment"));
	struct settings *again = malloc(sizeof *again);
	settings_init(again);
	settings_parse(again, out, warn, sizeof warn);
	CHECK(!warn[0]);
	CHECK(pad_maps_equal(&again->all, &s->all) && again->pads == 2 &&
	      pad_maps_equal(&again->pad[0].map, &s->pad[0].map) && pad_maps_equal(&again->pad[1].map, &s->pad[1].map));
	char *twice = malloc(len + 1);
	settings_format(again, twice, len + 1);
	CHECK(!strcmp(out, twice));                     /* (the same each time: no lines gather) */
	char small[64];
	CHECK_EQ(settings_format(s, small, sizeof small), len);
	CHECK_EQ(strlen(small), sizeof small - 1);
	free(twice);
	free(out);

	/* a type's own set, changed, and taken away (by its GUID or its name) */
	struct pad_map map = pad_default_map;
	map.control[PAD_A] = pad_key_action(0x0C, PAD_MOD_COMMAND);
	CHECK(settings_set_pad_map(again, "abcd", "New Pad", &map));
	CHECK_EQ(again->pads, 3);
	m = settings_pad_map(again, "ABCD", NULL, &own);
	CHECK(own && m->control[PAD_A].key == 0x0C && !strcmp(again->pad[2].name, "New Pad"));
	map.control[PAD_A] = pad_key_action(0x0D, 0);
	CHECK(settings_set_pad_map(again, "abcd", "New Pad", &map));
	CHECK(again->pads == 3 && settings_pad_map(again, "abcd", NULL, NULL)->control[PAD_A].key == 0x0D);
	CHECK(settings_set_pad_map(again, "", "Test Pad", NULL));
	CHECK(settings_set_pad_map(again, "abcd", "New Pad", NULL));
	CHECK_EQ(again->pads, 1);
	settings_pad_map(again, NULL, "Test Pad", &own);
	CHECK(!own);
	for (unsigned k = again->pads; k < SETTINGS_PADS; k++) {
		char id[16];
		snprintf(id, sizeof id, "pad%u", k);
		CHECK(settings_set_pad_map(again, id, id, &map));
	}
	CHECK(!settings_set_pad_map(again, "one too many", "", &map));

	/* what it can't make sense of: said (the first), and left out */
	settings_init(again);
	settings_parse(again, "[controller]\na = Return\nb = Hyper-Q\nfoo = Space\nx\n", warn, sizeof warn);
	CHECK(!strcmp(warn, "line 3: not a key, a click or Nothing"));
	CHECK(again->all.control[PAD_B].key == pad_default_map.control[PAD_B].key);
	settings_parse(again, "a = Return\n", warn, sizeof warn);
	CHECK(!strcmp(warn, "line 1: not in a [section]"));
	settings_parse(again, "[controller]\nleftstickmode = sideways\n", warn, sizeof warn);
	CHECK(!strcmp(warn, "line 2: a stick is Keys or Mouse"));
	settings_parse(again, "[controller\r\na = Q\r\n", warn, sizeof warn);
	CHECK(!strcmp(warn, "line 1: a section without its ]"));
	settings_parse(again, "[CONTROLLER]\r\nA = Q\r\n", warn, sizeof warn);
	CHECK(!warn[0] && again->all.control[PAD_A].key == 0x0C);
	free(again);
	free(s);
}

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	char *buf = calloc(1, 1 << 16);
	size_t n = fread(buf, 1, (1 << 16) - 1, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

static unsigned files_in(const char *dir)
{
	unsigned n = 0;
	DIR *d = opendir(dir);
	for (struct dirent *e; d && (e = readdir(d));)
		n += e->d_name[0] != '.';
	if (d)
		closedir(d);
	return n;
}

static void test_file(void)
{
	char dir[512], path[600], sub[600], err[1200];
	const char *tmp = getenv("TMPDIR");
	snprintf(dir, sizeof dir, "%s/blue-settings-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
	if (!mkdtemp(dir)) {
		CHECK(!"mkdtemp");
		return;
	}
	struct settings *s = malloc(sizeof *s), *t = malloc(sizeof *t);
	/* none yet: Blue's defaults */
	snprintf(path, sizeof path, "%s/blue/settings.ini", dir);
	CHECK_EQ(settings_load(s, path, err, sizeof err), 0);
	CHECK(!err[0] && pad_maps_equal(&s->all, &pad_default_map) && !s->pads);
	/* written: its folder made; read back the same */
	settings_parse(s, text, err, sizeof err);
	CHECK_EQ(settings_save(s, path, err, sizeof err), 0);
	snprintf(sub, sizeof sub, "%s/blue", dir);
	CHECK_EQ(files_in(sub), 1);
	CHECK_EQ(settings_load(t, path, err, sizeof err), 0);
	CHECK(!err[0] && pad_maps_equal(&t->all, &s->all) && t->pads == 2);
	struct stat st;
	CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0644);

	/* written again, whole, as a new file put in its place: what had it open still sees the old one */
	int fd = open(path, O_RDONLY);
	char *before = read_file(path);
	s->all.control[PAD_A] = pad_key_action(0x0C, PAD_MOD_COMMAND);
	CHECK_EQ(settings_save(s, path, err, sizeof err), 0);
	char *after = read_file(path), old[256] = "";
	CHECK(fd >= 0 && pread(fd, old, sizeof old - 1, 0) > 0);
	CHECK(before && after && !strncmp(old, before, strlen(old)) && strcmp(before, after));
	CHECK(strstr(after, "\na = Command-Q\n") != NULL);
	CHECK_EQ(files_in(sub), 1);                     /* (nothing left beside it) */
	if (fd >= 0)
		close(fd);
	free(before);
	free(after);

	/* where it can't go: said, and nothing left behind */
	snprintf(sub, sizeof sub, "%s/blue/settings.ini/inside.ini", dir);
	CHECK_EQ(settings_save(s, sub, err, sizeof err), -1);
	CHECK(strstr(err, "inside.ini") != NULL);
	snprintf(sub, sizeof sub, "%s/blue", dir);
	CHECK_EQ(files_in(sub), 1);
	/* ... and what can't be read */
	CHECK_EQ(settings_load(t, sub, err, sizeof err), -1);
	CHECK(err[0]);
	/* ... and what can, but not all of it: said */
	FILE *f = fopen(path, "w");
	fputs("[controller]\na = Wibble\n", f);
	fclose(f);
	CHECK_EQ(settings_load(t, path, err, sizeof err), 0);
	CHECK(strstr(err, "line 2: not a key, a click or Nothing (left out)") != NULL);
	unlink(path);
	rmdir(sub);
	rmdir(dir);
	free(s);
	free(t);
}

/* Where it goes: the platform's place */
static void test_default_path(void)
{
	char path[600], *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
	char saved_home[512] = "", saved_xdg[512] = "";
	snprintf(saved_home, sizeof saved_home, "%s", home ? home : "");
	snprintf(saved_xdg, sizeof saved_xdg, "%s", xdg ? xdg : "");
	setenv("HOME", "/Users/someone", 1);
	setenv("XDG_CONFIG_HOME", "/elsewhere/config", 1);
	CHECK(settings_default_path(path, sizeof path));
#ifdef __APPLE__
	CHECK(!strcmp(path, "/Users/someone/Library/Application Support/Blue/settings.ini"));
#else
	CHECK(!strcmp(path, "/elsewhere/config/blue/settings.ini"));
	unsetenv("XDG_CONFIG_HOME");
	CHECK(settings_default_path(path, sizeof path));
	CHECK(!strcmp(path, "/Users/someone/.config/blue/settings.ini"));
#endif
	unsetenv("XDG_CONFIG_HOME");
	unsetenv("HOME");
	CHECK(!settings_default_path(path, sizeof path));
	if (saved_home[0])
		setenv("HOME", saved_home, 1);
	if (saved_xdg[0])
		setenv("XDG_CONFIG_HOME", saved_xdg, 1);
}

void test_settings(void)
{
	test_parse();
	test_file();
	test_default_path();
}
