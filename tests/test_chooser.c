/*
 * The file chooser for hosts without their own (zenity's or kdialog's, on
 * the PATH), with stand-ins for them: a file chosen, the chooser cancelled
 * or failing, kdialog's when there's no zenity, none on the PATH, and one
 * closed unfinished (the window going).
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/chooser.h"
#include "test.h"

static atomic_int woken;

static void wake(void *arg)
{
	(void)arg;
	atomic_fetch_add(&woken, 1);
}

static char dir[256];

/* A stand-in for a chooser, in dir: a shell script (or none: `body` NULL) */
static void stand_in(const char *name, const char *body)
{
	char path[300];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	unlink(path);
	if (!body)
		return;
	FILE *f = fopen(path, "w");
	CHECK(f != NULL);
	if (!f)
		return;
	fprintf(f, "#!/bin/sh\n%s\n", body);
	fclose(f);
	chmod(path, 0755);
}

/* What came of it (waiting up to 5 s) */
static enum chooser_state outcome(struct chooser *c, char **path)
{
	enum chooser_state state = CHOOSER_WAITING;
	for (int k = 0; k < 500 && (state = chooser_poll(c, path)) == CHOOSER_WAITING; k++)
		usleep(10000);
	return state;
}

void test_chooser(void)
{
	const char *tmp = getenv("TMPDIR");
	snprintf(dir, sizeof dir, "%s/blue-test-XXXXXX", tmp ? tmp : "/tmp");
	if (!mkdtemp(dir)) {
		CHECK(!"mkdtemp");
		return;
	}
	const char *old = getenv("PATH");
	char *path_was = old ? strdup(old) : NULL;
	setenv("PATH", dir, 1);
	char *path = NULL;

	/* none on the PATH */
	CHECK(chooser_spawn("Choose a disk image", wake, NULL) == NULL);

	/* zenity: asked for a file, titled; what it prints is the file chosen */
	stand_in("zenity", "[ \"$1\" = --file-selection ] && [ \"$2\" = --title ] && [ \"$3\" = \"Choose a disk image\" ] "
		 "|| exit 3\necho \"/some/where/Disk One.img\"");
	struct chooser *c = chooser_spawn("Choose a disk image", wake, NULL);
	CHECK(c != NULL);
	if (c) {
		CHECK_EQ(outcome(c, &path), CHOOSER_CHOSEN);
		CHECK(path && strcmp(path, "/some/where/Disk One.img") == 0);
		CHECK_EQ(atomic_load(&woken), 1);
		free(path);
	}

	/* cancelled (it exits with 1), or failing */
	stand_in("zenity", "exit 1");
	if ((c = chooser_spawn("Choose", wake, NULL))) {
		CHECK_EQ(outcome(c, &path), CHOOSER_CANCELLED);
		CHECK(path == NULL);
	}
	stand_in("zenity", "echo /x.img; exit 255");
	if ((c = chooser_spawn("Choose", wake, NULL))) {
		CHECK_EQ(outcome(c, &path), CHOOSER_FAILED);
		CHECK(path == NULL);
	}

	/* kdialog, without zenity */
	stand_in("zenity", NULL);
	stand_in("kdialog", "[ \"$1\" = --title ] && [ \"$3\" = --getopenfilename ] || exit 3\necho /a/b.iso");
	c = chooser_spawn("Choose", wake, NULL);
	CHECK(c != NULL);
	if (c) {
		CHECK_EQ(outcome(c, &path), CHOOSER_CHOSEN);
		CHECK(path && strcmp(path, "/a/b.iso") == 0);
		free(path);
	}

	/* closed unfinished: it goes, and nothing's woken */
	stand_in("kdialog", "exec /bin/sleep 20");
	int before = atomic_load(&woken);
	c = chooser_spawn("Choose", wake, NULL);
	CHECK(c != NULL);
	if (c) {
		usleep(50000);
		chooser_close(c);
		usleep(300000);
		CHECK_EQ(atomic_load(&woken), before);
	}

	stand_in("kdialog", NULL);
	rmdir(dir);
	if (path_was)
		setenv("PATH", path_was, 1);
	else
		unsetenv("PATH");
	free(path_was);
}
