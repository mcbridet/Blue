#include "scc.h"

#include <stdio.h>
#include <string.h>

/* RR0 */
#define RR0_RX_AVAIL 0x01
#define RR0_TX_EMPTY 0x04
#define RR0_DCD 0x08
#define RR0_SYNC_HUNT 0x10
#define RR0_CTS 0x20
#define RR0_TX_UNDERRUN 0x40

/* RR1 */
#define RR1_ALL_SENT 0x01
#define RR1_RESIDUE_8BIT 0x06

/* RR3 (channel A's): interrupts pending */
#define RR3_B_EXT 0x01
#define RR3_B_TX 0x02
#define RR3_B_RX 0x04
#define RR3_A_EXT 0x08
#define RR3_A_TX 0x10
#define RR3_A_RX 0x20

/* WR1 */
#define WR1_EXT_IE 0x01
#define WR1_TX_IE 0x02
#define WR1_RX_MODE(v) (((v) >> 3) & 3)         /* 0 off, 1 first character, 2 every character, 3 special only */

#define WR1_RX_DMA 0xE0                 /* W/REQ on, as a DMA request, for the receiver */

/* WR3, WR4, WR5 */
#define WR3_RX_ENABLE 0x01
#define WR4_PARITY 0x01
#define WR5_RTS 0x02
#define WR5_TX_ENABLE 0x08
#define WR5_DTR 0x80

/* WR9 */
#define WR9_MIE 0x08                    /* master interrupt enable */
#define WR9_STATUS_HIGH 0x10
#define WR9_RESET_MASK 0xC0

/* WR14, WR15 */
#define WR14_BRG_PCLK 0x02
#define WR15_WR7P 0x01                  /* (the ESCC's: WR7 writes go to WR7') */
#define WR15_DCD_IE 0x08
#define WR15_CTS_IE 0x20
#define WR7P_TX_FIFO_EMPTY 0x20         /* the transmit interrupt (and empty bit) when the FIFO is empty, not just not full */

void scc_init(struct scc *s, uint32_t timebase_hz)
{
	memset(s, 0, sizeof *s);
	s->timebase_hz = timebase_hz;
	for (int ch = 0; ch < 2; ch++) {
		s->ch[ch].wr7p = WR7P_TX_FIFO_EMPTY;
		s->ch[ch].dcd = s->ch[ch].cts = true;
	}
}

void scc_attach(struct scc *s, int ch, const struct scc_port *port)
{
	struct scc_channel *c = &s->ch[ch];
	c->attached = true;
	c->port = *port;
	c->dcd = c->cts = true;
}

static void say(const struct scc *s, int ch, const char *fmt, unsigned a, unsigned b)
{
	if (!s->log)
		return;
	printf("scc: %c ", 'A' + ch);
	printf(fmt, a, b);
	printf("\n");
}

/* ---- timing ---- */

/* The clock in front of the clock-mode divider: the baud-rate generator's output, or the pins' (PCLK on Mac I/O) */
uint32_t scc_bit_rate(const struct scc *s, int ch, bool rx)
{
	const struct scc_channel *c = &s->ch[ch];
	unsigned source = rx ? (c->wr[11] >> 5) & 3 : (c->wr[11] >> 3) & 3;
	static const unsigned mode[4] = { 1, 16, 32, 64 };
	uint32_t clock = SCC_PCLK;
	if (source == 2) {              /* the baud-rate generator (PCLK or the RTxC pin: the same here) */
		uint32_t tc = (uint32_t)c->wr[13] << 8 | c->wr[12];
		clock = SCC_PCLK / (2 * (tc + 2));
	}
	uint32_t rate = clock / mode[c->wr[4] >> 6];
	return rate ? rate : 1;
}

uint64_t scc_char_time(const struct scc *s, int ch, bool rx)
{
	const struct scc_channel *c = &s->ch[ch];
	static const unsigned rx_bits[4] = { 5, 7, 6, 8 }, tx_bits[4] = { 5, 7, 6, 8 };
	unsigned data = rx ? rx_bits[c->wr[3] >> 6] : tx_bits[(c->wr[5] >> 5) & 3];
	unsigned stop2 = (c->wr[4] >> 2) & 3;   /* in half bits: 1, 1.5, 2 (0: synchronous modes: 1) */
	stop2 = stop2 ? stop2 + 1 : 2;
	unsigned half_bits = 2 * (1 + data + (c->wr[4] & WR4_PARITY)) + stop2;
	uint64_t t = (uint64_t)half_bits * s->timebase_hz / (2 * (uint64_t)scc_bit_rate(s, ch, rx));
	return t ? t : 1;
}

/* ---- the transmitter ---- */

static bool tx_room(const struct scc_channel *c)
{
	return (c->wr7p & WR7P_TX_FIFO_EMPTY) ? c->tx_count == 0 : c->tx_count < SCC_TX_FIFO;
}

/* The next character into the shift register, starting at guest time t */
static void tx_start(struct scc *s, int ch, uint64_t t)
{
	struct scc_channel *c = &s->ch[ch];
	if (c->tx_busy || !c->tx_count || !(c->wr[5] & WR5_TX_ENABLE))
		return;
	bool room = tx_room(c);
	c->tx_shift = c->tx_fifo[c->tx_head];
	c->tx_head = (c->tx_head + 1) % SCC_TX_FIFO;
	c->tx_count--;
	c->tx_busy = true;
	c->tx_done = t + scc_char_time(s, ch, false);
	if (!room && tx_room(c)) {
		if (c->tx_written && (c->wr[1] & WR1_TX_IE))
			c->ip_tx = true;
		if (s->dbdma)
			dbdma_kick(s->dbdma, c->dma_tx);
	}
}

static void tx_put(struct scc *s, int ch, uint8_t byte)
{
	struct scc_channel *c = &s->ch[ch];
	c->tx_written = true;
	c->ip_tx = false;
	if (c->tx_count < SCC_TX_FIFO) {
		c->tx_fifo[(c->tx_head + c->tx_count) % SCC_TX_FIFO] = byte;
		c->tx_count++;
	}
	tx_start(s, ch, s->tb);
}

static void tx_done(struct scc *s, int ch, uint64_t t)
{
	struct scc_channel *c = &s->ch[ch];
	c->tx_busy = false;
	c->tx_bytes++;
	if (c->port.tx)
		c->port.tx(c->port.ctx, c->tx_shift, t);
	tx_start(s, ch, t);
}

/* ---- the receiver ---- */

static bool rx_due(const struct scc_channel *c)
{
	return c->rx_waiting && c->rx_due_set && (c->rx_count < SCC_RX_FIFO || !(c->wr[3] & WR3_RX_ENABLE));
}

/* A character in from the port at guest time t (dropped while the receiver is off, as on the line) */
static void rx_in(struct scc *s, int ch, uint64_t t)
{
	struct scc_channel *c = &s->ch[ch];
	int b = c->port.rx ? c->port.rx(c->port.ctx, t) : -1;
	if (b < 0) {
		c->rx_waiting = false;
		c->rx_due_set = false;
		return;
	}
	c->rx_due = t + scc_char_time(s, ch, true);
	if (!(c->wr[3] & WR3_RX_ENABLE))
		return;
	c->rx_fifo[(c->rx_head + c->rx_count) % SCC_RX_FIFO] = (uint8_t)b;
	c->rx_count++;
	c->rx_bytes++;
	if (c->rx_first) {
		c->rx_first = false;
		c->ip_rx = true;
	}
	if (s->dbdma)
		dbdma_kick(s->dbdma, c->dma_rx);
}

static int rx_take(struct scc *s, int ch)
{
	struct scc_channel *c = &s->ch[ch];
	if (!c->rx_count)
		return -1;
	uint8_t b = c->rx_fifo[c->rx_head];
	c->rx_head = (c->rx_head + 1) % SCC_RX_FIFO;
	c->rx_count--;
	c->ip_rx = false;
	if (c->rx_waiting && !c->rx_due_set) {          /* (it waited for room) */
		c->rx_due_set = true;
		if (c->rx_due < s->tb)
			c->rx_due = s->tb;
	}
	return b;
}

void scc_rx_ready(struct scc *s, int ch)
{
	struct scc_channel *c = &s->ch[ch];
	if (c->rx_waiting)
		return;
	c->rx_waiting = true;
	if (!c->rx_due_set) {
		uint64_t from = c->rx_due > s->tb ? c->rx_due : s->tb;
		c->rx_due = from + scc_char_time(s, ch, true);
		c->rx_due_set = true;
	}
}

/* ---- time ---- */

void scc_set_time(struct scc *s, uint64_t tb)
{
	for (;;) {
		int ch = -1;
		bool tx = false;
		uint64_t t = UINT64_MAX;
		for (int k = 0; k < 2; k++) {
			const struct scc_channel *c = &s->ch[k];
			if (c->tx_busy && c->tx_done < t) {
				t = c->tx_done;
				ch = k;
				tx = true;
			}
			if (rx_due(c) && c->rx_due < t) {
				t = c->rx_due;
				ch = k;
				tx = false;
			}
		}
		if (ch < 0 || t > tb)
			break;
		if (t > s->tb)
			s->tb = t;
		if (tx)
			tx_done(s, ch, t);
		else
			rx_in(s, ch, t);
	}
	if (tb > s->tb)
		s->tb = tb;
}

uint64_t scc_next_event(const struct scc *s)
{
	uint64_t t = UINT64_MAX;
	for (int k = 0; k < 2; k++) {
		const struct scc_channel *c = &s->ch[k];
		if (c->tx_busy && c->tx_done < t)
			t = c->tx_done;
		if (rx_due(c) && c->rx_due < t)
			t = c->rx_due;
	}
	return t;
}

/* ---- status and interrupts ---- */

void scc_set_inputs(struct scc *s, int ch, bool dcd, bool cts)
{
	struct scc_channel *c = &s->ch[ch];
	uint8_t ie = c->wr[15];
	if (((dcd != c->dcd && (ie & WR15_DCD_IE)) || (cts != c->cts && (ie & WR15_CTS_IE))) && (c->wr[1] & WR1_EXT_IE))
		c->ip_ext = true;
	if (dcd != c->dcd || cts != c->cts)
		say(s, ch, "DCD %u, CTS %u", dcd, cts);
	c->dcd = dcd;
	c->cts = cts;
}

/* Interrupts pending on a channel (RR3's bits for channel A; shifted down 3 for B) */
static unsigned pending(const struct scc_channel *c)
{
	if (!c->attached)
		return 0;
	unsigned mode = WR1_RX_MODE(c->wr[1]), p = 0;
	if ((mode == 1 && c->ip_rx) || (mode == 2 && c->rx_count))
		p |= RR3_A_RX;
	if (c->ip_tx && (c->wr[1] & WR1_TX_IE))
		p |= RR3_A_TX;
	if (c->ip_ext && (c->wr[1] & WR1_EXT_IE))
		p |= RR3_A_EXT;
	return p;
}

bool scc_irq(const struct scc *s, int ch)
{
	return (s->ch[SCC_A].wr[9] & WR9_MIE) && pending(&s->ch[ch]);
}

/* RR2 read through channel B: the vector, with the highest interrupt pending's status in it */
static uint8_t modified_vector(const struct scc *s)
{
	uint8_t v = s->ch[SCC_A].wr[2], wr9 = s->ch[SCC_A].wr[9];
	unsigned a = pending(&s->ch[SCC_A]), b = pending(&s->ch[SCC_B]);
	unsigned code = a & RR3_A_RX ? 6 : a & RR3_A_TX ? 4 : a & RR3_A_EXT ? 5 :
			b & RR3_A_RX ? 2 : b & RR3_A_TX ? 0 : b & RR3_A_EXT ? 1 : 3;
	if (wr9 & WR9_STATUS_HIGH)              /* status in bits 4-6, reversed */
		return (uint8_t)((v & 0x8F) | (code & 1) << 6 | (code & 2) << 4 | (code & 4) << 2);
	return (uint8_t)((v & 0xF1) | code << 1);      /* status in bits 1-3 */
}

/* (CTS reads inverted, as on every Power Mac: the bit is clear while the port's CTS is asserted) */
static uint8_t read_rr0(const struct scc_channel *c)
{
	if (!c->attached)
		return RR0_TX_EMPTY | RR0_DCD | RR0_SYNC_HUNT | RR0_CTS | RR0_TX_UNDERRUN;
	return (uint8_t)((c->rx_count ? RR0_RX_AVAIL : 0) | (tx_room(c) ? RR0_TX_EMPTY : 0) | (c->dcd ? RR0_DCD : 0) |
			 RR0_SYNC_HUNT | (c->cts ? 0 : RR0_CTS) | RR0_TX_UNDERRUN);
}

static uint8_t read_register(struct scc *s, int ch, unsigned reg)
{
	const struct scc_channel *c = &s->ch[ch];

	switch (reg) {
	case 0: case 4:
		return read_rr0(c);
	case 1: case 5:
		return (uint8_t)(RR1_RESIDUE_8BIT | (!c->attached || (!c->tx_busy && !c->tx_count) ? RR1_ALL_SENT : 0));
	case 2: case 6:
		return ch == SCC_A ? c->wr[2] : modified_vector(s);
	case 3: case 7:
		return ch == SCC_A ? (uint8_t)(pending(&s->ch[SCC_A]) | pending(&s->ch[SCC_B]) >> 3) : 0;
	case 8: {
		int b = rx_take(s, ch);
		return b < 0 ? 0 : (uint8_t)b;
	}
	case 12:
		return c->wr[12];
	case 13: case 9:
		return c->wr[13];
	case 15: case 11:
		return c->wr[15];
	default:                                /* RR10/RR14: no loop, no missing clocks */
		return 0;
	}
}

static void reset_channel(struct scc *s, int ch)
{
	struct scc_channel *c = &s->ch[ch];
	uint8_t shared9 = c->wr[9], shared2 = c->wr[2], wr5 = c->wr[5];
	memset(c->wr, 0, sizeof c->wr);
	c->wr[9] = shared9;
	c->wr[2] = shared2;
	c->wr7p = WR7P_TX_FIFO_EMPTY;
	c->pointer = 0;
	c->tx_count = c->rx_count = 0;
	c->tx_busy = c->tx_written = c->rx_first = false;
	c->ip_rx = c->ip_tx = c->ip_ext = false;
	if (c->attached && (wr5 & (WR5_DTR | WR5_RTS)) && c->port.lines)
		c->port.lines(c->port.ctx, false, false, s->tb);
}

static void command(struct scc *s, int ch, unsigned cmd)
{
	struct scc_channel *c = &s->ch[ch];
	switch (cmd) {
	case 2:                                 /* reset external/status interrupts */
		c->ip_ext = false;
		break;
	case 4:                                 /* enable interrupt on next receive character */
		c->rx_first = true;
		break;
	case 5:                                 /* reset transmit interrupt pending */
		c->ip_tx = false;
		c->tx_written = false;
		break;
	}
}

static void write_register(struct scc *s, int ch, unsigned reg, uint8_t v)
{
	struct scc_channel *c = &s->ch[ch];
	uint8_t old = c->wr[reg];

	switch (reg) {
	case 0:
		/* register pointer and commands (point high, resets) */
		c->pointer = (uint8_t)((v & 7) | ((v & 0x38) == 0x08 ? 8 : 0));
		command(s, ch, (v >> 3) & 7);
		return;
	case 1:
		c->wr[1] = v;
		if (WR1_RX_MODE(v) == 1 && WR1_RX_MODE(old) != 1)
			c->rx_first = true;
		if ((v & WR1_RX_DMA) == WR1_RX_DMA && (old & WR1_RX_DMA) != WR1_RX_DMA && c->rx_count && s->dbdma)
			dbdma_kick(s->dbdma, c->dma_rx);
		break;
	case 2:                                 /* interrupt vector: shared */
		s->ch[SCC_A].wr[2] = s->ch[SCC_B].wr[2] = v;
		break;
	case 7:
		if (c->wr[15] & WR15_WR7P)
			c->wr7p = v;
		else
			c->wr[7] = v;
		break;
	case 8:                                 /* the transmit buffer */
		if (c->attached)
			tx_put(s, ch, v);
		break;
	case 9:                                 /* master interrupt control: shared */
		switch (v & WR9_RESET_MASK) {
		case 0x40: reset_channel(s, SCC_B); break;
		case 0x80: reset_channel(s, SCC_A); break;
		case 0xC0:
			reset_channel(s, SCC_A);
			reset_channel(s, SCC_B);
			break;
		}
		s->ch[SCC_A].wr[9] = s->ch[SCC_B].wr[9] = v & (uint8_t)~WR9_RESET_MASK;
		break;
	default:
		c->wr[reg] = v;
		break;
	}
	c->pointer = 0;
	if (!c->attached)
		return;
	if (reg == 5) {
		if ((v ^ old) & (WR5_DTR | WR5_RTS)) {
			say(s, ch, "DTR %u, RTS %u", !!(v & WR5_DTR), !!(v & WR5_RTS));
			if (c->port.lines)
				c->port.lines(c->port.ctx, v & WR5_DTR, v & WR5_RTS, s->tb);
		}
		if ((v & ~old) & WR5_TX_ENABLE)
			tx_start(s, ch, s->tb);
	}
	if (reg == 3 && (v & ~old & WR3_RX_ENABLE) && c->rx_waiting && !c->rx_due_set) {
		c->rx_due = s->tb + scc_char_time(s, ch, true);
		c->rx_due_set = true;
	}
}

static void decode(uint32_t offset, int *ch, int *data)
{
	if (offset >= 0x1000) {                 /* escc layout */
		*ch = (offset & 0x20) ? SCC_A : SCC_B;
		*data = (offset & 0x10) != 0;
	} else {                                /* legacy layout */
		*ch = (offset & 2) ? SCC_A : SCC_B;
		*data = (offset & 4) != 0;
	}
}

static uint32_t scc_read(void *ctx, uint32_t offset, unsigned size)
{
	struct scc *s = ctx;
	int ch, data;
	(void)size;

	decode(offset, &ch, &data);
	struct scc_channel *c = &s->ch[ch];
	if (data) {
		int b = c->attached ? rx_take(s, ch) : -1;
		return b < 0 ? 0 : (uint32_t)b;
	}
	unsigned reg = c->pointer;
	uint8_t v = read_register(s, ch, reg);
	c->pointer = 0;
	if (s->log && c->attached && reg != 0 && reg != 1)
		say(s, ch, "RR%u -> %02x", reg, v);
	return v;
}

static void scc_write(void *ctx, uint32_t offset, unsigned size, uint32_t value)
{
	struct scc *s = ctx;
	int ch, data;
	(void)size;

	decode(offset, &ch, &data);
	struct scc_channel *c = &s->ch[ch];
	if (data) {
		if (c->attached)
			tx_put(s, ch, (uint8_t)value);
		else
			c->tx_bytes++;
		return;
	}
	if (s->log && c->attached && c->pointer)
		say(s, ch, "WR%u <- %02x", c->pointer, value & 0xFF);
	else if (s->log && c->attached && (value & 0x38) > 0x08)
		say(s, ch, "WR0 command %u", (value >> 3) & 7, 0);
	write_register(s, ch, c->pointer, (uint8_t)value);
}

const struct mmio_ops scc_mmio_ops = { scc_read, scc_write };

/* ---- DMA ---- */

static uint32_t dma_output(void *ctx, unsigned key, const uint8_t *data, uint32_t len, bool last)
{
	struct scc_channel *c = ctx;
	struct scc *s = c->scc;
	(void)key, (void)last;
	if (!c->attached) {
		c->tx_bytes += len;
		return len;                     /* (nothing there: it vanishes, as ever) */
	}
	uint32_t n = 0;
	while (n < len && c->tx_count < SCC_TX_FIFO) {
		c->tx_fifo[(c->tx_head + c->tx_count) % SCC_TX_FIFO] = data[n++];
		c->tx_count++;
	}
	c->tx_written = true;
	tx_start(s, c->index, s->tb);
	return n;
}

static uint32_t dma_input(void *ctx, unsigned key, uint8_t *data, uint32_t len, bool *end)
{
	struct scc_channel *c = ctx;
	(void)key, (void)end;
	if ((c->wr[1] & WR1_RX_DMA) != WR1_RX_DMA)
		return 0;
	uint32_t n = 0;
	int b;
	while (n < len && (b = rx_take(c->scc, c->index)) >= 0)
		data[n++] = (uint8_t)b;
	return n;
}

void scc_dma(struct scc *s, struct dbdma *d)
{
	s->dbdma = d;
	for (int ch = 0; ch < 2; ch++) {
		struct scc_channel *c = &s->ch[ch];
		c->scc = s;
		c->index = ch;
		c->dma_tx = ch == SCC_A ? 4 : 6;
		c->dma_rx = ch == SCC_A ? 5 : 7;
		dbdma_attach(d, c->dma_tx, &(struct dbdma_device){ dma_output, NULL, c });
		dbdma_attach(d, c->dma_rx, &(struct dbdma_device){ NULL, dma_input, c });
	}
}
