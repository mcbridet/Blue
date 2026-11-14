#include "ppc.h"

#include <string.h>

#include "jit.h"
#include "ppc_fp.h"

#define OPCD(i) ((i) >> 26)
#define RD(i) (((i) >> 21) & 31)
#define RS(i) RD(i)
#define RA(i) (((i) >> 16) & 31)
#define RB(i) (((i) >> 11) & 31)
#define RC(i) ((i) & 1)
#define OE(i) (((i) >> 10) & 1)
#define SIMM(i) ((uint32_t)(int32_t)(int16_t)((i) & 0xFFFF))
#define UIMM(i) ((i) & 0xFFFFu)
#define XO10(i) (((i) >> 1) & 0x3FF)
#define XO9(i) (((i) >> 1) & 0x1FF)
#define SPRN(i) ((((i) >> 16) & 0x1F) | (((i) >> 6) & 0x3E0))
#define CRFD(i) (((i) >> 23) & 7)

static inline uint32_t rotl32(uint32_t v, unsigned n)
{
	n &= 31;
	return n ? (v << n) | (v >> (32 - n)) : v;
}

static inline uint32_t sra32(uint32_t v, unsigned n)
{
	if (n == 0)
		return v;
	uint32_t r = v >> n;
	if (v & 0x80000000u)
		r |= ~(0xFFFFFFFFu >> n);
	return r;
}

/* Mask with bits mb..me set (bit 0 = MSB), wrapping if mb > me. */
static inline uint32_t mask32(unsigned mb, unsigned me)
{
	uint32_t a = 0xFFFFFFFFu >> mb;
	uint32_t b = 0xFFFFFFFFu << (31 - me);
	return mb <= me ? (a & b) : (a | b);
}

static inline void set_crf(struct ppc_cpu *c, unsigned field, uint32_t v)
{
	unsigned sh = 28 - 4 * field;
	c->cr = (c->cr & ~(0xFu << sh)) | ((v & 0xFu) << sh);
}

static inline uint32_t so_bit(const struct ppc_cpu *c)
{
	return (c->xer & XER_SO) ? CR_SO : 0;
}

static inline uint32_t cmp_signed(const struct ppc_cpu *c, int32_t a, int32_t b)
{
	return (a < b ? CR_LT : a > b ? CR_GT : CR_EQ) | so_bit(c);
}

static inline uint32_t cmp_unsigned(const struct ppc_cpu *c, uint32_t a, uint32_t b)
{
	return (a < b ? CR_LT : a > b ? CR_GT : CR_EQ) | so_bit(c);
}

static inline void record(struct ppc_cpu *c, uint32_t r)
{
	set_crf(c, 0, cmp_signed(c, (int32_t)r, 0));
}

static inline void set_ca(struct ppc_cpu *c, bool ca)
{
	if (ca)
		c->xer |= XER_CA;
	else
		c->xer &= ~XER_CA;
}

static inline void set_ov(struct ppc_cpu *c, bool ov)
{
	if (ov)
		c->xer |= XER_OV | XER_SO;
	else
		c->xer &= ~XER_OV;
}

static inline uint32_t carry_in(const struct ppc_cpu *c)
{
	return (c->xer & XER_CA) ? 1 : 0;
}

/* a + b + cin, optionally updating XER[CA] and XER[OV,SO]. */
static inline uint32_t add_ext(struct ppc_cpu *c, uint32_t a, uint32_t b, uint32_t cin, bool want_ca, bool want_ov)
{
	uint64_t s = (uint64_t)a + b + cin;
	uint32_t r = (uint32_t)s;

	if (want_ca)
		set_ca(c, s >> 32);
	if (want_ov)
		set_ov(c, ((a ^ r) & (b ^ r)) >> 31);
	return r;
}

static inline bool branch_ok(struct ppc_cpu *c, unsigned bo, unsigned bi, bool use_ctr)
{
	bool ctr_ok = true;

	if (use_ctr && !(bo & 4)) {
		c->ctr--;
		ctr_ok = (c->ctr != 0) != ((bo & 2) != 0);
	}
	bool cond_ok = (bo & 16) || (((c->cr >> (31 - bi)) & 1) == ((bo >> 3) & 1));
	return ctr_ok && cond_ok;
}

static inline bool trap_taken(unsigned to, uint32_t a, uint32_t b)
{
	int32_t sa = (int32_t)a, sb = (int32_t)b;

	return ((to & 16) && sa < sb) || ((to & 8) && sa > sb) || ((to & 4) && a == b) ||
	       ((to & 2) && a < b) || ((to & 1) && a > b);
}

/* XO-form integer arithmetic under primary opcode 31. Returns false if `i` isn't one. */
static bool exec_arith(struct ppc_cpu *c, uint32_t i)
{
	uint32_t a = c->gpr[RA(i)], b = c->gpr[RB(i)], r;
	bool oe = OE(i);

	switch (XO9(i)) {
	case 266: r = add_ext(c, a, b, 0, false, oe); break;                          /* add */
	case 10:  r = add_ext(c, a, b, 0, true, oe); break;                           /* addc */
	case 138: r = add_ext(c, a, b, carry_in(c), true, oe); break;                 /* adde */
	case 202: r = add_ext(c, a, 0, carry_in(c), true, oe); break;                 /* addze */
	case 234: r = add_ext(c, a, 0xFFFFFFFFu, carry_in(c), true, oe); break;       /* addme */
	case 40:  r = add_ext(c, ~a, b, 1, false, oe); break;                         /* subf */
	case 8:   r = add_ext(c, ~a, b, 1, true, oe); break;                          /* subfc */
	case 136: r = add_ext(c, ~a, b, carry_in(c), true, oe); break;                /* subfe */
	case 200: r = add_ext(c, ~a, 0, carry_in(c), true, oe); break;                /* subfze */
	case 232: r = add_ext(c, ~a, 0xFFFFFFFFu, carry_in(c), true, oe); break;      /* subfme */
	case 104: r = add_ext(c, ~a, 0, 1, false, oe); break;                         /* neg */
	case 235: {                                                                   /* mullw */
		int64_t p = (int64_t)(int32_t)a * (int32_t)b;
		r = (uint32_t)p;
		if (oe)
			set_ov(c, p != (int64_t)(int32_t)r);
		break;
	}
	case 75:                                                                      /* mulhw */
		r = (uint32_t)((uint64_t)((int64_t)(int32_t)a * (int32_t)b) >> 32);
		break;
	case 11:                                                                      /* mulhwu */
		r = (uint32_t)(((uint64_t)a * b) >> 32);
		break;
	case 491: {                                                                   /* divw */
		bool bad = b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu);
		/* The result is architecturally undefined for bad divisors; keep rA. */
		r = bad ? a : (uint32_t)((int32_t)a / (int32_t)b);
		if (oe)
			set_ov(c, bad);
		break;
	}
	case 459:                                                                     /* divwu */
		r = b ? a / b : a;
		if (oe)
			set_ov(c, b == 0);
		break;
	default:
		return false;
	}

	c->gpr[RD(i)] = r;
	if (RC(i))
		record(c, r);
	return true;
}

/* Whether the match hook stops at its pc now (see ppc.h) */
static bool match_hook(struct ppc_cpu *c)
{
	if (c->hook_match_skip) {
		c->hook_match_skip = false;
		return false;
	}
	uint32_t a = c->gpr[c->hook_match_reg & 31];
	if (!mem_ok(c->mem, a, 2, MEM_R) || mem_r16(c->mem, a) != c->hook_match_value)
		return false;
	c->hook_match_skip = true;
	return true;
}

static inline __attribute__((always_inline)) enum ppc_exit interp(struct ppc_cpu *c, uint64_t budget, bool block)
{
	struct mem *m = c->mem;
	uint32_t *g = c->gpr;
	enum ppc_exit ex = PPC_EXIT_BUDGET;
	uint32_t pc = c->pc;
	uint64_t done = 0;                      /* instructions retired in this call */
	uint32_t code_page = 0xFFFFFFFFu;       /* page known to be executable */
	const bool slow = c->trace || c->break_lo != c->break_hi || c->break_pc != PPC_NO_HOOK;
	const uint32_t hook = c->hook_pc, match = c->hook_match_pc;

	for (; done < budget; done++) {
		/* page flags can't change while we run, so check each code page once */
		if (pc >> MEM_PAGE_SHIFT != code_page) {
			if (!mem_ok(m, pc, 4, MEM_X)) {
				c->fault_addr = pc;
				c->fault_write = false;
				c->exit_insn = 0;
				ex = PPC_EXIT_ISI;
				break;
			}
			code_page = pc >> MEM_PAGE_SHIFT;
		}
		if (pc == match && match_hook(c)) {
			c->exit_insn = 0;
			ex = PPC_EXIT_HOOK;
			break;
		}
		uint32_t i = mem_r32(m, pc);
		if (slow) {
			if ((pc - c->break_lo < c->break_hi - c->break_lo || pc == c->break_pc) && !c->break_skip) {
				c->exit_insn = 0;
				ex = PPC_EXIT_BREAK;
				break;
			}
			c->break_skip = false;
			if (c->trace && c->trace_hi) {
				if (pc - c->trace_lo < c->trace_hi - c->trace_lo && (!c->trace_insn || i == c->trace_insn))
					fprintf(c->trace, "%llu %08x  %08x  r3=%08x r4=%08x r5=%08x lr=%08x\n",
						(unsigned long long)(c->icount + done), pc, i, g[3], g[4], g[5], c->lr);
			} else if (c->trace) {
				if (c->trace_reg >= 0)
					fprintf(c->trace, "%08x  %08x  r%d=%08x\n", pc, i, c->trace_reg, g[c->trace_reg]);
				else
					fprintf(c->trace, "%08x  %08x\n", pc, i);
			}
		}

		uint32_t npc = pc + 4;
#define EXIT(e) do { ex = (e); goto stop; } while (0)
/* after a branch: stop with the branch retired if it went to the hook */
#define CHECK_HOOK() do { if (npc == hook) { pc = npc; done++; i = 0; EXIT(PPC_EXIT_HOOK); } } while (0)
#define IN_PLACE 1
#define UNRETIRED() done--              /* (the loop counts it back) */
#include "ppc_exec.inc"
#undef EXIT
#undef CHECK_HOOK
#undef IN_PLACE
#undef UNRETIRED
		if (block && (OPCD(i) == 16 || OPCD(i) == 18 || (OPCD(i) == 19 && (XO10(i) == 16 || XO10(i) == 528)) ||
			      !((pc + 4) & (MEM_PAGE_SIZE - 1)))) {
			pc = npc;
			done++;
			break;
		}
		pc = npc;
		continue;
	stop:
		c->exit_insn = i;
		break;
	}

	c->pc = pc;
	c->icount += done;
	c->tb += done * c->tb_step;
	return ex;
}

enum ppc_exit ppc_interp(struct ppc_cpu *c, uint64_t budget)
{
	return interp(c, budget, false);
}

enum ppc_exit ppc_interp_block(struct ppc_cpu *c, uint64_t budget)
{
	return interp(c, budget, true);
}

bool ppc_step(struct ppc_cpu *c, uint32_t i, uint32_t pc)
{
	struct mem *m = c->mem;
	uint32_t *g = c->gpr;
	uint32_t npc = pc + 4;
	const uint64_t done = 0;
#define EXIT(e) return false
#define CHECK_HOOK() do { } while (0)
#define IN_PLACE 0                      /* (generated code answers those itself) */
#define UNRETIRED() do { } while (0)
#include "ppc_exec.inc"
#undef EXIT
#undef CHECK_HOOK
#undef IN_PLACE
#undef UNRETIRED
	(void)npc;
	return true;
}

enum ppc_exit ppc_run(struct ppc_cpu *c, uint64_t budget)
{
	if (c->jit && !c->trace && c->break_lo == c->break_hi && c->break_pc == PPC_NO_HOOK)
		return jit_run(c->jit, c, budget);
	return ppc_interp(c, budget);
}

void ppc_init(struct ppc_cpu *cpu, struct mem *mem)
{
	memset(cpu, 0, sizeof *cpu);
	cpu->mem = mem;
	cpu->trace_reg = -1;
	cpu->tb_step = 1;
	cpu->hook_pc = PPC_NO_HOOK;
	cpu->hook_match_pc = PPC_NO_HOOK;
	cpu->break_pc = PPC_NO_HOOK;
}

const char *ppc_exit_name(enum ppc_exit e)
{
	switch (e) {
	case PPC_EXIT_BUDGET:   return "budget";
	case PPC_EXIT_SC:       return "sc";
	case PPC_EXIT_TRAP:     return "trap";
	case PPC_EXIT_PRIV:     return "privileged";
	case PPC_EXIT_ILLEGAL:  return "illegal";
	case PPC_EXIT_UNIMPL:   return "unimplemented";
	case PPC_EXIT_DSI:      return "dsi";
	case PPC_EXIT_ISI:      return "isi";
	case PPC_EXIT_HOSTCALL: return "hostcall";
	case PPC_EXIT_BREAK:    return "break";
	case PPC_EXIT_HOOK:     return "hook";
	case PPC_EXIT_NONE:     return "none";
	}
	return "?";
}

void ppc_dump(const struct ppc_cpu *c, FILE *f)
{
	fprintf(f, "pc %08x  lr %08x  ctr %08x  cr %08x  xer %08x  insns %llu\n",
		c->pc, c->lr, c->ctr, c->cr, c->xer, (unsigned long long)c->icount);
	for (int r = 0; r < 32; r++)
		fprintf(f, "r%-2d %08x%s", r, c->gpr[r], (r % 8 == 7) ? "\n" : "  ");
}
