#include "pad.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "be.h"

/* (the virtual key codes of keys without a character) */
#define VK_RETURN 0x24
#define VK_TAB 0x30
#define VK_SPACE 0x31
#define VK_ESCAPE 0x35
#define VK_COMMAND 0x37
#define VK_SHIFT 0x38
#define VK_OPTION 0x3A
#define VK_CONTROL 0x3B
#define VK_LEFT 0x7B
#define VK_RIGHT 0x7C
#define VK_DOWN 0x7D
#define VK_UP 0x7E

#define KEY(vk, ch) { PAD_DO_KEY, 0, vk, ch }
#define MODIFIER(vk) { PAD_DO_MODIFIER, 0, vk, 0 }

const struct pad_map pad_default_map = {
	.control = {
		[PAD_A] = KEY(VK_RETURN, 0x0D),
		[PAD_B] = { PAD_DO_KEY, PAD_MOD_COMMAND, 0x2F, '.' },
		[PAD_X] = KEY(VK_SPACE, ' '),
		[PAD_Y] = KEY(VK_TAB, 0x09),
		[PAD_BACK] = MODIFIER(VK_COMMAND),
		[PAD_START] = KEY(VK_ESCAPE, 0x1B),
		[PAD_RIGHT_STICK] = { PAD_DO_MOUSE, 0, 0, 0 },
		[PAD_LEFT_SHOULDER] = MODIFIER(VK_SHIFT),
		[PAD_RIGHT_SHOULDER] = MODIFIER(VK_OPTION),
		[PAD_UP] = KEY(VK_UP, 0x1E),
		[PAD_DOWN] = KEY(VK_DOWN, 0x1F),
		[PAD_LEFT] = KEY(VK_LEFT, 0x1C),
		[PAD_RIGHT] = KEY(VK_RIGHT, 0x1D),
		[PAD_TOUCHPAD] = { PAD_DO_MOUSE, 0, 0, 0 },
		[PAD_STICK_UP] = KEY(VK_UP, 0x1E),
		[PAD_STICK_DOWN] = KEY(VK_DOWN, 0x1F),
		[PAD_STICK_LEFT] = KEY(VK_LEFT, 0x1C),
		[PAD_STICK_RIGHT] = KEY(VK_RIGHT, 0x1D),
		[PAD_LT] = { PAD_DO_MOUSE, PAD_MOD_CONTROL, 0, 0 },    /* (a contextual menu's click) */
		[PAD_RT] = { PAD_DO_MOUSE, 0, 0, 0 },
		[PAD_RSTICK_UP] = KEY(VK_UP, 0x1E),                     /* (once the right stick is keys) */
		[PAD_RSTICK_DOWN] = KEY(VK_DOWN, 0x1F),
		[PAD_RSTICK_LEFT] = KEY(VK_LEFT, 0x1C),
		[PAD_RSTICK_RIGHT] = KEY(VK_RIGHT, 0x1D),
	},
	.stick = { PAD_STICK_KEYS, PAD_STICK_MOUSE },
};

/* The AppleJack switch each control is, plus one (0: none); see pad.h */
static const uint8_t applejack[PAD_CONTROLS] = {
	[PAD_A] = 15, [PAD_B] = 16, [PAD_X] = 10, [PAD_Y] = 9,         /* red S8, green S7, yellow S10, blue S9 */
	[PAD_BACK] = 1, [PAD_GUIDE] = 2, [PAD_START] = 3,               /* the system buttons S11, S12, S13 */
	[PAD_MISC] = 2, [PAD_TOUCHPAD] = 2,
	[PAD_LEFT_SHOULDER] = 18, [PAD_RIGHT_SHOULDER] = 17,            /* left fire S1, right fire S2 */
	[PAD_LT] = 18, [PAD_RT] = 17,
	[PAD_UP] = 11, [PAD_LEFT] = 12, [PAD_RIGHT] = 13, [PAD_DOWN] = 14,     /* the D-pad: S3 to S6 */
	[PAD_STICK_UP] = 11, [PAD_STICK_LEFT] = 12, [PAD_STICK_RIGHT] = 13, [PAD_STICK_DOWN] = 14,
};

int pad_applejack_switch(unsigned control)
{
	return control < PAD_CONTROLS ? applejack[control] - 1 : -1;
}

/* (the driver's, in the Pippin's ROM, as Apple's note gives them) */
const struct pad_switch pad_applejack_defaults[PAD_AJ_SWITCHES] = {
	KEY(0x67, 0x10), KEY(0x6F, 0x10), KEY(0x69, 0x10),      /* the system buttons: F11, F12, F13 */
	[8] = KEY(0x6B, 0x10),                                  /* blue: F14 */
	[9] = KEY(0x26, 'j'),                                   /* yellow: J */
	[10] = KEY(VK_UP, 0x1E), [11] = KEY(VK_LEFT, 0x1C), [12] = KEY(VK_RIGHT, 0x1D), [13] = KEY(VK_DOWN, 0x1F),
	[14] = KEY(0x64, 0x10),                                 /* red: F8 */
	[15] = KEY(0x28, 'k'),                                  /* green: K */
	[16] = { PAD_DO_MOUSE, 0, 0, 0 }, [17] = { PAD_DO_MOUSE, 0, 0, 0 },
};

bool pad_pipp(const uint8_t *data, size_t len, struct pad_switch map[PAD_AJ_SWITCHES])
{
	if (len < PAD_AJ_MAP + 4 * PAD_AJ_SWITCHES || be32(data) != PAD_AJ_SIGNATURE)
		return false;
	for (unsigned k = 0; k < PAD_AJ_SWITCHES; k++) {
		const uint8_t *s = data + PAD_AJ_MAP + 4 * k;
		/* (a key's modifiers mean something, others' are whatever the editor left) */
		map[k] = (struct pad_switch){ s[0], s[0] == PAD_DO_KEY ? s[1] & 0x1F : 0, s[2], s[3] };
	}
	return true;
}

uint8_t pad_adb_key(uint8_t vk)
{
	/* (the ADB keyboards' 'KMAP' the other way round: the arrows and modifiers differ) */
	switch (vk) {
	case VK_LEFT: return 0x3B;
	case VK_RIGHT: return 0x3C;
	case VK_DOWN: return 0x3D;
	case VK_UP: return 0x3E;
	case VK_CONTROL: case 0x36: case 0x3E: return 0x36;     /* (0x36: Apple's note's kControlKey; 0x3E: right) */
	case 0x3C: return 0x38;                                 /* right shift and option, as the left ones */
	case 0x3D: return 0x3A;
	case 0x39: return 0xFF;                                 /* caps lock (it latches) */
	default: return vk < 0x7B ? vk : 0xFF;
	}
}

/* The keys a control can be (a US keyboard's, by virtual key code), by name, with their characters */
static const struct key {
	uint8_t vk;
	char chr;
	const char *name;
} keys[] = {
	{ 0x00, 'a', "A" }, { 0x0B, 'b', "B" }, { 0x08, 'c', "C" }, { 0x02, 'd', "D" }, { 0x0E, 'e', "E" },
	{ 0x03, 'f', "F" }, { 0x05, 'g', "G" }, { 0x04, 'h', "H" }, { 0x22, 'i', "I" }, { 0x26, 'j', "J" },
	{ 0x28, 'k', "K" }, { 0x25, 'l', "L" }, { 0x2E, 'm', "M" }, { 0x2D, 'n', "N" }, { 0x1F, 'o', "O" },
	{ 0x23, 'p', "P" }, { 0x0C, 'q', "Q" }, { 0x0F, 'r', "R" }, { 0x01, 's', "S" }, { 0x11, 't', "T" },
	{ 0x20, 'u', "U" }, { 0x09, 'v', "V" }, { 0x0D, 'w', "W" }, { 0x07, 'x', "X" }, { 0x10, 'y', "Y" },
	{ 0x06, 'z', "Z" },
	{ 0x1D, '0', "0" }, { 0x12, '1', "1" }, { 0x13, '2', "2" }, { 0x14, '3', "3" }, { 0x15, '4', "4" },
	{ 0x17, '5', "5" }, { 0x16, '6', "6" }, { 0x1A, '7', "7" }, { 0x1C, '8', "8" }, { 0x19, '9', "9" },
	{ 0x1B, '-', "Minus" }, { 0x18, '=', "Equals" }, { 0x21, '[', "Left Bracket" }, { 0x1E, ']', "Right Bracket" },
	{ 0x2A, '\\', "Backslash" }, { 0x29, ';', "Semicolon" }, { 0x27, '\'', "Quote" }, { 0x2B, ',', "Comma" },
	{ 0x2F, '.', "Period" }, { 0x2C, '/', "Slash" }, { 0x32, '`', "Grave" }, { 0x0A, 0, "Section" },
	{ VK_RETURN, 0x0D, "Return" }, { VK_TAB, 0x09, "Tab" }, { VK_SPACE, ' ', "Space" }, { 0x33, 0x08, "Delete" },
	{ VK_ESCAPE, 0x1B, "Esc" }, { 0x4C, 0x03, "Enter" },
	{ VK_UP, 0x1E, "Up Arrow" }, { VK_DOWN, 0x1F, "Down Arrow" }, { VK_LEFT, 0x1C, "Left Arrow" },
	{ VK_RIGHT, 0x1D, "Right Arrow" }, { 0x72, 0x05, "Help" }, { 0x73, 0x01, "Home" }, { 0x77, 0x04, "End" },
	{ 0x74, 0x0B, "Page Up" }, { 0x79, 0x0C, "Page Down" }, { 0x75, 0x7F, "Forward Delete" },
	{ 0x7A, 0x10, "F1" }, { 0x78, 0x10, "F2" }, { 0x63, 0x10, "F3" }, { 0x76, 0x10, "F4" }, { 0x60, 0x10, "F5" },
	{ 0x61, 0x10, "F6" }, { 0x62, 0x10, "F7" }, { 0x64, 0x10, "F8" }, { 0x65, 0x10, "F9" }, { 0x6D, 0x10, "F10" },
	{ 0x67, 0x10, "F11" }, { 0x6F, 0x10, "F12" }, { 0x69, 0x10, "F13" }, { 0x6B, 0x10, "F14" }, { 0x71, 0x10, "F15" },
	{ 0x52, '0', "Keypad 0" }, { 0x53, '1', "Keypad 1" }, { 0x54, '2', "Keypad 2" }, { 0x55, '3', "Keypad 3" },
	{ 0x56, '4', "Keypad 4" }, { 0x57, '5', "Keypad 5" }, { 0x58, '6', "Keypad 6" }, { 0x59, '7', "Keypad 7" },
	{ 0x5B, '8', "Keypad 8" }, { 0x5C, '9', "Keypad 9" }, { 0x41, '.', "Keypad Period" },
	{ 0x45, '+', "Keypad Plus" }, { 0x4E, '-', "Keypad Minus" }, { 0x43, '*', "Keypad Times" },
	{ 0x4B, '/', "Keypad Divide" }, { 0x51, '=', "Keypad Equals" }, { 0x47, 0x1B, "Clear" },
	/* (modifier keys, alone: held as long as the control is) */
	{ VK_COMMAND, 0, "Command" }, { VK_SHIFT, 0, "Shift" }, { VK_OPTION, 0, "Option" }, { VK_CONTROL, 0, "Control" },
};
#define KEYS (sizeof keys / sizeof keys[0])

static const struct { uint8_t bit; const char *name; } modifier_names[] = {
	{ PAD_MOD_COMMAND, "Command" }, { PAD_MOD_SHIFT, "Shift" }, { PAD_MOD_OPTION, "Option" },
	{ PAD_MOD_CONTROL, "Control" },
};
#define MODIFIER_NAMES (sizeof modifier_names / sizeof modifier_names[0])

static const struct key *key_by_vk(uint8_t vk)
{
	for (size_t k = 0; k < KEYS; k++)
		if (keys[k].vk == vk)
			return &keys[k];
	return NULL;
}

const char *pad_key_name(uint8_t vk)
{
	const struct key *k = key_by_vk(vk);
	return k ? k->name : NULL;
}

static bool is_modifier_key(uint8_t vk)
{
	return vk == VK_COMMAND || vk == VK_SHIFT || vk == VK_OPTION || vk == VK_CONTROL;
}

uint8_t pad_virtual_key(uint8_t adb)
{
	switch (adb) {
	case 0x3B: return VK_LEFT;
	case 0x3C: return VK_RIGHT;
	case 0x3D: return VK_DOWN;
	case 0x3E: return VK_UP;
	case 0x36: case 0x7D: return VK_CONTROL;
	case 0x7B: return VK_SHIFT;
	case 0x7C: return VK_OPTION;
	default: return key_by_vk(adb) ? adb : 0xFF;
	}
}

struct pad_switch pad_key_action(uint8_t vk, uint8_t modifiers)
{
	const struct key *k = key_by_vk(vk);
	if (!k)
		return (struct pad_switch){ PAD_DO_NOTHING, 0, 0, 0 };
	if (is_modifier_key(vk))
		return (struct pad_switch){ PAD_DO_MODIFIER, 0, vk, 0 };
	return (struct pad_switch){ PAD_DO_KEY, (uint8_t)(modifiers & (PAD_MOD_COMMAND | PAD_MOD_SHIFT | PAD_MOD_OPTION |
								      PAD_MOD_CONTROL)), vk, (uint8_t)k->chr };
}

void pad_action_name(const struct pad_switch *s, char *out, size_t size)
{
	char mods[48] = "";
	const struct key *k = key_by_vk(s->key);
	if ((s->function == PAD_DO_KEY || s->function == PAD_DO_MOUSE) && s->modifiers)
		for (size_t n = 0; n < MODIFIER_NAMES; n++)
			if (s->modifiers & modifier_names[n].bit)
				snprintf(mods + strlen(mods), sizeof mods - strlen(mods), "%s-", modifier_names[n].name);
	switch (s->function) {
	case PAD_DO_MOUSE:
		snprintf(out, size, "%sClick", mods);
		return;
	case PAD_DO_DOUBLE_CLICK:
		snprintf(out, size, "Double-Click");
		return;
	case PAD_DO_MOUSE_LOCK:
		snprintf(out, size, "Click Lock");
		return;
	case PAD_DO_KEY:
	case PAD_DO_MODIFIER:
		if (k) {
			snprintf(out, size, "%s%s", s->function == PAD_DO_KEY ? mods : "", k->name);
			return;
		}
		break;
	default:
		break;
	}
	snprintf(out, size, "Nothing");
}

/* Whether `text` starts with `word` (ignoring case) followed by `end` (or the end of the text) */
static size_t starts(const char *text, const char *word, char end)
{
	size_t n = strlen(word);
	for (size_t k = 0; k < n; k++)
		if (tolower((unsigned char)text[k]) != tolower((unsigned char)word[k]))
			return 0;
	return text[n] == end || (!end && !text[n]) ? n : 0;
}

bool pad_action_parse(const char *text, struct pad_switch *s)
{
	uint8_t mods = 0;
	while (*text == ' ')
		text++;
	for (bool more = true; more;) {
		more = false;
		for (size_t n = 0; n < MODIFIER_NAMES; n++) {
			size_t len = starts(text, modifier_names[n].name, '-');
			if (len && text[len + 1]) {
				mods |= modifier_names[n].bit;
				text += len + 1;
				more = true;
			}
		}
	}
	size_t len = strlen(text);
	while (len && text[len - 1] == ' ')
		len--;
	char word[32];
	if (len >= sizeof word)
		return false;
	memcpy(word, text, len);
	word[len] = '\0';
	if (starts(word, "Nothing", 0) && !mods) {
		*s = (struct pad_switch){ PAD_DO_NOTHING, 0, 0, 0 };
		return true;
	}
	if (starts(word, "Click", 0)) {
		*s = (struct pad_switch){ PAD_DO_MOUSE, mods, 0, 0 };
		return true;
	}
	if ((starts(word, "Double-Click", 0) || starts(word, "Click Lock", 0)) && !mods) {
		*s = (struct pad_switch){ word[0] == 'D' || word[0] == 'd' ? PAD_DO_DOUBLE_CLICK : PAD_DO_MOUSE_LOCK, 0, 0, 0 };
		return true;
	}
	for (size_t k = 0; k < KEYS; k++) {
		/* (a character: the first key that types it, the keypad's after the others) */
		if (starts(word, keys[k].name, 0) || (len == 1 && keys[k].chr == word[0] && keys[k].chr > ' ')) {
			if (is_modifier_key(keys[k].vk) && mods)
				return false;
			*s = pad_key_action(keys[k].vk, mods);
			return true;
		}
	}
	return false;
}

bool pad_maps_equal(const struct pad_map *a, const struct pad_map *b)
{
	if (a->stick[0] != b->stick[0] || a->stick[1] != b->stick[1])
		return false;
	for (unsigned k = 0; k < PAD_CONTROLS; k++) {
		const struct pad_switch *x = &a->control[k], *y = &b->control[k];
		/* (the same action: a key's character isn't the action's) */
		if (x->function != y->function || x->modifiers != y->modifiers ||
		    ((x->function == PAD_DO_KEY || x->function == PAD_DO_MODIFIER) && x->key != y->key))
			return false;
	}
	return true;
}

static const char *const button_names[PAD_BUTTONS] = {
	"a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder", "rightshoulder",
	"dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad",
};
static const char *const axis_names[PAD_AXES] = {
	"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger",
};

int pad_button_named(const char *name)
{
	for (int k = 0; k < PAD_BUTTONS; k++)
		if (!strcmp(name, button_names[k]))
			return k;
	return -1;
}

int pad_axis_named(const char *name)
{
	for (int k = 0; k < PAD_AXES; k++)
		if (!strcmp(name, axis_names[k]))
			return k;
	return -1;
}

const char *pad_button_name(unsigned button)
{
	return button < PAD_BUTTONS ? button_names[button] : "?";
}

const char *pad_axis_name(unsigned axis)
{
	return axis < PAD_AXES ? axis_names[axis] : "?";
}

static const char *const control_names[PAD_CONTROLS - PAD_BUTTONS] = {
	"leftstickup", "leftstickdown", "leftstickleft", "leftstickright", "lefttrigger", "righttrigger",
	"rightstickup", "rightstickdown", "rightstickleft", "rightstickright",
};

const char *pad_control_name(unsigned control)
{
	return control < PAD_BUTTONS ? button_names[control] : control < PAD_CONTROLS ?
	       control_names[control - PAD_BUTTONS] : "?";
}

int pad_control_named(const char *name)
{
	for (unsigned k = 0; k < PAD_CONTROLS; k++)
		if (!strcmp(name, pad_control_name(k)))
			return (int)k;
	return -1;
}

static const char *key_name(uint8_t vk, uint8_t chr, char *buf, size_t size)
{
	static const struct { uint8_t vk; const char *name; } names[] = {
		{ VK_RETURN, "Return" }, { VK_TAB, "Tab" }, { VK_SPACE, "Space" }, { 0x33, "Delete" },
		{ VK_ESCAPE, "Esc" }, { VK_COMMAND, "Command" }, { VK_SHIFT, "Shift" }, { 0x39, "Caps Lock" },
		{ VK_OPTION, "Option" }, { VK_CONTROL, "Control" }, { 0x36, "Control" }, { 0x3C, "Shift" },
		{ 0x3D, "Option" }, { 0x3E, "Control" }, { 0x4C, "Enter" }, { VK_LEFT, "Left Arrow" },
		{ VK_RIGHT, "Right Arrow" }, { VK_DOWN, "Down Arrow" }, { VK_UP, "Up Arrow" }, { 0x7A, "F1" },
		{ 0x78, "F2" }, { 0x63, "F3" }, { 0x76, "F4" }, { 0x60, "F5" }, { 0x61, "F6" }, { 0x62, "F7" },
		{ 0x64, "F8" }, { 0x65, "F9" }, { 0x6D, "F10" }, { 0x67, "F11" }, { 0x6F, "F12" }, { 0x69, "F13" },
		{ 0x6B, "F14" }, { 0x71, "F15" },
	};
	for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
		if (names[k].vk == vk)
			return names[k].name;
	if (chr > ' ' && chr < 0x7F)
		snprintf(buf, size, "%c", chr >= 'a' && chr <= 'z' ? chr - 'a' + 'A' : chr);
	else
		snprintf(buf, size, "key %02X", vk);
	return buf;
}

void pad_switch_text(const struct pad_switch *s, char *out, size_t size)
{
	char buf[16];
	switch (s->function) {
	case PAD_DO_MOUSE:
		snprintf(out, size, "%s", s->modifiers & PAD_MOD_CONTROL ? "Control-click" : "the mouse button");
		return;
	case PAD_DO_DOUBLE_CLICK:
		snprintf(out, size, "a double-click");
		return;
	case PAD_DO_MOUSE_LOCK:
		snprintf(out, size, "the mouse button, locked");
		return;
	case PAD_DO_MODIFIER:
		snprintf(out, size, "%s", key_name(s->key, 0, buf, sizeof buf));
		return;
	case PAD_DO_KEY:
		snprintf(out, size, "%s%s%s%s%s", s->modifiers & PAD_MOD_COMMAND ? "Command-" : "",
			 s->modifiers & PAD_MOD_CONTROL ? "Control-" : "", s->modifiers & PAD_MOD_OPTION ? "Option-" : "",
			 s->modifiers & PAD_MOD_SHIFT ? "Shift-" : "", key_name(s->key, s->chr, buf, sizeof buf));
		return;
	default:
		snprintf(out, size, "nothing");
		return;
	}
}

void pad_init(struct pad *p, const struct pad_output *out)
{
	memset(p, 0, sizeof *p);
	p->out = *out;
	p->mapping = PAD_MAP_DEFAULT;
}

/* ---- keys and the mouse button, held by how many controls ---------------------------------- */

static void key(struct pad *p, uint8_t adb, bool down)
{
	if (adb >= 128)
		return;
	if (down ? p->keys[adb]++ == 0 : p->keys[adb] && --p->keys[adb] == 0)
		p->out.key(p->out.ctx, adb, down);
}

static void mouse(struct pad *p, bool down)
{
	if (down ? p->mouse++ == 0 : p->mouse && --p->mouse == 0)
		p->out.button(p->out.ctx, down);
}

static const struct { uint8_t bit, key; } modifier_keys[] = {
	{ PAD_MOD_COMMAND, 0x37 }, { PAD_MOD_SHIFT, 0x38 }, { PAD_MOD_OPTION, 0x3A }, { PAD_MOD_CONTROL, 0x36 },
};
#define MODIFIER_KEYS (sizeof modifier_keys / sizeof modifier_keys[0])

static void modifiers(struct pad *p, uint8_t mods, bool down)
{
	for (size_t n = 0; n < MODIFIER_KEYS; n++) {
		size_t k = down ? n : MODIFIER_KEYS - 1 - n;
		if (mods & modifier_keys[k].bit)
			key(p, modifier_keys[k].key, down);
	}
}

static void act(struct pad *p, const struct pad_switch *a, bool down)
{
	switch (a->function) {
	case PAD_DO_MOUSE:
		if (down)
			modifiers(p, a->modifiers, true);
		mouse(p, down);
		if (!down)
			modifiers(p, a->modifiers, false);
		break;
	case PAD_DO_DOUBLE_CLICK:
		if (down && !p->mouse) {
			for (int k = 0; k < 4; k++)
				p->out.button(p->out.ctx, k % 2 == 0);
		}
		break;
	case PAD_DO_MOUSE_LOCK:
		if (down) {
			p->locked = !p->locked;
			mouse(p, p->locked);
		}
		break;
	case PAD_DO_KEY:
		if (down)
			modifiers(p, a->modifiers, true);
		key(p, pad_adb_key(a->key), down);
		if (!down)
			modifiers(p, a->modifiers, false);
		break;
	case PAD_DO_MODIFIER:
		key(p, pad_adb_key(a->key), down);
		break;
	default:
		break;
	}
}

/* What a control does as it goes down, by the mapping now (a stick that's the mouse: nothing) */
static struct pad_switch action(const struct pad *p, unsigned slot, unsigned c)
{
	const struct pad_slot *s = &p->slot[slot];
	struct pad_switch a = { PAD_DO_NOTHING, 0, 0, 0 };
	if (p->quiet || p->mapping == PAD_MAP_OFF || (s->taken >> c & 1))
		return a;
	if (p->mapping == PAD_MAP_DEFAULT) {
		if ((c >= PAD_STICK_UP && c <= PAD_STICK_RIGHT && s->map.stick[0] == PAD_STICK_MOUSE) ||
		    (c >= PAD_RSTICK_UP && c <= PAD_RSTICK_RIGHT && s->map.stick[1] == PAD_STICK_MOUSE))
			return a;
		return s->map.control[c];
	}
	int sw = pad_applejack_switch(c);
	if (sw < 0)
		return a;
	a = p->pippin[pad_applejack_of(p, slot)][sw];
	a.modifiers &= PAD_MOD_COMMAND | PAD_MOD_SHIFT | PAD_MOD_OPTION | PAD_MOD_CONTROL;
	return a;
}

static void control(struct pad *p, unsigned slot, unsigned c, bool down)
{
	struct pad_slot *s = &p->slot[slot];
	uint32_t bit = 1u << c;
	if (down == ((s->down & bit) != 0))
		return;
	if (down) {
		s->down |= bit;
		s->held[c] = action(p, slot, c);
		act(p, &s->held[c], true);
	} else {
		s->down &= ~bit;
		act(p, &s->held[c], false);
		s->held[c] = (struct pad_switch){ PAD_DO_NOTHING, 0, 0, 0 };
	}
}

/* ---- pads ------------------------------------------------------------------------------- */

int pad_plug(struct pad *p, int slot, const char *name)
{
	if (slot < 0) {
		for (slot = 0; slot < PAD_MAX && p->slot[slot].connected; slot++)
			;
	}
	if (slot >= PAD_MAX)
		return -1;
	if (p->slot[slot].connected)
		pad_unplug(p, (unsigned)slot);
	struct pad_slot *s = &p->slot[slot];
	memset(s, 0, sizeof *s);
	s->connected = true;
	snprintf(s->name, sizeof s->name, "%s", name && name[0] ? name : "Game controller");
	s->map = pad_default_map;
	return slot;
}

void pad_set_map(struct pad *p, unsigned slot, const struct pad_map *map)
{
	if (slot < PAD_MAX && p->slot[slot].connected)
		p->slot[slot].map = map ? *map : pad_default_map;
}

void pad_unplug(struct pad *p, unsigned slot)
{
	if (slot >= PAD_MAX || !p->slot[slot].connected)
		return;
	for (unsigned c = 0; c < PAD_CONTROLS; c++)
		control(p, slot, c, false);
	memset(&p->slot[slot], 0, sizeof p->slot[slot]);
}

unsigned pad_count(const struct pad *p)
{
	unsigned n = 0;
	for (unsigned k = 0; k < PAD_MAX; k++)
		n += p->slot[k].connected;
	return n;
}

void pad_button(struct pad *p, unsigned slot, unsigned button, bool down)
{
	if (slot < PAD_MAX && p->slot[slot].connected && button < PAD_BUTTONS)
		control(p, slot, button, down);
}

/* A stick's direction or a trigger, `v` along it (with some slack between pressing and letting go) */
#define PRESS_AT 16000
#define RELEASE_AT 12000

static void threshold(struct pad *p, unsigned slot, unsigned c, int v)
{
	bool down = p->slot[slot].down >> c & 1;
	if (!down && v >= PRESS_AT)
		control(p, slot, c, true);
	else if (down && v < RELEASE_AT)
		control(p, slot, c, false);
}

void pad_axis(struct pad *p, unsigned slot, unsigned axis, int value)
{
	if (slot >= PAD_MAX || !p->slot[slot].connected || axis >= PAD_AXES)
		return;
	value = value < -32768 ? -32768 : value > 32767 ? 32767 : value;
	p->slot[slot].axis[axis] = (int16_t)value;
	/* (the way it's left let go of before the way it's gone pressed) */
	static const unsigned ways[4] = { PAD_STICK_LEFT, PAD_STICK_UP, PAD_RSTICK_LEFT, PAD_RSTICK_UP };
	unsigned less = axis < 4 ? ways[axis] : 0, more = less + 1;     /* (right, down) */
	switch (axis) {
	case PAD_LEFT_X:
	case PAD_LEFT_Y:
	case PAD_RIGHT_X:
	case PAD_RIGHT_Y:
		if (value < 0) {
			threshold(p, slot, more, value);
			threshold(p, slot, less, -value);
		} else {
			threshold(p, slot, less, -value);
			threshold(p, slot, more, value);
		}
		break;
	case PAD_LEFT_TRIGGER:
		threshold(p, slot, PAD_LT, value);
		break;
	case PAD_RIGHT_TRIGGER:
		threshold(p, slot, PAD_RT, value);
		break;
	default:
		break;
	}
}

/*
 * The right stick: the mouse's speed grows as the square of how far it's
 * pushed past a dead zone, to PAD_MOUSE_SPEED (in whole numbers, so that
 * instruction-time runs come out the same on any host)
 */
#define DEAD_ZONE 5000
#define LIVE (32767 - DEAD_ZONE)

static int64_t speed(int v)
{
	int64_t n = (v < 0 ? -(int64_t)v : v) - DEAD_ZONE;
	if (n <= 0)
		return 0;
	n = n > LIVE ? LIVE : n;
	return (v < 0 ? -1 : 1) * PAD_MOUSE_SPEED * n * n;
}

int32_t pad_stick_speed(int value)
{
	return (int32_t)(speed(value) * 65536 / ((int64_t)LIVE * LIVE));
}

void pad_tick(struct pad *p, uint64_t now, uint64_t hz)
{
	uint64_t last = p->tick;
	p->tick = now ? now : 1;
	if (!last || now <= last || p->mapping == PAD_MAP_OFF || p->quiet) {
		p->dx = p->dy = 0;
		return;
	}
	uint64_t t = now - last < hz / 10 ? now - last : hz / 10;      /* (a long gap isn't a long movement) */
	int64_t vx = 0, vy = 0;
	for (unsigned k = 0; k < PAD_MAX; k++) {
		const struct pad_slot *s = &p->slot[k];
		for (unsigned stick = 0; stick < 2 && s->connected; stick++) {
			/* (a Pippin title's trackball is the right stick) */
			bool mouse = p->mapping == PAD_MAP_PIPPIN ? stick == 1 : s->map.stick[stick] == PAD_STICK_MOUSE;
			if (!mouse || (s->taken & (stick ? PAD_RIGHT_STICK_DIRECTIONS : PAD_LEFT_STICK_DIRECTIONS)))
				continue;
			vx += speed(s->axis[stick ? PAD_RIGHT_X : PAD_LEFT_X]);
			vy += speed(s->axis[stick ? PAD_RIGHT_Y : PAD_LEFT_Y]);
		}
	}
	if (!vx && !vy) {
		p->dx = p->dy = 0;
		return;
	}
	const int64_t unit = (int64_t)hz * LIVE * LIVE;
	p->dx += vx * (int64_t)t;
	p->dy += vy * (int64_t)t;
	int64_t mx = p->dx / unit, my = p->dy / unit;
	p->dx -= mx * unit;
	p->dy -= my * unit;
	if (mx || my)
		p->out.motion(p->out.ctx, (int)mx, (int)my);
}

unsigned pad_applejack_of(const struct pad *p, unsigned slot)
{
	if (p->applejacks < 2 || slot >= PAD_MAX)
		return 0;
	unsigned before = 0;
	for (unsigned k = 0; k < slot; k++)
		before += p->slot[k].connected;
	return before == 1;
}

uint32_t pad_applejack(const struct pad *p, unsigned applejack)
{
	uint32_t down = 0;
	for (unsigned k = 0; k < PAD_MAX; k++) {
		if (!p->slot[k].connected || pad_applejack_of(p, k) != applejack)
			continue;
		for (unsigned c = 0; c < PAD_CONTROLS; c++) {
			int sw = pad_applejack_switch(c);
			if (sw >= 0 && (p->slot[k].down >> c & 1))
				down |= 1u << sw;
		}
	}
	return down;
}

void pad_forget(struct pad *p)
{
	memset(p->keys, 0, sizeof p->keys);
	p->mouse = 0;
	p->locked = false;
	for (unsigned k = 0; k < PAD_MAX; k++)
		for (unsigned c = 0; c < PAD_CONTROLS; c++)
			p->slot[k].held[c] = (struct pad_switch){ PAD_DO_NOTHING, 0, 0, 0 };
}
