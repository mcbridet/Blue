/*
 * Installing the guest tools (tools.h) on the startup disk, from inside Mac
 * OS. Before Mac OS starts, Blue looks in the startup disk's System Folder
 * for them (install_check()); once the Finder is up, if one is missing or
 * isn't this version's, it asks (in the window; or an input script answers),
 * and if the answer is yes, Mac OS writes the files itself:
 *
 *   - At an idle call (the trap the Process Manager's idle routine calls
 *     when no process needs time: _SynchIdleTime, from a _SystemTask patch,
 *     before Mac OS 9; in its place, Mac OS 9's trap AAAA with $82F0 in D0:
 *     in the current application's context, at the time desk accessories
 *     and drivers get theirs), Blue points that trap's entry in the
 *     Toolbox trap table at 68k code of its own, in memory it maps outside
 *     Mac OS's (INSTALL_REGION).
 *   - That code puts the entry back and saves the registers; prepares the
 *     installer, a code fragment generated like the drivers (ndrv.h), with
 *     the Code Fragment Manager's GetMemFragment (through the
 *     _CodeFragmentDispatch trap), which resolves its imports; calls its
 *     main routine through a routine descriptor (Mixed Mode switches to
 *     native code as for any 68k caller); closes the connection, restores
 *     the registers and goes on to the trap's own routine.
 *   - The installer's logic runs in Blue (install_call()): through
 *     InterfaceLib, the File Manager writes each tool to a temporary file in
 *     the folder it goes in, which then takes the old copy's place (so that
 *     no file is ever left half written). A copy of Enhancements that's
 *     running (an older one) is asked to quit first (helper.quit): its file
 *     is in use while it runs, and can't be replaced. Mac OS opens it as it
 *     launches it, some seconds before it first runs: if the installer still
 *     finds it in use as it replaces it, the temporary file goes, Enhancements
 *     is asked to quit (it does at its first WaitNextEvent), and the
 *     installer goes in again once it has (once; Enhancements goes first,
 *     so there's nothing else to undo).
 *   - Once everything is in place (and only then), FlushVol, and the Process
 *     Manager is sent a Restart Apple event: Mac OS restarts as from Special
 *     > Restart (programs can ask to save their work first, or stop it), and
 *     the tools load as any extension does.
 *
 * Don't Ask Again goes the same way: Mac OS writes Blue's preferences file
 * (below) in its Preferences folder, and the check finds it next time.
 *
 * Blue's OpenGL libraries have the fragment names of Apple's (Mac OS 9's
 * OpenGLLibrary, OpenGLMemory and OpenGLUtility), so only one of each can be
 * enabled: Apple's are in Extensions (Disabled) while Blue's are in the
 * Extensions folder. Once Blue's are there, the installer moves Apple's
 * (files of those names) to Extensions (Disabled); and if Blue's are gone
 * (none of them is anywhere) while Apple's are only in Extensions
 * (Disabled), the offer is to put Apple's back, instead of installing
 * Blue's. (The check knows the folders by their English names; the
 * installer by FindFolder's.)
 */
#ifndef BLUE_INSTALL_H
#define BLUE_INSTALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hfsread.h"
#include "mem.h"
#include "ndrv.h"
#include "tools.h"

#define HOSTCALL_TOOLS_INSTALL 0x103u   /* the installer's (never installed itself) */
#define INSTALL_REGION 0x5E000000u      /* where its memory is mapped: nothing of Mac OS's is near */
#define INSTALL_REGION_SIZE 0x50000u    /* (room for a fork of up to 300 KB) */
#define INSTALL_APPLE_GL TOOLS_GL_FILES /* Apple's OpenGL libraries of the names Blue's have */

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
	/* Apple's OpenGL libraries: a copy in the Extensions folder (0), in Extensions (Disabled) (1) */
	struct install_apple {
		bool found[2];
		uint32_t dir[2];
	} apple[INSTALL_APPLE_GL];
	bool apple_disable;             /* Blue's are all in the Extensions folder, and some of Apple's too */
	bool apple_restore;             /* Blue's aren't anywhere, Apple's only in Extensions (Disabled) */
	bool apple_kept;                /* Apple's were put back earlier in this run: Blue's aren't offered */
	/* Blue's preferences file, in one of the System Folder's folders (Preferences): don't offer the tools */
	bool prefs_found, never;
	uint32_t prefs_dir;
};

/*
 * Blue's preferences file: "Blue Preferences" (INSTALL_PREFS_NAME), of type
 * 'pref' and Blue Enhancements' creator, in the startup disk's Preferences
 * folder, which Mac OS writes for Blue as it writes the tools (see below).
 * It's text, CR-ended lines: "Blue Preferences", then settings ("Offer the
 * guest tools: no", for Don't Ask Again; lines Blue doesn't know are left
 * alone, and so is a file whose first line isn't that).
 */
#define INSTALL_PREFS_NAME "Blue Preferences"
#define INSTALL_PREFS_MAX 1024u

/* Makes the file's contents (with `never`: don't offer the tools); returns their length, or 0 if `len` is too small */
size_t install_prefs_make(char *out, size_t len, bool never);

/* Reads the file's contents: false if they aren't Blue's preferences, else whether not to offer the tools */
bool install_prefs_parse(const uint8_t *data, size_t len, bool *never);

/*
 * Looks for the tools in the blessed System Folder of partition `part` of
 * the image at `path`, in its folders (Extensions, whatever its name, and
 * Extensions (Disabled)): files of the tool's type and creator whose code
 * makes the tool's host call. A copy is current if both its forks are the
 * same as this version's. Returns 0, or -1 (`err` says why).
 */
int install_check(const char *path, unsigned part, struct install_check *ck, char *err, size_t errlen);

/* Whether something's to be installed or updated */
bool install_needed(const struct install_check *ck);

/* ---- once Mac OS runs --------------------------------------------------------- */

enum install_phase {
	INSTALL_OFF,            /* nothing to offer (or not offering: see install_begin()) */
	INSTALL_WAIT_FINDER,    /* until the Finder is up */
	INSTALL_ASKING,
	INSTALL_BUSY,           /* the installer found Enhancements in use: it's to quit, and the installer to go in again */
	INSTALL_QUITTING,       /* asked the running Enhancements to quit */
	INSTALL_INJECT,         /* at the next idle call */
	INSTALL_INJECTED,       /* the trap goes to the bootstrap */
	INSTALL_RUNNING,        /* the installer has started */
	INSTALL_DONE,
};

/* The answer to the offer */
enum install_answer {
	INSTALL_NOT_NOW,
	INSTALL_YES,            /* install (or update, or move, or put back), and restart */
	INSTALL_NEVER,          /* Don't Ask Again: Mac OS writes Blue's preferences file saying so */
};

/* What the installer does in Mac OS */
enum install_job { INSTALL_JOB_TOOLS, INSTALL_JOB_NEVER };

struct install {
	enum install_phase phase;
	bool on;                        /* offering (window runs, and runs whose input script answers) */
	bool checked;                   /* the startup disk was looked at, and can take the tools (writable, unlocked) */
	struct install_check check;
	enum install_answer answer;     /* a script's, before the question (`answered`) */
	bool answered;
	bool ask;                       /* the question is to be shown */
	char question[640];
	const char *yes;                /* "Install and Restart", "Update and Restart", ... */
	const char *never;              /* "Don't Ask Again" */
	char news[1024];                /* the outcome, to be shown once (news_ready) */
	bool news_ready;
	enum install_job job;
	/* until the Finder is up: the Finder is the current application, idling without disk transfers */
	uint64_t finder_tb, quiet_tb, idle_tb, disk;
	uint64_t quit_tb, stopped_tb;   /* when Enhancements was asked to quit, when it had */
	unsigned long quits;            /* ... its quits until then (helper.quits) */
	bool quit_running;              /* ... it was running then (else being launched: it quits once started) */
	bool retried;                   /* the installer found it in use, and went in again once it had quit */
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
	bool quit_asked;                /* Enhancements was asked to quit */
	char ext_name[HFSREAD_NAME_MAX];        /* the Extensions folder's */
	struct install_result {
		bool done, replaced;
		uint32_t dir;
		char folder[HFSREAD_NAME_MAX], old_name[HFSREAD_NAME_MAX];
	} result[TOOLS_FILES];
	/* Apple's OpenGL libraries: moved to Extensions (Disabled), or back */
	bool apple_done, apple_disabling, apple_restoring;
	unsigned apple;
	uint32_t disabled_dir;
	char disabled_name[HFSREAD_NAME_MAX];
	struct install_moved {
		bool moved;
		int16_t error;
	} apple_moved[INSTALL_APPLE_GL];
	int16_t error;
	const char *error_doing;
	unsigned error_tool;
	/* the restart: asked for (once all went well), or not (why) */
	bool restarting;
	int16_t restart_error;
	/* Blue's preferences file: written (Don't Ask Again), or deleted once the tools are installed */
	uint32_t prefs_dir;
	char prefs_folder[HFSREAD_NAME_MAX];
	bool prefs_forgotten;
	int16_t prefs_error;
	const char *prefs_doing;
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
 * found. From then on, the tools are offered if anything's to be done, and
 * Blue's preferences file doesn't say not to (Don't Ask Again; the menu
 * still offers them: install_offer()). `check.apple_kept` is kept as it was
 * (once Apple's OpenGL libraries have been put back, Blue's aren't offered
 * again after the restart that follows, nor later in the run: see nk.c).
 */
void install_begin(struct install *in, const char *path, unsigned part, bool read_only);

/*
 * At each idle call. Returns true if the guest has been diverted to the
 * bootstrap (then it doesn't idle): only at an idle trap's dispatch, once
 * the answer was yes or Don't Ask Again (phase INSTALL_INJECT). Afterwards
 * `ask` may say there's a question to show, and `news_ready` news.
 */
bool install_idle(struct install *in, const struct install_idle *w);

/* The Finder is the current application (seen between idle calls, at guest time `tb`) */
void install_finder(struct install *in, uint64_t tb);

/* The answer (from the window or a script); a script's may come before the question. */
void install_answer(struct install *in, enum install_answer a, struct helper *h);

/*
 * The offer, asked for (the window's menu: Install Guest Tools...), Don't
 * Ask Again or not. NULL if it can be made now; otherwise why not.
 */
const char *install_unofferable(const struct install *in);
void install_offer(struct install *in);

/* The installer's host call: its main routine, or a resumption after a callout. */
void install_call(struct install *in, struct ndrv_call *c);

#endif
