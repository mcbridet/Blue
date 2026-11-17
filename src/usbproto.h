/*
 * The USB helper's protocol (macOS): how Blue (usbremote.h) and its helper
 * (usbhelper.h), which takes devices from macOS's own drivers as root,
 * talk over the socket they share. Each message is a 32-byte head, its
 * numbers little-endian, and then `data` bytes:
 *
 *   0 type, 1 dev, 2 ep, 3 xfer, 4 flags, 5-7 zero, 8 id, 12 len,
 *   16 status, 20 data, 24 setup (8 bytes)
 *
 * Blue asks; the helper answers. A transfer (SUBMIT) is usb.h's: its
 * answer (DONE) comes later, with the same id, unless Blue gives it up
 * first (CANCEL), resets the device or closes it. The helper refuses, and
 * stops, at anything that isn't exactly one of these.
 */
#ifndef BLUE_USBPROTO_H
#define BLUE_USBPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usbhost.h"

#define USBPROTO_VERSION 1
#define USBPROTO_HEAD 32
#define USBPROTO_DATA 65536u            /* the most data a message carries */
#define USBPROTO_DEVICES 4              /* devices the helper has at once */
#define USBPROTO_REQUESTS 64            /* transfers in flight on one of them */
#define USBPROTO_SPEC 32                /* room for OPEN's VID:PID or BUS-PORT[.PORT...] */
#define USBPROTO_WHY 400                /* room for why OPEN failed */
#define USBPROTO_ISO 1023               /* the largest isochronous packet */

enum usbproto_type {
	/* Blue to the helper */
	USBPROTO_OPEN = 1,              /* id; data: the device's spec */
	USBPROTO_SUBMIT,                /* dev, id, ep, xfer, len (to send, or room), setup (control), flags; data: out's */
	USBPROTO_CANCEL,                /* dev, id: given up (no answer is wanted) */
	USBPROTO_RESET,                 /* dev: a bus reset (its transfers in flight are given up) */
	USBPROTO_CLOSE,                 /* dev: unplugged from the Mac (given back to macOS) */
	/* the helper to Blue */
	USBPROTO_HELLO = 16,            /* status: USBPROTO_VERSION; id: its process */
	USBPROTO_OPENED,                /* id: OPEN's; status 0: dev, and data: usbproto_put_info()'s; else data: why not */
	USBPROTO_DONE,                  /* dev, id; status: enum usb_status, len: bytes moved; data: those in */
	USBPROTO_GONE,                  /* dev: unplugged from the host */
};

#define USBPROTO_LOG 1u                 /* (SUBMIT's flags) the device's requests and transfers are printed */

struct usbproto_msg {
	uint8_t type, dev, ep, xfer, flags;
	uint32_t id, len, status, data;
	uint8_t setup[8];
};

void usbproto_put(uint8_t out[USBPROTO_HEAD], const struct usbproto_msg *m);
/* false if the head isn't one (its unused bytes aren't zero, or its data is too long) */
bool usbproto_get(struct usbproto_msg *m, const uint8_t in[USBPROTO_HEAD]);

/* NULL if Blue may ask this (`data`: its data), or why not */
const char *usbproto_check_request(const struct usbproto_msg *m, const uint8_t *data);
/* NULL if it's an answer the helper may give, or why not */
const char *usbproto_check_answer(const struct usbproto_msg *m);

/* A whole message; false if the other end has gone */
bool usbproto_send(int fd, const struct usbproto_msg *m, const void *data);
/* The next message, its data into `data` (USBPROTO_DATA bytes): 1; 0 at the end; -1 if its head isn't one */
int usbproto_recv(int fd, struct usbproto_msg *m, uint8_t *data);

/* OPENED's data: what the host says of the device, how fast it is, and its name for messages */
#define USBPROTO_INFO 256
void usbproto_put_info(uint8_t out[USBPROTO_INFO], const struct usbhost_info *info, unsigned speed, const char *name);
void usbproto_get_info(const uint8_t in[USBPROTO_INFO], struct usbhost_info *info, unsigned *speed, char *name,
		       size_t size);

#endif
