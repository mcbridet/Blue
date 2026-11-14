#include "nvram.h"

#include <string.h>

void nvram_init(struct nvram *n, uint8_t *bytes, uint32_t size)
{
	memset(n, 0, sizeof *n);
	n->bytes = bytes;
	n->size = size;
}

static uint8_t *byte_at(struct nvram *n, uint32_t offset)
{
	return &n->bytes[(offset / NVRAM_STRIDE) & (n->size - 1)];
}

static uint32_t nvram_read(void *ctx, uint32_t offset, unsigned size)
{
	struct nvram *n = ctx;
	uint32_t v = 0;
	for (unsigned k = 0; k < size; k++)
		v = v << 8 | *byte_at(n, offset + k);
	n->reads++;
	return v;
}

static void nvram_write(void *ctx, uint32_t offset, unsigned size, uint32_t value)
{
	struct nvram *n = ctx;
	for (unsigned k = 0; k < size; k++)
		*byte_at(n, offset + k) = (uint8_t)(value >> 8 * (size - 1 - k));
	n->writes++;
}

const struct mmio_ops nvram_mmio_ops = { nvram_read, nvram_write };
