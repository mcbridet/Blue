/*
 * The pixel density of the host's displays, for the window's scale (Auto:
 * display.c). A display's physical size comes from CoreGraphics on macOS
 * (from its EDID); elsewhere from the kernel's copy of its EDID (the
 * X server's and the compositor's come from there too).
 */
#ifndef BLUE_DENSITY_H
#define BLUE_DENSITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Pixels per inch across a display px_w x px_h pixels and mm_w x mm_h
 * millimetres (or the other way round: turned), or 0 if that isn't a
 * plausible display's (pixels far from square, or a size made up, as the
 * 16 x 9 cm some televisions give for their shape).
 */
double density_from_size(unsigned px_w, unsigned px_h, double mm_w, double mm_h);

/*
 * The size an EDID gives: the pixels and millimetres of its first
 * detailed timing (the display's own), or the centimetres of its basic
 * parameters. False if it gives none.
 */
bool density_edid(const uint8_t *edid, size_t size, unsigned *px_w, unsigned *px_h, double *mm_w, double *mm_h);

/*
 * The density of the display at (x, y), a point on it in the window
 * system's units, px_w x px_h pixels as the window draws on it (macOS asks
 * CoreGraphics instead): 0 if it isn't known. Elsewhere it's the kernel's
 * EDID of the display connected, or of the one of that size among several.
 */
double density_display(int x, int y, unsigned px_w, unsigned px_h);

#endif
