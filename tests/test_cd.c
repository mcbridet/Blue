/*
 * CD images and the drive's audio player: synthetic discs made here (an HFS
 * data track made with hfs.c, in raw 2352-byte sectors or 2048-byte ones,
 * and audio tracks of test patterns in .bin, MOTOROLA, WAVE and AIFF files)
 * read through cue sheets; the player's positions, pauses, lists, volume
 * and pacing by guest time; and its sound through the codec's analog input.
 */
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/awacs.h"
#include "../src/be.h"
#include "../src/cdaudio.h"
#include "../src/cdimage.h"
#include "../src/hfs.h"
#include "../src/hfsread.h"
#include "../src/mem.h"
#include "test.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

#define DATA_SECTORS 400u               /* the HFS volume: 800K */
#define GUEST_HZ 16600000u              /* Blue's timebase */

static char dir[1024];
static char made[64][1100];
static unsigned nmade;

static const char *path(const char *name)
{
	static char p[1100];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	return p;
}

static FILE *create(const char *name)
{
	if (nmade < 64)
		snprintf(made[nmade++], sizeof made[0], "%s", path(name));
	return fopen(path(name), "wb");
}

static void cleanup(void)
{
	for (unsigned k = 0; k < nmade; k++)
		unlink(made[k]);
	nmade = 0;
}

static void write_text(const char *name, const char *text)
{
	FILE *f = create(name);
	fputs(text, f);
	fclose(f);
}

/* The test pattern of audio track n: left counts frames up from n * 1000, right is a tone */
static void pattern(unsigned n, uint32_t frame, int16_t *l, int16_t *r)
{
	*l = (int16_t)(n * 1000 + frame % 20000);
	*r = (int16_t)(8000 * sin(2 * M_PI * (220.0 * n) * frame / CD_RATE));
}

static void put_samples(FILE *f, unsigned n, uint32_t frames, bool big)
{
	for (uint32_t k = 0; k < frames; k++) {
		int16_t s[2];
		pattern(n, k, &s[0], &s[1]);
		for (int c = 0; c < 2; c++) {
			uint8_t b[2] = { (uint8_t)s[c], (uint8_t)((uint16_t)s[c] >> 8) };
			if (big)
				fputc(b[1], f), fputc(b[0], f);
			else
				fputc(b[0], f), fputc(b[1], f);
		}
	}
}

static void put_silence(FILE *f, uint32_t sectors)
{
	for (uint32_t k = 0; k < sectors * CD_SECTOR; k++)
		fputc(0, f);
}

/* An HFS volume of DATA_SECTORS * 2048 bytes with one file, read into memory */
static uint8_t *hfs_volume(void)
{
	static const char text[] = "Hello from the data track";
	struct hfs_file file = { "Read Me", FOURCC('T', 'E', 'X', 'T'), FOURCC('t', 't', 'x', 't'), 0, 0, 0,
				 (const uint8_t *)text, NULL, sizeof text - 1, 0 };
	char p[1100];
	snprintf(p, sizeof p, "%s/volume.img", dir);
	int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
	uint8_t *v = malloc(DATA_SECTORS * CD_DATA);
	if (fd < 0 || !v || ftruncate(fd, DATA_SECTORS * CD_DATA) ||
	    hfs_format(fd, DATA_SECTORS * CD_DATA, "Test CD", 0xB0000000u, &file, 1) ||
	    pread(fd, v, DATA_SECTORS * CD_DATA, 0) != (ssize_t)(DATA_SECTORS * CD_DATA)) {
		free(v);
		v = NULL;
	}
	if (fd >= 0)
		close(fd);
	unlink(p);
	return v;
}

/* The volume as a mode 1 track: raw sectors (sync, header, data, no EDC/ECC), or the data alone */
static void put_data_track(FILE *f, const uint8_t *v, bool raw)
{
	for (uint32_t lba = 0; lba < DATA_SECTORS; lba++) {
		if (raw) {
			uint8_t h[16] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
			uint8_t m, s, fr;
			cd_msf(lba, &m, &s, &fr);
			h[12] = cd_bcd(m), h[13] = cd_bcd(s), h[14] = cd_bcd(fr), h[15] = 1;
			fwrite(h, 1, 16, f);
		}
		fwrite(v + (size_t)lba * CD_DATA, 1, CD_DATA, f);
		if (raw)
			for (int k = 0; k < 288; k++)
				fputc(0, f);
	}
}

static int read_volume(void *ctx, void *buf, size_t len, uint64_t at)
{
	return cdimage_read_data(ctx, buf, len, at);
}

/* Mac OS's volume on the data track, read with hfsread through the image's data */
static void check_volume(struct cdimage *c)
{
	struct hfsread_volume v;
	CHECK_EQ(c->data_size, DATA_SECTORS * CD_DATA);
	if (hfsread_open_with(&v, read_volume, c, c->data_size)) {
		CHECK(!"the data track holds the HFS volume");
		return;
	}
	struct hfsread_file f;
	CHECK_EQ(hfsread_find(&v, 2, FOURCC('T', 'E', 'X', 'T'), &f), 1);
	CHECK(!strcmp(f.name, "Read Me"));
	CHECK_EQ(f.data.size, 25);
	char text[32] = { 0 };
	CHECK_EQ(cdimage_read_data(c, text, 25, (uint64_t)v.offset + (uint64_t)f.data.ext[0].start * v.block_size), 0);
	CHECK(!strcmp(text, "Hello from the data track"));
}

/* An audio sector read back holds track n's pattern from frame `from` */
static bool audio_is(const struct cdimage *c, uint32_t lba, unsigned n, uint32_t from)
{
	uint8_t s[CD_SECTOR];
	if (cdimage_read_raw(c, lba, 1, s))
		return false;
	for (unsigned k = 0; k < CD_FRAMES; k++) {
		int16_t l, r;
		pattern(n, from + k, &l, &r);
		if ((int16_t)(s[4 * k] | s[4 * k + 1] << 8) != l || (int16_t)(s[4 * k + 2] | s[4 * k + 3] << 8) != r)
			return false;
	}
	return true;
}

static bool silent(const struct cdimage *c, uint32_t lba)
{
	uint8_t s[CD_SECTOR];
	if (cdimage_read_raw(c, lba, 1, s))
		return false;
	for (unsigned k = 0; k < CD_SECTOR; k++)
		if (s[k])
			return false;
	return true;
}

/*
 * One .bin with everything, as most rips: a data track, then audio track 2
 * with its 2 s pregap in the file (INDEX 00), then audio track 3.
 */
static void test_single_bin(const uint8_t *vol)
{
	FILE *f = create("game.bin");
	put_data_track(f, vol, true);
	put_silence(f, 150);
	put_samples(f, 2, 100 * CD_FRAMES, false);
	put_samples(f, 3, 50 * CD_FRAMES + 100, false);        /* (a last sector cut short) */
	fclose(f);
	write_text("Game.cue",
		   "REM a comment\r\n"
		   "CATALOG 0123456789012\r\n"
		   "FILE \"GAME.BIN\" BINARY\r\n"       /* (another case than the file's) */
		   "  TRACK 01 MODE1/2352\r\n"
		   "    INDEX 01 00:00:00\r\n"
		   "  TRACK 02 AUDIO\r\n"
		   "    FLAGS DCP\r\n"
		   "    ISRC USABC9900001\r\n"
		   "    INDEX 00 00:05:25\r\n"         /* 400 */
		   "    INDEX 01 00:07:25\r\n"         /* 550 */
		   "  TRACK 03 AUDIO\r\n"
		   "    INDEX 01 00:08:50\r\n");       /* 650 */
	char why[256] = "";
	struct cdimage *c = cdimage_open(path("Game.cue"), why, sizeof why);
	if (!c) {
		fprintf(stderr, "cdimage: %s\n", why);
		CHECK(!"the cue sheet opens");
		return;
	}
	CHECK_EQ(c->tracks, 3);
	CHECK_EQ(c->sessions, 1);
	CHECK(!strcmp(c->catalog, "0123456789012"));
	const struct cd_track *t = c->track;
	CHECK_EQ(t[0].number, 1);
	CHECK_EQ(t[0].mode, 1);
	CHECK_EQ(t[0].control, CD_CONTROL_DATA);
	CHECK_EQ(t[0].start, 0);
	CHECK_EQ(t[0].end, 400);
	CHECK_EQ(t[1].pregap, 400);
	CHECK_EQ(t[1].start, 550);
	CHECK_EQ(t[1].end, 650);
	CHECK_EQ(t[1].mode, 0);
	CHECK_EQ(t[1].control, CD_CONTROL_COPY);
	CHECK(!strcmp(t[1].isrc, "USABC9900001"));
	CHECK_EQ(t[2].pregap, 650);
	CHECK_EQ(t[2].start, 650);
	CHECK_EQ(t[2].end, 701);
	CHECK_EQ(c->leadout, 701);
	CHECK_EQ(c->data, 0);
	check_volume(c);

	CHECK_EQ(cdimage_track_at(c, 0), 0);
	CHECK_EQ(cdimage_track_at(c, 399), 0);
	CHECK_EQ(cdimage_track_at(c, 400), 1);          /* (its pregap) */
	CHECK_EQ(cdimage_track_at(c, 700), 2);
	CHECK_EQ(cdimage_track_at(c, 701), -1);
	CHECK(!cdimage_audio(c, 10));
	CHECK(cdimage_audio(c, 500));
	CHECK(silent(c, 549));
	CHECK(audio_is(c, 550, 2, 0));
	CHECK(audio_is(c, 649, 2, 99 * CD_FRAMES));
	CHECK(audio_is(c, 650, 3, 0));
	CHECK(audio_is(c, 699, 3, 49 * CD_FRAMES));
	uint8_t s[2 * CD_SECTOR];
	CHECK_EQ(cdimage_read_raw(c, 700, 2, s), 0);    /* the cut-short sector, then the lead-out */
	CHECK_EQ(s[4 * 100], 0);
	CHECK_EQ(s[CD_SECTOR + 7], 0);
	CHECK_EQ(cdimage_read_raw(c, 3, 1, s), 0);      /* a data sector, raw: at 00:02:03 */
	CHECK_EQ(s[1], 0xFF);
	CHECK_EQ(s[13], 0x02);
	CHECK_EQ(s[14], 0x03);
	CHECK_EQ(s[15], 1);
	CHECK(!memcmp(s + 16, vol + 3 * CD_DATA, CD_DATA));
	cdimage_close(c);
}

static void put_le(FILE *f, uint32_t v, int bytes)
{
	for (int k = 0; k < bytes; k++)
		fputc((int)(v >> 8 * k) & 0xFF, f);
}

static void put_be(FILE *f, uint32_t v, int bytes)
{
	for (int k = bytes - 1; k >= 0; k--)
		fputc((int)(v >> 8 * k) & 0xFF, f);
}

static void make_wave(const char *name, unsigned n, uint32_t sectors, uint32_t rate)
{
	FILE *f = create(name);
	uint32_t bytes = sectors * CD_SECTOR;
	fputs("RIFF", f);
	put_le(f, 4 + 26 + 8 + 4 + 8 + bytes, 4);
	fputs("WAVEfmt ", f);
	put_le(f, 16, 4);
	put_le(f, 1, 2);
	put_le(f, 2, 2);
	put_le(f, rate, 4);
	put_le(f, rate * 4, 4);
	put_le(f, 4, 2);
	put_le(f, 16, 2);
	fputs("LIST", f);                               /* (a chunk to pass over) */
	put_le(f, 3, 4);
	fputs("abc", f);
	fputc(0, f);
	fputs("data", f);
	put_le(f, bytes, 4);
	put_samples(f, n, sectors * CD_FRAMES, false);
	fclose(f);
}

/* An AIFF file whose first `quiet` sectors are silence */
static void make_aiff(const char *name, unsigned n, uint32_t quiet, uint32_t sectors)
{
	static const uint8_t rate[10] = { 0x40, 0x0E, 0xAC, 0x44 };
	FILE *f = create(name);
	uint32_t bytes = (quiet + sectors) * CD_SECTOR;
	fputs("FORM", f);
	put_be(f, 4 + 26 + 16 + bytes, 4);
	fputs("AIFFCOMM", f);
	put_be(f, 18, 4);
	put_be(f, 2, 2);
	put_be(f, bytes / 4, 4);
	put_be(f, 16, 2);
	fwrite(rate, 1, 10, f);
	fputs("SSND", f);
	put_be(f, 8 + bytes, 4);
	put_be(f, 0, 4);
	put_be(f, 0, 4);
	put_silence(f, quiet);
	put_samples(f, n, sectors * CD_FRAMES, true);
	fclose(f);
}

/* A track to a file each, in each kind of file, with PREGAP, POSTGAP and an in-file pregap */
static void test_files(const uint8_t *vol)
{
	FILE *f = create("disc (Track 1).bin");
	put_data_track(f, vol, false);
	fclose(f);
	make_wave("Track 2.wav", 2, 100, CD_RATE);
	f = create("track3.bin");
	put_samples(f, 3, 60 * CD_FRAMES, true);
	fclose(f);
	make_aiff("Track 4.aif", 4, 75, 40);
	write_text("disc.cue",
		   "FILE \"disc (Track 1).bin\" BINARY\n"
		   "  TRACK 01 MODE1/2048\n"
		   "    INDEX 01 00:00:00\n"
		   "FILE \"Track 2.wav\" WAVE\n"
		   "  TRACK 02 AUDIO\n"
		   "    PREGAP 00:02:00\n"
		   "    INDEX 01 00:00:00\n"
		   "FILE track3.bin MOTOROLA\n"
		   "  TRACK 03 AUDIO\n"
		   "    INDEX 01 00:00:00\n"
		   "    POSTGAP 00:01:00\n"
		   "FILE \"Track 4.aif\" AIFF\n"
		   "  TRACK 04 AUDIO\n"
		   "    FLAGS PRE 4CH\n"
		   "    INDEX 00 00:00:00\n"
		   "    INDEX 01 00:01:00\n");
	char why[256] = "";
	struct cdimage *c = cdimage_open(path("disc.cue"), why, sizeof why);
	if (!c) {
		fprintf(stderr, "cdimage: %s\n", why);
		CHECK(!"the cue sheet opens");
		return;
	}
	const struct cd_track *t = c->track;
	CHECK_EQ(c->tracks, 4);
	CHECK_EQ(t[0].end, 400);
	CHECK_EQ(t[1].pregap, 400);
	CHECK_EQ(t[1].start, 550);
	CHECK_EQ(t[1].end, 650);
	CHECK_EQ(t[2].start, 650);
	CHECK_EQ(t[2].end, 785);
	CHECK_EQ(t[3].pregap, 785);
	CHECK_EQ(t[3].start, 860);
	CHECK_EQ(t[3].end, 900);
	CHECK_EQ(t[3].control, CD_CONTROL_PREEMPHASIS | CD_CONTROL_FOUR);
	CHECK_EQ(c->leadout, 900);
	check_volume(c);
	CHECK(silent(c, 400));
	CHECK(silent(c, 549));
	CHECK(audio_is(c, 550, 2, 0));
	CHECK(audio_is(c, 649, 2, 99 * CD_FRAMES));
	CHECK(audio_is(c, 650, 3, 0));                  /* (MOTOROLA: big-endian in the file) */
	CHECK(audio_is(c, 709, 3, 59 * CD_FRAMES));
	CHECK(silent(c, 710));                          /* the POSTGAP */
	CHECK(silent(c, 859));                          /* track 4's pregap, in its file */
	CHECK(audio_is(c, 860, 4, 0));
	CHECK(audio_is(c, 899, 4, 39 * CD_FRAMES));

	/* the Q subchannel: track, index, time in the track (counting down in a pregap), time on the disc */
	uint8_t q[10];
	cdimage_q(c, 548, q);
	CHECK_EQ(q[0], 0x01);                           /* control 0 (audio), ADR 1 */
	CHECK_EQ(q[1], 0x02);
	CHECK_EQ(q[2], 0);
	CHECK_EQ(q[5], 2);                              /* 2 frames to go */
	CHECK_EQ(q[7], 0x00);                           /* 548 + 150: 00:09:23 */
	CHECK_EQ(q[8], 0x09);
	CHECK_EQ(q[9], 0x23);
	cdimage_q(c, 860 + 30, q);
	CHECK_EQ(q[0], 0x91);                           /* four channels, pre-emphasis */
	CHECK_EQ(q[1], 0x04);
	CHECK_EQ(q[2], 1);
	CHECK_EQ(q[4], 0x00);                           /* 00:00:30 into it */
	CHECK_EQ(q[5], 0x30);
	cdimage_q(c, 10, q);
	CHECK_EQ(q[0], 0x41);
	cdimage_q(c, 905, q);
	CHECK_EQ(q[1], 0xAA);                           /* the lead-out */
	CHECK_EQ(q[5], 5);
	uint8_t sub[CD_SUBCODE];
	cdimage_subcode(c, 548, sub);
	CHECK_EQ(sub[0], 0x80);                         /* P on (a pregap), Q's first bit 0 */
	CHECK_EQ(sub[7] & 0x40, 0x40);                  /* ADR 1: Q's bit 7 */
	cdimage_subcode(c, 600, sub);
	CHECK_EQ(sub[0] & 0x80, 0);
	cdimage_close(c);
}

/* A CD-Extra disc: audio in session 1, the data in session 2 (mode 2, form 1) */
static void test_sessions(const uint8_t *vol)
{
	FILE *f = create("extra.bin");
	put_samples(f, 1, 50 * CD_FRAMES, false);
	put_samples(f, 2, 50 * CD_FRAMES, false);
	fclose(f);
	f = create("extra (Track 3).bin");
	for (uint32_t k = 0; k < DATA_SECTORS; k++) {
		static const uint8_t sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
		static const uint8_t sub[8] = { 0, 0, 8, 0, 0, 0, 8, 0 };
		fwrite(sync, 1, 12, f);
		put_be(f, 0x000000, 3);
		fputc(2, f);
		fwrite(sub, 1, 8, f);
		fwrite(vol + (size_t)k * CD_DATA, 1, CD_DATA, f);
		for (int j = 0; j < 280; j++)
			fputc(0, f);
	}
	fclose(f);
	write_text("extra.cue",
		   "REM SESSION 01\n"
		   "FILE \"extra.bin\" BINARY\n"
		   "  TRACK 01 AUDIO\n"
		   "    INDEX 01 00:00:00\n"
		   "  TRACK 02 AUDIO\n"
		   "    INDEX 01 00:00:50\n"
		   "REM SESSION 02\n"
		   "FILE \"extra (Track 3).bin\" BINARY\n"
		   "  TRACK 03 MODE2/2352\n"
		   "    INDEX 01 00:00:00\n");
	char why[256] = "";
	struct cdimage *c = cdimage_open(path("extra.cue"), why, sizeof why);
	if (!c) {
		fprintf(stderr, "cdimage: %s\n", why);
		CHECK(!"the cue sheet opens");
		return;
	}
	CHECK_EQ(c->sessions, 2);
	CHECK_EQ(c->track[1].end, 100);
	CHECK_EQ(c->track[2].session, 2);
	CHECK_EQ(c->track[2].start, 100 + 11400);       /* after session 1's lead-out, session 2's lead-in */
	CHECK_EQ(c->leadout, 11500 + DATA_SECTORS);
	CHECK_EQ(c->data, 2);
	check_volume(c);
	CHECK_EQ(cdimage_track_at(c, 5000), -1);
	uint8_t q[10];
	cdimage_q(c, 110, q);
	CHECK_EQ(q[1], 0xAA);                           /* session 1's lead-out */
	CHECK_EQ(q[5], 0x10);
	cdimage_close(c);
}

/* Plain images, and what isn't taken */
static void test_plain_and_errors(const uint8_t *vol)
{
	FILE *f = create("plain.iso");
	put_data_track(f, vol, false);
	fclose(f);
	char why[256] = "";
	struct cdimage *c = cdimage_open(path("plain.iso"), why, sizeof why);
	CHECK(c != NULL);
	if (c) {
		CHECK_EQ(c->tracks, 1);
		CHECK_EQ(c->track[0].end, DATA_SECTORS);
		CHECK_EQ(c->leadout, DATA_SECTORS);
		check_volume(c);
		uint8_t s[CD_SECTOR];
		CHECK_EQ(cdimage_read_raw(c, 7, 1, s), 0);      /* raw: made up around the data */
		CHECK_EQ(s[15], 1);
		CHECK(!memcmp(s + 16, vol + 7 * CD_DATA, CD_DATA));
		CHECK_EQ(s[16 + CD_DATA], 0);
		cdimage_close(c);
	}
	CHECK(cdimage_is_cue("a/b.CUE"));
	CHECK(!cdimage_is_cue("a/b.iso"));

	make_wave("slow.wav", 1, 4, 22050);
	static const struct { const char *cue, *why; } bad[] = {
		{ "FILE \"missing.bin\" BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n", "missing.bin" },
		{ "FILE \"plain.iso\" MP3\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n", "MP3" },
		{ "FILE \"plain.iso\" BINARY\nTRACK 01 MODE1/2448\nINDEX 01 00:00:00\n", "track type" },
		{ "FILE \"plain.iso\" BINARY\nTRACK 01 AUDIO\n", "INDEX 01" },
		{ "FILE \"plain.iso\" BINARY\nTRACK 02 AUDIO\nINDEX 01 00:00:00\nTRACK 01 AUDIO\nINDEX 01 00:01:00\n",
		  "go up" },
		{ "FILE \"slow.wav\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n", "44.1 kHz" },
		{ "FILE \"Track 2.wav\" WAVE\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n", "data in an audio file" },
		{ "TRACK 01 AUDIO\n", "before FILE" },
		{ "REM nothing\n", "no tracks" },
	};
	for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
		write_text("bad.cue", bad[k].cue);
		why[0] = 0;
		c = cdimage_open(path("bad.cue"), why, sizeof why);
		CHECK(c == NULL);
		if (!strstr(why, bad[k].why)) {
			fprintf(stderr, "cdimage: \"%s\" for %s\n", why, bad[k].why);
			CHECK(!"the reason is given");
		}
		cdimage_close(c);
	}

	uint8_t m, sec, fr;
	cd_msf(0, &m, &sec, &fr);
	CHECK(m == 0 && sec == 2 && fr == 0);
	cd_msf(74 * 4500 - 151, &m, &sec, &fr);
	CHECK(m == 73 && sec == 59 && fr == 74);
	CHECK_EQ(cd_lba(73, 59, 74), 74 * 4500 - 151);
	CHECK_EQ(cd_lba(0, 1, 0), 0);
	CHECK_EQ(cd_bcd(74), 0x74);
	CHECK_EQ(cd_unbcd(0x59), 59);
}

static bool begin(void)
{
	const char *tmp = getenv("TMPDIR");
	snprintf(dir, sizeof dir, "%s/blue-cd-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
	if (!mkdtemp(dir)) {
		CHECK(!"a temporary folder");
		return false;
	}
	return true;
}

static void end(void)
{
	cleanup();
	rmdir(dir);
}

void test_cdimage(void)
{
	if (!begin())
		return;
	uint8_t *vol = hfs_volume();
	CHECK(vol != NULL);
	if (vol) {
		test_single_bin(vol);
		test_files(vol);
		test_sessions(vol);
		test_plain_and_errors(vol);
	}
	free(vol);
	end();
}

/* ---- the player ---------------------------------------------------------------- */

static struct cdaudio player;

/* An audio disc: tracks 1-3 of 20 sectors each (LBA 0, 20, 40), then a data track (60-69) */
static struct cdimage *audio_disc(void)
{
	FILE *f = create("audio.bin");
	for (unsigned n = 1; n <= 3; n++)
		put_samples(f, n, 20 * CD_FRAMES, false);
	fclose(f);
	f = create("audio.iso");
	for (unsigned k = 0; k < 10 * CD_DATA; k++)
		fputc(0x55, f);
	fclose(f);
	write_text("audio.cue",
		   "FILE audio.bin BINARY\n"
		   "TRACK 01 AUDIO\nINDEX 01 00:00:00\n"
		   "TRACK 02 AUDIO\nINDEX 01 00:00:20\n"
		   "TRACK 03 AUDIO\nINDEX 01 00:00:40\n"
		   "FILE audio.iso BINARY\n"
		   "TRACK 04 MODE1/2048\nINDEX 01 00:00:00\n");
	char why[256] = "";
	struct cdimage *c = cdimage_open(path("audio.cue"), why, sizeof why);
	if (!c)
		fprintf(stderr, "cdaudio: %s\n", why);
	return c;
}

/* The next `count` frames taken (at 44.1 kHz) are track n's pattern from `from` (n 0: silence) */
static bool took(unsigned count, unsigned n, uint32_t from)
{
	int16_t f[2 * 1024];
	while (count) {
		unsigned k = count < 1024 ? count : 1024;
		if (!cdaudio_pull(&player, f, k, CD_RATE))
			return false;
		for (unsigned j = 0; j < k; j++) {
			int16_t l = 0, r = 0;
			if (n)
				pattern(n, from + j, &l, &r);
			if (f[2 * j] != l || f[2 * j + 1] != r)
				return false;
		}
		count -= k;
		from += k;
	}
	return true;
}

static void test_play(const struct cdimage *c)
{
	uint32_t lba;
	cdaudio_init(&player, CD_RATE);                 /* (a tick a frame) */
	cdaudio_disc(&player, c);
	CHECK_EQ(cdaudio_status(&player, 100, &lba), CDAUDIO_IDLE);
	CHECK(!cdaudio_active(&player));
	int16_t f[2];
	CHECK(!cdaudio_pull(&player, f, 1, CD_RATE));

	/* from sector 15 to 25: across tracks 1 and 2 */
	struct cdaudio_range r = { 15, 25 };
	cdaudio_play(&player, 1000, &r, 1, 0, r.start, false, false);
	CHECK_EQ(cdaudio_status(&player, 1000 + 3 * CD_FRAMES + 7, &lba), CDAUDIO_PLAYING);
	CHECK_EQ(lba, 18);
	CHECK(cdaudio_active(&player));
	CHECK(took(3 * CD_FRAMES + 7, 1, 15 * CD_FRAMES));
	CHECK_EQ(player.count, 0);
	cdaudio_run(&player, 1000 + 7 * CD_FRAMES);
	CHECK(took(2 * CD_FRAMES - 7, 1, 18 * CD_FRAMES + 7));
	CHECK(took(2 * CD_FRAMES, 2, 0));
	cdaudio_run(&player, 1000 + 10 * CD_FRAMES + 50);
	CHECK_EQ(cdaudio_status(&player, 1000 + 10 * CD_FRAMES + 50, &lba), CDAUDIO_DONE);
	CHECK_EQ(lba, 25);
	CHECK(took(2 * CD_FRAMES, 2, 2 * CD_FRAMES));
	CHECK(cdaudio_active(&player));                 /* (frames still to take) */
	CHECK(took(CD_FRAMES, 2, 4 * CD_FRAMES));
	CHECK(!cdaudio_active(&player));
	CHECK(!cdaudio_pull(&player, f, 1, CD_RATE));

	/* pause and go on; the position stays meanwhile */
	r = (struct cdaudio_range){ 0, 60 };
	cdaudio_play(&player, 20000, &r, 1, 0, r.start, false, false);
	CHECK(cdaudio_pause(&player, 21000, true));
	CHECK_EQ(cdaudio_status(&player, 30000, &lba), CDAUDIO_PAUSED);
	CHECK_EQ(player.frame, 1000);
	CHECK(took(1000, 1, 0));
	CHECK(!cdaudio_active(&player));
	CHECK(cdaudio_pause(&player, 30000, false));
	CHECK_EQ(cdaudio_status(&player, 30500, &lba), CDAUDIO_PLAYING);
	CHECK_EQ(player.frame, 1500);
	CHECK(took(500, 1, 1000));

	/* held at a position (AudioTrackSearch without play), then played */
	cdaudio_stop(&player, 40000);
	player.count = 0;
	r = (struct cdaudio_range){ 40, 60 };
	cdaudio_play(&player, 40000, &r, 1, 0, r.start, false, true);
	CHECK_EQ(cdaudio_status(&player, 45000, &lba), CDAUDIO_PAUSED);
	CHECK_EQ(lba, 40);
	CHECK(cdaudio_pause(&player, 45000, false));
	cdaudio_run(&player, 45100);
	CHECK(took(100, 3, 0));

	/* the end moved (AudioStop with an address), before and after the position */
	cdaudio_stop_at(&player, 45100, 50);
	cdaudio_run(&player, 45100 + 10 * CD_FRAMES);
	CHECK_EQ(cdaudio_status(&player, 46000 + 10 * CD_FRAMES, &lba), CDAUDIO_DONE);
	CHECK_EQ(lba, 50);
	r = (struct cdaudio_range){ 0, 60 };
	cdaudio_play(&player, 50000, &r, 1, 0, r.start, false, false);
	cdaudio_stop_at(&player, 50000 + 3 * CD_FRAMES, 2);
	CHECK_EQ(cdaudio_status(&player, 50000 + 3 * CD_FRAMES, &lba), CDAUDIO_DONE);
	CHECK_EQ(lba, 3);
	cdaudio_stop(&player, 60000);
	CHECK_EQ(cdaudio_status(&player, 60000, &lba), CDAUDIO_IDLE);
	CHECK_EQ(lba, 3);
	player.count = 0;

	/* a list, over and over: track 3's first 2 sectors, then track 1's first */
	player.count = 0;
	struct cdaudio_range list[2] = { { 40, 42 }, { 0, 1 } };
	cdaudio_play(&player, 100000, list, 2, 0, list[0].start, true, false);
	cdaudio_run(&player, 100000 + 3 * CD_FRAMES + 10);
	CHECK(took(2 * CD_FRAMES, 3, 0));
	CHECK(took(CD_FRAMES, 1, 0));
	CHECK(took(10, 3, 0));
	CHECK_EQ(player.current, 0);
	CHECK_EQ(cdaudio_status(&player, 100000 + 3 * CD_FRAMES + 10, NULL), CDAUDIO_PLAYING);
	cdaudio_stop(&player, 200000);
	player.count = 0;
	cdaudio_play(&player, 200000, list, 2, 1, list[1].start, false, false);       /* from the second, once */
	cdaudio_run(&player, 200000 + 5 * CD_FRAMES);
	CHECK_EQ(cdaudio_status(&player, 200000 + 5 * CD_FRAMES, &lba), CDAUDIO_DONE);
	CHECK_EQ(lba, 1);
	CHECK(took(CD_FRAMES, 1, 0));

	/* over the data track: silence */
	r = (struct cdaudio_range){ 59, 62 };
	cdaudio_play(&player, 300000, &r, 1, 0, r.start, false, false);
	cdaudio_run(&player, 300000 + 3 * CD_FRAMES);
	CHECK(took(CD_FRAMES, 3, 19 * CD_FRAMES));
	CHECK(took(2 * CD_FRAMES, 0, 0));

	/* out comes the disc: silence at once */
	r = (struct cdaudio_range){ 0, 60 };
	cdaudio_play(&player, 400000, &r, 1, 0, r.start, false, false);
	cdaudio_run(&player, 401000);
	cdaudio_disc(&player, NULL);
	CHECK(!cdaudio_active(&player));
	CHECK_EQ(cdaudio_status(&player, 402000, &lba), CDAUDIO_IDLE);
	CHECK_EQ(lba, 0);
	CHECK(!cdaudio_pause(&player, 402000, false));
	cdaudio_disc(&player, c);

	/* nobody taking the frames: only the last ones stay */
	player.count = 0;
	unsigned long dropped = player.dropped;
	cdaudio_play(&player, 500000, &r, 1, 0, r.start, false, false);
	cdaudio_run(&player, 500000 + CD_RATE / 4);
	CHECK_EQ(player.count, CDAUDIO_FIFO);
	CHECK_EQ(player.dropped - dropped, CD_RATE / 4 - CDAUDIO_FIFO);
	CHECK(took(CDAUDIO_FIFO, 1, CD_RATE / 4 - CDAUDIO_FIFO));
}

static void test_volume_and_modes(const struct cdimage *c)
{
	struct cdaudio_range r = { 20, 40 };
	int16_t f[2 * 4];
	int16_t l, rr;
	pattern(2, 0, &l, &rr);
	pattern(2, 1, &l, &rr);
	cdaudio_init(&player, CD_RATE);
	cdaudio_disc(&player, c);
	cdaudio_play(&player, 0, &r, 1, 0, r.start, false, false);
	cdaudio_volume(&player, 0, 128, 255);
	cdaudio_run(&player, 2);
	cdaudio_mode(&player, 2, 6);                    /* channels swapped */
	cdaudio_run(&player, 4);
	cdaudio_mode(&player, 4, 15);                   /* mono: both in both */
	cdaudio_volume(&player, 4, 255, 255);
	cdaudio_run(&player, 6);
	cdaudio_mode(&player, 6, 0);                    /* muted */
	cdaudio_run(&player, 8);
	cdaudio_stop(&player, 8);
	CHECK(cdaudio_pull(&player, f, 4, CD_RATE));
	for (uint32_t k = 0; k < 2; k++) {
		pattern(2, k, &l, &rr);
		CHECK_EQ((uint16_t)f[2 * k], (uint16_t)(l * 128 / 255));
		CHECK_EQ((uint16_t)f[2 * k + 1], (uint16_t)rr);
		pattern(2, k + 2, &l, &rr);
		CHECK_EQ((uint16_t)f[4 + 2 * k], (uint16_t)(rr * 128 / 255));
		CHECK_EQ((uint16_t)f[4 + 2 * k + 1], (uint16_t)l);
	}
	CHECK(cdaudio_pull(&player, f, 4, CD_RATE));
	for (uint32_t k = 0; k < 2; k++) {
		pattern(2, k + 4, &l, &rr);
		CHECK_EQ((uint16_t)f[2 * k], (uint16_t)((l + rr) / 2));
		CHECK_EQ((uint16_t)f[2 * k + 1], (uint16_t)((l + rr) / 2));
		CHECK_EQ(f[4 + 2 * k], 0);
		CHECK_EQ(f[4 + 2 * k + 1], 0);
	}
}

static void test_scan(const struct cdimage *c)
{
	uint32_t lba, bit = CDAUDIO_SCAN_SECTORS * CD_FRAMES, jump = CDAUDIO_SCAN_SPEED * CDAUDIO_SCAN_SECTORS;
	cdaudio_init(&player, CD_RATE);
	cdaudio_disc(&player, c);
	cdaudio_scan(&player, 0, 2, 1);                 /* forward from sector 2: a bit of it, then on */
	CHECK_EQ(cdaudio_status(&player, bit + 1, &lba), CDAUDIO_PLAYING);
	CHECK_EQ(lba, 2 + jump);
	CHECK(took(bit, 1, 2 * CD_FRAMES));
	CHECK(took(1, 2, (2 + jump - 20) * CD_FRAMES));
	CHECK_EQ(cdaudio_status(&player, 3 * bit + 1, &lba), CDAUDIO_DONE);    /* (at the lead-out) */
	CHECK_EQ(lba, c->leadout);

	cdaudio_scan(&player, 10 * bit, 50, -1);        /* back from 50, to the start, then plays on */
	CHECK_EQ(cdaudio_status(&player, 11 * bit + 1, &lba), CDAUDIO_PLAYING);
	CHECK_EQ(lba, 50 - jump);
	cdaudio_run(&player, 12 * bit + 1);
	CHECK_EQ(cdaudio_status(&player, 12 * bit + 1, &lba), CDAUDIO_PLAYING);
	CHECK_EQ(player.scan, 0);
	CHECK_EQ(lba, 0);
	CHECK(cdaudio_pause(&player, 12 * bit + 1, false));
}

/* At guest time's pace: a second is 44100 frames, however it's cut up */
static void test_pacing(const struct cdimage *c)
{
	static const uint32_t steps[] = { 1, 999, 16599, 3333333, 2, 7777777, 54321 };
	uint64_t tb = 123456789, total = 0;
	uint32_t lba;
	struct cdaudio_range r = { 0, 60 };
	cdaudio_init(&player, GUEST_HZ);
	cdaudio_disc(&player, c);
	cdaudio_play(&player, tb, &r, 1, 0, r.start, false, false);
	for (size_t k = 0; k < sizeof steps / sizeof steps[0]; k++) {
		total += steps[k];
		cdaudio_run(&player, tb + total);
		player.count = 0;
	}
	CHECK_EQ(cdaudio_status(&player, tb + GUEST_HZ, &lba), CDAUDIO_DONE);  /* (60 sectors < 1 s) */
	r.end = 600;                                    /* (past the lead-out: silence) */
	cdaudio_play(&player, tb, &r, 1, 0, r.start, false, false);
	total = 0;
	for (unsigned rounds = 0; rounds < 3; rounds++)
		for (size_t k = 0; k < sizeof steps / sizeof steps[0]; k++) {
			total += steps[k];
			cdaudio_run(&player, tb + total);
		}
	cdaudio_status(&player, tb + 3 * GUEST_HZ, &lba);
	CHECK_EQ(player.frame, 3 * CD_RATE);
	CHECK_EQ(lba, 3 * CD_SECTORS_PER_SECOND);
}

/* At another rate: between frames, the delay of a frame or two aside */
static void test_resampling(const struct cdimage *c)
{
	struct cdaudio_range r = { 0, 20 };
	int16_t f[2 * 64], l, rr;
	cdaudio_init(&player, CD_RATE);
	cdaudio_disc(&player, c);
	cdaudio_play(&player, 0, &r, 1, 0, r.start, false, false);
	cdaudio_run(&player, 128);
	CHECK(cdaudio_pull(&player, f, 64, 22050));
	CHECK_EQ(player.count, 128 - 126);
	for (unsigned k = 1; k < 64; k++) {
		pattern(1, 2 * k - 2, &l, &rr);
		CHECK_EQ(f[2 * k], l);
		CHECK_EQ(f[2 * k + 1], rr);
	}
	cdaudio_run(&player, 128 + 150);
	CHECK(cdaudio_pull(&player, f, 50, 29400));     /* (2/3 the rate: half way between, every other one) */
	int16_t a, b;
	pattern(1, 128 + 1, &a, &rr);
	pattern(1, 128 + 2, &b, &rr);
	CHECK(f[2 * 2] >= a && f[2 * 2] <= b);
}

static void input_run(void *ctx, uint64_t time)
{
	cdaudio_run(ctx, time);
}

static bool input_pull(void *ctx, int16_t *frames, unsigned count, unsigned rate)
{
	return cdaudio_pull(ctx, frames, count, rate);
}

static bool input_active(const void *ctx)
{
	return cdaudio_active(ctx);
}

static uint8_t heard[4 * 50000];
static unsigned heard_frames;

static void sink(void *ctx, const uint8_t *frames, unsigned count, unsigned rate)
{
	(void)ctx;
	CHECK_EQ(rate, 44100);
	if ((heard_frames + count) * 4 <= sizeof heard)
		memcpy(heard + 4 * heard_frames, frames, 4 * count);
	heard_frames += count;
}

/* Through the sound chip's analog input, with no sound from Mac OS: the CD alone, at guest time's pace */
static void test_through_codec(const struct cdimage *c)
{
	static struct mem mem;
	static struct dbdma dma;
	static struct awacs snd;
	if (mem_init(&mem))
		return;
	mem_map(&mem, 0, 0x10000, MEM_RWX);
	dbdma_init(&dma, &mem, 0x10000);
	awacs_init(&snd, &dma, GUEST_HZ);
	snd.sink = sink;
	snd.input = (struct awacs_input){ input_run, input_pull, input_active, &player };
	cdaudio_init(&player, GUEST_HZ);
	cdaudio_disc(&player, c);
	struct cdaudio_range r = { 20, 40 };
	CHECK(!awacs_playing(&snd));
	cdaudio_play(&player, 1000, &r, 1, 0, r.start, false, false);
	CHECK(awacs_playing(&snd));
	CHECK(awacs_moving(&snd));
	heard_frames = 0;
	for (uint64_t tb = 1000; tb <= 1000 + GUEST_HZ / 10; tb += 4567)
		awacs_set_time(&snd, tb);
	awacs_set_time(&snd, 1000 + GUEST_HZ / 10);
	CHECK_EQ(heard_frames, (1000 + GUEST_HZ / 10) * (uint64_t)CD_RATE / GUEST_HZ);  /* (the codec's frames) */
	unsigned z = 0;                                 /* silence until the player is CDAUDIO_LEAD ahead */
	while (z < heard_frames && !be16(heard + 4 * z))
		z++;
	CHECK(z > CDAUDIO_LEAD / 2 && z < CDAUDIO_LEAD + 20);
	bool same = true;
	for (unsigned k = 0; z + k < heard_frames; k++) {
		int16_t l, rr;
		pattern(2, k, &l, &rr);
		same &= (int16_t)be16(heard + 4 * (z + k)) == l && (int16_t)be16(heard + 4 * (z + k) + 2) == rr;
	}
	CHECK(same);
	CHECK_EQ(snd.frames_through, heard_frames);
	CHECK_EQ(snd.frames_played, 0);

	/* the microphone selected: not heard; the CD with loop-through: heard again */
	awacs_mmio_ops.write(&snd, 0x10, 4, __builtin_bswap32(0x00000400));
	unsigned before = heard_frames;
	awacs_set_time(&snd, 1000 + GUEST_HZ / 5);
	CHECK_EQ(heard_frames, before);
	awacs_mmio_ops.write(&snd, 0x10, 4, __builtin_bswap32(0x00000233));
	awacs_mmio_ops.write(&snd, 0x10, 4, __builtin_bswap32(0x000018C0));
	CHECK(awacs_through(&snd));
	awacs_set_time(&snd, 1000 + GUEST_HZ / 4);
	CHECK(heard_frames > before + 2000);

	/* the end of the play: nothing more once the last frames are out */
	awacs_set_time(&snd, 1000 + GUEST_HZ);
	CHECK(!awacs_playing(&snd));
	before = heard_frames;
	awacs_set_time(&snd, 1000 + 2 * GUEST_HZ);
	CHECK_EQ(heard_frames, before);
	mem_free(&mem);
}

void test_cdaudio(void)
{
	if (!begin())
		return;
	struct cdimage *c = audio_disc();
	CHECK(c != NULL);
	if (c) {
		test_play(c);
		test_volume_and_modes(c);
		test_scan(c);
		test_pacing(c);
		test_resampling(c);
		test_through_codec(c);
	}
	cdimage_close(c);
	end();
}

/* ---- other formats: the same disc in each ------------------------------------------ */

/*
 * The disc: test_single_bin()'s, its last sector whole (as the CHD fixtures
 * have it, tests/fixtures/README): the data track at 0-399, 150 sectors of
 * silence (track 2's pregap), track 2 at 550-649, track 3 at 650-699.
 */
#define DISC_END 700u
static const uint32_t disc_pregap[3] = { 0, 400, 650 }, disc_start[3] = { 0, 550, 650 };
static const uint32_t disc_end[3] = { 400, 650, DISC_END };

#define FIXTURE(name) fixture_path(name)

/* (as a full path: cue sheets in the test folder name them) */
static const char *fixture_path(const char *name)
{
	static char p[1100], full[PATH_MAX];
	const char *slash = strrchr(__FILE__, '/');
	snprintf(p, sizeof p, "%.*sfixtures/%s", slash ? (int)(slash - __FILE__ + 1) : 0, __FILE__, name);
	return realpath(p, full) ? full : p;
}

/* A sector of the disc, raw: a mode 1 sector (sync, header, data, zeros), silence, or audio */
static void disc_sector(const uint8_t *vol, uint32_t lba, uint8_t *s)
{
	memset(s, 0, CD_SECTOR);
	if (lba < DATA_SECTORS) {
		uint8_t m, sec, fr;
		memset(s + 1, 0xFF, 10);
		cd_msf(lba, &m, &sec, &fr);
		s[12] = cd_bcd(m), s[13] = cd_bcd(sec), s[14] = cd_bcd(fr), s[15] = 1;
		memcpy(s + 16, vol + (size_t)lba * CD_DATA, CD_DATA);
	} else if (lba >= 550 && lba < DISC_END) {
		unsigned n = lba < 650 ? 2 : 3;
		uint32_t frame = (lba - (n == 2 ? 550 : 650)) * CD_FRAMES;
		for (unsigned k = 0; k < CD_FRAMES; k++) {
			int16_t l, r;
			pattern(n, frame + k, &l, &r);
			s[4 * k] = (uint8_t)l, s[4 * k + 1] = (uint8_t)((uint16_t)l >> 8);
			s[4 * k + 2] = (uint8_t)r, s[4 * k + 3] = (uint8_t)((uint16_t)r >> 8);
		}
	}
}

/* Sectors `from`-`to` of the disc into a file, `stride` bytes each (2048: the data alone; more: zeros after) */
static void put_disc(FILE *f, const uint8_t *vol, uint32_t from, uint32_t to, unsigned stride)
{
	uint8_t s[CD_MAX_STRIDE] = { 0 };
	for (uint32_t lba = from; lba < to; lba++) {
		disc_sector(vol, lba, s);
		if (stride == CD_DATA)
			fwrite(s + 16, 1, CD_DATA, f);
		else
			fwrite(s, 1, stride, f);
	}
}

/* An image of the disc reads as the disc: its table of contents, every sector, its volume */
static void check_disc(const char *name, bool whole)
{
	char why[256] = "";
	struct cdimage *c = cdimage_open(path(name), why, sizeof why);
	if (!c) {
		fprintf(stderr, "cdformats: %s: %s\n", name, why);
		CHECK(!"the image opens");
		return;
	}
	uint8_t *vol = hfs_volume(), s[CD_SECTOR], want[CD_SECTOR];
	CHECK_EQ(c->tracks, 3);
	CHECK_EQ(c->sessions, 1);
	CHECK_EQ(c->leadout, DISC_END);
	CHECK_EQ(c->data, 0);
	for (unsigned k = 0; k < 3 && k < c->tracks; k++) {
		CHECK_EQ(c->track[k].number, k + 1);
		CHECK_EQ(c->track[k].mode, k ? 0 : 1);
		CHECK_EQ(c->track[k].control & CD_CONTROL_DATA, k ? 0 : CD_CONTROL_DATA);
		CHECK_EQ(c->track[k].pregap, disc_pregap[k]);
		CHECK_EQ(c->track[k].start, disc_start[k]);
		CHECK_EQ(c->track[k].end, disc_end[k]);
	}
	unsigned wrong = 0;
	for (uint32_t lba = 0; vol && lba < DISC_END + 2; lba++) {
		if (lba < DISC_END)
			disc_sector(vol, lba, want);
		else
			memset(want, 0, sizeof want);
		/* (a data sector the image keeps the data of alone: as made up around it, sync, header, data, zeros) */
		if (cdimage_read_raw(c, lba, 1, s) || (memcmp(s, want, CD_SECTOR) && (whole || lba >= DATA_SECTORS)))
			wrong++;
		else if (memcmp(s + 16, want + 16, CD_DATA))
			wrong++;
	}
	CHECK_EQ(wrong, 0);
	check_volume(c);
	free(vol);
	cdimage_close(c);
}

/* Nero, disc at once: each track's pregap in the file, cue points (v2: CUEX, LBAs; v1: CUES, MSF) */
static void write_nrg(const char *name, const uint8_t *vol, bool v2, unsigned data_stride, unsigned audio_stride)
{
	FILE *f = create(name);
	uint64_t at[3][3];
	for (unsigned k = 0; k < 3; k++) {
		unsigned stride = k ? audio_stride : data_stride;
		at[k][0] = (uint64_t)ftell(f);
		put_disc(f, vol, disc_pregap[k], disc_start[k], stride);
		at[k][1] = (uint64_t)ftell(f);
		put_disc(f, vol, disc_start[k], disc_end[k], stride);
		at[k][2] = (uint64_t)ftell(f);
	}
	uint64_t chain = (uint64_t)ftell(f);
	static const struct { uint8_t ctl, track, index; int32_t lba; } cue[] = {
		{ 0x41, 0, 0, -150 }, { 0x41, 1, 0, -150 }, { 0x41, 1, 1, 0 }, { 0x01, 2, 0, 400 }, { 0x01, 2, 1, 550 },
		{ 0x01, 3, 0, 650 }, { 0x01, 3, 1, 650 }, { 0x01, 0xAA, 1, DISC_END },
	};
	fputs(v2 ? "CUEX" : "CUES", f);
	put_be(f, 8 * (sizeof cue / sizeof cue[0]), 4);
	for (size_t k = 0; k < sizeof cue / sizeof cue[0]; k++) {
		fputc(cue[k].ctl, f), fputc(cue[k].track == 0xAA ? 0xAA : cd_bcd(cue[k].track), f);
		fputc(cd_bcd(cue[k].index), f), fputc(0, f);
		if (v2) {
			put_be(f, (uint32_t)cue[k].lba, 4);
		} else {
			uint32_t n = (uint32_t)(cue[k].lba + (int32_t)CD_MSF_OFFSET);
			fputc(0, f), fputc((int)(n / 4500), f), fputc((int)(n / 75 % 60), f), fputc((int)(n % 75), f);
		}
	}
	unsigned block = v2 ? 42 : 30;
	fputs(v2 ? "DAOX" : "DAOI", f);
	put_be(f, 22 + 3 * block, 4);
	put_be(f, 22 + 3 * block, 4);
	fputs("0123456789012", f);              /* (the catalog number) */
	put_be(f, 0, 3);
	fputc(1, f), fputc(3, f);
	for (unsigned k = 0; k < 3; k++) {
		unsigned stride = k ? audio_stride : data_stride;
		fputs(k == 1 ? "USABC9900001" : "", f);
		for (int j = k == 1 ? 12 : 0; j < 12; j++)
			fputc(0, f);
		put_be(f, stride, 2);
		fputc(k ? (stride == CD_SECTOR ? 0x07 : 0x10) : stride == CD_DATA ? 0x00 : stride == CD_SECTOR ? 0x05 : 0x0F, f);
		put_be(f, 0, 3);
		for (int j = 0; j < 3; j++) {
			if (v2)
				put_be(f, (uint32_t)(at[k][j] >> 32), 4);
			put_be(f, (uint32_t)at[k][j], 4);
		}
	}
	fputs("SINF", f), put_be(f, 4, 4), put_be(f, 3, 4);
	fputs("END!", f), put_be(f, 0, 4);
	fputs(v2 ? "NER5" : "NERO", f);
	if (v2)
		put_be(f, 0, 4);
	put_be(f, (uint32_t)chain, 4);
	fclose(f);
}

/* Nero, track at once (ETN2): the tracks' data alone, a 2 s pregap before tracks 2 */
static void write_nrg_tao(const char *name, const uint8_t *vol)
{
	FILE *f = create(name);
	uint64_t at[3];
	for (unsigned k = 0; k < 3; k++) {
		at[k] = (uint64_t)ftell(f);
		put_disc(f, vol, disc_start[k], disc_end[k], k ? CD_SECTOR : CD_DATA);
	}
	uint64_t chain = (uint64_t)ftell(f);
	fputs("ETN2", f);
	put_be(f, 3 * 32, 4);
	for (unsigned k = 0; k < 3; k++) {
		put_be(f, 0, 4), put_be(f, (uint32_t)at[k], 4);
		put_be(f, 0, 4), put_be(f, (disc_end[k] - disc_start[k]) * (k ? CD_SECTOR : CD_DATA), 4);
		put_be(f, k ? 0x07 : 0x00, 4);
		put_be(f, disc_start[k], 4);
		put_be(f, 0, 4), put_be(f, 0, 4);
	}
	fputs("END!", f), put_be(f, 0, 4);
	fputs("NER5", f), put_be(f, 0, 4), put_be(f, (uint32_t)chain, 4);
	fclose(f);
}

static void put_le16(FILE *f, unsigned v)
{
	put_le(f, v, 2);
}

/* Alcohol 120%: the .mdf a dump of the disc from LBA 0 (with each sector's subchannel after it: 2448), the .mds */
static void write_mds(const char *name, const uint8_t *vol, unsigned stride)
{
	char mdf[64], mds[64];
	snprintf(mdf, sizeof mdf, "%s.mdf", name);
	snprintf(mds, sizeof mds, "%s.mds", name);
	FILE *f = create(mdf);
	put_disc(f, vol, 0, DISC_END, stride);
	fclose(f);
	const unsigned sessions = 0x58, blocks = sessions + 24, extra = blocks + 80 * 6, footer = extra + 3 * 8;
	f = create(mds);
	fputs("MEDIA DESCRIPTOR", f);
	fputc(1, f), fputc(3, f);
	put_le16(f, 0), put_le16(f, 1);
	for (unsigned k = 22; k < 0x50; k++)
		fputc(0, f);
	put_le(f, sessions, 4), put_le(f, 0, 4);
	put_le(f, (uint32_t)-150, 4), put_le(f, DISC_END, 4), put_le16(f, 1);
	fputc(6, f), fputc(3, f), put_le16(f, 1), put_le16(f, 3), put_le(f, 0, 4), put_le(f, blocks, 4);
	for (unsigned k = 0; k < 6; k++) {      /* A0, A1, A2, then the tracks */
		static const uint8_t points[6] = { 0xA0, 0xA1, 0xA2, 1, 2, 3 };
		unsigned point = points[k], t = k - 3;
		bool track = k >= 3;
		uint32_t at = track ? disc_start[t] + CD_MSF_OFFSET : DISC_END + CD_MSF_OFFSET;
		fputc(!track ? 0 : t ? 0xA9 : 0xAA, f);
		fputc(track && stride > CD_SECTOR ? 0x08 : 0, f);
		fputc(0x10 | (t == 0 || k == 0 ? 4 : 0), f);
		fputc(0, f), fputc((int)point, f), put_le(f, 0, 4);
		if (k < 2)
			fputc(k ? 3 : 1, f), fputc(0, f), fputc(0, f);
		else
			fputc((int)(at / 4500), f), fputc((int)(at / 75 % 60), f), fputc((int)(at % 75), f);
		put_le(f, track ? extra + 8 * t : 0, 4);
		put_le16(f, track ? stride : 0);
		for (unsigned j = 0; j < 18; j++)
			fputc(0, f);
		put_le(f, track ? disc_start[t] : 0, 4);
		put_le(f, track ? disc_start[t] * stride : 0, 4), put_le(f, 0, 4);
		put_le(f, track, 4), put_le(f, track ? footer : 0, 4);
		for (unsigned j = 0; j < 24; j++)
			fputc(0, f);
	}
	for (unsigned t = 0; t < 3; t++)
		put_le(f, t ? disc_start[t] - disc_pregap[t] : 150, 4), put_le(f, disc_end[t] - disc_start[t], 4);
	put_le(f, footer + 16, 4), put_le(f, 0, 4), put_le(f, 0, 4), put_le(f, 0, 4);
	fputs("*.mdf", f), fputc(0, f);
	fclose(f);
}

/* CloneCD: the .img a raw dump of the disc from LBA 0, the .sub its subchannel (zeros: not read), the .ccd */
static void write_ccd(const uint8_t *vol)
{
	FILE *f = create("clone.img");
	put_disc(f, vol, 0, DISC_END, CD_SECTOR);
	fclose(f);
	f = create("clone.sub");
	for (unsigned k = 0; k < DISC_END * CD_SUBCODE; k++)
		fputc(0, f);
	fclose(f);
	f = create("clone.ccd");
	fputs("[CloneCD]\r\nVersion=3\r\n[Disc]\r\nTocEntries=6\r\nSessions=1\r\nDataTracksScrambled=0\r\n"
	      "CDTextLength=0\r\nCATALOG=0123456789012\r\n[Session 1]\r\nPreGapMode=1\r\nPreGapSubC=0\r\n", f);
	static const struct { int point, control, plba; } e[] = {
		{ 0xA0, 4, 4350 }, { 0xA1, 0, 13350 }, { 0xA2, 0, DISC_END }, { 1, 4, 0 }, { 2, 0, 550 }, { 3, 0, 650 },
	};
	for (unsigned k = 0; k < 6; k++)
		fprintf(f, "[Entry %u]\r\nSession=1\r\nPoint=0x%02x\r\nADR=0x01\r\nControl=0x%02x\r\nTrackNo=0\r\n"
			"AMin=0\r\nASec=0\r\nAFrame=0\r\nALBA=-150\r\nZero=0\r\nPMin=0\r\nPSec=0\r\nPFrame=0\r\nPLBA=%d\r\n",
			k, e[k].point, e[k].control, e[k].plba);
	fputs("[TRACK 1]\r\nMODE=1\r\nINDEX 1=0\r\n[TRACK 2]\r\nMODE=0\r\nISRC=USABC9900001\r\nINDEX 0=400\r\n"
	      "INDEX 1=550\r\n[TRACK 3]\r\nMODE=0\r\nINDEX 1=650\r\n", f);
	fclose(f);
}

static void test_nrg_mds_ccd(const uint8_t *vol)
{
	char why[256];
	write_nrg("disc.nrg", vol, true, CD_DATA, CD_SECTOR);
	check_disc("disc.nrg", false);
	write_nrg("raw.nrg", vol, true, CD_SECTOR, CD_SECTOR);
	check_disc("raw.nrg", true);
	write_nrg("sub.nrg", vol, true, CD_MAX_STRIDE, CD_MAX_STRIDE);
	check_disc("sub.nrg", true);
	write_nrg("old.nrg", vol, false, CD_DATA, CD_SECTOR);
	check_disc("old.nrg", false);
	write_nrg_tao("tao.nrg", vol);
	check_disc("tao.nrg", false);
	struct cdimage *c = cdimage_open(path("disc.nrg"), why, sizeof why);
	if (c) {
		CHECK(!strcmp(c->catalog, "0123456789012"));
		CHECK(!strcmp(c->track[1].isrc, "USABC9900001"));
		CHECK(strstr(c->kind, "Nero") != NULL);
		cdimage_close(c);
	}
	cleanup();

	write_mds("alcohol", vol, CD_SECTOR);
	check_disc("alcohol.mds", true);
	check_disc("alcohol.mdf", true);        /* (given the data: its .mds beside it) */
	CHECK_EQ(cdimage_detect(path("alcohol.mdf"), NULL, why, sizeof why), CDIMAGE_MDS);
	write_mds("alcsub", vol, CD_MAX_STRIDE);
	check_disc("alcsub.mds", true);
	cleanup();

	write_ccd(vol);
	check_disc("clone.ccd", true);
	check_disc("clone.img", true);
	CHECK(cdimage_has_toc(path("clone.img")));
	c = cdimage_open(path("clone.ccd"), why, sizeof why);
	if (c) {
		CHECK(!strcmp(c->catalog, "0123456789012"));
		CHECK(!strcmp(c->track[1].isrc, "USABC9900001"));
		cdimage_close(c);
	}
	cleanup();
}

/* MAME's CHD: the fixtures, made by chdman from the disc's cue sheet */
static void test_chd(void)
{
	static const struct { const char *name, *codecs; } chds[] = {
		{ "game.chd", "cdlz, cdzl, cdfl" }, { "game-cdzs.chd", "cdzs, cdfl" },
	};
	char why[256];
	for (size_t k = 0; k < sizeof chds / sizeof chds[0]; k++) {
		const char *p = FIXTURE(chds[k].name);
		CHECK_EQ(cdimage_detect(p, NULL, why, sizeof why), CDIMAGE_CHD);
		struct cdimage *c = cdimage_open(p, why, sizeof why);
		CHECK(c != NULL);
		if (!c) {
			fprintf(stderr, "cdformats: %s: %s\n", chds[k].name, why);
			continue;
		}
		CHECK(strstr(c->kind, chds[k].codecs) != NULL);
		cdimage_close(c);
		/* (check_disc() reads from the test folder) */
		FILE *in = fopen(p, "rb"), *out = create(chds[k].name);
		int ch;
		while (in && out && (ch = fgetc(in)) != EOF)
			fputc(ch, out);
		if (in)
			fclose(in);
		if (out)
			fclose(out);
		check_disc(chds[k].name, true);
	}
	struct cdimage *c = cdimage_open(FIXTURE("data-cdzl.chd"), why, sizeof why);
	CHECK(c != NULL);
	if (c) {
		CHECK_EQ(c->tracks, 1);
		CHECK_EQ(c->leadout, DATA_SECTORS);
		CHECK(strstr(c->kind, "cdzl") != NULL);
		check_volume(c);
		cdimage_close(c);
	}
	cleanup();
}

/* The tone the audio fixtures hold, as 16-bit samples */
static int16_t tone(unsigned channel, uint32_t frame)
{
	return (int16_t)(8000 * sin(2 * M_PI * (channel ? 660.0 : 440.0) * frame / CD_RATE));
}

/* Compressed audio in cue sheets: FLAC exactly, MP3 and Ogg Vorbis near enough; seeking; what isn't taken */
static void test_compressed_audio(void)
{
	static const struct { const char *file; uint32_t frames; bool exact, mono; double snr; } files[] = {
		{ "tone.flac", 30 * CD_FRAMES, true, false, 0 }, { "tone-mono.flac", 30 * CD_FRAMES, true, true, 0 },
		{ "tone.mp3", 30 * CD_FRAMES, false, false, 20 }, { "tone.ogg", 17664, false, false, 25 },
	};
	char why[256], cue[PATH_MAX + 100];
	for (size_t k = 0; k < sizeof files / sizeof files[0]; k++) {
		snprintf(cue, sizeof cue, "FILE \"%s\" %s\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n", FIXTURE(files[k].file),
			 k == 2 ? "MP3" : "WAVE");
		write_text("audio.cue", cue);
		struct cdimage *c = cdimage_open(path("audio.cue"), why, sizeof why);
		if (!c) {
			fprintf(stderr, "cdformats: %s: %s\n", files[k].file, why);
			CHECK(!"the cue sheet opens");
			continue;
		}
		CHECK_EQ(c->leadout, (files[k].frames + CD_FRAMES - 1) / CD_FRAMES);
		/* the whole track, then a sector again (back), then one further on */
		uint8_t *s = malloc((size_t)c->leadout * CD_SECTOR), one[CD_SECTOR];
		CHECK_EQ(cdimage_read_raw(c, 0, c->leadout, s), 0);
		CHECK_EQ(cdimage_read_raw(c, 3, 1, one), 0);
		CHECK(!memcmp(one, s + 3 * CD_SECTOR, CD_SECTOR));
		CHECK_EQ(cdimage_read_raw(c, 20, 1, one), 0);
		CHECK(!memcmp(one, s + 20 * CD_SECTOR, CD_SECTOR));
		double sig = 0, err = 0;
		unsigned differ = 0;
		for (uint32_t f = 0; f < files[k].frames; f++) {
			int16_t l = (int16_t)(s[4 * f] | s[4 * f + 1] << 8), r = (int16_t)(s[4 * f + 2] | s[4 * f + 3] << 8);
			int16_t wl = tone(0, f), wr = files[k].mono ? wl : tone(1, f);
			differ += l != wl || r != wr;
			sig += (double)wl * wl;
			err += (double)(l - wl) * (l - wl);
		}
		if (files[k].exact)
			CHECK_EQ(differ, 0);
		else if (10 * log10(sig / (err + 1)) <= files[k].snr) {        /* (the left channel, near enough) */
			fprintf(stderr, "cdformats: %s: %.1f dB\n", files[k].file, 10 * log10(sig / (err + 1)));
			CHECK(!"near enough");
		}
		unsigned tail = 0;                      /* (past its end, to the end of the sector: silence) */
		for (uint32_t f = files[k].frames; f < c->leadout * CD_FRAMES; f++)
			tail += s[4 * f] || s[4 * f + 1];
		CHECK_EQ(tail, 0);
		free(s);
		cdimage_close(c);
	}

	/* what isn't taken: another rate, something that isn't audio */
	snprintf(cue, sizeof cue, "FILE \"%s\" MP3\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n", FIXTURE("tone-22k.mp3"));
	write_text("audio.cue", cue);
	why[0] = 0;
	CHECK(cdimage_open(path("audio.cue"), why, sizeof why) == NULL);
	CHECK(strstr(why, "22050 Hz") != NULL);
	snprintf(cue, sizeof cue, "FILE \"%s\" FLAC\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n", FIXTURE("game.chd"));
	write_text("audio.cue", cue);
	CHECK(cdimage_open(path("audio.cue"), why, sizeof why) == NULL);
	CHECK(strstr(why, "not audio Blue reads") != NULL);
	cleanup();
}

/* What each kind is taken for, by its contents */
static void test_detect(void)
{
	char why[256], other[4096];
	write_text("sheet.txt", "REM made by hand\r\nFILE \"x.bin\" BINARY\r\n  TRACK 01 AUDIO\r\n");
	CHECK_EQ(cdimage_detect(path("sheet.txt"), NULL, why, sizeof why), CDIMAGE_CUE);
	write_text("notes.txt", "Just some notes, not a cue sheet\n");
	CHECK_EQ(cdimage_detect(path("notes.txt"), NULL, why, sizeof why), CDIMAGE_PLAIN);
	CHECK(!cdimage_has_toc(path("notes.txt")));
	CHECK_EQ(cdimage_detect(FIXTURE("packed-zlib.dmg"), NULL, why, sizeof why), CDIMAGE_PACKED);

	/* a .bin of raw sectors, its cue sheet beside it */
	uint8_t *vol = hfs_volume();
	FILE *f = create("rip.bin");
	if (vol)
		put_disc(f, vol, 0, DISC_END, CD_SECTOR);
	fclose(f);
	write_text("RIP.CUE", "FILE \"rip.bin\" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\nTRACK 02 AUDIO\n"
		   "INDEX 00 00:05:25\nINDEX 01 00:07:25\nTRACK 03 AUDIO\nINDEX 01 00:08:50\n");
	CHECK_EQ(cdimage_detect(path("rip.bin"), other, why, sizeof why), CDIMAGE_CUE);
	CHECK(!strcmp(other, path("RIP.CUE")));
	check_disc("rip.bin", true);
	free(vol);

	/* Alcohol 120%'s later version, a DVD's */
	f = create("dvd.mds");
	fputs("MEDIA DESCRIPTOR", f);
	fputc(2, f), fputc(0, f);
	for (int k = 0; k < 100; k++)
		fputc(0, f);
	fclose(f);
	why[0] = 0;
	CHECK(cdimage_open(path("dvd.mds"), why, sizeof why) == NULL);
	CHECK(strstr(why, "version") != NULL);
	cleanup();
}

void test_cdformats(void)
{
	if (!begin())
		return;
	uint8_t *vol = hfs_volume();
	CHECK(vol != NULL);
	if (vol) {
		test_nrg_mds_ccd(vol);
		test_chd();
		test_compressed_audio();
		test_detect();
	}
	free(vol);
	end();
}
