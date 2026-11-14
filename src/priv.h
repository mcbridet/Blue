/*
 * Supervisor mode for Mac OS's code. Everything above the kernel runs in
 * user mode, but the NanoKernel has one way out of it, Gary Davidian's
 * skeleton key: ResetSystem (kernel call 2, the 68k RESET) with r3 'Gary'
 * and r4 0x05051956 doesn't reset; the caller goes on after the call with
 * MSR = (MSR & ~r5) | r7 (NanoKernel 1's KCallResetSystem, NanoKernel 2's
 * kcResetSystem). Utilities use it to read the processor's registers.
 *
 * syshm's processor always runs guest code in user mode, so the kernel
 * keeps that MSR here (the blue task's) and, while its PR bit is clear, does
 * the privileged instructions that come to it: mfmsr and mtmsr (setting PR
 * again ends supervisor mode), rfi, mfspr and mtspr of an iMac's PowerPC
 * 750's supervisor registers, mfsr, mtsr, mfsrin, mtsrin, and the TLB and
 * cache instructions (tlbie, tlbia, tlbsync, dcbi), which do nothing. The
 * registers start as Mac OS leaves them on the iMac and keep what's
 * written, with no effect on the machine; only what the hardware itself
 * changes is modelled on reading back (the decrementer, L2CR's invalidate,
 * the thermal assist unit's comparisons). In user mode, mfmsr reads the MSR
 * and the rest is a privileged instruction exception.
 */
#ifndef SYSHM_PRIV_H
#define SYSHM_PRIV_H

#include <stdbool.h>
#include <stdint.h>

#include "ppc.h"

#define MSR_EE 0x8000u
#define MSR_PR 0x4000u
#define MSR_IR 0x0020u
#define MSR_DR 0x0010u
#define PRIV_USER_MSR 0x0000D032u       /* EE PR ME IR DR RI, as the NanoKernel starts Mac OS */

#define PRIV_GARY 0x47617279u           /* 'Gary' */
#define PRIV_KEY 0x05051956u

/* Supervisor registers (SPR numbers) */
enum {
	SPR_DSISR = 18,
	SPR_DAR = 19,
	SPR_DEC = 22,
	SPR_SDR1 = 25,
	SPR_SRR0 = 26,
	SPR_SRR1 = 27,
	SPR_SPRG0 = 272,
	SPR_EAR = 282,
	SPR_TBL_W = 284,
	SPR_TBU_W = 285,
	SPR_PVR = 287,
	SPR_IBAT0U = 528,               /* IBAT0U, IBAT0L, ... DBAT3L: 528-543 */
	SPR_MMCR0 = 952,                /* and PMC1, PMC2, SIA, MMCR1, PMC3, PMC4 to 958 */
	SPR_HID0 = 1008,
	SPR_HID1 = 1009,
	SPR_IABR = 1010,
	SPR_DABR = 1013,
	SPR_L2CR = 1017,
	SPR_ICTC = 1019,
	SPR_THRM1 = 1020,
	SPR_THRM2 = 1021,
	SPR_THRM3 = 1022,
};

/* The iMac's processor as Mac OS runs it */
#define PRIV_HID0 0x8010C0A4u           /* EMCP DPM ICE DCE SGE BTIC BHT */
#define PRIV_HID1 0xA0000000u           /* PLL_CFG 1010: 4x (266 MHz on a 66 MHz bus) */
#define PRIV_L2CR 0xA9000000u           /* enabled, 512 KB, the core clock / 2, pipelined burst SRAM */
#define PRIV_TEMPERATURE 40u            /* degrees C, as the thermal assist unit finds it */

#define L2CR_L2IP 0x00000001u
#define THRM_TIN 0x80000000u
#define THRM_TIV 0x40000000u
#define THRM_TID 0x00000004u
#define THRM_V 0x00000001u
#define THRM3_E 0x00000001u

struct priv {
	uint32_t msr;
	uint32_t spr[1024];             /* by number: what's been written (see priv_read()) */
	uint32_t sr[16];                /* segment registers */
	uint64_t dec_tb;                /* time base when the decrementer was spr[SPR_DEC] */
	unsigned keys;                  /* times the skeleton key was used */
};

/*
 * User mode, and the 750's registers: pvr, SDR1, the BATs (IBAT0U, IBAT0L,
 * ... DBAT3L) and segment registers as the kernel sets them for Mac OS.
 */
void priv_init(struct priv *p, uint32_t pvr, uint32_t sdr1, const uint32_t bat[16], const uint32_t sr[16]);

static inline bool priv_supervisor(const struct priv *p)
{
	return !(p->msr & MSR_PR);
}

/* ResetSystem's test: with the key in r3 and r4, sets the MSR from r5 and r7 and returns true. */
bool priv_skeleton_key(struct priv *p, const uint32_t gpr[32]);

/*
 * The privileged instruction cpu->exit_insn at cpu->pc (an exit's), at time
 * base cpu->tb: done, with pc moved on (rfi's to SRR0), or false having
 * changed nothing: in user mode anything but mfmsr (a privileged instruction
 * exception), in supervisor mode what a 750 doesn't have (an illegal one).
 */
bool priv_insn(struct priv *p, struct ppc_cpu *cpu);

/* A supervisor register as mfspr reads it at time base `tb`; false if the 750 has none */
bool priv_read(const struct priv *p, unsigned spr, uint64_t tb, uint32_t *value);
bool priv_write(struct priv *p, unsigned spr, uint64_t tb, uint32_t value);

#endif
