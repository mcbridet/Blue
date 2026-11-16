/*
 * Cuda: the microcontroller behind VIA1 on Heathrow/Paddington Macs. It
 * carries ADB, the real-time clock, PRAM and power control over a byte
 * protocol on the VIA shift register and port B (TIP/TACK/TREQ).
 */
#ifndef BLUE_CUDA_H
#define BLUE_CUDA_H

#include <stdbool.h>
#include <stdint.h>

#include "adb.h"
#include "clock.h"
#include "via.h"

/* VIA port B lines */
#define CUDA_TREQ 0x08          /* in: Cuda requests a transfer (active low) */
#define CUDA_TACK 0x10          /* out: byte acknowledge (toggles) */
#define CUDA_TIP 0x20           /* out: transfer in progress (active low) */

struct cuda {
	struct via *via;
	uint8_t last_b;
	uint8_t out[32];                /* packet from the host */
	unsigned out_len;
	uint8_t in[64];                 /* packet to the host */
	unsigned in_len, in_pos;
	bool autopoll;
	uint16_t autopoll_devices;      /* bit n: poll ADB address n */
	unsigned autopoll_ms;
	uint64_t last_poll_ms;
	bool unsolicited;               /* the pending packet is autopoll data */
	uint8_t deferred[16];           /* autopoll packet pushed back by a host transfer */
	unsigned deferred_len;
	struct adb_bus adb;
	int64_t time_offset;            /* seconds added to the clock (SET_TIME) */
	const struct guest_clock *clock;
	uint8_t pram[256];
	bool power_off, reset;
	unsigned long packets;
	uint8_t unknown_logged[32];     /* bitmap of unknown commands already reported */
	/* Mac OS dozing, and mouse input waking it (see cuda_poll()) */
	bool dozed;
	uint64_t doze_ms;               /* when it last dozed */
	uint64_t wake_ms;               /* when a key tap last woke it */
	bool mouse_moved;               /* the cursor was put somewhere since the last poll (not through ADB) */
	unsigned long wake_taps;
};

/* Connects the Cuda to `via` (installs the VIA hooks). */
void cuda_init(struct cuda *c, struct via *via);

/*
 * Autopoll: sends pending ADB device data to the host when the bus is idle.
 * While Mac OS dozes, mouse input (ADB's, or cuda_mouse_moved()) goes after a
 * tap of the keyboard's Option key, which wakes it (see cuda.c).
 */
void cuda_poll(struct cuda *c, uint64_t now_ms);

/* Mac OS's power manager dozes at guest time `now_ms` (it naps through the kernel's PowerDispatch) */
void cuda_doze(struct cuda *c, uint64_t now_ms);

/* The cursor was put where the host pointer is (in low memory, not through ADB) */
void cuda_mouse_moved(struct cuda *c);

#endif
