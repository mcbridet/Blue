#include "priv.h"

#include <string.h>

#define MSR_750 0x0001FF77u             /* the 750's MSR bits, but POW: the processor doesn't nap here */

void priv_init(struct priv *p, uint32_t pvr, uint32_t sdr1, const uint32_t bat[16], const uint32_t sr[16])
{
	memset(p, 0, sizeof *p);
	p->msr = PRIV_USER_MSR;
	p->spr[SPR_PVR] = pvr;
	p->spr[SPR_SDR1] = sdr1;
	memcpy(&p->spr[SPR_IBAT0U], bat, 16 * sizeof bat[0]);
	memcpy(p->sr, sr, sizeof p->sr);
	p->spr[SPR_HID0] = PRIV_HID0;
	p->spr[SPR_HID1] = PRIV_HID1;
	p->spr[SPR_L2CR] = PRIV_L2CR;
}

bool priv_skeleton_key(struct priv *p, const uint32_t gpr[32])
{
	if (gpr[3] != PRIV_GARY || gpr[4] != PRIV_KEY)
		return false;
	p->msr = (p->msr & ~gpr[5]) | gpr[7];
	p->keys++;
	return true;
}

static bool known(unsigned spr)
{
	switch (spr) {
	case SPR_DSISR: case SPR_DAR: case SPR_DEC: case SPR_SDR1: case SPR_SRR0: case SPR_SRR1:
	case SPR_EAR: case SPR_PVR: case SPR_HID0: case SPR_HID1: case SPR_IABR: case SPR_DABR:
	case SPR_L2CR: case SPR_ICTC: case SPR_THRM1: case SPR_THRM2: case SPR_THRM3:
		return true;
	}
	return (spr >= SPR_SPRG0 && spr < SPR_SPRG0 + 4) || (spr >= SPR_IBAT0U && spr < SPR_IBAT0U + 16) ||
	       (spr >= SPR_MMCR0 && spr < SPR_MMCR0 + 7);
}

/*
 * The thermal assist unit compares the junction temperature with THRM1's
 * and THRM2's thresholds (7 bits from bit 2) while THRM3 enables it: TIN
 * says it's above the threshold, or below it with TID, and TIV that the
 * comparison is done (here at once).
 */
static uint32_t thermal(const struct priv *p, uint32_t thrm)
{
	if (!(p->spr[SPR_THRM3] & THRM3_E) || !(thrm & THRM_V))
		return thrm;
	unsigned threshold = thrm >> 23 & 0x7F;
	bool crossed = thrm & THRM_TID ? PRIV_TEMPERATURE < threshold : PRIV_TEMPERATURE > threshold;
	return thrm | THRM_TIV | (crossed ? THRM_TIN : 0);
}

bool priv_read(const struct priv *p, unsigned spr, uint64_t tb, uint32_t *value)
{
	if (!known(spr))
		return false;
	switch (spr) {
	case SPR_DEC:
		*value = p->spr[SPR_DEC] - (uint32_t)(tb - p->dec_tb);
		break;
	case SPR_L2CR:          /* (a global invalidate is over at once) */
		*value = p->spr[SPR_L2CR] & ~L2CR_L2IP;
		break;
	case SPR_THRM1: case SPR_THRM2:
		*value = thermal(p, p->spr[spr]);
		break;
	default:
		*value = p->spr[spr];
		break;
	}
	return true;
}

bool priv_write(struct priv *p, unsigned spr, uint64_t tb, uint32_t value)
{
	if (spr == SPR_TBL_W || spr == SPR_TBU_W)
		return true;    /* (guest time is the kernel's) */
	if (!known(spr))
		return false;
	switch (spr) {
	case SPR_PVR: case SPR_HID1:    /* read-only */
		break;
	case SPR_DEC:
		p->spr[SPR_DEC] = value;
		p->dec_tb = tb;
		break;
	case SPR_THRM1: case SPR_THRM2:
		p->spr[spr] = value & ~(THRM_TIN | THRM_TIV);
		break;
	default:
		p->spr[spr] = value;
		break;
	}
	return true;
}

bool priv_insn(struct priv *p, struct ppc_cpu *cpu)
{
	uint32_t i = cpu->exit_insn, *g = cpu->gpr;
	unsigned op = i >> 26, xo = i >> 1 & 0x3FF, rd = i >> 21 & 31, rb = i >> 11 & 31;
	unsigned spr = (i >> 16 & 0x1F) | (i >> 11 & 0x1F) << 5;

	if (op == 31 && xo == 83) {                     /* mfmsr */
		g[rd] = p->msr;
		cpu->pc += 4;
		return true;
	}
	if (!priv_supervisor(p))
		return false;
	if (op == 19 && xo == 50) {                     /* rfi */
		p->msr = p->spr[SPR_SRR1] & MSR_750;
		cpu->pc = p->spr[SPR_SRR0] & ~3u;
		return true;
	}
	if (op != 31)
		return false;
	switch (xo) {
	case 146:                                       /* mtmsr */
		p->msr = g[rd] & MSR_750;
		break;
	case 339:                                       /* mfspr */
		if (!priv_read(p, spr, cpu->tb, &g[rd]))
			return false;
		break;
	case 467:                                       /* mtspr */
		if (!priv_write(p, spr, cpu->tb, g[rd]))
			return false;
		break;
	case 595: g[rd] = p->sr[i >> 16 & 15]; break;   /* mfsr */
	case 659: g[rd] = p->sr[g[rb] >> 28]; break;    /* mfsrin */
	case 210: p->sr[i >> 16 & 15] = g[rd]; break;   /* mtsr */
	case 242: p->sr[g[rb] >> 28] = g[rd]; break;    /* mtsrin */
	case 306: case 566: case 470:                   /* tlbie, tlbsync, dcbi (no TLB or caches here) */
		break;
	default:                                        /* (tlbia among them: the 750 has none) */
		return false;
	}
	cpu->pc += 4;
	return true;
}
