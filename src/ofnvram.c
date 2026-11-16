#include "ofnvram.h"

#include <stdlib.h>
#include <string.h>

#define HEADER 16u
#define COMMON_NAME "common"

static uint8_t checksum(const uint8_t *h)
{
	unsigned sum = h[0];
	for (unsigned k = 2; k < HEADER; k++)
		sum += h[k];
	while (sum > 0xFFu)
		sum = (sum & 0xFFu) + (sum >> 8);
	return (uint8_t)sum;
}

bool ofnvram_common(const uint8_t *nv, size_t size, size_t *data, size_t *len)
{
	for (size_t at = 0; at + HEADER <= size;) {
		const uint8_t *h = nv + at;
		size_t blocks = (size_t)h[2] << 8 | h[3];
		if (h[1] != checksum(h) || !blocks || at + 16 * blocks > size)
			return false;
		if (h[0] == OFNVRAM_COMMON_SIG && !memcmp(h + 4, COMMON_NAME, sizeof COMMON_NAME)) {
			*data = at + HEADER;
			*len = 16 * blocks - HEADER;
			return true;
		}
		at += 16 * blocks;
	}
	return false;
}

bool ofnvram_format(uint8_t *nv, size_t size)
{
	size_t data, len;
	if (size < OFNVRAM_COMMON_SIZE || ofnvram_common(nv, size, &data, &len))
		return false;
	for (size_t k = 0; k < OFNVRAM_COMMON_SIZE; k++)
		if (nv[k])
			return false;
	nv[0] = OFNVRAM_COMMON_SIG;
	nv[2] = (uint8_t)(OFNVRAM_COMMON_SIZE / 16 >> 8);
	nv[3] = (uint8_t)(OFNVRAM_COMMON_SIZE / 16);
	memcpy(nv + 4, COMMON_NAME, sizeof COMMON_NAME);
	nv[1] = checksum(nv);
	return true;
}

/* (strings until an empty one ends them; empty ones before the first don't: Mac OS's own writer leaves one there) */
bool ofnvram_get(const uint8_t *nv, size_t size, const char *name, char *out, size_t len)
{
	size_t data, n, name_len = strlen(name);
	if (!len || !ofnvram_common(nv, size, &data, &n))
		return false;
	const char *s = (const char *)nv + data, *end = s + n;
	bool any = false;
	while (s < end) {
		size_t k = strnlen(s, (size_t)(end - s));
		if (!k && any)
			break;
		any |= k > 0;
		if (k > name_len && !memcmp(s, name, name_len) && s[name_len] == '=') {
			size_t v = k - name_len - 1;
			v = v < len - 1 ? v : len - 1;
			memcpy(out, s + name_len + 1, v);
			out[v] = '\0';
			return true;
		}
		s += k + 1;
	}
	return false;
}

bool ofnvram_disk_path(const char *path, unsigned *pci_dev, unsigned *map_entry)
{
	static const char node[] = "/blue,disk@";
	const char *p = strstr(path, node);
	char *end;
	if (!p)
		return false;
	p += sizeof node - 1;
	unsigned long dev = strtoul(p, &end, 16);
	if (end == p || dev > 31)
		return false;
	/* (a unit after the node's address: Mac OS adds the drive's, its SCSI target) */
	for (p = end; *p == '@' || (*p == '/' && p[1] == '@');) {
		p += *p == '/' ? 2 : 1;
		strtoul(p, &end, 16);
		if (end == p)
			return false;
		p = end;
	}
	unsigned long entry = 0;
	if (*p == ':') {
		p++;
		if (*p >= '0' && *p <= '9') {
			entry = strtoul(p, &end, 10);
			p = end;
		}
	}
	if (*p && *p != ',')
		return false;
	*pci_dev = (unsigned)dev;
	*map_entry = (unsigned)entry;
	return true;
}
