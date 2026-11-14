#include "rsrc.h"

#include <string.h>

#include "be.h"

#define DATA_OFFSET 256u                /* header, then the reserved area for system use */
#define MAP_HEADER 28u

size_t rsrc_build(const struct rsrc *r, unsigned n, uint8_t *out, size_t size)
{
	unsigned ntypes = 0;
	size_t data_len = 0;
	for (unsigned k = 0; k < n; k++) {
		if (!k || r[k].type != r[k - 1].type)
			ntypes++;
		data_len += 4 + r[k].len;
	}
	const size_t map_off = DATA_OFFSET + data_len;
	const size_t type_list = MAP_HEADER;                    /* from the start of the map */
	const size_t refs = type_list + 2 + 8 * ntypes;
	const size_t map_len = refs + 12 * n;
	if (n == 0 || map_off + map_len > size)
		return 0;
	memset(out, 0, map_off + map_len);

	put_be32(out + 0, DATA_OFFSET);
	put_be32(out + 4, (uint32_t)map_off);
	put_be32(out + 8, (uint32_t)data_len);
	put_be32(out + 12, (uint32_t)map_len);

	uint8_t *map = out + map_off;
	memcpy(map, out, 16);                   /* copy of the header */
	put_be16(map + 24, (uint16_t)type_list);
	put_be16(map + 26, (uint16_t)map_len);  /* name list: empty, at the end */
	put_be16(map + type_list, (uint16_t)(ntypes - 1));

	size_t data = 0;
	unsigned t = 0;
	for (unsigned k = 0; k < n; k++) {
		if (!k || r[k].type != r[k - 1].type) {
			unsigned count = 1;
			while (k + count < n && r[k + count].type == r[k].type)
				count++;
			uint8_t *e = map + type_list + 2 + 8 * t++;
			put_be32(e, r[k].type);
			put_be16(e + 4, (uint16_t)(count - 1));
			put_be16(e + 6, (uint16_t)(refs + 12 * k - type_list));
		}
		uint8_t *ref = map + refs + 12 * k;
		put_be16(ref, (uint16_t)r[k].id);
		put_be16(ref + 2, 0xFFFF);              /* no name */
		put_be32(ref + 4, (uint32_t)data);      /* attributes 0, data offset */
		put_be32(out + DATA_OFFSET + data, r[k].len);
		memcpy(out + DATA_OFFSET + data + 4, r[k].data, r[k].len);
		data += 4 + r[k].len;
	}
	return map_off + map_len;
}
