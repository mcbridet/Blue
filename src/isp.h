/*
 * Blue Game Controllers: an InputSprocket driver (for Apple Game Sprockets'
 * input library) among the guest tools, so that the host's pads (pad.h)
 * reach games that read game controllers through InputSprocket as
 * themselves: a device for each pad, its buttons, D-pad, sticks and
 * triggers named, in each game's configuration.
 *
 * InputSprocketLib loads every shared library of creator 'insp' (or
 * Apple's own, 'insi') in the Extensions folder when a game starts it. A
 * driver registers its devices as it's loaded (ISpDevice_New, with a meta
 * handler that gives InputSprocket the device's functions by selector, and
 * ISpElement_New for each control), or when InputSprocket 1.7 calls its
 * ISpDriver_FindAndLoadDevices. The game's ISpInit hands each device the
 * game's needs (its Init function): the device takes those it can meet
 * (autoconfiguration, isp_autoconfigure()), and from then on pushes its
 * controls' data to the game's virtual elements for them, as well as to
 * its own elements (ISpElement_PushSimpleData). ISpConfigure's dialog shows
 * each device's own panel (Show, Hide; its events through HandleEvent),
 * and InputSprocket keeps what the user chose for the game (GetState,
 * SetState).
 *
 * This one's code is the host's: every function is a host call (ndrv.h)
 * that calls InputSprocket and the Toolbox in turn. A Time Manager task
 * pushes the pads' changes every 10 ms of guest time (as does the game's
 * ISpTickle, if it calls it). The panel lists the game's needs, each with
 * the control that meets it in a box that pops up those that can. While a
 * game's needs are met by an active device of this driver, the controls it
 * uses aren't the keyboard and mouse (isp_in_use(), isp_taken()).
 */
#ifndef BLUE_ISP_H
#define BLUE_ISP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ndrv.h"
#include "pad.h"

#define ISP_LIBRARY_MAX (24u << 10)

/*
 * The fragment's entries (its host calls' r11): its initialization and
 * termination; InputSprocket 1.7's exports (CheckConfiguration,
 * FindAndLoadDevices, DisposeDevices, Tickle); the meta handler passed to
 * ISpDevice_New, and the function it gives for each of InputSprocket's
 * selectors (Init 1 to CalibrateDialog 22), at ISP_ENTRY_SELECTOR +
 * selector - 1; the Time Manager task's routine; the panel's user item's.
 */
#define ISP_SELECTORS 22
#define ISP_SEL_INIT 1
enum {
	ISP_ENTRY_INIT, ISP_ENTRY_TERM, ISP_ENTRY_CHECK, ISP_ENTRY_FIND, ISP_ENTRY_DISPOSE, ISP_ENTRY_TICKLE,
	ISP_ENTRY_META, ISP_ENTRY_SELECTOR, ISP_ENTRY_TIMER = ISP_ENTRY_SELECTOR + ISP_SELECTORS, ISP_ENTRY_DRAW
};

/* A pad's elements, as InputSprocket has them */
enum isp_element {
	ISP_A, ISP_B, ISP_X, ISP_Y, ISP_LEFT_SHOULDER, ISP_RIGHT_SHOULDER, ISP_BACK, ISP_START, ISP_LEFT_STICK,
	ISP_RIGHT_STICK, ISP_GUIDE, ISP_DPAD, ISP_LEFT_X, ISP_LEFT_Y, ISP_RIGHT_X, ISP_RIGHT_Y, ISP_LEFT_TRIGGER,
	ISP_RIGHT_TRIGGER, ISP_ELEMENTS
};
/* ... and what can meet a game's need: an element, a direction of the D-pad's or a stick's (as a button), or
 * the left stick as a whole (a movement) */
enum {
	ISP_DPAD_UP = ISP_ELEMENTS, ISP_DPAD_DOWN, ISP_DPAD_LEFT, ISP_DPAD_RIGHT, ISP_LEFT_X_LEFT, ISP_LEFT_X_RIGHT,
	ISP_LEFT_Y_UP, ISP_LEFT_Y_DOWN, ISP_RIGHT_X_LEFT, ISP_RIGHT_X_RIGHT, ISP_RIGHT_Y_UP, ISP_RIGHT_Y_DOWN,
	ISP_MOVEMENT, ISP_CONTROLS, ISP_NONE = 0xFF
};

/* InputSprocket's kinds and labels (InputSprocket.h) */
#define ISP_FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define ISP_KIND_BUTTON ISP_FOURCC('b', 'u', 't', 'n')
#define ISP_KIND_DPAD ISP_FOURCC('d', 'p', 'a', 'd')
#define ISP_KIND_AXIS ISP_FOURCC('a', 'x', 'i', 's')
#define ISP_KIND_DELTA ISP_FOURCC('d', 'l', 't', 'a')
#define ISP_KIND_MOVEMENT ISP_FOURCC('m', 'o', 'v', 'e')
#define ISP_NEED_NO_MULTI_CONFIG 0x00000001u
#define ISP_NEED_UTILITY 0x00000002u
#define ISP_NEED_NO_AUTO_CONFIG 0x00000010u
#define ISP_NEED_NO_CONFIG 0x00000020u
#define ISP_NEED_INVISIBLE 0x00000040u
#define ISP_NEED_BUTTON_ALREADY_AXIS 0x10000000u
#define ISP_NEED_BUTTON_ALREADY_DELTA 0x80000000u
#define ISP_NEED_AXIS_ASYMMETRIC 0x20000000u
#define ISP_NEED_SIZE 92                /* an ISpNeed: its name (Str63) at 0, kind at 68, label 72, flags 76 */

/* A game's need, as a device has it */
#define ISP_NEEDS 64
struct isp_need {
	uint32_t kind, label, flags;
	uint32_t element;               /* the game's virtual element for it */
	char name[64];
	uint8_t control;                /* what meets it (ISP_NONE: nothing of this device's) */
	bool pushed_any;                /* ... the last data pushed for it */
	uint32_t pushed;
	int64_t delta;                  /* (a delta's motion left over) */
};

/* Which control meets each need: the needs not yet used (by an earlier device), in order */
void isp_autoconfigure(struct isp_need *needs, unsigned count, bool *used);

/* What a control is called, and whether it can meet a need of a kind (and flags) */
const char *isp_control_name(unsigned control);
bool isp_control_meets(unsigned control, uint32_t kind, uint32_t flags);

/*
 * A control's data for a need of a kind, from a pad (NULL: none plugged in);
 * `delta` keeps a delta's remainder. A stick as a delta moves as the right
 * stick moves the mouse, ISP_DELTA_COUNT to a count: InputSprocket.h has a
 * delta as a Fixed and Apple's mouse driver pushes plain counts, but games
 * go their own way (Unreal Tournament takes a count as 128, turning about
 * 280 degrees a second, the stick all the way over, at its default
 * sensitivity).
 */
#define ISP_DELTA_COUNT 128
uint32_t isp_data(const struct pad_slot *pad, unsigned control, uint32_t kind, uint64_t ticks, uint64_t hz,
		  int64_t *delta);

struct isp_device {
	bool registered;
	unsigned pad;                   /* the pad it is */
	char name[PAD_NAME];
	uint32_t ref;                   /* its ISpDeviceReference */
	uint32_t element[ISP_ELEMENTS];
	bool pushed_any[ISP_ELEMENTS];
	uint32_t pushed[ISP_ELEMENTS];
	bool active;                    /* (InputSprocket's SetActive) */
	bool init;                      /* a game's needs are met (between InputSprocket's Init and Stop) */
	unsigned needs;
	struct isp_need need[ISP_NEEDS];
	uint8_t before[ISP_NEEDS];      /* what met them as InputSprocket's dialog opened */
	/* its panel in InputSprocket's dialog */
	bool shown;
	uint32_t dialog;
	int item;                       /* its user item */
	uint32_t handled_at;            /* (where to say a click was handled, as it's tracked) */
	int16_t rect[4];                /* top, left, bottom, right */
	unsigned rows;
	uint8_t row[ISP_NEEDS];         /* the needs the panel lists */
	unsigned popup;                 /* the row whose box popped up ... */
	unsigned choices;
	uint8_t choice[ISP_CONTROLS + 1];       /* ... its menu's controls */
};

#define ISP_INSTANCES 2                 /* the driver loaded at once (by games running at once) */
struct isp_instance {
	bool used;
	uint32_t toc;                   /* its data */
	uint32_t psn[2];                /* the process that loaded it */
	char name[32];                  /* ... its name */
	bool timer;                     /* its Time Manager task is in */
	uint64_t pushed_tb;             /* guest time it last pushed (for deltas) */
	struct isp_device dev[PAD_MAX];
};

/* A run of calls to Mac OS (InputSprocket, the Toolbox), one a host call, for an entry */
#define ISP_OPS 1024
#define ISP_PROGRAMS 4
struct isp_op {
	uint8_t imp, then, n;
	int16_t from[8];                /* (an argument that is an earlier call's result: its index, else -1) */
	uint32_t a[8];
	uint32_t then_arg;
};
struct isp_program {
	bool busy;
	unsigned inst;
	unsigned n, at;
	uint32_t value;                 /* the entry's result */
	struct isp_op op[ISP_OPS];
	uint32_t result[ISP_OPS];
};

struct isp {
	struct ndrv_layout layout;
	uint64_t hz;                    /* the timebase's rate */
	uint32_t hash;                  /* the fragment's layout (another version's copy isn't run) */
	bool stale_said;                /* ... and one has been said to be */
	struct isp_instance inst[ISP_INSTANCES];
	struct isp_program prog[ISP_PROGRAMS];
	bool log;                       /* BLUE_LOG_PADS: what it does */
	unsigned long calls, pushes;
};

void isp_init(struct isp *s, uint64_t timebase_hz);

/* The fragment; returns its size (0 if `size` is too small) */
size_t isp_build(uint8_t *out, size_t size, struct ndrv_layout *layout);

/* A host call from the driver */
void isp_call(struct isp *s, struct ndrv_call *c, const struct pad *pads);

/*
 * Whether a game reads the pads through InputSprocket now: a device of the
 * driver's meets its needs and is active (then `psn` is the game's process,
 * `name` its name)
 */
bool isp_in_use(const struct isp *s, uint32_t psn[2], const char **name);

/* ... and the pad's controls it uses then (pad_slot.taken: the rest keep Blue's mapping) */
uint32_t isp_taken(const struct isp *s, unsigned pad);

#endif
