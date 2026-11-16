#include "dmg.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "be.h"
#include "rsrc.h"

#include "../third_party/bzip2/bzlib.h"
#include "../third_party/libchdr/deps/miniz-3.1.2/miniz.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include "../third_party/lzfse/lzfse.h"
#pragma GCC diagnostic pop
#include "../third_party/lzma/Lzma2Dec.h"

#define SECTOR 512u
#define CACHE 4u                        /* decompressed chunks kept */
#define MAX_CHUNK (64u << 20)           /* a chunk's bytes, decompressed (hdiutil's are 1 MB at most) */
#define MAX_FORK (64u << 20)            /* a resource fork's, or the XML property list's */
#define MACBINARY 128u

/* Chunk kinds: UDIF's codes (NDIF's are made into these) */
#define ZERO 0x00000000u
#define RAW 0x00000001u
#define IGNORE 0x00000002u              /* (zeros: free space) */
#define COMMENT 0x7FFFFFFEu
#define ADC 0x80000004u
#define ZLIB 0x80000005u
#define BZIP2 0x80000006u
#define LZFSE 0x80000007u
#define XZ 0x80000008u
#define END 0xFFFFFFFFu

struct chunk {
	uint64_t sector, count;         /* where it goes in the image (512-byte sectors) */
	uint64_t at, len;               /* where its data is in the file */
	uint32_t kind;
};

struct dmg {
	int fd;
	uint64_t size;
	struct chunk *chunk;
	size_t chunks;
	struct {
		size_t chunk;           /* (SIZE_MAX: none) */
		uint8_t *data;
		unsigned long used;
	} cache[CACHE];
	unsigned long tick;
	uint8_t *in;                    /* a chunk's compressed data */
	size_t in_cap;
	char kind[80];
};

/* Where an image's forks are */
struct forks {
	uint64_t data, data_len;        /* the data fork, in the file */
	int rfd;                        /* the resource fork's file (-1: none), ... */
	bool own_rfd;                   /* ... opened here (not the image's own) */
	uint64_t rsrc, rsrc_len;
};

static bool read_at(int fd, void *buf, size_t len, uint64_t at)
{
	return pread(fd, buf, len, (off_t)at) == (ssize_t)len;
}

static int fail(char *why, size_t len, const char *what)
{
	snprintf(why, len, "%s", what);
	return -1;
}

/* ---- the forks ------------------------------------------------------------ */

static uint16_t crc16_xmodem(const uint8_t *p, size_t n)
{
	uint16_t crc = 0;
	for (size_t k = 0; k < n; k++) {
		crc ^= (uint16_t)(p[k] << 8);
		for (int b = 0; b < 8; b++)
			crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return crc;
}

/* A MacBinary header (I, II or III): its forks */
static bool macbinary(const uint8_t *h, uint64_t size, struct forks *f)
{
	if (h[0] || h[1] < 1 || h[1] > 63 || h[74] || h[82])
		return false;
	uint64_t dlen = be32(h + 83), rlen = be32(h + 87), rsrc = MACBINARY + ((dlen + 127) & ~(uint64_t)127);
	if (dlen > size - MACBINARY || (rlen && (rsrc > size || rlen > size - rsrc)))
		return false;
	bool ii = be16(h + 124) == crc16_xmodem(h, 124);
	for (unsigned k = 99; k < 126 && !ii; k++)
		if (h[k])
			return false;   /* (MacBinary I has zeros there) */
	f->data = MACBINARY;
	f->data_len = dlen;
	f->rsrc = rsrc;
	f->rsrc_len = rlen;
	return true;
}

/* An AppleSingle or AppleDouble file's resource fork (and an AppleSingle's data fork) */
static bool apple_double(int fd, uint64_t size, uint32_t magic, struct forks *f, bool data)
{
	uint8_t h[26], e[12];
	if (size < sizeof h || !read_at(fd, h, sizeof h, 0) || be32(h) != magic)
		return false;
	unsigned n = be16(h + 24);
	bool found = false;
	for (unsigned k = 0; k < n && k < 64; k++) {
		if (!read_at(fd, e, sizeof e, 26 + 12 * (uint64_t)k))
			return false;
		uint64_t at = be32(e + 4), len = be32(e + 8);
		if (at > size || len > size - at)
			continue;
		if (be32(e) == 2) {
			f->rsrc = at;
			f->rsrc_len = len;
			found = true;
		} else if (be32(e) == 1 && data) {
			f->data = at;
			f->data_len = len;
		}
	}
	return found || data;
}

static int open_regular(const char *path, uint64_t *size)
{
	struct stat st;
	int fd = open(path, O_RDONLY);
	if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode))) {
		close(fd);
		fd = -1;
	}
	if (fd >= 0)
		*size = (uint64_t)st.st_size;
	return fd;
}

/* A resource fork kept beside the data fork: AppleDouble (._NAME, .AppleDouble/NAME), or macOS's named fork */
static void fork_beside(const char *path, struct forks *f)
{
	const char *slash = strrchr(path, '/');
	int dir = slash ? (int)(slash - path + 1) : 0;
	char p[4096];
	uint64_t size;
	for (int k = 0; k < 3; k++) {
		if (k == 0)
			snprintf(p, sizeof p, "%.*s._%s", dir, path, path + dir);
		else if (k == 1)
			snprintf(p, sizeof p, "%.*s.AppleDouble/%s", dir, path, path + dir);
		else
			snprintf(p, sizeof p, "%s/..namedfork/rsrc", path);
		int fd = open_regular(p, &size);
		if (fd < 0)
			continue;
		if (k == 2 ? size > 0 : apple_double(fd, size, 0x00051607u, f, false)) {
			if (k == 2) {
				f->rsrc = 0;
				f->rsrc_len = size;
			}
			f->rfd = fd;
			f->own_rfd = true;
			return;
		}
		close(fd);
	}
}

static void find_forks(int fd, const char *path, uint64_t size, struct forks *f)
{
	uint8_t h[MACBINARY];
	memset(f, 0, sizeof *f);
	f->data_len = size;
	f->rfd = -1;
	if (size >= MACBINARY && read_at(fd, h, sizeof h, 0)) {
		if (macbinary(h, size, f) || apple_double(fd, size, 0x00051600u, f, true)) {
			f->rfd = f->rsrc_len ? fd : -1;
			return;
		}
	}
	fork_beside(path, f);
}

/* The resource fork, read in (NULL if there's none, or it's too big to be an image's) */
static uint8_t *read_fork(const struct forks *f)
{
	if (f->rfd < 0 || f->rsrc_len < 16 || f->rsrc_len > MAX_FORK)
		return NULL;
	uint8_t *r = malloc((size_t)f->rsrc_len);
	if (r && !read_at(f->rfd, r, (size_t)f->rsrc_len, f->rsrc)) {
		free(r);
		r = NULL;
	}
	return r;
}

/* ---- decompressing ---------------------------------------------------------- */

long dmg_adc(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	size_t i = 0, o = 0;
	while (i < n && o < cap) {
		uint8_t b = in[i];
		size_t run, dist;
		if (b & 0x80) {         /* literal bytes */
			run = (b & 0x7Fu) + 1;
			if (run > n - i - 1 || run > cap - o)
				return -1;
			memcpy(out + o, in + i + 1, run);
			i += 1 + run;
			o += run;
			continue;
		}
		if (b & 0x40) {         /* a copy from up to 64K back */
			if (n - i < 3)
				return -1;
			run = (b & 0x3Fu) + 4;
			dist = ((size_t)in[i + 1] << 8 | in[i + 2]) + 1;
			i += 3;
		} else {                /* ... from up to 1K back */
			if (n - i < 2)
				return -1;
			run = (b >> 2) + 3u;
			dist = ((size_t)(b & 3) << 8 | in[i + 1]) + 1;
			i += 2;
		}
		if (dist > o || run > cap - o)
			return -1;
		for (size_t k = 0; k < run; k++, o++)
			out[o] = out[o - dist];
	}
	return (long)o;
}

static void *sz_alloc(ISzAllocPtr p, size_t size)
{
	(void)p;
	return malloc(size);
}

static void sz_free(ISzAllocPtr p, void *a)
{
	(void)p;
	free(a);
}

static bool varint(const uint8_t *p, size_t n, size_t *i, uint64_t *v)
{
	*v = 0;
	for (unsigned shift = 0; *i < n && shift < 63; shift += 7) {
		uint8_t b = p[(*i)++];
		*v |= (uint64_t)(b & 0x7F) << shift;
		if (!(b & 0x80))
			return true;
	}
	return false;
}

/* An xz stream's blocks of LZMA2 (as hdiutil makes them), into `out`: bytes made, or -1 */
static long xz(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	static const uint8_t magic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0 };
	static const uint8_t check_size[16] = { 0, 4, 4, 4, 8, 8, 8, 16, 16, 16, 32, 32, 32, 64, 64, 64 };
	const ISzAlloc alloc = { sz_alloc, sz_free };
	if (n < 12 || memcmp(in, magic, 6))
		return -1;
	unsigned check = check_size[in[7] & 15];
	size_t i = 12, o = 0;
	while (o < cap && i < n && in[i]) {     /* (0: the index, after the last block) */
		size_t hs = (in[i] + 1u) * 4u, j = i + 2;
		uint64_t v, id, props;
		if (hs > n - i || (in[i + 1] & 0x3C))
			return -1;
		if ((in[i + 1] & 3) || ((in[i + 1] & 0x40) && !varint(in, i + hs, &j, &v)) ||
		    ((in[i + 1] & 0x80) && !varint(in, i + hs, &j, &v)) || !varint(in, i + hs, &j, &id) ||
		    !varint(in, i + hs, &j, &props) || id != 0x21 || props != 1 || j >= i + hs)
			return -1;      /* (a filter other than LZMA2 alone) */
		uint8_t prop = in[j];
		i += hs;
		CLzma2Dec dec;
		Lzma2Dec_CONSTRUCT(&dec);
		if (Lzma2Dec_Allocate(&dec, prop, &alloc) != SZ_OK)
			return -1;
		Lzma2Dec_Init(&dec);
		SizeT made = cap - o, used = n - i;
		ELzmaStatus status;
		SRes res = Lzma2Dec_DecodeToBuf(&dec, out + o, &made, in + i, &used, LZMA_FINISH_ANY, &status);
		Lzma2Dec_Free(&dec, &alloc);
		if (res != SZ_OK)
			return -1;
		o += made;
		i += used;
		if (status != LZMA_STATUS_FINISHED_WITH_MARK)
			break;
		i = ((i + 3) & ~(size_t)3) + check;
	}
	return (long)o;
}

static const char *method(uint32_t kind)
{
	switch (kind) {
	case ADC: return "ADC";
	case ZLIB: return "zlib";
	case BZIP2: return "bzip2";
	case LZFSE: return "LZFSE";
	case XZ: return "LZMA";
	default: return "";
	}
}

/* A chunk's data, decompressed: `out` holds its bytes (zeros after what the data makes); 0, or -1 */
static int unpack(struct dmg *d, const struct chunk *c, uint8_t *out, size_t size)
{
	if (c->len > d->in_cap) {
		uint8_t *in = realloc(d->in, (size_t)c->len);
		if (!in)
			return -1;
		d->in = in;
		d->in_cap = (size_t)c->len;
	}
	if (!read_at(d->fd, d->in, (size_t)c->len, c->at))
		return -1;
	long made = -1;
	switch (c->kind) {
	case ADC:
		made = dmg_adc(d->in, (size_t)c->len, out, size);
		break;
	case ZLIB: {
		size_t n = tinfl_decompress_mem_to_mem(out, size, d->in, (size_t)c->len, TINFL_FLAG_PARSE_ZLIB_HEADER);
		made = n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ? -1 : (long)n;
		break;
	}
	case BZIP2: {
		unsigned n = (unsigned)size;
		int rc = BZ2_bzBuffToBuffDecompress((char *)out, &n, (char *)d->in, (unsigned)c->len, 0, 0);
		made = rc == BZ_OK || rc == BZ_OUTBUFF_FULL ? (long)n : -1;
		break;
	}
	case LZFSE:
		made = (long)lzfse_decode_buffer(out, size, d->in, (size_t)c->len, NULL);
		break;
	case XZ:
		made = xz(d->in, (size_t)c->len, out, size);
		break;
	}
	if (made < 0)
		return -1;
	memset(out + made, 0, size - (size_t)made);
	return 0;
}

/* A compressed chunk's bytes, from the cache or decompressed into it */
static const uint8_t *chunk_data(struct dmg *d, size_t k)
{
	unsigned slot = 0;
	for (unsigned s = 0; s < CACHE; s++) {
		if (d->cache[s].chunk == k) {
			d->cache[s].used = ++d->tick;
			return d->cache[s].data;
		}
		if (d->cache[s].used < d->cache[slot].used)
			slot = s;
	}
	const struct chunk *c = &d->chunk[k];
	uint8_t *data = realloc(d->cache[slot].data, (size_t)c->count * SECTOR);
	if (!data)
		return NULL;
	d->cache[slot].data = data;
	d->cache[slot].chunk = SIZE_MAX;
	d->cache[slot].used = 0;
	if (unpack(d, c, data, (size_t)c->count * SECTOR)) {
		fprintf(stderr, "disk image: its %s chunk at sector %llu is damaged\n", method(c->kind),
			(unsigned long long)c->sector);
		return NULL;
	}
	d->cache[slot].chunk = k;
	d->cache[slot].used = ++d->tick;
	return data;
}

int dmg_read(void *ctx, void *buf, size_t len, uint64_t at)
{
	struct dmg *d = ctx;
	uint8_t *out = buf;
	if (at > d->size || len > d->size - at)
		return -1;
	while (len) {
		/* the last chunk starting at or before here */
		size_t lo = 0, hi = d->chunks;
		while (hi - lo > 1) {
			size_t mid = (lo + hi) / 2;
			if (d->chunk[mid].sector * SECTOR <= at)
				lo = mid;
			else
				hi = mid;
		}
		const struct chunk *c = d->chunks ? &d->chunk[lo] : NULL;
		uint64_t start = c ? c->sector * SECTOR : 0, end = c ? (c->sector + c->count) * SECTOR : 0;
		size_t n;
		if (!c || at < start || at >= end) {    /* between chunks: zeros */
			uint64_t next = lo + 1 < d->chunks ? d->chunk[lo + 1].sector * SECTOR : d->size;
			if (c && at < start)
				next = start;
			n = next - at < len ? (size_t)(next - at) : len;
			memset(out, 0, n ? n : len);
			if (!n)
				return 0;
		} else {
			n = end - at < len ? (size_t)(end - at) : len;
			if (c->kind == RAW) {
				if (!read_at(d->fd, out, n, c->at + (at - start)))
					return -1;
			} else if (c->kind == ZERO || c->kind == IGNORE) {
				memset(out, 0, n);
			} else {
				const uint8_t *data = chunk_data(d, lo);
				if (!data)
					return -1;
				memcpy(out, data + (at - start), n);
			}
		}
		out += n;
		at += n;
		len -= n;
	}
	return 0;
}

/* ---- block tables -------------------------------------------------------------- */

static int add_chunk(struct dmg *d, size_t *cap, const struct chunk *c)
{
	if (d->chunks == *cap) {
		size_t n = *cap ? *cap * 2 : 64;
		struct chunk *p = realloc(d->chunk, n * sizeof *p);
		if (!p)
			return -1;
		d->chunk = p;
		*cap = n;
	}
	d->chunk[d->chunks++] = *c;
	return 0;
}

/* A UDIF 'mish' block table: its chunks, from the data at `base` (+ its own data offset) */
static int mish(struct dmg *d, size_t *cap, const uint8_t *m, size_t len, uint64_t base, uint64_t file_size,
		char *why, size_t wlen)
{
	if (len < 204 || be32(m) != 0x6D697368u)        /* 'mish' */
		return fail(why, wlen, "its block table is damaged");
	uint64_t first = be64(m + 8), data = be64(m + 24);
	uint32_t n = be32(m + 200);
	if (n > (len - 204) / 40)
		return fail(why, wlen, "its block table is damaged");
	for (uint32_t k = 0; k < n; k++) {
		const uint8_t *e = m + 204 + 40 * (size_t)k;
		struct chunk c = { first + be64(e + 8), be64(e + 16), base + data + be64(e + 24), be64(e + 32), be32(e) };
		if (c.kind == COMMENT || c.kind == END || !c.count)
			continue;
		if (c.kind != ZERO && c.kind != RAW && c.kind != IGNORE && !method(c.kind)[0]) {
			snprintf(why, wlen, "it has chunks of a kind Blue doesn't know (0x%08X)", (unsigned)c.kind);
			return -1;
		}
		if (c.count > MAX_CHUNK / SECTOR && c.kind != RAW && c.kind != ZERO && c.kind != IGNORE)
			return fail(why, wlen, "its chunks are too big to unpack");
		if ((c.kind != ZERO && c.kind != IGNORE && (c.at > file_size || c.len > file_size - c.at)) ||
		    (c.kind == RAW && c.len < c.count * SECTOR))
			return fail(why, wlen, "its block table points past the end of the file (cut short?)");
		if (add_chunk(d, cap, &c))
			return fail(why, wlen, "out of memory");
	}
	return 0;
}

static int base64(char c)
{
	return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
	       c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* The XML property list's 'blkx' array: each <data> in it a block table */
static int plist(struct dmg *d, size_t *cap, char *xml, uint64_t base, uint64_t size, char *why, size_t wlen)
{
	char *p = strstr(xml, "<key>blkx</key>"), *end = p ? strstr(p, "</array>") : NULL;
	if (!p || !end)
		return fail(why, wlen, "its property list has no block table");
	*end = 0;
	while ((p = strstr(p, "<data>"))) {
		p += 6;
		char *q = strstr(p, "</data>");
		if (!q)
			return fail(why, wlen, "its property list is damaged");
		uint8_t *m = malloc((size_t)(q - p) / 4 * 3 + 3);
		size_t n = 0;
		unsigned bits = 0, acc = 0;
		for (char *s = p; m && s < q; s++) {
			int v = base64(*s);
			if (v < 0)
				continue;
			acc = acc << 6 | (unsigned)v;
			if ((bits += 6) >= 8) {
				bits -= 8;
				m[n++] = (uint8_t)(acc >> bits);
			}
		}
		int rc = m ? mish(d, cap, m, n, base, size, why, wlen) : fail(why, wlen, "out of memory");
		free(m);
		if (rc)
			return rc;
		p = q;
	}
	return 0;
}

static int by_sector(const void *a, const void *b)
{
	const struct chunk *x = a, *y = b;
	return x->sector < y->sector ? -1 : x->sector > y->sector;
}

/* What the image's chunks are, for messages */
static void describe(struct dmg *d, const char *format)
{
	static const uint32_t kinds[] = { ADC, ZLIB, BZIP2, LZFSE, XZ };
	size_t n = (size_t)snprintf(d->kind, sizeof d->kind, "%s", format);
	bool any = false;
	for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
		bool has = false;
		for (size_t j = 0; j < d->chunks && !has; j++)
			has = d->chunk[j].kind == kinds[k];
		if (has && n < sizeof d->kind)
			n += (size_t)snprintf(d->kind + n, sizeof d->kind - n, "%s%s", any ? ", " : " (", method(kinds[k]));
		any |= has;
	}
	if (n < sizeof d->kind)
		snprintf(d->kind + n, sizeof d->kind - n, "%s", any ? ")" : " (uncompressed)");
}

static int udif(struct dmg *d, const struct forks *f, const uint8_t *koly, char *why, size_t len)
{
	size_t cap = 0;
	uint64_t base = f->data, size = f->data + f->data_len;
	uint64_t data = be64(koly + 24), rsrc = be64(koly + 40), rsrc_len = be64(koly + 48);
	uint64_t xml = be64(koly + 216), xml_len = be64(koly + 224), sectors = be64(koly + 492);
	if (be32(koly + 60) > 1)
		return fail(why, len, "it's one segment of a segmented image (.dmgpart): join the segments first "
			    "(hdiutil convert on a Mac)");
	if (xml_len) {
		if (xml > f->data_len || xml_len > f->data_len - xml || xml_len > MAX_FORK)
			return fail(why, len, "its property list is past the end of the file (cut short?)");
		char *x = malloc((size_t)xml_len + 1);
		if (!x || !read_at(d->fd, x, (size_t)xml_len, base + xml)) {
			free(x);
			return fail(why, len, "cannot read its property list");
		}
		x[xml_len] = 0;
		int rc = plist(d, &cap, x, base + data, size, why, len);
		free(x);
		if (rc)
			return -1;
	} else {                /* (early UDIF: the block table in a resource fork, after the data) */
		struct forks r = { .rfd = d->fd, .rsrc = base + rsrc, .rsrc_len = rsrc_len };
		if (rsrc > f->data_len || rsrc_len > f->data_len - rsrc)
			return fail(why, len, "its resource fork is past the end of the file (cut short?)");
		uint8_t *fork = read_fork(&r);
		struct rsrc res;
		for (unsigned k = 0; fork && !rsrc_find(fork, (size_t)rsrc_len, 0x626C6B78u, k, &res); k++) /* 'blkx' */
			if (mish(d, &cap, res.data, res.len, base + data, size, why, len)) {
				free(fork);
				return -1;
			}
		free(fork);
	}
	if (!d->chunks)
		return fail(why, len, "it has no block table");
	qsort(d->chunk, d->chunks, sizeof *d->chunk, by_sector);
	uint64_t last = d->chunk[d->chunks - 1].sector + d->chunk[d->chunks - 1].count;
	d->size = (sectors > last ? sectors : last) * SECTOR;
	describe(d, "UDIF image");
	return 0;
}

/* NDIF: the 'bcem' resource (Disk Copy 6) */
static int ndif(struct dmg *d, const struct forks *f, const struct rsrc *bcem, char *why, size_t len)
{
	const uint8_t *b = bcem->data;
	size_t cap = 0;
	if (bcem->len < 128)
		return fail(why, len, "a Disk Copy 6 (NDIF) image whose block table is damaged");
	unsigned version = be16(b);
	if (version < 10 || version > 12) {
		snprintf(why, len, "a Disk Copy 6 (NDIF) image of a version Blue doesn't read (%u)", version);
		return -1;
	}
	uint64_t blocks = be32(b + 0x44), backing = be32(b + 0x4C);
	uint32_t n = be32(b + 0x7C);
	if (n > (bcem->len - 128) / 12)
		return fail(why, len, "a Disk Copy 6 (NDIF) image whose block table is damaged");
	for (uint32_t k = 0; k < n; k++) {
		const uint8_t *e = b + 128 + 12 * (size_t)k;
		uint32_t start = be32(e) >> 8, type = e[3];
		if (type == 0xFF)
			break;
		uint64_t next = k + 1 < n ? be32(e + 12) >> 8 : blocks;
		struct chunk c = { start, next > start ? next - start : 0, f->data + backing + be32(e + 4), be32(e + 8), 0 };
		if (type == 0x00)
			c.kind = ZERO;
		else if (type == 0x02)
			c.kind = RAW;
		else if (type == 0x83)
			c.kind = ADC;
		else if (type == 0x80)
			return fail(why, len, "a Disk Copy 6 (NDIF) image compressed with KenCode, which Blue doesn't read "
				    "(Disk Copy in Mac OS can convert it, to a read-only or ADC-compressed image)");
		else {
			snprintf(why, len, "a Disk Copy 6 (NDIF) image with chunks of a kind Blue doesn't know (%u)", type);
			return -1;
		}
		if (!c.count)
			continue;
		if (c.kind == ADC && c.count > MAX_CHUNK / SECTOR)
			return fail(why, len, "a Disk Copy 6 (NDIF) image whose chunks are too big to unpack");
		if (c.kind != ZERO && (c.at > f->data + f->data_len || c.len > f->data + f->data_len - c.at ||
				       (c.kind == RAW && c.len < c.count * SECTOR)))
			return fail(why, len, "a Disk Copy 6 (NDIF) image whose block table points past the end of its "
				    "data (cut short?)");
		if (add_chunk(d, &cap, &c))
			return fail(why, len, "out of memory");
	}
	if (!d->chunks)
		return fail(why, len, "a Disk Copy 6 (NDIF) image with an empty block table");
	qsort(d->chunk, d->chunks, sizeof *d->chunk, by_sector);
	d->size = blocks * SECTOR;
	describe(d, "Disk Copy 6 (NDIF) image");
	return 0;
}

int dmg_open(const char *path, struct dmg **out, char *why, size_t len)
{
	uint64_t size;
	*out = NULL;
	int fd = open_regular(path, &size);
	if (fd < 0)
		return 0;
	struct forks f;
	find_forks(fd, path, size, &f);
	uint8_t koly[512], head[8];
	int rc = 0;
	struct dmg *d = calloc(1, sizeof *d);
	if (!d) {
		rc = fail(why, len, "out of memory");
	} else if (f.data_len >= 8 && read_at(fd, head, 8, f.data) && !memcmp(head, "encrcdsa", 8)) {
		rc = fail(why, len, "an encrypted disk image: Blue can't read it (on a Mac, hdiutil convert with the "
			  "password makes a plain one)");
	} else if (f.data_len >= 8 && read_at(fd, head, 8, f.data + f.data_len - 8) && !memcmp(head, "cdsaencr", 8)) {
		rc = fail(why, len, "an encrypted disk image: Blue can't read it");
	} else if (f.data_len >= 512 && read_at(fd, koly, 512, f.data + f.data_len - 512) &&
		   be32(koly) == 0x6B6F6C79u) {        /* 'koly' */
		d->fd = fd;
		rc = 1;
		if (udif(d, &f, koly, why, len)) {
			char e[300];
			snprintf(e, sizeof e, "%s", why);
			snprintf(why, len, "a UDIF image (.dmg), but %s", e);
			rc = -1;
		}
	} else {
		uint8_t *fork = read_fork(&f);
		struct rsrc bcem;
		if (fork && !rsrc_find(fork, (size_t)f.rsrc_len, 0x6263656Du, 0, &bcem)) {        /* 'bcem' */
			d->fd = fd;
			rc = ndif(d, &f, &bcem, why, len) ? -1 : 1;
		}
		free(fork);
	}
	if (f.own_rfd)
		close(f.rfd);
	if (rc == 1) {
		for (unsigned s = 0; s < CACHE; s++)
			d->cache[s].chunk = SIZE_MAX;
		*out = d;
		return 1;
	}
	if (d)
		d->fd = -1;
	dmg_close(d);
	close(fd);
	return rc;
}

void dmg_close(struct dmg *d)
{
	if (!d)
		return;
	if (d->fd >= 0)
		close(d->fd);
	for (unsigned s = 0; s < CACHE; s++)
		free(d->cache[s].data);
	free(d->chunk);
	free(d->in);
	free(d);
}

uint64_t dmg_size(const struct dmg *d)
{
	return d->size;
}

const char *dmg_kind(const struct dmg *d)
{
	return d->kind;
}

/* bzip2's, built without stdio: an inconsistency inside it */
void bz_internal_error(int code);
void bz_internal_error(int code)
{
	fprintf(stderr, "bzip2: internal error %d\n", code);
}
