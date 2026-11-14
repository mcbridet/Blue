/*
 * Differential test of the JIT: random programs run once by the interpreter
 * and once by the JIT must end in exactly the same state (registers, exit
 * reason, instruction count, memory), whatever the budget, and with hooks. Environment:
 * SYSHM_JIT_PROGRAMS (default 3000), SYSHM_JIT_SEED (first program).
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

#include "../src/a64.h"
#include "../src/jit.h"
#include "../src/mem.h"
#include "../src/mmio.h"
#include "../src/ppc.h"
#include "../src/ppc_fp.h"
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
	unsigned c = pick(100);

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
		setenv("SYSHM_JIT_HOT", "0", 1);        /* translate at once: blocks here run a few times */
		s->cpu.jit = jit_create(&s->mem);
		unsetenv("SYSHM_JIT_HOT");
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
	const char *mode = getenv("SYSHM_JIT_FASTMEM");
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
	const char *first = getenv("SYSHM_JIT_SEED");
	uint64_t base = first ? strtoull(first, NULL, 0) : 0;

	for (unsigned p = 0; p < programs && failures < 3; p++) {
		uint64_t seed = 0x9E3779B97F4A7C15ull * (base + p + 1);
		rng = seed;
		uint32_t code[CODE_MAX + 1];
		unsigned n = 8 + pick(CODE_MAX - 8);
		for (unsigned k = 0; k < n; k++)
			code[k] = gen_insn(k, n);
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
		unsigned size = 1u << (31 - __builtin_clz((nrs >> 12 & 1) << 6 | (~nrs & 63)));
		if (!v || (nrs >> 6 & 63) >= size)     /* (larger rotations repeat the same values) */
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
		jit_free(s.cpu.jit);
		mem_free(&s.mem);
	}
}

void test_jit(void)
{
	const char *n = getenv("SYSHM_JIT_PROGRAMS");
	unsigned programs = n ? (unsigned)strtoul(n, NULL, 0) : 3000;
	test_quick_sc();
	test_kcalls();
	test_random_programs(programs, false);
	test_random_programs(programs / 2 + 1, true);
	setenv("SYSHM_JIT_FAR", "1", 1);        /* again, with the code far from the JIT's stubs */
	test_random_programs(programs / 4 + 1, false);
	unsetenv("SYSHM_JIT_FAR");
	setenv("SYSHM_JIT_FASTMEM", "0", 1);    /* and with every access checked */
	test_random_programs(programs / 4 + 1, false);
	unsetenv("SYSHM_JIT_FASTMEM");
	test_logic_imm();
}
