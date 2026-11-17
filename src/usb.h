/*
 * USB devices, as the OHCI controller's root hub ports see them (ohci.h):
 * the built-in test disk (usbmsd.h) and devices passed through from the
 * host (usbhost.h). Each takes transfers on its endpoints: a whole control
 * request on endpoint 0 (its setup packet, then its data), or one TD's
 * worth of a bulk or interrupt transfer, or one isochronous packet, on
 * another. It answers at once, or later (USB_PENDING, not for isochronous
 * packets): then whichever thread finishes it sets the transfer's status
 * and length, and `done` last, and the controller sees that at a frame.
 *
 * usb_control() turns endpoint 0's SETUP, IN and OUT stages into those
 * requests, and keeps the device's address: SET_ADDRESS never reaches it.
 */
#ifndef BLUE_USB_H
#define BLUE_USB_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

enum usb_speed { USB_SPEED_LOW, USB_SPEED_FULL, USB_SPEED_HIGH, USB_SPEED_SUPER };

/* Endpoint types, as in bmAttributes */
enum { USB_CONTROL, USB_ISO, USB_BULK, USB_INTERRUPT };

#define USB_DIR_IN 0x80u

/* Tokens */
enum usb_pid { USB_PID_SETUP, USB_PID_OUT, USB_PID_IN };

enum usb_status {
	USB_OK,                 /* done: `actual` bytes moved */
	USB_NAK,                /* not now: try again at a later frame */
	USB_STALL,              /* the endpoint (or the request) is halted */
	USB_PENDING,            /* the answer comes later */
	USB_NODEV,              /* no answer: the device isn't there */
	USB_OVERRUN,            /* more came than there was room for */
	USB_ERROR,              /* lost on the way */
};

/* Standard requests */
enum {
	USB_REQ_GET_STATUS = 0, USB_REQ_CLEAR_FEATURE = 1, USB_REQ_SET_FEATURE = 3, USB_REQ_SET_ADDRESS = 5,
	USB_REQ_GET_DESCRIPTOR = 6, USB_REQ_SET_DESCRIPTOR = 7, USB_REQ_GET_CONFIGURATION = 8,
	USB_REQ_SET_CONFIGURATION = 9, USB_REQ_GET_INTERFACE = 10, USB_REQ_SET_INTERFACE = 11,
};
enum { USB_DT_DEVICE = 1, USB_DT_CONFIG = 2, USB_DT_STRING = 3, USB_DT_INTERFACE = 4, USB_DT_ENDPOINT = 5 };

struct usb_xfer {
	uint8_t ep;             /* endpoint address: its number, | USB_DIR_IN for IN */
	uint8_t type;           /* USB_CONTROL, USB_ISO, USB_BULK or USB_INTERRUPT */
	uint8_t setup[8];       /* (control) */
	uint8_t *data;          /* what's sent, or where what's received goes */
	uint32_t len;           /* bytes to send, or room for those received */
	/* the answer */
	int status;             /* enum usb_status */
	uint32_t actual;        /* bytes moved */
	atomic_bool done;       /* (an answer that came later: set last) */
};

struct usb_device;

struct usb_device_ops {
	/* Starts a transfer: returns its status, or USB_PENDING (see above). */
	int (*submit)(struct usb_device *d, struct usb_xfer *x);
	/* Gives up a pending transfer: once this returns, it isn't touched again. */
	void (*cancel)(struct usb_device *d, struct usb_xfer *x);
	/* A bus reset: back to the default state (address 0, unconfigured). */
	void (*reset)(struct usb_device *d);
	/* Unplugged: its resources are freed (and the device itself). */
	void (*destroy)(struct usb_device *d);
};

enum usb_ctl_stage { USB_CTL_IDLE, USB_CTL_DATA_IN, USB_CTL_DATA_OUT, USB_CTL_STATUS_IN, USB_CTL_STATUS_OUT };

struct usb_device {
	const struct usb_device_ops *ops;
	char name[96];          /* (for messages) */
	unsigned speed;         /* enum usb_speed: as it's presented to Mac OS */
	uint8_t addr;           /* its address on the bus (0 after a reset) */
	bool log;               /* print requests and transfers */
	/* endpoint 0's request in progress (usb_control()) */
	unsigned stage;         /* enum usb_ctl_stage */
	bool submitted;         /* ... handed to the device */
	bool waiting;           /* ... which hasn't answered yet */
	bool set_address;       /* ... is SET_ADDRESS (done here) */
	uint32_t pos;           /* data bytes moved so far */
	struct usb_xfer ctl;
	uint8_t ctl_buf[65536];
};

void usb_device_init(struct usb_device *d, const struct usb_device_ops *ops, unsigned speed, const char *name);

/*
 * A SETUP (8 bytes), IN or OUT transaction on endpoint 0, whose data stage
 * is up to `len` bytes at `buf` (`*actual`: those moved). Returns USB_OK,
 * USB_NAK (the request's answer hasn't come yet) or USB_STALL.
 */
int usb_control(struct usb_device *d, enum usb_pid pid, uint8_t *buf, uint32_t len, uint32_t *actual);

/* A bus reset: a request in progress is given up, and the address is 0. */
void usb_device_reset(struct usb_device *d);

/* Gives up endpoint 0's request in progress, if it's pending. */
void usb_control_cancel(struct usb_device *d);

/* For devices' own descriptors: string descriptor `s` (ASCII) into `out`; its length. */
uint32_t usb_string_descriptor(const char *s, uint8_t *out, uint32_t size);

static inline uint16_t usb_w16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

#endif
