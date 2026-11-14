/*
 * Memory-mapped device emulation. Device pages are left unmapped, so guest
 * accesses exit the interpreter with a DSI; the kernel then decodes the
 * faulting load/store and forwards it to the device that owns the address.
 */
#ifndef SYSHM_MMIO_H
#define SYSHM_MMIO_H

#include <stdbool.h>
#include <stdint.h>

#include "ppc.h"

struct mmio_ops {
	/* Big-endian semantics: a 2- or 4-byte access at `offset` reads/writes that many bytes. */
	uint32_t (*read)(void *ctx, uint32_t offset, unsigned size);
	void (*write)(void *ctx, uint32_t offset, unsigned size, uint32_t value);
};

struct mmio_region {
	uint32_t base, size;
	const struct mmio_ops *ops;
	void *ctx;
	const char *name;
};

#define MMIO_MAX_REGIONS 16

struct mmio_bus {
	struct mmio_region region[MMIO_MAX_REGIONS];
	int count;
};

void mmio_add(struct mmio_bus *bus, uint32_t base, uint32_t size, const struct mmio_ops *ops,
	      void *ctx, const char *name);
const struct mmio_region *mmio_find(const struct mmio_bus *bus, uint32_t addr);

enum mmio_result {
	MMIO_DONE,              /* access performed, pc advanced */
	MMIO_NOT_DEVICE,        /* address isn't a device register */
	MMIO_BAD_INSN,          /* an access form we don't emulate (string, multiple, FP, ...) */
};

/* Emulates the load/store that just faulted (cpu->exit_insn at cpu->fault_addr). */
enum mmio_result mmio_emulate(const struct mmio_bus *bus, struct ppc_cpu *cpu);

/* ... in two steps: decoded, then done */
struct mmio_access {
	const struct mmio_region *region;       /* NULL: not a device register */
	bool store, update, reverse, sign;
	unsigned size, rt, ra;
};

/* false (changing nothing) if mmio_emulate() wouldn't return MMIO_DONE */
bool mmio_decode(const struct mmio_bus *bus, const struct ppc_cpu *cpu, struct mmio_access *a);
void mmio_perform(const struct mmio_access *a, struct ppc_cpu *cpu);

#endif
