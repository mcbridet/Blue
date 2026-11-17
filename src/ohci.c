#include "ohci.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Operational registers (byte offsets) */
enum {
	HC_REVISION = 0x00, HC_CONTROL = 0x04, HC_COMMAND_STATUS = 0x08, HC_INT_STATUS = 0x0C,
	HC_INT_ENABLE = 0x10, HC_INT_DISABLE = 0x14, HC_HCCA = 0x18, HC_PERIOD_CURRENT = 0x1C,
	HC_CONTROL_HEAD = 0x20, HC_CONTROL_CURRENT = 0x24, HC_BULK_HEAD = 0x28, HC_BULK_CURRENT = 0x2C,
	HC_DONE_HEAD = 0x30, HC_FM_INTERVAL = 0x34, HC_FM_REMAINING = 0x38, HC_FM_NUMBER = 0x3C,
	HC_PERIODIC_START = 0x40, HC_LS_THRESHOLD = 0x44, HC_RH_A = 0x48, HC_RH_B = 0x4C,
	HC_RH_STATUS = 0x50, HC_RH_PORT = 0x54,
};

/* HcControl */
#define CTL_PLE 0x00000004u             /* periodic list enable */
#define CTL_IE 0x00000008u              /* isochronous enable */
#define CTL_CLE 0x00000010u             /* control list enable */
#define CTL_BLE 0x00000020u             /* bulk list enable */
#define CTL_HCFS_SHIFT 6
#define CTL_HCFS 0x000000C0u
#define CTL_WRITABLE 0x000007FFu
enum { HCFS_RESET, HCFS_RESUME, HCFS_OPERATIONAL, HCFS_SUSPEND };

/* HcCommandStatus */
#define CMD_HCR 0x00000001u
#define CMD_CLF 0x00000002u
#define CMD_BLF 0x00000004u
#define CMD_OCR 0x00000008u

/* HcFmInterval */
#define FM_FI 0x00003FFFu
#define FM_FIT 0x80000000u
#define FM_DEFAULT 0x00002EDFu          /* 11999: frames of 12000 bit times */
#define LS_THRESHOLD_DEFAULT 0x00000628u

/* HcRhDescriptorA: 2 ports, always powered (no power switching), no overcurrent reporting */
#define RHA_NPS 0x00000200u
#define RHA_PSM 0x00000100u
#define RHA_NOCP 0x00001000u
#define RHA_WRITABLE 0xFF001B00u
#define RHA_DEFAULT (0x01000000u | RHA_NOCP | RHA_NPS | OHCI_PORTS)

/* HcRhStatus */
#define RHS_LPS 0x00000001u             /* write: clear global power */
#define RHS_DRWE 0x00008000u            /* write: set remote wakeup enable */
#define RHS_LPSC 0x00010000u            /* write: set global power */
#define RHS_OCIC 0x00020000u
#define RHS_CRWE 0x80000000u            /* write: clear remote wakeup enable */

/* HcRhPortStatus: what reads report, and the commands writes give */
#define PORT_CCS 0x00000001u            /* write: clear port enable */
#define PORT_PES 0x00000002u            /* write: set port enable */
#define PORT_PSS 0x00000004u            /* write: set port suspend */
#define PORT_POCI 0x00000008u           /* write: clear suspend status */
#define PORT_PRS 0x00000010u            /* write: set port reset */
#define PORT_PPS 0x00000100u            /* write: set port power */
#define PORT_LSDA 0x00000200u           /* write: clear port power */
#define PORT_CHANGES 0x001F0000u        /* CSC PESC PSSC OCIC PRSC: write 1 to clear */
#define PORT_CSC 0x00010000u
#define PORT_PESC 0x00020000u
#define PORT_PSSC 0x00040000u
#define PORT_PRSC 0x00100000u
#define RESET_FRAMES 10                 /* a port reset lasts 10 ms */

/* Endpoint descriptors: dword 0 */
#define ED_FA 0x0000007Fu
#define ED_EN_SHIFT 7
#define ED_D_SHIFT 11
#define ED_K 0x00004000u                /* skip */
#define ED_F 0x00008000u                /* isochronous TDs */
#define ED_MPS_SHIFT 16
/* ... dword 2 (HeadP) */
#define ED_H 0x00000001u                /* halted */
#define ED_C 0x00000002u                /* toggle carry */
#define PTR 0xFFFFFFF0u
#define ITD_PTR 0xFFFFFFE0u

/* General TDs: dword 0 */
#define TD_R 0x00040000u                /* buffer rounding: a short packet isn't an error */
#define TD_DP_SHIFT 19
#define TD_DI_SHIFT 21
#define TD_T0 0x01000000u
#define TD_T1 0x02000000u               /* the toggle is T0, not the ED's carry */
#define TD_EC_SHIFT 26
#define TD_CC_SHIFT 28
/* ... isochronous: dword 0 */
#define ITD_SF 0x0000FFFFu
#define ITD_FC_SHIFT 24

/* Condition codes */
enum {
	CC_NOERROR, CC_CRC, CC_BITSTUFFING, CC_TOGGLE, CC_STALL, CC_NORESPONSE, CC_PIDCHECK, CC_UNEXPECTEDPID,
	CC_DATAOVERRUN, CC_DATAUNDERRUN, CC_BUFFEROVERRUN = 12, CC_BUFFERUNDERRUN, CC_NOTACCESSED,
};

enum { LIST_PERIODIC, LIST_CONTROL, LIST_BULK };
enum { TD_RETIRED, TD_STAYS, TD_WAITING };

#define MAX_EDS 512                     /* EDs walked on a list in a frame (a guard against loops) */
#define MAX_TDS 512                     /* TDs serviced in a frame */
#define MAX_TDS_ED 64                   /* ... on one control or bulk ED */
#define MAX_FRAMES 64                   /* frames' lists serviced after a gap in time (the rest are counted) */
#define STALE_FRAMES 64                 /* a transfer in flight on an ED not seen for this long is given up */
#define TD_MAX 0x2000u                  /* a TD's buffer: two pages at most */

/* A bulk or interrupt TD's transfer, handed to its device, which answers later */
struct ohci_async {
	struct usb_xfer x;
	struct usb_device *dev;
	uint32_t ed, td;
	unsigned long seen;             /* the frame it was last looked for */
	bool used;
	uint8_t buf[TD_MAX];
};

struct ohci_work {
	struct ohci_async async[OHCI_ASYNC];
	uint8_t buf[TD_MAX];            /* control and isochronous transfers, which answer at once */
};

static unsigned hcfs(const struct ohci *o)
{
	return (o->control & CTL_HCFS) >> CTL_HCFS_SHIFT;
}

static uint64_t frame_length(const struct ohci *o)
{
	return (o->fm_interval & FM_FI) + 1u;
}

/* Guest memory, for descriptors and data (little-endian) */
static bool dma_ok(const struct ohci *o, uint32_t addr, uint32_t len, unsigned need)
{
	return len == 0 || mem_ok(o->mem, addr, len, need);
}

static uint32_t rd32(const struct ohci *o, uint32_t addr)
{
	return le32(mem_ptr(o->mem, addr));
}

static void wr32(struct ohci *o, uint32_t addr, uint32_t v)
{
	mem_invalidate(o->mem, addr, 4);
	put_le32(mem_ptr(o->mem, addr), v);
}

static void dma_copy(struct ohci *o, uint32_t addr, uint8_t *buf, uint32_t len, bool to_guest)
{
	if (!dma_ok(o, addr, len, to_guest ? MEM_W : MEM_R))
		return;
	if (to_guest) {
		mem_invalidate(o->mem, addr, len);
		memcpy(mem_ptr(o->mem, addr), buf, len);
	} else {
		memcpy(buf, mem_ptr(o->mem, addr), len);
	}
}

/* A general TD's buffer: from cbp to be, over two pages if it crosses one (the second: be's) */
static uint32_t td_length(uint32_t cbp, uint32_t be)
{
	if (!cbp)
		return 0;
	if ((cbp ^ be) & ~0xFFFu)
		return 0x1000u - (cbp & 0xFFFu) + (be & 0xFFFu) + 1;
	return be >= cbp ? be - cbp + 1 : 0;
}

static uint32_t td_address(uint32_t cbp, uint32_t be, uint32_t at)
{
	uint32_t first = 0x1000u - (cbp & 0xFFFu);
	if (!((cbp ^ be) & ~0xFFFu) || at < first)
		return cbp + at;
	return (be & ~0xFFFu) + (at - first);
}

static void td_copy(struct ohci *o, uint32_t cbp, uint32_t be, uint8_t *buf, uint32_t len, bool to_guest)
{
	uint32_t first = (cbp ^ be) & ~0xFFFu ? 0x1000u - (cbp & 0xFFFu) : len;
	if (first > len)
		first = len;
	dma_copy(o, cbp, buf, first, to_guest);
	if (len > first)
		dma_copy(o, be & ~0xFFFu, buf + first, len - first, to_guest);
}

/* Root hub status changes (any change bit) show in HcInterruptStatus. */
static void update_rhsc(struct ohci *o)
{
	bool change = (o->rh_status & RHS_OCIC) != 0;
	for (unsigned k = 0; k < OHCI_PORTS; k++)
		change |= (o->port[k] & PORT_CHANGES) != 0;
	if (change)
		o->int_status |= OHCI_INT_RHSC;
}

/* Frames start: the frame number advances and goes to the HCCA. */
static void start_frames(struct ohci *o, uint64_t n)
{
	uint16_t old = o->fm_number;
	o->fm_number = (uint16_t)(o->fm_number + n);
	o->frames += (unsigned long)n;
	o->frt = (o->fm_interval & FM_FIT) != 0;
	o->int_status |= OHCI_INT_SF;
	if (((old ^ o->fm_number) & 0x8000u) || n >= 0x8000u)
		o->int_status |= OHCI_INT_FNO;
	if (o->hcca && mem_ok(o->mem, o->hcca + 0x80, 4, MEM_W)) {
		mem_w8(o->mem, o->hcca + 0x80, (uint8_t)o->fm_number);         /* HccaFrameNumber */
		mem_w8(o->mem, o->hcca + 0x81, (uint8_t)(o->fm_number >> 8));
		mem_w8(o->mem, o->hcca + 0x82, 0);                              /* HccaPad1 */
		mem_w8(o->mem, o->hcca + 0x83, 0);
	}
}

/* The device at this address, on an enabled port that isn't being reset or suspended */
static struct usb_device *find_device(const struct ohci *o, unsigned addr)
{
	for (unsigned k = 0; k < OHCI_PORTS; k++) {
		struct usb_device *d = o->dev[k];
		if (d && (o->port[k] & (PORT_CCS | PORT_PES | PORT_PSS | PORT_PRS)) == (PORT_CCS | PORT_PES) &&
		    d->addr == addr)
			return d;
	}
	return NULL;
}

static void give_up(struct ohci_async *a)
{
	if (!atomic_load_explicit(&a->x.done, memory_order_acquire))
		a->dev->ops->cancel(a->dev, &a->x);
	a->used = false;
}

static void give_up_all(struct ohci *o, const struct usb_device *d, uint32_t ed)
{
	if (!o->work)
		return;
	for (unsigned k = 0; k < OHCI_ASYNC; k++) {
		struct ohci_async *a = &o->work->async[k];
		if (a->used && (!d || a->dev == d) && (!ed || a->ed == ed))
			give_up(a);
	}
}

static void unrecoverable(struct ohci *o)
{
	if (o->log)
		printf("usb: unrecoverable error (descriptors outside memory)\n");
	o->int_status |= OHCI_INT_UE;
	o->dead = true;
}

/* A TD goes on the done queue. */
static void retire(struct ohci *o, uint32_t td, uint32_t flags, unsigned cc)
{
	o->done_head = td;
	unsigned di = cc != CC_NOERROR ? 0 : (flags >> TD_DI_SHIFT) & 7;
	if (di < o->done_count)
		o->done_count = (uint8_t)di;
	o->tds++;
}

static unsigned packets_moved(int pid, uint32_t actual, uint32_t len, unsigned mps)
{
	if (!mps)
		return 1;
	if (pid == USB_PID_IN && actual < len)
		return actual / mps + 1;        /* (the last one short, or empty) */
	return len ? (len + mps - 1) / mps : 1;
}

/* Services the TD at the head of a general ED (e[]: its four words, kept up to date). */
static int service_td(struct ohci *o, uint32_t ed, uint32_t e[4], unsigned list)
{
	uint32_t td = e[2] & PTR;
	if (!dma_ok(o, td, 16, MEM_R | MEM_W)) {
		unrecoverable(o);
		return TD_STAYS;
	}
	uint32_t t[4];
	for (unsigned k = 0; k < 4; k++)
		t[k] = rd32(o, td + 4 * k);
	unsigned dir = (e[0] >> ED_D_SHIFT) & 3;
	if (dir == 0 || dir == 3)
		dir = (t[0] >> TD_DP_SHIFT) & 3;
	int pid = dir == 0 ? USB_PID_SETUP : dir == 1 ? USB_PID_OUT : USB_PID_IN;
	uint32_t cbp = t[1], be = t[3], len = td_length(cbp, be);
	unsigned fa = e[0] & ED_FA, ep = (e[0] >> ED_EN_SHIFT) & 15, mps = (e[0] >> ED_MPS_SHIFT) & 0x7FF;
	struct usb_device *dev = find_device(o, fa);
	uint32_t actual = 0;
	uint8_t *buf = o->work->buf;
	int st;

	if (dir == 3 || len > TD_MAX) {
		st = USB_ERROR;
	} else if (!dev) {
		st = USB_NODEV;
	} else if (ep == 0) {
		if (pid != USB_PID_IN)
			td_copy(o, cbp, be, buf, len, false);
		st = usb_control(dev, pid, buf, len, &actual);
	} else if (pid == USB_PID_SETUP) {
		st = USB_STALL;                 /* (no control endpoints but endpoint 0) */
	} else {
		struct ohci_async *a = NULL, *free_one = NULL;
		for (unsigned k = 0; k < OHCI_ASYNC; k++) {
			struct ohci_async *s = &o->work->async[k];
			if (s->used && s->ed == ed) {
				if (s->td == td && s->dev == dev)
					a = s;
				else
					give_up(s);     /* (the TD it was for isn't the ED's head any more) */
			}
			if (!s->used && !free_one)
				free_one = s;
		}
		if (a) {
			a->seen = o->frames;
			if (!atomic_load_explicit(&a->x.done, memory_order_acquire))
				return TD_WAITING;
			a->used = false;
		} else {
			if (!free_one)
				return TD_STAYS;
			a = free_one;
			a->x.ep = (uint8_t)(ep | (pid == USB_PID_IN ? USB_DIR_IN : 0));
			a->x.type = list == LIST_PERIODIC ? USB_INTERRUPT : USB_BULK;     /* (Mac OS has bulk EDs on the control list) */
			a->x.data = a->buf;
			a->x.len = len;
			a->x.actual = 0;
			a->x.status = USB_PENDING;
			atomic_store_explicit(&a->x.done, false, memory_order_relaxed);
			if (pid == USB_PID_OUT)
				td_copy(o, cbp, be, a->buf, len, false);
			int now = dev->ops->submit(dev, &a->x);
			if (now == USB_PENDING) {
				a->used = true;
				a->dev = dev;
				a->ed = ed;
				a->td = td;
				a->seen = o->frames;
				return TD_WAITING;
			}
			a->x.status = now;
		}
		st = a->x.status;
		actual = a->x.actual;
		buf = a->buf;
	}
	if (st == USB_NAK || st == USB_PENDING)
		return TD_STAYS;

	uint32_t flags = t[0];
	unsigned toggle = t[0] & TD_T1 ? (t[0] & TD_T0) != 0 : (e[2] & ED_C) != 0;
	unsigned cc = CC_NOERROR, ec = (t[0] >> TD_EC_SHIFT) & 3, packets = 0;
	uint32_t moved = 0;
	if (actual > len) {
		actual = len;
		if (st == USB_OK)
			st = USB_OVERRUN;
	}
	switch (st) {
	case USB_OK:
		moved = pid == USB_PID_IN ? actual : len;
		packets = packets_moved(pid, actual, len, mps);
		if (pid == USB_PID_IN && actual < len && !(t[0] & TD_R))
			cc = CC_DATAUNDERRUN;
		ec = 0;
		break;
	case USB_STALL:
	case USB_OVERRUN:
		cc = st == USB_STALL ? CC_STALL : CC_DATAOVERRUN;
		moved = actual;
		packets = mps ? actual / mps : 0;
		break;
	default:                                /* (no answer: tried three times; lost on the way: given up) */
		cc = st == USB_NODEV ? CC_NORESPONSE : CC_CRC;
		ec = st == USB_NODEV ? ec + 1 : 3;
		break;
	}
	if (pid == USB_PID_IN && moved)
		td_copy(o, cbp, be, buf, moved, true);
	if (packets) {
		toggle ^= packets & 1;
		t[0] = (t[0] & ~TD_T0) | TD_T1 | (toggle ? TD_T0 : 0);
	}
	t[0] = (t[0] & ~(0xFu << TD_CC_SHIFT | 3u << TD_EC_SHIFT)) | cc << TD_CC_SHIFT | (ec > 3 ? 3 : ec) << TD_EC_SHIFT;
	if (moved)
		t[1] = moved == len ? 0 : td_address(cbp, be, moved);
	if (o->log)
		printf("usb: TD %08X (%08X) ED %08X (%u.%u %s %s) %u/%u bytes -> cc %u%s\n", td, flags, ed, fa, ep,
		       list == LIST_PERIODIC ? "periodic" : list == LIST_CONTROL ? "control" : "bulk",
		       pid == USB_PID_SETUP ? "setup" : pid == USB_PID_IN ? "in" : "out", moved, len, cc,
		       cc && ec < 3 && st != USB_STALL && st != USB_OVERRUN ? " (again)" : "");
	wr32(o, td, t[0]);
	wr32(o, td + 4, t[1]);
	if (cc != CC_NOERROR && cc != CC_STALL && cc != CC_DATAOVERRUN && cc != CC_DATAUNDERRUN && ec < 3)
		return TD_STAYS;                /* (tried again at the next frame) */

	uint32_t next = t[2] & PTR;
	wr32(o, td + 8, o->done_head);
	e[2] = next | (t[0] & TD_T1 ? (t[0] & TD_T0 ? ED_C : 0) : e[2] & ED_C) | (cc != CC_NOERROR ? ED_H : 0);
	wr32(o, ed + 8, e[2]);
	retire(o, td, t[0], cc);
	return TD_RETIRED;
}

/* An isochronous TD's offset or packet status word k */
static uint16_t itd_word(const struct ohci *o, uint32_t td, unsigned k)
{
	return (uint16_t)(mem_r8(o->mem, td + 16 + 2 * k) | mem_r8(o->mem, td + 17 + 2 * k) << 8);
}

static void put_itd_word(struct ohci *o, uint32_t td, unsigned k, uint16_t v)
{
	mem_invalidate(o->mem, td + 16 + 2 * k, 2);
	mem_w8(o->mem, td + 16 + 2 * k, (uint8_t)v);
	mem_w8(o->mem, td + 17 + 2 * k, (uint8_t)(v >> 8));
}

/* Isochronous offsets: 13 bits, the top one choosing the page (BP0's or BE's) */
static void itd_copy(struct ohci *o, uint32_t bp0, uint32_t be, uint32_t from, uint8_t *buf, uint32_t len,
		     bool to_guest)
{
	while (len) {
		uint32_t page = from & 0x1000u ? be & ~0xFFFu : bp0 & ~0xFFFu;
		uint32_t n = 0x1000u - (from & 0xFFFu);
		if (n > len)
			n = len;
		dma_copy(o, page + (from & 0xFFFu), buf, n, to_guest);
		from += n;
		buf += n;
		len -= n;
	}
}

/* An isochronous ED: the packet its head TD has for this frame (late TDs are retired first). */
static void service_iso(struct ohci *o, uint32_t ed, uint32_t e[4])
{
	for (unsigned guard = 0; guard < 16; guard++) {
		uint32_t td = e[2] & ITD_PTR;
		if (td == (e[1] & ITD_PTR))
			return;
		if (!dma_ok(o, td, 32, MEM_R | MEM_W)) {
			unrecoverable(o);
			return;
		}
		uint32_t flags = rd32(o, td), bp0 = rd32(o, td + 4), next = rd32(o, td + 8), be = rd32(o, td + 12);
		unsigned fc = (flags >> ITD_FC_SHIFT) & 7;
		int16_t r = (int16_t)(o->fm_number - (flags & ITD_SF));
		if (r < 0)
			return;                 /* (not its time yet) */
		unsigned cc = CC_NOERROR;
		if (r > (int)fc) {
			if ((flags >> TD_CC_SHIFT) == CC_DATAOVERRUN)
				return;
			cc = CC_DATAOVERRUN;    /* its frames went by */
		} else {
			uint16_t off = itd_word(o, td, (unsigned)r);
			uint32_t from = off & 0x1FFFu, to;
			if ((unsigned)r < fc)
				to = itd_word(o, td, (unsigned)r + 1) & 0x1FFFu;
			else
				to = (((bp0 ^ be) & ~0xFFFu) ? 0x1000u : 0) + (be & 0xFFFu) + 1;
			if ((off >> 13) != 7 || to < from || to - from > 1023) {
				if (o->log)
					printf("usb: ITD %08X frame %d: bad offsets %04X to %X\n", td, r, off, to);
				return;
			}
			uint32_t len = to - from, actual = 0;
			unsigned dir = (e[0] >> ED_D_SHIFT) & 3;
			struct usb_device *dev = find_device(o, e[0] & ED_FA);
			int st = USB_NODEV;
			if (dev && (dir == 1 || dir == 2)) {
				struct usb_xfer x = { .ep = (uint8_t)((e[0] >> ED_EN_SHIFT & 15) | (dir == 2 ? USB_DIR_IN : 0)),
						      .type = USB_ISO, .data = o->work->buf, .len = len };
				if (dir == 1)
					itd_copy(o, bp0, be, from, o->work->buf, len, false);
				st = dev->ops->submit(dev, &x);
				if (st == USB_PENDING) {        /* (isochronous packets are answered at once) */
					dev->ops->cancel(dev, &x);
					st = USB_ERROR;
				}
				actual = x.actual > len ? len : x.actual;
				if (x.actual > len && st == USB_OK)
					st = USB_OVERRUN;
			}
			unsigned pcc;
			switch (st) {
			case USB_OK:
			case USB_NAK:
				pcc = dir == 2 && actual < len ? CC_DATAUNDERRUN : CC_NOERROR;
				break;
			case USB_STALL: pcc = CC_STALL; break;
			case USB_OVERRUN: pcc = CC_DATAOVERRUN; break;
			case USB_NODEV: pcc = CC_NORESPONSE; break;
			default: pcc = CC_CRC; break;
			}
			if (dir == 2 && actual)
				itd_copy(o, bp0, be, from, o->work->buf, actual, true);
			put_itd_word(o, td, (unsigned)r, (uint16_t)(pcc << 12 | (dir == 2 ? actual : 0)));
			if ((unsigned)r < fc)
				return;
		}
		/* the TD is done (or late) */
		flags = (flags & ~(0xFu << TD_CC_SHIFT)) | cc << TD_CC_SHIFT;
		wr32(o, td, flags);
		wr32(o, td + 8, o->done_head);
		e[2] = (next & ITD_PTR) | (e[2] & (ED_C | ED_H));
		wr32(o, ed + 8, e[2]);
		if (o->log)
			printf("usb: ITD %08X ED %08X frame %u -> cc %u\n", td, ed, o->fm_number, cc);
		retire(o, td, flags, cc);
		if (cc == CC_NOERROR)
			return;
	}
}

/* An ED's TDs, as many as this frame takes; whether it had any */
static bool service_ed(struct ohci *o, uint32_t ed, unsigned list, unsigned *budget)
{
	uint32_t e[4];
	for (unsigned k = 0; k < 4; k++)
		e[k] = rd32(o, ed + 4 * k);
	if ((e[0] & ED_K) || (e[2] & ED_H) || (e[2] & PTR) == (e[1] & PTR)) {
		give_up_all(o, NULL, ed);
		return false;
	}
	if (e[0] & ED_F) {
		if (list == LIST_PERIODIC)
			service_iso(o, ed, e);
		o->busy = true;
		return true;
	}
	unsigned most = list == LIST_PERIODIC ? 1 : MAX_TDS_ED;
	for (unsigned k = 0; k < most && *budget && !o->dead; k++) {
		--*budget;
		int r = service_td(o, ed, e, list);
		if (r != TD_WAITING)
			o->busy = true;
		if (r != TD_RETIRED || (e[2] & ED_H) || (e[2] & PTR) == (e[1] & PTR))
			break;
	}
	return true;
}

/* The EDs of a list from `ed` on; whether any had TDs */
static bool service_list(struct ohci *o, uint32_t ed, unsigned list, unsigned *budget, uint32_t *current)
{
	bool found = false;
	for (unsigned n = 0; ed && n < MAX_EDS && !o->dead; n++) {
		ed &= PTR;
		if (!dma_ok(o, ed, 16, MEM_R | MEM_W)) {
			unrecoverable(o);
			break;
		}
		if (list == LIST_PERIODIC && (rd32(o, ed) & ED_F) && !(o->control & CTL_IE))
			break;                  /* (isochronous EDs are last, and are skipped without IE) */
		*current = ed;
		found |= service_ed(o, ed, list, budget);
		ed = rd32(o, ed + 12);
	}
	*current = 0;
	return found;
}

/* The frame that ends: its lists, then the done queue, then the root hub's port resets. */
static void end_frame(struct ohci *o)
{
	o->busy = false;
	if (!o->dead) {
		unsigned budget = MAX_TDS;
		if ((o->control & CTL_PLE) && o->hcca && dma_ok(o, o->hcca, 0x80, MEM_R))
			service_list(o, rd32(o, o->hcca + 4 * (o->fm_number & 31)), LIST_PERIODIC, &budget,
				     &o->period_current);
		/* (ControlListFilled and BulkListFilled: set when a list had TDs, so it's looked at again) */
		if ((o->control & CTL_CLE) && (o->command_status & CMD_CLF)) {
			o->command_status &= ~CMD_CLF;
			if (service_list(o, o->control_head, LIST_CONTROL, &budget, &o->control_current))
				o->command_status |= CMD_CLF;
		}
		if ((o->control & CTL_BLE) && (o->command_status & CMD_BLF)) {
			o->command_status &= ~CMD_BLF;
			if (service_list(o, o->bulk_head, LIST_BULK, &budget, &o->bulk_current))
				o->command_status |= CMD_BLF;
		}
	}
	for (unsigned k = 0; o->work && k < OHCI_ASYNC; k++) {
		struct ohci_async *a = &o->work->async[k];
		if (a->used && o->frames - a->seen > STALE_FRAMES)
			give_up(a);
	}

	if (o->done_head && o->done_count == 0 && !(o->int_status & OHCI_INT_WDH) && o->hcca &&
	    dma_ok(o, o->hcca + 0x84, 4, MEM_W)) {
		bool others = (o->int_status & o->int_enable & ~OHCI_INT_WDH) != 0;
		wr32(o, o->hcca + 0x84, o->done_head | (others ? 1 : 0));
		o->done_head = 0;
		o->done_count = 7;
		o->int_status |= OHCI_INT_WDH;
	}
	if (o->done_count != 7 && o->done_count)
		o->done_count--;

	for (unsigned k = 0; k < OHCI_PORTS; k++) {
		if (o->reset_frames[k] && --o->reset_frames[k] == 0) {
			o->port[k] = (o->port[k] & ~(PORT_PRS | PORT_PSS)) | PORT_PES | PORT_PRSC;
			if (o->dev[k])
				usb_device_reset(o->dev[k]);
			update_rhsc(o);
		}
	}
}

/* Port k's device, as a reset of the bus leaves it: connected, not yet enabled */
static void reset_port(struct ohci *o, unsigned k)
{
	struct usb_device *d = o->dev[k];
	o->reset_frames[k] = 0;
	if (!d)
		return;
	o->port[k] = (o->port[k] & PORT_PPS) | PORT_CCS | PORT_CSC | (d->speed == USB_SPEED_LOW ? PORT_LSDA : 0);
	give_up_all(o, d, 0);
	usb_device_reset(d);
}

/* The software reset (HostControllerReset): registers to their reset values, then USBSUSPEND. */
static void reset_hc(struct ohci *o)
{
	give_up_all(o, NULL, 0);
	o->control = HCFS_SUSPEND << CTL_HCFS_SHIFT;
	o->command_status = o->int_status = o->int_enable = o->hcca = 0;
	o->period_current = o->control_head = o->control_current = 0;
	o->bulk_head = o->bulk_current = o->done_head = 0;
	o->fm_interval = FM_DEFAULT;
	o->fm_number = 0;
	o->frt = false;
	o->periodic_start = 0;
	o->ls_threshold = LS_THRESHOLD_DEFAULT;
	o->done_count = 7;
	o->dead = o->busy = false;
}

void ohci_init(struct ohci *o, struct mem *mem)
{
	memset(o, 0, sizeof *o);
	o->mem = mem;
	reset_hc(o);
	o->control = HCFS_RESET << CTL_HCFS_SHIFT;     /* (a hardware reset leaves it in USBRESET) */
	o->rh_a = RHA_DEFAULT;
	for (unsigned k = 0; k < OHCI_PORTS; k++)
		o->port[k] = PORT_PPS;
}

void ohci_free(struct ohci *o)
{
	for (unsigned k = 0; k < OHCI_PORTS; k++) {
		struct usb_device *d = ohci_disconnect(o, k);
		if (d)
			d->ops->destroy(d);
	}
	free(o->work);
	o->work = NULL;
}

bool ohci_connect(struct ohci *o, unsigned k, struct usb_device *d)
{
	if (k >= OHCI_PORTS || o->dev[k])
		return false;
	if (!o->work && !(o->work = calloc(1, sizeof *o->work)))
		return false;
	o->dev[k] = d;
	o->ever = true;
	d->log = o->log;
	reset_port(o, k);
	update_rhsc(o);
	return true;
}

struct usb_device *ohci_disconnect(struct ohci *o, unsigned k)
{
	struct usb_device *d = k < OHCI_PORTS ? o->dev[k] : NULL;
	if (!d)
		return NULL;
	give_up_all(o, d, 0);
	usb_control_cancel(d);
	o->dev[k] = NULL;
	o->reset_frames[k] = 0;
	uint32_t was = o->port[k];
	o->port[k] = (was & (PORT_PPS | PORT_CHANGES)) | PORT_CSC | (was & PORT_PES ? PORT_PESC : 0);
	update_rhsc(o);
	return d;
}

struct usb_device *ohci_device(const struct ohci *o, unsigned k)
{
	return k < OHCI_PORTS ? o->dev[k] : NULL;
}

bool ohci_waiting(const struct ohci *o)
{
	for (unsigned k = 0; o->work && k < OHCI_ASYNC; k++)
		if (o->work->async[k].used)
			return true;
	for (unsigned k = 0; k < OHCI_PORTS; k++)
		if (o->dev[k] && o->dev[k]->waiting)
			return true;
	return false;
}

static uint32_t port_read(const struct ohci *o, unsigned k)
{
	uint32_t v = o->port[k];
	if (o->rh_a & RHA_NPS)
		v |= PORT_PPS;
	return v;
}

static uint32_t reg_read(struct ohci *o, uint32_t reg)
{
	switch (reg) {
	case HC_REVISION: return 0x10;                  /* OpenHCI 1.0, no legacy support */
	case HC_CONTROL: return o->control;
	case HC_COMMAND_STATUS: return o->command_status;
	case HC_INT_STATUS: return o->int_status;
	case HC_INT_ENABLE:
	case HC_INT_DISABLE: return o->int_enable;
	case HC_HCCA: return o->hcca;
	case HC_PERIOD_CURRENT: return o->period_current;
	case HC_CONTROL_HEAD: return o->control_head;
	case HC_CONTROL_CURRENT: return o->control_current;
	case HC_BULK_HEAD: return o->bulk_head;
	case HC_BULK_CURRENT: return o->bulk_current;
	case HC_DONE_HEAD: return o->done_head;
	case HC_FM_INTERVAL: return o->fm_interval;
	case HC_FM_REMAINING: {
		if (hcfs(o) != HCFS_OPERATIONAL)
			return 0;
		uint64_t fi = o->fm_interval & FM_FI, gone = o->now - o->frame_start;
		return (uint32_t)(gone < fi ? fi - gone : 0) | (o->frt ? FM_FIT : 0);
	}
	case HC_FM_NUMBER: return o->fm_number;
	case HC_PERIODIC_START: return o->periodic_start;
	case HC_LS_THRESHOLD: return o->ls_threshold;
	case HC_RH_A: return o->rh_a;
	case HC_RH_B: return o->rh_b;
	case HC_RH_STATUS: return o->rh_status;
	default:
		if (reg >= HC_RH_PORT && reg < HC_RH_PORT + 4 * OHCI_PORTS)
			return port_read(o, (reg - HC_RH_PORT) / 4);
		return 0;
	}
}

static void port_write(struct ohci *o, unsigned k, uint32_t v)
{
	uint32_t *p = &o->port[k];
	*p &= ~(v & PORT_CHANGES);
	if (*p & PORT_CCS) {
		if (v & PORT_CCS)                       /* ClearPortEnable */
			*p &= ~PORT_PES;
		if (v & PORT_PES)
			*p |= PORT_PES;
		if ((v & PORT_PSS) && (*p & PORT_PES))
			*p |= PORT_PSS;
		if ((v & PORT_POCI) && (*p & PORT_PSS))  /* ClearSuspendStatus: it resumes */
			*p = (*p & ~PORT_PSS) | PORT_PSSC;
		if ((v & PORT_PRS) && !(*p & PORT_PRS)) {
			*p |= PORT_PRS;
			o->reset_frames[k] = RESET_FRAMES;
			if (hcfs(o) != HCFS_OPERATIONAL)        /* (no frames to count it in) */
				o->reset_frames[k] = 1;
		}
	} else if (v & (PORT_PES | PORT_PSS | PORT_PRS)) {
		/* With nothing connected, enabling, suspending or resetting the port only
		 * reports a connect status change (so that the driver looks again). */
		*p |= PORT_CSC;
	}
	if (!(o->rh_a & RHA_NPS)) {
		bool per_port = (o->rh_a & RHA_PSM) && (o->rh_b & (1u << (k + 17)));
		if (per_port && (v & PORT_PPS))
			*p |= PORT_PPS;
		if (per_port && (v & PORT_LSDA))
			*p &= ~PORT_PPS;
	}
	update_rhsc(o);
	if (o->reset_frames[k] == 1 && hcfs(o) != HCFS_OPERATIONAL) {
		o->reset_frames[k] = 0;
		*p = (*p & ~(PORT_PRS | PORT_PSS)) | PORT_PES | PORT_PRSC;
		if (o->dev[k])
			usb_device_reset(o->dev[k]);
	}
}

static void rh_status_write(struct ohci *o, uint32_t v)
{
	if (v & RHS_DRWE)
		o->rh_status |= RHS_DRWE;
	if (v & RHS_CRWE)
		o->rh_status &= ~RHS_DRWE;
	o->rh_status &= ~(v & RHS_OCIC);
	if (!(o->rh_a & RHA_NPS)) {     /* global power: every port not switched on its own */
		for (unsigned k = 0; k < OHCI_PORTS; k++) {
			if ((o->rh_a & RHA_PSM) && (o->rh_b & (1u << (k + 17))))
				continue;
			if (v & RHS_LPSC)
				o->port[k] |= PORT_PPS;
			if (v & RHS_LPS)
				o->port[k] &= ~PORT_PPS;
		}
	}
}

/* `v` holds the bytes written (little-endian), `mask` which bytes they are. */
static void reg_write(struct ohci *o, uint32_t reg, uint32_t v, uint32_t mask)
{
	uint32_t *plain = NULL;

	switch (reg) {
	case HC_CONTROL: {
		unsigned was = hcfs(o);
		o->control = (o->control & ~(mask & CTL_WRITABLE)) | (v & mask & CTL_WRITABLE);
		if (hcfs(o) == HCFS_OPERATIONAL && was != HCFS_OPERATIONAL) {
			o->frame_start = o->now;
			if (!o->ever)
				o->command_status &= ~(CMD_CLF | CMD_BLF);
			start_frames(o, 1);
		}
		if (hcfs(o) == HCFS_RESET && was != HCFS_RESET) {      /* reset signalled on the bus */
			bool any = false;
			for (unsigned k = 0; k < OHCI_PORTS; k++) {
				any |= o->dev[k] != NULL;
				reset_port(o, k);
			}
			if (any)
				update_rhsc(o);
		}
		return;
	}
	case HC_COMMAND_STATUS:
		v &= mask;
		if (v & CMD_HCR) {
			reset_hc(o);
			return;
		}
		o->command_status |= v & (CMD_CLF | CMD_BLF);  /* (OCR: no SMM to hand over from) */
		return;
	case HC_INT_STATUS:
		o->int_status &= ~(v & mask);
		return;
	case HC_INT_ENABLE:
		o->int_enable |= v & mask & (0xC000007Fu);
		return;
	case HC_INT_DISABLE:
		o->int_enable &= ~(v & mask);
		return;
	case HC_HCCA:
		mask &= 0xFFFFFF00u;
		plain = &o->hcca;
		break;
	case HC_PERIOD_CURRENT: plain = &o->period_current; break;
	case HC_CONTROL_HEAD: plain = &o->control_head; break;
	case HC_CONTROL_CURRENT: plain = &o->control_current; break;
	case HC_BULK_HEAD: plain = &o->bulk_head; break;
	case HC_BULK_CURRENT: plain = &o->bulk_current; break;
	case HC_FM_INTERVAL:
		mask &= 0xFFFF3FFFu;
		plain = &o->fm_interval;
		break;
	case HC_PERIODIC_START:
		mask &= FM_FI;
		plain = &o->periodic_start;
		break;
	case HC_LS_THRESHOLD:
		mask &= 0x0FFFu;
		plain = &o->ls_threshold;
		break;
	case HC_RH_A:
		mask &= RHA_WRITABLE;
		plain = &o->rh_a;
		break;
	case HC_RH_B: plain = &o->rh_b; break;
	case HC_RH_STATUS:
		rh_status_write(o, v & mask);
		return;
	default:
		if (reg >= HC_RH_PORT && reg < HC_RH_PORT + 4 * OHCI_PORTS)
			port_write(o, (reg - HC_RH_PORT) / 4, v & mask);
		return;         /* (read-only: revision, done head, frame remaining/number) */
	}
	*plain = (*plain & ~mask) | (v & mask);
}

/* Registers are little-endian: byte k of a register is bits 8k..8k+7 of its value. */
static uint32_t ohci_read(void *ctx, uint32_t offset, unsigned size)
{
	struct ohci *o = ctx;
	uint32_t v = reg_read(o, offset & ~3u), r = 0;
	for (unsigned k = 0; k < size; k++)
		r = r << 8 | ((v >> (8 * ((offset + k) & 3))) & 0xFF);
	if (o->log > 1 && (offset & ~3u) != HC_FM_NUMBER && (offset & ~3u) != HC_FM_REMAINING)     /* (not the busy-waits) */
		printf("usb: read  %02X (%u) -> %0*X (register %08X)\n", offset, size, 2 * size, r, v);
	return r;
}

static void ohci_write(void *ctx, uint32_t offset, unsigned size, uint32_t value)
{
	struct ohci *o = ctx;
	uint32_t v = 0, mask = 0;
	for (unsigned k = 0; k < size; k++) {
		unsigned shift = 8 * ((offset + k) & 3);
		v |= ((value >> (8 * (size - 1 - k))) & 0xFF) << shift;
		mask |= 0xFFu << shift;
	}
	if (o->log > 1)
		printf("usb: write %02X (%u) <- %0*X (register %08X, bytes %08X)\n", offset, size, 2 * size, value, v, mask);
	reg_write(o, offset & ~3u, v, mask);
}

const struct mmio_ops ohci_mmio_ops = { ohci_read, ohci_write };

bool ohci_pure_read(const void *ctx, uint32_t offset)
{
	(void)ctx;
	(void)offset;
	return true;
}

void ohci_set_time(struct ohci *o, uint64_t bit_times)
{
	if (bit_times <= o->now)
		return;
	o->now = bit_times;
	if (hcfs(o) != HCFS_OPERATIONAL)
		return;
	uint64_t len = frame_length(o);
	if (o->now - o->frame_start < len)
		return;
	uint64_t n = (o->now - o->frame_start) / len;
	o->frame_start += n * len;
	if (!o->ever) {
		/* (with no device, ever, the lists are found empty) */
		o->command_status &= ~(CMD_CLF | CMD_BLF);
		start_frames(o, n);
		return;
	}
	if (n > MAX_FRAMES) {
		start_frames(o, n - MAX_FRAMES);
		n = MAX_FRAMES;
	}
	while (n--) {
		end_frame(o);
		start_frames(o, 1);
	}
}

/* The next frame start that interrupts, frames' work aside */
static uint64_t next_frame_interrupt(const struct ohci *o)
{
	if (!(o->int_enable & OHCI_INT_MIE) || !(o->int_enable & (OHCI_INT_SF | OHCI_INT_FNO)))
		return UINT64_MAX;
	return o->frame_start + frame_length(o);
}

uint64_t ohci_next_event(const struct ohci *o)
{
	if (hcfs(o) != HCFS_OPERATIONAL)
		return UINT64_MAX;
	if (o->ever)
		return o->frame_start + frame_length(o);        /* (each frame's lists) */
	return next_frame_interrupt(o);
}

uint64_t ohci_next_interrupt(const struct ohci *o)
{
	if (hcfs(o) != HCFS_OPERATIONAL)
		return UINT64_MAX;
	bool resetting = false;
	for (unsigned k = 0; k < OHCI_PORTS; k++)
		resetting |= o->reset_frames[k] != 0;
	if (o->ever && (o->busy || o->done_head || resetting))
		return o->frame_start + frame_length(o);
	uint64_t next = next_frame_interrupt(o);
	if (next == UINT64_MAX || (o->int_enable & OHCI_INT_SF))
		return next;
	return o->frame_start + (0x8000u - (o->fm_number & 0x7FFFu)) * frame_length(o);
}
