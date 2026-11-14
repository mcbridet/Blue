#include "ohci.h"

#include <stdio.h>
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

static unsigned hcfs(const struct ohci *o)
{
	return (o->control & CTL_HCFS) >> CTL_HCFS_SHIFT;
}

static uint64_t frame_length(const struct ohci *o)
{
	return (o->fm_interval & FM_FI) + 1u;
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

/* A frame starts: the frame number advances and goes to the HCCA. */
static void start_frames(struct ohci *o, uint64_t n)
{
	uint16_t old = o->fm_number;
	o->fm_number = (uint16_t)(o->fm_number + n);
	o->frames += (unsigned long)n;
	o->frt = (o->fm_interval & FM_FIT) != 0;
	o->int_status |= OHCI_INT_SF;
	if (((old ^ o->fm_number) & 0x8000u) || n >= 0x8000u)
		o->int_status |= OHCI_INT_FNO;
	o->command_status &= ~(CMD_CLF | CMD_BLF);      /* (the lists are found empty) */
	if (o->hcca && mem_ok(o->mem, o->hcca + 0x80, 4, MEM_W)) {
		mem_w8(o->mem, o->hcca + 0x80, (uint8_t)o->fm_number);         /* HccaFrameNumber */
		mem_w8(o->mem, o->hcca + 0x81, (uint8_t)(o->fm_number >> 8));
		mem_w8(o->mem, o->hcca + 0x82, 0);                              /* HccaPad1 */
		mem_w8(o->mem, o->hcca + 0x83, 0);
	}
}

/* The software reset (HostControllerReset): registers to their reset values, then USBSUSPEND. */
static void reset_hc(struct ohci *o)
{
	o->control = HCFS_SUSPEND << CTL_HCFS_SHIFT;
	o->command_status = o->int_status = o->int_enable = o->hcca = 0;
	o->period_current = o->control_head = o->control_current = 0;
	o->bulk_head = o->bulk_current = o->done_head = 0;
	o->fm_interval = FM_DEFAULT;
	o->fm_number = 0;
	o->frt = false;
	o->periodic_start = 0;
	o->ls_threshold = LS_THRESHOLD_DEFAULT;
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
	/* With nothing connected, enabling, suspending or resetting the port only
	 * reports a connect status change (so that the driver looks again). */
	if (v & (PORT_PES | PORT_PSS | PORT_PRS))
		*p |= PORT_CSC;
	if (!(o->rh_a & RHA_NPS)) {
		bool per_port = (o->rh_a & RHA_PSM) && (o->rh_b & (1u << (k + 17)));
		if (per_port && (v & PORT_PPS))
			*p |= PORT_PPS;
		if (per_port && (v & PORT_LSDA))
			*p &= ~PORT_PPS;
	}
	update_rhsc(o);
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
			start_frames(o, 1);
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
	if (o->log && (offset & ~3u) != HC_FM_NUMBER && (offset & ~3u) != HC_FM_REMAINING)     /* (not the busy-waits) */
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
	if (o->log)
		printf("usb: write %02X (%u) <- %0*X (register %08X, bytes %08X)\n", offset, size, 2 * size, value, v, mask);
	reg_write(o, offset & ~3u, v, mask);
}

const struct mmio_ops ohci_mmio_ops = { ohci_read, ohci_write };

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
	start_frames(o, n);
}

uint64_t ohci_next_event(const struct ohci *o)
{
	if (hcfs(o) != HCFS_OPERATIONAL || !(o->int_enable & OHCI_INT_MIE) ||
	    !(o->int_enable & (OHCI_INT_SF | OHCI_INT_FNO)))
		return UINT64_MAX;
	return o->frame_start + frame_length(o);
}

uint64_t ohci_next_interrupt(const struct ohci *o)
{
	uint64_t next = ohci_next_event(o);
	if (next == UINT64_MAX || (o->int_enable & OHCI_INT_SF))
		return next;
	return o->frame_start + (0x8000u - (o->fm_number & 0x7FFFu)) * frame_length(o);
}

