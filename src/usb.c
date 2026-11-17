#include "usb.h"

#include <stdio.h>
#include <string.h>

void usb_device_init(struct usb_device *d, const struct usb_device_ops *ops, unsigned speed, const char *name)
{
	d->ops = ops;
	d->speed = speed;
	snprintf(d->name, sizeof d->name, "%s", name);
	d->addr = 0;
	d->stage = USB_CTL_IDLE;
	d->submitted = false;
	d->ctl.data = d->ctl_buf;
	atomic_init(&d->ctl.done, false);
}

/* Whether endpoint 0's request is still with the device */
static bool ctl_pending(struct usb_device *d)
{
	if (d->waiting && atomic_load_explicit(&d->ctl.done, memory_order_acquire))
		d->waiting = false;
	return d->waiting;
}

void usb_control_cancel(struct usb_device *d)
{
	if (ctl_pending(d))
		d->ops->cancel(d, &d->ctl);
	d->waiting = d->submitted = false;
	d->stage = USB_CTL_IDLE;
}

static void ctl_submit(struct usb_device *d)
{
	d->submitted = true;
	d->ctl.status = USB_PENDING;
	d->ctl.actual = 0;
	atomic_store_explicit(&d->ctl.done, false, memory_order_relaxed);
	int st = d->ops->submit(d, &d->ctl);
	d->waiting = st == USB_PENDING;
	if (st != USB_PENDING)
		d->ctl.status = st;
	if (d->log && st != USB_PENDING)
		printf("usb: %s: request %02X %02X -> %d (%u bytes)\n", d->name, d->ctl.setup[0], d->ctl.setup[1],
		       st, d->ctl.actual);
}

/* The request's answer: USB_NAK while it hasn't come */
static int ctl_result(struct usb_device *d)
{
	return ctl_pending(d) ? USB_NAK : d->ctl.status;
}

int usb_control(struct usb_device *d, enum usb_pid pid, uint8_t *buf, uint32_t len, uint32_t *actual)
{
	*actual = 0;
	if (pid == USB_PID_SETUP) {
		if (len != 8)
			return USB_STALL;
		usb_control_cancel(d);
		memcpy(d->ctl.setup, buf, 8);
		uint16_t length = usb_w16(buf + 6);
		d->ctl.ep = buf[0] & USB_DIR_IN;
		d->ctl.type = USB_CONTROL;
		d->ctl.data = d->ctl_buf;
		d->ctl.len = length;
		d->pos = 0;
		d->set_address = buf[0] == 0x00 && buf[1] == USB_REQ_SET_ADDRESS;
		if (d->log)
			printf("usb: %s: setup %02X %02X %04X %04X %04X\n", d->name, buf[0], buf[1], usb_w16(buf + 2),
			       usb_w16(buf + 4), length);
		if (buf[0] & USB_DIR_IN) {
			d->stage = USB_CTL_DATA_IN;
			ctl_submit(d);
		} else {
			d->stage = length ? USB_CTL_DATA_OUT : USB_CTL_STATUS_IN;
		}
		*actual = 8;
		return USB_OK;
	}

	if (pid == USB_PID_IN) {
		if (d->stage == USB_CTL_DATA_IN) {
			int st = ctl_result(d);
			if (st != USB_OK)
				return st;
			uint32_t got = d->ctl.actual < d->ctl.len ? d->ctl.actual : d->ctl.len;
			uint32_t n = got - d->pos < len ? got - d->pos : len;
			memcpy(buf, d->ctl_buf + d->pos, n);
			d->pos += n;
			*actual = n;
			return USB_OK;
		}
		if (d->stage == USB_CTL_DATA_OUT || d->stage == USB_CTL_STATUS_IN) {
			/* the status stage of a request without data, or with data out: it's carried out now */
			if (d->set_address) {
				d->addr = d->ctl.setup[2] & 0x7F;
				d->stage = USB_CTL_IDLE;
				if (d->log)
					printf("usb: %s: address %u\n", d->name, d->addr);
				return USB_OK;
			}
			if (!d->submitted) {
				d->ctl.len = d->pos;
				ctl_submit(d);
			}
			int st = ctl_result(d);
			if (st != USB_NAK)
				d->stage = USB_CTL_IDLE;
			return st;
		}
		return USB_STALL;
	}

	/* OUT */
	if (d->stage == USB_CTL_DATA_OUT) {
		if (len > d->ctl.len - d->pos)
			return USB_STALL;
		memcpy(d->ctl_buf + d->pos, buf, len);
		d->pos += len;
		*actual = len;
		return USB_OK;
	}
	if (d->stage == USB_CTL_DATA_IN) {      /* the status stage of a request with data in */
		int st = ctl_result(d);
		if (st != USB_NAK)
			d->stage = USB_CTL_IDLE;
		return st;
	}
	return USB_STALL;
}

void usb_device_reset(struct usb_device *d)
{
	usb_control_cancel(d);
	d->addr = 0;
	d->ops->reset(d);
}

uint32_t usb_string_descriptor(const char *s, uint8_t *out, uint32_t size)
{
	uint32_t n = 2;
	for (; *s && n + 2 <= size && n + 2 <= 254; s++) {
		out[n++] = (uint8_t)*s;
		out[n++] = 0;
	}
	out[0] = (uint8_t)n;
	out[1] = USB_DT_STRING;
	return n;
}
