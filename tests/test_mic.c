/*
 * Sound input from the host (mic.h): the ring its frames wait in (mono
 * made stereo, the oldest dropped when full), resampling to the codec's
 * rate, silence when it runs dry, WAV files standing in for the
 * microphone, and the host's device opened only while Mac OS records.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/mic.h"
#include "test.h"

static struct mic_stream st;

static void test_ring(void)
{
	struct mic_ring *r = &st.ring;
	mic_ring_init(r);
	const int16_t mono[3] = { 1, -2, 3 }, stereo[4] = { 10, 20, 30, 40 }, quad[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	mic_ring_put(r, mono, 3, 1);
	mic_ring_put(r, stereo, 2, 2);
	mic_ring_put(r, quad, 2, 4);
	CHECK_EQ(r->count, 7);
	const int16_t want[7][2] = { { 1, 1 }, { -2, -2 }, { 3, 3 }, { 10, 20 }, { 30, 40 }, { 1, 2 }, { 5, 6 } };
	for (unsigned k = 0; k < 7; k++) {
		int16_t f[2] = { 0, 0 };
		CHECK(mic_ring_get(r, f));
		CHECK(f[0] == want[k][0] && f[1] == want[k][1]);
	}
	int16_t f[2];
	CHECK(!mic_ring_get(r, f));

	/* full: the oldest go */
	for (int16_t k = 0; k < 100; k++)
		mic_ring_put(r, &k, 1, 1);
	static int16_t many[MIC_RING];
	for (unsigned k = 0; k < MIC_RING; k++)
		many[k] = (int16_t)(1000 + k % 1000);
	mic_ring_put(r, many, MIC_RING - 10, 1);
	CHECK_EQ(r->count, MIC_RING);
	CHECK_EQ(r->dropped, 90);
	CHECK(mic_ring_get(r, f));
	CHECK_EQ(f[0], 90);                             /* (0 to 89 dropped) */
}

/* Frames k = (k, -k), from k = 1 */
static void ramp(struct mic_ring *r, unsigned n)
{
	for (unsigned k = 1; k <= n; k++) {
		int16_t s[2] = { (int16_t)k, (int16_t)-(int)k };
		mic_ring_put(r, s, 1, 2);
	}
}

static void test_resample(void)
{
	int16_t out[2 * 64];

	/* the same rate: as they were */
	mic_stream_init(&st, 44100, 0, false);
	ramp(&st.ring, 10);
	CHECK(mic_stream_take(&st, out, 8, 44100));
	for (unsigned k = 0; k < 8; k++)
		CHECK(out[2 * k] == (int16_t)(k + 1) && out[2 * k + 1] == -(int16_t)(k + 1));

	/* half the rate: every other one */
	mic_stream_init(&st, 44100, 0, false);
	ramp(&st.ring, 20);
	CHECK(mic_stream_take(&st, out, 8, 22050));
	for (unsigned k = 0; k < 8; k++)
		CHECK_EQ(out[2 * k], 2 * k + 1);

	/* twice the rate: half way between, in a straight line */
	mic_stream_init(&st, 22050, 0, false);
	for (int16_t k = 0; k < 8; k++) {
		int16_t s[2] = { (int16_t)(100 * k), (int16_t)(-100 * k) };
		mic_ring_put(&st.ring, s, 1, 2);
	}
	CHECK(mic_stream_take(&st, out, 8, 44100));
	for (unsigned k = 0; k < 8; k++) {
		CHECK_EQ(out[2 * k], 50 * k);
		CHECK_EQ((int16_t)-out[2 * k + 1], 50 * k);
	}

	/* run dry: the rest silence, then nothing until it has a lead again */
	mic_stream_init(&st, 44100, 4, false);
	ramp(&st.ring, 3);
	CHECK(!mic_stream_take(&st, out, 4, 44100));    /* (short of the lead) */
	CHECK_EQ(out[0], 0);
	ramp(&st.ring, 3);
	CHECK(mic_stream_take(&st, out, 8, 44100));
	CHECK_EQ(out[2 * 3], 1);                        /* (the second ramp; its last frame is never between two) */
	CHECK_EQ(out[2 * 4], 2);
	CHECK_EQ(out[2 * 5], 0);
	CHECK_EQ(out[2 * 7 + 1], 0);
	CHECK_EQ(st.gaps, 1);
	CHECK(!st.taking);
	ramp(&st.ring, 2);
	CHECK(!mic_stream_take(&st, out, 2, 44100));
	CHECK_EQ(out[0], 0);

	/* paced: a backlog waiting when it starts is cut to twice the lead */
	mic_stream_init(&st, 48000, 100, true);
	ramp(&st.ring, 1000);
	CHECK(mic_stream_take(&st, out, 1, 48000));
	CHECK_EQ(out[0], 801);
	/* ... and fuller than that, it's taken a little faster */
	ramp(&st.ring, 4000);
	for (unsigned k = 0; k < 50; k++)
		mic_stream_take(&st, out, 64, 48000);
	CHECK(st.ring.count < 4198 - 50 * 64 - 8);
}

static void put16(FILE *f, unsigned v)
{
	fputc(v & 0xFF, f);
	fputc(v >> 8 & 0xFF, f);
}

static void put32(FILE *f, uint32_t v)
{
	put16(f, v & 0xFFFF);
	put16(f, v >> 16);
}

/* A WAV file of `frames` frames, `bytes` of data */
static void write_wav(const char *path, unsigned format, unsigned channels, unsigned rate, unsigned bits,
		      const void *data, unsigned bytes)
{
	FILE *f = fopen(path, "wb");
	fwrite("RIFF", 1, 4, f);
	put32(f, 36 + bytes);
	fwrite("WAVEfmt ", 1, 8, f);
	put32(f, 16);
	put16(f, format);
	put16(f, channels);
	put32(f, rate);
	put32(f, rate * channels * bits / 8);
	put16(f, channels * bits / 8);
	put16(f, bits);
	fwrite("data", 1, 4, f);
	put32(f, bytes);
	fwrite(data, 1, bytes, f);
	fclose(f);
}

static void test_wav(void)
{
	char path[] = "/tmp/blue-test-XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	close(fd);
	struct mic_wav w;
	char why[200];

	const uint8_t u8[3] = { 128, 255, 0 };          /* (8 bits: unsigned) */
	write_wav(path, 1, 1, 11025, 8, u8, 3);
	CHECK_EQ(mic_wav_load(path, &w, why, sizeof why), 0);
	CHECK_EQ(w.frames, 3);
	CHECK_EQ(w.rate, 11025);
	CHECK(w.f[0][0] == 0 && w.f[1][0] == 127 * 256 && w.f[2][0] == -32768 && w.f[2][1] == -32768);
	mic_wav_free(&w);

	const uint8_t s16[8] = { 0x34, 0x12, 0xCC, 0xED, 1, 0, 0xFF, 0xFF };
	write_wav(path, 1, 2, 48000, 16, s16, 8);
	CHECK_EQ(mic_wav_load(path, &w, why, sizeof why), 0);
	CHECK(w.frames == 2 && w.f[0][0] == 0x1234 && w.f[0][1] == -0x1234 && w.f[1][0] == 1 && w.f[1][1] == -1);
	mic_wav_free(&w);

	const float fl[2] = { 0.5f, -2.0f };            /* (32-bit floating point, clipped) */
	write_wav(path, 3, 1, 44100, 32, fl, 8);
	CHECK_EQ(mic_wav_load(path, &w, why, sizeof why), 0);
	CHECK(w.frames == 2 && w.f[0][0] == 16383 && w.f[1][1] == -32767);
	mic_wav_free(&w);

	write_wav(path, 2, 1, 44100, 4, u8, 3);         /* (ADPCM: not read) */
	CHECK_EQ(mic_wav_load(path, &w, why, sizeof why), -1);
	CHECK(strstr(why, "kind") != NULL);
	FILE *f = fopen(path, "wb");
	fputs("not a sound", f);
	fclose(f);
	CHECK_EQ(mic_wav_load(path, &w, why, sizeof why), -1);
	CHECK_EQ(mic_wav_load("/nonexistent/blue.wav", &w, why, sizeof why), -1);

	/* standing in for the microphone: as it's taken, over and over or once */
	static struct mic_file file;
	int16_t mono[5] = { 100, 200, 300, 400, 500 }, out[2 * 12];
	write_wav(path, 1, 1, 22050, 16, mono, sizeof mono);
	CHECK_EQ(mic_wav_load(path, &file.wav, why, sizeof why), 0);
	mic_file_start(&file, true);
	CHECK(mic_file_take(&file, out, 12, 22050));
	for (unsigned k = 0; k < 12; k++)
		CHECK(out[2 * k] == mono[k % 5] && out[2 * k + 1] == mono[k % 5]);
	mic_file_start(&file, false);
	CHECK(mic_file_take(&file, out, 12, 44100));    /* (at twice the rate: between them) */
	CHECK(out[0] == 100 && out[2] == 150 && out[4] == 200 && out[14] == 450);
	CHECK(out[16] == 0 && out[22] == 0);            /* the end: silence */
	CHECK(!mic_file_take(&file, out, 4, 44100));
	mic_wav_free(&file.wav);
	unlink(path);
}

/* A stand-in for the host's device: recording a mono ramp at 44.1 kHz */
static struct {
	bool fail;
	unsigned opens, closes, next;
	void *dev;
} host;

static void *host_open(void *ctx, unsigned *rate, unsigned *channels)
{
	(void)ctx;
	if (host.fail)
		return NULL;
	host.opens++;
	*rate = 44100;
	*channels = 1;
	return host.dev = &host;
}

static void host_close(void *ctx, void *dev)
{
	(void)ctx;
	CHECK(dev == host.dev);
	host.closes++;
	host.dev = NULL;
}

static unsigned host_read(void *ctx, void *dev, int16_t *samples, unsigned max_frames)
{
	(void)ctx;
	(void)dev;
	unsigned n = max_frames < 256 ? max_frames : 256;
	if (host.next >= 4096)
		return 0;
	for (unsigned k = 0; k < n; k++)
		samples[k] = (int16_t)host.next++;
	return n;
}

static const struct mic_host fake = { host_open, host_close, host_read, NULL };

static void test_privacy(void)
{
	static struct mic m;
	int16_t out[2 * 8];
	const uint64_t s = 1000000000u;
	memset(&host, 0, sizeof host);

	/* without a host's input (instruction time), never opened: silence */
	mic_init(&m, NULL);
	mic_want(&m, true, 0);
	CHECK(!mic_open(&m));
	CHECK(!mic_take(&m, out, 8, 44100));
	CHECK_EQ(out[0], 0);

	/* opened while Mac OS records, closed a while after it stops */
	mic_init(&m, &fake);
	mic_want(&m, false, 0);
	CHECK_EQ(host.opens, 0);
	mic_want(&m, true, 10 * s);
	CHECK(mic_open(&m));
	CHECK_EQ(host.opens, 1);
	CHECK(mic_take(&m, out, 8, 44100));             /* (mono, made stereo; the backlog cut) */
	CHECK(out[0] == out[1] && out[0] >= 2048 && out[14] >= out[0] + 6 && out[14] <= out[0] + 7);
	mic_want(&m, false, 11 * s);
	CHECK(mic_open(&m));
	mic_want(&m, true, 11 * s);                     /* (records again: stays open, not opened twice) */
	mic_want(&m, false, 12 * s);
	CHECK(mic_open(&m));
	mic_want(&m, false, 12 * s + MIC_CLOSE_NS);
	CHECK(!mic_open(&m));
	CHECK_EQ(host.opens, 1);
	CHECK_EQ(host.closes, 1);
	CHECK(!mic_take(&m, out, 8, 44100));

	/* turned off: closed at once, silence, and not opened again */
	mic_want(&m, true, 20 * s);
	CHECK(mic_open(&m));
	m.off = true;
	mic_want(&m, true, 20 * s);
	CHECK(!mic_open(&m));
	CHECK(!mic_take(&m, out, 8, 44100));
	mic_want(&m, true, 30 * s);
	CHECK_EQ(host.opens, 2);
	m.off = false;

	/* a device that won't open (no permission yet): tried again after a while, not at every call */
	host.fail = true;
	mic_want(&m, true, 40 * s);
	mic_want(&m, true, 40 * s + MIC_RETRY_NS / 2);
	host.fail = false;
	mic_want(&m, true, 40 * s + MIC_RETRY_NS / 2);
	CHECK(!mic_open(&m));
	mic_want(&m, true, 40 * s + MIC_RETRY_NS);
	CHECK(mic_open(&m));
	mic_close(&m);
	CHECK(!mic_open(&m));
	CHECK_EQ(host.opens, host.closes);
}

void test_mic(void)
{
	test_ring();
	test_resample();
	test_wav();
	test_privacy();
}
