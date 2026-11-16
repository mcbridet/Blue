#include "hfsread.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "be.h"
#include "disk.h"
#include "hfs.h"

#define SECTOR 512u
#define HEADER_OFFSET 1024u             /* of the MDB or volume header in a volume */
#define SIG_HFS 0x4244u                 /* 'BD' */
#define SIG_HFS_PLUS 0x482Bu            /* 'H+' */
#define SIG_HFSX 0x4858u                /* 'HX' */

/* HFS master directory block */
enum {
	MDB_ATRB = 10, MDB_NM_AL_BLKS = 18, MDB_AL_BLK_SIZ = 20, MDB_AL_BL_ST = 28, MDB_FNDR_INFO = 92, MDB_EMBED_SIG = 124,
	MDB_EMBED_EXTENT = 126, MDB_XT_FL_SIZ = 130, MDB_XT_EXT_REC = 134, MDB_CT_FL_SIZ = 146,
	MDB_CT_EXT_REC = 150,
};
/* HFS+ volume header */
enum { VH_ATTRIBUTES = 4, VH_BLOCK_SIZE = 40, VH_TOTAL_BLOCKS = 44, VH_FINDER_INFO = 80, VH_EXTENTS_FILE = 192, VH_CATALOG_FILE = 272 };

#define ROOT_PARENT_ID 1u
#define EXTENTS_FILE_ID 3u
#define CATALOG_FILE_ID 4u

/* B-tree nodes: the descriptor, then records, their offsets at the end of the node */
#define NODE_DESCRIPTOR 14u
#define NODE_INDEX 0x00u
#define NODE_HEADER 0x01u
#define NODE_LEAF 0xFFu
#define BT_VARIABLE_INDEX_KEYS 4u       /* header attributes */
#define MAX_DEPTH 16u
#define MAX_NODE_SIZE 32768u

/* Catalog records */
#define CAT_FOLDER 1
#define CAT_FILE 2
#define CAT_FOLDER_THREAD 3
#define CAT_FILE_THREAD 4
#define HFS_FOLDER_SIZE 70u
#define HFS_FILE_SIZE 102u
#define PLUS_FOLDER_SIZE 88u
#define HFS_THREAD_SIZE 46u
#define PLUS_FILE_SIZE 248u
#define PLUS_THREAD_MIN 10u

#define MAX_FILE_SIZE (64u << 20)       /* more than any ROM file */

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
	return -1;
}

/* ---- names ----------------------------------------------------------------- */


/* Appends a character in UTF-8 if it fits (with room for the NUL). */
static void put_utf8(char *out, size_t len, size_t *at, uint32_t c)
{
	uint8_t b[4];
	size_t n;
	if (c < 0x80) {
		b[0] = (uint8_t)c;
		n = 1;
	} else if (c < 0x800) {
		b[0] = (uint8_t)(0xC0 | c >> 6);
		b[1] = (uint8_t)(0x80 | (c & 0x3F));
		n = 2;
	} else if (c < 0x10000) {
		b[0] = (uint8_t)(0xE0 | c >> 12);
		b[1] = (uint8_t)(0x80 | (c >> 6 & 0x3F));
		b[2] = (uint8_t)(0x80 | (c & 0x3F));
		n = 3;
	} else {
		b[0] = (uint8_t)(0xF0 | c >> 18);
		b[1] = (uint8_t)(0x80 | (c >> 12 & 0x3F));
		b[2] = (uint8_t)(0x80 | (c >> 6 & 0x3F));
		b[3] = (uint8_t)(0x80 | (c & 0x3F));
		n = 4;
	}
	if (*at + n < len) {
		memcpy(out + *at, b, n);
		*at += n;
	}
}

/* An HFS name (Mac OS Roman) or an HFS+ one (UTF-16, big-endian) in UTF-8 */
static void name_utf8(bool plus, const uint8_t *s, size_t n, char *out, size_t len)
{
	size_t at = 0;
	for (size_t k = 0; k < n; k++) {
		uint32_t c = plus ? be16(s + 2 * k) : s[k];
		if (!plus && c >= 0x80)
			c = hfs_mac_roman[c - 0x80];
		else if (plus && c >= 0xD800 && c < 0xDC00 && k + 1 < n && be16(s + 2 * k + 2) >= 0xDC00 &&
			 be16(s + 2 * k + 2) < 0xE000)
			c = 0x10000 + ((c - 0xD800) << 10) + (be16(s + 2 * ++k) - 0xDC00);
		put_utf8(out, len, &at, c);
	}
	out[at] = '\0';
}

/* ---- forks ----------------------------------------------------------------- */

static int vread(const struct hfsread_volume *v, void *buf, size_t n, uint64_t at)
{
	if (v->read)
		return v->read(v->ctx, buf, n, at);
	return pread(v->fd, buf, n, (off_t)at) == (ssize_t)n ? 0 : -1;
}

static void hfs_extents(struct hfsread_fork *f, const uint8_t *p)
{
	for (unsigned k = 0; k < 3; k++)
		f->ext[k] = (struct hfsread_extent){ be16(p + 4 * k), be16(p + 4 * k + 2) };
}

/* An HFS+ fork's data: logical size, clump size, blocks, 8 extents */
static void plus_fork(struct hfsread_fork *f, const uint8_t *p)
{
	f->size = be64(p);
	for (unsigned k = 0; k < 8; k++)
		f->ext[k] = (struct hfsread_extent){ be32(p + 16 + 8 * k), be32(p + 20 + 8 * k) };
}

static int extents_lookup(const struct hfsread_volume *v, const struct hfsread_fork *f, uint32_t block,
			  uint32_t *phys, uint32_t *run);

/* Where a block of a fork is, in a list of extents starting at block `first`: its block and how many follow it */
static int in_extents(const struct hfsread_extent *ext, unsigned n, uint32_t first, uint32_t block, uint32_t *phys,
		      uint32_t *run)
{
	for (unsigned k = 0; k < n && ext[k].count; k++) {
		if (block - first < ext[k].count) {
			*phys = ext[k].start + (block - first);
			*run = ext[k].count - (block - first);
			return 0;
		}
		first += ext[k].count;
	}
	return -1;
}

static int map_block(const struct hfsread_volume *v, const struct hfsread_fork *f, uint32_t block, uint32_t *phys,
		     uint32_t *run)
{
	if (in_extents(f->ext, v->plus ? 8 : 3, 0, block, phys, run) == 0)
		return 0;
	if (f->id == EXTENTS_FILE_ID)
		return -1;
	return extents_lookup(v, f, block, phys, run);
}

int hfsread_read(const struct hfsread_volume *v, const struct hfsread_fork *fork, uint64_t pos, void *buf,
		 size_t len)
{
	uint8_t *p = buf;
	if (pos > fork->size || len > fork->size - pos)
		return -1;
	while (len) {
		uint64_t block = pos / v->block_size;
		uint32_t within = (uint32_t)(pos % v->block_size), phys, run;
		if (block > UINT32_MAX || map_block(v, fork, (uint32_t)block, &phys, &run) ||
		    phys >= v->blocks || run > v->blocks - phys)
			return -1;
		uint64_t n = (uint64_t)run * v->block_size - within;
		if (n > len)
			n = len;
		if (vread(v, p, (size_t)n, v->offset + (uint64_t)phys * v->block_size + within))
			return -1;
		p += n;
		pos += n;
		len -= (size_t)n;
	}
	return 0;
}

/* ---- B-trees --------------------------------------------------------------- */

static int read_node(const struct hfsread_volume *v, const struct hfsread_tree *t, uint32_t n, uint8_t *node)
{
	if (n >= t->nodes || hfsread_read(v, &t->fork, (uint64_t)n * t->node_size, node, t->node_size))
		return -1;
	unsigned count = be16(node + 10);
	return 2u * (count + 1) <= t->node_size - NODE_DESCRIPTOR ? 0 : -1;
}

/* Record k of a node: its key, and its data (to `end`) after the key; NULL if the node is damaged. */
static const uint8_t *record(const struct hfsread_volume *v, const struct hfsread_tree *t, const uint8_t *node,
			     unsigned k, const uint8_t **data, const uint8_t **end)
{
	unsigned ns = t->node_size, count = be16(node + 10);
	if (k >= count)
		return NULL;
	unsigned at = be16(node + ns - 2 * (k + 1)), next = be16(node + ns - 2 * (k + 2));
	if (at < NODE_DESCRIPTOR || next < at || next > ns - 2 * (count + 1))
		return NULL;
	const uint8_t *key = node + at;
	unsigned length;
	if (node[8] == NODE_INDEX && !t->variable_keys)
		length = t->max_key;
	else
		length = v->plus ? be16(key) : key[0];
	unsigned span = ((v->plus ? 2 : 1) + length + 1) & ~1u;     /* data starts at an even offset */
	if (span < 8 || at + span > next)
		return NULL;
	*data = key + span;
	*end = node + next;
	return key;
}

/* What a search looks for: a catalog key (parent ID, empty name) or an extents key */
struct target {
	bool catalog;
	uint32_t id;                    /* parent ID, or file ID */
	uint8_t fork;
	uint32_t start;
};

static int compare(uint32_t a, uint32_t b)
{
	return a < b ? -1 : a > b;
}

/* A key against the target: names compare only as empty or not, which the search needs no more than */
static int key_compare(const struct hfsread_volume *v, const struct target *t, const uint8_t *key)
{
	if (t->catalog) {
		int c = compare(be32(key + 2), t->id);
		return c ? c : (v->plus ? be16(key + 6) : key[6]) != 0;
	}
	int c = compare(be32(key + (v->plus ? 4 : 2)), t->id);
	if (!c)
		c = compare(key[v->plus ? 2 : 1], t->fork);
	if (!c)
		c = compare(v->plus ? be32(key + 8) : be16(key + 6), t->start);
	return c;
}

/*
 * Finds the last leaf record whose key is at most the target. Leaves its
 * node in `node` and returns its index, or -1 if all the leaf's keys are
 * greater (the first leaf); -2 if the tree is empty or damaged.
 */
static int search(const struct hfsread_volume *v, const struct hfsread_tree *t, const struct target *target,
		  uint8_t *node)
{
	uint32_t n = t->root;
	for (unsigned level = 0; level < MAX_DEPTH && n; level++) {
		if (read_node(v, t, n, node))
			return -2;
		unsigned count = be16(node + 10);
		int last = -1;
		const uint8_t *data, *end;
		for (unsigned k = 0; k < count; k++) {
			const uint8_t *key = record(v, t, node, k, &data, &end);
			if (!key)
				return -2;
			if (key_compare(v, target, key) > 0)
				break;
			last = (int)k;
		}
		if (node[8] == NODE_LEAF)
			return last;
		if (node[8] != NODE_INDEX || !count || !record(v, t, node, last < 0 ? 0 : (unsigned)last, &data, &end) ||
		    end - data < 4)
			return -2;
		n = be32(data);
	}
	return -2;
}

static int open_tree(const struct hfsread_volume *v, struct hfsread_tree *t)
{
	uint8_t node[SECTOR];
	if (hfsread_read(v, &t->fork, 0, node, SECTOR) || node[8] != NODE_HEADER)
		return -1;
	const uint8_t *h = node + NODE_DESCRIPTOR;
	t->root = be32(h + 2);
	t->first_leaf = be32(h + 10);
	t->node_size = be16(h + 18);
	t->max_key = be16(h + 20);
	t->nodes = be32(h + 22);
	t->variable_keys = v->plus && (be32(h + 38) & BT_VARIABLE_INDEX_KEYS);
	if (t->node_size < SECTOR || t->node_size > MAX_NODE_SIZE || (t->node_size & (t->node_size - 1)) ||
	    (uint64_t)t->nodes * t->node_size > t->fork.size)
		return -1;
	return 0;
}

static int extents_lookup(const struct hfsread_volume *v, const struct hfsread_fork *f, uint32_t block,
			  uint32_t *phys, uint32_t *run)
{
	const struct hfsread_tree *t = &v->extents;
	const struct target target = { false, f->id, f->type, block };
	uint8_t *node = malloc(t->node_size);
	int rc = -1;
	if (!node)
		return -1;
	int k = search(v, t, &target, node);
	const uint8_t *data, *end, *key = k >= 0 ? record(v, t, node, (unsigned)k, &data, &end) : NULL;
	if (key && be32(key + (v->plus ? 4 : 2)) == f->id && key[v->plus ? 2 : 1] == f->type) {
		struct hfsread_extent ext[8];
		unsigned count = v->plus ? 8 : 3;
		if (end - data >= (v->plus ? 64 : 12)) {
			for (unsigned j = 0; j < count; j++)
				ext[j] = v->plus ? (struct hfsread_extent){ be32(data + 8 * j), be32(data + 8 * j + 4) }
						 : (struct hfsread_extent){ be16(data + 4 * j), be16(data + 4 * j + 2) };
			rc = in_extents(ext, count, v->plus ? be32(key + 8) : be16(key + 6), block, phys, run);
		}
	}
	free(node);
	return rc;
}

/* ---- volumes --------------------------------------------------------------- */

static int open_plus(struct hfsread_volume *v, uint64_t offset, uint64_t size)
{
	uint8_t h[SECTOR];
	if (size < HEADER_OFFSET + SECTOR || vread(v, h, SECTOR, offset + HEADER_OFFSET) ||
	    (be16(h) != SIG_HFS_PLUS && be16(h) != SIG_HFSX))
		return -1;
	v->plus = true;
	v->offset = offset;
	v->attributes = be32(h + VH_ATTRIBUTES);
	v->block_size = be32(h + VH_BLOCK_SIZE);
	v->blocks = be32(h + VH_TOTAL_BLOCKS);
	v->blessed = be32(h + VH_FINDER_INFO);
	if (v->block_size < SECTOR || (v->block_size & (v->block_size - 1)))
		return -1;
	plus_fork(&v->extents.fork, h + VH_EXTENTS_FILE);
	plus_fork(&v->catalog.fork, h + VH_CATALOG_FILE);
	return 0;
}

static int open_volume(struct hfsread_volume *v, uint64_t offset, uint64_t size)
{
	uint8_t mdb[SECTOR];
	if (size < HEADER_OFFSET + SECTOR || vread(v, mdb, SECTOR, offset + HEADER_OFFSET))
		return -1;
	if (be16(mdb) == SIG_HFS) {
		uint32_t al_blk_siz = be32(mdb + MDB_AL_BLK_SIZ);
		uint64_t first = (uint64_t)be16(mdb + MDB_AL_BL_ST) * SECTOR;
		if (!al_blk_siz || al_blk_siz % SECTOR || first >= size)
			return -1;
		if (be16(mdb + MDB_EMBED_SIG) == SIG_HFS_PLUS) {
			uint64_t at = first + (uint64_t)be16(mdb + MDB_EMBED_EXTENT) * al_blk_siz;
			uint64_t len = (uint64_t)be16(mdb + MDB_EMBED_EXTENT + 2) * al_blk_siz;
			if (at >= size || open_plus(v, offset + at, len < size - at ? len : size - at))
				return -1;
			v->wrapped = true;
		} else {
			v->offset = offset + first;
			v->block_size = al_blk_siz;
			v->blocks = be16(mdb + MDB_NM_AL_BLKS);
			v->blessed = be32(mdb + MDB_FNDR_INFO);
			v->attributes = be16(mdb + MDB_ATRB);
			v->extents.fork.size = be32(mdb + MDB_XT_FL_SIZ);
			hfs_extents(&v->extents.fork, mdb + MDB_XT_EXT_REC);
			v->catalog.fork.size = be32(mdb + MDB_CT_FL_SIZ);
			hfs_extents(&v->catalog.fork, mdb + MDB_CT_EXT_REC);
		}
	} else if (open_plus(v, offset, size)) {
		return -1;
	}
	v->extents.fork.id = EXTENTS_FILE_ID;
	v->catalog.fork.id = CATALOG_FILE_ID;
	/* without a usable extents file, files can be read as far as their first extents go */
	if (open_tree(v, &v->extents))
		v->extents.root = 0;
	return open_tree(v, &v->catalog);
}

int hfsread_open(struct hfsread_volume *v, int fd, uint64_t offset, uint64_t size)
{
	memset(v, 0, sizeof *v);
	v->fd = fd;
	return open_volume(v, offset, size);
}

int hfsread_open_with(struct hfsread_volume *v, int (*read)(void *ctx, void *buf, size_t len, uint64_t at), void *ctx,
		      uint64_t size)
{
	memset(v, 0, sizeof *v);
	v->fd = -1;
	v->read = read;
	v->ctx = ctx;
	return open_volume(v, 0, size);
}

/* A catalog record's type (folder, file, thread), from its first byte (HFS) or halfword */
static int record_type(const struct hfsread_volume *v, const uint8_t *data)
{
	return v->plus ? be16(data) : data[0];
}

static void file_from(const struct hfsread_volume *v, const uint8_t *key, const uint8_t *data,
		      struct hfsread_file *f)
{
	memset(f, 0, sizeof *f);
	f->parent = be32(key + 2);
	if (v->plus) {
		name_utf8(true, key + 8, be16(key + 6), f->name, sizeof f->name);
		f->id = be32(data + 8);
		f->type = be32(data + 48);
		f->creator = be32(data + 52);
		f->created = be32(data + 12);
		f->modified = be32(data + 16);
		memcpy(f->finder, data + 48, 32);
		plus_fork(&f->data, data + 88);
		plus_fork(&f->rsrc, data + 168);
	} else {
		name_utf8(false, key + 7, key[6], f->name, sizeof f->name);
		memcpy(f->mac_name, key + 6, 1u + key[6]);
		f->id = be32(data + 20);
		f->type = be32(data + 4);
		f->creator = be32(data + 8);
		f->created = be32(data + 44);
		f->modified = be32(data + 48);
		memcpy(f->finder, data + 4, 16);
		memcpy(f->finder + 16, data + 56, 16);
		f->data.size = be32(data + 26);
		hfs_extents(&f->data, data + 74);
		f->rsrc.size = be32(data + 36);
		hfs_extents(&f->rsrc, data + 86);
	}
	f->data.id = f->rsrc.id = f->id;
	f->rsrc.type = 0xFF;
}

static void folder_from(const struct hfsread_volume *v, const uint8_t *key, const uint8_t *data,
			struct hfsread_file *f)
{
	memset(f, 0, sizeof *f);
	f->parent = be32(key + 2);
	f->folder = true;
	if (v->plus) {
		name_utf8(true, key + 8, be16(key + 6), f->name, sizeof f->name);
		f->id = be32(data + 8);
		f->created = be32(data + 12);
		f->modified = be32(data + 16);
		memcpy(f->finder, data + 48, 32);
	} else {
		name_utf8(false, key + 7, key[6], f->name, sizeof f->name);
		memcpy(f->mac_name, key + 6, 1u + key[6]);
		f->id = be32(data + 6);
		f->created = be32(data + 10);
		f->modified = be32(data + 14);
		memcpy(f->finder, data + 22, 32);
	}
}

/* Whether a key's name fits in its record (the search only looks at its length) */
static bool name_fits(const struct hfsread_volume *v, const uint8_t *key, const uint8_t *data)
{
	return v->plus ? key + 8 + 2u * be16(key + 6) <= data : key + 7 + key[6] <= data;
}

/* The folder's first file (in catalog order) of `type` (0: any), or named `name` (NULL: any; folders too): 1, 0, -1 */
static int scan(const struct hfsread_volume *v, uint32_t folder, uint32_t type, const char *name,
		struct hfsread_file *f)
{
	const struct hfsread_tree *t = &v->catalog;
	const struct target target = { true, folder, 0, 0 };
	uint8_t *node = malloc(t->node_size);
	int rc = -1;
	if (!node)
		return -1;
	int k = search(v, t, &target, node);
	if (k == -2)
		goto out;
	/* the folder's records follow its thread record, whose key is (folder, "") */
	for (unsigned visited = 0, at = (unsigned)(k + 1); visited < t->nodes;) {
		if (at >= be16(node + 10)) {
			uint32_t n = be32(node);            /* the next leaf */
			if (!n) {
				rc = 0;
				break;
			}
			if (read_node(v, t, n, node) || node[8] != NODE_LEAF)
				break;
			at = 0;
			visited++;
			continue;
		}
		const uint8_t *data, *end, *key = record(v, t, node, at++, &data, &end);
		if (!key)
			break;
		uint32_t parent = be32(key + 2);
		if (parent > folder) {
			rc = 0;
			break;
		}
		int kind = record_type(v, data);
		if (parent < folder || (kind != CAT_FILE && (kind != CAT_FOLDER || !name)))
			continue;
		size_t need = kind == CAT_FILE ? (v->plus ? PLUS_FILE_SIZE : HFS_FILE_SIZE) :
						 (v->plus ? PLUS_FOLDER_SIZE : HFS_FOLDER_SIZE);
		if ((size_t)(end - data) < need || !name_fits(v, key, data))
			break;
		if (name) {
			if (kind == CAT_FILE)
				file_from(v, key, data, f);
			else
				folder_from(v, key, data, f);
			if (strcasecmp(f->name, name) == 0 && (!type || f->type == type)) {
				rc = 1;
				break;
			}
		} else if (be32(data + (v->plus ? 48 : 4)) == type) {
			file_from(v, key, data, f);
			rc = 1;
			break;
		}
	}
out:
	free(node);
	return rc;
}

int hfsread_find(const struct hfsread_volume *v, uint32_t folder, uint32_t type, struct hfsread_file *f)
{
	return scan(v, folder, type, NULL, f);
}

int hfsread_lookup(const struct hfsread_volume *v, uint32_t folder, const char *name, struct hfsread_file *f)
{
	return scan(v, folder, 0, name, f);
}

int hfsread_walk(const struct hfsread_volume *v, int (*fn)(void *ctx, const struct hfsread_file *f), void *ctx)
{
	const struct hfsread_tree *t = &v->catalog;
	uint8_t *node = malloc(t->node_size);
	struct hfsread_file *f = malloc(sizeof *f);
	int rc = -1;
	if (!node || !f)
		goto out;
	uint32_t n = t->first_leaf;
	for (unsigned visited = 0; n && visited < t->nodes; visited++, n = be32(node)) {
		if (read_node(v, t, n, node) || node[8] != NODE_LEAF)
			goto out;
		for (unsigned k = 0; k < be16(node + 10); k++) {
			const uint8_t *data, *end, *key = record(v, t, node, k, &data, &end);
			if (!key)
				goto out;
			int type = record_type(v, data);
			size_t size = (size_t)(end - data);
			if (type == CAT_FOLDER && size >= (v->plus ? PLUS_FOLDER_SIZE : HFS_FOLDER_SIZE) &&
			    name_fits(v, key, data))
				folder_from(v, key, data, f);
			else if (type == CAT_FILE && size >= (v->plus ? PLUS_FILE_SIZE : HFS_FILE_SIZE) &&
				 name_fits(v, key, data))
				file_from(v, key, data, f);
			else if (type == CAT_FOLDER || type == CAT_FILE)
				goto out;
			else
				continue;
			if ((rc = fn(ctx, f)))
				goto out;
		}
	}
	rc = n ? -1 : 0;
out:
	free(node);
	free(f);
	return rc;
}

/* A folder's (or file's) thread record: its parent and name */
static int thread(const struct hfsread_volume *v, uint32_t id, uint32_t *parent, char *name, size_t len)
{
	const struct hfsread_tree *t = &v->catalog;
	const struct target target = { true, id, 0, 0 };
	uint8_t *node = malloc(t->node_size);
	int rc = -1;
	if (!node)
		return -1;
	int k = search(v, t, &target, node);
	const uint8_t *data, *end, *key = k >= 0 ? record(v, t, node, (unsigned)k, &data, &end) : NULL;
	if (key && !key_compare(v, &target, key) &&
	    (record_type(v, data) == CAT_FOLDER_THREAD || record_type(v, data) == CAT_FILE_THREAD)) {
		size_t size = (size_t)(end - data);
		if (v->plus && size >= PLUS_THREAD_MIN && size >= PLUS_THREAD_MIN + 2u * be16(data + 8)) {
			*parent = be32(data + 4);
			name_utf8(true, data + 10, be16(data + 8), name, len);
			rc = 0;
		} else if (!v->plus && size >= HFS_THREAD_SIZE && data[14] <= 31) {
			*parent = be32(data + 10);
			name_utf8(false, data + 15, data[14], name, len);
			rc = 0;
		}
	}
	free(node);
	return rc;
}

int hfsread_path(const struct hfsread_volume *v, uint32_t folder, char *buf, size_t len)
{
	char path[HFSREAD_PATH_MAX], name[HFSREAD_NAME_MAX];
	size_t at = sizeof path - 1;
	path[at] = '\0';
	for (unsigned depth = 0; folder != ROOT_PARENT_ID; depth++) {
		uint32_t parent;
		if (depth >= 64 || thread(v, folder, &parent, name, sizeof name))
			return -1;
		size_t n = strlen(name);
		if (n + (depth > 0) > at)
			return -1;
		if (depth > 0)
			path[--at] = ':';
		at -= n;
		memcpy(path + at, name, n);
		folder = parent;
	}
	snprintf(buf, len, "%s", path + at);
	return 0;
}

/* ---- the startup disk ------------------------------------------------------ */

/* The file in partition p's blessed System Folder: 1 found (and read, or -1), 0 not there */
static int system_file(struct disk *d, unsigned p, const char *path, uint32_t type, struct hfsread_found *found,
		       char *err, size_t errlen)
{
	struct hfsread_volume v;
	struct hfsread_file f;
	struct disk_reader r = { d, p };
	if (hfsread_open_with(&v, disk_read, &r, d->part[p].size) || !v.blessed || hfsread_find(&v, v.blessed, type, &f) != 1)
		return 0;
	char folder[HFSREAD_PATH_MAX];
	if (hfsread_path(&v, v.blessed, folder, sizeof folder))
		snprintf(folder, sizeof folder, "?");
	found->part = p;
	snprintf(found->partition, sizeof found->partition, "%s", d->part[p].name);
	if (snprintf(found->path, sizeof found->path, "%s:%s", folder, f.name) >= (int)sizeof found->path)
		memcpy(found->path + sizeof found->path - 4, "...", 4);
	int rc = 1;
	if (f.data.size > MAX_FILE_SIZE)
		rc = fail(err, errlen, "%s: \"%s\" is too big (%llu bytes)", path, found->path,
			  (unsigned long long)f.data.size);
	else if (!(found->data = malloc(f.data.size ? (size_t)f.data.size : 1)))
		rc = fail(err, errlen, "out of memory");
	else if (hfsread_read(&v, &f.data, 0, found->data, (size_t)f.data.size))
		rc = fail(err, errlen, "%s: cannot read \"%s\"", path, found->path);
	found->size = (size_t)f.data.size;
	if (rc < 0) {
		free(found->data);
		found->data = NULL;
	}
	return rc;
}

int hfsread_system_file(const char *const *paths, unsigned count, uint32_t type, struct hfsread_found *found,
			char *err, size_t errlen)
{
	memset(found, 0, sizeof *found);
	err[0] = '\0';
	for (unsigned k = 0; k < count; k++) {
		struct disk d;
		if (disk_open(&d, NULL, paths[k], true, cdimage_has_toc(paths[k])))     /* (any image, read-only) */
			return -1;
		for (unsigned p = 0; p < d.partitions; p++) {
			int rc = system_file(&d, p, paths[k], type, found, err, errlen);
			if (rc) {
				found->drive = k;
				disk_close(&d);
				return rc;
			}
		}
		disk_close(&d);
	}
	return 0;
}

int hfsread_system_file_in(const char *path, unsigned map_entry, uint32_t type, struct hfsread_found *found,
			   char *err, size_t errlen)
{
	struct disk d;
	memset(found, 0, sizeof *found);
	err[0] = '\0';
	if (disk_open(&d, NULL, path, true, cdimage_has_toc(path)))
		return -1;
	int rc = 0;
	for (unsigned p = 0; p < d.partitions && !rc; p++)
		if (d.part[p].map_entry == map_entry)
			rc = system_file(&d, p, path, type, found, err, errlen);
	disk_close(&d);
	return rc;
}

int hfsread_volumes(const char *path, int (*fn)(void *ctx, const struct hfsread_volume *v), void *ctx)
{
	struct disk d;
	if (disk_open(&d, NULL, path, true, cdimage_has_toc(path)))
		return -1;
	int rc = 0;
	for (unsigned p = 0; p < d.partitions && !rc; p++) {
		struct hfsread_volume v;
		struct disk_reader r = { &d, p };
		if (!hfsread_open_with(&v, disk_read, &r, d.part[p].size))
			rc = fn(ctx, &v);
	}
	disk_close(&d);
	return rc;
}
