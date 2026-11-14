#include "lzss.h"

#include <string.h>

#define RING_SIZE 4096
#define RING_START 0xFEE
#define MIN_MATCH 3

size_t lzss_decode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen)
{
	uint8_t ring[RING_SIZE];
	unsigned r = RING_START;
	unsigned flags = 0;
	size_t in = 0, out = 0;

	memset(ring, 0, sizeof ring);

	while (out < dstlen) {
		flags >>= 1;
		if (!(flags & 0x100)) {
			if (in >= srclen)
				break;
			flags = src[in++] | 0xFF00;
		}

		if (flags & 1) {
			if (in >= srclen)
				break;
			uint8_t c = src[in++];
			dst[out++] = c;
			ring[r] = c;
			r = (r + 1) & (RING_SIZE - 1);
		} else {
			if (in + 1 >= srclen)
				break;
			unsigned lo = src[in++];
			unsigned hi = src[in++];
			unsigned pos = lo | ((hi & 0xF0) << 4);
			unsigned len = (hi & 0x0F) + MIN_MATCH;

			for (unsigned k = 0; k < len && out < dstlen; k++) {
				uint8_t c = ring[(pos + k) & (RING_SIZE - 1)];
				dst[out++] = c;
				ring[r] = c;
				r = (r + 1) & (RING_SIZE - 1);
			}
		}
	}

	return out;
}
