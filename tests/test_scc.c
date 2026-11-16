/*
 * The SCC: the register model as Mac OS drives it, with nothing attached (as
 * it always was) and with a port: characters paced at the programmed bit
 * rate in guest time, the FIFOs, interrupts (RR3, the modified vector), the
 * modem-control lines, and the DMA channels the serial driver uses.
 */
#include <string.h>

#include "../src/be.h"
#include "../src/dbdma.h"
#include "../src/mem.h"
#include "../src/scc.h"
#include "test.h"

#define TB 16600000u
#define A_CTRL 0x1020u                  /* (escc layout) */
#define A_DATA 0x1030u
#define B_CTRL 0x1000u
#define LEGACY_B_CTRL 0x0u

static struct scc scc;
static struct {
	uint8_t sent[64];
	uint64_t sent_at[64];
	unsigned sent_len;
	const char *to_send;
	bool dtr, rts;
	unsigned line_changes;
} port;

static void port_tx(void *ctx, uint8_t byte, uint64_t tb)
{
	(void)ctx;
	if (port.sent_len < sizeof port.sent) {
		port.sent_at[port.sent_len] = tb;
		port.sent[port.sent_len++] = byte;
	}
}

static int port_rx(void *ctx, uint64_t tb)
{
	(void)ctx, (void)tb;
	return port.to_send && *port.to_send ? (uint8_t)*port.to_send++ : -1;
}

static void port_lines(void *ctx, bool dtr, bool rts, uint64_t tb)
{
	(void)ctx, (void)tb;
	port.dtr = dtr;
	port.rts = rts;
	port.line_changes++;
}

static void wr(uint32_t ctrl, unsigned reg, uint8_t v)
{
	if (reg)
		scc_mmio_ops.write(&scc, ctrl, 1, reg);
	scc_mmio_ops.write(&scc, ctrl, 1, v);
}

static uint8_t rr(uint32_t ctrl, unsigned reg)
{
	if (reg)
		scc_mmio_ops.write(&scc, ctrl, 1, reg);
	return (uint8_t)scc_mmio_ops.read(&scc, ctrl, 1);
}

/* Channel A as Mac OS's serial driver sets it up: 8 data bits, no parity, 1 stop bit, x16, the BRG at TC */
static void setup(unsigned tc)
{
	wr(A_CTRL, 9, 0x80);            /* (reset A) */
	wr(A_CTRL, 4, 0x44);
	wr(A_CTRL, 3, 0xC0);
	wr(A_CTRL, 5, 0xE2);
	wr(A_CTRL, 11, 0x50);
	wr(A_CTRL, 12, (uint8_t)tc);
	wr(A_CTRL, 13, (uint8_t)(tc >> 8));
	wr(A_CTRL, 14, 0x01);
	wr(A_CTRL, 3, 0xC1);
	wr(A_CTRL, 5, 0xEA);
}

static void fresh(bool attach)
{
	scc_init(&scc, TB);
	memset(&port, 0, sizeof port);
	if (attach)
		scc_attach(&scc, SCC_A, &(struct scc_port){ port_tx, port_rx, port_lines, NULL });
}

static void test_idle(void)
{
	fresh(false);
	/* nothing attached: as it always was */
	CHECK_EQ(rr(LEGACY_B_CTRL, 0), 0x7C);
	CHECK_EQ(rr(A_CTRL, 0), 0x7C);
	CHECK_EQ(rr(A_CTRL, 1), 0x07);
	wr(A_CTRL, 9, 0xC0);
	setup(10);
	wr(A_CTRL, 1, 0x13);
	wr(A_CTRL, 9, 0x0A);
	scc_mmio_ops.write(&scc, A_DATA, 1, 'x');
	CHECK_EQ(rr(A_CTRL, 0), 0x7C);
	CHECK(!scc_irq(&scc, SCC_A));
	CHECK_EQ(scc_next_event(&scc), UINT64_MAX);
	CHECK_EQ(scc.ch[SCC_A].tx_bytes, 1);
	/* the register pointer, point high, the shared vector */
	wr(A_CTRL, 12, 0x34);
	CHECK_EQ(rr(A_CTRL, 12), 0x34);
	scc_mmio_ops.write(&scc, A_CTRL, 1, 0x08 | 5);  /* (point high: register 13) */
	scc_mmio_ops.write(&scc, A_CTRL, 1, 0x12);
	CHECK_EQ(rr(A_CTRL, 13), 0x12);
	wr(A_CTRL, 2, 0x40);
	CHECK_EQ(rr(A_CTRL, 2), 0x40);
	CHECK_EQ(rr(B_CTRL, 2), 0x40 | 0x06);           /* (B: the modified vector, no interrupt: 011) */
	wr(A_CTRL, 9, 0x1A);                            /* (status high) */
	CHECK_EQ(rr(B_CTRL, 2), 0x60);                  /* (011 in bits 6-4, reversed) */
}

static void test_rates(void)
{
	fresh(true);
	setup(10);                      /* 3.6864 MHz / (2 * 12) / 16 */
	CHECK_EQ(scc_bit_rate(&scc, SCC_A, false), 9600);
	CHECK_EQ(scc_char_time(&scc, SCC_A, false), 10ull * TB / 9600);
	setup(0);
	CHECK_EQ(scc_bit_rate(&scc, SCC_A, true), 57600);
	wr(A_CTRL, 11, 0x00);           /* (the RTxC pin's clock, x32: 115200, as Mac OS gets it) */
	wr(A_CTRL, 4, 0x8C);            /* (... with 2 stop bits) */
	CHECK_EQ(scc_bit_rate(&scc, SCC_A, false), 115200);
	CHECK_EQ(scc_char_time(&scc, SCC_A, false), 11ull * TB / 115200);
	wr(A_CTRL, 4, 0x45);            /* (parity, 1 stop bit) */
	wr(A_CTRL, 5, 0xAA);            /* (7 data bits) */
	wr(A_CTRL, 11, 0x50);
	CHECK_EQ(scc_char_time(&scc, SCC_A, false), 10ull * TB / 57600);
}

static void test_transmit(void)
{
	fresh(true);
	setup(10);
	CHECK_EQ(port.line_changes, 1);
	CHECK(port.dtr && port.rts);
	uint64_t ct = scc_char_time(&scc, SCC_A, false);
	scc_set_time(&scc, 1000);
	for (const char *s = "hello"; *s; s++)
		scc_mmio_ops.write(&scc, A_DATA, 1, (uint8_t)*s);
	CHECK_EQ(port.sent_len, 0);
	CHECK_EQ(rr(A_CTRL, 1) & 1, 0);                 /* (not all sent) */
	CHECK_EQ(rr(A_CTRL, 0) & 0x04, 0);              /* (the FIFO isn't empty: 'h' went to the shift register) */
	CHECK_EQ(scc_next_event(&scc), 1000 + ct);
	scc_set_time(&scc, 1000 + ct - 1);
	CHECK_EQ(port.sent_len, 0);
	scc_set_time(&scc, 1000 + 3 * ct);
	CHECK_EQ(port.sent_len, 3);
	CHECK_EQ(port.sent_at[0], 1000 + ct);
	CHECK_EQ(port.sent_at[2], 1000 + 3 * ct);
	scc_set_time(&scc, 1000 + 10 * ct);
	CHECK_EQ(port.sent_len, 5);
	CHECK(memcmp(port.sent, "hello", 5) == 0);
	CHECK_EQ(rr(A_CTRL, 1) & 1, 1);
	CHECK_EQ(scc_next_event(&scc), UINT64_MAX);

	/* the transmit interrupt: once the FIFO has emptied, until reset */
	wr(A_CTRL, 1, 0x02);
	wr(A_CTRL, 9, 0x0A);
	CHECK(!scc_irq(&scc, SCC_A));
	scc_mmio_ops.write(&scc, A_DATA, 1, 'a');
	scc_mmio_ops.write(&scc, A_DATA, 1, 'b');
	CHECK(!scc_irq(&scc, SCC_A));
	scc_set_time(&scc, scc.tb + ct);                /* ('a' out, 'b' to the shift register) */
	CHECK(scc_irq(&scc, SCC_A));
	CHECK_EQ(rr(A_CTRL, 3), 0x10);
	CHECK_EQ(rr(B_CTRL, 2), 0x08);                  /* (A transmit: 100) */
	wr(A_CTRL, 0, 0x28);                            /* (reset transmit interrupt pending) */
	CHECK(!scc_irq(&scc, SCC_A));
	wr(A_CTRL, 9, 0x02);                            /* (MIE off) */

	/* DTR and RTS */
	wr(A_CTRL, 5, 0x6A);
	CHECK(!port.dtr && port.rts);
	wr(A_CTRL, 5, 0x68);
	CHECK(!port.dtr && !port.rts);
	wr(A_CTRL, 5, 0xEA);
	CHECK(port.dtr && port.rts);
	unsigned changes = port.line_changes;
	wr(A_CTRL, 5, 0xEA);
	CHECK_EQ(port.line_changes, changes);
}

static void test_receive(void)
{
	fresh(true);
	setup(10);
	uint64_t ct = scc_char_time(&scc, SCC_A, true);
	port.to_send = "OK\r\n";
	scc_set_time(&scc, 500);
	scc_rx_ready(&scc, SCC_A);
	CHECK_EQ(scc_next_event(&scc), 500 + ct);
	scc_set_time(&scc, 500 + ct - 1);
	CHECK_EQ(rr(A_CTRL, 0) & 1, 0);
	scc_set_time(&scc, 500 + ct);
	CHECK_EQ(rr(A_CTRL, 0) & 1, 1);
	CHECK_EQ(scc_mmio_ops.read(&scc, A_DATA, 1), 'O');
	CHECK_EQ(rr(A_CTRL, 0) & 1, 0);
	scc_set_time(&scc, 500 + 4 * ct);
	CHECK_EQ(scc_mmio_ops.read(&scc, A_DATA, 1), 'K');
	CHECK_EQ(scc_mmio_ops.read(&scc, A_DATA, 1), '\r');
	CHECK_EQ(scc_mmio_ops.read(&scc, A_DATA, 1), '\n');
	scc_set_time(&scc, 500 + 6 * ct);
	CHECK_EQ(scc_next_event(&scc), UINT64_MAX);     /* (the port had no more) */

	/* the FIFO holds 8; the port's characters wait while it's full */
	port.to_send = "0123456789";
	scc_rx_ready(&scc, SCC_A);
	scc_set_time(&scc, scc.tb + 20 * ct);
	CHECK_EQ(scc.ch[SCC_A].rx_count, 8);
	CHECK_EQ(scc_next_event(&scc), UINT64_MAX);
	CHECK_EQ(scc_mmio_ops.read(&scc, A_DATA, 1), '0');
	scc_set_time(&scc, scc.tb + 5 * ct);
	CHECK_EQ(scc.ch[SCC_A].rx_count, 8);
	char got[16] = "";
	for (int k = 0; k < 8; k++)
		got[k] = (char)scc_mmio_ops.read(&scc, A_DATA, 1);
	scc_set_time(&scc, scc.tb + 5 * ct);
	got[8] = (char)scc_mmio_ops.read(&scc, A_DATA, 1);
	CHECK(memcmp(got, "123456789", 9) == 0);

	/* receive interrupts: every character, and RR3, the modified vector */
	wr(A_CTRL, 2, 0x40);
	wr(A_CTRL, 1, 0x10);
	wr(A_CTRL, 9, 0x0A);
	port.to_send = "x";
	scc_rx_ready(&scc, SCC_A);
	scc_set_time(&scc, scc.tb + ct);
	CHECK(scc_irq(&scc, SCC_A));
	CHECK(!scc_irq(&scc, SCC_B));
	CHECK_EQ(rr(A_CTRL, 3), 0x20);
	CHECK_EQ(rr(B_CTRL, 3), 0);
	CHECK_EQ(rr(B_CTRL, 2), 0x40 | 0x0C);           /* (A receive: 110) */
	scc_mmio_ops.read(&scc, A_DATA, 1);
	CHECK(!scc_irq(&scc, SCC_A));

	/* the first character only, once armed */
	wr(A_CTRL, 1, 0x08);
	port.to_send = "yz";
	scc_rx_ready(&scc, SCC_A);
	scc_set_time(&scc, scc.tb + 2 * ct);
	CHECK(scc_irq(&scc, SCC_A));
	scc_mmio_ops.read(&scc, A_DATA, 1);
	CHECK(!scc_irq(&scc, SCC_A));
	CHECK_EQ(scc_mmio_ops.read(&scc, A_DATA, 1), 'z');

	/* with the receiver off, what comes is lost */
	wr(A_CTRL, 3, 0xC0);
	port.to_send = "lost";
	scc_rx_ready(&scc, SCC_A);
	scc_set_time(&scc, scc.tb + 10 * ct);
	CHECK_EQ(scc.ch[SCC_A].rx_count, 0);
	CHECK(*port.to_send == '\0');
}

static void test_status(void)
{
	fresh(true);
	setup(10);
	/* DCD as it is, CTS read inverted (as on every Power Mac) */
	scc_set_inputs(&scc, SCC_A, false, true);
	CHECK_EQ(rr(A_CTRL, 0) & 0x28, 0x00);
	scc_set_inputs(&scc, SCC_A, true, false);
	CHECK_EQ(rr(A_CTRL, 0) & 0x28, 0x28);
	/* the external/status interrupt for the changes enabled (WR15), until reset */
	wr(A_CTRL, 15, 0x08);
	wr(A_CTRL, 1, 0x01);
	wr(A_CTRL, 9, 0x0A);
	scc_set_inputs(&scc, SCC_A, true, true);        /* (CTS: not enabled) */
	CHECK(!scc_irq(&scc, SCC_A));
	scc_set_inputs(&scc, SCC_A, false, true);
	CHECK(scc_irq(&scc, SCC_A));
	CHECK_EQ(rr(A_CTRL, 3), 0x08);
	CHECK_EQ(rr(B_CTRL, 2) & 0x0E, 0x0A);           /* (A external/status: 101) */
	wr(A_CTRL, 0, 0x10);
	CHECK(!scc_irq(&scc, SCC_A));
}

/* Mac OS's serial driver: transmit by DMA channel 4, receive by channel 5 (W/REQ for the receiver) */
static void test_dma(void)
{
	static struct mem mem;
	static struct dbdma dma;
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		return;
	}
	mem_map(&mem, 0, 0x10000, MEM_RWX);
	dbdma_init(&dma, &mem, 0x10000);
	fresh(true);
	scc_dma(&scc, &dma);
	setup(10);
	uint64_t ct = scc_char_time(&scc, SCC_A, false);
	uint8_t *p = mem_ptr(&mem, 0);
	memcpy(p + 0x2000, "ATDT123\r", 8);
	put_le32(p + 0x1000, (uint32_t)DBDMA_OUTPUT_LAST << 28 | 8);
	put_le32(p + 0x1004, 0x2000);
	put_le32(p + 0x1010, (uint32_t)DBDMA_STOP << 28);
	dbdma_mmio_ops.write(&dma, 4 * DBDMA_CHANNEL_SIZE + DBDMA_CMDPTR, 4, __builtin_bswap32(0x1000));
	dbdma_mmio_ops.write(&dma, 4 * DBDMA_CHANNEL_SIZE + DBDMA_CONTROL, 4, __builtin_bswap32(0x80008000));
	CHECK_EQ(scc.ch[SCC_A].tx_count, 4);            /* ('A' to the shift register, 4 in the FIFO; it waits) */
	scc_set_time(&scc, scc.tb + 8 * ct);
	CHECK_EQ(port.sent_len, 8);
	CHECK(memcmp(port.sent, "ATDT123\r", 8) == 0);
	CHECK_EQ(le32(p + 0x100C) >> 16 & DBDMA_ACTIVE, DBDMA_ACTIVE);      /* (the command completed) */

	put_le32(p + 0x1100, (uint32_t)DBDMA_INPUT_MORE << 28 | 64);
	put_le32(p + 0x1104, 0x3000);
	dbdma_mmio_ops.write(&dma, 5 * DBDMA_CHANNEL_SIZE + DBDMA_CMDPTR, 4, __builtin_bswap32(0x1100));
	dbdma_mmio_ops.write(&dma, 5 * DBDMA_CHANNEL_SIZE + DBDMA_CONTROL, 4, __builtin_bswap32(0x80008000));
	port.to_send = "OK";
	scc_rx_ready(&scc, SCC_A);
	scc_set_time(&scc, scc.tb + 3 * ct);
	CHECK_EQ(scc.ch[SCC_A].rx_count, 2);            /* (no DMA request yet: WR1) */
	wr(A_CTRL, 1, 0xE0);
	CHECK_EQ(scc.ch[SCC_A].rx_count, 0);
	CHECK(memcmp(p + 0x3000, "OK", 2) == 0);
	mem_free(&mem);
}

void test_scc(void)
{
	test_idle();
	test_rates();
	test_transmit();
	test_receive();
	test_status();
	test_dma();
}
