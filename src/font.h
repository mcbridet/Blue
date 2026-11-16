/*
 * The window's font, for its menu, prompts and status bar: ChicagoFLF
 * (third_party/fonts), a TrueType revival of Mac OS's Chicago, kept in the
 * program: tools/fonts.py makes src/font_data.inc from it.
 */
#ifndef BLUE_FONT_H
#define BLUE_FONT_H

#include <stddef.h>
#include <stdint.h>

/* The TrueType file, unpacked (to free(); *size is its length); NULL if it doesn't unpack */
uint8_t *font_ttf(size_t *size);

/*
 * What a size (its height from ascent to descent, as Dear ImGui has it) is
 * scaled by to be the font's em: Chicago is drawn on a grid of 12 to the
 * em, so at 12 pixels to the em (24 on high-density displays) its strokes
 * are whole pixels, as Mac OS's 12-point Chicago was.
 */
extern const float font_em_scale;

#endif
