/*
 * Installing the guest tools (tools.h) on the startup disk, from inside Mac
 * OS. Before Mac OS starts, syshm looks in the startup disk's System Folder
 * for them (install_check()); once the Finder is up, if one is missing or
 * isn't this syshm's, it asks (in the window; or an input script answers),
 * and if the answer is yes, Mac OS writes the files itself:
 *
 *   - At an idle call (the trap the Process Manager's idle routine calls
 *     when no process needs time: _SynchIdleTime, from a _SystemTask patch,
 *     before Mac OS 9; in its place, Mac OS 9's trap AAAA with $82F0 in D0:
 *     in the current application's context, at the time desk accessories
 *     and drivers get theirs), syshm points that trap's entry in the
 *     Toolbox trap table at 68k code of its own, in memory it maps outside
 *     Mac OS's (INSTALL_REGION).
 *   - That code puts the entry back and saves the registers; prepares the
 *     installer, a code fragment generated like the drivers (ndrv.h), with
 *     the Code Fragment Manager's GetMemFragment (through the
 *     _CodeFragmentDispatch trap), which resolves its imports; calls its
 *     main routine through a routine descriptor (Mixed Mode switches to
 *     native code as for any 68k caller); closes the connection, restores
 *     the registers and goes on to the trap's own routine.
 *   - The installer's logic runs in syshm (install_call()): through
 *     InterfaceLib, the File Manager writes each tool to a temporary file in
 *     the folder it goes in, which then takes the old copy's place (so that
 *     no file is ever left half written), and LaunchApplication starts
 *     syshm Enhancements. A copy of Enhancements that's running (an older
 *     one) is asked to quit first (helper.quit).
 */
#ifndef SYSHM_INSTALL_H
#define SYSHM_INSTALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hfsread.h"
#include "mem.h"
#include "ndrv.h"
#include "tools.h"

#define HOSTCALL_TOOLS_INSTALL 0x103u   /* the installer's (never installed itself) */
#define INSTALL_REGION 0x5E000000u      /* where its memory is mapped: nothing of Mac OS's is near */
#define INSTALL_REGION_SIZE 0x10000u

/* ---- the check, before Mac OS starts ------------------------------------------ */

enum install_tool { INSTALL_MISSING, INSTALL_DIFFERS, INSTALL_CURRENT };

struct install_check {
	char volume[HFSREAD_NAME_MAX];          /* the startup disk's name (UTF-8) */
	char system[HFSREAD_PATH_MAX];          /* its System Folder: "Volume:System Folder" */
	uint32_t system_dir;
	bool locked;                            /* the volume is (by software or hardware) */
	struct install_copy {
		enum install_tool state;
		uint32_t dir, id;               /* the copy found (not MISSING): its folder, its file ID */
		char folder[HFSREAD_NAME_MAX];  /* ... the folder's name */
	} tool[TOOLS_FILES];
};

/*
 * Looks for the tools in the blessed System Folder of partition `part` of
 * the image at `path`, in its folders (Extensions, whatever its name, and
 * Extensions (Disabled)): files of the tool's type and creator whose code
 * makes the tool's host call. A copy is current if both its forks are the
 * same as this syshm's. Returns 0, or -1 (`err` says why).
 */
int install_check(const char *path, unsigned part, struct install_check *ck, char *err, size_t errlen);

/* Whether something's to be installed or updated */
bool install_needed(const struct install_check *ck);

/* ---- once Mac OS runs --------------------------------------------------------- */

enum install_phase {
	INSTALL_OFF,            /* nothing to offer (or not offering: see install_begin()) */
	INSTALL_WAIT_FINDER,    /* until the Finder is up */
	INSTALL_ASKING,
	INSTALL_QUITTING,       /* asked the running Enhancements to quit */
	INSTALL_INJECT,         /* at the next idle call */
	INSTALL_INJECTED,       /* the trap goes to the bootstrap */
	INSTALL_RUNNING,        /* the installer has started */
	INSTALL_DONE,
};

struct install {
	enum install_phase phase;
	bool on;                        /* offering (window runs, and runs whose input script answers) */
	struct install_check check;
	int answer;                     /* a script's, before the question: 1 yes, -1 no */
	bool ask;                       /* the question is to be shown */
	char question[640];
	const char *yes;                /* "Install" or "Update" */
	char news[1024];                /* the outcome, to be shown once (news_ready) */
	bool news_ready;
	/* until the Finder is up: the Finder is the current application, idling without disk transfers */
	uint64_t finder_tb, quiet_tb, idle_tb, disk;
	uint64_t quit_tb, stopped_tb;   /* when Enhancements was asked to quit, when it had */
	uint64_t inject_tb;             /* when the installer first waited for the idle trap */
	/* the bootstrap */
	uint32_t trap_entry, trap_routine;      /* the idle trap's entry in the trap table, its own routine */
	uint32_t fragment_len;
	struct ndrv_layout layout;
	/* the installer's progress */
	uint16_t drive;
	unsigned step, tool;
	int16_t vref, refnum;
	uint32_t ext_dir, dir, scan_dir, scan_index;
	bool scanned_ext, replacing, temp_made, temp_open, old_deleted;
	bool quit_asked, old_found;     /* the running Enhancements was asked to quit; its copy was found ... */
	uint32_t old_dir;               /* ... here */
	char ext_name[HFSREAD_NAME_MAX];        /* the Extensions folder's */
	struct install_result {
		bool done, replaced;
		uint32_t dir;
		char folder[HFSREAD_NAME_MAX], old_name[HFSREAD_NAME_MAX];
	} result[TOOLS_FILES];
	int16_t error;
	const char *error_doing;
	unsigned error_tool;
	bool launch_tried, relaunch;    /* (relaunch: the old Enhancements, not replaced after all) */
	int16_t launch_error;
};

/* What the installer sees at an idle call */
struct install_idle {
	struct mem *mem;
	struct helper *helper;
	bool finder;                    /* the Finder is the current application */
	uint64_t tb, tb_hz;             /* guest time */
	uint64_t disk_bytes;            /* what the disks have moved so far */
	uint16_t drive;                 /* the startup disk's drive number (where its volume must be) */
	uint16_t trap;                  /* the idle trap being dispatched now, whose entry can be diverted (or 0) */
};

/* No offer: what runs without a window or a script's answer get */
void install_init(struct install *in);

/*
 * Looks at the startup disk (partition `part` of the image at `path`;
 * `read_only` if it's attached so) before Mac OS starts, and says what it
 * found. From then on, the tools are offered if anything's to be done.
 */
void install_begin(struct install *in, const char *path, unsigned part, bool read_only);

/*
 * At each idle call. Returns true if the guest has been diverted to the
 * bootstrap (then it doesn't idle): only at an idle trap's dispatch, once
 * the answer was yes (phase INSTALL_INJECT). Afterwards `ask` may say
 * there's a question to show, and `news_ready` news.
 */
bool install_idle(struct install *in, const struct install_idle *w);

/* The Finder is the current application (seen between idle calls, at guest time `tb`) */
void install_finder(struct install *in, uint64_t tb);

/* The answer (from the window or a script); a script's may come before the question. */
void install_answer(struct install *in, bool yes, struct helper *h);

/* The installer's host call: its main routine, or a resumption after a callout. */
void install_call(struct install *in, struct ndrv_call *c, struct helper *h);

#endif
