#define _GNU_SOURCE                     /* memfd_create() */
#include "mem.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#define PAGES (MEM_SPACE >> MEM_PAGE_SHIFT)

int mem_init(struct mem *m)
{
	memset(m, 0, sizeof *m);
	m->fd = -1;

	void *p = MAP_FAILED;
#ifdef __linux__
	int fd = memfd_create("syshm-guest", MFD_CLOEXEC);
	if (fd >= 0 && ftruncate(fd, (off_t)MEM_SPAN) == 0)
		p = mmap(NULL, MEM_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, fd, 0);
	if (p != MAP_FAILED)
		m->fd = fd;
	else if (fd >= 0)
		close(fd);
#endif
	if (p == MAP_FAILED)
		p = mmap(NULL, MEM_SPAN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED)
		return -1;

	m->pageflags = calloc(PAGES, 1);
	if (!m->pageflags) {
		munmap(p, MEM_SPAN);
		if (m->fd >= 0)
			close(m->fd);
		return -1;
	}
	m->base = p;
	return 0;
}

void mem_free(struct mem *m)
{
	if (m->base)
		munmap(m->base, MEM_SPAN);
	if (m->fd >= 0)
		close(m->fd);
	free(m->pageflags);
	memset(m, 0, sizeof *m);
	m->fd = -1;
}

uint8_t *mem_view(struct mem *m)
{
	void *p = MAP_FAILED;
	if (m->fd >= 0)
		p = mmap(NULL, MEM_SPAN, PROT_NONE, MAP_SHARED | MAP_NORESERVE, m->fd, 0);
#ifdef __APPLE__
	/* no copy: the alias shares the anonymous memory behind `base`, page protections apart */
	mach_vm_address_t alias = 0;
	vm_prot_t cur, max;
	if (p == MAP_FAILED &&
	    mach_vm_remap(mach_task_self(), &alias, MEM_SPAN, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
			  (mach_vm_address_t)(uintptr_t)m->base, FALSE, &cur, &max, VM_INHERIT_NONE) == KERN_SUCCESS) {
		p = (void *)(uintptr_t)alias;
		if (mprotect(p, MEM_SPAN, PROT_NONE)) {
			munmap(p, MEM_SPAN);
			p = MAP_FAILED;
		}
	}
#endif
	return p == MAP_FAILED ? NULL : p;
}

void mem_view_free(uint8_t *view)
{
	if (view)
		munmap(view, MEM_SPAN);
}

/* MEM_FAST_W and MEM_FAST_R for pages first - 1 to last */
static void update_fast(struct mem *m, uint64_t first, uint64_t last)
{
	for (uint64_t pg = first ? first - 1 : 0; pg <= last && pg < PAGES; pg++) {
		unsigned f = m->pageflags[pg], next = pg + 1 < PAGES ? m->pageflags[pg + 1] : 0;
		f &= ~(unsigned)(MEM_FAST_W | MEM_FAST_R);
		if ((f & (MEM_W | MEM_CODE)) == MEM_W && (next & (MEM_W | MEM_CODE)) == MEM_W)
			f |= MEM_FAST_W;
		if ((f & next & MEM_R))
			f |= MEM_FAST_R;
		m->pageflags[pg] = (uint8_t)f;
	}
}

void mem_map(struct mem *m, uint32_t addr, uint32_t size, unsigned flags)
{
	if (size == 0)
		return;
	uint64_t first = addr >> MEM_PAGE_SHIFT;
	uint64_t last = ((uint64_t)addr + size - 1) >> MEM_PAGE_SHIFT;
	if (last >= PAGES)
		last = PAGES - 1;
	mem_invalidate(m, addr, size);
	for (uint64_t pg = first; pg <= last; pg++)
		m->pageflags[pg] = (uint8_t)(flags & MEM_RWX);
	update_fast(m, first, last);
	if (m->flags_changed)
		m->flags_changed(m->code_ctx, (uint32_t)first, (uint32_t)(last - first + 1));
}

void mem_set_code(struct mem *m, uint32_t page, bool code)
{
	uint8_t old = m->pageflags[page];
	m->pageflags[page] = (uint8_t)(code ? old | MEM_CODE : old & ~MEM_CODE);
	if (m->pageflags[page] == old)
		return;
	update_fast(m, page, page);
	if (m->flags_changed)
		m->flags_changed(m->code_ctx, page, 1);
}

void mem_load(struct mem *m, uint32_t addr, const void *data, uint32_t size)
{
	mem_invalidate(m, addr, size);
	memcpy(m->base + addr, data, size);
}

void mem_invalidate(struct mem *m, uint32_t addr, uint32_t size)
{
	if (size == 0)
		return;
	uint64_t first = addr >> MEM_PAGE_SHIFT;
	uint64_t last = ((uint64_t)addr + size - 1) >> MEM_PAGE_SHIFT;
	uint64_t end = (uint64_t)addr + size;
	for (uint64_t pg = first; pg <= last && pg < PAGES; pg++) {
		if (!(m->pageflags[pg] & MEM_CODE))
			continue;
		uint64_t lo = pg << MEM_PAGE_SHIFT, hi = lo + MEM_PAGE_SIZE;
		if (lo < addr)
			lo = addr;
		if (hi > end)
			hi = end;
		if (m->code_changed)
			m->code_changed(m->code_ctx, (uint32_t)lo, (uint32_t)(hi - lo));
		else
			mem_set_code(m, (uint32_t)pg, false);
	}
}

bool mem_write_slow(struct mem *m, uint32_t addr, uint32_t len)
{
	if (!mem_ok(m, addr, len, MEM_W))
		return false;
	mem_invalidate(m, addr, len);
	return true;
}
