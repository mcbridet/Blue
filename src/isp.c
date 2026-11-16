#include "isp.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "be.h"
#include "tools.h"

#define FOURCC ISP_FOURCC

/* ---- the fragment ------------------------------------------------------------------- */

/* Its imports, InputSprocketLib's then InterfaceLib's */
enum {
	IMP_DEVICE_NEW, IMP_DEVICE_DISPOSE, IMP_ELEMENT_NEW, IMP_ELEMENT_DISPOSE, IMP_PUSH_SIMPLE, IMP_PUSH_COMPLEX,
	IMP_INS_TIME, IMP_PRIME_TIME, IMP_RMV_TIME, IMP_GET_CURRENT_PROCESS, IMP_NEW_HANDLE, IMP_DISPOSE_HANDLE,
	IMP_COUNT_DITL, IMP_APPEND_DITL, IMP_SHORTEN_DITL, IMP_SET_DIALOG_ITEM, IMP_SET_PORT, IMP_TEXT_FONT,
	IMP_TEXT_SIZE, IMP_ERASE_RECT, IMP_MOVE_TO, IMP_LINE_TO, IMP_DRAW_STRING, IMP_TRUNC_STRING, IMP_GET_MOUSE, IMP_GLOBAL_TO_LOCAL,
	IMP_LOCAL_TO_GLOBAL, IMP_NEW_MENU, IMP_APPEND_MENU, IMP_SET_MENU_ITEM_TEXT, IMP_CHECK_ITEM, IMP_INSERT_MENU,
	IMP_POP_UP_MENU_SELECT, IMP_DELETE_MENU, IMP_DISPOSE_MENU, IMP_COUNT
};

static const struct ndrv_import imports[IMP_COUNT] = {
	[IMP_DEVICE_NEW] = { "InputSprocketLib", "ISpDevice_New" },
	[IMP_DEVICE_DISPOSE] = { "InputSprocketLib", "ISpDevice_Dispose" },
	[IMP_ELEMENT_NEW] = { "InputSprocketLib", "ISpElement_New" },
	[IMP_ELEMENT_DISPOSE] = { "InputSprocketLib", "ISpElement_Dispose" },
	[IMP_PUSH_SIMPLE] = { "InputSprocketLib", "ISpElement_PushSimpleData" },
	[IMP_PUSH_COMPLEX] = { "InputSprocketLib", "ISpElement_PushComplexData" },
	[IMP_INS_TIME] = { "InterfaceLib", "InsTime" },
	[IMP_PRIME_TIME] = { "InterfaceLib", "PrimeTime" },
	[IMP_RMV_TIME] = { "InterfaceLib", "RmvTime" },
	[IMP_GET_CURRENT_PROCESS] = { "InterfaceLib", "GetCurrentProcess" },
	[IMP_NEW_HANDLE] = { "InterfaceLib", "NewHandle" },
	[IMP_DISPOSE_HANDLE] = { "InterfaceLib", "DisposeHandle" },
	[IMP_COUNT_DITL] = { "InterfaceLib", "CountDITL" },
	[IMP_APPEND_DITL] = { "InterfaceLib", "AppendDITL" },
	[IMP_SHORTEN_DITL] = { "InterfaceLib", "ShortenDITL" },
	[IMP_SET_DIALOG_ITEM] = { "InterfaceLib", "SetDialogItem" },
	[IMP_SET_PORT] = { "InterfaceLib", "SetPort" },
	[IMP_TEXT_FONT] = { "InterfaceLib", "TextFont" },
	[IMP_TEXT_SIZE] = { "InterfaceLib", "TextSize" },
	[IMP_ERASE_RECT] = { "InterfaceLib", "EraseRect" },
	[IMP_MOVE_TO] = { "InterfaceLib", "MoveTo" },
	[IMP_LINE_TO] = { "InterfaceLib", "LineTo" },
	[IMP_DRAW_STRING] = { "InterfaceLib", "DrawString" },
	[IMP_TRUNC_STRING] = { "InterfaceLib", "TruncString" },
	[IMP_GET_MOUSE] = { "InterfaceLib", "GetMouse" },
	[IMP_GLOBAL_TO_LOCAL] = { "InterfaceLib", "GlobalToLocal" },
	[IMP_LOCAL_TO_GLOBAL] = { "InterfaceLib", "LocalToGlobal" },
	[IMP_NEW_MENU] = { "InterfaceLib", "NewMenu" },
	[IMP_APPEND_MENU] = { "InterfaceLib", "AppendMenu" },
	[IMP_SET_MENU_ITEM_TEXT] = { "InterfaceLib", "SetMenuItemText" },
	[IMP_CHECK_ITEM] = { "InterfaceLib", "CheckItem" },
	[IMP_INSERT_MENU] = { "InterfaceLib", "InsertMenu" },
	[IMP_POP_UP_MENU_SELECT] = { "InterfaceLib", "PopUpMenuSelect" },
	[IMP_DELETE_MENU] = { "InterfaceLib", "DeleteMenu" },
	[IMP_DISPOSE_MENU] = { "InterfaceLib", "DisposeMenu" },
};

enum { SEL_INIT = ISP_SEL_INIT, SEL_STOP, SEL_GET_SIZE, SEL_HANDLE_EVENT, SEL_SHOW, SEL_HIDE, SEL_BEGIN, SEL_END,
       SEL_GET_ICON, SEL_GET_STATE, SEL_SET_STATE, SEL_DIRTY, SEL_SET_ACTIVE, SEL_ITEM_HIT, SEL_TICKLE,
       SEL_INTERRUPT_TICKLE, SEL_DRAW, SEL_CLICK, SEL_ADB_REINIT, SEL_GET_CALIBRATION, SEL_SET_CALIBRATION,
       SEL_CALIBRATE_DIALOG, SELECTORS = SEL_CALIBRATE_DIALOG };
_Static_assert(SELECTORS == ISP_SELECTORS, "InputSprocket's selectors");
enum { E_INIT = ISP_ENTRY_INIT, E_TERM, E_CHECK, E_FIND, E_DISPOSE, E_TICKLE, E_META, E_SEL, E_TIMER = ISP_ENTRY_TIMER,
       E_DRAW, E_COUNT };

static const struct ndrv_export exports[] = {
	{ "ISpDriver_CheckConfiguration", E_CHECK, false, 0 },
	{ "ISpDriver_FindAndLoadDevices", E_FIND, false, 0 },
	{ "ISpDriver_DisposeDevices", E_DISPOSE, false, 0 },
	{ "ISpDriver_Tickle", E_TICKLE, false, 0 },
};
#define EXPORTS (sizeof exports / sizeof exports[0])

/* Its data (after the entry TVectors): what it is, then what the calls are passed */
#define ISP_MAGIC FOURCC('B', 'l', 'I', 's')
#define NAME_SLOT 32u                   /* (each control's name, a Pascal string) */
enum {
	X_MAGIC = 0,                    /* 'BlIs', and the layout's hash (see layout_hash()) */
	X_PSN = 8,                      /* ProcessSerialNumber */
	X_TIME = 16,                    /* AbsoluteTime, of the data pushed */
	X_POINT = 24,                   /* Point (v, h) */
	X_TASK = 28,                    /* TMTask (22 bytes) */
	X_TIMER_RD = 52,                /* RoutineDescriptors of the Time Manager task's routine, */
	X_DRAW_RD = 84,                 /* ... and the user item's */
	X_ITEM_RECT = 116,              /* Rect */
	X_EMPTY = 124,                  /* "" and " " (Pascal strings) */
	X_SPACE = 126,
	X_DEVDEF = 128,                 /* an ISpDeviceDefinition for each pad (92 bytes) */
	X_ELDEF = X_DEVDEF + PAD_MAX * 92,      /* an ISpElementDefinitionStruct for each element (104 bytes) */
	X_CONFIG = X_ELDEF + ISP_ELEMENTS * 104,        /* their configuration information (8 bytes) */
	X_OUT = X_CONFIG + ISP_ELEMENTS * 8,    /* a reference out, and one for each element */
	X_COMPLEX = X_OUT + 4 * (1 + ISP_ELEMENTS),     /* movement data pushed (12 bytes), in turn */
	X_NAMES = X_COMPLEX + 16 * 12,  /* the controls' names, and "None" */
	X_TEXT = X_NAMES + NAME_SLOT * (ISP_CONTROLS + 1),      /* the panel's text, as it fits (Str63s, 2 a row) */
	X_SIZE = X_TEXT + 2 * 64 * ISP_NEEDS,
};
#define COMPLEX_SLOTS 16u

#define TIMER_PROC_INFO 0x0000B802u     /* uppTimerProcInfo: the task in A1 */
#define USER_ITEM_PROC_INFO 0x000002C0u /* uppUserItemProcInfo: (DialogRef, short) */
#define TIMER_MS 10
#define DEVICE_CLASS FOURCC('g', 'm', 'p', 'd')         /* kISpDeviceClass_Gamepad */
#define DEVICE_IDENTIFIER FOURCC('B', 'l', 'G', 'p')
#define ICON_ID (-16455)                /* (the Finder's custom icon: the file's own) */
#define MENU_ID 16967
#define STATE_MAGIC FOURCC('B', 'l', 'G', 'p')        /* (a device's state: and its version, the needs' controls) */
#define STATE_VERSION 1
#define CUR_AP_NAME 0x0910u
#define MOUSE_DOWN 1                    /* (an EventRecord's what) */             /* (low memory: the current application's name, a Str31) */

/*
 * The fragment's layout as a number: its imports, exports, entries and
 * data. A copy of another version's may be laid out otherwise, and then
 * isn't run (its calls would be taken for others).
 */
static uint32_t layout_hash(void)
{
	uint32_t h = 2166136261u;
	const char *names[2 * IMP_COUNT + EXPORTS];
	for (unsigned k = 0; k < IMP_COUNT; k++) {
		names[2 * k] = imports[k].library;
		names[2 * k + 1] = imports[k].name;
	}
	for (unsigned k = 0; k < EXPORTS; k++)
		names[2 * IMP_COUNT + k] = exports[k].name;
	for (unsigned k = 0; k < 2 * IMP_COUNT + EXPORTS; k++)
		for (const char *c = names[k]; ; c++) {
			h = (h ^ (uint8_t)*c) * 16777619u;
			if (!*c)
				break;
		}
	const uint32_t sizes[] = { E_COUNT, SELECTORS, X_SIZE, ISP_CONTROLS, ISP_NEEDS };
	for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++)
		h = (h ^ sizes[k]) * 16777619u;
	return h;
}

size_t isp_build(uint8_t *out, size_t size, struct ndrv_layout *layout)
{
	static uint8_t extra[X_SIZE];
	memset(extra, 0, sizeof extra);
	put_be32(extra + X_MAGIC, ISP_MAGIC);
	put_be32(extra + X_MAGIC + 4, layout_hash());
	extra[X_SPACE] = 1;
	extra[X_SPACE + 1] = ' ';
	for (unsigned k = 0; k <= ISP_CONTROLS; k++) {
		const char *name = k < ISP_CONTROLS ? isp_control_name(k) : "None";
		extra[X_NAMES + NAME_SLOT * k] = (uint8_t)strlen(name);
		memcpy(extra + X_NAMES + NAME_SLOT * k + 1, name, strlen(name));
	}
	const struct ndrv_spec spec = {
		.kind = FRAG_LIBRARY,
		.entries = E_COUNT,
		.init_entry = E_INIT,
		.term_entry = E_TERM,
		.exports = exports,
		.export_count = EXPORTS,
		.hostcall = HOSTCALL_TOOLS_ISP,
		.imports = imports,
		.import_count = IMP_COUNT,
		.extra = extra,
		.extra_len = sizeof extra,
	};
	return ndrv_build(&spec, out, size, layout);
}

void isp_init(struct isp *s, uint64_t timebase_hz)
{
	static uint8_t scratch[ISP_LIBRARY_MAX];
	memset(s, 0, sizeof *s);
	s->hz = timebase_hz;
	s->hash = layout_hash();
	isp_build(scratch, sizeof scratch, &s->layout);
}

/* ---- controls --------------------------------------------------------------------- */

static const char *const control_names[ISP_CONTROLS] = {
	"A", "B", "X", "Y", "Left shoulder", "Right shoulder", "Back", "Start", "Left stick button",
	"Right stick button", "Guide", "D-pad", "Left stick left/right", "Left stick up/down", "Right stick left/right",
	"Right stick up/down", "Left trigger", "Right trigger", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
	"Left stick left", "Left stick right", "Left stick up", "Left stick down", "Right stick left",
	"Right stick right", "Right stick up", "Right stick down", "Left stick",
};

const char *isp_control_name(unsigned control)
{
	return control < ISP_CONTROLS ? control_names[control] : "None";
}

/* The pad's buttons that are the elements' */
static const uint8_t element_button[ISP_DPAD] = {
	PAD_A, PAD_B, PAD_X, PAD_Y, PAD_LEFT_SHOULDER, PAD_RIGHT_SHOULDER, PAD_BACK, PAD_START, PAD_LEFT_STICK,
	PAD_RIGHT_STICK, PAD_GUIDE,
};
static const uint8_t element_axis[ISP_ELEMENTS - ISP_LEFT_X] = {
	PAD_LEFT_X, PAD_LEFT_Y, PAD_RIGHT_X, PAD_RIGHT_Y, PAD_LEFT_TRIGGER, PAD_RIGHT_TRIGGER,
};

static bool is_button(unsigned c)
{
	return c < ISP_DPAD || (c >= ISP_DPAD_UP && c < ISP_MOVEMENT) || c == ISP_LEFT_TRIGGER ||
	       c == ISP_RIGHT_TRIGGER;
}

bool isp_control_meets(unsigned c, uint32_t kind, uint32_t flags)
{
	(void)flags;
	switch (kind) {
	case ISP_KIND_BUTTON: return is_button(c);
	case ISP_KIND_AXIS: return c >= ISP_LEFT_X && c <= ISP_RIGHT_TRIGGER;
	case ISP_KIND_DELTA: return c >= ISP_LEFT_X && c <= ISP_RIGHT_Y;
	case ISP_KIND_DPAD: return c == ISP_DPAD;
	case ISP_KIND_MOVEMENT: return c == ISP_MOVEMENT;
	default: return false;
	}
}

#define PRESSED 16000                   /* (a stick or a trigger as a button: past half way) */

static int axis(const struct pad_slot *p, unsigned pad_axis)
{
	return p ? p->axis[pad_axis] : 0;
}

static bool down(const struct pad_slot *p, unsigned pad_button)
{
	return p && (p->down >> pad_button & 1);
}

static bool button(const struct pad_slot *p, unsigned c)
{
	switch (c) {
	case ISP_LEFT_TRIGGER: return axis(p, PAD_LEFT_TRIGGER) >= PRESSED;
	case ISP_RIGHT_TRIGGER: return axis(p, PAD_RIGHT_TRIGGER) >= PRESSED;
	case ISP_DPAD_UP: return down(p, PAD_UP);
	case ISP_DPAD_DOWN: return down(p, PAD_DOWN);
	case ISP_DPAD_LEFT: return down(p, PAD_LEFT);
	case ISP_DPAD_RIGHT: return down(p, PAD_RIGHT);
	case ISP_LEFT_X_LEFT: return axis(p, PAD_LEFT_X) <= -PRESSED;
	case ISP_LEFT_X_RIGHT: return axis(p, PAD_LEFT_X) >= PRESSED;
	case ISP_LEFT_Y_UP: return axis(p, PAD_LEFT_Y) <= -PRESSED;
	case ISP_LEFT_Y_DOWN: return axis(p, PAD_LEFT_Y) >= PRESSED;
	case ISP_RIGHT_X_LEFT: return axis(p, PAD_RIGHT_X) <= -PRESSED;
	case ISP_RIGHT_X_RIGHT: return axis(p, PAD_RIGHT_X) >= PRESSED;
	case ISP_RIGHT_Y_UP: return axis(p, PAD_RIGHT_Y) <= -PRESSED;
	case ISP_RIGHT_Y_DOWN: return axis(p, PAD_RIGHT_Y) >= PRESSED;
	default: return c < ISP_DPAD && down(p, element_button[c]);
	}
}

/* ISpDPadData: idle, then from left clockwise */
static uint32_t dpad(bool up, bool dn, bool left, bool right)
{
	int x = right - left, y = dn - up;
	static const uint8_t dir[3][3] = { { 2, 3, 4 }, { 1, 0, 5 }, { 8, 7, 6 } };    /* [y + 1][x + 1] */
	return dir[y + 1][x + 1];
}

/* (Up is a Y axis's maximum, and a positive delta, as games take them, though SDL has it negative) */
static bool is_y(unsigned c)
{
	return c == ISP_LEFT_Y || c == ISP_RIGHT_Y;
}

/* An axis, from its minimum (left, down) to its maximum (right, up); a trigger from 0 */
static uint32_t axis_data(const struct pad_slot *p, unsigned c)
{
	int v = axis(p, element_axis[c - ISP_LEFT_X]);
	if (c >= ISP_LEFT_TRIGGER)
		return v <= 0 ? 0 : (uint32_t)((uint64_t)v * 0xFFFFFFFFu / 32767);
	return (uint32_t)(is_y(c) ? 32767 - v : v + 32768) * 0x10001u;
}

uint32_t isp_data(const struct pad_slot *p, unsigned c, uint32_t kind, uint64_t ticks, uint64_t hz, int64_t *delta)
{
	switch (kind) {
	case ISP_KIND_BUTTON:
		return button(p, c);
	case ISP_KIND_AXIS:
		return c >= ISP_LEFT_X && c <= ISP_RIGHT_TRIGGER ? axis_data(p, c) : 0x7FFFFFFFu;
	case ISP_KIND_DPAD:
		return dpad(down(p, PAD_UP), down(p, PAD_DOWN), down(p, PAD_LEFT), down(p, PAD_RIGHT));
	case ISP_KIND_MOVEMENT: {
		int x = axis(p, PAD_LEFT_X), y = axis(p, PAD_LEFT_Y);
		return dpad(y <= -PRESSED, y >= PRESSED, x <= -PRESSED, x >= PRESSED);
	}
	case ISP_KIND_DELTA: {
		/* (the mouse's counts in the time passed, the stick's speed as the right stick moves the mouse, the
		 * rest of a unit kept for later) */
		if (c < ISP_LEFT_X || c > ISP_RIGHT_Y || !hz || !delta)
			return 0;
		int64_t speed = pad_stick_speed(axis(p, element_axis[c - ISP_LEFT_X]));
		int64_t v = (is_y(c) ? -speed : speed) * (int64_t)ticks + *delta;
		int64_t unit = (int64_t)hz * (65536 / ISP_DELTA_COUNT);
		*delta = v % unit;
		return (uint32_t)(int32_t)(v / unit);
	}
	default:
		return 0;
	}
}

/* ---- autoconfiguration -------------------------------------------------------------- */

/* Controls that can't both be given (a stick and its halves, or as a movement; the D-pad and its directions) */
static bool overlaps(unsigned a, unsigned b)
{
	static const uint8_t whole[ISP_CONTROLS] = {
		[ISP_DPAD_UP] = ISP_DPAD, [ISP_DPAD_DOWN] = ISP_DPAD, [ISP_DPAD_LEFT] = ISP_DPAD,
		[ISP_DPAD_RIGHT] = ISP_DPAD, [ISP_LEFT_X_LEFT] = ISP_LEFT_X, [ISP_LEFT_X_RIGHT] = ISP_LEFT_X,
		[ISP_LEFT_Y_UP] = ISP_LEFT_Y, [ISP_LEFT_Y_DOWN] = ISP_LEFT_Y, [ISP_RIGHT_X_LEFT] = ISP_RIGHT_X,
		[ISP_RIGHT_X_RIGHT] = ISP_RIGHT_X, [ISP_RIGHT_Y_UP] = ISP_RIGHT_Y, [ISP_RIGHT_Y_DOWN] = ISP_RIGHT_Y,
	};
	if (a == b)
		return true;
	if (a == ISP_MOVEMENT || b == ISP_MOVEMENT) {
		unsigned o = a == ISP_MOVEMENT ? b : a;
		return o == ISP_LEFT_X || o == ISP_LEFT_Y || (whole[o] == ISP_LEFT_X || whole[o] == ISP_LEFT_Y);
	}
	return (whole[a] && whole[a] == b) || (whole[b] && whole[b] == a);
}

static bool free_control(const bool *taken, unsigned c)
{
	for (unsigned k = 0; k < ISP_CONTROLS; k++)
		if (taken[k] && overlaps(k, c))
			return false;
	return true;
}

/*
 * The controls a need's label asks for, best first (none: nothing in
 * particular). A shooter's layout: the left stick moves and slides, the
 * right one turns and looks (as the mouse does), the triggers fire; a
 * flight or driving game's axes go to the left stick, its rudder or twist
 * to the right, its pedals to the triggers.
 */
#define PREFERRED 3
static const uint8_t *by_label(uint32_t kind, uint32_t label)
{
	static const struct { uint32_t kind, label; uint8_t control[PREFERRED]; } table[] = {
		{ ISP_KIND_AXIS, FOURCC('x', 'a', 'x', 'i'), { ISP_LEFT_X, ISP_RIGHT_X, ISP_NONE } },
		{ ISP_KIND_AXIS, FOURCC('y', 'a', 'x', 'i'), { ISP_LEFT_Y, ISP_RIGHT_Y, ISP_NONE } },
		{ ISP_KIND_AXIS, FOURCC('r', 'z', 'a', 'x'), { ISP_RIGHT_X, ISP_LEFT_X, ISP_NONE } },  /* (roll, yaw) */
		{ ISP_KIND_AXIS, FOURCC('r', 'y', 'a', 'x'), { ISP_RIGHT_X, ISP_LEFT_X, ISP_NONE } },
		{ ISP_KIND_AXIS, FOURCC('r', 'u', 'd', 'd'), { ISP_RIGHT_X, ISP_LEFT_X, ISP_NONE } },
		{ ISP_KIND_AXIS, FOURCC('r', 'x', 'a', 'x'), { ISP_RIGHT_Y, ISP_LEFT_Y, ISP_NONE } },  /* (pitch) */
		{ ISP_KIND_AXIS, FOURCC('z', 'a', 'x', 'i'), { ISP_RIGHT_Y, ISP_LEFT_Y, ISP_NONE } },
		{ ISP_KIND_AXIS, FOURCC('g', 'a', 's', 'p'), { ISP_RIGHT_TRIGGER, ISP_NONE } },
		{ ISP_KIND_AXIS, FOURCC('t', 'h', 'r', 't'), { ISP_RIGHT_TRIGGER, ISP_NONE } },
		{ ISP_KIND_AXIS, FOURCC('b', 'r', 'a', 'k'), { ISP_LEFT_TRIGGER, ISP_NONE } },
		{ ISP_KIND_AXIS, FOURCC('c', 'l', 't', 'c'), { ISP_LEFT_TRIGGER, ISP_NONE } },
		{ ISP_KIND_AXIS, FOURCC('t', 'o', 'e', 'b'), { ISP_LEFT_TRIGGER, ISP_NONE } },
		{ ISP_KIND_DELTA, FOURCC('x', 'd', 'l', 't'), { ISP_RIGHT_X, ISP_NONE } },
		{ ISP_KIND_DELTA, FOURCC('c', 'u', 'r', 'x'), { ISP_RIGHT_X, ISP_NONE } },
		{ ISP_KIND_DELTA, FOURCC('r', 'z', 'd', 'l'), { ISP_RIGHT_X, ISP_NONE } },
		{ ISP_KIND_DELTA, FOURCC('r', 'y', 'd', 'l'), { ISP_RIGHT_X, ISP_NONE } },
		{ ISP_KIND_DELTA, FOURCC('y', 'd', 'l', 't'), { ISP_RIGHT_Y, ISP_NONE } },
		{ ISP_KIND_DELTA, FOURCC('c', 'u', 'r', 'y'), { ISP_RIGHT_Y, ISP_NONE } },
		{ ISP_KIND_DELTA, FOURCC('r', 'x', 'd', 'l'), { ISP_RIGHT_Y, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('f', 'i', 'r', 'e'), { ISP_RIGHT_TRIGGER, ISP_RIGHT_SHOULDER, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('m', 'o', 'u', '1'), { ISP_RIGHT_TRIGGER, ISP_RIGHT_SHOULDER, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('s', 'f', 'i', 'r'), { ISP_LEFT_TRIGGER, ISP_LEFT_SHOULDER, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('m', 'o', 'u', '2'), { ISP_LEFT_TRIGGER, ISP_LEFT_SHOULDER, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('j', 'u', 'm', 'p'), { ISP_A, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('p', 'a', 'u', 's'), { ISP_START, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('s', 't', 'r', 't'), { ISP_BACK, ISP_NONE } },      /* (Quit: Esc too) */
		{ ISP_KIND_BUTTON, FOURCC('o', 'p', 't', 'n'), { ISP_BACK, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('b', 't', 'p', 'v'), { ISP_LEFT_SHOULDER, ISP_DPAD_LEFT, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('b', 't', 'n', 'x'), { ISP_RIGHT_SHOULDER, ISP_DPAD_RIGHT, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('b', 'l', 'f', 't'), { ISP_LEFT_X_LEFT, ISP_DPAD_LEFT, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('b', 'r', 'g', 't'), { ISP_LEFT_X_RIGHT, ISP_DPAD_RIGHT, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('b', 't', 'm', 'f'), { ISP_LEFT_Y_UP, ISP_DPAD_UP, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('b', 't', 'm', 'b'), { ISP_LEFT_Y_DOWN, ISP_DPAD_DOWN, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('b', 't', 't', 'l'), { ISP_RIGHT_X_LEFT, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('b', 't', 't', 'r'), { ISP_RIGHT_X_RIGHT, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('b', 't', 'l', 'l'), { ISP_RIGHT_X_LEFT, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('b', 't', 'l', 'r'), { ISP_RIGHT_X_RIGHT, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('b', 't', 'l', 'u'), { ISP_RIGHT_Y_UP, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('b', 't', 'l', 'd'), { ISP_RIGHT_Y_DOWN, ISP_NONE } },
		{ ISP_KIND_BUTTON, FOURCC('q', 'u', 'i', 'k'), { ISP_LEFT_STICK, ISP_NONE } },       /* (run) */
		{ ISP_KIND_BUTTON, FOURCC('b', 'l', 'o', 'k'), { ISP_RIGHT_STICK, ISP_NONE } },      /* (look) */
		{ ISP_KIND_DPAD, 0, { ISP_DPAD, ISP_NONE } },
		{ ISP_KIND_MOVEMENT, 0, { ISP_MOVEMENT, ISP_NONE } },
	};
	static const uint8_t none[] = { ISP_NONE };
	for (size_t k = 0; k < sizeof table / sizeof table[0]; k++)
		if (table[k].kind == kind && (table[k].label == label || !table[k].label))
			return table[k].control;
	return none;
}

/* ... and the controls taken in turn for the rest (Start, Back and Guide are left for menus: see pad.h) */
static const uint8_t *fallback(uint32_t kind, uint32_t flags)
{
	static const uint8_t buttons[] = { ISP_A, ISP_B, ISP_X, ISP_Y, ISP_LEFT_SHOULDER, ISP_RIGHT_SHOULDER,
					   ISP_RIGHT_TRIGGER, ISP_LEFT_TRIGGER, ISP_DPAD_UP, ISP_DPAD_DOWN, ISP_DPAD_LEFT,
					   ISP_DPAD_RIGHT, ISP_LEFT_STICK, ISP_RIGHT_STICK, ISP_NONE };
	static const uint8_t axes[] = { ISP_LEFT_X, ISP_LEFT_Y, ISP_RIGHT_X, ISP_RIGHT_Y, ISP_NONE };
	static const uint8_t asymmetric[] = { ISP_RIGHT_TRIGGER, ISP_LEFT_TRIGGER, ISP_NONE };
	static const uint8_t deltas[] = { ISP_RIGHT_X, ISP_RIGHT_Y, ISP_LEFT_X, ISP_LEFT_Y, ISP_NONE };
	static const uint8_t none[] = { ISP_NONE };
	switch (kind) {
	case ISP_KIND_BUTTON: return buttons;
	case ISP_KIND_AXIS: return flags & ISP_NEED_AXIS_ASYMMETRIC ? asymmetric : axes;
	case ISP_KIND_DELTA: return deltas;
	default: return none;
	}
}

/* (a list's first free control) */
static unsigned first_free(const bool *taken, const uint8_t *list)
{
	for (; *list != ISP_NONE; list++)
		if (free_control(taken, *list))
			return *list;
	return ISP_NONE;
}

/* A need's pass (see isp_autoconfigure()), or -1 if it's left alone */
static int pass_of(const struct isp_need *n)
{
	static const uint32_t moves[] = { FOURCC('b', 't', 'm', 'f'), FOURCC('b', 't', 'm', 'b'),
					  FOURCC('b', 'l', 'f', 't'), FOURCC('b', 'r', 'g', 't') };
	const uint8_t *wants = by_label(n->kind, n->label);
	switch (n->kind) {
	case ISP_KIND_DELTA:
		return 0;
	case ISP_KIND_AXIS:
		return wants[0] == ISP_LEFT_TRIGGER || wants[0] == ISP_RIGHT_TRIGGER ? 1 : 3;
	case ISP_KIND_BUTTON:
		if (n->flags & (ISP_NEED_BUTTON_ALREADY_AXIS | ISP_NEED_BUTTON_ALREADY_DELTA)) {
			for (size_t k = 0; k < sizeof moves / sizeof moves[0]; k++)
				if (n->label == moves[k])
					return 2;
			return -1;
		}
		return 2;
	default:
		return 3;
	}
}

void isp_autoconfigure(struct isp_need *needs, unsigned count, bool *used)
{
	bool taken[ISP_CONTROLS] = { false };
	for (unsigned k = 0; k < count; k++)
		needs[k].control = ISP_NONE;
	/*
	 * By what their labels ask for: deltas first (the right stick turns and
	 * looks, as the mouse does), then pedals and throttles (the triggers),
	 * then buttons, then the other axes; then the rest in turn. A button the
	 * game also has as an axis or a delta is left to them, but for moving
	 * and sliding: a shooter's "move" axis often looks up and down as well,
	 * so the left stick moves by its buttons, before any axis can take it.
	 */
	for (int pass = 0; pass < 5; pass++) {
		for (unsigned k = 0; k < count; k++) {
			struct isp_need *n = &needs[k];
			int of = pass_of(n);
			if (n->control != ISP_NONE || of < 0 || (used[k] && (n->flags & ISP_NEED_NO_MULTI_CONFIG)) ||
			    (n->flags & (ISP_NEED_NO_AUTO_CONFIG | ISP_NEED_UTILITY)))
				continue;
			unsigned c = pass == 4 ? first_free(taken, fallback(n->kind, n->flags)) :
				     pass == of ? first_free(taken, by_label(n->kind, n->label)) : ISP_NONE;
			if (c == ISP_NONE)
				continue;
			n->control = (uint8_t)c;
			taken[c] = true;
			used[k] = true;
		}
	}
}

/* ---- programs: Mac OS's routines called in turn ---------------------------------------- */

enum then {
	T_NONE, T_PSN, T_DEVICE, T_ELEMENT, T_SHOW_COUNT, T_SHOW_HANDLE, T_HIDE_COUNT, T_MOUSE, T_GLOBAL, T_POPUP,
};

static uint32_t data_at(const struct isp *s, const struct isp_instance *in)
{
	return in->toc + s->layout.extra;
}

static struct isp_program *begin(struct isp *s, unsigned inst)
{
	for (unsigned k = 0; k < ISP_PROGRAMS; k++) {
		struct isp_program *p = &s->prog[k];
		if (!p->busy) {
			p->busy = true;
			p->inst = inst;
			p->n = p->at = 0;
			p->value = 0;
			return p;
		}
	}
	return NULL;
}

/* A call, its arguments, and what's done once it returns; its index */
static unsigned op(struct isp_program *p, enum then then, uint32_t then_arg, unsigned imp, unsigned n, ...)
{
	if (p->n >= ISP_OPS)
		return ISP_OPS - 1;
	struct isp_op *o = &p->op[p->n];
	memset(o, 0, sizeof *o);
	o->imp = (uint8_t)imp;
	o->then = (uint8_t)then;
	o->then_arg = then_arg;
	o->n = (uint8_t)n;
	va_list ap;
	va_start(ap, n);
	for (unsigned k = 0; k < 8; k++) {
		o->from[k] = -1;
		if (k < n)
			o->a[k] = va_arg(ap, uint32_t);
	}
	va_end(ap);
	return p->n++;
}

#define CALL(p, imp, n, ...) op(p, T_NONE, 0, imp, n, __VA_ARGS__)

static void issue(struct isp *s, struct ndrv_call *c, struct isp_program *p)
{
	const struct isp_op *o = &p->op[p->at];
	uint32_t a[8];
	for (unsigned k = 0; k < 8; k++)
		a[k] = o->from[k] >= 0 ? p->result[o->from[k]] : o->a[k];
	ndrv_callout(c, ndrv_import(c, &s->layout, o->imp), a[0], a[1], a[2], a[3]);
	memcpy(&c->out[4], &a[4], 4 * sizeof a[0]);
	ndrv_set_state(c, 0, (uint32_t)(p - s->prog) + 1);
	ndrv_set_state(c, 1, p->at);
}

static void run(struct isp *s, struct ndrv_call *c, struct isp_program *p)
{
	if (p->at < p->n) {
		issue(s, c, p);
		return;
	}
	ndrv_finish(c, p->value);
	p->busy = false;
}

/* ---- what's pushed ------------------------------------------------------------------- */

static void put_pstring(struct mem *m, uint32_t at, const char *text, unsigned max)
{
	size_t n = strlen(text);
	n = n > max ? max : n;
	mem_w8(m, at, (uint8_t)n);
	memcpy(mem_ptr(m, at + 1), text, n);
}

/* The pads' changes since the last push, to their elements and the game's needs (`timer`: and the task again) */
static void push(struct isp *s, struct ndrv_call *c, struct isp_instance *in, struct isp_program *p,
		 const struct pad *pads, bool timer)
{
	struct mem *m = c->mem;
	uint32_t d = data_at(s, in);
	mem_w32(m, d + X_TIME, (uint32_t)(c->tb >> 32));
	mem_w32(m, d + X_TIME + 4, (uint32_t)c->tb);
	uint64_t ticks = in->pushed_tb && c->tb > in->pushed_tb ? c->tb - in->pushed_tb : 0;
	ticks = ticks < s->hz / 10 ? ticks : s->hz / 10;
	in->pushed_tb = c->tb;
	unsigned complex = 0;
	for (unsigned k = 0; k < PAD_MAX; k++) {
		struct isp_device *dev = &in->dev[k];
		if (!dev->registered)
			continue;
		const struct pad_slot *pad = pads && pads->slot[dev->pad].connected ? &pads->slot[dev->pad] : NULL;
		for (unsigned e = 0; e < ISP_ELEMENTS; e++) {
			uint32_t kind = e == ISP_DPAD ? ISP_KIND_DPAD : e >= ISP_LEFT_X ? ISP_KIND_AXIS : ISP_KIND_BUTTON;
			uint32_t v = isp_data(pad, e, kind, 0, 0, NULL);
			if (!dev->element[e] || (dev->pushed_any[e] && dev->pushed[e] == v))
				continue;
			dev->pushed_any[e] = true;
			dev->pushed[e] = v;
			CALL(p, IMP_PUSH_SIMPLE, 3, dev->element[e], v, d + X_TIME);
			s->pushes++;
		}
		if (!dev->init || !dev->active)
			continue;
		for (unsigned j = 0; j < dev->needs; j++) {
			struct isp_need *n = &dev->need[j];
			if (n->control == ISP_NONE || !n->element)
				continue;
			uint32_t v = isp_data(pad, n->control, n->kind, ticks, s->hz, &n->delta);
			if (n->kind == ISP_KIND_MOVEMENT) {
				uint32_t x = axis_data(pad, ISP_LEFT_X), y = axis_data(pad, ISP_LEFT_Y);
				uint32_t sum = x ^ (y * 31u) ^ (v * 0x9E3779B9u);
				if (n->pushed_any && n->pushed == sum)
					continue;
				n->pushed = sum;
				uint32_t at = d + X_COMPLEX + 12 * (complex++ % COMPLEX_SLOTS);
				mem_w32(m, at, x);
				mem_w32(m, at + 4, y);
				mem_w32(m, at + 8, v);
				n->pushed_any = true;
				CALL(p, IMP_PUSH_COMPLEX, 4, n->element, 12, at, d + X_TIME);
				s->pushes++;
				continue;
			}
			if (n->kind == ISP_KIND_DELTA ? !v : n->pushed_any && n->pushed == v)
				continue;
			n->pushed_any = true;
			n->pushed = v;
			CALL(p, IMP_PUSH_SIMPLE, 3, n->element, v, d + X_TIME);
			s->pushes++;
		}
	}
	if (timer)
		CALL(p, IMP_PRIME_TIME, 2, d + X_TASK, TIMER_MS);
}

/* ---- devices ----------------------------------------------------------------------- */

/* The elements' definitions (all but their device) */
static void define_elements(struct mem *m, uint32_t d)
{
	static const uint32_t labels[ISP_ELEMENTS] = {
		[ISP_START] = FOURCC('p', 'a', 'u', 's'), [ISP_BACK] = FOURCC('o', 'p', 't', 'n'),
		[ISP_DPAD] = FOURCC('m', 'o', 'v', 'e'), [ISP_LEFT_X] = FOURCC('x', 'a', 'x', 'i'),
		[ISP_LEFT_Y] = FOURCC('y', 'a', 'x', 'i'), [ISP_RIGHT_X] = FOURCC('r', 'z', 'a', 'x'),
		[ISP_RIGHT_Y] = FOURCC('r', 'x', 'a', 'x'), [ISP_LEFT_TRIGGER] = FOURCC('b', 'r', 'a', 'k'),
		[ISP_RIGHT_TRIGGER] = FOURCC('g', 'a', 's', 'p'),
	};
	for (unsigned e = 0; e < ISP_ELEMENTS; e++) {
		uint32_t at = d + X_ELDEF + 104 * e, config = d + X_CONFIG + 8 * e;
		memset(mem_ptr(m, at), 0, 104);
		memset(mem_ptr(m, config), 0, 8);
		mem_w32(m, at + 4, e >= ISP_LEFT_X && e <= ISP_LEFT_Y ? 1 : e >= ISP_RIGHT_X && e <= ISP_RIGHT_Y ? 2 : 0);
		put_pstring(m, at + 8, control_names[e], 63);
		mem_w32(m, at + 76, labels[e] ? labels[e] : FOURCC('n', 'o', 'n', 'e'));
		mem_w32(m, at + 80, config);
		mem_w32(m, at + 88, 4);                 /* (dataSize) */
		if (e < ISP_DPAD) {                     /* (ISpButtonConfigurationInfo: the order to give them in) */
			mem_w32(m, at + 72, ISP_KIND_BUTTON);
			mem_w32(m, at + 84, 4);
			mem_w32(m, config, e + 1);
		} else if (e == ISP_DPAD) {             /* (ISpDPadConfigurationInfo) */
			mem_w32(m, at + 72, ISP_KIND_DPAD);
			mem_w32(m, at + 84, 8);
			mem_w32(m, config, 1);
		} else {                                /* (ISpAxisConfigurationInfo: whether it's symmetric) */
			mem_w32(m, at + 72, ISP_KIND_AXIS);
			mem_w32(m, at + 84, 1);
			mem_w8(m, config, e < ISP_LEFT_TRIGGER);
		}
	}
}

static void write_rd(struct mem *m, uint32_t at, uint32_t proc_info, uint32_t tvector)
{
	memset(mem_ptr(m, at), 0, 32);
	mem_w16(m, at, 0xAAFE);                 /* _MixedModeMagic */
	mem_w8(m, at + 2, 7);                   /* version */
	mem_w32(m, at + 12, proc_info);
	mem_w8(m, at + 17, 1);                  /* kPowerPCISA */
	mem_w32(m, at + 20, tvector);
}

/* A device for each pad plugged in (not yet one), each with its elements; and the Time Manager task */
static void find_devices(struct isp *s, struct ndrv_call *c, struct isp_instance *in, struct isp_program *p,
			 const struct pad *pads)
{
	struct mem *m = c->mem;
	uint32_t d = data_at(s, in);
	define_elements(m, d);
	for (unsigned k = 0; k < PAD_MAX; k++) {
		struct isp_device *dev = &in->dev[k];
		if (dev->registered || !pads || !pads->slot[k].connected)
			continue;
		uint32_t def = d + X_DEVDEF + 92 * k;
		memset(mem_ptr(m, def), 0, 92);
		put_pstring(m, def, pads->slot[k].name, 63);
		mem_w32(m, def + 64, DEVICE_CLASS);
		mem_w32(m, def + 68, DEVICE_IDENTIFIER);
		mem_w32(m, def + 72, k + 1);            /* (permanentID: configurations are kept by it) */
		dev->pad = k;
		snprintf(dev->name, sizeof dev->name, "%s", pads->slot[k].name);
		op(p, T_DEVICE, k, IMP_DEVICE_NEW, 4, def, in->toc + s->layout.entry[E_META], k, d + X_OUT);
		for (unsigned e = 0; e < ISP_ELEMENTS; e++)
			op(p, T_ELEMENT, k << 8 | e, IMP_ELEMENT_NEW, 2, d + X_ELDEF + 104 * e, d + X_OUT + 4 + 4 * e);
	}
	if (!in->timer && p->n) {
		memset(mem_ptr(m, d + X_TASK), 0, 22);
		mem_w32(m, d + X_TASK + 6, d + X_TIMER_RD);     /* tmAddr */
		write_rd(m, d + X_TIMER_RD, TIMER_PROC_INFO, in->toc + s->layout.entry[E_TIMER]);
		CALL(p, IMP_INS_TIME, 1, d + X_TASK);
		CALL(p, IMP_PRIME_TIME, 2, d + X_TASK, TIMER_MS);
		in->timer = true;
	}
}

static void dispose_devices(struct isp *s, struct isp_instance *in, struct isp_program *p)
{
	uint32_t d = data_at(s, in);
	if (in->timer)
		CALL(p, IMP_RMV_TIME, 1, d + X_TASK);
	in->timer = false;
	for (unsigned k = 0; k < PAD_MAX; k++) {
		struct isp_device *dev = &in->dev[k];
		if (!dev->registered)
			continue;
		for (unsigned e = 0; e < ISP_ELEMENTS; e++)
			if (dev->element[e])
				CALL(p, IMP_ELEMENT_DISPOSE, 1, dev->element[e]);
		CALL(p, IMP_DEVICE_DISPOSE, 1, dev->ref);
		memset(dev, 0, sizeof *dev);
	}
}

/* ---- the panel --------------------------------------------------------------------- */

#define ROW 14                          /* (Geneva 9, a row) */
#define TOP 4
#define GENEVA 3

struct layout {
	int per_col, cols, col_w, top;
};

static struct layout panel_layout(const struct isp_device *dev)
{
	struct layout l;
	l.top = dev->rect[0] + TOP;
	l.per_col = (dev->rect[2] - l.top - 2) / ROW;
	l.per_col = l.per_col < 1 ? 1 : l.per_col;
	l.cols = (int)dev->rows > l.per_col ? 2 : 1;
	l.col_w = (dev->rect[3] - dev->rect[1] - 8) / l.cols;
	return l;
}

/* A row's box: left, right, top; and where its column starts */
static void box(const struct isp_device *dev, const struct layout *l, unsigned r, int *x0, int *x1, int *y, int *x)
{
	int col = (int)r / l->per_col, line = (int)r % l->per_col;
	*x = dev->rect[1] + 4 + col * l->col_w;
	*x0 = *x + l->col_w / 2;
	*x1 = *x + l->col_w - 8;
	*y = l->top + line * ROW;
}

/* The needs the panel lists: those this pad can meet that the user may see */
static void list_rows(struct isp_device *dev)
{
	dev->rows = 0;
	for (unsigned j = 0; j < dev->needs; j++) {
		bool any = false;
		for (unsigned k = 0; k < ISP_CONTROLS && !any; k++)
			any = isp_control_meets(k, dev->need[j].kind, dev->need[j].flags);
		if (any && !(dev->need[j].flags & ISP_NEED_INVISIBLE))
			dev->row[dev->rows++] = (uint8_t)j;
	}
}

/* Text drawn at the pen, cut to fit `width` (in a slot of the panel's text) */
static void text(struct mem *m, struct isp_program *p, uint32_t slot, const char *s, int width)
{
	put_pstring(m, slot, s, 63);
	CALL(p, IMP_TRUNC_STRING, 3, (uint32_t)(width > 0 ? width : 0), slot, 0);      /* (smTruncEnd) */
	CALL(p, IMP_DRAW_STRING, 1, slot);
}

/* Draws the panel: the game's needs this pad can meet, each with what does, in a popup's box */
static void draw(struct isp *s, struct mem *m, struct isp_instance *in, struct isp_device *dev, struct isp_program *p)
{
	uint32_t d = data_at(s, in);
	list_rows(dev);
	struct layout l = panel_layout(dev);
	CALL(p, IMP_SET_PORT, 1, dev->dialog);
	CALL(p, IMP_TEXT_FONT, 1, GENEVA);
	CALL(p, IMP_TEXT_SIZE, 1, 9);
	CALL(p, IMP_ERASE_RECT, 1, d + X_ITEM_RECT);
	for (unsigned r = 0; r < dev->rows && (int)r < 2 * l.per_col; r++) {
		const struct isp_need *n = &dev->need[dev->row[r]];
		int x0, x1, y, x;
		box(dev, &l, r, &x0, &x1, &y, &x);
		CALL(p, IMP_MOVE_TO, 2, x + 2, y + 10);
		text(m, p, d + X_TEXT + 128 * r, n->name, x0 - x - 8);
		CALL(p, IMP_MOVE_TO, 2, x0, y + 1);     /* (the box, its shadow, and its triangle) */
		CALL(p, IMP_LINE_TO, 2, x1, y + 1);
		CALL(p, IMP_LINE_TO, 2, x1, y + 12);
		CALL(p, IMP_LINE_TO, 2, x0, y + 12);
		CALL(p, IMP_LINE_TO, 2, x0, y + 1);
		CALL(p, IMP_MOVE_TO, 2, x0 + 1, y + 13);
		CALL(p, IMP_LINE_TO, 2, x1 + 1, y + 13);
		CALL(p, IMP_LINE_TO, 2, x1 + 1, y + 2);
		for (int k = 0; k < 3; k++) {
			CALL(p, IMP_MOVE_TO, 2, x1 - 10 + k, y + 5 + k);
			CALL(p, IMP_LINE_TO, 2, x1 - 4 - k, y + 5 + k);
		}
		CALL(p, IMP_MOVE_TO, 2, x0 + 3, y + 10);
		text(m, p, d + X_TEXT + 128 * r + 64, isp_control_name(n->control), x1 - x0 - 16);
	}
	CALL(p, IMP_TEXT_FONT, 1, 0);
	CALL(p, IMP_TEXT_SIZE, 1, 0);
}

/*
 * A click in the panel, at a point on the screen (an event's) or where the
 * mouse is (`event` 0): then (see then()) the box it's in pops up, and the
 * event is handled (if `handled`, as InputSprocket asks)
 */
static void click(struct isp *s, struct mem *m, struct isp_instance *in, struct isp_device *dev,
		  struct isp_program *p, uint32_t event, uint32_t handled)
{
	if (!dev->shown)
		return;
	uint32_t d = data_at(s, in);
	dev->handled_at = handled;
	CALL(p, IMP_SET_PORT, 1, dev->dialog);
	if (event) {
		mem_w32(m, d + X_POINT, mem_r32(m, event + 10));        /* (EventRecord.where) */
		op(p, T_MOUSE, (uint32_t)(dev - in->dev), IMP_GLOBAL_TO_LOCAL, 1, d + X_POINT);
	} else {
		op(p, T_MOUSE, (uint32_t)(dev - in->dev), IMP_GET_MOUSE, 1, d + X_POINT);
	}
}

static void popup(struct isp *s, struct ndrv_call *c, struct isp_instance *in, struct isp_device *dev,
		  struct isp_program *p, int v, int h)
{
	struct layout l = panel_layout(dev);
	uint32_t d = data_at(s, in);
	for (unsigned r = 0; r < dev->rows && (int)r < 2 * l.per_col; r++) {
		int x0, x1, y, x;
		box(dev, &l, r, &x0, &x1, &y, &x);
		if (h < x0 || h > x1 || v < y + 1 || v > y + 13)
			continue;
		const struct isp_need *n = &dev->need[dev->row[r]];
		if (dev->handled_at && mem_ok(c->mem, dev->handled_at, 1, MEM_W))
			mem_w8(c->mem, dev->handled_at, 1);
		if (n->flags & ISP_NEED_NO_CONFIG)
			return;
		dev->popup = r;
		dev->choices = 0;
		unsigned current = 0;
		dev->choice[dev->choices++] = ISP_NONE;
		for (unsigned k = 0; k < ISP_CONTROLS; k++) {
			if (!isp_control_meets(k, n->kind, n->flags))
				continue;
			if (k == n->control)
				current = dev->choices;
			dev->choice[dev->choices++] = (uint8_t)k;
		}
		unsigned menu = CALL(p, IMP_NEW_MENU, 2, MENU_ID, d + X_EMPTY);
		for (unsigned k = 0; k < dev->choices; k++) {
			unsigned name = dev->choice[k] == ISP_NONE ? ISP_CONTROLS : dev->choice[k];
			p->op[CALL(p, IMP_APPEND_MENU, 2, 0, d + X_SPACE)].from[0] = (int16_t)menu;
			p->op[CALL(p, IMP_SET_MENU_ITEM_TEXT, 3, 0, k + 1, d + X_NAMES + NAME_SLOT * name)].from[0] =
				(int16_t)menu;
		}
		p->op[CALL(p, IMP_CHECK_ITEM, 3, 0, current + 1, 1)].from[0] = (int16_t)menu;
		p->op[CALL(p, IMP_INSERT_MENU, 2, 0, 0xFFFFFFFFu)].from[0] = (int16_t)menu;
		mem_w16(c->mem, d + X_POINT, (uint16_t)(y + 1));
		mem_w16(c->mem, d + X_POINT + 2, (uint16_t)x0);
		op(p, T_GLOBAL, 0, IMP_LOCAL_TO_GLOBAL, 1, d + X_POINT);
		p->op[op(p, T_POPUP, (uint32_t)(dev - in->dev), IMP_POP_UP_MENU_SELECT, 4, 0, 0, 0, current + 1)].from[0] =
			(int16_t)menu;
		CALL(p, IMP_DELETE_MENU, 1, MENU_ID);
		p->op[CALL(p, IMP_DISPOSE_MENU, 1, 0)].from[0] = (int16_t)menu;
		return;
	}
}

/* What's done as a call returns */
static void then(struct isp *s, struct ndrv_call *c, struct isp_program *p, const struct isp_op *o, uint32_t result)
{
	struct isp_instance *in = &s->inst[p->inst];
	struct mem *m = c->mem;
	uint32_t d = data_at(s, in);
	struct isp_device *dev = &in->dev[(o->then_arg >> 8 & 0xFF) < PAD_MAX ? o->then_arg >> 8 & 0xFF : 0];
	switch (o->then) {
	case T_PSN:
		in->psn[0] = mem_r32(m, d + X_PSN);
		in->psn[1] = mem_r32(m, d + X_PSN + 4);
		break;
	case T_DEVICE:
		dev = &in->dev[o->then_arg];
		dev->ref = result ? 0 : mem_r32(m, d + X_OUT);
		dev->registered = dev->ref != 0;
		dev->active = true;
		for (unsigned e = 0; e < ISP_ELEMENTS; e++)
			mem_w32(m, d + X_ELDEF + 104 * e, dev->ref);
		printf("isp: %s is an InputSprocket device for %s%s\n", dev->name, in->name,
		       dev->registered ? "" : ": InputSprocket refused it");
		fflush(stdout);
		break;
	case T_ELEMENT:
		dev->element[o->then_arg & 0xFF] = result ? 0 : mem_r32(m, d + X_OUT + 4 + 4 * (o->then_arg & 0xFF));
		break;
	case T_SHOW_COUNT:
		dev = &in->dev[o->then_arg];
		dev->item = (int16_t)result + 1;
		p->op[p->at + 4].a[1] = (uint32_t)dev->item;    /* (SetDialogItem's) */
		break;
	case T_SHOW_HANDLE:
		if (result && mem_ok(m, result, 4, MEM_R)) {
			uint32_t at = mem_r32(m, result);
			if (mem_ok(m, at, 16, MEM_W)) {         /* a DITL: one enabled user item */
				dev = &in->dev[o->then_arg];
				memset(mem_ptr(m, at), 0, 16);
				for (unsigned k = 0; k < 4; k++)
					mem_w16(m, at + 6 + 2 * k, (uint16_t)dev->rect[k]);
			}
		}
		break;
	case T_HIDE_COUNT:
		dev = &in->dev[o->then_arg];
		p->op[p->at + 1].a[1] = (int16_t)result >= dev->item ? (uint32_t)((int16_t)result - dev->item + 1) : 0;
		break;
	case T_MOUSE:
		dev = &in->dev[o->then_arg];
		popup(s, c, in, dev, p, (int16_t)mem_r16(m, d + X_POINT), (int16_t)mem_r16(m, d + X_POINT + 2));
		break;
	case T_GLOBAL:
		p->op[p->at + 1].a[1] = (uint32_t)(int16_t)mem_r16(m, d + X_POINT);
		p->op[p->at + 1].a[2] = (uint32_t)(int16_t)mem_r16(m, d + X_POINT + 2);
		break;
	case T_POPUP: {
		dev = &in->dev[o->then_arg];
		unsigned item = result & 0xFFFF;
		if (item >= 1 && item <= dev->choices && dev->popup < dev->rows) {
			dev->need[dev->row[dev->popup]].control = dev->choice[item - 1];
			dev->need[dev->row[dev->popup]].pushed_any = false;
		}
		draw(s, c->mem, in, dev, p);
		break;
	}
	default:
		break;
	}
}

/* ---- the entries ------------------------------------------------------------------- */

static struct isp_instance *instance(struct isp *s, uint32_t toc, bool make)
{
	for (unsigned k = 0; k < ISP_INSTANCES; k++)
		if (s->inst[k].used && s->inst[k].toc == toc)
			return &s->inst[k];
	if (!make)
		return NULL;
	for (unsigned k = 0; k < ISP_INSTANCES; k++) {
		if (!s->inst[k].used) {
			memset(&s->inst[k], 0, sizeof s->inst[k]);
			s->inst[k].used = true;
			s->inst[k].toc = toc;
			return &s->inst[k];
		}
	}
	return NULL;
}

/* InputSprocket's Init: the game's needs, met by this device as far as it can */
static void fourcc_text(uint32_t v, char out[5])
{
	for (unsigned k = 0; k < 4; k++) {
		uint8_t ch = (uint8_t)(v >> (24 - 8 * k));
		out[k] = ch >= 32 && ch < 127 ? (char)ch : '?';
	}
	out[4] = 0;
}

static void init_needs(struct isp_instance *in, struct isp_device *dev, struct ndrv_call *c, bool log)
{
	struct mem *m = c->mem;
	uint32_t count = c->arg[1], needs = c->arg[2], elements = c->arg[3], used_at = c->arg[4];
	count = count > ISP_NEEDS ? ISP_NEEDS : count;
	if (!mem_ok(m, needs, ISP_NEED_SIZE * count, MEM_R) || !mem_ok(m, elements, 4 * count, MEM_R) ||
	    !mem_ok(m, used_at, count, MEM_RW))
		return;
	bool used[ISP_NEEDS];
	dev->needs = count;
	for (unsigned k = 0; k < count; k++) {
		struct isp_need *n = &dev->need[k];
		uint32_t at = needs + ISP_NEED_SIZE * k;
		memset(n, 0, sizeof *n);
		uint8_t len = mem_r8(m, at);
		len = len > 63 ? 63 : len;
		memcpy(n->name, mem_ptr(m, at + 1), len);
		n->kind = mem_r32(m, at + 68);
		n->label = mem_r32(m, at + 72);
		n->flags = mem_r32(m, at + 76);
		n->element = mem_r32(m, elements + 4 * k);
		used[k] = mem_r8(m, used_at + k) != 0;
		if (log) {
			char kind[5], label[5];
			fourcc_text(n->kind, kind);
			fourcc_text(n->label, label);
			printf("isp: need %u: \"%s\" %s %s flags %08X%s\n", k, n->name, kind, label, n->flags,
			       used[k] ? " (used)" : "");
		}
	}
	isp_autoconfigure(dev->need, count, used);
	unsigned met = 0;
	for (unsigned k = 0; k < count; k++) {
		mem_w8(m, used_at + k, used[k]);
		met += dev->need[k].control != ISP_NONE;
	}
	dev->init = true;
	printf("isp: %s asked for %u controls: %s meets %u of them:", in->name, count, dev->name, met);
	for (unsigned k = 0, said = 0; k < count; k++)
		if (dev->need[k].control != ISP_NONE)
			printf("%s %s %s", said++ ? "," : "", dev->need[k].name, isp_control_name(dev->need[k].control));
	printf("\n");
	fflush(stdout);
}

/* GetState: what meets each need, as InputSprocket keeps it for the game */
static uint32_t get_state(struct isp_device *dev, struct ndrv_call *c)
{
	struct mem *m = c->mem;
	uint32_t len = 6 + dev->needs, buflen = c->arg[1], buf = c->arg[2], out = c->arg[3];
	if (out && mem_ok(m, out, 4, MEM_W))
		mem_w32(m, out, len);
	if (!buf || buflen < len || !mem_ok(m, buf, len, MEM_W))
		return buf ? (uint32_t)-50 : 0;         /* (paramErr: too small) */
	mem_w32(m, buf, STATE_MAGIC);
	mem_w8(m, buf + 4, STATE_VERSION);
	mem_w8(m, buf + 5, (uint8_t)dev->needs);
	for (unsigned k = 0; k < dev->needs; k++)
		mem_w8(m, buf + 6 + k, dev->need[k].control);
	return 0;
}

static void set_state(struct isp_device *dev, struct ndrv_call *c)
{
	struct mem *m = c->mem;
	uint32_t len = c->arg[1], buf = c->arg[2];
	if (len < 6 || !mem_ok(m, buf, len, MEM_R) || mem_r32(m, buf) != STATE_MAGIC || mem_r8(m, buf + 4) != STATE_VERSION ||
	    mem_r8(m, buf + 5) != dev->needs || len < 6u + dev->needs)
		return;
	for (unsigned k = 0; k < dev->needs; k++) {
		uint8_t control = mem_r8(m, buf + 6 + k);
		if (control == ISP_NONE || (control < ISP_CONTROLS && isp_control_meets(control, dev->need[k].kind, 0))) {
			dev->need[k].control = control;
			dev->need[k].pushed_any = false;
		}
	}
}

/* An InputSprocket function of a device's (the selector's) */
static void selector(struct isp *s, struct ndrv_call *c, struct isp_instance *in, struct isp_program *p, unsigned sel,
		     const struct pad *pads)
{
	struct mem *m = c->mem;
	uint32_t d = data_at(s, in);
	struct isp_device *dev = &in->dev[c->arg[0] < PAD_MAX ? c->arg[0] : 0];
	switch (sel) {
	case SEL_INIT:
		init_needs(in, dev, c, s->log);
		break;
	case SEL_STOP:
		dev->init = false;
		dev->needs = 0;
		break;
	case SEL_GET_SIZE: {                    /* (minimum, best: Points, v then h; two columns of rows) */
		list_rows(dev);
		int best_v = TOP + (int)(dev->rows + 1) / 2 * ROW + 8;
		if (mem_ok(m, c->arg[1], 4, MEM_W)) {
			mem_w16(m, c->arg[1], 120);
			mem_w16(m, c->arg[1] + 2, 300);
		}
		if (mem_ok(m, c->arg[2], 4, MEM_W)) {
			mem_w16(m, c->arg[2], (uint16_t)(best_v > 120 ? best_v : 120));
			mem_w16(m, c->arg[2] + 2, 460);
		}
		break;
	}
	case SEL_HANDLE_EVENT:                  /* (InputSprocket's dialog's every event: a mouse-down may be ours) */
		if (mem_ok(m, c->arg[2], 1, MEM_W))
			mem_w8(m, c->arg[2], 0);
		if (mem_ok(m, c->arg[1], 16, MEM_R) && mem_r16(m, c->arg[1]) == MOUSE_DOWN)
			click(s, m, in, dev, p, c->arg[1], mem_ok(m, c->arg[2], 1, MEM_W) ? c->arg[2] : 0);
		break;
	case SEL_SHOW: {
		if (!mem_ok(m, c->arg[3], 8, MEM_R))
			break;
		unsigned k = (unsigned)(dev - in->dev);
		dev->dialog = c->arg[1];
		for (unsigned j = 0; j < 4; j++) {
			dev->rect[j] = (int16_t)mem_r16(m, c->arg[3] + 2 * j);
			mem_w16(m, d + X_ITEM_RECT + 2 * j, (uint16_t)dev->rect[j]);
		}
		dev->shown = true;
		write_rd(m, d + X_DRAW_RD, USER_ITEM_PROC_INFO, in->toc + s->layout.entry[E_DRAW]);
		op(p, T_SHOW_COUNT, k, IMP_COUNT_DITL, 1, dev->dialog);
		unsigned h = op(p, T_SHOW_HANDLE, k, IMP_NEW_HANDLE, 1, 16);
		p->op[CALL(p, IMP_APPEND_DITL, 3, dev->dialog, 0, 0)].from[1] = (int16_t)h;
		p->op[CALL(p, IMP_DISPOSE_HANDLE, 1, 0)].from[0] = (int16_t)h;
		CALL(p, IMP_SET_DIALOG_ITEM, 5, dev->dialog, 0, 0, d + X_DRAW_RD, d + X_ITEM_RECT);
		draw(s, c->mem, in, dev, p);
		break;
	}
	case SEL_HIDE:
		if (dev->shown) {
			op(p, T_HIDE_COUNT, (uint32_t)(dev - in->dev), IMP_COUNT_DITL, 1, dev->dialog);
			CALL(p, IMP_SHORTEN_DITL, 2, dev->dialog, 0);
		}
		dev->shown = false;
		break;
	case SEL_BEGIN:
		for (unsigned k = 0; k < dev->needs; k++)
			dev->before[k] = dev->need[k].control;
		break;
	case SEL_END:
		if (!(c->arg[1] & 0xFF))
			for (unsigned k = 0; k < dev->needs; k++)
				dev->need[k].control = dev->before[k];
		for (unsigned k = 0; k < dev->needs; k++)
			dev->need[k].pushed_any = false;
		break;
	case SEL_GET_ICON:
		if (mem_ok(m, c->arg[1], 2, MEM_W))
			mem_w16(m, c->arg[1], (uint16_t)ICON_ID);
		break;
	case SEL_GET_STATE:
		p->value = get_state(dev, c);
		break;
	case SEL_SET_STATE:
		set_state(dev, c);
		break;
	case SEL_DIRTY: {
		bool dirty = false;
		for (unsigned k = 0; k < dev->needs; k++)
			dirty |= dev->before[k] != dev->need[k].control;
		if (mem_ok(m, c->arg[1], 1, MEM_W))
			mem_w8(m, c->arg[1], dirty);
		break;
	}
	case SEL_SET_ACTIVE:
		dev->active = (c->arg[1] & 0xFF) != 0;
		for (unsigned k = 0; k < dev->needs; k++)
			dev->need[k].pushed_any = false;
		break;
	case SEL_ITEM_HIT:
		if ((int16_t)c->arg[1] == dev->item)
			click(s, m, in, dev, p, 0, 0);
		break;
	case SEL_TICKLE:
	case SEL_INTERRUPT_TICKLE:
		push(s, c, in, p, pads, false);
		break;
	case SEL_DRAW:
		if (dev->shown)
			draw(s, c->mem, in, dev, p);
		break;
	case SEL_CLICK:
		if (mem_ok(m, c->arg[1], 16, MEM_R))
			click(s, m, in, dev, p, c->arg[1], 0);
		break;
	case SEL_GET_CALIBRATION:
		if (mem_ok(m, c->arg[2], 4, MEM_W))
			mem_w32(m, c->arg[2], 0);
		break;
	case SEL_CALIBRATE_DIALOG:
		if (mem_ok(m, c->arg[1], 1, MEM_W))
			mem_w8(m, c->arg[1], 0);
		break;
	default:                                /* ADBReInit, SetCalibration */
		break;
	}
}

/* Whether the devices are the pads plugged in (InputSprocket 1.7 finds them again if not) */
static bool same_devices(const struct isp_instance *in, const struct pad *pads)
{
	for (unsigned k = 0; k < PAD_MAX; k++)
		if (in->dev[k].registered != (pads && pads->slot[k].connected))
			return false;
	return true;
}

void isp_call(struct isp *s, struct ndrv_call *c, const struct pad *pads)
{
	s->calls++;
	if (c->entry == NDRV_ENTRY_RESUME) {
		uint32_t slot = ndrv_state(c, 0) - 1, at = ndrv_state(c, 1);
		struct isp_program *p = slot < ISP_PROGRAMS && s->prog[slot].busy ? &s->prog[slot] : NULL;
		if (!p || at >= p->n) {
			ndrv_finish(c, 0);
			return;
		}
		p->result[at] = c->arg[0];
		p->at = at;
		then(s, c, p, &p->op[at], c->arg[0]);
		p->at = at + 1;
		run(s, c, p);
		return;
	}
	uint32_t at = c->toc + s->layout.extra;
	if (!mem_ok(c->mem, at, 8, MEM_R) || mem_r32(c->mem, at) != ISP_MAGIC || mem_r32(c->mem, at + 4) != s->hash) {
		if (!s->stale_said)
			printf("isp: the guest's Blue Game Controllers is from another version of Blue: update it (Blue "
			       "offers to, in a window), or by hand from a volume made with \"blue tools create\"\n");
		fflush(stdout);
		s->stale_said = true;
		ndrv_finish(c, 0);
		return;
	}
	struct isp_instance *in = instance(s, c->toc, c->entry == E_INIT || c->entry == E_FIND);
	struct isp_program *p = in ? begin(s, (unsigned)(in - s->inst)) : NULL;
	if (s->log && c->entry != E_TIMER && c->entry != E_TICKLE && c->entry != E_SEL + SEL_TICKLE - 1 &&
	    c->entry != E_SEL + SEL_INTERRUPT_TICKLE - 1) {
		static const char *const names[] = { "init", "term", "CheckConfiguration", "FindAndLoadDevices",
						     "DisposeDevices", "Tickle", "meta" };
		static const char *const sels[SELECTORS] = {
			"Init", "Stop", "GetSize", "HandleEvent", "Show", "Hide", "BeginConfiguration",
			"EndConfiguration", "GetIcon", "GetState", "SetState", "Dirty", "SetActive", "DialogItemHit",
			"Tickle", "InterruptTickle", "Draw", "Click", "ADBReInit", "GetCalibration", "SetCalibration",
			"CalibrateDialog" };
		const char *name = c->entry >= E_COUNT ? "?" : c->entry < E_SEL ? names[c->entry] :
				   c->entry < E_TIMER ? sels[c->entry - E_SEL] : c->entry == E_TIMER ? "timer" : "draw";
		printf("isp: %s (%08X %08X %08X %08X)%s\n", name, c->arg[0], c->arg[1], c->arg[2], c->arg[3],
		       in ? "" : ": no instance");
		fflush(stdout);
	}
	if (!p) {
		ndrv_finish(c, 0);
		return;
	}
	struct mem *m = c->mem;
	uint32_t d = data_at(s, in);
	if (!in->name[0] && mem_ok(m, CUR_AP_NAME, 32, MEM_R)) {
		uint8_t len = mem_r8(m, CUR_AP_NAME);
		snprintf(in->name, sizeof in->name, "%.*s", len < 31 ? len : 31, (const char *)mem_ptr(m, CUR_AP_NAME + 1));
	}
	switch (c->entry) {
	case E_INIT:                            /* (InputSprocket before 1.7 takes the devices made now) */
		op(p, T_PSN, 0, IMP_GET_CURRENT_PROCESS, 1, d + X_PSN);
		find_devices(s, c, in, p, pads);
		break;
	case E_FIND:
		if (mem_ok(m, c->arg[0], 1, MEM_W))
			mem_w8(m, c->arg[0], 1);        /* (keepDriverLoaded) */
		find_devices(s, c, in, p, pads);
		break;
	case E_CHECK:
		if (mem_ok(m, c->arg[0], 1, MEM_W))
			mem_w8(m, c->arg[0], same_devices(in, pads));
		break;
	case E_DISPOSE:
		dispose_devices(s, in, p);
		break;
	case E_TERM:                            /* (InputSprocket may be gone: only the task goes) */
		if (in->timer)
			CALL(p, IMP_RMV_TIME, 1, d + X_TASK);
		in->used = false;
		break;
	case E_TICKLE:
		push(s, c, in, p, pads, false);
		break;
	case E_TIMER:
		if (in->timer)
			push(s, c, in, p, pads, true);
		break;
	case E_DRAW:
		for (unsigned k = 0; k < PAD_MAX; k++)
			if (in->dev[k].shown && in->dev[k].dialog == c->arg[0])
				draw(s, c->mem, in, &in->dev[k], p);
		break;
	case E_META:
		p->value = c->arg[1] >= 1 && c->arg[1] <= SELECTORS ? in->toc + s->layout.entry[E_SEL + c->arg[1] - 1] : 0;
		break;
	default:
		if (c->entry >= E_SEL && c->entry < E_SEL + SELECTORS)
			selector(s, c, in, p, c->entry - E_SEL + 1, pads);
		break;
	}
	run(s, c, p);
}

bool isp_in_use(const struct isp *s, uint32_t psn[2], const char **name)
{
	for (unsigned i = 0; i < ISP_INSTANCES; i++) {
		const struct isp_instance *in = &s->inst[i];
		for (unsigned k = 0; in->used && k < PAD_MAX; k++) {
			if (in->dev[k].registered && in->dev[k].init && in->dev[k].active) {
				if (psn) {
					psn[0] = in->psn[0];
					psn[1] = in->psn[1];
				}
				if (name)
					*name = in->name;
				return true;
			}
		}
	}
	return false;
}

/* The pad's controls (pad.h's) that one does */
static uint32_t pad_controls(unsigned c)
{
	const uint32_t dpad = 1u << PAD_UP | 1u << PAD_DOWN | 1u << PAD_LEFT | 1u << PAD_RIGHT;
	const uint32_t x = 1u << PAD_STICK_LEFT | 1u << PAD_STICK_RIGHT, y = 1u << PAD_STICK_UP | 1u << PAD_STICK_DOWN;
	switch (c) {
	case ISP_DPAD: return dpad;
	case ISP_DPAD_UP: return 1u << PAD_UP;
	case ISP_DPAD_DOWN: return 1u << PAD_DOWN;
	case ISP_DPAD_LEFT: return 1u << PAD_LEFT;
	case ISP_DPAD_RIGHT: return 1u << PAD_RIGHT;
	case ISP_LEFT_X: return x;
	case ISP_LEFT_Y: return y;
	case ISP_LEFT_X_LEFT: return 1u << PAD_STICK_LEFT;
	case ISP_LEFT_X_RIGHT: return 1u << PAD_STICK_RIGHT;
	case ISP_LEFT_Y_UP: return 1u << PAD_STICK_UP;
	case ISP_LEFT_Y_DOWN: return 1u << PAD_STICK_DOWN;
	case ISP_MOVEMENT: return x | y;
	case ISP_LEFT_TRIGGER: return 1u << PAD_LT;
	case ISP_RIGHT_TRIGGER: return 1u << PAD_RT;
	case ISP_RIGHT_X: case ISP_RIGHT_Y: case ISP_RIGHT_X_LEFT: case ISP_RIGHT_X_RIGHT: case ISP_RIGHT_Y_UP:
	case ISP_RIGHT_Y_DOWN:
		return PAD_TAKEN_RIGHT_STICK;
	default:
		return c < ISP_DPAD ? 1u << element_button[c] : 0;
	}
}

uint32_t isp_taken(const struct isp *s, unsigned pad)
{
	uint32_t taken = 0;
	for (unsigned i = 0; i < ISP_INSTANCES; i++) {
		const struct isp_instance *in = &s->inst[i];
		for (unsigned k = 0; in->used && k < PAD_MAX; k++) {
			const struct isp_device *dev = &in->dev[k];
			if (!dev->registered || !dev->init || !dev->active || dev->pad != pad)
				continue;
			for (unsigned j = 0; j < dev->needs; j++)
				if (dev->need[j].control != ISP_NONE)
					taken |= pad_controls(dev->need[j].control);
		}
	}
	return taken;
}
