/*
 * The startup disk: Open Firmware's variables in NVRAM (the "common"
 * partition, made when NVRAM is blank, boot-device as Mac OS 9 writes it),
 * paths to Blue's disks, and which drive's ROM file is started: the one
 * boot-device names, else the first (with a real ROM file, if there's one:
 * $BLUE_TEST_ROM or ./MOS.rom); without one, a Pippin disc among them.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/be.h"
#include "../src/hfs.h"
#include "../src/ofnvram.h"
#include "../src/startup.h"
#include "test.h"

#define NVRAM_BYTES 8192u
#define TBXI 0x74627869u

static void test_variables(void)
{
	static uint8_t nv[NVRAM_BYTES];
	size_t data, len;
	char value[OFNVRAM_VALUE_MAX];

	memset(nv, 0, sizeof nv);
	CHECK(!ofnvram_common(nv, sizeof nv, &data, &len));
	CHECK(!ofnvram_get(nv, sizeof nv, "boot-device", value, sizeof value));
	CHECK(ofnvram_format(nv, sizeof nv));
	CHECK(ofnvram_common(nv, sizeof nv, &data, &len));
	CHECK(data == 16 && len == OFNVRAM_COMMON_SIZE - 16);
	CHECK(nv[0] == 0x70 && nv[1] == 0x7C && nv[2] == 0x00 && nv[3] == 0x80);  /* (as Mac OS 9.1 checks it) */
	CHECK(!memcmp(nv + 4, "common\0\0\0\0\0\0", 12));
	CHECK(!ofnvram_format(nv, sizeof nv));                  /* (there is one) */
	CHECK(!ofnvram_get(nv, sizeof nv, "boot-device", value, sizeof value));

	/* as Mac OS 9.1's ROM writes a variable in an empty partition: after an empty string */
	static const char written[] = "\0boot-device=/pci@80000000/blue,disk@3@3:8,\\\\:tbxi\0auto-boot?=true";
	memcpy(nv + data, written, sizeof written);
	CHECK(ofnvram_get(nv, sizeof nv, "boot-device", value, sizeof value));
	CHECK(strcmp(value, "/pci@80000000/blue,disk@3@3:8,\\\\:tbxi") == 0);
	CHECK(ofnvram_get(nv, sizeof nv, "auto-boot?", value, sizeof value) && strcmp(value, "true") == 0);
	CHECK(!ofnvram_get(nv, sizeof nv, "boot", value, sizeof value));
	CHECK(ofnvram_get(nv, sizeof nv, "boot-device", value, 8) && strcmp(value, "/pci@80") == 0);
	/* (what follows the end, an empty string after some, isn't read) */
	static const char ended[] = "boot-file=\0\0boot-device=stale";
	memset(nv + data, 0, len);
	memcpy(nv + data, ended, sizeof ended);
	CHECK(ofnvram_get(nv, sizeof nv, "boot-file", value, sizeof value) && value[0] == '\0');
	CHECK(!ofnvram_get(nv, sizeof nv, "boot-device", value, sizeof value));

	/* a broken header: no partition (and none made over what's there) */
	nv[1] ^= 1;
	CHECK(!ofnvram_common(nv, sizeof nv, &data, &len));
	CHECK(!ofnvram_format(nv, sizeof nv));
	/* "common" after another partition; one that runs past the end */
	memset(nv, 0, sizeof nv);
	nv[0] = 0xA0;
	nv[3] = 0x20;                                           /* (512 bytes) */
	memcpy(nv + 4, "APL,MacOS75", 11);
	unsigned sum = 0xA0 + 0x20 + 'A' + 'P' + 'L' + ',' + 'M' + 'a' + 'c' + 'O' + 'S' + '7' + '5';
	while (sum > 0xFF)
		sum = (sum & 0xFF) + (sum >> 8);
	nv[1] = (uint8_t)sum;
	CHECK(!ofnvram_common(nv, sizeof nv, &data, &len));
	CHECK(!ofnvram_format(nv, sizeof nv));                  /* (not blank) */
	uint8_t blank[NVRAM_BYTES] = { 0 };
	CHECK(ofnvram_format(blank, sizeof blank));
	memcpy(nv + 512, blank, 16);
	CHECK(ofnvram_common(nv, sizeof nv, &data, &len) && data == 512 + 16);
	CHECK(!ofnvram_common(nv, 1024, &data, &len));          /* (past the end) */
	CHECK(!ofnvram_format(blank, 1024));
}

static void test_paths(void)
{
	unsigned dev, entry;
	CHECK(ofnvram_disk_path("/pci@80000000/blue,disk@3@3:8,\\\\:tbxi", &dev, &entry) && dev == 3 && entry == 8);
	CHECK(ofnvram_disk_path("/pci@80000000/blue,disk@2@2:,\\\\:tbxi", &dev, &entry) && dev == 2 && entry == 0);
	CHECK(ofnvram_disk_path("/pci/blue,disk@4:12,\\\\:tbxi", &dev, &entry) && dev == 4 && entry == 12);
	CHECK(ofnvram_disk_path("/pci/blue,disk@a/@a:3", &dev, &entry) && dev == 10 && entry == 3);
	CHECK(ofnvram_disk_path("/pci/blue,disk@5", &dev, &entry) && dev == 5 && entry == 0);
	CHECK(!ofnvram_disk_path("hd:,\\\\:tbxi", &dev, &entry));
	CHECK(!ofnvram_disk_path("enet:bootp", &dev, &entry));
	CHECK(!ofnvram_disk_path("/pci/blue,floppy@7:,\\\\:tbxi", &dev, &entry));
	CHECK(!ofnvram_disk_path("/pci/blue,disk@:3", &dev, &entry));
	CHECK(!ofnvram_disk_path("/pci/blue,disk@3x:3", &dev, &entry));
	CHECK(!ofnvram_disk_path("/pci/blue,disk@3:3x", &dev, &entry));
	CHECK(!ofnvram_disk_path("/pci/blue,disk@40:3", &dev, &entry));

	/* boot-device, as a drive's index: PCI device 2 is the first drive */
	static uint8_t nv[NVRAM_BYTES];
	size_t data, len;
	unsigned drive;
	memset(nv, 0, sizeof nv);
	CHECK(!startup_choice(nv, sizeof nv, &drive, &entry));
	ofnvram_format(nv, sizeof nv);
	CHECK(ofnvram_common(nv, sizeof nv, &data, &len));
	strcpy((char *)nv + data + 1, "boot-device=/pci@80000000/blue,disk@3@3:8,\\\\:tbxi");
	CHECK(startup_choice(nv, sizeof nv, &drive, &entry) && drive == 1 && entry == 8);
	strcpy((char *)nv + data + 1, "boot-device=/pci/blue,disk@1:8");
	CHECK(!startup_choice(nv, sizeof nv, &drive, &entry));
	strcpy((char *)nv + data + 1, "boot-device=hd:,\\\\:tbxi");
	CHECK(!startup_choice(nv, sizeof nv, &drive, &entry));
}

static int temp_file(char *path, size_t len)
{
	const char *dir = getenv("TMPDIR");
	snprintf(path, len, "%s/blue-test-XXXXXX", dir ? dir : "/tmp");
	return mkstemp(path);
}

/* A volume "name" whose root folder is blessed and holds a Mac OS ROM file (`rom`), or nothing */
static void make_disk(const char *path, const char *name, const uint8_t *rom, uint32_t rom_len)
{
	int fd = open(path, O_RDWR);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	struct hfs_file f = { "Mac OS ROM", TBXI, 0x63687270u, 0, 0, 0, rom, NULL, rom_len, 0 };      /* ('chrp') */
	CHECK(ftruncate(fd, 8u << 20) == 0);
	CHECK_EQ(hfs_format(fd, 8u << 20, name, 0, &f, rom ? 1 : 0), 0);
	uint8_t blessed[4];
	put_be32(blessed, 2);                   /* (the root) */
	CHECK(pwrite(fd, blessed, 4, 1024 + 92) == 4);
	close(fd);
}

/* Which drive's ROM file starts: boot-device's, else the first's (with a real ROM file only) */
static void test_startup_rom(void)
{
	const char *rom_path = getenv("BLUE_TEST_ROM") ? getenv("BLUE_TEST_ROM") : "MOS.rom";
	FILE *f = fopen(rom_path, "rb");
	if (!f)
		return;
	static uint8_t rom[4u << 20];
	size_t rom_len = fread(rom, 1, sizeof rom, f);
	fclose(f);

	char a[256], b[256], c[256], note[512];
	int fa = temp_file(a, sizeof a), fb = temp_file(b, sizeof b), fc = temp_file(c, sizeof c);
	CHECK(fa >= 0 && fb >= 0 && fc >= 0);
	if (fa < 0 || fb < 0 || fc < 0)
		return;
	close(fa);
	close(fb);
	close(fc);
	make_disk(a, "First", rom, (uint32_t)rom_len);
	make_disk(b, "Empty", NULL, 0);
	make_disk(c, "Third", rom, (uint32_t)rom_len);
	const char *paths[4] = { a, NULL, b, c };
	struct rom r;
	struct startup s;

	/* no choice: the first */
	CHECK_EQ(startup_rom(paths, 4, NULL, 0, &r, &s, note, sizeof note), 0);
	CHECK(s.drive == 0 && s.part == 0 && !s.chosen && !note[0]);
	CHECK(strstr(s.from, "\"First:Mac OS ROM\" from ") != NULL);
	rom_free(&r);

	/* boot-device names the fourth drive (PCI device 5) */
	static uint8_t nv[NVRAM_BYTES];
	size_t data, len;
	memset(nv, 0, sizeof nv);
	ofnvram_format(nv, sizeof nv);
	ofnvram_common(nv, sizeof nv, &data, &len);
	strcpy((char *)nv + data + 1, "boot-device=/pci@80000000/blue,disk@5@5:,\\\\:tbxi");
	CHECK_EQ(startup_rom(paths, 4, nv, sizeof nv, &r, &s, note, sizeof note), 0);
	CHECK(s.drive == 3 && s.chosen && !note[0]);
	CHECK(strstr(s.from, "\"Third:Mac OS ROM\"") != NULL);
	rom_free(&r);

	/* ... or one without a ROM file, an empty drive, one that isn't there, a partition there isn't: the first */
	static const char *const others[] = { "disk@4@4:,", "disk@3@3:,", "disk@9@9:,", "disk@5@5:3," };
	static const char *const why[] = { "no System Folder with a Mac OS ROM file", "drive 1, which isn't there",
					   "drive 7, which isn't there", "no System Folder with a Mac OS ROM file" };
	for (unsigned k = 0; k < 4; k++) {
		snprintf((char *)nv + data + 1, len - 1, "boot-device=/pci@80000000/blue,%s\\\\:tbxi", others[k]);
		CHECK_EQ(startup_rom(paths, 4, nv, sizeof nv, &r, &s, note, sizeof note), 0);
		CHECK(s.drive == 0 && !s.chosen);
		CHECK(strstr(note, why[k]) != NULL);
		rom_free(&r);
	}

	/* nothing to start up from */
	const char *none[2] = { NULL, b };
	CHECK_EQ(startup_rom(none, 2, nv, sizeof nv, &r, &s, note, sizeof note), 1);
	unlink(a);
	unlink(b);
	unlink(c);
}

/* A volume "name" of `files`, its root folder blessed (as a System Folder) if `blessed` */
static void make_volume(const char *path, const char *name, const struct hfs_file *files, unsigned n, bool blessed)
{
	int fd = open(path, O_RDWR);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(ftruncate(fd, 8u << 20) == 0);
	CHECK_EQ(hfs_format(fd, 8u << 20, name, 0, files, n), 0);
	uint8_t root[4];
	put_be32(root, 2);
	if (blessed)
		CHECK(pwrite(fd, root, 4, 1024 + 92) == 4);
	close(fd);
}

/*
 * A Pippin disc, by its PippinAuthenticationFile at the root or by its
 * System Folder's Pippin enabler ('gbly'), among disks without a Mac OS ROM
 * file; and a plain disk, which is neither a startup disk nor a Pippin's
 */
static void test_pippin_disc(void)
{
	static const uint8_t bytes[16] = { 1, 2, 3 };
	const struct hfs_file auth[] = {
		{ "Read Me", 0x54455854u, 0x74747874u, 0, 0, 0, bytes, NULL, sizeof bytes, 0 },      /* ('TEXT', 'ttxt') */
		{ "PippinAuthenticationFile", 0x50706E56u, 0x50706E41u, 0, 0, 0, bytes, NULL, sizeof bytes, 0 },
										/* ('PpnV', 'PpnA') */
	};
	const struct hfs_file system[] = {
		{ "Finder", 0x464E4452u, 0x4D414353u, 0, 0, 0, NULL, bytes, 0, sizeof bytes },     /* ('FNDR', 'MACS') */
		{ "Pippin", 0x67626C79u, 0x4D414353u, 0, 0, 0, NULL, bytes, 0, sizeof bytes },     /* ('gbly') */
		{ "System", 0x7A737973u, 0x4D414353u, 0, 0, 0, NULL, bytes, 0, sizeof bytes },     /* ('zsys') */
	};
	const struct hfs_file not_enabler[] = {
		{ "Pippin", 0x4150504Cu, 0x3F3F3F3Fu, 0, 0, 0, bytes, NULL, sizeof bytes, 0 },     /* ('APPL') */
		{ "System", 0x7A737973u, 0x4D414353u, 0, 0, 0, NULL, bytes, 0, sizeof bytes },
	};
	char plain[256], game[256], enabler[256], other[256], name[64] = "";
	int fds[4] = { temp_file(plain, sizeof plain), temp_file(game, sizeof game), temp_file(enabler, sizeof enabler),
		       temp_file(other, sizeof other) };
	for (unsigned k = 0; k < 4; k++) {
		CHECK(fds[k] >= 0);
		if (fds[k] < 0)
			return;
		close(fds[k]);
	}
	make_volume(plain, "Untitled", NULL, 0, false);
	make_volume(game, "Super Marathon", auth, 2, false);
	make_volume(enabler, "Pippin Disc", system, 3, true);
	make_volume(other, "Not a Pippin", not_enabler, 2, true);
	unsigned drive = 99;

	/* the plain disk: no startup disk, and no Pippin disc */
	const char *alone[1] = { plain };
	struct rom r;
	struct startup s;
	char note[256];
	CHECK_EQ(startup_rom(alone, 1, NULL, 0, &r, &s, note, sizeof note), 1);
	CHECK(!startup_pippin(alone, 1, &drive, name, sizeof name));

	/* the authentication file (after an empty drive and the plain disk) */
	const char *with_game[3] = { NULL, plain, game };
	CHECK_EQ(startup_rom(with_game, 3, NULL, 0, &r, &s, note, sizeof note), 1);
	CHECK(startup_pippin(with_game, 3, &drive, name, sizeof name));
	CHECK(drive == 2 && strcmp(name, "Super Marathon") == 0);

	/* the enabler in the blessed System Folder; a "Pippin" that isn't one isn't enough */
	const char *with_enabler[2] = { other, enabler };
	CHECK(startup_pippin(with_enabler, 2, &drive, name, sizeof name));
	CHECK(drive == 1 && strcmp(name, "Pippin Disc") == 0);
	CHECK(!startup_pippin(with_enabler, 1, &drive, name, sizeof name));

	/* (the name, in any case) */
	int fd = open(game, O_RDONLY);
	struct hfsread_volume v;
	struct hfsread_file f;
	CHECK(fd >= 0 && hfsread_open(&v, fd, 0, 8u << 20) == 0);
	CHECK(hfsread_lookup(&v, 2, "pippinauthenticationfile", &f) == 1 && !f.folder && f.type == 0x50706E56u);
	CHECK(hfsread_lookup(&v, 2, "PippinAuthentication", &f) == 0);
	if (fd >= 0)
		close(fd);
	unlink(plain);
	unlink(game);
	unlink(enabler);
	unlink(other);
}

void test_startup(void)
{
	test_variables();
	test_paths();
	test_startup_rom();
	test_pippin_disc();
}
