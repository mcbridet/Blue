#define _GNU_SOURCE                     /* dladdr(), for the profiler */
#include "jit.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) && (defined(__linux__) || defined(__APPLE__))

#include <signal.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <pthread.h>
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

#include "a64.h"
#include "ppc_fp.h"

#define CODE_SIZE (64u << 20)
#define BLOCK_RESERVE (64u << 10)       /* free code space needed to start a block */
#define BLOCK_MAX_WORDS 6000u           /* stop growing a block beyond this (branch ranges) */
#define MAX_BLOCK_INSNS 128u
#define MAX_FIXUPS 1024u
#define GUEST_PAGES (1u << (32 - MEM_PAGE_SHIFT))
#define MAX_BLOCKS (1u << 20)
#define MAX_CODE_PAGES (1u << 16)
#define MAX_SITES (1u << 22)
#define FAST_W_BIT 4                    /* MEM_FAST_W */
#define FAST_R_BIT 5                    /* MEM_FAST_R */

/* Why generated code returned to C */
enum { EXIT_MISS, EXIT_BUDGET, EXIT_INTERP, EXIT_HOOK, EXIT_LINK, EXIT_TRAP, EXIT_SC, EXIT_HOSTCALL, EXIT_MMIO,
       EXIT_KINDS };

/* Host registers held for the whole of a run */
enum {
	R_CPU = 19,     /* struct ppc_cpu * */
	R_MEM = 20,     /* guest address 0, in generated code's view (see fastmem) */
	R_BUDGET = 22,  /* instructions that may still run */
	R_FLAGS = 21,   /* page flags */
	R_TABLE = 23,   /* block table, first level */
	R_STATE = 25,   /* struct jit_state * */
	R_CBASE = 30,   /* guest address 0, as C sees it (never faults); calls into C go through it */
};

/*
 * A translated block. Its code starts with a budget check (and is preceded
 * by a pointer to this struct); exits to a known pc are patched into direct
 * branches to the block there once it exists. When a block is invalidated,
 * its first instruction becomes a branch to `redispatch` (back to C, which
 * looks the pc up again), so branches into it stay correct.
 */
struct jit_block {
	uint32_t pc, ninsn;             /* instructions it retires (at most) */
	uint32_t cover;                 /* words translated from pc on in its page (the rest is ROM) */
	const void *code;
	uint32_t *redispatch;
	struct jit_block *next;         /* same page */
};

#define PAGE_WORDS (MEM_PAGE_SIZE / 4)

/* A guest page that blocks were translated from */
struct jit_page {
	struct jit_block *blocks;
	uint64_t words[PAGE_WORDS / 64];        /* instruction words those blocks cover */
	const void **l2;                        /* the page's block table */
	uint8_t runs[PAGE_WORDS];               /* interpreted runs from each word (see find()) */
	bool followed;                          /* blocks elsewhere hold code from here (ROM) */
	uint64_t slow[PAGE_WORDS / 64];         /* words whose memory accesses are checked (see fastmem) */
	uint64_t io[PAGE_WORDS / 64];           /* ... and offered to the mmio hook out of line (slow_ios()) */
};

/* An access in generated code that may fault, and its side exit (offsets in words from `code`) */
struct jit_site {
	uint32_t at, stub;
	uint32_t pc;                            /* guest instruction */
	uint32_t block;                         /* index in blocks */
};

/* Read and written by generated code through R_STATE */
struct jit_state {
	uint8_t *membase;
	uint8_t *pageflags;
	const void ***l1;
	uint64_t remaining;
	bool (*step)(struct ppc_cpu *, uint32_t, uint32_t);
	void (*code_store)(struct jit_state *, uint32_t, uint64_t, uint32_t);
	unsigned (*mmio)(struct ppc_cpu *, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t);
	uint32_t *link_site;            /* EXIT_LINK: the branch to point at the next block */
	struct jit_page **pages;
	uint32_t hook;
	uint32_t code_changed;          /* set by invalidations; checked after the interpreter's stores */
	uint8_t *fastbase;              /* R_MEM */
	uint32_t fault_site;            /* set by the fault handler: site + 1 */
	uint64_t entry_budget;          /* R_BUDGET when generated code was entered */
	/*
	 * FP arithmetic whose FR, FI and FPRF are still to be worked out (see
	 * fp_arith()), or 0; 0x10 | FPCC if a compare has set FPCC since, or 0;
	 * the instruction's operands.
	 */
	uint32_t fp_insn, fp_fpcc;
	uint64_t fp_ops[3];
	uint32_t scratch;
	uint64_t mmio_return;           /* (the mmio stub's) */
	uint32_t mmio_exit;             /* how slow_ios() leaves after an access the hook did */
};

/* (generated code clears fp_insn and fp_fpcc with one 64-bit store) */
_Static_assert(offsetof(struct jit_state, fp_insn) % 8 == 0 &&
	       offsetof(struct jit_state, fp_fpcc) == offsetof(struct jit_state, fp_insn) + 4, "jit_state layout");

struct jit {
	struct jit_state st;
	struct mem *mem;
	uint32_t *code, *code_end, *code_free, *blocks_start;
	uint32_t (*enter)(struct ppc_cpu *, const void *, uint64_t, struct jit_state *);
	const uint32_t *exit, *side_exit, *link, *link_branch, *hook_exit, *miss_exit, *lookup, *mmio_call;
	uint32_t linked_hook;           /* blocks are never linked to this pc */
	/* the match hook blocks are translated for: only a block of its own runs its pc, checking first */
	uint32_t match_pc;
	uint16_t match_value;
	uint8_t match_reg;
	bool sync_pending;              /* see icache_sync() */
	bool running;                   /* generated code is running (in a call into C) */
	bool flush_pending;
	unsigned hot;                   /* interpreted runs before a block is translated */
	/*
	 * Block table for generated code: l1[guest page][word] is the code of
	 * the block starting there, or miss_exit (none yet), or hook_exit.
	 * Pages without blocks share l2_empty.
	 */
	const void ***l1;
	const void **l2_empty, **l2_pool;
	struct jit_page **pages;        /* per guest page */
	struct jit_page *page_pool;
	uint32_t *code_pages;           /* guest pages that have a struct jit_page */
	uint32_t ncode_pages;
	struct jit_block *blocks;
	uint32_t nblocks;

	/* fastmem: generated code's view of guest memory, and its accesses that may fault */
	bool fastmem;
	uint8_t *view;
	unsigned host_shift;            /* host page size */
	uint8_t *host_prot;             /* per host page of the view */
	struct jit_site *sites;         /* in order of address */
	uint32_t nsites;
	uint32_t fault_ea;              /* guest address of the last fault (set with st.fault_site) */

	unsigned long translated, invalidations, flushes, links, exits[EXIT_KINDS], faults, slowed, protects;
	uint64_t translated_insns;
};

/* ---- code emission ------------------------------------------------------- */

enum { FIX_IMM19, FIX_IMM14, FIX_SITE };

/*
 * Guest registers in host registers. The hottest (across the 68k emulator
 * and native code) live in fixed host registers for as long as generated
 * code runs: loaded on entry, stored on every exit to C and around calls
 * into C. The others are cached within a block: loaded on first use,
 * changed in place, and written back at block exits, before calls into C
 * and in side exits (each stores what is dirty at its point). Every
 * instruction makes its checks (which may exit) before it changes a
 * register, and pins the cache slots it uses so they aren't evicted under
 * it. Register indices: GPRs 0-31, then these. x18 is never used (macOS
 * reserves it), so generated code is the same on every host.
 */
#define G_CR 32u
#define G_XER 33u
#define G_LR 34u
#define G_CTR 35u
#define NGUEST 36u
#define NFIXED 8u
#define CALLER_SAVED_FIXED 4u           /* the first fixed registers don't survive calls to C */
#define NCACHE 5u
#define CALLER_SAVED_SLOTS 4u           /* nor do the first cache slots */

static const uint8_t fixed_guest[NFIXED] = { G_CR, G_LR, 3, 4, 1, 24, 27, 29 };
static const uint8_t fixed_reg[NFIXED] = { 12, 13, 14, 15, 26, 27, 28, 29 };
static const uint8_t cache_reg[NCACHE] = { 7, 8, 16, 17, 24 };

/* The host register a guest register is fixed in, or 0 */
static unsigned fixed_host(unsigned g)
{
	for (unsigned k = 0; k < NFIXED; k++)
		if (fixed_guest[k] == g)
			return fixed_reg[k];
	return 0;
}

struct cache_state {
	int8_t guest[NCACHE];           /* guest register in each slot, or -1 */
	uint8_t dirty;                  /* slots not yet written back */
};

struct fixup {
	uint32_t *at;
	unsigned type;
	unsigned k;                     /* side exit: instructions retired before it */
	bool after;                     /* leave after instruction k instead (it has completed) */
	struct cache_state cs;          /* registers to write back on the way out */
};

/* Out-of-line path of a store to anything but plain writable memory (MEM_FAST_W clear) */
struct slow_store {
	uint32_t *at;                   /* the TBZ to it */
	uint32_t *rejoin;
	unsigned k, size;
	struct cache_state cs;
	bool update;                    /* rA is still to update (from w1) */
	unsigned ra;
	bool io;                        /* one for the mmio hook, if not writable (see slow_ios()) */
	uint32_t insn, pc;
};

/*
 * Out-of-line path of a load or store to a page that isn't readable or
 * writable (a device's, perhaps), offered to the mmio hook (slow_ios())
 */
struct slow_io {
	uint32_t *at;                   /* the TBZ to it */
	uint32_t *rejoin;               /* after the instruction */
	unsigned k, bytes;
	bool write, reload_ea;          /* (the store's update of rA, after rejoin, needs the EA in w1) */
	uint32_t insn, pc;
	struct cache_state before, after;
};

#define MAX_SLOW_STORES 160u
#define MAX_SLOW_IOS 160u
#define MAX_LINKS 160u
#define MAX_SLOW_FP MAX_BLOCK_INSNS
#define MAX_FP_EXITS 16u

/* A conditional branch the block continues past: its taken path, out of line */
struct branch_exit {
	uint32_t *at;                   /* the branch to it */
	unsigned type;                  /* FIX_IMM14, FIX_IMM19 */
	unsigned k;                     /* the branch's index in the block */
	uint32_t target;
	struct cache_state cs;          /* registers to write back on the way out */
};

/* Out-of-line path of a floating-point instruction: the interpreter runs it (see fp_insn()) */
struct slow_fp {
	uint32_t *at[MAX_FP_EXITS];     /* conditional branches to it (imm19) */
	unsigned nat;
	uint32_t *rejoin;
	struct cache_state cs;
	uint32_t insn, pc;
};

/* Exit to a known pc */
struct link {
	uint32_t *site;                 /* B, first to a stub that asks for the link */
	uint32_t target;
	bool after_branch;
};

struct tx {
	struct jit *j;
	uint32_t *p, *end;
	bool overflow;
	bool too_far;                   /* a branch to out-of-line code is out of range */
	unsigned max_insns;
	uint32_t pc;                    /* instruction being translated */
	uint32_t insn;                  /* ... its word */
	unsigned k;                     /* its index in the block */
	uint32_t pcs[MAX_BLOCK_INSNS + 1];      /* the pc of each (the block may follow branches) */
	struct fixup fix[MAX_FIXUPS];
	unsigned nfix;
	struct slow_store slow[MAX_SLOW_STORES];
	unsigned nslow;
	struct slow_io io[MAX_SLOW_IOS];
	unsigned nio;
	struct link links[MAX_LINKS];
	unsigned nlinks;
	struct slow_fp fps[MAX_SLOW_FP];
	unsigned nfps;
	struct branch_exit bexits[MAX_BLOCK_INSNS];
	unsigned nbexits;
	struct cache_state rc;
	int8_t slot_of[NGUEST];
	uint32_t used[NCACHE], tick;
	unsigned pins;                  /* slots the current instruction uses */
};

#define OFF(field) ((unsigned)offsetof(struct ppc_cpu, field))
#define GPR(r) (OFF(gpr) + 4u * (r))
#define FPR(r) (OFF(fpr) + 8u * (r))

static void emit(struct tx *t, uint32_t w)
{
	if (t->p < t->end)
		*t->p++ = w;
	else
		t->overflow = true;
}

static void ld32(struct tx *t, unsigned reg, unsigned off) { emit(t, a64_ldst_imm(2, true, reg, R_CPU, off)); }
static void st32(struct tx *t, unsigned reg, unsigned off) { emit(t, a64_ldst_imm(2, false, reg, R_CPU, off)); }

static void mov_reg(struct tx *t, unsigned d, unsigned n)
{
	if (d != n)
		emit(t, a64_logic(0, A64_ORR, 0, d, A64_ZR, n, A64_LSL, 0));
}

static void mov32(struct tx *t, unsigned reg, uint32_t v)
{
	uint32_t nrs;
	if (!(v & 0xFFFF0000u)) {
		emit(t, a64_movw(0, A64_MOVZ, reg, v, 0));
	} else if (!(v & 0xFFFFu)) {
		emit(t, a64_movw(0, A64_MOVZ, reg, v >> 16, 1));
	} else if (!(~v & 0xFFFF0000u)) {
		emit(t, a64_movw(0, A64_MOVN, reg, ~v & 0xFFFF, 0));
	} else if (!(~v & 0xFFFFu)) {
		emit(t, a64_movw(0, A64_MOVN, reg, ~v >> 16, 1));
	} else if (a64_logic_imm_encode(0, v, &nrs)) {
		emit(t, a64_logic_imm(0, A64_ORR, reg, A64_ZR, nrs));
	} else {
		emit(t, a64_movw(0, A64_MOVZ, reg, v & 0xFFFF, 0));
		emit(t, a64_movw(0, A64_MOVK, reg, v >> 16, 1));
	}
}

/* d = n + imm (32-bit, wrapping); may use `tmp` */
static void add_imm(struct tx *t, unsigned d, unsigned n, int32_t imm, unsigned tmp)
{
	if (imm == 0)
		mov_reg(t, d, n);
	else if (imm > 0 && imm < 4096)
		emit(t, a64_addsub_imm(0, 0, 0, d, n, (unsigned)imm));
	else if (imm < 0 && imm > -4096)
		emit(t, a64_addsub_imm(0, 1, 0, d, n, (unsigned)-imm));
	else {
		mov32(t, tmp, (uint32_t)imm);
		emit(t, a64_addsub(0, 0, 0, d, n, tmp, A64_LSL, 0));
	}
}

/* d = n OP imm for AND/ORR/EOR/ANDS; may use `tmp` */
static void logic_imm(struct tx *t, enum a64_logic op, unsigned d, unsigned n, uint32_t imm, unsigned tmp)
{
	uint32_t nrs;
	if (a64_logic_imm_encode(0, imm, &nrs)) {
		emit(t, a64_logic_imm(0, op, d, n, nrs));
	} else {
		mov32(t, tmp, imm);
		emit(t, a64_logic(0, op, 0, d, n, tmp, A64_LSL, 0));
	}
}

/* BFI d, n, #lsb, #width */
static void bfi(struct tx *t, unsigned d, unsigned n, unsigned lsb, unsigned width)
{
	emit(t, a64_bitfield(0, A64_BFM, d, n, (32 - lsb) & 31, width - 1));
}

/* UBFX d, n, #lsb, #width */
static void ubfx(struct tx *t, unsigned d, unsigned n, unsigned lsb, unsigned width)
{
	emit(t, a64_bitfield(0, A64_UBFM, d, n, lsb, lsb + width - 1));
}

static void cset(struct tx *t, unsigned d, enum a64_cond cond)
{
	emit(t, a64_csel(0, 0, 1, d, A64_ZR, A64_ZR, (enum a64_cond)(cond ^ 1)));
}

static void branch_to(struct tx *t, const uint32_t *target)
{
	emit(t, a64_b(false, (int32_t)((target - t->p) * 4)));
}

static void patch(struct tx *t, uint32_t *at, unsigned type, const uint32_t *target)
{
	int32_t words = (int32_t)(target - at), range = type == FIX_IMM19 ? 1 << 18 : 1 << 13;
	if (words < -range || words >= range)
		t->too_far = true;
	if (type == FIX_IMM19)
		*at |= ((uint32_t)words & 0x7FFFF) << 5;
	else
		*at |= ((uint32_t)words & 0x3FFF) << 5;
}

/* ---- guest register cache ------------------------------------------------ */

static unsigned guest_off(unsigned g)
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
	if (h)
		return h;
	int s = t->slot_of[g];
	if (s < 0) {
		s = (int)rc_alloc(t, g);
		ld32(t, cache_reg[s], guest_off(g));
	}
	return rc_use(t, (unsigned)s);
}

/* rA, or zero for r0 */
static unsigned rd0(struct tx *t, unsigned r)
{
	return r ? rd(t, r) : A64_ZR;
}

/* A host register for guest register g's new value (all 32 bits must be written) */
static unsigned wr(struct tx *t, unsigned g)
{
	unsigned h = fixed_host(g);
	if (h)
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
	if (!fixed_host(g))
		t->rc.dirty |= (uint8_t)(1u << t->slot_of[g]);
	return h;
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

static void add_fixup(struct tx *t, unsigned type)
{
	if (t->nfix < MAX_FIXUPS)
		t->fix[t->nfix++] = (struct fixup){ t->p, type, t->k, false, t->rc };
	else
		t->overflow = true;
}

static void exit_if(struct tx *t, enum a64_cond cond)
{
	add_fixup(t, FIX_IMM19);
	emit(t, a64_bcond(cond, 0));
}

static void exit_if_zero(struct tx *t, bool nonzero, unsigned reg)
{
	add_fixup(t, FIX_IMM19);
	emit(t, a64_cbz(0, nonzero, reg, 0));
}

static void exit_if_bit(struct tx *t, bool set, unsigned reg, unsigned bit)
{
	add_fixup(t, FIX_IMM14);
	emit(t, a64_tbz(set, reg, bit, 0));
}

/* If translated code changed (flag set by code_changed()), leave after the current instruction */
static void exit_if_code_changed(struct tx *t)
{
	emit(t, a64_ldst_imm(2, true, 9, R_STATE, (unsigned)offsetof(struct jit_state, code_changed)));
	add_fixup(t, FIX_IMM19);
	if (t->nfix)
		t->fix[t->nfix - 1].after = true;
	emit(t, a64_cbz(0, true, 9, 0));
}

/* ---- condition register and XER ----------------------------------------- */

/* CR field `f` from the host flags of a compare, with SO from XER */
static void cr_from_flags(struct tx *t, unsigned f, bool is_signed)
{
	emit(t, a64_movw(0, A64_MOVZ, 5, CR_EQ, 0));
	emit(t, a64_movw(0, A64_MOVZ, 6, CR_LT, 0));
	emit(t, a64_csel(0, 0, 0, 5, 6, 5, is_signed ? A64_LT : A64_LO));
	emit(t, a64_movw(0, A64_MOVZ, 6, CR_GT, 0));
	emit(t, a64_csel(0, 0, 0, 5, 6, 5, is_signed ? A64_GT : A64_HI));
	unsigned xer = rd(t, G_XER);
	emit(t, a64_logic(0, A64_ORR, 0, 5, 5, xer, A64_LSR, 31));
	bfi(t, rw(t, G_CR), 5, 28 - 4 * f, 4);
}

/* Rc = 1: CR0 from the result in `reg` */
static void record(struct tx *t, unsigned reg)
{
	emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, reg, 0));
	cr_from_flags(t, 0, true);
}

/* XER[CA] from the host carry, XER[OV] (and SO) from the host overflow flag */
static void xer_from_flags(struct tx *t, bool ca, bool ov)
{
	if (!ca && !ov)
		return;
	unsigned xer = rw(t, G_XER);
	if (ca) {
		cset(t, 5, A64_HS);
		bfi(t, xer, 5, 29, 1);
	}
	if (ov) {
		cset(t, 6, A64_VS);
		bfi(t, xer, 6, 30, 1);
		emit(t, a64_logic(0, A64_ORR, 0, xer, xer, 6, A64_LSL, 31));
	}
}

/* XER[OV] (and SO) from the 0/1 in `reg` */
static void xer_ov_from(struct tx *t, unsigned reg)
{
	unsigned xer = rw(t, G_XER);
	bfi(t, xer, reg, 30, 1);
	emit(t, a64_logic(0, A64_ORR, 0, xer, xer, reg, A64_LSL, 31));
}

/* Host carry = XER[CA] */
static void carry_from_xer(struct tx *t)
{
	ubfx(t, 5, rd(t, G_XER), 29, 1);
	emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, 5, 1));
}

/* ---- instructions --------------------------------------------------------- */

enum { EMITTED, BRANCHED, STOPPED, EXITED };

static uint32_t ppc_mask(unsigned mb, unsigned me)
{
	uint32_t a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
	return mb <= me ? (a & b) : (a | b);
}

/* Calls the C function in x16 */
/* Calls the function whose pointer is at `fn` in struct jit_state (arguments in x0-x3 are set up) */
static void call_c(struct tx *t, unsigned fn)
{
	emit(t, a64_ldst_imm(3, true, R_CBASE, R_STATE, fn));
	emit(t, a64_blr(R_CBASE));
	emit(t, a64_ldst_imm(3, true, R_CBASE, R_STATE, (unsigned)offsetof(struct jit_state, membase)));
}

/*
 * The interpreter executes it, from inside the block. If it stored over
 * translated code (perhaps this block's own), leave right after it.
 */
static void call_step(struct tx *t, uint32_t insn)
{
	rc_flush(t);
	fixed_store(t, NFIXED);
	emit(t, a64_logic(1, A64_ORR, 0, 0, A64_ZR, R_CPU, A64_LSL, 0));
	mov32(t, 1, insn);
	mov32(t, 2, t->pc);
	call_c(t, (unsigned)offsetof(struct jit_state, step));
	fixed_load(t, NFIXED);          /* it may have changed any register */
	rc_reset(t);
	exit_if_zero(t, false, 0);
	exit_if_code_changed(t);
}

/*
 * d = rotl(s, sh) & mask(mb, me), in one UBFM where the mask allows. For
 * rlwimi (insert), d keeps the bits outside the mask.
 */
static void rotate_mask(struct tx *t, bool insert, unsigned d, unsigned s, unsigned sh, unsigned mb, unsigned me)
{
	uint32_t mask = ppc_mask(mb, me);
	unsigned r = (32 - sh) & 31;            /* as a right rotation */
	enum a64_bfm op = insert ? A64_BFM : A64_UBFM;

	if (mb <= me) {
		unsigned lo = 31 - me, hi = 31 - mb;    /* host bit numbers */
		if (lo == 0 && hi + r <= 31) {
			emit(t, a64_bitfield(0, op, d, s, r, hi + r));
			return;
		}
		if (r > 0 && lo == 32 - r) {
			emit(t, a64_bitfield(0, op, d, s, r, hi - lo));
			return;
		}
	}
	if (mask == 0xFFFFFFFFu) {
		if (r)
			emit(t, a64_extr(0, d, s, s, r));
		else
			mov_reg(t, d, s);
		return;
	}
	unsigned tmp = insert ? 10 : d;
	if (r)
		emit(t, a64_extr(0, tmp, s, s, r));
	else
		mov_reg(t, tmp, s);
	logic_imm(t, A64_AND, tmp, tmp, mask, 11);
	if (insert) {
		logic_imm(t, A64_AND, d, d, ~mask, 11);
		emit(t, a64_logic(0, A64_ORR, 0, d, d, tmp, A64_LSL, 0));
	}
}

/* An access that may fault (fastmem): if it does, the side exit for the instruction, with `cs` */
static void add_site(struct tx *t, const struct cache_state *cs)
{
	if (t->nfix < MAX_FIXUPS)
		t->fix[t->nfix++] = (struct fixup){ t->p, FIX_SITE, t->k, false, *cs };
	else
		t->overflow = true;
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
 * Loads and stores of `1 << size` bytes. EA = (rA|0, or rA for update) +
 * (rB or d), in w1; a store's value goes byte-swapped into w4/x4. An
 * update of rA comes after the access.
 *
 * With fastmem, the access goes straight to generated code's view of guest
 * memory, where the host faults if the guest's page flags say no (or the
 * page holds translated code, for stores). Instructions that faulted that
 * way before, or all without fastmem, test page flags instead and access
 * memory through R_CBASE: loads take a slow path (slow_ios()) unless the
 * page (and the next) is readable; stores take one (slow_stores()) unless
 * MEM_FAST_W says the page is plain writable memory.
 */
static void load_single(struct tx *t, unsigned r);
static void store_single(struct tx *t, unsigned r);

enum { LS_GPR, LS_DOUBLE, LS_SINGLE };          /* load_store()'s register kinds */

/*
 * A TBZ on bit `bit` of w9 (page flags) to an out-of-line path for the
 * mmio hook (see slow_ios()) for instruction k; NULL, and nothing emitted,
 * if there's no room.
 */
static struct slow_io *add_io(struct tx *t, unsigned bit, unsigned k, uint32_t insn, uint32_t pc, unsigned size,
			      bool write, const struct cache_state *before)
{
	if (t->nio >= MAX_SLOW_IOS)
		return NULL;
	struct slow_io *io = &t->io[t->nio++];
	*io = (struct slow_io){ t->p, NULL, k, 1u << size, write, false, insn, pc, *before, *before };
	emit(t, a64_tbz(false, 9, bit, 0));
	return io;
}

static void load_store(struct tx *t, unsigned size, bool load, bool sign, bool update, bool indexed, unsigned fp,
		       unsigned rd_, unsigned ra, unsigned rb, int32_t d)
{
	unsigned base = update ? rd(t, ra) : rd0(t, ra);
	if (indexed)
		emit(t, a64_addsub(0, 0, 0, 1, base, rd(t, rb), A64_LSL, 0));
	else if (base == A64_ZR)
		mov32(t, 1, (uint32_t)d);
	else
		add_imm(t, 1, base, d, 2);
	if (!load) {
		if (fp == LS_DOUBLE) {
			emit(t, a64_ldst_imm(3, true, 4, R_CPU, FPR(rd_)));
			emit(t, a64_dp1(1, A64_REV64, 4, 4));
		} else if (fp == LS_SINGLE) {
			store_single(t, rd_);
			emit(t, a64_dp1(0, A64_REV32, 4, 4));
		} else {
			unsigned v = rd(t, rd_);
			if (size == 0)
				mov_reg(t, 4, v);
			else
				emit(t, a64_dp1(0, size == 1 ? A64_REV16 : A64_REV32, 4, v));
		}
	}

	bool fast = fast_access(t);
	if (!fast) {
		emit(t, a64_bitfield(0, A64_UBFM, 9, 1, MEM_PAGE_SHIFT, 31));
		emit(t, a64_ldst_uxtw(0, true, 9, R_FLAGS, 9));
	}
	struct slow_io *io = NULL;
	if (load) {
		struct cache_state before = t->rc;              /* a fault leaves the registers as they are here */
		if (!fast) {
			unsigned bit = size ? FAST_R_BIT : 0;  /* MEM_FAST_R / MEM_R */
			if (fp != LS_GPR || !io_access(t) || !(io = add_io(t, bit, t->k, t->insn, t->pc, size, false, &before)))
				exit_if_bit(t, false, 9, bit);
		}
		unsigned v = fp ? 2 : wr(t, rd_);
		if (fast)
			add_site(t, &before);
		emit(t, a64_ldst_uxtw(size, true, v, fast ? R_MEM : R_CBASE, 1));
		if (fp == LS_DOUBLE) {
			emit(t, a64_dp1(1, A64_REV64, 2, 2));
			emit(t, a64_ldst_imm(3, false, 2, R_CPU, FPR(rd_)));
		} else if (fp == LS_SINGLE) {
			emit(t, a64_dp1(0, A64_REV32, 2, 2));
			load_single(t, rd_);
		} else if (size == 1) {
			emit(t, a64_dp1(0, A64_REV16, v, v));
			if (sign)
				emit(t, a64_bitfield(0, A64_SBFM, v, v, 0, 15));
		} else if (size == 2) {
			emit(t, a64_dp1(0, A64_REV32, v, v));
		}
	} else if (fast) {
		add_site(t, &t->rc);
		emit(t, a64_ldst_uxtw(size, false, 4, R_MEM, 1));
	} else {
		struct slow_store *slow = NULL;
		if (t->nslow < MAX_SLOW_STORES) {
			slow = &t->slow[t->nslow++];
			*slow = (struct slow_store){ t->p, NULL, t->k, size, t->rc, update, ra, fp == LS_GPR && io_access(t),
						     t->insn, t->pc };
			emit(t, a64_tbz(false, 9, FAST_W_BIT, 0));
		} else {
			exit_if_bit(t, false, 9, FAST_W_BIT);
		}
		emit(t, a64_ldst_uxtw(size, false, 4, R_CBASE, 1));
		if (slow)
			slow->rejoin = t->p;
	}
	if (update)
		mov_reg(t, wr(t, ra), 1);
	if (io) {
		io->rejoin = t->p;
		io->after = t->rc;
	}
}

/* The host address of the EA in w1 into x10, if the n bytes there are accessible within one page */
static void block_access(struct tx *t, bool load, unsigned n)
{
	emit(t, a64_bitfield(0, A64_UBFM, 9, 1, MEM_PAGE_SHIFT, 31));
	emit(t, a64_ldst_uxtw(0, true, 9, R_FLAGS, 9));
	exit_if_bit(t, false, 9, load ? 0 : 1);                 /* MEM_R / MEM_W */
	if (!load)
		exit_if_bit(t, true, 9, 3);                     /* MEM_CODE: the interpreter's job */
	if (n > 1) {
		ubfx(t, 10, 1, 0, MEM_PAGE_SHIFT);
		emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, 10, MEM_PAGE_SIZE - n));
		exit_if(t, A64_HI);
	}
	emit(t, a64_add_uxtw(10, R_CBASE, 1));
}

/* lmw, stmw: rS..r31 at (rA|0) + d */
static void load_store_multiple(struct tx *t, bool load, unsigned rs, unsigned ra, int32_t d)
{
	unsigned n = 32 - rs, base = rd0(t, ra);
	if (base == A64_ZR)
		mov32(t, 1, (uint32_t)d);
	else
		add_imm(t, 1, base, d, 2);
	block_access(t, load, 4 * n);
	t->pins = 0;                    /* each register is used once */
	for (unsigned k = 0; k < n; k++) {
		if (load) {
			unsigned v = wr(t, rs + k);
			emit(t, a64_ldst_imm(2, true, v, 10, 4 * k));
			emit(t, a64_dp1(0, A64_REV32, v, v));
		} else {
			emit(t, a64_dp1(0, A64_REV32, 4, rd(t, rs + k)));
			emit(t, a64_ldst_imm(2, false, 4, 10, 4 * k));
		}
		t->pins = 0;
	}
}

/*
 * Stores that MEM_FAST_W (in w9) turned away. Unless the page is writable
 * and the store stays within it, the mmio hook may do it (slow_ios()), or
 * the interpreter executes the instruction.
 * Then, if the page holds no translated code, or the word stored to isn't
 * one that code was translated from (the page's bitmap says), the store
 * just happens. Otherwise a host function stores w4/x4 at the EA in w1,
 * invalidating the blocks translated from those bytes; the cached registers
 * in caller-saved host registers are reloaded afterwards, and if any code
 * changed, the instruction completes (rA) and the block is left after it.
 */
static void slow_stores(struct tx *t)
{
	for (unsigned s = 0; s < t->nslow; s++) {
		struct slow_store *ss = &t->slow[s];
		unsigned bytes = 1u << ss->size;
		uint32_t *to_plain[2] = { NULL, NULL }, *to_call = NULL;
		patch(t, ss->at, FIX_IMM14, t->p);
		t->k = ss->k;
		t->rc = ss->cs;
		struct slow_io *io = NULL;
		if (ss->io && (io = add_io(t, 1, ss->k, ss->insn, ss->pc, ss->size, true, &ss->cs))) {     /* MEM_W */
			io->rejoin = ss->rejoin;
			io->reload_ea = ss->update;
		} else {
			exit_if_bit(t, false, 9, 1);
		}
		if (ss->size) {
			ubfx(t, 10, 1, 0, MEM_PAGE_SHIFT);
			emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, 10, MEM_PAGE_SIZE - bytes));
			exit_if(t, A64_HI);
		}
		to_plain[0] = t->p;
		emit(t, a64_tbz(false, 9, 3, 0));                               /* no MEM_CODE */
		if (bytes <= 4) {
			if (bytes > 1) {                /* within one word? */
				ubfx(t, 11, 1, 0, 2);
				emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, 11, 4 - bytes));
				to_call = t->p;
				emit(t, a64_bcond(A64_HI, 0));
			}
			emit(t, a64_bitfield(0, A64_UBFM, 9, 1, MEM_PAGE_SHIFT, 31));
			emit(t, a64_ldst_imm(3, true, 10, R_STATE, (unsigned)offsetof(struct jit_state, pages)));
			emit(t, a64_ldst_uxtw_scaled(3, true, 10, 10, 9));
			emit(t, a64_addsub_imm(1, 0, 0, 10, 10, (unsigned)offsetof(struct jit_page, words)));
			ubfx(t, 11, 1, 2, MEM_PAGE_SHIFT - 2);
			emit(t, a64_bitfield(0, A64_UBFM, 9, 11, 6, 31));
			emit(t, a64_ldst_uxtw_scaled(3, true, 9, 10, 9));
			emit(t, a64_dp2(1, A64_LSRV, 9, 9, 11));
			to_plain[1] = t->p;
			emit(t, a64_tbz(false, 9, 0, 0));
		}
		if (to_call)
			patch(t, to_call, FIX_IMM19, t->p);

		rc_store(t, &ss->cs);
		fixed_store(t, CALLER_SAVED_FIXED);
		if (ss->update)                         /* w1 must survive the call */
			emit(t, a64_ldst_imm(2, false, 1, R_STATE, (unsigned)offsetof(struct jit_state, scratch)));
		emit(t, a64_logic(1, A64_ORR, 0, 0, A64_ZR, R_STATE, A64_LSL, 0));
		emit(t, a64_logic(1, A64_ORR, 0, 2, A64_ZR, 4, A64_LSL, 0));
		emit(t, a64_movw(0, A64_MOVZ, 3, ss->size, 0));
		call_c(t, (unsigned)offsetof(struct jit_state, code_store));
		fixed_load(t, CALLER_SAVED_FIXED);
		for (unsigned c = 0; c < CALLER_SAVED_SLOTS; c++)
			if (ss->cs.guest[c] >= 0)
				ld32(t, cache_reg[c], guest_off((unsigned)ss->cs.guest[c]));
		rc_reset(t);                            /* (everything was written back) */
		if (ss->update) {
			emit(t, a64_ldst_imm(2, true, 1, R_STATE, (unsigned)offsetof(struct jit_state, scratch)));
			emit(t, a64_ldst_imm(2, true, 9, R_STATE, (unsigned)offsetof(struct jit_state, code_changed)));
			emit(t, a64_cbz(0, false, 9, (int32_t)((ss->rejoin - t->p) * 4)));
			unsigned h = fixed_host(ss->ra);        /* code changed: rA = EA here, then leave */
			if (h)
				mov_reg(t, h, 1);
			else
				st32(t, 1, GPR(ss->ra));
			add_fixup(t, FIX_IMM19);
			if (t->nfix)
				t->fix[t->nfix - 1].after = true;
			emit(t, a64_bcond(A64_AL, 0));
		} else {
			exit_if_code_changed(t);
			branch_to(t, ss->rejoin);
		}

		for (unsigned k = 0; k < 2; k++)
			if (to_plain[k])
				patch(t, to_plain[k], FIX_IMM14, t->p);
		emit(t, a64_ldst_uxtw(ss->size, false, 4, R_CBASE, 1));
		branch_to(t, ss->rejoin);
	}
}

/*
 * Loads and stores (to and from GPRs) that page flags turned away: the
 * page isn't readable, or writable. A host function (mmio() below) offers
 * the access to the mmio hook, as the interpreter would, with everything
 * written back; then all registers are reloaded, as the hook may have
 * changed any. If it didn't take it, the interpreter executes the
 * instruction; if it did, the block goes on after it, or leaves there
 * (as mmio() says: for the kernel, or because code changed).
 */
static void slow_ios(struct tx *t)
{
	for (unsigned s = 0; s < t->nio && !t->overflow; s++) {
		struct slow_io *io = &t->io[s];
		patch(t, io->at, FIX_IMM14, t->p);
		t->k = io->k;
		rc_store(t, &io->before);
		mov_reg(t, 3, 1);
		mov32(t, 1, io->insn);
		mov32(t, 2, io->pc);
		mov32(t, 4, io->bytes | (unsigned)io->write << 8);
		emit(t, a64_addsub_imm(1, 1, 0, 5, R_BUDGET, io->k));         /* instructions left, this one included */
		emit(t, a64_b(true, (int32_t)((t->j->mmio_call - t->p) * 4)));
		emit(t, a64_ldst_imm(3, true, R_CBASE, R_STATE, (unsigned)offsetof(struct jit_state, membase)));
		rc_reset(t);                            /* (all written back) */
		exit_if_zero(t, false, 0);
		emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, 0, PPC_MMIO_DONE));
		uint32_t *stop = t->p;
		emit(t, a64_bcond(A64_NE, 0));
		for (unsigned c = 0; c < NCACHE; c++)
			if (io->after.guest[c] >= 0)
				ld32(t, cache_reg[c], guest_off((unsigned)io->after.guest[c]));
		if (io->reload_ea)
			ld32(t, 1, OFF(fault_addr));
		branch_to(t, io->rejoin);
		if (!t->overflow)
			patch(t, stop, FIX_IMM19, t->p);
		emit(t, a64_addsub_imm(1, 1, 0, R_BUDGET, R_BUDGET, io->k + 1));
		mov32(t, 0, io->pc + 4);
		st32(t, 0, OFF(pc));
		emit(t, a64_ldst_imm(2, true, 1, R_STATE, (unsigned)offsetof(struct jit_state, mmio_exit)));
		branch_to(t, t->j->exit);
	}
}

/* ---- floating point --------------------------------------------------------- */

/*
 * Floating-point instructions run natively where the interpreter's fast path
 * (ppc_fp.c) would take them: rounding to nearest, no exceptions enabled, and
 * operands and results well inside the normal range (or zero), so that the
 * only exception an operation can raise is inexact, which an error-free
 * transformation detects. Results, FPSCR included, are exactly the
 * interpreter's. Anything else goes out of line, where the interpreter runs
 * the instruction (struct slow_fp): every check comes before the instruction
 * changes anything. Values are loaded from and stored to struct ppc_cpu by
 * each instruction; host registers d0-d7 and d16-d31 are scratch, and the
 * host always rounds to nearest here (see ppc_fp.c).
 */

#define FPSCR_XX_BIT 25
#define FPSCR_FR_FI (3u << 17)
#define FPSCR_FI_BIT 17

/* A branch within the instruction, to point at the code to come with land() */
static uint32_t *fwd(struct tx *t, uint32_t branch)
{
	uint32_t *at = t->p;
	emit(t, branch);
	return at;
}

static void land(struct tx *t, uint32_t *at)
{
	if (at && !t->overflow)
		patch(t, at, FIX_IMM19, t->p);
}

static void logic_imm64(struct tx *t, enum a64_logic op, unsigned d, unsigned n, uint64_t imm)
{
	uint32_t nrs;
	if (a64_logic_imm_encode(1, imm, &nrs))
		emit(t, a64_logic_imm(1, op, d, n, nrs));
	else
		t->overflow = true;     /* (only encodable values are used) */
}

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

/* To the slow path, by a branch with an imm19 offset (B.cond, CBZ, CBNZ) */
static void fp_slow(struct tx *t, uint32_t branch)
{
	struct slow_fp *f = &t->fps[t->nfps - 1];
	if (f->nat < MAX_FP_EXITS)
		f->at[f->nat++] = t->p;
	else
		t->overflow = true;
	emit(t, branch);
}

static void fp_end(struct tx *t)
{
	t->fps[t->nfps - 1].rejoin = t->p;
}

/*
 * The slow paths: the interpreter executes the instruction, which changes
 * only FPRs, the FPSCR and CR (memory and GPRs are the business of loads
 * and stores, which take other paths); the cached registers in caller-saved
 * host registers are reloaded afterwards.
 */
static void slow_fps(struct tx *t)
{
	for (unsigned k = 0; k < t->nfps && !t->overflow; k++) {
		struct slow_fp *f = &t->fps[k];
		if (!f->nat)
			continue;
		for (unsigned s = 0; s < f->nat; s++)
			patch(t, f->at[s], FIX_IMM19, t->p);
		rc_store(t, &f->cs);
		fixed_store(t, CALLER_SAVED_FIXED);
		emit(t, a64_logic(1, A64_ORR, 0, 0, A64_ZR, R_CPU, A64_LSL, 0));
		mov32(t, 1, f->insn);
		mov32(t, 2, f->pc);
		call_c(t, (unsigned)offsetof(struct jit_state, step));
		fixed_load(t, CALLER_SAVED_FIXED);
		for (unsigned c = 0; c < CALLER_SAVED_SLOTS; c++)
			if (f->cs.guest[c] >= 0)
				ld32(t, cache_reg[c], guest_off((unsigned)f->cs.guest[c]));
		branch_to(t, f->rejoin);
	}
}

/* Out of line unless the FPSCR (loaded into w5) enables no exceptions and, if `nearest`, rounds to nearest */
static void fp_mode(struct tx *t, bool nearest)
{
	ld32(t, 5, OFF(fpscr));
	if (nearest) {
		mov32(t, 9, 0xFB);      /* (NI, 0x04, changes nothing here) */
		emit(t, a64_logic(0, A64_ANDS, 0, A64_ZR, 5, 9, A64_LSL, 0));
	} else {
		logic_imm(t, A64_ANDS, A64_ZR, 5, 0xF8, 9);
	}
	fp_slow(t, a64_bcond(A64_NE, 0));
}

/* x`x` and d`d` = FPR r */
static void fp_load(struct tx *t, unsigned x, unsigned d, unsigned r)
{
	emit(t, a64_ldst_imm(3, true, x, R_CPU, FPR(r)));
	emit(t, a64_fmov_gp(true, true, d, x));
}

/*
 * Out of line unless the double in x`x` has a biased exponent from lo to
 * hi, or (if zero_ok) is zero. For zero_ok, returns the branch taken in
 * range, to land() after code that checks the zero case further.
 */
static uint32_t *fp_range(struct tx *t, unsigned x, unsigned lo, unsigned hi, bool zero_ok)
{
	emit(t, a64_bitfield(1, A64_UBFM, 9, x, 52, 62));
	emit(t, a64_addsub_imm(0, 1, 0, 9, 9, lo));
	emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, 9, hi - lo));
	if (!zero_ok) {
		fp_slow(t, a64_bcond(A64_HI, 0));
		return NULL;
	}
	uint32_t *ok = fwd(t, a64_bcond(A64_LS, 0));
	emit(t, a64_bitfield(1, A64_UBFM, 9, x, 63, 62));      /* x << 1: zero? */
	fp_slow(t, a64_cbz(1, true, 9, 0));
	return ok;
}

/* tame(): zero, or a magnitude from 2^-900 to 2^1000 */
#define TAME_LO 123u
#define TAME_HI 2022u

/* Out of line unless d`a` or d`b` is zero (a zero product is exact then; otherwise it underflowed) */
static void fp_either_zero(struct tx *t, unsigned a, unsigned b)
{
	emit(t, a64_fcmp(1, a, 0, true));
	uint32_t *zero = fwd(t, a64_bcond(A64_EQ, 0));
	emit(t, a64_fcmp(1, b, 0, true));
	fp_slow(t, a64_bcond(A64_NE, 0));
	land(t, zero);
}

/* w4 = (d`d` != 0), or w4 |= that */
static void fp_nonzero(struct tx *t, unsigned d, bool or)
{
	emit(t, a64_fcmp(1, d, 0, true));
	if (or)
		emit(t, a64_csel(0, 0, 1, 4, 4, A64_ZR, A64_EQ));
	else
		cset(t, 4, A64_NE);
}

/* Rounds d3 to single precision as opcode 59 does (out of line unless tame_single()); w4 |= it changed if `track` */
static void fp_round_single(struct tx *t, bool track)
{
	emit(t, a64_fp1(1, A64_FCVT_S, 6, 3));
	emit(t, a64_fp1(0, A64_FCVT_D, 6, 6));
	emit(t, a64_fmov_gp(true, false, 10, 6));
	uint32_t *ok = fp_range(t, 10, 903, 1148, true);
	emit(t, a64_fmov_gp(true, false, 11, 3));               /* rounded to zero: was it zero? */
	emit(t, a64_bitfield(1, A64_UBFM, 11, 11, 63, 62));
	fp_slow(t, a64_cbz(1, true, 11, 0));
	land(t, ok);
	if (track) {
		emit(t, a64_fcmp(1, 6, 3, false));
		emit(t, a64_csel(0, 0, 1, 4, 4, A64_ZR, A64_EQ));
	}
	emit(t, a64_fp1(1, A64_FMOV, 3, 6));
}

/*
 * The FPSCR (w5) after an operation with result x3, inexact if w4: FR and FI
 * (and FX, XX), FPRF if `fprf`; it and the result stored, CR1 set for Rc.
 */
static void fp_commit(struct tx *t, unsigned frt, bool fprf, bool rc)
{
	if (fprf) {             /* zero: 0x02, or 0x12 if negative; normal: 0x04, or 0x08 */
		emit(t, a64_bitfield(1, A64_UBFM, 9, 3, 63, 63));
		emit(t, a64_bitfield(1, A64_UBFM, 10, 3, 63, 62));
		emit(t, a64_addsub_imm(1, 1, 1, A64_ZR, 10, 0));
		emit(t, a64_bitfield(0, A64_UBFM, 11, 9, 28, 27));
		logic_imm(t, A64_ORR, 11, 11, 2, 10);
		emit(t, a64_movw(0, A64_MOVZ, 10, 4, 0));
		emit(t, a64_dp2(0, A64_LSLV, 10, 10, 9));
		emit(t, a64_csel(0, 0, 0, 10, 11, 10, A64_EQ));
		bfi(t, 5, 10, 12, 5);
	}
	logic_imm(t, A64_AND, 5, 5, ~FPSCR_FR_FI, 9);
	uint32_t *exact = fwd(t, a64_cbz(0, false, 4, 0));
	logic_imm(t, A64_AND, 9, 5, 1u << FPSCR_XX_BIT, 10);
	uint32_t *had = fwd(t, a64_cbz(0, true, 9, 0));
	logic_imm(t, A64_ORR, 5, 5, 0x80000000u, 10);         /* FX: XX is new */
	land(t, had);
	logic_imm(t, A64_ORR, 5, 5, 1u << FPSCR_XX_BIT, 10);
	logic_imm(t, A64_ORR, 5, 5, 1u << FPSCR_FI_BIT, 10);
	land(t, exact);
	st32(t, 5, OFF(fpscr));
	emit(t, a64_ldst_imm(3, false, 3, R_CPU, FPR(frt)));
	if (rc) {
		ubfx(t, 9, 5, 28, 4);
		bfi(t, rw(t, G_CR), 9, 24, 4);
	}
}

/* Rc = 1 for instructions that leave the FPSCR alone: CR1 from it */
static void fp_cr1(struct tx *t, bool rc)
{
	if (rc) {
		ld32(t, 9, OFF(fpscr));
		ubfx(t, 9, 9, 28, 4);
		bfi(t, rw(t, G_CR), 9, 24, 4);
	}
}

/* FPR r = the single in w2, as a double (lfs: exactly; infinities and NaNs keep their bits) */
static void load_single(struct tx *t, unsigned r)
{
	ubfx(t, 9, 2, 23, 8);
	emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, 9, 255));
	uint32_t *special = fwd(t, a64_bcond(A64_EQ, 0));
	emit(t, a64_fmov_gp(false, true, 0, 2));
	emit(t, a64_fp1(0, A64_FCVT_D, 0, 0));
	emit(t, a64_fp_ldst(true, false, 0, R_CPU, FPR(r)));
	uint32_t *done = fwd(t, a64_bcond(A64_AL, 0));
	land(t, special);
	emit(t, a64_bitfield(1, A64_UBFM, 10, 2, 35, 22));     /* fraction << 29 */
	logic_imm64(t, A64_ORR, 10, 10, 0x7FF0000000000000ull);
	emit(t, a64_bitfield(0, A64_UBFM, 9, 2, 31, 31));
	emit(t, a64_logic(1, A64_ORR, 0, 10, 10, 9, A64_LSL, 63));
	emit(t, a64_ldst_imm(3, false, 10, R_CPU, FPR(r)));
	land(t, done);
}

/* w4 = FPR r in single precision, as stfs stores it (see ppc_fp_double_to_single()) */
static void store_single(struct tx *t, unsigned r)
{
	emit(t, a64_ldst_imm(3, true, 5, R_CPU, FPR(r)));
	emit(t, a64_bitfield(1, A64_UBFM, 9, 5, 52, 62));      /* exponent */
	emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, 9, 896));
	uint32_t *small = fwd(t, a64_bcond(A64_LS, 0));
	emit(t, a64_bitfield(1, A64_UBFM, 10, 5, 62, 63));     /* bits 0-1 */
	emit(t, a64_bitfield(1, A64_UBFM, 11, 5, 29, 58));     /* bits 5-34 */
	emit(t, a64_logic(0, A64_ORR, 0, 4, 11, 10, A64_LSL, 30));
	uint32_t *done = fwd(t, a64_bcond(A64_AL, 0));
	land(t, small);
	emit(t, a64_bitfield(1, A64_UBFM, 10, 5, 63, 63));
	emit(t, a64_bitfield(0, A64_UBFM, 4, 10, 1, 0));        /* the sign */
	emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, 9, 874));
	uint32_t *zero = fwd(t, a64_bcond(A64_LO, 0));
	emit(t, a64_bitfield(1, A64_UBFM, 11, 5, 0, 51));      /* denormal: (fraction | 1 << 52) >> (897 - exponent + 29) */
	logic_imm64(t, A64_ORR, 11, 11, 1ull << 52);
	emit(t, a64_movw(0, A64_MOVZ, 6, 897, 0));
	emit(t, a64_addsub(0, 1, 0, 6, 6, 9, A64_LSL, 0));
	emit(t, a64_dp2(1, A64_LSRV, 11, 11, 6));
	emit(t, a64_bitfield(1, A64_UBFM, 11, 11, 29, 63));
	emit(t, a64_logic(0, A64_ORR, 0, 4, 4, 11, A64_LSL, 0));
	land(t, zero);
	land(t, done);
}

/* The error-free transformation of fma_inexact() (ppc_fp.c): w4 = d`r` = d0 * d2 + d`b` inexactly, u1 = d0 * d2 in d16 */
static void fma_inexact(struct tx *t, unsigned b, unsigned r)
{
	emit(t, a64_fp3(1, 1, 1, 17, 0, 2, 16));       /* u2 = a * c - u1 */
	emit(t, a64_fp2(1, A64_FADD, 18, b, 17));      /* a1 = b + u2 */
	emit(t, a64_fp2(1, A64_FSUB, 19, 18, b));      /* a1 - b */
	emit(t, a64_fp2(1, A64_FSUB, 20, 18, 19));
	emit(t, a64_fp2(1, A64_FSUB, 20, b, 20));      /* b - (a1 - (a1 - b)) */
	emit(t, a64_fp2(1, A64_FSUB, 21, 17, 19));     /* u2 - (a1 - b) */
	emit(t, a64_fp2(1, A64_FADD, 20, 20, 21));     /* z */
	emit(t, a64_fp2(1, A64_FADD, 22, 16, 18));     /* b1 = u1 + a1 */
	emit(t, a64_fp2(1, A64_FSUB, 23, 22, 16));     /* bv = b1 - u1 */
	emit(t, a64_fp2(1, A64_FSUB, 24, 22, 23));
	emit(t, a64_fp2(1, A64_FSUB, 24, 16, 24));     /* u1 - (b1 - bv) */
	emit(t, a64_fp2(1, A64_FSUB, 25, 18, 23));     /* a1 - bv */
	emit(t, a64_fp2(1, A64_FADD, 24, 24, 25));     /* b2 */
	emit(t, a64_fp2(1, A64_FSUB, 26, 22, r));      /* b1 - r */
	emit(t, a64_fp2(1, A64_FADD, 26, 26, 24));     /* g */
	emit(t, a64_fp2(1, A64_FADD, 27, 26, 20));     /* r2 = g + z */
	emit(t, a64_fp2(1, A64_FSUB, 28, 27, 26));
	emit(t, a64_fp2(1, A64_FSUB, 28, 20, 28));     /* r3 = z - (r2 - g) */
	fp_nonzero(t, 27, false);
	fp_nonzero(t, 28, true);
}

/* w4 = s = a + b inexactly (Knuth's TwoSum, as sum_inexact() in ppc_fp.c), with d4 and d5 */
static void sum_inexact(struct tx *t, unsigned a, unsigned b, unsigned s)
{
	emit(t, a64_fp2(1, A64_FSUB, 4, s, a));        /* bv = s - a */
	emit(t, a64_fp2(1, A64_FSUB, 5, s, 4));        /* av = s - bv */
	emit(t, a64_fp2(1, A64_FSUB, 5, a, 5));
	emit(t, a64_fp2(1, A64_FSUB, 4, b, 4));
	emit(t, a64_fp2(1, A64_FADD, 4, 5, 4));        /* (a - av) + (b - bv) */
	fp_nonzero(t, 4, false);
}

/*
 * A-form arithmetic (opcode 59 single, 63 double precision). As in the
 * interpreter's fast path, only division and square roots check their
 * operands: for the others a tame result is enough (see fp_fast()).
 *
 * The FPSCR is kept up to date lazily. XX and FX change only while XX is
 * clear, and are dealt with at once (with the inexact check, which is then
 * needed); but FR, FI and FPRF, which the next such operation replaces,
 * are left pending: the instruction and its operands go in struct
 * jit_state, for fp_settle() to work them out when something else might
 * look (C, the interpreter, a compare).
 */
static void fp_arith(struct tx *t, unsigned op, unsigned xo5, unsigned frt, unsigned fra, unsigned frb, unsigned frc,
		     bool rc)
{
	bool uses_a = xo5 != 22 && xo5 != 24 && xo5 != 26, uses_b = xo5 != 25, uses_c = xo5 == 25 || xo5 >= 28;
	bool checked = xo5 == 18 || xo5 == 22 || xo5 == 24 || xo5 == 26;
	unsigned addend = xo5 & 1 ? 1 : 5;             /* (fused: b, or -b in d5) */
	fp_mode(t, true);
	const unsigned regs[3] = { fra, frb, frc };
	const bool uses[3] = { uses_a, uses_b, uses_c };
	for (unsigned k = 0; k < 3; k++) {
		if (!uses[k])
			continue;
		if (checked) {
			fp_load(t, k, k, regs[k]);
			land(t, fp_range(t, k, TAME_LO, TAME_HI, true));
		} else {
			emit(t, a64_fp_ldst(true, true, k, R_CPU, FPR(regs[k])));
		}
	}
	switch (xo5) {
	case 18:                                        /* fdiv */
		emit(t, a64_fcmp(1, 1, 0, true));
		fp_slow(t, a64_bcond(A64_EQ, 0));
		emit(t, a64_fp2(1, A64_FDIV, 3, 0, 1));
		emit(t, a64_fmov_gp(true, false, 3, 3));
		{
			uint32_t *ok = fp_range(t, 3, TAME_LO, TAME_HI, true);
			emit(t, a64_bitfield(1, A64_UBFM, 11, 0, 63, 62));     /* underflow unless a = 0 */
			fp_slow(t, a64_cbz(1, true, 11, 0));
			land(t, ok);
		}
		break;
	case 20: case 21:                               /* fsub, fadd */
		emit(t, a64_fp2(1, xo5 == 20 ? A64_FSUB : A64_FADD, 3, 0, 1));
		emit(t, a64_fmov_gp(true, false, 3, 3));
		land(t, fp_range(t, 3, TAME_LO, TAME_HI, true));
		break;
	case 22:                                        /* fsqrt */
		emit(t, a64_fcmp(1, 1, 0, true));
		fp_slow(t, a64_bcond(A64_LT, 0));
		emit(t, a64_fp1(1, A64_FSQRT, 3, 1));
		break;
	case 24:                                        /* fres */
		emit(t, a64_fcmp(1, 1, 0, true));
		fp_slow(t, a64_bcond(A64_EQ, 0));
		emit(t, a64_fmov_one(7));
		emit(t, a64_fp2(1, A64_FDIV, 3, 7, 1));
		emit(t, a64_fmov_gp(true, false, 3, 3));
		fp_range(t, 3, TAME_LO, TAME_HI, false);
		break;
	case 25:                                        /* fmul */
		emit(t, a64_fp2(1, A64_FMUL, 3, 0, 2));
		emit(t, a64_fmov_gp(true, false, 3, 3));
		{
			uint32_t *ok = fp_range(t, 3, TAME_LO, TAME_HI, true);
			fp_either_zero(t, 0, 2);
			land(t, ok);
		}
		break;
	case 26:                                        /* frsqrte */
		emit(t, a64_fcmp(1, 1, 0, true));
		fp_slow(t, a64_bcond(A64_LE, 0));
		emit(t, a64_fp1(1, A64_FSQRT, 6, 1));
		emit(t, a64_fmov_one(7));
		emit(t, a64_fp2(1, A64_FDIV, 3, 7, 6));
		break;
	default:                                        /* fmsub, fmadd, fnmsub, fnmadd */
		if (addend != 1)
			emit(t, a64_fp1(1, A64_FNEG, addend, 1));
		emit(t, a64_fp3(1, 0, 0, 3, 0, 2, addend));
		emit(t, a64_fmov_gp(true, false, 3, 3));
		land(t, fp_range(t, 3, TAME_LO, TAME_HI, true));
		emit(t, a64_fp2(1, A64_FMUL, 16, 0, 2));
		emit(t, a64_fmov_gp(true, false, 10, 16));
		{
			uint32_t *ok = fp_range(t, 10, TAME_LO, TAME_HI, true);
			fp_either_zero(t, 0, 2);
			land(t, ok);
		}
		break;
	}
	unsigned raw = 3;                               /* the result before negation and rounding */
	if (xo5 >= 30 || op == 59) {
		emit(t, a64_fp1(1, A64_FMOV, 30, 3));
		raw = 30;
	}
	if (xo5 >= 30)
		emit(t, a64_fp1(1, A64_FNEG, 3, 3));
	if (op == 59) {
		emit(t, a64_fp1(1, A64_FMOV, 31, 3));
		fp_round_single(t, false);
	}
	emit(t, a64_fmov_gp(true, false, 3, 3));

	/* XX clear: was it inexact? Then XX, and FX */
	uint32_t *xx_set = t->p;
	emit(t, a64_tbz(true, 5, FPSCR_XX_BIT, 0));
	switch (xo5) {
	case 18:
		emit(t, a64_fp3(1, 0, 1, 4, raw, 1, 0));       /* a - r * b */
		fp_nonzero(t, 4, false);
		break;
	case 20:
		emit(t, a64_fp1(1, A64_FNEG, 6, 1));
		sum_inexact(t, 0, 6, raw);
		break;
	case 21:
		sum_inexact(t, 0, 1, raw);
		break;
	case 22:
		emit(t, a64_fp3(1, 0, 1, 4, raw, raw, 1));     /* b - r * r */
		fp_nonzero(t, 4, false);
		break;
	case 24:
		emit(t, a64_fp3(1, 0, 1, 4, raw, 1, 7));       /* 1 - r * b */
		fp_nonzero(t, 4, false);
		break;
	case 25:
		emit(t, a64_fp3(1, 1, 1, 4, 0, 2, raw));       /* a * c - r */
		fp_nonzero(t, 4, false);
		break;
	case 26:
		emit(t, a64_fp3(1, 0, 1, 4, 6, 6, 1));         /* b - s * s */
		fp_nonzero(t, 4, false);
		emit(t, a64_fp3(1, 0, 1, 4, raw, 6, 7));       /* 1 - r * s */
		fp_nonzero(t, 4, true);
		break;
	default:
		fma_inexact(t, addend, raw);
		break;
	}
	if (op == 59) {                                 /* ... or rounded to single */
		emit(t, a64_fcmp(1, 3, 31, false));
		emit(t, a64_csel(0, 0, 1, 4, 4, A64_ZR, A64_EQ));
	}
	uint32_t *exact = fwd(t, a64_cbz(0, false, 4, 0));
	logic_imm(t, A64_ORR, 5, 5, 0x80000000u | 1u << FPSCR_XX_BIT, 10);
	st32(t, 5, OFF(fpscr));
	land(t, exact);
	if (!t->overflow)
		patch(t, xx_set, FIX_IMM14, t->p);

	/* the result, and the rest pending */
	emit(t, a64_ldst_imm(3, false, 3, R_CPU, FPR(frt)));
	for (unsigned k = 0; k < 3; k++)
		if (uses[k])
			emit(t, a64_fp_ldst(true, false, k, R_STATE, (unsigned)offsetof(struct jit_state, fp_ops) + 8 * k));
	mov32(t, 9, t->fps[t->nfps - 1].insn);
	emit(t, a64_ldst_imm(3, false, 9, R_STATE, (unsigned)offsetof(struct jit_state, fp_insn)));   /* (and fp_fpcc = 0) */
	if (rc) {
		ubfx(t, 9, 5, 28, 4);
		bfi(t, rw(t, G_CR), 9, 24, 4);
	}
}

/* Opcodes 59 and 63; false for call_step() */
static bool fp_insn(struct tx *t, uint32_t i)
{
	unsigned op = i >> 26, frt = (i >> 21) & 31, fra = (i >> 16) & 31, frb = (i >> 11) & 31, frc = (i >> 6) & 31;
	unsigned xo5 = (i >> 1) & 31, xo10 = (i >> 1) & 0x3FF;
	bool rc = i & 1;

	if (op == 63 && !(xo5 & 0x10)) {
		switch (xo10) {
		case 40: case 72: case 136: case 264:   /* fneg, fmr, fnabs, fabs */
			emit(t, a64_ldst_imm(3, true, 3, R_CPU, FPR(frb)));
			if (xo10 != 72)
				logic_imm64(t, xo10 == 40 ? A64_EOR : xo10 == 136 ? A64_ORR : A64_AND, 3, 3,
					    xo10 == 264 ? ~(1ull << 63) : 1ull << 63);
			emit(t, a64_ldst_imm(3, false, 3, R_CPU, FPR(frt)));
			fp_cr1(t, rc);
			return true;
		case 0: case 32: {                      /* fcmpu, fcmpo */
			unsigned crf = (i >> 23) & 7;
			if (!fp_begin(t, i))
				return false;
			emit(t, a64_fp_ldst(true, true, 0, R_CPU, FPR(fra)));
			emit(t, a64_fp_ldst(true, true, 1, R_CPU, FPR(frb)));
			emit(t, a64_fcmp(1, 0, 1, false));
			fp_slow(t, a64_bcond(A64_VS, 0));       /* unordered: VXSNAN, VXVC */
			emit(t, a64_movw(0, A64_MOVZ, 10, 2, 0));
			emit(t, a64_movw(0, A64_MOVZ, 11, 8, 0));
			emit(t, a64_csel(0, 0, 0, 10, 11, 10, A64_MI));
			emit(t, a64_movw(0, A64_MOVZ, 11, 4, 0));
			emit(t, a64_csel(0, 0, 0, 10, 11, 10, A64_GT));
			emit(t, a64_ldst_imm(2, true, 9, R_STATE, (unsigned)offsetof(struct jit_state, fp_insn)));
			uint32_t *settled = fwd(t, a64_cbz(0, false, 9, 0));
			logic_imm(t, A64_ORR, 9, 10, 0x10, 11);         /* FPCC over what's pending */
			emit(t, a64_ldst_imm(2, false, 9, R_STATE, (unsigned)offsetof(struct jit_state, fp_fpcc)));
			uint32_t *done = fwd(t, a64_bcond(A64_AL, 0));
			land(t, settled);
			ld32(t, 5, OFF(fpscr));
			bfi(t, 5, 10, 12, 4);
			st32(t, 5, OFF(fpscr));
			land(t, done);
			bfi(t, rw(t, G_CR), 10, 28 - 4 * crf, 4);
			fp_end(t);
			return true;
		}
		case 12:                                /* frsp */
			if (!fp_begin(t, i))
				return false;
			fp_mode(t, true);
			fp_load(t, 1, 1, frb);
			land(t, fp_range(t, 1, TAME_LO, TAME_HI, true));
			emit(t, a64_fp1(1, A64_FMOV, 3, 1));
			emit(t, a64_movw(0, A64_MOVZ, 4, 0, 0));
			fp_round_single(t, true);
			emit(t, a64_fmov_gp(true, false, 3, 3));
			fp_commit(t, frt, true, rc);
			emit(t, a64_ldst_imm(3, false, A64_ZR, R_STATE, (unsigned)offsetof(struct jit_state, fp_insn)));
										/* (it replaced all that was pending) */
			fp_end(t);
			return true;
		case 14: case 15:                       /* fctiw, fctiwz */
			if (!fp_begin(t, i))
				return false;
			fp_mode(t, xo10 == 14);
			emit(t, a64_ldst_imm(2, true, 9, R_STATE, (unsigned)offsetof(struct jit_state, fp_insn)));
			fp_slow(t, a64_cbz(0, true, 9, 0));     /* (FPRF stays, so it must be settled first) */
			emit(t, a64_fp_ldst(true, true, 1, R_CPU, FPR(frb)));
			emit(t, a64_fp1(1, xo10 == 14 ? A64_FRINTN : A64_FRINTZ, 2, 1));
			emit(t, a64_fcvtzs(10, 2));
			emit(t, a64_scvtf(3, 10));
			emit(t, a64_fcmp(1, 3, 2, false));
			fp_slow(t, a64_bcond(A64_NE, 0));       /* out of range, NaN */
			emit(t, a64_fcmp(1, 2, 1, false));
			cset(t, 4, A64_NE);
			logic_imm64(t, A64_ORR, 3, 10, 0xFFF8000000000000ull);  /* FP_INT_HI */
			fp_commit(t, frt, false, rc);
			fp_end(t);
			return true;
		default:
			return false;
		}
	}

	static const uint32_t valid59 = 1u << 18 | 1u << 20 | 1u << 21 | 1u << 22 | 1u << 24 | 1u << 25 |
					1u << 28 | 1u << 29 | 1u << 30 | 1u << 31;
	static const uint32_t valid63 = 1u << 18 | 1u << 20 | 1u << 21 | 1u << 22 | 1u << 23 | 1u << 25 |
					1u << 26 | 1u << 28 | 1u << 29 | 1u << 30 | 1u << 31;
	if (!((op == 59 ? valid59 : valid63) & (1u << xo5)))
		return false;
	if (xo5 == 23) {                                /* fsel */
		emit(t, a64_fp_ldst(true, true, 0, R_CPU, FPR(fra)));
		emit(t, a64_fp_ldst(true, true, 1, R_CPU, FPR(frb)));
		emit(t, a64_fp_ldst(true, true, 2, R_CPU, FPR(frc)));
		emit(t, a64_fcmp(1, 0, 0, true));
		emit(t, a64_fcsel(1, 3, 2, 1, A64_GE));
		emit(t, a64_fp_ldst(true, false, 3, R_CPU, FPR(frt)));
		fp_cr1(t, rc);
		return true;
	}
	if (!fp_begin(t, i))
		return false;
	fp_arith(t, op, xo5, frt, fra, frb, frc, rc);
	fp_end(t);
	return true;
}

/*
 * Leaves the block for the pc in w0 (after a branch: stop at the hook). The
 * lookup is inline, so that each such exit has its own indirect branch for
 * the host to predict.
 */
static void finish(struct tx *t, unsigned n, bool after_branch)
{
	rc_flush(t);
	if (n)
		emit(t, a64_addsub_imm(1, 1, 0, R_BUDGET, R_BUDGET, n));
	if (!after_branch) {                    /* (rare) the table's hook entry is only for branches */
		branch_to(t, t->j->miss_exit);
		return;
	}
	emit(t, a64_bitfield(0, A64_UBFM, 9, 0, MEM_PAGE_SHIFT, 31));
	emit(t, a64_ldst_uxtw_scaled(3, true, 9, R_TABLE, 9));
	ubfx(t, 10, 0, 2, MEM_PAGE_SHIFT - 2);
	emit(t, a64_ldst_uxtw_scaled(3, true, 9, 9, 10));
	emit(t, a64_br(9));
}

/* Leaves the block for `next`: a branch, patched to the block there once it exists */
static void finish_at(struct tx *t, unsigned n, uint32_t next, bool after_branch)
{
	if (t->nlinks >= MAX_LINKS) {
		mov32(t, 0, next);
		finish(t, n, after_branch);
		return;
	}
	rc_flush(t);
	if (n)
		emit(t, a64_addsub_imm(1, 1, 0, R_BUDGET, R_BUDGET, n));
	t->links[t->nlinks++] = (struct link){ t->p, next, after_branch };
	emit(t, a64_b(false, 0));
}

/*
 * Conditional branch: decrements CTR if BO says so, then tests CTR and/or
 * the CR bit. The taken path continues at `target` (or at the register
 * `target_reg` if it isn't 0xFF), the other at pc + 4.
 */
static void cond_branch(struct tx *t, unsigned bo, unsigned bi, bool use_ctr, uint32_t target, unsigned target_reg)
{
	uint32_t *nt[2];
	unsigned nnt = 0, n = t->k + 1, ctr = 0, cr = 0;

	if (use_ctr && !(bo & 4)) {
		ctr = rw(t, G_CTR);
		emit(t, a64_addsub_imm(0, 1, 0, ctr, ctr, 1));
	}
	if (!(bo & 16))
		cr = rd(t, G_CR);
	rc_flush(t);
	if (use_ctr && !(bo & 4)) {
		nt[nnt++] = t->p;
		emit(t, a64_cbz(0, (bo & 2) != 0, ctr, 0));    /* not taken if CTR != 0 for bdz, == 0 for bdnz */
	}
	if (!(bo & 16)) {
		nt[nnt++] = t->p;
		emit(t, a64_tbz(!(bo & 8), cr, 31 - bi, 0));
	}
	if (target_reg != 0xFF) {
		mov_reg(t, 0, target_reg);
		finish(t, n, true);
	} else {
		finish_at(t, n, target, true);
	}
	if (nnt) {
		for (unsigned k = 0; k < nnt; k++)
			patch(t, nt[k], (*nt[k] & 0x7E000000u) == 0x36000000u ? FIX_IMM14 : FIX_IMM19, t->p);
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
	unsigned ctr = 0, cr = 0;
	if (!(bo & 4)) {
		ctr = rw(t, G_CTR);
		emit(t, a64_addsub_imm(0, 1, 0, ctr, ctr, 1));
	}
	if (!(bo & 16))
		cr = rd(t, G_CR);
	struct branch_exit *x = &t->bexits[t->nbexits++];
	x->k = t->k;
	x->target = target;
	x->cs = t->rc;
	uint32_t *skip = NULL;
	if (!(bo & 4) && !(bo & 16)) {
		skip = fwd(t, a64_cbz(0, (bo & 2) != 0, ctr, 0));      /* not taken: CTR says no */
		x->at = t->p;
		x->type = FIX_IMM14;
		emit(t, a64_tbz((bo & 8) != 0, cr, 31 - bi, 0));
	} else if (!(bo & 4)) {
		x->at = t->p;
		x->type = FIX_IMM19;
		emit(t, a64_cbz(0, !(bo & 2), ctr, 0));
	} else {
		x->at = t->p;
		x->type = FIX_IMM14;
		emit(t, a64_tbz((bo & 8) != 0, cr, 31 - bi, 0));
	}
	land(t, skip);
	return true;
}

static void branch_exits(struct tx *t)
{
	for (unsigned e = 0; e < t->nbexits && !t->overflow; e++) {
		struct branch_exit *x = &t->bexits[e];
		patch(t, x->at, x->type, t->p);
		t->rc = x->cs;
		finish_at(t, x->k + 1, x->target, true);
	}
}

static void set_lr(struct tx *t)
{
	mov32(t, wr(t, G_LR), t->pc + 4);
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
		cond_branch(t, bo, bi, true, (i & 2) ? bd : pc + bd, 0xFF);
		return BRANCHED;
	}
	default: {                                                      /* bclr, bcctr */
		bool lr = ((i >> 1) & 0x3FF) == 16;
		logic_imm(t, A64_AND, 3, rd(t, lr ? G_LR : G_CTR), ~3u, 4);
		if (i & 1)
			set_lr(t);
		cond_branch(t, bo, bi, lr, 0, 3);
		return BRANCHED;
	}
	}
}

/*
 * rD = the time base (its upper half if `high`), as the interpreter has it:
 * the value when generated code was entered, plus the step for each
 * instruction retired since (those before this block's, then this
 * block's up to here).
 */
static void read_tb(struct tx *t, unsigned rd_, bool high)
{
	emit(t, a64_ldst_imm(3, true, 9, R_CPU, OFF(tb)));
	emit(t, a64_ldst_imm(3, true, 10, R_STATE, (unsigned)offsetof(struct jit_state, entry_budget)));
	emit(t, a64_addsub(1, 1, 0, 10, 10, R_BUDGET, A64_LSL, 0));
	if (t->k)
		emit(t, a64_addsub_imm(1, 0, 0, 10, 10, t->k));
	emit(t, a64_ldst_imm(2, true, 11, R_CPU, OFF(tb_step)));
	emit(t, a64_madd(1, 9, 10, 11, 9));
	unsigned d = wr(t, rd_);
	if (high)
		emit(t, a64_bitfield(1, A64_UBFM, d, 9, 32, 63));
	else
		mov_reg(t, d, 9);
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
	case 10:                                                        /* addc */
		a = rd(t, ra);
		b = rd(t, rb);
		d = wr(t, rd_);
		emit(t, a64_addsub(0, 0, xo == 10 || oe, d, a, b, A64_LSL, 0));
		xer_from_flags(t, xo == 10, oe);
		break;
	case 40:                                                        /* subf: b - a */
	case 8:                                                         /* subfc */
		a = rd(t, ra);
		b = rd(t, rb);
		d = wr(t, rd_);
		emit(t, a64_addsub(0, 1, xo == 8 || oe, d, b, a, A64_LSL, 0));
		xer_from_flags(t, xo == 8, oe);
		break;
	case 104:                                                       /* neg */
		a = rd(t, ra);
		d = wr(t, rd_);
		emit(t, a64_addsub(0, 1, oe, d, A64_ZR, a, A64_LSL, 0));
		xer_from_flags(t, false, oe);
		break;
	case 138: case 136: case 202: case 234: case 200: case 232: {
		/* a (or ~a for subf*) + b (or 0, or -1) + CA */
		bool inv = xo == 136 || xo == 200 || xo == 232;
		unsigned src = a = rd(t, ra), bsrc;
		if (inv) {
			emit(t, a64_logic(0, A64_ORR, 1, 1, A64_ZR, a, A64_LSL, 0));
			src = 1;
		}
		if (xo == 202 || xo == 200) {
			bsrc = A64_ZR;
		} else if (xo == 234 || xo == 232) {
			emit(t, a64_movw(0, A64_MOVN, 2, 0, 0));
			bsrc = 2;
		} else {
			bsrc = rd(t, rb);
		}
		d = wr(t, rd_);
		carry_from_xer(t);
		emit(t, a64_adc(0, 0, 1, d, src, bsrc));
		xer_from_flags(t, true, oe);
		break;
	}
	case 235:                                                       /* mullw */
		a = rd(t, ra);
		b = rd(t, rb);
		if (!oe) {
			d = wr(t, rd_);
			emit(t, a64_madd(0, d, a, b, A64_ZR));
		} else {
			emit(t, a64_maddl(false, 3, a, b, A64_ZR));
			emit(t, a64_bitfield(1, A64_SBFM, 4, 3, 0, 31));      /* sxtw */
			emit(t, a64_addsub(1, 1, 1, A64_ZR, 4, 3, A64_LSL, 0));
			cset(t, 6, A64_NE);
			xer_ov_from(t, 6);
			d = wr(t, rd_);
			mov_reg(t, d, 3);
		}
		break;
	case 491: case 459:                                             /* divw, divwu */
		if (oe)
			return false;
		/* bad divisors leave rA (the interpreter's choice); SDIV already gives INT_MIN / -1 = rA */
		a = rd(t, ra);
		b = rd(t, rb);
		emit(t, a64_dp2(0, xo == 491 ? A64_SDIV : A64_UDIV, 1, a, b));
		emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, b, 0));
		d = wr(t, rd_);
		emit(t, a64_csel(0, 0, 0, d, a, 1, A64_EQ));
		break;
	case 75: case 11:                                               /* mulhw, mulhwu */
		a = rd(t, ra);
		b = rd(t, rb);
		emit(t, a64_maddl(xo == 11, 3, a, b, A64_ZR));
		emit(t, a64_bitfield(1, A64_UBFM, 3, 3, 32, 63));
		d = wr(t, rd_);
		mov_reg(t, d, 3);
		break;
	default:
		return false;
	}
	if (i & 1)
		record(t, d);
	return true;
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
		emit(t, a64_addsub(0, 1, 1, A64_ZR, a, b, A64_LSL, 0));
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
		mov_reg(t, wr(t, rd_), cr);
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
			mov_reg(t, wr(t, G_CR), s);
		} else {
			unsigned cr = rw(t, G_CR);
			mov32(t, 9, mask);
			emit(t, a64_logic(0, A64_AND, 1, cr, cr, 9, A64_LSL, 0));       /* bic */
			emit(t, a64_logic(0, A64_AND, 0, 10, s, 9, A64_LSL, 0));
			emit(t, a64_logic(0, A64_ORR, 0, cr, cr, 10, A64_LSL, 0));
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
			mov_reg(t, wr(t, rd_), s);
		} else {
			unsigned s = rd(t, rd_);
			mov_reg(t, wr(t, g), s);
		}
		return EMITTED;
	}
	case 26: case 922: case 954: {                                  /* cntlzw, extsh, extsb */
		unsigned s = rd(t, rd_), d = wr(t, ra);
		if (xo == 26)
			emit(t, a64_dp1(0, A64_CLZ, d, s));
		else
			emit(t, a64_bitfield(0, A64_SBFM, d, s, 0, xo == 922 ? 15 : 7));
		if (rc)
			record(t, d);
		return EMITTED;
	}
	case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476: {
		unsigned s = rd(t, rd_), d;
		if (xo == 444 && rb == rd_) {                           /* mr */
			d = wr(t, ra);
			mov_reg(t, d, s);
		} else {
			unsigned b = rd(t, rb);
			enum a64_logic op = A64_AND;
			unsigned inv = 0;
			bool not_after = false;
			switch (xo) {
			case 28:  op = A64_AND; break;                          /* and */
			case 60:  op = A64_AND; inv = 1; break;                 /* andc */
			case 124: op = A64_ORR; not_after = true; break;        /* nor */
			case 284: op = A64_EOR; inv = 1; break;                 /* eqv */
			case 316: op = A64_EOR; break;                          /* xor */
			case 412: op = A64_ORR; inv = 1; break;                 /* orc */
			case 444: op = A64_ORR; break;                          /* or */
			case 476: op = A64_AND; not_after = true; break;        /* nand */
			}
			d = wr(t, ra);
			emit(t, a64_logic(0, op, inv, d, s, b, A64_LSL, 0));
			if (not_after)
				emit(t, a64_logic(0, A64_ORR, 1, d, A64_ZR, d, A64_LSL, 0));
		}
		if (rc)
			record(t, d);
		return EMITTED;
	}
	case 24: case 536: {                                            /* slw, srw */
		unsigned s = rd(t, rd_), b = rd(t, rb);
		emit(t, a64_dp2(0, xo == 24 ? A64_LSLV : A64_LSRV, 1, s, b));
		logic_imm(t, A64_ANDS, A64_ZR, b, 0x20, 9);
		unsigned d = wr(t, ra);
		emit(t, a64_csel(0, 0, 0, d, A64_ZR, 1, A64_NE));
		if (rc)
			record(t, d);
		return EMITTED;
	}

	case 824: case 792: {                                           /* srawi, sraw */
		/* CA: negative and ones shifted out; sraw by 32-63 fills with the sign */
		unsigned s = rd(t, rd_), d;
		if (xo == 824 && rb == 0) {
			d = wr(t, ra);
			mov_reg(t, d, s);
			bfi(t, rw(t, G_XER), A64_ZR, 29, 1);
		} else {
			if (xo == 824) {
				emit(t, a64_bitfield(0, A64_SBFM, 1, s, rb, 31));
				logic_imm(t, A64_ANDS, A64_ZR, s, (1u << rb) - 1, 9);
			} else {
				unsigned b = rd(t, rb);
				logic_imm(t, A64_AND, 2, b, 0x3F, 9);
				emit(t, a64_movw(0, A64_MOVZ, 3, 31, 0));
				emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, 2, 31));
				emit(t, a64_csel(0, 0, 0, 3, 2, 3, A64_LS));
				emit(t, a64_dp2(0, A64_ASRV, 1, s, 3));
				emit(t, a64_movw(0, A64_MOVZ, 4, 1, 0));
				emit(t, a64_dp2(0, A64_LSLV, 4, 4, 2));
				emit(t, a64_addsub_imm(0, 1, 0, 4, 4, 1));
				emit(t, a64_logic(0, A64_AND, 0, 4, 4, s, A64_LSL, 0));
				emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, 2, 31));
				emit(t, a64_csel(0, 1, 0, 4, 4, A64_ZR, A64_LS));
				emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, 4, 0));
			}
			cset(t, 5, A64_NE);
			emit(t, a64_logic(0, A64_AND, 0, 5, 5, s, A64_LSR, 31));
			bfi(t, rw(t, G_XER), 5, 29, 1);
			d = wr(t, ra);
			mov_reg(t, d, 1);
		}
		if (rc)
			record(t, d);
		return EMITTED;
	}
	case 1014: {                                                    /* dcbz */
		unsigned base = rd0(t, ra), b = rd(t, rb);
		emit(t, a64_addsub(0, 0, 0, 1, base, b, A64_LSL, 0));
		logic_imm(t, A64_AND, 1, 1, ~31u, 9);
		block_access(t, false, 1);      /* (32 aligned bytes don't cross a page) */
		emit(t, a64_pair(false, A64_OFFSET, A64_ZR, A64_ZR, 10, 0));
		emit(t, a64_pair(false, A64_OFFSET, A64_ZR, A64_ZR, 10, 16));
		return EMITTED;
	}

	case 23: case 55:   load_store(t, 2, true, false, xo == 55, true, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 87: case 119:  load_store(t, 0, true, false, xo == 119, true, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 279: case 311: load_store(t, 1, true, false, xo == 311, true, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 343: case 375: load_store(t, 1, true, true, xo == 375, true, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 151: case 183: load_store(t, 2, false, false, xo == 183, true, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 215: case 247: load_store(t, 0, false, false, xo == 247, true, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 407: case 439: load_store(t, 1, false, false, xo == 439, true, LS_GPR, rd_, ra, rb, 0); return EMITTED;
	case 599: case 631: load_store(t, 3, true, false, xo == 631, true, LS_DOUBLE, rd_, ra, rb, 0); return EMITTED;
	case 727: case 759: load_store(t, 3, false, false, xo == 759, true, LS_DOUBLE, rd_, ra, rb, 0); return EMITTED;
	case 535: case 567: load_store(t, 2, true, false, xo == 567, true, LS_SINGLE, rd_, ra, rb, 0); return EMITTED;
	case 663: case 695: load_store(t, 2, false, false, xo == 695, true, LS_SINGLE, rd_, ra, rb, 0); return EMITTED;

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
		ubfx(t, 2, cr, 28 - 4 * s, 4);
		bfi(t, cr, 2, 28 - 4 * d, 4);
		return EMITTED;
	}
	case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449: {
		unsigned cr = rw(t, G_CR);
		if (ba == bt && bb == bt && (xo == 193 || xo == 289)) { /* crclr, crset */
			logic_imm(t, xo == 193 ? A64_AND : A64_ORR, cr, cr,
				  xo == 193 ? ~(1u << (31 - bt)) : 1u << (31 - bt), 9);
		} else {
			ubfx(t, 2, cr, 31 - ba, 1);
			ubfx(t, 3, cr, 31 - bb, 1);
			enum a64_logic op = A64_AND;
			unsigned inv = 0;
			bool not_after = false;
			switch (xo) {
			case 33:  op = A64_ORR; not_after = true; break;        /* crnor */
			case 129: op = A64_AND; inv = 1; break;                 /* crandc */
			case 193: op = A64_EOR; break;                          /* crxor */
			case 225: op = A64_AND; not_after = true; break;        /* crnand */
			case 257: op = A64_AND; break;                          /* crand */
			case 289: op = A64_EOR; inv = 1; break;                 /* creqv */
			case 417: op = A64_ORR; inv = 1; break;                 /* crorc */
			case 449: op = A64_ORR; break;                          /* cror */
			}
			emit(t, a64_logic(0, op, inv, 4, 2, 3, A64_LSL, 0));
			if (not_after)
				emit(t, a64_logic(0, A64_ORR, 1, 4, A64_ZR, 4, A64_LSL, 0));
			bfi(t, cr, 4, 31 - bt, 1);
		}
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
	mov32(t, 0, t->pc);
	st32(t, 0, OFF(pc));
	mov32(t, 0, insn);
	st32(t, 0, OFF(exit_insn));
	if (t->k)
		emit(t, a64_addsub_imm(1, 1, 0, R_BUDGET, R_BUDGET, t->k));
	emit(t, a64_movw(0, A64_MOVZ, 1, kind, 0));
	branch_to(t, t->j->exit);
	return EXITED;
}

static int insn(struct tx *t, uint32_t i)
{
	unsigned op = i >> 26, rd_ = (i >> 21) & 31, ra = (i >> 16) & 31;
	int32_t simm = (int16_t)(i & 0xFFFF);
	uint32_t uimm = i & 0xFFFF;

	switch (op) {
	case 3:                                 /* twi: unless it always traps, the interpreter tests */
		return rd_ == 31 ? kernel_exit(t, i, EXIT_TRAP) : STOPPED;
	case 6:                                 /* host call */
		return kernel_exit(t, i, EXIT_HOSTCALL);
	case 17:                                /* sc */
		return kernel_exit(t, i, EXIT_SC);
	case 4:                                 /* AltiVec */
		return STOPPED;
	case 7: {                                                       /* mulli */
		unsigned a = rd(t, ra);
		mov32(t, 2, (uint32_t)simm);
		emit(t, a64_madd(0, wr(t, rd_), a, 2, A64_ZR));
		return EMITTED;
	}
	case 8: {                                                       /* subfic */
		unsigned a = rd(t, ra);
		mov32(t, 2, (uint32_t)simm);
		emit(t, a64_addsub(0, 1, 1, wr(t, rd_), 2, a, A64_LSL, 0));
		xer_from_flags(t, true, false);
		return EMITTED;
	}
	case 10: case 11: {                                             /* cmpli, cmpi */
		bool is_signed = op == 11;
		unsigned a = rd(t, ra);
		if (is_signed ? (simm >= 0 && simm < 4096) : uimm < 4096) {
			emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, a, is_signed ? (unsigned)simm : uimm));
		} else if (is_signed && simm < 0 && simm > -4096) {
			emit(t, a64_addsub_imm(0, 0, 1, A64_ZR, a, (unsigned)-simm));  /* cmn: signed flags agree */
		} else {
			mov32(t, 2, is_signed ? (uint32_t)simm : uimm);
			emit(t, a64_addsub(0, 1, 1, A64_ZR, a, 2, A64_LSL, 0));
		}
		cr_from_flags(t, (i >> 23) & 7, is_signed);
		return EMITTED;
	}
	case 12: case 13: {                                             /* addic, addic. */
		unsigned a = rd(t, ra);
		mov32(t, 2, (uint32_t)simm);
		unsigned d = wr(t, rd_);
		emit(t, a64_addsub(0, 0, 1, d, a, 2, A64_LSL, 0));
		xer_from_flags(t, true, false);
		if (op == 13)
			record(t, d);
		return EMITTED;
	}
	case 14: case 15: {                                             /* addi, addis */
		int32_t imm = op == 15 ? (int32_t)(uimm << 16) : simm;
		if (ra) {
			unsigned a = rd(t, ra);
			add_imm(t, wr(t, rd_), a, imm, 2);
		} else {
			mov32(t, wr(t, rd_), (uint32_t)imm);
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
		emit(t, a64_addsub(0, 1, 0, 2, A64_ZR, b, A64_LSL, 0));
		emit(t, a64_dp2(0, A64_RORV, 1, s, 2));
		unsigned d = wr(t, ra);
		if (mask != 0xFFFFFFFFu)
			logic_imm(t, A64_AND, d, 1, mask, 9);
		else
			mov_reg(t, d, 1);
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
			logic_imm(t, op < 26 ? A64_ORR : A64_EOR, d, s, imm, 2);
		else
			mov_reg(t, d, s);
		return EMITTED;
	}
	case 28: case 29: {                                             /* andi., andis. */
		uint32_t imm = op == 29 ? uimm << 16 : uimm;
		unsigned d;
		if (imm) {
			unsigned s = rd(t, rd_);
			d = wr(t, ra);
			logic_imm(t, A64_AND, d, s, imm, 2);
		} else {
			d = wr(t, ra);
			mov_reg(t, d, A64_ZR);
		}
		record(t, d);
		return EMITTED;
	}
	case 31:
		return insn31(t, i);
	case 32: case 33: load_store(t, 2, true, false, op == 33, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 34: case 35: load_store(t, 0, true, false, op == 35, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 40: case 41: load_store(t, 1, true, false, op == 41, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 42: case 43: load_store(t, 1, true, true, op == 43, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 36: case 37: load_store(t, 2, false, false, op == 37, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 38: case 39: load_store(t, 0, false, false, op == 39, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 44: case 45: load_store(t, 1, false, false, op == 45, false, LS_GPR, rd_, ra, 0, simm); return EMITTED;
	case 48: case 49: load_store(t, 2, true, false, op == 49, false, LS_SINGLE, rd_, ra, 0, simm); return EMITTED;
	case 50: case 51: load_store(t, 3, true, false, op == 51, false, LS_DOUBLE, rd_, ra, 0, simm); return EMITTED;
	case 52: case 53: load_store(t, 2, false, false, op == 53, false, LS_SINGLE, rd_, ra, 0, simm); return EMITTED;
	case 54: case 55: load_store(t, 3, false, false, op == 55, false, LS_DOUBLE, rd_, ra, 0, simm); return EMITTED;
	case 59: case 63:
		if (!fp_insn(t, i))
			call_step(t, i);
		return EMITTED;
	case 46: case 47: load_store_multiple(t, op == 46, rd_, ra, simm); return EMITTED;
	default:                /* single-precision loads and stores, floating point, string operations, ... */
		call_step(t, i);
		return EMITTED;
	}
}

/* ---- blocks ----------------------------------------------------------- */

static struct jit_page *get_page(struct jit *j, uint32_t page);

/* The hook's table entry stops branches to it (its block is found through the page) */
static void set_hook_entry(struct jit *j)
{
	struct jit_page *pg;
	if (j->linked_hook != PPC_NO_HOOK && (pg = get_page(j, j->linked_hook >> MEM_PAGE_SHIFT)))
		pg->l2[(j->linked_hook >> 2) & (PAGE_WORDS - 1)] = j->hook_exit;
}

static void flush(struct jit *j)
{
	for (uint32_t k = 0; k < j->ncode_pages; k++) {
		uint32_t pg = j->code_pages[k];
		j->pages[pg] = NULL;
		j->l1[pg] = j->l2_empty;
		mem_set_code(j->mem, pg, false);
	}
	j->ncode_pages = 0;
	j->nblocks = 0;
	j->nsites = 0;
	j->code_free = j->blocks_start;
	j->flushes++;
	set_hook_entry(j);
}

/* The page's record, made if need be; NULL if there's no room (flush first) */
static struct jit_page *get_page(struct jit *j, uint32_t page)
{
	struct jit_page *pg = j->pages[page];
	if (pg || j->ncode_pages >= MAX_CODE_PAGES)
		return pg;
	pg = &j->page_pool[j->ncode_pages];
	memset(pg, 0, sizeof *pg);
	pg->l2 = j->l2_pool + (size_t)j->ncode_pages * PAGE_WORDS;
	for (unsigned w = 0; w < PAGE_WORDS; w++)
		pg->l2[w] = j->miss_exit;
	j->code_pages[j->ncode_pages++] = page;
	j->pages[page] = pg;
	j->l1[page] = pg->l2;
	return pg;
}

/* Bits first..last of a 64-bit word's range [64 * k, 64 * k + 63], as a mask */
static uint64_t word_mask(unsigned k, unsigned first, unsigned last)
{
	unsigned lo = first > 64 * k ? first - 64 * k : 0, hi = last < 64 * k + 63 ? last - 64 * k : 63;
	return (~0ull >> (63 - hi)) & (~0ull << lo);
}

static void set_words(struct jit_page *pg, unsigned first, unsigned count)
{
	unsigned last = first + count - 1 < PAGE_WORDS ? first + count - 1 : PAGE_WORDS - 1;
	for (unsigned k = first / 64; k <= last / 64; k++)
		pg->words[k] |= word_mask(k, first, last);
}

static bool any_words(const struct jit_page *pg, unsigned first, unsigned last)
{
	for (unsigned k = first / 64; k <= last / 64; k++)
		if (pg->words[k] & word_mask(k, first, last))
			return true;
	return false;
}

/*
 * Instruction cache maintenance for changed code (the host's data cache is
 * coherent with instruction fetch, CTR_EL0.IDC, but its instruction cache
 * isn't). icache_lines() invalidates; the barriers that complete it come
 * before generated code next runs (icache_sync()), unless the change was a
 * B turned into another B (a link): the architecture lets a core execute
 * such an instruction while it changes, seeing the old or the new one, and
 * both are right here.
 *
 * The barrier is DSB NSH on Linux: generated code runs in one thread, and if
 * Linux moves it to another core, the context switch completes this core's
 * outstanding maintenance first (DSB ISH in __switch_to). macOS makes no
 * such promise, so there it's DSB ISH.
 */
static void icache_lines(struct jit *j, const void *start, const void *end, bool b_to_b)
{
	__asm__ volatile("dsb ishst" ::: "memory");
	for (uintptr_t a = (uintptr_t)start & ~(uintptr_t)63; a < (uintptr_t)end; a += 64)
		__asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
	if (!b_to_b)
		j->sync_pending = true;
}

static void icache_sync(struct jit *j)
{
	if (j->sync_pending) {
#ifdef __APPLE__
		__asm__ volatile("dsb ish\n\tisb" ::: "memory");
#else
		__asm__ volatile("dsb nsh\n\tisb" ::: "memory");
#endif
		j->sync_pending = false;
	}
}

/*
 * Writes to the code buffer go between these. On macOS it's MAP_JIT memory,
 * which each thread sees either as writable or as executable, so they
 * switch this thread over and back. They nest: generated code may be
 * further up the stack (in a call into C), and runs again only after the
 * outermost pair has closed.
 */
#ifdef __APPLE__
static _Thread_local unsigned code_writes;

static void code_write_begin(void)
{
	if (!code_writes++)
		pthread_jit_write_protect_np(0);
}

static void code_write_end(void)
{
	if (!--code_writes)
		pthread_jit_write_protect_np(1);
}
#else
static void code_write_begin(void) {}
static void code_write_end(void) {}
#endif

/* Takes a block out of the table, and sends branches into it back to C */
static void retire(struct jit *j, struct jit_page *pg, struct jit_block *b)
{
	unsigned first = (b->pc & (MEM_PAGE_SIZE - 1)) / 4;
	if (pg->l2[first] == b->code)
		pg->l2[first] = j->miss_exit;
	uint32_t *entry = (uint32_t *)(uintptr_t)b->code;
	code_write_begin();
	*entry = a64_b(false, (int32_t)((b->redispatch - entry) * 4));
	code_write_end();
	icache_lines(j, entry, entry + 1, false);
	icache_sync(j);                 /* (we may be in a call from generated code) */
}

/*
 * [addr, addr + len) (within one page) is about to change: drop the blocks
 * translated from those bytes. Called by struct mem for pages with MEM_CODE.
 */
static void code_changed(void *ctx, uint32_t addr, uint32_t len)
{
	struct jit *j = ctx;
	uint32_t page = addr >> MEM_PAGE_SHIFT;
	struct jit_page *pg = j->pages[page];
	unsigned first = (addr & (MEM_PAGE_SIZE - 1)) / 4, last = ((addr + len - 1) & (MEM_PAGE_SIZE - 1)) / 4;

	/*
	 * New code there gets its own chance at fastmem, and must become hot
	 * again before it's translated: the 68k emulator writes translations
	 * over old ones all the time, and many run only a few times.
	 */
	if (pg) {
		for (unsigned k = first / 64; k <= last / 64; k++) {
			pg->slow[k] &= ~word_mask(k, first, last);
			pg->io[k] &= ~word_mask(k, first, last);
		}
		memset(pg->runs + first, 0, last - first + 1);
	}
	if (pg && pg->followed) {               /* "ROM" changed: start afresh (never seen) */
		if (j->running) {
			j->flush_pending = true;
			j->st.code_changed = 1;
		} else {
			flush(j);
		}
		return;
	}
	if (!pg || !pg->blocks) {
		mem_set_code(j->mem, page, false);
		return;
	}
	if (!any_words(pg, first, last))
		return;
	memset(pg->words, 0, sizeof pg->words);
	for (struct jit_block **pb = &pg->blocks; *pb;) {
		struct jit_block *b = *pb;
		unsigned bfirst = (b->pc & (MEM_PAGE_SIZE - 1)) / 4;
		if (bfirst <= last && bfirst + b->cover > first) {
			retire(j, pg, b);
			*pb = b->next;
		} else {
			set_words(pg, bfirst, b->cover);
			pb = &b->next;
		}
	}
	if (!pg->blocks) {              /* (stores here go unseen from now on: assume they're new code) */
		mem_set_code(j->mem, page, false);
		memset(pg->runs, 0, sizeof pg->runs);
	}
	j->invalidations++;
	j->st.code_changed = 1;
}

/* Drops a block whose memory access faulted (if it's still there), so that it's translated again */
static void drop_block(struct jit *j, struct jit_block *b)
{
	struct jit_page *pg = j->pages[b->pc >> MEM_PAGE_SHIFT];
	struct jit_block **pb = pg ? &pg->blocks : NULL;
	while (pb && *pb && *pb != b)
		pb = &(*pb)->next;
	if (!pb || !*pb)
		return;
	*pb = b->next;
	retire(j, pg, b);
	memset(pg->words, 0, sizeof pg->words);
	for (const struct jit_block *o = pg->blocks; o; o = o->next)
		set_words(pg, (o->pc & (MEM_PAGE_SIZE - 1)) / 4, o->cover);
	if (!pg->blocks && !pg->followed)
		mem_set_code(j->mem, b->pc >> MEM_PAGE_SHIFT, false);
}

/*
 * Works out what generated code left pending in the FPSCR (see fp_arith()):
 * the interpreter replays that instruction, with its operands, on a scratch
 * processor whose FPSCR is ours (XX and FX already right for it).
 */
static void fp_settle(struct jit *j, struct ppc_cpu *c)
{
	uint32_t insn = j->st.fp_insn;
	if (!insn)
		return;
	static struct ppc_cpu scratch;
	scratch.fpscr = c->fpscr;
	memcpy(&scratch.fpr[1], j->st.fp_ops, sizeof j->st.fp_ops);
	ppc_fp_exec(&scratch, (insn & 0xFC00003Eu) | 1u << 16 | 2u << 11 | 3u << 6);    /* (f0 = f1 op f2, f3) */
	c->fpscr = scratch.fpscr;
	if (j->st.fp_fpcc)
		c->fpscr = (c->fpscr & ~0xF000u) | (j->st.fp_fpcc & 0xFu) << 12;
	j->st.fp_insn = j->st.fp_fpcc = 0;
}

/* The interpreter's step for generated code, which may read the FPSCR */
static bool step(struct ppc_cpu *c, uint32_t insn, uint32_t pc)
{
	fp_settle(c->jit, c);
	return ppc_step(c, insn, pc);
}

/* A store by translated code to a page holding translated code; `value` is in guest byte order */
static void code_store(struct jit_state *st, uint32_t ea, uint64_t value, uint32_t size)
{
	struct jit *j = (struct jit *)st;
	mem_invalidate(j->mem, ea, 1u << size);
	memcpy(mem_ptr(j->mem, ea), &value, 1u << size);        /* (a little-endian host) */
}

/*
 * A load or store in generated code that page flags turned away (see
 * slow_ios()), with `left` instructions left in the run, this one
 * included: if it faults as the interpreter's would, the mmio hook may do
 * it. Unless that goes on (PPC_MMIO_DONE), the interpreter takes it, or
 * the block is left after it: for the kernel (EXIT_MMIO), or because code
 * changed, perhaps the block's own (EXIT_MISS: C looks for the next).
 * `access` is the size in bytes, | 0x100 for a store.
 */
static unsigned mmio(struct ppc_cpu *c, uint32_t insn, uint32_t pc, uint32_t ea, uint32_t access, uint64_t left)
{
	struct jit *j = c->jit;
	bool write = access >> 8;
	if (!c->mmio || mem_ok(c->mem, ea, access & 0xFF, write ? MEM_W : MEM_R))
		return PPC_MMIO_NO;
	c->pc = pc;
	c->exit_insn = insn;
	c->fault_addr = ea;
	c->fault_write = write;
	int r = ppc_mmio(c, j->st.entry_budget - left);
	if (r == PPC_MMIO_DONE && j->st.code_changed) {
		j->st.mmio_exit = EXIT_MISS;
		return PPC_MMIO_STOP;
	}
	j->st.mmio_exit = EXIT_MMIO;
	return (unsigned)r;
}

/*
 * Executable and not writable, like the ROM: blocks may follow code into
 * such a page without registering in it. It's marked instead, so that any
 * change to it (which doesn't happen) flushes everything.
 */
static bool rom_like(struct jit *j, uint32_t pc)
{
	if ((j->mem->pageflags[pc >> MEM_PAGE_SHIFT] & (MEM_X | MEM_W)) != MEM_X)
		return false;
	struct jit_page *pg = get_page(j, pc >> MEM_PAGE_SHIFT);
	if (!pg)
		return false;
	pg->followed = true;
	mem_set_code(j->mem, pc >> MEM_PAGE_SHIFT, true);
	return true;
}

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
	uint32_t *no[3];

	emit(t, a64_ldst_imm(0, true, 9, R_CPU, OFF(hook_match_skip)));
	uint32_t *resume = fwd(t, a64_cbz(0, true, 9, 0));
	if (!a) {
		ld32(t, 10, GPR(j->match_reg & 31u));
		a = 10;
	}
	emit(t, a64_bitfield(0, A64_UBFM, 9, a, MEM_PAGE_SHIFT, 31));
	emit(t, a64_ldst_uxtw(0, true, 9, R_FLAGS, 9));
	uint32_t *readable = fwd(t, a64_tbz(true, 9, FAST_R_BIT, 0));     /* this page and the next */
	no[0] = fwd(t, a64_tbz(false, 9, 0, 0));                            /* MEM_R */
	logic_imm(t, A64_AND, 9, a, MEM_PAGE_SIZE - 1, 9);
	emit(t, a64_addsub_imm(0, 1, 1, A64_ZR, 9, MEM_PAGE_SIZE - 1));
	no[1] = fwd(t, a64_bcond(A64_EQ, 0));
	if (!t->overflow)
		patch(t, readable, FIX_IMM14, t->p);
	emit(t, a64_ldst_uxtw(1, true, 9, R_CBASE, a));
	mov32(t, 10, __builtin_bswap16(j->match_value));
	emit(t, a64_addsub(0, 1, 1, A64_ZR, 9, 10, A64_LSL, 0));
	no[2] = fwd(t, a64_bcond(A64_NE, 0));
	emit(t, a64_movw(0, A64_MOVZ, 9, 1, 0));
	emit(t, a64_ldst_imm(0, false, 9, R_CPU, OFF(hook_match_skip)));
	mov32(t, 0, pc0);
	st32(t, 0, OFF(pc));
	emit(t, a64_movw(0, A64_MOVZ, 1, EXIT_HOOK, 0));
	branch_to(t, j->exit);
	land(t, resume);
	emit(t, a64_ldst_imm(0, false, A64_ZR, R_CPU, OFF(hook_match_skip)));
	if (!t->overflow)
		patch(t, no[0], FIX_IMM14, t->p);
	land(t, no[1]);
	land(t, no[2]);
}

/*
 * A block of at most `max_insns` instructions from pc0; NULL if there's no
 * room (and the JIT was flushed), or (*too_far) if a branch in it couldn't
 * reach its target.
 */
static struct jit_block *translate_upto(struct jit *j, uint32_t pc0, unsigned max_insns, bool *too_far)
{
	*too_far = false;
	if (!mem_ok(j->mem, pc0, 4, MEM_X))
		return NULL;
	uint32_t page = pc0 >> MEM_PAGE_SHIFT;
	if ((size_t)(j->code_end - j->code_free) < BLOCK_RESERVE / 4 || j->nblocks >= MAX_BLOCKS ||
	    (!j->pages[page] && j->ncode_pages + 1 >= MAX_CODE_PAGES) || j->nsites + MAX_BLOCK_INSNS > MAX_SITES)
		flush(j);
	struct jit_page *pg = get_page(j, page);
	uint32_t nsites = j->nsites;

	static struct tx t;
	t.j = j;
	t.max_insns = max_insns;
	t.too_far = false;
	t.p = j->code_free + ((uintptr_t)j->code_free & 4 ? 1 : 0);    /* the header is 8-byte aligned */
	uint32_t *header = t.p;
	t.p += sizeof(struct jit_block *) / 4;
	t.end = j->code_end;
	t.overflow = false;
	t.nfix = 0;
	t.nslow = 0;
	t.nio = 0;
	t.nlinks = 0;
	t.nfps = 0;
	t.nbexits = 0;
	rc_reset(&t);
	uint32_t *start = t.p;
	uint32_t pc = pc0;
	unsigned n = 0, budget_needed, cover = 0;

	emit(&t, a64_addsub_imm(1, 1, 1, A64_ZR, R_BUDGET, 0));        /* cmp x22, #ninsn (patched) */
	uint32_t *to_budget = t.p;
	emit(&t, a64_bcond(A64_LO, 0));
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
		    n + 2 < t.max_insns &&
		    t.p - start < BLOCK_MAX_WORDS / 2 && !visited(&t, n, target) && rom_like(j, target)) {
			budget_needed = ++n;
			pc = target;
			continue;
		}
		t.insn = i;
		int r = insn(&t, i);
		if (r == STOPPED) {
			add_fixup(&t, FIX_IMM19);       /* straight to the side exit */
			emit(&t, a64_bcond(A64_AL, 0));
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
		bool crossing = !(pc & (MEM_PAGE_SIZE - 1)) && !(rom_like(j, pc - 4) && rom_like(j, pc));
		if (crossing || pc == j->match_pc || n >= t.max_insns || t.p - start > BLOCK_MAX_WORDS ||
		    t.nfix + 16 > MAX_FIXUPS) {
			finish_at(&t, n, pc, false);
			break;
		}
	}
	if (!cover)
		cover = budget_needed;

	rc_reset(&t);                   /* (what follows is out of line) */
	slow_stores(&t);
	slow_ios(&t);
	slow_fps(&t);
	branch_exits(&t);

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
		uint32_t *stub = t.p;
		rc_store(&t, &cs);
		if (after) {
			emit(&t, a64_ldst_imm(2, false, A64_ZR, R_STATE, (unsigned)offsetof(struct jit_state, code_changed)));
			finish_at(&t, k + 1, t.pcs[k] + 4, false);
		} else {
			mov32(&t, 0, t.pcs[k]);
			if (k)
				emit(&t, a64_addsub_imm(1, 1, 0, R_BUDGET, R_BUDGET, k));
			branch_to(&t, j->side_exit);
		}
		for (; f < t.nfix && t.fix[f].k == k && t.fix[f].after == after &&
		       !memcmp(&t.fix[f].cs, &cs, sizeof cs); f++) {
			if (t.fix[f].type == FIX_SITE)
				j->sites[j->nsites++] = (struct jit_site){ (uint32_t)(t.fix[f].at - j->code),
									   (uint32_t)(stub - j->code), t.pcs[k], j->nblocks };
			else
				patch(&t, t.fix[f].at, t.fix[f].type, stub);
		}
	}

	/* exits to known pcs: ask C for a link the first time */
	for (unsigned l = 0; l < t.nlinks; l++) {
		struct link *ln = &t.links[l];
		*ln->site = a64_b(false, (int32_t)((t.p - ln->site) * 4));
		mov32(&t, 0, ln->target);
		emit(&t, a64_adr(8, (int32_t)((ln->site - t.p) * 4)));
		branch_to(&t, ln->after_branch ? j->link_branch : j->link);
	}

	/* not enough budget for the block */
	patch(&t, to_budget, FIX_IMM19, t.p);
	*start |= budget_needed << 10;
	mov32(&t, 0, pc0);
	st32(&t, 0, OFF(pc));
	emit(&t, a64_movw(0, A64_MOVZ, 1, EXIT_BUDGET, 0));
	branch_to(&t, j->exit);

	/* where the first instruction leads once the block is invalid */
	uint32_t *redispatch = t.p;
	mov32(&t, 0, pc0);
	branch_to(&t, j->lookup);

	if (t.overflow) {
		flush(j);
		return NULL;    /* the interpreter runs this instruction; next time there's room */
	}
	if (t.too_far) {
		j->nsites = nsites;
		*too_far = true;
		return NULL;
	}
	icache_lines(j, start, t.p, false);
	j->code_free = t.p;

	struct jit_block *b = &j->blocks[j->nblocks++];
	*b = (struct jit_block){ pc0, budget_needed, cover, start, redispatch, pg->blocks };
	memcpy(header, &b, sizeof b);
	pg->blocks = b;
	set_words(pg, (pc0 & (MEM_PAGE_SIZE - 1)) / 4, cover);
	if (pg->l2[(pc0 >> 2) & (PAGE_WORDS - 1)] != j->hook_exit)
		pg->l2[(pc0 >> 2) & (PAGE_WORDS - 1)] = start;
	mem_set_code(j->mem, page, true);
	j->translated++;
	j->translated_insns += n;
	return b;
}

static struct jit_block *translate(struct jit *j, uint32_t pc0)
{
	bool too_far;
	struct jit_block *b;
	code_write_begin();
	for (unsigned max = MAX_BLOCK_INSNS;; max /= 2) {
		b = translate_upto(j, pc0, max, &too_far);
		if (!too_far || max == 1)
			break;
	}
	code_write_end();
	return b;
}

/*
 * The block at pc, translated if need be; or NULL, and then the interpreter
 * runs the block's instructions: if they aren't executable, or until they
 * have run `hot` times (the 68k emulator rewrites much of its translation
 * cache before it runs twice, and translating costs more than that).
 */
static struct jit_block *find(struct jit *j, uint32_t pc)
{
	const void *code = j->l1[pc >> MEM_PAGE_SHIFT][(pc >> 2) & (PAGE_WORDS - 1)];
	if (code != j->miss_exit && code != j->hook_exit) {
		struct jit_block *b;
		memcpy(&b, (const uint32_t *)code - sizeof b / 4, sizeof b);
		return b;
	}
	if (code == j->hook_exit) {
		const struct jit_page *pg = j->pages[pc >> MEM_PAGE_SHIFT];
		for (struct jit_block *b = pg->blocks; b; b = b->next)
			if (b->pc == pc)
				return b;
	}
	if (!mem_ok(j->mem, pc, 4, MEM_X))
		return NULL;
	struct jit_page *pg = get_page(j, pc >> MEM_PAGE_SHIFT);
	if (pg && pg->runs[(pc >> 2) & (PAGE_WORDS - 1)] < j->hot) {
		pg->runs[(pc >> 2) & (PAGE_WORDS - 1)]++;
		return NULL;
	}
	return translate(j, pc);
}

/* ---- fastmem --------------------------------------------------------------- */

/*
 * Generated code reaches guest memory through a view of its own (R_MEM): a
 * second mapping of the same memory (mem_view()), where each host page has
 * the protection that all the guest pages in it allow (host pages may be
 * larger). Loads and most stores then need no checks of their own. An
 * access that faults (device registers, unmapped or read-only pages, or a
 * neighbouring guest page's protection) is found in the site table by the
 * signal handler (SIGSEGV on Linux, SIGBUS on macOS) and sent to its
 * instruction's side exit, so the interpreter executes that instruction.
 * The instruction is translated with checks from then on (its bit in
 * jit_page.slow): what touched a device once tends to do it again.
 *
 * A faulting access has changed nothing, so the interpreter starts afresh.
 * For a store that straddles into a protected page, the architecture would
 * let the host write the part before the boundary first; Apple's cores
 * don't, and where the guest's store is allowed the interpreter writes the
 * same bytes again anyway.
 */

#define MAX_JITS 8

static struct jit *live_jits[MAX_JITS];
static struct sigaction old_segv, old_bus;

static uintptr_t context_pc(void *ctx)
{
	ucontext_t *uc = ctx;
#ifdef __APPLE__
	return (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
#else
	return (uintptr_t)uc->uc_mcontext.pc;
#endif
}

static void set_context_pc(void *ctx, const uint32_t *pc)
{
	ucontext_t *uc = ctx;
#ifdef __APPLE__
	__darwin_arm_thread_state64_set_pc_fptr(uc->uc_mcontext->__ss, (void *)(uintptr_t)pc);
#else
	uc->uc_mcontext.pc = (uintptr_t)pc;
#endif
}

static void fault_handler(int sig, siginfo_t *si, void *ctx)
{
	uintptr_t pc = context_pc(ctx), addr = (uintptr_t)si->si_addr;

	for (unsigned k = 0; k < MAX_JITS; k++) {
		struct jit *j = live_jits[k];
		if (!j || !j->view || pc < (uintptr_t)j->blocks_start || pc >= (uintptr_t)j->code_free ||
		    addr - (uintptr_t)j->view >= MEM_SPAN)
			continue;
		uint32_t at = (uint32_t)((const uint32_t *)pc - j->code), lo = 0, hi = j->nsites;
		while (lo < hi) {
			uint32_t mid = lo + (hi - lo) / 2;
			if (j->sites[mid].at < at)
				lo = mid + 1;
			else
				hi = mid;
		}
		if (lo < j->nsites && j->sites[lo].at == at) {
			set_context_pc(ctx, j->code + j->sites[lo].stub);
			j->st.fault_site = lo + 1;
			j->fault_ea = (uint32_t)(addr - (uintptr_t)j->view);
			j->faults++;
			return;
		}
	}
	/* not ours */
	const struct sigaction *old = sig == SIGBUS ? &old_bus : &old_segv;
	if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction) {
		old->sa_sigaction(sig, si, ctx);
	} else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
		old->sa_handler(sig);
	} else {
		sigaction(sig, old, NULL);      /* the fault happens again, as if we weren't here */
	}
}

/* The protection for host page h of the view */
static unsigned host_prot(const struct jit *j, uint64_t h)
{
	unsigned per = 1u << (j->host_shift - MEM_PAGE_SHIFT);
	const uint8_t *f = j->mem->pageflags + h * per;
	unsigned prot = PROT_READ | PROT_WRITE;
	for (unsigned k = 0; k < per; k++) {
		if (!(f[k] & MEM_R))
			return PROT_NONE;
		if ((f[k] & (MEM_W | MEM_CODE)) != MEM_W)
			prot = PROT_READ;
	}
	return prot;
}

static void fastmem_off(struct jit *j)
{
	j->fastmem = false;
	j->st.fastbase = j->mem->base;
	if (j->running) {
		j->flush_pending = true;
		j->st.code_changed = 1;
	} else {
		flush(j);
	}
}

/* Guest pages [page, page + count) changed flags: the view follows (called by struct mem) */
static void flags_changed(void *ctx, uint32_t page, uint32_t count)
{
	struct jit *j = ctx;
	if (!j->fastmem)
		return;
	unsigned shift = j->host_shift - MEM_PAGE_SHIFT;
	uint64_t h = page >> shift, end = ((uint64_t)page + count + (1u << shift) - 1) >> shift;
	while (h < end) {
		unsigned prot = host_prot(j, h);
		if (prot == j->host_prot[h]) {
			h++;
			continue;
		}
		uint64_t first = h;
		do
			j->host_prot[h++] = (uint8_t)prot;
		while (h < end && j->host_prot[h] != prot && host_prot(j, h) == prot);
		j->protects++;
		if (mprotect(j->view + (first << j->host_shift), (h - first) << j->host_shift, (int)prot)) {
			fprintf(stderr, "jit: cannot protect guest memory (%s): checking every access\n", strerror(errno));
			fastmem_off(j);
			return;
		}
	}
}

static void fastmem_init(struct jit *j)
{
	const char *mode = getenv("SYSHM_JIT_FASTMEM");
	long host_page = sysconf(_SC_PAGESIZE);
	if ((mode && !strcmp(mode, "0")) || host_page < (long)MEM_PAGE_SIZE ||
	    host_page > (long)MEM_SLACK || (host_page & (host_page - 1)))
		return;
	unsigned shift = (unsigned)__builtin_ctzl((unsigned long)host_page);
	uint8_t *view = mem_view(j->mem);
	void *sites = mmap(NULL, MAX_SITES * sizeof(struct jit_site), PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	uint8_t *prot = calloc(MEM_SPACE >> shift, 1);                  /* PROT_NONE */
	unsigned slot = 0;
	while (slot < MAX_JITS && live_jits[slot])
		slot++;
	if (!view || sites == MAP_FAILED || !prot || slot == MAX_JITS) {
		mem_view_free(view);
		if (sites != MAP_FAILED)
			munmap(sites, MAX_SITES * sizeof(struct jit_site));
		free(prot);
		return;
	}
	static bool installed;
	if (!installed) {
		struct sigaction sa = { 0 };
		sa.sa_sigaction = fault_handler;
		sa.sa_flags = SA_SIGINFO;
		sigemptyset(&sa.sa_mask);
		if (sigaction(SIGSEGV, &sa, &old_segv) || sigaction(SIGBUS, &sa, &old_bus)) {
			sigaction(SIGSEGV, &old_segv, NULL);
			mem_view_free(view);
			munmap(sites, MAX_SITES * sizeof(struct jit_site));
			free(prot);
			return;
		}
		installed = true;
	}
	live_jits[slot] = j;
	j->view = view;
	j->sites = sites;
	j->host_prot = prot;
	j->host_shift = shift;
	j->fastmem = true;
	j->st.fastbase = j->view;
	flags_changed(j, 0, (uint32_t)(MEM_SPACE >> MEM_PAGE_SHIFT));
}

static void fastmem_free(struct jit *j)
{
	for (unsigned k = 0; k < MAX_JITS; k++)
		if (live_jits[k] == j)
			live_jits[k] = NULL;
	mem_view_free(j->view);
	if (j->sites)
		munmap(j->sites, MAX_SITES * sizeof(struct jit_site));
	free(j->host_prot);
}

/*
 * A fastmem access faulted: its instruction gets checks, in a new
 * translation of its block; and if the page wasn't even readable (a
 * device's, perhaps), the way to the mmio hook too.
 */
static void slow_down(struct jit *j, uint32_t site)
{
	const struct jit_site *s = &j->sites[site];
	struct jit_page *pg = get_page(j, s->pc >> MEM_PAGE_SHIFT);
	unsigned w = (s->pc >> 2) & (PAGE_WORDS - 1);
	if (pg) {
		pg->slow[w / 64] |= 1ull << (w % 64);
		if (!(j->mem->pageflags[j->fault_ea >> MEM_PAGE_SHIFT] & MEM_R))
			pg->io[w / 64] |= 1ull << (w % 64);
	}
	drop_block(j, &j->blocks[s->block]);
	j->slowed++;
}

/* ---- entry, exit and dispatch ------------------------------------------ */

static void build_stubs(struct jit *j)
{
	static struct tx t;
	t.j = j;
	t.p = j->code;
	t.end = j->code_end;
	const unsigned st_off_mem = offsetof(struct jit_state, fastbase), st_off_flags = offsetof(struct jit_state, pageflags);
	const unsigned st_off_table = offsetof(struct jit_state, l1), st_off_rem = offsetof(struct jit_state, remaining);
	const unsigned st_off_hook = offsetof(struct jit_state, hook);

	/* enter(cpu x0, code x1, budget x2, state x3) */
	memcpy(&j->enter, &(void *){ t.p }, sizeof j->enter);   /* object to function pointer */
	emit(&t, a64_pair(false, A64_PRE, 29, 30, A64_SP, -96));
	emit(&t, a64_addsub_imm(1, 0, 0, 29, A64_SP, 0));
	emit(&t, a64_pair(false, A64_OFFSET, 19, 20, A64_SP, 16));
	emit(&t, a64_pair(false, A64_OFFSET, 21, 22, A64_SP, 32));
	emit(&t, a64_pair(false, A64_OFFSET, 23, 24, A64_SP, 48));
	emit(&t, a64_pair(false, A64_OFFSET, 25, 26, A64_SP, 64));
	emit(&t, a64_pair(false, A64_OFFSET, 27, 28, A64_SP, 80));
	emit(&t, a64_logic(1, A64_ORR, 0, R_CPU, A64_ZR, 0, A64_LSL, 0));
	emit(&t, a64_logic(1, A64_ORR, 0, R_BUDGET, A64_ZR, 2, A64_LSL, 0));
	emit(&t, a64_logic(1, A64_ORR, 0, R_STATE, A64_ZR, 3, A64_LSL, 0));
	emit(&t, a64_ldst_imm(3, false, R_BUDGET, R_STATE, (unsigned)offsetof(struct jit_state, entry_budget)));
	emit(&t, a64_ldst_imm(3, true, R_MEM, R_STATE, st_off_mem));
	emit(&t, a64_ldst_imm(3, true, R_FLAGS, R_STATE, st_off_flags));
	emit(&t, a64_ldst_imm(3, true, R_TABLE, R_STATE, st_off_table));
	emit(&t, a64_ldst_imm(3, true, R_CBASE, R_STATE, (unsigned)offsetof(struct jit_state, membase)));
	fixed_load(&t, NFIXED);
	emit(&t, a64_br(1));

	/* exit (kind in w1) */
	j->exit = t.p;
	fixed_store(&t, NFIXED);
	emit(&t, a64_ldst_imm(3, false, R_BUDGET, R_STATE, st_off_rem));
	mov_reg(&t, 0, 1);
	emit(&t, a64_pair(true, A64_OFFSET, 19, 20, A64_SP, 16));
	emit(&t, a64_pair(true, A64_OFFSET, 21, 22, A64_SP, 32));
	emit(&t, a64_pair(true, A64_OFFSET, 23, 24, A64_SP, 48));
	emit(&t, a64_pair(true, A64_OFFSET, 25, 26, A64_SP, 64));
	emit(&t, a64_pair(true, A64_OFFSET, 27, 28, A64_SP, 80));
	emit(&t, a64_pair(true, A64_POST, 29, 30, A64_SP, 96));
	emit(&t, a64_ret());

	/* side exit: the interpreter runs the instruction at w0 */
	j->side_exit = t.p;
	st32(&t, 0, OFF(pc));
	emit(&t, a64_movw(0, A64_MOVZ, 1, EXIT_INTERP, 0));
	branch_to(&t, j->exit);

	/* stop at the hook w0; or at w0, which has no block in the table */
	j->hook_exit = t.p;
	st32(&t, 0, OFF(pc));
	emit(&t, a64_movw(0, A64_MOVZ, 1, EXIT_HOOK, 0));
	branch_to(&t, j->exit);
	j->miss_exit = t.p;
	st32(&t, 0, OFF(pc));
	emit(&t, a64_movw(0, A64_MOVZ, 1, EXIT_MISS, 0));
	branch_to(&t, j->exit);

	/* continue at w0, through the block table (for branches into invalidated blocks) */
	j->lookup = t.p;
	emit(&t, a64_bitfield(0, A64_UBFM, 9, 0, MEM_PAGE_SHIFT, 31));
	emit(&t, a64_ldst_uxtw_scaled(3, true, 9, R_TABLE, 9));
	ubfx(&t, 10, 0, 2, MEM_PAGE_SHIFT - 2);
	emit(&t, a64_ldst_uxtw_scaled(3, true, 9, 9, 10));
	emit(&t, a64_br(9));

	/* exit to a known pc w0 from the B at x8: C finds the block there and points the B at it */
	j->link_branch = t.p;
	emit(&t, a64_ldst_imm(2, true, 9, R_STATE, st_off_hook));
	emit(&t, a64_addsub(0, 1, 1, A64_ZR, 0, 9, A64_LSL, 0));
	emit(&t, a64_bcond(A64_NE, 4 * 4));
	st32(&t, 0, OFF(pc));
	emit(&t, a64_movw(0, A64_MOVZ, 1, EXIT_HOOK, 0));
	branch_to(&t, j->exit);
	j->link = t.p;
	st32(&t, 0, OFF(pc));
	emit(&t, a64_ldst_imm(3, false, 8, R_STATE, (unsigned)offsetof(struct jit_state, link_site)));
	emit(&t, a64_movw(0, A64_MOVZ, 1, EXIT_LINK, 0));
	branch_to(&t, j->exit);

	/* mmio() for slow_ios(), called with BL (arguments from w1 on set), with every register stored */
	j->mmio_call = t.p;
	emit(&t, a64_ldst_imm(3, false, 30, R_STATE, (unsigned)offsetof(struct jit_state, mmio_return)));
	fixed_store(&t, NFIXED);
	emit(&t, a64_logic(1, A64_ORR, 0, 0, A64_ZR, R_CPU, A64_LSL, 0));
	emit(&t, a64_ldst_imm(3, true, 30, R_STATE, (unsigned)offsetof(struct jit_state, mmio)));
	emit(&t, a64_blr(30));
	fixed_load(&t, NFIXED);
	emit(&t, a64_ldst_imm(3, true, 30, R_STATE, (unsigned)offsetof(struct jit_state, mmio_return)));
	emit(&t, a64_ret());

	__builtin___clear_cache((char *)j->code, (char *)t.p);
	j->blocks_start = j->code_free = t.p;
}

static void prof_start(struct jit *j);

struct jit *jit_create(struct mem *mem)
{
	struct jit *j = calloc(1, sizeof *j);
	if (!j)
		return NULL;
#ifdef __APPLE__
	int jit_flag = MAP_JIT;         /* (Apple silicon maps nothing else writable and executable) */
#else
	int jit_flag = 0;
#endif
	void *code = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | jit_flag, -1, 0);
	void *pages = mmap(NULL, GUEST_PAGES * sizeof(struct jit_page *), PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	void *blocks = mmap(NULL, MAX_BLOCKS * sizeof(struct jit_block), PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	void *l1 = mmap(NULL, GUEST_PAGES * sizeof(void **), PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	void *l2 = mmap(NULL, ((size_t)MAX_CODE_PAGES + 1) * PAGE_WORDS * sizeof(void *), PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	j->code_pages = malloc(MAX_CODE_PAGES * sizeof *j->code_pages);
	j->page_pool = malloc(MAX_CODE_PAGES * sizeof *j->page_pool);
	if (code == MAP_FAILED || pages == MAP_FAILED || blocks == MAP_FAILED || l1 == MAP_FAILED ||
	    l2 == MAP_FAILED || !j->code_pages || !j->page_pool) {
		if (l1 != MAP_FAILED)
			munmap(l1, GUEST_PAGES * sizeof(void **));
		if (l2 != MAP_FAILED)
			munmap(l2, ((size_t)MAX_CODE_PAGES + 1) * PAGE_WORDS * sizeof(void *));
		if (code != MAP_FAILED)
			munmap(code, CODE_SIZE);
		if (pages != MAP_FAILED)
			munmap(pages, GUEST_PAGES * sizeof(struct jit_page *));
		if (blocks != MAP_FAILED)
			munmap(blocks, MAX_BLOCKS * sizeof(struct jit_block));
		free(j->code_pages);
		free(j->page_pool);
		free(j);
		return NULL;
	}
	j->mem = mem;
	j->code = code;
	j->code_end = j->code + CODE_SIZE / 4;
	j->pages = pages;
	j->blocks = blocks;
	j->l1 = l1;
	j->l2_empty = l2;
	j->l2_pool = j->l2_empty + PAGE_WORDS;
	j->st = (struct jit_state){
		.membase = mem->base,
		.pageflags = mem->pageflags,
		.l1 = j->l1,
		.step = step,
		.code_store = code_store,
		.mmio = mmio,
		.pages = j->pages,
		.fastbase = mem->base,
	};
	code_write_begin();
	build_stubs(j);
	code_write_end();
	for (unsigned w = 0; w < PAGE_WORDS; w++)
		j->l2_empty[w] = j->miss_exit;
	for (uint32_t pg = 0; pg < GUEST_PAGES; pg++)
		j->l1[pg] = j->l2_empty;
	j->linked_hook = PPC_NO_HOOK;
	j->match_pc = PPC_NO_HOOK;
	if (getenv("SYSHM_JIT_FAR"))            /* tests: blocks out of short branch range of the stubs */
		j->blocks_start += (CODE_SIZE - (8u << 20)) / 4;
	flush(j);
	j->flushes = 0;
	const char *hot = getenv("SYSHM_JIT_HOT");
	j->hot = hot ? (unsigned)strtoul(hot, NULL, 0) : 16;
	if (j->hot > 255)
		j->hot = 255;
	mem->code_changed = code_changed;
	mem->flags_changed = flags_changed;
	mem->code_ctx = j;
	fastmem_init(j);
	if (getenv("SYSHM_JIT_PROFILE"))
		prof_start(j);
	return j;
}

void jit_free(struct jit *j)
{
	if (!j)
		return;
	flush(j);
	j->mem->code_changed = NULL;
	j->mem->flags_changed = NULL;
	fastmem_free(j);
	munmap(j->code, CODE_SIZE);
	munmap(j->pages, GUEST_PAGES * sizeof(struct jit_page *));
	munmap(j->blocks, MAX_BLOCKS * sizeof(struct jit_block));
	munmap(j->l1, GUEST_PAGES * sizeof(void **));
	munmap(j->l2_empty, ((size_t)MAX_CODE_PAGES + 1) * PAGE_WORDS * sizeof(void *));
	free(j->code_pages);
	free(j->page_pool);
	free(j);
}

enum ppc_exit jit_run(struct jit *j, struct ppc_cpu *c, uint64_t budget)
{
	if (c->hook_pc != j->linked_hook || c->hook_match_pc != j->match_pc || c->hook_match_reg != j->match_reg ||
	    c->hook_match_value != j->match_value) {    /* links may lead past the old hooks */
		j->linked_hook = c->hook_pc;
		j->match_pc = c->hook_match_pc;
		j->match_reg = c->hook_match_reg;
		j->match_value = c->hook_match_value;
		flush(j);
	}
	j->st.hook = c->hook_pc;
	while (budget) {
		struct jit_block *e = find(j, c->pc);
		if (!e || e->ninsn > budget) {
			/* not hot yet (a block's worth) or not executable (ISI), or the end of the budget */
			uint64_t before = c->icount;
			enum ppc_exit ex = e ? ppc_interp(c, budget) :
					   ppc_interp_block(c, budget < MAX_BLOCK_INSNS ? budget : MAX_BLOCK_INSNS);
			budget -= c->icount - before;
			if (ex != PPC_EXIT_BUDGET)
				return ex;
			continue;
		}
		if (j->flush_pending) {
			j->flush_pending = false;
			flush(j);
			continue;
		}
		j->st.code_changed = 0;
		icache_sync(j);
		j->running = true;
		unsigned kind = j->enter(c, e->code, budget, &j->st);
		j->running = false;
		fp_settle(j, c);
		uint64_t retired = budget - j->st.remaining;
		c->icount += retired;
		c->tb += retired * c->tb_step;
		budget -= retired;
		j->exits[kind]++;
		switch (kind) {
		case EXIT_MISS:
		case EXIT_BUDGET:       /* the next block is longer than the budget: the loop interprets */
			break;
		case EXIT_INTERP: {
			if (j->st.fault_site) {
				slow_down(j, j->st.fault_site - 1);
				j->st.fault_site = 0;
			}
			if (!budget)
				break;
			if (c->pc == j->match_pc)       /* (its block's first instruction: the guard has been) */
				c->hook_match_skip = true;
			uint64_t before = c->icount;
			enum ppc_exit ex = ppc_interp(c, 1);
			budget -= c->icount - before;
			if (ex != PPC_EXIT_BUDGET)
				return ex;
			break;
		}
		case EXIT_HOOK:
			c->exit_insn = 0;
			return PPC_EXIT_HOOK;
		case EXIT_TRAP:
			return PPC_EXIT_TRAP;
		case EXIT_SC:
			return PPC_EXIT_SC;
		case EXIT_HOSTCALL:
			return PPC_EXIT_HOSTCALL;
		case EXIT_MMIO:
			return PPC_EXIT_MMIO;
		case EXIT_LINK: {
			uint32_t *site = j->st.link_site;
			unsigned long flushes = j->flushes;
			struct jit_block *next = find(j, c->pc);
			if (next && j->flushes == flushes && c->pc != j->linked_hook) {
				code_write_begin();
				*site = a64_b(false, (int32_t)(((const uint32_t *)next->code - site) * 4));
				code_write_end();
				icache_lines(j, site, site + 1, true);
				j->links++;
			}
			break;
		}
		}
	}
	return PPC_EXIT_BUDGET;
}

/* ---- sampling profiler (SYSHM_JIT_PROFILE=1) ------------------------------- */

#define PROF_MAX 1000000u

struct prof_sample {
	uint32_t guest_pc;              /* block, or 0 */
	uint32_t where;                 /* offset in the block, or host address bits */
	uintptr_t host_pc;
};

static struct {
	struct jit *j;
	unsigned long total, blocks, stubs, host, other;
	struct prof_sample *s;
	unsigned n;
	uintptr_t text_lo, text_hi;     /* syshm's own code */
} prof;

static void prof_signal(int sig, siginfo_t *si, void *ctx)
{
	(void)sig;
	(void)si;
	uintptr_t pc = context_pc(ctx);
	struct jit *j = prof.j;
	struct prof_sample smp = { 0, 0, pc };

	prof.total++;
	if (pc >= (uintptr_t)j->code && pc < (uintptr_t)j->blocks_start) {
		prof.stubs++;
		smp.where = (uint32_t)(pc - (uintptr_t)j->code);
	} else if (pc >= (uintptr_t)j->blocks_start && pc < (uintptr_t)j->code_free) {
		prof.blocks++;
		uint32_t lo = 0, hi = j->nblocks;       /* last block starting at or below pc */
		while (hi - lo > 1) {
			uint32_t mid = (lo + hi) / 2;
			if ((uintptr_t)j->blocks[mid].code <= pc)
				lo = mid;
			else
				hi = mid;
		}
		if (j->nblocks && j->blocks[lo].code) {
			smp.guest_pc = j->blocks[lo].pc;
			smp.where = (uint32_t)(pc - (uintptr_t)j->blocks[lo].code);
		}
	} else if (pc >= prof.text_lo && pc < prof.text_hi) {
		prof.host++;
	} else {
		prof.other++;
	}
	if (prof.n < PROF_MAX)
		prof.s[prof.n++] = smp;
}

static void prof_start(struct jit *j)
{
	prof.j = j;
	prof.s = malloc(PROF_MAX * sizeof *prof.s);
	if (!prof.s)
		return;
#ifdef __APPLE__
	unsigned long size = 0;
	prof.text_lo = (uintptr_t)getsegmentdata(&_mh_execute_header, "__TEXT", &size);
	prof.text_hi = prof.text_lo + size;
#else
	extern char __executable_start, etext;
	prof.text_lo = (uintptr_t)&__executable_start;
	prof.text_hi = (uintptr_t)&etext;
#endif
	struct sigaction sa = { 0 };
	sa.sa_sigaction = prof_signal;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPROF, &sa, NULL);
	struct itimerval it = { { 0, 250 }, { 0, 250 } };
	setitimer(ITIMER_PROF, &it, NULL);
}

static int cmp_sample(const void *x, const void *y)
{
	const struct prof_sample *a = x, *b = y;
	if (a->guest_pc != b->guest_pc)
		return a->guest_pc < b->guest_pc ? -1 : 1;
	return a->host_pc < b->host_pc ? -1 : a->host_pc > b->host_pc;
}

struct prof_count {
	uintptr_t key;
	unsigned long count;
};

static int cmp_count(const void *x, const void *y)
{
	const struct prof_count *a = x, *b = y;
	return a->count < b->count ? 1 : a->count > b->count ? -1 : 0;
}

static void prof_report(FILE *f)
{
	struct itimerval off = { { 0, 0 }, { 0, 0 } };
	setitimer(ITIMER_PROF, &off, NULL);
	if (!prof.total)
		return;
	fprintf(f, "profile: %lu samples: %.1f%% translated blocks, %.1f%% stubs, %.1f%% syshm, %.1f%% elsewhere\n",
		prof.total, 100.0 * prof.blocks / prof.total, 100.0 * prof.stubs / prof.total,
		100.0 * prof.host / prof.total, 100.0 * prof.other / prof.total);
	qsort(prof.s, prof.n, sizeof *prof.s, cmp_sample);
	struct prof_count *c = calloc(prof.n + 1, sizeof *c);
	if (!c)
		return;

	/* guest blocks */
	unsigned nc = 0;
	for (unsigned k = 0; k < prof.n; k++) {
		if (!prof.s[k].guest_pc)
			continue;
		if (!nc || c[nc - 1].key != prof.s[k].guest_pc)
			c[nc++] = (struct prof_count){ prof.s[k].guest_pc, 0 };
		c[nc - 1].count++;
	}
	qsort(c, nc, sizeof *c, cmp_count);
	fprintf(f, "profile: hottest blocks (guest pc, samples):");
	for (unsigned k = 0; k < nc && k < 40; k++)
		fprintf(f, "%s %08lX %lu", k % 8 ? "," : "\n ", (unsigned long)c[k].key, c[k].count);
	fprintf(f, "\n");

	/*
	 * host code outside the blocks: addresses relative to the executable
	 * (addr2line -f -e build/syshm; on macOS, atos -o build/syshm with 0x100000000 added)
	 */
	nc = 0;
	for (unsigned k = 0; k < prof.n; k++) {
		if (prof.s[k].guest_pc)
			continue;
		uintptr_t pc = prof.s[k].host_pc;
		uintptr_t key = pc >= (uintptr_t)prof.j->code && pc < (uintptr_t)prof.j->blocks_start ? 0x1 :
				pc >= prof.text_lo && pc < prof.text_hi ? pc - prof.text_lo : pc;
		if (!nc || c[nc - 1].key != key)
			c[nc++] = (struct prof_count){ key, 0 };
		c[nc - 1].count++;
	}
	qsort(c, nc, sizeof *c, cmp_count);
	{
		unsigned long st[8] = { 0 };
		const uint32_t *marks[] = { prof.j->exit, prof.j->side_exit, prof.j->hook_exit, prof.j->miss_exit,
					    prof.j->link_branch, prof.j->blocks_start };
		static const char *const names[] = { "enter", "exit", "side exit", "hook", "miss", "link" };
		for (unsigned k = 0; k < prof.n; k++) {
			uintptr_t pc = prof.s[k].host_pc;
			if (prof.s[k].guest_pc || pc < (uintptr_t)prof.j->code || pc >= (uintptr_t)prof.j->blocks_start)
				continue;
			unsigned m = 0;
			while (m < 5 && pc >= (uintptr_t)marks[m])
				m++;
			st[m]++;
		}
		fprintf(f, "profile: stubs:");
		for (unsigned m = 0; m < 6; m++)
			fprintf(f, " %s %lu", names[m], st[m]);
		fprintf(f, "\n");
	}
	fprintf(f, "profile: host addresses (1 = stubs):");
	for (unsigned k = 0; k < nc && k < 60; k++)
		fprintf(f, "%s %lx %lu", k % 8 ? "," : "\n ", (unsigned long)c[k].key, c[k].count);
	fprintf(f, "\n");
	fprintf(f, "profile: in libraries:");
	for (unsigned k = 0, shown = 0; k < nc && shown < 8; k++) {
		Dl_info info;
		if (c[k].key < 0x100000000ull || !dladdr((void *)c[k].key, &info) || !info.dli_sname)
			continue;
		fprintf(f, " %s %lu", info.dli_sname, c[k].count);
		shown++;
	}
	fprintf(f, "\n");
	free(c);
}

void jit_print_stats(const struct jit *j, FILE *f)
{
	if (prof.j == j)
		prof_report(f);
	fprintf(f, "jit: %lu blocks (%llu instructions, %zu KB of code), %lu links, %lu invalidations, %lu flushes; "
		"exits: %lu miss, %lu budget, %lu interpreter, %lu hook, %lu link, %lu kernel, %lu device\n",
		j->translated, (unsigned long long)j->translated_insns,
		(size_t)(j->code_free - j->blocks_start) * 4 / 1024, j->links, j->invalidations, j->flushes,
		j->exits[EXIT_MISS], j->exits[EXIT_BUDGET], j->exits[EXIT_INTERP], j->exits[EXIT_HOOK],
		j->exits[EXIT_LINK], j->exits[EXIT_TRAP] + j->exits[EXIT_SC] + j->exits[EXIT_HOSTCALL],
		j->exits[EXIT_MMIO]);
	if (j->view)
		fprintf(f, "jit: fastmem%s: %lu faults, %lu instructions checked since, %lu protection changes\n",
			j->fastmem ? "" : " (off)", j->faults, j->slowed, j->protects);
}

#else   /* no JIT for this host */

struct jit *jit_create(struct mem *mem)
{
	(void)mem;
	return NULL;
}

void jit_free(struct jit *j)
{
	(void)j;
}

enum ppc_exit jit_run(struct jit *j, struct ppc_cpu *c, uint64_t budget)
{
	(void)j;
	return ppc_interp(c, budget);
}

void jit_print_stats(const struct jit *j, FILE *f)
{
	(void)j;
	(void)f;
}

#endif
