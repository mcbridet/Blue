#include "pad.h"

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

static const struct pad_switch default_map[PAD_CONTROLS] = {
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

/* What a control does as it goes down, by the mapping now */
static struct pad_switch action(const struct pad *p, const struct pad_slot *s, unsigned c)
{
	struct pad_switch a = { PAD_DO_NOTHING, 0, 0, 0 };
	if (p->quiet || p->mapping == PAD_MAP_OFF || (s->taken >> c & 1))
		return a;
	if (p->mapping == PAD_MAP_DEFAULT)
		return default_map[c];
	int sw = pad_applejack_switch(c);
	if (sw < 0)
		return a;
	a = p->pippin[sw];
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
		s->held[c] = action(p, s, c);
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
	return slot;
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
	unsigned less = axis == PAD_LEFT_X ? PAD_STICK_LEFT : PAD_STICK_UP, more = less + 1;     /* (right, down) */
	switch (axis) {
	case PAD_LEFT_X:
	case PAD_LEFT_Y:
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
		if (!p->slot[k].connected || (p->slot[k].taken & PAD_TAKEN_RIGHT_STICK))
			continue;
		vx += speed(p->slot[k].axis[PAD_RIGHT_X]);
		vy += speed(p->slot[k].axis[PAD_RIGHT_Y]);
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

uint32_t pad_applejack(const struct pad *p)
{
	uint32_t down = 0;
	for (unsigned k = 0; k < PAD_MAX; k++) {
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
