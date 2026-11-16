/*
 * Game controllers as the Mac's keyboard and mouse. The host's pads (SDL's
 * game controllers in a window, or an input script's) come here as buttons
 * and axes, by SDL's names and layout (A is the bottom face button, B the
 * right one, X the left, Y the top, whatever the pad's labels say), and go
 * to the ADB keyboard and mouse, by one of these mappings:
 *
 *   Blue's default   the D-pad and left stick the arrow keys, A Return, B
 *                    Command-period, X Space, Y Tab, Start Esc, Back
 *                    Command, the left and right shoulders Shift and
 *                    Option, the right trigger the mouse button and the
 *                    left one a Control-click, the right stick the mouse
 *                    (pressed: its button)
 *   a Pippin title's the pad as the Bandai Pippin's AppleJack, whose driver
 *                    turned its 18 switches into keys and clicks by the
 *                    mapping a title gives it (the switchMappingArray of its
 *                    AJGlobalData, as the title's 'pipp' resources hold it):
 *                    the D-pad and left stick are its D-pad; A, B, X and Y
 *                    its red, green, yellow and blue buttons (as they sit,
 *                    bottom, right, left and top); the shoulders and
 *                    triggers its left and right fire buttons; Back, Guide
 *                    and Start its three system buttons (left, middle,
 *                    right); and the right stick its trackball, the mouse
 *   off              nothing (InputSprocket reads the pads instead)
 *
 * What a control does is decided as it goes down, and undone as it comes
 * up, whatever the mapping is by then; keys and the mouse button held by
 * several controls go up when the last lets go.
 */
#ifndef BLUE_PAD_H
#define BLUE_PAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAD_MAX 4                       /* pads at once */
#define PAD_NAME 64

/* SDL_GameControllerButton's and SDL_GameControllerAxis's */
enum pad_button {
	PAD_A, PAD_B, PAD_X, PAD_Y, PAD_BACK, PAD_GUIDE, PAD_START, PAD_LEFT_STICK, PAD_RIGHT_STICK,
	PAD_LEFT_SHOULDER, PAD_RIGHT_SHOULDER, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_MISC, PAD_PADDLE1,
	PAD_PADDLE2, PAD_PADDLE3, PAD_PADDLE4, PAD_TOUCHPAD, PAD_BUTTONS
};
enum pad_axis { PAD_LEFT_X, PAD_LEFT_Y, PAD_RIGHT_X, PAD_RIGHT_Y, PAD_LEFT_TRIGGER, PAD_RIGHT_TRIGGER, PAD_AXES };

/* A control: a button, or what the left stick and the triggers make buttons of */
enum { PAD_STICK_UP = PAD_BUTTONS, PAD_STICK_DOWN, PAD_STICK_LEFT, PAD_STICK_RIGHT, PAD_LT, PAD_RT, PAD_CONTROLS };

/*
 * The AppleJack's driver's data, AJGlobalData (Apple's Pippin Technical
 * Note PPBD-JHACEF003): 'pipp', four bytes, switchStates (bit n switch n's,
 * 0 while it's down), then a SwitchData for each switch, then two pointers;
 * a 'pipp' resource is one, and a title copies its switches' mappings into
 * the driver's. A SwitchData's key is a virtual key code (an event's, the
 * KeyMap's), its modifiers bits of PAD_MOD_*.
 */
#define PAD_AJ_SIGNATURE 0x70697070u    /* 'pipp' */
#define PAD_AJ_STATES 8                 /* (offsets in AJGlobalData) */
#define PAD_AJ_MAP 12
#define PAD_AJ_SIZE 92
#define PAD_AJ_SWITCHES 18

struct pad_switch {
	uint8_t function, modifiers, key, chr;
};
enum { PAD_DO_NOTHING, PAD_DO_MOUSE, PAD_DO_DOUBLE_CLICK, PAD_DO_MOUSE_LOCK, PAD_DO_KEY, PAD_DO_FRONT_PANEL,
       PAD_DO_MODIFIER };
#define PAD_MOD_COMMAND 0x01u
#define PAD_MOD_SHIFT 0x02u
#define PAD_MOD_CAPS_LOCK 0x04u
#define PAD_MOD_OPTION 0x08u
#define PAD_MOD_CONTROL 0x10u

/* A 'pipp' resource's (or an AJGlobalData's) switches; false if it isn't one */
bool pad_pipp(const uint8_t *data, size_t len, struct pad_switch map[PAD_AJ_SWITCHES]);

/* The AppleJack switch a control is (-1: none), and the driver's own mapping, without a title's */
int pad_applejack_switch(unsigned control);
extern const struct pad_switch pad_applejack_defaults[PAD_AJ_SWITCHES];

/* What a mapping does with a control (for the menu and logs): "Command-Q", "the mouse button" */
void pad_switch_text(const struct pad_switch *s, char *out, size_t size);

/* An ADB keyboard's key code for a virtual key code (0xFF: none) */
uint8_t pad_adb_key(uint8_t virtual_key);

/* By SDL's names ("a", "dpup", "leftshoulder"; "leftx", "righttrigger"): -1 if not one */
int pad_button_named(const char *name);
int pad_axis_named(const char *name);
const char *pad_button_name(unsigned button);
const char *pad_axis_name(unsigned axis);

enum pad_mapping { PAD_MAP_OFF, PAD_MAP_DEFAULT, PAD_MAP_PIPPIN };

/* Where the keys and the mouse go */
struct pad_output {
	void *ctx;
	void (*key)(void *ctx, uint8_t adb_key, bool down);
	void (*button)(void *ctx, bool down);
	void (*motion)(void *ctx, int dx, int dy);
};

struct pad_slot {
	bool connected;
	char name[PAD_NAME];
	int16_t axis[PAD_AXES];
	uint32_t down;                  /* controls down (bit k: control k) */
	struct pad_switch held[PAD_CONTROLS];   /* ... and what each does meanwhile */
};

struct pad {
	struct pad_output out;
	/* set before each change: the mapping, a Pippin title's, and whether presses are let through */
	enum pad_mapping mapping;
	struct pad_switch pippin[PAD_AJ_SWITCHES];
	bool quiet;                     /* (the menu or a prompt is up: presses do nothing) */
	struct pad_slot slot[PAD_MAX];
	uint8_t keys[128];              /* how many controls hold each ADB key down */
	unsigned mouse;                 /* ... and the mouse button */
	bool locked;                    /* (by a "mouse lock" switch) */
	uint64_t tick;                  /* time pad_tick() last moved the mouse (0: not yet) */
	int64_t dx, dy;                 /* ... motion left over, in counts times the clock's rate */
};

void pad_init(struct pad *p, const struct pad_output *out);

/* A pad plugged in (in `slot`, or -1: the first free): its slot, or -1 if all are taken */
int pad_plug(struct pad *p, int slot, const char *name);
/* ... and out: what it held goes up */
void pad_unplug(struct pad *p, unsigned slot);
unsigned pad_count(const struct pad *p);

void pad_button(struct pad *p, unsigned slot, unsigned button, bool down);
void pad_axis(struct pad *p, unsigned slot, unsigned axis, int value);     /* (-32768 to 32767; triggers 0 up) */

/* The right stick moves the mouse as time passes: `now` on a clock of `hz` */
void pad_tick(struct pad *p, uint64_t now, uint64_t hz);
#define PAD_MOUSE_SPEED 900             /* the mouse's counts a second, the stick all the way over */

/* The AppleJack's switches down (bit n: switch n), on any pad */
uint32_t pad_applejack(const struct pad *p);

/* The keyboard and mouse were reset (the Mac restarted): what's held is forgotten, not let go */
void pad_forget(struct pad *p);

#endif
