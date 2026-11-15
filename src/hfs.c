#include "hfs.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "be.h"

#define SECTOR 512u
#define NODE 512u
#define MAX_ALLOC_BLOCKS 65535u
#define MAX_SECTORS_PER_BLOCK (1u << 17)
#define HEADER_MAP_NODES 2048u          /* nodes a header node's map record covers (256 bytes) */
#define MAP_NODE_NODES 3936u            /* ... and a map node's (492 bytes) */
#define MAC_EPOCH 2082844800            /* 1904-01-01 in Unix time */

#define ROOT_PAR_ID 1u
#define ROOT_DIR_ID 2u

/* Master directory block fields */
enum {
	MDB_SIG = 0, MDB_CR_DATE = 2, MDB_LS_MOD = 6, MDB_ATRB = 10, MDB_NM_FLS = 12,
	MDB_VBM_ST = 14, MDB_ALLOC_PTR = 16, MDB_NM_AL_BLKS = 18, MDB_AL_BLK_SIZ = 20,
	MDB_CLP_SIZ = 24, MDB_AL_BL_ST = 28, MDB_NXT_CNID = 30, MDB_FREE_BKS = 34, MDB_VN = 36,
	MDB_XT_CLP_SIZ = 74, MDB_CT_CLP_SIZ = 78, MDB_NM_RT_DIRS = 82, MDB_FIL_CNT = 84, MDB_DIR_CNT = 88,
	MDB_XT_FL_SIZ = 130, MDB_XT_EXT_REC = 134, MDB_CT_FL_SIZ = 146, MDB_CT_EXT_REC = 150,
};

/* B-tree nodes */
#define NODE_INDEX 0x00u
#define NODE_HEADER 0x01u
#define NODE_MAP 0x02u
#define NODE_LEAF 0xFFu
#define CATALOG_KEY_MAX 37u
#define EXTENTS_KEY_MAX 7u
#define INDEX_RECORD (1u + CATALOG_KEY_MAX + 4u)
#define INDEX_FANOUT 11u                /* index records in a node */

/* Catalog records */
#define CAT_FOLDER 1u
#define CAT_FILE 2u
#define CAT_THREAD 3u
#define FOLDER_SIZE 70u
#define FILE_SIZE 102u
#define THREAD_SIZE 46u
#define HAS_CUSTOM_ICON 0x0400u         /* (Finder flags) */

/* ---- names and dates ----------------------------------------------------- */

const uint16_t hfs_mac_roman[128] = {
	0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7,
	0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5,
	0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9,
	0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
	0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248,
	0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D,
	0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7,
	0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
	0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD,
	0x02DB, 0x02C7,
};

/*
 * Each character's place in the catalog's order: the ranks of the sort
 * words of Apple's FastRelString (the HFS code in Apple's hfs sources),
 * which hfsutils' and Linux's tables agree with. Letters rank with their
 * upper case, each accented letter after its plain one; a non-breaking
 * space ranks with a space.
 */
static const uint8_t name_rank[256] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
	0x20, 0x21, 0x22, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
	0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45,
	0x46, 0x47, 0x51, 0x52, 0x54, 0x55, 0x5A, 0x5B, 0x5C, 0x5D, 0x62, 0x63, 0x64, 0x65, 0x66, 0x68,
	0x71, 0x72, 0x73, 0x74, 0x76, 0x77, 0x7C, 0x7D, 0x7E, 0x7F, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86,
	0x4D, 0x47, 0x51, 0x52, 0x54, 0x55, 0x5A, 0x5B, 0x5C, 0x5D, 0x62, 0x63, 0x64, 0x65, 0x66, 0x68,
	0x71, 0x72, 0x73, 0x74, 0x76, 0x77, 0x7C, 0x7D, 0x7E, 0x7F, 0x81, 0x87, 0x88, 0x89, 0x8A, 0x8B,
	0x49, 0x4B, 0x53, 0x56, 0x67, 0x69, 0x78, 0x4E, 0x48, 0x4F, 0x49, 0x4A, 0x4B, 0x53, 0x56, 0x57,
	0x58, 0x59, 0x5E, 0x5F, 0x60, 0x61, 0x67, 0x6D, 0x6E, 0x6F, 0x69, 0x6A, 0x79, 0x7A, 0x7B, 0x78,
	0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92, 0x75, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x4C, 0x6B,
	0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0x50, 0x70, 0xA4, 0x4C, 0x6B,
	0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0x25, 0x26, 0xAC, 0x20, 0x48, 0x4A, 0x6A, 0x6C, 0x6C,
	0xAD, 0xAE, 0x23, 0x24, 0x2C, 0x2D, 0xAF, 0xB0, 0x80, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
	0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
	0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
};

int hfs_compare_names(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
	size_t n = alen < blen ? alen : blen;
	for (size_t k = 0; k < n; k++)
		if (name_rank[a[k]] != name_rank[b[k]])
			return name_rank[a[k]] < name_rank[b[k]] ? -1 : 1;
	return alen < blen ? -1 : alen > blen;
}

/* The next character of UTF-8 (U+FFFD for a malformed one) */
static uint32_t utf8_next(const char **s)
{
	const uint8_t *p = (const uint8_t *)*s;
	uint32_t c = *p++;
	unsigned more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
	if (c >= 0x80 && !more)
		c = 0xFFFD;
	else if (more)
		c &= 0x3Fu >> more;
	for (; more; more--, p++) {
		if ((*p & 0xC0) != 0x80) {
			c = 0xFFFD;
			break;
		}
		c = c << 6 | (*p & 0x3Fu);
	}
	*s = (const char *)p;
	return c;
}

/* Latin-1's letters with a diacritical mark, U+00C0 on: the letter and the combining mark (U+0300 on) */
static const struct {
	char letter;
	uint8_t mark;
} latin1_marks[0x1D] = {
	{ 'A', 0x00 }, { 'A', 0x01 }, { 'A', 0x02 }, { 'A', 0x03 }, { 'A', 0x08 }, { 'A', 0x0A }, { 0, 0 },
	{ 'C', 0x27 }, { 'E', 0x00 }, { 'E', 0x01 }, { 'E', 0x02 }, { 'E', 0x08 }, { 'I', 0x00 }, { 'I', 0x01 },
	{ 'I', 0x02 }, { 'I', 0x08 }, { 0, 0 }, { 'N', 0x03 }, { 'O', 0x00 }, { 'O', 0x01 }, { 'O', 0x02 },
	{ 'O', 0x03 }, { 'O', 0x08 }, { 0, 0 }, { 0, 0 }, { 'U', 0x00 }, { 'U', 0x01 }, { 'U', 0x02 }, { 'U', 0x08 },
};

/* A letter and a combining mark as one character (Mac OS Roman has these), or 0 */
static uint32_t compose(uint32_t letter, uint32_t mark)
{
	for (uint32_t k = 0; k < 0x1D; k++) {
		if (!latin1_marks[k].letter || mark != 0x300u + latin1_marks[k].mark)
			continue;
		if (letter == (uint32_t)latin1_marks[k].letter)
			return 0xC0 + k;
		if (letter == (uint32_t)latin1_marks[k].letter + 0x20)
			return 0xE0 + k;
	}
	if (mark == 0x308 && (letter == 'y' || letter == 'Y'))
		return letter == 'y' ? 0xFF : 0x178;
	return 0;
}

static int mac_roman_of(uint32_t c)
{
	if (c < 0x80)
		return (int)c;
	for (unsigned k = 0; k < 128; k++)
		if (hfs_mac_roman[k] == c)
			return (int)(0x80 + k);
	return -1;
}

static bool combining(uint32_t c)
{
	return c >= 0x300 && c < 0x370;
}

size_t hfs_from_utf8(const char *s, uint8_t *out, size_t max)
{
	size_t n = 0;
	uint32_t c = *s ? utf8_next(&s) : 0;
	while (c) {
		uint32_t next = *s ? utf8_next(&s) : 0, both;
		for (; combining(next); next = *s ? utf8_next(&s) : 0)
			if ((both = compose(c, next)))
				c = both;
		int m = mac_roman_of(c);
		if (n < max)
			out[n] = m < 0 ? '_' : (uint8_t)m;
		n++;
		c = next;
	}
	return n;
}

size_t hfs_to_utf8(const uint8_t *s, size_t n, char *out, size_t len)
{
	size_t at = 0;
	for (size_t k = 0; k < n; k++) {
		uint32_t c = s[k] < 0x80 ? s[k] : hfs_mac_roman[s[k] - 0x80];
		uint8_t b[3];
		size_t m;
		if (c < 0x80) {
			b[0] = (uint8_t)c;
			m = 1;
		} else if (c < 0x800) {
			b[0] = (uint8_t)(0xC0 | c >> 6);
			b[1] = (uint8_t)(0x80 | (c & 0x3F));
			m = 2;
		} else {
			b[0] = (uint8_t)(0xE0 | c >> 12);
			b[1] = (uint8_t)(0x80 | (c >> 6 & 0x3F));
			b[2] = (uint8_t)(0x80 | (c & 0x3F));
			m = 3;
		}
		if (at + m >= len)
			break;
		memcpy(out + at, b, m);
		at += m;
	}
	if (len)
		out[at] = '\0';
	return at;
}

uint32_t hfs_mac_time(int64_t t)
{
	time_t tt = (time_t)t;
	struct tm tm;
	localtime_r(&tt, &tm);
	int64_t m = t + tm.tm_gmtoff + MAC_EPOCH;
	return m < 0 ? 0 : m > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)m;
}

int64_t hfs_unix_time(uint32_t mac_time)
{
	int64_t t = (int64_t)mac_time - MAC_EPOCH;
	time_t tt = (time_t)t;
	struct tm tm;
	localtime_r(&tt, &tm);
	return t - tm.tm_gmtoff;
}

uint32_t hfs_mac_time_now(void)
{
	const char *epoch = getenv("SOURCE_DATE_EPOCH");        /* reproducible images */
	return hfs_mac_time(epoch ? strtoll(epoch, NULL, 10) : (int64_t)time(NULL));
}

/* ---- B-trees ---------------------------------------------------------------- */

/* Sets the record offsets at the end of a node: records start at `offsets[0..n-1]`, free space at offsets[n]. */
static void node_offsets(uint8_t *node, const uint16_t *offsets, unsigned n)
{
	for (unsigned k = 0; k <= n; k++)
		put_be16(node + NODE - 2 * (k + 1), offsets[k]);
}

static void descriptor(uint8_t *node, uint32_t next, uint32_t prev, uint8_t kind, uint8_t height, unsigned records)
{
	put_be32(node + 0, next);
	put_be32(node + 4, prev);
	node[8] = kind;
	node[9] = height;
	put_be16(node + 10, (uint16_t)records);
}

static uint32_t map_nodes_for(uint32_t nodes)
{
	return nodes > HEADER_MAP_NODES ? (nodes - HEADER_MAP_NODES + MAP_NODE_NODES - 1) / MAP_NODE_NODES : 0;
}

/*
 * The header node (node 0) of a B-tree file of `nodes` nodes, nodes 0 to
 * used - 1 in use: those past what its map record covers are map nodes,
 * which follow the others.
 */
static void header_node(uint8_t *file, unsigned depth, uint32_t root, uint32_t leaf_records, uint32_t first_leaf,
			uint32_t last_leaf, unsigned max_key, uint32_t nodes, uint32_t used)
{
	uint32_t map_nodes = map_nodes_for(nodes);
	descriptor(file, map_nodes ? used - map_nodes : 0, 0, NODE_HEADER, 0, 3);    /* records: header, reserved, map */
	uint8_t *h = file + 14;
	put_be16(h + 0, (uint16_t)depth);
	put_be32(h + 2, root);
	put_be32(h + 6, leaf_records);
	put_be32(h + 10, first_leaf);
	put_be32(h + 14, last_leaf);
	put_be16(h + 18, NODE);
	put_be16(h + 20, (uint16_t)max_key);
	put_be32(h + 22, nodes);
	put_be32(h + 26, nodes - used);
	static const uint16_t offsets[4] = { 14, 120, 248, NODE - 8 };
	node_offsets(file, offsets, 3);

	for (uint32_t k = 0; k < map_nodes; k++) {
		uint8_t *m = file + (size_t)(used - map_nodes + k) * NODE;
		static const uint16_t map_offsets[2] = { 14, NODE - 6 };
		descriptor(m, k + 1 < map_nodes ? used - map_nodes + k + 1 : 0, 0, NODE_MAP, 0, 1);
		node_offsets(m, map_offsets, 1);
	}
	for (uint32_t k = 0; k < used; k++) {
		uint8_t *map = file + 248;
		uint32_t bit = k;
		if (k >= HEADER_MAP_NODES) {
			bit = (k - HEADER_MAP_NODES) % MAP_NODE_NODES;
			map = file + (size_t)(used - map_nodes + (k - HEADER_MAP_NODES) / MAP_NODE_NODES) * NODE + 14;
		}
		map[bit / 8] |= (uint8_t)(0x80u >> (bit % 8));
	}
}

/* ---- the catalog --------------------------------------------------------- */

/* A catalog record to be: its key (parent, name), what it is and its entry */
struct record {
	uint32_t parent;
	const uint8_t *name;            /* Pascal string */
	unsigned entry;
	uint8_t kind;
	uint8_t len;                    /* key and data */
};

static const uint8_t no_name[1];

static size_t key_size(unsigned name_len)
{
	return (7 + name_len + 1) & ~(size_t)1;
}

static int id_order(const void *x, const void *y)
{
	uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
	return a < b ? -1 : a > b;
}

static int record_order(const void *x, const void *y)
{
	const struct record *a = x, *b = y;
	if (a->parent != b->parent)
		return a->parent < b->parent ? -1 : 1;
	return hfs_compare_names(a->name + 1, a->name[0], b->name + 1, b->name[0]);
}

static void add_record(struct record *r, unsigned *n, uint32_t parent, const uint8_t *name, unsigned entry,
		       uint8_t kind)
{
	size_t size = kind == CAT_FOLDER ? FOLDER_SIZE : kind == CAT_FILE ? FILE_SIZE : THREAD_SIZE;
	r[*n] = (struct record){ parent, name, entry, kind, (uint8_t)(key_size(name[0]) + size) };
	(*n)++;
}

static uint32_t blocks_for(uint64_t bytes, uint32_t block_size)
{
	return (uint32_t)((bytes + block_size - 1) / block_size);
}

static void put_record(uint8_t *p, const struct record *r, const struct hfs_entry *e, const unsigned *valence,
		       uint32_t block_size)
{
	const struct hfs_entry *x = &e[r->entry];
	p[0] = (uint8_t)(6 + r->name[0]);
	put_be32(p + 2, r->parent);
	memcpy(p + 6, r->name, 1u + r->name[0]);
	uint8_t *d = p + key_size(r->name[0]);
	d[0] = r->kind;
	switch (r->kind) {
	case CAT_FOLDER:
		put_be16(d + 4, (uint16_t)valence[r->entry]);
		put_be32(d + 6, x->id);
		put_be32(d + 10, x->created);
		put_be32(d + 14, x->modified);
		memcpy(d + 22, x->finder, 32);
		break;
	case CAT_FILE:
		memcpy(d + 4, x->finder, 16);
		put_be32(d + 20, x->id);
		put_be32(d + 26, x->data_len);
		put_be32(d + 30, blocks_for(x->data_len, block_size) * block_size);
		put_be32(d + 36, x->rsrc_len);
		put_be32(d + 40, blocks_for(x->rsrc_len, block_size) * block_size);
		put_be32(d + 44, x->created);
		put_be32(d + 48, x->modified);
		memcpy(d + 56, x->finder + 16, 16);
		put_be16(d + 74, (uint16_t)x->data_start);
		put_be16(d + 76, (uint16_t)blocks_for(x->data_len, block_size));
		put_be16(d + 86, (uint16_t)x->rsrc_start);
		put_be16(d + 88, (uint16_t)blocks_for(x->rsrc_len, block_size));
		break;
	case CAT_THREAD:
		put_be32(d + 10, r->entry ? e[x->parent].id : ROOT_PAR_ID);
		memcpy(d + 14, x->name, 1u + x->name[0]);
		break;
	}
}

/* Whether record k fits in a leaf that holds `count` records up to `at` */
static bool fits(const struct record *r, size_t at, unsigned count)
{
	return at + r->len + 2 * (count + 2) <= NODE;
}

/* The catalog's shape: leaves, then index levels up to the root */
struct shape {
	unsigned leaves, *first;        /* each leaf's first record */
	unsigned levels;                /* index levels */
	unsigned count[32];             /* nodes at each level (0: the leaves) */
	unsigned *child[32];            /* each node's first node of the level below (level > 0) */
	unsigned nodes;                 /* leaves and index nodes */
};

static void shape_free(struct shape *s)
{
	free(s->first);
	for (unsigned k = 1; k <= s->levels; k++)
		free(s->child[k]);
}

static int catalog_shape(struct shape *s, const struct record *r, unsigned n)
{
	memset(s, 0, sizeof *s);
	if (!(s->first = malloc((n + 1) * sizeof *s->first)))
		return -1;
	for (unsigned k = 0; k < n;) {
		size_t at = 14;
		unsigned count = 0;
		s->first[s->leaves++] = k;
		while (k < n && fits(&r[k], at, count)) {
			at += r[k++].len;
			count++;
		}
		if (!count)
			return -1;
	}
	s->first[s->leaves] = n;
	s->count[0] = s->nodes = s->leaves;
	while (s->count[s->levels] > 1) {
		unsigned below = s->count[s->levels], level = ++s->levels;
		unsigned nodes = (below + INDEX_FANOUT - 1) / INDEX_FANOUT;
		if (level >= 32 || !(s->child[level] = malloc((nodes + 1) * sizeof(unsigned))))
			return -1;
		for (unsigned j = 0; j <= nodes; j++)          /* the children, spread evenly */
			s->child[level][j] = (unsigned)((uint64_t)below * j / nodes);
		s->count[level] = nodes;
		s->nodes += nodes;
	}
	return 0;
}

/* The first node of a level (from 1, after the header) */
static unsigned level_start(const struct shape *s, unsigned level)
{
	unsigned at = 1;
	for (unsigned k = 0; k < level; k++)
		at += s->count[k];
	return at;
}

/* The record whose key a node of a level starts with */
static unsigned first_record(const struct shape *s, unsigned level, unsigned node)
{
	for (; level > 0; level--)
		node = s->child[level][node];
	return s->first[node];
}

/* Writes the catalog's leaves and index nodes, from node 1 of `file` */
static void write_catalog(uint8_t *file, const struct shape *s, const struct record *r, const struct hfs_entry *e,
			  const unsigned *valence, uint32_t block_size)
{
	uint16_t offsets[NODE / 2];
	for (unsigned l = 0; l < s->leaves; l++) {
		uint8_t *node = file + (size_t)(1 + l) * NODE;
		size_t at = 14;
		unsigned count = 0;
		for (unsigned k = s->first[l]; k < s->first[l + 1]; k++, count++) {
			offsets[count] = (uint16_t)at;
			put_record(node + at, &r[k], e, valence, block_size);
			at += r[k].len;
		}
		offsets[count] = (uint16_t)at;
		descriptor(node, l + 1 < s->leaves ? l + 2 : 0, l, NODE_LEAF, 1, count);
		node_offsets(node, offsets, count);
	}
	for (unsigned level = 1; level <= s->levels; level++) {
		unsigned start = level_start(s, level), below = level_start(s, level - 1);
		for (unsigned j = 0; j < s->count[level]; j++) {
			uint8_t *node = file + (size_t)(start + j) * NODE;
			size_t at = 14;
			unsigned count = 0;
			for (unsigned c = s->child[level][j]; c < s->child[level][j + 1]; c++, count++) {
				const struct record *key = &r[first_record(s, level - 1, c)];
				offsets[count] = (uint16_t)at;
				node[at] = CATALOG_KEY_MAX;     /* index keys are all as long as the longest */
				put_be32(node + at + 2, key->parent);
				memcpy(node + at + 6, key->name, 1u + key->name[0]);
				put_be32(node + at + 1 + CATALOG_KEY_MAX, below + c);
				at += INDEX_RECORD;
			}
			offsets[count] = (uint16_t)at;
			descriptor(node, j + 1 < s->count[level] ? start + j + 1 : 0, j ? start + j - 1 : 0, NODE_INDEX,
				   (uint8_t)(level + 1), count);
			node_offsets(node, offsets, count);
		}
	}
}

/* ---- volumes --------------------------------------------------------------- */

static uint32_t clamp_u32(uint64_t v, uint32_t max)
{
	return v > max ? max : (uint32_t)v;
}

/* Allocation blocks the forks take */
static uint64_t fork_blocks(const struct hfs_entry *e, unsigned n, uint32_t block_size)
{
	uint64_t blocks = 0;
	for (unsigned k = 1; k < n; k++)
		if (!e[k].folder)
			blocks += blocks_for(e[k].data_len, block_size) + blocks_for(e[k].rsrc_len, block_size);
	return blocks;
}

/* Allocation blocks a volume needs, with `spb` sectors per block */
static uint64_t blocks_needed(const struct hfs_entry *e, unsigned n, uint32_t spb, uint32_t ext_nodes,
			      uint32_t cat_nodes, uint64_t free_bytes)
{
	uint32_t bs = spb * SECTOR;
	return blocks_for((uint64_t)ext_nodes * NODE, bs) + blocks_for((uint64_t)cat_nodes * NODE, bs) +
	       fork_blocks(e, n, bs) + (free_bytes + bs - 1) / bs;
}

void hfs_layout_free(struct hfs_layout *l)
{
	free(l->meta);
	l->meta = NULL;
}

int hfs_build(struct hfs_layout *l, struct hfs_entry *e, unsigned n, uint64_t size, uint64_t free_bytes, uint32_t now)
{
	memset(l, 0, sizeof *l);
	if (!n || !e[0].folder || !e[0].name[0] || e[0].name[0] > HFS_MAX_NAME || memchr(e[0].name + 1, ':', e[0].name[0]))
		return -1;

	/* entries: IDs, folders' valences, counts */
	unsigned *valence = calloc(n, sizeof *valence);
	struct record *r = malloc(((size_t)n * 2) * sizeof *r);
	uint32_t *ids = NULL;
	struct shape s = { 0 };
	int rc = -1;
	if (!valence || !r)
		goto out;
	unsigned files = 0, folders = 0, root_files = 0, root_folders = 0, nrec = 0;
	uint32_t next_id = HFS_FIRST_ID;
	for (unsigned k = 1; k < n; k++) {
		if (e[k].id && (e[k].id < HFS_FIRST_ID || e[k].id == UINT32_MAX))
			goto out;
		if (e[k].id >= next_id)
			next_id = e[k].id + 1;
	}
	for (unsigned k = 1; k < n; k++)
		if (!e[k].id) {
			if (next_id == UINT32_MAX)
				goto out;
			e[k].id = next_id++;
		}
	if (!(ids = malloc(n * sizeof *ids)))
		goto out;
	for (unsigned k = 0; k < n; k++)
		ids[k] = k ? e[k].id : ROOT_DIR_ID;
	qsort(ids, n, sizeof *ids, id_order);
	for (unsigned k = 1; k < n; k++)
		if (ids[k] == ids[k - 1])
			goto out;
	e[0].id = ROOT_DIR_ID;
	add_record(r, &nrec, ROOT_PAR_ID, e[0].name, 0, CAT_FOLDER);
	add_record(r, &nrec, ROOT_DIR_ID, no_name, 0, CAT_THREAD);
	for (unsigned k = 1; k < n; k++) {
		struct hfs_entry *x = &e[k];
		if (!x->name[0] || x->name[0] > HFS_MAX_FILE_NAME || memchr(x->name + 1, ':', x->name[0]) ||
		    x->parent >= k || !e[x->parent].folder || ++valence[x->parent] > 0xFFFF ||
		    x->data_len > HFS_MAX_FORK || x->rsrc_len > HFS_MAX_FORK)
			goto out;
		add_record(r, &nrec, e[x->parent].id, x->name, k, x->folder ? CAT_FOLDER : CAT_FILE);
		if (x->folder) {
			add_record(r, &nrec, x->id, no_name, k, CAT_THREAD);
			folders++;
			root_folders += x->parent == 0;
		} else {
			files++;
			root_files += x->parent == 0;
		}
	}
	qsort(r, nrec, sizeof *r, record_order);
	for (unsigned k = 1; k < nrec; k++)
		if (!record_order(&r[k - 1], &r[k]))
			goto out;               /* two of the same name in a folder */
	if (catalog_shape(&s, r, nrec))
		goto out;
	uint32_t cat_used = 1 + s.nodes;

	/*
	 * Sizes. A volume of a given size gets the layout Disk Initialization
	 * would give it (B-tree files of a size for the volume's); otherwise
	 * room is left in the catalog for as many records again.
	 */
	uint32_t spb, nblocks, bitmap_sectors, ext_alloc, cat_alloc;
	uint64_t total;
	if (size) {
		total = size / SECTOR;
		if (total < 800)
			goto out;
		spb = (uint32_t)((total - 5 + MAX_ALLOC_BLOCKS - 1) / MAX_ALLOC_BLOCKS);
		uint32_t max_blocks = clamp_u32((total - 5) / spb, MAX_ALLOC_BLOCKS);
		bitmap_sectors = (max_blocks + 4095) / 4096;
		nblocks = clamp_u32((total - 3 - bitmap_sectors - 2) / spb, MAX_ALLOC_BLOCKS);
		uint32_t node_limit = HEADER_MAP_NODES / spb ? HEADER_MAP_NODES / spb : 1;
		uint32_t cat_target = clamp_u32(total / 256, HEADER_MAP_NODES);
		if (cat_target < 32)
			cat_target = 32;
		ext_alloc = (8 + spb - 1) / spb;
		cat_alloc = (cat_target + spb - 1) / spb;
		if (cat_alloc > node_limit)
			cat_alloc = node_limit;
		if (ext_alloc > node_limit)
			ext_alloc = node_limit;
		while (cat_used + map_nodes_for(cat_alloc * spb) > cat_alloc * spb)
			cat_alloc++;
	} else {
		uint32_t ext_want = 64, cat_want = 2 * cat_used + 256;
		uint32_t lo = 1, hi = MAX_SECTORS_PER_BLOCK;
		if (blocks_needed(e, n, hi, ext_want, cat_want, free_bytes) > MAX_ALLOC_BLOCKS)
			goto out;               /* too many files for 65535 allocation blocks */
		while (lo < hi) {
			uint32_t mid = lo + (hi - lo) / 2;
			if (blocks_needed(e, n, mid, ext_want, cat_want, free_bytes) <= MAX_ALLOC_BLOCKS)
				hi = mid;
			else
				lo = mid + 1;
		}
		for (spb = lo;; spb++) {                /* (map nodes may want a block more) */
			uint32_t bs = spb * SECTOR;
			uint64_t free_blocks = (free_bytes + bs - 1) / bs, want;
			ext_alloc = blocks_for((uint64_t)ext_want * NODE, bs);
			cat_alloc = blocks_for((uint64_t)cat_want * NODE, bs);
			while (cat_used + map_nodes_for(cat_alloc * spb) > cat_alloc * spb)
				cat_alloc++;
			want = ext_alloc + cat_alloc + fork_blocks(e, n, bs) + (free_blocks ? free_blocks : 1);
			if (want <= MAX_ALLOC_BLOCKS) {
				nblocks = (uint32_t)want;
				break;
			}
			if (spb == MAX_SECTORS_PER_BLOCK)
				goto out;
		}
		bitmap_sectors = (nblocks + 4095) / 4096;
		total = 3 + bitmap_sectors + (uint64_t)nblocks * spb + 2;
	}
	uint32_t bs = spb * SECTOR, al_bl_st = 3 + bitmap_sectors;
	uint32_t ext_nodes = clamp_u32((uint64_t)ext_alloc * spb, HEADER_MAP_NODES);
	uint32_t cat_nodes = cat_alloc * spb, cat_map = map_nodes_for(cat_nodes);
	cat_used += cat_map;

	/* the forks, one extent each, after the B-trees */
	uint32_t next = ext_alloc + cat_alloc;
	if (next >= nblocks)
		goto out;
	for (unsigned k = 1; k < n; k++) {
		struct hfs_entry *x = &e[k];
		x->data_start = x->rsrc_start = 0;
		if (x->folder)
			continue;
		uint32_t data = blocks_for(x->data_len, bs), rsrc = blocks_for(x->rsrc_len, bs);
		if (data > nblocks - next || rsrc > nblocks - next - data)
			goto out;
		x->data_start = data ? next : 0;
		next += data;
		x->rsrc_start = rsrc ? next : 0;
		next += rsrc;
	}

	l->size = total * SECTOR;
	l->block_size = bs;
	l->first_block = (uint64_t)al_bl_st * SECTOR;
	l->blocks = nblocks;
	l->free_blocks = nblocks - next;
	l->meta_len = (size_t)(l->first_block + (uint64_t)(ext_alloc + cat_alloc) * bs);
	if (!(l->meta = calloc(1, l->meta_len)))
		goto out;

	for (uint32_t k = 0; k < next; k++)             /* the bitmap */
		l->meta[3 * SECTOR + k / 8] |= (uint8_t)(0x80u >> (k % 8));
	uint8_t *ext = l->meta + l->first_block, *cat = ext + (size_t)ext_alloc * bs;
	header_node(ext, 0, 0, 0, 0, 0, EXTENTS_KEY_MAX, ext_nodes, 1);
	header_node(cat, 1 + s.levels, s.nodes, nrec, 1, s.leaves, CATALOG_KEY_MAX, cat_nodes, cat_used);
	write_catalog(cat, &s, r, e, valence, bs);

	uint8_t *mdb = l->mdb;
	put_be16(mdb + MDB_SIG, 0x4244);                /* 'BD' */
	put_be32(mdb + MDB_CR_DATE, now);
	put_be32(mdb + MDB_LS_MOD, now);
	put_be16(mdb + MDB_ATRB, HFS_UNMOUNTED);
	put_be16(mdb + MDB_NM_FLS, (uint16_t)root_files);
	put_be16(mdb + MDB_VBM_ST, 3);
	put_be16(mdb + MDB_ALLOC_PTR, (uint16_t)next);
	put_be16(mdb + MDB_NM_AL_BLKS, (uint16_t)nblocks);
	put_be32(mdb + MDB_AL_BLK_SIZ, bs);
	put_be32(mdb + MDB_CLP_SIZ, 4 * bs);
	put_be16(mdb + MDB_AL_BL_ST, (uint16_t)al_bl_st);
	put_be32(mdb + MDB_NXT_CNID, next_id);
	put_be16(mdb + MDB_FREE_BKS, (uint16_t)(nblocks - next));
	memcpy(mdb + MDB_VN, e[0].name, 1u + e[0].name[0]);
	put_be32(mdb + MDB_XT_CLP_SIZ, ext_alloc * bs);
	put_be32(mdb + MDB_CT_CLP_SIZ, cat_alloc * bs);
	put_be16(mdb + MDB_NM_RT_DIRS, (uint16_t)root_folders);
	put_be32(mdb + MDB_FIL_CNT, files);
	put_be32(mdb + MDB_DIR_CNT, folders);
	put_be32(mdb + MDB_XT_FL_SIZ, ext_nodes * NODE);
	put_be16(mdb + MDB_XT_EXT_REC + 2, (uint16_t)ext_alloc);       /* starts at block 0 */
	put_be32(mdb + MDB_CT_FL_SIZ, cat_nodes * NODE);
	put_be16(mdb + MDB_CT_EXT_REC + 0, (uint16_t)ext_alloc);
	put_be16(mdb + MDB_CT_EXT_REC + 2, (uint16_t)cat_alloc);
	memcpy(l->meta + 2 * SECTOR, mdb, SECTOR);
	rc = 0;
out:
	shape_free(&s);
	free(valence);
	free(r);
	free(ids);
	if (rc)
		hfs_layout_free(l);
	return rc;
}

static int write_at(int fd, const void *p, size_t n, uint64_t at)
{
	return pwrite(fd, p, n, (off_t)at) == (ssize_t)n ? 0 : -1;
}

int hfs_format(int fd, uint64_t size, const char *name, uint32_t now, const struct hfs_file *files,
	       unsigned nfiles)
{
	size_t name_len = strlen(name);
	struct hfs_entry *e = calloc(nfiles + 1, sizeof *e);
	struct hfs_layout l;
	int err = -1;
	if (!e || !name_len || name_len > HFS_MAX_NAME || size < 800 * SECTOR)
		goto out;
	e[0] = (struct hfs_entry){ .folder = true, .created = now, .modified = now };
	e[0].name[0] = (uint8_t)name_len;
	memcpy(e[0].name + 1, name, name_len);
	for (unsigned k = 0; k < nfiles; k++) {
		const struct hfs_file *f = &files[k];
		struct hfs_entry *x = &e[1 + k];
		size_t n = strlen(f->name);
		if (!n || n > HFS_MAX_FILE_NAME)
			goto out;
		x->name[0] = (uint8_t)n;
		memcpy(x->name + 1, f->name, n);
		put_be32(x->finder + 0, f->type);
		put_be32(x->finder + 4, f->creator);
		put_be16(x->finder + 8, f->finder_flags);
		put_be16(x->finder + 10, (uint16_t)f->v);
		put_be16(x->finder + 12, (uint16_t)f->h);
		x->created = x->modified = now;
		x->data_len = f->data_len;
		x->rsrc_len = f->rsrc_len;
		if (!strcmp(f->name, HFS_VOLUME_ICON))
			put_be16(e[0].finder + 8, HAS_CUSTOM_ICON);     /* (the root folder's frFlags) */
	}
	if (hfs_build(&l, e, nfiles + 1, size, 0, now))
		goto out;
	err = write_at(fd, l.meta, l.meta_len, 0);
	for (unsigned k = 0; k < nfiles; k++) {
		const struct hfs_entry *x = &e[1 + k];
		if (x->data_len)
			err |= write_at(fd, files[k].data, x->data_len, l.first_block + (uint64_t)x->data_start * l.block_size);
		if (x->rsrc_len)
			err |= write_at(fd, files[k].rsrc, x->rsrc_len, l.first_block + (uint64_t)x->rsrc_start * l.block_size);
	}
	err |= write_at(fd, l.mdb, SECTOR, l.size - 2 * SECTOR);       /* the alternate MDB */
	hfs_layout_free(&l);
out:
	free(e);
	return err ? -1 : 0;
}
