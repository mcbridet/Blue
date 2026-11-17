/*
 * The host's USB devices that macOS's own drivers have, taken through
 * Blue's USB helper (usbhelper.h, macOS): only root can take those from
 * macOS, so a small program of Blue's does it as root, started with an
 * administrator's password (macOS's own dialog) when first wanted, and
 * kept until Blue quits. Blue stays an ordinary process: the helper takes
 * the devices it names and carries out their transfers, which come and go
 * over the socket between them (usbproto.h). A device taken this way is
 * like one usbhost.h takes itself: usbhost.c, in the helper, does the work.
 *
 * Elsewhere (Linux) there's no helper: a udev rule gives Blue its devices.
 */
#ifndef BLUE_USBREMOTE_H
#define BLUE_USBREMOTE_H

#include <stdbool.h>
#include <stddef.h>

#include "usb.h"
#include "usbhost.h"

struct usbremote;

enum usbremote_state {
	USBREMOTE_ASKING,               /* waiting for the password (and the helper) */
	USBREMOTE_READY,                /* the helper's there */
	USBREMOTE_FAILED,               /* it isn't: the password wasn't given, or the helper stopped */
};

#ifdef __APPLE__

/*
 * Starts the helper (blue-usb-helper, beside Blue's executable), asking for
 * an administrator's password: it's asked for on a thread of its own, so
 * this returns at once (NULL, with `err`, only if it can't even ask).
 * BLUE_USB_HELPER_TEST runs it as the user instead, without asking (tests).
 */
struct usbremote *usbremote_start(char *err, size_t size);

/* A helper already running, at the other end of socket `fd` (tests); NULL if it doesn't say hello */
struct usbremote *usbremote_connect(int fd, char *err, size_t size);

/* Where it is; why it failed, in a sentence, into `why` */
enum usbremote_state usbremote_state(struct usbremote *r, char *why, size_t size);

/* Stops the helper (its devices destroyed first), which gives back what it had to macOS */
void usbremote_stop(struct usbremote *r);

/* Takes the device `spec` names (as usbhost_open()'s), through the helper; NULL with `err` saying why not */
struct usb_device *usbremote_open(struct usbremote *r, const char *spec, char *err, size_t size);

bool usbremote_is(const struct usb_device *d);
const struct usbhost_info *usbremote_device_info(const struct usb_device *d);
/* Whether it's gone from the host (unplugged), or the helper has */
bool usbremote_gone(const struct usb_device *d);

#else

static inline struct usbremote *usbremote_start(char *err, size_t size)
{
	(void)size;
	*err = 0;
	return NULL;
}

static inline enum usbremote_state usbremote_state(struct usbremote *r, char *why, size_t size)
{
	(void)r, (void)why, (void)size;
	return USBREMOTE_FAILED;
}

static inline void usbremote_stop(struct usbremote *r)
{
	(void)r;
}

static inline struct usb_device *usbremote_open(struct usbremote *r, const char *spec, char *err, size_t size)
{
	(void)r, (void)spec, (void)size;
	*err = 0;
	return NULL;
}

static inline bool usbremote_is(const struct usb_device *d)
{
	(void)d;
	return false;
}

static inline const struct usbhost_info *usbremote_device_info(const struct usb_device *d)
{
	(void)d;
	return NULL;
}

static inline bool usbremote_gone(const struct usb_device *d)
{
	(void)d;
	return false;
}

#endif

#endif
