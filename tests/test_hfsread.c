#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/be.h"
#include "../src/hfs.h"
#include "../src/hfsread.h"
#include "test.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define TBXI FOURCC('t', 'b', 'x', 'i')
#define TEXT FOURCC('T', 'E', 'X', 'T')

#define BLOCK 512u
#define BLOCKS 2048u                    /* allocation blocks in a test volume */
#define HFS_FIRST_BLOCK 4096u           /* where an HFS volume's allocation blocks start (drAlBlSt 8) */
#define VOLUME_BYTES (HFS_FIRST_BLOCK + BLOCKS * BLOCK)
#define ROM_BYTES 20000u
#define ROOT_ID 2u
#define SYSTEM_ID 20u
#define OTHER_ID 21u
#define ROM_ID 40u
#define MAX_NODES 32u
#define MAX_RECORDS 48u

static int temp_file(char *path, size_t len)
{
	const char *dir = getenv("TMPDIR");
	snprintf(path, len, "%s/blue-test-XXXXXX", dir ? dir : "/tmp");
	return mkstemp(path);
}

/* ---- building volumes ---------------------------------------------------- */

struct rec {
	uint8_t b[320];
	size_t key, len;                /* the key's size (with its padding), the record's */
};

struct tree {
	bool plus, variable;            /* HFS+; index keys as long as they say */
	uint32_t node_size, max_key;
	uint8_t *nodes;
	unsigned count;
};

static size_t cat_key(bool plus, uint8_t *p, uint32_t parent, const char *name)
{
	size_t n = strlen(name);
	put_be32(p + 2, parent);
	if (plus) {
		put_be16(p, (uint16_t)(6 + 2 * n));
		put_be16(p + 6, (uint16_t)n);
		for (size_t k = 0; k < n; k++)
			put_be16(p + 8 + 2 * k, (uint8_t)name[k]);
		return 8 + 2 * n;
	}
	p[0] = (uint8_t)(6 + n);
	p[6] = (uint8_t)n;
	memcpy(p + 7, name, n);
	return (7 + n + 1) & ~(size_t)1;
}

static struct rec folder_rec(bool plus, uint32_t parent, const char *name, uint32_t id)
{
	struct rec r = { { 0 }, 0, 0 };
	r.key = cat_key(plus, r.b, parent, name);
	uint8_t *d = r.b + r.key;
	if (plus) {
		put_be16(d, 1);
		put_be32(d + 8, id);
		r.len = r.key + 88;
	} else {
		d[0] = 1;
		put_be32(d + 6, id);
		r.len = r.key + 70;
	}
	return r;
}

static struct rec thread_rec(bool plus, uint32_t id, uint32_t parent, const char *name)
{
	struct rec r = { { 0 }, 0, 0 };
	size_t n = strlen(name);
	r.key = cat_key(plus, r.b, id, "");
	uint8_t *d = r.b + r.key;
	if (plus) {
		put_be16(d, 3);
		put_be32(d + 4, parent);
		put_be16(d + 8, (uint16_t)n);
		for (size_t k = 0; k < n; k++)
			put_be16(d + 10 + 2 * k, (uint8_t)name[k]);
		r.len = r.key + 10 + 2 * n;
	} else {
		d[0] = 3;
		put_be32(d + 10, parent);
		d[14] = (uint8_t)n;
		memcpy(d + 15, name, n);
		r.len = r.key + 46;
	}
	return r;
}

/* A file record; its data fork's first extents from `ext` (3 on HFS, 8 on HFS+) */
static struct rec file_rec(bool plus, uint32_t parent, const char *name, uint32_t id, uint32_t type, uint32_t size,
			   const struct hfsread_extent *ext)
{
	struct rec r = { { 0 }, 0, 0 };
	r.key = cat_key(plus, r.b, parent, name);
	uint8_t *d = r.b + r.key;
	if (plus) {
		put_be16(d, 2);
		put_be32(d + 8, id);
		put_be32(d + 48, type);
		put_be32(d + 52, FOURCC('c', 'h', 'r', 'p'));
		put_be64(d + 88, size);
		for (unsigned k = 0; ext && k < 8; k++) {
			put_be32(d + 88 + 16 + 8 * k, ext[k].start);
			put_be32(d + 88 + 20 + 8 * k, ext[k].count);
		}
		r.len = r.key + 248;
	} else {
		d[0] = 2;
		put_be32(d + 4, type);
		put_be32(d + 8, FOURCC('c', 'h', 'r', 'p'));
		put_be32(d + 20, id);
		put_be32(d + 26, size);
		for (unsigned k = 0; ext && k < 3; k++) {
			put_be16(d + 74 + 4 * k, (uint16_t)ext[k].start);
			put_be16(d + 76 + 4 * k, (uint16_t)ext[k].count);
		}
		r.len = r.key + 102;
	}
	return r;
}

static struct rec extents_rec(bool plus, uint32_t id, uint8_t fork, uint32_t start, const struct hfsread_extent *ext,
			      unsigned n)
{
	struct rec r = { { 0 }, 0, 0 };
	if (plus) {
		put_be16(r.b, 10);
		r.b[2] = fork;
		put_be32(r.b + 4, id);
		put_be32(r.b + 8, start);
		r.key = 12;
		for (unsigned k = 0; k < n; k++) {
			put_be32(r.b + 12 + 8 * k, ext[k].start);
			put_be32(r.b + 16 + 8 * k, ext[k].count);
		}
		r.len = 12 + 64;
	} else {
		r.b[0] = 7;
		r.b[1] = fork;
		put_be32(r.b + 2, id);
		put_be16(r.b + 6, (uint16_t)start);
		r.key = 8;
		for (unsigned k = 0; k < n; k++) {
			put_be16(r.b + 8 + 4 * k, (uint16_t)ext[k].start);
			put_be16(r.b + 10 + 4 * k, (uint16_t)ext[k].count);
		}
		r.len = 8 + 12;
	}
	return r;
}

static void put_node(struct tree *t, unsigned n, uint8_t kind, unsigned height, const struct rec *r, unsigned count)
{
	uint8_t *node = t->nodes + (size_t)n * t->node_size;
	unsigned ns = t->node_size, at = 14;
	memset(node, 0, ns);
	node[8] = kind;
	node[9] = (uint8_t)height;
	put_be16(node + 10, (uint16_t)count);
	for (unsigned k = 0; k < count; k++) {
		put_be16(node + ns - 2 * (k + 1), (uint16_t)at);
		memcpy(node + at, r[k].b, r[k].len);
		at += (unsigned)r[k].len;
	}
	put_be16(node + ns - 2 * (count + 1), (uint16_t)at);
	CHECK(at <= ns - 2 * (count + 1));
}

/* An index record for a node, with the key of its first record (HFS's padded to the maximum) */
static struct rec index_rec(const struct tree *t, const struct rec *first, uint32_t node)
{
	struct rec r = { { 0 }, 0, 0 };
	if (t->plus && t->variable) {
		memcpy(r.b, first->b, first->key);
		r.key = first->key;
	} else if (t->plus) {
		put_be16(r.b, (uint16_t)t->max_key);
		memcpy(r.b + 2, first->b + 2, be16(first->b));
		r.key = 2 + t->max_key;
	} else {
		r.b[0] = (uint8_t)t->max_key;
		memcpy(r.b + 1, first->b + 1, first->b[0]);
		r.key = (1 + t->max_key + 1) & ~1u;
	}
	put_be32(r.b + r.key, node);
	r.len = r.key + 4;
	return r;
}

/* A B-tree of records in key order, `fanout` to a node: the leaves, then index levels up to the root */
static void build_tree(struct tree *t, const struct rec *r, unsigned n, unsigned fanout)
{
	static struct rec level[MAX_NODES], up[MAX_NODES];
	unsigned count = 0, height = 1, first = 1, root;
	t->count = 1;
	for (unsigned k = 0; k < n; k += fanout) {
		put_node(t, t->count, 0xFF, 1, r + k, n - k < fanout ? n - k : fanout);
		level[count++] = index_rec(t, &r[k], t->count);
		t->count++;
	}
	unsigned last = t->count - 1;
	for (unsigned l = first; l <= last; l++) {
		put_be32(t->nodes + (size_t)l * t->node_size, l < last ? l + 1 : 0);
		put_be32(t->nodes + (size_t)l * t->node_size + 4, l > first ? l - 1 : 0);
	}
	root = last;
	while (count > 1) {
		unsigned next = 0;
		height++;
		for (unsigned k = 0; k < count; k += fanout) {
			put_node(t, t->count, 0x00, height, level + k, count - k < fanout ? count - k : fanout);
			up[next++] = index_rec(t, &level[k], t->count);
			root = t->count++;
		}
		memcpy(level, up, next * sizeof up[0]);
		count = next;
	}
	CHECK(t->count <= MAX_NODES);

	uint8_t *h = t->nodes;
	memset(h, 0, t->node_size);
	h[8] = 1;
	put_be16(h + 10, 3);
	put_be16(h + 14, (uint16_t)height);
	put_be32(h + 16, root);
	put_be32(h + 20, n);
	put_be32(h + 24, first);
	put_be32(h + 28, last);
	put_be16(h + 32, (uint16_t)t->node_size);
	put_be16(h + 34, (uint16_t)t->max_key);
	put_be32(h + 36, t->count);
	if (t->plus)
		put_be32(h + 14 + 38, t->variable ? 6 : 2);
	static const uint16_t offsets[4] = { 14, 120, 248, 504 };
	for (unsigned k = 0; k < 4; k++)
		put_be16(h + t->node_size - 2 * (k + 1), offsets[k]);
}

/* Writes data along extents, in order */
static void write_extents(uint8_t *blocks, const struct hfsread_extent *ext, unsigned n, const uint8_t *data,
			  size_t size)
{
	size_t at = 0;
	for (unsigned k = 0; k < n && at < size; k++) {
		size_t len = (size_t)ext[k].count * BLOCK;
		if (len > size - at)
			len = size - at;
		memcpy(blocks + (size_t)ext[k].start * BLOCK, data + at, len);
		at += len;
	}
	CHECK_EQ(at, size);
}

static void put_fork(uint8_t *p, uint64_t size, const struct hfsread_extent *ext, unsigned n)
{
	uint32_t blocks = 0;
	put_be64(p, size);
	for (unsigned k = 0; k < n; k++) {
		put_be32(p + 16 + 8 * k, ext[k].start);
		put_be32(p + 20 + 8 * k, ext[k].count);
		blocks += ext[k].count;
	}
	put_be32(p + 12, blocks);
}

static void rom_data(uint8_t *rom)
{
	for (uint32_t k = 0; k < ROM_BYTES; k++)
		rom[k] = (uint8_t)(k * 7 + (k >> 9));
}

/*
 * A volume (VOLUME_BYTES) holding
 *   Test Volume (ID 2)     Decoy ('tbxi'), System Folder, Zeta
 *   System Folder (ID 20)  File 00-29 ('TEXT'), Mac OS ROM ('tbxi', in pieces out of order: the first
 *                          extents in its catalog record, the rest in two records of the extents file),
 *                          Zz ROM ('tbxi')
 *   Zeta (ID 21)           Other ROM ('tbxi'), Weird ('zzzz')
 * The catalog is three levels deep and in two pieces; the extents file has decoys around Mac OS ROM's
 * records (its resource fork's, another file's). `blessed` goes in the Finder info.
 */
static uint8_t *make_volume(bool plus, uint32_t blessed)
{
	static const uint32_t hfs_pieces[] = { 5, 7, 3, 6, 4, 2, 13 };            /* 3 in the record, 3 + 1 */
	static const uint32_t plus_pieces[] = { 2, 2, 2, 2, 2, 2, 2, 2, 8, 4, 4, 8 };      /* 8, 3 + 1 */
	const uint32_t *pieces = plus ? plus_pieces : hfs_pieces;
	unsigned npieces = plus ? 12 : 7, first = plus ? 8 : 3;
	uint8_t *vol = calloc(1, VOLUME_BYTES), rom[ROM_BYTES];
	uint8_t *blocks = vol + (plus ? 0 : HFS_FIRST_BLOCK);
	struct hfsread_extent ext[12], decoy[8] = { { 250, 1 } };     /* (a record holds 3 or 8) */
	static struct rec r[MAX_RECORDS];
	char name[16];
	unsigned n = 0;

	rom_data(rom);
	for (unsigned k = 0, at = BLOCKS - 8; k < npieces; k++) {         /* from the end backwards */
		at -= pieces[k];
		ext[k] = (struct hfsread_extent){ at, pieces[k] };
		at--;
	}
	write_extents(blocks, ext, npieces, rom, ROM_BYTES);
	memset(blocks + decoy[0].start * BLOCK, 0xEE, BLOCK);

	r[n++] = folder_rec(plus, 1, "Test Volume", ROOT_ID);
	r[n++] = thread_rec(plus, ROOT_ID, 1, "Test Volume");
	r[n++] = file_rec(plus, ROOT_ID, "Decoy", 30, TBXI, BLOCK, decoy);
	r[n++] = folder_rec(plus, ROOT_ID, "System Folder", SYSTEM_ID);
	r[n++] = folder_rec(plus, ROOT_ID, "Zeta", OTHER_ID);
	r[n++] = thread_rec(plus, SYSTEM_ID, ROOT_ID, "System Folder");
	for (unsigned k = 0; k < 30; k++) {
		snprintf(name, sizeof name, "File %02u", k);
		r[n++] = file_rec(plus, SYSTEM_ID, name, 100 + k, TEXT, 0, NULL);
	}
	r[n++] = file_rec(plus, SYSTEM_ID, "Mac OS ROM", ROM_ID, TBXI, ROM_BYTES, ext);
	r[n++] = file_rec(plus, SYSTEM_ID, "Zz ROM", ROM_ID + 1, TBXI, 0, NULL);
	r[n++] = thread_rec(plus, OTHER_ID, ROOT_ID, "Zeta");
	r[n++] = file_rec(plus, OTHER_ID, "Other ROM", ROM_ID + 2, TBXI, 0, NULL);
	r[n++] = file_rec(plus, OTHER_ID, "Weird", ROM_ID + 3, FOURCC('z', 'z', 'z', 'z'), 0, NULL);

	struct tree cat = { plus, plus, plus ? 4096 : 512, plus ? 516 : 37, calloc(MAX_NODES, 4096), 0 };
	build_tree(&cat, r, n, 4);
	uint32_t cat_bytes = cat.count * cat.node_size, cat_blocks = cat_bytes / BLOCK;
	uint32_t split = plus ? 13 : 7;         /* (HFS+: in the middle of a node) */
	struct hfsread_extent cat_ext[2] = { { 16, split }, { 300, cat_blocks - split } };
	write_extents(blocks, cat_ext, 2, cat.nodes, cat_bytes);

	struct rec e[4];
	uint32_t later = 0;
	for (unsigned k = 0; k < first; k++)
		later += pieces[k];
	e[0] = extents_rec(plus, ROM_ID, 0, later, ext + first, 3);
	e[1] = extents_rec(plus, ROM_ID, 0, later + pieces[first] + pieces[first + 1] + pieces[first + 2], ext + first + 3,
			   1);
	e[2] = extents_rec(plus, ROM_ID, 0xFF, 0, decoy, 1);
	e[3] = extents_rec(plus, ROM_ID + 1, 0, 0, decoy, 1);
	struct tree xt = { plus, false, plus ? 1024 : 512, plus ? 10 : 7, calloc(MAX_NODES, 4096), 0 };
	build_tree(&xt, e, 4, 2);
	uint32_t xt_bytes = xt.count * xt.node_size;
	struct hfsread_extent xt_ext = { 200, xt_bytes / BLOCK };
	write_extents(blocks, &xt_ext, 1, xt.nodes, xt_bytes);

	uint8_t *h = vol + 1024;
	if (plus) {
		put_be16(h, 0x482B);
		put_be16(h + 2, 4);
		put_be32(h + 40, BLOCK);
		put_be32(h + 44, BLOCKS);
		put_be32(h + 80, blessed);
		put_fork(h + 192, xt_bytes, &xt_ext, 1);
		put_fork(h + 272, cat_bytes, cat_ext, 2);
	} else {
		put_be16(h, 0x4244);
		put_be16(h + 18, BLOCKS);
		put_be32(h + 20, BLOCK);
		put_be16(h + 28, HFS_FIRST_BLOCK / 512);
		h[36] = 11;
		memcpy(h + 37, "Test Volume", 11);
		put_be32(h + 92, blessed);
		put_be32(h + 130, xt_bytes);
		put_be16(h + 134, (uint16_t)xt_ext.start);
		put_be16(h + 136, (uint16_t)xt_ext.count);
		put_be32(h + 146, cat_bytes);
		for (unsigned k = 0; k < 2; k++) {
			put_be16(h + 150 + 4 * k, (uint16_t)cat_ext[k].start);
			put_be16(h + 152 + 4 * k, (uint16_t)cat_ext[k].count);
		}
	}
	free(cat.nodes);
	free(xt.nodes);
	return vol;
}

/* ---- tests ----------------------------------------------------------------- */

static void check_volume(int fd, uint64_t offset, uint64_t size, bool plus, bool wrapped)
{
	struct hfsread_volume v;
	struct hfsread_file f;
	static uint8_t rom[ROM_BYTES], buf[ROM_BYTES];
	char path[HFSREAD_PATH_MAX];

	rom_data(rom);
	CHECK_EQ(hfsread_open(&v, fd, offset, size), 0);
	CHECK_EQ(v.plus, plus);
	CHECK_EQ(v.wrapped, wrapped);
	CHECK_EQ(v.blessed, SYSTEM_ID);
	CHECK_EQ(hfsread_path(&v, SYSTEM_ID, path, sizeof path), 0);
	CHECK(strcmp(path, "Test Volume:System Folder") == 0);

	CHECK_EQ(hfsread_find(&v, SYSTEM_ID, TBXI, &f), 1);              /* the first in the folder */
	CHECK(strcmp(f.name, "Mac OS ROM") == 0);
	CHECK_EQ(f.id, ROM_ID);
	CHECK_EQ(f.creator, FOURCC('c', 'h', 'r', 'p'));
	CHECK_EQ(f.data.size, ROM_BYTES);
	memset(buf, 0, sizeof buf);
	CHECK_EQ(hfsread_read(&v, &f.data, 0, buf, ROM_BYTES), 0);
	CHECK(memcmp(buf, rom, ROM_BYTES) == 0);
	static const uint32_t at[] = { 1, 511, 2555, 7679, 13000, 16383, 19000 };   /* across pieces */
	for (unsigned k = 0; k < sizeof at / sizeof at[0]; k++) {
		size_t len = ROM_BYTES - at[k] < 3000 ? ROM_BYTES - at[k] : 3000;
		memset(buf, 0, len);
		CHECK_EQ(hfsread_read(&v, &f.data, at[k], buf, len), 0);
		CHECK(memcmp(buf, rom + at[k], len) == 0);
	}
	CHECK(hfsread_read(&v, &f.data, ROM_BYTES - 10, buf, 11) != 0);  /* past the end */

	CHECK_EQ(hfsread_find(&v, SYSTEM_ID, TEXT, &f), 1);
	CHECK(strcmp(f.name, "File 00") == 0);
	CHECK_EQ(hfsread_find(&v, SYSTEM_ID, FOURCC('z', 'z', 'z', 'z'), &f), 0);     /* only in the next folder */
	CHECK_EQ(hfsread_find(&v, OTHER_ID, TBXI, &f), 1);
	CHECK(strcmp(f.name, "Other ROM") == 0);
	CHECK_EQ(hfsread_find(&v, ROOT_ID, TBXI, &f), 1);
	CHECK(strcmp(f.name, "Decoy") == 0);
	CHECK_EQ(hfsread_find(&v, 99, TBXI, &f), 0);
	CHECK_EQ(hfsread_find(&v, 1, TBXI, &f), 0);
}

static void test_volumes(void)
{
	char path[256];
	int fd = temp_file(path, sizeof path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	for (int plus = 0; plus < 2; plus++) {
		uint8_t *vol = make_volume(plus, SYSTEM_ID);
		CHECK(ftruncate(fd, 0) == 0);
		CHECK(pwrite(fd, vol, VOLUME_BYTES, 0) == (ssize_t)VOLUME_BYTES);
		check_volume(fd, 0, VOLUME_BYTES, plus, false);
		free(vol);
	}
	struct hfsread_volume v;
	uint8_t zero[2048] = { 0 };
	CHECK(pwrite(fd, zero, sizeof zero, 0) == (ssize_t)sizeof zero);
	CHECK(hfsread_open(&v, fd, 0, VOLUME_BYTES) != 0);              /* not a volume */
	close(fd);
	unlink(path);
}

static void put_entry(uint8_t *b, uint32_t start, uint32_t blocks, const char *name, const char *type)
{
	memset(b, 0, 512);
	put_be16(b, 0x504D);
	put_be32(b + 4, 3);
	put_be32(b + 8, start);
	put_be32(b + 12, blocks);
	strcpy((char *)b + 16, name);
	strcpy((char *)b + 48, type);
}

/* An empty volume, a partitioned image (an HFS volume without a blessed folder, then HFS+ in a wrapper) */
static void test_startup_disk(void)
{
	char empty[256], disk[256];
	int efd = temp_file(empty, sizeof empty), fd = temp_file(disk, sizeof disk);
	CHECK(efd >= 0 && fd >= 0);
	if (efd < 0 || fd < 0)
		return;
	CHECK(ftruncate(efd, 4u << 20) == 0);
	CHECK_EQ(hfs_format(efd, 4u << 20, "Empty", 0, NULL, 0), 0);
	close(efd);

	const uint32_t first = 64, second = 4096, size = VOLUME_BYTES / 512 + 8;
	uint8_t b[512] = { 0 };
	put_be16(b, 0x4552);
	put_be16(b + 2, 512);
	CHECK(pwrite(fd, b, 512, 0) == 512);
	put_entry(b, 1, 63, "Apple", "Apple_partition_map");
	CHECK(pwrite(fd, b, 512, 512) == 512);
	put_entry(b, first, size, "First", "Apple_HFS");
	CHECK(pwrite(fd, b, 512, 1024) == 512);
	put_entry(b, second, size, "Second", "Apple_HFS");
	CHECK(pwrite(fd, b, 512, 1536) == 512);
	uint8_t *vol = make_volume(false, 0);
	CHECK(pwrite(fd, vol, VOLUME_BYTES, (off_t)first * 512) == (ssize_t)VOLUME_BYTES);
	free(vol);
	/* the wrapper: allocation blocks from sector 4, the HFS+ volume from its block 4 */
	memset(b, 0, sizeof b);
	put_be16(b, 0x4244);
	put_be16(b + 18, BLOCKS + 8);
	put_be32(b + 20, BLOCK);
	put_be16(b + 28, 4);
	put_be32(b + 92, 99);
	put_be16(b + 124, 0x482B);
	put_be16(b + 126, 4);
	put_be16(b + 128, BLOCKS);
	CHECK(pwrite(fd, b, 512, (off_t)second * 512 + 1024) == 512);
	vol = make_volume(true, SYSTEM_ID);
	CHECK(pwrite(fd, vol, BLOCKS * BLOCK, (off_t)second * 512 + 8 * 512) == (ssize_t)(BLOCKS * BLOCK));
	free(vol);
	CHECK(ftruncate(fd, (off_t)(second + size) * 512) == 0);
	check_volume(fd, (uint64_t)second * 512, (uint64_t)size * 512, true, true);
	close(fd);

	struct hfsread_found found;
	char err[256];
	const char *paths[] = { empty, disk, "/nonexistent/blue-test" };
	static uint8_t rom[ROM_BYTES];
	rom_data(rom);
	CHECK_EQ(hfsread_system_file(paths, 1, TBXI, &found, err, sizeof err), 0);
	CHECK_EQ(hfsread_system_file(paths, 2, TBXI, &found, err, sizeof err), 1);
	CHECK_EQ(found.drive, 1);
	CHECK_EQ(found.part, 1);
	CHECK(strcmp(found.partition, "Second") == 0);
	CHECK(strcmp(found.path, "Test Volume:System Folder:Mac OS ROM") == 0);
	CHECK_EQ(found.size, ROM_BYTES);
	CHECK(found.data && memcmp(found.data, rom, ROM_BYTES) == 0);
	free(found.data);
	CHECK_EQ(hfsread_system_file(paths + 1, 2, TBXI, &found, err, sizeof err), 1);   /* found before the bad one */
	free(found.data);
	unlink(empty);
	unlink(disk);
}

/* A volume as Blue makes them, its root folder blessed */
static void test_formatted(void)
{
	char path[256];
	int fd = temp_file(path, sizeof path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	static uint8_t rom[ROM_BYTES];
	struct hfs_file files[30];
	char names[30][16];
	rom_data(rom);
	for (unsigned k = 0; k < 30; k++) {
		snprintf(names[k], sizeof names[k], k == 20 ? "Mac OS ROM" : "File %02u", k);
		files[k] = (struct hfs_file){ names[k], k == 20 ? TBXI : TEXT, 0, 0, 0, 0, k == 20 ? rom : NULL, NULL,
					      k == 20 ? ROM_BYTES : 0, 0 };
	}
	CHECK(ftruncate(fd, 8u << 20) == 0);
	CHECK_EQ(hfs_format(fd, 8u << 20, "Disk", 0, files, 30), 0);

	const char *paths[] = { path };
	struct hfsread_found found;
	char err[256];
	CHECK_EQ(hfsread_system_file(paths, 1, TBXI, &found, err, sizeof err), 0);      /* nothing blessed */
	uint8_t blessed[4];
	put_be32(blessed, ROOT_ID);
	CHECK(pwrite(fd, blessed, 4, 1024 + 92) == 4);
	CHECK_EQ(hfsread_system_file(paths, 1, TBXI, &found, err, sizeof err), 1);
	CHECK(strcmp(found.path, "Disk:Mac OS ROM") == 0);
	CHECK(found.partition[0] == '\0');
	CHECK_EQ(found.size, ROM_BYTES);
	CHECK(found.data && memcmp(found.data, rom, ROM_BYTES) == 0);
	free(found.data);

	struct hfsread_volume v;
	CHECK_EQ(hfsread_open(&v, fd, 0, 8u << 20), 0);
	CHECK(!v.plus);
	CHECK(v.catalog.root > 1);              /* leaves, then an index node */
	close(fd);
	unlink(path);
}

void test_hfsread(void)
{
	test_volumes();
	test_startup_disk();
	test_formatted();
}
