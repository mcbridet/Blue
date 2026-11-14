/*
 * AArch64 instruction encodings for the JIT (Arm ARM, "A64 Instruction Set
 * Encoding"). Register 31 is the zero register in data processing and SP
 * as a load/store base. `sf` selects 64-bit (1) or 32-bit (0) operation.
 */
#ifndef SYSHM_A64_H
#define SYSHM_A64_H

#include <stdbool.h>
#include <stdint.h>

enum a64_cond {
	A64_EQ, A64_NE, A64_HS, A64_LO, A64_MI, A64_PL, A64_VS, A64_VC,
	A64_HI, A64_LS, A64_GE, A64_LT, A64_GT, A64_LE, A64_AL,
};

#define A64_ZR 31u
#define A64_SP 31u

enum a64_shift { A64_LSL, A64_LSR, A64_ASR, A64_ROR };

/* Data processing, register */
static inline uint32_t a64_addsub(unsigned sf, unsigned sub, unsigned setflags, unsigned d, unsigned n, unsigned m,
				  enum a64_shift sh, unsigned amount)
{
	return sf << 31 | sub << 30 | setflags << 29 | 0x0B000000u | (unsigned)sh << 22 | m << 16 | amount << 10 |
	       n << 5 | d;
}

/* ADD Xd, Xn, Wm, UXTW (a host address from a base and a 32-bit offset) */
static inline uint32_t a64_add_uxtw(unsigned d, unsigned n, unsigned m)
{
	return 0x8B204000u | m << 16 | n << 5 | d;
}

static inline uint32_t a64_addsub_imm(unsigned sf, unsigned sub, unsigned setflags, unsigned d, unsigned n,
				      unsigned imm12)
{
	return sf << 31 | sub << 30 | setflags << 29 | 0x11000000u | (imm12 & 0xFFF) << 10 | n << 5 | d;
}

/* ADC/SBC (sub = 1), optionally setting flags */
static inline uint32_t a64_adc(unsigned sf, unsigned sub, unsigned setflags, unsigned d, unsigned n, unsigned m)
{
	return sf << 31 | sub << 30 | setflags << 29 | 0x1A000000u | m << 16 | n << 5 | d;
}

enum a64_logic { A64_AND, A64_ORR, A64_EOR, A64_ANDS };

/* invert: BIC, ORN, EON, BICS */
static inline uint32_t a64_logic(unsigned sf, enum a64_logic opc, unsigned invert, unsigned d, unsigned n, unsigned m,
				 enum a64_shift sh, unsigned amount)
{
	return sf << 31 | (unsigned)opc << 29 | 0x0A000000u | (unsigned)sh << 22 | invert << 21 | m << 16 |
	       amount << 10 | n << 5 | d;
}

/* `nrs` is N:immr:imms from a64_logic_imm_encode() */
static inline uint32_t a64_logic_imm(unsigned sf, enum a64_logic opc, unsigned d, unsigned n, uint32_t nrs)
{
	return sf << 31 | (unsigned)opc << 29 | 0x12000000u | nrs << 10 | n << 5 | d;
}

enum a64_movw { A64_MOVN = 0, A64_MOVZ = 2, A64_MOVK = 3 };

static inline uint32_t a64_movw(unsigned sf, enum a64_movw opc, unsigned d, unsigned imm16, unsigned hw)
{
	return sf << 31 | (unsigned)opc << 29 | 0x12800000u | hw << 21 | (imm16 & 0xFFFF) << 5 | d;
}

enum a64_bfm { A64_SBFM, A64_BFM, A64_UBFM };

static inline uint32_t a64_bitfield(unsigned sf, enum a64_bfm opc, unsigned d, unsigned n, unsigned immr, unsigned imms)
{
	return sf << 31 | (unsigned)opc << 29 | 0x13000000u | sf << 22 | immr << 16 | imms << 10 | n << 5 | d;
}

/* EXTR: (n:m) >> lsb; ROR when n == m */
static inline uint32_t a64_extr(unsigned sf, unsigned d, unsigned n, unsigned m, unsigned lsb)
{
	return sf << 31 | 0x13800000u | sf << 22 | m << 16 | lsb << 10 | n << 5 | d;
}

enum a64_dp2 { A64_UDIV = 2, A64_SDIV = 3, A64_LSLV = 8, A64_LSRV = 9, A64_ASRV = 10, A64_RORV = 11 };

static inline uint32_t a64_dp2(unsigned sf, enum a64_dp2 op, unsigned d, unsigned n, unsigned m)
{
	return sf << 31 | 0x1AC00000u | m << 16 | (unsigned)op << 10 | n << 5 | d;
}

enum a64_dp1 { A64_RBIT = 0, A64_REV16 = 1, A64_REV32 = 2, A64_REV64 = 3, A64_CLZ = 4 };

/* REV32 is REV for sf = 0; REV64 needs sf = 1 */
static inline uint32_t a64_dp1(unsigned sf, enum a64_dp1 op, unsigned d, unsigned n)
{
	return sf << 31 | 0x5AC00000u | (unsigned)op << 10 | n << 5 | d;
}

/* MADD d = a + n * m (MUL with a = ZR) */
static inline uint32_t a64_madd(unsigned sf, unsigned d, unsigned n, unsigned m, unsigned a)
{
	return sf << 31 | 0x1B000000u | m << 16 | a << 10 | n << 5 | d;
}

/* SMADDL / UMADDL: Xd = Xa + Wn * Wm (SMULL / UMULL with a = ZR) */
static inline uint32_t a64_maddl(bool is_unsigned, unsigned d, unsigned n, unsigned m, unsigned a)
{
	return (is_unsigned ? 0x9BA00000u : 0x9B200000u) | m << 16 | a << 10 | n << 5 | d;
}

/* CSEL (inv 0, inc 0), CSINC (0, 1), CSINV (1, 0), CSNEG (1, 1) */
static inline uint32_t a64_csel(unsigned sf, unsigned inv, unsigned inc, unsigned d, unsigned n, unsigned m,
				enum a64_cond cond)
{
	return sf << 31 | inv << 30 | 0x1A800000u | m << 16 | (unsigned)cond << 12 | inc << 10 | n << 5 | d;
}

/* Loads and stores. size: 0 byte, 1 halfword, 2 word, 3 doubleword; load zero-extends. */
static inline uint32_t a64_ldst_imm(unsigned size, bool load, unsigned t, unsigned n, unsigned byte_offset)
{
	return size << 30 | 0x39000000u | (unsigned)load << 22 | (byte_offset >> size) << 10 | n << 5 | t;
}

/* [Xn, Wm, UXTW] */
static inline uint32_t a64_ldst_uxtw(unsigned size, bool load, unsigned t, unsigned n, unsigned m)
{
	return size << 30 | 0x38204800u | (unsigned)load << 22 | m << 16 | n << 5 | t;
}

/* [Xn, Wm, UXTW #size] */
static inline uint32_t a64_ldst_uxtw_scaled(unsigned size, bool load, unsigned t, unsigned n, unsigned m)
{
	return size << 30 | 0x38205800u | (unsigned)load << 22 | m << 16 | n << 5 | t;
}

enum a64_pair_mode { A64_POST = 1, A64_OFFSET = 2, A64_PRE = 3 };

/* STP/LDP of X registers; offset in bytes (multiple of 8) */
static inline uint32_t a64_pair(bool load, enum a64_pair_mode mode, unsigned t1, unsigned t2, unsigned n, int offset)
{
	return 0xA8000000u | (unsigned)mode << 23 | (unsigned)load << 22 | ((unsigned)(offset / 8) & 0x7F) << 15 |
	       t2 << 10 | n << 5 | t1;
}

/* Branches; offsets in bytes from the branch */
static inline uint32_t a64_b(bool link, int32_t offset)
{
	return (link ? 0x94000000u : 0x14000000u) | ((uint32_t)(offset >> 2) & 0x03FFFFFFu);
}

static inline uint32_t a64_bcond(enum a64_cond cond, int32_t offset)
{
	return 0x54000000u | ((uint32_t)(offset >> 2) & 0x7FFFF) << 5 | (unsigned)cond;
}

static inline uint32_t a64_cbz(unsigned sf, bool nonzero, unsigned t, int32_t offset)
{
	return sf << 31 | 0x34000000u | (unsigned)nonzero << 24 | ((uint32_t)(offset >> 2) & 0x7FFFF) << 5 | t;
}

static inline uint32_t a64_tbz(bool nonzero, unsigned t, unsigned bit, int32_t offset)
{
	return (bit >> 5) << 31 | 0x36000000u | (unsigned)nonzero << 24 | (bit & 31) << 19 |
	       ((uint32_t)(offset >> 2) & 0x3FFF) << 5 | t;
}

/* ADR Xd, . + offset */
static inline uint32_t a64_adr(unsigned d, int32_t offset)
{
	return 0x10000000u | ((uint32_t)offset & 3) << 29 | ((uint32_t)(offset >> 2) & 0x7FFFF) << 5 | d;
}

/* Floating point; `type` 1 is double precision, 0 single */
enum a64_fp2 { A64_FMUL, A64_FDIV, A64_FADD, A64_FSUB };

static inline uint32_t a64_fp2(unsigned type, enum a64_fp2 op, unsigned d, unsigned n, unsigned m)
{
	return 0x1E200800u | type << 22 | m << 16 | (unsigned)op << 12 | n << 5 | d;
}

/* FMADD d = a + n * m (o1 0, o0 0), FMSUB d = a - n * m (0, 1), FNMADD d = -a - n * m (1, 0), FNMSUB d = n * m - a (1, 1) */
static inline uint32_t a64_fp3(unsigned type, unsigned o1, unsigned o0, unsigned d, unsigned n, unsigned m, unsigned a)
{
	return 0x1F000000u | type << 22 | o1 << 21 | m << 16 | o0 << 15 | a << 10 | n << 5 | d;
}

/* FCVT_S converts to single, FCVT_D to double (`type` is the source's) */
enum a64_fp1 { A64_FMOV, A64_FABS, A64_FNEG, A64_FSQRT, A64_FCVT_S, A64_FCVT_D, A64_FRINTN = 8, A64_FRINTZ = 11 };

static inline uint32_t a64_fp1(unsigned type, enum a64_fp1 op, unsigned d, unsigned n)
{
	return 0x1E204000u | type << 22 | (unsigned)op << 15 | n << 5 | d;
}

/* FCMP Dn, Dm; or FCMP Dn, #0.0 */
static inline uint32_t a64_fcmp(unsigned type, unsigned n, unsigned m, bool zero)
{
	return 0x1E202000u | type << 22 | (zero ? 0 : m) << 16 | n << 5 | (zero ? 8u : 0);
}

static inline uint32_t a64_fcsel(unsigned type, unsigned d, unsigned n, unsigned m, enum a64_cond cond)
{
	return 0x1E200C00u | type << 22 | m << 16 | (unsigned)cond << 12 | n << 5 | d;
}

/* FMOV between a general register and an FP one: Xd <- Dn (to_fp false) or Dd <- Xn; W and S for !dbl */
static inline uint32_t a64_fmov_gp(bool dbl, bool to_fp, unsigned d, unsigned n)
{
	return (dbl ? 0x9E660000u : 0x1E260000u) | (unsigned)to_fp << 16 | n << 5 | d;
}

/* FMOV Dd, #1.0 */
static inline uint32_t a64_fmov_one(unsigned d) { return 0x1E6E1000u | d; }

/* FCVTZS Wd, Dn; SCVTF Dd, Wn */
static inline uint32_t a64_fcvtzs(unsigned d, unsigned n) { return 0x1E780000u | n << 5 | d; }
static inline uint32_t a64_scvtf(unsigned d, unsigned n) { return 0x1E620000u | n << 5 | d; }

/* LDR/STR of an FP register (D, or S for !dbl) at [Xn, #byte_offset] */
static inline uint32_t a64_fp_ldst(bool dbl, bool load, unsigned t, unsigned n, unsigned byte_offset)
{
	unsigned size = dbl ? 3 : 2;
	return size << 30 | 0x3D000000u | (unsigned)load << 22 | (byte_offset >> size) << 10 | n << 5 | t;
}

static inline uint32_t a64_br(unsigned n) { return 0xD61F0000u | n << 5; }
static inline uint32_t a64_blr(unsigned n) { return 0xD63F0000u | n << 5; }
static inline uint32_t a64_ret(void) { return 0xD65F03C0u; }

/*
 * Encodes `value` (replicated to 64 bits when !sf) as a logical immediate
 * N:immr:imms; false if it has no such encoding (0, all ones, or not a
 * rotated run of ones repeated in 2-64 bit elements).
 */
static inline bool a64_logic_imm_encode(unsigned sf, uint64_t value, uint32_t *nrs)
{
	if (!sf)
		value = (value & 0xFFFFFFFFu) | value << 32;
	if (value == 0 || value == ~(uint64_t)0)
		return false;
	unsigned size = 64;
	while (size > 2) {
		unsigned half = size / 2;
		uint64_t mask = (1ull << half) - 1;
		if ((value & mask) != ((value >> half) & mask))
			break;
		size = half;
	}
	uint64_t mask = size == 64 ? ~(uint64_t)0 : (1ull << size) - 1;
	uint64_t elt = value & mask;
	/*
	 * One run of ones, maybe wrapping around the element: then the zeros
	 * are one run. The rotation left that brings the ones down to bit 0
	 * starts from where they begin.
	 */
	uint64_t zeros = ~elt & mask, ones_run = elt >> __builtin_ctzll(elt);
	unsigned start;
	if ((ones_run & (ones_run + 1)) == 0) {
		start = (unsigned)__builtin_ctzll(elt);
	} else {
		uint64_t zero_run = zeros >> __builtin_ctzll(zeros);
		if (zero_run & (zero_run + 1))
			return false;
		start = (unsigned)(__builtin_ctzll(zeros) + __builtin_popcountll(zeros));
	}
	unsigned ones = (unsigned)__builtin_popcountll(elt), r = (size - start) & (size - 1);
	unsigned n = size == 64;
	unsigned imms = ((0x7Fu << 1) & ~(2 * size - 1) & 0x3F) | (ones - 1);
	*nrs = n << 12 | r << 6 | imms;
	return true;
}

#endif
