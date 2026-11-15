/*
 * Blue's icon (art/blue.svg), for the window: on macOS SDL makes it the
 * Dock's too. tools/icons.py makes src/icon_data.inc from the master.
 */
#ifndef BLUE_ICON_H
#define BLUE_ICON_H

#include <stdbool.h>
#include <stdint.h>

#define ICON_SIZE 256u

/* Its ICON_SIZE x ICON_SIZE pixels, as 0xAARRGGBB (not premultiplied); false if they don't unpack */
bool icon_argb(uint32_t *out);

#endif
