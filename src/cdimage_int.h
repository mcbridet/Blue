/*
 * What cdimage.c shares with the readers of other track layouts
 * (cdformats.c): an image's files, and how a layout is checked.
 */
#ifndef BLUE_CDIMAGE_INT_H
#define BLUE_CDIMAGE_INT_H

#include "cdimage.h"

struct cd_file {
	int fd;
	char *path;
	uint64_t offset;                /* of the first sector (WAVE, AIFF: of the samples) */
	uint64_t size;                  /* bytes from there */
	bool swap;                      /* 16-bit samples big-endian (MOTOROLA, AIFF, CHD) */
	bool audio;                     /* an audio file: samples only */
	/* decoded rather than read (compressed audio, a CHD's hunks, a compressed image): read() gives its bytes */
	void *dec;
	int (*read)(void *dec, void *buf, size_t len, uint64_t at);
	void (*close)(void *dec);
};

/*
 * Opens a file named in an image's description, found as a cue sheet's are
 * (beside it, by its last component, in another case): its index in files[],
 * or -1 with the reason in `why`.
 */
int cdimage_add_file(struct cdimage *c, const char *name, char *why, size_t len);

/* `len` bytes of a file at `at` */
bool cdimage_file_read(const struct cd_file *f, void *buf, size_t len, uint64_t at);

/* The readers: the tracks, sessions, lead-out and files of the image at c->path; 0, or -1 with why */
int cdimage_nrg(struct cdimage *c, char *why, size_t len);
int cdimage_mds(struct cdimage *c, char *why, size_t len);
int cdimage_ccd(struct cdimage *c, char *why, size_t len);
int cdimage_chd(struct cdimage *c, char *why, size_t len);

/* Checks a layout a reader made (tracks in order, each within the disc and its file): 0, or -1 with why */
int cdimage_check(struct cdimage *c, char *why, size_t len);

#endif
