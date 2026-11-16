#include "font.h"

#include <stdlib.h>

#include "font_data.inc"
#include "lzss.h"

const float font_em_scale = FONT_EM_SCALE;

uint8_t *font_ttf(size_t *size)
{
	uint8_t *ttf = malloc(FONT_TTF_SIZE);
	if (ttf && lzss_decode(font_lzss, sizeof font_lzss, ttf, FONT_TTF_SIZE) != FONT_TTF_SIZE) {
		free(ttf);
		ttf = NULL;
	}
	*size = ttf ? FONT_TTF_SIZE : 0;
	return ttf;
}
