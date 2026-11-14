#ifndef SYSHM_MEM_H
#define SYSHM_MEM_H

#include <stdbool.h>
#include <stdint.h>

#include "be.h"

/*
 * Guest physical memory: one flat 4 GB host reservation, so a guest address is
 * just an offset from `base`. Validity is tracked per 4 KB guest page in
 * `pageflags`, independent of the host page size (16 KB on Apple silicon).
 * Where the host allows, the JIT maps the same memory a second time, with page
 * protections of its own (mem_view()): on Linux, `base` is a mapping of a memfd
 * (`fd`); on macOS, the view is a shared alias made with mach_vm_remap().
 */

#define MEM_PAGE_SHIFT 12
#define MEM_PAGE_SIZE (1u << MEM_PAGE_SHIFT)
#define MEM_SPACE (1ull << 32)
#define MEM_SLACK (1ull << 16)          /* so an access starting just below 4 GB stays mapped */
#define MEM_SPAN (MEM_SPACE + MEM_SLACK)

enum {
	MEM_R = 1,
	MEM_W = 2,
	MEM_X = 4,
	MEM_RW = MEM_R | MEM_W,
	MEM_RWX = MEM_R | MEM_W | MEM_X,
	MEM_RX = MEM_R | MEM_X,
	MEM_CODE = 8,           /* the JIT holds code translated from this page */
	/*
	 * Kept by mem_map() and mem_set_code(), for the JIT's checks: this page
	 * and the next are writable and hold no translated code (FAST_W), or
	 * are readable (FAST_R).
	 */
	MEM_FAST_W = 16,
	MEM_FAST_R = 32,
};

struct mem {
	uint8_t *base;
	uint8_t *pageflags;
	int fd;                 /* behind `base`, or -1 */
	/*
	 * Called before [addr, addr + len) changes, for each page with MEM_CODE
	 * (len stays within the page); it clears MEM_CODE when the page no
	 * longer holds translated code. Without it, MEM_CODE is just cleared.
	 */
	void (*code_changed)(void *ctx, uint32_t addr, uint32_t len);
	/* Called after the flags of `count` pages from `page` on changed */
	void (*flags_changed)(void *ctx, uint32_t page, uint32_t count);
	void *code_ctx;         /* for both */
};

int mem_init(struct mem *m);
void mem_free(struct mem *m);

/*
 * A second mapping of all of guest memory (MEM_SPAN bytes), sharing its
 * contents, with no access to begin with (mprotect() it page by page), or
 * NULL if the host can't make one. mem_view_free() unmaps it.
 */
uint8_t *mem_view(struct mem *m);
void mem_view_free(uint8_t *view);

/* Sets the access flags of every page overlapping [addr, addr + size). */
void mem_map(struct mem *m, uint32_t addr, uint32_t size, unsigned flags);

/* Sets or clears MEM_CODE on a page (all other flag changes go through mem_map()). */
void mem_set_code(struct mem *m, uint32_t page, bool code);

/* Copies host data into guest memory regardless of page flags. */
void mem_load(struct mem *m, uint32_t addr, const void *data, uint32_t size);

/* Tells the JIT that the host is about to change [addr, addr + size). */
void mem_invalidate(struct mem *m, uint32_t addr, uint32_t size);

__attribute__((cold)) bool mem_write_slow(struct mem *m, uint32_t addr, uint32_t len);

/* True if [addr, addr + len) is accessible with `need`; len must be <= 4096. */
static inline bool mem_ok(const struct mem *m, uint32_t addr, uint32_t len, unsigned need)
{
	uint32_t last = addr + len - 1;
	if (last < addr)
		return false;
	return (m->pageflags[addr >> MEM_PAGE_SHIFT] & need) == need &&
	       (m->pageflags[last >> MEM_PAGE_SHIFT] & need) == need;
}

/* mem_ok(MEM_W) for a guest store, which also invalidates translated code it overwrites. */
static inline bool mem_ok_w(struct mem *m, uint32_t addr, uint32_t len)
{
	uint32_t last = addr + len - 1;
	if (__builtin_expect(last >= addr && (m->pageflags[addr >> MEM_PAGE_SHIFT] & (MEM_W | MEM_CODE)) == MEM_W &&
			     (m->pageflags[last >> MEM_PAGE_SHIFT] & (MEM_W | MEM_CODE)) == MEM_W, 1))
		return true;
	return mem_write_slow(m, addr, len);
}

static inline uint8_t *mem_ptr(const struct mem *m, uint32_t addr)
{
	return m->base + addr;
}

static inline uint8_t mem_r8(const struct mem *m, uint32_t a) { return m->base[a]; }
static inline uint16_t mem_r16(const struct mem *m, uint32_t a) { return be16(m->base + a); }
static inline uint32_t mem_r32(const struct mem *m, uint32_t a) { return be32(m->base + a); }
static inline uint64_t mem_r64(const struct mem *m, uint32_t a) { return be64(m->base + a); }
static inline void mem_w8(struct mem *m, uint32_t a, uint8_t v) { m->base[a] = v; }
static inline void mem_w16(struct mem *m, uint32_t a, uint16_t v) { put_be16(m->base + a, v); }
static inline void mem_w32(struct mem *m, uint32_t a, uint32_t v) { put_be32(m->base + a, v); }
static inline void mem_w64(struct mem *m, uint32_t a, uint64_t v) { put_be64(m->base + a, v); }

#endif
