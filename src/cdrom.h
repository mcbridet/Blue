/*
 * CD-ROM drives as Apple's CD-ROM driver (".AppleCD") presents them to Mac
 * OS (Technical Note DV22, "CD-ROM Driver Calls"): the Control and Status
 * calls that AppleCD Audio Player, the Control Strip's CD module, the Audio
 * CD Access file system, QuickTime and games make for the disc's table of
 * contents, its subchannel, playing audio (into the sound chip's input:
 * cdaudio.h, burgundy.h), reading audio digitally, and the track play lists
 * the driver plays itself. disk.c hands a CD-ROM drive's calls here first,
 * with the disc in it (NULL: none, as far as Mac OS knows).
 *
 * Addresses come as a type and a long: 0 a logical block, 1 an absolute
 * MSF (0x00MMSSFF, BCD), 2 a track number (BCD), 3 an index into the play
 * list (BCD).
 */
#ifndef BLUE_CDROM_H
#define BLUE_CDROM_H

#include <stdbool.h>
#include <stdint.h>

#include "cdaudio.h"
#include "cdimage.h"
#include "mem.h"

struct cdrom {
	struct cdaudio audio;
	uint8_t list[CD_MAX_TRACKS];    /* the track play list (SetTrackList): track numbers, BCD */
	unsigned list_len;
	uint8_t list_mode;              /* SetPlayMode: 0 normal, 1 shuffle, 2 programmed */
	bool repeat;
	uint32_t stop;                  /* where play is to stop (AudioPlay's stop address): an LBA, or ... */
	bool stop_set, stop_list;       /* ... an index into the play list */
	bool list_play;                 /* the last play was the list's (GetTrackIndex) */
	uint8_t play_mode;              /* where the channels go (as cdaudio's mode) */
	uint8_t speed;                  /* SetSpindleSpeed */
	uint16_t block_size;
	uint32_t device_ident;          /* ReturnDeviceIdent: bus, SCSI target, LUN */
	uint32_t last_read;             /* the byte offset of the last read (Get2KOffset) */
	bool read_once;
	bool log;
	unsigned long audio_calls;      /* (statistics) */
};

/* A drive's state (`hz`: guest time's clock); NULL if out of memory. */
struct cdrom *cdrom_new(uint64_t hz, uint32_t device_ident);
void cdrom_free(struct cdrom *cd);

/* A disc goes in (or out: NULL): play stops. */
void cdrom_disc(struct cdrom *cd, const struct cdimage *disc);

/*
 * A Control or Status call (`pb` the parameter block, at guest time `tb`)
 * to the drive with `disc` in it: true if it's one of these, with its result
 * in `err`; false for the disk driver's own (DriveStatus, DriverGestalt,
 * Eject, icons ...).
 */
bool cdrom_control_call(struct cdrom *cd, const struct cdimage *disc, struct mem *m, uint32_t pb, uint64_t tb,
			int32_t *err);
bool cdrom_status_call(struct cdrom *cd, const struct cdimage *disc, struct mem *m, uint32_t pb, uint64_t tb,
		       int32_t *err);

/*
 * The icons of a CD-ROM drive and of its discs, as the floppy drive's
 * (floppy_icons()): 'ICN#' data for Control 22 and 21, icon families for
 * DriverGestalt 'mics' and 'dics'.
 */
void cdrom_icons(uint8_t *media_icn, uint8_t *drive_icn, uint8_t *media_icns, uint8_t *drive_icns);

#endif
