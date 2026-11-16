#include <string.h>
#include <time.h>

#include "../src/cuda.h"
#include "../src/via.h"
#include "test.h"

/* VIA register offsets (registers are 0x200 apart) */
#define ORB 0x0000
#define DDRB 0x0400
#define SR 0x1400
#define ACR 0x1600
#define IFR 0x1A00

static struct via via;
static struct cuda cuda;

static uint8_t rd(uint32_t reg)
{
	return (uint8_t)via_mmio_ops.read(&via, reg, 1);
}

static void wr(uint32_t reg, uint8_t v)
{
	via_mmio_ops.write(&via, reg, 1, v);
}

static int sr_flag(void)
{
	return (rd(IFR) & VIA_IRQ_SR) != 0;
}

/* Sends a packet the way the Mac OS / Linux Cuda drivers do. */
static void send(const uint8_t *p, unsigned n)
{
	wr(ACR, rd(ACR) | VIA_ACR_SR_OUT);
	wr(SR, p[0]);
	wr(ORB, rd(ORB) & ~CUDA_TIP);
	CHECK(sr_flag());
	for (unsigned k = 1; k < n; k++) {
		wr(SR, p[k]);
		wr(ORB, rd(ORB) ^ CUDA_TACK);
		CHECK(sr_flag());
	}
	wr(ACR, rd(ACR) & ~VIA_ACR_SR_OUT);
	(void)rd(SR);
	wr(ORB, rd(ORB) | CUDA_TIP | CUDA_TACK);
}

/* Reads a reply packet; returns its length. */
static unsigned receive(uint8_t *buf, unsigned max)
{
	unsigned n = 0;

	CHECK(sr_flag());                       /* attention */
	CHECK(!(rd(ORB) & CUDA_TREQ));
	(void)rd(SR);
	wr(ORB, rd(ORB) & ~CUDA_TIP);
	for (;;) {
		CHECK(sr_flag());
		uint8_t b = rd(SR);
		if (n < max)
			buf[n++] = b;
		if (rd(ORB) & CUDA_TREQ) {          /* that was the last byte */
			wr(ORB, rd(ORB) | CUDA_TACK | CUDA_TIP);
			CHECK(sr_flag());           /* end-of-read acknowledge */
			(void)rd(SR);
			return n;
		}
		wr(ORB, rd(ORB) ^ CUDA_TACK);
		if (n > 64)
			return n;
	}
}

void test_cuda(void)
{
	via_init(&via);
	cuda_init(&cuda, &via);
	wr(DDRB, CUDA_TIP | CUDA_TACK);
	wr(ORB, CUDA_TIP | CUDA_TACK);

	uint8_t r[64];

	/* GET_TIME: [pseudo, 0, cmd, t3, t2, t1, t0], seconds since 1904 */
	static const uint8_t get_time[] = { 0x01, 0x03 };
	send(get_time, sizeof get_time);
	CHECK_EQ(receive(r, sizeof r), 7);
	CHECK_EQ(r[0], 0x01);
	CHECK_EQ(r[2], 0x03);
	uint32_t t = (uint32_t)r[3] << 24 | (uint32_t)r[4] << 16 | (uint32_t)r[5] << 8 | r[6];
	CHECK(t > 3786825600u);                 /* after 2024-01-01 in Mac time */

	/* SET_TIME moves the clock: GET_TIME counts on from the time set */
	uint64_t tb = 0;
	const struct guest_clock fixed = { 1000000000, &tb, 1000 };
	cuda.clock = &fixed;
	static const uint8_t set_time[] = { 0x01, 0x09, 0xB0, 0x00, 0x00, 0x00 };
	send(set_time, sizeof set_time);
	CHECK_EQ(receive(r, sizeof r), 3);
	tb = 5000;
	send(get_time, sizeof get_time);
	CHECK_EQ(receive(r, sizeof r), 7);
	CHECK_EQ((uint32_t)r[3] << 24 | (uint32_t)r[4] << 16 | (uint32_t)r[5] << 8 | r[6], 0xB0000005u);

	/* ADB talk to an absent device times out */
	static const uint8_t talk[] = { 0x00, 0x2C };
	send(talk, sizeof talk);
	CHECK_EQ(receive(r, sizeof r), 3);
	CHECK_EQ(r[0], 0x00);
	CHECK_EQ(r[1], 0x02);
	CHECK_EQ(r[2], 0x2C);

	/* PRAM round trip */
	static const uint8_t set_pram[] = { 0x01, 0x0C, 0x00, 0x10, 0xA5 };
	static const uint8_t get_pram[] = { 0x01, 0x07, 0x00, 0x10 };
	send(set_pram, sizeof set_pram);
	CHECK_EQ(receive(r, sizeof r), 3);
	send(get_pram, sizeof get_pram);
	CHECK_EQ(receive(r, sizeof r), 4);
	CHECK_EQ(r[3], 0xA5);
	CHECK_EQ(cuda.packets, 6);

	/* T2 one-shot: flag after the loaded count elapses */
	via_set_time(&via, 1000);
	wr(0x1000, 100);                        /* T2 latch low */
	wr(0x1200, 0);                          /* T2 high: load 100 and start */
	via_set_time(&via, 1050);
	CHECK(!(rd(IFR) & VIA_IRQ_T2));
	via_set_time(&via, 1101);
	CHECK(rd(IFR) & VIA_IRQ_T2);

	/* Autopoll: the mouse's click as it is, and while Mac OS dozes, after a tap of Option */
	static const uint8_t autopoll[] = { 0x01, 0x01, 0x01 };
	send(autopoll, sizeof autopoll);
	CHECK_EQ(receive(r, sizeof r), 3);
	adb_mouse_button(&cuda.adb, true);
	cuda_poll(&cuda, 10000);
	CHECK_EQ(receive(r, sizeof r), 5);
	CHECK_EQ(r[1], 0x40);
	CHECK_EQ(r[2], 0x3C);                   /* Talk R0 from the mouse */
	CHECK_EQ(r[3], 0x00);                   /* button down */
	cuda_doze(&cuda, 10005);
	adb_mouse_button(&cuda.adb, false);
	cuda_poll(&cuda, 10020);
	CHECK_EQ(receive(r, sizeof r), 5);
	CHECK_EQ(r[2], 0x2C);                   /* from the keyboard: Option down and up */
	CHECK_EQ(r[3], 0x3A);
	CHECK_EQ(r[4], 0xBA);
	CHECK_EQ(cuda.wake_taps, 1);
	cuda_poll(&cuda, 10040);
	CHECK_EQ(receive(r, sizeof r), 5);
	CHECK_EQ(r[2], 0x3C);
	CHECK_EQ(r[3], 0x80);                   /* button up */
	cuda_doze(&cuda, 10050);                /* (at most a tap a second) */
	cuda_mouse_moved(&cuda);
	cuda_poll(&cuda, 10060);
	cuda_mouse_moved(&cuda);
	cuda_doze(&cuda, 11000);
	cuda_poll(&cuda, 11300);                /* (napped too long ago: not dozing) */
	CHECK_EQ(cuda.wake_taps, 1);
	cuda_mouse_moved(&cuda);                /* the cursor put where the host pointer is */
	cuda_doze(&cuda, 11400);
	cuda_poll(&cuda, 11410);
	CHECK_EQ(receive(r, sizeof r), 5);
	CHECK_EQ(r[3], 0x3A);
	CHECK_EQ(cuda.wake_taps, 2);
}
