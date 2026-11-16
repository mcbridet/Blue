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

/*
 * FPRs are cached the same way, in xmm6-xmm15; xmm0-xmm5 are scratch. No
 * XMM register survives a call to C (System V), so all are written back
 * before one and loaded again after.
 */
#define NFCACHE 10u
#define FREG(s) (6u + (s))
#define X0 0u
#define X1 1u
#define X2 2u
#define X3 3u
#define X4 4u
#define X5 5u

struct cache_state {
	uint16_t fp_dirty;
	/* FP_PENDING() of an operation whose record is still to be stored (see fp_arith()), or 0; its operand FPRs */
	uint16_t fp_rec;
	int8_t guest[NCACHE];           /* guest register in each slot, or -1 */
	uint8_t dirty;                  /* slots not yet written back */
	int8_t fpr[NFCACHE];            /* FPR in each FP slot (FREG()), or -1 */
	int8_t fp_rec_ops[3];
};

_Static_assert(sizeof(struct cache_state) == 22, "no padding (cache states are compared with memcmp())");

struct fixup {
	uint8_t *at;                    /* rel32 field, or the faulting instruction (FIX_SITE) */
	unsigned type;
	unsigned k;                     /* side exit: instructions retired before it */
	bool after;                     /* leave after instruction k instead (it has completed) */
	struct cache_state cs;          /* registers to write back on the way out */
};

#define MAX_LINKS 160u
#define MAX_SLOW_STORES 160u

/* Out-of-line path of a store to anything but plain writable memory (MEM_FAST_W clear) */
struct slow_store {
	uint8_t *at;                    /* the rel32 field of the jump to it */
	uint8_t *rejoin;
	unsigned k, size;
	struct cache_state cs;
	bool update, swap;              /* rA is still to update (from eax); a byte-reversed store */
	bool fp;                        /* the value is in XMM register s (stfs: its single's bits) */
	unsigned ra, s;                 /* rA, and the host register holding the value */
	bool io;                        /* one for the mmio hook, if not writable (see slow_ios()) */
	uint32_t insn, pc;
};

#define MAX_SLOW_IOS 160u
#define MAX_SLOW_LOADS 160u

/* Out-of-line path of a load that MEM_FAST_R turned away, for the page's flags alone (see slow_loads()) */
struct slow_load {
	uint8_t *at;                    /* the rel32 field of the jump to it */
	uint8_t *back;                  /* right after that jump */
	uint8_t *rejoin;                /* after the instruction (for the mmio hook's path) */
	unsigned k, size;
	bool io;
	uint32_t insn, pc;
	struct cache_state cs, after;
};
#define MAX_OOL (2 * MAX_BLOCK_INSNS)

/* Out-of-line code for an instruction's rare outcome, rejoining right after the jump to it (see ools()) */
struct ool {
	uint8_t *at, *rejoin;
	uint8_t kind;                   /* OOL_SO, OOL_OV */
	uint8_t xer, ca;                /* OOL_OV: XER's host register; CA from the carry (1), from its inverse (2), or not (0) */
	uint32_t bits;                  /* OOL_SO: SO's bit in eax; OOL_OV: the XER bits to replace */
};

enum { OOL_SO, OOL_OV };

/*
 * Out-of-line path of a load or store to a page that isn't readable or
 * writable (a device's, perhaps), offered to the mmio hook (slow_ios())
 */
struct slow_io {
	uint8_t *at;                    /* the rel32 field of the jump to it */
	uint8_t *rejoin;                /* after the instruction */
	unsigned k, bytes;
	bool write, reload_ea;          /* (the store's update of rA, after rejoin, needs the EA in eax) */
	uint32_t insn, pc;
	struct cache_state before, after;
};

#define MAX_SLOW_FP MAX_BLOCK_INSNS
#define MAX_FP_EXITS 16u

/* Out-of-line path of a floating-point instruction: the interpreter runs it (see fp_insn()) */
struct slow_fp {
	uint8_t *at[MAX_FP_EXITS];      /* rel32 fields of the jumps to it */
	unsigned nat;
	uint8_t *rejoin;
	struct cache_state cs, after;   /* at the jumps to it, where it rejoins */
	uint32_t insn, pc;
};

/* A word the block stored, at rA + d, from rS (both unchanged since) */
struct stored_word {
	int8_t base, src;
	int16_t off;
};

#define MAX_STORED 8u

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
	struct slow_store slow[MAX_SLOW_STORES];
	unsigned nslow;
	struct slow_io io[MAX_SLOW_IOS];
	unsigned nio;
	struct slow_load sld[MAX_SLOW_LOADS];
	unsigned nsld;
	struct ool ool[MAX_OOL];
	unsigned nool;
	struct slow_fp fps[MAX_SLOW_FP];
	unsigned nfps;
	struct cache_state rc;
	int8_t slot_of[NGUEST];
	uint32_t used[NCACHE], tick;
	unsigned pins;                  /* slots the current instruction uses */
	int8_t fp_slot_of[32];
	uint32_t fp_used[NFCACHE];
	unsigned fp_pins;
	bool fp_mode_ok;                /* the FPSCR is known to round to nearest with no exceptions enabled */
	struct stored_word stored[MAX_STORED];  /* see forward_double() */
	unsigned nstored;
};

#define B (&t->x)
#define OFF(field) ((int32_t)offsetof(struct ppc_cpu, field))
#define GPR(r) (OFF(gpr) + 4 * (int32_t)(r))
#define FPR(r) (OFF(fpr) + 8 * (int32_t)(r))
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
	memset(t->rc.fpr, -1, sizeof t->rc.fpr);
	t->rc.fp_dirty = 0;
	memset(t->fp_slot_of, -1, sizeof t->fp_slot_of);
	t->fp_pins = 0;
	t->rc.fp_rec = 0;
	memset(t->rc.fp_rec_ops, -1, sizeof t->rc.fp_rec_ops);
}

/*
 * Stores the record of the FP operation `cs` defers (see fp_arith()): its
 * operands, which are still in their FPRs, and FP_PENDING(). Uses xmm5
 * (and no general register: exits keep the pc in eax).
 */
static void fp_record(struct tx *t, const struct cache_state *cs)
{
	if (!cs->fp_rec)
		return;
	for (unsigned k = 0; k < 3; k++) {
		int r = cs->fp_rec_ops[k];
		if (r < 0)
			continue;
		unsigned x = X5;
		for (unsigned s = 0; s < NFCACHE; s++)
			if (cs->fpr[s] == r)
				x = FREG(s);
		if (x == X5)
			x86_vmovsd(B, false, X5, x86_m(R_CPU, FPR((unsigned)r)));
		x86_vmovsd(B, true, x, ST(fp_ops[k]));
	}
	x86_store_imm(B, 8, ST(fp_op), cs->fp_rec);             /* (and fp_fpcc = 0) */
}

/* Writes back the registers dirty in `cs` (and a deferred FP record) */
static void rc_store(struct tx *t, const struct cache_state *cs)
{
	fp_record(t, cs);
	for (unsigned s = 0; s < NCACHE; s++)
		if (cs->dirty & (1u << s))
			st32(t, cache_reg[s], guest_off((unsigned)cs->guest[s]));
	for (unsigned s = 0; s < NFCACHE; s++)
		if (cs->fp_dirty & (1u << s))
			x86_vmovsd(B, true, FREG(s), x86_m(R_CPU, FPR((unsigned)cs->fpr[s])));
}

/* Loads the FPRs cached in `cs` after a call into C */
static void rc_fp_load(struct tx *t, const struct cache_state *cs)
{
	for (unsigned s = 0; s < NFCACHE; s++)
		if (cs->fpr[s] >= 0)
			x86_vmovsd(B, false, FREG(s), x86_m(R_CPU, FPR((unsigned)cs->fpr[s])));
}

static void rc_flush(struct tx *t)
{
	rc_store(t, &t->rc);
	t->rc.dirty = 0;
	t->rc.fp_dirty = 0;
	t->rc.fp_rec = 0;
}

/* Stores the deferred FP record now */
static void fp_record_now(struct tx *t)
{
	fp_record(t, &t->rc);
	t->rc.fp_rec = 0;
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

/* Guest register g changes: stored words it was the base or the value of are forgotten */
static void forget_stored(struct tx *t, unsigned g)
{
	for (unsigned k = 0; k < t->nstored;) {
		if (t->stored[k].base == (int8_t)g || t->stored[k].src == (int8_t)g)
			t->stored[k] = t->stored[--t->nstored];
		else
			k++;
	}
}

/* A host register for guest register g's new value (all 32 bits must be written) */
static unsigned wr(struct tx *t, unsigned g)
{
	forget_stored(t, g);
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
	forget_stored(t, g);
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

/* An FP slot for FPR r: a free one, or the least recently used (written back) */
static unsigned fp_alloc(struct tx *t, unsigned r)
{
	unsigned best = NFCACHE;
	for (unsigned s = 0; s < NFCACHE; s++) {
		if (t->fp_pins & (1u << s))
			continue;
		if (t->rc.fpr[s] < 0) {
			best = s;
			break;
		}
		if (best == NFCACHE || t->fp_used[s] < t->fp_used[best])
			best = s;
	}
	if (best == NFCACHE) {                  /* (an instruction uses at most four) */
		fprintf(stderr, "jit: no register for f%u at %08X\n", r, t->pc);
		abort();
	}
	int8_t old = t->rc.fpr[best];
	if (old >= 0) {
		if (t->rc.fp_dirty & (1u << best))
			x86_vmovsd(B, true, FREG(best), x86_m(R_CPU, FPR((unsigned)old)));
		t->fp_slot_of[old] = -1;
	}
	t->rc.fpr[best] = (int8_t)r;
	t->rc.fp_dirty &= (uint16_t)~(1u << best);
	t->fp_slot_of[r] = (int8_t)best;
	return best;
}

static unsigned fp_use(struct tx *t, unsigned s)
{
	t->fp_pins |= 1u << s;
	t->fp_used[s] = ++t->tick;
	return FREG(s);
}

/* An XMM register holding FPR r */
static unsigned frd(struct tx *t, unsigned r)
{
	int s = t->fp_slot_of[r];
	if (s < 0) {
		s = (int)fp_alloc(t, r);
		x86_vmovsd(B, false, FREG((unsigned)s), x86_m(R_CPU, FPR(r)));
	}
	return fp_use(t, (unsigned)s);
}

/* An XMM register for FPR r's new value (the deferred FP record stored first if r is an operand of it) */
static unsigned fwr(struct tx *t, unsigned r)
{
	const int8_t *ops = t->rc.fp_rec_ops;
	if (t->rc.fp_rec && (ops[0] == (int8_t)r || ops[1] == (int8_t)r || ops[2] == (int8_t)r))
		fp_record_now(t);
	int s = t->fp_slot_of[r];
	if (s < 0)
		s = (int)fp_alloc(t, r);
	t->rc.fp_dirty |= (uint16_t)(1u << s);
	return fp_use(t, (unsigned)s);
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

/* A jump on cc to out-of-line code (see ools()), or NULL if there's no room for it */
static struct ool *ool_add(struct tx *t, enum x86_cond cc, unsigned kind)
{
	if (t->nool >= MAX_OOL)
		return NULL;
	struct ool *o = &t->ool[t->nool++];
	*o = (struct ool){ .at = x86_jcc(B, cc, NULL), .kind = (uint8_t)kind };
	o->rejoin = t->x.p;
	return o;
}

/* CR field `f` from the host flags of a compare, with SO from XER (rarely set: out of line) */
static void cr_from_flags(struct tx *t, unsigned f, bool is_signed)
{
	unsigned sh = 28 - 4 * f, cr;
	x86_mov_ri(B, T0, CR_EQ << sh);
	x86_cmov_m(B, is_signed ? X86_G : X86_A, 0, T0, x86_rip(t->j->k_cr + 8 * f));
	x86_cmov_m(B, is_signed ? X86_L : X86_B, 0, T0, x86_rip(t->j->k_cr + 8 * f + 4));
	struct x86_mem m;
	unsigned xer = rd_where(t, G_XER, &m);
	if (xer != NO_REG)
		x86_test_rr(B, 0, xer, xer);
	else
		x86_alu_mi(B, X86_CMP, 4, m, 0);
	struct ool *o = ool_add(t, X86_S, OOL_SO);
	if (o) {
		o->bits = 1u << sh;
	} else {
		uint8_t *clear = fwd(t, X86_NS);
		x86_alu_ri(B, X86_OR, 0, T0, (int32_t)(1u << sh));
		land(t, clear);
	}
	cr = rw(t, G_CR);
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

/*
 * XER from the host flags of the operation just done (only MOVs since): CA
 * from the carry (`ca` 1) or its inverse (2), OV and SO from the overflow
 * if `ov`, which is rare and goes out of line. Uses ecx.
 */
static void xer_update(struct tx *t, unsigned ca, bool ov)
{
	if (ov && t->nool >= MAX_OOL) {
		xer_flags(t, ca, ca == 2 ? X86_AE : X86_B, true);
		xer_insert(t, ca, true);
		return;
	}
	unsigned xer = rw(t, G_XER);
	uint32_t bits = (ca ? XER_CA : 0) | (ov ? XER_OV : 0);
	struct ool *o = NULL;
	if (ov) {
		o = ool_add(t, X86_O, OOL_OV);
		o->xer = (uint8_t)xer;
		o->ca = (uint8_t)ca;
		o->bits = bits;
	}
	if (ca) {
		x86_alu_rr(B, X86_SBB, 0, T1, T1);
		x86_alu_ri(B, X86_AND, 0, T1, (int32_t)XER_CA);
		if (ca == 2)
			x86_alu_ri(B, X86_XOR, 0, T1, (int32_t)XER_CA);
	}
	x86_alu_ri(B, X86_AND, 0, xer, (int32_t)~bits);
	if (ca)
		x86_alu_rr(B, X86_OR, 0, xer, T1);
	if (o)
		o->rejoin = t->x.p;
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

/* The out-of-line code of ool_add(): eax |= SO's bit; or XER from the overflow as xer_update() would */
static void ools(struct tx *t)
{
	for (unsigned k = 0; k < t->nool && !t->x.overflow; k++) {
		const struct ool *o = &t->ool[k];
		land(t, o->at);
		if (o->kind == OOL_SO) {
			x86_alu_ri(B, X86_OR, 0, T0, (int32_t)o->bits);
		} else {
			if (o->ca) {
				x86_alu_rr(B, X86_SBB, 0, T1, T1);
				x86_alu_ri(B, X86_AND, 0, T1, (int32_t)XER_CA);
				if (o->ca == 2)
					x86_alu_ri(B, X86_XOR, 0, T1, (int32_t)XER_CA);
				x86_alu_ri(B, X86_OR, 0, T1, (int32_t)(XER_OV | XER_SO));
			} else {
				x86_mov_ri(B, T1, XER_OV | XER_SO);
			}
			x86_alu_ri(B, X86_AND, 0, o->xer, (int32_t)~o->bits);
			x86_alu_rr(B, X86_OR, 0, o->xer, T1);
		}
		x86_jmp(B, o->rejoin);
	}
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
	t->fp_mode_ok = false;
	t->nstored = 0;
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
		if (!r && (mask == 0xFF || mask == 0xFFFF)) {         /* (a byte or halfword into place) */
			x86_mov_low(B, mask == 0xFF ? 1 : 2, d, s);
			return;
		}
		if (sh >= 1 && sh <= 3 && (mask == 0xFFu << sh || mask == 0xFFFFu << sh) && d != s) {
			/* a byte or halfword times 2, 4 or 8: added into the cleared field */
			x86_alu_ri(B, X86_AND, 0, d, (int32_t)~mask);
			x86_extend(B, mask >> sh == 0xFF ? 1 : 2, false, 0, T0, s);
			x86_lea(B, 0, d, x86_mx(d, T0, sh, 0));
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
	x86_jmp_m(B, x86_mx(T2, T0, 1, 0));                     /* (see l1_entry(): eax is a word's address) */
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
		alu3(t, X86_ADD, true, d, a, b);
		xer_update(t, 0, true);
		break;
	case 10:                                                        /* addc */
	case 40:                                                        /* subf: b - a */
	case 8:                                                         /* subfc */
		a = rd(t, ra);
		b = rd(t, rb);
		d = wr(t, rd_);
		if (xo == 10)
			alu3(t, X86_ADD, true, d, a, b);
		else
			alu3(t, X86_SUB, false, d, b, a);
		xer_update(t, xo == 10 ? 1 : xo == 8 ? 2 : 0, oe);
		break;
	case 104:                                                       /* neg */
		a = rd(t, ra);
		d = wr(t, rd_);
		mov_rr(t, d, a);
		x86_unary(B, X86_NEG, 0, d);
		xer_update(t, 0, oe);
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
		x86_mov_rr(B, 0, d, T0);
		xer_update(t, 1, oe);
		break;
	}
	case 235:                                                       /* mullw */
		a = rd(t, ra);
		b = rd(t, rb);
		d = wr(t, rd_);
		x86_mov_rr(B, 0, T0, a);
		x86_imul_rr(B, 0, T0, b);
		x86_mov_rr(B, 0, d, T0);
		xer_update(t, 0, oe);
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

/* ---- loads and stores ------------------------------------------------------ */

/*
 * Loads and stores of 1, 2 or 4 bytes, the EA in eax. With fastmem, the
 * access goes straight to generated code's view of guest memory (r15),
 * where the host faults if the guest's page flags say no (or the page
 * holds translated code, for stores); the fault handler sends it to the
 * instruction's side exit, so the interpreter executes it, and the
 * instruction is translated with checks from then on. Those, or all
 * without fastmem, test page flags and access memory through C's view:
 * loads leave for the interpreter unless the page (and the next) is
 * readable; stores take a slow path (slow_stores()) unless MEM_FAST_W says
 * the page is plain writable memory.
 */

/* An access that may fault (fastmem): if it does, the side exit for the instruction, with `cs` */
static void add_site(struct tx *t, const struct cache_state *cs)
{
	if (t->nfix < MAX_FIXUPS)
		t->fix[t->nfix++] = (struct fixup){ t->x.p, FIX_SITE, t->k, false, *cs };
	else
		t->x.overflow = true;
}

/* Whether the current instruction may access memory without checks */
static bool fast_access(const struct tx *t)
{
	if (!t->j->fastmem)
		return false;
	const struct jit_page *pg = t->j->pages[t->pc >> MEM_PAGE_SHIFT];
	unsigned w = (t->pc >> 2) & (PAGE_WORDS - 1);
	return !pg || !(pg->slow[w / 64] >> (w % 64) & 1);
}

/* Whether its checked accesses have a path to the mmio hook (see slow_ios()) */
static bool io_access(const struct tx *t)
{
	if (!t->j->fastmem)
		return true;
	const struct jit_page *pg = t->j->pages[t->pc >> MEM_PAGE_SHIFT];
	unsigned w = (t->pc >> 2) & (PAGE_WORDS - 1);
	return pg && pg->io[w / 64] >> (w % 64) & 1;
}

/*
 * A JE (after a TEST of the page flags) to an out-of-line path for the
 * mmio hook (see slow_ios()) for instruction k; NULL, and nothing emitted,
 * if there's no room.
 */
static struct slow_io *add_io(struct tx *t, unsigned k, uint32_t insn, uint32_t pc, unsigned size, bool write,
			      const struct cache_state *before)
{
	if (t->nio >= MAX_SLOW_IOS)
		return NULL;
	struct slow_io *io = &t->io[t->nio++];
	*io = (struct slow_io){ x86_jcc(B, X86_E, NULL), NULL, k, 1u << size, write, false, insn, pc, *before, *before };
	return io;
}

/* eax = (rA|0, or rA for update) + (rB or d) */
static void effective_address(struct tx *t, bool update, bool indexed, unsigned ra, unsigned rb, int32_t d)
{
	bool base = update || ra;
	if (indexed) {
		unsigned b = rd(t, rb);
		if (base) {
			unsigned a = rd(t, ra);
			x86_lea(B, 0, T0, x86_mx(a, b, 0, 0));
		} else {
			x86_mov_rr(B, 0, T0, b);
		}
	} else if (base) {
		unsigned a = rd(t, ra);
		if (d)
			x86_lea(B, 0, T0, x86_m(a, d));
		else
			x86_mov_rr(B, 0, T0, a);
	} else {
		x86_mov_ri(B, T0, (uint32_t)d);
	}
}

/* ecx = the flags of the page of the EA in eax; rdx = guest address 0 as C sees it */
static void page_flags(struct tx *t)
{
	x86_mov_rr(B, 0, T1, T0);
	x86_shift_ri(B, X86_SHR, 0, T1, MEM_PAGE_SHIFT);
	x86_load(B, 8, false, T2, ST(pageflags));
	x86_load(B, 1, false, T1, x86_mx(T2, T1, 0, 0));
	x86_load(B, 8, false, T2, ST(membase));
}

/* Stores 1 << size bytes of register s at m (big-endian, unless `swap`); `fp`: of XMM register s, through rcx */
static void store_value(struct tx *t, unsigned size, bool swap, bool fp, unsigned s, struct x86_mem m)
{
	if (fp) {
		if (size == 3)
			x86_vmovq_to(B, T1, s);
		else
			x86_vmovd_to(B, T1, s);
		s = T1;
	}
	if (size == 0)
		x86_store(B, 1, s, m);
	else if (swap)
		x86_store(B, 1u << size, s, m);
	else
		x86_movbe(B, true, 1u << size, s, m);
}

static void load_single(struct tx *t, unsigned r);
static void store_single(struct tx *t, unsigned r);

enum { LS_GPR, LS_DOUBLE, LS_SINGLE };          /* load_store()'s register kinds */

/*
 * lfd of the two words the block has just stored with stw, as compilers
 * turn integers into doubles: the double is put together from the
 * registers stored (the host can't forward two stores to one load, which
 * then waits for them to reach its cache). They went to plain memory
 * (fastmem: else the block would have been left), and nothing has stored
 * there since. False if it isn't that.
 */
static bool forward_double(struct tx *t, unsigned frt, unsigned ra, int32_t d)
{
	int hi = -1, lo = -1;
	for (unsigned k = 0; k < t->nstored; k++) {
		if (t->stored[k].base != (int8_t)ra)
			continue;
		if (t->stored[k].off == d)
			hi = t->stored[k].src;
		else if (t->stored[k].off == d + 4)
			lo = t->stored[k].src;
	}
	if (hi < 0 || lo < 0)
		return false;
	unsigned h = rd(t, (unsigned)hi), l = rd(t, (unsigned)lo);
	x86_mov_rr(B, 0, T1, h);
	x86_shift_ri(B, X86_SHL, 1, T1, 32);
	x86_alu_rr(B, X86_OR, 1, T1, l);
	x86_vmovq_from(B, fwr(t, frt), T1);
	return true;
}

/* After a store of `bytes` at rA + d (indexed: unknown), from rS: what's known stored, and if `keep`, this word too */
static void note_store(struct tx *t, bool keep, bool indexed, unsigned ra, int32_t d, unsigned bytes, unsigned rs)
{
	if (indexed || !ra) {
		t->nstored = 0;
		return;
	}
	for (unsigned k = 0; k < t->nstored;) {         /* (other bases may point there too) */
		const struct stored_word *w = &t->stored[k];
		if (w->base != (int8_t)ra || (w->off < d + (int32_t)bytes && d < w->off + 4))
			t->stored[k] = t->stored[--t->nstored];
		else
			k++;
	}
	if (keep && t->nstored < MAX_STORED && d >= -32768 && d <= 32767)
		t->stored[t->nstored++] = (struct stored_word){ (int8_t)ra, (int8_t)rs, (int16_t)d };
}

static void load_store(struct tx *t, unsigned size, bool load, bool sign, bool update, bool indexed, bool swap,
		       unsigned fp, unsigned rd_, unsigned ra, unsigned rb, int32_t d)
{
	if (load && fp == LS_DOUBLE && !update && !indexed && ra && forward_double(t, rd_, ra, d))
		return;
	unsigned s = NO_REG;
	if (!load && fp == LS_DOUBLE) {
		s = frd(t, rd_);
	} else if (!load && fp == LS_SINGLE) {
		store_single(t, rd_);                   /* (before the EA: it needs the scratch registers) */
		s = X5;
	}
	bool fast = fast_access(t);
	struct x86_mem m;
	if (fast && !update && (indexed ? !ra : ra && d >= 0)) {
		/*
		 * Straight from the guest register (whose upper half is zero), with
		 * no EA to compute: what runs past 4 GB, instead of wrapping, faults
		 * in the view's slack (MEM_SLACK), and the interpreter takes it
		 */
		m = x86_mx(R_MEM, rd(t, indexed ? rb : ra), 0, indexed ? 0 : d);
	} else {
		effective_address(t, update, indexed, ra, rb, d);
		if (!fast)
			page_flags(t);
		m = x86_mx(fast ? R_MEM : T2, T0, 0, 0);
	}
	struct slow_io *io = NULL;
	struct slow_load *sld = NULL;
	if (load) {
		struct cache_state before = t->rc;              /* a fault leaves the registers as they are here */
		if (!fast) {
			x86_test_ri(B, 0, T1, size ? MEM_FAST_R : MEM_R);       /* (a byte can't run into the next page) */
			if (size && t->nsld < MAX_SLOW_LOADS) {
				sld = &t->sld[t->nsld++];
				*sld = (struct slow_load){ x86_jcc(B, X86_E, NULL), t->x.p, NULL, t->k, size,
							   fp == LS_GPR && io_access(t), t->insn, t->pc, before, before };
			} else if (fp != LS_GPR || !io_access(t) ||
				   !(io = add_io(t, t->k, t->insn, t->pc, size, false, &before))) {
				exit_if(t, X86_E);
			}
		}
		unsigned v = fp ? T1 : wr(t, rd_);
		if (fast)
			add_site(t, &before);
		if (size == 0) {
			x86_load(B, 1, false, v, m);
		} else if (swap) {
			x86_load(B, 1u << size, false, v, m);
		} else if (size == 1) {
			x86_movbe(B, false, 2, v, m);
			x86_extend(B, 2, sign, 0, v, v);
		} else {
			x86_movbe(B, false, 1u << size, v, m);
		}
		if (fp == LS_DOUBLE)
			x86_vmovq_from(B, fwr(t, rd_), T1);
		else if (fp == LS_SINGLE)
			load_single(t, rd_);
	} else {
		if (fp == LS_GPR)
			s = rd(t, rd_);
		if (fast) {
			if (fp == LS_DOUBLE)                    /* (the access itself must be the fault site) */
				x86_vmovq_to(B, T1, s);
			else if (fp == LS_SINGLE)
				x86_vmovd_to(B, T1, s);
			add_site(t, &t->rc);
			store_value(t, size, swap, false, fp ? T1 : s, m);
		} else if (t->nslow < MAX_SLOW_STORES) {
			x86_test_ri(B, 0, T1, MEM_FAST_W);
			struct slow_store *ss = &t->slow[t->nslow++];
			*ss = (struct slow_store){ x86_jcc(B, X86_E, NULL), NULL, t->k, size, t->rc, update, swap, fp != LS_GPR,
						   ra, s, fp == LS_GPR && io_access(t), t->insn, t->pc };
			store_value(t, size, swap, fp != LS_GPR, s, m);
			ss->rejoin = t->x.p;
		} else {
			x86_test_ri(B, 0, T1, MEM_FAST_W);
			exit_if(t, X86_E);
			store_value(t, size, swap, fp != LS_GPR, s, m);
		}
		note_store(t, fast && size == 2 && fp == LS_GPR && !update, indexed, ra, d, 1u << size, rd_);
	}
	if (update)
		x86_mov_rr(B, 0, wr(t, ra), T0);
	if (io) {
		io->rejoin = t->x.p;
		io->after = t->rc;
	}
	if (sld) {
		sld->rejoin = t->x.p;
		sld->after = t->rc;
	}
	if (io || (sld && sld->io)) {
		t->fp_mode_ok = false;          /* (the hook might switch contexts, change registers) */
		t->nstored = 0;
	}
}

/*
 * Loads that MEM_FAST_R turned away: it says this page and the next are
 * readable, for an access that might run into the next. One that stays
 * within a readable page goes back and loads; otherwise the mmio hook may
 * do it (slow_ios()), or the interpreter executes the instruction.
 */
static void slow_loads(struct tx *t)
{
	for (unsigned s = 0; s < t->nsld && !t->x.overflow; s++) {
		struct slow_load *sl = &t->sld[s];
		land(t, sl->at);
		t->k = sl->k;
		t->rc = sl->cs;
		x86_test_ri(B, 0, T1, MEM_R);
		struct slow_io *io = NULL;
		if (sl->io && (io = add_io(t, sl->k, sl->insn, sl->pc, sl->size, false, &sl->cs))) {
			io->rejoin = sl->rejoin;
			io->after = sl->after;
		} else {
			exit_if(t, X86_E);
		}
		x86_mov_rr(B, 0, T1, T0);
		x86_alu_ri(B, X86_AND, 0, T1, MEM_PAGE_SIZE - 1);
		x86_alu_ri(B, X86_CMP, 0, T1, (int32_t)(MEM_PAGE_SIZE - (1u << sl->size)));
		exit_if(t, X86_A);
		x86_jmp(B, sl->back);
	}
}

/*
 * Stores that MEM_FAST_W (in ecx) turned away. Unless the page is writable
 * and the store stays within it, the mmio hook may do it (slow_ios()), or
 * the interpreter executes the instruction. Then, if the page holds no
 * translated code, or the word stored to isn't one that code was
 * translated from (the page's bitmap says), the store just happens.
 * Otherwise a host function stores it, invalidating the blocks translated
 * from those bytes; the registers in caller-saved host registers are
 * reloaded afterwards, and if any code changed, the instruction completes
 * (rA) and the block is left after it.
 */
static void slow_stores(struct tx *t)
{
	for (unsigned k = 0; k < t->nslow && !t->x.overflow; k++) {
		struct slow_store *ss = &t->slow[k];
		unsigned bytes = 1u << ss->size;
		land(t, ss->at);
		t->k = ss->k;
		t->rc = ss->cs;
		x86_test_ri(B, 0, T1, MEM_W);
		struct slow_io *io = NULL;
		if (ss->io && (io = add_io(t, ss->k, ss->insn, ss->pc, ss->size, true, &ss->cs))) {
			io->rejoin = ss->rejoin;
			io->reload_ea = ss->update;
		} else {
			exit_if(t, X86_E);
		}
		if (bytes > 1) {                                        /* within the page? */
			x86_mov_rr(B, 0, T2, T0);
			x86_alu_ri(B, X86_AND, 0, T2, MEM_PAGE_SIZE - 1);
			x86_alu_ri(B, X86_CMP, 0, T2, (int32_t)(MEM_PAGE_SIZE - bytes));
			exit_if(t, X86_A);
		}
		x86_test_ri(B, 0, T1, MEM_CODE);
		uint8_t *plain[2] = { fwd(t, X86_E), NULL }, *call = NULL;
		if (bytes <= 4) {
			if (bytes > 1) {                                /* within one word? */
				x86_mov_rr(B, 0, T2, T0);
				x86_alu_ri(B, X86_AND, 0, T2, 3);
				x86_alu_ri(B, X86_CMP, 0, T2, (int32_t)(4 - bytes));
				call = fwd(t, X86_A);
			}
			x86_load(B, 8, false, T2, ST(pages));           /* the word's bit in its page's bitmap */
			x86_mov_rr(B, 0, T1, T0);
			x86_shift_ri(B, X86_SHR, 0, T1, MEM_PAGE_SHIFT);
			x86_load(B, 8, false, T2, x86_mx(T2, T1, 3, 0));
			x86_mov_rr(B, 0, T1, T0);
			x86_shift_ri(B, X86_SHR, 0, T1, 8);
			x86_alu_ri(B, X86_AND, 0, T1, PAGE_WORDS / 64 - 1);
			x86_load(B, 8, false, T2, x86_mx(T2, T1, 3, (int32_t)offsetof(struct jit_page, words)));
			x86_mov_rr(B, 0, T1, T0);
			x86_shift_ri(B, X86_SHR, 0, T1, 2);
			x86_op_rr(B, 0, 1, 0x0FA3, T1, T2, 0);          /* bt rdx, rcx */
			plain[1] = fwd(t, X86_AE);
		}
		land(t, call);

		/* code_store(&jit_state, EA, the bytes in guest order, size), the value taken first (xmm5) */
		if (ss->fp && ss->size == 3)
			x86_vmovq_to(B, T2, ss->s);
		else if (ss->fp)
			x86_vmovd_to(B, T2, ss->s);
		else
			x86_mov_rr(B, 0, T2, ss->s);
		if (ss->size && !ss->swap) {
			x86_bswap(B, ss->size == 3, T2);
			if (ss->size == 1)
				x86_shift_ri(B, X86_SHR, 0, T2, 16);
		}
		rc_store(t, &ss->cs);
		fixed_store(t, CALLER_SAVED_FIXED);
		if (ss->update)                                         /* eax must survive the call */
			x86_store(B, 4, T0, ST(scratch));
		x86_mov_rr(B, 0, X86_RSI, T0);
		x86_lea(B, 1, X86_RDI, x86_rip(&t->j->st));
		x86_mov_ri(B, X86_RCX, ss->size);
		x86_call_m(B, ST(code_store));
		fixed_load(t, CALLER_SAVED_FIXED);
		for (unsigned c = 0; c < NCACHE; c++)
			if (ss->cs.guest[c] >= 0)
				ld32(t, cache_reg[c], guest_off((unsigned)ss->cs.guest[c]));
		rc_fp_load(t, &ss->cs);
		rc_reset(t);                                            /* (everything was written back) */
		if (ss->update) {
			x86_load(B, 4, false, T0, ST(scratch));
			x86_alu_mi(B, X86_CMP, 4, ST(code_changed), 0);
			x86_jcc(B, X86_E, ss->rejoin);
			unsigned h = fixed_host(ss->ra);                /* code changed: rA = EA here, then leave */
			if (h != NO_REG)
				x86_mov_rr(B, 0, h, T0);
			else
				st32(t, T0, GPR(ss->ra));
			exit_always(t);
			t->fix[t->nfix - 1].after = true;
		} else {
			exit_if_code_changed(t);
			x86_jmp(B, ss->rejoin);
		}

		for (unsigned p = 0; p < 2; p++)
			land(t, plain[p]);
		x86_load(B, 8, false, T2, ST(membase));
		store_value(t, ss->size, ss->swap, ss->fp, ss->s, x86_mx(T2, T0, 0, 0));
		x86_jmp(B, ss->rejoin);
	}
}

/*
 * Loads and stores (to and from GPRs) that page flags turned away: the
 * page isn't readable, or writable. A host function (mmio() in jit.c)
 * offers the access to the mmio hook, as the interpreter would, with
 * everything written back; then all registers are reloaded, as the hook
 * may have changed any. If it didn't take it, the interpreter executes the
 * instruction; if it did, the block goes on after it, or leaves there (as
 * mmio() says: for the kernel, or because code changed).
 */
static void slow_ios(struct tx *t)
{
	for (unsigned s = 0; s < t->nio && !t->x.overflow; s++) {
		struct slow_io *io = &t->io[s];
		land(t, io->at);
		t->k = io->k;
		rc_store(t, &io->before);
		x86_mov_rr(B, 0, X86_RCX, T0);                          /* mmio(cpu, insn, pc, EA, access, left) */
		x86_mov_ri(B, X86_RSI, io->insn);
		x86_mov_ri(B, X86_RDX, io->pc);
		x86_mov_ri(B, X86_R8, io->bytes | (unsigned)io->write << 8);
		x86_lea(B, 1, X86_R9, x86_m(R_BUDGET, -(int32_t)io->k));       /* (instructions left, this one included) */
		x86_call(B, t->j->mmio_call);
		rc_reset(t);                                            /* (all written back) */
		x86_test_rr(B, 0, T0, T0);                              /* PPC_MMIO_NO: the interpreter */
		exit_if(t, X86_E);
		x86_alu_ri(B, X86_CMP, 0, T0, PPC_MMIO_DONE);
		uint8_t *stop = fwd(t, X86_NE);
		for (unsigned c = 0; c < NCACHE; c++)
			if (io->after.guest[c] >= 0)
				ld32(t, cache_reg[c], guest_off((unsigned)io->after.guest[c]));
		rc_fp_load(t, &io->after);
		if (io->reload_ea)
			ld32(t, T0, OFF(fault_addr));
		x86_jmp(B, io->rejoin);
		land(t, stop);
		budget_sub(t, io->k + 1);
		x86_store_imm(B, 4, x86_m(R_CPU, OFF(pc)), io->pc + 4);
		x86_load(B, 4, false, T0, ST(mmio_exit));
		x86_jmp(B, t->j->exit);
	}
}

/* The host address of the EA in eax into rdx, if the n bytes there are accessible within one page */
static void block_access(struct tx *t, bool load, unsigned n)
{
	page_flags(t);
	x86_test_ri(B, 0, T1, load ? MEM_R : MEM_W);
	exit_if(t, X86_E);
	if (!load) {
		x86_test_ri(B, 0, T1, MEM_CODE);                /* the interpreter's job */
		exit_if(t, X86_NE);
	}
	if (n > 1) {
		x86_mov_rr(B, 0, T1, T0);
		x86_alu_ri(B, X86_AND, 0, T1, MEM_PAGE_SIZE - 1);
		x86_alu_ri(B, X86_CMP, 0, T1, (int32_t)(MEM_PAGE_SIZE - n));
		exit_if(t, X86_A);
	}
	x86_lea(B, 1, T2, x86_mx(T2, T0, 0, 0));
}

/* lmw, stmw: rS..r31 at (rA|0) + d */
static void load_store_multiple(struct tx *t, bool load, unsigned rs, unsigned ra, int32_t d)
{
	unsigned n = 32 - rs;
	if (!load)
		t->nstored = 0;
	effective_address(t, false, false, ra, 0, d);
	block_access(t, load, 4 * n);
	t->pins = 0;                    /* each register is used once */
	for (unsigned k = 0; k < n; k++) {
		if (load)
			x86_movbe(B, false, 4, wr(t, rs + k), x86_m(T2, 4 * (int32_t)k));
		else
			x86_movbe(B, true, 4, rd(t, rs + k), x86_m(T2, 4 * (int32_t)k));
		t->pins = 0;
	}
}

/* dcbz: 32 aligned bytes of zeros */
static void zero_block(struct tx *t, unsigned ra, unsigned rb)
{
	effective_address(t, false, true, ra, rb, 0);
	x86_alu_ri(B, X86_AND, 0, T0, ~31);
	block_access(t, false, 1);      /* (32 aligned bytes don't cross a page) */
	for (int k = 0; k < 4; k++)
		x86_store_imm(B, 8, x86_m(T2, 8 * k), 0);
}

/* ---- floating point --------------------------------------------------------- */

/*
 * As on AArch64 (see jit_a64.c): arithmetic natively where the interpreter's
 * fast path would take it (round to nearest, no exceptions enabled,
 * operands and results well inside the normal range, so that inexact is the
 * only possible exception and an error-free transformation finds it, FMA3
 * doing the exact products), the FPSCR's FR, FI and FPRF left pending as a
 * record for the settle stub (build_settle()); anything else out of line,
 * where the interpreter runs the instruction (struct slow_fp). The FPSCR
 * stays in memory, its bits tested there. Scratch: xmm0-xmm5 (the result in
 * xmm0), rax, rcx and rdx (dl: whether inexact).
 */

#define FPSCR_FX 0x80000000u
#define FPSCR_XX (1u << 25)
#define FPSCR_FI (1u << 17)
#define FPSCR_VX_ALL 0x01F80700u                        /* VXSNAN to VXVC, VXSOFT, VXSQRT, VXCVI */
#define FPSCR_EXC_ALL (0xFu << 25 | FPSCR_VX_ALL)       /* OX UX ZX XX and those */

/* tame(): zero, or a magnitude from 2^-900 to 2^1000 (biased exponents) */
#define TAME_LO 123u
#define TAME_HI 2022u

#define FPSCR_MEM x86_m(R_CPU, OFF(fpscr))
#define K_SIGN x86_rip(t->j->k_sign)
#define K_ABS x86_rip(t->j->k_abs)
#define K_ONE x86_rip(t->j->k_one)

/* After the instruction's FPR operands are in the cache (the slow path writes back what's dirty here) */
static bool fp_begin(struct tx *t, uint32_t insn)
{
	if (t->nfps >= MAX_SLOW_FP)
		return false;
	struct slow_fp *f = &t->fps[t->nfps++];
	f->nat = 0;
	f->rejoin = NULL;
	f->cs = t->rc;
	f->insn = insn;
	f->pc = t->pc;
	return true;
}

/* To the slow path if cc */
static void fp_slow(struct tx *t, enum x86_cond cc)
{
	struct slow_fp *f = &t->fps[t->nfps - 1];
	uint8_t *at = x86_jcc(B, cc, NULL);
	if (f->nat < MAX_FP_EXITS)
		f->at[f->nat++] = at;
	else
		t->x.overflow = true;
}

static void fp_end(struct tx *t)
{
	struct slow_fp *f = &t->fps[t->nfps - 1];
	f->rejoin = t->x.p;
	f->after = t->rc;
}

/*
 * The slow paths: the interpreter executes the instruction, which changes
 * only FPRs, the FPSCR and CR (memory and GPRs are the business of loads
 * and stores, which take other paths); the registers are loaded again
 * afterwards, as they're cached where it rejoins.
 */
static void slow_fps(struct tx *t)
{
	for (unsigned k = 0; k < t->nfps && !t->x.overflow; k++) {
		struct slow_fp *f = &t->fps[k];
		if (!f->nat)
			continue;
		for (unsigned s = 0; s < f->nat; s++)
			land(t, f->at[s]);
		rc_store(t, &f->cs);
		fixed_store(t, NFIXED);
		x86_mov_rr(B, 1, X86_RDI, R_CPU);
		x86_mov_ri(B, X86_RSI, f->insn);
		x86_mov_ri(B, X86_RDX, f->pc);
		x86_call_m(B, ST(step));
		fixed_load(t, NFIXED);
		for (unsigned c = 0; c < NCACHE; c++)
			if (f->after.guest[c] >= 0)
				ld32(t, cache_reg[c], guest_off((unsigned)f->after.guest[c]));
		rc_fp_load(t, &f->after);
		x86_jmp(B, f->rejoin);
	}
}

/*
 * Leaves the block for the interpreter unless the FPSCR rounds to nearest
 * and enables no exceptions (with `any_rounding`, just enables none), which
 * the rest of the block then knows
 */
static void fp_mode(struct tx *t, bool any_rounding)
{
	if (t->fp_mode_ok)
		return;
	x86_test_mi(B, 1, FPSCR_MEM, any_rounding ? 0xF8 : 0xFB);       /* (NI, 0x04, changes nothing here) */
	exit_if(t, X86_NE);
	if (!any_rounding)
		t->fp_mode_ok = true;
}

/*
 * Out of line unless the double in x has a biased exponent from lo to hi,
 * or (if zero_ok) is zero. For zero_ok, returns the jump taken in range,
 * to land() after code that checks the zero case further. Uses rax, rcx.
 */
static uint8_t *fp_range(struct tx *t, unsigned x, unsigned lo, unsigned hi, bool zero_ok)
{
	x86_vmovq_to(B, T0, x);
	x86_rorx(B, 1, T1, T0, 52);                             /* (sign and exponent at the bottom) */
	x86_alu_ri(B, X86_AND, 0, T1, 0x7FF);
	x86_alu_ri(B, X86_SUB, 0, T1, (int32_t)lo);
	x86_alu_ri(B, X86_CMP, 0, T1, (int32_t)(hi - lo));
	if (!zero_ok) {
		fp_slow(t, X86_A);
		return NULL;
	}
	uint8_t *ok = fwd(t, X86_BE);
	x86_alu_rr(B, X86_ADD, 1, T0, T0);                      /* x << 1: zero? */
	fp_slow(t, X86_NE);
	return ok;
}

/* rax = the bits of x << 1: ZF if x is zero */
static void fp_zero_test(struct tx *t, unsigned x)
{
	x86_vmovq_to(B, T0, x);
	x86_alu_rr(B, X86_ADD, 1, T0, T0);
}

/* Out of line unless a or c is zero (a zero product is exact then; otherwise it underflowed) */
static void fp_either_zero(struct tx *t, unsigned a, unsigned c)
{
	fp_zero_test(t, a);
	uint8_t *zero = fwd(t, X86_E);
	fp_zero_test(t, c);
	fp_slow(t, X86_NE);
	land(t, zero);
}

/* edx = (x != 0), or edx |= that */
static void fp_nonzero(struct tx *t, unsigned x, bool or)
{
	fp_zero_test(t, x);
	if (or) {
		x86_setcc(B, X86_NE, T0);
		x86_extend(B, 1, false, 0, T0, T0);
		x86_alu_rr(B, X86_OR, 0, T2, T0);
	} else {
		x86_setcc(B, X86_NE, T2);
		x86_extend(B, 1, false, 0, T2, T2);
	}
}

/* Rounds xmm0 to single precision as opcode 59 does, out of line unless tame_single(); edx |= it changed if `track` */
static void fp_round_single(struct tx *t, bool track)
{
	x86_vcvt(B, true, X5, X0, X0);
	x86_vcvt(B, false, X5, X5, X5);
	uint8_t *ok = fp_range(t, X5, 903, 1148, true);
	fp_zero_test(t, X0);                                    /* rounded to zero: was it zero? */
	fp_slow(t, X86_NE);
	land(t, ok);
	if (track) {
		x86_vmovq_to(B, T0, X5);
		x86_vmovq_to(B, T1, X0);
		x86_alu_rr(B, X86_CMP, 1, T0, T1);
		x86_setcc(B, X86_NE, T0);
		x86_extend(B, 1, false, 0, T0, T0);
		x86_alu_rr(B, X86_OR, 0, T2, T0);
	}
	x86_vmovapd(B, X0, X5);
}

/* eax = FPRF for the zero or normal double in rax: 0x02, or 0x12 if negative; 0x04, or 0x08. Uses rcx */
static void fp_fprf(struct tx *t)
{
	x86_mov_rr(B, 1, T1, T0);
	x86_shift_ri(B, X86_SHR, 1, T1, 63);
	x86_alu_rr(B, X86_ADD, 1, T0, T0);
	uint8_t *zero = fwd(t, X86_E);
	x86_shift_ri(B, X86_SHL, 0, T1, 2);
	x86_lea(B, 0, T0, x86_m(T1, 4));
	uint8_t *done = fwd_always(t);
	land(t, zero);
	x86_shift_ri(B, X86_SHL, 0, T1, 4);
	x86_lea(B, 0, T0, x86_m(T1, 2));
	land(t, done);
}

/* CR1 from the FPSCR in register `reg` */
static void cr1_from(struct tx *t, unsigned reg)
{
	mov_rr(t, T0, reg);
	x86_shift_ri(B, X86_SHR, 0, T0, 4);
	x86_alu_ri(B, X86_AND, 0, T0, 0x0F000000);
	unsigned cr = rw(t, G_CR);
	x86_alu_ri(B, X86_AND, 0, cr, ~0x0F000000);
	x86_alu_rr(B, X86_OR, 0, cr, T0);
}

/* Rc = 1 for instructions that leave the FPSCR alone: CR1 from it */
static void fp_cr1(struct tx *t, bool rc)
{
	if (rc) {
		x86_load(B, 4, false, T0, FPSCR_MEM);
		cr1_from(t, T0);
	}
}

/*
 * The FPSCR after an operation with result xmm0, inexact if dl: FR and FI
 * (and FX, XX), FPRF if `fprf`; it and the result stored, CR1 set for Rc.
 */
static void fp_commit(struct tx *t, unsigned frt, bool fprf, bool rc)
{
	if (fprf) {
		x86_vmovq_to(B, T0, X0);
		fp_fprf(t);
		x86_shift_ri(B, X86_SHL, 0, T0, 12);
	}
	x86_load(B, 4, false, T1, FPSCR_MEM);
	x86_alu_ri(B, X86_AND, 0, T1, (int32_t)~(FPSCR_FR_FI | (fprf ? 0x1Fu << 12 : 0)));
	if (fprf)
		x86_alu_rr(B, X86_OR, 0, T1, T0);
	x86_test_ri(B, 0, T2, 0xFF);
	uint8_t *exact = fwd(t, X86_E);
	x86_test_ri(B, 0, T1, FPSCR_XX);
	uint8_t *had = fwd(t, X86_NE);
	x86_alu_ri(B, X86_OR, 0, T1, (int32_t)FPSCR_FX);       /* FX: XX is new */
	land(t, had);
	x86_alu_ri(B, X86_OR, 0, T1, (int32_t)(FPSCR_XX | FPSCR_FI));
	land(t, exact);
	x86_store(B, 4, T1, FPSCR_MEM);
	x86_vmovapd(B, fwr(t, frt), X0);
	if (rc)
		cr1_from(t, T1);
}

/* FPR r = the single in ecx, as a double (lfs: exactly; infinities and NaNs keep their bits). Uses rdx, xmm5 */
static void load_single(struct tx *t, unsigned r)
{
	unsigned d = fwr(t, r);
	x86_mov_rr(B, 0, T2, T1);
	x86_shift_ri(B, X86_SHR, 0, T2, 23);
	x86_alu_ri(B, X86_AND, 0, T2, 0xFF);
	x86_alu_ri(B, X86_CMP, 0, T2, 255);
	uint8_t *special = fwd(t, X86_E);
	x86_vmovd_from(B, X5, T1);
	x86_vcvt(B, false, d, d, X5);
	uint8_t *done = fwd_always(t);
	land(t, special);
	x86_extend(B, 4, true, 1, T1, T1);                      /* sign, exponent 0xFF, fraction << 29 */
	x86_shift_ri(B, X86_SHL, 1, T1, 29);
	x86_mov_ri64(B, T2, 0x7FF0000000000000ull);
	x86_alu_rr(B, X86_OR, 1, T1, T2);
	x86_vmovq_from(B, d, T1);
	land(t, done);
}

/* xmm5 = FPR r in single precision, as stfs stores it (see ppc_fp_double_to_single()). Uses rax, rcx, rdx */
static void store_single(struct tx *t, unsigned r)
{
	x86_vmovq_to(B, T0, frd(t, r));
	x86_rorx(B, 1, T1, T0, 52);
	x86_alu_ri(B, X86_AND, 0, T1, 0x7FF);                  /* exponent */
	x86_alu_ri(B, X86_CMP, 0, T1, 896);
	uint8_t *small = fwd(t, X86_BE);
	x86_mov_rr(B, 1, T2, T0);                               /* bits 0-1 and 5-34 */
	x86_shift_ri(B, X86_SHR, 1, T2, 32);
	x86_alu_ri(B, X86_AND, 0, T2, (int32_t)0xC0000000u);
	x86_shift_ri(B, X86_SHR, 1, T0, 29);
	x86_alu_ri(B, X86_AND, 0, T0, 0x3FFFFFFF);
	x86_alu_rr(B, X86_OR, 0, T0, T2);
	uint8_t *done = fwd_always(t);
	land(t, small);
	x86_mov_rr(B, 1, T2, T0);                               /* the sign */
	x86_shift_ri(B, X86_SHR, 1, T2, 63);
	x86_shift_ri(B, X86_SHL, 0, T2, 31);
	x86_alu_ri(B, X86_CMP, 0, T1, 874);
	uint8_t *zero = fwd(t, X86_B);
	/* a denormal single: (fraction | 1 << 52) >> (897 - exponent + 29) */
	x86_shift_ri(B, X86_SHL, 1, T0, 12);
	x86_shift_ri(B, X86_SHR, 1, T0, 12);
	x86_bt_ri(B, X86_BTS, 1, T0, 52);
	x86_unary(B, X86_NEG, 0, T1);
	x86_alu_ri(B, X86_ADD, 0, T1, 897 + 29);
	x86_shiftx(B, X86_SHR, 1, T0, T0, T1);
	x86_alu_rr(B, X86_OR, 0, T2, T0);
	land(t, zero);
	x86_mov_rr(B, 0, T0, T2);
	land(t, done);
	x86_vmovd_from(B, X5, T0);
}

/*
 * The error-free transformation of fma_inexact() (ppc_fp.c): dl = raw =
 * a * c + ad inexactly, u1 = a * c in xmm2 (which it uses up), with the
 * other scratch registers (xmm0, unless it's raw, kept on the stack)
 */
static void fma_inexact(struct tx *t, unsigned a, unsigned c, unsigned ad, unsigned raw)
{
	unsigned t0 = raw == X0 ? X1 : X0, t1 = X4, t2 = X5, t3 = X3, u = X2;
	if (t0 == X0)
		x86_vmovsd(B, true, X0, x86_m(X86_RSP, 0));
	x86_vmovapd(B, t0, u);
	x86_vfma(B, X86_VFMSUB231SD, t0, a, c);                 /* u2 = a * c - u1 */
	x86_vsd(B, X86_VADDSD, t1, ad, t0);                     /* a1 = b + u2 */
	x86_vsd(B, X86_VSUBSD, t2, t1, ad);                     /* a1 - b */
	x86_vsd(B, X86_VSUBSD, t0, t0, t2);                     /* u2 - (a1 - b) */
	x86_vsd(B, X86_VSUBSD, t2, t1, t2);
	x86_vsd(B, X86_VSUBSD, t2, ad, t2);                     /* b - (a1 - (a1 - b)) */
	x86_vsd(B, X86_VADDSD, t0, t2, t0);                     /* z (ad no longer needed: xmm3 is free) */
	x86_vsd(B, X86_VADDSD, t2, u, t1);                      /* b1 = u1 + a1 */
	x86_vsd(B, X86_VSUBSD, t3, t2, u);                      /* bv = b1 - u1 */
	x86_vsd(B, X86_VSUBSD, t1, t1, t3);                     /* a1 - bv */
	x86_vsd(B, X86_VSUBSD, t3, t2, t3);                     /* b1 - bv */
	x86_vsd(B, X86_VSUBSD, u, u, t3);                       /* u1 - (b1 - bv) */
	x86_vsd(B, X86_VADDSD, u, u, t1);                       /* b2 */
	x86_vsd(B, X86_VSUBSD, t2, t2, raw);                    /* b1 - r */
	x86_vsd(B, X86_VADDSD, t2, t2, u);                      /* g */
	x86_vsd(B, X86_VADDSD, t1, t2, t0);                     /* r2 = g + z */
	x86_vsd(B, X86_VSUBSD, t3, t1, t2);
	x86_vsd(B, X86_VSUBSD, t3, t0, t3);                     /* r3 = z - (r2 - g) */
	fp_nonzero(t, t1, false);
	fp_nonzero(t, t3, true);
	if (t0 == X0)
		x86_vmovsd(B, false, X0, x86_m(X86_RSP, 0));
}

/* dl = s = a + b inexactly (Knuth's TwoSum, as sum_inexact() in ppc_fp.c), with xmm3 and xmm5 */
static void sum_inexact(struct tx *t, unsigned a, unsigned b, unsigned s)
{
	x86_vsd(B, X86_VSUBSD, X5, s, a);                       /* bv = s - a */
	x86_vsd(B, X86_VSUBSD, X3, s, X5);                      /* av = s - bv */
	x86_vsd(B, X86_VSUBSD, X3, a, X3);
	x86_vsd(B, X86_VSUBSD, X5, b, X5);
	x86_vsd(B, X86_VADDSD, X5, X3, X5);                     /* (a - av) + (b - bv) */
	fp_nonzero(t, X5, false);
}

/* Which A-form operations natively, and which operands they use (opcode 63 also has fsel and frsqrte) */
static bool fp_a_form(unsigned op, unsigned xo5, bool uses[3])
{
	static const uint32_t valid59 = 1u << 18 | 1u << 20 | 1u << 21 | 1u << 22 | 1u << 24 | 1u << 25 |
					1u << 28 | 1u << 29 | 1u << 30 | 1u << 31;
	static const uint32_t valid63 = 1u << 18 | 1u << 20 | 1u << 21 | 1u << 22 | 1u << 23 | 1u << 25 |
					1u << 26 | 1u << 28 | 1u << 29 | 1u << 30 | 1u << 31;
	uses[0] = xo5 != 22 && xo5 != 24 && xo5 != 26;
	uses[1] = xo5 != 25;
	uses[2] = xo5 == 23 || xo5 == 25 || xo5 >= 28;
	return (op == 59 ? valid59 : valid63) & (1u << xo5);
}

/* Out of line unless the operands of fdiv, fsqrt, fres and frsqrte are tame */
static void fp_operands_tame(struct tx *t, const unsigned h[3], const bool uses[3])
{
	for (unsigned k = 0; k < 3; k++)
		if (uses[k])
			land(t, fp_range(t, h[k], TAME_LO, TAME_HI, true));
}

/* Out of line unless a * c (into xmm2) is tame and didn't underflow to zero (fused operations) */
static void fp_product_tame(struct tx *t, unsigned a, unsigned c)
{
	x86_vsd(B, X86_VMULSD, X2, a, c);
	uint8_t *ok = fp_range(t, X2, TAME_LO, TAME_HI, true);
	fp_either_zero(t, a, c);
	land(t, ok);
}

/*
 * The result of an A-form operation (opcode 59 single, 63 double
 * precision) in xmm0, with its operands in XMM registers h (a, b, c), out
 * of line unless the interpreter's fast path would take it. As there, only
 * division and square roots check their operands: for the others a tame
 * result is enough. With `lazy`, the checks only the inexact check needs
 * (fp_lazy_checks()) are left out; the result is still the interpreter's,
 * with no exception but inexact. Returns the register holding the result
 * before negation and rounding (xmm0 or xmm1). Keeps for fp_inexact():
 * frsqrte's square root, and fmsub's and fnmsub's -b, in xmm3.
 */
static unsigned fp_compute(struct tx *t, unsigned op, unsigned xo5, const unsigned h[3], const bool uses[3], bool lazy)
{
	unsigned a = h[0], b = h[1], c = h[2];
	bool checked = xo5 == 22 || xo5 == 24 || xo5 == 26 || (xo5 == 18 && !lazy);
	if (checked)
		fp_operands_tame(t, h, uses);
	switch (xo5) {
	case 18:                                                /* fdiv */
		fp_zero_test(t, b);
		fp_slow(t, X86_E);
		x86_vsd(B, X86_VDIVSD, X0, a, b);
		{
			uint8_t *ok = fp_range(t, X0, TAME_LO, TAME_HI, true);
			fp_zero_test(t, a);                     /* underflow unless a = 0 */
			fp_slow(t, X86_NE);
			land(t, ok);
		}
		break;
	case 20: case 21:                                       /* fsub, fadd */
		x86_vsd(B, xo5 == 20 ? X86_VSUBSD : X86_VADDSD, X0, a, b);
		land(t, fp_range(t, X0, TAME_LO, TAME_HI, true));
		break;
	case 22: {                                              /* fsqrt: b >= 0 */
		x86_vmovq_to(B, T0, b);
		x86_test_rr(B, 1, T0, T0);
		uint8_t *positive = fwd(t, X86_NS);
		x86_alu_rr(B, X86_ADD, 1, T0, T0);
		fp_slow(t, X86_NE);
		land(t, positive);
		x86_vsd(B, X86_VSQRTSD, X0, b, b);
		break;
	}
	case 24:                                                /* fres */
		fp_zero_test(t, b);
		fp_slow(t, X86_E);
		x86_vmovsd(B, false, X0, K_ONE);
		x86_vsd(B, X86_VDIVSD, X0, X0, b);
		fp_range(t, X0, TAME_LO, TAME_HI, false);
		break;
	case 25: {                                              /* fmul */
		x86_vsd(B, X86_VMULSD, X0, a, c);
		uint8_t *ok = fp_range(t, X0, TAME_LO, TAME_HI, true);
		fp_either_zero(t, a, c);
		land(t, ok);
		break;
	}
	case 26:                                                /* frsqrte: b > 0 */
		x86_vmovq_to(B, T0, b);
		x86_test_rr(B, 1, T0, T0);
		fp_slow(t, X86_LE);
		x86_vsd(B, X86_VSQRTSD, X3, b, b);
		x86_vmovsd(B, false, X0, K_ONE);
		x86_vsd(B, X86_VDIVSD, X0, X0, X3);
		break;
	default: {                                              /* fmsub, fmadd, fnmsub, fnmadd */
		unsigned ad = xo5 & 1 ? b : X3;                 /* (b, or -b as the interpreter has it) */
		if (ad != b)
			x86_vpd_m(B, X86_VXORPD, X3, b, K_SIGN);
		x86_vmovapd(B, X0, ad);
		x86_vfma(B, X86_VFMADD231SD, X0, a, c);
		uint8_t *ok = fp_range(t, X0, TAME_LO, TAME_HI, true);
		if (lazy)                                       /* zero: exact unless the product underflowed */
			fp_product_tame(t, a, c);
		land(t, ok);
		if (!lazy)
			fp_product_tame(t, a, c);
		break;
	}
	}
	unsigned raw = X0;
	if (xo5 >= 30 || op == 59) {
		x86_vmovapd(B, X1, X0);
		raw = X1;
	}
	if (xo5 >= 30)                                          /* (the rounded fma negated, as the interpreter does) */
		x86_vpd_m(B, X86_VXORPD, X0, X0, K_SIGN);
	if (op == 59)
		fp_round_single(t, false);
	return raw;
}

/* What fp_compute() left out for `lazy` */
static void fp_lazy_checks(struct tx *t, unsigned xo5, const unsigned h[3], const bool uses[3])
{
	if (xo5 == 18)
		fp_operands_tame(t, h, uses);
	else if (xo5 >= 28)
		fp_product_tame(t, h[0], h[2]);
}

/* dl = whether the operation fp_compute() did was inexact */
static void fp_inexact(struct tx *t, unsigned op, unsigned xo5, const unsigned h[3], unsigned raw)
{
	unsigned a = h[0], b = h[1], c = h[2];
	switch (xo5) {
	case 18:                                                /* a - r * b */
		x86_vmovapd(B, X4, a);
		x86_vfma(B, X86_VFNMADD231SD, X4, raw, b);
		fp_nonzero(t, X4, false);
		break;
	case 20:
		x86_vpd_m(B, X86_VXORPD, X4, b, K_SIGN);
		sum_inexact(t, a, X4, raw);
		break;
	case 21:
		sum_inexact(t, a, b, raw);
		break;
	case 22:                                                /* b - r * r */
		x86_vmovapd(B, X4, b);
		x86_vfma(B, X86_VFNMADD231SD, X4, raw, raw);
		fp_nonzero(t, X4, false);
		break;
	case 24:                                                /* 1 - r * b */
		x86_vmovsd(B, false, X4, K_ONE);
		x86_vfma(B, X86_VFNMADD231SD, X4, raw, b);
		fp_nonzero(t, X4, false);
		break;
	case 25:                                                /* a * c - r */
		x86_vmovapd(B, X4, raw);
		x86_vfma(B, X86_VFMSUB231SD, X4, a, c);
		fp_nonzero(t, X4, false);
		break;
	case 26:                                                /* b - s * s, 1 - r * s */
		x86_vmovapd(B, X4, b);
		x86_vfma(B, X86_VFNMADD231SD, X4, X3, X3);
		fp_nonzero(t, X4, false);
		x86_vmovsd(B, false, X4, K_ONE);
		x86_vfma(B, X86_VFNMADD231SD, X4, raw, X3);
		fp_nonzero(t, X4, true);
		break;
	default:
		fma_inexact(t, a, c, xo5 & 1 ? b : X3, raw);
		break;
	}
	if (op == 59) {                                         /* ... or rounded to single */
		x86_vmovq_to(B, T0, X0);
		x86_vmovq_to(B, T1, raw);
		if (xo5 >= 30)
			x86_bt_ri(B, X86_BTC, 1, T1, 63);
		x86_alu_rr(B, X86_CMP, 1, T0, T1);
		x86_setcc(B, X86_NE, T0);
		x86_extend(B, 1, false, 0, T0, T0);
		x86_alu_rr(B, X86_OR, 0, T2, T0);
	}
}

/*
 * A-form arithmetic, natively where the interpreter's fast path would take
 * it (see fp_compute()), the FPSCR kept lazily as on AArch64: XX and FX at
 * once (with the inexact check, which is then needed, only while XX is
 * clear), FR, FI and FPRF left pending: the operation and its operands go
 * in struct jit_state, for fp_settle() or the settle stub to work them out
 * when something else might look (C, the interpreter, a compare, mffs).
 */
static void fp_arith(struct tx *t, unsigned op, unsigned xo5, unsigned frt, const unsigned regs[3], const unsigned h[3],
		     const bool uses[3], bool rc)
{
	fp_mode(t, false);
	unsigned raw = fp_compute(t, op, xo5, h, uses, true);

	/* XX clear: was it inexact? Then XX, and FX */
	x86_test_mi(B, 1, x86_m(R_CPU, OFF(fpscr) + 3), FPSCR_XX >> 24);
	uint8_t *xx_set = fwd(t, X86_NE);
	fp_lazy_checks(t, xo5, h, uses);
	fp_inexact(t, op, xo5, h, raw);
	x86_test_ri(B, 0, T2, 0xFF);
	uint8_t *exact = fwd(t, X86_E);
	x86_alu_mi(B, X86_OR, 4, FPSCR_MEM, (int32_t)(FPSCR_FX | FPSCR_XX));
	land(t, exact);
	land(t, xx_set);

	/*
	 * The rest pending: its record (replacing any before) is left to the
	 * next exit, call into C or reader of the FPSCR to store, unless the
	 * result replaces an operand (see fp_record(), fwr()); then the result.
	 */
	struct cache_state *cs = &t->rc;
	cs->fp_rec = FP_PENDING(op, xo5);
	bool replaced = false;
	for (unsigned k = 0; k < 3; k++) {
		cs->fp_rec_ops[k] = uses[k] ? (int8_t)regs[k] : -1;
		replaced |= uses[k] && regs[k] == frt;
	}
	if (replaced)
		fp_record_now(t);
	x86_vmovapd(B, fwr(t, frt), X0);
	fp_cr1(t, rc);
}

/* The settle stub first, if the instruction touches what may be pending and something is */
static void fp_settle_first(struct tx *t, uint32_t touched)
{
	if (!(touched & FPSCR_PENDING))
		return;
	uint16_t pending = t->rc.fp_rec;
	fp_record_now(t);
	uint8_t *settled = NULL;
	if (pending) {
		x86_mov_ri(B, T0, pending);
	} else {
		x86_load(B, 4, false, T0, ST(fp_op));
		x86_test_rr(B, 0, T0, T0);
		settled = fwd(t, X86_E);
	}
	x86_call(B, t->j->settle);
	land(t, settled);
}

/* VX and FEX in ecx worked out again, as fpscr_summarize() (ppc_fp.c) does */
static void fp_summarize(struct tx *t)
{
	x86_alu_ri(B, X86_AND, 0, T1, ~0x60000000);
	x86_test_ri(B, 0, T1, FPSCR_VX_ALL);
	x86_setcc(B, X86_NE, T0);
	x86_extend(B, 1, false, 0, T0, T0);
	x86_shift_ri(B, X86_SHL, 0, T0, 29);
	x86_alu_rr(B, X86_OR, 0, T1, T0);
	x86_mov_rr(B, 0, T0, T1);                               /* VX OX UX ZX XX over their enables */
	x86_shift_ri(B, X86_SHR, 0, T0, 22);
	x86_alu_rr(B, X86_AND, 0, T0, T1);
	x86_test_ri(B, 0, T0, 0xF8);
	x86_setcc(B, X86_NE, T0);
	x86_extend(B, 1, false, 0, T0, T0);
	x86_shift_ri(B, X86_SHL, 0, T0, 30);
	x86_alu_rr(B, X86_OR, 0, T1, T0);
}

/*
 * mffs, mtfsf, mtfsfi, mtfsb0, mtfsb1 and mcrfs, as ppc_fp_exec() does
 * them. What arithmetic left pending stays so, unless they read or change
 * part of it: then it's worked out first, or it's all replaced.
 */
static void fp_fpscr(struct tx *t, uint32_t i, unsigned xo10)
{
	unsigned frt = (i >> 21) & 31, frb = (i >> 11) & 31;
	bool rc = i & 1;
	unsigned hb = xo10 == 711 ? frd(t, frb) : NO_REG;
	switch (xo10) {
	case 583:                                       /* mffs */
		fp_settle_first(t, FPSCR_PENDING);
		x86_load(B, 4, false, T1, FPSCR_MEM);
		x86_vmovq_from(B, fwr(t, frt), T1);
		break;
	case 711: {                                     /* mtfsf */
		unsigned flm = (i >> 17) & 0xFF;
		uint32_t mask = 0;
		for (unsigned k = 0; k < 8; k++)
			if (flm & (0x80u >> k))
				mask |= 0xFu << (28 - 4 * k);
		if ((mask & FPSCR_PENDING) == FPSCR_PENDING) {
			x86_store_imm(B, 8, ST(fp_op), 0);
			t->rc.fp_rec = 0;
		} else {
			fp_settle_first(t, mask);
		}
		x86_vmovd_to(B, T0, hb);                /* (its low word) */
		if (mask == 0xFFFFFFFFu) {
			x86_mov_rr(B, 0, T1, T0);
		} else {
			x86_load(B, 4, false, T1, FPSCR_MEM);
			x86_alu_ri(B, X86_AND, 0, T1, (int32_t)~mask);
			x86_alu_ri(B, X86_AND, 0, T0, (int32_t)mask);
			x86_alu_rr(B, X86_OR, 0, T1, T0);
		}
		break;
	}
	case 134: {                                     /* mtfsfi */
		unsigned sh = 28 - 4 * ((i >> 23) & 7);
		uint32_t mask = 0xFu << sh, val = ((i >> 12) & 0xF) << sh;
		fp_settle_first(t, mask);
		x86_load(B, 4, false, T1, FPSCR_MEM);
		x86_alu_ri(B, X86_AND, 0, T1, (int32_t)~mask);
		if (val)
			x86_alu_ri(B, X86_OR, 0, T1, (int32_t)val);
		break;
	}
	case 38: case 70: {                             /* mtfsb1, mtfsb0 */
		uint32_t bit = 0x80000000u >> frt;
		fp_settle_first(t, bit);
		x86_load(B, 4, false, T1, FPSCR_MEM);
		if (bit == 0x40000000u || bit == 0x20000000u)   /* (FEX and VX are summaries) */
			break;
		if (xo10 == 70) {
			x86_alu_ri(B, X86_AND, 0, T1, (int32_t)~bit);
			break;
		}
		if (bit & FPSCR_EXC_ALL) {              /* a new exception: FX */
			x86_test_ri(B, 0, T1, bit);
			x86_setcc(B, X86_E, T0);
			x86_extend(B, 1, false, 0, T0, T0);
			x86_shift_ri(B, X86_SHL, 0, T0, 31);
			x86_alu_rr(B, X86_OR, 0, T1, T0);
		}
		x86_alu_ri(B, X86_OR, 0, T1, (int32_t)bit);
		break;
	}
	default: {                                      /* mcrfs */
		unsigned crfd = (i >> 23) & 7, sh = 28 - 4 * ((i >> 18) & 7);
		uint32_t clear = 0xFu << sh & (0x80000000u | FPSCR_EXC_ALL), mask = 0xFu << (28 - 4 * crfd);
		fp_settle_first(t, 0xFu << sh);
		x86_load(B, 4, false, T1, FPSCR_MEM);
		x86_rorx(B, 0, T0, T1, (sh - (28 - 4 * crfd)) & 31);   /* the field, moved */
		x86_alu_ri(B, X86_AND, 0, T0, (int32_t)mask);
		unsigned cr = rw(t, G_CR);
		x86_alu_ri(B, X86_AND, 0, cr, (int32_t)~mask);
		x86_alu_rr(B, X86_OR, 0, cr, T0);
		if (clear)
			x86_alu_ri(B, X86_AND, 0, T1, (int32_t)~clear);
		rc = false;
		break;
	}
	}
	if (xo10 != 583) {
		fp_summarize(t);
		x86_store(B, 4, T1, FPSCR_MEM);
		if (xo10 != 64)
			t->fp_mode_ok = false;
	}
	if (rc)
		cr1_from(t, T1);
}

/* Opcodes 59 and 63; false for call_step() */
static bool fp_insn(struct tx *t, uint32_t i)
{
	unsigned op = i >> 26, frt = (i >> 21) & 31, fra = (i >> 16) & 31, frb = (i >> 11) & 31, frc = (i >> 6) & 31;
	unsigned xo5 = (i >> 1) & 31, xo10 = (i >> 1) & 0x3FF;
	bool rc = i & 1;

	if (op == 63 && !(xo5 & 0x10)) {
		switch (xo10) {
		case 40: case 72: case 136: case 264: { /* fneg, fmr, fnabs, fabs */
			unsigned b = frd(t, frb), d = fwr(t, frt);
			if (xo10 == 72)
				x86_vmovapd(B, d, b);
			else if (xo10 == 40)
				x86_vpd_m(B, X86_VXORPD, d, b, K_SIGN);
			else if (xo10 == 264)
				x86_vpd_m(B, X86_VANDPD, d, b, K_ABS);
			else
				x86_vpd_m(B, X86_VORPD, d, b, K_SIGN);
			fp_cr1(t, rc);
			return true;
		}
		case 0: case 32: {                      /* fcmpu, fcmpo */
			unsigned crf = (i >> 23) & 7, a = frd(t, fra), b = frd(t, frb);
			fp_record_now(t);
			if (!fp_begin(t, i))
				return false;
			x86_vucomisd(B, a, b);
			fp_slow(t, X86_P);              /* unordered: VXSNAN, VXVC */
			x86_mov_ri(B, T0, CR_EQ);
			x86_mov_ri(B, T1, CR_LT);
			x86_cmov(B, X86_B, 0, T0, T1);
			x86_mov_ri(B, T1, CR_GT);
			x86_cmov(B, X86_A, 0, T0, T1);
			x86_load(B, 4, false, T1, ST(fp_op));
			x86_test_rr(B, 0, T1, T1);
			uint8_t *settled = fwd(t, X86_E);
			x86_lea(B, 0, T1, x86_m(T0, 0x10));     /* FPCC over what's pending */
			x86_store(B, 4, T1, ST(fp_fpcc));
			uint8_t *done = fwd_always(t);
			land(t, settled);
			x86_load(B, 4, false, T1, FPSCR_MEM);
			x86_alu_ri(B, X86_AND, 0, T1, ~0xF000);
			x86_mov_rr(B, 0, T2, T0);
			x86_shift_ri(B, X86_SHL, 0, T2, 12);
			x86_alu_rr(B, X86_OR, 0, T1, T2);
			x86_store(B, 4, T1, FPSCR_MEM);
			land(t, done);
			uint32_t mask = 0xFu << (28 - 4 * crf);
			if (28 - 4 * crf)
				x86_shift_ri(B, X86_SHL, 0, T0, 28 - 4 * crf);
			unsigned cr = rw(t, G_CR);
			x86_alu_ri(B, X86_AND, 0, cr, (int32_t)~mask);
			x86_alu_rr(B, X86_OR, 0, cr, T0);
			fp_end(t);
			return true;
		}
		case 12: {                              /* frsp */
			unsigned b = frd(t, frb);
			if (!fp_begin(t, i))
				return false;
			fp_mode(t, false);
			land(t, fp_range(t, b, TAME_LO, TAME_HI, true));
			x86_vmovapd(B, X0, b);
			x86_alu_rr(B, X86_XOR, 0, T2, T2);
			fp_round_single(t, true);
			fp_commit(t, frt, true, rc);
			x86_store_imm(B, 8, ST(fp_op), 0);
			t->rc.fp_rec = 0;                       /* (it replaced all that was pending) */
			fp_end(t);
			return true;
		}
		case 14: case 15: {                     /* fctiw, fctiwz */
			unsigned b = frd(t, frb);
			if (!fp_begin(t, i))
				return false;
			fp_mode(t, xo10 == 15);
			fp_settle_first(t, FPSCR_PENDING);      /* (FPRF stays) */
			x86_vroundsd(B, X5, b, b, (xo10 == 14 ? 0 : 3) | 8);
			x86_vcvttsd2si(B, 0, T0, X5);
			x86_vcvtsi2sd(B, 0, X4, X4, T0);
			x86_vucomisd(B, X4, X5);
			fp_slow(t, X86_NE);                     /* out of range, NaN */
			fp_slow(t, X86_P);
			x86_vucomisd(B, X5, b);
			x86_setcc(B, X86_NE, T2);
			x86_extend(B, 1, false, 0, T2, T2);
			x86_mov_ri64(B, T1, 0xFFF8000000000000ull);     /* FP_INT_HI */
			x86_alu_rr(B, X86_OR, 1, T0, T1);
			x86_vmovq_from(B, X0, T0);
			fp_commit(t, frt, false, rc);
			fp_end(t);
			return true;
		}
		case 583: case 711: case 134: case 38: case 70: case 64:
			fp_fpscr(t, i, xo10);
			return true;
		default:
			return false;
		}
	}

	bool uses[3];
	if (!fp_a_form(op, xo5, uses))
		return false;
	unsigned h[3] = { 0, 0, 0 };
	const unsigned regs[3] = { fra, frb, frc };
	for (unsigned k = 0; k < 3; k++)
		if (uses[k])
			h[k] = frd(t, regs[k]);
	if (xo5 == 23) {                                /* fsel: a >= 0 (not a NaN) ? c : b */
		x86_vpd(B, X86_VXORPD, X5, X5, X5);
		x86_vcmpsd(B, X4, X5, h[0], 2);
		x86_vblendvpd(B, fwr(t, frt), h[1], h[2], X4);
		fp_cr1(t, rc);
		return true;
	}
	if (!fp_begin(t, i))
		return false;
	fp_arith(t, op, xo5, frt, regs, h, uses, rc);
	fp_end(t);
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
	case 1014:                                                      /* dcbz */
		t->nstored = 0;
		zero_block(t, ra, rb);
		return EMITTED;

	case 23: case 55:   load_store(t, 2, true, false, xo == 55, true, false, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 87: case 119:  load_store(t, 0, true, false, xo == 119, true, false, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 279: case 311: load_store(t, 1, true, false, xo == 311, true, false, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 343: case 375: load_store(t, 1, true, true, xo == 375, true, false, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 151: case 183: load_store(t, 2, false, false, xo == 183, true, false, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 215: case 247: load_store(t, 0, false, false, xo == 247, true, false, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 407: case 439: load_store(t, 1, false, false, xo == 439, true, false, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 535: case 567: load_store(t, 2, true, false, xo == 567, true, false, LS_SINGLE, rd_, ra, rb, 0); return EMITTED;
	case 599: case 631: load_store(t, 3, true, false, xo == 631, true, false, LS_DOUBLE, rd_, ra, rb, 0); return EMITTED;
	case 663: case 695: load_store(t, 2, false, false, xo == 695, true, false, LS_SINGLE, rd_, ra, rb, 0); return EMITTED;
	case 727: case 759: load_store(t, 3, false, false, xo == 759, true, false, LS_DOUBLE, rd_, ra, rb, 0); return EMITTED;
	case 534: case 790: load_store(t, xo == 534 ? 2 : 1, true, false, false, true, true, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 662: case 918: load_store(t, xo == 662 ? 2 : 1, false, false, false, true, true, LS_GPR, rd_, ra, rb, 0);
		return EMITTED;                                         /* (lwbrx lhbrx stwbrx sthbrx) */

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

/*
 * An instruction that only leaves for the kernel (an unconditional trap, a
 * system call, a host call): straight out, with pc and exit_insn as the
 * interpreter would leave them (the instruction not retired).
 */
static int kernel_exit(struct tx *t, uint32_t insn, unsigned kind)
{
	rc_flush(t);
	x86_store_imm(B, 4, x86_m(R_CPU, OFF(pc)), t->pc);
	x86_store_imm(B, 4, x86_m(R_CPU, OFF(exit_insn)), insn);
	budget_sub(t, t->k);
	x86_mov_ri(B, T0, kind);
	x86_jmp(B, t->j->exit);
	return EXITED;
}

/*
 * sc: NanoKernel 2's quick calls (r0 -1 and -2) are answered here while the
 * kernel lets them be (see ppc.h), retiring nothing (the budget gets the
 * instruction back); otherwise the kernel takes it.
 */
static int system_call(struct tx *t, uint32_t insn)
{
	unsigned r0 = rd(t, 0), xer = rd(t, G_XER);
	struct cache_state at = t->rc;
	x86_lea(B, 0, T0, x86_m(r0, 2));                        /* -2, -1: 0, 1 */
	x86_alu_ri(B, X86_CMP, 0, T0, 1);
	uint8_t *other = fwd(t, X86_A);
	time_base(t);
	x86_alu_rm(B, X86_CMP, 1, T0, x86_m(R_CPU, OFF(sc_until)));
	uint8_t *quick = fwd(t, X86_B);
	land(t, other);
	kernel_exit(t, insn, EXIT_SC);
	t->rc = at;
	land(t, quick);
	x86_load(B, 4, false, T2, x86_m(R_CPU, OFF(sc_blue)));
	x86_mov_ri(B, T0, CR_EQ);                               /* CR0: GT if sc_blue isn't 0, else EQ; SO */
	x86_mov_ri(B, T1, CR_GT);
	x86_test_rr(B, 0, T2, T2);
	x86_cmov(B, X86_NE, 0, T0, T1);
	x86_mov_rr(B, 0, T1, xer);
	x86_shift_ri(B, X86_SHR, 0, T1, 31);
	x86_alu_rr(B, X86_OR, 0, T0, T1);
	x86_shift_ri(B, X86_SHL, 0, T0, 28);
	unsigned cr = rw(t, G_CR);
	x86_alu_ri(B, X86_AND, 0, cr, 0x0FFFFFFF);
	x86_alu_rr(B, X86_OR, 0, cr, T0);
	x86_load(B, 4, false, T0, x86_m(R_CPU, OFF(sc_word)));  /* r0 = sc_blue for -1, sc_word for -2 */
	x86_alu_ri(B, X86_CMP, 0, r0, -1);
	x86_cmov(B, X86_E, 0, T0, T2);
	x86_mov_rr(B, 0, wr(t, 0), T0);
	x86_store_imm(B, 4, x86_m(R_CPU, OFF(exit_insn)), insn);
	x86_alu_ri(B, X86_ADD, 1, R_BUDGET, 1);
	return EMITTED;
}

/*
 * A kernel call (twi 31,r31,n): while the kernel lets it (see ppc.h), its
 * function makes the call in place, and the block is left for wherever
 * that leads, retiring nothing; not as a branch (no stop at the hook).
 * Otherwise, or if the function declines, the kernel takes it.
 */
static int kernel_call(struct tx *t, uint32_t insn)
{
	rc_flush(t);
	time_base(t);
	x86_alu_rm(B, X86_CMP, 1, T0, x86_m(R_CPU, OFF(kcall_until)));
	uint8_t *late = fwd(t, X86_AE);
	fixed_store(t, NFIXED);
	x86_store_imm(B, 4, x86_m(R_CPU, OFF(pc)), t->pc);
	x86_mov_rr(B, 1, X86_RDI, R_CPU);
	x86_mov_ri(B, X86_RSI, insn & 0xFFFF);
	x86_call_m(B, ST(kcall));
	fixed_load(t, NFIXED);
	x86_test_ri(B, 0, X86_RAX, 0xFF);
	uint8_t *declined = fwd(t, X86_E);
	rc_reset(t);
	ld32(t, T0, OFF(pc));
	budget_sub(t, t->k);
	x86_test_ri(B, 0, T0, 3);                               /* (lookup_jump() needs a word's address) */
	x86_jcc(B, X86_NE, t->j->miss_exit);
	x86_alu_rm(B, X86_CMP, 0, T0, ST(hook));
	uint8_t *other = fwd(t, X86_NE);
	x86_jmp(B, t->j->miss_exit);
	land(t, other);
	lookup_jump(t);
	land(t, late);
	land(t, declined);
	return kernel_exit(t, insn, EXIT_TRAP);
}

static int insn(struct tx *t, uint32_t i)
{
	unsigned op = i >> 26, rd_ = (i >> 21) & 31, ra = (i >> 16) & 31;
	int32_t simm = (int16_t)(i & 0xFFFF);
	uint32_t uimm = i & 0xFFFF;

	switch (op) {
	case 3:                                 /* twi: unless it always traps, the interpreter tests */
		if (rd_ == 31 && ra == 31)
			return kernel_call(t, i);
		return rd_ == 31 ? kernel_exit(t, i, EXIT_TRAP) : STOPPED;
	case 6:                                 /* host call */
		return kernel_exit(t, i, EXIT_HOSTCALL);
	case 17:                                /* sc */
		return system_call(t, i);
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
		x86_mov_rr(B, 0, wr(t, rd_), T0);
		xer_update(t, 2, false);
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
		unsigned a = rd(t, ra), d = wr(t, rd_);
		alu_imm(t, X86_ADD, d, a, (uint32_t)simm);
		xer_update(t, 1, false);
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
	case 32: case 33: load_store(t, 2, true, false, op == 33, false, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 34: case 35: load_store(t, 0, true, false, op == 35, false, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 40: case 41: load_store(t, 1, true, false, op == 41, false, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 42: case 43: load_store(t, 1, true, true, op == 43, false, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 36: case 37: load_store(t, 2, false, false, op == 37, false, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 38: case 39: load_store(t, 0, false, false, op == 39, false, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 44: case 45: load_store(t, 1, false, false, op == 45, false, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 46: case 47: load_store_multiple(t, op == 46, rd_, ra, simm); return EMITTED;
	case 48: case 49: load_store(t, 2, true, false, op == 49, false, false, LS_SINGLE, rd_, ra, 0, simm); return EMITTED;
	case 50: case 51: load_store(t, 3, true, false, op == 51, false, false, LS_DOUBLE, rd_, ra, 0, simm); return EMITTED;
	case 52: case 53: load_store(t, 2, false, false, op == 53, false, false, LS_SINGLE, rd_, ra, 0, simm); return EMITTED;
	case 54: case 55: load_store(t, 3, false, false, op == 55, false, false, LS_DOUBLE, rd_, ra, 0, simm); return EMITTED;
	case 59: case 63:
		if (!fp_insn(t, i))
			call_step(t, i);
		return EMITTED;
	default:                /* string operations, ... */
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
	if ((size_t)(j->cold_start - j->code_free) < BLOCK_RESERVE || (size_t)(j->code_end - j->cold_free) < BLOCK_RESERVE ||
	    j->nblocks >= MAX_BLOCKS || (!j->pages[page] && j->ncode_pages + 1 >= MAX_CODE_PAGES) ||
	    j->nsites + MAX_BLOCK_INSNS > MAX_SITES)
		jit_flush(j);
	struct jit_page *pg = jit_get_page(j, page);

	static struct tx t;
	t.j = j;
	t.max_insns = max_insns;
	uint8_t *start = (uint8_t *)(((uintptr_t)j->code_free + sizeof(struct jit_block *) + 15) & ~(uintptr_t)15);
	t.x = (struct x86_buf){ start, j->cold_start, false };  /* (the block pointer goes just before) */
	t.nfix = 0;
	t.nlinks = 0;
	t.nbexits = 0;
	t.nslow = 0;
	t.nio = 0;
	t.nsld = 0;
	t.nool = 0;
	t.nfps = 0;
	t.fp_mode_ok = false;
	t.nstored = 0;
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
		t.fp_pins = 0;
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

	/*
	 * What follows is out of line: the taken paths of conditional branches,
	 * which run often, after the main line; the rest apart (see struct jit)
	 */
	rc_reset(&t);
	branch_exits(&t);
	uint8_t *end = t.x.p;
	t.x = (struct x86_buf){ j->cold_free, j->code_end, t.x.overflow };
	slow_stores(&t);
	slow_loads(&t);
	slow_ios(&t);
	slow_fps(&t);
	ools(&t);
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

	/*
	 * Exits to known pcs: straight to the block there if there is one (this
	 * one, perhaps), as C would link it; else ask C for a link the first time
	 */
	for (unsigned l = 0; l < t.nlinks && !t.x.overflow; l++) {
		struct link *ln = &t.links[l];
		const void *to = ln->target == pc0 ? (pc0 != j->linked_hook ? start : NULL) : *table_entry(j, ln->target);
		if (to && to != j->miss_exit && to != j->hook_exit) {
			x86_patch32(ln->site, to);
			j->links++;
			continue;
		}
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
	j->cold_free = t.x.p;
	struct jit_block *b = jit_add_block(j, pg, pc0, budget_needed, cover, start, end, redispatch);
	j->translated_insns += n;
	return b;
}

struct jit_block *arch_translate(struct jit *j, uint32_t pc0)
{
	return translate_upto(j, pc0, MAX_BLOCK_INSNS);
}

/* ---- entry, exit and dispatch ------------------------------------------ */

/*
 * The settle stub, called with FP_PENDING() in eax: works out FR, FI and
 * FPRF of the pending operation as fp_settle() does, through a routine for
 * each (the operands in xmm13-xmm15, saved meanwhile), or, for operands
 * the interpreter's fast path wouldn't take (which fp_compute()'s lazy
 * checks let through), by calling fp_settle(), keeping the registers
 * generated code holds in caller-saved ones. Uses the scratch registers.
 */
static void build_settle(struct tx *t)
{
	static const uint8_t keep[] = { X86_R10, X86_R11, X86_RSI, X86_RDI, X86_R8, X86_R9 };
	uint8_t *slow[64 * MAX_FP_EXITS], *exits[64];
	unsigned nslow = 0, nexits = 0;
	x86_alu_ri(B, X86_SUB, 1, X86_RSP, 40);                 /* ([rsp] for fma_inexact()) */
	for (unsigned k = 0; k < 3; k++)
		x86_vmovsd(B, true, 13 + k, x86_m(X86_RSP, 8 + 8 * (int32_t)k));
	x86_alu_ri(B, X86_AND, 0, T0, 0x3F);
	x86_lea(B, 1, T1, x86_rip(t->x.p));                     /* (the table, below) */
	uint8_t *lea_end = t->x.p;
	x86_jmp_m(B, x86_mx(T1, T0, 3, 0));
	while ((uintptr_t)t->x.p & 7)
		x86_int3(B);
	uint8_t *table = t->x.p;
	if (!t->x.overflow) {
		int32_t rel = (int32_t)(table - lea_end);
		memcpy(lea_end - 4, &rel, 4);
	}
	for (unsigned k = 0; k < 64; k++)
		x86_imm(B, 0, 8);
	const uint8_t *entry[64] = { 0 };
	for (unsigned k = 0; k < 64; k++) {
		unsigned op = k & 0x20 ? 59 : 63, xo5 = k & 31;
		bool uses[3];
		if (xo5 == 23 || !fp_a_form(op, xo5, uses))
			continue;
		entry[k] = t->x.p;
		const unsigned h[3] = { 13, 14, 15 };
		for (unsigned r = 0; r < 3; r++)
			if (uses[r])
				x86_vmovsd(B, false, h[r], ST(fp_ops[r]));
		t->nfps = 1;
		t->fps[0].nat = 0;
		unsigned raw = fp_compute(t, op, xo5, h, uses, false);
		for (unsigned b = 0; b < t->fps[0].nat; b++)
			slow[nslow++] = t->fps[0].at[b];
		fp_inexact(t, op, xo5, h, raw);
		x86_vmovq_to(B, T0, X0);
		fp_fprf(t);
		x86_shift_ri(B, X86_SHL, 0, T0, 12);
		x86_load(B, 4, false, T1, FPSCR_MEM);
		x86_alu_ri(B, X86_AND, 0, T1, (int32_t)~FPSCR_PENDING);
		x86_alu_rr(B, X86_OR, 0, T1, T0);
		x86_test_ri(B, 0, T2, 0xFF);
		uint8_t *exact = fwd(t, X86_E);
		x86_alu_ri(B, X86_OR, 0, T1, (int32_t)FPSCR_FI);
		land(t, exact);
		x86_load(B, 4, false, T0, ST(fp_fpcc));
		x86_test_rr(B, 0, T0, T0);
		uint8_t *no_fpcc = fwd(t, X86_E);
		x86_alu_ri(B, X86_AND, 0, T1, ~0xF000);
		x86_alu_ri(B, X86_AND, 0, T0, 0xF);
		x86_shift_ri(B, X86_SHL, 0, T0, 12);
		x86_alu_rr(B, X86_OR, 0, T1, T0);
		land(t, no_fpcc);
		x86_store(B, 4, T1, FPSCR_MEM);
		x86_store_imm(B, 8, ST(fp_op), 0);
		exits[nexits++] = fwd_always(t);
	}
	t->nfps = 0;

	/* fp_settle(), keeping the registers generated code holds in caller-saved ones */
	const uint8_t *c_settle = t->x.p;
	for (unsigned b = 0; b < nslow; b++)
		land(t, slow[b]);
	x86_alu_ri(B, X86_SUB, 1, X86_RSP, 112);
	for (unsigned k = 0; k < sizeof keep; k++)
		x86_store(B, 8, keep[k], x86_m(X86_RSP, 8 * (int32_t)k));
	for (unsigned k = 0; k < 7; k++)
		x86_vmovsd(B, true, 6 + k, x86_m(X86_RSP, 48 + 8 * (int32_t)k));
	x86_mov_rr(B, 1, X86_RDI, R_CPU);
	x86_call_m(B, ST(settle));
	for (unsigned k = 0; k < sizeof keep; k++)
		x86_load(B, 8, false, keep[k], x86_m(X86_RSP, 8 * (int32_t)k));
	for (unsigned k = 0; k < 7; k++)
		x86_vmovsd(B, false, 6 + k, x86_m(X86_RSP, 48 + 8 * (int32_t)k));
	x86_alu_ri(B, X86_ADD, 1, X86_RSP, 112);

	for (unsigned e = 0; e < nexits; e++)
		land(t, exits[e]);
	for (unsigned k = 0; k < 3; k++)
		x86_vmovsd(B, false, 13 + k, x86_m(X86_RSP, 8 + 8 * (int32_t)k));
	x86_alu_ri(B, X86_ADD, 1, X86_RSP, 40);
	x86_ret(B);
	if (!t->x.overflow)
		for (unsigned k = 0; k < 64; k++) {
			uint64_t to = (uint64_t)(uintptr_t)(entry[k] ? entry[k] : c_settle);
			memcpy(table + 8 * k, &to, 8);
		}
}

static void build_stubs(struct jit *j)
{
	static struct tx tx;
	struct tx *t = &tx;
	t->j = j;
	t->x = (struct x86_buf){ j->code, j->code_end, false };
	static const uint8_t saved[] = { X86_RBP, X86_RBX, X86_R12, X86_R13, X86_R14, X86_R15 };

	/* constants (16 bytes each, for the packed logical operations) */
	j->k_sign = t->x.p;
	x86_imm(B, 0x8000000000000000ull, 8);
	x86_imm(B, 0, 8);
	j->k_abs = t->x.p;
	x86_imm(B, 0x7FFFFFFFFFFFFFFFull, 8);
	x86_imm(B, 0, 8);
	j->k_one = t->x.p;
	x86_imm(B, 0x3FF0000000000000ull, 8);
	x86_imm(B, 0, 8);
	j->k_cr = t->x.p;
	for (unsigned f = 0; f < 8; f++) {
		x86_imm(B, (uint32_t)CR_GT << (28 - 4 * f), 4);
		x86_imm(B, (uint32_t)CR_LT << (28 - 4 * f), 4);
	}

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

	/* mmio() for slow_ios(), called with its arguments from rsi on set, cached registers written back */
	j->mmio_call = t->x.p;
	fixed_store(t, NFIXED);
	x86_alu_ri(B, X86_SUB, 1, X86_RSP, 8);                 /* (rsp 16-byte aligned again) */
	x86_mov_rr(B, 1, X86_RDI, R_CPU);
	x86_call_m(B, ST(mmio));
	x86_alu_ri(B, X86_ADD, 1, X86_RSP, 8);
	fixed_load(t, NFIXED);
	x86_ret(B);

	j->settle = t->x.p;
	build_settle(t);
	while ((uintptr_t)t->x.p & 4095)        /* (the blocks area at a 4 KB boundary: see the plan's findings) */
		x86_int3(B);

	j->blocks_start = j->code_free = t->x.p;
}

void arch_init(struct jit *j)
{
	build_stubs(j);
	j->cold_share = 7;              /* (the out-of-line code is about 40%) */
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
	const char *hide = getenv("BLUE_JIT_HIDE");     /* (tests: features to take as missing, comma-separated) */
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
	marks[0] = (struct jit_mark){ "enter", j->code };     /* (with the constants) */
	marks[1] = (struct jit_mark){ "exit", j->exit };
	marks[2] = (struct jit_mark){ "side exit", j->side_exit };
	marks[3] = (struct jit_mark){ "hook", j->hook_exit };
	marks[4] = (struct jit_mark){ "miss", j->miss_exit };
	marks[5] = (struct jit_mark){ "link", j->link_branch };
	marks[6] = (struct jit_mark){ "mmio", j->mmio_call };
	marks[7] = (struct jit_mark){ "settle", j->settle };
	return 8;
}

#endif
