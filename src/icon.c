#include "icon.h"

#include <stdlib.h>

#include "icon_data.inc"
#include "lzss.h"

bool icon_argb(uint32_t *out)
{
	const size_t n = (size_t)ICON_SIZE * ICON_SIZE;
	uint8_t *index = malloc(n);
	if (!index)
		return false;
	bool ok = lzss_decode(icon_lzss, sizeof icon_lzss, index, n) == n;
	for (size_t k = 0; k < n; k++) {
		const uint8_t *c = icon_palette + 4 * index[k];
		out[k] = (uint32_t)c[3] << 24 | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
	}
	free(index);
	return ok;
}
