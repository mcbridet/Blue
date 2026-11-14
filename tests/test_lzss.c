#include <stdlib.h>
#include <string.h>

#include "../src/lzss.h"
#include "test.h"

/* Greedy encoder producing the same format the decoder expects. */
static size_t encode(const uint8_t *in, size_t n, uint8_t *out)
{
	size_t o = 0, p = 0;

	while (p < n) {
		size_t flag_pos = o++;
		uint8_t flags = 0;

		for (int bit = 0; bit < 8 && p < n; bit++) {
			size_t best_len = 0, best_src = 0;
			size_t lo = p > 4078 ? p - 4078 : 0;
			for (size_t s = lo; s < p; s++) {
				size_t len = 0;
				while (len < 18 && s + len < p && p + len < n && in[s + len] == in[p + len])
					len++;
				if (len > best_len) {
					best_len = len;
					best_src = s;
				}
			}
			if (best_len >= 3) {
				unsigned pos = (0xFEE + best_src) & 0xFFF;
				out[o++] = (uint8_t)pos;
				out[o++] = (uint8_t)(((pos >> 4) & 0xF0) | (best_len - 3));
				p += best_len;
			} else {
				flags |= 1u << bit;
				out[o++] = in[p++];
			}
		}
		out[flag_pos] = flags;
	}
	return o;
}

static void roundtrip(const uint8_t *data, size_t n)
{
	uint8_t *enc = malloc(n * 2 + 16), *dec = malloc(n + 16);
	size_t elen = encode(data, n, enc);
	size_t dlen = lzss_decode(enc, elen, dec, n);

	CHECK_EQ(dlen, n);
	CHECK(memcmp(dec, data, n) == 0);
	free(enc);
	free(dec);
}

void test_lzss(void)
{
	static uint8_t buf[20000];

	for (size_t k = 0; k < sizeof buf; k++)
		buf[k] = (uint8_t)("Macintosh "[k % 10] + (k / 997));
	roundtrip(buf, sizeof buf);

	srand(1);
	for (size_t k = 0; k < sizeof buf; k++)
		buf[k] = (uint8_t)(rand() % 7);
	roundtrip(buf, sizeof buf);

	memset(buf, 0, sizeof buf);
	roundtrip(buf, 5000);
	roundtrip((const uint8_t *)"ab", 2);
}
