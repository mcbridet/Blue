/*
 * Byte-order helpers. Mac OS data in guest memory is big-endian; PCI-side
 * structures (DBDMA registers and descriptors) are little-endian.
 */
#ifndef SYSHM_BE_H
#define SYSHM_BE_H

#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SYSHM_BSWAP16(x) (x)
#define SYSHM_BSWAP32(x) (x)
#define SYSHM_BSWAP64(x) (x)
#else
#define SYSHM_BSWAP16(x) __builtin_bswap16(x)
#define SYSHM_BSWAP32(x) __builtin_bswap32(x)
#define SYSHM_BSWAP64(x) __builtin_bswap64(x)
#endif

static inline uint16_t be16(const void *p)
{
	uint16_t v;
	memcpy(&v, p, sizeof v);
	return SYSHM_BSWAP16(v);
}

static inline uint32_t be32(const void *p)
{
	uint32_t v;
	memcpy(&v, p, sizeof v);
	return SYSHM_BSWAP32(v);
}

static inline uint64_t be64(const void *p)
{
	uint64_t v;
	memcpy(&v, p, sizeof v);
	return SYSHM_BSWAP64(v);
}

static inline void put_be16(void *p, uint16_t v)
{
	v = SYSHM_BSWAP16(v);
	memcpy(p, &v, sizeof v);
}

static inline void put_be32(void *p, uint32_t v)
{
	v = SYSHM_BSWAP32(v);
	memcpy(p, &v, sizeof v);
}

static inline void put_be64(void *p, uint64_t v)
{
	v = SYSHM_BSWAP64(v);
	memcpy(p, &v, sizeof v);
}

static inline uint32_t le32(const void *p)
{
	const uint8_t *b = p;
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static inline void put_le32(void *p, uint32_t v)
{
	uint8_t *b = p;
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

#endif
