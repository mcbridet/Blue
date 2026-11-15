/*
 * Differential test of the JIT: random programs run once by the interpreter
 * and once by the JIT must end in exactly the same state (registers, exit
 * reason, instruction count, memory), whatever the budget, and with hooks. Environment:
 * BLUE_JIT_PROGRAMS (default 3000), BLUE_JIT_SEED (first program);
 * BLUE_TEST_JIT=1 fails, rather than skips, without a JIT.
 *
 * Memory: code and data in RAM, and a read-only page whose neighbours in
 * the same 16 KB are unmapped, so that fastmem faults (genuinely, or just
 * because of the host page size) for loads and stores of every kind, and
 * for stores into the code. And an unmapped page with a device behind it,
 * through the mmio hook (fake_device() below), which must see the same
 * accesses, at the same instruction counts and times, either way.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/a64.h"
#include "../src/jit.h"
#include "../src/mem.h"
#include "../src/mmio.h"
#include "../src/ppc.h"
#include "../src/ppc_fp.h"
#include "../src/x86.h"
#include "ppcasm.h"
#include "test.h"

#define CODE 0x00010000u
#define CODE_MAX 96u                    /* instructions */
#define DATA 0x00040000u                /* 64 KB, read/write */
#define DATA_SIZE 0x10000u
#define RO_PAGE 0x00050000u             /* readable only */
#define IO_PAGE 0x00060000u             /* a device's registers (unmapped) */
#define BASE_A 0x00044000u              /* r31 */
#define BASE_B 0x00048000u              /* r29 (update forms) */
#define BASE_END (DATA + DATA_SIZE - 0x1000u)   /* r31 sometimes: crossing into RO_PAGE */

static uint64_t rng;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)(rng >> 16);
}

static unsigned pick(unsigned n) { return rnd() % n; }

static uint32_t interesting(void)
{
	static const uint32_t v[] = { 0, 1, 2, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFF,
				      0x8000, 0xFFFF8000u, 31, 32, 33, 63, 0x12345678u, 0xFFFFFFFEu };
	return pick(3) ? rnd() << 16 ^ rnd() : v[pick(sizeof v / sizeof v[0])];
}

/* FP register contents: mostly ordinary numbers (which generated code handles itself), sometimes anything */
static uint64_t fp_value(void)
{
	uint64_t sign = (uint64_t)pick(2) << 63;
	switch (pick(8)) {
	case 0:                                         /* any bits */
		return (uint64_t)interesting() << 32 | interesting();
	case 1:                                         /* zero */
		return sign;
	case 2:                                         /* near the native path's limits */
		return sign | (uint64_t)((pick(2) ? 123 : 2022) + pick(5) - 2) << 52 | (uint64_t)rnd() << 20;
	case 3:                                         /* single precision (some denormal, infinite, NaN) */
		return ppc_fp_single_to_double(pick(2) ? rnd() << 16 ^ rnd() : (rnd() & 0x807FFFFFu) | pick(2) * 0x7F800000u);
	case 4:                                         /* small integers, halves */
		return sign | (uint64_t)(1023 + pick(12)) << 52 | (uint64_t)pick(8) << 49;
	default:                                        /* ordinary magnitudes */
		return sign | (uint64_t)(1023 + pick(64) - 32) << 52 | ((uint64_t)rnd() << 32 | rnd()) >> 12;
	}
}

static unsigned rdst(void) { return pick(29); }         /* r0-r28: keep the base registers */
static unsigned rsrc(void) { return pick(32); }

static uint32_t d_form(unsigned op, unsigned rt, unsigned ra, uint32_t imm)
{
	return op << 26 | rt << 21 | ra << 16 | (imm & 0xFFFF);
}

static uint32_t x_form(unsigned rt, unsigned ra, unsigned rb, unsigned xo, unsigned rc)
{
	return 31u << 26 | rt << 21 | ra << 16 | rb << 11 | xo << 1 | rc;
}

static bool more_jumps;         /* plain B, which the JIT follows into ROM-like code */
static bool fp_heavy;           /* mostly floating point, as MathLib's code is */

/* An FPSCR instruction: mffs, mtfsf (often all fields, as MathLib saves and restores it), mtfsfi, mtfsb1/0, mcrfs */
static uint32_t gen_fpscr(unsigned rc)
{
	switch (pick(6)) {
	case 0:  return 63u << 26 | pick(32) << 21 | 583 << 1 | rc;
	case 1:  return 63u << 26 | (pick(2) ? 0xFFu : pick(4) ? pick(256) : 0x18u >> pick(2)) << 17 | pick(32) << 11 |
			711 << 1 | rc;
	case 2:  return 63u << 26 | pick(8) << 23 | pick(16) << 12 | 134 << 1 | rc;
	case 3:  return 63u << 26 | (pick(2) ? pick(32) : 13 + pick(7)) << 21 | (pick(2) ? 38 : 70) << 1 | rc;
	default: return 63u << 26 | pick(8) << 23 | (pick(2) ? pick(8) : 3 + pick(2)) << 18 | 64 << 1;
	}
}

static uint32_t gen_insn(unsigned k, unsigned n)
{
	static const unsigned arith[] = { 266, 10, 138, 202, 234, 40, 8, 136, 200, 232, 104, 235, 75, 11, 491, 459 };
	static const unsigned logic[] = { 28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 26, 922, 954 };
	static const unsigned crops[] = { 33, 129, 193, 225, 257, 289, 417, 449 };
	static const unsigned bos[] = { 4, 12, 16, 18, 20, 0, 2, 8, 10, 5, 13 };
	static const unsigned sprs[] = { 1, 8, 9, 256 };
	static const unsigned ld_ops[] = { 32, 33, 34, 35, 40, 41, 42, 43, 36, 37, 38, 39, 44, 45, 50, 51, 54, 55, 48, 52,
					   49, 53 };
	static const unsigned x_mem[] = { 23, 55, 87, 119, 279, 311, 343, 375, 151, 183, 215, 247, 407, 439,
					  599, 631, 727, 759, 534, 790, 662, 918, 20, 150, 1014, 535, 663, 567, 695 };
	unsigned c = fp_heavy && pick(4) ? 82 + pick(6) : pick(100);

	if (more_jumps && pick(8) == 0) {
		int32_t target = (int32_t)pick(n + 4) - (int32_t)k;
		return 18u << 26 | ((uint32_t)((target ? target : 1) * 4) & 0x03FFFFFC);
	}
	if (c < 14) {                                                   /* immediate forms */
		static const unsigned ops[] = { 14, 15, 12, 13, 8, 7, 24, 25, 26, 27, 28, 29, 10, 11 };
		unsigned op = ops[pick(sizeof ops / sizeof ops[0])];
		unsigned rt = op == 10 || op == 11 ? pick(8) << 2 : rdst();
		return d_form(op, rt, (op >= 24 && op <= 29) ? rdst() : rsrc(), interesting());
	}
	if (c < 28)                                                     /* XO arithmetic */
		return 31u << 26 | rdst() << 21 | rsrc() << 16 | rsrc() << 11 | pick(2) << 10 |
		       arith[pick(sizeof arith / sizeof arith[0])] << 1 | pick(2);
	if (c < 36) {                                                   /* logical, shifts */
		unsigned xo = logic[pick(sizeof logic / sizeof logic[0])];
		return x_form(rsrc(), rdst(), rsrc(), xo, pick(2));
	}
	if (c < 38)                                                     /* srawi */
		return x_form(rsrc(), rdst(), pick(32), 824, pick(2));
	if (c < 48) {                                                   /* rotates */
		unsigned op = (unsigned[]){ 20, 21, 23 }[pick(3)];
		return op << 26 | rsrc() << 21 | rdst() << 16 | pick(32) << 11 | pick(32) << 6 | pick(32) << 1 | pick(2);
	}
	if (c < 52)                                                     /* cmp, cmpl */
		return x_form(pick(8) << 2, rsrc(), rsrc(), pick(2) ? 0 : 32, 0);
	if (c < 57) {                                                   /* CR operations */
		switch (pick(5)) {
		case 0:  return 19u << 26 | pick(8) << 23 | pick(8) << 18;                     /* mcrf */
		case 1:  return x_form(rdst(), 0, 0, 19, 0);                                    /* mfcr */
		case 2:  return 31u << 26 | rsrc() << 21 | pick(256) << 12 | 144 << 1;          /* mtcrf */
		case 3:  return x_form(pick(8) << 2, 0, 0, 512, 0);                             /* mcrxr */
		default: {
			unsigned bt = pick(32);
			if (pick(4) == 0)
				return 19u << 26 | bt << 21 | bt << 16 | bt << 11 | (pick(2) ? 193 : 289) << 1;
			return 19u << 26 | bt << 21 | pick(32) << 16 | pick(32) << 11 | crops[pick(8)] << 1;
		}
		}
	}
	if (c < 61) {                                                   /* mtspr, mfspr */
		unsigned spr = sprs[pick(4)];
		unsigned field = (spr & 0x1F) << 5 | spr >> 5;
		return 31u << 26 | (pick(2) ? rsrc() : rdst()) << 21 | field << 11 | (pick(2) ? 467 : 339) << 1;
	}
	if (c < 74) {                                                   /* D-form loads and stores */
		unsigned op = ld_ops[pick(sizeof ld_ops / sizeof ld_ops[0])];
		bool update = op & 1;
		unsigned ra = update ? (pick(4) ? 29 : 28) : (pick(8) ? 31 : (pick(2) ? 28 : rsrc()));
		unsigned rt = (op >= 48) ? pick(32) : rdst();
		int32_t d = (int32_t)(pick(4096)) - 2048;
		if (pick(16) == 0)
			d = (int32_t)(0x1000 - (pick(8) + 1)) - (int32_t)(BASE_A & 0xFFF);    /* page crossing */
		return d_form(op, rt, ra, (uint32_t)d);
	}
	if (c < 80) {                                                   /* indexed loads and stores */
		unsigned xo = x_mem[pick(sizeof x_mem / sizeof x_mem[0])];
		bool update = xo == 55 || xo == 119 || xo == 311 || xo == 375 || xo == 183 || xo == 247 ||
			      xo == 439 || xo == 631 || xo == 759 || xo == 567 || xo == 695;
		unsigned rt = (xo >= 535 && xo <= 759 && xo != 662 && xo != 534) ? pick(32) : rdst();
		return x_form(rt, update ? (pick(4) ? 29 : 28) : 31, 30, xo, xo == 150);
	}
	if (c < 82)                                                     /* lmw, stmw */
		return d_form(pick(2) ? 46 : 47, 20 + pick(12), 31, (uint32_t)(pick(256) * 4));
	if (c < 88) {                                                   /* floating point */
		/* fadd fsub fmul fdiv fmadd fmsub fnmadd fnmsub fsqrt fres frsqrte fsel */
		static const unsigned a_ops[] = { 21, 20, 25, 18, 29, 28, 31, 30, 21, 25, 29, 22, 24, 26, 23 };
		static const unsigned x_ops[] = { 72, 40, 264, 136, 12, 15, 14, 12, 15, 583, 711, 134, 38, 70, 64 };
		unsigned rc = pick(4) == 0;
		if (fp_heavy && pick(3) == 0)
			return gen_fpscr(rc);
		switch (pick(5)) {
		case 0: case 1:
			return (pick(2) ? 63u : 59u) << 26 | pick(32) << 21 | pick(32) << 16 | pick(32) << 11 | pick(32) << 6 |
			       a_ops[pick(sizeof a_ops / sizeof a_ops[0])] << 1 | rc;
		case 2: case 3: {
			unsigned xo = x_ops[pick(sizeof x_ops / sizeof x_ops[0])];
			uint32_t i = 63u << 26 | pick(32) << 21 | pick(32) << 11 | xo << 1 | rc;
			if (xo == 711)                                          /* mtfsf */
				i = 63u << 26 | pick(256) << 17 | pick(32) << 11 | xo << 1 | rc;
			else if (xo == 134)                                     /* mtfsfi */
				i = 63u << 26 | pick(8) << 23 | pick(16) << 12 | xo << 1 | rc;
			else if (xo == 64)                                      /* mcrfs */
				i = 63u << 26 | pick(8) << 23 | pick(8) << 18 | xo << 1;
			return i;
		}
		default: return 63u << 26 | pick(8) << 23 | pick(32) << 16 | pick(32) << 11 | (pick(2) ? 0 : 32) << 1;
		}
	}
	if (c < 96) {                                                   /* branches */
		int32_t target = (int32_t)pick(n + 4) - (int32_t)k;
		if (target == 0)
			target = 1;
		switch (pick(5)) {
		case 0:  return 18u << 26 | ((uint32_t)(target * 4) & 0x03FFFFFC) | pick(2);
		case 1: case 2:
			return 16u << 26 | bos[pick(sizeof bos / sizeof bos[0])] << 21 | pick(32) << 16 |
			       ((uint32_t)(target * 4) & 0xFFFC) | pick(2);
		case 3:  return 19u << 26 | bos[pick(sizeof bos / sizeof bos[0])] << 21 | pick(32) << 16 | 16 << 1 | pick(2);
		default: return 19u << 26 | (bos[pick(sizeof bos / sizeof bos[0])] | 4) << 21 | pick(32) << 16 | 528 << 1 |
				pick(2);
		}
	}
	switch (pick(6)) {                                              /* kernel-bound and odd ones */
	case 0:  return 3u << 26 | pick(32) << 21 | rsrc() << 16 | (interesting() & 0xFFFF);  /* twi */
	case 1:  return x_form(pick(32), rsrc(), rsrc(), 4, 0);                               /* tw */
	case 2:  return x_form(rdst(), 12, 8, 371, 0);                                        /* mftb */
	case 3:  return 17u << 26 | 2;                                                        /* sc */
	case 4:  return 0x0FFF0000u | pick(4);                  /* a kernel call (see fuzz_kcall()) */
	default: return rnd() << 16 ^ rnd();                                                   /* anything */
	}
}

static unsigned fuzz_insns;     /* the program's */

/*
 * The kernel's calls in place for random programs: odd ones decline;
 * others move registers about, change the FPSCR, store and load data, and
 * go on somewhere in the program, as a context switch would (past kernel
 * calls there, which would retire nothing forever).
 */
static bool fuzz_kcall(struct ppc_cpu *c, unsigned n)
{
	uint32_t *g = c->gpr;
	if (n & 1)
		return false;
	uint32_t to = CODE + 4 * ((g[5]++ + n) % (fuzz_insns + 1));
	while ((mem_r32(c->mem, to) & 0xFFFF0000u) == 0x0FFF0000u || mem_r32(c->mem, to) == 0x44000002u)
		to += 4;
	mem_w32(c->mem, DATA + 0x100 + 4 * (g[7] & 15), g[3] ^ g[24]);
	uint32_t t = g[3];
	g[3] = g[4] + n;
	g[4] = g[27] ^ c->cr;
	g[27] = t;
	g[8] = mem_r32(c->mem, DATA + 0x140);
	c->cr = c->cr << 4 | c->xer >> 28;
	c->lr = c->ctr;
	c->ctr = g[1];
	c->xer ^= 0x80000000u;
	c->fpr[1] = c->fpr[2];
	ppc_fp_set_fpscr(c, c->fpscr ^ (g[5] & 1));
	c->resv_valid = false;
	c->pc = to;
	return true;
}

/*
 * The device behind IO_PAGE: what its registers read follows from all it
 * has seen, the accesses and the instruction counts and times of them
 * (mixed into `state`). As the kernel's hook would, it takes an access or
 * not depending on its address and its state, and not on the run's last
 * instruction; and having taken one, it sometimes changes CR (as posting
 * an interrupt does), data, the code (as DMA might) or what may be done in
 * place, or stops the run.
 */
struct device {
	struct mmio_bus bus;
	struct mem *mem;
	uint64_t state;
	unsigned long accesses;
	uint64_t until;                 /* the run's end */
};

struct side {
	struct mem mem;
	struct ppc_cpu cpu;
	struct device dev;
};

static uint64_t mix(uint64_t h, uint64_t v)
{
	h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
	return h * 0xBF58476D1CE4E5B9ull;
}

static uint32_t device_read(void *ctx, uint32_t off, unsigned size)
{
	struct device *d = ctx;
	d->state = mix(d->state, off << 4 | size);
	return (uint32_t)(d->state >> 13) & (uint32_t)(0xFFFFFFFFull >> (32 - 8 * size));
}

static void device_write(void *ctx, uint32_t off, unsigned size, uint32_t v)
{
	struct device *d = ctx;
	d->state = mix(d->state, (uint64_t)v << 32 | off << 4 | size);
}

static const struct mmio_ops device_ops = { device_read, device_write };

static int fake_device(void *ctx, struct ppc_cpu *cpu)
{
	struct device *d = ctx;
	struct mmio_access a;
	if (cpu->icount + 1 >= d->until || (cpu->fault_addr & 0x700) == 0x700 || !((d->state ^ cpu->fault_addr) & 15) ||
	    !mmio_decode(&d->bus, cpu, &a))
		return PPC_MMIO_NO;
	d->state = mix(mix(mix(d->state, cpu->icount), cpu->tb), cpu->pc);
	mmio_perform(&a, cpu);
	d->accesses++;
	switch (d->state >> 60) {
	case 0:
		cpu->cr ^= (uint32_t)d->state;
		break;
	case 1:
		mem_w32(d->mem, DATA + ((uint32_t)(d->state >> 20) & (DATA_SIZE - 4)), (uint32_t)d->state);
		break;
	case 2: {                                                       /* (just ahead: the block that's running, perhaps) */
		uint32_t at = cpu->pc + 4 * ((uint32_t)(d->state >> 32) % 8);
		if (at - CODE < 4 * CODE_MAX) {
			mem_invalidate(d->mem, at, 4);
			mem_w32(d->mem, at, 0x60000000u);                      /* nop */
		}
		break;
	}
	case 3: case 4:
		return PPC_MMIO_STOP;
	case 5:                                                         /* (as the kernel's in_place() might) */
		cpu->kcall_until = cpu->tb + (d->state >> 8 & 255);
		break;
	case 6:
		cpu->sc_until = cpu->tb + (d->state >> 8 & 255);
		break;
	}
	return PPC_MMIO_DONE;
}

static void setup(struct side *s, bool jit)
{
	CHECK_EQ(mem_init(&s->mem), 0);
	mem_map(&s->mem, 0, 0x40000, MEM_RWX);
	mem_map(&s->mem, DATA, DATA_SIZE, MEM_RW);
	mem_map(&s->mem, RO_PAGE, MEM_PAGE_SIZE, MEM_R);
	ppc_init(&s->cpu, &s->mem);
	memset(&s->dev, 0, sizeof s->dev);
	s->dev.mem = &s->mem;
	mmio_add(&s->dev.bus, IO_PAGE, MEM_PAGE_SIZE, &device_ops, &s->dev, "device");
	if (jit) {
		setenv("BLUE_JIT_HOT", "0", 1);         /* translate at once: blocks here run a few times */
		s->cpu.jit = jit_create(&s->mem);
		unsetenv("BLUE_JIT_HOT");
	}
}

static bool same(const struct side *a, const struct side *b, enum ppc_exit ea, enum ppc_exit eb)
{
	const struct ppc_cpu *x = &a->cpu, *y = &b->cpu;
	bool ok = ea == eb && x->pc == y->pc && x->icount == y->icount && x->tb == y->tb && x->cr == y->cr &&
		  x->xer == y->xer && x->lr == y->lr && x->ctr == y->ctr && x->fpscr == y->fpscr &&
		  x->vrsave == y->vrsave && !memcmp(x->gpr, y->gpr, sizeof x->gpr) &&
		  !memcmp(x->fpr, y->fpr, sizeof x->fpr) && x->resv_valid == y->resv_valid &&
		  x->hook_match_skip == y->hook_match_skip && x->exit_insn == y->exit_insn &&
		  (!x->resv_valid || x->resv_addr == y->resv_addr) &&
		  a->dev.state == b->dev.state && a->dev.accesses == b->dev.accesses;
	if (ok && (ea == PPC_EXIT_DSI || ea == PPC_EXIT_MMIO))
		ok = x->fault_addr == y->fault_addr && x->fault_write == y->fault_write;
	return ok && !memcmp(mem_ptr(&a->mem, DATA), mem_ptr(&b->mem, DATA), DATA_SIZE) &&
	       !memcmp(mem_ptr(&a->mem, CODE), mem_ptr(&b->mem, CODE), CODE_MAX * 4 + 64);
}

static void report(const struct side *a, const struct side *b, enum ppc_exit ea, enum ppc_exit eb,
		   const uint32_t *code, unsigned n, uint64_t seed)
{
	const struct ppc_cpu *x = &a->cpu, *y = &b->cpu;
	fprintf(stderr, "jit mismatch (seed %llu): exit %s/%s pc %08x/%08x icount %llu/%llu insn %08x/%08x\n",
		(unsigned long long)seed, ppc_exit_name(ea), ppc_exit_name(eb), x->pc, y->pc,
		(unsigned long long)x->icount, (unsigned long long)y->icount, x->exit_insn, y->exit_insn);
	for (int r = 0; r < 32; r++)
		if (x->gpr[r] != y->gpr[r])
			fprintf(stderr, "  r%d %08x / %08x\n", r, x->gpr[r], y->gpr[r]);
	for (int r = 0; r < 32; r++)
		if (x->fpr[r] != y->fpr[r])
			fprintf(stderr, "  f%d %016llx / %016llx\n", r, (unsigned long long)x->fpr[r],
				(unsigned long long)y->fpr[r]);
	if (x->cr != y->cr) fprintf(stderr, "  cr %08x / %08x\n", x->cr, y->cr);
	if (x->xer != y->xer) fprintf(stderr, "  xer %08x / %08x\n", x->xer, y->xer);
	if (x->lr != y->lr) fprintf(stderr, "  lr %08x / %08x\n", x->lr, y->lr);
	if (x->ctr != y->ctr) fprintf(stderr, "  ctr %08x / %08x\n", x->ctr, y->ctr);
	if (x->fpscr != y->fpscr) fprintf(stderr, "  fpscr %08x / %08x\n", x->fpscr, y->fpscr);
	if (x->fault_addr != y->fault_addr) fprintf(stderr, "  fault %08x / %08x\n", x->fault_addr, y->fault_addr);
	if (a->dev.state != b->dev.state || a->dev.accesses != b->dev.accesses)
		fprintf(stderr, "  device %016llx (%lu accesses) / %016llx (%lu)\n", (unsigned long long)a->dev.state,
			a->dev.accesses, (unsigned long long)b->dev.state, b->dev.accesses);
	for (uint32_t o = 0; o < DATA_SIZE; o += 4)
		if (mem_r32(&a->mem, DATA + o) != mem_r32(&b->mem, DATA + o)) {
			fprintf(stderr, "  mem %08x: %08x / %08x\n", DATA + o, mem_r32(&a->mem, DATA + o),
				mem_r32(&b->mem, DATA + o));
			break;
		}
	for (unsigned k = 0; k < n; k++)
		fprintf(stderr, "  %08x: %08x\n", CODE + 4 * k, code[k]);
}

/* Unless turned off, fastmem must have been on, and faulted */
static void check_fastmem(const struct jit *j)
{
	const char *mode = getenv("BLUE_JIT_FASTMEM");
	char *text = NULL;
	size_t size = 0;
	FILE *f = open_memstream(&text, &size);
	jit_print_stats(j, f);
	fclose(f);
	const char *line = strstr(text, "fastmem: ");
	if (mode && !strcmp(mode, "0"))
		CHECK(line == NULL);
	else
		CHECK(line && strtoul(line + 9, NULL, 10) > 0);
	free(text);
}

/* The JIT's exits of one kind ("interpreter", "kernel", "device", ...), from its statistics */
static unsigned long jit_exits(const struct jit *j, const char *kind)
{
	char *text = NULL;
	size_t size = 0, len = strlen(kind);
	FILE *f = open_memstream(&text, &size);
	jit_print_stats(j, f);
	fclose(f);
	unsigned long n = 0;
	char *p = strstr(text, "exits: "), *end;
	while (p && *p != '\n' && (p = strpbrk(p, "0123456789\n")) && *p != '\n') {
		unsigned long v = strtoul(p, &end, 10);
		if (*end == ' ' && !strncmp(end + 1, kind, len) && strchr(",\n", end[1 + len])) {
			n = v;
			break;
		}
		p = end;
	}
	free(text);
	return n;
}

/* With rom_code, the code can't be written (as in ROM, where the JIT follows branches) */
static void test_random_programs(unsigned programs, bool rom_code)
{
	static struct side a, b;
	setup(&a, false);
	setup(&b, true);
	more_jumps = rom_code;
	if (rom_code) {
		mem_map(&a.mem, CODE, MEM_PAGE_SIZE, MEM_RX);
		mem_map(&b.mem, CODE, MEM_PAGE_SIZE, MEM_RX);
	}
	if (!b.cpu.jit) {
		mem_free(&a.mem);
		mem_free(&b.mem);
		return;         /* no JIT on this host */
	}
	uint8_t *data = malloc(DATA_SIZE);
	unsigned failures = 0;
	unsigned long device_accesses = 0;
	const char *first = getenv("BLUE_JIT_SEED");
	uint64_t base = first ? strtoull(first, NULL, 0) : 0;

	for (unsigned p = 0; p < programs && failures < 3; p++) {
		uint64_t seed = 0x9E3779B97F4A7C15ull * (base + p + 1);
		rng = seed;
		uint32_t code[CODE_MAX + 1] = { 0 };   /* (all of it goes into guest memory) */
		unsigned n = 8 + pick(CODE_MAX - 8);
		fp_heavy = pick(4) == 0;
		for (unsigned k = 0; k < n; k++)
			code[k] = gen_insn(k, n);
		for (unsigned m = fp_heavy ? pick(4) : 0; m; m--) {    /* integers into doubles: stw, stw, lfd */
			unsigned k = pick(n - 4), base = pick(4) ? 31 : 29;
			int32_t d = (int32_t)(pick(256) * 4) - 512;
			code[k] = d_form(36, rsrc(), base, (uint32_t)d);
			code[k + 1] = d_form(36, rsrc(), base, (uint32_t)(d + 4));
			if (pick(3) == 0)               /* something in between, perhaps over them */
				code[k + 2] = pick(2) ? d_form((unsigned[]){ 36, 38, 44, 37, 32, 14 }[pick(6)], rdst(),
								 pick(4) ? base : rsrc(), (uint32_t)(d + (int32_t)pick(8))) :
							gen_insn(k + 2, n);
			code[k + 3] = d_form(pick(8) ? 50 : 51, pick(32), base, (uint32_t)d);
		}
		code[n] = 17u << 26 | 2;                                /* sc */
		for (unsigned k = 1; k < n; k++)                        /* the quick system calls, often */
			if (code[k] == code[n] && pick(2))
				code[k - 1] = d_form(14, 0, 0, (uint32_t)-1 - pick(2));       /* li r0, -1 or -2 */
		for (uint32_t o = 0; o < DATA_SIZE; o++)
			data[o] = (uint8_t)rnd();

		struct ppc_cpu init;
		ppc_init(&init, NULL);
		for (int r = 0; r < 32; r++)
			init.gpr[r] = interesting();
		init.gpr[28] = pick(4) ? DATA + 0x100 : pick(2) ? CODE + 4 * pick(n) : IO_PAGE + 0x400;  /* sometimes stores hit code */
		init.gpr[29] = BASE_B;
		init.gpr[30] = pick(64) * 4 + pick(4);
		init.gpr[31] = pick(8) ? BASE_A : pick(3) == 0 ? RO_PAGE + 0x800 : pick(2) ? IO_PAGE + 0x800 : BASE_END;
		for (int r = 0; r < 32; r++)
			init.fpr[r] = fp_value();
		switch (pick(8)) {                                      /* FPSCR: mostly as programs run */
		case 0: ppc_fp_set_fpscr(&init, interesting()); break;
		case 1: init.fpscr = pick(4); break;            /* rounding modes */
		case 2: init.fpscr = 0x02000000u; break;        /* XX already set */
		default: init.fpscr = 0; break;
		}
		init.cr = interesting();
		init.xer = interesting() & 0xE000007Fu;
		init.lr = pick(2) ? CODE + 4 * pick(n + 1) : interesting();
		init.ctr = pick(2) ? pick(5) : interesting();
		init.pc = CODE;
		init.tb = 1000;
		init.tb_step = pick(8) ? 1 : 0;
		switch (pick(4)) {                                      /* quick system calls: never, always, until */
		case 0:  init.sc_until = 0; break;
		case 1:  init.sc_until = UINT64_MAX; break;
		default: init.sc_until = init.tb + pick(400); break;
		}
		init.sc_blue = pick(2) ? 0x00200000u : 0;
		init.sc_word = interesting();
		switch (pick(4)) {                                      /* kernel calls in place: the same */
		case 0:  init.kcall_until = 0; break;
		case 1:  init.kcall_until = UINT64_MAX; break;
		default: init.kcall_until = init.tb + pick(400); break;
		}
		init.kcall = fuzz_kcall;
		fuzz_insns = n;
		if (pick(4) == 0)
			init.hook_pc = CODE + 4 * pick(n);
		if (pick(4) == 0) {                                     /* the match hook: often matching */
			unsigned reg = 28 + pick(4);
			if (pick(8) == 0)       /* the last halfword of a page, or one running into nothing */
				init.gpr[reg] = RO_PAGE + MEM_PAGE_SIZE - 1 - pick(2);
			uint32_t a = init.gpr[reg];
			init.hook_match_pc = CODE + 4 * pick(n);
			init.hook_match_reg = (uint8_t)reg;
			init.hook_match_value = pick(4) == 0 ? (uint16_t)rnd() :
						a - DATA < DATA_SIZE - 1 ? (uint16_t)(data[a - DATA] << 8 | data[a - DATA + 1]) : 0;
		}

		bool device = pick(4) != 0;                             /* (otherwise its accesses all fault) */
		uint64_t device_state = (uint64_t)rnd() << 32 | rnd();
		for (int side = 0; side < 2; side++) {
			struct side *s = side ? &b : &a;
			struct jit *j = s->cpu.jit;
			mem_load(&s->mem, CODE, code, sizeof code);
			for (unsigned k = 0; k <= n; k++)
				mem_w32(&s->mem, CODE + 4 * k, code[k]);
			mem_load(&s->mem, DATA, data, DATA_SIZE);
			s->cpu = init;
			s->cpu.mem = &s->mem;
			s->cpu.jit = j;
			if (device) {
				s->cpu.mmio = fake_device;
				s->cpu.mmio_ctx = &s->dev;
			}
			s->dev.state = device_state;
			s->dev.accesses = 0;
		}

		/* several runs with random budgets, as the kernel would make them */
		for (int run = 0; run < 6; run++) {
			uint64_t budget = 1 + pick(run & 1 ? 8 : 400);
			a.dev.until = a.cpu.icount + budget;
			b.dev.until = b.cpu.icount + budget;
			enum ppc_exit ea = ppc_interp(&a.cpu, budget);
			enum ppc_exit eb = ppc_run(&b.cpu, budget);
			if (!same(&a, &b, ea, eb)) {
				report(&a, &b, ea, eb, code, n + 1, seed);
				failures++;
				test_failures++;
				break;
			}
			if (ea == PPC_EXIT_HOOK || ea == PPC_EXIT_BUDGET || ea == PPC_EXIT_MMIO)
				continue;
			if (ea == PPC_EXIT_SC || ea == PPC_EXIT_TRAP || ea == PPC_EXIT_ILLEGAL || ea == PPC_EXIT_PRIV ||
			    ea == PPC_EXIT_DSI || ea == PPC_EXIT_UNIMPL) {
				/* skip the instruction, as a kernel might */
				a.cpu.pc += 4;
				b.cpu.pc += 4;
				continue;
			}
			break;
		}
		device_accesses += b.dev.accesses;
	}
	free(data);
	check_fastmem(b.cpu.jit);
	CHECK(device_accesses > 0);
	jit_free(b.cpu.jit);
	mem_free(&a.mem);
	mem_free(&b.mem);
}

/* The value of a logical immediate N:immr:imms (DecodeBitMasks, in the Arm ARM); 0 if invalid */
static uint64_t decode_logic_imm(uint32_t nrs)
{
	unsigned n = nrs >> 12 & 1, immr = nrs >> 6 & 63, imms = nrs & 63;
	unsigned combined = n << 6 | (~imms & 63);
	if (combined < 2)
		return 0;
	unsigned size = 1u << (31 - __builtin_clz(combined)), levels = size - 1;
	unsigned s = imms & levels, r = immr & levels;
	if (s == levels)
		return 0;
	uint64_t mask = size == 64 ? ~(uint64_t)0 : (1ull << size) - 1;
	uint64_t elt = (1ull << (s + 1)) - 1;
	if (r)
		elt = ((elt >> r) | (elt << (size - r))) & mask;
	uint64_t v = 0;
	for (unsigned k = 0; k < 64; k += size)
		v |= elt << k;
	return v;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

/* a64_logic_imm_encode(): every encodable value encodes to itself, and nothing else encodes */
static void test_logic_imm(void)
{
	static uint64_t all[5334];
	unsigned count = 0;
	for (uint32_t nrs = 0; nrs < 1u << 13; nrs++) {
		uint64_t v = decode_logic_imm(nrs);
		if (!v)
			continue;
		unsigned size = 1u << (31 - __builtin_clz((nrs >> 12 & 1) << 6 | (~nrs & 63)));
		if ((nrs >> 6 & 63) >= size)                    /* (larger rotations repeat the same values) */
			continue;
		uint32_t got = 0;
		CHECK(a64_logic_imm_encode(1, v, &got) && decode_logic_imm(got) == v);
		if (size <= 32) {
			got = 0;
			CHECK(a64_logic_imm_encode(0, v & 0xFFFFFFFFu, &got) && !(got >> 12) &&
			      decode_logic_imm(got) == v);
		}
		if (count < sizeof all / sizeof all[0])
			all[count++] = v;
	}
	CHECK_EQ(count, 5334);
	qsort(all, count, sizeof all[0], cmp_u64);
	uint64_t x = 0x0123456789ABCDEFull;
	for (unsigned k = 0; k < 200000; k++) {
		x = x * 6364136223846793005ull + 1442695040888963407ull;
		uint64_t v = k < count ? all[k] ^ 1ull << (x & 63) :    /* (near misses, then anything) */
			     k & 1 ? x : x >> (x & 63);
		uint32_t got;
		bool listed = bsearch(&v, all, count, sizeof all[0], cmp_u64) != NULL;
		if (a64_logic_imm_encode(1, v, &got) != listed)
			CHECK(!"64-bit logical immediate misjudged");
		uint64_t w = (v & 0xFFFFFFFFu) | v << 32;
		listed = bsearch(&w, all, count, sizeof all[0], cmp_u64) != NULL;
		if (a64_logic_imm_encode(0, v, &got) != listed)
			CHECK(!"32-bit logical immediate misjudged");
	}
}

static void x86_expect(const uint8_t *got, const uint8_t *end, const char *text, const char *want, size_t n)
{
	if ((size_t)(end - got) != n || memcmp(got, want, n)) {
		fprintf(stderr, "x86.h: %s:", text);
		for (const uint8_t *p = got; p < end; p++)
			fprintf(stderr, " %02x", *p);
		fprintf(stderr, ", not");
		for (size_t k = 0; k < n; k++)
			fprintf(stderr, " %02x", (uint8_t)want[k]);
		fprintf(stderr, "\n");
		test_failures++;
	}
}

#define X86_CASE(call, text, bytes) \
	do { start = b->p; call; x86_expect(start, b->p, text, bytes, sizeof bytes - 1); } while (0)

/*
 * x86.h's encodings, each as LLVM's assembler makes it from the text (but
 * where it picks another encoding as good, noted)
 */
static void test_x86_encoding(void)
{
	static uint8_t buf[1 << 16];
	struct x86_buf xb = { buf, buf + sizeof buf, false }, *b = &xb;
	uint8_t *start;
	X86_CASE(x86_mov_rr(b, 0, X86_RAX, X86_R9), "mov eax, r9d", "\x44\x89\xc8");
	X86_CASE(x86_mov_rr(b, 1, X86_R13, X86_RBX), "mov r13, rbx", "\x49\x89\xdd");
	X86_CASE(x86_mov_rr(b, 0, X86_RSI, X86_RDI), "mov esi, edi", "\x89\xfe");
	X86_CASE(x86_mov_ri(b, X86_R10, 0x12345678), "mov r10d, 0x12345678", "\x41\xba\x78\x56\x34\x12");
	X86_CASE(x86_mov_ri(b, X86_RBP, 0), "mov ebp, 0", "\xbd\x00\x00\x00\x00");
	X86_CASE(x86_mov_ri64(b, X86_RAX, 0x123456789ull),
		 "movabs rax, 0x123456789", "\x48\xb8\x89\x67\x45\x23\x01\x00\x00\x00");
	X86_CASE(x86_mov_ri64(b, X86_RCX, (uint64_t)-2), "mov rcx, -2", "\x48\xc7\xc1\xfe\xff\xff\xff");
	X86_CASE(x86_mov_ri64(b, X86_R8, 0xFFFFFFFFu), "mov r8d, 0xffffffff", "\x41\xb8\xff\xff\xff\xff");
	X86_CASE(x86_load(b, 1, false, X86_RAX, x86_m(X86_RBX, 4)),
		 "movzx eax, byte ptr [rbx + 4]", "\x0f\xb6\x43\x04");
	X86_CASE(x86_load(b, 2, true, X86_RCX, x86_m(X86_R13, 0)), "movsx ecx, word ptr [r13]", "\x41\x0f\xbf\x4d\x00");
	X86_CASE(x86_load(b, 2, false, X86_R9, x86_m(X86_RBP, -8)),
		 "movzx r9d, word ptr [rbp - 8]", "\x44\x0f\xb7\x4d\xf8");
	X86_CASE(x86_load(b, 1, true, X86_RDX, x86_m(X86_R12, 0x200)),
		 "movsx edx, byte ptr [r12 + 0x200]", "\x41\x0f\xbe\x94\x24\x00\x02\x00\x00");
	X86_CASE(x86_load(b, 4, false, X86_RDX, x86_m(X86_R12, 0x200)),
		 "mov edx, dword ptr [r12 + 0x200]", "\x41\x8b\x94\x24\x00\x02\x00\x00");
	X86_CASE(x86_load(b, 8, false, X86_RAX, x86_m(X86_RBP, 0)), "mov rax, qword ptr [rbp]", "\x48\x8b\x45\x00");
	X86_CASE(x86_load(b, 4, false, X86_R9, x86_m(X86_RSP, 8)),
		 "mov r9d, dword ptr [rsp + 8]", "\x44\x8b\x4c\x24\x08");
	X86_CASE(x86_load(b, 4, false, X86_RAX, x86_mx(X86_R15, X86_RAX, 0, 0)),
		 "mov eax, dword ptr [r15 + rax]", "\x41\x8b\x04\x07");
	X86_CASE(x86_load(b, 4, false, X86_RSI, x86_mx(X86_R15, X86_R8, 1, 0x10)),
		 "mov esi, dword ptr [r15 + 2*r8 + 0x10]", "\x43\x8b\x74\x47\x10");
	X86_CASE(x86_load(b, 4, false, X86_RCX, x86_mx(X86_RBX, X86_R12, 0, 0)),
		 "mov ecx, dword ptr [rbx + r12]", "\x42\x8b\x0c\x23");
	X86_CASE(x86_load(b, 8, false, X86_RDX, x86_mx(X86_RBP, X86_RCX, 3, 0)),
		 "mov rdx, qword ptr [rbp + 8*rcx]", "\x48\x8b\x54\xcd\x00");
	X86_CASE(x86_load(b, 4, false, X86_RAX, x86_mx(X86_R13, X86_RAX, 0, 0)),
		 "mov eax, dword ptr [r13 + rax]", "\x41\x8b\x44\x05\x00");
	X86_CASE(x86_load(b, 4, false, X86_R11, x86_mx(X86_RSP, X86_RAX, 2, 0x1000)),
		 "mov r11d, dword ptr [rsp + 4*rax + 0x1000]", "\x44\x8b\x9c\x84\x00\x10\x00\x00");
	X86_CASE(x86_load(b, 4, false, X86_RAX, x86_rip(start + 0x1000)),
		 "mov eax, dword ptr [rip + L + 0x1000]", "\x8b\x05\xfa\x0f\x00\x00");
	X86_CASE(x86_store(b, 1, X86_RSI, x86_m(X86_RBX, 1)), "mov byte ptr [rbx + 1], sil", "\x40\x88\x73\x01");
	X86_CASE(x86_store(b, 1, X86_RAX, x86_mx(X86_R15, X86_RCX, 0, 0)),
		 "mov byte ptr [r15 + rcx], al", "\x41\x88\x04\x0f");
	X86_CASE(x86_store(b, 2, X86_RCX, x86_mx(X86_R15, X86_RAX, 0, 0)),
		 "mov word ptr [r15 + rax], cx", "\x66\x41\x89\x0c\x07");
	X86_CASE(x86_store(b, 4, X86_R11, x86_m(X86_RBX, 0x180)),
		 "mov dword ptr [rbx + 0x180], r11d", "\x44\x89\x9b\x80\x01\x00\x00");
	X86_CASE(x86_store(b, 8, X86_R14, x86_m(X86_RSP, 0)), "mov qword ptr [rsp], r14", "\x4c\x89\x34\x24");
	X86_CASE(x86_store_imm(b, 4, x86_m(X86_RBX, 0x184), 0x12345678),
		 "mov dword ptr [rbx + 0x184], 0x12345678", "\xc7\x83\x84\x01\x00\x00\x78\x56\x34\x12");
	X86_CASE(x86_store_imm(b, 1, x86_m(X86_RBX, 3), 1), "mov byte ptr [rbx + 3], 1", "\xc6\x43\x03\x01");
	X86_CASE(x86_store_imm(b, 2, x86_m(X86_RBX, 2), 0x1234),
		 "mov word ptr [rbx + 2], 0x1234", "\x66\xc7\x43\x02\x34\x12");
	X86_CASE(x86_store_imm(b, 8, x86_m(X86_RAX, 0), 0xFFFFFFFFu),
		 "mov qword ptr [rax], -1", "\x48\xc7\x00\xff\xff\xff\xff");
	X86_CASE(x86_store_imm(b, 4, x86_rip(start + 0x1000), 7),
		 "mov dword ptr [rip + L + 0x1000], 7", "\xc7\x05\xf6\x0f\x00\x00\x07\x00\x00\x00");
	X86_CASE(x86_extend(b, 1, false, 0, X86_RAX, X86_RSI), "movzx eax, sil", "\x40\x0f\xb6\xc6");
	X86_CASE(x86_extend(b, 1, false, 0, X86_R8, X86_RDI), "movzx r8d, dil", "\x44\x0f\xb6\xc7");
	X86_CASE(x86_extend(b, 1, false, 0, X86_RSI, X86_RCX), "movzx esi, cl", "\x0f\xb6\xf1");
	X86_CASE(x86_extend(b, 2, true, 0, X86_RCX, X86_RDX), "movsx ecx, dx", "\x0f\xbf\xca");
	X86_CASE(x86_extend(b, 1, true, 0, X86_R13, X86_R9), "movsx r13d, r9b", "\x45\x0f\xbe\xe9");
	X86_CASE(x86_extend(b, 4, true, 1, X86_RAX, X86_R13), "movsxd rax, r13d", "\x49\x63\xc5");
	X86_CASE(x86_lea(b, 0, X86_RAX, x86_m(X86_R13, 0x7FFF)),
		 "lea eax, [r13 + 0x7fff]", "\x41\x8d\x85\xff\x7f\x00\x00");
	X86_CASE(x86_lea(b, 0, X86_RAX, x86_m(X86_R13, -32768)),
		 "lea eax, [r13 - 0x8000]", "\x41\x8d\x85\x00\x80\xff\xff");
	X86_CASE(x86_lea(b, 0, X86_RCX, x86_mx(X86_RBX, X86_RDX, 0, 0)), "lea ecx, [rbx + rdx]", "\x8d\x0c\x13");
	X86_CASE(x86_lea(b, 0, X86_R9, x86_mx(X86_RBP, X86_R12, 0, 0)), "lea r9d, [rbp + r12]", "\x46\x8d\x4c\x25\x00");
	X86_CASE(x86_lea(b, 1, X86_RDX, x86_rip(start + 0x1000)),
		 "lea rdx, [rip + L + 0x1000]", "\x48\x8d\x15\xf9\x0f\x00\x00");
	X86_CASE(x86_movbe(b, false, 4, X86_RAX, x86_mx(X86_R15, X86_RCX, 0, 0)),
		 "movbe eax, dword ptr [r15 + rcx]", "\x41\x0f\x38\xf0\x04\x0f");
	X86_CASE(x86_movbe(b, true, 4, X86_R10, x86_mx(X86_R15, X86_RAX, 0, 0)),
		 "movbe dword ptr [r15 + rax], r10d", "\x45\x0f\x38\xf1\x14\x07");
	X86_CASE(x86_movbe(b, false, 2, X86_RCX, x86_mx(X86_R15, X86_RAX, 0, 0)),
		 "movbe cx, word ptr [r15 + rax]", "\x66\x41\x0f\x38\xf0\x0c\x07");
	X86_CASE(x86_movbe(b, true, 2, X86_RDX, x86_mx(X86_R15, X86_RAX, 0, 0)),
		 "movbe word ptr [r15 + rax], dx", "\x66\x41\x0f\x38\xf1\x14\x07");
	X86_CASE(x86_movbe(b, false, 8, X86_RAX, x86_mx(X86_R15, X86_RDX, 0, 0)),
		 "movbe rax, qword ptr [r15 + rdx]", "\x49\x0f\x38\xf0\x04\x17");
	X86_CASE(x86_bswap(b, 0, X86_RAX), "bswap eax", "\x0f\xc8");
	X86_CASE(x86_bswap(b, 0, X86_R9), "bswap r9d", "\x41\x0f\xc9");
	X86_CASE(x86_bswap(b, 1, X86_RCX), "bswap rcx", "\x48\x0f\xc9");
	X86_CASE(x86_alu_rr(b, X86_ADD, 0, X86_RAX, X86_R9), "add eax, r9d", "\x44\x01\xc8");
	X86_CASE(x86_alu_rr(b, X86_SUB, 0, X86_R13, X86_RSI), "sub r13d, esi", "\x41\x29\xf5");
	X86_CASE(x86_alu_rr(b, X86_AND, 0, X86_RBP, X86_RDI), "and ebp, edi", "\x21\xfd");
	X86_CASE(x86_alu_rr(b, X86_OR, 1, X86_RCX, X86_R8), "or rcx, r8", "\x4c\x09\xc1");
	X86_CASE(x86_alu_rr(b, X86_XOR, 0, X86_RDX, X86_RDX), "xor edx, edx", "\x31\xd2");
	X86_CASE(x86_alu_rr(b, X86_CMP, 0, X86_R12, X86_RAX), "cmp r12d, eax", "\x41\x39\xc4");
	X86_CASE(x86_alu_rr(b, X86_ADC, 0, X86_RAX, X86_R10), "adc eax, r10d", "\x44\x11\xd0");
	X86_CASE(x86_alu_rr(b, X86_SBB, 0, X86_RCX, X86_RCX), "sbb ecx, ecx", "\x19\xc9");
	X86_CASE(x86_alu_rm(b, X86_ADD, 0, X86_RAX, x86_m(X86_RBX, 0x10)),
		 "add eax, dword ptr [rbx + 0x10]", "\x03\x43\x10");
	X86_CASE(x86_alu_rm(b, X86_CMP, 0, X86_RCX, x86_rip(start + 0x1000)),
		 "cmp ecx, dword ptr [rip + L + 0x1000]", "\x3b\x0d\xfa\x0f\x00\x00");
	X86_CASE(x86_alu_mr(b, X86_OR, 0, x86_m(X86_RBX, 8), X86_RAX), "or dword ptr [rbx + 8], eax", "\x09\x43\x08");
	X86_CASE(x86_alu_ri(b, X86_ADD, 0, X86_RCX, 1), "add ecx, 1", "\x83\xc1\x01");
	X86_CASE(x86_alu_ri(b, X86_SUB, 1, X86_R14, 128), "sub r14, 128", "\x49\x81\xee\x80\x00\x00\x00");
	X86_CASE(x86_alu_ri(b, X86_CMP, 1, X86_R14, 0x7FFFFFFF), "cmp r14, 0x7fffffff", "\x49\x81\xfe\xff\xff\xff\x7f");
	X86_CASE(x86_alu_ri(b, X86_AND, 0, X86_RBP, (int32_t)0xFFFF0FFFu),
		 "and ebp, 0xffff0fff", "\x81\xe5\xff\x0f\xff\xff");
	X86_CASE(x86_alu_ri(b, X86_OR, 0, X86_RCX, -1), "or ecx, -1", "\x83\xc9\xff");
	X86_CASE(x86_alu_ri(b, X86_XOR, 0, X86_RDX, (int32_t)0x80000000u),
		 "xor edx, 0x80000000", "\x81\xf2\x00\x00\x00\x80");
	X86_CASE(x86_alu_ri(b, X86_CMP, 0, X86_R11, -128), "cmp r11d, -128", "\x41\x83\xfb\x80");
	X86_CASE(x86_alu_mi(b, X86_CMP, 4, x86_rip(start + 0x1000), 0),
		 "cmp dword ptr [rip + L + 0x1000], 0", "\x83\x3d\xf9\x0f\x00\x00\x00");
	X86_CASE(x86_alu_mi(b, X86_SUB, 8, x86_m(X86_RBX, 0), 5), "sub qword ptr [rbx], 5", "\x48\x83\x2b\x05");
	X86_CASE(x86_alu_mi(b, X86_CMP, 1, x86_m(X86_RBX, 2), 0), "cmp byte ptr [rbx + 2], 0", "\x80\x7b\x02\x00");
	X86_CASE(x86_alu_mi(b, X86_AND, 4, x86_m(X86_RBX, 0x184), (int32_t)0xDFFFFFFFu),
		 "and dword ptr [rbx + 0x184], 0xdfffffff", "\x81\xa3\x84\x01\x00\x00\xff\xff\xff\xdf");
	X86_CASE(x86_test_rr(b, 0, X86_RAX, X86_RAX), "test eax, eax", "\x85\xc0");
	X86_CASE(x86_test_rr(b, 1, X86_R10, X86_R10), "test r10, r10", "\x4d\x85\xd2");
	X86_CASE(x86_test_ri(b, 0, X86_RBP, 0x10000), "test ebp, 0x10000", "\xf7\xc5\x00\x00\x01\x00");
	X86_CASE(x86_test_ri(b, 0, X86_RSI, 4), "test sil, 4", "\x40\xf6\xc6\x04");
	X86_CASE(x86_test_ri(b, 0, X86_R12, 0x80000000u), "test r12d, 0x80000000", "\x41\xf7\xc4\x00\x00\x00\x80");
	X86_CASE(x86_test_ri(b, 0, X86_RCX, 0x80), "test cl, 0x80", "\xf6\xc1\x80");
	X86_CASE(x86_test_mi(b, 1, x86_m(X86_RBX, 0x183), 2),
		 "test byte ptr [rbx + 0x183], 2", "\xf6\x83\x83\x01\x00\x00\x02");
	X86_CASE(x86_test_mi(b, 4, x86_m(X86_RBX, 4), 0x10000),
		 "test dword ptr [rbx + 4], 0x10000", "\xf7\x43\x04\x00\x00\x01\x00");
	X86_CASE(x86_unary(b, X86_NOT, 0, X86_RCX), "not ecx", "\xf7\xd1");
	X86_CASE(x86_unary(b, X86_NEG, 0, X86_R9), "neg r9d", "\x41\xf7\xd9");
	X86_CASE(x86_unary(b, X86_MUL, 1, X86_RCX), "mul rcx", "\x48\xf7\xe1");
	X86_CASE(x86_unary(b, X86_IMUL1, 0, X86_RCX), "imul ecx", "\xf7\xe9");
	X86_CASE(x86_unary(b, X86_DIV, 0, X86_RCX), "div ecx", "\xf7\xf1");
	X86_CASE(x86_unary(b, X86_IDIV, 1, X86_RCX), "idiv rcx", "\x48\xf7\xf9");
	X86_CASE(x86_inc(b, true, 0, X86_RSI), "dec esi", "\xff\xce");
	X86_CASE(x86_inc(b, false, 1, X86_R11), "inc r11", "\x49\xff\xc3");
	X86_CASE(x86_shift_ri(b, X86_SHL, 0, X86_RAX, 3), "shl eax, 3", "\xc1\xe0\x03");
	X86_CASE(x86_shift_ri(b, X86_SHR, 0, X86_R10, 1), "shr r10d, 1", "\x41\xd1\xea");
	X86_CASE(x86_shift_ri(b, X86_SAR, 1, X86_RCX, 63), "sar rcx, 63", "\x48\xc1\xf9\x3f");
	X86_CASE(x86_shift_ri(b, X86_ROL, 0, X86_RBP, 8), "rol ebp, 8", "\xc1\xc5\x08");
	X86_CASE(x86_shift_ri(b, X86_ROR, 0, X86_RDX, 31), "ror edx, 31", "\xc1\xca\x1f");
	X86_CASE(x86_shift_cl(b, X86_SHL, 0, X86_RAX), "shl eax, cl", "\xd3\xe0");
	X86_CASE(x86_shift_cl(b, X86_ROL, 0, X86_R9), "rol r9d, cl", "\x41\xd3\xc1");
	X86_CASE(x86_imul_rr(b, 0, X86_RAX, X86_R9), "imul eax, r9d", "\x41\x0f\xaf\xc1");
	X86_CASE(x86_imul_rr(b, 1, X86_RAX, X86_RCX), "imul rax, rcx", "\x48\x0f\xaf\xc1");
	X86_CASE(x86_imul_rri(b, 0, X86_RCX, X86_RSI, 5), "imul ecx, esi, 5", "\x6b\xce\x05");
	X86_CASE(x86_imul_rri(b, 0, X86_R8, X86_R13, 0x10000),
		 "imul r8d, r13d, 0x10000", "\x45\x69\xc5\x00\x00\x01\x00");
	X86_CASE(x86_cdq(b, 0), "cdq", "\x99");
	X86_CASE(x86_cdq(b, 1), "cqo", "\x48\x99");
	X86_CASE(x86_setcc(b, X86_L, X86_RAX), "setl al", "\x0f\x9c\xc0");
	X86_CASE(x86_setcc(b, X86_G, X86_RCX), "setg cl", "\x0f\x9f\xc1");
	X86_CASE(x86_setcc(b, X86_NE, X86_RSI), "setne sil", "\x40\x0f\x95\xc6");
	X86_CASE(x86_setcc(b, X86_B, X86_R9), "setb r9b", "\x41\x0f\x92\xc1");
	X86_CASE(x86_setcc(b, X86_O, X86_RDX), "seto dl", "\x0f\x90\xc2");
	X86_CASE(x86_cmov(b, X86_G, 0, X86_RAX, X86_RCX), "cmovg eax, ecx", "\x0f\x4f\xc1");
	X86_CASE(x86_cmov(b, X86_L, 0, X86_R10, X86_RDX), "cmovl r10d, edx", "\x44\x0f\x4c\xd2");
	X86_CASE(x86_cmov(b, X86_E, 1, X86_RAX, X86_R9), "cmove rax, r9", "\x49\x0f\x44\xc1");
	X86_CASE(x86_bt_ri(b, X86_BT, 0, X86_RBP, 31), "bt ebp, 31", "\x0f\xba\xe5\x1f");
	X86_CASE(x86_bt_ri(b, X86_BTS, 0, X86_R12, 5), "bts r12d, 5", "\x41\x0f\xba\xec\x05");
	X86_CASE(x86_bt_ri(b, X86_BTR, 0, X86_RCX, 29), "btr ecx, 29", "\x0f\xba\xf1\x1d");
	X86_CASE(x86_bt_ri(b, X86_BTC, 1, X86_RAX, 63), "btc rax, 63", "\x48\x0f\xba\xf8\x3f");
	X86_CASE(x86_bt_mi(b, X86_BT, x86_m(X86_RBX, 0x184), 31),
		 "bt dword ptr [rbx + 0x184], 31", "\x0f\xba\xa3\x84\x01\x00\x00\x1f");
	X86_CASE(x86_cmc(b), "cmc", "\xf5");
	X86_CASE(x86_lzcnt(b, 0, X86_RAX, X86_R9), "lzcnt eax, r9d", "\xf3\x41\x0f\xbd\xc1");
	X86_CASE(x86_tzcnt(b, 1, X86_RCX, X86_RDX), "tzcnt rcx, rdx", "\xf3\x48\x0f\xbc\xca");
	X86_CASE(x86_andn(b, 0, X86_RAX, X86_RCX, X86_RDX), "andn eax, ecx, edx", "\xc4\xe2\x70\xf2\xc2");
	X86_CASE(x86_andn(b, 1, X86_R9, X86_R10, X86_R11), "andn r9, r10, r11", "\xc4\x42\xa8\xf2\xcb");
	X86_CASE(x86_bzhi(b, 0, X86_RAX, X86_RDX, X86_RCX), "bzhi eax, edx, ecx", "\xc4\xe2\x70\xf5\xc2");
	X86_CASE(x86_bzhi(b, 0, X86_R8, X86_R13, X86_RSI), "bzhi r8d, r13d, esi", "\xc4\x42\x48\xf5\xc5");
	X86_CASE(x86_shiftx(b, X86_SHL, 0, X86_RAX, X86_RDI, X86_RCX), "shlx eax, edi, ecx", "\xc4\xe2\x71\xf7\xc7");
	X86_CASE(x86_shiftx(b, X86_SHR, 0, X86_R8, X86_R13, X86_R10), "shrx r8d, r13d, r10d", "\xc4\x42\x2b\xf7\xc5");
	X86_CASE(x86_shiftx(b, X86_SAR, 1, X86_RDX, X86_RAX, X86_RCX), "sarx rdx, rax, rcx", "\xc4\xe2\xf2\xf7\xd0");
	X86_CASE(x86_shiftx(b, X86_SHL, 1, X86_RAX, X86_RSI, X86_R11), "shlx rax, rsi, r11", "\xc4\xe2\xa1\xf7\xc6");
	X86_CASE(x86_rorx(b, 0, X86_RAX, X86_RBP, 4), "rorx eax, ebp, 4", "\xc4\xe3\x7b\xf0\xc5\x04");
	X86_CASE(x86_rorx(b, 1, X86_R10, X86_R13, 63), "rorx r10, r13, 63", "\xc4\x43\xfb\xf0\xd5\x3f");
	X86_CASE(x86_rorx(b, 0, X86_RSI, X86_R9, 0), "rorx esi, r9d, 0", "\xc4\xc3\x7b\xf0\xf1\x00");
	X86_CASE(x86_mulx(b, 0, X86_RAX, X86_RCX, X86_RSI), "mulx eax, ecx, esi", "\xc4\xe2\x73\xf6\xc6");
	X86_CASE(x86_mulx(b, 1, X86_R8, X86_R9, X86_R10), "mulx r8, r9, r10", "\xc4\x42\xb3\xf6\xc2");
	X86_CASE(x86_jmp_r(b, X86_RAX), "jmp rax", "\xff\xe0");
	X86_CASE(x86_jmp_r(b, X86_R11), "jmp r11", "\x41\xff\xe3");
	X86_CASE(x86_jmp_m(b, x86_mx(X86_RDX, X86_RCX, 1, 0)), "jmp qword ptr [rdx + 2*rcx]", "\xff\x24\x4a");
	X86_CASE(x86_jmp_m(b, x86_mx(X86_RDX, X86_R9, 1, 0)), "jmp qword ptr [rdx + 2*r9]", "\x42\xff\x24\x4a");
	X86_CASE(x86_call_m(b, x86_rip(start + 0x1000)),
		 "call qword ptr [rip + L + 0x1000]", "\xff\x15\xfa\x0f\x00\x00");
	X86_CASE(x86_ret(b), "ret", "\xc3");
	X86_CASE(x86_call(b, start + 0x1000), "L: call L + 0x1000", "\xe8\xfb\x0f\x00\x00");
	X86_CASE(x86_push(b, X86_RBP), "push rbp", "\x55");
	X86_CASE(x86_push(b, X86_R15), "push r15", "\x41\x57");
	X86_CASE(x86_pop(b, X86_R12), "pop r12", "\x41\x5c");
	X86_CASE(x86_pop(b, X86_RBX), "pop rbx", "\x5b");
	X86_CASE(x86_int3(b), "int3", "\xcc");
	X86_CASE(x86_jmp(b, start + 0x1000), "jmp L + 0x1000", "\xe9\xfb\x0f\x00\x00");
	X86_CASE(x86_jcc(b, X86_L, start + 0x1000), "jl L + 0x1000", "\x0f\x8c\xfa\x0f\x00\x00");
	X86_CASE(x86_jcc(b, X86_NE, start - 0x100), "jne L - 0x100", "\x0f\x85\xfa\xfe\xff\xff");
	X86_CASE(x86_patch8(x86_jmp8(b), start + 0x10), "jmp L + 0x10", "\xeb\x0e");
	X86_CASE(x86_patch8(x86_jcc8(b, X86_AE), start + 0x40), "jae L + 0x40", "\x73\x3e");
	X86_CASE(x86_vsd(b, X86_VADDSD, 0, 1, 2), "vaddsd xmm0, xmm1, xmm2", "\xc5\xf3\x58\xc2");
	X86_CASE(x86_vsd(b, X86_VMULSD, 9, 10, 15), "vmulsd xmm9, xmm10, xmm15", "\xc4\x41\x2b\x59\xcf");
	X86_CASE(x86_vsd(b, X86_VSUBSD, 3, 12, 4), "vsubsd xmm3, xmm12, xmm4", "\xc5\x9b\x5c\xdc");
	X86_CASE(x86_vsd(b, X86_VDIVSD, 8, 8, 0), "vdivsd xmm8, xmm8, xmm0", "\xc5\x3b\x5e\xc0");
	X86_CASE(x86_vsd(b, X86_VSQRTSD, 1, 1, 9), "vsqrtsd xmm1, xmm1, xmm9", "\xc4\xc1\x73\x51\xc9");
	X86_CASE(x86_vmovapd(b, 6, 14), "vmovapd xmm6, xmm14", "\xc4\xc1\x79\x28\xf6");    /* (LLVM: c5 79 29 f6) */
	X86_CASE(x86_vmovapd(b, 14, 6), "vmovapd xmm14, xmm6", "\xc5\x79\x28\xf6");
	X86_CASE(x86_vmovq_from(b, 3, X86_RAX), "vmovq xmm3, rax", "\xc4\xe1\xf9\x6e\xd8");
	X86_CASE(x86_vmovq_from(b, 12, X86_R9), "vmovq xmm12, r9", "\xc4\x41\xf9\x6e\xe1");
	X86_CASE(x86_vmovq_to(b, X86_RCX, 12), "vmovq rcx, xmm12", "\xc4\x61\xf9\x7e\xe1");
	X86_CASE(x86_vmovsd(b, false, 1, x86_m(X86_RBX, 0x100)),
		 "vmovsd xmm1, qword ptr [rbx + 0x100]", "\xc5\xfb\x10\x8b\x00\x01\x00\x00");
	X86_CASE(x86_vmovsd(b, true, 9, x86_m(X86_RBX, 0x108)),
		 "vmovsd qword ptr [rbx + 0x108], xmm9", "\xc5\x7b\x11\x8b\x08\x01\x00\x00");
	X86_CASE(x86_vmovsd(b, false, 2, x86_mx(X86_R15, X86_RAX, 0, 0)),
		 "vmovsd xmm2, qword ptr [r15 + rax]", "\xc4\xc1\x7b\x10\x14\x07");
	X86_CASE(x86_vucomisd(b, 0, 7), "vucomisd xmm0, xmm7", "\xc5\xf9\x2e\xc7");
	X86_CASE(x86_vucomisd(b, 10, 11), "vucomisd xmm10, xmm11", "\xc4\x41\x79\x2e\xd3");
	X86_CASE(x86_vpd(b, X86_VXORPD, 2, 2, 2), "vxorpd xmm2, xmm2, xmm2", "\xc5\xe9\x57\xd2");
	X86_CASE(x86_vpd(b, X86_VANDPD, 0, 1, 13),
		 "vandpd xmm0, xmm1, xmm13", "\xc4\xc1\x71\x54\xc5");        /* (LLVM: c5 91 54 c1) */
	X86_CASE(x86_vfma(b, X86_VFMADD231SD, 0, 1, 2), "vfmadd231sd xmm0, xmm1, xmm2", "\xc4\xe2\xf1\xb9\xc2");
	X86_CASE(x86_vfma(b, X86_VFMSUB231SD, 9, 10, 11), "vfmsub231sd xmm9, xmm10, xmm11", "\xc4\x42\xa9\xbb\xcb");
	X86_CASE(x86_vfma(b, X86_VFNMADD231SD, 3, 14, 5), "vfnmadd231sd xmm3, xmm14, xmm5", "\xc4\xe2\x89\xbd\xdd");
	X86_CASE(x86_vfma(b, X86_VFNMSUB231SD, 15, 0, 8), "vfnmsub231sd xmm15, xmm0, xmm8", "\xc4\x42\xf9\xbf\xf8");
	X86_CASE(x86_vcvt(b, true, 0, 0, 1), "vcvtsd2ss xmm0, xmm0, xmm1", "\xc5\xfb\x5a\xc1");
	X86_CASE(x86_vcvt(b, false, 2, 2, 9), "vcvtss2sd xmm2, xmm2, xmm9", "\xc4\xc1\x6a\x5a\xd1");
	X86_CASE(x86_vroundsd(b, 0, 0, 1, 3), "vroundsd xmm0, xmm0, xmm1, 3", "\xc4\xe3\x79\x0b\xc1\x03");
	X86_CASE(x86_vcvttsd2si(b, 0, X86_RAX, 1), "vcvttsd2si eax, xmm1", "\xc5\xfb\x2c\xc1");
	X86_CASE(x86_vcvttsd2si(b, 1, X86_R10, 9), "vcvttsd2si r10, xmm9", "\xc4\x41\xfb\x2c\xd1");
	X86_CASE(x86_vcvtsi2sd(b, 0, 0, 0, X86_RAX), "vcvtsi2sd xmm0, xmm0, eax", "\xc5\xfb\x2a\xc0");
	X86_CASE(x86_vmovd_from(b, 5, X86_RCX), "vmovd xmm5, ecx", "\xc5\xf9\x6e\xe9");
	X86_CASE(x86_vmovd_from(b, 13, X86_R9), "vmovd xmm13, r9d", "\xc4\x41\x79\x6e\xe9");
	X86_CASE(x86_vmovd_to(b, X86_RAX, 14), "vmovd eax, xmm14", "\xc5\x79\x7e\xf0");
	X86_CASE(x86_vpd_m(b, X86_VXORPD, 0, 9, x86_rip(start + 0x1000)),
		 "vxorpd xmm0, xmm9, xmmword ptr [rip + L + 0x1000]", "\xc5\xb1\x57\x05\xf8\x0f\x00\x00");
	X86_CASE(x86_vpd_m(b, X86_VANDPD, 12, 3, x86_m(X86_RBX, 0x10)),
		 "vandpd xmm12, xmm3, xmmword ptr [rbx + 0x10]", "\xc5\x61\x54\x63\x10");
	X86_CASE(x86_vcmpsd(b, 4, 5, 9, 2), "vcmplesd xmm4, xmm5, xmm9", "\xc4\xc1\x53\xc2\xe1\x02");
	X86_CASE(x86_vblendvpd(b, 7, 3, 14, 4), "vblendvpd xmm7, xmm3, xmm14, xmm4", "\xc4\xc3\x61\x4b\xfe\x40");
	X86_CASE(x86_vblendvpd(b, 12, 12, 0, 13), "vblendvpd xmm12, xmm12, xmm0, xmm13", "\xc4\x63\x19\x4b\xe0\xd0");
	X86_CASE(x86_vmovsd(b, false, 13, x86_rip(start + 0x1000)),
		 "vmovsd xmm13, qword ptr [rip + L + 0x1000]", "\xc5\x7b\x10\x2d\xf8\x0f\x00\x00");
	X86_CASE(x86_vmovsd(b, true, 0, x86_m(X86_RSP, 0)), "vmovsd qword ptr [rsp], xmm0", "\xc5\xfb\x11\x04\x24");
	X86_CASE(x86_cmov_m(b, X86_G, 0, X86_RAX, x86_rip(start + 0x1000)),
		 "cmovg eax, dword ptr [rip + L + 0x1000]", "\x0f\x4f\x05\xf9\x0f\x00\x00");
	X86_CASE(x86_cmov_m(b, X86_B, 0, X86_R9, x86_m(X86_RBX, 0x40)),
		 "cmovb r9d, dword ptr [rbx + 0x40]", "\x44\x0f\x42\x4b\x40");
	X86_CASE(x86_mov_low(b, 1, X86_RSI, X86_RDI), "mov sil, dil", "\x40\x88\xfe");
	X86_CASE(x86_mov_low(b, 1, X86_R12, X86_RBP), "mov r12b, bpl", "\x41\x88\xec");
	X86_CASE(x86_mov_low(b, 1, X86_RAX, X86_R13), "mov al, r13b", "\x44\x88\xe8");
	X86_CASE(x86_mov_low(b, 2, X86_R8, X86_RSI), "mov r8w, si", "\x66\x41\x89\xf0");
	X86_CASE(x86_mov_low(b, 2, X86_RBP, X86_R11), "mov bp, r11w", "\x66\x44\x89\xdd");
	CHECK(!b->overflow);
}

/*
 * NanoKernel 2's quick system calls, by both engines: answered in place,
 * retiring nothing, while the time base is below sc_until; else (and for
 * other calls) left to the kernel.
 */
static void test_quick_sc(void)
{
	const uint32_t prog[] = {
		LI(0, -1), SC, ORI(5, 0, 0), MFCR(6), LI(0, -2), SC, ORI(7, 0, 0), MFCR(8), LI(0, 5), SC,
	};
	const unsigned n = sizeof prog / sizeof prog[0];
	static struct side s;
	for (int jit = 0; jit < 2; jit++) {
		setup(&s, jit);
		if (jit && !s.cpu.jit) {
			mem_free(&s.mem);
			return;
		}
		mem_invalidate(&s.mem, CODE, sizeof prog);
		for (unsigned k = 0; k < n; k++)
			mem_w32(&s.mem, CODE + 4 * k, prog[k]);
		/* off; the blue task; another, with XER[SO]; only the first call in time */
		for (int c = 0; c < 4; c++) {
			struct jit *j = s.cpu.jit;
			ppc_init(&s.cpu, &s.mem);
			s.cpu.jit = j;
			s.cpu.xer = c == 2 ? XER_SO : 0;
			s.cpu.sc_until = c == 0 ? 0 : c == 3 ? 1002 : UINT64_MAX;
			s.cpu.sc_blue = c == 2 ? 0 : 0x00200000u;
			s.cpu.sc_word = 0xCAFE0001u;
			for (int run = 0; run < 2; run++) {     /* (again, through what the first run translated) */
				s.cpu.pc = CODE;
				s.cpu.icount = 0;
				s.cpu.tb = 1000;
				s.cpu.cr = 0x01234567u;
				s.cpu.gpr[5] = s.cpu.gpr[6] = s.cpu.gpr[7] = s.cpu.gpr[8] = 0;
				enum ppc_exit e = ppc_run(&s.cpu, 1000);
				CHECK_EQ(e, PPC_EXIT_SC);
				if (c == 0) {
					CHECK_EQ(s.cpu.pc, CODE + 4);
					CHECK_EQ(s.cpu.icount, 1);
					CHECK_EQ(s.cpu.gpr[0], 0xFFFFFFFFu);
					CHECK_EQ(s.cpu.cr, 0x01234567u);
				} else if (c == 3) {            /* the second call is at time base 1004 */
					CHECK_EQ(s.cpu.pc, CODE + 4 * 5);
					CHECK_EQ(s.cpu.icount, 4);
					CHECK_EQ(s.cpu.tb, 1004);
					CHECK_EQ(s.cpu.gpr[5], 0x00200000u);
					CHECK_EQ(s.cpu.gpr[6], 0x41234567u);
					CHECK_EQ(s.cpu.gpr[0], 0xFFFFFFFEu);
				} else {
					uint32_t cr0 = c == 2 ? 0x30000000u : 0x40000000u;
					CHECK_EQ(s.cpu.pc, CODE + 4 * (n - 1));
					CHECK_EQ(s.cpu.icount, n - 3);
					CHECK_EQ(s.cpu.tb, 1000 + n - 3);
					CHECK_EQ(s.cpu.gpr[5], c == 2 ? 0 : 0x00200000u);
					CHECK_EQ(s.cpu.gpr[6], cr0 | 0x01234567u);
					CHECK_EQ(s.cpu.gpr[7], 0xCAFE0001u);
					CHECK_EQ(s.cpu.gpr[8], cr0 | 0x01234567u);
					CHECK_EQ(s.cpu.gpr[0], 5);
				}
			}
		}
		if (jit)
			CHECK_EQ(jit_exits(s.cpu.jit, "interpreter"), 0);       /* (the JIT answered them itself) */
		jit_free(s.cpu.jit);
		mem_free(&s.mem);
	}
}

#define NATIVE (CODE + 0x100)

/*
 * A kernel's context switches for test_kcalls(): 1 runs the native code
 * (unless r4 & CR), saving the caller's registers; 0 with r3 32 returns to
 * it, the native r5 in its r3.
 */
static struct { uint32_t gpr[32], cr, lr, pc; unsigned made; } kc;

static bool switch_kcall(struct ppc_cpu *c, unsigned n)
{
	if (n == 1 && !(c->gpr[4] & c->cr)) {
		memcpy(kc.gpr, c->gpr, sizeof kc.gpr);
		kc.cr = c->cr;
		kc.lr = c->lr;
		kc.pc = c->pc + 4;
		for (int r = 0; r < 32; r++)
			c->gpr[r] = 0x1000u + (uint32_t)r;
		c->cr = 0;
		c->pc = NATIVE;
	} else if (n == 0 && c->gpr[3] == 32) {
		uint32_t result = c->gpr[5];
		memcpy(c->gpr, kc.gpr, sizeof kc.gpr);
		c->gpr[3] = result;
		c->cr = kc.cr;
		c->lr = kc.lr;
		c->pc = kc.pc;
	} else {
		return false;
	}
	kc.made++;
	return true;
}

/*
 * A host call with floating-point arguments, by both engines: f1 to f13
 * are in the processor's registers at the exit (the JIT keeps some in host
 * registers, written back at exits), as the guest computed them.
 */
static void test_hostcall_fprs(void)
{
	uint32_t prog[CODE_MAX];
	unsigned n = 0;
	for (unsigned k = 1; k <= 13; k++)
		prog[n++] = LFD(k, (int)(8 * k), 3);
	prog[n++] = FADD(13, 12, 1);            /* (computed in the block) */
	prog[n++] = FMUL(2, 2, 3);
	prog[n++] = HOSTCALL(0x123);
	static struct side s;
	uint64_t want[14];
	for (int jit = 0; jit < 2; jit++) {
		setup(&s, jit);
		if (jit && !s.cpu.jit) {
			mem_free(&s.mem);
			return;
		}
		mem_invalidate(&s.mem, CODE, 4 * n);
		for (unsigned k = 0; k < n; k++)
			mem_w32(&s.mem, CODE + 4 * k, prog[k]);
		for (unsigned k = 1; k <= 13; k++) {
			double d = 1.5 * k - 0.25;
			uint64_t b;
			memcpy(&b, &d, 8);
			mem_w64(&s.mem, DATA + 8 * k, b);
		}
		for (int run = 0; run < 3; run++) {     /* (translated, then through the translation) */
			s.cpu.pc = CODE;
			s.cpu.gpr[3] = DATA;
			memset(s.cpu.fpr, 0, sizeof s.cpu.fpr);
			CHECK_EQ(ppc_run(&s.cpu, 1000), PPC_EXIT_HOSTCALL);
			CHECK_EQ(s.cpu.pc, CODE + 4 * (n - 1));
			if (!jit && !run)
				memcpy(want, s.cpu.fpr, sizeof want);
			CHECK(!memcmp(s.cpu.fpr, want, sizeof want));
		}
		jit_free(s.cpu.jit);
		mem_free(&s.mem);
	}
	double f13, f2;
	memcpy(&f13, &want[13], 8);
	memcpy(&f2, &want[2], 8);
	CHECK(f13 == 1.5 * 12 - 0.25 + 1.25 && f2 == 2.75 * 4.25);
}

/* Kernel calls made in place by both engines: there and back; declined; out of time; never */
static void test_kcalls(void)
{
	const uint32_t prog[] = { ORI(0, 0, 0), TWI(31, 31, 1), ORI(6, 3, 0), SC };
	const uint32_t native[] = { ADDI(5, 5, 1), LI(3, 32), TWI(31, 31, 0) };
	static struct side s;
	for (int jit = 0; jit < 2; jit++) {
		setup(&s, jit);
		if (jit && !s.cpu.jit) {
			mem_free(&s.mem);
			return;
		}
		mem_invalidate(&s.mem, CODE, 0x200);
		for (unsigned k = 0; k < 4; k++)
			mem_w32(&s.mem, CODE + 4 * k, prog[k]);
		for (unsigned k = 0; k < 3; k++)
			mem_w32(&s.mem, NATIVE + 4 * k, native[k]);
		for (int c = 0; c < 4; c++) {
			struct jit *j = s.cpu.jit;
			ppc_init(&s.cpu, &s.mem);
			s.cpu.jit = j;
			s.cpu.kcall = switch_kcall;
			s.cpu.kcall_until = c == 2 ? 1002 : c == 3 ? 0 : UINT64_MAX;
			for (int run = 0; run < 2; run++) {     /* (again, through what the first run translated) */
				s.cpu.pc = CODE;
				s.cpu.icount = 0;
				s.cpu.tb = 1000;
				s.cpu.cr = c == 1 ? 0x00800000u : 0x00400000u;
				s.cpu.gpr[4] = 0x00800000u;
				s.cpu.gpr[5] = 7;
				s.cpu.gpr[6] = 0;
				kc.made = 0;
				enum ppc_exit e = ppc_run(&s.cpu, 1000);
				if (c == 0) {                   /* native code ran, and the System code went on */
					CHECK_EQ(e, PPC_EXIT_SC);
					CHECK_EQ(s.cpu.pc, CODE + 12);
					CHECK_EQ(s.cpu.icount, 4);
					CHECK_EQ(s.cpu.tb, 1004);
					CHECK_EQ(s.cpu.gpr[6], 0x1006);
					CHECK_EQ(s.cpu.gpr[5], 7);
					CHECK_EQ(s.cpu.cr, 0x00400000u);
					CHECK_EQ(s.cpu.exit_insn, SC);
					CHECK_EQ(kc.made, 2);
				} else if (c == 2) {            /* the way back comes too late */
					CHECK_EQ(e, PPC_EXIT_TRAP);
					CHECK_EQ(s.cpu.pc, NATIVE + 8);
					CHECK_EQ(s.cpu.icount, 3);
					CHECK_EQ(s.cpu.gpr[3], 32);
					CHECK_EQ(s.cpu.gpr[5], 0x1006);
					CHECK_EQ(s.cpu.exit_insn, TWI(31, 31, 0));
					CHECK_EQ(kc.made, 1);
				} else {                        /* declined (an interrupt pending), or never */
					CHECK_EQ(e, PPC_EXIT_TRAP);
					CHECK_EQ(s.cpu.pc, CODE + 4);
					CHECK_EQ(s.cpu.icount, 1);
					CHECK_EQ(s.cpu.gpr[5], 7);
					CHECK_EQ(s.cpu.exit_insn, TWI(31, 31, 1));
					CHECK_EQ(kc.made, 0);
				}
			}
		}
		if (jit)
			CHECK_EQ(jit_exits(s.cpu.jit, "interpreter"), 0);       /* (the JIT made them itself) */
		jit_free(s.cpu.jit);
		mem_free(&s.mem);
	}
}

/* A device whose every access the mmio hook takes */
static int plain_device(void *ctx, struct ppc_cpu *cpu)
{
	struct device *d = ctx;
	struct mmio_access a;
	if (!mmio_decode(&d->bus, cpu, &a))
		return PPC_MMIO_NO;
	mmio_perform(&a, cpu);
	d->accesses++;
	return PPC_MMIO_DONE;
}

/*
 * Device registers through the mmio hook, by both engines, in a loop: the
 * JIT offers each access to the hook itself, at once without fastmem, or
 * once its instruction has faulted (and the interpreter has taken that one)
 */
static void test_device_in_place(void)
{
	const uint32_t prog[] = {
		LIS(3, IO_PAGE >> 16), LI(0, 40), MTCTR(0),
		LWZ(4, 0x10, 3), STW(4, 0x20, 3), LHA(5, 0x12, 3), STH(5, 0x30, 3), LWZU(7, 4, 3), STWU(7, 4, 3),
		ADDI(6, 6, 1), BDNZ(-28),
		SC,
	};
	static struct side s[2];
	enum ppc_exit e[2];
	for (int jit = 0; jit < 2; jit++) {
		setup(&s[jit], jit);
		for (unsigned k = 0; k < sizeof prog / sizeof prog[0]; k++)
			mem_w32(&s[jit].mem, CODE + 4 * k, prog[k]);
		s[jit].cpu.mmio = plain_device;
		s[jit].cpu.mmio_ctx = &s[jit].dev;
		s[jit].cpu.pc = CODE;
		e[jit] = ppc_run(&s[jit].cpu, 10000);
	}
	if (s[1].cpu.jit) {
		CHECK_EQ(e[0], PPC_EXIT_SC);
		CHECK(same(&s[0], &s[1], e[0], e[1]));
		CHECK_EQ(s[0].dev.accesses, 240);
		CHECK_EQ(s[1].dev.accesses, 240);
		CHECK_EQ(s[0].dev.state, s[1].dev.state);
		CHECK(jit_exits(s[1].cpu.jit, "interpreter") <= 6);     /* (each access instruction's fault) */
		jit_free(s[1].cpu.jit);
	}
	mem_free(&s[0].mem);
	mem_free(&s[1].mem);
}

/* Effective addresses that wrap past 4 GB (rA + d), by both engines, in a loop: the same memory */
static void test_wrapping_ea(void)
{
	const uint32_t prog[] = {
		LIS(3, 0xFFFF), ORI(3, 3, 0xFFF0), LI(0, 20), MTCTR(0),
		ADDI(5, 5, 7), STW(5, 0x30, 3), LWZ(4, 0x30, 3), LHZ(6, 0x32, 3), STH(6, 0x36, 3), LBZ(7, 0x3F, 3),
		ADD(8, 8, 4), BDNZ(-28),
		SC,
	};
	static struct side s[2];
	enum ppc_exit e[2];
	for (int jit = 0; jit < 2; jit++) {
		setup(&s[jit], jit);
		for (unsigned k = 0; k < sizeof prog / sizeof prog[0]; k++)
			mem_w32(&s[jit].mem, CODE + 4 * k, prog[k]);
		mem_w32(&s[jit].mem, 0x2C, 0x11223344);
		s[jit].cpu.pc = CODE;
		e[jit] = ppc_run(&s[jit].cpu, 10000);
	}
	if (s[1].cpu.jit) {
		CHECK_EQ(e[0], PPC_EXIT_SC);
		CHECK(same(&s[0], &s[1], e[0], e[1]));
		CHECK_EQ(s[0].cpu.gpr[8], 7u * 20 * 21 / 2);
		CHECK(!memcmp(mem_ptr(&s[0].mem, 0x20), mem_ptr(&s[1].mem, 0x20), 0x20));
		CHECK_EQ(mem_r32(&s[1].mem, 0x20), 140u);
		jit_free(s[1].cpu.jit);
	}
	mem_free(&s[0].mem);
	mem_free(&s[1].mem);
}

/* ---- BLUE_JIT_BENCH=1: speed, not a test ---- */

#define X_FORM(rt, ra, rb, xo) enc_x(31, rt, ra, rb, xo, 0)

/* Guest instructions a second for `insns` instructions of a program, interpreted or translated */
static double bench_run(const uint32_t *prog, unsigned n, bool jit, uint64_t insns)
{
	static struct side s;
	setup(&s, false);
	if (jit) {
		s.cpu.jit = jit_create(&s.mem);
		if (!s.cpu.jit) {
			mem_free(&s.mem);
			return 0;
		}
	}
	for (unsigned k = 0; k < n; k++)
		mem_w32(&s.mem, CODE + 4 * k, prog[k]);
	uint64_t x = 0x9E3779B97F4A7C15ull;
	for (uint32_t o = 0; o < DATA_SIZE; o += 2) {           /* the dispatch loop's "opcodes": handlers 0-7 */
		x = x * 6364136223846793005ull + 1442695040888963407ull;
		mem_w16(&s.mem, DATA + o, (uint16_t)(x >> 40));
	}
	mem_w16(&s.mem, DATA + 0x3FFE, 7);                      /* (the end) */
	for (unsigned k = 0; k < 8; k++) {                      /* its table: handler k at CODE + 0x100 + 0x40 * k */
		mem_w32(&s.mem, DATA + 0x8000 + 4 * k, CODE + 0x100 + 0x40 * k);
		uint64_t d;
		double v = 1.0 + k / 8.0;
		memcpy(&d, &v, 8);
		mem_w64(&s.mem, DATA + 0x9000 + 8 * k, d);
	}
	struct ppc_cpu *c = &s.cpu;
	c->pc = CODE;
	c->gpr[24] = DATA - 2;
	c->gpr[25] = DATA + 0x3FFE;
	c->gpr[26] = DATA - 2;
	c->gpr[27] = DATA + 0x8000;
	c->gpr[28] = DATA + 0xA000;
	c->gpr[3] = DATA + 0x9000;
	c->gpr[4] = DATA + 0xB000;
	c->ctr = 1000;
	static const double fp[] = { 0, 0, 0, 0.5, 0, 0.25, 0, 0, 0.5 };
	for (unsigned k = 0; k < sizeof fp / sizeof fp[0]; k++)
		memcpy(&c->fpr[k], &fp[k], 8);
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (uint64_t left = insns; left;) {                    /* in the kernel's chunks */
		uint64_t chunk = left < 20000 ? left : 20000, before = c->icount;
		enum ppc_exit e = jit ? ppc_run(c, chunk) : ppc_interp(c, chunk);
		CHECK_EQ(e, PPC_EXIT_BUDGET);
		if (e != PPC_EXIT_BUDGET)
			break;
		left -= c->icount - before;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double secs = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
	jit_free(s.cpu.jit);
	mem_free(&s.mem);
	return (double)insns / secs;
}

static void bench(uint64_t insns)
{
	/* the 68k emulator's way: fetch an opcode, look its handler up, jump there; handlers jump back */
	uint32_t dispatch[0x300 / 4] = { 0 };
	const uint32_t loop[] = { LHZ(3, 2, 24) | 1u << 26, RLWINM(4, 3, 2, 27, 29), X_FORM(5, 4, 27, 23), MTCTR(5),
					 BCTR };
	const uint32_t handlers[8][8] = {
		{ ADDI(6, 6, 1), RLWIMI(7, 3, 8, 16, 23), CMPWI(1, 6, 100), ADD(8, 8, 3) },
		{ LWZ(9, 0x100, 28), ADDI(9, 9, 3), STW(9, 0x100, 28) },
		{ SUBF(10, 3, 10), SRAWI(11, 10, 2), X_FORM(3, 12, 11, 28) },
		{ X_FORM(0, 3, 6, 32), BGT(0, 8), ADDI(6, 6, -1) },
		{ RLWINM(14, 3, 24, 24, 31), X_FORM(15, 15, 14, 316), X_FORM(16, 16, 15, 444) },
		{ MULLW(17, 3, 6), MFCR(18), MTCRF(0x80, 18) },
		{ LHZ(19, 0, 24), STH(19, 0x200, 28) },
		{ X_FORM(0, 24, 25, 0), BC(12, 0, 8), ADDI(24, 26, 0) },
	};
	memcpy(dispatch, loop, sizeof loop);
	for (unsigned k = 0; k < 8; k++) {
		unsigned at = (0x100 + 0x40 * k) / 4, m = 0;
		while (m < 8 && handlers[k][m])
			dispatch[at + m] = handlers[k][m], m++;
		dispatch[at + m] = B(-4 * (int)(at + m));
	}
	/* compares, branches and arithmetic */
	const uint32_t integer[] = {
		ADDI(3, 3, 1), RLWINM(4, 3, 3, 0, 28), ADD(5, 5, 4), CMPWI(0, 5, 0), BC(12, 0, 8),
		enc_xo(5, 5, 0, 0, 104, 0), X_FORM(5, 6, 6, 316), SUBF(7, 6, 3), X_FORM(7, 8, 4, 28), BDNZ(-36),
		LIS(0, 0x7FFF), MTCTR(0), B(-48),
	};
	/* a copy loop */
	const uint32_t copy[] = {
		LWZ(6, 0, 3), STW(6, 0, 4), ADDI(3, 3, 4), ADDI(4, 4, 4), BDNZ(-16),
		LIS(3, DATA >> 16), LIS(4, (DATA + 0x8000) >> 16), LI(0, 0x1000), MTCTR(0), B(-36),
	};
	/* floating point, MathLib's way with the FPSCR */
	const uint32_t fpu[] = {
		LFD(1, 0, 3), FMADD(2, 2, 3, 1), FMUL(4, 2, 5), FMADD(6, 6, 8, 4), STFD(6, 8, 3), FSUB(9, 4, 2),
		MFFS(10), 63u << 26 | 0xFFu << 17 | 10u << 11 | 711u << 1, BDNZ(-32),
		LI(0, 1000), MTCTR(0), B(-44),
	};
	const struct { const char *name; const uint32_t *prog; unsigned n; } kernels[] = {
		{ "dispatch", dispatch, sizeof dispatch / 4 },
		{ "integer", integer, sizeof integer / 4 },
		{ "copy", copy, sizeof copy / 4 },
		{ "fp", fpu, sizeof fpu / 4 },
	};
	for (unsigned k = 0; k < sizeof kernels / sizeof kernels[0]; k++) {
		double interp = bench_run(kernels[k].prog, kernels[k].n, false, insns / 10);
		double jit = bench_run(kernels[k].prog, kernels[k].n, true, insns);
		printf("jit bench: %-8s interpreter %6.0f million/s, JIT %6.0f million/s (%.1fx)\n", kernels[k].name,
		       interp / 1e6, jit / 1e6, jit / interp);
	}
}

void test_jit(void)
{
	const char *n = getenv("BLUE_JIT_PROGRAMS"), *need = getenv("BLUE_TEST_JIT");
	unsigned programs = n ? (unsigned)strtoul(n, NULL, 0) : 3000;
	const char *speed = getenv("BLUE_JIT_BENCH");
	if (speed) {
		bench(strtoull(speed, NULL, 0) * 500000000ull);
		return;
	}
	if (need && !strcmp(need, "1")) {       /* (CI: a host that should have it) */
		struct mem m;
		CHECK_EQ(mem_init(&m), 0);
		struct jit *j = jit_create(&m);
		if (!j) {
			fprintf(stderr, "jit: BLUE_TEST_JIT=1, but %s\n", jit_unavailable());
			test_failures++;
		}
		jit_free(j);
		mem_free(&m);
	}
	test_quick_sc();
	test_kcalls();
	test_device_in_place();
	test_wrapping_ea();
	test_hostcall_fprs();
	test_random_programs(programs, false);
	test_random_programs(programs / 2 + 1, true);
	setenv("BLUE_JIT_FAR", "1", 1);         /* again, with the code far from the JIT's stubs */
	test_random_programs(programs / 4 + 1, false);
	unsetenv("BLUE_JIT_FAR");
	setenv("BLUE_JIT_FASTMEM", "0", 1);     /* and with every access checked */
	test_random_programs(programs / 4 + 1, false);
	unsetenv("BLUE_JIT_FASTMEM");
	test_logic_imm();
	test_x86_encoding();
}
