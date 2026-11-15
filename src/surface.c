#include "surface.h"

#include <stdlib.h>
#include <string.h>

#include "be.h"

bool surface_init(struct surface *s, int width, int height, struct gpu *gpu)
{
	memset(s, 0, sizeof *s);
	if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
		return false;
	size_t n = (size_t)width * (size_t)height;
	s->target.width = width;
	s->target.height = height;
	s->target.color = calloc(n, 4);
	s->target.depth = calloc(n, sizeof(float));
	s->target.stencil = calloc(n, 1);
	if (!s->target.color || !s->target.depth || !s->target.stencil) {
		surface_free(s);
		return false;
	}
	if (gpu) {
		s->gpu_target = gpu_target_new(gpu, width, height);
		s->gpu = s->gpu_target ? gpu : NULL;
	}
	return true;
}

void surface_wait(struct surface *s)
{
	raster_wait();
	if (s->gpu)
		gpu_wait(s->gpu);
}

void surface_lose_gpu(struct surface *s)
{
	if (s->gpu_target)
		gpu_target_free(s->gpu, s->gpu_target);
	s->gpu = NULL;
	s->gpu_target = NULL;
}

void surface_free(struct surface *s)
{
	surface_wait(s);
	surface_lose_gpu(s);
	free(s->target.color);
	free(s->target.depth);
	free(s->target.stencil);
	for (int k = 0; k < 2; k++) {
		free(s->batch[k].cmds);
		free(s->batch[k].states);
	}
	memset(s, 0, sizeof *s);
}

/* A batch drawn in the background, by the surface's backend */
static void draw_batch(struct surface *s, const struct surface_batch *b)
{
	if (!b->ncmds)
		return;
	if (s->gpu)
		gpu_start(s->gpu, s->gpu_target, b->states, b->cmds, b->ncmds);
	else
		raster_start(&s->target, b->states, b->cmds, b->ncmds);
}

/* The state for the next command: the last one recorded, unless it differs */
static bool push_state(struct surface_batch *b, const struct raster_state *st, unsigned *index)
{
	if (!b->nstates || memcmp(&b->states[b->nstates - 1], st, sizeof *st)) {
		if (b->nstates == b->states_cap) {
			unsigned cap = b->states_cap ? 2 * b->states_cap : 64;
			struct raster_state *n = realloc(b->states, cap * sizeof *n);
			if (!n)
				return false;
			b->states = n;
			b->states_cap = cap;
		}
		b->states[b->nstates++] = *st;
	}
	*index = b->nstates - 1;
	return true;
}

struct raster_cmd *surface_cmd(struct surface *s, const struct raster_state *state, uint8_t kind)
{
	struct surface_batch *b = &s->batch[s->rec];
	if (b->ncmds >= SURFACE_BATCH) {
		draw_batch(s, b);
		s->rec ^= 1;
		b = &s->batch[s->rec];
		b->ncmds = 0;           /* (drawing this one was waited for before the other started) */
		b->nstates = 0;
	}
	unsigned index;
	if (!push_state(b, state, &index))
		return NULL;
	if (b->ncmds == b->cmds_cap) {
		unsigned cap = b->cmds_cap ? 2 * b->cmds_cap : SURFACE_BATCH;
		struct raster_cmd *c = realloc(b->cmds, cap * sizeof *c);
		if (!c)
			return NULL;
		b->cmds = c;
		b->cmds_cap = cap;
	}
	struct raster_cmd *c = &b->cmds[b->ncmds++];
	memset(c, 0, sizeof *c);
	c->kind = kind;
	c->state = index;
	return c;
}

bool surface_pending(const struct surface *s)
{
	return s->batch[s->rec].ncmds != 0;
}

bool surface_busy(const struct surface *s)
{
	return s->batch[0].ncmds || s->batch[1].ncmds;
}

bool surface_finish(struct surface *s)
{
	draw_batch(s, &s->batch[s->rec]);
	surface_wait(s);
	for (int k = 0; k < 2; k++) {
		s->batch[k].ncmds = 0;
		s->batch[k].nstates = 0;
	}
	if (!s->gpu)
		return true;
	if (gpu_read(s->gpu, s->gpu_target, s->target.color))
		return true;
	surface_lose_gpu(s);
	return false;
}

bool surface_read_depth(struct surface *s)
{
	if (!s->gpu || gpu_read_depth(s->gpu, s->gpu_target, s->target.depth, s->target.stencil))
		return true;
	surface_lose_gpu(s);
	return false;
}

/* ---- guest memory --------------------------------------------------------------- */

/* Whether [addr, addr + len) can be read or (with translated code there dropped) written: page by page */
static bool guest_ok(struct mem *m, uint32_t addr, uint32_t len, bool write)
{
	if (!len)
		return true;
	if (addr + len - 1 < addr)
		return false;
	while (len) {
		uint32_t n = MEM_PAGE_SIZE - (addr & (MEM_PAGE_SIZE - 1));
		n = n < len ? n : len;
		if (write ? !mem_ok_w(m, addr, n) : !mem_ok(m, addr, n, MEM_R))
			return false;
		addr += n;
		len -= n;
	}
	return true;
}

/* ---- QuickDraw regions ------------------------------------------------------------- */

#define REGION_POINTS 256               /* inversion points on a row (more: its bounding box) */

/*
 * A region's rows, one after another going down. A region is its size, its
 * bounding box, then (unless it's that rectangle) the rows where it
 * changes, each its y, the x where insideness flips, and 32767; then
 * 32767. A pixel is inside if an odd number of the flips at or above its
 * row are at or left of it.
 */
struct region {
	struct mem *m;
	int top, left, bottom, right;
	bool rect;
	uint32_t at, end;               /* the rows' changes not yet applied */
	int16_t x[REGION_POINTS];       /* the flips so far, in order */
	unsigned n;
};

static bool region_open(struct region *r, struct mem *m, uint32_t handle)
{
	memset(r, 0, sizeof *r);
	r->m = m;
	if (!mem_ok(m, handle, 4, MEM_R))
		return false;
	uint32_t p = mem_r32(m, handle);
	if (!mem_ok(m, p, 10, MEM_R))
		return false;
	uint32_t size = mem_r16(m, p);
	r->top = (int16_t)mem_r16(m, p + 2);
	r->left = (int16_t)mem_r16(m, p + 4);
	r->bottom = (int16_t)mem_r16(m, p + 6);
	r->right = (int16_t)mem_r16(m, p + 8);
	r->rect = size <= 10 || !guest_ok(m, p, size, false);
	r->at = p + 10;
	r->end = p + size;
	return true;
}

/* Flips at these x (in order) toggled into the row's */
static void region_flip(struct region *r, const int16_t *x, unsigned n)
{
	int16_t out[REGION_POINTS];
	unsigned i = 0, j = 0, k = 0;
	while ((i < r->n || j < n) && k < REGION_POINTS) {
		if (j == n || (i < r->n && r->x[i] < x[j])) {
			out[k++] = r->x[i++];
		} else if (i == r->n || x[j] < r->x[i]) {
			out[k++] = x[j++];
		} else {                        /* (the same x twice: no flip) */
			i++;
			j++;
		}
	}
	if (i < r->n || j < n) {                /* (too many: the bounding box from here) */
		r->rect = true;
		return;
	}
	memcpy(r->x, out, k * sizeof *out);
	r->n = k;
}

/* Row y's spans [x0, x1) as pairs in `spans` (rows asked for going down); returns how many */
static unsigned region_row(struct region *r, int y, int *spans, unsigned max)
{
	if (y < r->top || y >= r->bottom || r->left >= r->right)
		return 0;
	if (!r->rect) {
		while (r->at + 2 <= r->end) {
			int ry = (int16_t)mem_r16(r->m, r->at);
			if (ry == 32767 || ry > y)
				break;
			int16_t x[REGION_POINTS];
			unsigned n = 0;
			uint32_t p = r->at + 2;
			while (p + 2 <= r->end && mem_r16(r->m, p) != 32767) {
				if (n < REGION_POINTS)
					x[n++] = (int16_t)mem_r16(r->m, p);
				p += 2;
			}
			r->at = p + 2;
			region_flip(r, x, n);
			if (r->rect)
				break;
		}
	}
	if (r->rect) {
		spans[0] = r->left;
		spans[1] = r->right;
		return 1;
	}
	unsigned k = 0;
	for (unsigned i = 0; i + 1 < r->n && k < max; i += 2, k++) {
		spans[2 * k] = r->x[i];
		spans[2 * k + 1] = r->x[i + 1];
	}
	return k;
}

/* The spans in both lists */
static unsigned intersect(const int *a, unsigned na, const int *b, unsigned nb, int *out, unsigned max)
{
	unsigned i = 0, j = 0, k = 0;
	while (i < na && j < nb && k < max) {
		int l = a[2 * i] > b[2 * j] ? a[2 * i] : b[2 * j];
		int r = a[2 * i + 1] < b[2 * j + 1] ? a[2 * i + 1] : b[2 * j + 1];
		if (l < r) {
			out[2 * k] = l;
			out[2 * k + 1] = r;
			k++;
		}
		if (a[2 * i + 1] < b[2 * j + 1])
			i++;
		else
			j++;
	}
	return k;
}

/* ---- presenting ------------------------------------------------------------------- */

static void put_pixels(uint8_t *p, const uint32_t *src, int n, unsigned bits, bool alpha)
{
	for (int k = 0; k < n; k++) {
		uint32_t c = src[k];
		if (bits == 16)
			put_be16(p + 2 * k, (uint16_t)((c >> 9 & 0x7C00) | (c >> 6 & 0x03E0) | (c >> 3 & 0x001F)));
		else
			put_be32(p + 4 * k, alpha ? c : c & 0x00FFFFFFu);
	}
}

void surface_present(const struct surface *s, struct mem *m, const struct surface_dest *d, int l, int t, int r,
		     int b)
{
	/* within the surface, and the destination's writable part */
	l = l > 0 ? l : 0;
	t = t > 0 ? t : 0;
	r = r < s->target.width ? r : s->target.width;
	b = b < s->target.height ? b : s->target.height;
	l = l > d->left - d->x ? l : d->left - d->x;
	t = t > d->top - d->y ? t : d->top - d->y;
	r = r < d->right - d->x ? r : d->right - d->x;
	b = b < d->bottom - d->y ? b : d->bottom - d->y;
	if (l >= r || t >= b || (d->bits != 16 && d->bits != 32))
		return;
	struct region rg[2];
	unsigned nregions = 0;
	for (int k = 0; k < 2; k++)
		if (d->region[k] && region_open(&rg[nregions], m, d->region[k]))
			nregions++;
	const unsigned bpp = d->bits / 8;
	enum { MAX_SPANS = REGION_POINTS / 2 };
	for (int y = t; y < b; y++) {
		int spans[2 * MAX_SPANS] = { l, r }, row[2 * MAX_SPANS], both[2 * MAX_SPANS];
		unsigned n = 1;
		for (unsigned k = 0; k < nregions && n; k++) {
			unsigned nr = region_row(&rg[k], y + d->region_y, row, MAX_SPANS);
			for (unsigned i = 0; i < nr; i++) {
				row[2 * i] -= d->region_x;
				row[2 * i + 1] -= d->region_x;
			}
			n = intersect(spans, n, row, nr, both, MAX_SPANS);
			memcpy(spans, both, 2 * n * sizeof *spans);
		}
		for (unsigned i = 0; i < n; i++) {
			int x0 = spans[2 * i], x1 = spans[2 * i + 1];
			uint32_t addr = d->base + (uint32_t)(d->y + y) * d->row_bytes + (uint32_t)(d->x + x0) * bpp;
			uint32_t len = (uint32_t)(x1 - x0) * bpp;
			if (!guest_ok(m, addr, len, true))
				continue;
			put_pixels(mem_ptr(m, addr), s->target.color + (size_t)y * (size_t)s->target.width + (size_t)x0,
				   x1 - x0, d->bits, d->alpha);
		}
	}
}
