#define _GNU_SOURCE                     /* dladdr(), for the profiler */
#include "jit_int.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const char *unavailable = "no JIT on this host";

const char *jit_unavailable(void)
{
	return unavailable;
}

#ifdef JIT_HOST

#include <signal.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#endif

#include "ppc_fp.h"

/* sampling profiler (see below) */
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
	const uint64_t *icount;         /* samples only from instruction `from` to `to` */
	uint64_t from, to;
} prof;

/* ---- blocks ----------------------------------------------------------- */

/* The hook's table entry stops branches to it (its block is found through the page) */
static void set_hook_entry(struct jit *j)
{
	struct jit_page *pg;
	if (j->linked_hook != PPC_NO_HOOK && (pg = jit_get_page(j, j->linked_hook >> MEM_PAGE_SHIFT)))
		pg->l2[(j->linked_hook >> 2) & (PAGE_WORDS - 1)] = j->hook_exit;
}

void jit_flush(struct jit *j)
{
	for (uint32_t k = 0; k < j->ncode_pages; k++) {
		uint32_t pg = j->code_pages[k];
		j->pages[pg] = NULL;
		j->l1[pg] = l1_entry(j->l2_empty, pg);
		mem_set_code(j->mem, pg, false);
	}
	j->ncode_pages = 0;
	j->nblocks = 0;
	j->nsites = 0;
	j->code_free = j->blocks_start;
	j->flushes++;
	set_hook_entry(j);
}

struct jit_page *jit_get_page(struct jit *j, uint32_t page)
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
	j->l1[page] = l1_entry(pg->l2, page);
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

struct jit_block *jit_add_block(struct jit *j, struct jit_page *pg, uint32_t pc, uint32_t ninsn, uint32_t cover,
				void *code, void *end, void *redispatch)
{
	j->code_free = end;
	struct jit_block *b = &j->blocks[j->nblocks++];
	*b = (struct jit_block){ pc, ninsn, cover, code, redispatch, pg->blocks };
	memcpy((uint8_t *)code - sizeof b, &b, sizeof b);
	pg->blocks = b;
	set_words(pg, (pc & (MEM_PAGE_SIZE - 1)) / 4, cover);
	if (pg->l2[(pc >> 2) & (PAGE_WORDS - 1)] != j->hook_exit)
		pg->l2[(pc >> 2) & (PAGE_WORDS - 1)] = code;
	mem_set_code(j->mem, pc >> MEM_PAGE_SHIFT, true);
	j->translated++;
	return b;
}

/* Takes a block out of the table, and sends branches into it back to C */
static void retire(struct jit *j, struct jit_page *pg, struct jit_block *b)
{
	unsigned first = (b->pc & (MEM_PAGE_SIZE - 1)) / 4;
	if (pg->l2[first] == b->code)
		pg->l2[first] = j->miss_exit;
	arch_retire(j, b);
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
			jit_flush(j);
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
 * processor, in the mode it ran in (round to nearest, no exceptions
 * enabled), for its FR, FI and FPRF; the rest of the FPSCR (XX and FX,
 * already right for it) may have changed since (see fp_fpscr()).
 */
static void fp_settle(struct jit *j, struct ppc_cpu *c)
{
	uint32_t code = j->st.fp_op;
	if (!code)
		return;
	static struct ppc_cpu scratch;
	scratch.fpscr = c->fpscr & ~0xFFu;
	memcpy(&scratch.fpr[1], j->st.fp_ops, sizeof j->st.fp_ops);
	ppc_fp_exec(&scratch, (code & 0x20 ? 59u : 63u) << 26 | 1u << 16 | 2u << 11 | 3u << 6 | (code & 31) << 1);
										/* (f0 = f1 op f2, f3) */
	c->fpscr = (c->fpscr & ~FPSCR_PENDING) | (scratch.fpscr & FPSCR_PENDING);
	if (j->st.fp_fpcc)
		c->fpscr = (c->fpscr & ~0xF000u) | (j->st.fp_fpcc & 0xFu) << 12;
	j->st.fp_op = j->st.fp_fpcc = 0;
}

/* For the settle stub (see build_settle()) */
static void settle(struct ppc_cpu *c)
{
	fp_settle(c->jit, c);
}

/* The interpreter's step for generated code, which may read the FPSCR */
static bool step(struct ppc_cpu *c, uint32_t insn, uint32_t pc)
{
	fp_settle(c->jit, c);
	return ppc_step(c, insn, pc);
}

/* The kernel's call in place (see ppc.h), which may read and set the FPSCR */
static bool kcall(struct ppc_cpu *c, unsigned n)
{
	fp_settle(c->jit, c);
	return c->kcall(c, n);
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
bool jit_rom_like(struct jit *j, uint32_t pc)
{
	if ((j->mem->pageflags[pc >> MEM_PAGE_SHIFT] & (MEM_X | MEM_W)) != MEM_X)
		return false;
	struct jit_page *pg = jit_get_page(j, pc >> MEM_PAGE_SHIFT);
	if (!pg)
		return false;
	pg->followed = true;
	mem_set_code(j->mem, pc >> MEM_PAGE_SHIFT, true);
	return true;
}

/*
 * The block at pc, translated if need be; or NULL, and then the interpreter
 * runs the block's instructions: if they aren't executable, or until they
 * have run `hot` times (the 68k emulator rewrites much of its translation
 * cache before it runs twice, and translating costs more than that).
 */
static struct jit_block *find(struct jit *j, uint32_t pc)
{
	const void *code = *table_entry(j, pc);
	if (code != j->miss_exit && code != j->hook_exit) {
		struct jit_block *b;
		memcpy(&b, (const uint8_t *)code - sizeof b, sizeof b);
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
	struct jit_page *pg = jit_get_page(j, pc >> MEM_PAGE_SHIFT);
	if (pg && pg->runs[(pc >> 2) & (PAGE_WORDS - 1)] < j->hot) {
		pg->runs[(pc >> 2) & (PAGE_WORDS - 1)]++;
		return NULL;
	}
	return arch_translate(j, pc);
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

static void fault_handler(int sig, siginfo_t *si, void *ctx)
{
	uintptr_t pc = arch_context_pc(ctx), addr = (uintptr_t)si->si_addr;

	for (unsigned k = 0; k < MAX_JITS; k++) {
		struct jit *j = live_jits[k];
		if (!j || !j->view || pc < (uintptr_t)j->blocks_start || pc >= (uintptr_t)j->code_free ||
		    addr - (uintptr_t)j->view >= MEM_SPAN)
			continue;
		uint32_t at = (uint32_t)(pc - (uintptr_t)j->code), lo = 0, hi = j->nsites;
		while (lo < hi) {
			uint32_t mid = lo + (hi - lo) / 2;
			if (j->sites[mid].at < at)
				lo = mid + 1;
			else
				hi = mid;
		}
		if (lo < j->nsites && j->sites[lo].at == at) {
			arch_set_context_pc(ctx, j->code + j->sites[lo].stub);
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
		jit_flush(j);
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
	struct jit_page *pg = jit_get_page(j, s->pc >> MEM_PAGE_SHIFT);
	unsigned w = (s->pc >> 2) & (PAGE_WORDS - 1);
	if (pg) {
		pg->slow[w / 64] |= 1ull << (w % 64);
		if (!(j->mem->pageflags[j->fault_ea >> MEM_PAGE_SHIFT] & MEM_R))
			pg->io[w / 64] |= 1ull << (w % 64);
	}
	drop_block(j, &j->blocks[s->block]);
	j->slowed++;
}

static void prof_start(struct jit *j, const char *window);

struct jit *jit_create(struct mem *mem)
{
	struct jit *j = arch_alloc(&unavailable);
	if (!j)
		return NULL;
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
	if (pages == MAP_FAILED || blocks == MAP_FAILED || l1 == MAP_FAILED ||
	    l2 == MAP_FAILED || !j->code_pages || !j->page_pool) {
		if (l1 != MAP_FAILED)
			munmap(l1, GUEST_PAGES * sizeof(void **));
		if (l2 != MAP_FAILED)
			munmap(l2, ((size_t)MAX_CODE_PAGES + 1) * PAGE_WORDS * sizeof(void *));
		if (pages != MAP_FAILED)
			munmap(pages, GUEST_PAGES * sizeof(struct jit_page *));
		if (blocks != MAP_FAILED)
			munmap(blocks, MAX_BLOCKS * sizeof(struct jit_block));
		free(j->code_pages);
		free(j->page_pool);
		arch_release(j);
		unavailable = "no JIT: no memory for its tables";
		return NULL;
	}
	j->mem = mem;
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
		.settle = settle,
		.kcall = kcall,
		.code_store = code_store,
		.mmio = mmio,
		.pages = j->pages,
		.fastbase = mem->base,
	};
	arch_init(j);
	for (unsigned w = 0; w < PAGE_WORDS; w++)
		j->l2_empty[w] = j->miss_exit;
	for (uint32_t pg = 0; pg < GUEST_PAGES; pg++)
		j->l1[pg] = l1_entry(j->l2_empty, pg);
	j->linked_hook = PPC_NO_HOOK;
	j->match_pc = PPC_NO_HOOK;
	if (getenv("SYSHM_JIT_FAR"))            /* tests: blocks out of short branch range of the stubs */
		j->blocks_start += CODE_SIZE - (8u << 20);
	jit_flush(j);
	j->flushes = 0;
	const char *hot = getenv("SYSHM_JIT_HOT");
	j->hot = hot ? (unsigned)strtoul(hot, NULL, 0) : 16;
	if (j->hot > 255)
		j->hot = 255;
	mem->code_changed = code_changed;
	mem->flags_changed = flags_changed;
	mem->code_ctx = j;
	fastmem_init(j);
	const char *window = getenv("SYSHM_JIT_PROFILE");
	if (window)
		prof_start(j, window);
	return j;
}

void jit_free(struct jit *j)
{
	if (!j)
		return;
	jit_flush(j);
	j->mem->code_changed = NULL;
	j->mem->flags_changed = NULL;
	fastmem_free(j);
	munmap(j->pages, GUEST_PAGES * sizeof(struct jit_page *));
	munmap(j->blocks, MAX_BLOCKS * sizeof(struct jit_block));
	munmap(j->l1, GUEST_PAGES * sizeof(void **));
	munmap(j->l2_empty, ((size_t)MAX_CODE_PAGES + 1) * PAGE_WORDS * sizeof(void *));
	free(j->code_pages);
	free(j->page_pool);
	arch_release(j);
}

enum ppc_exit jit_run(struct jit *j, struct ppc_cpu *c, uint64_t budget)
{
	if (c->hook_pc != j->linked_hook || c->hook_match_pc != j->match_pc || c->hook_match_reg != j->match_reg ||
	    c->hook_match_value != j->match_value) {    /* links may lead past the old hooks */
		j->linked_hook = c->hook_pc;
		j->match_pc = c->hook_match_pc;
		j->match_reg = c->hook_match_reg;
		j->match_value = c->hook_match_value;
		jit_flush(j);
	}
	j->st.hook = c->hook_pc;
	if (prof.j == j)
		prof.icount = &c->icount;
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
			jit_flush(j);
			continue;
		}
		j->st.code_changed = 0;
		arch_sync(j);
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
			void *site = j->st.link_site;
			unsigned long flushes = j->flushes;
			struct jit_block *next = find(j, c->pc);
			if (next && j->flushes == flushes && c->pc != j->linked_hook) {
				arch_link(j, site, next->code);
				j->links++;
			}
			break;
		}
		}
	}
	return PPC_EXIT_BUDGET;
}

/* ---- sampling profiler (SYSHM_JIT_PROFILE=1) ------------------------------- */


static void prof_signal(int sig, siginfo_t *si, void *ctx)
{
	(void)sig;
	(void)si;
	uintptr_t pc = arch_context_pc(ctx);
	struct jit *j = prof.j;
	struct prof_sample smp = { 0, 0, pc };
	if (!prof.icount || *prof.icount < prof.from || *prof.icount >= prof.to)
		return;

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

static void prof_start(struct jit *j, const char *window)
{
	char *end;
	prof.j = j;
	prof.from = strtoull(window, &end, 0);
	prof.to = *end == ':' ? strtoull(end + 1, NULL, 0) : UINT64_MAX;
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
		struct jit_mark marks[8];
		unsigned nm = arch_marks(prof.j, marks);
		for (unsigned k = 0; k < prof.n; k++) {
			uintptr_t pc = prof.s[k].host_pc;
			if (prof.s[k].guest_pc || pc < (uintptr_t)prof.j->code || pc >= (uintptr_t)prof.j->blocks_start)
				continue;
			unsigned m = 0;
			while (m + 1 < nm && pc >= (uintptr_t)marks[m + 1].from)
				m++;
			st[m]++;
		}
		fprintf(f, "profile: stubs:");
		for (unsigned m = 0; m < nm; m++)
			fprintf(f, " %s %lu", marks[m].name, st[m]);
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
		(size_t)(j->code_free - j->blocks_start) / 1024, j->links, j->invalidations, j->flushes,
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
