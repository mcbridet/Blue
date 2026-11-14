/*
 * An OpenHCI (OHCI 1.0a) USB host controller with nothing plugged in. Every
 * New World Mac has USB, and Mac OS 9 waits at startup until its root hubs
 * have been found (or 10 seconds have passed); the ROM's own driver
 * ("OHCIUIM", for PCI class 0c0310) finds this one, whose two ports stay
 * empty.
 *
 * Registers are little-endian, as on PCI. Frames are counted from the guest
 * clock in 12 MHz bit times while the controller is operational: each one
 * advances the frame number, writes it to the HCCA in guest memory and raises
 * start-of-frame. Transfers on the lists would be for devices that aren't
 * there, so the lists aren't walked.
 */
#ifndef SYSHM_OHCI_H
#define SYSHM_OHCI_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"
#include "mmio.h"

#define OHCI_SIZE 0x1000u
#define OHCI_PORTS 2
#define OHCI_BIT_HZ 12000000u           /* full-speed bit times per second */

/* PCI identity: the OPTi 82C861 the first iMacs used for USB */
#define OHCI_PCI_VENDOR 0x1045u
#define OHCI_PCI_DEVICE 0xC861u
#define OHCI_PCI_REVISION 0x10u
#define OHCI_PCI_CLASS 0x0C0310u

/* HcInterruptStatus / HcInterruptEnable bits */
#define OHCI_INT_SO 0x00000001u         /* scheduling overrun */
#define OHCI_INT_WDH 0x00000002u        /* writeback done head */
#define OHCI_INT_SF 0x00000004u         /* start of frame */
#define OHCI_INT_RD 0x00000008u         /* resume detected */
#define OHCI_INT_UE 0x00000010u         /* unrecoverable error */
#define OHCI_INT_FNO 0x00000020u        /* frame number overflow */
#define OHCI_INT_RHSC 0x00000040u       /* root hub status change */
#define OHCI_INT_OC 0x40000000u         /* ownership change */
#define OHCI_INT_MIE 0x80000000u        /* master interrupt enable */

struct ohci {
	struct mem *mem;                /* for the HCCA */
	uint32_t control, command_status, int_status, int_enable, hcca;
	uint32_t period_current, control_head, control_current, bulk_head, bulk_current, done_head;
	uint32_t fm_interval, periodic_start, ls_threshold;
	uint16_t fm_number;
	bool frt;                       /* frame remaining toggle */
	uint32_t rh_a, rh_b, rh_status, port[OHCI_PORTS];
	uint64_t now;                   /* current time in bit times */
	uint64_t frame_start;           /* when the current frame began (while operational) */
	unsigned long frames;           /* frames counted (statistics) */
	bool log;                       /* print register accesses */
};

void ohci_init(struct ohci *o, struct mem *mem);
extern const struct mmio_ops ohci_mmio_ops;

/* Advances time (in bit times); may count frames. */
void ohci_set_time(struct ohci *o, uint64_t bit_times);

/* The bit time at which the next enabled interrupt is due; UINT64_MAX if none is. */
uint64_t ohci_next_event(const struct ohci *o);

/*
 * The bit time of the next frame whose start interrupts: with frame number
 * overflow enabled but not start of frame, the frame that overflows
 * (ohci_next_event()'s frames in between only update the frame number).
 */
uint64_t ohci_next_interrupt(const struct ohci *o);

/* Whether the interrupt line is asserted. */
static inline bool ohci_irq(const struct ohci *o)
{
	return (o->int_enable & OHCI_INT_MIE) && (o->int_status & o->int_enable & ~OHCI_INT_MIE);
}

#endif
