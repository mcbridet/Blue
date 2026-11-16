#include "via.h"

#include <string.h>

enum {
	REG_ORB = 0, REG_ORA_HS = 1, REG_DDRB = 2, REG_DDRA = 3,
	REG_T1CL = 4, REG_T1CH = 5, REG_T1LL = 6, REG_T1LH = 7,
	REG_T2CL = 8, REG_T2CH = 9, REG_SR = 10, REG_ACR = 11,
	REG_PCR = 12, REG_IFR = 13, REG_IER = 14, REG_ORA = 15,
};

void via_init(struct via *v)
{
	struct via_hooks hooks = v->hooks;
	memset(v, 0, sizeof *v);
	v->hooks = hooks;
	v->in_a = v->in_b = 0xFF;
	v->t1_latch = v->t2_latch = 0xFFFF;
}

/* Number of times T1 has underflowed since it was loaded. */
static uint64_t t1_underflows(const struct via *v)
{
	uint64_t elapsed = v->now - v->t1_start;
	if (elapsed <= v->t1_load)
		return 0;
	if (!(v->acr & VIA_ACR_T1_FREE))
		return 1;
	return 1 + (elapsed - v->t1_load - 1) / ((uint64_t)v->t1_latch + 2);
}

static uint16_t t1_counter(const struct via *v)
{
	uint64_t elapsed = v->now - v->t1_start;
	if (elapsed <= v->t1_load || !(v->acr & VIA_ACR_T1_FREE))
		return (uint16_t)(v->t1_load - elapsed);
	uint64_t period = (uint64_t)v->t1_latch + 2;
	return (uint16_t)(v->t1_latch - (int64_t)((elapsed - v->t1_load - 1) % period));
}

static uint16_t t2_counter(const struct via *v)
{
	return (uint16_t)(v->t2_load - (v->now - v->t2_start));
}

void via_set_time(struct via *v, uint64_t ticks)
{
	v->now = ticks;
	if (v->t1_armed || (v->acr & VIA_ACR_T1_FREE)) {
		uint64_t n = t1_underflows(v);
		if (n > v->t1_seen) {
			v->ifr |= VIA_IRQ_T1;
			v->t1_seen = n;
			if (!(v->acr & VIA_ACR_T1_FREE))
				v->t1_armed = false;
		}
	}
	if (v->t2_armed && v->now - v->t2_start > v->t2_load) {
		v->ifr |= VIA_IRQ_T2;
		v->t2_armed = false;
	}
}

uint64_t via_next_event(const struct via *v)
{
	uint64_t next = UINT64_MAX;
	/* underflow k (from 1) comes t1_load + 1 + (k - 1) * (latch + 2) ticks after the load */
	if ((v->ier & VIA_IRQ_T1) && (v->t1_armed || (v->acr & VIA_ACR_T1_FREE)))
		next = v->t1_start + v->t1_load + 1 + v->t1_seen * ((uint64_t)v->t1_latch + 2);
	if ((v->ier & VIA_IRQ_T2) && v->t2_armed && v->t2_start + v->t2_load + 1 < next)
		next = v->t2_start + v->t2_load + 1;
	return next;
}

void via_raise(struct via *v, uint8_t bits)
{
	v->ifr |= bits & 0x7F;
}

uint8_t via_port_b(const struct via *v)
{
	return (uint8_t)((v->orb & v->ddrb) | (v->in_b & ~v->ddrb));
}

static uint32_t via_read(void *ctx, uint32_t offset, unsigned size)
{
	struct via *v = ctx;
	(void)size;

	switch ((offset >> 9) & 15) {
	case REG_ORB:  return via_port_b(v);
	case REG_ORA_HS:
	case REG_ORA:  return (uint8_t)((v->ora & v->ddra) | (v->in_a & ~v->ddra));
	case REG_DDRB: return v->ddrb;
	case REG_DDRA: return v->ddra;
	case REG_T1CL:
		v->ifr &= ~VIA_IRQ_T1;
		return t1_counter(v) & 0xFF;
	case REG_T1CH: return t1_counter(v) >> 8;
	case REG_T1LL: return v->t1_latch & 0xFF;
	case REG_T1LH: return v->t1_latch >> 8;
	case REG_T2CL:
		v->ifr &= ~VIA_IRQ_T2;
		return t2_counter(v) & 0xFF;
	case REG_T2CH: return t2_counter(v) >> 8;
	case REG_SR: {
		uint8_t val = v->sr;
		v->ifr &= ~VIA_IRQ_SR;
		if (v->hooks.sr_accessed)
			v->hooks.sr_accessed(v->hooks.ctx, v, false);
		return val;
	}
	case REG_ACR:  return v->acr;
	case REG_PCR:  return v->pcr;
	case REG_IFR:  return v->ifr | (via_irq(v) ? 0x80 : 0);
	case REG_IER:  return v->ier | 0x80;
	}
	return 0;
}

static void via_write(void *ctx, uint32_t offset, unsigned size, uint32_t value)
{
	struct via *v = ctx;
	uint8_t b = (uint8_t)value;
	(void)size;

	switch ((offset >> 9) & 15) {
	case REG_ORB:
		v->orb = b;
		if (v->hooks.port_b_written)
			v->hooks.port_b_written(v->hooks.ctx, v);
		break;
	case REG_ORA_HS:
	case REG_ORA:
		v->ora = b;
		break;
	case REG_DDRB:
		v->ddrb = b;
		if (v->hooks.port_b_written)
			v->hooks.port_b_written(v->hooks.ctx, v);
		break;
	case REG_DDRA:
		v->ddra = b;
		break;
	case REG_T1CL:
	case REG_T1LL:
		v->t1_latch = (uint16_t)((v->t1_latch & 0xFF00) | b);
		break;
	case REG_T1CH:
		v->t1_latch = (uint16_t)((v->t1_latch & 0x00FF) | b << 8);
		v->t1_load = v->t1_latch;
		v->t1_start = v->now;
		v->t1_armed = true;
		v->t1_seen = 0;
		v->ifr &= ~VIA_IRQ_T1;
		break;
	case REG_T1LH:
		v->t1_latch = (uint16_t)((v->t1_latch & 0x00FF) | b << 8);
		v->ifr &= ~VIA_IRQ_T1;
		break;
	case REG_T2CL:
		v->t2_latch = (uint16_t)((v->t2_latch & 0xFF00) | b);
		break;
	case REG_T2CH:
		v->t2_load = (uint16_t)((v->t2_latch & 0x00FF) | b << 8);
		v->t2_start = v->now;
		v->t2_armed = true;
		v->ifr &= ~VIA_IRQ_T2;
		break;
	case REG_SR:
		v->sr = b;
		v->ifr &= ~VIA_IRQ_SR;
		if (v->hooks.sr_accessed)
			v->hooks.sr_accessed(v->hooks.ctx, v, true);
		break;
	case REG_ACR:
		v->acr = b;
		break;
	case REG_PCR:
		v->pcr = b;
		break;
	case REG_IFR:
		v->ifr &= (uint8_t)~(b & 0x7F);
		break;
	case REG_IER:
		if (b & 0x80)
			v->ier |= b & 0x7F;
		else
			v->ier &= (uint8_t)~(b & 0x7F);
		break;
	}
}

const struct mmio_ops via_mmio_ops = { via_read, via_write };

bool via_pure_read(const void *ctx, uint32_t offset)
{
	(void)ctx;
	unsigned reg = (offset >> 9) & 15;
	return reg != REG_T1CL && reg != REG_T2CL && reg != REG_SR;     /* (they clear interrupt flags) */
}
