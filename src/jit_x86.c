/* The JIT's translator for x86-64 hosts (see jit_int.h), for processors with x86-64-v3 */
#define _GNU_SOURCE                     /* REG_RIP */
#include "jit_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef JIT_X86

#include <cpuid.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

#include "x86.h"

#define JIT_DATA (64u << 10)            /* struct jit, before the code (its own 16 KB pages, for Rosetta) */
#define BLOCK_RESERVE (256u << 10)      /* free code space needed to start a block */
#define BLOCK_MAX_BYTES (32u << 10)     /* stop growing a block beyond this */
#define MAX_FIXUPS 1024u
#define FRAME 104u                      /* generated code's stack frame (keeping rsp 16-byte aligned) */

_Static_assert(sizeof(struct jit) <= JIT_DATA, "struct jit fits before the code");

/*
 * Host registers held for the whole of a run. jit_state needs none: struct
 * jit sits just before the code, so generated code reaches it RIP-relative.
 */
enum {
	R_CPU = X86_RBX,        /* struct ppc_cpu * */
	R_BUDGET = X86_R14,     /* instructions that may still run */
	R_MEM = X86_R15,        /* guest address 0, in generated code's view (see fastmem) */
};

/* Scratch, free within an instruction (and the implicit operands of division) */
#define T0 X86_RAX
#define T1 X86_RCX
#define T2 X86_RDX

/* ---- code emission ------------------------------------------------------- */

enum { FIX_REL32, FIX_SITE };

/*
 * Guest registers in host registers, as on AArch64 (see jit_a64.c): the
 * hottest fixed for the whole run, the others cached within a block.
 * Their upper halves are always zero (32-bit operations clear them), so a
 * guest value is good as a 64-bit index. Register indices: GPRs 0-31,
 * then these.
 */
#define G_CR 32u
#define G_XER 33u
#define G_LR 34u
#define G_CTR 35u
#define NGUEST 36u
#define NFIXED 5u
#define CALLER_SAVED_FIXED 2u           /* the first fixed registers don't survive calls to C */
#define NCACHE 4u                       /* (none of which survive calls to C) */

static const uint8_t fixed_guest[NFIXED] = { 4, G_LR, G_CR, 24, 3 };
static const uint8_t fixed_reg[NFIXED] = { X86_R10, X86_R11, X86_RBP, X86_R12, X86_R13 };
static const uint8_t cache_reg[NCACHE] = { X86_RSI, X86_RDI, X86_R8, X86_R9 };

#define NO_REG 0xFFu

/* The host register a guest register is fixed in, or NO_REG */
static unsigned fixed_host(unsigned g)
{
	for (unsigned k = 0; k < NFIXED; k++)
		if (fixed_guest[k] == g)
			return fixed_reg[k];
	return NO_REG;
}

struct cache_state {
	int8_t guest[NCACHE];           /* guest register in each slot, or -1 */
	uint8_t dirty;                  /* slots not yet written back */
};

struct fixup {
	uint8_t *at;                    /* rel32 field, or the faulting instruction (FIX_SITE) */
	unsigned type;
	unsigned k;                     /* side exit: instructions retired before it */
	bool after;                     /* leave after instruction k instead (it has completed) */
	struct cache_state cs;          /* registers to write back on the way out */
};

#define MAX_LINKS 160u

/* A conditional branch the block continues past: its taken path, out of line */
struct branch_exit {
	uint8_t *at;                    /* the rel32 field of the jump to it */
	unsigned k;                     /* the branch's index in the block */
	uint32_t target;
	struct cache_state cs;          /* registers to write back on the way out */
};

/* Exit to a known pc */
struct link {
	uint8_t *site;                  /* rel32 field of a JMP, first to a stub that asks for the link */
	uint32_t target;
	bool after_branch;
};

struct tx {
	struct jit *j;
	struct x86_buf x;
	unsigned max_insns;
	uint32_t pc;                    /* instruction being translated */
	uint32_t insn;                  /* ... its word */
	unsigned k;                     /* its index in the block */
	uint32_t pcs[MAX_BLOCK_INSNS + 1];      /* the pc of each (the block may follow branches) */
	struct fixup fix[MAX_FIXUPS];
	unsigned nfix;
	struct link links[MAX_LINKS];
	unsigned nlinks;
	struct branch_exit bexits[MAX_BLOCK_INSNS];
	unsigned nbexits;
	struct cache_state rc;
	int8_t slot_of[NGUEST];
	uint32_t used[NCACHE], tick;
	unsigned pins;                  /* slots the current instruction uses */
};

#define B (&t->x)
#define OFF(field) ((int32_t)offsetof(struct ppc_cpu, field))
#define GPR(r) (OFF(gpr) + 4 * (int32_t)(r))
#define ST(field) x86_rip(&t->j->st.field)

static void ld32(struct tx *t, unsigned reg, int32_t off) { x86_load(B, 4, false, reg, x86_m(R_CPU, off)); }
static void st32(struct tx *t, unsigned reg, int32_t off) { x86_store(B, 4, reg, x86_m(R_CPU, off)); }

static void mov_rr(struct tx *t, unsigned d, unsigned s)
{
	if (d != s)
		x86_mov_rr(B, 0, d, s);
}

/* ---- guest register cache ------------------------------------------------ */

static int32_t guest_off(unsigned g)
{
	switch (g) {
	case G_CR:  return OFF(cr);
	case G_XER: return OFF(xer);
	case G_LR:  return OFF(lr);
	case G_CTR: return OFF(ctr);
	default:    return GPR(g);
	}
}

static void rc_reset(struct tx *t)
{
	memset(t->rc.guest, -1, sizeof t->rc.guest);
	t->rc.dirty = 0;
	memset(t->slot_of, -1, sizeof t->slot_of);
	t->pins = 0;
}

/* Writes back the registers dirty in `cs` */
static void rc_store(struct tx *t, const struct cache_state *cs)
{
	for (unsigned s = 0; s < NCACHE; s++)
		if (cs->dirty & (1u << s))
			st32(t, cache_reg[s], guest_off((unsigned)cs->guest[s]));
}

static void rc_flush(struct tx *t)
{
	rc_store(t, &t->rc);
	t->rc.dirty = 0;
}

/* A slot for guest register g: a free one, or the least recently used (written back) */
static unsigned rc_alloc(struct tx *t, unsigned g)
{
	unsigned best = NCACHE;
	for (unsigned s = 0; s < NCACHE; s++) {
		if (t->pins & (1u << s))
			continue;
		if (t->rc.guest[s] < 0) {
			best = s;
			break;
		}
		if (best == NCACHE || t->used[s] < t->used[best])
			best = s;
	}
	if (best == NCACHE) {                   /* (an instruction uses at most four: a translator bug) */
		fprintf(stderr, "jit: no register for r%u at %08X\n", g, t->pc);
		abort();
	}
	int8_t old = t->rc.guest[best];
	if (old >= 0) {
		if (t->rc.dirty & (1u << best))
			st32(t, cache_reg[best], guest_off((unsigned)old));
		t->slot_of[old] = -1;
	}
	t->rc.guest[best] = (int8_t)g;
	t->rc.dirty &= (uint8_t)~(1u << best);
	t->slot_of[g] = (int8_t)best;
	return best;
}

static unsigned rc_use(struct tx *t, unsigned s)
{
	t->pins |= 1u << s;
	t->used[s] = ++t->tick;
	return cache_reg[s];
}

/* A host register holding guest register g */
static unsigned rd(struct tx *t, unsigned g)
{
	unsigned h = fixed_host(g);
	if (h != NO_REG)
		return h;
	int s = t->slot_of[g];
	if (s < 0) {
		s = (int)rc_alloc(t, g);
		ld32(t, cache_reg[s], guest_off(g));
	}
	return rc_use(t, (unsigned)s);
}

/* A host register for guest register g's new value (all 32 bits must be written) */
static unsigned wr(struct tx *t, unsigned g)
{
	unsigned h = fixed_host(g);
	if (h != NO_REG)
		return h;
	int s = t->slot_of[g];
	if (s < 0)
		s = (int)rc_alloc(t, g);
	t->rc.dirty |= (uint8_t)(1u << s);
	return rc_use(t, (unsigned)s);
}

/* Guest register g, to be changed in place */
static unsigned rw(struct tx *t, unsigned g)
{
	unsigned h = rd(t, g);
	if (fixed_host(g) == NO_REG)
		t->rc.dirty |= (uint8_t)(1u << t->slot_of[g]);
	return h;
}

/* Where guest register g is to be read now without caching it: its host register, or NO_REG and *m */
static unsigned rd_where(struct tx *t, unsigned g, struct x86_mem *m)
{
	unsigned h = fixed_host(g);
	if (h != NO_REG)
		return h;
	int s = t->slot_of[g];
	if (s >= 0)
		return rc_use(t, (unsigned)s);
	*m = x86_m(R_CPU, guest_off(g));
	return NO_REG;
}

/* The fixed registers to and from memory, before and after calls into C */
static void fixed_store(struct tx *t, unsigned count)
{
	for (unsigned k = 0; k < count; k++)
		st32(t, fixed_reg[k], guest_off(fixed_guest[k]));
}

static void fixed_load(struct tx *t, unsigned count)
{
	for (unsigned k = 0; k < count; k++)
		ld32(t, fixed_reg[k], guest_off(fixed_guest[k]));
}

/* ---- side exits: the interpreter takes over at the current instruction ------ */

static void add_fixup(struct tx *t, unsigned type, uint8_t *at)
{
	if (t->nfix < MAX_FIXUPS)
		t->fix[t->nfix++] = (struct fixup){ at, type, t->k, false, t->rc };
	else
		t->x.overflow = true;
}

static void exit_if(struct tx *t, enum x86_cond cc)
{
	add_fixup(t, FIX_REL32, x86_jcc(B, cc, NULL));
}

static void exit_always(struct tx *t)
{
	add_fixup(t, FIX_REL32, x86_jmp(B, NULL));
}

/* If translated code changed (flag set by code_changed()), leave after the current instruction */
static void exit_if_code_changed(struct tx *t)
{
	x86_alu_mi(B, X86_CMP, 4, ST(code_changed), 0);
	exit_if(t, X86_NE);
	if (t->nfix)
		t->fix[t->nfix - 1].after = true;
}

/* A jump within the instruction, to land() at the code to come */
static uint8_t *fwd(struct tx *t, enum x86_cond cc)
{
	return x86_jcc(B, cc, NULL);
}

static uint8_t *fwd_always(struct tx *t)
{
	return x86_jmp(B, NULL);
}

static void land(struct tx *t, uint8_t *at)
{
	if (at && !t->x.overflow)
		x86_patch32(at, t->x.p);
}

/* ---- condition register and XER ----------------------------------------- */

/* CR field `f` from the host flags of a compare, with SO from XER */
static void cr_from_flags(struct tx *t, unsigned f, bool is_signed)
{
	x86_mov_ri(B, T0, CR_EQ);
	x86_mov_ri(B, T1, CR_GT);
	x86_mov_ri(B, T2, CR_LT);
	x86_cmov(B, is_signed ? X86_G : X86_A, 0, T0, T1);
	x86_cmov(B, is_signed ? X86_L : X86_B, 0, T0, T2);
	struct x86_mem m;
	unsigned xer = rd_where(t, G_XER, &m);
	if (xer != NO_REG)
		x86_bt_ri(B, X86_BT, 0, xer, 31);
	else
		x86_bt_mi(B, X86_BT, m, 31);
	x86_alu_ri(B, X86_ADC, 0, T0, 0);
	unsigned sh = 28 - 4 * f, cr = rw(t, G_CR);
	if (sh)
		x86_shift_ri(B, X86_SHL, 0, T0, sh);
	x86_alu_ri(B, X86_AND, 0, cr, (int32_t)~(0xFu << sh));
	x86_alu_rr(B, X86_OR, 0, cr, T0);
}

/* Rc = 1: CR0 from the result in `reg` */
static void record(struct tx *t, unsigned reg)
{
	x86_test_rr(B, 0, reg, reg);
	cr_from_flags(t, 0, true);
}

/*
 * XER[CA] from the 0 or 1 in T1's low byte (if `ca`), XER[OV] and SO from
 * T2's (if `ov`)
 */
static void xer_insert(struct tx *t, bool ca, bool ov)
{
	if (!ca && !ov)
		return;
	unsigned xer = rw(t, G_XER);
	if (ca) {
		x86_extend(B, 1, false, 0, T1, T1);
		x86_shift_ri(B, X86_SHL, 0, T1, 29);
		x86_alu_ri(B, X86_AND, 0, xer, (int32_t)~XER_CA);
		x86_alu_rr(B, X86_OR, 0, xer, T1);
	}
	if (ov) {
		x86_extend(B, 1, false, 0, T2, T2);
		x86_shift_ri(B, X86_SHL, 0, T2, 30);
		x86_alu_ri(B, X86_AND, 0, xer, (int32_t)~XER_OV);
		x86_alu_rr(B, X86_OR, 0, xer, T2);
		x86_alu_rr(B, X86_ADD, 0, T2, T2);
		x86_alu_rr(B, X86_OR, 0, xer, T2);
	}
}

/* The host's flags into T1 (CA: the carry, or `ca_cc`) and T2 (OV), for xer_insert() */
static void xer_flags(struct tx *t, bool ca, enum x86_cond ca_cc, bool ov)
{
	if (ca)
		x86_setcc(B, ca_cc, T1);
	if (ov)
		x86_setcc(B, X86_O, T2);
}

/* Host carry = XER[CA] */
static void carry_from_xer(struct tx *t)
{
	struct x86_mem m;
	unsigned xer = rd_where(t, G_XER, &m);
	if (xer != NO_REG)
		x86_bt_ri(B, X86_BT, 0, xer, 29);
	else
		x86_bt_mi(B, X86_BT, m, 29);
}

/* ---- instructions --------------------------------------------------------- */

enum { EMITTED, BRANCHED, STOPPED, EXITED };

static uint32_t ppc_mask(unsigned mb, unsigned me)
{
	uint32_t a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
	return mb <= me ? (a & b) : (a | b);
}

/*
 * The interpreter executes it, from inside the block. If it stored over
 * translated code (perhaps this block's own), leave right after it.
 */
static void call_step(struct tx *t, uint32_t insn)
{
	rc_flush(t);
	fixed_store(t, NFIXED);
	x86_mov_rr(B, 1, X86_RDI, R_CPU);
	x86_mov_ri(B, X86_RSI, insn);
	x86_mov_ri(B, X86_RDX, t->pc);
	x86_call_m(B, ST(step));
	fixed_load(t, NFIXED);          /* it may have changed any register */
	rc_reset(t);
	x86_test_ri(B, 0, X86_RAX, 0xFF);
	exit_if(t, X86_E);
	exit_if_code_changed(t);
}

/* d = a OP b (OP commutative or not), the flags OP's */
static void alu3(struct tx *t, enum x86_alu op, bool commutative, unsigned d, unsigned a, unsigned b)
{
	if (d == a) {
		x86_alu_rr(B, op, 0, d, b);
	} else if (d == b && commutative) {
		x86_alu_rr(B, op, 0, d, a);
	} else if (d == b) {
		x86_mov_rr(B, 0, T0, a);
		x86_alu_rr(B, op, 0, T0, b);
		x86_mov_rr(B, 0, d, T0);
	} else {
		x86_mov_rr(B, 0, d, a);
		x86_alu_rr(B, op, 0, d, b);
	}
}

/* d = s OP imm */
static void alu_imm(struct tx *t, enum x86_alu op, unsigned d, unsigned s, uint32_t imm)
{
	mov_rr(t, d, s);
	x86_alu_ri(B, op, 0, d, (int32_t)imm);
}

/*
 * d = rotl(s, sh) & mask(mb, me); for rlwimi (insert), d keeps the bits
 * outside the mask
 */
static void rotate_mask(struct tx *t, bool insert, unsigned d, unsigned s, unsigned sh, unsigned mb, unsigned me)
{
	uint32_t mask = ppc_mask(mb, me);
	unsigned r = (32 - sh) & 31;            /* as a right rotation */

	if (insert) {
		if (mask == 0xFFFFFFFFu) {
			x86_rorx(B, 0, d, s, r);
			return;
		}
		x86_rorx(B, 0, T0, s, r);
		x86_alu_rr(B, X86_XOR, 0, T0, d);
		x86_alu_ri(B, X86_AND, 0, T0, (int32_t)mask);
		x86_alu_rr(B, X86_XOR, 0, d, T0);
		return;
	}
	if (mask == 0xFFFFFFFFu) {
		if (r)
			x86_rorx(B, 0, d, s, r);
		else
			mov_rr(t, d, s);
	} else if (!r && (mask == 0xFF || mask == 0xFFFF)) {
		x86_extend(B, mask == 0xFF ? 1 : 2, false, 0, d, s);
	} else if (!r) {
		alu_imm(t, X86_AND, d, s, mask);
	} else if (mask == 0xFFFFFFFFu << sh) {
		mov_rr(t, d, s);
		x86_shift_ri(B, X86_SHL, 0, d, sh);
	} else if (mask == 0xFFFFFFFFu >> r) {
		mov_rr(t, d, s);
		x86_shift_ri(B, X86_SHR, 0, d, r);
	} else {
		x86_rorx(B, 0, d, s, r);
		x86_alu_ri(B, X86_AND, 0, d, (int32_t)mask);
	}
}

/*
 * Goes on at the pc in eax through the block table (a branch: the hook's
 * entry stops there). Inline, so that each such exit has its own indirect
 * branch for the host to predict.
 */
static void lookup_jump(struct tx *t)
{
	x86_mov_rr(B, 0, T1, T0);
	x86_shift_ri(B, X86_SHR, 0, T1, MEM_PAGE_SHIFT);
	x86_load(B, 8, false, T2, ST(l1));
	x86_load(B, 8, false, T2, x86_mx(T2, T1, 3, 0));
	x86_mov_rr(B, 0, T1, T0);
	x86_alu_ri(B, X86_AND, 0, T1, (MEM_PAGE_SIZE - 1) & ~3u);
	x86_jmp_m(B, x86_mx(T2, T1, 1, 0));
}

static void budget_sub(struct tx *t, unsigned n)
{
	if (n)
		x86_alu_ri(B, X86_SUB, 1, R_BUDGET, (int32_t)n);
}

/* Leaves the block for the pc in eax (after a branch: stop at the hook) */
static void finish(struct tx *t, unsigned n, bool after_branch)
{
	rc_flush(t);
	budget_sub(t, n);
	if (!after_branch) {                    /* (rare) the table's hook entry is only for branches */
		x86_jmp(B, t->j->miss_exit);
		return;
	}
	lookup_jump(t);
}

/* Leaves the block for `next`: a jump, patched to the block there once it exists */
static void finish_at(struct tx *t, unsigned n, uint32_t next, bool after_branch)
{
	if (t->nlinks >= MAX_LINKS) {
		x86_mov_ri(B, T0, next);
		finish(t, n, after_branch);
		return;
	}
	rc_flush(t);
	budget_sub(t, n);
	uint8_t *site = x86_jmp(B, NULL);
	t->links[t->nlinks++] = (struct link){ site, next, after_branch };
}

/* TEST of CR bit bi (cr: its host register) */
static void test_cr(struct tx *t, unsigned cr, unsigned bi)
{
	x86_test_ri(B, 0, cr, 1u << (31 - bi));
}

/*
 * Conditional branch: decrements CTR if BO says so, then tests CTR and/or
 * the CR bit. The taken path continues at `target` (or at the pc in eax
 * if `in_t0`), the other at pc + 4.
 */
static void cond_branch(struct tx *t, unsigned bo, unsigned bi, bool use_ctr, uint32_t target, bool in_t0)
{
	uint8_t *nt[2];
	unsigned nnt = 0, n = t->k + 1, ctr = NO_REG, cr = NO_REG;

	if (use_ctr && !(bo & 4)) {
		ctr = rw(t, G_CTR);
		x86_inc(B, true, 0, ctr);
	}
	if (!(bo & 16))
		cr = rd(t, G_CR);
	rc_flush(t);
	if (use_ctr && !(bo & 4)) {
		x86_test_rr(B, 0, ctr, ctr);
		nt[nnt++] = fwd(t, bo & 2 ? X86_NE : X86_E);    /* not taken if CTR != 0 for bdz, == 0 for bdnz */
	}
	if (!(bo & 16)) {
		test_cr(t, cr, bi);
		nt[nnt++] = fwd(t, bo & 8 ? X86_E : X86_NE);
	}
	if (in_t0)
		finish(t, n, true);
	else
		finish_at(t, n, target, true);
	if (nnt) {
		for (unsigned k = 0; k < nnt; k++)
			land(t, nt[k]);
		finish_at(t, n, t->pc + 4, true);
	}
}

/*
 * A conditional branch (no link) that the block continues past, on its
 * not-taken path, with the registers still cached: only the taken path
 * leaves the block, through out-of-line code (branch_exits()) that writes
 * back what is dirty here. Unless the next instruction is the hook.
 */
static bool cond_exit(struct tx *t, unsigned bo, unsigned bi, uint32_t target)
{
	if ((bo & 20) == 20 || t->nbexits >= MAX_BLOCK_INSNS || t->pc + 4 == t->j->linked_hook ||
	    t->k + 2 >= t->max_insns)
		return false;
	unsigned ctr = NO_REG, cr = NO_REG;
	if (!(bo & 4)) {
		ctr = rw(t, G_CTR);
		x86_inc(B, true, 0, ctr);
	}
	if (!(bo & 16))
		cr = rd(t, G_CR);
	struct branch_exit *x = &t->bexits[t->nbexits++];
	x->k = t->k;
	x->target = target;
	x->cs = t->rc;
	uint8_t *skip = NULL;
	if (!(bo & 4)) {
		x86_test_rr(B, 0, ctr, ctr);
		if (!(bo & 16))
			skip = fwd(t, bo & 2 ? X86_NE : X86_E);         /* not taken: CTR says no */
		else
			x->at = x86_jcc(B, bo & 2 ? X86_E : X86_NE, NULL);
	}
	if (!(bo & 16)) {
		test_cr(t, cr, bi);
		x->at = x86_jcc(B, bo & 8 ? X86_NE : X86_E, NULL);
	}
	land(t, skip);
	return true;
}

static void branch_exits(struct tx *t)
{
	for (unsigned e = 0; e < t->nbexits && !t->x.overflow; e++) {
		struct branch_exit *x = &t->bexits[e];
		land(t, x->at);
		t->rc = x->cs;
		finish_at(t, x->k + 1, x->target, true);
	}
}

static void set_lr(struct tx *t)
{
	x86_mov_ri(B, wr(t, G_LR), t->pc + 4);
}

static int branch(struct tx *t, uint32_t i)
{
	unsigned bo = (i >> 21) & 31, bi = (i >> 16) & 31;
	uint32_t pc = t->pc;

	switch (i >> 26) {
	case 18: {                                                      /* b */
		uint32_t li = i & 0x03FFFFFCu;
		if (li & 0x02000000u)
			li |= 0xFC000000u;
		if (i & 1)
			set_lr(t);
		finish_at(t, t->k + 1, (i & 2) ? li : pc + li, true);
		return BRANCHED;
	}
	case 16: {                                                      /* bc */
		uint32_t bd = (uint32_t)(int32_t)(int16_t)(i & 0xFFFC);
		if (!(i & 1) && cond_exit(t, bo, bi, (i & 2) ? bd : pc + bd))
			return EMITTED;
		if (i & 1)
			set_lr(t);
		cond_branch(t, bo, bi, true, (i & 2) ? bd : pc + bd, false);
		return BRANCHED;
	}
	default: {                                                      /* bclr, bcctr */
		bool lr = ((i >> 1) & 0x3FF) == 16;
		x86_mov_rr(B, 0, T0, rd(t, lr ? G_LR : G_CTR));
		x86_alu_ri(B, X86_AND, 0, T0, ~3);
		if (i & 1)
			set_lr(t);
		cond_branch(t, bo, bi, lr, 0, true);
		return BRANCHED;
	}
	}
}

/*
 * rax = the time base as the interpreter has it: the value when generated
 * code was entered, plus the step for each instruction retired since
 * (those before this block's, then this block's up to here). Uses rcx, rdx.
 */
static void time_base(struct tx *t)
{
	x86_load(B, 8, false, T0, x86_m(R_CPU, OFF(tb)));
	x86_load(B, 8, false, T1, ST(entry_budget));
	x86_alu_rr(B, X86_SUB, 1, T1, R_BUDGET);
	if (t->k)
		x86_alu_ri(B, X86_ADD, 1, T1, (int32_t)t->k);
	x86_load(B, 4, false, T2, x86_m(R_CPU, OFF(tb_step)));
	x86_imul_rr(B, 1, T1, T2);
	x86_alu_rr(B, X86_ADD, 1, T0, T1);
}

/* rD = the time base (its upper half if `high`) */
static void read_tb(struct tx *t, unsigned rd_, bool high)
{
	time_base(t);
	unsigned d = wr(t, rd_);
	if (high)
		x86_shift_ri(B, X86_SHR, 1, T0, 32);
	x86_mov_rr(B, 0, d, T0);
}

/* divw, divwu: bad divisors leave rA (the interpreter's choice), without a host divide error */
static void divide(struct tx *t, bool is_signed, unsigned d, unsigned a, unsigned b)
{
	x86_mov_rr(B, 0, T1, b);
	x86_mov_rr(B, 0, T0, a);
	uint8_t *neg = NULL;
	if (is_signed) {                        /* a / -1 = -a (INT_MIN / -1 = INT_MIN = rA) */
		x86_alu_ri(B, X86_CMP, 0, T1, -1);
		neg = fwd(t, X86_E);
	}
	x86_test_rr(B, 0, T1, T1);
	x86_mov_ri(B, T2, 1);
	x86_cmov(B, X86_E, 0, T1, T2);
	if (is_signed) {
		x86_cdq(B, 0);
		x86_unary(B, X86_IDIV, 0, T1);
		uint8_t *done = fwd_always(t);
		land(t, neg);
		x86_unary(B, X86_NEG, 0, T0);
		land(t, done);
	} else {
		x86_alu_rr(B, X86_XOR, 0, T2, T2);
		x86_unary(B, X86_DIV, 0, T1);
	}
	x86_mov_rr(B, 0, d, T0);
}

/* XO-form arithmetic (opcode 31); false if the interpreter should do it */
static bool arith(struct tx *t, uint32_t i)
{
	unsigned rd_ = (i >> 21) & 31, ra = (i >> 16) & 31, rb = (i >> 11) & 31;
	unsigned xo = (i >> 1) & 0x1FF;
	bool oe = (i >> 10) & 1;
	unsigned a, b, d;

	switch (xo) {
	case 266:                                                       /* add */
		a = rd(t, ra);
		b = rd(t, rb);
		d = wr(t, rd_);
		if (!oe) {
			x86_lea(B, 0, d, x86_mx(a, b, 0, 0));
			break;
		}
		x86_mov_rr(B, 0, T0, a);
		x86_alu_rr(B, X86_ADD, 0, T0, b);
		xer_flags(t, false, X86_B, true);
		x86_mov_rr(B, 0, d, T0);
		xer_insert(t, false, true);
		break;
	case 10:                                                        /* addc */
	case 40:                                                        /* subf: b - a */
	case 8:                                                         /* subfc */
		a = rd(t, ra);
		b = rd(t, rb);
		d = wr(t, rd_);
		if (xo == 10) {
			x86_mov_rr(B, 0, T0, a);
			x86_alu_rr(B, X86_ADD, 0, T0, b);
		} else {
			x86_mov_rr(B, 0, T0, b);
			x86_alu_rr(B, X86_SUB, 0, T0, a);
		}
		xer_flags(t, xo != 40, xo == 10 ? X86_B : X86_AE, oe);
		x86_mov_rr(B, 0, d, T0);
		xer_insert(t, xo != 40, oe);
		break;
	case 104:                                                       /* neg */
		a = rd(t, ra);
		d = wr(t, rd_);
		x86_mov_rr(B, 0, T0, a);
		x86_unary(B, X86_NEG, 0, T0);
		xer_flags(t, false, X86_B, oe);
		x86_mov_rr(B, 0, d, T0);
		xer_insert(t, false, oe);
		break;
	case 138: case 136: case 202: case 234: case 200: case 232: {
		/* a (or ~a for subf*) + b (or 0, or -1) + CA */
		bool inv = xo == 136 || xo == 200 || xo == 232;
		a = rd(t, ra);
		b = xo == 138 || xo == 136 ? rd(t, rb) : NO_REG;
		d = wr(t, rd_);
		x86_mov_rr(B, 0, T0, a);
		if (inv)
			x86_unary(B, X86_NOT, 0, T0);
		carry_from_xer(t);
		if (b != NO_REG)
			x86_alu_rr(B, X86_ADC, 0, T0, b);
		else
			x86_alu_ri(B, X86_ADC, 0, T0, xo == 234 || xo == 232 ? -1 : 0);
		xer_flags(t, true, X86_B, oe);
		x86_mov_rr(B, 0, d, T0);
		xer_insert(t, true, oe);
		break;
	}
	case 235:                                                       /* mullw */
		a = rd(t, ra);
		b = rd(t, rb);
		d = wr(t, rd_);
		x86_mov_rr(B, 0, T0, a);
		x86_imul_rr(B, 0, T0, b);
		xer_flags(t, false, X86_B, oe);
		x86_mov_rr(B, 0, d, T0);
		xer_insert(t, false, oe);
		break;
	case 491: case 459:                                             /* divw, divwu */
		if (oe)
			return false;
		a = rd(t, ra);
		b = rd(t, rb);
		d = wr(t, rd_);
		divide(t, xo == 491, d, a, b);
		break;
	case 75:                                                        /* mulhw */
		a = rd(t, ra);
		b = rd(t, rb);
		d = wr(t, rd_);
		x86_extend(B, 4, true, 1, T0, a);
		x86_extend(B, 4, true, 1, T1, b);
		x86_imul_rr(B, 1, T0, T1);
		x86_shift_ri(B, X86_SHR, 1, T0, 32);
		x86_mov_rr(B, 0, d, T0);
		break;
	case 11:                                                        /* mulhwu (upper halves are zero) */
		a = rd(t, ra);
		b = rd(t, rb);
		d = wr(t, rd_);
		x86_mov_rr(B, 0, T0, a);
		x86_imul_rr(B, 1, T0, b);
		x86_shift_ri(B, X86_SHR, 1, T0, 32);
		x86_mov_rr(B, 0, d, T0);
		break;
	default:
		return false;
	}
	if (i & 1)
		record(t, d);
	return true;
}

/* slw, srw, sraw: 64-bit shifts, whose 6-bit count is PowerPC's (rB & 63) */
static void shift(struct tx *t, unsigned xo, unsigned d, unsigned s, unsigned b)
{
	if (xo == 536) {                                                /* srw */
		x86_shiftx(B, X86_SHR, 1, d, s, b);
		return;
	}
	if (xo == 24) {                                                 /* slw */
		x86_shiftx(B, X86_SHL, 1, d, s, b);
		x86_mov_rr(B, 0, d, d);
		return;
	}
	/* sraw: CA if negative and ones were shifted out (all of rS from 32 on) */
	x86_mov_rr(B, 0, T1, b);
	x86_alu_ri(B, X86_AND, 0, T1, 63);
	x86_bzhi(B, 0, T2, s, T1);                      /* the bits shifted out */
	x86_unary(B, X86_NEG, 0, T2);                   /* carry: any */
	x86_alu_rr(B, X86_SBB, 0, T2, T2);
	x86_alu_rr(B, X86_AND, 0, T2, s);               /* sign: CA */
	x86_extend(B, 4, true, 1, T0, s);
	x86_shiftx(B, X86_SAR, 1, T0, T0, T1);
	unsigned xer = rw(t, G_XER);
	x86_shift_ri(B, X86_SHR, 0, T2, 31);
	x86_shift_ri(B, X86_SHL, 0, T2, 29);
	x86_alu_ri(B, X86_AND, 0, xer, (int32_t)~XER_CA);
	x86_alu_rr(B, X86_OR, 0, xer, T2);
	x86_mov_rr(B, 0, d, T0);
}

static int insn31(struct tx *t, uint32_t i)
{
	unsigned rd_ = (i >> 21) & 31, ra = (i >> 16) & 31, rb = (i >> 11) & 31, rc = i & 1;
	unsigned xo = (i >> 1) & 0x3FF;

	if (arith(t, i))
		return EMITTED;

	switch (xo) {
	case 0: case 32: {                                              /* cmp, cmpl */
		unsigned a = rd(t, ra), b = rd(t, rb);
		x86_alu_rr(B, X86_CMP, 0, a, b);
		cr_from_flags(t, (i >> 23) & 7, xo == 0);
		return EMITTED;
	}
	case 371: {                                                     /* mftb */
		unsigned spr = ((i >> 16) & 0x1F) | ((i >> 6) & 0x3E0);
		if (spr != 268 && spr != 269)
			return STOPPED;
		read_tb(t, rd_, spr == 269);
		return EMITTED;
	}
	case 4:                                                         /* tw */
		return STOPPED;
	case 19: {                                                      /* mfcr */
		unsigned cr = rd(t, G_CR);
		x86_mov_rr(B, 0, wr(t, rd_), cr);
		return EMITTED;
	}
	case 144: {                                                     /* mtcrf */
		unsigned crm = (i >> 12) & 0xFF;
		uint32_t mask = 0;
		for (unsigned f = 0; f < 8; f++)
			if (crm & (0x80u >> f))
				mask |= 0xFu << (28 - 4 * f);
		unsigned s = rd(t, rd_);
		if (mask == 0xFFFFFFFFu) {
			x86_mov_rr(B, 0, wr(t, G_CR), s);
		} else if (mask) {
			unsigned cr = rw(t, G_CR);
			x86_mov_rr(B, 0, T0, s);
			x86_alu_ri(B, X86_AND, 0, T0, (int32_t)mask);
			x86_alu_ri(B, X86_AND, 0, cr, (int32_t)~mask);
			x86_alu_rr(B, X86_OR, 0, cr, T0);
		}
		return EMITTED;
	}
	case 339: case 467: {                                           /* mfspr, mtspr */
		unsigned spr = ((i >> 16) & 0x1F) | ((i >> 6) & 0x3E0), g;
		switch (spr) {
		case 1:   g = G_XER; break;
		case 8:   g = G_LR; break;
		case 9:   g = G_CTR; break;
		case 256:                       /* VRSAVE: not cached */
			if (xo == 339)
				ld32(t, wr(t, rd_), OFF(vrsave));
			else
				st32(t, rd(t, rd_), OFF(vrsave));
			return EMITTED;
		case 268: case 269:
			if (xo == 467)
				return STOPPED;         /* (privileged) */
			read_tb(t, rd_, spr == 269);
			return EMITTED;
		default:
			call_step(t, i);        /* privileged or invalid: the kernel's business */
			return EMITTED;
		}
		if (xo == 339) {
			unsigned s = rd(t, g);
			x86_mov_rr(B, 0, wr(t, rd_), s);
		} else {
			unsigned s = rd(t, rd_);
			x86_mov_rr(B, 0, wr(t, g), s);
		}
		return EMITTED;
	}
	case 26: case 922: case 954: {                                  /* cntlzw, extsh, extsb */
		unsigned s = rd(t, rd_), d = wr(t, ra);
		if (xo == 26)
			x86_lzcnt(B, 0, d, s);
		else
			x86_extend(B, xo == 922 ? 2 : 1, true, 0, d, s);
		if (rc)
			record(t, d);
		return EMITTED;
	}
	case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476: {
		unsigned s = rd(t, rd_), d;
		if (xo == 444 && rb == rd_) {                           /* mr */
			d = wr(t, ra);
			mov_rr(t, d, s);
		} else {
			unsigned b = rd(t, rb);
			d = wr(t, ra);
			switch (xo) {
			case 28:  alu3(t, X86_AND, true, d, s, b); break;                       /* and */
			case 60:  x86_andn(B, 0, d, b, s); break;                               /* andc */
			case 124: alu3(t, X86_OR, true, d, s, b); x86_unary(B, X86_NOT, 0, d); break;  /* nor */
			case 284: alu3(t, X86_XOR, true, d, s, b); x86_unary(B, X86_NOT, 0, d); break; /* eqv */
			case 316: alu3(t, X86_XOR, true, d, s, b); break;                       /* xor */
			case 412:                                                               /* orc */
				x86_mov_rr(B, 0, T0, b);
				x86_unary(B, X86_NOT, 0, T0);
				alu3(t, X86_OR, true, d, s, T0);
				break;
			case 444: alu3(t, X86_OR, true, d, s, b); break;                        /* or */
			case 476: alu3(t, X86_AND, true, d, s, b); x86_unary(B, X86_NOT, 0, d); break; /* nand */
			}
		}
		if (rc)
			record(t, d);
		return EMITTED;
	}
	case 24: case 536: case 792: {                                  /* slw, srw, sraw */
		unsigned s = rd(t, rd_), b = rd(t, rb), d = wr(t, ra);
		shift(t, xo, d, s, b);
		if (rc)
			record(t, d);
		return EMITTED;
	}
	case 824: {                                                     /* srawi */
		/* CA: negative and ones shifted out */
		unsigned s = rd(t, rd_), d = wr(t, ra);
		if (rb == 0) {
			mov_rr(t, d, s);
			x86_bt_ri(B, X86_BTR, 0, rw(t, G_XER), 29);
		} else {
			x86_mov_rr(B, 0, T0, s);
			x86_shift_ri(B, X86_SAR, 0, T0, 31);
			x86_alu_rr(B, X86_AND, 0, T0, s);       /* rS if negative, else 0 */
			x86_test_ri(B, 0, T0, (1u << rb) - 1);
			x86_setcc(B, X86_NE, T1);
			x86_mov_rr(B, 0, T0, s);
			x86_shift_ri(B, X86_SAR, 0, T0, rb);
			mov_rr(t, d, T0);
			xer_insert(t, true, false);
		}
		if (rc)
			record(t, d);
		return EMITTED;
	}
	case 54: case 86: case 246: case 278: case 758: case 982: case 598: case 854:
		return EMITTED;         /* dcbst dcbf dcbtst dcbt dcba icbi sync eieio: no caches to manage */
	default:
		call_step(t, i);
		return EMITTED;
	}
}

static int insn19(struct tx *t, uint32_t i)
{
	unsigned bt = (i >> 21) & 31, ba = (i >> 16) & 31, bb = (i >> 11) & 31;
	unsigned xo = (i >> 1) & 0x3FF;

	switch (xo) {
	case 16: case 528:
		return branch(t, i);
	case 0: {                                                       /* mcrf */
		unsigned d = (i >> 23) & 7, s = (i >> 18) & 7, cr = rw(t, G_CR);
		uint32_t mask = 0xFu << (28 - 4 * d);
		x86_rorx(B, 0, T0, cr, (4 * (d - s)) & 31);
		x86_alu_ri(B, X86_AND, 0, T0, (int32_t)mask);
		x86_alu_ri(B, X86_AND, 0, cr, (int32_t)~mask);
		x86_alu_rr(B, X86_OR, 0, cr, T0);
		return EMITTED;
	}
	case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449: {
		unsigned cr = rw(t, G_CR);
		if (ba == bt && bb == bt && (xo == 193 || xo == 289)) { /* crclr, crset */
			x86_bt_ri(B, xo == 193 ? X86_BTR : X86_BTS, 0, cr, 31 - bt);
			return EMITTED;
		}
		x86_rorx(B, 0, T0, cr, 31 - ba);                /* bit ba at bit 0 */
		x86_rorx(B, 0, T1, cr, 31 - bb);
		bool not_after = false;
		switch (xo) {
		case 33:  x86_alu_rr(B, X86_OR, 0, T0, T1); not_after = true; break;           /* crnor */
		case 129: x86_andn(B, 0, T0, T1, T0); break;                                    /* crandc */
		case 193: x86_alu_rr(B, X86_XOR, 0, T0, T1); break;                             /* crxor */
		case 225: x86_alu_rr(B, X86_AND, 0, T0, T1); not_after = true; break;          /* crnand */
		case 257: x86_alu_rr(B, X86_AND, 0, T0, T1); break;                             /* crand */
		case 289: x86_alu_rr(B, X86_XOR, 0, T0, T1); not_after = true; break;          /* creqv */
		case 417: x86_unary(B, X86_NOT, 0, T1); x86_alu_rr(B, X86_OR, 0, T0, T1); break; /* crorc */
		case 449: x86_alu_rr(B, X86_OR, 0, T0, T1); break;                              /* cror */
		}
		if (not_after)
			x86_unary(B, X86_NOT, 0, T0);
		x86_alu_ri(B, X86_AND, 0, T0, 1);
		if (31 - bt)
			x86_shift_ri(B, X86_SHL, 0, T0, 31 - bt);
		x86_bt_ri(B, X86_BTR, 0, cr, 31 - bt);
		x86_alu_rr(B, X86_OR, 0, cr, T0);
		return EMITTED;
	}
	case 150:                                                       /* isync */
		return EMITTED;
	case 50:                                                        /* rfi */
		return STOPPED;
	default:
		call_step(t, i);
		return EMITTED;
	}
}

static int insn(struct tx *t, uint32_t i)
{
	unsigned op = i >> 26, rd_ = (i >> 21) & 31, ra = (i >> 16) & 31;
	int32_t simm = (int16_t)(i & 0xFFFF);
	uint32_t uimm = i & 0xFFFF;

	switch (op) {
	case 3:                                 /* twi, host calls, sc: the interpreter (which may make them in place) */
	case 6:
	case 17:
	case 4:                                 /* AltiVec */
		return STOPPED;
	case 7: {                                                       /* mulli */
		unsigned a = rd(t, ra);
		x86_imul_rri(B, 0, wr(t, rd_), a, simm);
		return EMITTED;
	}
	case 8: {                                                       /* subfic */
		unsigned a = rd(t, ra);
		x86_mov_ri(B, T0, (uint32_t)simm);
		x86_alu_rr(B, X86_SUB, 0, T0, a);
		xer_flags(t, true, X86_AE, false);
		x86_mov_rr(B, 0, wr(t, rd_), T0);
		xer_insert(t, true, false);
		return EMITTED;
	}
	case 10: case 11: {                                             /* cmpli, cmpi */
		bool is_signed = op == 11;
		unsigned a = rd(t, ra);
		x86_alu_ri(B, X86_CMP, 0, a, is_signed ? simm : (int32_t)uimm);
		cr_from_flags(t, (i >> 23) & 7, is_signed);
		return EMITTED;
	}
	case 12: case 13: {                                             /* addic, addic. */
		unsigned a = rd(t, ra);
		x86_mov_rr(B, 0, T0, a);
		x86_alu_ri(B, X86_ADD, 0, T0, simm);
		xer_flags(t, true, X86_B, false);
		unsigned d = wr(t, rd_);
		x86_mov_rr(B, 0, d, T0);
		xer_insert(t, true, false);
		if (op == 13)
			record(t, d);
		return EMITTED;
	}
	case 14: case 15: {                                             /* addi, addis */
		int32_t imm = op == 15 ? (int32_t)(uimm << 16) : simm;
		if (ra) {
			unsigned a = rd(t, ra), d = wr(t, rd_);
			if (imm)
				x86_lea(B, 0, d, x86_m(a, imm));
			else
				mov_rr(t, d, a);
		} else {
			x86_mov_ri(B, wr(t, rd_), (uint32_t)imm);
		}
		return EMITTED;
	}
	case 16: case 18:
		return branch(t, i);
	case 19:
		return insn19(t, i);
	case 20: case 21: {                                             /* rlwimi, rlwinm */
		unsigned sh = (i >> 11) & 31, mb = (i >> 6) & 31, me = (i >> 1) & 31;
		unsigned s = rd(t, rd_);
		unsigned d = op == 20 ? rw(t, ra) : wr(t, ra);
		rotate_mask(t, op == 20, d, s, sh, mb, me);
		if (i & 1)
			record(t, d);
		return EMITTED;
	}
	case 23: {                                                      /* rlwnm */
		unsigned rb = (i >> 11) & 31, mb = (i >> 6) & 31, me = (i >> 1) & 31;
		uint32_t mask = ppc_mask(mb, me);
		unsigned s = rd(t, rd_), b = rd(t, rb);
		x86_mov_rr(B, 0, T1, b);
		x86_mov_rr(B, 0, T0, s);
		x86_shift_cl(B, X86_ROL, 0, T0);
		if (mask != 0xFFFFFFFFu)
			x86_alu_ri(B, X86_AND, 0, T0, (int32_t)mask);
		unsigned d = wr(t, ra);
		x86_mov_rr(B, 0, d, T0);
		if (i & 1)
			record(t, d);
		return EMITTED;
	}
	case 24: case 25: case 26: case 27: {                           /* ori, oris, xori, xoris */
		uint32_t imm = (op & 1) ? uimm << 16 : uimm;
		if (imm == 0 && ra == rd_)
			return EMITTED;                                 /* nop */
		unsigned s = rd(t, rd_), d = wr(t, ra);
		if (imm)
			alu_imm(t, op < 26 ? X86_OR : X86_XOR, d, s, imm);
		else
			mov_rr(t, d, s);
		return EMITTED;
	}
	case 28: case 29: {                                             /* andi., andis. */
		uint32_t imm = op == 29 ? uimm << 16 : uimm;
		unsigned d;
		if (imm) {
			unsigned s = rd(t, rd_);
			d = wr(t, ra);
			alu_imm(t, X86_AND, d, s, imm);
		} else {
			d = wr(t, ra);
			x86_mov_ri(B, d, 0);
		}
		record(t, d);
		return EMITTED;
	}
	case 31:
		return insn31(t, i);
	default:                /* loads and stores, floating point, string operations, ... */
		call_step(t, i);
		return EMITTED;
	}
}

/* ---- blocks ----------------------------------------------------------- */

static bool visited(const struct tx *t, unsigned n, uint32_t pc)
{
	for (unsigned k = 0; k < n; k++)
		if (t->pcs[k] == pc)
			return true;
	return false;
}

/*
 * The match hook (see ppc.h), at the start of the block at its pc: stop
 * there before anything runs if the halfword at the address in its
 * register is its value, as the interpreter would, reading it the same
 * way (a halfword running into a page that isn't readable doesn't match).
 */
static void match_guard(struct tx *t, uint32_t pc0)
{
	struct jit *j = t->j;
	unsigned a = fixed_host(j->match_reg & 31u);
	uint8_t *no[3];

	x86_alu_mi(B, X86_CMP, 1, x86_m(R_CPU, OFF(hook_match_skip)), 0);
	uint8_t *resume = fwd(t, X86_NE);
	if (a == NO_REG)
		ld32(t, T0, GPR(j->match_reg & 31u));
	else
		x86_mov_rr(B, 0, T0, a);
	x86_mov_rr(B, 0, T1, T0);
	x86_shift_ri(B, X86_SHR, 0, T1, MEM_PAGE_SHIFT);
	x86_load(B, 8, false, T2, ST(pageflags));
	x86_load(B, 1, false, T2, x86_mx(T2, T1, 0, 0));
	x86_test_ri(B, 0, T2, MEM_FAST_R);                      /* this page and the next */
	uint8_t *readable = fwd(t, X86_NE);
	x86_test_ri(B, 0, T2, MEM_R);
	no[0] = fwd(t, X86_E);
	x86_mov_rr(B, 0, T1, T0);
	x86_alu_ri(B, X86_AND, 0, T1, MEM_PAGE_SIZE - 1);
	x86_alu_ri(B, X86_CMP, 0, T1, MEM_PAGE_SIZE - 1);
	no[1] = fwd(t, X86_E);
	land(t, readable);
	x86_load(B, 8, false, T2, ST(membase));
	x86_load(B, 2, false, T1, x86_mx(T2, T0, 0, 0));
	x86_alu_ri(B, X86_CMP, 0, T1, __builtin_bswap16(j->match_value));
	no[2] = fwd(t, X86_NE);
	x86_store_imm(B, 1, x86_m(R_CPU, OFF(hook_match_skip)), 1);
	x86_store_imm(B, 4, x86_m(R_CPU, OFF(pc)), pc0);
	x86_mov_ri(B, T0, EXIT_HOOK);
	x86_jmp(B, j->exit);
	land(t, resume);
	x86_store_imm(B, 1, x86_m(R_CPU, OFF(hook_match_skip)), 0);
	for (unsigned k = 0; k < 3; k++)
		land(t, no[k]);
}

/* A block of at most `max_insns` instructions from pc0; NULL if there's no room (and the JIT was flushed) */
static struct jit_block *translate_upto(struct jit *j, uint32_t pc0, unsigned max_insns)
{
	if (!mem_ok(j->mem, pc0, 4, MEM_X))
		return NULL;
	uint32_t page = pc0 >> MEM_PAGE_SHIFT;
	if ((size_t)(j->code_end - j->code_free) < BLOCK_RESERVE || j->nblocks >= MAX_BLOCKS ||
	    (!j->pages[page] && j->ncode_pages + 1 >= MAX_CODE_PAGES) || j->nsites + MAX_BLOCK_INSNS > MAX_SITES)
		jit_flush(j);
	struct jit_page *pg = jit_get_page(j, page);

	static struct tx t;
	t.j = j;
	t.max_insns = max_insns;
	uint8_t *start = (uint8_t *)(((uintptr_t)j->code_free + sizeof(struct jit_block *) + 15) & ~(uintptr_t)15);
	t.x = (struct x86_buf){ start, j->code_end, false };    /* (the block pointer goes just before) */
	t.nfix = 0;
	t.nlinks = 0;
	t.nbexits = 0;
	rc_reset(&t);
	uint32_t pc = pc0;
	unsigned n = 0, budget_needed, cover = 0;

	x86_op_rr(&t.x, 0, 1, 0x81, X86_CMP, R_BUDGET, 0);     /* cmp r14, imm32 (the block's length, below) */
	uint8_t *ninsn = t.x.p;
	x86_imm(&t.x, 0, 4);
	uint8_t *to_budget = x86_jcc(&t.x, X86_B, NULL);
	if (pc0 == j->match_pc)
		match_guard(&t, pc0);

	for (;;) {
		t.pc = t.pcs[n] = pc;
		t.k = n;
		t.pins = 0;
		uint32_t i = mem_r32(j->mem, pc);
		if (!cover && pc - pc0 != 4 * n)
			cover = n;              /* the block has left its first run of words */
		/*
		 * Follow a plain B into code that can't change (the ROM): the 68k
		 * emulator's handlers branch to shared dispatch code, and so get
		 * their own copy of it, and of its indirect branch to predict.
		 */
		uint32_t target = pc + (uint32_t)(((int32_t)(i << 6)) >> 6 & ~3);
		if ((i & 0xFC000003u) == 0x48000000u && target != j->linked_hook && target != j->match_pc &&
		    n + 2 < t.max_insns && t.x.p - start < (ptrdiff_t)BLOCK_MAX_BYTES / 2 && !visited(&t, n, target) &&
		    jit_rom_like(j, target)) {
			budget_needed = ++n;
			pc = target;
			continue;
		}
		t.insn = i;
		int r = insn(&t, i);
		if (r == STOPPED) {
			exit_always(&t);                /* straight to the side exit */
			budget_needed = n + 1;          /* ... where the interpreter runs one more */
			break;
		}
		if (r == EXITED) {
			budget_needed = n + 1;          /* (as the interpreter would need to get that far) */
			break;
		}
		budget_needed = n + 1;
		n++;
		if (r == BRANCHED)
			break;
		pc += 4;
		bool crossing = !(pc & (MEM_PAGE_SIZE - 1)) && !(jit_rom_like(j, pc - 4) && jit_rom_like(j, pc));
		if (crossing || pc == j->match_pc || n >= t.max_insns || t.x.p - start > (ptrdiff_t)BLOCK_MAX_BYTES ||
		    t.nfix + 16 > MAX_FIXUPS) {
			finish_at(&t, n, pc, false);
			break;
		}
	}
	if (!cover)
		cover = budget_needed;

	rc_reset(&t);                   /* (what follows is out of line) */
	branch_exits(&t);
	rc_reset(&t);

	/*
	 * Side exits: the registers dirty at that point are written back; then
	 * the pc of instruction k and the k instructions retired before it, for
	 * the interpreter; or, after k has completed, on to the next block (its
	 * own code may just have changed).
	 */
	for (unsigned f = 0; f < t.nfix;) {
		unsigned k = t.fix[f].k;
		bool after = t.fix[f].after;
		struct cache_state cs = t.fix[f].cs;
		uint8_t *stub = t.x.p;
		rc_store(&t, &cs);
		if (after) {
			x86_store_imm(&t.x, 4, x86_rip(&j->st.code_changed), 0);
			finish_at(&t, k + 1, t.pcs[k] + 4, false);
		} else {
			x86_mov_ri(&t.x, T0, t.pcs[k]);
			if (k)
				x86_alu_ri(&t.x, X86_SUB, 1, R_BUDGET, (int32_t)k);
			x86_jmp(&t.x, j->side_exit);
		}
		for (; f < t.nfix && t.fix[f].k == k && t.fix[f].after == after &&
		       !memcmp(&t.fix[f].cs, &cs, sizeof cs); f++) {
			if (t.fix[f].type == FIX_SITE)
				jit_add_site(j, t.fix[f].at, stub, t.pcs[k]);
			else if (!t.x.overflow)
				x86_patch32(t.fix[f].at, stub);
		}
	}

	/* exits to known pcs: ask C for a link the first time */
	for (unsigned l = 0; l < t.nlinks && !t.x.overflow; l++) {
		struct link *ln = &t.links[l];
		x86_patch32(ln->site, t.x.p);
		x86_mov_ri(&t.x, T0, ln->target);
		x86_lea(&t.x, 1, T1, x86_rip(ln->site));
		x86_jmp(&t.x, ln->after_branch ? j->link_branch : j->link);
	}

	/* not enough budget for the block */
	land(&t, to_budget);
	x86_store_imm(&t.x, 4, x86_m(R_CPU, OFF(pc)), pc0);
	x86_mov_ri(&t.x, T0, EXIT_BUDGET);
	x86_jmp(&t.x, j->exit);

	/* where the first instruction leads once the block is invalid */
	uint8_t *redispatch = t.x.p;
	x86_mov_ri(&t.x, T0, pc0);
	x86_jmp(&t.x, j->lookup);

	if (t.x.overflow) {
		jit_flush(j);
		return NULL;    /* the interpreter runs this instruction; next time there's room */
	}
	memcpy(ninsn, &budget_needed, 4);
	struct jit_block *b = jit_add_block(j, pg, pc0, budget_needed, cover, start, t.x.p, redispatch);
	j->translated_insns += n;
	return b;
}

struct jit_block *arch_translate(struct jit *j, uint32_t pc0)
{
	return translate_upto(j, pc0, MAX_BLOCK_INSNS);
}

/* ---- entry, exit and dispatch ------------------------------------------ */

static void build_stubs(struct jit *j)
{
	static struct tx tx;
	struct tx *t = &tx;
	t->j = j;
	t->x = (struct x86_buf){ j->code, j->code_end, false };
	static const uint8_t saved[] = { X86_RBP, X86_RBX, X86_R12, X86_R13, X86_R14, X86_R15 };

	/* enter(cpu rdi, code rsi, budget rdx, state rcx) */
	memcpy(&j->enter, &(void *){ t->x.p }, sizeof j->enter);         /* object to function pointer */
	for (unsigned k = 0; k < sizeof saved; k++)
		x86_push(B, saved[k]);
	x86_alu_ri(B, X86_SUB, 1, X86_RSP, FRAME);
	x86_mov_rr(B, 1, R_CPU, X86_RDI);
	x86_mov_rr(B, 1, R_BUDGET, X86_RDX);
	x86_store(B, 8, X86_RDX, ST(entry_budget));
	x86_load(B, 8, false, R_MEM, ST(fastbase));
	fixed_load(t, NFIXED);
	x86_jmp_r(B, X86_RSI);

	/* exit (kind in eax) */
	j->exit = t->x.p;
	fixed_store(t, NFIXED);
	x86_store(B, 8, R_BUDGET, ST(remaining));
	x86_alu_ri(B, X86_ADD, 1, X86_RSP, FRAME);
	for (unsigned k = sizeof saved; k--;)
		x86_pop(B, saved[k]);
	x86_ret(B);

	/* side exit: the interpreter runs the instruction at eax */
	j->side_exit = t->x.p;
	st32(t, T0, OFF(pc));
	x86_mov_ri(B, T0, EXIT_INTERP);
	x86_jmp(B, j->exit);

	/* stop at the hook eax; or at eax, which has no block in the table */
	j->hook_exit = t->x.p;
	st32(t, T0, OFF(pc));
	x86_mov_ri(B, T0, EXIT_HOOK);
	x86_jmp(B, j->exit);
	j->miss_exit = t->x.p;
	st32(t, T0, OFF(pc));
	x86_mov_ri(B, T0, EXIT_MISS);
	x86_jmp(B, j->exit);

	/* continue at eax, through the block table (for branches into invalidated blocks) */
	j->lookup = t->x.p;
	lookup_jump(t);

	/* exit to a known pc eax from the JMP whose rel32 is at rcx: C finds the block there and points it there */
	j->link_branch = t->x.p;
	x86_alu_rm(B, X86_CMP, 0, T0, ST(hook));
	uint8_t *other = x86_jcc8(B, X86_NE);
	st32(t, T0, OFF(pc));
	x86_mov_ri(B, T0, EXIT_HOOK);
	x86_jmp(B, j->exit);
	j->link = t->x.p;
	x86_patch8(other, j->link);
	st32(t, T0, OFF(pc));
	x86_store(B, 8, T1, ST(link_site));
	x86_mov_ri(B, T0, EXIT_LINK);
	x86_jmp(B, j->exit);

	j->blocks_start = j->code_free = t->x.p;
}

void arch_init(struct jit *j)
{
	build_stubs(j);
}

/* ---- the host ---------------------------------------------------------- */

/* Whether this processor has x86-64-v3 (and the OS keeps its AVX state); else why not, in `why` */
static bool cpu_ok(char *why, size_t size)
{
	unsigned a = 0, b = 0, c = 0, d = 0, max = __get_cpuid_max(0, NULL);
	unsigned c1 = 0, b7 = 0, ce = 0;
	if (__get_cpuid(1, &a, &b, &c, &d))
		c1 = c;
	if (max >= 7 && __get_cpuid_count(7, 0, &a, &b, &c, &d))
		b7 = b;
	if (__get_cpuid(0x80000001, &a, &b, &c, &d))
		ce = c;
	bool ymm = false;
	if (c1 & 1u << 27) {                            /* OSXSAVE: the OS saves YMM state? */
		uint32_t lo, hi;
		__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
		ymm = (lo & 6) == 6;
	}
	static const struct { const char *name; unsigned word, bit; } need[] = {
		{ "SSE3", 1, 0 }, { "SSSE3", 1, 9 }, { "FMA", 1, 12 }, { "CMPXCHG16B", 1, 13 }, { "SSE4.1", 1, 19 },
		{ "SSE4.2", 1, 20 }, { "MOVBE", 1, 22 }, { "POPCNT", 1, 23 }, { "AVX", 1, 28 }, { "F16C", 1, 29 },
		{ "BMI1", 7, 3 }, { "AVX2", 7, 5 }, { "BMI2", 7, 8 }, { "LAHF", 0x81, 0 }, { "LZCNT", 0x81, 5 },
	};
	const char *hide = getenv("SYSHM_JIT_HIDE");    /* (tests: features to take as missing, comma-separated) */
	size_t len = 0;
	why[0] = 0;
	for (unsigned k = 0; k < sizeof need / sizeof need[0]; k++) {
		unsigned word = need[k].word == 1 ? c1 : need[k].word == 7 ? b7 : ce;
		bool hidden = false;
		size_t nl = strlen(need[k].name);
		for (const char *h = hide; h && *h; h += strcspn(h, ","), h += *h == ',')
			hidden |= strcspn(h, ",") == nl && !strncasecmp(h, need[k].name, nl);
		if ((word >> need[k].bit & 1) && !hidden)
			continue;
		len += (size_t)snprintf(why + len, len < size ? size - len : 0, "%s%s", len ? ", " : "", need[k].name);
	}
	if (!ymm && !len)
		len += (size_t)snprintf(why, size, "AVX state saved by the OS");
	return len == 0;
}

struct jit *arch_alloc(const char **why)
{
	static char missing[200], message[400];
	if (!cpu_ok(missing, sizeof missing)) {
		bool rosetta = false;
#ifdef __APPLE__
		int translated = 0;
		size_t size = sizeof translated;
		rosetta = !sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) && translated;
#endif
		snprintf(message, sizeof message, "no JIT: the processor lacks what it needs (x86-64-v3): %s%s", missing,
			 rosetta ? " (Rosetta offers them with ROSETTA_ADVERTISE_AVX=1 in the environment)" : "");
		*why = message;
		return NULL;
	}
	*why = "no JIT: no memory for its code";
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef __APPLE__
	flags |= MAP_JIT;               /* (only needed under the hardened runtime, with its allow-jit entitlement) */
#endif
	uint8_t *m = mmap(NULL, JIT_DATA + CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
	if (m == MAP_FAILED)
		return NULL;
	mprotect(m, JIT_DATA, PROT_READ | PROT_WRITE);
	struct jit *j = (struct jit *)m;
	j->code = m + JIT_DATA;
	j->code_end = j->code + CODE_SIZE;
	return j;
}

void arch_release(struct jit *j)
{
	munmap(j, JIT_DATA + CODE_SIZE);
}

void arch_retire(struct jit *j, struct jit_block *b)
{
	(void)j;
	uint8_t *entry = (uint8_t *)(uintptr_t)b->code;
	entry[0] = 0xE9;                                /* JMP rel32 over the budget check */
	x86_patch32(entry + 1, b->redispatch);
}

void arch_link(struct jit *j, void *site, const void *to)
{
	(void)j;
	x86_patch32(site, to);
}

void arch_sync(struct jit *j)
{
	(void)j;                        /* (instruction fetch sees this thread's stores) */
}

uintptr_t arch_context_pc(void *ctx)
{
	ucontext_t *uc = ctx;
#ifdef __APPLE__
	return (uintptr_t)uc->uc_mcontext->__ss.__rip;
#else
	return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#endif
}

void arch_set_context_pc(void *ctx, const void *pc)
{
	ucontext_t *uc = ctx;
#ifdef __APPLE__
	uc->uc_mcontext->__ss.__rip = (uint64_t)(uintptr_t)pc;
#else
	uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)pc;
#endif
}

unsigned arch_marks(const struct jit *j, struct jit_mark marks[8])
{
	marks[0] = (struct jit_mark){ "enter", j->code };
	marks[1] = (struct jit_mark){ "exit", j->exit };
	marks[2] = (struct jit_mark){ "side exit", j->side_exit };
	marks[3] = (struct jit_mark){ "hook", j->hook_exit };
	marks[4] = (struct jit_mark){ "miss", j->miss_exit };
	marks[5] = (struct jit_mark){ "link", j->link_branch };
	return 6;
}

#endif
