/*
 * The built-in USB test disk: a USB mass storage device (bulk-only
 * transport, SCSI transparent command set) whose medium is an image file,
 * read and written in 512-byte blocks. It answers at once, so runs that
 * use it in instruction time are reproducible; it's for testing the USB
 * controller through Mac OS's own USB stack, and for debugging.
 *
 * It's removable (START STOP UNIT ejects the medium) and has one LUN, and
 * it takes INQUIRY, READ CAPACITY, READ FORMAT CAPACITIES, READ and WRITE
 * (6 and 10), VERIFY, TEST UNIT READY, REQUEST SENSE, MODE SENSE (6 and 10),
 * PREVENT/ALLOW MEDIUM REMOVAL, START STOP UNIT and SYNCHRONIZE CACHE.
 */
#ifndef BLUE_USBMSD_H
#define BLUE_USBMSD_H

#include <stdbool.h>
#include <stddef.h>

#include "usb.h"

#define USBMSD_VENDOR 0x1209u           /* pid.codes' test ID */
#define USBMSD_PRODUCT 0x0001u

/* A test disk for this image (read-only if it can't be written, or if asked); NULL, with `err` saying why. */
struct usb_device *usbmsd_open(const char *path, bool read_only, char *err, size_t size);

/* Whether a device is a test disk */
bool usbmsd_is(const struct usb_device *d);

/* Whether Mac OS has ejected its medium (START STOP UNIT) */
bool usbmsd_ejected(const struct usb_device *d);

#endif
