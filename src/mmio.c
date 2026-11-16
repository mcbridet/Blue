#include "mmio.h"

#include <stdio.h>

void mmio_add(struct mmio_bus *bus, uint32_t base, uint32_t size, const struct mmio_ops *ops,
	      void *ctx, const char *name)
{
	if (bus->count >= MMIO_MAX_REGIONS) {
		fprintf(stderr, "mmio: too many regions (%s)\n", name);
		return;
	}
	bus->region[bus->count++] = (struct mmio_region){ .base = base, .size = size, .ops = ops, .ctx = ctx, .name = name };
}

void mmio_pure_reads(struct mmio_bus *bus, bool (*pure_read)(const void *ctx, uint32_t offset))
{
	if (bus->count)
		bus->region[bus->count - 1].pure_read = pure_read;
}

const struct mmio_region *mmio_find(const struct mmio_bus *bus, uint32_t addr)
{
	for (int k = 0; k < bus->count; k++)
		if (addr - bus->region[k].base < bus->region[k].size)
			return &bus->region[k];
	return NULL;
}

static bool decode(uint32_t i, struct mmio_access *a)
{
	unsigned op = i >> 26;

	*a = (struct mmio_access){ .region = a->region, .rt = (i >> 21) & 31, .ra = (i >> 16) & 31 };
	switch (op) {
	case 32: case 33: a->size = 4; break;                           /* lwz lwzu */
	case 34: case 35: a->size = 1; break;                           /* lbz lbzu */
	case 40: case 41: a->size = 2; break;                           /* lhz lhzu */
	case 42: case 43: a->size = 2; a->sign = true; break;           /* lha lhau */
	case 36: case 37: a->size = 4; a->store = true; break;          /* stw stwu */
	case 38: case 39: a->size = 1; a->store = true; break;          /* stb stbu */
	case 44: case 45: a->size = 2; a->store = true; break;          /* sth sthu */
	case 31:
		switch ((i >> 1) & 0x3FF) {
		case 23: case 55: a->size = 4; break;                   /* lwzx lwzux */
		case 87: case 119: a->size = 1; break;                  /* lbzx lbzux */
		case 279: case 311: a->size = 2; break;                 /* lhzx lhzux */
		case 343: case 375: a->size = 2; a->sign = true; break; /* lhax lhaux */
		case 151: case 183: a->size = 4; a->store = true; break; /* stwx stwux */
		case 215: case 247: a->size = 1; a->store = true; break; /* stbx stbux */
		case 407: case 439: a->size = 2; a->store = true; break; /* sthx sthux */
		case 534: a->size = 4; a->reverse = true; break;        /* lwbrx */
		case 790: a->size = 2; a->reverse = true; break;        /* lhbrx */
		case 662: a->size = 4; a->reverse = a->store = true; break; /* stwbrx */
		case 918: a->size = 2; a->reverse = a->store = true; break; /* sthbrx */
		default:
			return false;
		}
		/* update forms have bit 5 of the extended opcode set (lwzux = 55 = 23 + 32) */
		a->update = !a->reverse && (((i >> 1) & 0x3FF) & 32);
		return true;
	default:
		return false;
	}
	a->update = op & 1;
	return true;
}

static uint32_t swap(uint32_t v, unsigned size)
{
	return size == 4 ? __builtin_bswap32(v) : size == 2 ? __builtin_bswap16((uint16_t)v) : v;
}

bool mmio_decode(const struct mmio_bus *bus, const struct ppc_cpu *cpu, struct mmio_access *a)
{
	a->region = mmio_find(bus, cpu->fault_addr);
	return a->region && decode(cpu->exit_insn, a) && a->store == cpu->fault_write;
}

void mmio_perform(const struct mmio_access *a, struct ppc_cpu *cpu)
{
	const struct mmio_region *r = a->region;
	uint32_t ea = cpu->fault_addr, off = ea - r->base;
	if (a->store) {
		uint32_t v = cpu->gpr[a->rt];
		if (a->size < 4)
			v &= (1u << (8 * a->size)) - 1;
		r->ops->write(r->ctx, off, a->size, a->reverse ? swap(v, a->size) : v);
	} else {
		uint32_t v = r->ops->read(r->ctx, off, a->size);
		if (a->reverse)
			v = swap(v, a->size);
		if (a->sign)
			v = (uint32_t)(int32_t)(int16_t)v;
		cpu->gpr[a->rt] = v;
	}
	if (a->update)
		cpu->gpr[a->ra] = ea;
	cpu->pc += 4;
	cpu->icount++;
	cpu->tb += cpu->tb_step;
}

enum mmio_result mmio_emulate(const struct mmio_bus *bus, struct ppc_cpu *cpu)
{
	struct mmio_access a;
	if (!mmio_decode(bus, cpu, &a))
		return a.region ? MMIO_BAD_INSN : MMIO_NOT_DEVICE;
	mmio_perform(&a, cpu);
	return MMIO_DONE;
}
