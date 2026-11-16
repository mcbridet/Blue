#include "drives.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "dmg.h"

#define FLOPPY_MAX (2880u * 1024)       /* (the biggest floppy's) */

static const char *file_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash && slash[1] ? slash + 1 : path;
}

const char *drives_name(const struct drives *dr, const struct disk *d, char *buf, size_t len)
{
	unsigned n = 0;
	for (unsigned k = 0; k < dr->cds; k++)
		if (dr->cd[k] == d)
			n = k + 1;
	if (d == dr->floppy)
		snprintf(buf, len, "floppy drive");
	else
		snprintf(buf, len, dr->cds > 1 ? "CD-ROM drive %u" : "CD-ROM drive", n);
	return buf;
}

/* A drive whose disc Mac OS is using: what to do first */
static void in_use(const struct drives *dr, const struct disk *d, const char *path, char *why, size_t len)
{
	char name[24];
	snprintf(why, len, "The %s has %.100s in it, which Mac OS is using: put it away there first (drag it to the "
		 "Trash), then put in %.100s", drives_name(dr, d, name, sizeof name), file_name(d->media_path),
		 file_name(path));
}

/* Whether an image is one of the kind the drive takes */
static bool takes(const struct disk *d, const char *path, char *why, size_t len)
{
	char reason[400];
	if (d->floppy ? floppy_check(path, reason, sizeof reason) : disk_check_cd(path, reason, sizeof reason)) {
		snprintf(why, len, "%.60s: %s", file_name(path), reason);
		return false;
	}
	return true;
}

struct disk *drives_pick_in(const struct drives *dr, struct disk *d, const char *path, char *why, size_t len)
{
	if (!takes(d, path, why, len))
		return NULL;
	if (d->media && disk_in_use(d)) {
		in_use(dr, d, path, why, len);
		return NULL;
	}
	return d;
}

struct disk *drives_pick(const struct drives *dr, const char *path, enum drives_for kind, char *why, size_t len)
{
	struct stat st;
	if (kind == DRIVES_ANY) {               /* (a cue sheet is small, but a CD's; a compressed image, by its size unpacked) */
		struct dmg *d;
		char reason[400];
		uint64_t size = stat(path, &st) == 0 ? (uint64_t)st.st_size : UINT64_MAX;
		if (dmg_open(path, &d, reason, sizeof reason) > 0) {
			size = dmg_size(d);
			dmg_close(d);
		}
		kind = size <= FLOPPY_MAX && !cdimage_has_toc(path) ? DRIVES_FLOPPY : DRIVES_CD;
	}
	if (kind == DRIVES_FLOPPY) {
		if (!dr->floppy) {
			snprintf(why, len, "There's no floppy drive for %.100s: start Blue with --floppy IMAGE, or "
				 "--floppy-drive for an empty one", file_name(path));
			return NULL;
		}
		return drives_pick_in(dr, dr->floppy, path, why, len);
	}
	if (!dr->cds) {
		snprintf(why, len, "There's no CD-ROM drive for %.100s: start Blue with --cdrom IMAGE, or "
			 "--cdrom-drive for an empty one", file_name(path));
		return NULL;
	}
	if (!takes(dr->cd[0], path, why, len))
		return NULL;
	struct disk *idle = NULL;
	for (unsigned k = 0; k < dr->cds; k++) {
		if (!dr->cd[k]->media)
			return dr->cd[k];
		if (!idle && !disk_in_use(dr->cd[k]))
			idle = dr->cd[k];
	}
	if (!idle && dr->cds == 1)
		in_use(dr, dr->cd[0], path, why, len);
	else if (!idle)
		snprintf(why, len, "The CD-ROM drives have discs Mac OS is using: put one away there first "
			 "(drag it to the Trash), then put in %.100s", file_name(path));
	return idle;
}

void drives_menu(const struct drives *dr, const char *const *queue, unsigned queued, struct ui_drives *out,
		 struct disk *order[UI_DRIVES])
{
	memset(out, 0, sizeof *out);
	memset(order, 0, UI_DRIVES * sizeof order[0]);
	if (dr->floppy)
		order[out->count++] = dr->floppy;
	for (unsigned k = 0; k < dr->cds && out->count < UI_DRIVES; k++)
		order[out->count++] = dr->cd[k];
	for (unsigned k = 0; k < out->count; k++) {
		const struct disk *d = order[k];
		struct ui_drive *u = &out->drive[k];
		drives_name(dr, d, u->name, sizeof u->name);
		if (u->name[0] >= 'a' && u->name[0] <= 'z')
			u->name[0] = (char)(u->name[0] - 'a' + 'A');
		if (d->media) {
			snprintf(u->in, sizeof u->in, "%.63s", file_name(d->media_path));
			u->locked = d->floppy && d->read_only;
			u->in_use = disk_in_use(d);
		}
	}
	for (unsigned k = 0; k < queued && dr->floppy; k++) {
		size_t n = strlen(out->next);
		snprintf(out->next + n, sizeof out->next - n, "%s%.60s", k ? ", " : "", file_name(queue[k]));
	}
}
