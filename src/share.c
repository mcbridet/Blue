#include "share.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/attr.h>
#include <sys/xattr.h>
#endif

#include "be.h"
#include "hfs.h"
#include "hfsread.h"

#define SECTOR 512u
#define MIN_FREE (1ull << 30)
#define MAX_FREE ((2ull << 30) - (64ull << 20))         /* old installers count free space in 31 bits */
#define MAX_DEPTH 64u
#define MAX_ITEMS 0xFFFFu                               /* in a folder */
#define OPEN_FILES 8u
#define CHUNK (1u << 20)
#define MAX_NOTES 30u

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define FINDER_BOOKKEEPING 0x0301u      /* Finder flags of Mac OS's own: has been inited, reserved, on desk */
#define FINDER_INVISIBLE 0x4000u
#define AD_MAGIC 0x00051607u
#define AD_HEADER 50u                   /* an AppleDouble header with two entries */
#define AD_RSRC 2u
#define AD_FINDER 9u

/* A file or folder of the shared folder, as the volume was made from it */
struct item {
	char *path;                     /* in the folder ("" for the folder itself) */
	char *ad_path;                  /* its AppleDouble file, if it has one */
	uint64_t ad_rsrc;               /* ... and the resource fork's offset in it */
	bool ad_beside;                 /* ... which is "._name" beside it (not in a __MACOSX folder) */
	bool named_fork;                /* the resource fork is the file's own (macOS) */
	bool xattr_finder;              /* the Finder info is the file's com.apple.FinderInfo (macOS) */
	uint64_t ino, size;             /* how it was, to tell whether it changes meanwhile */
	int64_t mtime_ns;
};

/* A fork's data in the volume */
struct span {
	uint64_t at, len;
	unsigned item;
	bool rsrc;
};

struct share {
	char *dir;
	char volume[HFS_MAX_NAME * 3 + 1];      /* its name, in UTF-8 */
	struct item *items;
	struct hfs_entry *entries;
	unsigned n, cap;
	uint32_t next_id;
	uint32_t clock;                 /* the guest's clock at the start (Mac OS time): no date is later */
	unsigned skipped;
	struct hfs_layout layout;
	struct span *spans;
	unsigned nspans;
	struct {
		unsigned item;
		bool rsrc;
		int fd;
	} open[OPEN_FILES];
	unsigned next_open;
	int overlay;
	uint64_t *written;              /* a bit per sector written to the overlay */
	uint64_t written_sectors;
	bool written_back;              /* what Mac OS changed is in the folder (until the volume is made again) */
	bool gone;                      /* there's no volume: it couldn't be made again */
	bool log;
	struct disk_backend disk;
};

static void skip(struct share *s, const char *path, const char *why)
{
	if (s->skipped++ < MAX_NOTES)
		printf("share: left out %s/%s: %s\n", s->dir, path, why);
}

static int host_path(const struct share *s, const char *rel, char *out, size_t len)
{
	int n = snprintf(out, len, "%s%s%s", s->dir, rel[0] ? "/" : "", rel);
	return n < 0 || (size_t)n >= len ? -1 : 0;
}

static int64_t mtime_ns(const struct stat *st)
{
#ifdef __APPLE__
	return (int64_t)st->st_mtimespec.tv_sec * 1000000000 + st->st_mtimespec.tv_nsec;
#else
	return (int64_t)st->st_mtim.tv_sec * 1000000000 + st->st_mtim.tv_nsec;
#endif
}

static int64_t birth_time(const struct stat *st)
{
#ifdef __APPLE__
	return st->st_birthtimespec.tv_sec;
#else
	return st->st_mtime;
#endif
}

static bool zeros(const uint8_t *p, size_t n)
{
	for (size_t k = 0; k < n; k++)
		if (p[k])
			return false;
	return true;
}

/* ---- types and creators ------------------------------------------------------ */

static const struct {
	const char *ext;
	uint32_t type, creator;
} extensions[] = {
	{ "sit", FOURCC('S', 'I', 'T', '!'), FOURCC('S', 'I', 'T', 'x') },
	{ "sea", FOURCC('S', 'I', 'T', '!'), FOURCC('S', 'I', 'T', 'x') },
	{ "hqx", FOURCC('T', 'E', 'X', 'T'), FOURCC('S', 'I', 'T', 'x') },
	{ "bin", FOURCC('B', 'I', 'N', 'A'), FOURCC('S', 'I', 'T', 'x') },
	{ "zip", FOURCC('Z', 'I', 'P', ' '), FOURCC('S', 'I', 'T', 'x') },
	{ "cpt", FOURCC('P', 'A', 'C', 'T'), FOURCC('S', 'I', 'T', 'x') },
	{ "gz", FOURCC('G', 'z', 'i', 'p'), FOURCC('S', 'I', 'T', 'x') },
	{ "tgz", FOURCC('G', 'z', 'i', 'p'), FOURCC('S', 'I', 'T', 'x') },
	{ "tar", FOURCC('T', 'A', 'R', 'F'), FOURCC('S', 'I', 'T', 'x') },
	{ "txt", FOURCC('T', 'E', 'X', 'T'), FOURCC('t', 't', 'x', 't') },
	{ "text", FOURCC('T', 'E', 'X', 'T'), FOURCC('t', 't', 'x', 't') },
	{ "html", FOURCC('T', 'E', 'X', 'T'), FOURCC('M', 'S', 'I', 'E') },
	{ "htm", FOURCC('T', 'E', 'X', 'T'), FOURCC('M', 'S', 'I', 'E') },
	{ "jpg", FOURCC('J', 'P', 'E', 'G'), FOURCC('o', 'g', 'l', 'e') },
	{ "jpeg", FOURCC('J', 'P', 'E', 'G'), FOURCC('o', 'g', 'l', 'e') },
	{ "gif", FOURCC('G', 'I', 'F', 'f'), FOURCC('o', 'g', 'l', 'e') },
	{ "png", FOURCC('P', 'N', 'G', 'f'), FOURCC('o', 'g', 'l', 'e') },
	{ "tif", FOURCC('T', 'I', 'F', 'F'), FOURCC('o', 'g', 'l', 'e') },
	{ "tiff", FOURCC('T', 'I', 'F', 'F'), FOURCC('o', 'g', 'l', 'e') },
	{ "bmp", FOURCC('B', 'M', 'P', 'f'), FOURCC('o', 'g', 'l', 'e') },
	{ "pict", FOURCC('P', 'I', 'C', 'T'), FOURCC('o', 'g', 'l', 'e') },
	{ "pct", FOURCC('P', 'I', 'C', 'T'), FOURCC('o', 'g', 'l', 'e') },
	{ "pdf", FOURCC('P', 'D', 'F', ' '), FOURCC('C', 'A', 'R', 'O') },
	{ "img", FOURCC('r', 'o', 'h', 'd'), FOURCC('d', 'C', 'p', 'y') },
	{ "dmg", FOURCC('r', 'o', 'h', 'd'), FOURCC('d', 'C', 'p', 'y') },
	{ "toast", FOURCC('r', 'o', 'h', 'd'), FOURCC('d', 'C', 'p', 'y') },
	{ "dsk", FOURCC('d', 'I', 'm', 'g'), FOURCC('d', 'C', 'p', 'y') },
	{ "image", FOURCC('d', 'I', 'm', 'g'), FOURCC('d', 'C', 'p', 'y') },
	{ "mov", FOURCC('M', 'o', 'o', 'V'), FOURCC('T', 'V', 'O', 'D') },
	{ "mpg", FOURCC('M', 'P', 'E', 'G'), FOURCC('T', 'V', 'O', 'D') },
	{ "mpeg", FOURCC('M', 'P', 'E', 'G'), FOURCC('T', 'V', 'O', 'D') },
	{ "avi", FOURCC('V', 'f', 'W', ' '), FOURCC('T', 'V', 'O', 'D') },
	{ "mp3", FOURCC('M', 'P', 'G', '3'), FOURCC('T', 'V', 'O', 'D') },
	{ "aif", FOURCC('A', 'I', 'F', 'F'), FOURCC('T', 'V', 'O', 'D') },
	{ "aiff", FOURCC('A', 'I', 'F', 'F'), FOURCC('T', 'V', 'O', 'D') },
	{ "aifc", FOURCC('A', 'I', 'F', 'C'), FOURCC('T', 'V', 'O', 'D') },
	{ "wav", FOURCC('W', 'A', 'V', 'E'), FOURCC('T', 'V', 'O', 'D') },
	{ "au", FOURCC('U', 'L', 'A', 'W'), FOURCC('T', 'V', 'O', 'D') },
	{ "mid", FOURCC('M', 'i', 'd', 'i'), FOURCC('T', 'V', 'O', 'D') },
	{ "midi", FOURCC('M', 'i', 'd', 'i'), FOURCC('T', 'V', 'O', 'D') },
};

/*
 * A file's type and creator from its name and its first bytes, for files
 * without Finder info: by extension, else plain text (no control
 * characters but tabs, line and form feeds) for SimpleText, else unknown.
 */
static void guess_type(const char *name, const uint8_t *head, size_t n, uint32_t *type, uint32_t *creator)
{
	const char *dot = strrchr(name, '.');
	if (dot && dot != name)
		for (size_t k = 0; k < sizeof extensions / sizeof extensions[0]; k++)
			if (!strcasecmp(dot + 1, extensions[k].ext)) {
				*type = extensions[k].type;
				*creator = extensions[k].creator;
				return;
			}
	bool text = true;
	for (size_t k = 0; k < n && text; k++)
		text = head[k] >= 0x20 || head[k] == '\t' || head[k] == '\n' || head[k] == '\r' || head[k] == '\f';
	*type = text ? FOURCC('T', 'E', 'X', 'T') : FOURCC('?', '?', '?', '?');
	*creator = text ? FOURCC('t', 't', 'x', 't') : FOURCC('?', '?', '?', '?');
}

/* ---- AppleDouble files ------------------------------------------------------- */

/* An AppleDouble file's Finder info (if it has some) and resource fork. Returns 0 if it is one. */
static int read_appledouble(const char *path, uint8_t finder[32], bool *has_finder, uint64_t *rsrc_at,
			    uint64_t *rsrc_len)
{
	uint8_t h[26 + 12 * 16];
	struct stat st;
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = pread(fd, h, sizeof h, 0);
	int rc = -1;
	if (n < 26 || fstat(fd, &st) || be32(h) != AD_MAGIC || (be32(h + 4) != 0x00020000u && be32(h + 4) != 0x00010000u))
		goto out;
	*has_finder = false;
	*rsrc_at = *rsrc_len = 0;
	for (unsigned k = 0; k < be16(h + 24) && 26 + 12 * (k + 1) <= (size_t)n; k++) {
		const uint8_t *e = h + 26 + 12 * k;
		uint64_t at = be32(e + 4), len = be32(e + 8);
		if (at > (uint64_t)st.st_size || len > (uint64_t)st.st_size - at)
			continue;
		if (be32(e) == AD_FINDER && len >= 16) {
			memset(finder, 0, 32);
			*has_finder = pread(fd, finder, len < 32 ? (size_t)len : 32, (off_t)at) >= 16 && !zeros(finder, 32);
		} else if (be32(e) == AD_RSRC && len <= HFS_MAX_FORK) {
			*rsrc_at = at;
			*rsrc_len = len;
		}
	}
	rc = 0;
out:
	close(fd);
	return rc;
}

/*
 * Where a file's AppleDouble file is: "._name" beside it, else in a
 * __MACOSX folder in its folder or one above (as zip files have them).
 */
static bool find_appledouble(const struct share *s, const char *rel, char *ad, size_t len, bool *beside)
{
	const char *slash = strrchr(rel, '/'), *name = slash ? slash + 1 : rel;
	size_t dir = slash ? (size_t)(slash - rel) : 0;
	char path[PATH_MAX];
	struct stat st;
	for (size_t cut = dir + 2; cut-- > 0;) {
		int n;
		if (cut == dir + 1) {           /* (first: beside it) */
			n = snprintf(ad, len, "%.*s%s._%s", (int)dir, rel, slash ? "/" : "", name);
		} else {
			if (cut && cut != dir && rel[cut] != '/')
				continue;
			size_t rest = cut ? cut + 1 : 0, rest_len = rest < dir ? dir - rest : 0;
			n = snprintf(ad, len, "%.*s%s__MACOSX/%.*s%s._%s", (int)cut, rel, cut ? "/" : "", (int)rest_len,
				     rel + rest, rest_len ? "/" : "", name);
		}
		if (n > 0 && (size_t)n < len && !host_path(s, ad, path, sizeof path) && !stat(path, &st) &&
		    S_ISREG(st.st_mode)) {
			*beside = cut == dir + 1;
			return true;
		}
	}
	return false;
}

/* ---- the volume -------------------------------------------------------------- */

static int add_item(struct share *s)
{
	if (s->n == s->cap) {
		unsigned cap = s->cap ? 2 * s->cap : 256;
		struct item *items = realloc(s->items, cap * sizeof *items);
		if (items)
			s->items = items;
		struct hfs_entry *entries = realloc(s->entries, cap * sizeof *entries);
		if (entries)
			s->entries = entries;
		if (!items || !entries)
			return -1;
		s->cap = cap;
	}
	memset(&s->items[s->n], 0, sizeof s->items[0]);
	memset(&s->entries[s->n], 0, sizeof s->entries[0]);
	return (int)s->n++;
}

/*
 * An item's dates: the host's, but none later than the guest's clock
 * (Norton Utilities takes a date in the future for damage: in the MDB,
 * "damage to the Master Directory Block"), and created no later than
 * modified (a file copied or unpacked on the host is born anew but keeps
 * its modification date; Norton Disk Doctor calls that a bad date too).
 */
static void host_dates(const struct share *s, struct hfs_entry *e, const struct stat *st)
{
	uint32_t created = hfs_mac_time(birth_time(st)), modified = hfs_mac_time(st->st_mtime);
	e->modified = modified < s->clock ? modified : s->clock;
	e->created = created < e->modified ? created : e->modified;
}

/* Finder info, resource fork and dates from the host */
static void metadata(struct share *s, unsigned k, const struct stat *st)
{
	struct item *it = &s->items[k];
	struct hfs_entry *e = &s->entries[k];
	char path[PATH_MAX], ad[PATH_MAX];
	if (host_path(s, it->path, path, sizeof path))
		return;
	host_dates(s, e, st);
	it->ino = st->st_ino;
	it->size = (uint64_t)st->st_size;
	it->mtime_ns = mtime_ns(st);
	if (!e->folder)
		e->data_len = (uint32_t)st->st_size;
	bool finder = false;
#ifdef __APPLE__
	finder = it->xattr_finder = getxattr(path, XATTR_FINDERINFO_NAME, e->finder, 32, 0, 0) == 32 && !zeros(e->finder, 32);
	if (!finder)
		memset(e->finder, 0, 32);
	ssize_t rsrc = e->folder ? 0 : getxattr(path, XATTR_RESOURCEFORK_NAME, NULL, 0, 0, 0);
	if (rsrc > 0 && rsrc <= (ssize_t)HFS_MAX_FORK) {
		e->rsrc_len = (uint32_t)rsrc;
		it->named_fork = true;
	}
#endif
	bool beside;
	if ((!finder || (!e->folder && !it->named_fork)) && find_appledouble(s, it->path, ad, sizeof ad, &beside)) {
		uint8_t info[32];
		bool has_finder;
		uint64_t rsrc_at, rsrc_len;
		char ad_path[PATH_MAX];
		if (!host_path(s, ad, ad_path, sizeof ad_path) &&
		    !read_appledouble(ad_path, info, &has_finder, &rsrc_at, &rsrc_len)) {
			it->ad_path = strdup(ad);
			it->ad_beside = beside;
			if (has_finder && !finder) {
				memcpy(e->finder, info, 32);
				finder = true;
			}
			if (!e->folder && !it->named_fork && rsrc_len) {
				e->rsrc_len = (uint32_t)rsrc_len;
				it->ad_rsrc = rsrc_at;
			}
		}
	}
	if (!finder && !e->folder) {
		uint8_t head[512];
		ssize_t n = 0;
		uint32_t type, creator;
		int fd = open(path, O_RDONLY);
		if (fd >= 0) {
			n = pread(fd, head, sizeof head, 0);
			close(fd);
		}
		const char *name = strrchr(it->path, '/');
		guess_type(name ? name + 1 : it->path, head, n > 0 ? (size_t)n : 0, &type, &creator);
		put_be32(e->finder, type);
		put_be32(e->finder + 4, creator);
	}
}

/* Shortens a name to 31 characters, keeping its extension: "A long name#1A2B.txt", as Mac OS 9 does */
static size_t mangle(uint8_t *name, size_t len, uint32_t id)
{
	char tag[12];
	size_t t = (size_t)snprintf(tag, sizeof tag, "#%X", id), ext = 0;
	for (size_t k = len; k > 1 && len - k < 5; k--)
		if (name[k - 1] == '.') {
			ext = len - k + 1;
			break;
		}
	size_t stem = len - ext;
	if (stem + t + ext > HFS_MAX_FILE_NAME)
		stem = HFS_MAX_FILE_NAME - t - ext;
	memmove(name + stem + t, name + len - ext, ext);
	memcpy(name + stem, tag, t);
	return stem + t + ext;
}

struct child {
	char *name;
	struct stat st;
	uint8_t mac[256];
	size_t mac_len;
	uint32_t id;
};

static int child_by_name(const void *x, const void *y)
{
	return strcmp(((const struct child *)x)->name, ((const struct child *)y)->name);
}

static int child_by_mac_name(const void *x, const void *y)
{
	const struct child *a = x, *b = y;
	int c = hfs_compare_names(a->mac, a->mac_len, b->mac, b->mac_len);
	return c ? c : a->id < b->id ? -1 : a->id > b->id;
}

/* A host name in Mac OS Roman, with ':' as '/' (as macOS shows them) */
static size_t mac_name(const char *name, uint8_t *out, size_t max)
{
	char swapped[NAME_MAX * 4 + 1];
	snprintf(swapped, sizeof swapped, "%s", name);
	for (char *p = swapped; *p; p++)
		if (*p == ':')
			*p = '/';
	size_t n = hfs_from_utf8(swapped, out, max);
	return n < max ? n : max;
}

/*
 * Whether a folder's entry goes on the volume (`st` filled in): 1; 0 if
 * it's left out without a word (dot files, AppleDouble folders, other
 * kinds of files); -1 if it's left out, `why` saying why.
 */
static int eligible(const char *dir_path, const char *name, unsigned depth, struct stat *st, const char **why)
{
	char path[PATH_MAX];
	*why = NULL;
	if (name[0] == '.' || !strcmp(name, "__MACOSX"))
		return 0;
	if (snprintf(path, sizeof path, "%s/%s", dir_path, name) >= (int)sizeof path || lstat(path, st))
		return 0;
	if (S_ISLNK(st->st_mode))
		*why = stat(path, st) ? "a symbolic link to nothing" : S_ISDIR(st->st_mode) ? "a symbolic link to a folder" :
										 NULL;
	if (!*why && !S_ISDIR(st->st_mode) && !S_ISREG(st->st_mode))
		return 0;
	if (!*why && S_ISREG(st->st_mode) && (uint64_t)st->st_size > HFS_MAX_FORK)
		*why = "over 2 GB, too big for HFS";
	if (!*why && access(path, S_ISDIR(st->st_mode) ? R_OK | X_OK : R_OK))
		*why = "can't be read";
	if (!*why && S_ISDIR(st->st_mode) && depth >= MAX_DEPTH)
		*why = "folders nested too deep";
	return *why ? -1 : 1;
}

/* Adds what's in folder k: its files and folders, in the catalog's order */
static void scan_folder(struct share *s, unsigned k, unsigned depth)
{
	char dir_path[PATH_MAX], path[PATH_MAX];
	if (host_path(s, s->items[k].path, dir_path, sizeof dir_path))
		return;
	DIR *dir = opendir(dir_path);
	if (!dir) {
		skip(s, s->items[k].path, strerror(errno));
		return;
	}
	struct child *c = NULL;
	size_t n = 0, cap = 0;
	struct dirent *de;
	while ((de = readdir(dir))) {
		const char *name = de->d_name, *why;
		struct stat st;
		int ok = eligible(dir_path, name, depth, &st, &why);
		if (!ok)
			continue;
		if (ok > 0 && n >= MAX_ITEMS)
			why = "more than 65535 items in the folder";
		if (why) {
			char rel[PATH_MAX];
			snprintf(rel, sizeof rel, "%s%s%s", s->items[k].path, s->items[k].path[0] ? "/" : "", name);
			skip(s, rel, why);
			continue;
		}
		if (n == cap) {
			struct child *more = realloc(c, (cap = cap ? 2 * cap : 64) * sizeof *c);
			if (!more)
				break;
			c = more;
		}
		c[n] = (struct child){ .name = strdup(name), .st = st };
		if (c[n].name)
			n++;
	}
	closedir(dir);

	/* Mac OS names: shortened (with the item's ID) if too long, or if two would be the same */
	qsort(c, n, sizeof *c, child_by_name);
	for (size_t j = 0; j < n; j++) {
		c[j].id = s->next_id++;
		c[j].mac_len = mac_name(c[j].name, c[j].mac, sizeof c[j].mac);
		if (c[j].mac_len > HFS_MAX_FILE_NAME || !c[j].mac_len)
			c[j].mac_len = c[j].mac_len ? mangle(c[j].mac, c[j].mac_len, c[j].id) : 1;
		if (c[j].mac[0] == 0)
			c[j].mac[0] = '_';
	}
	for (unsigned pass = 0;; pass++) {
		bool clash = false;
		qsort(c, n, sizeof *c, child_by_mac_name);
		for (size_t j = 1; j < n; j++)
			if (!hfs_compare_names(c[j].mac, c[j].mac_len, c[j - 1].mac, c[j - 1].mac_len)) {
				clash = true;
				if (pass < 2)
					c[j].mac_len = mangle(c[j].mac, c[j].mac_len, c[j].id + pass * 0x1000000u);
			}
		if (!clash || pass >= 2)
			break;
	}

	for (size_t j = 0; j < n; j++) {
		const char *base = s->items[k].path;
		int m;
		if (j && !hfs_compare_names(c[j].mac, c[j].mac_len, c[j - 1].mac, c[j - 1].mac_len)) {
			snprintf(path, sizeof path, "%s%s%s", base, base[0] ? "/" : "", c[j].name);
			skip(s, path, "its name is another's to Mac OS");
		} else if ((m = add_item(s)) >= 0) {
			struct item *it = &s->items[m];
			struct hfs_entry *e = &s->entries[m];
			size_t len = strlen(base) + strlen(c[j].name) + 2;
			if ((it->path = malloc(len)))
				snprintf(it->path, len, "%s%s%s", base, base[0] ? "/" : "", c[j].name);
			e->parent = k;
			e->folder = S_ISDIR(c[j].st.st_mode);
			e->id = c[j].id;
			e->name[0] = (uint8_t)c[j].mac_len;
			memcpy(e->name + 1, c[j].mac, c[j].mac_len);
			if (it->path)
				metadata(s, (unsigned)m, &c[j].st);
			else
				s->n--;
		}
		free(c[j].name);
	}
	free(c);
}

static int scan(struct share *s, const struct stat *st)
{
	int root = add_item(s);
	if (root < 0 || !(s->items[0].path = strdup("")))
		return -1;
	struct hfs_entry *e = &s->entries[0];
	const char *base = strrchr(s->dir, '/');
	size_t len = mac_name(base && base[1] ? base + 1 : "Shared Folder", e->name + 1, HFS_MAX_NAME);
	e->name[0] = (uint8_t)(len ? len : 1);
	if (!len)
		e->name[1] = '_';
	e->folder = true;
	host_dates(s, e, st);           /* (the volume's too: created when the folder was, on macOS) */
	s->items[0].ino = st->st_ino;
	s->items[0].mtime_ns = mtime_ns(st);
	hfs_to_utf8(e->name + 1, e->name[0], s->volume, sizeof s->volume);
	s->next_id = HFS_FIRST_ID;
	for (unsigned k = 0; k < s->n; k++)
		if (s->entries[k].folder) {
			unsigned depth = 0;
			for (const char *p = s->items[k].path; *p; p++)
				depth += *p == '/';
			scan_folder(s, k, depth + 1);
		}
	if (s->skipped > MAX_NOTES)
		printf("share: left out %u more\n", s->skipped - MAX_NOTES);
	return 0;
}

static int build(struct share *s)
{
	uint64_t content = 0;
	for (unsigned k = 0; k < s->n; k++)
		content += (uint64_t)s->entries[k].data_len + s->entries[k].rsrc_len;
	uint64_t free_bytes = content / 2 < MIN_FREE ? MIN_FREE : content / 2 > MAX_FREE ? MAX_FREE : content / 2;
	if (hfs_build(&s->layout, s->entries, s->n, 0, free_bytes, s->entries[0].created)) {
		fprintf(stderr, "share: %s has too many files for an HFS volume (%u)\n", s->dir, s->n);
		return -1;
	}
	if (!(s->spans = malloc(2 * (size_t)s->n * sizeof *s->spans)))
		return -1;
	for (unsigned k = 1; k < s->n; k++) {
		const struct hfs_entry *e = &s->entries[k];
		uint64_t bs = s->layout.block_size;
		if (e->data_len)
			s->spans[s->nspans++] = (struct span){ s->layout.first_block + e->data_start * bs, e->data_len, k, false };
		if (e->rsrc_len)
			s->spans[s->nspans++] = (struct span){ s->layout.first_block + e->rsrc_start * bs, e->rsrc_len, k, true };
	}
	return 0;
}

/* ---- blocks ------------------------------------------------------------------ */

static int source_fd(struct share *s, unsigned item, bool rsrc)
{
	for (unsigned k = 0; k < OPEN_FILES; k++)
		if (s->open[k].fd >= 0 && s->open[k].item == item && s->open[k].rsrc == rsrc)
			return s->open[k].fd;
	const struct item *it = &s->items[item];
	char path[PATH_MAX];
	if (host_path(s, rsrc && !it->named_fork ? it->ad_path : it->path, path, sizeof path))
		return -1;
	if (rsrc && it->named_fork && strlen(path) + sizeof "/..namedfork/rsrc" <= sizeof path)
		strcat(path, "/..namedfork/rsrc");
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	unsigned k = s->next_open++ % OPEN_FILES;
	if (s->open[k].fd >= 0)
		close(s->open[k].fd);
	s->open[k].item = item;
	s->open[k].rsrc = rsrc;
	s->open[k].fd = fd;
	return fd;
}

/* The last fork starting at or before `at`, or NULL */
static const struct span *span_at(const struct share *s, uint64_t at)
{
	unsigned lo = 0, hi = s->nspans;
	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;
		if (s->spans[mid].at <= at)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo ? &s->spans[lo - 1] : NULL;
}

/* The volume as it was made: metadata from memory, forks from the host's files, zeros elsewhere */
static void base_read(struct share *s, uint8_t *p, size_t len, uint64_t at)
{
	const struct hfs_layout *l = &s->layout;
	uint64_t alt = l->size - 2 * SECTOR;
	while (len) {
		size_t n = len;
		if (at < l->meta_len) {
			n = (size_t)(l->meta_len - at) < n ? (size_t)(l->meta_len - at) : n;
			memcpy(p, l->meta + at, n);
		} else if (at >= alt && at < alt + SECTOR) {
			n = (size_t)(alt + SECTOR - at) < n ? (size_t)(alt + SECTOR - at) : n;
			memcpy(p, l->mdb + (at - alt), n);
		} else {
			const struct span *sp = span_at(s, at);
			if (sp && at < sp->at + sp->len) {
				n = (size_t)(sp->at + sp->len - at) < n ? (size_t)(sp->at + sp->len - at) : n;
				const struct item *it = &s->items[sp->item];
				uint64_t from = at - sp->at + (sp->rsrc && !it->named_fork ? it->ad_rsrc : 0);
				int fd = source_fd(s, sp->item, sp->rsrc);
				ssize_t got = fd < 0 ? 0 : pread(fd, p, n, (off_t)from);
				if (got < (ssize_t)n)   /* (changed on the host meanwhile) */
					memset(p + (got > 0 ? got : 0), 0, n - (size_t)(got > 0 ? got : 0));
			} else {
				uint64_t end = sp && sp + 1 < s->spans + s->nspans ? sp[1].at : !sp && s->nspans ? s->spans[0].at :
													l->size;
				if (at < alt && alt < end)
					end = alt;
				n = (size_t)(end - at) < n ? (size_t)(end - at) : n;
				memset(p, 0, n);
			}
		}
		p += n;
		at += n;
		len -= n;
	}
}

static bool written(const struct share *s, uint64_t sector)
{
	return s->written[sector / 64] >> (sector % 64) & 1;
}

static bool any_written(const struct share *s, uint64_t at, uint64_t len)
{
	for (uint64_t k = at / SECTOR; k < (at + len + SECTOR - 1) / SECTOR; k++)
		if (written(s, k))
			return true;
	return false;
}

static int share_read(void *ctx, void *buf, size_t len, uint64_t at)
{
	struct share *s = ctx;
	uint8_t *p = buf;
	if (at > s->layout.size || len > s->layout.size - at)
		return -1;
	while (len) {
		bool w = written(s, at / SECTOR);
		uint64_t end = (at / SECTOR + 1) * SECTOR;
		while (end < at + len && written(s, end / SECTOR) == w)
			end += SECTOR;
		size_t n = (size_t)(end - at) < len ? (size_t)(end - at) : len;
		if (w) {
			if (pread(s->overlay, p, n, (off_t)at) != (ssize_t)n)
				return -1;
		} else {
			base_read(s, p, n, at);
		}
		p += n;
		at += n;
		len -= n;
	}
	return 0;
}

static int share_write(void *ctx, const void *buf, size_t len, uint64_t at)
{
	struct share *s = ctx;
	const uint8_t *p = buf;
	if (at > s->layout.size || len > s->layout.size - at)
		return -1;
	if (s->log)
		printf("share: write %zu bytes at %llu%s\n", len, (unsigned long long)at,
		       at <= 1024 && at + len >= 1024 + 12 ? (be16(p + 1024 - at + 10) & HFS_UNMOUNTED ? " (MDB: put away)" :
										 " (MDB: in use)") : "");
	while (len) {
		uint64_t sector = at / SECTOR;
		size_t within = (size_t)(at % SECTOR), n;
		if (within || len < SECTOR) {           /* part of a sector */
			uint8_t b[SECTOR];
			n = SECTOR - within < len ? SECTOR - within : len;
			if (share_read(s, b, SECTOR, sector * SECTOR))
				return -1;
			memcpy(b + within, p, n);
			if (pwrite(s->overlay, b, SECTOR, (off_t)(sector * SECTOR)) != SECTOR)
				return -1;
		} else {
			n = len - len % SECTOR;
			if (pwrite(s->overlay, p, n, (off_t)at) != (ssize_t)n)
				return -1;
		}
		for (uint64_t k = sector; k < (at + n + SECTOR - 1) / SECTOR; k++)
			if (!written(s, k)) {
				s->written[k / 64] |= 1ull << (k % 64);
				s->written_sectors++;
			}
		p += n;
		at += n;
		len -= n;
	}
	return 0;
}

/* The volume from the folder (`st`, its stat): its metadata, and where its forks are; Mac OS has written nothing */
static int make(struct share *s, const struct stat *st)
{
	if (scan(s, st) || build(s))
		return -1;
	if (!(s->written = calloc((s->layout.size / SECTOR + 63) / 64, 8)))
		return -1;
	s->disk = (struct disk_backend){ s->dir, s, s->layout.size, share_read, share_write };
	return 0;
}

/* What the volume was made of goes (the overlay's file stays) */
static void unmake(struct share *s)
{
	for (unsigned k = 0; k < OPEN_FILES; k++) {
		if (s->open[k].fd >= 0)
			close(s->open[k].fd);
		s->open[k].fd = -1;
	}
	for (unsigned k = 0; k < s->n; k++) {
		free(s->items[k].path);
		free(s->items[k].ad_path);
	}
	hfs_layout_free(&s->layout);
	free(s->items);
	free(s->entries);
	free(s->spans);
	free(s->written);
	s->items = NULL;
	s->entries = NULL;
	s->spans = NULL;
	s->written = NULL;
	s->n = s->cap = s->nspans = s->next_open = s->skipped = 0;
	s->written_sectors = 0;
}

/* Says what the volume holds */
static void say_made(const struct share *s, const char *how)
{
	unsigned files = 0;
	for (unsigned k = 1; k < s->n; k++)
		files += !s->entries[k].folder;
	printf("share: %s as \"%s\"%s: %u files, %u folders, %llu MB free\n", s->dir, s->volume, how, files,
	       s->n - 1 - files, (unsigned long long)((uint64_t)s->layout.free_blocks * s->layout.block_size >> 20));
}

struct share *share_open(const char *dir, int64_t start)
{
	struct share *s = calloc(1, sizeof *s);
	struct stat st;
	char real[PATH_MAX];
	if (!s)
		return NULL;
	errno = 0;
	s->overlay = -1;
	s->clock = hfs_mac_time(start ? start : (int64_t)time(NULL));
	for (unsigned k = 0; k < OPEN_FILES; k++)
		s->open[k].fd = -1;
	if (!realpath(dir, real) || stat(real, &st) || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "share: %s: %s\n", dir, errno ? strerror(errno) : "not a folder");
		goto fail;
	}
	const char *tmp = getenv("TMPDIR");
	char overlay[PATH_MAX];
	snprintf(overlay, sizeof overlay, "%s/blue-share-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
	if (!(s->dir = strdup(real)))
		goto fail;
	if ((s->overlay = mkstemp(overlay)) < 0 || unlink(overlay)) {
		fprintf(stderr, "share: no room for Mac OS's changes in %s\n", tmp && tmp[0] ? tmp : "/tmp");
		goto fail;
	}
	if (make(s, &st))
		goto fail;
	s->log = getenv("BLUE_LOG_SHARE") != NULL;
	say_made(s, "");
	return s;
fail:
	share_close(s);
	return NULL;
}

const struct disk_backend *share_disk(struct share *s)
{
	return s->gone ? NULL : &s->disk;
}

const char *share_name(const struct share *s)
{
	return s->volume;
}

const char *share_folder(const struct share *s)
{
	return s->dir;
}

bool share_put_away(struct share *s)
{
	uint8_t mdb[SECTOR];
	return !s->gone && s->written && written(s, 1024 / SECTOR) && !share_read(s, mdb, SECTOR, 1024) &&
	       (be16(mdb + 10) & HFS_UNMOUNTED);
}

int share_rebuild(struct share *s, int64_t start)
{
	struct stat st;
	unmake(s);
	s->written_back = false;
	s->gone = true;
	s->clock = hfs_mac_time(start ? start : (int64_t)time(NULL));
	errno = 0;
	if (stat(s->dir, &st) || !S_ISDIR(st.st_mode)) {
		printf("share: %s: %s: \"%s\" isn't offered again\n", s->dir, errno ? strerror(errno) : "not a folder",
		       s->volume);
		return -1;
	}
	if (ftruncate(s->overlay, 0) || make(s, &st)) {
		printf("share: \"%s\" couldn't be made again: it isn't offered again\n", s->volume);
		unmake(s);
		return -1;
	}
	s->gone = false;
	say_made(s, " again");
	return 0;
}

bool share_host_changed(const struct share *s)
{
	struct stat st;
	if (s->gone)
		return !stat(s->dir, &st) && S_ISDIR(st.st_mode);
	unsigned *children = calloc(s->n, sizeof *children);
	bool changed = !children;
	for (unsigned k = 1; children && k < s->n; k++)
		children[s->entries[k].parent]++;
	for (unsigned k = 0; !changed && k < s->n; k++) {
		const struct item *it = &s->items[k];
		char path[PATH_MAX];
		if (host_path(s, it->path, path, sizeof path) || stat(path, &st) || (uint64_t)st.st_ino != it->ino) {
			changed = true;
		} else if (!s->entries[k].folder) {
			changed = (uint64_t)st.st_size != it->size || mtime_ns(&st) != it->mtime_ns;
		} else if (mtime_ns(&st) != it->mtime_ns) {
			/* something in the folder came or went, or was renamed: what Mac OS sees, or only what it doesn't? */
			DIR *dir = opendir(path);
			struct dirent *de;
			unsigned count = 0, depth = 1;
			for (const char *p = it->path; *p; p++)
				depth += *p == '/';
			while (dir && (de = readdir(dir))) {
				struct stat est;
				const char *why;
				count += eligible(path, de->d_name, depth, &est, &why) > 0;
			}
			if (dir)
				closedir(dir);
			changed = !dir || count != children[k];
		}
	}
	free(children);
	return changed;
}

/* ---- the drive's icons ---------------------------------------------------------- */

/* A disk with a folder on it (characters as floppy.c's drawings): the volume's and the drive's */
static const char *const share32[32] = {
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"....KKKKKKKKKKKKKKKKKKKKKKKK....",
	"...KWWWWWWWWWWWWWWWWWWWWWWWLK...",
	"..KWLLLLLLLLLLLLLLLLLLLLLLLLGK..",
	".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
	".KWWWWWWWWWWWWWWWWWWWWWWWWWWWWK.",
	".KWLLLLLLLLLLLLLLLLLLLLLLLLLLGK.",
	".KWLLKKKKLLLLLLLLLLLLLLLLLLLLGK.",
	".KWLKccccKKKKKLLLLLLLLLLLLLLLGK.",
	".KWLKcccccccccKLLLLLLLLLLLLLLGK.",
	".KWLKcccccccccKLLLLLLLLLLLZZLGK.",
	".KWLKcccccccccKLLLLLLLLLLLLLLGK.",
	".KWLKKKKKKKKKKKLLLLLLLLLLLLLLGK.",
	".KWLLLLLLLLLLLLLLLLLLLLLLLLLLGK.",
	".KGGGGGGGGGGGGGGGGGGGGGGGGGGGGK.",
	".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
	"................................",
};

static const char *const share16[16] = {
	"................",
	"................",
	"................",
	"................",
	"..KKKKKKKKKKKK..",
	".KWLLLLLLLLLLGK.",
	"KKKKKKKKKKKKKKKK",
	"KWWWWWWWWWWWWWWK",
	"KWKKKLLLLLLLLLGK",
	"KWKccKKKLLLLLLGK",
	"KWKccccKLLLLZZGK",
	"KWKKKKKKLLLLLLGK",
	"KGGGGGGGGGGGGGGK",
	"KKKKKKKKKKKKKKKK",
	"................",
	"................",
};

void share_icons(uint8_t *media_icn, uint8_t *drive_icn, uint8_t *media_icns, uint8_t *drive_icns)
{
	floppy_icon_family(share32, share16, media_icn, media_icns);
	floppy_icon_family(share32, share16, drive_icn, drive_icns);
}

/* ---- writing back -------------------------------------------------------------- */

/* A file or folder of the volume as Mac OS left it */
struct node {
	uint32_t id, parent;
	bool folder;
	bool live;                      /* in the folder: not in the Trash, nor Mac OS's own */
	uint8_t name[32];
	uint8_t finder[32];
	uint32_t created, modified;
	struct hfsread_fork data, rsrc;
	int item;                       /* the host's file or folder it was made from, or -1 */
	char *host;                     /* where it is in the folder, once there */
};

/* What becomes of a host item */
struct fate {
	int node;                       /* -1: Mac OS deleted it */
	bool moved;                     /* renamed or moved */
	bool content;                   /* its forks changed */
	bool finder;                    /* its Finder info changed */
	bool host_changed;              /* it changed on the host meanwhile */
	char *where;                    /* where it is now, if not where its folder says */
};

struct sync {
	struct share *s;
	struct hfsread_volume v;
	struct node *nodes;
	unsigned n, cap;
	unsigned *by_id;
	struct fate *fate;
	unsigned changes;               /* to the user's files and folders */
	char staging[PATH_MAX];         /* (in the folder) */
	char recovery[PATH_MAX];        /* ... once made */
	unsigned added, changed, moved, deleted, kept, failed, notes;
};

static void note(struct sync *y, const char *fmt, ...)
{
	va_list ap;
	if (y->notes++ >= MAX_NOTES)
		return;
	va_start(ap, fmt);
	printf("share:   ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
}

static int add_node(void *ctx, const struct hfsread_file *f)
{
	struct sync *y = ctx;
	if (y->n == y->cap) {
		struct node *more = realloc(y->nodes, (y->cap = y->cap ? 2 * y->cap : 256) * sizeof *more);
		if (!more)
			return -1;
		y->nodes = more;
	}
	y->nodes[y->n++] = (struct node){ f->id, f->parent, f->folder, false, { 0 }, { 0 }, f->created, f->modified,
					 f->data, f->rsrc, -1, NULL };
	memcpy(y->nodes[y->n - 1].name, f->mac_name, sizeof f->mac_name);
	memcpy(y->nodes[y->n - 1].finder, f->finder, 32);
	return 0;
}

static struct sync *sync_y;             /* (for the sort) */

static int node_by_id(const void *x, const void *y)
{
	uint32_t a = sync_y->nodes[*(const unsigned *)x].id, b = sync_y->nodes[*(const unsigned *)y].id;
	return a < b ? -1 : a > b;
}

static int find_node(const struct sync *y, uint32_t id)
{
	unsigned lo = 0, hi = y->n;
	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;
		if (y->nodes[y->by_id[mid]].id < id)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < y->n && y->nodes[y->by_id[lo]].id == id ? (int)y->by_id[lo] : -1;
}

/* A folder's files and folders: nodes first..first+count-1 (the catalog keeps them together) */
static unsigned children(const struct sync *y, uint32_t id, unsigned *first)
{
	unsigned lo = 0, hi = y->n;
	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;
		if (y->nodes[mid].parent < id)
			lo = mid + 1;
		else
			hi = mid;
	}
	*first = lo;
	while (hi < y->n && y->nodes[hi].parent == id)
		hi++;
	return hi > lo ? hi - lo : 0;
}

static bool named(const uint8_t *name, const char *s)
{
	return name[0] == strlen(s) && !memcmp(name + 1, s, name[0]);
}

/*
 * What Mac OS (and its extensions) keep on a volume for themselves, at its
 * top: these, and anything invisible but the desktop's folder.
 */
static bool mac_os_own(const struct node *nd)
{
	static const char *const own[] = {
		"Desktop DB", "Desktop DF", "Desktop", "Trash", "TheVolumeSettingsFolder", "TheFindByContentFolder",
		"Temporary Items", "Network Trash Folder", "Shutdown Check", "Cleanup At Startup", "OpenFolderListDF\r",
		"Move&Rename", "VM Storage", "DesktopPrinters DB",
	};
	if (named(nd->name, "Desktop Folder"))
		return false;
	for (size_t k = 0; k < sizeof own / sizeof own[0]; k++)
		if (named(nd->name, own[k]))
			return true;
	return (be16(nd->finder + 8) & FINDER_INVISIBLE) != 0;
}

static int read_final(struct sync *y)
{
	struct share *s = y->s;
	if (hfsread_open_with(&y->v, share_read, s, s->layout.size) || y->v.plus || hfsread_walk(&y->v, add_node, y) ||
	    !(y->by_id = malloc((y->n + 1) * sizeof *y->by_id)) || !(y->fate = calloc(s->n, sizeof *y->fate)))
		return -1;
	for (unsigned k = 1; k < y->n; k++)
		if (y->nodes[k].parent < y->nodes[k - 1].parent)
			return -1;
	for (unsigned k = 0; k < y->n; k++)
		y->by_id[k] = k;
	sync_y = y;
	qsort(y->by_id, y->n, sizeof *y->by_id, node_by_id);
	for (unsigned k = 0; k < s->n; k++) {
		int nd = find_node(y, s->entries[k].id);
		y->fate[k].node = nd >= 0 && y->nodes[nd].folder == s->entries[k].folder ? nd : -1;
		if (y->fate[k].node >= 0)
			y->nodes[nd].item = (int)k;
	}

	/* what's in the folder: from the top, but for the Trash and Mac OS's own */
	int root = find_node(y, 2);
	if (root < 0)
		return -1;
	unsigned *queue = malloc(y->n * sizeof *queue), head = 0, tail = 0;
	if (!queue)
		return -1;
	y->nodes[root].live = true;
	queue[tail++] = (unsigned)root;
	while (head < tail) {
		const struct node *dir = &y->nodes[queue[head++]];
		unsigned first, count = children(y, dir->id, &first);
		for (unsigned k = first; k < first + count; k++) {
			struct node *nd = &y->nodes[k];
			if (nd->live || (dir->id == 2 && nd->item < 0 && mac_os_own(nd)))
				continue;
			nd->live = true;
			if (nd->folder && tail < y->n)
				queue[tail++] = k;
		}
	}
	free(queue);
	for (unsigned k = 0; k < s->n; k++)
		if (y->fate[k].node >= 0 && !y->nodes[y->fate[k].node].live)
			y->fate[k].node = -1;
	return 0;
}

static bool same_finder(const uint8_t *a, const uint8_t *b)
{
	return !memcmp(a, b, 8) && !((be16(a + 8) ^ be16(b + 8)) & ~FINDER_BOOKKEEPING);
}

/* Whether a fork's bytes are the host's: where they were made, untouched, or else the same */
static bool same_fork(struct sync *y, unsigned k, const struct hfsread_fork *f, bool rsrc)
{
	struct share *s = y->s;
	const struct hfs_entry *e = &s->entries[k];
	uint32_t len = rsrc ? e->rsrc_len : e->data_len, start = rsrc ? e->rsrc_start : e->data_start;
	uint32_t blocks = (uint32_t)(((uint64_t)len + s->layout.block_size - 1) / s->layout.block_size);
	uint64_t at = s->layout.first_block + (uint64_t)start * s->layout.block_size;
	if (f->size != len)
		return false;
	if (!len || (f->ext[0].start == start && f->ext[0].count >= blocks && !any_written(s, at, len)))
		return true;
	uint8_t *a = malloc(CHUNK), *b = malloc(CHUNK);
	const struct item *it = &s->items[k];
	int fd = source_fd(s, k, rsrc);
	bool same = a && b && fd >= 0;
	for (uint64_t pos = 0; same && pos < len; pos += CHUNK) {
		size_t n = len - pos < CHUNK ? (size_t)(len - pos) : CHUNK;
		off_t from = (off_t)(pos + (rsrc && !it->named_fork ? it->ad_rsrc : 0));
		same = !hfsread_read(&y->v, f, pos, a, n) && pread(fd, b, n, from) == (ssize_t)n && !memcmp(a, b, n);
	}
	free(a);
	free(b);
	return same;
}

static bool host_changed(const struct share *s, unsigned k)
{
	const struct item *it = &s->items[k];
	char path[PATH_MAX];
	struct stat st;
	return host_path(s, it->path, path, sizeof path) || stat(path, &st) || (uint64_t)st.st_ino != it->ino ||
	       (!s->entries[k].folder && ((uint64_t)st.st_size != it->size || mtime_ns(&st) != it->mtime_ns));
}

/* What Mac OS changed, by the host items it was made from, and how many changes there are in all */
static void plan(struct sync *y)
{
	struct share *s = y->s;
	for (unsigned k = 1; k < s->n; k++) {
		struct fate *f = &y->fate[k];
		const struct hfs_entry *e = &s->entries[k];
		if (f->node < 0) {
			bool top = y->fate[e->parent].node >= 0;
			y->changes += top;
			f->host_changed = top && host_changed(s, k);
			continue;
		}
		const struct node *nd = &y->nodes[f->node];
		f->moved = memcmp(nd->name, e->name, 1u + e->name[0]) || nd->parent != s->entries[e->parent].id;
		if (!e->folder) {
			f->content = !same_fork(y, k, &nd->data, false) || !same_fork(y, k, &nd->rsrc, true);
			f->finder = !f->content && !same_finder(nd->finder, e->finder);
		}
		if (f->content || f->moved)
			f->host_changed = host_changed(s, k);
		y->changes += f->moved + f->content + f->finder;
	}
	for (unsigned k = 0; k < y->n; k++) {
		const struct node *nd = &y->nodes[k];
		unsigned first;
		if (nd->live && nd->item < 0 && !(nd->folder && nd->parent == 2 && named(nd->name, "Desktop Folder") &&
						  !children(y, nd->id, &first)))
			y->changes++;
	}
}

/* ---- host files ---------------------------------------------------------------- */

static void host_name(const uint8_t *mac, char *out, size_t len)
{
	hfs_to_utf8(mac + 1, mac[0], out, len);
	for (char *p = out; *p; p++)
		if (*p == '/')
			*p = ':';
	if (!strcmp(out, ".") || !strcmp(out, ".."))
		out[0] = '_';
}

static int copy_fork(struct sync *y, const struct hfsread_fork *f, int fd, uint64_t at)
{
	uint8_t *buf = malloc(CHUNK);
	int rc = buf ? 0 : -1;
	for (uint64_t pos = 0; !rc && pos < f->size; pos += CHUNK) {
		size_t n = f->size - pos < CHUNK ? (size_t)(f->size - pos) : CHUNK;
		rc = hfsread_read(&y->v, f, pos, buf, n) || pwrite(fd, buf, n, (off_t)(at + pos)) != (ssize_t)n ? -1 : 0;
	}
	free(buf);
	return rc;
}

/* An AppleDouble file with a node's Finder info and resource fork */
static int write_appledouble(struct sync *y, const struct node *nd, const char *path)
{
	uint8_t h[AD_HEADER + 32] = { 0 };
	put_be32(h, AD_MAGIC);
	put_be32(h + 4, 0x00020000u);
	memcpy(h + 8, "Mac OS X        ", 16);
	put_be16(h + 24, 2);
	put_be32(h + 26, AD_FINDER);
	put_be32(h + 30, AD_HEADER);
	put_be32(h + 34, 32);
	put_be32(h + 38, AD_RSRC);
	put_be32(h + 42, AD_HEADER + 32);
	put_be32(h + 46, (uint32_t)nd->rsrc.size);
	memcpy(h + AD_HEADER, nd->finder, 32);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	int rc = pwrite(fd, h, sizeof h, 0) != (ssize_t)sizeof h || copy_fork(y, &nd->rsrc, fd, sizeof h) ? -1 : 0;
	return close(fd) ? -1 : rc;
}

/* Whether a file's Finder info says more than its name and contents would */
static bool finder_needed(struct sync *y, const struct node *nd, const char *name)
{
	uint8_t head[512];
	size_t n = nd->data.size < sizeof head ? (size_t)nd->data.size : sizeof head;
	uint32_t type, creator;
	if (hfsread_read(&y->v, &nd->data, 0, head, n))
		n = 0;
	guess_type(name, head, n, &type, &creator);
	return be32(nd->finder) != type || be32(nd->finder + 4) != creator || (be16(nd->finder + 8) & ~FINDER_BOOKKEEPING);
}

/* Whether metadata goes in an AppleDouble file rather than the file's own (macOS, unless it came that way) */
static bool appledouble_style(const struct share *s, int k)
{
#ifdef __APPLE__
	return k >= 0 && s->items[k].ad_path && !s->items[k].named_fork && !s->items[k].xattr_finder;
#else
	(void)s;
	(void)k;
	return true;
#endif
}

static void set_dates(const char *path, const struct node *nd)
{
	struct timespec t[2] = { { 0, UTIME_OMIT }, { (time_t)hfs_unix_time(nd->modified), 0 } };
	utimensat(AT_FDCWD, path, t, 0);
#ifdef __APPLE__
	struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT, .commonattr = ATTR_CMN_CRTIME };
	struct timespec created = { (time_t)hfs_unix_time(nd->created), 0 };
	setattrlist(path, &al, &created, sizeof created, 0);
#else
	(void)nd;
#endif
}

/*
 * A file's new contents in the staging folder: "c<ID>" (with its resource
 * fork and Finder info on macOS), and "c<ID>.ad" for an AppleDouble file.
 */
static int stage_file(struct sync *y, const struct node *nd, const char *name)
{
	char path[PATH_MAX + 32];
	snprintf(path, sizeof path, "%s/c%u", y->staging, nd->id);
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0)
		return -1;
	int rc = copy_fork(y, &nd->data, fd, 0);
	if (close(fd))
		rc = -1;
	bool finder = finder_needed(y, nd, name) || (nd->item >= 0 && y->s->items[nd->item].ad_path);
#ifdef __APPLE__
	if (!rc && nd->rsrc.size) {
		char rsrc[PATH_MAX + 64];
		snprintf(rsrc, sizeof rsrc, "%s/..namedfork/rsrc", path);
		fd = open(rsrc, O_WRONLY | O_CREAT, 0644);
		rc = fd < 0 || copy_fork(y, &nd->rsrc, fd, 0) ? -1 : 0;
		if (fd >= 0 && close(fd))
			rc = -1;
	}
	if (!rc && (finder || (nd->item >= 0 && y->s->items[nd->item].xattr_finder)))
		rc = setxattr(path, XATTR_FINDERINFO_NAME, nd->finder, 32, 0, 0);
#else
	if (!rc && (finder || nd->rsrc.size)) {
		snprintf(path, sizeof path, "%s/c%u.ad", y->staging, nd->id);
		rc = write_appledouble(y, nd, path);
	}
#endif
	return rc;
}

static bool pathf(char *out, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Formats a path: false (ENAMETOOLONG) if it doesn't fit */
static bool pathf(char *out, size_t len, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(out, len, fmt, ap);
	va_end(ap);
	if (n >= 0 && (size_t)n < len)
		return true;
	errno = ENAMETOOLONG;
	return false;
}

/* A path in the folder, from one relative to it ("" if too long, which no file operation takes) */
static const char *full(const struct sync *y, const char *rel, char *out, size_t len)
{
	if (host_path(y->s, rel, out, len)) {
		out[0] = '\0';
		errno = ENAMETOOLONG;
	}
	return out;
}

/* Where a host item is now (relative to the folder); false if that's too long */
static bool current(const struct sync *y, unsigned k, char *out, size_t len)
{
	const struct item *it = &y->s->items[k];
	const char *name = strrchr(it->path, '/');
	if (y->fate[k].where)
		return pathf(out, len, "%s", y->fate[k].where);
	if (!k) {
		out[0] = '\0';
		return true;
	}
	if (!current(y, y->s->entries[k].parent, out, len))
		return false;
	size_t at = strlen(out);
	return pathf(out + at, len - at, "%s%s", at ? "/" : "", name ? name + 1 : it->path);
}

static int make_dirs(char *path)
{
	for (char *p = path + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			int rc = mkdir(path, 0755);
			*p = '/';
			if (rc && errno != EEXIST)
				return -1;
		}
	return 0;
}

/*
 * A name no one has in a folder (a full path): `name`, or "name (Mac OS)",
 * "name (Mac OS 2)", ... before its extension. False if there's none.
 */
static bool free_name(const char *dir, const char *name, bool tagged, char *out, size_t len)
{
	const char *dot = strrchr(name, '.');
	size_t stem = dot && dot != name && strlen(dot) <= 6 ? (size_t)(dot - name) : strlen(name);
	struct stat st;
	if (!dir[0])
		return false;
	for (unsigned k = tagged; k < 1000; k++) {
		char tag[24] = "";
		if (k)
			snprintf(tag, sizeof tag, k == 1 ? " (Mac OS)" : " (Mac OS %u)", k);
		if (!pathf(out, len, "%s/%.*s%s%s", dir, (int)stem, name, tag, name + stem))
			return false;
		if (lstat(out, &st) && errno == ENOENT)
			return true;
	}
	errno = EEXIST;
	return false;
}

/* Moves a host item (a path in the folder) to the recovery folder, under the path it had */
static bool recover(struct sync *y, const char *from, const char *original, char *where, size_t len)
{
	char to[PATH_MAX], dir[PATH_MAX], path[PATH_MAX], src[PATH_MAX];
	if (!y->recovery[0]) {
		time_t t = time(NULL);
		struct tm tm;
		char stamp[32];
		localtime_r(&t, &tm);
		strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", &tm);
		snprintf(y->recovery, sizeof y->recovery, "%s/%s", SHARE_RECOVERY, stamp);
	}
	errno = EINVAL;
	if (!from[0] || !pathf(to, sizeof to, "%s/%s", y->recovery, original))
		return false;
	char *slash = strrchr(to, '/');
	*slash = '\0';
	size_t at = strlen(full(y, to, dir, sizeof dir));
	if (!at || !pathf(dir + at, sizeof dir - at, "/") || make_dirs(dir))
		return false;
	dir[at] = '\0';
	if (!free_name(dir, slash + 1, false, path, sizeof path) || !*full(y, from, src, sizeof src) || rename(src, path))
		return false;
	return pathf(where, len, "%s", path + strlen(y->s->dir) + 1);
}

/* The AppleDouble file beside a host item */
static bool beside(const char *path, char *out, size_t len)
{
	const char *slash = strrchr(path, '/');
	size_t dir = slash ? (size_t)(slash - path) + 1 : 0;
	return pathf(out, len, "%.*s._%s", (int)dir, path, path + dir);
}

static void writeback_failed(struct sync *y, const char *what, const char *path)
{
	y->failed++;
	note(y, "failed: %s %s (%s)", what, path, strerror(errno));
}

static int stage_all(struct sync *y)
{
	struct share *s = y->s;
	char name[HFSREAD_NAME_MAX], path[PATH_MAX + 32];
	for (unsigned k = 0; k < y->n; k++) {
		const struct node *nd = &y->nodes[k];
		const struct fate *f = nd->item >= 0 ? &y->fate[nd->item] : NULL;
		if (!nd->live || nd->folder || (f && !f->content && !f->finder && !(f->moved && s->items[nd->item].ad_path &&
										     !s->items[nd->item].ad_beside)))
			continue;
		host_name(nd->name, name, sizeof name);
		if (!f || f->content) {
			if (stage_file(y, nd, name))
				return -1;
		} else if (appledouble_style(s, nd->item)) {
			snprintf(path, sizeof path, "%s/a%u", y->staging, nd->id);
			if (write_appledouble(y, nd, path))
				return -1;
		}
	}
	return 0;
}

/* Puts a staged file (and its AppleDouble file) in a folder as `name`, or a free name like it */
static bool place(struct sync *y, const char *staged, const char *dir, const char *name, bool tagged, char *rel,
		  size_t len)
{
	char from[PATH_MAX], to[PATH_MAX], ad_from[PATH_MAX], ad[PATH_MAX], base[PATH_MAX];
	if (!pathf(from, sizeof from, "%s/%s", y->staging, staged) ||
	    !free_name(full(y, dir, base, sizeof base), name, tagged, to, sizeof to) || rename(from, to) ||
	    !pathf(rel, len, "%s", to + strlen(y->s->dir) + 1))
		return false;
	if (pathf(ad_from, sizeof ad_from, "%s.ad", from) && !access(ad_from, F_OK))
		return beside(to, ad, sizeof ad) && !rename(ad_from, ad);
	return true;
}

static void apply(struct sync *y)
{
	struct share *s = y->s;
	char a[PATH_MAX], b[PATH_MAX], c[PATH_MAX], d[PATH_MAX], name[HFSREAD_NAME_MAX];

	/* first everything that's read from the volume (which reads host files where they are) */
	if (!pathf(a, sizeof a, "%s/.blue-writeback-XXXXXX", s->dir) || !mkdtemp(a)) {
		writeback_failed(y, "making a folder in", s->dir);
		return;
	}
	memcpy(y->staging, a, sizeof y->staging);
	if (stage_all(y)) {
		writeback_failed(y, "staging Mac OS's files in", y->staging);
		return;
	}
	const char *staging_rel = y->staging + strlen(s->dir) + 1;

	/* renamed and moved items into the staging folder, those deepest first */
	unsigned *order = malloc(s->n * sizeof *order), count = 0;
	if (!order)
		return;
	for (unsigned depth = MAX_DEPTH + 1; depth-- > 0;)
		for (unsigned k = 1; k < s->n; k++) {
			unsigned level = 0;
			for (const char *p = s->items[k].path; *p; p++)
				level += *p == '/';
			if (level == depth && y->fate[k].node >= 0 && y->fate[k].moved)
				order[count++] = k;
		}
	for (unsigned j = 0; j < count; j++) {
		unsigned k = order[j];
		struct item *it = &s->items[k];
		if (!current(y, k, a, sizeof a) || !pathf(b, sizeof b, "%s/m%u", staging_rel, s->entries[k].id) ||
		    rename(full(y, a, c, sizeof c), full(y, b, d, sizeof d))) {
			if (errno == ENOENT)
				note(y, "not moved: %s, gone from the host", it->path);
			else
				writeback_failed(y, "moving", it->path);
			y->fate[k].moved = false;
			continue;
		}
		y->fate[k].where = strdup(b);
		if (it->ad_path && it->ad_beside && beside(a, c, sizeof c) && pathf(d, sizeof d, "%s.ad", b))
			rename(full(y, c, a, sizeof a), full(y, d, b, sizeof b));
	}
	free(order);

	/* deleted items to the recovery folder */
	for (unsigned k = 1; k < s->n; k++) {
		struct fate *f = &y->fate[k];
		const struct item *it = &s->items[k];
		if (f->node >= 0 || y->fate[s->entries[k].parent].node < 0)
			continue;
		if (f->host_changed && !s->entries[k].folder) {
			y->kept++;
			note(y, "kept %s: deleted in Mac OS, but it changed on the host too", it->path);
			continue;
		}
		if (!current(y, k, a, sizeof a) || !recover(y, a, it->path, b, sizeof b)) {
			if (errno == ENOENT)
				note(y, "deleted %s (already gone from the host)", it->path);
			else
				writeback_failed(y, "putting away", it->path);
			continue;
		}
		y->deleted++;
		note(y, "deleted %s (now in %s)", it->path, b);
		if (it->ad_path && it->ad_beside && beside(a, c, sizeof c) && beside(it->path, d, sizeof d))
			recover(y, c, d, b, sizeof b);
	}

	/* the volume's tree, from the top: new folders and files, moved ones put in place, changed ones replaced */
	unsigned *queue = malloc(y->n * sizeof *queue), head = 0, tail = 0;
	int root = find_node(y, 2);
	if (!queue)
		return;
	y->nodes[root].host = strdup("");
	queue[tail++] = (unsigned)root;
	while (head < tail) {
		const struct node *dir = &y->nodes[queue[head++]];
		unsigned first, n = children(y, dir->id, &first);
		if (!dir->host)
			continue;
		for (unsigned j = first; j < first + n; j++) {
			struct node *nd = &y->nodes[j];
			struct fate *f = nd->item >= 0 ? &y->fate[nd->item] : NULL;
			const struct item *it = f ? &s->items[nd->item] : NULL;
			if (!nd->live)
				continue;
			const char *kept = it ? strrchr(it->path, '/') : NULL;
			if (it && !memcmp(nd->name, s->entries[nd->item].name, 1u + nd->name[0]))
				snprintf(name, sizeof name, "%s", kept ? kept + 1 : it->path);
			else
				host_name(nd->name, name, sizeof name);
			if (f && !f->where) {                   /* where it was */
				if (!current(y, (unsigned)nd->item, a, sizeof a)) {
					writeback_failed(y, "finding", it->path);
					continue;
				}
				nd->host = strdup(a);
			} else if (f) {                         /* moved, renamed */
				if (!pathf(a, sizeof a, "m%u", nd->id) || !place(y, a, dir->host, name, false, b, sizeof b)) {
					writeback_failed(y, "moving", it->path);
					continue;
				}
				free(f->where);
				f->where = strdup(b);
				nd->host = strdup(b);
				y->moved++;
				note(y, "moved %s to %s", it->path, b);
			} else if (nd->folder) {                /* new */
				struct stat st;
				unsigned none;
				if (dir == &y->nodes[root] && named(nd->name, "Desktop Folder") && !children(y, nd->id, &none))
					continue;
				if (!pathf(b, sizeof b, "%s%s%s", dir->host, dir->host[0] ? "/" : "", name) ||
				    !*full(y, b, a, sizeof a)) {
					writeback_failed(y, "making", name);
					continue;
				}
				if (stat(a, &st) || !S_ISDIR(st.st_mode)) {
					if (!free_name(full(y, dir->host, c, sizeof c), name, false, a, sizeof a) || mkdir(a, 0755)) {
						writeback_failed(y, "making", b);
						continue;
					}
					y->added++;
					note(y, "new %s", a + strlen(s->dir) + 1);
				}
				nd->host = strdup(a + strlen(s->dir) + 1);
			} else {
				if (!pathf(a, sizeof a, "c%u", nd->id) || !place(y, a, dir->host, name, false, b, sizeof b)) {
					writeback_failed(y, "writing", name);
					continue;
				}
				set_dates(full(y, b, c, sizeof c), nd);
				nd->host = strdup(b);
				y->added++;
				note(y, "new %s", b);
			}
			if (!nd->host)
				continue;
			if (nd->folder) {
				if (tail < y->n)
					queue[tail++] = j;
				continue;
			}
			if (f && f->content) {                  /* the host's goes, unless it changed too */
				const char *slash = strrchr(nd->host, '/');
				const char *base = slash ? slash + 1 : nd->host;
				if (!pathf(c, sizeof c, "%.*s", slash ? (int)(slash - nd->host) : 0, nd->host) ||
				    !pathf(a, sizeof a, "c%u", nd->id)) {
					writeback_failed(y, "writing", nd->host);
					continue;
				}
				if (f->host_changed) {
					if (!place(y, a, c, base, true, b, sizeof b))
						writeback_failed(y, "writing", nd->host);
					else
						note(y, "kept %s: changed on the host too; Mac OS's is %s", nd->host, b);
					y->kept++;
					continue;
				}
				if (!recover(y, nd->host, it->path, b, sizeof b)) {
					writeback_failed(y, "replacing", nd->host);
					continue;
				}
				if (it->ad_path && it->ad_beside && beside(nd->host, d, sizeof d) && beside(it->path, name, sizeof name))
					recover(y, d, name, b, sizeof b);
				if (!place(y, a, c, base, false, b, sizeof b)) {
					writeback_failed(y, "writing", nd->host);
					continue;
				}
				set_dates(full(y, b, d, sizeof d), nd);
				y->changed++;
				note(y, "changed %s (the host's was put in %s)", b, y->recovery);
			} else if (f && (f->finder || (f->moved && it->ad_path && !it->ad_beside))) {
				if (!beside(nd->host, d, sizeof d)) {
					writeback_failed(y, "writing the Finder info of", nd->host);
					continue;
				}
				if (appledouble_style(s, nd->item)) {
					if (!access(full(y, d, b, sizeof b), F_OK))
						recover(y, d, d, c, sizeof c);
					if (!pathf(a, sizeof a, "%s/a%u", staging_rel, nd->id) ||
					    rename(full(y, a, c, sizeof c), full(y, d, b, sizeof b)))
						writeback_failed(y, "writing", d);
				}
#ifdef __APPLE__
				else if (setxattr(full(y, nd->host, b, sizeof b), XATTR_FINDERINFO_NAME, nd->finder, 32, 0, 0))
					writeback_failed(y, "writing the Finder info of", nd->host);
#endif
				if (f->finder) {
					y->changed++;
					note(y, "changed %s's type, creator or Finder flags", nd->host);
				}
			}
		}
	}
	free(queue);
	if (rmdir(y->staging))
		note(y, "left behind: %s", y->staging);
}

/* ---- the end of the run ---------------------------------------------------- */

/* The volume as Mac OS left it, as a (sparse) disk image */
static int save_image(struct share *s, const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	uint8_t *buf = malloc(CHUNK);
	int rc = fd < 0 || !buf || ftruncate(fd, (off_t)s->layout.size) ? -1 : 0;
	for (uint64_t at = 0; !rc && at < s->layout.size; at += CHUNK) {
		size_t n = s->layout.size - at < CHUNK ? (size_t)(s->layout.size - at) : CHUNK;
		const struct span *sp = span_at(s, at + n - 1);
		bool used = at < s->layout.meta_len || at + n > s->layout.size - 2 * SECTOR || any_written(s, at, n) ||
			    (sp && sp->at + sp->len > at);
		if (used && (share_read(s, buf, n, at) || pwrite(fd, buf, n, (off_t)at) != (ssize_t)n))
			rc = -1;
	}
	free(buf);
	if (fd >= 0 && close(fd))
		rc = -1;
	return rc;
}

/* Keeps the volume as an image in the recovery folder */
static int keep_image(struct sync *y)
{
	char stamp[32], path[PATH_MAX], name[HFS_MAX_NAME * 3 + 8];
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", &tm);
	snprintf(name, sizeof name, "%s.img", y->s->volume);
	for (char *p = name; *p; p++)
		if (*p == '/')
			*p = ':';
	snprintf(path, sizeof path, "%s/%s/%s/", y->s->dir, SHARE_RECOVERY, stamp);
	if (make_dirs(path))
		return -1;
	snprintf(path + strlen(path), sizeof path - strlen(path), "%s", name);
	if (save_image(y->s, path)) {
		unlink(path);
		fprintf(stderr, "share: cannot write %s\n", path);
		return -1;
	}
	printf("share: the volume as Mac OS left it is %s (attach it with --disk)\n", path);
	return 0;
}

static void free_share(struct share *s)
{
	unmake(s);
	if (s->overlay >= 0)
		close(s->overlay);
	free(s->dir);
	free(s);
}

/*
 * What Mac OS changed, written back if it put the volume away; otherwise
 * (`end`: the run's over) kept as an image. Returns 0, or -1 if something
 * couldn't be written back (it is then in the recovery folder).
 */
static int write_back(struct share *s, bool end)
{
	if (s->gone || !s->written_sectors) {
		if (!s->gone && end)
			printf("share: Mac OS didn't change \"%s\"\n", s->volume);
		return 0;
	}
	struct sync y = { .s = s };
	bool clean = share_put_away(s);
	bool readable = !read_final(&y);
	int rc = 0;
	if (readable)
		plan(&y);
	if (readable && !y.changes) {
		printf("share: nothing of yours changed on \"%s\"\n", s->volume);
	} else if (!readable) {
		printf("share: \"%s\" can't be read as Mac OS left it: nothing was written back to %s\n", s->volume, s->dir);
		rc = keep_image(&y);
	} else if (!clean) {
		printf("share: Mac OS didn't put \"%s\" away (no Shut Down): nothing was written back to %s\n", s->volume,
		       s->dir);
		rc = keep_image(&y);
	} else {
		printf("share: writing back what Mac OS changed on \"%s\" to %s\n", s->volume, s->dir);
		apply(&y);
		if (y.notes > MAX_NOTES)
			printf("share:   (and %u more)\n", y.notes - MAX_NOTES);
		printf("share: %u new, %u changed, %u moved or renamed, %u deleted", y.added, y.changed, y.moved, y.deleted);
		if (y.kept)
			printf(", %u kept as the host had them", y.kept);
		if (y.recovery[0])
			printf("; the host's old ones are in %s/%s", s->dir, y.recovery);
		printf("\n");
		if (y.failed) {
			fprintf(stderr, "share: %u things couldn't be written back\n", y.failed);
			keep_image(&y);
			rc = -1;
		}
	}
	for (unsigned k = 0; k < y.n; k++)
		free(y.nodes[k].host);
	for (unsigned k = 0; y.fate && k < s->n; k++)
		free(y.fate[k].where);
	free(y.nodes);
	free(y.by_id);
	free(y.fate);
	return rc;
}

int share_write_back(struct share *s)
{
	if (s->written_back || !share_put_away(s))
		return 0;
	s->written_back = true;
	return write_back(s, false);
}

int share_close(struct share *s)
{
	if (!s)
		return 0;
	int rc = s->written && !s->written_back ? write_back(s, true) : 0;
	free_share(s);
	return rc;
}
