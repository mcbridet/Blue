/*
 * CD images with a table of contents: cue sheets (CDRWIN's `.cue`, the
 * usual companion of `.bin` files) whose tracks are in raw 2352-byte
 * sectors (audio, mode 1 or mode 2 data), in 2048- or 2336-byte data
 * sectors, or in WAVE or AIFF files (audio at 44.1 kHz, 16-bit stereo);
 * and plain images (a `.iso`, `.cdr` or `.toast`: one data track of
 * 2048-byte sectors). Like a drive, it gives the table of contents (track
 * numbers, types and addresses, sessions), the data track's 2048 bytes a
 * sector as one stream (what Mac OS mounts), and any sector's 2352 bytes
 * (audio as samples, little-endian like a drive's digital audio), with the
 * Q subchannel for any address. Files are read as needed (pread()).
 *
 * Addresses are logical blocks (LBA, sectors from the first track's start,
 * 75 a second); MSF (minutes, seconds, frames) counts from 2 s earlier.
 */
#ifndef SYSHM_CDIMAGE_H
#define SYSHM_CDIMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CD_SECTOR 2352u                 /* raw bytes in a sector: 588 stereo frames of audio */
#define CD_DATA 2048u                   /* user data in a mode 1 (or mode 2 form 1) sector */
#define CD_FRAMES 588u                  /* audio frames in a sector */
#define CD_RATE 44100u
#define CD_SECTORS_PER_SECOND 75u
#define CD_MSF_OFFSET 150u              /* LBA 0 is 00:02:00 */
#define CD_MAX_TRACKS 99u
#define CD_SUBCODE 96u                  /* P-W subchannel bytes in a sector */

/* The Q subchannel's control field */
#define CD_CONTROL_PREEMPHASIS 0x1u
#define CD_CONTROL_COPY 0x2u            /* digital copy permitted */
#define CD_CONTROL_DATA 0x4u
#define CD_CONTROL_FOUR 0x8u            /* four channels */

struct cd_track {
	unsigned number;                /* 1-99 */
	unsigned session;               /* from 1 */
	uint8_t control;
	uint8_t mode;                   /* 0 audio, 1 or 2 data */
	uint32_t pregap;                /* LBA of index 0 (its pregap; = start without one) */
	uint32_t start;                 /* ... of index 1 */
	uint32_t end;                   /* ... after its last sector (the next track's pregap, or the lead-out) */
	char isrc[13];                  /* "" if none */
	/* where its sectors are: `file_sectors` from LBA `file_lba` at `file_pos`, `sector_size` bytes each */
	int file;                       /* -1: none (silence) */
	uint32_t file_lba, file_sectors;
	uint64_t file_pos;
	unsigned sector_size;           /* 2352, 2048 or 2336 */
};

struct cd_file;

struct cdimage {
	char *path;
	struct cd_track track[CD_MAX_TRACKS];
	unsigned tracks;
	unsigned sessions;
	uint32_t leadout;               /* LBA */
	int data;                       /* the track Mac OS sees the data of (the first data track); -1: none */
	uint64_t data_size;             /* ... its bytes, 2048 a sector */
	char catalog[14];               /* media catalog number (13 digits), "" if none */
	struct cd_file *files;
	unsigned nfiles;
	uint8_t *buf;                   /* (for reading raw sectors) */
};

/*
 * Opens a cue sheet (by its name, `.cue`) or a plain image. Returns NULL,
 * with the reason in `why`, if it can't (a cue sheet's files missing,
 * audio in another format, ...).
 */
struct cdimage *cdimage_open(const char *path, char *why, size_t len);
void cdimage_close(struct cdimage *c);

/* Whether a path names a cue sheet */
bool cdimage_is_cue(const char *path);

/*
 * The data track's user data, `len` bytes at byte `at`: 0, or -1 (a
 * disk_backend's read). A disc of audio alone reads as zeros, 2048 bytes a
 * sector up to the lead-out (Mac OS then mounts it with Audio CD Access).
 */
int cdimage_read_data(void *ctx, void *buf, size_t len, uint64_t at);

/*
 * `count` raw sectors from `lba` (CD_SECTOR bytes each): audio samples
 * (16-bit little-endian, left then right), a data sector as recorded (made
 * up of sync, header and zeros around the data if the file has only the
 * data), zeros for pregaps and gaps and past the lead-out. 0, or -1.
 */
int cdimage_read_raw(const struct cdimage *c, uint32_t lba, unsigned count, uint8_t *buf);

/* The track an address is in (from its pregap on): an index into track[], or -1 (the lead-out, past it) */
int cdimage_track_at(const struct cdimage *c, uint32_t lba);

/* Whether a sector is audio */
bool cdimage_audio(const struct cdimage *c, uint32_t lba);

/*
 * The Q subchannel at an address (mode 1, the current position): control
 * and ADR, track, index, the time in the track (counting down in a
 * pregap), 0, the absolute time; BCD as on the disc. 10 bytes, without the
 * CRC; cdimage_subcode() adds it.
 */
void cdimage_q(const struct cdimage *c, uint32_t lba, uint8_t q[10]);

/* The P-W subchannel of a sector (96 bytes, P and Q in bits 7 and 6 of each, R-W zero) */
void cdimage_subcode(const struct cdimage *c, uint32_t lba, uint8_t sub[CD_SUBCODE]);

/* MSF of an LBA (binary, not BCD), and back */
void cd_msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f);
uint32_t cd_lba(unsigned m, unsigned s, unsigned f);

static inline uint8_t cd_bcd(unsigned v)
{
	return (uint8_t)(v / 10 << 4 | v % 10);
}

static inline unsigned cd_unbcd(uint8_t v)
{
	return (v >> 4) * 10u + (v & 15u);
}

#endif
