/*
 * Asking Mac OS to put a volume away: the shared folder's, for Refresh in
 * Blue's menu and input scripts' "share eject" (see share.h). Code of
 * Blue's runs in Mac OS at an idle call, as the guest tools' installer does
 * (install.h), and does what a program putting a disk away does: it
 * unmounts the drive's volume (UnmountVol, which flushes it and marks it
 * put away, or refuses while files on it are open: fBsyErr), then ejects
 * it (Eject), and the drive's driver takes the disk out.
 */
#ifndef BLUE_PUTAWAY_H
#define BLUE_PUTAWAY_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"
#include "ndrv.h"

#define HOSTCALL_PUTAWAY 0x10Fu         /* (never installed) */
#define PUTAWAY_FILES_OPEN (-47)        /* (fBsyErr: Mac OS has files open on the volume) */

enum putaway_phase {
	PUTAWAY_OFF,
	PUTAWAY_WAIT,                   /* for an idle call */
	PUTAWAY_INJECTED,               /* the idle trap goes to the bootstrap */
	PUTAWAY_RUNNING,
	PUTAWAY_DONE,                   /* (see below how it went) */
};

struct putaway {
	enum putaway_phase phase;
	uint16_t drive;                 /* whose volume */
	uint64_t asked_tb;              /* guest time it was asked for */
	bool ran;                       /* the code ran (else `why` says why not) */
	bool unmounted;                 /* ... and the volume was unmounted */
	int16_t error;                  /* ... or why not (UnmountVol's), or why it wasn't ejected (Eject's); 0: it was */
	char why[400];
	/* the code's */
	struct ndrv_layout layout;
	uint32_t trap_entry, trap_routine;
	bool ejecting;
};

/* Mac OS is to put away the volume on drive `drive` (from guest time `tb`) */
void putaway_ask(struct putaway *p, uint16_t drive, uint64_t tb);

/*
 * At an idle call (`trap`: the idle trap being dispatched, whose entry can
 * be diverted, else 0; `region_free`: the installer isn't using the
 * region): true if the guest goes to the code instead. Waiting more than
 * 10 s of guest time for an idle call it can divert, it gives up (DONE).
 */
bool putaway_idle(struct putaway *p, struct mem *m, uint16_t trap, bool region_free, uint64_t tb, uint64_t tb_hz);

/* Whether it waits for an idle trap's dispatch to divert */
bool putaway_waiting(const struct putaway *p);

/* Its host call: the main routine, or a resumption after a callout */
void putaway_call(struct putaway *p, struct ndrv_call *c);

#endif
