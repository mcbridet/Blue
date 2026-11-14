#include <string.h>

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
}
