/*
 * Translates guest PowerPC code into host (AArch64 or x86-64) code, block
 * by block, on Linux and macOS.
 *
 * A block is a straight run of instructions ending at a branch (or
 * following unconditional ones into the ROM). Guest registers are written
 * back to struct ppc_cpu at every exit, so each exit is a precise state.
 * Anything unusual (a fault, a device register, an instruction that needs
 * the kernel) leaves the block, and the interpreter executes that
 * instruction; rarer instructions call the interpreter from inside the
 * block. Blocks run only when the instruction budget covers them, so
 * instruction counts (and with them device timing) are exactly the
 * interpreter's.
 *
 * Guest memory is accessed through a second mapping of it whose host page
 * protection reflects the guest's page flags, so that loads and stores need
 * no checks; the few that fault are sent to the interpreter (and checked
 * from then on). This installs a SIGSEGV and SIGBUS handler (macOS reports
 * these faults as SIGBUS); BLUE_JIT_FASTMEM=0 turns it off.
 *
 * Stores to a page that code was translated from (MEM_CODE) invalidate the
 * blocks translated from those words, whoever makes them.
 */
#ifndef BLUE_JIT_H
#define BLUE_JIT_H

#include <stdint.h>
#include <stdio.h>

#include "mem.h"
#include "ppc.h"

struct jit;

/* NULL if this host has no JIT or no executable memory can be had (jit_unavailable() says which). */
struct jit *jit_create(struct mem *mem);
const char *jit_unavailable(void);
void jit_free(struct jit *j);

enum ppc_exit jit_run(struct jit *j, struct ppc_cpu *c, uint64_t budget);

void jit_print_stats(const struct jit *j, FILE *f);

#endif
