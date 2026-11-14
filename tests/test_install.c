/*
 * Installing the guest tools: the check of the startup disk (on small HFS+
 * volumes made here: tools missing, current, differing, disabled, in a
 * localized Extensions folder, a locked volume), when they're offered, and
 * the installer driven as Mac OS would drive it, against a small File
 * Manager of the test's own.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/be.h"
#include "../src/install.h"
#include "test.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* ---- volumes ------------------------------------------------------------------- */

#define BLOCK 512u
#define BLOCKS 256u
#define NODE 4096u
#define CATALOG_BLOCK 16u               /* the catalog: a header node and a leaf */
#define DATA_BLOCK 40u                  /* files' forks from here */
#define VOLUME_BYTES (BLOCKS * BLOCK)
#define ROOT 2u
#define SYSTEM 20u
#define EXTENSIONS 21u
#define DISABLED 22u

struct item {
	uint32_t parent, id;
	const char *name;
	bool folder;
	uint32_t type, creator;
	const uint8_t *data, *rsrc;
	uint32_t data_len, rsrc_len;
};

static struct item folder(uint32_t parent, uint32_t id, const char *name)
{
	return (struct item){ .parent = parent, .id = id, .name = name, .folder = true };
}

static size_t put_key(uint8_t *p, uint32_t parent, const char *name)
{
	size_t n = strlen(name);
	put_be16(p, (uint16_t)(6 + 2 * n));
	put_be32(p + 2, parent);
	put_be16(p + 6, (uint16_t)n);
	for (size_t k = 0; k < n; k++)
		put_be16(p + 8 + 2 * k, (uint8_t)name[k]);
	return 8 + 2 * n;
}

static void put_fork(uint8_t *p, uint32_t len, uint32_t *next, uint8_t *vol, const uint8_t *data)
{
	uint32_t blocks = (len + BLOCK - 1) / BLOCK;
	put_be64(p, len);
	put_be32(p + 12, blocks);
	put_be32(p + 16, *next);
	put_be32(p + 20, blocks);
	memcpy(vol + (size_t)*next * BLOCK, data, len);
	*next += blocks;
}

/* A catalog record: an item's, or a folder's thread (keyed by the folder's ID and no name) */
struct record {
	uint32_t parent;
	const char *name;
	const struct item *item;
	bool thread;
};

static int compare_records(const void *x, const void *y)
{
	const struct record *a = x, *b = y;
	return a->parent != b->parent ? (a->parent < b->parent ? -1 : 1) : strcmp(a->name, b->name);
}

/* A volume "Test" whose blessed System Folder (SYSTEM) holds the `items`; `attributes` for its header */
static uint8_t *make_volume(const struct item *items, unsigned n, uint32_t attributes)
{
	const struct item root = folder(1, ROOT, "Test"), system = folder(ROOT, SYSTEM, "System Folder");
	uint8_t *vol = calloc(1, VOLUME_BYTES), *leaf = vol + CATALOG_BLOCK * BLOCK + NODE;
	struct record r[64];
	unsigned count = 0;
	for (unsigned k = 0; k < n + 2; k++) {
		const struct item *it = k == 0 ? &root : k == 1 ? &system : &items[k - 2];
		r[count++] = (struct record){ it->parent, it->name, it, false };
		if (it->folder)
			r[count++] = (struct record){ it->id, "", it, true };
	}
	qsort(r, count, sizeof r[0], compare_records);

	uint32_t next = DATA_BLOCK;
	unsigned at = 14;
	for (unsigned k = 0; k < count; k++) {
		const struct item *it = r[k].item;
		put_be16(leaf + NODE - 2 * (k + 1), (uint16_t)at);
		at += (unsigned)put_key(leaf + at, r[k].parent, r[k].name);
		uint8_t *d = leaf + at;
		if (r[k].thread) {
			size_t len = strlen(it->name);
			put_be16(d, 3);
			put_be32(d + 4, it->parent);
			put_be16(d + 8, (uint16_t)len);
			for (size_t c = 0; c < len; c++)
				put_be16(d + 10 + 2 * c, (uint8_t)it->name[c]);
			at += (unsigned)(10 + 2 * len);
		} else if (it->folder) {
			put_be16(d, 1);
			put_be32(d + 8, it->id);
			at += 88;
		} else {
			put_be16(d, 2);
			put_be32(d + 8, it->id);
			put_be32(d + 48, it->type);
			put_be32(d + 52, it->creator);
			put_fork(d + 88, it->data_len, &next, vol, it->data);
			put_fork(d + 168, it->rsrc_len, &next, vol, it->rsrc);
			at += 248;
		}
	}
	put_be16(leaf + NODE - 2 * (count + 1), (uint16_t)at);
	CHECK(at + 2 * (count + 1) <= NODE && next <= BLOCKS);
	leaf[8] = 0xFF;
	leaf[9] = 1;
	put_be16(leaf + 10, (uint16_t)count);

	uint8_t *header = vol + CATALOG_BLOCK * BLOCK, *hr = header + 14;
	header[8] = 1;
	put_be16(header + 10, 3);
	put_be16(hr, 1);                        /* depth */
	put_be32(hr + 2, 1);                    /* root: the leaf */
	put_be32(hr + 6, count);
	put_be32(hr + 10, 1);
	put_be32(hr + 14, 1);
	put_be16(hr + 18, NODE);
	put_be16(hr + 20, 516);
	put_be32(hr + 22, 2);
	put_be32(hr + 38, 6);                   /* big keys, variable index keys */

	uint8_t *h = vol + 1024;
	put_be16(h, 0x482B);
	put_be16(h + 2, 4);
	put_be32(h + 4, attributes);
	put_be32(h + 40, BLOCK);
	put_be32(h + 44, BLOCKS);
	put_be32(h + 80, SYSTEM);
	put_be64(h + 272, 2 * NODE);
	put_be32(h + 272 + 12, 2 * NODE / BLOCK);
	put_be32(h + 272 + 16, CATALOG_BLOCK);
	put_be32(h + 272 + 20, 2 * NODE / BLOCK);
	return vol;
}

static int temp_file(char *path, size_t len)
{
	const char *dir = getenv("TMPDIR");
	snprintf(path, len, "%s/syshm-test-XXXXXX", dir ? dir : "/tmp");
	return mkstemp(path);
}

static char volume_path[256];

static void write_volume(const struct item *items, unsigned n, uint32_t attributes)
{
	uint8_t *vol = make_volume(items, n, attributes);
	int fd = open(volume_path, O_WRONLY | O_TRUNC);
	CHECK(fd >= 0 && pwrite(fd, vol, VOLUME_BYTES, 0) == (ssize_t)VOLUME_BYTES);
	if (fd >= 0)
		close(fd);
	free(vol);
}

static struct hfs_file tools[TOOLS_FILES];
static uint8_t older[4096];                     /* an older Enhancements: the same, but for a byte */

static struct item tool_item(unsigned t, uint32_t folder, uint32_t id, const char *name, bool old)
{
	const struct hfs_file *f = &tools[t];
	if (old) {
		memcpy(older, f->data, f->data_len);
		older[f->data_len - 1] ^= 0x55;
	}
	return (struct item){ folder, id, name ? name : f->name, false, f->type, f->creator, old ? older : f->data,
			      f->rsrc, f->data_len, f->rsrc_len };
}

static int check(const struct item *items, unsigned n, struct install_check *ck)
{
	char err[512];
	write_volume(items, n, 0);
	return install_check(volume_path, 0, ck, err, sizeof err);
}

static void test_check(void)
{
	static const uint8_t pef[24] = "Joy!peffpwpc\0\0\0\1";
	struct install_check ck;
	struct item folders[] = {
		folder(SYSTEM, EXTENSIONS, "Extensions"),
		folder(SYSTEM, DISABLED, "Extensions (Disabled)"),
	};

	/* missing: neither is there (another 3D engine is, and a file in the System Folder itself doesn't count) */
	struct item none[] = { folders[0], folders[1],
			       { EXTENSIONS, 40, "ATI 3D Accelerator", false, FOURCC('s', 'h', 'l', 'b'),
				 FOURCC('t', 'n', 's', 'l'), pef, pef, sizeof pef, sizeof pef },
			       tool_item(TOOLS_ENHANCEMENTS, SYSTEM, 41, NULL, false) };
	CHECK_EQ(check(none, 4, &ck), 0);
	CHECK(strcmp(ck.volume, "Test") == 0 && strcmp(ck.system, "Test:System Folder") == 0);
	CHECK_EQ(ck.system_dir, SYSTEM);
	CHECK(!ck.locked);
	CHECK_EQ(ck.tool[TOOLS_ENHANCEMENTS].state, INSTALL_MISSING);
	CHECK_EQ(ck.tool[TOOLS_3D_ENGINE].state, INSTALL_MISSING);
	CHECK(install_needed(&ck));

	/* current, under other names */
	struct item both[] = { folders[0], folders[1], tool_item(TOOLS_ENHANCEMENTS, EXTENSIONS, 50, "Helper", false),
			       tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 51, "3D", false) };
	CHECK_EQ(check(both, 4, &ck), 0);
	CHECK_EQ(ck.tool[TOOLS_ENHANCEMENTS].state, INSTALL_CURRENT);
	CHECK_EQ(ck.tool[TOOLS_ENHANCEMENTS].dir, EXTENSIONS);
	CHECK_EQ(ck.tool[TOOLS_ENHANCEMENTS].id, 50);
	CHECK(strcmp(ck.tool[TOOLS_ENHANCEMENTS].folder, "Extensions") == 0);
	CHECK_EQ(ck.tool[TOOLS_3D_ENGINE].state, INSTALL_CURRENT);
	CHECK_EQ(ck.tool[TOOLS_3D_ENGINE].id, 51);
	CHECK(!install_needed(&ck));

	/* differing: an older Enhancements; the engine disabled, a differing copy elsewhere too */
	struct item old[] = { folders[0], folders[1], tool_item(TOOLS_ENHANCEMENTS, EXTENSIONS, 52, NULL, true),
			      tool_item(TOOLS_3D_ENGINE, DISABLED, 53, NULL, false) };
	CHECK_EQ(check(old, 4, &ck), 0);
	CHECK_EQ(ck.tool[TOOLS_ENHANCEMENTS].state, INSTALL_DIFFERS);
	CHECK_EQ(ck.tool[TOOLS_ENHANCEMENTS].id, 52);
	CHECK_EQ(ck.tool[TOOLS_3D_ENGINE].state, INSTALL_CURRENT);
	CHECK_EQ(ck.tool[TOOLS_3D_ENGINE].dir, DISABLED);
	CHECK(strcmp(ck.tool[TOOLS_3D_ENGINE].folder, "Extensions (Disabled)") == 0);
	CHECK(install_needed(&ck));

	/* a current copy wins over a differing one, wherever they are */
	struct item two[] = { folders[0], folders[1], tool_item(TOOLS_ENHANCEMENTS, EXTENSIONS, 54, NULL, true),
			      tool_item(TOOLS_ENHANCEMENTS, DISABLED, 55, NULL, false) };
	CHECK_EQ(check(two, 4, &ck), 0);
	CHECK_EQ(ck.tool[TOOLS_ENHANCEMENTS].state, INSTALL_CURRENT);
	CHECK_EQ(ck.tool[TOOLS_ENHANCEMENTS].id, 55);

	/* a localized Extensions folder */
	struct item german[] = { folder(SYSTEM, EXTENSIONS, "Systemerweiterungen"),
				 tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 56, NULL, true) };
	CHECK_EQ(check(german, 2, &ck), 0);
	CHECK_EQ(ck.tool[TOOLS_3D_ENGINE].state, INSTALL_DIFFERS);
	CHECK(strcmp(ck.tool[TOOLS_3D_ENGINE].folder, "Systemerweiterungen") == 0);
	CHECK_EQ(ck.tool[TOOLS_ENHANCEMENTS].state, INSTALL_MISSING);

	/* locked (by software) */
	char err[256];
	write_volume(none, 4, 0x8000);
	CHECK_EQ(install_check(volume_path, 0, &ck, err, sizeof err), 0);
	CHECK(ck.locked);

	/* nothing blessed */
	uint8_t *vol = make_volume(none, 4, 0);
	put_be32(vol + 1024 + 80, 0);
	int fd = open(volume_path, O_WRONLY | O_TRUNC);
	CHECK(fd >= 0 && pwrite(fd, vol, VOLUME_BYTES, 0) == (ssize_t)VOLUME_BYTES);
	close(fd);
	free(vol);
	CHECK(install_check(volume_path, 0, &ck, err, sizeof err) != 0);
}

/* When the tools are offered: read-only disks (CDs), locked volumes and current tools never are */
static void test_offer(void)
{
	static struct install in;
	struct item folders[] = { folder(SYSTEM, EXTENSIONS, "Extensions") };
	write_volume(folders, 1, 0);
	install_begin(&in, volume_path, 0, true);
	CHECK_EQ(in.phase, INSTALL_OFF);
	install_begin(&in, volume_path, 0, false);
	CHECK_EQ(in.phase, INSTALL_WAIT_FINDER);
	CHECK(strstr(in.question, "aren't installed on \"Test\"") != NULL);
	CHECK(strcmp(in.yes, "Install") == 0);
	write_volume(folders, 1, 0x0080);
	install_begin(&in, volume_path, 0, false);
	CHECK_EQ(in.phase, INSTALL_OFF);

	struct item both[] = { folders[0], tool_item(TOOLS_ENHANCEMENTS, EXTENSIONS, 50, NULL, false),
			       tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 51, NULL, false) };
	write_volume(both, 3, 0);
	install_begin(&in, volume_path, 0, false);
	CHECK_EQ(in.phase, INSTALL_OFF);
	both[2] = tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 51, NULL, true);
	write_volume(both, 3, 0);
	install_begin(&in, volume_path, 0, false);
	CHECK_EQ(in.phase, INSTALL_WAIT_FINDER);
	CHECK(strcmp(in.yes, "Update") == 0);
	CHECK(strstr(in.question, "syshm 3D Engine is from another version of syshm") != NULL);
}

/* ---- the installer, as Mac OS runs it ----------------------------------------------- */

#define HZ 16600000u
#define TOC 0x10000u
#define FRAME 0x20000u
#define TVECTORS 0x40000u
#define TRAP_ENTRY 0x1DDCu              /* _SynchIdleTime's in the Toolbox trap table */
#define ROUTINE 0x00123456u
#define VREF 0xFFFFu                    /* (-1) */
#define DRIVE 8u

/* Its imports, in order */
enum { FIND_FOLDER, HGET_VINFO, GET_CAT_INFO, DELETE, CREATE, OPEN_DF, OPEN_RF, WRITE, CLOSE, SET_FINFO, RENAME, LAUNCH };

static struct mem mem;

/* A File Manager over a few files */
struct file {
	bool used;
	uint32_t dir, id;
	char name[64];
	uint32_t type, creator;
	uint8_t fork[2][4096];
	uint32_t len[2];
	bool locked;
};

static struct fs {
	struct file f[8];
	uint32_t next_id;
	bool volume_locked, full;
	unsigned launches;
	char launched[64];
	uint32_t launched_dir;
	unsigned opened;                /* forks open */
} fs;

static void pname(uint32_t p, char *out)
{
	uint8_t n = mem_r8(&mem, p);
	memcpy(out, mem_ptr(&mem, p + 1), n);
	out[n] = '\0';
}

static struct file *find(uint32_t dir, const char *name)
{
	for (unsigned k = 0; k < 8; k++)
		if (fs.f[k].used && fs.f[k].dir == dir && strcmp(fs.f[k].name, name) == 0)
			return &fs.f[k];
	return NULL;
}

static struct file *by_spec(uint32_t spec)
{
	char name[64];
	pname(spec + 6, name);
	return find(mem_r32(&mem, spec + 2), name);
}

static struct file *add_file(uint32_t dir, const char *name, uint32_t id, const struct hfs_file *f)
{
	for (unsigned k = 0; k < 8; k++) {
		struct file *x = &fs.f[k];
		if (x->used)
			continue;
		memset(x, 0, sizeof *x);
		x->used = true;
		x->dir = dir;
		x->id = id;
		snprintf(x->name, sizeof x->name, "%s", name);
		if (f) {
			x->type = f->type;
			x->creator = f->creator;
			memcpy(x->fork[0], f->data, f->data_len);
			memcpy(x->fork[1], f->rsrc, f->rsrc_len);
			x->len[0] = f->data_len;
			x->len[1] = f->rsrc_len;
		}
		return x;
	}
	return NULL;
}

static int16_t file_manager(unsigned call, const uint32_t *a)
{
	struct file *f;
	char name[64];
	uint32_t pb = a[0];
	switch (call) {
	case FIND_FOLDER:
		CHECK_EQ(a[0], 0xFFFF8000u);
		CHECK_EQ(a[1], FOURCC('e', 'x', 't', 'n'));
		mem_w16(&mem, a[3], VREF);
		mem_w32(&mem, a[4], EXTENSIONS);
		return 0;
	case HGET_VINFO:
		CHECK_EQ(mem_r16(&mem, pb + 22), VREF);
		mem_w16(&mem, pb + 38, fs.volume_locked ? 0x8000 : 0);
		mem_w16(&mem, pb + 66, DRIVE);
		mem_w32(&mem, pb + 90, SYSTEM);
		return 0;
	case GET_CAT_INFO: {
		int16_t index = (int16_t)mem_r16(&mem, pb + 28);
		uint32_t dir = mem_r32(&mem, pb + 48), name_ptr = mem_r32(&mem, pb + 18);
		if (index < 0) {
			const char *folder = dir == EXTENSIONS ? "Extensions" : "Extensions (Disabled)";
			mem_w8(&mem, name_ptr, (uint8_t)strlen(folder));
			memcpy(mem_ptr(&mem, name_ptr + 1), folder, strlen(folder));
			return 0;
		}
		for (unsigned k = 0, n = 0; k < 8; k++) {
			f = &fs.f[k];
			if (!f->used || f->dir != dir || ++n != (unsigned)index)
				continue;
			mem_w8(&mem, name_ptr, (uint8_t)strlen(f->name));
			memcpy(mem_ptr(&mem, name_ptr + 1), f->name, strlen(f->name));
			mem_w8(&mem, pb + 30, 0);
			mem_w32(&mem, pb + 32, f->type);
			mem_w32(&mem, pb + 48, f->id);
			return 0;
		}
		return -43;
	}
	case DELETE:
		if (!(f = by_spec(a[0])))
			return -43;
		if (f->locked)
			return -45;
		f->used = false;
		return 0;
	case CREATE:
		pname(a[0] + 6, name);
		if (find(mem_r32(&mem, a[0] + 2), name))
			return -48;
		f = add_file(mem_r32(&mem, a[0] + 2), name, fs.next_id++, NULL);
		f->creator = a[1];
		f->type = a[2];
		return 0;
	case OPEN_DF:
	case OPEN_RF:
		if (!(f = by_spec(a[0])))
			return -43;
		f->len[call == OPEN_RF] = 0;
		mem_w16(&mem, a[2], (uint16_t)(1 + 2 * (unsigned)(f - fs.f) + (call == OPEN_RF)));
		fs.opened++;
		return 0;
	case WRITE: {
		f = &fs.f[(a[0] - 1) / 2];
		unsigned fork = (a[0] - 1) % 2;
		uint32_t n = mem_r32(&mem, a[1]);
		if (fs.full)
			return -34;
		memcpy(f->fork[fork] + f->len[fork], mem_ptr(&mem, a[2]), n);
		f->len[fork] += n;
		return 0;
	}
	case CLOSE:
		fs.opened--;
		return 0;
	case SET_FINFO:
		if (!(f = by_spec(a[0])))
			return -43;
		f->type = mem_r32(&mem, a[1]);
		f->creator = mem_r32(&mem, a[1] + 4);
		return 0;
	case RENAME:
		if (!(f = by_spec(a[0])))
			return -43;
		pname(a[1], name);
		if (find(f->dir, name))
			return -48;
		snprintf(f->name, sizeof f->name, "%s", name);
		return 0;
	case LAUNCH:
		CHECK_EQ(mem_r16(&mem, pb + 6), 0x4C43);
		CHECK_EQ(mem_r16(&mem, pb + 14) & 0x4000, 0x4000);     /* launchContinue */
		fs.launches++;
		pname(mem_r32(&mem, pb + 16) + 6, fs.launched);
		fs.launched_dir = mem_r32(&mem, mem_r32(&mem, pb + 16) + 2);
		return 0;
	default:
		CHECK(!"an import");
		return -50;
	}
}

/* Idle calls with the Finder current and nothing on the disks, until the installer is diverted to */
static bool idle_until_diverted(struct install *in, struct helper *h, uint64_t *tb)
{
	for (unsigned k = 0; k < 200; k++) {
		struct install_idle w = { &mem, h, true, *tb, HZ, 1000, DRIVE };
		*tb += HZ / 20;
		if (install_idle(in, &w))
			return true;
	}
	return false;
}

/* The bootstrap's work, then the installer's main routine and its callouts until it returns */
static void run_installer(struct install *in, struct helper *h)
{
	for (unsigned k = 0; k < 12; k++)
		mem_w32(&mem, TOC + in->layout.import[k], TVECTORS + 8 * k);
	struct ndrv_call c = { .mem = &mem, .entry = 0, .toc = TOC, .frame = FRAME };
	install_call(in, &c, h);
	for (unsigned steps = 0; !c.done && steps < 1000; steps++) {
		CHECK(c.tvector >= TVECTORS && c.tvector < TVECTORS + 8 * 12 && c.tvector % 8 == 0);
		int16_t err = file_manager((c.tvector - TVECTORS) / 8, c.out);
		c = (struct ndrv_call){ .mem = &mem, .entry = NDRV_ENTRY_RESUME, .toc = TOC, .frame = FRAME };
		c.arg[0] = (uint32_t)(int32_t)err;
		install_call(in, &c, h);
	}
	CHECK(c.done);
}

static bool same_as(const struct file *f, unsigned t)
{
	return f && f->type == tools[t].type && f->creator == tools[t].creator && f->len[0] == tools[t].data_len &&
	       f->len[1] == tools[t].rsrc_len && !memcmp(f->fork[0], tools[t].data, tools[t].data_len) &&
	       !memcmp(f->fork[1], tools[t].rsrc, tools[t].rsrc_len);
}

/* Enhancements missing, an older engine in Extensions (Disabled): installed, updated where it is, launched */
static void test_installer(void)
{
	static struct install in;
	struct helper h = { .running = false };
	struct item items[] = { folder(SYSTEM, EXTENSIONS, "Extensions"), folder(SYSTEM, DISABLED, "Extensions (Disabled)"),
				tool_item(TOOLS_3D_ENGINE, DISABLED, 60, "Old 3D", true) };
	write_volume(items, 3, 0);
	memset(&fs, 0, sizeof fs);
	fs.next_id = 100;
	add_file(DISABLED, "Old 3D", 60, &tools[TOOLS_3D_ENGINE])->fork[0][0] ^= 1;
	install_begin(&in, volume_path, 0, false);
	CHECK_EQ(in.phase, INSTALL_WAIT_FINDER);

	/* asked once the Finder is up; the answer diverts the trap to the bootstrap */
	uint64_t tb = HZ;
	mem_w32(&mem, TRAP_ENTRY, ROUTINE);
	struct install_idle w = { &mem, &h, false, tb, HZ, 1000, DRIVE };
	CHECK(!install_idle(&in, &w));                  /* (not the Finder) */
	CHECK(!in.ask);
	CHECK(!idle_until_diverted(&in, &h, &tb));
	CHECK_EQ(in.phase, INSTALL_ASKING);
	CHECK(in.ask);
	install_answer(&in, true, &h);
	CHECK_EQ(in.phase, INSTALL_INJECT);
	CHECK(idle_until_diverted(&in, &h, &tb));
	CHECK_EQ(in.phase, INSTALL_INJECTED);
	CHECK_EQ(mem_r32(&mem, TRAP_ENTRY), INSTALL_REGION);
	CHECK_EQ(mem_r32(&mem, INSTALL_REGION), 0x48E7FFFEu);  /* movem.l d0-d7/a0-a6,-(sp) */
	CHECK_EQ(mem_r16(&mem, INSTALL_REGION + 4), 0x23FC);    /* move.l #ROUTINE,TRAP_ENTRY */
	CHECK_EQ(mem_r32(&mem, INSTALL_REGION + 6), ROUTINE);
	CHECK_EQ(mem_r32(&mem, INSTALL_REGION + 10), TRAP_ENTRY);
	bool jumps = false;                             /* ... and in the end, jmp ROUTINE */
	for (uint32_t a = INSTALL_REGION; a < INSTALL_REGION + 0x100; a += 2)
		jumps |= mem_r16(&mem, a) == 0x4EF9 && mem_r32(&mem, a + 2) == ROUTINE;
	CHECK(jumps);
	CHECK_EQ(mem_r16(&mem, INSTALL_REGION + 0x100), 0xAAFE);       /* main's routine descriptor */

	run_installer(&in, &h);
	CHECK_EQ(in.phase, INSTALL_DONE);
	CHECK(in.news_ready);
	CHECK(same_as(find(EXTENSIONS, "syshm Enhancements"), TOOLS_ENHANCEMENTS));
	CHECK(same_as(find(DISABLED, "Old 3D"), TOOLS_3D_ENGINE));     /* (still disabled, still its name) */
	CHECK(!find(DISABLED, "syshm installing") && !find(EXTENSIONS, "syshm installing"));
	CHECK_EQ(fs.opened, 0);
	CHECK_EQ(fs.launches, 1);
	CHECK(strcmp(fs.launched, "syshm Enhancements") == 0 && fs.launched_dir == EXTENSIONS);
	CHECK(strstr(in.news, "installed syshm Enhancements in \"Test:System Folder:Extensions\" and updated "
			      "syshm 3D Engine (\"Old 3D\") in \"Test:System Folder:Extensions (Disabled)\".") == in.news);
	CHECK(strstr(in.news, "syshm Enhancements is running.") != NULL);
}

/* The disk fills up while the engine is written: what was done stays, the temporary file goes */
static void test_installer_errors(void)
{
	static struct install in;
	struct helper h = { .running = false };
	struct item items[] = { folder(SYSTEM, EXTENSIONS, "Extensions"),
				tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 61, NULL, true) };
	write_volume(items, 2, 0);
	memset(&fs, 0, sizeof fs);
	fs.next_id = 100;
	struct file *old = add_file(EXTENSIONS, "syshm 3D Engine", 61, &tools[TOOLS_3D_ENGINE]);
	old->fork[0][0] ^= 1;
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);                  /* (a script's, before the question) */
	uint64_t tb = HZ;
	mem_w32(&mem, TRAP_ENTRY, ROUTINE);
	CHECK(idle_until_diverted(&in, &h, &tb));
	for (unsigned k = 0; k < 12; k++)
		mem_w32(&mem, TOC + in.layout.import[k], TVECTORS + 8 * k);

	/* (run it by hand to fill the disk after Enhancements) */
	struct ndrv_call c = { .mem = &mem, .entry = 0, .toc = TOC, .frame = FRAME };
	install_call(&in, &c, &h);
	for (unsigned steps = 0; !c.done && steps < 1000; steps++) {
		unsigned call = (c.tvector - TVECTORS) / 8;
		fs.full = find(EXTENSIONS, "syshm Enhancements") != NULL;
		int16_t err = file_manager(call, c.out);
		c = (struct ndrv_call){ .mem = &mem, .entry = NDRV_ENTRY_RESUME, .toc = TOC, .frame = FRAME };
		c.arg[0] = (uint32_t)(int32_t)err;
		install_call(&in, &c, &h);
	}
	CHECK(c.done);
	CHECK(same_as(find(EXTENSIONS, "syshm Enhancements"), TOOLS_ENHANCEMENTS));
	CHECK(old->used && old->fork[0][0] != tools[TOOLS_3D_ENGINE].data[0]);  /* the old engine, as it was */
	CHECK(!find(EXTENSIONS, "syshm installing"));
	CHECK_EQ(fs.opened, 0);
	CHECK(strstr(in.news, "installed syshm Enhancements in \"Test:System Folder:Extensions\"; couldn't update "
			      "syshm 3D Engine (writing it): the disk is full.") == in.news);

	/* a locked volume: nothing done */
	fs.full = false;
	write_volume(items, 2, 0);
	memset(&fs, 0, sizeof fs);
	fs.volume_locked = true;
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	CHECK(idle_until_diverted(&in, &h, &tb));
	run_installer(&in, &h);
	CHECK(!find(EXTENSIONS, "syshm Enhancements") && !find(EXTENSIONS, "syshm installing"));
	CHECK(strstr(in.news, "couldn't install the guest tools (looking at the startup disk): the disk is locked.") ==
	      in.news);
	CHECK_EQ(fs.launches, 0);
}

/* A running Enhancements that's to be replaced quits first; if it can't be replaced after all, it starts again */
static void test_installer_quit(void)
{
	static struct install in;
	struct helper h = { .running = true };
	struct item items[] = { folder(SYSTEM, EXTENSIONS, "Extensions"),
				tool_item(TOOLS_ENHANCEMENTS, EXTENSIONS, 62, "syshm Helper", true),
				tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 63, NULL, false) };
	write_volume(items, 3, 0);
	memset(&fs, 0, sizeof fs);
	fs.next_id = 100;
	add_file(EXTENSIONS, "syshm Helper", 62, &tools[TOOLS_ENHANCEMENTS])->locked = true;
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	struct install_idle w = { &mem, &h, true, HZ, HZ, 1000, DRIVE };
	for (unsigned k = 0; k < 40 && in.phase != INSTALL_QUITTING; k++, w.tb += HZ / 20)
		CHECK(!install_idle(&in, &w));
	CHECK_EQ(in.phase, INSTALL_QUITTING);
	CHECK(h.quit);
	for (unsigned k = 0; k < 20; k++, w.tb += HZ / 20)     /* (it takes its time) */
		CHECK(!install_idle(&in, &w));
	h.quit = h.running = false;                     /* it has quit */
	CHECK(!install_idle(&in, &w));
	w.tb += HZ / 5;
	CHECK(install_idle(&in, &w));                   /* soon after, the installer */
	run_installer(&in, &h);
	CHECK(strstr(in.news, "couldn't update syshm Enhancements (replacing the old copy): the file is locked. "
			      "The old syshm Enhancements is running again.") == in.news);
	CHECK(strcmp(fs.launched, "syshm Helper") == 0);
	CHECK(!find(EXTENSIONS, "syshm installing"));

	/* one that doesn't quit: syshm goes ahead after a while */
	h.running = true;
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	uint64_t tb = HZ;
	CHECK(idle_until_diverted(&in, &h, &tb));
	CHECK(tb > 6 * (uint64_t)HZ);

	/* not now */
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, false, &h);
	CHECK(!idle_until_diverted(&in, &h, &tb));
	CHECK_EQ(in.phase, INSTALL_DONE);
}

void test_install(void)
{
	int fd = temp_file(volume_path, sizeof volume_path);
	CHECK(fd >= 0);
	if (fd < 0 || tools_files(tools)) {
		CHECK(!"set up");
		return;
	}
	close(fd);
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		unlink(volume_path);
		return;
	}
	mem_map(&mem, 0, 0x100000, MEM_RW);
	fflush(stdout);                         /* (what install.c says goes nowhere) */
	int out = dup(1), null = open("/dev/null", O_WRONLY);
	if (null >= 0)
		dup2(null, 1);
	test_check();
	test_offer();
	test_installer();
	test_installer_errors();
	test_installer_quit();
	fflush(stdout);
	if (out >= 0)
		dup2(out, 1);
	if (null >= 0)
		close(null);
	if (out >= 0)
		close(out);
	mem_free(&mem);
	unlink(volume_path);
}
