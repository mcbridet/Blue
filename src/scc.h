/*
 * Zilog Z85C30 ESCC (serial channels A and B) in Mac I/O. On the iMac,
 * channel A is the internal modem's and channel B the infrared port's.
 *
 * A channel with a port attached (scc_attach()) carries data: what Mac OS
 * sends goes through a 4-byte transmit FIFO and the shift register to the
 * port, one character per character time at the bit rate Mac OS programs
 * (the baud-rate generator, clock mode, data, parity and stop bits), and the
 * port's characters come into an 8-byte receive FIFO at the same pace.
 * Both are in guest time (timebase ticks), so what Mac OS sees never
 * depends on the host. The channel raises its interrupts (receive,
 * transmit, external/status: DCD and CTS changes, which come from the
 * port), reports them in RR3 and the modified vector, and has its DMA
 * channels (DBDMA, transmit and receive), which Mac OS's serial driver
 * uses. DTR and RTS go to the port.
 *
 * A channel with nothing attached is as it always was: what Mac OS sends
 * vanishes at once, the receiver sees an idle line (hunting in synchronous
 * modes, so LocalTalk finds the network free) and it raises no interrupts.
 *
 * Address layouts inside the 0x2000-byte window (Mac I/O + 0x12000):
 *   legacy: B control +0, A control +2, B data +4, A data +6
 *   escc:   +0x1000 + (channel A ? 0x20 : 0) + (data ? 0x10 : 0)
 */
#ifndef BLUE_SCC_H
#define BLUE_SCC_H

#include <stdbool.h>
#include <stdint.h>

#include "dbdma.h"
#include "mmio.h"

#define SCC_SIZE 0x2000u
#define SCC_PCLK 3686400u               /* the SCC's clock, and its RTxC pins' */
#define SCC_TX_FIFO 4u
#define SCC_RX_FIFO 8u

enum { SCC_A = 0, SCC_B = 1 };

/* What's attached to a channel */
struct scc_port {
	/* a character has been sent (its last stop bit has gone), at guest time tb */
	void (*tx)(void *ctx, uint8_t byte, uint64_t tb);
	/* the next character for the receiver, or -1 if there's none (then scc_rx_ready() says when there is) */
	int (*rx)(void *ctx, uint64_t tb);
	/* DTR or RTS changed (asserted: true) */
	void (*lines)(void *ctx, bool dtr, bool rts, uint64_t tb);
	void *ctx;
};

struct scc_channel {
	uint8_t wr[16];
	uint8_t wr7p;                   /* WR7' (the ESCC's: WR15 bit 0 selects it) */
	uint8_t pointer;                /* register selected for the next control access */
	bool attached;
	struct scc_port port;
	/* the transmitter */
	uint8_t tx_fifo[SCC_TX_FIFO];
	unsigned tx_head, tx_count;
	bool tx_busy;                   /* a character is in the shift register ... */
	uint8_t tx_shift;
	uint64_t tx_done;               /* ... until this guest time */
	bool tx_written;                /* a character has been written since the transmit interrupt was reset */
	/* the receiver */
	uint8_t rx_fifo[SCC_RX_FIFO];
	unsigned rx_head, rx_count;
	bool rx_waiting;                /* the port has characters */
	bool rx_due_set;
	uint64_t rx_due;                /* ... the next one is in at this guest time */
	bool rx_first;                  /* "interrupt on next receive character" armed */
	/* the inputs (asserted: true) */
	bool dcd, cts;
	/* interrupts pending */
	bool ip_rx, ip_tx, ip_ext;
	unsigned dma_tx, dma_rx;        /* the DBDMA channels */
	struct scc *scc;                /* (for them) */
	int index;
	unsigned long tx_bytes, rx_bytes;
};

struct scc {
	struct scc_channel ch[2];       /* [0] = A, [1] = B */
	uint64_t tb;                    /* guest time the channels are at */
	uint32_t timebase_hz;
	struct dbdma *dbdma;            /* the DMA channels (scc_dma()), or NULL */
	bool log;
};

void scc_init(struct scc *s, uint32_t timebase_hz);
extern const struct mmio_ops scc_mmio_ops;

/* Attaches a port to a channel (its DCD and CTS asserted until scc_set_inputs() says otherwise) */
void scc_attach(struct scc *s, int ch, const struct scc_port *port);

/* The DMA channels: transmit and receive for A (4, 5) and B (6, 7), whose devices this attaches */
void scc_dma(struct scc *s, struct dbdma *d);

/* Brings the channels up to guest time tb (characters sent and received by then) */
void scc_set_time(struct scc *s, uint64_t tb);

/* The first guest time at which a channel changes by itself (UINT64_MAX: none) */
uint64_t scc_next_event(const struct scc *s);

/* The port has characters for the receiver */
void scc_rx_ready(struct scc *s, int ch);

/* The port's DCD and CTS outputs (asserted: true) */
void scc_set_inputs(struct scc *s, int ch, bool dcd, bool cts);

/* The channel's interrupt line (Mac I/O has one for each) */
bool scc_irq(const struct scc *s, int ch);

/* The bit rate the channel's transmitter (or receiver) runs at, in bits a second (0: none) */
uint32_t scc_bit_rate(const struct scc *s, int ch, bool rx);

/* Guest time one character takes, start and stop bits included */
uint64_t scc_char_time(const struct scc *s, int ch, bool rx);

#endif
