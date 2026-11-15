/*
 * The host sound queue's pace: frames resampled to play a little faster or
 * slower so that the queue stays near its goal when the device's clock
 * drifts from the guest's, or a stall left it fuller, without running dry
 * or overflowing (audio.c).
 */
#include <string.h>

#include "../src/audio.h"
#include "../src/be.h"
#include "test.h"

#define RATE 44100u
#define CHUNK 256u
#define DEVICE 1024u
#define GOAL (RATE * 60u / 1000u + DEVICE / 2u)

static uint8_t in[CHUNK * 4], out[(CHUNK + CHUNK / 128 + 2) * 4];

/* A frame of a slow ramp, left and right apart */
static void ramp(unsigned k, uint8_t *f)
{
	put_be16(f, (uint16_t)(int16_t)((int)(k % 2000) - 1000));
	put_be16(f + 2, (uint16_t)(int16_t)(1000 - (int)(k % 2000)));
}

/*
 * `seconds` of the guest's chunks against a device taking DEVICE frames at
 * a time at `drift` times the rate, from `queued` frames: the lowest and
 * highest the queue was before each chunk, from `settle` seconds on, and
 * where it ended (smoothed as the pace sees it).
 */
static void run(double drift, double seconds, double settle, unsigned queued, unsigned *lo, unsigned *hi,
		double *end, unsigned *ran_dry)
{
	struct audio_pace p;
	audio_pace_start(&p, GOAL);
	double t = 0, device_at = 0, chunk_s = (double)CHUNK / RATE, device_s = DEVICE / (RATE * drift);
	unsigned long k = 0;
	*lo = ~0u;
	*hi = 0;
	*ran_dry = 0;
	while (t < seconds) {
		for (; device_at <= t; device_at += device_s) {
			if (queued < DEVICE)
				(*ran_dry)++;
			queued = queued < DEVICE ? 0 : queued - DEVICE;
		}
		if (t >= settle) {
			*lo = queued < *lo ? queued : *lo;
			*hi = queued > *hi ? queued : *hi;
		}
		for (unsigned f = 0; f < CHUNK; f++)
			ramp(k++, in + 4 * f);
		queued += audio_pace(&p, queued, RATE, in, CHUNK, out);
		t += chunk_s;
	}
	*end = p.level;
}

void test_audio(void)
{
	/* at its goal, frames pass unchanged, a frame late */
	struct audio_pace p;
	audio_pace_start(&p, GOAL);
	for (unsigned f = 0; f < CHUNK; f++)
		ramp(f, in + 4 * f);
	CHECK_EQ(audio_pace(&p, GOAL, RATE, in, CHUNK, out), CHUNK);
	CHECK_EQ(be32(out), 0);
	CHECK(memcmp(out + 4, in, (CHUNK - 1) * 4) == 0);
	CHECK_EQ(audio_pace(&p, GOAL, RATE, in, CHUNK, out), CHUNK);
	CHECK(memcmp(out, in + (CHUNK - 1) * 4, 4) == 0);

	/* fuller: faster, by at most 0.5% (fewer frames out); a constant stays constant */
	audio_pace_start(&p, GOAL);
	for (unsigned f = 0; f < CHUNK; f++) {
		put_be16(in + 4 * f, 1234);
		put_be16(in + 4 * f + 2, (uint16_t)-1234);
	}
	audio_pace(&p, GOAL, RATE, in, CHUNK, out);
	unsigned long total = 0;
	for (unsigned c = 0; c < 400; c++) {
		unsigned n = audio_pace(&p, GOAL + RATE, RATE, in, CHUNK, out);
		total += n;
		for (unsigned f = 0; f < n; f++) {
			CHECK_EQ(be16(out + 4 * f), 1234);
			CHECK_EQ(be16(out + 4 * f + 2), (uint16_t)-1234);
		}
	}
	CHECK(total < 400ul * CHUNK && total >= (unsigned long)(400 * CHUNK / 1.005));
	CHECK(p.step > 1ull << 32 && p.step <= (uint64_t)(1.005 * (1ull << 32)) + 1);

	/* a device 0.3% faster or slower than the guest: the queue settles near its goal and never runs dry */
	unsigned lo, hi, dry;
	double end;
	run(1.003, 300, 60, GOAL, &lo, &hi, &end, &dry);
	CHECK_EQ(dry, 0);
	CHECK(end > GOAL - RATE * 0.04 && end < GOAL);
	CHECK(lo > RATE / 100);
	run(1 / 1.003, 300, 60, GOAL, &lo, &hi, &end, &dry);
	CHECK_EQ(dry, 0);
	CHECK(end < GOAL + RATE * 0.04 && end > GOAL);
	CHECK(hi < RATE / 4);

	/* 150 ms fuller after a stall: back within 5 ms of the goal in a minute, never overflowing */
	run(1, 60, 0, GOAL + RATE * 150 / 1000, &lo, &hi, &end, &dry);
	CHECK_EQ(dry, 0);
	CHECK(hi < RATE / 4);
	CHECK(end > GOAL - RATE * 0.005 && end < GOAL + RATE * 0.005);
}
