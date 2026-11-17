/*
 * USB transfers through the OHCI controller (its lists, TDs, the done
 * queue and the root hub's ports), with a device of the test's own; and
 * the USB test disk's bulk-only transport and SCSI commands.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/mem.h"
#include "../src/ohci.h"
#include "../src/usb.h"
#include "../src/usbmsd.h"
#include "test.h"

#define RAM 0x100000u
#define HCCA 0x2000u
#define FRAME 12000u

enum {
	HC_CONTROL = 0x04, HC_COMMAND_STATUS = 0x08, HC_INT_STATUS = 0x0C, HC_INT_ENABLE = 0x10, HC_HCCA = 0x18,
	HC_CONTROL_HEAD = 0x20, HC_BULK_HEAD = 0x28, HC_DONE_HEAD = 0x30, HC_RH_PORT1 = 0x54, HC_RH_PORT2 = 0x58,
};
#define CLF 2u
#define BLF 4u
#define OPERATIONAL 0x80u
#define LISTS 0x3Cu                     /* PLE IE CLE BLE */

/* descriptors' words */
#define ED(fa, ep, dir, mps) ((fa) | (ep) << 7 | (dir) << 11 | (mps) << 16)
#define ED_SKIP (1u << 14)
#define ED_ISO (1u << 15)
#define TD(dp, di, toggle) (0xF0000000u | (dp) << 19 | (di) << 21 | (toggle) << 24)
#define TD_R (1u << 18)
#define CC(flags) ((flags) >> 28)
#define EC(flags) (((flags) >> 26) & 3)
enum { DP_SETUP, DP_OUT, DP_IN };
enum { D_TD, D_OUT, D_IN };

static struct mem mem;
static struct ohci usb;
static uint64_t now;

static uint32_t rd(uint32_t reg)
{
	return __builtin_bswap32(ohci_mmio_ops.read(&usb, reg, 4));
}

static void wr(uint32_t reg, uint32_t v)
{
	ohci_mmio_ops.write(&usb, reg, 4, __builtin_bswap32(v));
}

static uint32_t get(uint32_t a)
{
	return le32(mem_ptr(&mem, a));
}

static void put(uint32_t a, uint32_t v)
{
	put_le32(mem_ptr(&mem, a), v);
}

static void ed(uint32_t at, uint32_t flags, uint32_t head, uint32_t tail, uint32_t next)
{
	put(at, flags);
	put(at + 4, tail);
	put(at + 8, head);
	put(at + 12, next);
}

static void td(uint32_t at, uint32_t flags, uint32_t cbp, uint32_t len, uint32_t next)
{
	put(at, flags);
	put(at + 4, len ? cbp : 0);
	put(at + 8, next);
	put(at + 12, len ? cbp + len - 1 : 0);
}

static void frames(unsigned n)
{
	now += (uint64_t)n * FRAME;
	ohci_set_time(&usb, now);
}

/* The done queue as written back to the HCCA (and WDH cleared) */
static uint32_t done_head(void)
{
	uint32_t head = get(HCCA + 0x84);
	wr(HC_INT_STATUS, OHCI_INT_WDH);
	return head;
}

/* The test's device: endpoint 0 answers GET_DESCRIPTOR; others as set up */
struct tdev {
	struct usb_device dev;
	unsigned naks;                  /* bulk and interrupt transfers NAKed first */
	bool stall, later, iso_short;
	struct usb_xfer *pending;
	uint32_t in_left;               /* bulk/interrupt in: bytes it has to send */
	uint8_t out[1024];
	uint32_t out_len;
	unsigned resets, cancels, transfers;
};

static int tdev_submit(struct usb_device *d, struct usb_xfer *x)
{
	struct tdev *t = (struct tdev *)d;
	x->actual = 0;
	if (x->type == USB_CONTROL) {
		if (x->setup[0] == 0x80 && x->setup[1] == USB_REQ_GET_DESCRIPTOR) {
			for (uint32_t k = 0; k < 18 && k < x->len; k++)
				x->data[k] = (uint8_t)(0x40 + k);
			x->actual = x->len < 18 ? x->len : 18;
			return USB_OK;
		}
		return x->setup[1] == 0x7F ? USB_STALL : USB_OK;
	}
	t->transfers++;
	if (x->type == USB_ISO) {
		if (x->ep & USB_DIR_IN) {
			x->actual = t->iso_short ? x->len / 2 : x->len;
			memset(x->data, 0x5A, x->actual);
		} else {
			memcpy(t->out, x->data, x->len);
			t->out_len = x->len;
			x->actual = x->len;
		}
		return USB_OK;
	}
	if (t->naks) {
		t->naks--;
		return USB_NAK;
	}
	if (t->stall)
		return USB_STALL;
	if (x->ep & USB_DIR_IN) {
		x->actual = t->in_left < x->len ? t->in_left : x->len;
		t->in_left -= x->actual;
		for (uint32_t k = 0; k < x->actual; k++)
			x->data[k] = (uint8_t)k;
	} else {
		memcpy(t->out, x->data, x->len < sizeof t->out ? x->len : sizeof t->out);
		t->out_len = x->len;
		x->actual = x->len;
	}
	if (t->later) {
		t->pending = x;
		return USB_PENDING;
	}
	return USB_OK;
}

static void tdev_cancel(struct usb_device *d, struct usb_xfer *x)
{
	struct tdev *t = (struct tdev *)d;
	if (t->pending == x)
		t->pending = NULL;
	t->cancels++;
}

static void tdev_reset(struct usb_device *d)
{
	((struct tdev *)d)->resets++;
}

static void tdev_destroy(struct usb_device *d)
{
	(void)d;
}

static const struct usb_device_ops tdev_ops = { tdev_submit, tdev_cancel, tdev_reset, tdev_destroy };
static struct tdev dev;

/* The controller running, the device connected to port 1, reset and enabled, at address 0 */
static void start(unsigned speed)
{
	ohci_free(&usb);
	ohci_init(&usb, &mem);
	memset(mem_ptr(&mem, 0), 0, RAM);
	memset(&dev, 0, sizeof dev);
	usb_device_init(&dev.dev, &tdev_ops, speed, "test");
	now = 0;
	wr(HC_HCCA, HCCA);
	wr(HC_CONTROL, OPERATIONAL | LISTS);
	CHECK(ohci_connect(&usb, 0, &dev.dev));
	CHECK(!ohci_connect(&usb, 0, &dev.dev));                /* (taken) */
	wr(HC_RH_PORT1, 0x10);                                  /* SetPortReset */
	frames(10);
	CHECK_EQ(rd(HC_RH_PORT1) & 0x1F001F, 0x110003);         /* PRSC CSC, PES CCS */
	wr(HC_RH_PORT1, 0x1F0000);
	wr(HC_INT_STATUS, 0xFFFFFFFF);
}

static void test_root_hub_ports(void)
{
	ohci_free(&usb);
	ohci_init(&usb, &mem);
	memset(&dev, 0, sizeof dev);
	usb_device_init(&dev.dev, &tdev_ops, USB_SPEED_LOW, "test");
	wr(HC_CONTROL, OPERATIONAL);
	wr(HC_INT_STATUS, 0xFFFFFFFF);
	CHECK(ohci_connect(&usb, 1, &dev.dev));
	CHECK_EQ(rd(HC_RH_PORT2), 0x10301);                     /* CSC; low speed, powered, connected */
	CHECK_EQ(rd(HC_RH_PORT1), 0x100);
	CHECK(rd(HC_INT_STATUS) & OHCI_INT_RHSC);
	CHECK_EQ(dev.resets, 1);

	wr(HC_RH_PORT2, 0x10000);
	wr(HC_RH_PORT2, 0x10);                                  /* a reset lasts 10 frames */
	CHECK_EQ(rd(HC_RH_PORT2) & 0x1F, 0x11);
	frames(9);
	CHECK_EQ(rd(HC_RH_PORT2) & 0x1F, 0x11);
	frames(1);
	CHECK_EQ(rd(HC_RH_PORT2) & 0x1F001F, 0x100003);         /* PRSC; enabled */
	CHECK_EQ(dev.resets, 2);

	wr(HC_RH_PORT2, 0x4);                                   /* SetPortSuspend, then ClearSuspendStatus */
	CHECK(rd(HC_RH_PORT2) & 0x4);
	wr(HC_RH_PORT2, 0x8);
	CHECK_EQ(rd(HC_RH_PORT2) & 0x4001F, 0x40003);           /* PSSC */
	wr(HC_RH_PORT2, 0x1);                                   /* ClearPortEnable */
	CHECK_EQ(rd(HC_RH_PORT2) & 0x3, 0x1);
	wr(HC_RH_PORT2, 0x2);
	wr(HC_RH_PORT2, 0x1F0000);
	wr(HC_INT_STATUS, OHCI_INT_RHSC);

	CHECK(ohci_disconnect(&usb, 1) == &dev.dev);
	CHECK_EQ(rd(HC_RH_PORT2), 0x30100);                     /* CSC PESC, powered */
	CHECK(rd(HC_INT_STATUS) & OHCI_INT_RHSC);
	CHECK(ohci_disconnect(&usb, 1) == NULL);

	/* a reset of the bus (USBRESET) resets the device, connected but not enabled */
	CHECK(ohci_connect(&usb, 0, &dev.dev));
	wr(HC_RH_PORT1, 0x12);
	frames(10);
	wr(HC_CONTROL, 0);
	CHECK_EQ(rd(HC_RH_PORT1) & 0xFFFF, 0x301);
	ohci_free(&usb);
}

/* A control request: SETUP, data in, status, with the done queue in reverse order */
static void test_control(void)
{
	start(USB_SPEED_FULL);
	static const uint8_t get_device[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };
	memcpy(mem_ptr(&mem, 0x8000), get_device, 8);
	ed(0x4000, ED(0, 0, D_TD, 8), 0x5000, 0x5100, 0);
	td(0x5000, TD(DP_SETUP, 7, 2), 0x8000, 8, 0x5040);
	td(0x5040, TD(DP_IN, 7, 3) | TD_R, 0x9000, 64, 0x5080);   /* (asks for more: rounding) */
	td(0x5080, TD(DP_OUT, 0, 3), 0, 0, 0x5100);
	wr(HC_CONTROL_HEAD, 0x4000);
	wr(HC_COMMAND_STATUS, CLF);
	frames(1);
	CHECK_EQ(get(0x4008), 0x5100);                          /* all retired, not halted */
	CHECK_EQ(CC(get(0x5000)), 0);
	CHECK_EQ(CC(get(0x5040)), 0);
	CHECK_EQ(get(0x5044), 0x9000 + 18);                     /* (short: where it stopped) */
	CHECK_EQ(mem_r8(&mem, 0x9000 + 17), 0x40 + 17);
	CHECK_EQ(get(0x5004), 0);                               /* (all moved) */
	CHECK_EQ(get(0x5040) >> 24 & 3, 2);                     /* 3 packets of 8: DATA1, 0, 1: next 0, T1 set */
	CHECK(rd(HC_INT_STATUS) & OHCI_INT_WDH);                /* (the status TD's delay: 0) */
	CHECK_EQ(done_head(), 0x5080);
	CHECK_EQ(get(0x5088), 0x5040);
	CHECK_EQ(get(0x5048), 0x5000);
	CHECK_EQ(rd(HC_COMMAND_STATUS) & CLF, CLF);             /* (TDs were found: looked at again) */
	frames(1);
	CHECK_EQ(rd(HC_COMMAND_STATUS) & CLF, 0);               /* (none: not again until it's set) */

	/* SET_ADDRESS takes effect after its status stage */
	static const uint8_t set_address[8] = { 0, 5, 9, 0, 0, 0, 0, 0 };
	memcpy(mem_ptr(&mem, 0x8000), set_address, 8);
	ed(0x4000, ED(0, 0, D_TD, 8), 0x5000, 0x5080, 0);
	td(0x5000, TD(DP_SETUP, 7, 2), 0x8000, 8, 0x5040);
	td(0x5040, TD(DP_IN, 1, 3), 0, 0, 0x5080);
	wr(HC_COMMAND_STATUS, CLF);
	frames(1);
	CHECK_EQ(dev.dev.addr, 9);
	CHECK(!(rd(HC_INT_STATUS) & OHCI_INT_WDH));             /* (DI 1: a frame later) */
	frames(1);
	CHECK(rd(HC_INT_STATUS) & OHCI_INT_WDH);
	CHECK_EQ(done_head() & ~1u, 0x5040);

	/* a request the device stalls: its status TD, and the ED halts */
	static const uint8_t odd[8] = { 0x40, 0x7F, 0, 0, 0, 0, 0, 0 };
	memcpy(mem_ptr(&mem, 0x8000), odd, 8);
	ed(0x4000, ED(9, 0, D_TD, 8), 0x5000, 0x5080, 0);
	td(0x5000, TD(DP_SETUP, 7, 2), 0x8000, 8, 0x5040);
	td(0x5040, TD(DP_IN, 7, 3), 0, 0, 0x5080);
	wr(HC_COMMAND_STATUS, CLF);
	frames(2);
	CHECK_EQ(CC(get(0x5040)), 4);                           /* Stall */
	CHECK_EQ(get(0x4008), 0x5080 | 3);                      /* halted; the carry the status TD's (DATA1) */
	CHECK_EQ(done_head(), 0x5040);                          /* (an error: at the frame's end) */
}

/* Bulk: toggles over packets, the carry, short packets, NAKs, stalls, answers that come later */
static void test_bulk(void)
{
	start(USB_SPEED_FULL);
	dev.dev.addr = 3;
	ed(0x4000, ED(3, 1, D_IN, 64), 0x5000, 0x5080, 0);
	td(0x5000, TD(DP_IN, 7, 0) | TD_R, 0x9000, 192, 0x5040);       /* 3 full packets: the toggle flips once */
	td(0x5040, TD(DP_IN, 0, 0), 0x9100, 64, 0x5080);               /* (8 more bytes: short, an error without R) */
	wr(HC_BULK_HEAD, 0x4000);
	dev.in_left = 192;
	wr(HC_COMMAND_STATUS, BLF);
	frames(1);
	CHECK_EQ(CC(get(0x5000)), 0);
	CHECK_EQ(get(0x5004), 0);
	CHECK_EQ(get(0x5000) >> 24 & 3, 3);                     /* T1, then DATA1 next */
	CHECK_EQ(CC(get(0x5040)), 9);                           /* the second: nothing left, DataUnderrun */
	CHECK_EQ(get(0x5044), 0x9100);
	CHECK_EQ(get(0x4008), 0x5080 | 1 | 0);                  /* halted; carry from the empty packet: DATA1 -> 0 */
	CHECK_EQ(mem_r8(&mem, 0x9000 + 191), 191);

	/* NAKs leave the TD at the head, retried every frame; the carry goes on from the ED */
	dev.in_left = 64;
	dev.naks = 2;
	ed(0x4000, ED(3, 1, D_IN, 64), 0x5000 | 2, 0x5040, 0);  /* carry: DATA1 */
	td(0x5000, TD(DP_IN, 7, 0), 0x9000, 64, 0x5040);
	wr(HC_COMMAND_STATUS, BLF);
	frames(1);
	CHECK_EQ(get(0x4008), 0x5000 | 2);
	CHECK_EQ(CC(get(0x5000)), 15);                          /* (not accessed yet) */
	frames(2);
	CHECK_EQ(get(0x4008), 0x5040);                          /* retired: carry DATA0 now */
	CHECK_EQ(dev.transfers, 2 + 3);

	/* a stall halts the ED: Stall */
	dev.stall = true;
	ed(0x4000, ED(3, 2, D_OUT, 64), 0x5000, 0x5040, 0);
	td(0x5000, TD(DP_OUT, 7, 0), 0x9000, 100, 0x5040);
	wr(HC_COMMAND_STATUS, BLF);
	frames(1);
	CHECK_EQ(CC(get(0x5000)), 4);
	CHECK_EQ(get(0x4008), 0x5040 | 1);
	dev.stall = false;

	/* out, two pages: from CBP's page and from BE's */
	for (unsigned k = 0; k < 100; k++)
		mem_w8(&mem, 0x9FCE + k, (uint8_t)k);
	ed(0x4000, ED(3, 2, D_OUT, 64), 0x5000, 0x5040, 0);
	put(0x5000, TD(DP_OUT, 7, 0));
	put(0x5004, 0x9FCE);
	put(0x5008, 0x5040);
	put(0x500C, 0xC000 + 100 - 0x32 - 1);                   /* (0x32 bytes in the first page, then 0xC000 on) */
	memcpy(mem_ptr(&mem, 0xC000), mem_ptr(&mem, 0xA000), 50);
	wr(HC_COMMAND_STATUS, BLF);
	frames(1);
	CHECK_EQ(dev.out_len, 100);
	CHECK_EQ(dev.out[0x31], 0x31);
	CHECK_EQ(dev.out[0x32], mem_r8(&mem, 0xC000));
	CHECK_EQ(get(0x5004), 0);

	/* an answer that comes later: the TD stays until it's done; skipping the ED gives it up */
	dev.later = true;
	dev.in_left = 100;
	ed(0x4000, ED(3, 1, D_IN, 64), 0x5000, 0x5040, 0);
	td(0x5000, TD(DP_IN, 7, 0) | TD_R, 0x9000, 64, 0x5040);
	wr(HC_COMMAND_STATUS, BLF);
	frames(1);
	CHECK(dev.pending != NULL);
	CHECK(ohci_waiting(&usb));
	frames(3);
	CHECK_EQ(get(0x4008), 0x5000);
	CHECK_EQ(dev.transfers, 5 + 3);                         /* (asked once) */
	dev.pending->actual = 10;
	dev.pending->status = USB_OK;
	atomic_store(&dev.pending->done, true);
	frames(1);
	CHECK_EQ(get(0x4008), 0x5040 | 2);
	CHECK_EQ(get(0x5004), 0x9000 + 10);
	CHECK(!ohci_waiting(&usb));

	td(0x5040, TD(DP_IN, 7, 0) | TD_R, 0x9000, 64, 0x5080);
	put(0x4004, 0x5080);
	wr(HC_COMMAND_STATUS, BLF);
	frames(1);
	CHECK(dev.pending != NULL);
	put(0x4000, ED(3, 1, D_IN, 64) | ED_SKIP);
	frames(1);
	CHECK_EQ(dev.cancels, 1);
	CHECK(!ohci_waiting(&usb));
	dev.later = false;
}

/* Interrupt EDs from the HCCA's table, one TD a frame; the done queue's delay */
static void test_interrupt(void)
{
	start(USB_SPEED_LOW);
	dev.dev.addr = 2;
	dev.in_left = 4;
	ed(0x4000, ED(2, 1, D_IN, 8), 0x5000, 0x5080, 0);
	td(0x5000, TD(DP_IN, 2, 0) | TD_R, 0x9000, 8, 0x5040);
	td(0x5040, TD(DP_IN, 2, 0) | TD_R, 0x9010, 8, 0x5080);
	for (unsigned k = 0; k < 32; k += 8)                    /* every 8 frames, from frame 3 */
		put(HCCA + 4 * ((k + 3) & 31), 0x4000);
	uint16_t f = (uint16_t)rd(0x3C);
	unsigned wait = (3 - f) & 7;
	frames(wait);
	CHECK_EQ(get(0x4008), 0x5000);
	frames(1);
	CHECK_EQ(get(0x4008), 0x5040 | 2);                      /* one TD in the frame (a packet: DATA1 next) */
	CHECK_EQ(get(0x5004), 0x9004);
	CHECK(!(rd(HC_INT_STATUS) & OHCI_INT_WDH));
	frames(2);                                              /* (DI 2) */
	CHECK(rd(HC_INT_STATUS) & OHCI_INT_WDH);
	CHECK_EQ(done_head() & ~1u, 0x5000);
	dev.in_left = 4;
	frames(5);
	CHECK_EQ(get(0x4008), 0x5040 | 2);
	frames(1);
	CHECK_EQ(get(0x4008), 0x5080);
}

/* Isochronous TDs: a packet a frame, from their start frame; late ones retire as DataOverrun */
static void test_iso(void)
{
	start(USB_SPEED_FULL);
	dev.dev.addr = 4;
	dev.iso_short = true;
	uint16_t f = (uint16_t)rd(0x3C);
	ed(0x4000, ED(4, 3, D_IN, 200) | ED_ISO, 0x6000, 0x6040, 0);
	for (unsigned k = 0; k < 32; k++)
		put(HCCA + 4 * k, 0x4000);
	/* frames f+1 to f+3: packets of 100, 200 and 50 bytes, the second over a page boundary */
	put(0x6000, 0xF0000000u | 2u << 24 | 7u << 21 | (uint16_t)(f + 1));
	put(0x6004, 0x9F00);
	put(0x6008, 0x6040);
	put(0x600C, 0xA000 + 0x2C + 50 - 1);
	put(0x6010, (0xE000u | 0xF00) | (0xE000u | 0xF64) << 16);
	put(0x6014, (0xE000u | 0x102C) | 0xE000u << 16);
	frames(1);                                              /* (frame f: not its time) */
	CHECK_EQ(get(0x4008), 0x6000);
	frames(1);
	CHECK_EQ(get(0x6010) & 0xFFFF, 0x9000 | 50);            /* DataUnderrun (short), 50 bytes */
	CHECK_EQ(mem_r8(&mem, 0x9F00), 0x5A);
	frames(1);
	CHECK_EQ(get(0x6010) >> 16, 0x9000 | 100);
	CHECK_EQ(get(0x4008), 0x6000);
	frames(1);
	CHECK_EQ(get(0x6014) & 0xFFFF, 0x9000 | 25);
	CHECK_EQ(get(0x4008), 0x6040);                          /* done after its last frame */
	CHECK_EQ(CC(get(0x6000)), 0);

	/* out, late: */
	ed(0x4000, ED(4, 3, D_OUT, 200) | ED_ISO, 0x6000, 0x6040, 0);
	put(0x6000, 0xF0000000u | 0u << 24 | 0u << 21 | (uint16_t)(rd(0x3C) - 5));
	put(0x6008, 0x6040);                                    /* (the done queue had it) */
	frames(1);
	CHECK_EQ(CC(get(0x6000)), 8);                           /* DataOverrun */
	CHECK_EQ(get(0x4008), 0x6040);
	put(0x6000, 0xF0000000u | 0u << 24 | 0u << 21 | (uint16_t)rd(0x3C));
	put(0x6004, 0x9000);
	put(0x600C, 0x9000 + 30 - 1);
	put(0x6010, 0xE000u);
	ed(0x4000, ED(4, 3, D_OUT, 200) | ED_ISO, 0x6000, 0x6040, 0);
	frames(1);
	CHECK_EQ(dev.out_len, 30);
	CHECK_EQ(get(0x6010) & 0xFFFF, 0);                      /* NoError, size 0 for out */
}

/* No device at the address: three tries, a frame each, then DeviceNotResponding */
static void test_no_answer(void)
{
	start(USB_SPEED_FULL);
	ed(0x4000, ED(7, 0, D_TD, 8), 0x5000, 0x5040, 0);
	td(0x5000, TD(DP_SETUP, 0, 2), 0x8000, 8, 0x5040);
	wr(HC_CONTROL_HEAD, 0x4000);
	wr(HC_COMMAND_STATUS, CLF);
	frames(1);
	CHECK_EQ(EC(get(0x5000)), 1);
	CHECK_EQ(CC(get(0x5000)), 5);
	CHECK_EQ(get(0x4008), 0x5000);
	frames(2);
	CHECK_EQ(EC(get(0x5000)), 3);
	CHECK_EQ(get(0x4008), 0x5040 | 1);

	/* unplugged: transfers in flight are given up */
	dev.later = true;
	dev.in_left = 64;
	dev.dev.addr = 1;
	ed(0x4000, ED(1, 1, D_IN, 64), 0x5000, 0x5040, 0);
	td(0x5000, TD(DP_IN, 7, 0), 0x9000, 64, 0x5040);
	wr(HC_BULK_HEAD, 0x4000);
	wr(HC_COMMAND_STATUS, BLF);
	frames(1);
	CHECK(ohci_waiting(&usb));
	ohci_disconnect(&usb, 0);
	CHECK_EQ(dev.cancels, 1);
	CHECK(!ohci_waiting(&usb));
	frames(3);
	CHECK_EQ(CC(get(0x5000)), 5);
	ohci_free(&usb);
}

/* The test disk, through its endpoints */
static struct usb_device *msd;
static uint8_t buf[8192];

static int xfer(uint8_t ep, uint8_t *data, uint32_t len, uint32_t *actual)
{
	struct usb_xfer x = { .ep = ep, .type = USB_BULK, .data = data, .len = len };
	int st = msd->ops->submit(msd, &x);
	*actual = x.actual;
	return st;
}

static int request(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t length, uint32_t *actual)
{
	struct usb_xfer x = { .type = USB_CONTROL, .data = buf, .len = length,
			      .setup = { type, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index,
					 (uint8_t)(index >> 8), (uint8_t)length, (uint8_t)(length >> 8) } };
	int st = msd->ops->submit(msd, &x);
	*actual = x.actual;
	return st;
}

/* A command: CBW, data (in or out, of len), CSW; the CSW's status (-1: the transport failed), and residue */
static int command(const uint8_t *cdb, unsigned cdb_len, bool in, uint8_t *data, uint32_t len, uint32_t *residue)
{
	static uint32_t tag = 100;
	uint8_t cbw[31] = { 'U', 'S', 'B', 'C' };
	put_le32(cbw + 4, ++tag);
	put_le32(cbw + 8, len);
	cbw[12] = in ? 0x80 : 0;
	cbw[14] = (uint8_t)cdb_len;
	memcpy(cbw + 15, cdb, cdb_len);
	uint32_t got;
	if (xfer(0x02, cbw, 31, &got) != USB_OK)
		return -1;
	for (uint32_t done = 0; done < len;) {
		uint32_t n = len - done < 4096 ? len - done : 4096;
		if (xfer(in ? 0x81 : 0x02, data + done, n, &got) != USB_OK || got != n)
			return -1;
		done += n;
	}
	uint8_t csw[13];
	if (xfer(0x81, csw, 13, &got) != USB_OK || got != 13 || le32(csw) != 0x53425355u || le32(csw + 4) != tag)
		return -1;
	*residue = le32(csw + 8);
	return csw[12];
}

static void test_test_disk(void)
{
	char path[] = "/tmp/blue-usb-XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	static uint8_t image[64 * 512];
	for (unsigned k = 0; k < sizeof image; k++)
		image[k] = (uint8_t)(k / 512);
	CHECK(write(fd, image, sizeof image) == (ssize_t)sizeof image);
	close(fd);
	char err[200];
	msd = usbmsd_open(path, false, err, sizeof err);
	CHECK(msd != NULL);
	if (!msd)
		return;
	CHECK(usbmsd_is(msd));
	CHECK(usbmsd_open("/nonexistent/blue", false, err, sizeof err) == NULL);

	uint32_t got, residue;
	CHECK_EQ(request(0x80, 6, 0x0100, 0, 64, &got), USB_OK);        /* device descriptor */
	CHECK_EQ(got, 18);
	CHECK_EQ(buf[8] | buf[9] << 8, USBMSD_VENDOR);
	CHECK_EQ(request(0x80, 6, 0x0200, 0, 255, &got), USB_OK);       /* configuration: mass storage, SCSI, BOT */
	CHECK_EQ(got, 32);
	CHECK_EQ(buf[14] << 16 | buf[15] << 8 | buf[16], 0x080650);
	CHECK_EQ(request(0x80, 6, 0x0302, 0x409, 255, &got), USB_OK);
	CHECK_EQ(buf[2], 'U');
	CHECK_EQ(xfer(0x02, buf, 31, &got), USB_STALL);                 /* (not configured) */
	CHECK_EQ(request(0x00, 9, 1, 0, 0, &got), USB_OK);
	CHECK_EQ(request(0xA1, 0xFE, 0, 0, 1, &got), USB_OK);           /* Get Max LUN: one */
	CHECK_EQ(buf[0], 0);
	CHECK_EQ(xfer(0x81, buf, 64, &got), USB_NAK);                   /* (nothing to send) */

	static const uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
	CHECK_EQ(command(inquiry, 6, true, buf, 36, &residue), 0);
	CHECK_EQ(buf[0], 0x00);                                         /* direct access, removable */
	CHECK_EQ(buf[1], 0x80);
	CHECK(!memcmp(buf + 8, "Blue    ", 8));
	static const uint8_t capacity[10] = { 0x25 };
	CHECK_EQ(command(capacity, 10, true, buf, 8, &residue), 0);
	CHECK_EQ(be32(buf), 63);
	CHECK_EQ(be32(buf + 4), 512);
	static const uint8_t ready[6] = { 0 };
	CHECK_EQ(command(ready, 6, false, NULL, 0, &residue), 0);

	/* reads and writes */
	static const uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 5, 0, 0, 3, 0 };
	CHECK_EQ(command(read10, 10, true, buf, 3 * 512, &residue), 0);
	CHECK_EQ(buf[0], 5);
	CHECK_EQ(buf[1023], 6);
	CHECK_EQ(buf[1535], 7);
	memset(buf, 0xA5, 1024);
	static const uint8_t write10[10] = { 0x2A, 0, 0, 0, 0, 62, 0, 0, 2, 0 };
	CHECK_EQ(command(write10, 10, false, buf, 1024, &residue), 0);
	static const uint8_t read6[6] = { 0x08, 0, 0, 63, 1, 0 };
	memset(buf, 0, 512);
	CHECK_EQ(command(read6, 6, true, buf, 512, &residue), 0);
	CHECK_EQ(buf[511], 0xA5);

	/* the host asks for more than there is: zeros, and the residue says so */
	memset(buf, 0xFF, 64);
	CHECK_EQ(command(inquiry, 6, true, buf, 64, &residue), 0);
	CHECK_EQ(residue, 64 - 36);
	CHECK_EQ(buf[40], 0);

	/* out of range, and an unknown command: failed, and REQUEST SENSE says why */
	static const uint8_t past[10] = { 0x28, 0, 0, 0, 0, 63, 0, 0, 2, 0 };
	CHECK_EQ(command(past, 10, true, buf, 1024, &residue), 1);
	static const uint8_t sense[6] = { 0x03, 0, 0, 0, 18, 0 };
	CHECK_EQ(command(sense, 6, true, buf, 18, &residue), 0);
	CHECK_EQ(buf[2], 5);                                            /* ILLEGAL REQUEST */
	CHECK_EQ(buf[12], 0x21);                                        /* LBA out of range */
	static const uint8_t unknown[6] = { 0xC7 };
	CHECK_EQ(command(unknown, 6, false, NULL, 0, &residue), 1);
	CHECK_EQ(command(sense, 6, true, buf, 18, &residue), 0);
	CHECK_EQ(buf[12], 0x20);

	/* MODE SENSE: not write-protected */
	static const uint8_t mode6[6] = { 0x1A, 0, 0x3F, 0, 4, 0 };
	CHECK_EQ(command(mode6, 6, true, buf, 4, &residue), 0);
	CHECK_EQ(buf[2] & 0x80, 0);

	/* eject: refused while prevented, then the medium's gone */
	static const uint8_t prevent[6] = { 0x1E, 0, 0, 0, 1, 0 }, allow[6] = { 0x1E };
	static const uint8_t eject[6] = { 0x1B, 0, 0, 0, 2, 0 };
	CHECK_EQ(command(prevent, 6, false, NULL, 0, &residue), 0);
	CHECK_EQ(command(eject, 6, false, NULL, 0, &residue), 1);
	CHECK_EQ(command(allow, 6, false, NULL, 0, &residue), 0);
	CHECK_EQ(command(eject, 6, false, NULL, 0, &residue), 0);
	CHECK(usbmsd_ejected(msd));
	CHECK_EQ(command(ready, 6, false, NULL, 0, &residue), 1);
	CHECK_EQ(command(sense, 6, true, buf, 18, &residue), 0);
	CHECK_EQ(buf[2], 2);                                            /* NOT READY: medium not present */
	CHECK_EQ(buf[12], 0x3A);

	/* a bad CBW stalls both endpoints until the reset recovery */
	CHECK_EQ(xfer(0x02, buf, 20, &got), USB_STALL);
	CHECK_EQ(xfer(0x81, buf, 13, &got), USB_STALL);
	CHECK_EQ(request(0x82, 0, 0, 0x81, 2, &got), USB_OK);           /* GET_STATUS: halted */
	CHECK_EQ(buf[0], 1);
	CHECK_EQ(request(0x21, 0xFF, 0, 0, 0, &got), USB_OK);           /* Bulk-Only Mass Storage Reset */
	CHECK_EQ(request(0x02, 1, 0, 0x81, 0, &got), USB_OK);
	CHECK_EQ(request(0x02, 1, 0, 0x02, 0, &got), USB_OK);
	CHECK_EQ(command(sense, 6, true, buf, 18, &residue), 0);

	msd->ops->destroy(msd);
	unlink(path);
}

/* The test disk behind the controller, as Mac OS's driver would use it */
static void test_test_disk_on_bus(void)
{
	char path[] = "/tmp/blue-usb-XXXXXX";
	int fd = mkstemp(path);
	static uint8_t image[16 * 512];
	memset(image, 0x77, sizeof image);
	CHECK(write(fd, image, sizeof image) == (ssize_t)sizeof image);
	close(fd);
	char err[200];
	struct usb_device *d = usbmsd_open(path, false, err, sizeof err);
	ohci_free(&usb);
	ohci_init(&usb, &mem);
	memset(mem_ptr(&mem, 0), 0, RAM);
	now = 0;
	wr(HC_HCCA, HCCA);
	wr(HC_CONTROL, OPERATIONAL | LISTS);
	ohci_connect(&usb, 0, d);
	wr(HC_RH_PORT1, 0x10);
	frames(10);
	static const uint8_t set_config[8] = { 0, 9, 1, 0, 0, 0, 0, 0 };
	memcpy(mem_ptr(&mem, 0x8000), set_config, 8);
	ed(0x4000, ED(0, 0, D_TD, 64), 0x5000, 0x5080, 0x4040);
	td(0x5000, TD(DP_SETUP, 7, 2), 0x8000, 8, 0x5040);
	td(0x5040, TD(DP_IN, 0, 3), 0, 0, 0x5080);
	/* READ(10) of block 2, on the bulk endpoints' EDs (on the control list, as Mac OS has them) */
	uint8_t cbw[31] = { 'U', 'S', 'B', 'C', 1, 0, 0, 0, 0, 2, 0, 0, 0x80, 0, 10, 0x28, 0, 0, 0, 0, 2, 0, 0, 1, 0 };
	memcpy(mem_ptr(&mem, 0x8100), cbw, 31);
	ed(0x4040, ED(0, 2, D_OUT, 64), 0x5100, 0x5140, 0x4080);
	td(0x5100, TD(DP_OUT, 7, 0), 0x8100, 31, 0x5140);
	ed(0x4080, ED(0, 1, D_IN, 64), 0x5200, 0x5280, 0);
	td(0x5200, TD(DP_IN, 7, 0), 0x9000, 512, 0x5240);
	td(0x5240, TD(DP_IN, 0, 0), 0x9800, 13, 0x5280);
	wr(HC_CONTROL_HEAD, 0x4000);
	wr(HC_COMMAND_STATUS, CLF);
	frames(2);
	CHECK_EQ(get(0x4008), 0x5080);
	CHECK_EQ(get(0x4048) & ~2u, 0x5140);
	CHECK_EQ(get(0x4088) & ~2u, 0x5280);
	CHECK_EQ(mem_r8(&mem, 0x9000 + 511), 0x77);
	CHECK_EQ(le32(mem_ptr(&mem, 0x9800)), 0x53425355u);
	CHECK_EQ(mem_r8(&mem, 0x9800 + 12), 0);
	ohci_free(&usb);                        /* (destroys it) */
	unlink(path);
}

void test_usb(void)
{
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		return;
	}
	mem_map(&mem, 0, RAM, MEM_RWX);
	test_root_hub_ports();
	test_control();
	test_bulk();
	test_interrupt();
	test_iso();
	test_no_answer();
	test_test_disk();
	test_test_disk_on_bus();
	ohci_free(&usb);
	mem_free(&mem);
}
