/*
 * Blue's USB helper (macOS): a small program of its own, blue-usb-helper,
 * that Blue starts as root, with an administrator's password, to take the
 * USB devices macOS's own drivers have (which only root can). It does USB
 * and nothing else: it takes the devices Blue names, one at a time, with
 * usbhost.h (the same capture and the same handling of their requests as
 * Blue's own), and carries out their transfers, for as long as Blue's end
 * of the socket between them (usbproto.h) is open. Then, or at the first
 * request that isn't one, it gives them all back to macOS and ends.
 */
#ifndef BLUE_USBHELPER_H
#define BLUE_USBHELPER_H

#include <signal.h>

/* Serves Blue on socket `fd` until it closes it (0) or asks what it mayn't (1) */
int usbhelper_serve(int fd);

/* Set (by a signal's handler): the helper gives its devices back and ends */
extern volatile sig_atomic_t usbhelper_quit;

#endif
