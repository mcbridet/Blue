/*
 * Compressed Mac images (dmg.h): the UDIF fixtures hdiutil made (zlib, bzip2,
 * LZFSE, xz, ADC chunks) read back as the volume they were made from; UDIF
 * images made here, with raw, zero and zlib chunks in an XML property list,
 * and early ones whose block table is a resource fork after the data, with
 * ADC chunks counted from each partition's own data offset; Disk Copy 6
 * (NDIF) images, their resource fork in an AppleDouble file, MacBinary or
 * AppleSingle; ADC itself; what isn't read, and why. Then such images in the
 * floppy drive, as a disk and as a CD, and a GUID partition table's HFS+
 * partition.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/be.h"
#include "../src/cdimage.h"
#include "../src/disk.h"
#include "../src/dmg.h"
#include "../src/drives.h"
#include "../src/floppy.h"
#include "../src/hfs.h"
#include "../src/hfsread.h"
#include "../src/rsrc.h"
#include "test.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define VOLUME 409600u                  /* the fixtures' volume: 400K */
#define SECTORS (VOLUME / 512)

static char dir[1024];

static const char *path(const char *name)
{
	static char p[2][1100];
	static int k;
	k ^= 1;
	snprintf(p[k], sizeof p[k], "%s/%s", dir, name);
	return p[k];
}

static const char *fixture(const char *name)
{
	static char p[1100];
	const char *slash = strrchr(__FILE__, '/');
	snprintf(p, sizeof p, "%.*sfixtures/%s", slash ? (int)(slash - __FILE__ + 1) : 0, __FILE__, name);
	return p;
}

static void write_file(const char *name, const void *data, size_t len)
{
	FILE *f = fopen(path(name), "wb");
	if (f) {
		fwrite(data, 1, len, f);
		fclose(f);
	}
}

/* The volume the fixtures hold (tests/fixtures/README) */
static uint8_t *volume(void)
{
	static const char hello[] = "Hello from a compressed image";
	struct hfs_file file = { "Read Me", FOURCC('T', 'E', 'X', 'T'), FOURCC('t', 't', 'x', 't'), 0, 0, 0,
				 (const uint8_t *)hello, NULL, sizeof hello - 1, 0 };
	int fd = open(path("volume"), O_RDWR | O_CREAT | O_TRUNC, 0644);
	uint8_t *v = malloc(VOLUME);
	if (fd < 0 || !v || ftruncate(fd, VOLUME) || hfs_format(fd, VOLUME, "Packed", 0xB0000000u, &file, 1) ||
	    pread(fd, v, VOLUME, 0) != (ssize_t)VOLUME) {
		free(v);
		v = NULL;
	}
	if (fd >= 0)
		close(fd);
	unlink(path("volume"));
	return v;
}

/* An image opens as one of these, and reads as `want` (`len` bytes) */
static void check_image(const char *p, const uint8_t *want, size_t len, const char *kind)
{
	char why[300] = "";
	struct dmg *d;
	int rc = dmg_open(p, &d, why, sizeof why);
	CHECK_EQ(rc, 1);
	if (rc != 1) {
		fprintf(stderr, "dmg: %s: %s\n", p, why);
		return;
	}
	CHECK_EQ(dmg_size(d), len);
	if (kind && !strstr(dmg_kind(d), kind)) {
		fprintf(stderr, "dmg: %s: \"%s\", not %s\n", p, dmg_kind(d), kind);
		CHECK(!"the kind of image");
	}
	uint8_t *got = malloc(len);
	CHECK_EQ(dmg_read(d, got, len, 0), 0);
	CHECK(!memcmp(got, want, len));
	/* again, in odd pieces, back to front */
	uint8_t piece[1000];
	bool same = true;
	for (size_t at = len; at > 0;) {
		size_t n = at < sizeof piece ? at : 777;
		at -= n;
		same &= !dmg_read(d, piece, n, at) && !memcmp(piece, want + at, n);
	}
	CHECK(same);
	CHECK_EQ(dmg_read(d, piece, 2, len - 1), -1);
	free(got);
	dmg_close(d);
}

/* ---- ADC ---------------------------------------------------------------------------- */

/* ADC, as Disk Copy writes it: literal runs, copies from up to 1K back (2 bytes) or 64K back (3) */
static size_t adc_encode(const uint8_t *in, size_t n, uint8_t *out)
{
	static uint32_t last[4096];
	memset(last, 0xFF, sizeof last);
	size_t o = 0, lit = 0, i = 0;
	while (i < n) {
		size_t best = 0, dist = 0;
		if (i + 3 <= n) {
			unsigned h = (in[i] * 33u * 33u + in[i + 1] * 33u + in[i + 2]) & 4095;
			uint32_t cand[2] = { last[h], i ? (uint32_t)i - 1 : UINT32_MAX };
			last[h] = (uint32_t)i;
			for (int c = 0; c < 2; c++) {
				if (cand[c] == UINT32_MAX || cand[c] >= i || i - cand[c] > 65536)
					continue;
				size_t len = 0;
				while (i + len < n && len < 67 && in[cand[c] + len] == in[i + len])
					len++;
				if (len > best)
					best = len, dist = i - cand[c];
			}
		}
		bool two = best >= 3 && dist <= 1024, three = best >= 4;
		if (!two && !three) {
			out[o + 1 + lit++] = in[i++];
			if (lit == 128) {
				out[o] = 0xFF;
				o += 1 + lit, lit = 0;
			}
			continue;
		}
		if (lit)
			out[o] = (uint8_t)(0x80 | (lit - 1)), o += 1 + lit, lit = 0;
		if (two && (!three || best <= 18)) {
			best = best > 18 ? 18 : best;
			out[o++] = (uint8_t)((best - 3) << 2 | (dist - 1) >> 8);
			out[o++] = (uint8_t)(dist - 1);
		} else {
			out[o++] = (uint8_t)(0x40 | (best - 4));
			out[o++] = (uint8_t)((dist - 1) >> 8);
			out[o++] = (uint8_t)(dist - 1);
		}
		i += best;
	}
	if (lit)
		out[o] = (uint8_t)(0x80 | (lit - 1)), o += 1 + lit;
	return o;
}

static void test_adc(const uint8_t *vol)
{
	uint8_t *packed = malloc(2 * VOLUME), *back = malloc(VOLUME);
	size_t n = adc_encode(vol, VOLUME, packed);
	CHECK(n < VOLUME / 4);
	CHECK_EQ(dmg_adc(packed, n, back, VOLUME), VOLUME);
	CHECK(!memcmp(back, vol, VOLUME));
	/* noise: literals; and what's damaged */
	for (size_t k = 0; k < 5000; k++)
		back[k] = (uint8_t)(k * 2654435761u >> 13);
	memcpy(back + 5000, back, 3000);
	uint8_t *again = malloc(8000);
	n = adc_encode(back, 8000, packed);
	CHECK_EQ(dmg_adc(packed, n, again, 8000), 8000);
	CHECK(!memcmp(again, back, 8000));
	static const uint8_t far_back[] = { 0x80, 0x41, 0x40, 0x00, 0x05 }, cut[] = { 0x85, 1, 2 };
	CHECK_EQ(dmg_adc(far_back, sizeof far_back, again, 100), -1);
	CHECK_EQ(dmg_adc(cut, sizeof cut, again, 100), -1);
	CHECK_EQ(dmg_adc(packed, n, again, 100), -1);   /* (more than fits) */
	free(again);
	free(packed);
	free(back);
}

/* ---- UDIF ------------------------------------------------------------------------------- */

static void test_fixtures(const uint8_t *vol)
{
	static const struct { const char *file, *kind; } images[] = {
		{ "packed-zlib.dmg", "zlib" }, { "packed-bzip2.dmg", "bzip2" }, { "packed-lzfse.dmg", "LZFSE" },
		{ "packed-lzma.dmg", "LZMA" }, { "packed-adc.dmg", "ADC" },
	};
	for (size_t k = 0; k < sizeof images / sizeof images[0]; k++)
		check_image(fixture(images[k].file), vol, VOLUME, images[k].kind);
}

struct chunk_plan {
	uint32_t kind;
	uint32_t sector, count;         /* in the image */
};

/* zlib with stored blocks (no compression): what a zlib chunk may hold */
static size_t zlib_stored(const uint8_t *in, size_t n, uint8_t *out)
{
	size_t o = 0;
	uint32_t a = 1, b = 0;
	out[o++] = 0x78, out[o++] = 0x01;
	for (size_t at = 0; at < n || !n; at += 65535) {
		size_t len = n - at < 65535 ? n - at : 65535;
		out[o++] = at + len >= n;
		out[o++] = (uint8_t)len, out[o++] = (uint8_t)(len >> 8);
		out[o++] = (uint8_t)~len, out[o++] = (uint8_t)(~len >> 8);
		memcpy(out + o, in + at, len);
		o += len;
		if (!n)
			break;
	}
	for (size_t k = 0; k < n; k++)
		a = (a + in[k]) % 65521, b = (b + a) % 65521;
	put_be32(out + o, b << 16 | a);
	return o + 4;
}

/*
 * A partition's block table ('mish'), its chunks' data appended to `data`
 * (at *at), counted from `base` (its own data offset, in early images; 0 in
 * later ones, whose offsets are from the start of the data fork)
 */
static size_t mish(uint8_t *m, const uint8_t *vol, const struct chunk_plan *plan, unsigned n, uint32_t first,
		   uint32_t count, uint8_t *data, size_t *at, uint64_t base)
{
	memset(m, 0, 204);
	put_be32(m, 0x6D697368u);
	put_be32(m + 4, 1);
	put_be64(m + 8, first);
	put_be64(m + 16, count);
	put_be64(m + 24, base);
	put_be32(m + 200, n + 1);
	for (unsigned k = 0; k <= n; k++) {
		uint8_t *e = m + 204 + 40 * k;
		memset(e, 0, 40);
		if (k == n) {
			put_be32(e, 0xFFFFFFFFu);
			put_be64(e + 8, count);
			continue;
		}
		const struct chunk_plan *p = &plan[k];
		const uint8_t *src = vol + (size_t)(first + p->sector) * 512;
		size_t len = 0;
		if (p->kind == 1)
			memcpy(data + *at, src, len = (size_t)p->count * 512);
		else if (p->kind == 0x80000004u)
			len = adc_encode(src, (size_t)p->count * 512, data + *at);
		else if (p->kind == 0x80000005u)
			len = zlib_stored(src, (size_t)p->count * 512, data + *at);
		put_be32(e, p->kind);
		put_be64(e + 8, p->sector);
		put_be64(e + 16, p->count);
		put_be64(e + 24, *at - base);
		put_be64(e + 32, len);
		*at += len;
	}
	return 204 + 40 * (size_t)(n + 1);
}

static void base64(const uint8_t *in, size_t n, char *out)
{
	static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;
	for (size_t k = 0; k < n; k += 3) {
		uint32_t v = (uint32_t)in[k] << 16 | (k + 1 < n ? in[k + 1] << 8 : 0) | (k + 2 < n ? in[k + 2] : 0);
		out[o++] = abc[v >> 18 & 63], out[o++] = abc[v >> 12 & 63];
		out[o++] = k + 1 < n ? abc[v >> 6 & 63] : '=';
		out[o++] = k + 2 < n ? abc[v & 63] : '=';
		if (k % 36 == 33)
			out[o++] = '\n', out[o++] = '\t';
	}
	out[o] = 0;
}

static void koly(uint8_t *k, uint64_t data_len, uint64_t rsrc, uint64_t rsrc_len, uint64_t xml, uint64_t xml_len)
{
	memset(k, 0, 512);
	put_be32(k, 0x6B6F6C79u);
	put_be32(k + 4, 4);
	put_be32(k + 8, 512);
	put_be32(k + 12, 1);
	put_be64(k + 32, data_len);
	put_be64(k + 40, rsrc);
	put_be64(k + 48, rsrc_len);
	put_be32(k + 60, 1);
	put_be64(k + 216, xml);
	put_be64(k + 224, xml_len);
	put_be32(k + 488, 1);
	put_be64(k + 492, SECTORS);
}

/* Two partitions (sectors 0-63 and 64 on), their block tables in an XML property list or in a resource fork */
static void make_udif(const char *name, const uint8_t *vol, bool early)
{
	/* (the volume's sectors 5-11 and 364-463 are zeros) */
	static const struct chunk_plan first[] = {
		{ 1, 0, 5 }, { 0, 5, 7 }, { 0x80000004u, 12, 36 }, { 0x80000005u, 48, 16 },
	};
	static const struct chunk_plan rest[] = {
		{ 0x80000004u, 0, 300 }, { 2, 300, 100 }, { 0x7FFFFFFEu, 400, 0 }, { 0x80000005u, 400, SECTORS - 464 },
	};
	uint8_t *data = malloc(4 * VOLUME), m[2][2048];
	size_t at = 0, n[2];
	/* (early images: each partition's chunks from its own data offset) */
	n[0] = mish(m[0], vol, first, 4, 0, 64, data, &at, 0);
	size_t second = at;
	n[1] = mish(m[1], vol + 0, rest, 4, 64, SECTORS - 64, data, &at, early ? second : 0);
	size_t data_len = at, tail;
	if (early) {
		struct rsrc r[2] = { { 0x626C6B78u, 0, m[0], (uint32_t)n[0] }, { 0x626C6B78u, 1, m[1], (uint32_t)n[1] } };
		tail = rsrc_build(r, 2, data + at, 3 * VOLUME - at);
		koly(data + at + tail, data_len, data_len, tail, 0, 0);
	} else {
		char *xml = (char *)data + at, b64[2][3000];
		base64(m[0], n[0], b64[0]);
		base64(m[1], n[1], b64[1]);
		tail = (size_t)sprintf(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
				      "\t<key>resource-fork</key>\n\t<dict>\n\t\t<key>blkx</key>\n\t\t<array>\n"
				      "\t\t\t<dict>\n\t\t\t\t<key>Data</key>\n\t\t\t\t<data>\n\t%s\n\t\t\t\t</data>\n"
				      "\t\t\t\t<key>Name</key>\n\t\t\t\t<string>one</string>\n\t\t\t</dict>\n"
				      "\t\t\t<dict>\n\t\t\t\t<key>Data</key>\n\t\t\t\t<data>\n\t%s\n\t\t\t\t</data>\n"
				      "\t\t\t</dict>\n\t\t</array>\n\t</dict>\n</dict>\n</plist>\n", b64[0], b64[1]);
		koly(data + at + tail, data_len, 0, 0, data_len, tail);
	}
	write_file(name, data, at + tail + 512);
	free(data);
}

static void test_udif(const uint8_t *vol)
{
	make_udif("plist.dmg", vol, false);
	check_image(path("plist.dmg"), vol, VOLUME, "ADC, zlib");
	make_udif("early.dmg", vol, true);
	check_image(path("early.dmg"), vol, VOLUME, "UDIF image (ADC, zlib)");

	/* in MacBinary */
	FILE *in = fopen(path("early.dmg"), "rb"), *out = fopen(path("early.bin"), "wb");
	uint8_t h[128] = { 0 }, buf[4096];
	fseek(in, 0, SEEK_END);
	long len = ftell(in);
	fseek(in, 0, SEEK_SET);
	h[1] = 9;
	memcpy(h + 2, "early.dmg", 9);
	memcpy(h + 65, "devrddsk", 8);
	put_be32(h + 83, (uint32_t)len);
	put_be16(h + 124, 0);
	uint16_t crc = 0;
	for (int k = 0; k < 124; k++) {
		crc ^= (uint16_t)(h[k] << 8);
		for (int b = 0; b < 8; b++)
			crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
	}
	put_be16(h + 124, crc);
	fwrite(h, 1, 128, out);
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, in)))
		fwrite(buf, 1, n, out);
	fclose(in);
	fclose(out);
	check_image(path("early.bin"), vol, VOLUME, "UDIF");

	/* what isn't read: encrypted, segmented, chunks of an unknown kind, cut short */
	static const struct { const char *head, *why; } bad[] = {
		{ "encrcdsa", "encrypted" }, { "segment", "segmented" }, { "kind", "kind Blue doesn't know" },
		{ "short", "cut short" },
	};
	for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
		uint8_t img[4096] = { 0 }, m[300];
		size_t at = 0, end = 4096;
		struct chunk_plan p = { 1, 0, 2 };
		mish(m, vol, &p, 1, 0, 2, img, &at, 0);
		struct rsrc r = { 0x626C6B78u, 0, m, 204 + 80 };
		size_t fork = rsrc_build(&r, 1, img + 1024, 1024);
		koly(img + end - 512, 1024, 1024, fork, 0, 0);
		if (!strcmp(bad[k].head, "encrcdsa"))
			memcpy(img, "encrcdsa", 8);
		else if (!strcmp(bad[k].head, "segment"))
			put_be32(img + end - 512 + 60, 3);
		else if (!strcmp(bad[k].head, "kind"))
			put_be32(img + 1024 + 256 + 4 + 204, 0x80000009u);
		else
			put_be64(img + 1024 + 256 + 4 + 204 + 32, 8192);
		write_file("bad.dmg", img, end);
		char why[300] = "";
		struct dmg *d;
		CHECK_EQ(dmg_open(path("bad.dmg"), &d, why, sizeof why), -1);
		if (!strstr(why, bad[k].why)) {
			fprintf(stderr, "dmg: \"%s\", not %s\n", why, bad[k].why);
			CHECK(!"the reason");
		}
	}
	unlink(path("plist.dmg"));
	unlink(path("early.dmg"));
	unlink(path("early.bin"));
	unlink(path("bad.dmg"));
}

/* ---- NDIF ---------------------------------------------------------------------------- */

/* A Disk Copy 6 image: its data fork (ADC, raw and zero chunks) and its resource fork with the 'bcem' resource */
static void make_ndif(const uint8_t *vol, uint8_t *data, size_t *data_len, uint8_t *fork, size_t *fork_len,
		      unsigned version, uint8_t ken)
{
	static const struct { uint32_t start; uint8_t type; } chunks[] = {
		{ 0, 0x83 }, { 5, 0x00 }, { 12, 0x83 }, { 100, 0x02 }, { 140, 0x83 }, { SECTORS, 0xFF },
	};
	unsigned n = sizeof chunks / sizeof chunks[0];
	uint8_t bcem[128 + 12 * 6] = { 0 };
	put_be16(bcem, (uint16_t)version);
	bcem[4] = 6;
	memcpy(bcem + 5, "Packed", 6);
	put_be32(bcem + 0x44, SECTORS);
	put_be32(bcem + 0x48, 2048);
	put_be32(bcem + 0x7C, n);
	size_t at = 0;
	for (unsigned k = 0; k < n; k++) {
		uint8_t *e = bcem + 128 + 12 * k;
		uint8_t type = ken && k == 2 ? ken : chunks[k].type;
		put_be32(e, chunks[k].start << 8 | type);
		size_t len = 0;
		if (chunks[k].type != 0xFF && chunks[k].type) {
			const uint8_t *src = vol + (size_t)chunks[k].start * 512;
			size_t bytes = (size_t)(chunks[k + 1].start - chunks[k].start) * 512;
			if (chunks[k].type == 0x02)
				memcpy(data + at, src, len = bytes);
			else
				len = adc_encode(src, bytes, data + at);
		}
		put_be32(e + 4, (uint32_t)at);
		put_be32(e + 8, (uint32_t)len);
		at += len;
	}
	*data_len = at;
	struct rsrc r = { 0x6263656Du, 128, bcem, sizeof bcem };
	*fork_len = rsrc_build(&r, 1, fork, 4096);
}

static void test_ndif(const uint8_t *vol)
{
	uint8_t *data = malloc(2 * VOLUME), fork[4096], h[128];
	size_t len, fork_len;
	make_ndif(vol, data, &len, fork, &fork_len, 11, 0);

	/* alone: its data fork isn't an image Blue knows */
	char why[300] = "";
	struct dmg *d;
	write_file("Disk.img", data, len);
	CHECK_EQ(dmg_open(path("Disk.img"), &d, why, sizeof why), 0);

	/* with an AppleDouble file beside it, as copied off a Mac */
	uint8_t ad[26 + 12 + 4096] = { 0 };
	put_be32(ad, 0x00051607u);
	put_be32(ad + 4, 0x00020000u);
	put_be16(ad + 24, 1);
	put_be32(ad + 26, 2);
	put_be32(ad + 30, 38);
	put_be32(ad + 34, (uint32_t)fork_len);
	memcpy(ad + 38, fork, fork_len);
	write_file("._Disk.img", ad, 38 + fork_len);
	check_image(path("Disk.img"), vol, VOLUME, "Disk Copy 6 (NDIF) image (ADC)");
	unlink(path("._Disk.img"));
	unlink(path("Disk.img"));

	/* MacBinary */
	FILE *f = fopen(path("Disk.bin"), "wb");
	memset(h, 0, sizeof h);
	h[1] = 8;
	memcpy(h + 2, "Disk.img", 8);
	memcpy(h + 65, "rohddcpy", 8);
	put_be32(h + 83, (uint32_t)len);
	put_be32(h + 87, (uint32_t)fork_len);
	fwrite(h, 1, 128, f);
	fwrite(data, 1, len, f);
	for (size_t k = len; k % 128; k++)
		fputc(0, f);
	fwrite(fork, 1, fork_len, f);
	fclose(f);
	check_image(path("Disk.bin"), vol, VOLUME, "NDIF");
	unlink(path("Disk.bin"));

	/* AppleSingle */
	f = fopen(path("Disk.as"), "wb");
	uint8_t as[26 + 24] = { 0 };
	put_be32(as, 0x00051600u);
	put_be32(as + 4, 0x00020000u);
	put_be16(as + 24, 2);
	put_be32(as + 26, 1), put_be32(as + 30, 50), put_be32(as + 34, (uint32_t)len);
	put_be32(as + 38, 2), put_be32(as + 42, 50 + (uint32_t)len), put_be32(as + 46, (uint32_t)fork_len);
	fwrite(as, 1, sizeof as, f);
	fwrite(data, 1, len, f);
	fwrite(fork, 1, fork_len, f);
	fclose(f);
	check_image(path("Disk.as"), vol, VOLUME, "NDIF");
	unlink(path("Disk.as"));

	/* KenCode, and a version Blue doesn't read */
	static const struct { unsigned version; uint8_t ken; const char *why; } bad[] = {
		{ 11, 0x80, "KenCode" }, { 2, 0, "version" },
	};
	for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
		make_ndif(vol, data, &len, fork, &fork_len, bad[k].version, bad[k].ken);
		write_file("Disk.img", data, len);
		put_be32(ad + 34, (uint32_t)fork_len);
		memcpy(ad + 38, fork, fork_len);
		write_file("._Disk.img", ad, 38 + fork_len);
		CHECK_EQ(dmg_open(path("Disk.img"), &d, why, sizeof why), -1);
		CHECK(strstr(why, bad[k].why) != NULL);
		unlink(path("._Disk.img"));
		unlink(path("Disk.img"));
	}
	free(data);
}

/* ---- in the drives ------------------------------------------------------------------- */

static void test_in_drives(const uint8_t *vol)
{
	char why[400] = "";
	struct floppy_image img;
	struct dmg *d = NULL;

	/* a floppy, read-only */
	CHECK_EQ(floppy_packed(fixture("packed-lzfse.dmg"), &d, &img, why, sizeof why), 1);
	CHECK_EQ(img.kind, FLOPPY_PACKED);
	CHECK_EQ(img.data_size, VOLUME);
	CHECK(!strcmp(img.volume, "HFS \"Packed\""));
	CHECK(!strcmp(img.format, "UDIF image (LZFSE)"));
	dmg_close(d);
	CHECK_EQ(floppy_check(fixture("packed-adc.dmg"), why, sizeof why), 0);
	static struct disk fl, cd1, cd2;
	disk_open_floppy(&fl, NULL);
	CHECK_EQ(disk_insert(&fl, fixture("packed-bzip2.dmg"), why, sizeof why), 0);
	CHECK(fl.read_only);
	CHECK(fl.packed != NULL);
	uint8_t buf[1024];
	struct disk_reader r = { &fl, 0 };
	CHECK_EQ(disk_read(&r, buf, sizeof buf, 1024), 0);
	CHECK(!memcmp(buf, vol + 1024, sizeof buf));
	disk_eject(&fl);
	CHECK(fl.packed == NULL);
	CHECK_EQ(floppy_check(fixture("game.chd"), why, sizeof why), -1);

	/* where a dropped image goes: a floppy's size unpacked to the floppy drive, a CHD to a CD-ROM drive */
	CHECK_EQ(disk_open_cdrom(&cd1, NULL, NULL), 0);
	CHECK_EQ(disk_open_cdrom(&cd2, NULL, NULL), 0);
	struct drives dr = { .floppy = &fl, .cd = { &cd1, &cd2 }, .cds = 2 };
	CHECK(drives_pick(&dr, fixture("packed-zlib.dmg"), DRIVES_ANY, why, sizeof why) == &fl);
	CHECK(drives_pick(&dr, fixture("data-cdzl.chd"), DRIVES_ANY, why, sizeof why) == &cd1);

	/* in a CD-ROM drive: its data, through the disc */
	CHECK_EQ(disk_check_cd(fixture("packed-lzma.dmg"), why, sizeof why), 0);
	CHECK_EQ(disk_insert(&cd1, fixture("packed-lzma.dmg"), why, sizeof why), 0);
	CHECK(cd1.disc != NULL && strstr(cd1.disc->kind, "UDIF image (LZMA)"));
	struct disk_reader rc = { &cd1, 0 };
	CHECK_EQ(disk_read(&rc, buf, sizeof buf, 2048), 0);
	CHECK(!memcmp(buf, vol + 2048, sizeof buf));
	disk_close(&cd1);
	disk_close(&cd2);
	disk_close(&fl);

	/* a disk: read-only, its volume found by hfsread */
	struct disk dk;
	CHECK_EQ(disk_open(&dk, NULL, fixture("packed-zlib.dmg"), false, false), 0);
	CHECK(dk.read_only);
	CHECK_EQ(dk.partitions, 1);
	struct disk_reader rd = { &dk, 0 };
	struct hfsread_volume v;
	struct hfsread_file file;
	CHECK_EQ(hfsread_open_with(&v, disk_read, &rd, dk.part[0].size), 0);
	CHECK_EQ(hfsread_find(&v, 2, FOURCC('T', 'E', 'X', 'T'), &file), 1);
	CHECK(!strcmp(file.name, "Read Me"));
	disk_close(&dk);
}

/* A GUID partition table (as images made on Macs today have): its HFS+ partition is a drive */
static void test_gpt(void)
{
	uint8_t *img = calloc(1, 64 * 1024);
	memcpy(img + 512, "EFI PART", 8);
	put_le32(img + 512 + 72, 2);            /* the entries at LBA 2 */
	put_le32(img + 512 + 80, 128);
	put_le32(img + 512 + 84, 128);
	static const uint8_t hfs[16] = { 0x00, 0x53, 0x46, 0x48, 0x00, 0x00, 0xAA, 0x11,
					 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC };
	uint8_t *e = img + 1024 + 128;          /* (the second entry: the first is empty) */
	memcpy(e, hfs, 16);
	put_le32(e + 32, 40);
	put_le32(e + 40, 119);
	const char *name = "Files";
	for (int k = 0; name[k]; k++)
		e[56 + 2 * k] = (uint8_t)name[k];
	write_file("gpt.img", img, 64 * 1024);
	struct disk d;
	CHECK_EQ(disk_open(&d, NULL, path("gpt.img"), true, false), 0);
	CHECK_EQ(d.partitions, 1);
	CHECK_EQ(d.part[0].offset, 40 * 512);
	CHECK_EQ(d.part[0].size, 80 * 512);
	CHECK(!strcmp(d.part[0].name, "Files"));
	disk_close(&d);
	unlink(path("gpt.img"));
	free(img);
}

void test_dmg(void)
{
	const char *tmp = getenv("TMPDIR");
	snprintf(dir, sizeof dir, "%s/blue-dmg-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
	if (!mkdtemp(dir)) {
		CHECK(!"a temporary folder");
		return;
	}
	uint8_t *vol = volume();
	CHECK(vol != NULL);
	if (vol) {
		test_adc(vol);
		test_fixtures(vol);
		test_udif(vol);
		test_ndif(vol);
		test_in_drives(vol);
		test_gpt();
	}
	free(vol);
	rmdir(dir);
}
