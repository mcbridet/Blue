# The JIT

Blue translates PowerPC code to AArch64 or x86-64 as it runs, with
exactly the interpreter's results. This document describes how; the
x86-64 translator's plan and measurements are in
[x86-jit-plan.md](x86-jit-plan.md), and the interpreter in
[architecture.md](architecture.md#the-processor).

## Overview

Blue has a JIT to AArch64 and x86-64, on Linux and macOS (`src/jit.c`, the
runtime both share; `src/jit_a64.c` and `src/a64.h`, `src/jit_x86.c` and
`src/x86.h`, the translators; `--interp` turns it off). What follows is
the AArch64 JIT; the x86-64 one works the same way, with differences
below (and in [x86-jit-plan.md](x86-jit-plan.md)). Blocks of up to 128
instructions are translated
natively once they have run 16 times (`BLUE_JIT_HOT`). A block ends at
an unconditional or computed branch; past a conditional one it goes on
along the not-taken path, and the taken path leaves through out-of-line
code. Rarer instructions call the interpreter from inside the block
(`lwarx` and `stwcx.`, which the CD boots run nearly 2 million times
a boot, are translated);
traps, system calls and host calls go straight to the kernel (but for
those made [in place](#kernel-calls-in-place)); anything
that faults leaves the block so the interpreter executes it (but for
[device registers](#device-registers-in-place)). The time base is read natively.

## Exactness

Blocks run only when the instruction budget covers them, and
each instruction checks before it changes anything, so instruction
counts, device timing and faults are exactly the interpreter's (boots,
the install and `--fixed-time` runs give identical results either
way; `tests/test_jit.c` compares random programs).

## Kernel calls in place

The 68k emulator's switches to native code and
back (kernel calls 1 and 0, RunAlternateContext and
ReturnFromException: 7 million in 4 billion instructions of 9.1 from
its CD, 2.8 million in 8.5's) and NanoKernel 2's quick `sc` (see [Mac
OS 9.1](bring-up.md#mac-os-91-from-its-cd)) don't leave generated code or the interpreter. For the
switches, generated code calls the kernel's function from inside the
block (`kcall` in `src/ppc.h`), which makes the call in its common
cases (no interrupt pending; verdict 0 or 32-255) with the same code
as at an exit, then goes on through the block table. The kernel allows
it before each run only while an exit would do nothing more
(Multiprocessing Services tick due, task switch, software interrupt or
preemption waiting: `in_place()` in `src/nk.c`); with the devices to
be brought up to date (after an exit that may have touched them), the
call does that first, at the instruction count and time base that
exit would have had, and goes on unless an interrupt changed or the
loop has more to do. As at an exit the call retires nothing, so runs
are the same instruction for instruction. Kernel exits in that 9.1 run
went from 11.7 to 1.5 million (8.5: 2.8 million to 153,000), host time
from 1.50 to 1.32 s (8.5: 1.03 to 1.02 s). The context switch copies
registers in vectors (NEON; AVX2 where the processor has it): a call
in place costs about 17 ns.

## Device registers in place

Loads and stores to device registers (1.6
million in 4 billion instructions of the 8.5 CD boot: the Time Manager
programming and reading VIA timer 2, its interrupt flags, and the USB
driver polling the frame number; 0.7 million for 8.5 and 8.6
installed, nearly all VIA) don't leave generated code or the
interpreter either. Both offer a load or store that faults to the
kernel's hook (`mmio` in `src/ppc.h`, `device_hook()` in `src/nk.c`),
with the instruction count and time base as at the exit; it does what
the kernel's loop does at that exit, with the same code (devices
brought up to date, the access, interrupts, the next device event,
what the engines may do in place), and the run goes on; or it stops
right after the access when the loop has more to do (interrupts that
changed, a preemption, a Multiprocessing Services tick, a Cuda
shutdown or restart: about 2% of accesses). Not on a run's last
instruction (see [When devices are serviced](debugging.md#when-devices-are-serviced)), nor with `clock`. Generated code gets
the hook's path, out of line, for instructions whose fastmem access
faulted on a page that isn't mapped; it writes all registers back and
reloads them afterwards (the hook may change any, CR when it posts an
interrupt). In the 8.5 CD boot device-register exits went from 1.64
million to 36,000 and host time from 1.00 to 0.96 s (8.5 installed:
0.87 to 0.86 s; 8.6: 1.24 to 1.21 s; 9.1's CD, with 17,000, as before)
on an M5, where an access cost about 50 ns with its exit and costs
about 25 now, nearly all of it the devices' own bookkeeping.
A read that changes nothing (the VIA's registers but for T1CL, T2CL
and SR, which clear interrupt flags; the USB controller's), with the
devices not due to change by themselves, skips the bookkeeping after
the access (interrupts, the next device event), which would find
everything as it was: about 12 ns less, for most of 8.5's accesses.
`BLUE_DEVICE_EXITS=1` has every access exit as before, for
comparisons. Stores to the NanoKernel's info records, which the
kernel keeps read-only to see them (the 68k emulator's, mostly), go
the same way: the hook makes them as the exit would (`info_store()`),
and generated code offers it stores that fault on a page that isn't
writable too.

## Registers

CR, LR, r1, r3, r4, r24, r27 and r29 (the hottest across the
68k emulator and native code) live in host registers while generated
code runs; others are cached within a block and written back at exits
(each side exit stores what is dirty at its point).

## Control flow

Exits to known addresses are patched into direct branches
between blocks; computed ones look the target up in a two-level table
(guest page, word) inline. Blocks follow unconditional branches into the
ROM, so each 68k opcode handler gets its own copy of the emulator's
dispatch code and of its indirect branch.

## Memory ("fastmem")

Guest memory is mapped a second time (a memfd on
Linux, `mach_vm_remap()` on macOS) for generated code with host page
protections that follow the guest's page flags (and write-protect pages
holding translated code). Loads and stores then need no checks; one
that faults (a device register, an unmapped or read-only page) is sent
by a SIGSEGV handler (SIGBUS on macOS) to the interpreter, and
translated with flag checks from then on. A few thousand do so in a
boot. `BLUE_JIT_FASTMEM=0` checks every access. In a window the
framebuffer's pages are write-protected there too while they're watched
for the window (`MEM_WATCH`, see [window.md](window.md#drawing-the-screen)): a store to one makes the
handler let the page go and the store go ahead, the instruction as it
was; checked stores call into C for watched pages.

## Code changes

Code changes are tracked per instruction word: stores (by either engine,
or the host, e.g. disk reads) to a page with translated code drop only
the blocks translated from those words, which matters because the 68k
emulator rewrites its own translation cache all the time. Rewritten
code must run 16 times again before it's translated: most of the 68k
emulator's translations are replaced after a few runs (a boot
translated 222,000 blocks before this, 140,000 after).

## Floating point

Arithmetic, conversions, compares, moves, FPR loads
and stores and the FPSCR instructions (mffs, mtfsf, mtfsfi, mtfsb0,
mtfsb1, mcrfs) run natively, arithmetic where the interpreter's fast
path would apply (round to nearest, no exceptions enabled, operands and
results well inside the normal range, so that inexact is the only
possible exception and an error-free transformation finds it). The
FPSCR comes out exactly as the interpreter's; anything else calls the
interpreter for that instruction from out-of-line code. FPRs are cached
in host registers within a block as GPRs are (sixteen: d8-d15, which
calls into C keep, and d24-d31), and the rounding mode and enables are
checked once a block (again after anything that may change them): in
another mode each FP operation leaves the block for the interpreter.
The FPSCR is kept lazily: XX and FX at once (they change only while XX
is clear; checks only the inexact test needs are skipped while it's
set), but an operation's FR, FI and result class, which the next one
replaces, wait. Its record (the operation and its operands, still in
their FPRs) is itself only stored when something might look (an exit,
a call into C, an FPSCR reader) or an operand is about to change; then
a native routine (mffs and the like) or the interpreter (C) replays the
operation to work them out. Keeping them exactly at every operation
cost QuickDraw 3D's code (Bugdom) 13%. An lfd of the two words a block
has just stored with stw (how compilers turn integers into doubles) is
put together from the registers stored: the host can't forward two
stores to one load.

## Hosts

Generated code is the same on Linux and macOS. It never uses
x18, which macOS reserves; on macOS the code buffer is `MAP_JIT`
memory, which the emulation thread makes writable only while it writes
code (`pthread_jit_write_protect_np()`, about 25 ns a switch). What's
left that only macOS pays (those switches, its slower fault signals) is
under 1% of a boot.

## x86-64

The x86-64 JIT ([x86-jit-plan.md](x86-jit-plan.md)) has the same blocks,
links, budget, exits, hooks and time base, with integer, compare,
condition-register and branch instructions translated, loads and
stores (fastmem, as above, with MOVBE; checked once one faults; stores
to pages with translated code in place), floating point as above (with
FMA3; FPRs cached in xmm6-xmm15), and kernel calls, the quick `sc` and
device registers in place as above (byte-reversed accesses to device
registers too, which AArch64 leaves to the interpreter). String
instructions call the interpreter from inside the block, as on
AArch64. It needs x86-64-v3 (AVX2, FMA3,
BMI1/2, LZCNT, MOVBE; other processors interpret, and Blue says
what's missing). Five guest registers live in host registers (CR, LR,
r3, r4, r24) and four more are cached within a block. Blocks' main
lines are packed together and their out-of-line code (but for the
taken paths of conditional branches) kept apart, which the Zen 5's
small instruction caches gain from (its front end, starved about half
the time, is what limits these boots there). Its code takes
about as many host instructions per guest instruction as AArch64's:
in the CD boots, everything included, 1.04-1.10 times AArch64's
(counted by QEMU; [x86-jit-plan.md](x86-jit-plan.md)). On an AMD Zen 5 (Linux) the
8.5, 9.1 and 9.2.2 CD boots took 1.46, 1.54 and 1.51 s when it was
finished (interpreted: 14.7, 14.5 and 10.2 s); 9.2.2's takes 1.21 s
now. Under Rosetta 2,
`ROSETTA_ADVERTISE_AVX=1` in the environment has it offer x86-64-v3.
The 8.5, 9.1 and 9.2.2 CD boots in instruction time end exactly as on
AArch64, and so do an installed 9.2.2's start and Unreal Tournament's
intro on 8.6; under Rosetta on an M5 the 8.5 CD's 4 billion
instructions take about 5.5 s (interpreted: 21 s), about 30% of it in
Rosetta's handling of Blue's writes to code that has run (new blocks,
links: about 13 µs each), a cost x86 processors don't have.

## Profiling

`BLUE_JIT_PROFILE=1` samples where host time goes (translated blocks
by guest address, stubs, C functions) and prints it at the end;
`BLUE_JIT_PROFILE=FROM:TO` samples only from instruction FROM to TO.
On Linux it's SIGPROF; on macOS, which delivers SIGPROF at the next
system call rather than where the time went, a thread of its own
stops the emulation thread every 250 µs to read its pc.
`BLUE_PERF=1` reads the host processor's counters for the emulation
thread (Linux): every second, guest instructions per second and host
instructions and cycles per guest instruction; a total at the end.

## Speed

Speed on an M1 Max: the boot to the Finder (about 3 billion instructions)
takes 1.1-1.8 s with the JIT, which runs the 68k emulator's code at about
2.5 billion PowerPC instructions per second (about 5 host instructions
and 1.2 cycles each; much of the rest is mispredicted indirect jumps,
which Apple's emulator makes by design); the interpreter manages about
200 million. The Mac OS 8.5 install takes under 25 s. On an M5 under
macOS, the same boot takes 0.8 s with the JIT and 11 s interpreted.
Memory copies stay in Mac OS's own code: BlockMove and its relatives
(InterfaceLib's, and the 68k emulator's for the `_BlockMove` trap) take
0.3-1.2% of guest instructions in the CD boots and a game's startup, and
1-2% of host time, mostly in copies too small to be worth a kernel exit
([plan.md](plan.md#findings), Findings).
