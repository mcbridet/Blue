/*
 * Apple Desktop Bus with a keyboard (address 2) and a mouse (address 3), as
 * the Cuda sees them. Input comes from the host; the Cuda turns pending data
 * into autopoll packets.
 */
#ifndef BLUE_ADB_H
#define BLUE_ADB_H

#include <stdbool.h>
#include <stdint.h>

#define ADB_KEY_QUEUE 64

struct adb_device {
	uint8_t addr, handler;
	uint8_t default_addr, default_handler;
	const uint8_t *handlers;        /* handler IDs the device accepts, 0-terminated */
};

struct adb_bus {
	struct adb_device kbd, mouse;
	uint8_t keys[ADB_KEY_QUEUE];    /* ADB key codes, bit 7 set for key up */
	unsigned key_head, key_count;
	int mouse_dx, mouse_dy;
	bool buttons[8];                /* button state changes not yet reported */
	unsigned button_count;
	bool button;                    /* last reported state */
	unsigned long commands;
};

void adb_init(struct adb_bus *b);

/*
 * Executes the ADB command byte `cmd` (with Listen data); returns the number
 * of reply bytes in `reply`, or -1 if no device answered (timeout).
 */
int adb_command(struct adb_bus *b, uint8_t cmd, const uint8_t *data, unsigned len, uint8_t reply[8]);

/* Returns the address of a device in `mask` (bit n = address n) with data to send, or -1. */
int adb_pending(const struct adb_bus *b, uint16_t mask);

void adb_key(struct adb_bus *b, uint8_t code, bool down);
void adb_power_key(struct adb_bus *b, bool down);       /* the keyboard's power key (Mac OS asks to shut down) */
void adb_mouse_move(struct adb_bus *b, int dx, int dy);
void adb_mouse_button(struct adb_bus *b, bool down);

#endif
