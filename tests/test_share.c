/*
 * Shared folders: names in Mac OS Roman and the catalog's order, volumes
 * made from host folders (read back with hfsread, and with hfsutils if
 * installed), their dates, and Mac OS's changes written back: new,
 * changed, renamed, moved and deleted files, the recovery folder, changes
 * on both sides, and a volume Mac OS didn't put away; written back during
 * the run and made again, and not written back twice.
 */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/attr.h>
#include <sys/xattr.h>
#endif

#include "../src/be.h"
#include "../src/hfs.h"
#include "../src/hfsread.h"
#include "../src/share.h"
#include "test.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define TEXT FOURCC('T', 'E', 'X', 'T')
#define TTXT FOURCC('t', 't', 'x', 't')
#define MANY 4000u                     /* (a catalog of over 2048 nodes, so with a map node) */
#define MANY_DIRS 20u

static int saved_stdout = -1;

/* The share says what it does on stdout; the tests needn't (unless BLUE_TEST_VERBOSE is set) */
static void quiet(bool on)
{
	fflush(stdout);
	if (getenv("BLUE_TEST_VERBOSE"))
		return;
	if (on) {
		int null = open("/dev/null", O_WRONLY);
		saved_stdout = dup(1);
		dup2(null, 1);
		close(null);
	} else if (saved_stdout >= 0) {
		dup2(saved_stdout, 1);
		close(saved_stdout);
		saved_stdout = -1;
	}
}

static void put(const char *dir, const char *rel, const void *data, size_t len)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", dir, rel);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK(write(fd, data, len) == (ssize_t)len);
		close(fd);
	}
}

static void put_text(const char *dir, const char *rel, const char *text)
{
	put(dir, rel, text, strlen(text));
}

static void make(const char *dir, const char *rel)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", dir, rel);
	CHECK(mkdir(path, 0755) == 0);
}

/* A file's contents (NUL-terminated, malloc'd), or NULL */
static char *get(const char *dir, const char *rel, size_t *len)
{
	char path[1024];
	struct stat st;
	snprintf(path, sizeof path, "%s/%s", dir, rel);
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	char *p = NULL;
	if (!fstat(fileno(f), &st) && (p = malloc((size_t)st.st_size + 1))) {
		size_t n = fread(p, 1, (size_t)st.st_size, f);
		p[n] = '\0';
		if (len)
			*len = n;
	}
	fclose(f);
	return p;
}

static bool is_text(const char *dir, const char *rel, const char *text)
{
	char *p = get(dir, rel, NULL);
	bool same = p && !strcmp(p, text);
	free(p);
	return same;
}

static bool exists(const char *dir, const char *rel)
{
	char path[1024];
	struct stat st;
	snprintf(path, sizeof path, "%s/%s", dir, rel);
	return !lstat(path, &st);
}

static void remove_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st))
		return;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		struct dirent *de;
		while (d && (de = readdir(d))) {
			char sub[1024];
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			snprintf(sub, sizeof sub, "%s/%s", path, de->d_name);
			remove_tree(sub);
		}
		if (d)
			closedir(d);
		rmdir(path);
	} else {
		unlink(path);
	}
}

static int temp_dir(char *path, size_t len)
{
	const char *dir = getenv("TMPDIR");
	snprintf(path, len, "%s/blue-test-XXXXXX", dir && dir[0] ? dir : "/tmp");
	return mkdtemp(path) ? 0 : -1;
}

/* An AppleDouble file: Finder info (type, creator, flags) and a resource fork */
static void put_appledouble(const char *dir, const char *rel, uint32_t type, uint32_t creator, const char *rsrc)
{
	uint8_t b[256] = { 0 };
	size_t n = strlen(rsrc);
	put_be32(b, 0x00051607u);
	put_be32(b + 4, 0x00020000u);
	memcpy(b + 8, "Mac OS X        ", 16);
	put_be16(b + 24, 2);
	put_be32(b + 26, 9);
	put_be32(b + 30, 50);
	put_be32(b + 34, 32);
	put_be32(b + 38, 2);
	put_be32(b + 42, 82);
	put_be32(b + 46, (uint32_t)n);
	put_be32(b + 50, type);
	put_be32(b + 54, creator);
	memcpy(b + 82, rsrc, n);
	put(dir, rel, b, 82 + n);
}

/* ---- names --------------------------------------------------------------------- */

static void test_names(void)
{
	uint8_t m[64];
	char u[64];
	CHECK_EQ(hfs_from_utf8("caf\xC3\xA9", m, sizeof m), 4);         /* precomposed */
	CHECK(!memcmp(m, "caf\x8E", 4));
	CHECK_EQ(hfs_from_utf8("cafe\xCC\x81", m, sizeof m), 4);        /* decomposed */
	CHECK(!memcmp(m, "caf\x8E", 4));
	CHECK_EQ(hfs_from_utf8("U\xCC\x88" "ber", m, sizeof m), 4);        /* Ü */
	CHECK_EQ(m[0], 0x86);
	CHECK_EQ(hfs_from_utf8("o\xCC\x84", m, sizeof m), 1);           /* ō: Mac OS Roman has no macron */
	CHECK_EQ(m[0], 'o');
	CHECK_EQ(hfs_from_utf8("\xE2\x82\xAC\xE6\x97\xA5", m, sizeof m), 2);   /* €, then what it lacks */
	CHECK(m[0] == 0xDB && m[1] == '_');
	CHECK_EQ(hfs_from_utf8("abcdef", m, 3), 6);                     /* (writes at most 3) */
	CHECK_EQ(hfs_to_utf8((const uint8_t *)"caf\x8E" " \xA5", 6, u, sizeof u), 9);
	CHECK(!strcmp(u, "caf\xC3\xA9" " \xE2\x80\xA2"));

	/* the catalog's order: without regard to case, accented letters after their plain ones */
	CHECK(hfs_compare_names((const uint8_t *)"abc", 3, (const uint8_t *)"ABC", 3) == 0);
	CHECK(hfs_compare_names((const uint8_t *)"ab", 2, (const uint8_t *)"abc", 3) < 0);
	CHECK(hfs_compare_names((const uint8_t *)"e", 1, (const uint8_t *)"\x8E", 1) < 0);     /* e < é */
	CHECK(hfs_compare_names((const uint8_t *)"\x8E", 1, (const uint8_t *)"F", 1) < 0);     /* é < F */
	CHECK(hfs_compare_names((const uint8_t *)"\x83", 1, (const uint8_t *)"\x8E", 1) == 0); /* É = é */
	CHECK(hfs_compare_names((const uint8_t *)"Z", 1, (const uint8_t *)"[", 1) < 0);
	CHECK(hfs_compare_names((const uint8_t *)" ", 1, (const uint8_t *)"\xCA", 1) == 0);    /* non-breaking space */
}

/* ---- the volume ---------------------------------------------------------------- */

struct seen {
	unsigned files, folders;
	uint32_t parent;
	uint8_t name[32];
	bool ordered;
	struct hfsread_file found[12];
	const char *want[12];
	char long_name[HFSREAD_NAME_MAX];
	uint32_t many_id, many_dirs[MANY_DIRS];
	unsigned dirs;
};

static int look(void *ctx, const struct hfsread_file *f)
{
	struct seen *s = ctx;
	if (f->parent < s->parent ||
	    (f->parent == s->parent && hfs_compare_names(s->name + 1, s->name[0], f->mac_name + 1, f->mac_name[0]) >= 0))
		s->ordered = false;
	s->parent = f->parent;
	memcpy(s->name, f->mac_name, 32);
	if (f->folder)
		s->folders++;
	else
		s->files++;
	for (unsigned k = 0; k < 12 && s->want[k]; k++)
		if (!strcmp(f->name, s->want[k]))
			s->found[k] = *f;
	if (!strncmp(f->name, "A very long", 11))
		snprintf(s->long_name, sizeof s->long_name, "%s", f->name);
	if (!strcmp(f->name, "many"))
		s->many_id = f->id;
	if (f->folder && !strncmp(f->name, "dir ", 4) && s->dirs < MANY_DIRS)
		s->many_dirs[s->dirs++] = f->id;
	return 0;
}

/* Writes a volume to an image file (sparse: zero chunks are left out) */
static int save(const struct disk_backend *b, const char *path)
{
	static uint8_t buf[1 << 20];
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	int rc = fd < 0 || ftruncate(fd, (off_t)b->size) ? -1 : 0;
	for (uint64_t at = 0; !rc && at < b->size; at += sizeof buf) {
		size_t n = b->size - at < sizeof buf ? (size_t)(b->size - at) : sizeof buf;
		if (b->read(b->ctx, buf, n, at))
			rc = -1;
		bool zero = true;
		for (size_t k = 0; k < n && zero; k++)
			zero = !buf[k];
		if (!rc && !zero && pwrite(fd, buf, n, (off_t)at) != (ssize_t)n)
			rc = -1;
	}
	if (fd >= 0)
		close(fd);
	return rc;
}

/* hfsutils' view of an image, if it's installed: hfsck finds nothing wrong, hls lists everything */
static void check_hfsutils(const char *image, const char *home)
{
	char cmd[8192];
	if (system("command -v hfsck > /dev/null 2>&1") == 0) {
		/* (its -n fails to read anything; without it, it would only offer repairs, to no one) */
		snprintf(cmd, sizeof cmd, "hfsck -v '%s' < /dev/null > '%s/hfsck.txt' 2>&1", image, home);
		int rc = system(cmd);
		CHECK(rc == 0);
		if (rc || getenv("BLUE_TEST_VERBOSE")) {
			char *out = get(home, "hfsck.txt", NULL);
			fprintf(stderr, "%s", out ? out : "");
			free(out);
		}
	}
	if (system("command -v fsck.hfs > /dev/null 2>&1") == 0) {        /* Apple's fsck_hfs (Linux's hfsprogs) */
		snprintf(cmd, sizeof cmd, "fsck.hfs -n -f '%s' > '%s/fsck.txt' 2>&1", image, home);
		int rc = system(cmd);
		char *out = get(home, "fsck.txt", NULL);
		CHECK(rc == 0 && out && strstr(out, "appears to be OK"));
		if (rc || getenv("BLUE_TEST_VERBOSE"))
			fprintf(stderr, "%s", out ? out : "");
		free(out);
	}
	if (system("command -v hmount > /dev/null 2>&1") == 0) {
		/* (hfsutils keep their state in $HOME) */
		snprintf(cmd, sizeof cmd, "HOME='%s' hmount '%s' > /dev/null && HOME='%s' hls -R > '%s/ls.txt' 2>&1; "
			 "HOME='%s' humount > /dev/null 2>&1", home, image, home, home, home);
		CHECK(system(cmd) == 0);
		char *ls = get(home, "ls.txt", NULL);
		CHECK(ls && strstr(ls, "hello.txt") && strstr(ls, "file 3999") && strstr(ls, "x.txt"));
		free(ls);
	}
}

static void test_volume(void)
{
	char dir[512], scratch[512], image[600];
	if (temp_dir(dir, sizeof dir) || temp_dir(scratch, sizeof scratch)) {
		CHECK(!"temp_dir");
		return;
	}
	put_text(dir, "hello.txt", "Hello\n");
	put_text(dir, "plain", "no extension, but text\n");
	put(dir, "noext", "\x00\x01\x02", 3);
	put_text(dir, "archive.SIT", "SIT!");
	put_text(dir, "A very long file name that goes on and on.txt", "long");
	put_text(dir, "cafe\xCC\x81.txt", "decomposed");
	put_text(dir, "a:b", "colon");
	put_text(dir, ".DS_Store", "x");
	put_text(dir, ".hidden", "x");
	put_text(dir, "withfork", "data fork");
	put_appledouble(dir, "._withfork", FOURCC('A', 'P', 'P', 'L'), FOURCC('t', 'e', 's', 't'), "RSRCDATA");
	make(dir, "sub");
	make(dir, "sub/deeper");
	put_text(dir, "sub/deeper/x.txt", "deep");
	put_text(dir, "sub/zipped", "zipped");
	make(dir, "__MACOSX");
	make(dir, "__MACOSX/sub");
	put_appledouble(dir, "__MACOSX/sub/._zipped", FOURCC('Z', 'I', 'P', 'D'), FOURCC('z', 'z', 'z', 'z'), "");
	char path[1024], target[1024];
	snprintf(path, sizeof path, "%s/link to sub", dir);
	snprintf(target, sizeof target, "%s/sub", dir);
	CHECK(symlink(target, path) == 0);              /* left out */
	snprintf(path, sizeof path, "%s/link to hello", dir);
	snprintf(target, sizeof target, "%s/hello.txt", dir);
	CHECK(symlink(target, path) == 0);              /* a file like any other */
	make(dir, "many");
	for (unsigned k = 0; k < MANY; k++) {
		char name[64];
		snprintf(name, sizeof name, "many/file %04u", k);
		put_text(dir, name, name);
	}
	for (unsigned k = 0; k < MANY_DIRS; k++) {
		char name[64];
		snprintf(name, sizeof name, "many/dir %02u", k);
		make(dir, name);
	}
	/* two names that are one to Mac OS, where the host can have both */
	put_text(dir, "Same", "1");
	put_text(dir, "same", "2");
	bool both = is_text(dir, "Same", "1") && is_text(dir, "same", "2");
#ifdef __APPLE__
	snprintf(path, sizeof path, "%s/xattr file", dir);
	put_text(dir, "xattr file", "data");
	uint8_t fi[32] = { 'P', 'R', 'E', 'F', 'a', 'b', 'c', 'd' };
	CHECK(setxattr(path, XATTR_FINDERINFO_NAME, fi, 32, 0, 0) == 0);
	CHECK(setxattr(path, XATTR_RESOURCEFORK_NAME, "FORK", 4, 0, 0) == 0);
#endif

	quiet(true);
	struct share *s = share_open(dir, 0);
	quiet(false);
	CHECK(s != NULL);
	if (!s)
		goto out;
	const struct disk_backend *b = share_disk(s);
	CHECK(b->size >= (1u << 30));
	struct hfsread_volume v;
	CHECK_EQ(hfsread_open_with(&v, b->read, b->ctx, b->size), 0);
	CHECK(!v.plus && !v.blessed && (v.attributes & HFS_UNMOUNTED));
	uint8_t boot[2];
	CHECK(b->read(b->ctx, boot, 2, 0) == 0 && boot[0] == 0 && boot[1] == 0);      /* not a startup disk */

	struct seen seen = { .ordered = true,
			     .want = { "hello.txt", "plain", "noext", "archive.SIT", "withfork", "zipped", "a/b",
				       "caf\xC3\xA9.txt", "link to hello", "xattr file" } };
	CHECK_EQ(hfsread_walk(&v, look, &seen), 0);
	CHECK(seen.ordered);
#ifdef __APPLE__
	const unsigned xattr_files = 1;
#else
	const unsigned xattr_files = 0;
#endif
	CHECK_EQ(seen.files, 12 + both + MANY + xattr_files);
	CHECK_EQ(seen.folders, 4 + MANY_DIRS);
	CHECK(v.catalog.root > 1 && v.catalog.nodes > 2048);
	const struct hfsread_file *f = seen.found;
	CHECK(f[0].type == TEXT && f[0].creator == TTXT && f[0].data.size == 6);
	char buf[64] = { 0 };
	CHECK(hfsread_read(&v, &f[0].data, 0, buf, 6) == 0 && !memcmp(buf, "Hello\n", 6));
	CHECK(f[1].type == TEXT && f[1].creator == TTXT);
	CHECK_EQ(f[2].type, FOURCC('?', '?', '?', '?'));
	CHECK(f[3].type == FOURCC('S', 'I', 'T', '!') && f[3].creator == FOURCC('S', 'I', 'T', 'x'));
	CHECK(f[4].type == FOURCC('A', 'P', 'P', 'L') && f[4].creator == FOURCC('t', 'e', 's', 't'));
	CHECK(f[4].rsrc.size == 8 && hfsread_read(&v, &f[4].rsrc, 0, buf, 8) == 0 && !memcmp(buf, "RSRCDATA", 8));
	CHECK(f[4].data.size == 9 && hfsread_read(&v, &f[4].data, 0, buf, 9) == 0 && !memcmp(buf, "data fork", 9));
	CHECK_EQ(f[5].type, FOURCC('Z', 'I', 'P', 'D'));       /* from __MACOSX */
	CHECK(f[6].id && f[6].data.size == 5);                  /* "a:b" is "a/b" */
	CHECK(f[7].id && f[7].data.size == 10);                 /* decomposed é */
	CHECK(f[8].id && f[8].data.size == 6);                  /* a symbolic link to a file */
#ifdef __APPLE__
	CHECK(f[9].type == FOURCC('P', 'R', 'E', 'F') && f[9].creator == FOURCC('a', 'b', 'c', 'd') && f[9].rsrc.size == 4);
#endif
	/* the long name, shortened with its ID, keeps its extension */
	size_t long_len = strlen(seen.long_name);
	CHECK(long_len > 0 && long_len <= 31 && strchr(seen.long_name, '#') && !strcmp(seen.long_name + long_len - 4, ".txt"));
	struct hfsread_file found;
	char volume_path[1024];
	CHECK(hfsread_path(&v, 2, volume_path, sizeof volume_path) == 0);
	const char *base = strrchr(dir, '/') + 1;
	CHECK(!strcmp(volume_path, base));

	/* every folder's thread record is found through the index nodes */
	CHECK_EQ(seen.dirs, MANY_DIRS);
	for (unsigned k = 0; k < seen.dirs; k++) {
		char want[64];
		snprintf(want, sizeof want, "%s:many:dir %02u", base, k);
		CHECK(hfsread_path(&v, seen.many_dirs[k], volume_path, sizeof volume_path) == 0 && !strcmp(volume_path, want));
	}
	CHECK(hfsread_find(&v, seen.many_id, TEXT, &found) == 1 && !strcmp(found.name, "file 0000"));

	snprintf(image, sizeof image, "%s/share.img", scratch);
	CHECK(save(b, image) == 0);
	check_hfsutils(image, scratch);

	/* long and clashing names */
	struct hfsread_volume iv;
	int fd = open(image, O_RDONLY);
	CHECK(fd >= 0 && hfsread_open(&iv, fd, 0, b->size) == 0);
	struct seen names = { .ordered = true, .want = { "Same", "same" } };
	CHECK(hfsread_walk(&iv, look, &names) == 0);
	close(fd);
	if (both)
		CHECK(names.found[0].id && !names.found[1].id);         /* "same" became "same#..." */
	quiet(true);
	CHECK_EQ(share_close(s), 0);                                    /* (Mac OS wrote nothing) */
	quiet(false);
	CHECK(!exists(dir, SHARE_RECOVERY));
out:
	remove_tree(dir);
	remove_tree(scratch);
}

/* Sparse files: the biggest HFS allows, one too big for it */
static void test_big_files(void)
{
	char dir[512], path[1024];
	if (temp_dir(dir, sizeof dir)) {
		CHECK(!"temp_dir");
		return;
	}
	snprintf(path, sizeof path, "%s/big", dir);
	int fd = open(path, O_WRONLY | O_CREAT, 0644);
	CHECK(fd >= 0 && pwrite(fd, "END!", 4, HFS_MAX_FORK - 4) == 4);
	close(fd);
	snprintf(path, sizeof path, "%s/huge", dir);
	fd = open(path, O_WRONLY | O_CREAT, 0644);
	CHECK(fd >= 0 && ftruncate(fd, (off_t)HFS_MAX_FORK + 1) == 0);
	close(fd);
	put_text(dir, "small", "small");

	quiet(true);
	struct share *s = share_open(dir, 0);
	quiet(false);
	CHECK(s != NULL);
	if (s) {
		const struct disk_backend *b = share_disk(s);
		struct hfsread_volume v;
		struct seen seen = { .ordered = true, .want = { "big", "huge", "small" } };
		CHECK(hfsread_open_with(&v, b->read, b->ctx, b->size) == 0 && hfsread_walk(&v, look, &seen) == 0);
		CHECK_EQ(seen.files, 2);
		CHECK(seen.found[0].data.size == HFS_MAX_FORK && !seen.found[1].id);
		CHECK(b->size > (uint64_t)HFS_MAX_FORK + (1u << 30) && b->size < (4ull << 30));
		char end[4];
		CHECK(hfsread_read(&v, &seen.found[0].data, HFS_MAX_FORK - 4, end, 4) == 0 && !memcmp(end, "END!", 4));
		CHECK(hfsread_read(&v, &seen.found[2].data, 0, end, 4) == 0 && !memcmp(end, "smal", 4));
		quiet(true);
		share_close(s);
		quiet(false);
	}
	remove_tree(dir);
}

/* ---- writing back ---------------------------------------------------------------- */

/* The IDs the volume gave the host's files, by name */
struct ids {
	const char *name[16];
	uint32_t id[16];
};

#define IDS(...) { { __VA_ARGS__ }, { 0 } }

static int find_ids(void *ctx, const struct hfsread_file *f)
{
	struct ids *ids = ctx;
	for (unsigned k = 0; k < 16 && ids->name[k]; k++)
		if (!strcmp(f->name, ids->name[k]))
			ids->id[k] = f->id;
	return 0;
}

static uint32_t id_of(const struct ids *ids, const char *name)
{
	for (unsigned k = 0; k < 16 && ids->name[k]; k++)
		if (!strcmp(ids->name[k], name))
			return ids->id[k];
	return 0;
}

struct fake {
	struct hfs_entry e;
	const char *data, *rsrc;
};

static void entry(struct fake *f, unsigned *n, const char *name, unsigned parent, bool folder, uint32_t id,
		  const char *data, uint32_t type, uint32_t creator, const char *rsrc)
{
	struct fake *x = &f[(*n)++];
	memset(x, 0, sizeof *x);
	x->e.name[0] = (uint8_t)strlen(name);
	memcpy(x->e.name + 1, name, x->e.name[0]);
	x->e.parent = parent;
	x->e.folder = folder;
	x->e.id = id;
	x->e.created = x->e.modified = 3000000000u;
	put_be32(x->e.finder, type);
	put_be32(x->e.finder + 4, creator);
	x->data = data;
	x->rsrc = rsrc;
	x->e.data_len = data ? (uint32_t)strlen(data) : 0;
	x->e.rsrc_len = rsrc ? (uint32_t)strlen(rsrc) : 0;
}

/* Puts a volume of these entries where the share's was, through its drive's writes, as Mac OS would */
static int pretend_mac_os(const struct disk_backend *b, struct fake *f, unsigned n, bool put_away)
{
	struct hfs_entry e[32];
	struct hfs_layout l;
	for (unsigned k = 0; k < n; k++)
		e[k] = f[k].e;
	if (hfs_build(&l, e, n, b->size, 0, 3000000000u))
		return -1;
	if (!put_away) {
		put_be16(l.meta + 1024 + 10, 0);
		put_be16(l.mdb + 10, 0);
	}
	int rc = b->write(b->ctx, l.meta, l.meta_len, 0) | b->write(b->ctx, l.mdb, 512, l.size - 1024);
	for (unsigned k = 0; k < n; k++) {
		if (e[k].data_len)
			rc |= b->write(b->ctx, f[k].data, e[k].data_len, l.first_block + (uint64_t)e[k].data_start * l.block_size);
		if (e[k].rsrc_len)
			rc |= b->write(b->ctx, f[k].rsrc, e[k].rsrc_len, l.first_block + (uint64_t)e[k].rsrc_start * l.block_size);
	}
	hfs_layout_free(&l);
	return rc ? -1 : 0;
}

/* The one folder in the recovery folder (named for when) */
static void recovery_dir(const char *dir, char *out, size_t len)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", dir, SHARE_RECOVERY);
	out[0] = '\0';
	DIR *d = opendir(path);
	struct dirent *de;
	while (d && (de = readdir(d)))
		if (de->d_name[0] != '.')
			snprintf(out, len, "%s/%s", SHARE_RECOVERY, de->d_name);
	if (d)
		closedir(d);
}

/* A file's type and creator, as the host keeps them */
static bool has_type(const char *dir, const char *rel, uint32_t type, uint32_t creator, const char *rsrc)
{
	uint8_t fi[32] = { 0 };
	char path[1024], *fork = NULL;
	size_t rsrc_len = 0;
#ifdef __APPLE__
	snprintf(path, sizeof path, "%s/%s", dir, rel);
	if (getxattr(path, XATTR_FINDERINFO_NAME, fi, 32, 0, 0) != 32)
		return false;
	ssize_t n = getxattr(path, XATTR_RESOURCEFORK_NAME, NULL, 0, 0, 0);
	if (n > 0 && (fork = malloc((size_t)n)))
		rsrc_len = (size_t)getxattr(path, XATTR_RESOURCEFORK_NAME, fork, (size_t)n, 0, 0);
#else
	const char *slash = strrchr(rel, '/');
	size_t at = slash ? (size_t)(slash - rel) + 1 : 0;
	snprintf(path, sizeof path, "%.*s._%s", (int)at, rel, rel + at);
	size_t len;
	char *ad = get(dir, path, &len);
	if (!ad || len < 82 || be32(ad) != 0x00051607u) {
		free(ad);
		return false;
	}
	memcpy(fi, ad + 50, 32);
	if ((rsrc_len = len - 82) && (fork = malloc(rsrc_len)))
		memcpy(fork, ad + 82, rsrc_len);
	free(ad);
#endif
	bool ok = be32(fi) == type && be32(fi + 4) == creator && rsrc_len == strlen(rsrc) &&
		  (!rsrc_len || (fork && !memcmp(fork, rsrc, rsrc_len)));
	free(fork);
	return ok;
}

static void test_write_back(void)
{
	char dir[512], rec[512], p[1024];
	if (temp_dir(dir, sizeof dir)) {
		CHECK(!"temp_dir");
		return;
	}
	put_text(dir, "a.txt", "a");
	put_text(dir, "b.txt", "b");
	put_text(dir, "c.txt", "c");
	put_text(dir, "keep.txt", "keep");
	put_text(dir, "conflict.txt", "before");
	put_text(dir, "withfork", "data fork");
	put_appledouble(dir, "._withfork", FOURCC('A', 'P', 'P', 'L'), FOURCC('t', 'e', 's', 't'), "RSRC");
	make(dir, "sub");
	put_text(dir, "sub/d.txt", "d");
	make(dir, "gone");
	put_text(dir, "gone/e.txt", "e");
	put_text(dir, ".hidden", "not Mac OS's business");

	quiet(true);
	struct share *s = share_open(dir, 0);
	quiet(false);
	CHECK(s != NULL);
	if (!s) {
		remove_tree(dir);
		return;
	}
	const struct disk_backend *b = share_disk(s);
	struct hfsread_volume v;
	struct ids ids = IDS("a.txt", "b.txt", "c.txt", "keep.txt", "conflict.txt", "withfork", "sub", "d.txt", "gone",
			     "e.txt");
	CHECK(hfsread_open_with(&v, b->read, b->ctx, b->size) == 0 && hfsread_walk(&v, find_ids, &ids) == 0);
	const char *root = strrchr(dir, '/') + 1;

	/* what Mac OS did: */
	struct fake f[32];
	unsigned n = 0;
	entry(f, &n, root, 0, true, 0, NULL, 0, 0, NULL);
	entry(f, &n, "sub", 0, true, id_of(&ids, "sub"), NULL, 0, 0, NULL);
	entry(f, &n, "a renamed.txt", 1, false, id_of(&ids, "a.txt"), "a", TEXT, TTXT, NULL);        /* moved, renamed */
	entry(f, &n, "b.txt", 0, false, id_of(&ids, "b.txt"), "new b", TEXT, TTXT, NULL);            /* changed */
	entry(f, &n, "keep.txt", 0, false, id_of(&ids, "keep.txt"), "keep", FOURCC('t', 't', 'r', 'o'), TTXT, NULL);
	entry(f, &n, "conflict.txt", 0, false, id_of(&ids, "conflict.txt"), "Mac OS's", TEXT, TTXT, NULL);
	entry(f, &n, "withfork", 0, false, id_of(&ids, "withfork"), "data fork", FOURCC('A', 'P', 'P', 'L'),
	      FOURCC('t', 'e', 's', 't'), "RSRC");                                                 /* the same */
	entry(f, &n, "d.txt", 1, false, id_of(&ids, "d.txt"), "d", TEXT, TTXT, NULL);                /* the same */
	entry(f, &n, "new.txt", 0, false, 100, "new", TEXT, TTXT, NULL);
	entry(f, &n, "New Folder", 0, true, 101, NULL, 0, 0, NULL);
	entry(f, &n, "Tool", 9, false, 102, "code", FOURCC('A', 'P', 'P', 'L'), FOURCC('a', 'b', 'c', 'd'), "FORK");
	entry(f, &n, "Desktop DB", 0, false, 103, "btree", FOURCC('B', 'T', 'F', 'L'), FOURCC('D', 'M', 'G', 'R'), NULL);
	entry(f, &n, "Trash", 0, true, 104, NULL, 0, 0, NULL);
	entry(f, &n, "c.txt", 12, false, id_of(&ids, "c.txt"), "c", TEXT, TTXT, NULL);              /* in the Trash */
	entry(f, &n, "Desktop Folder", 0, true, 105, NULL, 0, 0, NULL);                              /* empty */
	entry(f, &n, "An Index", 0, false, 106, "index", FOURCC('i', 'n', 'd', 'x'), FOURCC('x', 'x', 'x', 'x'), NULL);
	put_be16(f[n - 1].e.finder + 8, 0x4000);                                                       /* invisible */
	/* "gone" and its e.txt were deleted */
	CHECK(pretend_mac_os(b, f, n, true) == 0);
	put_text(dir, "conflict.txt", "the host's");                                                 /* meanwhile */

	quiet(true);
	CHECK_EQ(share_close(s), 0);
	quiet(false);
	recovery_dir(dir, rec, sizeof rec);
	CHECK(rec[0]);

	CHECK(!exists(dir, "a.txt") && is_text(dir, "sub/a renamed.txt", "a"));
	CHECK(is_text(dir, "b.txt", "new b"));
	snprintf(p, sizeof p, "%s/b.txt", rec);
	CHECK(is_text(dir, p, "b"));                                    /* the host's old one */
	CHECK(!exists(dir, "c.txt"));
	snprintf(p, sizeof p, "%s/c.txt", rec);
	CHECK(is_text(dir, p, "c"));
	CHECK(!exists(dir, "gone"));
	snprintf(p, sizeof p, "%s/gone/e.txt", rec);
	CHECK(is_text(dir, p, "e"));
	CHECK(is_text(dir, "keep.txt", "keep") && has_type(dir, "keep.txt", FOURCC('t', 't', 'r', 'o'), TTXT, ""));
	CHECK(is_text(dir, "conflict.txt", "the host's"));
	CHECK(is_text(dir, "conflict (Mac OS).txt", "Mac OS's"));
	CHECK(is_text(dir, "withfork", "data fork") && exists(dir, "._withfork"));
	CHECK(is_text(dir, "sub/d.txt", "d"));
	CHECK(is_text(dir, "new.txt", "new") && !exists(dir, "._new.txt"));   /* (its type goes without saying) */
	CHECK(is_text(dir, "New Folder/Tool", "code"));
	CHECK(has_type(dir, "New Folder/Tool", FOURCC('A', 'P', 'P', 'L'), FOURCC('a', 'b', 'c', 'd'), "FORK"));
	CHECK(!exists(dir, "Desktop DB") && !exists(dir, "Trash") && !exists(dir, "Desktop Folder") &&
	      !exists(dir, "An Index"));
	CHECK(is_text(dir, ".hidden", "not Mac OS's business"));

	/* the folder as it is now, shown again */
	quiet(true);
	s = share_open(dir, 0);
	quiet(false);
	CHECK(s != NULL);
	if (s) {
		b = share_disk(s);
		struct seen seen = { .ordered = true, .want = { "Tool", "keep.txt", "withfork" } };
		CHECK(hfsread_open_with(&v, b->read, b->ctx, b->size) == 0 && hfsread_walk(&v, look, &seen) == 0);
		CHECK(seen.found[0].type == FOURCC('A', 'P', 'P', 'L') && seen.found[0].rsrc.size == 4);
		CHECK_EQ(seen.found[1].type, FOURCC('t', 't', 'r', 'o'));
		CHECK(seen.found[2].type == FOURCC('A', 'P', 'P', 'L') && seen.found[2].rsrc.size == 4);
		quiet(true);
		share_close(s);
		quiet(false);
	}
	remove_tree(dir);
}

/* Not put away (the window closed): nothing written back, the volume kept as an image */
static void test_not_put_away(void)
{
	char dir[512], rec[512], p[1024];
	if (temp_dir(dir, sizeof dir)) {
		CHECK(!"temp_dir");
		return;
	}
	put_text(dir, "a.txt", "a");
	for (int changes = 0; changes < 2; changes++) {
		quiet(true);
		struct share *s = share_open(dir, 0);
		quiet(false);
		CHECK(s != NULL);
		if (!s)
			break;
		const struct disk_backend *b = share_disk(s);
		struct hfsread_volume v;
		struct ids ids = IDS("a.txt");
		CHECK(hfsread_open_with(&v, b->read, b->ctx, b->size) == 0 && hfsread_walk(&v, find_ids, &ids) == 0);
		struct fake f[8];
		unsigned n = 0;
		entry(f, &n, strrchr(dir, '/') + 1, 0, true, 0, NULL, 0, 0, NULL);
		entry(f, &n, "a.txt", 0, false, id_of(&ids, "a.txt"), "a", TEXT, TTXT, NULL);
		entry(f, &n, "Desktop DF", 0, false, 100, "x", 0, 0, NULL);
		if (changes)
			entry(f, &n, "unsaved.txt", 0, false, 101, "work", TEXT, TTXT, NULL);
		CHECK(pretend_mac_os(b, f, n, false) == 0);
		quiet(true);
		CHECK_EQ(share_close(s), 0);
		quiet(false);
		recovery_dir(dir, rec, sizeof rec);
		CHECK(!exists(dir, "unsaved.txt") && is_text(dir, "a.txt", "a"));
		CHECK(!rec[0] == !changes);             /* an image only if there was something to lose */
	}
	/* the image has what Mac OS wrote */
	const char *name = strrchr(dir, '/') + 1;
	int fd = snprintf(p, sizeof p, "%s/%s/%s.img", dir, rec, name) < (int)sizeof p ? open(p, O_RDONLY) : -1;
	struct stat st;
	struct hfsread_volume v;
	struct seen seen = { .ordered = true, .want = { "unsaved.txt" } };
	CHECK(fd >= 0 && !fstat(fd, &st) && hfsread_open(&v, fd, 0, (uint64_t)st.st_size) == 0);
	CHECK(hfsread_walk(&v, look, &seen) == 0 && seen.found[0].data.size == 4);
	if (fd >= 0)
		close(fd);
	remove_tree(dir);
}

/* How many write-backs have kept the host's old files (the recovery folder's folders, one each) */
static unsigned recoveries(const char *dir)
{
	char path[1024];
	unsigned n = 0;
	snprintf(path, sizeof path, "%s/%s", dir, SHARE_RECOVERY);
	DIR *d = opendir(path);
	struct dirent *de;
	while (d && (de = readdir(d)))
		n += de->d_name[0] != '.';
	if (d)
		closedir(d);
	return n;
}

/* Files in the recovery folder's folders, all told */
static unsigned recovered(const char *dir)
{
	char path[1024], sub[1300];
	unsigned n = 0;
	snprintf(path, sizeof path, "%s/%s", dir, SHARE_RECOVERY);
	DIR *d = opendir(path);
	struct dirent *de;
	while (d && (de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(sub, sizeof sub, "%s/%s", path, de->d_name);
		DIR *e = opendir(sub);
		struct dirent *f;
		while (e && (f = readdir(e)))
			n += f->d_name[0] != '.';
		if (e)
			closedir(e);
	}
	if (d)
		closedir(d);
	return n;
}

/*
 * Mac OS puts the volume away during the run: what it changed is written
 * back then, the volume made again from the folder (with what the host
 * changed meanwhile) and Mac OS's next changes written back at the end.
 * Written back once, a volume isn't again (not even at the end, the host
 * having changed things since), nor one Mac OS hasn't put away.
 */
static void test_write_back_again(void)
{
	char dir[512];
	if (temp_dir(dir, sizeof dir)) {
		CHECK(!"temp_dir");
		return;
	}
	const char *root = strrchr(dir, '/') + 1;
	put_text(dir, "a.txt", "a");
	put_text(dir, "b.txt", "b");
	quiet(true);
	struct share *s = share_open(dir, 0);
	quiet(false);
	CHECK(s != NULL);
	if (!s) {
		remove_tree(dir);
		return;
	}
	char real[PATH_MAX];
	CHECK(realpath(dir, real) && !strcmp(share_folder(s), real));
	CHECK(!strcmp(share_name(s), root) && !share_put_away(s));
	const struct disk_backend *b = share_disk(s);
	struct hfsread_volume v;
	struct ids ids = IDS("a.txt", "b.txt");
	CHECK(hfsread_open_with(&v, b->read, b->ctx, b->size) == 0 && hfsread_walk(&v, find_ids, &ids) == 0);
	CHECK(!share_host_changed(s));

	/* Mac OS changes a.txt but hasn't put the volume away: nothing is written back */
	struct fake f[8];
	unsigned n = 0;
	entry(f, &n, root, 0, true, 0, NULL, 0, 0, NULL);
	entry(f, &n, "a.txt", 0, false, id_of(&ids, "a.txt"), "Mac OS's a", TEXT, TTXT, NULL);
	entry(f, &n, "b.txt", 0, false, id_of(&ids, "b.txt"), "b", TEXT, TTXT, NULL);
	CHECK(pretend_mac_os(b, f, n, false) == 0);
	CHECK(!share_put_away(s));
	quiet(true);
	CHECK_EQ(share_write_back(s), 0);
	quiet(false);
	CHECK(is_text(dir, "a.txt", "a") && !exists(dir, SHARE_RECOVERY));

	/* put away: written back now, the host's old a.txt kept */
	CHECK(pretend_mac_os(b, f, n, true) == 0);
	CHECK(share_put_away(s));
	quiet(true);
	CHECK_EQ(share_write_back(s), 0);
	quiet(false);
	CHECK(is_text(dir, "a.txt", "Mac OS's a"));
	CHECK_EQ(recovered(dir), 1);

	/* not again: the host changes a.txt meanwhile, and it stays the host's */
	put_text(dir, "a.txt", "the host's a");
	quiet(true);
	CHECK_EQ(share_write_back(s), 0);
	quiet(false);
	CHECK(is_text(dir, "a.txt", "the host's a") && !exists(dir, "a (Mac OS).txt"));
	CHECK_EQ(recovered(dir), 1);

	/* made again, from the folder as it is: the host's new c.txt, its a.txt, Mac OS's nothing yet */
	put_text(dir, "c.txt", "new on the host");
	put_text(dir, ".DS_Store", "not Mac OS's");
	CHECK(share_host_changed(s));
	quiet(true);
	CHECK_EQ(share_rebuild(s, 0), 0);
	quiet(false);
	CHECK(!share_host_changed(s) && !share_put_away(s));
	b = share_disk(s);
	CHECK(b != NULL);
	if (!b) {
		share_close(s);
		remove_tree(dir);
		return;
	}
	struct seen seen = { .ordered = true, .want = { "a.txt", "b.txt", "c.txt" } };
	CHECK(hfsread_open_with(&v, b->read, b->ctx, b->size) == 0 && hfsread_walk(&v, look, &seen) == 0);
	CHECK_EQ(seen.files, 3);
	char buf[64] = { 0 };
	CHECK(seen.found[0].data.size == 12 && hfsread_read(&v, &seen.found[0].data, 0, buf, 12) == 0 &&
	      !memcmp(buf, "the host's a", 12));
	CHECK(seen.found[2].id && seen.found[2].data.size == 15);
	CHECK(v.attributes & HFS_UNMOUNTED);
	put_text(dir, ".DS_Store", "still not Mac OS's");
	CHECK(!share_host_changed(s));                  /* (what Mac OS doesn't see doesn't count) */

	/* Mac OS's changes to the new volume go back at the end */
	struct ids ids2 = IDS("a.txt", "b.txt", "c.txt");
	CHECK(hfsread_walk(&v, find_ids, &ids2) == 0);
	n = 0;
	entry(f, &n, root, 0, true, 0, NULL, 0, 0, NULL);
	entry(f, &n, "a.txt", 0, false, id_of(&ids2, "a.txt"), "the host's a", TEXT, TTXT, NULL);
	entry(f, &n, "b.txt", 0, false, id_of(&ids2, "b.txt"), "Mac OS's b", TEXT, TTXT, NULL);
	entry(f, &n, "c.txt", 0, false, id_of(&ids2, "c.txt"), "new on the host", TEXT, TTXT, NULL);
	CHECK(pretend_mac_os(b, f, n, true) == 0);
	quiet(true);
	CHECK_EQ(share_close(s), 0);
	quiet(false);
	CHECK(is_text(dir, "b.txt", "Mac OS's b") && is_text(dir, "a.txt", "the host's a") &&
	      is_text(dir, "c.txt", "new on the host"));
	CHECK_EQ(recovered(dir), 2);
	CHECK(recoveries(dir) >= 1);

	/* written back during the run, not made again: nothing more at the end */
	quiet(true);
	s = share_open(dir, 0);
	quiet(false);
	CHECK(s != NULL);
	if (s) {
		b = share_disk(s);
		struct ids ids3 = IDS("a.txt", "b.txt", "c.txt");
		CHECK(hfsread_open_with(&v, b->read, b->ctx, b->size) == 0 && hfsread_walk(&v, find_ids, &ids3) == 0);
		n = 0;
		entry(f, &n, root, 0, true, 0, NULL, 0, 0, NULL);
		entry(f, &n, "a.txt", 0, false, id_of(&ids3, "a.txt"), "the host's a", TEXT, TTXT, NULL);
		entry(f, &n, "b.txt", 0, false, id_of(&ids3, "b.txt"), "Mac OS's b", TEXT, TTXT, NULL);
		CHECK(pretend_mac_os(b, f, n, true) == 0);              /* (c.txt deleted) */
		quiet(true);
		CHECK_EQ(share_write_back(s), 0);
		quiet(false);
		CHECK(!exists(dir, "c.txt"));
		unsigned kept = recovered(dir);
		put_text(dir, "c.txt", "the host's again");
		quiet(true);
		CHECK_EQ(share_close(s), 0);
		quiet(false);
		CHECK(is_text(dir, "c.txt", "the host's again"));
		CHECK_EQ(recovered(dir), kept);
	}

	/* the folder gone: no volume to offer again */
	quiet(true);
	s = share_open(dir, 0);
	quiet(false);
	CHECK(s != NULL);
	if (s) {
		char moved[600];
		snprintf(moved, sizeof moved, "%s-moved", dir);
		CHECK(rename(dir, moved) == 0);
		quiet(true);
		CHECK_EQ(share_rebuild(s, 0), -1);
		quiet(false);
		CHECK(share_disk(s) == NULL && !share_put_away(s) && !share_host_changed(s));
		CHECK(rename(moved, dir) == 0);
		CHECK(share_host_changed(s));                   /* (it's back) */
		quiet(true);
		CHECK_EQ(share_rebuild(s, 0), 0);
		CHECK_EQ(share_close(s), 0);
		quiet(false);
	}
	remove_tree(dir);
}

/* ---- dates ------------------------------------------------------------------------ */

static void set_mtime(const char *dir, const char *rel, time_t t)
{
	char path[1024];
	snprintf(path, sizeof path, "%s%s%s", dir, rel[0] ? "/" : "", rel);
	struct timespec ts[2] = { { t, 0 }, { t, 0 } };
	CHECK(utimensat(AT_FDCWD, path, ts, 0) == 0);
}

struct dated {
	uint32_t latest;                /* no date may be later */
	unsigned items, bad;
	uint32_t root_created, old_modified, reborn_created, reborn_modified;
};

static int check_dates(void *ctx, const struct hfsread_file *f)
{
	struct dated *d = ctx;
	d->items++;
	if (f->created > f->modified || f->modified > d->latest)
		d->bad++;
	if (f->id == 2)
		d->root_created = f->created;
	if (!strcmp(f->name, "old.txt"))
		d->old_modified = f->modified;
	if (!strcmp(f->name, "reborn.txt")) {
		d->reborn_created = f->created;
		d->reborn_modified = f->modified;
	}
	return 0;
}

/*
 * No date on the volume is later than the guest's clock (Norton FileSaver
 * took the MDB's for damage, "error 16,005", when the folder's were later),
 * and none is created after it was modified.
 */
static void test_dates(void)
{
	char dir[512];
	if (temp_dir(dir, sizeof dir)) {
		CHECK(!"temp_dir");
		return;
	}
	const time_t start = 1000000000, old = 778982400, future = time(NULL) + 30 * 86400;
	put_text(dir, "old.txt", "1994");
	set_mtime(dir, "old.txt", old);
	put_text(dir, "reborn.txt", "unpacked: born now, changed in 1994");
	set_mtime(dir, "reborn.txt", old);
#ifdef __APPLE__
	char path[1024];
	struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT, .commonattr = ATTR_CMN_CRTIME };
	struct timespec born = { time(NULL), 0 };
	snprintf(path, sizeof path, "%s/reborn.txt", dir);
	CHECK(setattrlist(path, &al, &born, sizeof born, 0) == 0);
#endif
	put_text(dir, "new.txt", "from the future");
	set_mtime(dir, "new.txt", future);
	make(dir, "sub");
	put_text(dir, "sub/in.txt", "in");
	set_mtime(dir, "sub", future);
	set_mtime(dir, "", future);

	for (unsigned pass = 0; pass < 2; pass++) {
		quiet(true);
		struct share *s = share_open(dir, pass ? 0 : start);    /* (--fixed-time, then the host's clock) */
		quiet(false);
		CHECK(s != NULL);
		if (!s)
			break;
		const struct disk_backend *b = share_disk(s);
		struct dated d = { .latest = hfs_mac_time(pass ? time(NULL) + 1 : start) };
		uint8_t mdb[512], alt[512];
		CHECK(b->read(b->ctx, mdb, 512, 1024) == 0 && b->read(b->ctx, alt, 512, b->size - 1024) == 0);
		CHECK(!memcmp(mdb, alt, 512));
		CHECK(be32(mdb + 2) <= be32(mdb + 6) && be32(mdb + 6) <= d.latest);       /* drCrDate, drLsMod */
		struct hfsread_volume v;
		CHECK(hfsread_open_with(&v, b->read, b->ctx, b->size) == 0 && hfsread_walk(&v, check_dates, &d) == 0);
		CHECK_EQ(d.items, 6);
		CHECK_EQ(d.bad, 0);
		CHECK_EQ(d.root_created, be32(mdb + 2));
		CHECK_EQ(d.old_modified, hfs_mac_time(old));                           /* (earlier ones stay) */
		CHECK(d.reborn_created == hfs_mac_time(old) && d.reborn_modified == hfs_mac_time(old));
		quiet(true);
		CHECK_EQ(share_close(s), 0);
		quiet(false);
	}
	remove_tree(dir);
}

void test_share(void)
{
	test_names();
	test_volume();
	test_big_files();
	test_dates();
	test_write_back();
	test_not_put_away();
	test_write_back_again();
}
