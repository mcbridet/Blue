/*
 * Cuda: the microcontroller behind VIA1 on Heathrow/Paddington Macs. It
 * carries ADB, the real-time clock, PRAM and power control over a byte
 * protocol on the VIA shift register and port B (TIP/TACK/TREQ).
 */
#ifndef SYSHM_CUDA_H
#define SYSHM_CUDA_H

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
};

/* Connects the Cuda to `via` (installs the VIA hooks). */
void cuda_init(struct cuda *c, struct via *via);

/* Autopoll: sends pending ADB device data to the host when the bus is idle. */
void cuda_poll(struct cuda *c, uint64_t now_ms);

#endif
