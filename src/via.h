/*
 * MOS 6522 VIA as found in Mac I/O (registers every 0x200 bytes). Timers run
 * from a clock supplied by the caller in VIA ticks (783.36 kHz on Macs).
 */
#ifndef BLUE_VIA_H
#define BLUE_VIA_H

#include <stdbool.h>
#include <stdint.h>

#include "mmio.h"

#define VIA_HZ 783360u
#define VIA_SIZE 0x2000u

/* Interrupt flag/enable bits */
#define VIA_IRQ_CA2 0x01
#define VIA_IRQ_CA1 0x02
#define VIA_IRQ_SR 0x04
#define VIA_IRQ_CB2 0x08
#define VIA_IRQ_CB1 0x10
#define VIA_IRQ_T2 0x20
#define VIA_IRQ_T1 0x40

#define VIA_ACR_SR_OUT 0x10     /* shift register shifting out (host -> device) */
#define VIA_ACR_T1_FREE 0x40    /* T1 free-running */

struct via;

struct via_hooks {
	void *ctx;
	void (*port_b_written)(void *ctx, struct via *via);     /* ORB or DDRB changed */
	void (*sr_accessed)(void *ctx, struct via *via, bool write);
};

struct via {
	uint8_t orb, ora, ddrb, ddra;
	uint8_t in_b, in_a;             /* input pin levels */
	uint8_t sr, acr, pcr, ifr, ier;
	uint16_t t1_latch, t2_latch;
	uint64_t t1_start, t2_start;    /* tick at which the counter was loaded */
	uint16_t t1_load, t2_load;      /* value loaded */
	bool t1_armed, t2_armed;        /* one-shot interrupt not yet delivered */
	uint64_t t1_seen;               /* T1 underflows already reported */
	uint64_t now;                   /* current time in VIA ticks */
	struct via_hooks hooks;
};

void via_init(struct via *v);
extern const struct mmio_ops via_mmio_ops;

/* Advances time; may set timer flags. */
void via_set_time(struct via *v, uint64_t ticks);

/* The tick at which the next enabled timer interrupt is due; UINT64_MAX if none is. */
uint64_t via_next_event(const struct via *v);

void via_raise(struct via *v, uint8_t bits);

static inline bool via_irq(const struct via *v)
{
	return (v->ifr & v->ier & 0x7F) != 0;
}

/* Port B as the outside world sees it: driven outputs, inputs elsewhere. */
uint8_t via_port_b(const struct via *v);

#endif
