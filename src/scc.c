#include "scc.h"

#include <string.h>

enum { CH_A = 0, CH_B = 1 };

/* RR0 bits */
#define RR0_TX_EMPTY 0x04
#define RR0_DCD 0x08
#define RR0_SYNC_HUNT 0x10
#define RR0_CTS 0x20
#define RR0_TX_UNDERRUN 0x40

#define RR1_ALL_SENT 0x01
#define RR1_RESIDUE_8BIT 0x06

/* WR9 */
#define WR9_VIS 0x01                    /* vector includes status */
#define WR9_STATUS_HIGH 0x10
#define WR9_RESET_MASK 0xC0

void scc_init(struct scc *s)
{
	void (*tx)(void *, int, uint8_t) = s->tx;
	void *ctx = s->tx_ctx;
	memset(s, 0, sizeof *s);
	s->tx = tx;
	s->tx_ctx = ctx;
}

static void reset_channel(struct scc_channel *c)
{
	uint8_t shared9 = c->wr[9], shared2 = c->wr[2];
	memset(c->wr, 0, sizeof c->wr);
	c->wr[9] = shared9;
	c->wr[2] = shared2;
	c->pointer = 0;
}

/* RR2 read through channel B: the vector with "no interrupt pending" status (011). */
static uint8_t modified_vector(const struct scc *s)
{
	uint8_t v = s->ch[CH_A].wr[2], wr9 = s->ch[CH_A].wr[9];
	if (!(wr9 & WR9_VIS))
		return v;
	if (wr9 & WR9_STATUS_HIGH)
		return (uint8_t)((v & 0x8F) | 0x60);    /* status in bits 4-6, reversed: 110 */
	return (uint8_t)((v & 0xF1) | 0x06);            /* status in bits 1-3: 011 */
}

static uint8_t read_register(const struct scc *s, int ch, unsigned reg)
{
	const struct scc_channel *c = &s->ch[ch];

	switch (reg) {
	case 0: case 4:
		return RR0_TX_EMPTY | RR0_DCD | RR0_SYNC_HUNT | RR0_CTS | RR0_TX_UNDERRUN;
	case 1: case 5:
		return RR1_ALL_SENT | RR1_RESIDUE_8BIT;
	case 2: case 6:
		return ch == CH_A ? c->wr[2] : modified_vector(s);
	case 3: case 7:
		return 0;                       /* no interrupts pending */
	case 8:
		return 0;                       /* receive buffer: nothing arrives */
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

static void write_register(struct scc *s, int ch, unsigned reg, uint8_t v)
{
	struct scc_channel *c = &s->ch[ch];

	switch (reg) {
	case 0:
		/* register pointer and commands (point high, resets); nothing is latched here */
		c->pointer = (uint8_t)((v & 7) | ((v & 0x38) == 0x08 ? 8 : 0));
		return;
	case 2:                                 /* interrupt vector: shared */
		s->ch[CH_A].wr[2] = s->ch[CH_B].wr[2] = v;
		break;
	case 9:                                 /* master interrupt control: shared */
		switch (v & WR9_RESET_MASK) {
		case 0x40: reset_channel(&s->ch[CH_B]); break;
		case 0x80: reset_channel(&s->ch[CH_A]); break;
		case 0xC0:
			reset_channel(&s->ch[CH_A]);
			reset_channel(&s->ch[CH_B]);
			break;
		}
		s->ch[CH_A].wr[9] = s->ch[CH_B].wr[9] = v & (uint8_t)~WR9_RESET_MASK;
		break;
	default:
		c->wr[reg] = v;
		break;
	}
	c->pointer = 0;
}

static void decode(uint32_t offset, int *ch, int *data)
{
	if (offset >= 0x1000) {                 /* escc layout */
		*ch = (offset & 0x20) ? CH_A : CH_B;
		*data = (offset & 0x10) != 0;
	} else {                                /* legacy layout */
		*ch = (offset & 2) ? CH_A : CH_B;
		*data = (offset & 4) != 0;
	}
}

static uint32_t scc_read(void *ctx, uint32_t offset, unsigned size)
{
	struct scc *s = ctx;
	int ch, data;
	(void)size;

	decode(offset, &ch, &data);
	if (data)
		return 0;
	struct scc_channel *c = &s->ch[ch];
	uint8_t v = read_register(s, ch, c->pointer);
	c->pointer = 0;
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
		c->tx_bytes++;
		if (s->tx)
			s->tx(s->tx_ctx, ch, (uint8_t)value);
		return;
	}
	write_register(s, ch, c->pointer, (uint8_t)value);
}

const struct mmio_ops scc_mmio_ops = { scc_read, scc_write };
