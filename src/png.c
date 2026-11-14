#include "png.h"

#include <stdio.h>
#include <stdlib.h>

#include "be.h"

static uint32_t crc_table[256];

static void crc_init(void)
{
	for (uint32_t n = 0; n < 256; n++) {
		uint32_t c = n;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
	for (size_t k = 0; k < n; k++)
		crc = crc_table[(crc ^ p[k]) & 0xFF] ^ (crc >> 8);
	return crc;
}

static int chunk(FILE *f, const char type[4], const uint8_t *data, size_t len)
{
	uint8_t hdr[8];
	put_be32(hdr, (uint32_t)len);
	memcpy(hdr + 4, type, 4);
	uint32_t crc = crc_update(0xFFFFFFFFu, hdr + 4, 4);
	crc = crc_update(crc, data, len) ^ 0xFFFFFFFFu;
	uint8_t tail[4];
	put_be32(tail, crc);
	return fwrite(hdr, 1, 8, f) != 8 || (len && fwrite(data, 1, len, f) != len) ||
	       fwrite(tail, 1, 4, f) != 4;
}

int png_write_rgb(const char *path, const uint8_t *rgb, unsigned width, unsigned height)
{
	if (!crc_table[1])
		crc_init();

	/* Raw image: a filter byte (0) before each row */
	size_t row = (size_t)width * 3 + 1;
	size_t raw_len = row * height;

	/* zlib stream of stored deflate blocks (at most 65535 bytes each) */
	size_t blocks = raw_len / 65535 + 1;
	size_t z_len = 2 + raw_len + 5 * blocks + 4;
	uint8_t *z = malloc(z_len);
	if (!z)
		return -1;
	uint8_t *p = z;
	*p++ = 0x78;
	*p++ = 0x01;
	uint32_t a = 1, b = 0;
	size_t done = 0;
	for (size_t blk = 0; blk < blocks; blk++) {
		size_t n = raw_len - done < 65535 ? raw_len - done : 65535;
		*p++ = blk + 1 == blocks;
		*p++ = (uint8_t)n;
		*p++ = (uint8_t)(n >> 8);
		*p++ = (uint8_t)~n;
		*p++ = (uint8_t)(~n >> 8);
		for (size_t k = 0; k < n; k++, done++) {
			size_t y = done / row, x = done % row;
			uint8_t v = x == 0 ? 0 : rgb[y * width * 3 + x - 1];
			*p++ = v;
			a = (a + v) % 65521;
			b = (b + a) % 65521;
		}
	}
	put_be32(p, b << 16 | a);
	p += 4;

	FILE *f = fopen(path, "wb");
	if (!f) {
		free(z);
		return -1;
	}
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	uint8_t ihdr[13];
	put_be32(ihdr, width);
	put_be32(ihdr + 4, height);
	ihdr[8] = 8;            /* bit depth */
	ihdr[9] = 2;            /* RGB */
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	int err = fwrite(sig, 1, 8, f) != 8;
	err |= chunk(f, "IHDR", ihdr, sizeof ihdr);
	err |= chunk(f, "IDAT", z, (size_t)(p - z));
	err |= chunk(f, "IEND", NULL, 0);
	err |= fclose(f) != 0;
	free(z);
	return err ? -1 : 0;
}
