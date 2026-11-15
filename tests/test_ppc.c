#include <string.h>

#include "../src/mem.h"
#include "../src/ppc.h"
#include "../src/ppc_fp.h"
#include "ppcasm.h"
#include "test.h"

#define CODE 0x1000u
#define RAM_SIZE 0x100000u
#define RO_PAGE 0x200000u

static struct mem mem;
static struct ppc_cpu cpu;

/* Loads `code` at CODE and runs it until the first exit. */
static enum ppc_exit run(const uint32_t *code, size_t n)
{
	memset(mem_ptr(&mem, 0), 0, RAM_SIZE);
	for (size_t k = 0; k < n; k++)
		mem_w32(&mem, CODE + 4 * (uint32_t)k, code[k]);
	ppc_init(&cpu, &mem);
	cpu.pc = CODE;
	return ppc_run(&cpu, 100000);
}

#define RUN(...)                                                         \
	do {                                                             \
		const uint32_t prog_[] = { __VA_ARGS__ };         \
		exit_ = run(prog_, sizeof prog_ / sizeof prog_[0]);      \
	} while (0)

static enum ppc_exit exit_;

static void test_arith(void)
{
	RUN(LIS(3, 0x1234), ORI(3, 3, 0x5678), LI(4, -1), ADDI(5, 0, 0x7FFF), SC);
	CHECK_EQ(exit_, PPC_EXIT_SC);
	CHECK_EQ(cpu.pc, CODE + 16);
	CHECK_EQ(cpu.gpr[3], 0x12345678);
	CHECK_EQ(cpu.gpr[4], 0xFFFFFFFF);
	CHECK_EQ(cpu.gpr[5], 0x7FFF);

	/* carry chain */
	RUN(LI(3, 1), LI(4, -1), LI(5, 0), LI(6, 1),
	    ADDC(8, 4, 6), MFXER(20),
	    ADDE(7, 3, 5), MFXER(21),
	    SUBFIC(10, 5, 0), MFXER(22),
	    SUBFIC(9, 6, 0), MFXER(23),
	    ADDZE(11, 3),
	    SUBF(12, 3, 4),
	    SC);
	CHECK_EQ(cpu.gpr[8], 0);
	CHECK_EQ(cpu.gpr[20], XER_CA);
	CHECK_EQ(cpu.gpr[7], 2);
	CHECK_EQ(cpu.gpr[21], 0);
	CHECK_EQ(cpu.gpr[10], 0);
	CHECK_EQ(cpu.gpr[22], XER_CA);
	CHECK_EQ(cpu.gpr[9], 0xFFFFFFFF);
	CHECK_EQ(cpu.gpr[23], 0);
	CHECK_EQ(cpu.gpr[11], 1);
	CHECK_EQ(cpu.gpr[12], 0xFFFFFFFE);

	/* overflow, summary overflow, record forms */
	RUN(LIS(3, 0x7FFF), ORI(3, 3, 0xFFFF), ADDO_(4, 3, 3), MFCR(15), MFXER(6),
	    LIS(8, 0x8000), NEGO(7, 8),
	    LI(9, 0), MTXER(9), NEGO(10, 9), MFXER(11),
	    LIS(12, 1), MULLWO(13, 12, 12), MFXER(14),
	    SC);
	CHECK_EQ(cpu.gpr[4], 0xFFFFFFFE);
	CHECK_EQ(cpu.gpr[15], 0x90000000);          /* LT | SO */
	CHECK_EQ(cpu.gpr[6], XER_SO | XER_OV);
	CHECK_EQ(cpu.gpr[7], 0x80000000);
	CHECK_EQ(cpu.gpr[10], 0);
	CHECK_EQ(cpu.gpr[11], 0);
	CHECK_EQ(cpu.gpr[13], 0);
	CHECK_EQ(cpu.gpr[14], XER_SO | XER_OV);

	/* multiply and divide */
	RUN(LI(3, -7), LI(4, 2),
	    DIVW(5, 3, 4), DIVWU(6, 3, 4), MULHW(7, 3, 4), MULHWU(8, 3, 4),
	    MULLW(9, 3, 4), MULLI(10, 3, 100),
	    SC);
	CHECK_EQ(cpu.gpr[5], 0xFFFFFFFD);
	CHECK_EQ(cpu.gpr[6], 0x7FFFFFFC);
	CHECK_EQ(cpu.gpr[7], 0xFFFFFFFF);
	CHECK_EQ(cpu.gpr[8], 1);
	CHECK_EQ(cpu.gpr[9], 0xFFFFFFF2);
	CHECK_EQ(cpu.gpr[10], 0xFFFFFD44);
}

static void test_logic(void)
{
	RUN(LIS(3, 0x1234), ORI(3, 3, 0x5678),
	    RLWINM(4, 3, 8, 24, 31),
	    RLWINM(5, 3, 0, 16, 31),
	    RLWINM(6, 3, 4, 0, 27),
	    RLWINM(7, 3, 28, 4, 31),
	    RLWINM(8, 3, 0, 28, 3),
	    LI(9, -1), RLWIMI(9, 3, 16, 8, 15),
	    LI(10, 8), RLWNM(11, 3, 10, 0, 31),
	    LI(0, 0), CNTLZW(12, 3), CNTLZW(13, 0),
	    EXTSB(14, 3), LI(15, 0x80), EXTSB(16, 15),
	    ORI(17, 0, 0x8001), EXTSH(18, 17),
	    SC);
	CHECK_EQ(cpu.gpr[4], 0x12);
	CHECK_EQ(cpu.gpr[5], 0x5678);
	CHECK_EQ(cpu.gpr[6], 0x23456780);
	CHECK_EQ(cpu.gpr[7], 0x01234567);
	CHECK_EQ(cpu.gpr[8], 0x10000008);
	CHECK_EQ(cpu.gpr[9], 0xFF78FFFF);
	CHECK_EQ(cpu.gpr[11], 0x34567812);
	CHECK_EQ(cpu.gpr[12], 3);
	CHECK_EQ(cpu.gpr[13], 32);
	CHECK_EQ(cpu.gpr[14], 0x78);
	CHECK_EQ(cpu.gpr[16], 0xFFFFFF80);
	CHECK_EQ(cpu.gpr[18], 0xFFFF8001);

	/* shifts and XER[CA] from algebraic shifts */
	RUN(LIS(3, 0x8000), ORI(3, 3, 0x10),
	    SRAWI(4, 3, 4), MFXER(5),
	    SRAWI(6, 3, 5), MFXER(7),
	    LI(8, 40), SRAW(9, 3, 8), MFXER(19),
	    SLW(10, 3, 8), LI(11, 4), SLW(12, 3, 11), SRW(13, 3, 11),
	    LI(14, 32), SRW(15, 3, 14),
	    SC);
	CHECK_EQ(cpu.gpr[4], 0xF8000001);
	CHECK_EQ(cpu.gpr[5], 0);
	CHECK_EQ(cpu.gpr[6], 0xFC000000);
	CHECK_EQ(cpu.gpr[7], XER_CA);
	CHECK_EQ(cpu.gpr[9], 0xFFFFFFFF);
	CHECK_EQ(cpu.gpr[19], XER_CA);
	CHECK_EQ(cpu.gpr[10], 0);
	CHECK_EQ(cpu.gpr[12], 0x00000100);
	CHECK_EQ(cpu.gpr[13], 0x08000001);
	CHECK_EQ(cpu.gpr[15], 0);
}

static void test_branches_and_cr(void)
{
	/* sum 1..10 with a CTR loop */
	RUN(LI(3, 0), LI(4, 10), MTCTR(4), LI(5, 1),
	    ADD(3, 3, 5), ADDI(5, 5, 1), BDNZ(-8),
	    SC);
	CHECK_EQ(exit_, PPC_EXIT_SC);
	CHECK_EQ(cpu.gpr[3], 55);
	CHECK_EQ(cpu.gpr[5], 11);
	CHECK_EQ(cpu.ctr, 0);

	/* bl / blr */
	RUN(BL(12), LI(6, 7), SC, LI(7, 9), BLR);
	CHECK_EQ(cpu.pc, CODE + 8);
	CHECK_EQ(cpu.gpr[6], 7);
	CHECK_EQ(cpu.gpr[7], 9);
	CHECK_EQ(cpu.lr, CODE + 4);

	/* compares and conditional branches */
	RUN(LI(3, 55), LI(4, -1),
	    CMPWI(0, 3, 55), BEQ(0, 8), LI(8, 1),
	    CMPLWI(1, 4, 5), BGT(1, 8), LI(9, 1),
	    CMPWI(2, 4, 5), BGT(2, 8), LI(10, 1),
	    SC);
	CHECK_EQ(cpu.gpr[8], 0);
	CHECK_EQ(cpu.gpr[9], 0);
	CHECK_EQ(cpu.gpr[10], 1);

	/* CR field moves and logic */
	RUN(LIS(3, 0x1234), ORI(3, 3, 0x5678),
	    MTCRF(0x81, 3), MFCR(4),
	    MCRF(1, 7), MFCR(5),
	    CRXOR(3, 3, 3), MFCR(6),
	    CROR(0, 4, 4), MFCR(7),
	    LIS(8, 0xE000), MTXER(8), MCRXR(2), MFXER(9), MFCR(10),
	    SC);
	CHECK_EQ(cpu.gpr[4], 0x10000008);
	CHECK_EQ(cpu.gpr[5], 0x18000008);
	CHECK_EQ(cpu.gpr[6], 0x08000008);
	CHECK_EQ(cpu.gpr[7], 0x88000008);
	CHECK_EQ(cpu.gpr[9], 0);
	CHECK_EQ(cpu.gpr[10], 0x88E00008);
}

static void test_memory(void)
{
	RUN(LI(10, 0x4000), LIS(3, 0x1234), ORI(3, 3, 0x5678),
	    STW(3, 0, 10), LBZ(4, 0, 10), LBZ(5, 3, 10), LHZ(6, 2, 10),
	    LI(7, -2), STH(7, 4, 10), LHA(8, 4, 10), LHZ(9, 4, 10),
	    LWBRX(11, 0, 10),
	    STWU(3, 8, 10), LWZU(12, -8, 10),
	    STHBRX(3, 0, 10), LWZ(13, 0, 10),
	    SC);
	CHECK_EQ(exit_, PPC_EXIT_SC);
	CHECK_EQ(cpu.gpr[4], 0x12);
	CHECK_EQ(cpu.gpr[5], 0x78);
	CHECK_EQ(cpu.gpr[6], 0x5678);
	CHECK_EQ(cpu.gpr[8], 0xFFFFFFFE);
	CHECK_EQ(cpu.gpr[9], 0xFFFE);
	CHECK_EQ(cpu.gpr[11], 0x78563412);
	CHECK_EQ(cpu.gpr[12], 0x12345678);
	CHECK_EQ(cpu.gpr[10], 0x4000);
	CHECK_EQ(cpu.gpr[13], 0x78565678);
	CHECK_EQ(mem_ptr(&mem, 0x4008)[0], 0x12);
	CHECK_EQ(mem_ptr(&mem, 0x4008)[3], 0x78);

	/* load/store multiple and string */
	{
		const uint32_t prog[] = {
			LI(10, 0x4000),
			LI(29, 1), LI(30, 2), LI(31, 3), STMW(29, 0x100, 10),
			LI(29, 0), LI(30, 0), LI(31, 0), LMW(29, 0x100, 10), LWZ(14, 0x104, 10),
			LI(20, 0x4200), LSWI(21, 20, 7),
			LI(23, 0x4300), STSWI(21, 23, 5),
			LI(24, 0x4410), DCBZ(0, 24),
			SC,
		};
		memset(mem_ptr(&mem, 0), 0, RAM_SIZE);
		for (size_t k = 0; k < sizeof prog / sizeof prog[0]; k++)
			mem_w32(&mem, CODE + 4 * (uint32_t)k, prog[k]);
		memcpy(mem_ptr(&mem, 0x4200), "ABCDEFG", 7);
		memset(mem_ptr(&mem, 0x4400), 0xAA, 0x40);
		ppc_init(&cpu, &mem);
		cpu.pc = CODE;
		exit_ = ppc_run(&cpu, 1000);
	}
	CHECK_EQ(exit_, PPC_EXIT_SC);
	CHECK_EQ(cpu.gpr[29], 1);
	CHECK_EQ(cpu.gpr[30], 2);
	CHECK_EQ(cpu.gpr[31], 3);
	CHECK_EQ(cpu.gpr[14], 2);
	CHECK_EQ(cpu.gpr[21], 0x41424344);
	CHECK_EQ(cpu.gpr[22], 0x45464700);
	CHECK(memcmp(mem_ptr(&mem, 0x4300), "ABCDE\0", 6) == 0);
	CHECK_EQ(mem_ptr(&mem, 0x4400)[0], 0);
	CHECK_EQ(mem_ptr(&mem, 0x441F)[0], 0);
	CHECK_EQ(mem_ptr(&mem, 0x4420)[0], 0xAA);

	/* lwarx / stwcx. */
	{
		const uint32_t prog[] = {
			LI(15, 0x4500),
			LWARX(16, 0, 15), ADDI(16, 16, 1), STWCX_(16, 0, 15), MFCR(17),
			STWCX_(16, 0, 15), MFCR(18),
			SC,
		};
		memset(mem_ptr(&mem, 0), 0, RAM_SIZE);
		for (size_t k = 0; k < sizeof prog / sizeof prog[0]; k++)
			mem_w32(&mem, CODE + 4 * (uint32_t)k, prog[k]);
		mem_w32(&mem, 0x4500, 41);
		ppc_init(&cpu, &mem);
		cpu.pc = CODE;
		exit_ = ppc_run(&cpu, 1000);
	}
	CHECK_EQ(mem_r32(&mem, 0x4500), 42);
	CHECK_EQ(cpu.gpr[17] >> 28, CR_EQ);
	CHECK_EQ(cpu.gpr[18] >> 28, 0);
}

static void test_exits(void)
{
	RUN(LI(3, 5), TWI(16, 3, 0), TWI(4, 3, 5), SC);
	CHECK_EQ(exit_, PPC_EXIT_TRAP);
	CHECK_EQ(cpu.pc, CODE + 8);
	CHECK_EQ(cpu.exit_insn, TWI(4, 3, 5));

	RUN(TWI(31, 31, 7));
	CHECK_EQ(exit_, PPC_EXIT_TRAP);

	RUN(MFMSR(3));
	CHECK_EQ(exit_, PPC_EXIT_PRIV);
	RUN(MFSPR(3, 287));                     /* PVR */
	CHECK_EQ(exit_, PPC_EXIT_PRIV);

	RUN(LIS(3, 0x5000), LWZ(4, 0, 3));
	CHECK_EQ(exit_, PPC_EXIT_DSI);
	CHECK_EQ(cpu.pc, CODE + 4);
	CHECK_EQ(cpu.fault_addr, 0x50000000);
	CHECK(!cpu.fault_write);

	RUN(LIS(3, RO_PAGE >> 16), LWZ(4, 0, 3), STW(4, 0, 3));
	CHECK_EQ(exit_, PPC_EXIT_DSI);
	CHECK_EQ(cpu.pc, CODE + 8);
	CHECK(cpu.fault_write);

	RUN(LIS(3, 0x6000), MTCTR(3), BCTR);
	CHECK_EQ(exit_, PPC_EXIT_ISI);
	CHECK_EQ(cpu.pc, 0x60000000);

	RUN(HOSTCALL(0x123));
	CHECK_EQ(exit_, PPC_EXIT_HOSTCALL);
	CHECK_EQ(cpu.exit_insn & 0x03FFFFFF, 0x123);

	RUN(0x00000000u);
	CHECK_EQ(exit_, PPC_EXIT_ILLEGAL);
	RUN(0x10000000u);                       /* vaddubm v0,v0,v0 (AltiVec) */
	CHECK_EQ(exit_, PPC_EXIT_UNIMPL);

	RUN(MFTB(3), MFTB(4), SC);
	CHECK_EQ(cpu.gpr[4] - cpu.gpr[3], 1);
}

static uint64_t dbits(double d)
{
	uint64_t b;
	memcpy(&b, &d, sizeof b);
	return b;
}

static void test_fp(void)
{
	const uint32_t prog[] = {
		LI(10, 0x4000),
		LFD(1, 0x00, 10), LFD(2, 0x08, 10),             /* 1.5, 2.0 */
		FADD(3, 1, 2), FMUL(4, 1, 2), FDIV(5, 1, 2), FMADD(6, 1, 2, 1), FSUB(7, 1, 2),
		STFD(3, 0x100, 10), STFD(4, 0x108, 10), STFD(5, 0x110, 10),
		STFD(6, 0x118, 10), STFD(7, 0x120, 10),
		LFD(8, 0x10, 10), FCTIWZ(9, 8), LI(11, 0x128), STFIWX(9, 10, 11),      /* -2.75 */
		FCMPU(1, 1, 2), MFCR(12),
		LFD(13, 0x18, 10), FRSP(14, 13), STFD(14, 0x130, 10),                    /* 0.1 */
		LFS(15, 0x20, 10), STFD(15, 0x138, 10),                                  /* 1.5f */
		LFS(16, 0x40, 10), LFS(17, 0x44, 10), FDIVS(18, 16, 17), STFS(18, 0x140, 10),
		FNEG(19, 1), STFD(19, 0x148, 10),
		LFD(20, 0x28, 10), LFD(21, 0x30, 10), FDIV(22, 20, 21), MFFS(23), STFD(23, 0x150, 10),
		SC,
	};
	memset(mem_ptr(&mem, 0), 0, RAM_SIZE);
	for (size_t k = 0; k < sizeof prog / sizeof prog[0]; k++)
		mem_w32(&mem, CODE + 4 * (uint32_t)k, prog[k]);
	mem_w64(&mem, 0x4000, dbits(1.5));
	mem_w64(&mem, 0x4008, dbits(2.0));
	mem_w64(&mem, 0x4010, dbits(-2.75));
	mem_w64(&mem, 0x4018, dbits(0.1));
	mem_w32(&mem, 0x4020, 0x3FC00000);              /* 1.5f */
	mem_w64(&mem, 0x4028, dbits(1.0));
	mem_w64(&mem, 0x4030, dbits(0.0));
	mem_w32(&mem, 0x4040, 0x3F800000);              /* 1.0f */
	mem_w32(&mem, 0x4044, 0x40400000);              /* 3.0f */
	ppc_init(&cpu, &mem);
	cpu.pc = CODE;
	exit_ = ppc_run(&cpu, 1000);

	CHECK_EQ(exit_, PPC_EXIT_SC);
	CHECK_EQ(mem_r64(&mem, 0x4100), dbits(3.5));
	CHECK_EQ(mem_r64(&mem, 0x4108), dbits(3.0));
	CHECK_EQ(mem_r64(&mem, 0x4110), dbits(0.75));
	CHECK_EQ(mem_r64(&mem, 0x4118), dbits(4.5));
	CHECK_EQ(mem_r64(&mem, 0x4120), dbits(-0.5));
	CHECK_EQ(mem_r32(&mem, 0x4128), 0xFFFFFFFE);    /* fctiwz(-2.75) = -2 */
	CHECK_EQ((cpu.gpr[12] >> 24) & 0xF, CR_LT);     /* cr1: 1.5 < 2.0 */
	CHECK_EQ(mem_r64(&mem, 0x4130), dbits((double)(float)0.1));
	CHECK_EQ(mem_r64(&mem, 0x4138), dbits(1.5));
	CHECK_EQ(mem_r32(&mem, 0x4140), 0x3EAAAAAB);    /* 1.0f / 3.0f */
	CHECK_EQ(mem_r64(&mem, 0x4148), dbits(-1.5));
	CHECK_EQ(mem_r64(&mem, 0x4150) & 0x84000000u, 0x84000000u);     /* FX and ZX after 1/0 */

	/* exact single <-> double conversions, including NaN payloads and denormals */
	CHECK_EQ(ppc_fp_single_to_double(0x7FA00001), 0x7FF4000020000000ull);  /* SNaN stays SNaN */
	CHECK_EQ(ppc_fp_double_to_single(0x7FF4000020000000ull), 0x7FA00001);
	CHECK_EQ(ppc_fp_single_to_double(0x00000001), dbits(0x1p-149));        /* smallest denormal */
	CHECK_EQ(ppc_fp_double_to_single(dbits(0x1p-149)), 0x00000001);
	CHECK_EQ(ppc_fp_double_to_single(dbits(-0.0)), 0x80000000);

	/* fnmsub and fnmadd negate the rounded result: an exact +0 becomes -0, on either path */
	for (int fast = 0; fast < 2; fast++) {
		ppc_fp_fast_path = fast;
		for (uint32_t op = 59; op <= 63; op += 4)
			for (uint32_t xo5 = 30; xo5 <= 31; xo5++) {
				memset(&cpu, 0, sizeof cpu);
				cpu.fpr[2] = dbits(1.0);
				cpu.fpr[3] = dbits(xo5 == 31 ? -1.0 : 1.0);
				cpu.fpr[4] = dbits(1.0);
				ppc_fp_exec(&cpu, op << 26 | 1u << 21 | 2u << 16 | 3u << 11 | 4u << 6 | xo5 << 1);
				CHECK_EQ(cpu.fpr[1], dbits(-0.0));
			}
	}
	ppc_fp_fast_path = true;
}

/* Random operands of the kinds that matter to the FP fast path's limits */
static uint64_t fp_operand(uint64_t *seed)
{
	uint64_t r = *seed = *seed * 6364136223846793005ull + 1442695040888963407ull;
	uint64_t x = r >> 11;
	unsigned kind = (unsigned)(r >> 3) & 7;
	uint64_t sign = (r & 1) << 63;
	switch (kind) {
	case 0:                                         /* any bits (NaNs, infinities, denormals too) */
		return x * 0x9E3779B97F4A7C15ull;
	case 1:                                         /* few significant bits: exact results */
		return sign | (uint64_t)(1000 + (x & 63)) << 52 | (x >> 6 & 7) << 49;
	case 2:                                         /* near the fast path's magnitude limits */
		return sign | (uint64_t)((x & 1 ? 1023 - 900 : 1023 + 1000) + (int)(x >> 1 & 7) - 4) << 52 |
		       (x >> 4 & 0xFFFFFFFFFFFFFull);
	case 3:                                         /* zero */
		return sign;
	case 4:                                         /* single-precision values */
		return ppc_fp_single_to_double((uint32_t)x);
	default:                                        /* ordinary magnitudes */
		return sign | (uint64_t)(1023 + (int)(x & 63) - 32) << 52 | (x >> 6 & 0xFFFFFFFFFFFFFull);
	}
}

/* The FP fast path gives exactly the general path's results and FPSCR */
static void test_fp_fast_path(void)
{
	static const uint16_t forms[] = { 18, 20, 21, 22, 25, 28, 29, 30, 31, 24, 26, 23 };
	uint64_t seed = 12345;
	unsigned diffs = 0;
	unsigned long fast_before = ppc_fp_fast_ops;
	for (unsigned n = 0; n < 400000; n++) {
		unsigned op = n & 1 ? 59 : 63, xo5 = forms[(n >> 1) % 12];
		bool frsp = n % 23 == 0;
		uint32_t insn = frsp ? 63u << 26 | 1u << 21 | 3u << 11 | 12u << 1 | (n >> 5 & 1) :
				op << 26 | 1u << 21 | 2u << 16 | 3u << 11 | 4u << 6 | xo5 << 1 | (n >> 5 & 1);
		uint64_t a = fp_operand(&seed), b = fp_operand(&seed), c = fp_operand(&seed);
		if (xo5 >= 28 && n % 5 == 0) {          /* cancellation: an exact zero, or what rounding left */
			double p = 0, da, dc;
			memcpy(&da, &a, 8);
			memcpy(&dc, &c, 8);
			p = da * dc;
			if (xo5 & 1)
				p = -p;
			memcpy(&b, &p, 8);
		}
		uint32_t fpscr = (uint32_t)(seed >> 20) & 0x9FFFF0F8u;  /* sticky bits, enables; nearest */
		if (n % 7 == 0)
			fpscr |= (uint32_t)(seed >> 40) & 3;        /* other rounding modes now and then */
		uint64_t res[2];
		uint32_t fs[2], cr[2];
		for (int fast = 0; fast < 2; fast++) {
			ppc_fp_fast_path = fast;
			memset(&cpu, 0, sizeof cpu);
			cpu.fpr[2] = a;
			cpu.fpr[3] = b;
			cpu.fpr[4] = c;
			cpu.fpscr = fpscr;
			ppc_fp_exec(&cpu, insn);
			res[fast] = cpu.fpr[1];
			fs[fast] = cpu.fpscr;
			cr[fast] = cpu.cr;
		}
		if (res[0] != res[1] || fs[0] != fs[1] || cr[0] != cr[1]) {
			if (diffs++ < 5)
				fprintf(stderr, "fp fast path: insn %08x a %016llx b %016llx c %016llx fpscr %08x: "
					"%016llx %08x, fast %016llx %08x\n", insn, (unsigned long long)a,
					(unsigned long long)b, (unsigned long long)c, fpscr, (unsigned long long)res[0],
					fs[0], (unsigned long long)res[1], fs[1]);
		}
	}
	ppc_fp_fast_path = true;
	CHECK_EQ(diffs, 0);
	CHECK(ppc_fp_fast_ops - fast_before > 100000);  /* (most ordinary operands take it) */
}

/*
 * NaN results as a 750's, the same on every host: the first NaN of frA,
 * frB and frC (of those the operation uses), quieted, else the default NaN
 * 0x7FF8000000000000; never negated by fnmadd and fnmsub; cut to single
 * precision's fraction by single-precision operations and frsp
 */
static void test_fp_nans(void)
{
	enum { ADD = 21, SUB = 20, MUL = 25, DIV = 18, SQRT = 22, RES = 24, RSQRTE = 26, MSUB = 28, MADD = 29,
	       NMSUB = 30, NMADD = 31, RSP = 0x100 };
	const uint64_t qa = 0x7FF8123456789ABCull, qb = 0xFFF800000000B0B0ull, qc = 0x7FF8000000C0C000ull;
	const uint64_t sa = 0x7FF000000000A0A1ull, sb = 0xFFF400000000B0B1ull, sn = 0x7FF4123456789ABCull;
	const uint64_t one = 0x3FF0000000000000ull, zero = 0, inf = 0x7FF0000000000000ull, ninf = 0xFFF0000000000000ull;
	const uint64_t dnan = 0x7FF8000000000000ull, q = 0x0008000000000000ull, single = 0xFFFFFFFFE0000000ull;
	const uint32_t vxsnan = 0x01000000u, vxisi = 0x00800000u, vximz = 0x00100000u, vxsqrt = 0x00000200u;
	const struct { unsigned op, xo5; uint64_t a, b, c, want; uint32_t vx; } t[] = {
		{ 63, ADD, qa, sb, 0, qa, vxsnan },             /* frA first, quiet or not */
		{ 63, ADD, sa, qb, 0, sa | q, vxsnan },
		{ 63, ADD, one, qb, 0, qb, 0 },
		{ 63, SUB, qb, qa, 0, qb, 0 },
		{ 63, MUL, one, sn, qc, qc, 0 },                /* (fmul doesn't use frB) */
		{ 63, MUL, qa, one, qc, qa, 0 },
		{ 63, DIV, one, sb, 0, sb | q, vxsnan },
		{ 63, MADD, one, qb, qc, qb, 0 },               /* frA, frB, frC */
		{ 63, MADD, qa, qb, qc, qa, 0 },
		{ 63, MADD, one, one, sn, sn | q, vxsnan },
		{ 63, MADD, inf, qb, zero, qb, vximz },         /* ∞ × 0 + a quiet NaN: invalid */
		{ 63, NMADD, one, qb, one, qb, 0 },             /* not negated */
		{ 63, NMSUB, qa, one, one, qa, 0 },
		{ 63, SUB, inf, inf, 0, dnan, vxisi },          /* the default NaN */
		{ 63, MUL, inf, 0, zero, dnan, vximz },
		{ 63, DIV, zero, zero, 0, dnan, 0x00200000u },
		{ 63, DIV, ninf, inf, 0, dnan, 0x00400000u },
		{ 63, SQRT, 0, 0xBFF0000000000000ull, 0, dnan, vxsqrt },
		{ 63, RSQRTE, 0, 0xBFF0000000000000ull, 0, dnan, vxsqrt },
		{ 63, NMADD, inf, ninf, one, dnan, vxisi },     /* (not negated either) */
		{ 63, SQRT, qa, sb, qc, sb | q, vxsnan },       /* (fsqrt uses only frB) */
		{ 59, ADD, qa, one, 0, qa & single, 0 },        /* single precision */
		{ 59, MADD, one, sn, one, (sn | q) & single, vxsnan },
		{ 59, RES, 0, qb, 0, qb & single, 0 },
		{ 63, RSP, 0, sn, 0, (sn | q) & single, vxsnan },
		{ 63, RSP, 0, qa, 0, qa & single, 0 },
	};
	for (int fast = 0; fast < 2; fast++) {
		ppc_fp_fast_path = fast;
		for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) {
			memset(&cpu, 0, sizeof cpu);
			cpu.fpr[2] = t[k].a;
			cpu.fpr[3] = t[k].b;
			cpu.fpr[4] = t[k].c;
			uint32_t insn = t[k].xo5 == RSP ? 63u << 26 | 1u << 21 | 3u << 11 | 12u << 1 :
					t[k].op << 26 | 1u << 21 | 2u << 16 | 3u << 11 | 4u << 6 | t[k].xo5 << 1;
			ppc_fp_exec(&cpu, insn);
			if (cpu.fpr[1] != t[k].want || (cpu.fpscr & 0x01F80700u) != t[k].vx) {
				fprintf(stderr, "fp nans: case %u: %016llx, fpscr %08x\n", k, (unsigned long long)cpu.fpr[1],
					cpu.fpscr);
				test_failures++;
			}
		}
	}
	ppc_fp_fast_path = true;
}

void test_ppc(void)
{
	if (mem_init(&mem)) {
		CHECK(!"mem_init failed");
		return;
	}
	mem_map(&mem, 0, RAM_SIZE, MEM_RWX);
	mem_map(&mem, RO_PAGE, MEM_PAGE_SIZE, MEM_RX);

	test_arith();
	test_logic();
	test_branches_and_cr();
	test_memory();
	test_exits();
	test_fp();
	test_fp_fast_path();
	test_fp_nans();

	mem_free(&mem);
}
