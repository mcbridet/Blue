/*
 * PowerPC floating point for the interpreter: loads/stores conversions and
 * the opcode 59/63 instructions. Arithmetic uses the host's IEEE-754 doubles;
 * FPSCR rounding mode, sticky exception bits and FPRF are maintained.
 * Enabled FP exceptions (FPSCR VE/OE/UE/ZE/XE) are not raised.
 */
#ifndef SYSHM_PPC_FP_H
#define SYSHM_PPC_FP_H

#include <stdint.h>

#include "ppc.h"

/* Exact bit-level conversions as done by lfs/stfs (no rounding, NaNs preserved). */
uint64_t ppc_fp_single_to_double(uint32_t s);
uint32_t ppc_fp_double_to_single(uint64_t d);

/*
 * Sets the FPSCR as mtfsf with every field would: the summary bits VX and
 * FEX follow from the others. (Code that runs FP instructions natively
 * counts on their always being consistent.)
 */
void ppc_fp_set_fpscr(struct ppc_cpu *c, uint32_t fpscr);

/* Executes an opcode 59 or 63 instruction. Returns false if it isn't a valid one. */
bool ppc_fp_exec(struct ppc_cpu *c, uint32_t insn);

/* Whether common arithmetic may skip the host's exception flags (for tests: always the same results) */
extern bool ppc_fp_fast_path;
extern unsigned long ppc_fp_fast_ops;   /* operations it did */

#endif
