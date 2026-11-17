/*
 * An OpenHCI (OHCI 1.0a) USB host controller, with a root hub of two ports.
 * Every New World Mac has USB, and Mac OS 9 waits at startup until its root
 * hubs have been found (or 10 seconds have passed); the ROM's own driver
 * ("OHCIUIM", for PCI class 0c0310) finds this one.
 *
 * Registers are little-endian, as on PCI, and so are the descriptors in
 * guest memory. Frames are counted from the guest clock in 12 MHz bit times
 * while the controller is operational: each one advances the frame number,
 * writes it to the HCCA and raises start-of-frame. Once a device has been
 * connected (usb.h), each frame that ends services the lists, as the
 * hardware would in it: the periodic list (the HCCA's interrupt table for
 * the frame's number: interrupt EDs, then isochronous ones), then the
 * control and bulk lists while ControlListFilled and BulkListFilled say they
 * have TDs. Then the done queue is written back when its delay interrupt
 * count runs out, and port resets (10 ms) finish. A TD's transfer is moved
 * whole (all its packets) when its device answers: at once, or (bulk and
 * interrupt endpoints of devices passed through) at a later frame, the TD
 * staying at its ED's head meanwhile. Until a device is connected, the
 * lists are found empty without being walked.
 */
#ifndef BLUE_OHCI_H
#define BLUE_OHCI_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"
#include "mmio.h"
#include "usb.h"

#define OHCI_SIZE 0x1000u
#define OHCI_PORTS 2
#define OHCI_BIT_HZ 12000000u           /* full-speed bit times per second */
#define OHCI_ASYNC 32                   /* transfers in flight at once */

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
	unsigned long frames;           /* frames counted */
	struct usb_device *dev[OHCI_PORTS];     /* what's plugged into each port, or NULL */
	unsigned reset_frames[OHCI_PORTS];      /* frames until a port's reset finishes */
	uint8_t done_count;             /* frames until the done queue is written back (7: none due) */
	bool ever;                      /* a device has been connected: frames service the lists */
	bool busy;                      /* the last frame had TDs to service (not just ones in flight) */
	bool dead;                      /* an unrecoverable error: no more lists until a reset */
	struct ohci_work *work;         /* transfers in flight, and buffers (from the first connection) */
	unsigned long tds;              /* TDs retired (statistics) */
	int log;                        /* 1: print transfers; 2: register accesses too */
};

void ohci_init(struct ohci *o, struct mem *mem);
void ohci_free(struct ohci *o);        /* (disconnecting and destroying the devices) */

/* Plugs a device into port k (0 or 1); false if one's there. */
bool ohci_connect(struct ohci *o, unsigned k, struct usb_device *d);

/* Unplugs port k's device, its transfers given up; it's returned (NULL: none) for the caller to destroy. */
struct usb_device *ohci_disconnect(struct ohci *o, unsigned k);

struct usb_device *ohci_device(const struct ohci *o, unsigned k);

/* Whether a device has transfers to answer later (whose answers frames look for) */
bool ohci_waiting(const struct ohci *o);
extern const struct mmio_ops ohci_mmio_ops;
bool ohci_pure_read(const void *ctx, uint32_t offset);      /* (see struct mmio_region) */

/* Advances time (in bit times); may count frames. */
void ohci_set_time(struct ohci *o, uint64_t bit_times);

/* The bit time at which the next enabled interrupt is due; UINT64_MAX if none is. */
uint64_t ohci_next_event(const struct ohci *o);

/*
 * The bit time of the next frame whose start interrupts: with frame number
 * overflow enabled but not start of frame, the frame that overflows
 * (ohci_next_event()'s frames in between only update the frame number);
 * the next frame while the lists have work that isn't in flight.
 */
uint64_t ohci_next_interrupt(const struct ohci *o);

/* Whether the interrupt line is asserted. */
static inline bool ohci_irq(const struct ohci *o)
{
	return (o->int_enable & OHCI_INT_MIE) && (o->int_status & o->int_enable & ~OHCI_INT_MIE);
}

#endif
