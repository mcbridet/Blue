/*
 * Installing the guest tools: the check of the startup disk (on small HFS
 * volumes made with hfs.c: tools missing, current, differing, disabled, in
 * a localized Extensions folder, a locked volume; Apple's OpenGL libraries
 * enabled or disabled), when they're offered, and the installer driven as
 * Mac OS would drive it, against a small File Manager of the test's own
 * (Apple's OpenGL libraries moved to Extensions (Disabled) and back), and
 * the restart it asks the Process Manager for once all is in place (or not).
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/be.h"
#include "../src/hfs.h"
#include "../src/install.h"
#include "test.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* ---- volumes ------------------------------------------------------------------- */

#define VOLUME_BYTES (1u << 20)
#define SYSTEM 20u
#define EXTENSIONS 21u
#define DISABLED 22u
#define MDB_ATRB (1024u + 10)
#define MDB_BLESSED (1024u + 92)          /* (Finder info word 0) */

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

static char volume_path[256];

/* A volume "Test" whose blessed System Folder (SYSTEM) holds the `items` (folders before what's in them) */
static void write_volume(const struct item *items, unsigned n, uint16_t attributes)
{
	struct hfs_entry e[24] = { { .name = "\x04Test", .folder = true },
				   { .name = "\x0DSystem Folder", .folder = true, .id = SYSTEM } };
	CHECK(n + 2 <= 24);
	for (unsigned k = 0; k < n && k + 2 < 24; k++) {
		const struct item *it = &items[k];
		struct hfs_entry *x = &e[k + 2];
		x->name[0] = (uint8_t)strlen(it->name);
		memcpy(x->name + 1, it->name, x->name[0]);
		x->parent = it->parent == SYSTEM ? 1 : 0;
		for (unsigned j = 0; j < k; j++)
			if (items[j].folder && items[j].id == it->parent)
				x->parent = j + 2;
		x->folder = it->folder;
		x->id = it->id;
		put_be32(x->finder, it->type);
		put_be32(x->finder + 4, it->creator);
		x->data_len = it->data_len;
		x->rsrc_len = it->rsrc_len;
	}
	struct hfs_layout l;
	CHECK_EQ(hfs_build(&l, e, n + 2, VOLUME_BYTES, 0, 3000000000u), 0);
	put_be32(l.meta + MDB_BLESSED, SYSTEM);
	put_be16(l.meta + MDB_ATRB, be16(l.meta + MDB_ATRB) | attributes);
	int fd = open(volume_path, O_WRONLY | O_TRUNC);
	CHECK(fd >= 0 && ftruncate(fd, VOLUME_BYTES) == 0 && pwrite(fd, l.meta, l.meta_len, 0) == (ssize_t)l.meta_len);
	for (unsigned k = 0; k < n && fd >= 0; k++) {
		const struct hfs_entry *x = &e[k + 2];
		if (x->data_len)
			CHECK(pwrite(fd, items[k].data, x->data_len, (off_t)(l.first_block + (uint64_t)x->data_start *
										    l.block_size)) == (ssize_t)x->data_len);
		if (x->rsrc_len)
			CHECK(pwrite(fd, items[k].rsrc, x->rsrc_len, (off_t)(l.first_block + (uint64_t)x->rsrc_start *
										    l.block_size)) == (ssize_t)x->rsrc_len);
	}
	if (fd >= 0)
		close(fd);
	hfs_layout_free(&l);
}

static int temp_file(char *path, size_t len)
{
	const char *dir = getenv("TMPDIR");
	snprintf(path, len, "%s/blue-test-XXXXXX", dir ? dir : "/tmp");
	return mkstemp(path);
}

static struct hfs_file tools[TOOLS_FILES];
static uint8_t older[4096];                     /* an older Enhancements: the same, but for a byte */
static const uint8_t apple_pef[24] = "Joy!peffpwpc\0\0\0\1";

/* Blue's libraries (OpenGL's and the Pippin's), current, in a folder: appended to items (returns how many there are
 * then) */
static unsigned with_libs(struct item *items, unsigned n, uint32_t folder, uint32_t first_id)
{
	for (unsigned t = TOOLS_GL_FIRST; t <= TOOLS_PIPPIN; t++) {
		const struct hfs_file *f = &tools[t];
		items[n++] = (struct item){ folder, first_id + t, f->name, false, f->type, f->creator, f->data, f->rsrc,
					    f->data_len, f->rsrc_len };
	}
	return n;
}

/* Apple's OpenGL libraries in a folder: appended to items */
static unsigned with_apple(struct item *items, unsigned n, uint32_t folder, uint32_t first_id)
{
	static const char *const names[3] = { "OpenGLLibrary", "OpenGLMemory", "OpenGLUtility" };
	for (unsigned k = 0; k < 3; k++)
		items[n++] = (struct item){ folder, first_id + k, names[k], false, FOURCC('s', 'h', 'l', 'b'),
					    FOURCC('g', 'l', 'l', 'b'), apple_pef, apple_pef, sizeof apple_pef,
					    sizeof apple_pef };
	return n;
}

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

	CHECK_EQ(ck.tool[TOOLS_GL_LIBRARY].state, INSTALL_MISSING);

	/* current, under other names */
	struct item both[12] = { folders[0], folders[1], tool_item(TOOLS_ENHANCEMENTS, EXTENSIONS, 50, "Helper", false),
				 tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 51, "3D", false) };
	CHECK_EQ(check(both, with_libs(both, 4, EXTENSIONS, 70), &ck), 0);
	CHECK_EQ(ck.tool[TOOLS_GL_LIBRARY].state, INSTALL_CURRENT);
	CHECK_EQ(ck.tool[TOOLS_GL_UTILITY].id, 70 + TOOLS_GL_UTILITY);
	CHECK_EQ(ck.tool[TOOLS_ENHANCEMENTS].state, INSTALL_CURRENT);
	CHECK_EQ(ck.tool[TOOLS_ENHANCEMENTS].dir, EXTENSIONS);
	CHECK_EQ(ck.tool[TOOLS_ENHANCEMENTS].id, 50);
	CHECK(strcmp(ck.tool[TOOLS_ENHANCEMENTS].folder, "Extensions") == 0);
	CHECK_EQ(ck.tool[TOOLS_3D_ENGINE].state, INSTALL_CURRENT);
	CHECK_EQ(ck.tool[TOOLS_3D_ENGINE].id, 51);
	CHECK(!install_needed(&ck));
	CHECK(!ck.apple_disable && !ck.apple_restore);

	/* Apple's OpenGL in Extensions beside Blue's: to be disabled */
	CHECK_EQ(check(both, with_apple(both, with_libs(both, 4, EXTENSIONS, 70), EXTENSIONS, 80), &ck), 0);
	CHECK(ck.apple[0].found[0] && ck.apple[2].found[0] && !ck.apple[0].found[1]);
	CHECK_EQ(ck.apple[1].dir[0], EXTENSIONS);
	CHECK(ck.apple_disable && !ck.apple_restore);
	CHECK(install_needed(&ck));
	/* ... without Blue's (on 9.x before they're installed): they are, and Apple's go then */
	CHECK_EQ(check(both, with_apple(both, 4, EXTENSIONS, 80), &ck), 0);
	CHECK(!ck.apple_disable && !ck.apple_restore);
	CHECK_EQ(ck.tool[TOOLS_GL_MEMORY].state, INSTALL_MISSING);
	CHECK(install_needed(&ck));
	/* Apple's disabled, Blue's gone: Apple's to be put back, Blue's not wanted */
	CHECK_EQ(check(both, with_apple(both, 4, DISABLED, 80), &ck), 0);
	CHECK(!ck.apple_disable && ck.apple_restore);
	CHECK(ck.apple[0].found[1] && ck.apple[0].dir[1] == DISABLED);
	CHECK(install_needed(&ck));
	/* ... unless some of Blue's are there: then they're installed, and Apple's stay */
	both[4] = tool_item(TOOLS_GL_MEMORY, DISABLED, 79, NULL, false);
	CHECK_EQ(check(both, with_apple(both, 5, DISABLED, 80), &ck), 0);
	CHECK(!ck.apple_disable && !ck.apple_restore);
	/* (a file of one of those names but of Blue's creator isn't Apple's) */
	struct item mine[] = { folders[0], tool_item(TOOLS_GL_LIBRARY, EXTENSIONS, 90, "OpenGLLibrary", false) };
	CHECK_EQ(check(mine, 2, &ck), 0);
	CHECK(!ck.apple[0].found[0] && ck.tool[TOOLS_GL_LIBRARY].state == INSTALL_CURRENT);

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
	write_volume(none, 4, 0);
	uint8_t zero[4] = { 0 };
	int fd = open(volume_path, O_WRONLY);
	CHECK(fd >= 0 && pwrite(fd, zero, 4, MDB_BLESSED) == 4);
	if (fd >= 0)
		close(fd);
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
	CHECK(strcmp(in.yes, "Install and Restart") == 0);
	write_volume(folders, 1, 0x0080);
	install_begin(&in, volume_path, 0, false);
	CHECK_EQ(in.phase, INSTALL_OFF);

	struct item both[12] = { folders[0], tool_item(TOOLS_ENHANCEMENTS, EXTENSIONS, 50, NULL, false),
				 tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 51, NULL, false) };
	unsigned n = with_libs(both, 3, EXTENSIONS, 70);
	write_volume(both, n, 0);
	install_begin(&in, volume_path, 0, false);
	CHECK_EQ(in.phase, INSTALL_OFF);
	both[2] = tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 51, NULL, true);
	write_volume(both, n, 0);
	install_begin(&in, volume_path, 0, false);
	CHECK_EQ(in.phase, INSTALL_WAIT_FINDER);
	CHECK(strcmp(in.yes, "Update and Restart") == 0);
	CHECK(strstr(in.question, "need updating: Blue 3D Engine is from another version of Blue.") != NULL);

	/* Mac OS 9: Apple's OpenGL goes when Blue's comes; Blue's beside Apple's; Apple's back */
	struct item folders9[] = { folder(SYSTEM, EXTENSIONS, "Extensions"), folder(SYSTEM, DISABLED, "Extensions (Disabled)") };
	struct item nine[12] = { folders9[0], folders9[1] };
	write_volume(nine, with_apple(nine, 2, EXTENSIONS, 80), 0);
	install_begin(&in, volume_path, 0, false);
	CHECK(strstr(in.question, "aren't installed on \"Test\"") != NULL);
	CHECK(strstr(in.question, "Apple's OpenGL libraries go to \"Extensions (Disabled)\": Blue's take their place.") !=
	      NULL);
	nine[2] = tool_item(TOOLS_ENHANCEMENTS, EXTENSIONS, 50, NULL, false);
	nine[3] = tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 51, NULL, false);
	write_volume(nine, with_apple(nine, with_libs(nine, 4, EXTENSIONS, 70), EXTENSIONS, 80), 0);
	install_begin(&in, volume_path, 0, false);
	CHECK_EQ(in.phase, INSTALL_WAIT_FINDER);
	CHECK(strcmp(in.yes, "Move and Restart") == 0);
	CHECK(strstr(in.question, "move Apple's to \"Extensions (Disabled)\"?") != NULL);
	nine[4] = tool_item(TOOLS_PIPPIN, EXTENSIONS, 75, NULL, false);
	write_volume(nine, with_apple(nine, 5, DISABLED, 80), 0);
	install_begin(&in, volume_path, 0, false);
	CHECK_EQ(in.phase, INSTALL_WAIT_FINDER);
	CHECK(strcmp(in.yes, "Put Back and Restart") == 0);
	CHECK(strstr(in.question, "Blue's OpenGL libraries aren't on \"Test\", and Apple's are in") != NULL);
}

/* ---- the installer, as Mac OS runs it ----------------------------------------------- */

#define HZ 16600000u
#define TOC 0x10000u
#define FRAME 0x20000u
#define TVECTORS 0x40000u
#define IDLE_TRAP 0xABF7u               /* _SynchIdleTime, ... */
#define TRAP_ENTRY 0x1DDCu              /* ... its entry in the Toolbox trap table */
#define IDLE_TRAP_9 0xAAAAu             /* Mac OS 9's idle routine's trap, ... */
#define TRAP_ENTRY_9 0x18A8u
#define ROUTINE 0x00123456u
#define VREF 0xFFFFu                    /* (-1) */
#define DRIVE 8u

/* Its imports, in order */
enum { FIND_FOLDER, HGET_VINFO, GET_CAT_INFO, DELETE, CREATE, OPEN_DF, OPEN_RF, WRITE, CLOSE, SET_FINFO, RENAME, CAT_MOVE,
       FLUSH_VOL, AE_CREATE_DESC, AE_CREATE_APPLE_EVENT, AE_SEND, AE_DISPOSE_DESC, IMPORTS };
#define FILES 16
#define FORK_MAX 0x20000u

static struct mem mem;

/* A File Manager over a few files */
struct file {
	bool used;
	uint32_t dir, id;
	char name[64];
	uint32_t type, creator;
	uint8_t fork[2][FORK_MAX];
	uint32_t len[2];
	bool locked;
};

static struct fs {
	struct file f[FILES];
	uint32_t next_id;
	bool volume_locked, full;
	unsigned opened;                /* forks open */
	/* ... and the Apple Event Manager: what's flushed, the restarts asked for, descriptors not disposed of */
	unsigned flushes, restarts, descs;
	int16_t send_error;
} fs;

static void pname(uint32_t p, char *out)
{
	uint8_t n = mem_r8(&mem, p);
	memcpy(out, mem_ptr(&mem, p + 1), n);
	out[n] = '\0';
}

static struct file *find(uint32_t dir, const char *name)
{
	for (unsigned k = 0; k < FILES; k++)
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
	for (unsigned k = 0; k < FILES; k++) {
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
		CHECK(a[1] == FOURCC('e', 'x', 't', 'n') || (a[1] == FOURCC('e', 'x', 't', 'D') && a[2] == 1));
		mem_w16(&mem, a[3], VREF);
		mem_w32(&mem, a[4], a[1] == FOURCC('e', 'x', 't', 'n') ? EXTENSIONS : DISABLED);
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
		for (unsigned k = 0, n = 0; k < FILES; k++) {
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
		CHECK(f->len[fork] + n <= FORK_MAX);
		if (f->len[fork] + n > FORK_MAX)
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
	case CAT_MOVE:                          /* (a CMovePBRec: the name, ioDirID, ioNewDirID) */
		pname(mem_r32(&mem, pb + 18), name);
		CHECK_EQ(mem_r32(&mem, pb + 28), 0);
		if (!(f = find(mem_r32(&mem, pb + 48), name)))
			return -43;
		if (find(mem_r32(&mem, pb + 36), name))
			return -48;
		f->dir = mem_r32(&mem, pb + 36);
		return 0;
	case FLUSH_VOL:                         /* FlushVol(NULL, vRefNum) */
		CHECK(a[0] == 0 && (int16_t)a[1] == (int16_t)VREF);
		fs.flushes++;
		return 0;
	case AE_CREATE_DESC:                    /* the Process Manager's address: {0, kSystemProcess} */
		CHECK(a[0] == FOURCC('p', 's', 'n', ' ') && a[2] == 8);
		CHECK(mem_r32(&mem, a[1]) == 0 && mem_r32(&mem, a[1] + 4) == 1);
		mem_w32(&mem, a[3], a[0]);
		mem_w32(&mem, a[3] + 4, 0x100 + fs.descs++);
		return 0;
	case AE_CREATE_APPLE_EVENT:             /* kCoreEventClass, kAERestart, its address, a new ID, any transaction */
		CHECK(a[0] == FOURCC('a', 'e', 'v', 't') && a[1] == FOURCC('r', 'e', 's', 't'));
		CHECK_EQ(mem_r32(&mem, a[2]), FOURCC('p', 's', 'n', ' '));
		CHECK(a[3] == 0xFFFFFFFFu && a[4] == 0);
		mem_w32(&mem, a[5], FOURCC('a', 'e', 'v', 't'));
		mem_w32(&mem, a[5] + 4, 0x100 + fs.descs++);
		return 0;
	case AE_SEND:                           /* no reply, the default timeout; the tools all in place by then */
		CHECK_EQ(mem_r32(&mem, a[0]), FOURCC('a', 'e', 'v', 't'));
		CHECK(a[1] != 0 && (a[2] & 3) == 1 && a[4] == 0xFFFFFFFFu && a[5] == 0 && a[6] == 0);
		CHECK(fs.opened == 0 && fs.flushes > fs.restarts);
		if (fs.send_error)
			return fs.send_error;
		fs.restarts++;
		return 0;
	case AE_DISPOSE_DESC:
		if (mem_r32(&mem, a[0] + 4)) {
			CHECK(fs.descs > 0);
			fs.descs--;
		}
		mem_w32(&mem, a[0], 0);
		mem_w32(&mem, a[0] + 4, 0);
		return 0;
	default:
		CHECK(!"an import");
		return -50;
	}
}

static uint16_t idle_trap = IDLE_TRAP;

/* Idle calls with the Finder current and nothing on the disks, until the installer is diverted to */
static bool idle_until_diverted(struct install *in, struct helper *h, uint64_t *tb)
{
	for (unsigned k = 0; k < 200; k++) {
		struct install_idle w = { &mem, h, true, *tb, HZ, 1000, DRIVE, idle_trap };
		*tb += HZ / 20;
		if (install_idle(in, &w))
			return true;
	}
	return false;
}

/* The bootstrap's work, then the installer's main routine and its callouts until it returns */
static void run_installer(struct install *in)
{
	for (unsigned k = 0; k < IMPORTS; k++)
		mem_w32(&mem, TOC + in->layout.import[k], TVECTORS + 8 * k);
	struct ndrv_call c = { .mem = &mem, .entry = 0, .toc = TOC, .frame = FRAME };
	install_call(in, &c);
	for (unsigned steps = 0; !c.done && steps < 1000; steps++) {
		CHECK(c.tvector >= TVECTORS && c.tvector < TVECTORS + 8 * IMPORTS && c.tvector % 8 == 0);
		int16_t err = file_manager((c.tvector - TVECTORS) / 8, c.out);
		c = (struct ndrv_call){ .mem = &mem, .entry = NDRV_ENTRY_RESUME, .toc = TOC, .frame = FRAME };
		c.arg[0] = (uint32_t)(int32_t)err;
		install_call(in, &c);
	}
	CHECK(c.done);
	CHECK_EQ(fs.descs, 0);
}

static bool same_as(const struct file *f, unsigned t)
{
	return f && f->type == tools[t].type && f->creator == tools[t].creator && f->len[0] == tools[t].data_len &&
	       f->len[1] == tools[t].rsrc_len && !memcmp(f->fork[0], tools[t].data, tools[t].data_len) &&
	       !memcmp(f->fork[1], tools[t].rsrc, tools[t].rsrc_len);
}

/* Enhancements missing, an older engine in Extensions (Disabled): installed, updated where it is, then a restart */
static void test_installer(void)
{
	static struct install in;
	struct helper h = { .running = false };
	struct item items[8] = { folder(SYSTEM, EXTENSIONS, "Extensions"), folder(SYSTEM, DISABLED, "Extensions (Disabled)"),
				 tool_item(TOOLS_3D_ENGINE, DISABLED, 60, "Old 3D", true) };
	write_volume(items, with_libs(items, 3, EXTENSIONS, 70), 0);
	memset(&fs, 0, sizeof fs);
	fs.next_id = 100;
	add_file(DISABLED, "Old 3D", 60, &tools[TOOLS_3D_ENGINE])->fork[0][0] ^= 1;
	install_begin(&in, volume_path, 0, false);
	CHECK_EQ(in.phase, INSTALL_WAIT_FINDER);

	/* asked once the Finder is up; the answer diverts the trap to the bootstrap */
	uint64_t tb = HZ;
	mem_w32(&mem, TRAP_ENTRY, ROUTINE);
	struct install_idle w = { &mem, &h, false, tb, HZ, 1000, DRIVE, IDLE_TRAP };
	CHECK(!install_idle(&in, &w));                  /* (not the Finder) */
	CHECK(!in.ask);
	CHECK(!idle_until_diverted(&in, &h, &tb));
	CHECK_EQ(in.phase, INSTALL_ASKING);
	CHECK(in.ask);
	install_answer(&in, true, &h);
	CHECK_EQ(in.phase, INSTALL_INJECT);
	w = (struct install_idle){ &mem, &h, true, tb, HZ, 1000, DRIVE, 0 };
	CHECK(!install_idle(&in, &w));                  /* (a nap: not at the idle trap) */
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

	run_installer(&in);
	CHECK_EQ(in.phase, INSTALL_DONE);
	CHECK(in.news_ready);
	CHECK(same_as(find(EXTENSIONS, "Blue Enhancements"), TOOLS_ENHANCEMENTS));
	CHECK(same_as(find(DISABLED, "Old 3D"), TOOLS_3D_ENGINE));     /* (still disabled, still its name) */
	CHECK(!find(DISABLED, "Blue installing") && !find(EXTENSIONS, "Blue installing"));
	CHECK_EQ(fs.opened, 0);
	CHECK(fs.flushes == 1 && fs.restarts == 1 && in.restarting);
	CHECK(strcmp(in.news, "installed Blue Enhancements in \"Test:System Folder:Extensions\" and updated "
			      "Blue 3D Engine (\"Old 3D\") in \"Test:System Folder:Extensions (Disabled)\". Mac OS is "
			      "restarting.") == 0);
}

/*
 * The disk fills up while the engine is written: what was done stays, the
 * temporary file goes, and Mac OS doesn't restart. A locked volume, and a
 * restart Mac OS can't be asked for, likewise.
 */
static void test_installer_errors(void)
{
	static struct install in;
	struct helper h = { .running = false };
	struct item items[8] = { folder(SYSTEM, EXTENSIONS, "Extensions"),
				 tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 61, NULL, true) };
	unsigned n = with_libs(items, 2, EXTENSIONS, 70);
	write_volume(items, n, 0);
	memset(&fs, 0, sizeof fs);
	fs.next_id = 100;
	struct file *old = add_file(EXTENSIONS, "Blue 3D Engine", 61, &tools[TOOLS_3D_ENGINE]);
	old->fork[0][0] ^= 1;
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);                  /* (a script's, before the question) */
	uint64_t tb = HZ;
	mem_w32(&mem, TRAP_ENTRY_9, ROUTINE);           /* (Mac OS 9's idle trap, this time) */
	idle_trap = IDLE_TRAP_9;
	CHECK(idle_until_diverted(&in, &h, &tb));
	idle_trap = IDLE_TRAP;
	CHECK_EQ(mem_r32(&mem, TRAP_ENTRY_9), INSTALL_REGION);
	CHECK_EQ(mem_r32(&mem, INSTALL_REGION + 10), TRAP_ENTRY_9);
	for (unsigned k = 0; k < IMPORTS; k++)
		mem_w32(&mem, TOC + in.layout.import[k], TVECTORS + 8 * k);

	/* (run it by hand to fill the disk after Enhancements) */
	struct ndrv_call c = { .mem = &mem, .entry = 0, .toc = TOC, .frame = FRAME };
	install_call(&in, &c);
	for (unsigned steps = 0; !c.done && steps < 1000; steps++) {
		unsigned call = (c.tvector - TVECTORS) / 8;
		fs.full = find(EXTENSIONS, "Blue Enhancements") != NULL;
		int16_t err = file_manager(call, c.out);
		c = (struct ndrv_call){ .mem = &mem, .entry = NDRV_ENTRY_RESUME, .toc = TOC, .frame = FRAME };
		c.arg[0] = (uint32_t)(int32_t)err;
		install_call(&in, &c);
	}
	CHECK(c.done);
	CHECK(same_as(find(EXTENSIONS, "Blue Enhancements"), TOOLS_ENHANCEMENTS));
	CHECK(old->used && old->fork[0][0] != tools[TOOLS_3D_ENGINE].data[0]);  /* the old engine, as it was */
	CHECK(!find(EXTENSIONS, "Blue installing"));
	CHECK_EQ(fs.opened, 0);
	CHECK(fs.flushes == 0 && fs.restarts == 0 && !in.restarting);
	CHECK(strcmp(in.news, "installed Blue Enhancements in \"Test:System Folder:Extensions\"; couldn't update "
			      "Blue 3D Engine (writing it): the disk is full. Blue Enhancements starts with Mac OS next "
			      "time.") == 0);

	/* a locked volume: nothing done */
	fs.full = false;
	write_volume(items, n, 0);
	memset(&fs, 0, sizeof fs);
	fs.volume_locked = true;
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	CHECK(idle_until_diverted(&in, &h, &tb));
	run_installer(&in);
	CHECK(!find(EXTENSIONS, "Blue Enhancements") && !find(EXTENSIONS, "Blue installing"));
	CHECK(strcmp(in.news, "couldn't install the guest tools (looking at the startup disk): the disk is locked.") == 0);
	CHECK_EQ(fs.restarts, 0);

	/* all written, but the restart can't be asked for: said, the event disposed of */
	memset(&fs, 0, sizeof fs);
	fs.next_id = 100;
	fs.send_error = -108;
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	CHECK(idle_until_diverted(&in, &h, &tb));
	run_installer(&in);
	CHECK(same_as(find(EXTENSIONS, "Blue 3D Engine"), TOOLS_3D_ENGINE));
	CHECK(fs.flushes == 1 && fs.restarts == 0 && !in.restarting);
	CHECK(strcmp(in.news, "installed Blue Enhancements and Blue 3D Engine in \"Test:System Folder:Extensions\"; "
			      "couldn't restart Mac OS: there isn't enough memory. Restart Mac OS to use them.") == 0);
}

/* A running Enhancements that's to be replaced quits first; if it can't be replaced after all, it starts with Mac OS */
static void test_installer_quit(void)
{
	static struct install in;
	struct helper h = { .running = true };
	struct item items[8] = { folder(SYSTEM, EXTENSIONS, "Extensions"),
				 tool_item(TOOLS_ENHANCEMENTS, EXTENSIONS, 62, "Blue Helper", true),
				 tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 63, NULL, false) };
	write_volume(items, with_libs(items, 3, EXTENSIONS, 70), 0);
	memset(&fs, 0, sizeof fs);
	fs.next_id = 100;
	add_file(EXTENSIONS, "Blue Helper", 62, &tools[TOOLS_ENHANCEMENTS])->locked = true;
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	struct install_idle w = { &mem, &h, true, HZ, HZ, 1000, DRIVE, IDLE_TRAP };
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
	run_installer(&in);
	CHECK(strcmp(in.news, "couldn't update Blue Enhancements (replacing the old copy): the file is locked. "
			      "Blue Enhancements starts with Mac OS next time.") == 0);
	CHECK_EQ(fs.restarts, 0);
	CHECK(!find(EXTENSIONS, "Blue installing"));

	/* ... or is replaced, and Mac OS restarts */
	find(EXTENSIONS, "Blue Helper")->locked = false;
	h.running = true;
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	w.tb = HZ;
	for (unsigned k = 0; k < 40 && in.phase != INSTALL_QUITTING; k++, w.tb += HZ / 20)
		CHECK(!install_idle(&in, &w));
	h.quit = h.running = false;
	w.tb += HZ / 5;
	CHECK(!install_idle(&in, &w));
	w.tb += HZ / 5;
	CHECK(install_idle(&in, &w));
	run_installer(&in);
	CHECK(same_as(find(EXTENSIONS, "Blue Helper"), TOOLS_ENHANCEMENTS));
	CHECK(strcmp(in.news, "updated Blue Enhancements (\"Blue Helper\") in \"Test:System Folder:Extensions\". "
			      "Mac OS is restarting.") == 0);
	CHECK_EQ(fs.restarts, 1);

	/* ... or starts after the answer: it's asked to quit before the installer goes in */
	h.running = false;
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	w = (struct install_idle){ &mem, &h, true, HZ, HZ, 1000, DRIVE, 0 };
	for (unsigned k = 0; k < 40 && in.phase != INSTALL_INJECT; k++, w.tb += HZ / 20)
		CHECK(!install_idle(&in, &w));
	CHECK_EQ(in.phase, INSTALL_INJECT);
	h.running = true;
	w.trap = IDLE_TRAP;
	CHECK(!install_idle(&in, &w));
	CHECK(in.phase == INSTALL_QUITTING && h.quit);
	h.quit = false;

	/* one that doesn't quit: Blue goes ahead after a while */
	h.running = true;
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	uint64_t tb = HZ;
	CHECK(idle_until_diverted(&in, &h, &tb));
	CHECK(tb > 6 * (uint64_t)HZ);

	/* a system that idles without the idle trap: Blue gives up, and says so */
	h.running = false;
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	w = (struct install_idle){ &mem, &h, true, tb, HZ, 1000, DRIVE, 0 };
	for (unsigned k = 0; k < 400 && in.phase != INSTALL_DONE; k++, w.tb += HZ / 20)
		CHECK(!install_idle(&in, &w));
	CHECK_EQ(in.phase, INSTALL_DONE);
	CHECK(strstr(in.news, "Mac OS idles without a call Blue knows") != NULL);

	/* not now */
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, false, &h);
	CHECK(!idle_until_diverted(&in, &h, &tb));
	CHECK_EQ(in.phase, INSTALL_DONE);
}

/* Mac OS 9: Blue's OpenGL in, Apple's out to Extensions (Disabled); then, Blue's gone, Apple's back */
static void test_installer_apple(void)
{
	static struct install in;
	struct helper h = { .running = false };
	static const char *const names[3] = { "OpenGLLibrary", "OpenGLMemory", "OpenGLUtility" };

	/* 8.5 and 8.6 (no OpenGL of Apple's): all six in Extensions, said in one go */
	struct item none[] = { folder(SYSTEM, EXTENSIONS, "Extensions") };
	write_volume(none, 1, 0);
	memset(&fs, 0, sizeof fs);
	fs.next_id = 100;
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	uint64_t tb = HZ;
	CHECK(idle_until_diverted(&in, &h, &tb));
	run_installer(&in);
	for (unsigned t = 0; t < TOOLS_FILES; t++)
		CHECK(same_as(find(EXTENSIONS, tools[t].name), t));
	CHECK(strcmp(in.news, "installed Blue Enhancements, Blue 3D Engine, Blue OpenGL Library, Blue OpenGL Memory, "
			      "Blue OpenGL Utility and Blue Pippin Library in \"Test:System Folder:Extensions\". Mac OS is "
			      "restarting.") == 0);
	h.running = true;

	struct item items[12] = { folder(SYSTEM, EXTENSIONS, "Extensions"),
				  tool_item(TOOLS_ENHANCEMENTS, EXTENSIONS, 50, NULL, false),
				  tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 51, NULL, false),
				  tool_item(TOOLS_PIPPIN, EXTENSIONS, 52, NULL, false) };
	write_volume(items, with_apple(items, 4, EXTENSIONS, 80), 0);
	memset(&fs, 0, sizeof fs);
	fs.next_id = 100;
	for (unsigned k = 0; k < 3; k++)
		add_file(EXTENSIONS, names[k], 80 + k, NULL);
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	CHECK(idle_until_diverted(&in, &h, &tb));
	run_installer(&in);
	for (unsigned t = TOOLS_GL_FIRST; t < TOOLS_GL_FIRST + TOOLS_GL_FILES; t++)
		CHECK(same_as(find(EXTENSIONS, tools[t].name), t));
	for (unsigned k = 0; k < 3; k++)
		CHECK(find(DISABLED, names[k]) && !find(EXTENSIONS, names[k]));
	CHECK(strcmp(in.news, "installed Blue OpenGL Library, Blue OpenGL Memory and Blue OpenGL Utility in "
			      "\"Test:System Folder:Extensions\"; moved Apple's OpenGLLibrary, OpenGLMemory and OpenGLUtility "
			      "to \"Test:System Folder:Extensions (Disabled)\". Mac OS is restarting.") == 0);
	CHECK_EQ(fs.restarts, 1);

	/* Blue's deleted (the user's doing): Apple's back, Blue's not installed */
	for (unsigned t = TOOLS_GL_FIRST; t < TOOLS_GL_FIRST + TOOLS_GL_FILES; t++)
		find(EXTENSIONS, tools[t].name)->used = false;
	struct item folders[2] = { folder(SYSTEM, EXTENSIONS, "Extensions"),
				   folder(SYSTEM, DISABLED, "Extensions (Disabled)") };
	memcpy(items, folders, sizeof folders);
	items[2] = tool_item(TOOLS_ENHANCEMENTS, EXTENSIONS, 50, NULL, false);
	items[3] = tool_item(TOOLS_3D_ENGINE, EXTENSIONS, 51, NULL, false);
	items[4] = tool_item(TOOLS_PIPPIN, EXTENSIONS, 52, NULL, false);
	write_volume(items, with_apple(items, 5, DISABLED, 80), 0);
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	CHECK(idle_until_diverted(&in, &h, &tb));
	run_installer(&in);
	for (unsigned k = 0; k < 3; k++)
		CHECK(find(EXTENSIONS, names[k]) && !find(DISABLED, names[k]));
	CHECK(!find(EXTENSIONS, tools[TOOLS_GL_LIBRARY].name));
	CHECK(strcmp(in.news, "put Apple's OpenGLLibrary, OpenGLMemory and OpenGLUtility back in "
			      "\"Test:System Folder:Extensions\". Mac OS is restarting.") == 0);
	CHECK_EQ(fs.restarts, 2);
	/* ... after which Blue's aren't offered again, in this run */
	CHECK(in.check.apple_kept);
	write_volume(items, with_apple(items, 5, EXTENSIONS, 80), 0);
	install_begin(&in, volume_path, 0, false);
	CHECK_EQ(in.phase, INSTALL_OFF);
	in.check.apple_kept = false;            /* (another run) */
	install_begin(&in, volume_path, 0, false);
	CHECK_EQ(in.phase, INSTALL_WAIT_FINDER);

	/* one that can't move (one of that name in the way): said, the rest moved, and no restart */
	memset(&fs, 0, sizeof fs);
	fs.next_id = 100;
	for (unsigned k = 0; k < 3; k++)
		add_file(EXTENSIONS, names[k], 80 + k, NULL);
	add_file(DISABLED, names[1], 99, NULL);
	write_volume(items, with_apple(items, 5, EXTENSIONS, 80), 0);
	install_begin(&in, volume_path, 0, false);
	install_answer(&in, true, &h);
	CHECK(idle_until_diverted(&in, &h, &tb));
	run_installer(&in);
	CHECK(find(DISABLED, names[0]) && find(EXTENSIONS, names[1]) && find(DISABLED, names[2]));
	CHECK(strstr(in.news, "; moved Apple's OpenGLLibrary and OpenGLUtility to \"Test:System Folder:Extensions "
			      "(Disabled)\"; couldn't move Apple's OpenGLMemory: something of that name is in the way.") !=
	      NULL);
	CHECK(fs.flushes == 0 && fs.restarts == 0);
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
	test_installer_apple();
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
