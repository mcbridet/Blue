#ifndef SYSHM_PPC_H
#define SYSHM_PPC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "mem.h"

/*
 * User-mode 32-bit PowerPC (G3/G4) interpreter.
 *
 * Everything above the NanoKernel runs in PowerPC user mode, so this core only
 * implements the user ISA. Anything that would enter the kernel on real
 * hardware (sc, traps, privileged instructions, faults) stops execution and
 * returns to the native kernel with `pc` still pointing at the instruction.
 */

enum ppc_exit {
	PPC_EXIT_BUDGET,    /* instruction budget used up; just call ppc_run again */
	PPC_EXIT_SC,        /* sc */
	PPC_EXIT_TRAP,      /* tw/twi whose condition was true (NanoKernel calls) */
	PPC_EXIT_PRIV,      /* privileged instruction in user mode */
	PPC_EXIT_ILLEGAL,   /* reserved/invalid instruction */
	PPC_EXIT_UNIMPL,    /* valid, but not implemented here yet (FP, AltiVec, ...) */
	PPC_EXIT_DSI,       /* data access to an unmapped or protected address */
	PPC_EXIT_ISI,       /* instruction fetch from a non-executable address */
	PPC_EXIT_HOSTCALL,  /* primary opcode 6: call into native code, selector in low 26 bits */
	PPC_EXIT_BREAK,     /* pc entered [break_lo, break_hi) or is break_pc (instruction not executed) */
	PPC_EXIT_HOOK,      /* a branch arrived at hook_pc (the branch is done, the target isn't),
			       or the match hook stopped at its pc (nothing there done) */
	PPC_EXIT_NONE,      /* (internal: keep going) */
};

#define PPC_NO_HOOK 1u      /* never a branch target */

#define XER_SO 0x80000000u
#define XER_OV 0x40000000u
#define XER_CA 0x20000000u

/* CR field bits */
#define CR_LT 8u
#define CR_GT 4u
#define CR_EQ 2u
#define CR_SO 1u

#define PPC_OPCODE_HOSTCALL 6u
#define PPC_HOSTCALL_INSN(selector) (PPC_OPCODE_HOSTCALL << 26 | ((selector) & 0x03FFFFFFu))

struct ppc_cpu {
	uint32_t gpr[32];
	uint64_t fpr[32];       /* raw IEEE-754 double bits */
	uint32_t cr, xer, lr, ctr, pc;
	uint32_t fpscr;
	uint32_t vrsave;
	uint64_t tb;            /* timebase */
	uint32_t tb_step;       /* added per retired instruction (1; 0 when the host clock drives it) */

	uint32_t resv_addr;     /* lwarx/stwcx. reservation */
	bool resv_valid;

	/* Set on exit */
	uint32_t exit_insn;     /* instruction word at pc */
	uint32_t fault_addr;    /* effective address for DSI/ISI */
	bool fault_write;

	uint64_t icount;        /* instructions retired */
	FILE *trace;            /* if set, log every instruction */
	uint32_t break_lo, break_hi;    /* exit before executing anything in this range */
	uint32_t break_pc;              /* ... or at this pc (PPC_NO_HOOK: none) */
	uint32_t trace_lo, trace_hi;    /* if trace_hi: trace only in this range (with counts) */
	uint32_t trace_insn;            /* ... and only this instruction word, if set */
	bool break_skip;                /* let the next instruction through once */
	int trace_reg;          /* also log this GPR in the trace (-1 for none) */
	uint32_t hook_pc;       /* exit when a branch arrives here; checked on branches only */
	/*
	 * The match hook, for code that many callers share: exit before running
	 * the instruction at hook_match_pc if the halfword at the address in
	 * GPR hook_match_reg is hook_match_value. Stopping there sets
	 * hook_match_skip, which lets the next arrival through (resuming).
	 * PPC_NO_HOOK for none.
	 */
	uint32_t hook_match_pc;
	uint16_t hook_match_value;
	uint8_t hook_match_reg;
	bool hook_match_skip;
	/*
	 * NanoKernel 2's quick system calls, which `sc` answers itself while
	 * the time base is below sc_until (the kernel's to set; 0: never):
	 * r0 -1 becomes sc_blue, -2 sc_word, and CR0 GT if sc_blue isn't 0,
	 * else EQ (SO from XER). As when the kernel answers, that retires
	 * nothing.
	 */
	uint64_t sc_until;
	uint32_t sc_blue, sc_word;
	/*
	 * Kernel calls (twi 31,r31,n) the kernel makes without an exit, while
	 * the time base is below kcall_until (0: never): kcall(cpu, n), with
	 * pc at the call (but the time base and instruction count not
	 * current), makes it, leaving pc where to go on, or returns false
	 * having changed nothing (the call exits then, as any other). That
	 * retires nothing either.
	 */
	uint64_t kcall_until;
	bool (*kcall)(struct ppc_cpu *cpu, unsigned n);

	struct mem *mem;
	struct jit *jit;        /* translate to host code (NULL: interpret) */
};

struct jit;

void ppc_init(struct ppc_cpu *cpu, struct mem *mem);

/* Runs up to `budget` instructions (with the JIT if there is one and no tracing or breakpoints). */
enum ppc_exit ppc_run(struct ppc_cpu *cpu, uint64_t budget);

/* The same with the interpreter only. */
enum ppc_exit ppc_interp(struct ppc_cpu *cpu, uint64_t budget);

/* ... stopping after the first branch, or the last instruction of a page */
enum ppc_exit ppc_interp_block(struct ppc_cpu *cpu, uint64_t budget);

/*
 * Executes one instruction that isn't a branch; false if it can't complete
 * (an exception or a kernel exit), and then nothing has changed. The time
 * base must be current (no mftb). For the JIT's slow paths.
 */
bool ppc_step(struct ppc_cpu *cpu, uint32_t insn, uint32_t pc);
const char *ppc_exit_name(enum ppc_exit e);
void ppc_dump(const struct ppc_cpu *cpu, FILE *f);

#endif
