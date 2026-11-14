#include "pci.h"

#include <stdio.h>
#include <string.h>

#include "be.h"

void pci_init(struct pci_bus *b)
{
	memset(b, 0, sizeof *b);
}

static struct pci_function *new_function(struct pci_bus *b, uint8_t devfn)
{
	if (b->count >= PCI_MAX_FUNCTIONS)
		return NULL;
	struct pci_function *f = &b->fn[b->count++];
	memset(f, 0, sizeof *f);
	f->devfn = devfn;
	return f;
}

void pci_add(struct pci_bus *b, uint8_t devfn, uint16_t vendor, uint16_t device, uint32_t class_code,
	     uint8_t revision, uint32_t bar, uint32_t bar_size)
{
	struct pci_function *f = new_function(b, devfn);
	if (!f)
		return;
	uint8_t *c = f->config;
	c[0x00] = (uint8_t)vendor;
	c[0x01] = (uint8_t)(vendor >> 8);
	c[0x02] = (uint8_t)device;
	c[0x03] = (uint8_t)(device >> 8);
	c[0x04] = bar_size ? 0x06 : 0x00;               /* memory space and bus master, as firmware leaves them */
	c[0x06] = 0x80;                                 /* status: fast back-to-back, medium DEVSEL */
	c[0x07] = 0x02;
	put_le32(c + 0x08, class_code << 8 | revision);
	if (bar_size) {
		put_le32(c + 0x10, bar);
		f->bar_mask = ~(bar_size - 1);
	}
}

static struct pci_function *find(struct pci_bus *b, unsigned bus, unsigned devfn)
{
	if (bus != 0)
		return NULL;
	for (unsigned k = 0; k < b->count; k++)
		if (b->fn[k].devfn == devfn)
			return &b->fn[k];
	return NULL;
}

static uint32_t config_read(struct pci_bus *b, unsigned bus, unsigned devfn, unsigned reg, unsigned size)
{
	struct pci_function *f = find(b, bus, devfn);
	if (!f || reg + size > 256)
		return size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
	uint32_t v = 0;
	for (unsigned k = 0; k < size; k++)
		v |= (uint32_t)f->config[reg + k] << (8 * k);
	return v;
}

uint32_t pci_config_read(struct pci_bus *b, unsigned bus, unsigned devfn, unsigned reg, unsigned size)
{
	uint32_t v = config_read(b, bus, devfn, reg, size);
	if (b->log)
		printf("pci: read  %02X:%02X.%u +%02X (%u) -> %0*X\n", bus, devfn >> 3, devfn & 7, reg, size, 2 * size, v);
	return v;
}

void pci_config_write(struct pci_bus *b, unsigned bus, unsigned devfn, unsigned reg, unsigned size, uint32_t value)
{
	if (b->log)
		printf("pci: write %02X:%02X.%u +%02X (%u) <- %0*X\n", bus, devfn >> 3, devfn & 7, reg, size, 2 * size, value);
	struct pci_function *f = find(b, bus, devfn);
	if (!f || reg + size > 256)
		return;
	for (unsigned k = 0; k < size; k++) {
		unsigned r = reg + k;
		uint8_t v = (uint8_t)(value >> (8 * k));
		switch (r) {
		case 0x04: f->config[r] = v & 0x57; break;      /* command: I/O, memory, master, MWI, parity */
		case 0x05: f->config[r] = v & 0x01; break;      /* ... SERR */
		case 0x0C: case 0x0D: case 0x3C: f->config[r] = v; break;
		case 0x10: case 0x11: case 0x12: case 0x13:     /* BAR0 (sizes, but the device stays put) */
			f->config[r] = v & (uint8_t)(f->bar_mask >> (8 * (r - 0x10)));
			break;
		default: break;                                  /* read-only */
		}
	}
}

/* Both registers are little-endian: an access of `size` bytes at `offset`, in memory order. */
static uint32_t le_bytes(uint32_t v, uint32_t offset, unsigned size)
{
	uint32_t r = 0;
	for (unsigned k = 0; k < size; k++)
		r = r << 8 | ((v >> (8 * ((offset + k) & 3))) & 0xFF);
	return r;
}

static uint32_t addr_read(void *ctx, uint32_t offset, unsigned size)
{
	const struct pci_bus *b = ctx;
	return le_bytes(b->address, offset, size);
}

static void addr_write(void *ctx, uint32_t offset, unsigned size, uint32_t value)
{
	struct pci_bus *b = ctx;
	for (unsigned k = 0; k < size; k++) {
		unsigned shift = 8 * ((offset + k) & 3);
		b->address = (b->address & ~(0xFFu << shift)) |
			     ((value >> (8 * (size - 1 - k))) & 0xFF) << shift;
	}
}

/* The data register's bytes 0-3 are the addressed dword's. */
static uint32_t data_read(void *ctx, uint32_t offset, unsigned size)
{
	struct pci_bus *b = ctx;
	uint32_t a = b->address, reg = (a & 0xFC) + (offset & 3);
	if (!(a & 0x80000000u) || (offset & 3) + size > 4)
		return size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
	uint32_t v = pci_config_read(b, (a >> 16) & 0xFF, (a >> 8) & 0xFF, reg, size);
	return le_bytes(v << (8 * (offset & 3)), offset, size);
}

static void data_write(void *ctx, uint32_t offset, unsigned size, uint32_t value)
{
	struct pci_bus *b = ctx;
	uint32_t a = b->address, reg = (a & 0xFC) + (offset & 3);
	if (!(a & 0x80000000u) || (offset & 3) + size > 4)
		return;
	uint32_t v = 0;
	for (unsigned k = 0; k < size; k++)
		v |= ((value >> (8 * (size - 1 - k))) & 0xFF) << (8 * k);
	pci_config_write(b, (a >> 16) & 0xFF, (a >> 8) & 0xFF, reg, size, v);
}

const struct mmio_ops pci_addr_mmio_ops = { addr_read, addr_write };
const struct mmio_ops pci_data_mmio_ops = { data_read, data_write };
