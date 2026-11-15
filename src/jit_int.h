/*
 * What the JIT's runtime (jit.c) and its translator for the host
 * (jit_a64.c, jit_x86.c) share. The runtime keeps the block table, the
 * pages blocks were translated from and their invalidation, links,
 * fastmem and its fault sites, the C functions generated code calls, the
 * run loop, statistics and the profiler; a translator makes the code.
 */
#ifndef SYSHM_JIT_INT_H
#define SYSHM_JIT_INT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jit.h"

#if defined(__aarch64__) && (defined(__linux__) || defined(__APPLE__))
#define JIT_A64 1
#elif defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))
#define JIT_X86 1
#endif

#if defined(JIT_A64) || defined(JIT_X86)
#define JIT_HOST 1
#endif

#ifdef JIT_HOST

#define CODE_SIZE (64u << 20)
#define MAX_BLOCK_INSNS 128u
#define GUEST_PAGES (1u << (32 - MEM_PAGE_SHIFT))
#define MAX_BLOCKS (1u << 20)
#define MAX_CODE_PAGES (1u << 16)
#define MAX_SITES (1u << 22)
#define FAST_W_BIT 4                    /* MEM_FAST_W */
#define FAST_R_BIT 5                    /* MEM_FAST_R */

/* Why generated code returned to C */
enum { EXIT_MISS, EXIT_BUDGET, EXIT_INTERP, EXIT_HOOK, EXIT_LINK, EXIT_TRAP, EXIT_SC, EXIT_HOSTCALL, EXIT_MMIO,
       EXIT_KINDS };

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
	void *redispatch;
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

/* An access in generated code that may fault, and its side exit (offsets in bytes from `code`) */
struct jit_site {
	uint32_t at, stub;
	uint32_t pc;                            /* guest instruction */
	uint32_t block;                         /* index in blocks */
};

/* Read and written by generated code */
struct jit_state {
	uint8_t *membase;
	uint8_t *pageflags;
	const void ***l1;
	uint64_t remaining;
	bool (*step)(struct ppc_cpu *, uint32_t, uint32_t);
	void (*settle)(struct ppc_cpu *);
	bool (*kcall)(struct ppc_cpu *, unsigned);
	void (*code_store)(struct jit_state *, uint32_t, uint64_t, uint32_t);
	unsigned (*mmio)(struct ppc_cpu *, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t);
	void *link_site;                /* EXIT_LINK: the branch to point at the next block */
	struct jit_page **pages;
	uint32_t hook;
	uint32_t code_changed;          /* set by invalidations; checked after the interpreter's stores */
	uint8_t *fastbase;              /* guest address 0 for generated code (see fastmem) */
	uint32_t fault_site;            /* set by the fault handler: site + 1 */
	uint64_t entry_budget;          /* the budget when generated code was entered */
	/*
	 * FP arithmetic whose FR, FI and FPRF are still to be worked out (see
	 * fp_settle()): FP_PENDING(), or 0; 0x10 | FPCC if a compare has set
	 * FPCC since, or 0; the instruction's operands (a, b, c).
	 */
	uint32_t fp_op, fp_fpcc;
	uint64_t fp_ops[3];
	uint32_t scratch;
	uint64_t mmio_return;           /* (the mmio stub's) */
	uint32_t mmio_exit;             /* how generated code leaves after an access the hook did */
};

/* An opcode-59 or 63 operation in fp_op: 16 bits, nonzero */
#define FP_PENDING(op, xo5) (0x8000u | ((op) == 59 ? 0x20u : 0) | (xo5))

#define FPSCR_FR_FI (3u << 17)
#define FPSCR_PENDING (FPSCR_FR_FI | 0x1Fu << 12)     /* what FP arithmetic leaves pending: FR, FI, FPRF */

/* (generated code clears fp_op and fp_fpcc with one 64-bit store) */
_Static_assert(offsetof(struct jit_state, fp_op) % 8 == 0 &&
	       offsetof(struct jit_state, fp_fpcc) == offsetof(struct jit_state, fp_op) + 4, "jit_state layout");

struct jit {
	struct jit_state st;            /* (first: code_store() finds the jit from it) */
	struct mem *mem;
	uint8_t *code, *code_end, *code_free, *blocks_start;
	uint32_t (*enter)(struct ppc_cpu *, const void *, uint64_t, struct jit_state *);
	const void *hook_exit, *miss_exit;      /* (see l1) */
	uint32_t linked_hook;           /* blocks are never linked to this pc */
	/* the match hook blocks are translated for: only a block of its own runs its pc, checking first */
	uint32_t match_pc;
	uint16_t match_value;
	uint8_t match_reg;
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

#ifdef JIT_A64
	const uint32_t *exit, *side_exit, *link, *link_branch, *lookup, *mmio_call, *settle;
	bool sync_pending;              /* see icache_sync() */
#else
	const uint8_t *exit, *side_exit, *link, *link_branch, *lookup;
#endif
};

/* ---- the runtime, for translators (jit.c) ---- */

/* The page's record, made if need be; NULL if there's no room (flush first) */
struct jit_page *jit_get_page(struct jit *j, uint32_t page);

/* Whether blocks may follow code into the page of pc without registering there (see jit.c) */
bool jit_rom_like(struct jit *j, uint32_t pc);

void jit_flush(struct jit *j);

/*
 * Registers a block translated from pc (into pg) at `code`, which a
 * pointer to the block will precede, ending at `end`: in the table, its
 * page and the words it covers
 */
struct jit_block *jit_add_block(struct jit *j, struct jit_page *pg, uint32_t pc, uint32_t ninsn, uint32_t cover,
				void *code, void *end, void *redispatch);

/* An access at `at` that may fault, with its side exit at `stub`, for instruction pc of the next block */
static inline void jit_add_site(struct jit *j, const void *at, const void *stub, uint32_t pc)
{
	j->sites[j->nsites++] = (struct jit_site){ (uint32_t)((const uint8_t *)at - j->code),
						   (uint32_t)((const uint8_t *)stub - j->code), pc, j->nblocks };
}

/* ---- the translator, for the runtime (jit_a64.c, jit_x86.c) ---- */

/* struct jit (zeroed) and its code buffer (code, code_end); NULL, with why, if this host can't */
struct jit *arch_alloc(const char **why);
void arch_release(struct jit *j);

/* The stubs (enter, miss_exit, hook_exit, blocks_start...) */
void arch_init(struct jit *j);

/* The block at pc, translated; NULL if it can't be */
struct jit_block *arch_translate(struct jit *j, uint32_t pc);

/* An exit to a known pc (st.link_site) now goes straight to `to` */
void arch_link(struct jit *j, void *site, const void *to);

/* The block's first instruction now goes to its redispatch code */
void arch_retire(struct jit *j, struct jit_block *b);

/* Before generated code runs again */
void arch_sync(struct jit *j);

/* The pc in a signal's context, and moving it */
uintptr_t arch_context_pc(void *ctx);
void arch_set_context_pc(void *ctx, const void *pc);

/* The stubs' regions, for the profiler: the first from `code`, in order */
struct jit_mark {
	const char *name;
	const void *from;
};

unsigned arch_marks(const struct jit *j, struct jit_mark marks[8]);

#endif
#endif
