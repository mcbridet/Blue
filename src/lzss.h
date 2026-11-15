#ifndef BLUE_LZSS_H
#define BLUE_LZSS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Decode the LZSS variant used for the Toolbox image in NewWorld "Mac OS ROM"
 * files (Okumura's LZSS: 4 KB ring buffer starting at 0xFEE, one flag byte per
 * eight items read LSB first, 1 = literal byte, 0 = 2-byte back-reference with
 * a 12-bit ring position and a 4-bit length minus 3).
 *
 * Returns the number of bytes written to dst. Decoding stops when the input is
 * exhausted or dst is full.
 */
size_t lzss_decode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);

#endif
