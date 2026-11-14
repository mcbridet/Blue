#include "adb.h"

#include <string.h>

#define ADB_CMD_RESET 0x00
#define ADB_CMD_FLUSH 0x01
#define ADB_CMD_LISTEN 0x08
#define ADB_CMD_TALK 0x0C

/* Listen R3 special handler values */
#define HANDLER_SELF_TEST 0xFF
#define HANDLER_MOVE_IF_NO_COLLISION 0xFE
#define HANDLER_MOVE_IF_ACTIVATED 0xFD
#define HANDLER_MOVE_AND_FLAGS 0x00

static const uint8_t keyboard_handlers[] = { 1, 2, 3, 0 };      /* standard, extended, extended+right mods */
static const uint8_t mouse_handlers[] = { 1, 2, 0 };            /* 100 and 200 cpi */

static void reset_device(struct adb_device *d)
{
	d->addr = d->default_addr;
	d->handler = d->default_handler;
}

void adb_init(struct adb_bus *b)
{
	memset(b, 0, sizeof *b);
	b->kbd = (struct adb_device){ 2, 2, 2, 2, keyboard_handlers };
	b->mouse = (struct adb_device){ 3, 1, 3, 1, mouse_handlers };
}

static struct adb_device *device_at(struct adb_bus *b, unsigned addr)
{
	if (b->kbd.addr == addr)
		return &b->kbd;
	if (b->mouse.addr == addr)
		return &b->mouse;
	return NULL;
}

static bool accepts_handler(const struct adb_device *d, uint8_t handler)
{
	for (const uint8_t *h = d->handlers; *h; h++)
		if (*h == handler)
			return true;
	return false;
}

static int clamp7(int v)
{
	return v < -64 ? -64 : v > 63 ? 63 : v;
}

static int talk_register0(struct adb_bus *b, struct adb_device *d, uint8_t reply[8])
{
	if (d == &b->kbd) {
		if (!b->key_count)
			return -1;
		reply[0] = b->keys[b->key_head];
		b->key_head = (b->key_head + 1) % ADB_KEY_QUEUE;
		b->key_count--;
		reply[1] = 0xFF;
		if (b->key_count) {
			reply[1] = b->keys[b->key_head];
			b->key_head = (b->key_head + 1) % ADB_KEY_QUEUE;
			b->key_count--;
		}
		return 2;
	}
	if (!b->mouse_dx && !b->mouse_dy && !b->button_count)
		return -1;
	int dx = clamp7(b->mouse_dx), dy = clamp7(b->mouse_dy);
	b->mouse_dx -= dx;
	b->mouse_dy -= dy;
	if (b->button_count) {
		b->button = b->buttons[0];
		memmove(b->buttons, b->buttons + 1, --b->button_count * sizeof b->buttons[0]);
	}
	reply[0] = (uint8_t)((b->button ? 0x00 : 0x80) | (dy & 0x7F));
	reply[1] = (uint8_t)(0x80 | (dx & 0x7F));
	return 2;
}

int adb_command(struct adb_bus *b, uint8_t cmd, const uint8_t *data, unsigned len, uint8_t reply[8])
{
	b->commands++;
	if ((cmd & 0x0F) == ADB_CMD_RESET) {
		reset_device(&b->kbd);
		reset_device(&b->mouse);
		b->key_count = 0;
		b->mouse_dx = b->mouse_dy = 0;
		return 0;
	}

	struct adb_device *d = device_at(b, cmd >> 4);
	unsigned reg = cmd & 3;
	if (!d)
		return -1;

	switch (cmd & 0x0C) {
	case ADB_CMD_TALK:
		switch (reg) {
		case 0:
			return talk_register0(b, d, reply);
		case 2:
			if (d != &b->kbd)
				return -1;
			reply[0] = reply[1] = 0xFF;     /* no modifiers or LEDs (active low) */
			return 2;
		case 3:
			reply[0] = (uint8_t)(0x60 | d->addr);   /* SRQ enabled */
			reply[1] = d->handler;
			return 2;
		default:
			return -1;
		}
	case ADB_CMD_LISTEN:
		if (reg == 3 && len >= 2) {
			switch (data[1]) {
			case HANDLER_SELF_TEST:
			case HANDLER_MOVE_IF_ACTIVATED:
				break;
			case HANDLER_MOVE_IF_NO_COLLISION:
			case HANDLER_MOVE_AND_FLAGS:
				d->addr = data[0] & 0x0F;
				break;
			default:
				if (accepts_handler(d, data[1]))
					d->handler = data[1];
				break;
			}
		}
		return 0;
	default:        /* flush */
		if (d == &b->kbd)
			b->key_count = 0;
		return 0;
	}
}

int adb_pending(const struct adb_bus *b, uint16_t mask)
{
	if (b->key_count && (mask >> b->kbd.addr & 1))
		return b->kbd.addr;
	if ((b->mouse_dx || b->mouse_dy || b->button_count) && (mask >> b->mouse.addr & 1))
		return b->mouse.addr;
	return -1;
}

void adb_key(struct adb_bus *b, uint8_t code, bool down)
{
	if (b->key_count >= ADB_KEY_QUEUE)
		return;
	b->keys[(b->key_head + b->key_count) % ADB_KEY_QUEUE] = (uint8_t)((code & 0x7F) | (down ? 0 : 0x80));
	b->key_count++;
}

void adb_mouse_move(struct adb_bus *b, int dx, int dy)
{
	b->mouse_dx += dx;
	b->mouse_dy += dy;
}

void adb_mouse_button(struct adb_bus *b, bool down)
{
	bool last = b->button_count ? b->buttons[b->button_count - 1] : b->button;
	if (down != last && b->button_count < sizeof b->buttons / sizeof b->buttons[0])
		b->buttons[b->button_count++] = down;
}
