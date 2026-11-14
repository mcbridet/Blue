/* Minimal PNG writer (8-bit RGB, stored deflate blocks; no external libraries). */
#ifndef SYSHM_PNG_H
#define SYSHM_PNG_H

#include <stdint.h>

/* Writes `rgb` (width * height * 3 bytes, rows top to bottom). Returns 0 on success. */
int png_write_rgb(const char *path, const uint8_t *rgb, unsigned width, unsigned height);

#endif
