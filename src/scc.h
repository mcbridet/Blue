/*
 * Zilog Z85C30 SCC (serial ports A and B) with nothing attached. Registers
 * are modelled at the level the Mac OS drivers poll them: writes go through
 * the WR0 register pointer, the transmitter drains instantly, and the
 * receiver sees an idle line (hunting in synchronous modes, so LocalTalk
 * finds the network free). No interrupts are raised yet.
 *
 * Address layouts inside the 0x2000-byte window (Mac I/O + 0x12000):
 *   legacy: B control +0, A control +2, B data +4, A data +6
 *   escc:   +0x1000 + (channel A ? 0x20 : 0) + (data ? 0x10 : 0)
 */
#ifndef SYSHM_SCC_H
#define SYSHM_SCC_H

#include <stdint.h>

#include "mmio.h"

#define SCC_SIZE 0x2000u

struct scc_channel {
	uint8_t wr[16];
	uint8_t pointer;                /* register selected for the next control access */
	unsigned long tx_bytes;
};

struct scc {
	struct scc_channel ch[2];       /* [0] = A, [1] = B */
	void (*tx)(void *ctx, int channel, uint8_t byte);       /* optional sink for sent bytes */
	void *tx_ctx;
};

void scc_init(struct scc *s);
extern const struct mmio_ops scc_mmio_ops;

#endif
