/*
 * The host's USB devices, passed through to Mac OS (in real time only):
 * a device taken from the host is plugged into one of the USB
 * controller's ports (ohci.h), and Mac OS's own drivers, or the maker's
 * installed in Mac OS, drive it. Blue only moves its transfers.
 *
 * libusb-1.0 does the work; it's loaded at run time (hostlib.h), so Blue
 * needs it neither to build nor to start. Its transfers go asynchronously,
 * finished on a thread of its own that handles its events; the answers
 * reach the controller through usb.h's transfers (`done`).
 *
 * Taking a device claims all its interfaces, first detaching the host's
 * drivers from them where the host allows it (Linux: the device's node
 * writable; macOS: root, which captures the whole device, or the USB
 * helper, usbremote.h, which does it as root for Blue). Mac OS's
 * SET_CONFIGURATION and SET_INTERFACE are carried out on the device, so
 * that the host's view of it stays the same, as are CLEAR_FEATURE
 * (ENDPOINT_HALT) and, once Mac OS has configured it, bus resets. The
 * iMac's USB is 1.1: a device connected to the host at high speed (USB 2.0)
 * or faster isn't taken.
 */
#ifndef BLUE_USBHOST_H
#define BLUE_USBHOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usb.h"

struct usbhost;

struct usbhost_info {
	char id[24];                    /* where it's plugged in: "BUS-PORT[.PORT...]", as Linux names it */
	uint16_t vendor, product;
	unsigned speed;                 /* enum usb_speed */
	char name[128];                 /* the maker's and product's names, as far as the host says ("": not) */
	bool held;                      /* (macOS) its own driver has it: only root can take it */
};

/* libusb, loaded and started; NULL with `err` saying why */
struct usbhost *usbhost_start(char *err, size_t size);
void usbhost_stop(struct usbhost *h);   /* (devices taken must have been destroyed) */

/* The host's devices, hubs left out; how many there are (up to `max` are given) */
unsigned usbhost_list(struct usbhost *h, struct usbhost_info *out, unsigned max);

/*
 * Takes the device `spec` names (VID:PID in hex, or BUS-PORT[.PORT...]) for
 * Mac OS; NULL with `err` saying why not (in a sentence for the menu), and
 * `*held` (if not NULL) set if that's because the host's own driver has it
 * and only root could take it from it (macOS).
 */
struct usb_device *usbhost_open(struct usbhost *h, const char *spec, bool *held, char *err, size_t size);

bool usbhost_is(const struct usb_device *d);
const struct usbhost_info *usbhost_device_info(const struct usb_device *d);

/* Whether a device taken has gone from the host (unplugged) */
bool usbhost_gone(const struct usb_device *d);

/* A device plugged into the host since last asked; false if none was */
bool usbhost_arrived(struct usbhost *h, struct usbhost_info *info);

/* Calls fn(arg), on libusb's thread, whenever a transfer's answer has come or a device taken has gone */
void usbhost_notify(struct usbhost *h, void (*fn)(void *), void *arg);

/* Whether `spec` (as for usbhost_open()) names this device */
bool usbhost_matches(const struct usbhost_info *info, const char *spec);

#endif
