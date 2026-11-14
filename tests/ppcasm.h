/* Minimal PowerPC instruction encoders for tests. */
#ifndef SYSHM_PPCASM_H
#define SYSHM_PPCASM_H

#include <stdint.h>

static inline uint32_t enc_d(unsigned op, unsigned rt, unsigned ra, uint32_t imm)
{
	return (uint32_t)op << 26 | rt << 21 | ra << 16 | (imm & 0xFFFF);
}

static inline uint32_t enc_x(unsigned op, unsigned rt, unsigned ra, unsigned rb, unsigned xo, unsigned rc)
{
	return (uint32_t)op << 26 | rt << 21 | ra << 16 | rb << 11 | xo << 1 | rc;
}

static inline uint32_t enc_xo(unsigned rt, unsigned ra, unsigned rb, unsigned oe, unsigned xo, unsigned rc)
{
	return 31u << 26 | rt << 21 | ra << 16 | rb << 11 | oe << 10 | xo << 1 | rc;
}

static inline uint32_t enc_m(unsigned op, unsigned ra, unsigned rs, unsigned sh, unsigned mb, unsigned me)
{
	return (uint32_t)op << 26 | rs << 21 | ra << 16 | sh << 11 | mb << 6 | me << 1;
}

#define SPRF(n) ((((n) & 0x1Fu) << 16) | ((((n) >> 5) & 0x1Fu) << 11))

#define LI(rt, v)            enc_d(14, rt, 0, (uint32_t)(v))
#define LIS(rt, v)           enc_d(15, rt, 0, v)
#define ADDI(rt, ra, v)      enc_d(14, rt, ra, (uint32_t)(v))
#define ADDIC(rt, ra, v)     enc_d(12, rt, ra, (uint32_t)(v))
#define SUBFIC(rt, ra, v)    enc_d(8, rt, ra, (uint32_t)(v))
#define MULLI(rt, ra, v)     enc_d(7, rt, ra, (uint32_t)(v))
#define ORI(ra, rs, v)       enc_d(24, rs, ra, v)
#define ANDI_(ra, rs, v)     enc_d(28, rs, ra, v)
#define CMPWI(bf, ra, v)     enc_d(11, (bf) << 2, ra, (uint32_t)(v))
#define CMPLWI(bf, ra, v)    enc_d(10, (bf) << 2, ra, v)
#define TWI(to, ra, v)       enc_d(3, to, ra, (uint32_t)(v))

#define LWZ(rt, d, ra)       enc_d(32, rt, ra, (uint32_t)(d))
#define LWZU(rt, d, ra)      enc_d(33, rt, ra, (uint32_t)(d))
#define LBZ(rt, d, ra)       enc_d(34, rt, ra, (uint32_t)(d))
#define STW(rs, d, ra)       enc_d(36, rs, ra, (uint32_t)(d))
#define STWU(rs, d, ra)      enc_d(37, rs, ra, (uint32_t)(d))
#define LHZ(rt, d, ra)       enc_d(40, rt, ra, (uint32_t)(d))
#define LHA(rt, d, ra)       enc_d(42, rt, ra, (uint32_t)(d))
#define STH(rs, d, ra)       enc_d(44, rs, ra, (uint32_t)(d))
#define LMW(rt, d, ra)       enc_d(46, rt, ra, (uint32_t)(d))
#define STMW(rs, d, ra)      enc_d(47, rs, ra, (uint32_t)(d))

#define ADD(rt, ra, rb)      enc_xo(rt, ra, rb, 0, 266, 0)
#define ADDO_(rt, ra, rb)    enc_xo(rt, ra, rb, 1, 266, 1)
#define ADDC(rt, ra, rb)     enc_xo(rt, ra, rb, 0, 10, 0)
#define ADDE(rt, ra, rb)     enc_xo(rt, ra, rb, 0, 138, 0)
#define ADDZE(rt, ra)        enc_xo(rt, ra, 0, 0, 202, 0)
#define SUBF(rt, ra, rb)     enc_xo(rt, ra, rb, 0, 40, 0)
#define NEGO(rt, ra)         enc_xo(rt, ra, 0, 1, 104, 0)
#define MULLW(rt, ra, rb)    enc_xo(rt, ra, rb, 0, 235, 0)
#define MULLWO(rt, ra, rb)   enc_xo(rt, ra, rb, 1, 235, 0)
#define MULHW(rt, ra, rb)    enc_xo(rt, ra, rb, 0, 75, 0)
#define MULHWU(rt, ra, rb)   enc_xo(rt, ra, rb, 0, 11, 0)
#define DIVW(rt, ra, rb)     enc_xo(rt, ra, rb, 0, 491, 0)
#define DIVWU(rt, ra, rb)    enc_xo(rt, ra, rb, 0, 459, 0)

#define SLW(ra, rs, rb)      enc_x(31, rs, ra, rb, 24, 0)
#define SRW(ra, rs, rb)      enc_x(31, rs, ra, rb, 536, 0)
#define SRAW(ra, rs, rb)     enc_x(31, rs, ra, rb, 792, 0)
#define SRAWI(ra, rs, sh)    enc_x(31, rs, ra, sh, 824, 0)
#define CNTLZW(ra, rs)       enc_x(31, rs, ra, 0, 26, 0)
#define EXTSB(ra, rs)        enc_x(31, rs, ra, 0, 954, 0)
#define EXTSH(ra, rs)        enc_x(31, rs, ra, 0, 922, 0)
#define RLWINM(ra, rs, sh, mb, me) enc_m(21, ra, rs, sh, mb, me)
#define RLWIMI(ra, rs, sh, mb, me) enc_m(20, ra, rs, sh, mb, me)
#define RLWNM(ra, rs, rb, mb, me)  enc_m(23, ra, rs, rb, mb, me)

#define LWBRX(rt, ra, rb)    enc_x(31, rt, ra, rb, 534, 0)
#define STWBRX(rs, ra, rb)   enc_x(31, rs, ra, rb, 662, 0)
#define STHBRX(rs, ra, rb)   enc_x(31, rs, ra, rb, 918, 0)
#define LWARX(rt, ra, rb)    enc_x(31, rt, ra, rb, 20, 0)
#define STWCX_(rs, ra, rb)   enc_x(31, rs, ra, rb, 150, 1)
#define LSWI(rt, ra, nb)     enc_x(31, rt, ra, nb, 597, 0)
#define STSWI(rs, ra, nb)    enc_x(31, rs, ra, nb, 725, 0)
#define DCBZ(ra, rb)         enc_x(31, 0, ra, rb, 1014, 0)

#define MFCR(rt)             enc_x(31, rt, 0, 0, 19, 0)
#define MTCRF(crm, rs)       (31u << 26 | (rs) << 21 | (crm) << 12 | 144u << 1)
#define MCRXR(bf)            enc_x(31, (bf) << 2, 0, 0, 512, 0)
#define MCRF(bf, bfa)        (19u << 26 | (bf) << 23 | (bfa) << 18)
#define CRXOR(bt, ba, bb)    enc_x(19, bt, ba, bb, 193, 0)
#define CROR(bt, ba, bb)     enc_x(19, bt, ba, bb, 449, 0)
#define MFSPR(rt, n)         (31u << 26 | (rt) << 21 | SPRF(n) | 339u << 1)
#define MTSPR(n, rs)         (31u << 26 | (rs) << 21 | SPRF(n) | 467u << 1)
#define MFXER(rt)            MFSPR(rt, 1)
#define MTXER(rs)            MTSPR(1, rs)
#define MTCTR(rs)            MTSPR(9, rs)
#define MFTB(rt)             (31u << 26 | (rt) << 21 | SPRF(268) | 371u << 1)
#define MFMSR(rt)            enc_x(31, rt, 0, 0, 83, 0)

#define B(off)               (18u << 26 | ((uint32_t)(off) & 0x03FFFFFCu))
#define BL(off)              (B(off) | 1)
#define BC(bo, bi, off)      (16u << 26 | (bo) << 21 | (bi) << 16 | ((uint32_t)(off) & 0xFFFCu))
#define BDNZ(off)            BC(16, 0, off)
#define BEQ(bf, off)         BC(12, (bf) * 4 + 2, off)
#define BGT(bf, off)         BC(12, (bf) * 4 + 1, off)
#define BLR                  0x4E800020u
#define BCTR                 0x4E800420u
#define SC                   0x44000002u
#define HOSTCALL(sel)        (6u << 26 | (sel))

/* Floating point */
static inline uint32_t enc_a(unsigned op, unsigned frt, unsigned fra, unsigned frb, unsigned frc, unsigned xo)
{
	return (uint32_t)op << 26 | frt << 21 | fra << 16 | frb << 11 | frc << 6 | xo << 1;
}
#define LFS(frt, d, ra)      enc_d(48, frt, ra, (uint32_t)(d))
#define LFD(frt, d, ra)      enc_d(50, frt, ra, (uint32_t)(d))
#define STFS(frs, d, ra)     enc_d(52, frs, ra, (uint32_t)(d))
#define STFD(frs, d, ra)     enc_d(54, frs, ra, (uint32_t)(d))
#define STFIWX(frs, ra, rb)  enc_x(31, frs, ra, rb, 983, 0)
#define FADD(t, a, b)        enc_a(63, t, a, b, 0, 21)
#define FSUB(t, a, b)        enc_a(63, t, a, b, 0, 20)
#define FDIV(t, a, b)        enc_a(63, t, a, b, 0, 18)
#define FMUL(t, a, c)        enc_a(63, t, a, 0, c, 25)
#define FMADD(t, a, c, b)    enc_a(63, t, a, b, c, 29)
#define FADDS(t, a, b)       enc_a(59, t, a, b, 0, 21)
#define FDIVS(t, a, b)       enc_a(59, t, a, b, 0, 18)
#define FRSP(t, b)           enc_x(63, t, 0, b, 12, 0)
#define FCTIWZ(t, b)         enc_x(63, t, 0, b, 15, 0)
#define FCMPU(bf, a, b)      enc_x(63, (bf) << 2, a, b, 0, 0)
#define FNEG(t, b)           enc_x(63, t, 0, b, 40, 0)
#define MFFS(t)              enc_x(63, t, 0, 0, 583, 0)

#endif
