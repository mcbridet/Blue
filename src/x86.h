/*
 * x86-64 instruction encodings for the JIT (Intel SDM volume 2): legacy
 * prefixes, REX and VEX, ModRM, SIB and displacements, for the integer,
 * BMI1/BMI2, MOVBE and LZCNT instructions and AVX's scalar doubles. `w`
 * selects 64-bit (1) or 32-bit (0) operation; 32-bit operations clear the
 * upper half of their destination.
 */
#ifndef SYSHM_X86_H
#define SYSHM_X86_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum x86_reg {
	X86_RAX, X86_RCX, X86_RDX, X86_RBX, X86_RSP, X86_RBP, X86_RSI, X86_RDI,
	X86_R8, X86_R9, X86_R10, X86_R11, X86_R12, X86_R13, X86_R14, X86_R15,
	X86_NONE,                       /* (no index) */
	X86_RIP,                        /* (a base: RIP-relative) */
};

enum x86_cond {
	X86_O, X86_NO, X86_B, X86_AE, X86_E, X86_NE, X86_BE, X86_A,
	X86_S, X86_NS, X86_P, X86_NP, X86_L, X86_GE, X86_LE, X86_G,
};

/* The code being written; past `end` nothing is, and `overflow` is set */
struct x86_buf {
	uint8_t *p, *end;
	bool overflow;
};

/* A memory operand: [base + index << scale + disp], or [rip + target] */
struct x86_mem {
	uint8_t base, index, scale;
	int32_t disp;
	const void *target;
};

static inline struct x86_mem x86_m(unsigned base, int32_t disp)
{
	return (struct x86_mem){ (uint8_t)base, X86_NONE, 0, disp, NULL };
}

static inline struct x86_mem x86_mx(unsigned base, unsigned index, unsigned scale, int32_t disp)
{
	return (struct x86_mem){ (uint8_t)base, (uint8_t)index, (uint8_t)scale, disp, NULL };
}

static inline struct x86_mem x86_rip(const void *target)
{
	return (struct x86_mem){ X86_RIP, X86_NONE, 0, 0, target };
}

static inline void x86_byte(struct x86_buf *b, unsigned v)
{
	if (b->p < b->end)
		*b->p++ = (uint8_t)v;
	else
		b->overflow = true;
}

static inline void x86_imm(struct x86_buf *b, uint64_t v, unsigned bytes)
{
	for (unsigned k = 0; k < bytes; k++)
		x86_byte(b, (unsigned)(v >> 8 * k) & 0xFF);
}

static inline bool x86_fits8(int64_t v)
{
	return v >= -128 && v <= 127;
}

/* Opcode bytes, most significant first: 0x0F38F0 is three, 0x0FAF two */
static inline void x86_opcode(struct x86_buf *b, uint32_t op)
{
	if (op > 0xFFFF)
		x86_byte(b, op >> 16);
	if (op > 0xFF)
		x86_byte(b, (op >> 8) & 0xFF);
	x86_byte(b, op & 0xFF);
}

/* ModRM (and SIB, displacement) for `reg` and a memory operand; `imm` bytes follow (for RIP) */
static inline void x86_modrm_mem(struct x86_buf *b, unsigned reg, const struct x86_mem *m, unsigned imm)
{
	reg &= 7;
	if (m->base == X86_RIP) {
		x86_byte(b, reg << 3 | 5);
		int64_t disp = (const uint8_t *)m->target - (b->p + 4 + imm);
		x86_imm(b, (uint64_t)disp, 4);
		return;
	}
	unsigned base = m->base & 7, mod;
	if (m->disp == 0 && base != 5)
		mod = 0;
	else if (x86_fits8(m->disp))
		mod = 1;
	else
		mod = 2;
	if (m->index != X86_NONE) {
		x86_byte(b, mod << 6 | reg << 3 | 4);
		x86_byte(b, (unsigned)m->scale << 6 | (m->index & 7u) << 3 | base);
	} else if (base == 4) {
		x86_byte(b, mod << 6 | reg << 3 | 4);
		x86_byte(b, 0x24);
	} else {
		x86_byte(b, mod << 6 | reg << 3 | base);
	}
	if (mod == 1)
		x86_byte(b, (unsigned)m->disp & 0xFF);
	else if (mod == 2)
		x86_imm(b, (uint32_t)m->disp, 4);
}

/* REX if needed: W, the reg field's, index's and base's (or r/m register's) high bits; `byte` for spl..dil */
static inline void x86_rex(struct x86_buf *b, unsigned w, unsigned reg, unsigned index, unsigned base, bool byte)
{
	unsigned rex = w << 3 | (reg >> 3 & 1) << 2;
	if (index != X86_NONE)
		rex |= (index >> 3 & 1) << 1;
	if (base < 16)
		rex |= base >> 3 & 1;
	if (rex || byte)
		x86_byte(b, 0x40 | rex);
}

/*
 * A legacy-encoded instruction: prefix (0, 0x66, 0xF2, 0xF3), REX, opcode,
 * ModRM with `reg` (a register or an opcode extension) and register `rm`.
 * `byte`: X86_BREG if reg is a byte register, X86_BRM if rm is (spl, bpl,
 * sil and dil need REX).
 */
enum { X86_BREG = 1, X86_BRM = 2 };

static inline void x86_op_rr(struct x86_buf *b, unsigned pfx, unsigned w, uint32_t op, unsigned reg, unsigned rm,
			     unsigned byte)
{
	if (pfx)
		x86_byte(b, pfx);
	x86_rex(b, w, reg, X86_NONE, rm, ((byte & X86_BREG) && reg >= 4 && reg < 8) || ((byte & X86_BRM) && rm >= 4 && rm < 8));
	x86_opcode(b, op);
	x86_byte(b, 0xC0 | (reg & 7) << 3 | (rm & 7));
}

/* The same with a memory operand, `imm` bytes of immediate to follow */
static inline void x86_op_rm(struct x86_buf *b, unsigned pfx, unsigned w, uint32_t op, unsigned reg,
			     struct x86_mem m, unsigned imm, unsigned byte)
{
	if (pfx)
		x86_byte(b, pfx);
	x86_rex(b, w, reg, m.index, m.base == X86_RIP ? X86_NONE : m.base, (byte & X86_BREG) && reg >= 4 && reg < 8);
	x86_opcode(b, op);
	x86_modrm_mem(b, reg, &m, imm);
}

/*
 * VEX: `map` 1 (0F), 2 (0F38), 3 (0F3A); `pp` 0, 1 (66), 2 (F3), 3 (F2);
 * `v` the extra register operand (VEX.vvvv), 0 for none. The two-byte
 * form where it can be.
 */
static inline void x86_vex(struct x86_buf *b, unsigned map, unsigned pp, unsigned w, unsigned l, unsigned v,
			   unsigned reg, unsigned index, unsigned base)
{
	unsigned r = reg >> 3 & 1, x = index != X86_NONE ? index >> 3 & 1 : 0, bb = base < 16 ? base >> 3 & 1 : 0;
	if (map == 1 && !w && !x && !bb) {
		x86_byte(b, 0xC5);
		x86_byte(b, (r ^ 1) << 7 | (~v & 15) << 3 | l << 2 | pp);
	} else {
		x86_byte(b, 0xC4);
		x86_byte(b, (r ^ 1) << 7 | (x ^ 1) << 6 | (bb ^ 1) << 5 | map);
		x86_byte(b, w << 7 | (~v & 15) << 3 | l << 2 | pp);
	}
}

static inline void x86_vex_rr(struct x86_buf *b, unsigned map, unsigned pp, unsigned w, unsigned op, unsigned reg,
			      unsigned v, unsigned rm)
{
	x86_vex(b, map, pp, w, 0, v, reg, X86_NONE, rm);
	x86_byte(b, op);
	x86_byte(b, 0xC0 | (reg & 7) << 3 | (rm & 7));
}

static inline void x86_vex_rm(struct x86_buf *b, unsigned map, unsigned pp, unsigned w, unsigned op, unsigned reg,
			      unsigned v, struct x86_mem m, unsigned imm)
{
	x86_vex(b, map, pp, w, 0, v, reg, m.index, m.base == X86_RIP ? X86_NONE : m.base);
	x86_byte(b, op);
	x86_modrm_mem(b, reg, &m, imm);
}

/* ---- moves ---- */

static inline void x86_mov_rr(struct x86_buf *b, unsigned w, unsigned d, unsigned s)
{
	x86_op_rr(b, 0, w, 0x89, s, d, 0);
}

/* d = imm, zero-extended (flags unchanged) */
static inline void x86_mov_ri(struct x86_buf *b, unsigned d, uint32_t imm)
{
	x86_rex(b, 0, 0, X86_NONE, d, 0);
	x86_byte(b, 0xB8 | (d & 7));
	x86_imm(b, imm, 4);
}

static inline void x86_mov_ri64(struct x86_buf *b, unsigned d, uint64_t imm)
{
	if (imm <= 0xFFFFFFFFu) {
		x86_mov_ri(b, d, (uint32_t)imm);
	} else if ((int64_t)imm == (int32_t)imm) {
		x86_op_rr(b, 0, 1, 0xC7, 0, d, 0);
		x86_imm(b, imm, 4);
	} else {
		x86_rex(b, 1, 0, X86_NONE, d, 0);
		x86_byte(b, 0xB8 | (d & 7));
		x86_imm(b, imm, 8);
	}
}

/* Loads of 1, 2, 4 or 8 bytes into d, zero- or sign-extended to 32 bits (8: 64) */
static inline void x86_load(struct x86_buf *b, unsigned size, bool sign, unsigned d, struct x86_mem m)
{
	switch (size) {
	case 1:  x86_op_rm(b, 0, 0, sign ? 0x0FBE : 0x0FB6, d, m, 0, 0); break;
	case 2:  x86_op_rm(b, 0, 0, sign ? 0x0FBF : 0x0FB7, d, m, 0, 0); break;
	case 4:  x86_op_rm(b, 0, 0, 0x8B, d, m, 0, 0); break;
	default: x86_op_rm(b, 0, 1, 0x8B, d, m, 0, 0); break;
	}
}

static inline void x86_store(struct x86_buf *b, unsigned size, unsigned s, struct x86_mem m)
{
	switch (size) {
	case 1:  x86_op_rm(b, 0, 0, 0x88, s, m, 0, X86_BREG); break;
	case 2:  x86_op_rm(b, 0x66, 0, 0x89, s, m, 0, 0); break;
	case 4:  x86_op_rm(b, 0, 0, 0x89, s, m, 0, 0); break;
	default: x86_op_rm(b, 0, 1, 0x89, s, m, 0, 0); break;
	}
}

/* An immediate store (8 bytes: imm sign-extended from 32 bits) */
static inline void x86_store_imm(struct x86_buf *b, unsigned size, struct x86_mem m, uint32_t imm)
{
	switch (size) {
	case 1:  x86_op_rm(b, 0, 0, 0xC6, 0, m, 1, 0); x86_imm(b, imm, 1); break;
	case 2:  x86_op_rm(b, 0x66, 0, 0xC7, 0, m, 2, 0); x86_imm(b, imm, 2); break;
	case 4:  x86_op_rm(b, 0, 0, 0xC7, 0, m, 4, 0); x86_imm(b, imm, 4); break;
	default: x86_op_rm(b, 0, 1, 0xC7, 0, m, 4, 0); x86_imm(b, imm, 4); break;
	}
}

/* MOVZX, MOVSX from a byte (1) or halfword (2) register; MOVSXD (4) */
static inline void x86_extend(struct x86_buf *b, unsigned size, bool sign, unsigned w, unsigned d, unsigned s)
{
	if (size == 4)
		x86_op_rr(b, 0, 1, 0x63, d, s, 0);
	else
		x86_op_rr(b, 0, w, (size == 1 ? 0x0FB6 : 0x0FB7) | (sign ? 8 : 0), d, s, size == 1 ? X86_BRM : 0);
}

static inline void x86_lea(struct x86_buf *b, unsigned w, unsigned d, struct x86_mem m)
{
	x86_op_rm(b, 0, w, 0x8D, d, m, 0, 0);
}

/* MOVBE: a load (store false) into d of 2, 4 or 8 bytes, or a store of s */
static inline void x86_movbe(struct x86_buf *b, bool store, unsigned size, unsigned r, struct x86_mem m)
{
	x86_op_rm(b, size == 2 ? 0x66 : 0, size == 8, store ? 0x0F38F1 : 0x0F38F0, r, m, 0, 0);
}

static inline void x86_bswap(struct x86_buf *b, unsigned w, unsigned r)
{
	x86_rex(b, w, 0, X86_NONE, r, 0);
	x86_byte(b, 0x0F);
	x86_byte(b, 0xC8 | (r & 7));
}

/* ---- arithmetic and logic ---- */

enum x86_alu { X86_ADD, X86_OR, X86_ADC, X86_SBB, X86_AND, X86_SUB, X86_XOR, X86_CMP };

/* d = d OP s */
static inline void x86_alu_rr(struct x86_buf *b, enum x86_alu op, unsigned w, unsigned d, unsigned s)
{
	x86_op_rr(b, 0, w, (unsigned)op << 3 | 1, s, d, 0);
}

/* d = d OP [m] */
static inline void x86_alu_rm(struct x86_buf *b, enum x86_alu op, unsigned w, unsigned d, struct x86_mem m)
{
	x86_op_rm(b, 0, w, (unsigned)op << 3 | 3, d, m, 0, 0);
}

/* [m] = [m] OP s */
static inline void x86_alu_mr(struct x86_buf *b, enum x86_alu op, unsigned w, struct x86_mem m, unsigned s)
{
	x86_op_rm(b, 0, w, (unsigned)op << 3 | 1, s, m, 0, 0);
}

/* d = d OP imm (sign-extended from 32 bits for 64-bit operation) */
static inline void x86_alu_ri(struct x86_buf *b, enum x86_alu op, unsigned w, unsigned d, int32_t imm)
{
	if (x86_fits8(imm)) {
		x86_op_rr(b, 0, w, 0x83, op, d, 0);
		x86_imm(b, (uint32_t)imm, 1);
	} else {
		x86_op_rr(b, 0, w, 0x81, op, d, 0);
		x86_imm(b, (uint32_t)imm, 4);
	}
}

/* [m] OP= imm, on 1, 4 or 8 bytes */
static inline void x86_alu_mi(struct x86_buf *b, enum x86_alu op, unsigned size, struct x86_mem m, int32_t imm)
{
	if (size == 1) {
		x86_op_rm(b, 0, 0, 0x80, op, m, 1, 0);
		x86_imm(b, (uint32_t)imm, 1);
	} else if (x86_fits8(imm)) {
		x86_op_rm(b, 0, size == 8, 0x83, op, m, 1, 0);
		x86_imm(b, (uint32_t)imm, 1);
	} else {
		x86_op_rm(b, 0, size == 8, 0x81, op, m, 4, 0);
		x86_imm(b, (uint32_t)imm, 4);
	}
}

static inline void x86_test_rr(struct x86_buf *b, unsigned w, unsigned a, unsigned c)
{
	x86_op_rr(b, 0, w, 0x85, c, a, 0);
}

/* TEST r, imm (of its low byte if imm fits there) */
static inline void x86_test_ri(struct x86_buf *b, unsigned w, unsigned r, uint32_t imm)
{
	if (imm <= 0xFF) {
		x86_op_rr(b, 0, 0, 0xF6, 0, r, X86_BRM);
		x86_imm(b, imm, 1);
	} else {
		x86_op_rr(b, 0, w, 0xF7, 0, r, 0);
		x86_imm(b, imm, 4);
	}
}

/* TEST [m], imm on 1 or 4 bytes */
static inline void x86_test_mi(struct x86_buf *b, unsigned size, struct x86_mem m, uint32_t imm)
{
	if (size == 1) {
		x86_op_rm(b, 0, 0, 0xF6, 0, m, 1, 0);
		x86_imm(b, imm, 1);
	} else {
		x86_op_rm(b, 0, 0, 0xF7, 0, m, 4, 0);
		x86_imm(b, imm, 4);
	}
}

enum x86_unary { X86_NOT = 2, X86_NEG, X86_MUL, X86_IMUL1, X86_DIV, X86_IDIV };

static inline void x86_unary(struct x86_buf *b, enum x86_unary op, unsigned w, unsigned r)
{
	x86_op_rr(b, 0, w, 0xF7, op, r, 0);
}

static inline void x86_inc(struct x86_buf *b, bool dec, unsigned w, unsigned r)
{
	x86_op_rr(b, 0, w, 0xFF, dec, r, 0);
}

enum x86_shift { X86_ROL, X86_ROR, X86_SHL = 4, X86_SHR, X86_SAR = 7 };

static inline void x86_shift_ri(struct x86_buf *b, enum x86_shift op, unsigned w, unsigned r, unsigned n)
{
	if (n == 1) {
		x86_op_rr(b, 0, w, 0xD1, op, r, 0);
	} else {
		x86_op_rr(b, 0, w, 0xC1, op, r, 0);
		x86_byte(b, n);
	}
}

/* By CL */
static inline void x86_shift_cl(struct x86_buf *b, enum x86_shift op, unsigned w, unsigned r)
{
	x86_op_rr(b, 0, w, 0xD3, op, r, 0);
}

static inline void x86_imul_rr(struct x86_buf *b, unsigned w, unsigned d, unsigned s)
{
	x86_op_rr(b, 0, w, 0x0FAF, d, s, 0);
}

static inline void x86_imul_rri(struct x86_buf *b, unsigned w, unsigned d, unsigned s, int32_t imm)
{
	if (x86_fits8(imm)) {
		x86_op_rr(b, 0, w, 0x6B, d, s, 0);
		x86_imm(b, (uint32_t)imm, 1);
	} else {
		x86_op_rr(b, 0, w, 0x69, d, s, 0);
		x86_imm(b, (uint32_t)imm, 4);
	}
}

/* CDQ, or CQO */
static inline void x86_cdq(struct x86_buf *b, unsigned w)
{
	if (w)
		x86_byte(b, 0x48);
	x86_byte(b, 0x99);
}

static inline void x86_setcc(struct x86_buf *b, enum x86_cond cc, unsigned r)
{
	x86_op_rr(b, 0, 0, 0x0F90 | cc, 0, r, X86_BRM);
}

static inline void x86_cmov(struct x86_buf *b, enum x86_cond cc, unsigned w, unsigned d, unsigned s)
{
	x86_op_rr(b, 0, w, 0x0F40 | cc, d, s, 0);
}

static inline void x86_cmov_m(struct x86_buf *b, enum x86_cond cc, unsigned w, unsigned d, struct x86_mem m)
{
	x86_op_rm(b, 0, w, 0x0F40 | cc, d, m, 0, 0);
}

/* MOV of s's low byte (size 1) or halfword (2) into d's, the rest of d as it was */
static inline void x86_mov_low(struct x86_buf *b, unsigned size, unsigned d, unsigned s)
{
	if (size == 1)
		x86_op_rr(b, 0, 0, 0x88, s, d, X86_BREG | X86_BRM);
	else
		x86_op_rr(b, 0x66, 0, 0x89, s, d, 0);
}

enum x86_bt { X86_BT = 4, X86_BTS, X86_BTR, X86_BTC };

static inline void x86_bt_ri(struct x86_buf *b, enum x86_bt op, unsigned w, unsigned r, unsigned bit)
{
	x86_op_rr(b, 0, w, 0x0FBA, op, r, 0);
	x86_byte(b, bit);
}

static inline void x86_bt_mi(struct x86_buf *b, enum x86_bt op, struct x86_mem m, unsigned bit)
{
	x86_op_rm(b, 0, 0, 0x0FBA, op, m, 1, 0);
	x86_byte(b, bit);
}

static inline void x86_cmc(struct x86_buf *b)
{
	x86_byte(b, 0xF5);
}

static inline void x86_lzcnt(struct x86_buf *b, unsigned w, unsigned d, unsigned s)
{
	x86_op_rr(b, 0xF3, w, 0x0FBD, d, s, 0);
}

static inline void x86_tzcnt(struct x86_buf *b, unsigned w, unsigned d, unsigned s)
{
	x86_op_rr(b, 0xF3, w, 0x0FBC, d, s, 0);
}

/* ---- BMI1, BMI2 ---- */

/* d = ~a & s */
static inline void x86_andn(struct x86_buf *b, unsigned w, unsigned d, unsigned a, unsigned s)
{
	x86_vex_rr(b, 2, 0, w, 0xF2, d, a, s);
}

/* d = s with bits from index n (n's low byte) up cleared */
static inline void x86_bzhi(struct x86_buf *b, unsigned w, unsigned d, unsigned s, unsigned n)
{
	x86_vex_rr(b, 2, 0, w, 0xF5, d, n, s);
}

/* d = s shifted by n's low 5 (6 for w) bits: SHLX, SHRX, SARX */
static inline void x86_shiftx(struct x86_buf *b, enum x86_shift op, unsigned w, unsigned d, unsigned s, unsigned n)
{
	x86_vex_rr(b, 2, op == X86_SHL ? 1 : op == X86_SHR ? 3 : 2, w, 0xF7, d, n, s);
}

/* d = s rotated right by n */
static inline void x86_rorx(struct x86_buf *b, unsigned w, unsigned d, unsigned s, unsigned n)
{
	x86_vex_rr(b, 3, 3, w, 0xF0, d, 0, s);
	x86_byte(b, n);
}

/* hi:lo = edx (rdx) * s, unsigned */
static inline void x86_mulx(struct x86_buf *b, unsigned w, unsigned hi, unsigned lo, unsigned s)
{
	x86_vex_rr(b, 2, 3, w, 0xF6, hi, lo, s);
}

/* ---- control ---- */

/* A rel32 (or rel8) field's target */
static inline void x86_patch32(uint8_t *at, const void *target)
{
	int32_t rel = (int32_t)((const uint8_t *)target - (at + 4));
	memcpy(at, &rel, 4);
}

static inline bool x86_patch8(uint8_t *at, const void *target)
{
	int64_t rel = (const uint8_t *)target - (at + 1);
	*at = (uint8_t)rel;
	return x86_fits8(rel);
}

/* JMP rel32 / Jcc rel32 to `target` (NULL: to be patched); the rel32 field */
static inline uint8_t *x86_jmp(struct x86_buf *b, const void *target)
{
	x86_byte(b, 0xE9);
	uint8_t *at = b->p;
	x86_imm(b, 0, 4);
	if (target && !b->overflow)
		x86_patch32(at, target);
	return at;
}

static inline uint8_t *x86_jcc(struct x86_buf *b, enum x86_cond cc, const void *target)
{
	x86_byte(b, 0x0F);
	x86_byte(b, 0x80 | cc);
	uint8_t *at = b->p;
	x86_imm(b, 0, 4);
	if (target && !b->overflow)
		x86_patch32(at, target);
	return at;
}

/* JMP rel8 / Jcc rel8, to be patched */
static inline uint8_t *x86_jmp8(struct x86_buf *b)
{
	x86_byte(b, 0xEB);
	uint8_t *at = b->p;
	x86_byte(b, 0);
	return at;
}

static inline uint8_t *x86_jcc8(struct x86_buf *b, enum x86_cond cc)
{
	x86_byte(b, 0x70 | cc);
	uint8_t *at = b->p;
	x86_byte(b, 0);
	return at;
}

/* CALL rel32 */
static inline void x86_call(struct x86_buf *b, const void *target)
{
	x86_byte(b, 0xE8);
	uint8_t *at = b->p;
	x86_imm(b, 0, 4);
	if (!b->overflow)
		x86_patch32(at, target);
}

static inline void x86_jmp_r(struct x86_buf *b, unsigned r)
{
	x86_op_rr(b, 0, 0, 0xFF, 4, r, 0);
}

static inline void x86_jmp_m(struct x86_buf *b, struct x86_mem m)
{
	x86_op_rm(b, 0, 0, 0xFF, 4, m, 0, 0);
}

static inline void x86_call_m(struct x86_buf *b, struct x86_mem m)
{
	x86_op_rm(b, 0, 0, 0xFF, 2, m, 0, 0);
}

static inline void x86_ret(struct x86_buf *b)
{
	x86_byte(b, 0xC3);
}

static inline void x86_push(struct x86_buf *b, unsigned r)
{
	x86_rex(b, 0, 0, X86_NONE, r, 0);
	x86_byte(b, 0x50 | (r & 7));
}

static inline void x86_pop(struct x86_buf *b, unsigned r)
{
	x86_rex(b, 0, 0, X86_NONE, r, 0);
	x86_byte(b, 0x58 | (r & 7));
}

static inline void x86_int3(struct x86_buf *b)
{
	x86_byte(b, 0xCC);
}

/* ---- AVX: scalar doubles ---- */

enum x86_sd { X86_VSQRTSD = 0x51, X86_VADDSD = 0x58, X86_VMULSD, X86_VSUBSD = 0x5C, X86_VDIVSD = 0x5E };

/* d = a OP s */
static inline void x86_vsd(struct x86_buf *b, enum x86_sd op, unsigned d, unsigned a, unsigned s)
{
	x86_vex_rr(b, 1, 3, 0, op, d, a, s);
}

/* VMOVAPD d, s (a register copy) */
static inline void x86_vmovapd(struct x86_buf *b, unsigned d, unsigned s)
{
	x86_vex_rr(b, 1, 1, 0, 0x28, d, 0, s);
}

/* VMOVQ: xmm d = r (64 bits), or r = xmm s */
static inline void x86_vmovq_from(struct x86_buf *b, unsigned xd, unsigned r)
{
	x86_vex_rr(b, 1, 1, 1, 0x6E, xd, 0, r);
}

static inline void x86_vmovq_to(struct x86_buf *b, unsigned r, unsigned xs)
{
	x86_vex_rr(b, 1, 1, 1, 0x7E, xs, 0, r);
}

/* VMOVD: the same with 32 bits */
static inline void x86_vmovd_from(struct x86_buf *b, unsigned xd, unsigned r)
{
	x86_vex_rr(b, 1, 1, 0, 0x6E, xd, 0, r);
}

static inline void x86_vmovd_to(struct x86_buf *b, unsigned r, unsigned xs)
{
	x86_vex_rr(b, 1, 1, 0, 0x7E, xs, 0, r);
}

/* VMOVSD xmm, [m] or [m], xmm */
static inline void x86_vmovsd(struct x86_buf *b, bool store, unsigned x, struct x86_mem m)
{
	x86_vex_rm(b, 1, 3, 0, store ? 0x11 : 0x10, x, 0, m, 0);
}

/* VUCOMISD a, s */
static inline void x86_vucomisd(struct x86_buf *b, unsigned a, unsigned s)
{
	x86_vex_rr(b, 1, 1, 0, 0x2E, a, 0, s);
}

enum x86_pd { X86_VANDPD = 0x54, X86_VANDNPD, X86_VORPD, X86_VXORPD };

static inline void x86_vpd(struct x86_buf *b, enum x86_pd op, unsigned d, unsigned a, unsigned s)
{
	x86_vex_rr(b, 1, 1, 0, op, d, a, s);
}

/* ... with 16 bytes at m */
static inline void x86_vpd_m(struct x86_buf *b, enum x86_pd op, unsigned d, unsigned a, struct x86_mem m)
{
	x86_vex_rm(b, 1, 1, 0, op, d, a, m, 0);
}

/* VCMPSD d, a, s, predicate (0 EQ, 1 LT, 2 LE, ...): all ones where it holds */
static inline void x86_vcmpsd(struct x86_buf *b, unsigned d, unsigned a, unsigned s, unsigned pred)
{
	x86_vex_rr(b, 1, 3, 0, 0xC2, d, a, s);
	x86_byte(b, pred);
}

/* VBLENDVPD d, a, s, mask: s where mask's sign bits are set, else a */
static inline void x86_vblendvpd(struct x86_buf *b, unsigned d, unsigned a, unsigned s, unsigned mask)
{
	x86_vex_rr(b, 3, 1, 0, 0x4B, d, a, s);
	x86_byte(b, mask << 4);
}

/* FMA3, 231 forms: d = a * s + d (VFMADD), a * s - d (VFMSUB), -(a * s) + d, -(a * s) - d */
enum x86_fma { X86_VFMADD231SD = 0xB9, X86_VFMSUB231SD = 0xBB, X86_VFNMADD231SD = 0xBD, X86_VFNMSUB231SD = 0xBF };

static inline void x86_vfma(struct x86_buf *b, enum x86_fma op, unsigned d, unsigned a, unsigned s)
{
	x86_vex_rr(b, 2, 1, 1, op, d, a, s);
}

/* d = s converted between double and single (VCVTSD2SS, VCVTSS2SD), upper half from a */
static inline void x86_vcvt(struct x86_buf *b, bool to_single, unsigned d, unsigned a, unsigned s)
{
	x86_vex_rr(b, 1, to_single ? 3 : 2, 0, 0x5A, d, a, s);
}

/* VROUNDSD d, a, s, mode (0 nearest even, 3 toward zero; 8: no precision exception) */
static inline void x86_vroundsd(struct x86_buf *b, unsigned d, unsigned a, unsigned s, unsigned mode)
{
	x86_vex_rr(b, 3, 1, 0, 0x0B, d, a, s);
	x86_byte(b, mode);
}

/* VCVTTSD2SI r, xmm (truncating) and VCVTSI2SD xmm, a, r */
static inline void x86_vcvttsd2si(struct x86_buf *b, unsigned w, unsigned r, unsigned x)
{
	x86_vex_rr(b, 1, 3, w, 0x2C, r, 0, x);
}

static inline void x86_vcvtsi2sd(struct x86_buf *b, unsigned w, unsigned x, unsigned a, unsigned r)
{
	x86_vex_rr(b, 1, 3, w, 0x2A, x, a, r);
}

#endif
