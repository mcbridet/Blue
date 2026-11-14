#include <stdlib.h>
#include <string.h>

#include "../src/jit.h"
#include "../src/mem.h"
#include "../src/mmio.h"
#include "../src/ppc.h"
#include "ppcasm.h"
#include "test.h"

#define DEV 0x80000000u

struct fake {
	uint32_t last_off, last_val;
	unsigned last_size, reads, writes;
};

static uint32_t fake_read(void *ctx, uint32_t off, unsigned size)
{
	struct fake *f = ctx;
	f->reads++;
	f->last_off = off;
	f->last_size = size;
	return size == 4 ? 0x11223344u : size == 2 ? 0x8001u : 0xAB;
}

static void fake_write(void *ctx, uint32_t off, unsigned size, uint32_t v)
{
	struct fake *f = ctx;
	f->writes++;
	f->last_off = off;
	f->last_size = size;
	f->last_val = v;
}

static const struct mmio_ops fake_ops = { fake_read, fake_write };

/*
 * The mmio hook, as the kernel's would be: it does the access (at DEV +
 * 0x40, "posting an interrupt" too, stopping the run after it) or not (at
 * DEV + 0x80, which then faults as without it).
 */
struct hooked {
	struct mmio_bus bus;
	struct fake dev;
	unsigned long icount_sum;       /* (the instruction counts it saw) */
};

static int hook(void *ctx, struct ppc_cpu *cpu)
{
	struct hooked *h = ctx;
	struct mmio_access a;
	if (cpu->fault_addr - DEV == 0x80 || !mmio_decode(&h->bus, cpu, &a))
		return PPC_MMIO_NO;
	h->icount_sum += cpu->icount;
	mmio_perform(&a, cpu);
	if (cpu->fault_addr - DEV == 0x40) {
		cpu->cr ^= 0x00000F00u;
		return PPC_MMIO_STOP;
	}
	return PPC_MMIO_DONE;
}

/* A loop on device registers, interpreted or translated (and then translated again with the way to the hook) */
static void test_hook_run(bool jit)
{
	struct mem mem;
	struct ppc_cpu cpu;
	static struct hooked h;

	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		return;
	}
	mem_map(&mem, 0, 0x10000, MEM_RWX);
	memset(&h, 0, sizeof h);
	mmio_add(&h.bus, DEV, 0x1000, &fake_ops, &h.dev, "fake");

	const uint32_t prog[] = {
		LIS(3, DEV >> 16),
		LI(10, 100),
		MTCTR(10),
		LWZ(4, 0x10, 3),                /* loop: 0x11223344 */
		enc_d(38, 4, 3, 0x20),          /* stb */
		LHZ(5, 0x30, 3),                /* 0x8001 */
		ADDI(6, 6, 1),
		STW(6, 0x40, 3),                /* (the run stops after it) */
		BDNZ(-5 * 4),
		LWZ(7, 0x80, 3),                /* (faults) */
		SC,
	};
	for (size_t k = 0; k < sizeof prog / sizeof prog[0]; k++)
		mem_w32(&mem, 0x1000 + 4 * (uint32_t)k, prog[k]);
	ppc_init(&cpu, &mem);
	if (jit) {
		setenv("SYSHM_JIT_HOT", "0", 1);
		cpu.jit = jit_create(&mem);
		unsetenv("SYSHM_JIT_HOT");
		if (!cpu.jit) {
			mem_free(&mem);
			return;         /* no JIT on this host */
		}
	}
	cpu.pc = 0x1000;
	cpu.mmio = hook;
	cpu.mmio_ctx = &h;

	enum ppc_exit e;
	unsigned stops = 0, faults = 0;
	while ((e = ppc_run(&cpu, 1000)) == PPC_EXIT_MMIO || e == PPC_EXIT_DSI || e == PPC_EXIT_BUDGET) {
		if (e == PPC_EXIT_MMIO) {
			CHECK_EQ(cpu.pc, 0x1000 + 8 * 4);       /* (after the stw) */
			CHECK_EQ(cpu.exit_insn, STW(6, 0x40, 3));
			CHECK_EQ(cpu.cr, stops & 1 ? 0 : 0x00000F00u);
			stops++;
		} else if (e == PPC_EXIT_DSI) {
			CHECK_EQ(cpu.fault_addr, DEV + 0x80);
			CHECK_EQ(mmio_emulate(&h.bus, &cpu), MMIO_DONE);
			faults++;
		}
	}
	CHECK_EQ(e, PPC_EXIT_SC);
	CHECK_EQ(stops, 100);
	CHECK_EQ(faults, 1);
	CHECK_EQ(cpu.icount, 3 + 6 * 100 + 1);
	CHECK_EQ(cpu.tb, cpu.icount);
	CHECK_EQ(h.icount_sum, 100 * (3 + 4 + 5 + 7) + 4 * 6 * (100 * 99 / 2));        /* (the loop's four accesses) */
	CHECK_EQ(h.dev.reads, 201);
	CHECK_EQ(h.dev.writes, 200);
	CHECK_EQ(h.dev.last_val, 100);
	CHECK_EQ(cpu.gpr[4], 0x11223344);
	CHECK_EQ(cpu.gpr[5], 0x8001);
	CHECK_EQ(cpu.gpr[7], 0x11223344);
	jit_free(cpu.jit);
	mem_free(&mem);
}

static void test_hook(void)
{
	test_hook_run(false);
	test_hook_run(true);
}

void test_mmio(void)
{
	struct mem mem;
	struct ppc_cpu cpu;
	struct mmio_bus bus = { 0 };
	struct fake dev = { 0 };

	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		return;
	}
	mem_map(&mem, 0, 0x10000, MEM_RWX);
	mmio_add(&bus, DEV, 0x1000, &fake_ops, &dev, "fake");

	const uint32_t prog[] = {
		LIS(3, DEV >> 16),
		LWZ(4, 0x10, 3),                /* 0x11223344 */
		LBZ(5, 0x21, 3),                /* 0xAB */
		LHA(6, 0x30, 3),                /* 0xFFFF8001 */
		LWBRX(7, 0, 3),                 /* 0x44332211 */
		LI(8, 0x40),
		STW(4, 0x44, 3),
		STHBRX(4, 0, 3),
		LWZU(9, 0x80, 3),               /* r3 = DEV + 0x80 */
		SC,
	};
	for (size_t k = 0; k < sizeof prog / sizeof prog[0]; k++)
		mem_w32(&mem, 0x1000 + 4 * (uint32_t)k, prog[k]);
	ppc_init(&cpu, &mem);
	cpu.pc = 0x1000;

	enum ppc_exit e;
	unsigned faults = 0;
	while ((e = ppc_run(&cpu, 100)) == PPC_EXIT_DSI) {
		CHECK_EQ(mmio_emulate(&bus, &cpu), MMIO_DONE);
		if (dev.writes == 1 && faults == 4) {
			CHECK_EQ(dev.last_off, 0x44);
			CHECK_EQ(dev.last_val, 0x11223344);
		}
		if (dev.writes == 2 && faults == 5) {
			CHECK_EQ(dev.last_size, 2);
			CHECK_EQ(dev.last_val, 0x4433);         /* sthbrx of 0x3344 */
		}
		faults++;
	}
	CHECK_EQ(e, PPC_EXIT_SC);
	CHECK_EQ(faults, 7);
	CHECK_EQ(cpu.gpr[4], 0x11223344);
	CHECK_EQ(cpu.gpr[5], 0xAB);
	CHECK_EQ(cpu.gpr[6], 0xFFFF8001);
	CHECK_EQ(cpu.gpr[7], 0x44332211);
	CHECK_EQ(cpu.gpr[9], 0x11223344);
	CHECK_EQ(cpu.gpr[3], DEV + 0x80);
	CHECK_EQ(dev.reads, 5);

	mem_free(&mem);
	test_hook();
}
