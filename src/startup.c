#include "startup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootinfo.h"
#include "ofnvram.h"

#define TYPE_TBXI 0x74627869u           /* 'tbxi': the Mac OS ROM file's type */

bool startup_choice(const uint8_t *nvram, size_t size, unsigned *drive, unsigned *map_entry)
{
	char path[OFNVRAM_VALUE_MAX];
	unsigned dev;
	if (!ofnvram_get(nvram, size, "boot-device", path, sizeof path) || !ofnvram_disk_path(path, &dev, map_entry) ||
	    dev < PCI_DEVFN_DISK >> 3)
		return false;
	*drive = dev - (PCI_DEVFN_DISK >> 3);
	return true;
}

/* The ROM file found, loaded (its data freed): 0, or -1 (err says why) */
static int load(const char *path, struct hfsread_found *found, unsigned drive, struct rom *rom, struct startup *s,
		char *err, size_t errlen)
{
	char name[HFSREAD_PATH_MAX + 256];
	snprintf(name, sizeof name, "%s: \"%s\"", path, found->path);
	int rc = rom_load_mem(found->data, found->size, name, rom, err, errlen);
	free(found->data);
	found->data = NULL;
	if (rc)
		return -1;
	s->drive = drive;
	s->part = found->part;
	snprintf(s->from, sizeof s->from, "\"%s\" from %s%s%s%s", found->path, path,
		 found->partition[0] ? ", partition \"" : "", found->partition, found->partition[0] ? "\"" : "");
	return 0;
}

int startup_rom(const char *const *paths, unsigned count, const uint8_t *nvram, size_t size, struct rom *rom,
		struct startup *s, char *note, size_t notelen)
{
	struct hfsread_found found;
	char err[HFSREAD_PATH_MAX + 256];
	unsigned drive, entry;
	memset(s, 0, sizeof *s);
	note[0] = '\0';
	if (nvram && startup_choice(nvram, size, &drive, &entry)) {
		char partition[32] = "";
		if (entry)
			snprintf(partition, sizeof partition, " (partition %u)", entry);
		int rc = drive < count && paths[drive] ? hfsread_system_file_in(paths[drive], entry, TYPE_TBXI, &found, err,
										   sizeof err) : 0;
		if (rc > 0 && !load(paths[drive], &found, drive, rom, s, err, sizeof err)) {
			s->chosen = true;
			return 0;
		}
		if (drive >= count || !paths[drive])
			snprintf(note, notelen, "boot-device names drive %u%s, which isn't there", drive, partition);
		else if (rc < 0 || (rc > 0 && err[0]))
			snprintf(note, notelen, "boot-device names %s%s, whose Mac OS ROM file can't be started (%s)",
				 paths[drive], partition, err[0] ? err : "it can't be read");
		else
			snprintf(note, notelen, "boot-device names %s%s, which has no System Folder with a Mac OS ROM file",
				 paths[drive], partition);
	}

	/* the first drive with one */
	const char *list[32];
	unsigned index[32], n = 0;
	for (unsigned k = 0; k < count && n < 32; k++)
		if (paths[k]) {
			index[n] = k;
			list[n++] = paths[k];
		}
	int rc = hfsread_system_file(list, n, TYPE_TBXI, &found, err, sizeof err);
	if (rc <= 0) {
		if (rc < 0 && err[0])
			fprintf(stderr, "%s\n", err);
		return rc < 0 ? -1 : 1;
	}
	if (load(list[found.drive], &found, index[found.drive], rom, s, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return -1;
	}
	return 0;
}
