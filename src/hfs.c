#include "hfs.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "be.h"

#define SECTOR 512u
#define NODE 512u
#define MAX_ALLOC_BLOCKS 65535u
#define MAP_NODES 2048u                 /* nodes a header node's map record covers */

#define ROOT_PAR_ID 1u
#define ROOT_DIR_ID 2u
#define FIRST_USER_CNID 16u

/* Master directory block fields */
enum {
	MDB_SIG = 0, MDB_CR_DATE = 2, MDB_LS_MOD = 6, MDB_ATRB = 10, MDB_NM_FLS = 12,
	MDB_VBM_ST = 14, MDB_ALLOC_PTR = 16, MDB_NM_AL_BLKS = 18, MDB_AL_BLK_SIZ = 20,
	MDB_CLP_SIZ = 24, MDB_AL_BL_ST = 28, MDB_NXT_CNID = 30, MDB_FREE_BKS = 34, MDB_VN = 36,
	MDB_XT_CLP_SIZ = 74, MDB_CT_CLP_SIZ = 78, MDB_FIL_CNT = 84, MDB_DIR_CNT = 88, MDB_XT_FL_SIZ = 130,
	MDB_XT_EXT_REC = 134,
	MDB_CT_FL_SIZ = 146, MDB_CT_EXT_REC = 150,
};
#define VOLUME_UNMOUNTED_CLEANLY 0x0100u
#define MAX_FILES 64u
#define CATALOG_KEY_MAX 37u
#define FILE_NAME_MAX 31u

uint32_t hfs_mac_time_now(void)
{
	const char *epoch = getenv("SOURCE_DATE_EPOCH");        /* reproducible images */
	time_t t = epoch ? (time_t)strtoll(epoch, NULL, 10) : time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	return (uint32_t)((int64_t)t + tm.tm_gmtoff + 2082844800);
}

/* Sets the record offsets at the end of a node: records start at `offsets[0..n-1]`, free space at offsets[n]. */
static void node_offsets(uint8_t *node, const uint16_t *offsets, unsigned n)
{
	for (unsigned k = 0; k <= n; k++)
		put_be16(node + NODE - 2 * (k + 1), offsets[k]);
}

/* B-tree header node (node 0). `used` nodes are marked in the map, starting at node 0. */
static void header_node(uint8_t *node, unsigned depth, uint32_t root, uint32_t leaf_records,
			uint32_t first_leaf, uint32_t last_leaf, unsigned max_key, uint32_t nodes, uint32_t used)
{
	memset(node, 0, NODE);
	node[8] = 1;                            /* ndType: header node */
	put_be16(node + 10, 3);                 /* records: header, reserved, map */
	uint8_t *h = node + 14;
	put_be16(h + 0, (uint16_t)depth);
	put_be32(h + 2, root);
	put_be32(h + 6, leaf_records);
	put_be32(h + 10, first_leaf);
	put_be32(h + 14, last_leaf);
	put_be16(h + 18, NODE);
	put_be16(h + 20, (uint16_t)max_key);
	put_be32(h + 22, nodes);
	put_be32(h + 26, nodes - used);
	for (uint32_t k = 0; k < used; k++)
		node[248 + k / 8] |= (uint8_t)(0x80u >> (k % 8));
	static const uint16_t offsets[4] = { 14, 120, 248, NODE - 8 };
	node_offsets(node, offsets, 3);
}

/* A catalog key: length, reserved, parent ID, name; padded to an even size. Returns its size. */
static size_t catalog_key(uint8_t *p, uint32_t parent, const char *name)
{
	size_t n = strlen(name);
	p[0] = (uint8_t)(6 + n);
	p[1] = 0;
	put_be32(p + 2, parent);
	p[6] = (uint8_t)n;
	memcpy(p + 7, name, n);
	return (7 + n + 1) & ~(size_t)1;
}

/* A catalog record: key and data, as stored in a leaf node */
struct record {
	uint8_t b[160];
	size_t len;
	uint32_t parent;
	const char *name;
};

/* The catalog orders names without regard to case (names here are letters, digits and spaces) */
static int name_order(const char *a, const char *b)
{
	for (;; a++, b++) {
		int x = toupper((unsigned char)*a), y = toupper((unsigned char)*b);
		if (x != y || !x)
			return x - y;
	}
}

static int record_order(const void *x, const void *y)
{
	const struct record *a = x, *b = y;
	if (a->parent != b->parent)
		return a->parent < b->parent ? -1 : 1;
	return name_order(a->name, b->name);
}

/* Fork extents: the first of the three in an extent record, and the logical and physical lengths */
struct fork {
	uint16_t start, count;
	uint32_t len;
};

static void file_record(struct record *r, const struct hfs_file *f, uint32_t cnid, const struct fork *data,
			const struct fork *rsrc, uint32_t al_blk_siz, uint32_t now)
{
	memset(r, 0, sizeof *r);
	r->parent = ROOT_DIR_ID;
	r->name = f->name;
	size_t at = catalog_key(r->b, ROOT_DIR_ID, f->name);
	uint8_t *p = r->b + at;
	p[0] = 2;                               /* cdrFilRec */
	put_be32(p + 4, f->type);               /* FInfo */
	put_be32(p + 8, f->creator);
	put_be16(p + 12, f->finder_flags);
	put_be16(p + 14, (uint16_t)f->v);
	put_be16(p + 16, (uint16_t)f->h);
	put_be32(p + 20, cnid);
	put_be32(p + 26, data->len);
	put_be32(p + 30, (uint32_t)data->count * al_blk_siz);
	put_be32(p + 36, rsrc->len);
	put_be32(p + 40, (uint32_t)rsrc->count * al_blk_siz);
	put_be32(p + 44, now);                  /* created */
	put_be32(p + 48, now);                  /* modified */
	put_be16(p + 74, data->start);          /* first extents */
	put_be16(p + 76, data->count);
	put_be16(p + 86, rsrc->start);
	put_be16(p + 88, rsrc->count);
	r->len = at + 102;
}

/* The root folder and its thread */
static void root_records(struct record *dir, struct record *thread, const char *name, unsigned valence,
			 uint32_t now)
{
	memset(dir, 0, sizeof *dir);
	dir->parent = ROOT_PAR_ID;
	dir->name = name;
	size_t at = catalog_key(dir->b, ROOT_PAR_ID, name);
	uint8_t *f = dir->b + at;
	f[0] = 1;                               /* cdrDirRec */
	put_be16(f + 4, (uint16_t)valence);
	put_be32(f + 6, ROOT_DIR_ID);
	put_be32(f + 10, now);                  /* created */
	put_be32(f + 14, now);                  /* modified */
	dir->len = at + 70;

	memset(thread, 0, sizeof *thread);
	thread->parent = ROOT_DIR_ID;
	thread->name = "";
	at = catalog_key(thread->b, ROOT_DIR_ID, "");
	uint8_t *t = thread->b + at;
	t[0] = 3;                               /* cdrThdRec */
	put_be32(t + 10, ROOT_PAR_ID);
	t[14] = (uint8_t)strlen(name);
	memcpy(t + 15, name, strlen(name));
	thread->len = at + 46;
}

/*
 * Packs the (sorted) records into leaf nodes 1..; with more than one leaf,
 * an index node follows them. Returns the number of nodes written into
 * `nodes` (from node 1), with the root and the leaves for the header.
 */
static unsigned catalog_nodes(uint8_t *nodes, unsigned max_nodes, const struct record *r, unsigned n,
			      uint32_t *root, uint32_t *last_leaf, unsigned *depth)
{
	unsigned leaves = 0, k = 0;
	unsigned first[MAX_FILES + 2];
	while (k < n) {
		if (leaves + 2 > max_nodes)
			return 0;
		uint8_t *node = nodes + (size_t)leaves * NODE;
		uint16_t offsets[MAX_FILES + 3];
		unsigned count = 0;
		size_t at = 14;
		first[leaves] = k;
		while (k < n && at + r[k].len + 2 * (count + 2) <= NODE) {
			offsets[count++] = (uint16_t)at;
			memcpy(node + at, r[k].b, r[k].len);
			at += r[k].len;
			k++;
		}
		if (!count)
			return 0;
		offsets[count] = (uint16_t)at;
		node[8] = 0xFF;                 /* leaf */
		node[9] = 1;
		put_be16(node + 10, (uint16_t)count);
		node_offsets(node, offsets, count);
		leaves++;
	}
	for (unsigned l = 0; l < leaves; l++) {          /* links between leaves */
		uint8_t *node = nodes + (size_t)l * NODE;
		put_be32(node + 0, l + 1 < leaves ? l + 2 : 0);
		put_be32(node + 4, l ? l : 0);
	}
	*last_leaf = leaves;
	if (leaves == 1) {
		*root = 1;
		*depth = 1;
		return 1;
	}
	/* index node: one record per leaf, keys padded to the maximum length */
	uint8_t *index = nodes + (size_t)leaves * NODE;
	uint16_t offsets[MAX_FILES + 3];
	size_t at = 14;
	for (unsigned l = 0; l < leaves; l++) {
		const uint8_t *key = r[first[l]].b;
		offsets[l] = (uint16_t)at;
		index[at] = CATALOG_KEY_MAX;
		memcpy(index + at + 1, key + 1, key[0]);
		put_be32(index + at + 1 + CATALOG_KEY_MAX, l + 1);
		at += 1 + CATALOG_KEY_MAX + 4;
	}
	offsets[leaves] = (uint16_t)at;
	index[8] = 0;                           /* index node */
	index[9] = 2;
	put_be16(index + 10, (uint16_t)leaves);
	node_offsets(index, offsets, leaves);
	*root = leaves + 1;
	*depth = 2;
	return leaves + 1;
}

static int write_at(int fd, const void *p, size_t n, uint64_t at)
{
	return pwrite(fd, p, n, (off_t)at) == (ssize_t)n ? 0 : -1;
}

static uint32_t clamp_u32(uint64_t v, uint32_t max)
{
	return v > max ? max : (uint32_t)v;
}

int hfs_format(int fd, uint64_t size, const char *name, uint32_t now, const struct hfs_file *files,
	       unsigned nfiles)
{
	uint64_t total = size / SECTOR;
	size_t name_len = strlen(name);
	if (total < 800 || name_len == 0 || name_len > HFS_MAX_NAME || strchr(name, ':') || nfiles > MAX_FILES)
		return -1;
	for (unsigned k = 0; k < nfiles; k++) {
		size_t n = strlen(files[k].name);
		if (n == 0 || n > FILE_NAME_MAX)
			return -1;
		for (size_t c = 0; c < n; c++)
			if (!isalnum((unsigned char)files[k].name[c]) && files[k].name[c] != ' ')
				return -1;
	}

	/* allocation blocks: as small as possible with at most 65535 of them */
	uint32_t spb = (uint32_t)((total - 5 + MAX_ALLOC_BLOCKS - 1) / MAX_ALLOC_BLOCKS);
	uint32_t al_blk_siz = spb * SECTOR;
	uint32_t max_blocks = clamp_u32((total - 5) / spb, MAX_ALLOC_BLOCKS);
	uint32_t bitmap_sectors = (max_blocks + 4095) / 4096;
	uint32_t al_bl_st = 3 + bitmap_sectors;
	uint32_t nblocks = clamp_u32((total - al_bl_st - 2) / spb, MAX_ALLOC_BLOCKS);

	/* B-tree files, in whole allocation blocks, each within one header map */
	uint32_t node_limit = MAP_NODES / spb ? MAP_NODES / spb : 1;
	uint32_t ext_alloc = (8 + spb - 1) / spb;
	uint32_t cat_target = clamp_u32(total / 256, MAP_NODES);
	if (cat_target < 32)
		cat_target = 32;
	uint32_t cat_alloc = (cat_target + spb - 1) / spb;
	if (cat_alloc > node_limit)
		cat_alloc = node_limit;
	if (ext_alloc > node_limit)
		ext_alloc = node_limit;
	uint32_t ext_nodes = clamp_u32((uint64_t)ext_alloc * spb, MAP_NODES);
	uint32_t cat_nodes = clamp_u32((uint64_t)cat_alloc * spb, MAP_NODES);
	if (ext_alloc + cat_alloc >= nblocks)
		return -1;

	/* the files' forks, one extent each, after the B-trees */
	struct fork forks[MAX_FILES][2];
	uint32_t next = ext_alloc + cat_alloc;
	for (unsigned k = 0; k < nfiles; k++)
		for (int f = 0; f < 2; f++) {
			uint32_t len = f ? files[k].rsrc_len : files[k].data_len;
			uint32_t count = (uint32_t)(((uint64_t)len + al_blk_siz - 1) / al_blk_siz);
			if (count > 0xFFFF || next + count > nblocks)
				return -1;
			forks[k][f] = (struct fork){ (uint16_t)(count ? next : 0), (uint16_t)count, len };
			next += count;
		}

	/* the catalog's records, in order */
	static struct record records[MAX_FILES + 2];
	root_records(&records[0], &records[1], name, nfiles, now);
	for (unsigned k = 0; k < nfiles; k++)
		file_record(&records[2 + k], &files[k], FIRST_USER_CNID + k, &forks[k][0], &forks[k][1], al_blk_siz,
			    now);
	qsort(records, nfiles + 2, sizeof records[0], record_order);
	for (unsigned k = 1; k < nfiles + 2; k++)
		if (!record_order(&records[k - 1], &records[k]))
			return -1;              /* two files of the same name */
	uint8_t *cat_nodes_buf = calloc(MAX_FILES + 2, NODE);
	if (!cat_nodes_buf)
		return -1;
	uint32_t root, last_leaf;
	unsigned depth;
	unsigned cat_used = catalog_nodes(cat_nodes_buf, cat_alloc * spb - 1, records, nfiles + 2, &root, &last_leaf,
					  &depth);
	if (!cat_used) {
		free(cat_nodes_buf);
		return -1;
	}

	uint8_t *zero = calloc(1, (size_t)(ext_alloc + cat_alloc) * al_blk_siz + al_bl_st * SECTOR);
	uint8_t mdb[SECTOR] = { 0 }, node[NODE];
	if (!zero)
		return -1;
	int err = write_at(fd, zero, (size_t)al_bl_st * SECTOR, 0);         /* boot blocks, MDB, bitmap */
	err |= write_at(fd, zero, (size_t)(ext_alloc + cat_alloc) * al_blk_siz, (uint64_t)al_bl_st * SECTOR);
	free(zero);

	/* Bitmap: the two B-tree files occupy the first allocation blocks */
	uint8_t *bitmap = calloc(bitmap_sectors, SECTOR);
	if (!bitmap)
		return -1;
	for (uint32_t k = 0; k < next; k++)
		bitmap[k / 8] |= (uint8_t)(0x80u >> (k % 8));
	err |= write_at(fd, bitmap, (size_t)bitmap_sectors * SECTOR, 3 * SECTOR);
	free(bitmap);

	uint64_t ext_start = (uint64_t)al_bl_st * SECTOR;
	uint64_t cat_start = ext_start + (uint64_t)ext_alloc * al_blk_siz;
	header_node(node, 0, 0, 0, 0, 0, 7, ext_nodes, 1);
	err |= write_at(fd, node, NODE, ext_start);
	header_node(node, depth, root, nfiles + 2, 1, last_leaf, CATALOG_KEY_MAX, cat_nodes, 1 + cat_used);
	err |= write_at(fd, node, NODE, cat_start);
	err |= write_at(fd, cat_nodes_buf, (size_t)cat_used * NODE, cat_start + NODE);
	free(cat_nodes_buf);
	for (unsigned k = 0; k < nfiles; k++)
		for (int f = 0; f < 2; f++)
			if (forks[k][f].count)
				err |= write_at(fd, f ? files[k].rsrc : files[k].data, forks[k][f].len,
						(uint64_t)al_bl_st * SECTOR + (uint64_t)forks[k][f].start * al_blk_siz);

	put_be16(mdb + MDB_SIG, 0x4244);                /* 'BD' */
	put_be32(mdb + MDB_CR_DATE, now);
	put_be32(mdb + MDB_LS_MOD, now);
	put_be16(mdb + MDB_ATRB, VOLUME_UNMOUNTED_CLEANLY);
	put_be16(mdb + MDB_NM_FLS, (uint16_t)nfiles);
	put_be16(mdb + MDB_VBM_ST, 3);
	put_be16(mdb + MDB_ALLOC_PTR, (uint16_t)next);
	put_be16(mdb + MDB_NM_AL_BLKS, (uint16_t)nblocks);
	put_be32(mdb + MDB_AL_BLK_SIZ, al_blk_siz);
	put_be32(mdb + MDB_CLP_SIZ, 4 * al_blk_siz);
	put_be16(mdb + MDB_AL_BL_ST, (uint16_t)al_bl_st);
	put_be32(mdb + MDB_NXT_CNID, FIRST_USER_CNID + nfiles);
	put_be16(mdb + MDB_FREE_BKS, (uint16_t)(nblocks - next));
	mdb[MDB_VN] = (uint8_t)name_len;
	memcpy(mdb + MDB_VN + 1, name, name_len);
	put_be32(mdb + MDB_XT_CLP_SIZ, ext_alloc * al_blk_siz);
	put_be32(mdb + MDB_CT_CLP_SIZ, cat_alloc * al_blk_siz);
	put_be32(mdb + MDB_FIL_CNT, nfiles);
	put_be32(mdb + MDB_DIR_CNT, 0);
	put_be32(mdb + MDB_XT_FL_SIZ, ext_nodes * NODE);
	put_be16(mdb + MDB_XT_EXT_REC + 2, (uint16_t)ext_alloc);       /* starts at block 0 */
	put_be32(mdb + MDB_CT_FL_SIZ, cat_nodes * NODE);
	put_be16(mdb + MDB_CT_EXT_REC + 0, (uint16_t)ext_alloc);
	put_be16(mdb + MDB_CT_EXT_REC + 2, (uint16_t)cat_alloc);
	err |= write_at(fd, mdb, SECTOR, 2 * SECTOR);
	err |= write_at(fd, mdb, SECTOR, (total - 2) * SECTOR);         /* alternate MDB */
	return err ? -1 : 0;
}
