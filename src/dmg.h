/*
 * Compressed Mac disk images, read-only: UDIF (`.dmg`: a "koly" trailer, its
 * block table in an XML property list or, in early images (Disk Copy 6.3 to
 * Mac OS X 10.1), in 'blkx' resources of a resource fork appended to the data
 * fork, each partition's chunks then counted from its own data offset; chunks
 * raw, zeros, ADC, zlib, bzip2, LZFSE or xz) and Disk Copy 6's NDIF (`.img`:
 * the block table in the 'bcem' resource of its resource fork; chunks raw,
 * zeros or ADC). An image's resource fork is read from MacBinary or
 * AppleSingle, an AppleDouble file beside it (`._NAME`, or in
 * `.AppleDouble/`), or macOS's named fork; UDIF in MacBinary is read too.
 * Chunks are decompressed as they're read, the last few kept, so a big image
 * isn't unpacked into memory.
 */
#ifndef BLUE_DMG_H
#define BLUE_DMG_H

#include <stddef.h>
#include <stdint.h>

struct dmg;

/*
 * Opens an image of these kinds: 1 (and *out), 0 if the file isn't one (or
 * can't be opened), -1 if it is but can't be read here, with the reason in
 * `why` (encrypted, segmented, chunks of an unknown kind, an NDIF image's
 * resource fork damaged, ...).
 */
int dmg_open(const char *path, struct dmg **out, char *why, size_t len);
void dmg_close(struct dmg *d);

/* The image's bytes, `len` at `at` (a disk_backend's read): 0, or -1 */
int dmg_read(void *ctx, void *buf, size_t len, uint64_t at);

/* Its size in bytes, and what it is, for messages ("UDIF image (zlib)") */
uint64_t dmg_size(const struct dmg *d);
const char *dmg_kind(const struct dmg *d);

/* ADC (Apple Data Compression): bytes made into `out`, or -1 if the data is damaged or doesn't fit */
long dmg_adc(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

#endif
