#include <string.h>

#include "../src/mem.h"
#include "../src/ppc.h"
#include "../src/priv.h"
#include "ppcasm.h"
#include "test.h"

#define CODE 0x1000u
#define SUB 0x2000u
#define RAM_SIZE 0x10000u
#define PVR 0x00080301u

#define MTMSR(rs)            enc_x(31, rs, 0, 0, 146, 0)
#define MFSR(rt, sr)         enc_x(31, rt, sr, 0, 595, 0)
#define MTSR(sr, rs)         enc_x(31, rs, sr, 0, 210, 0)
#define MFSRIN(rt, rb)       enc_x(31, rt, 0, rb, 659, 0)
#define MTSRIN(rs, rb)       enc_x(31, rs, 0, rb, 242, 0)
#define TLBIE(rb)            enc_x(31, 0, 0, rb, 306, 0)
#define TLBIA                enc_x(31, 0, 0, 0, 370, 0)
#define RFI                  0x4C000064u
#define ISYNC                0x4C00012Cu
#define BLRL                 0x4E800021u
#define MFLR(rt)             MFSPR(rt, 8)
#define MTLR(rs)             MTSPR(8, rs)

static struct priv p;
static struct ppc_cpu cpu;

static void start(void)
{
	uint32_t bat[16], sr[16];
	for (unsigned k = 0; k < 16; k++) {
		bat[k] = 0x100u + k;
		sr[k] = 0x20000000u + k;
	}
	priv_init(&p, PVR, 0x07F0000Fu, bat, sr);
	memset(&cpu, 0, sizeof cpu);
	cpu.pc = CODE;
}

/* The privileged instruction `insn` at CODE: whether it was done */
static bool exec(uint32_t insn)
{
	cpu.pc = CODE;
	cpu.exit_insn = insn;
	return priv_insn(&p, &cpu);
}

static void enter(void)
{
	cpu.gpr[3] = PRIV_GARY;
	cpu.gpr[4] = PRIV_KEY;
	cpu.gpr[5] = 0xC010;            /* EE PR DR off */
	cpu.gpr[7] = 0;
	CHECK(priv_skeleton_key(&p, cpu.gpr));
	CHECK(priv_supervisor(&p));
	CHECK_EQ(p.msr, 0x1022);
}

static uint32_t mfspr(unsigned spr)
{
	CHECK(exec(MFSPR(9, spr)));
	return cpu.gpr[9];
}

static void mtspr(unsigned spr, uint32_t v)
{
	cpu.gpr[9] = v;
	CHECK(exec(MTSPR(spr, 9)));
}

static void test_user(void)
{
	start();
	CHECK(!priv_supervisor(&p));
	CHECK(exec(MFMSR(5)));
	CHECK_EQ(cpu.gpr[5], 0xD032);
	CHECK_EQ(cpu.pc, CODE + 4);
	CHECK(!exec(MTMSR(5)));
	CHECK_EQ(cpu.pc, CODE);
	CHECK(!exec(MFSPR(3, SPR_PVR)));
	CHECK(!exec(RFI));

	/* the key must be right */
	cpu.gpr[3] = PRIV_GARY;
	cpu.gpr[4] = PRIV_KEY + 1;
	cpu.gpr[5] = 0xC010;
	CHECK(!priv_skeleton_key(&p, cpu.gpr));
	cpu.gpr[3] = 0x47617278u;
	cpu.gpr[4] = PRIV_KEY;
	CHECK(!priv_skeleton_key(&p, cpu.gpr));
	CHECK_EQ(p.msr, PRIV_USER_MSR);
	CHECK_EQ(p.keys, 0);

	/* bits set too */
	start();
	cpu.gpr[3] = PRIV_GARY;
	cpu.gpr[4] = PRIV_KEY;
	cpu.gpr[5] = 0x4000;
	cpu.gpr[7] = 0x2000;            /* FP */
	CHECK(priv_skeleton_key(&p, cpu.gpr));
	CHECK_EQ(p.msr, 0xB032);
	CHECK_EQ(p.keys, 1);
}

static void test_registers(void)
{
	start();
	enter();
	CHECK_EQ(mfspr(SPR_PVR), PVR);
	CHECK_EQ(mfspr(SPR_HID0), PRIV_HID0);
	CHECK_EQ(mfspr(SPR_HID1) >> 28, 0xA);   /* 4x */
	CHECK_EQ(mfspr(SPR_L2CR), PRIV_L2CR);
	CHECK_EQ(mfspr(SPR_SDR1), 0x07F0000F);
	CHECK_EQ(mfspr(SPR_IBAT0U), 0x100);
	CHECK_EQ(mfspr(SPR_IBAT0U + 15), 0x10F);   /* DBAT3L */

	/* kept, read-only, or without effect */
	mtspr(SPR_SPRG0 + 2, 0x12345678);
	CHECK_EQ(mfspr(SPR_SPRG0 + 2), 0x12345678);
	mtspr(SPR_HID0, 0x80000000);
	CHECK_EQ(mfspr(SPR_HID0), 0x80000000);
	mtspr(SPR_PVR, 0);
	CHECK_EQ(mfspr(SPR_PVR), PVR);
	mtspr(SPR_HID1, 0);
	CHECK_EQ(mfspr(SPR_HID1), PRIV_HID1);
	mtspr(SPR_TBL_W, 0);
	CHECK(!exec(MFSPR(3, SPR_TBL_W)));
	mtspr(SPR_ICTC, 0x1F);
	CHECK_EQ(mfspr(SPR_ICTC), 0x1F);
	mtspr(SPR_MMCR0 + 1, 77);               /* PMC1 */
	CHECK_EQ(mfspr(SPR_MMCR0 + 1), 77);

	/* an L2 global invalidate is over at once */
	mtspr(SPR_L2CR, PRIV_L2CR | 0x00200000 | L2CR_L2IP);
	CHECK_EQ(mfspr(SPR_L2CR), PRIV_L2CR | 0x00200000);

	/* the decrementer counts down with the time base */
	cpu.tb = 5000;
	mtspr(SPR_DEC, 1000);
	cpu.tb = 5300;
	CHECK_EQ(mfspr(SPR_DEC), 700);
	cpu.tb = 6100;
	CHECK_EQ(mfspr(SPR_DEC), 0xFFFFFF9C);

	/* what a 750 doesn't have */
	CHECK(!exec(MFSPR(3, 1023)));           /* PIR */
	CHECK(!exec(MTSPR(959, 3)));            /* SDA */
	CHECK(!exec(TLBIA));
	CHECK_EQ(cpu.pc, CODE);
	CHECK(exec(TLBIE(4)));
	CHECK_EQ(cpu.pc, CODE + 4);

	/* segment registers */
	CHECK(exec(MFSR(6, 3)));
	CHECK_EQ(cpu.gpr[6], 0x20000003);
	cpu.gpr[7] = 0xABCDEF;
	cpu.gpr[8] = 0xC0000000;
	CHECK(exec(MTSRIN(7, 8)));
	CHECK(exec(MFSR(6, 12)));
	CHECK_EQ(cpu.gpr[6], 0xABCDEF);
	CHECK(exec(MTSR(1, 7)));
	cpu.gpr[8] = 0x10000000;
	CHECK(exec(MFSRIN(6, 8)));
	CHECK_EQ(cpu.gpr[6], 0xABCDEF);

	/* rfi to user mode ends supervisor mode */
	mtspr(SPR_SRR0, 0x4567);
	mtspr(SPR_SRR1, 0x0004D032);            /* (POW: no nap) */
	CHECK(exec(RFI));
	CHECK_EQ(cpu.pc, 0x4564);
	CHECK_EQ(p.msr, 0xD032);
	CHECK(!priv_supervisor(&p));
	CHECK(!exec(MFSPR(3, SPR_SRR0)));
}

/* Reading the temperature as software does: the lowest threshold it isn't above */
static void test_thermal(void)
{
	start();
	enter();
	mtspr(SPR_THRM1, 30u << 23 | THRM_V);
	CHECK_EQ(mfspr(SPR_THRM1), 30u << 23 | THRM_V);         /* (not enabled) */
	mtspr(SPR_THRM3, 0x2000 << 1 | THRM3_E);
	CHECK_EQ(mfspr(SPR_THRM1), 30u << 23 | THRM_V | THRM_TIV | THRM_TIN);
	unsigned lo = 0, hi = 127;
	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;
		mtspr(SPR_THRM1, mid << 23 | THRM_V);
		uint32_t v = mfspr(SPR_THRM1);
		CHECK(v & THRM_TIV);
		if (v & THRM_TIN)
			lo = mid + 1;
		else
			hi = mid;
	}
	CHECK_EQ(lo, PRIV_TEMPERATURE);
	mtspr(SPR_THRM2, 41u << 23 | THRM_TID | THRM_V);       /* below the threshold */
	CHECK_EQ(mfspr(SPR_THRM2) & (THRM_TIV | THRM_TIN), THRM_TIV | THRM_TIN);
	mtspr(SPR_THRM2, 40u << 23 | THRM_TID | THRM_V);
	CHECK_EQ(mfspr(SPR_THRM2) & (THRM_TIV | THRM_TIN), THRM_TIV);
}

/*
 * ZD MacBench 5.0's use of the key, in the interpreter: into supervisor mode
 * with data translation off, then on for a subroutine that saves L2CR and
 * HID0 to low memory, and back to user mode.
 */
static void test_macbench(void)
{
	struct mem mem;
	if (mem_init(&mem)) {
		CHECK(!"mem_init failed");
		return;
	}
	mem_map(&mem, 0, RAM_SIZE, MEM_RWX);
	const uint32_t code[] = {
		LIS(3, 0x4761), ORI(3, 3, 0x7279), LIS(4, 0x0505), ORI(4, 4, 0x1956),
		ORI(5, 0, 0xC010), LI(7, 0), TWI(31, 31, 2),
		MFLR(7), MTLR(8), MFMSR(8), ORI(3, 8, 0x10), MTMSR(3), ISYNC,
		BLRL,
		MTMSR(8), ISYNC, MTLR(7), MFMSR(5), ORI(5, 5, 0xC010), MTMSR(5), ISYNC,
		SC,
	};
	const uint32_t sub[] = {
		MFSPR(3, SPR_L2CR), STW(3, 0xA78, 0), MFSPR(3, SPR_HID0), STW(3, 0xA7C, 0), BLR,
	};
	for (size_t k = 0; k < sizeof code / sizeof code[0]; k++)
		mem_w32(&mem, CODE + 4 * (uint32_t)k, code[k]);
	for (size_t k = 0; k < sizeof sub / sizeof sub[0]; k++)
		mem_w32(&mem, SUB + 4 * (uint32_t)k, sub[k]);
	start();
	ppc_init(&cpu, &mem);
	cpu.pc = CODE;
	cpu.gpr[8] = SUB;
	cpu.lr = 0x3000;
	unsigned msrs = 0;
	for (int n = 0; n < 40; n++) {
		enum ppc_exit e = ppc_run(&cpu, 1000);
		if (e == PPC_EXIT_SC)
			break;
		if (e == PPC_EXIT_TRAP) {
			CHECK(priv_skeleton_key(&p, cpu.gpr));
			cpu.pc += 4;
		} else {
			CHECK_EQ(e, PPC_EXIT_PRIV);
			if (!priv_insn(&p, &cpu))
				break;
			msrs++;
		}
	}
	CHECK_EQ(cpu.pc, CODE + 4 * (sizeof code / sizeof code[0] - 1));
	CHECK_EQ(msrs, 7);
	CHECK_EQ(mem_r32(&mem, 0xA78), PRIV_L2CR);
	CHECK_EQ(mem_r32(&mem, 0xA7C), PRIV_HID0);
	CHECK_EQ(cpu.lr, 0x3000);
	CHECK_EQ(p.msr, PRIV_USER_MSR);
	CHECK(!priv_supervisor(&p));
	mem_free(&mem);
}

void test_priv(void)
{
	test_user();
	test_registers();
	test_thermal();
	test_macbench();
}
