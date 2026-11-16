# The JIT

Blue translates PowerPC code to AArch64 or x86-64 as it runs, with
exactly the interpreter's results. This document describes how, on both
hosts, and what has been measured; the interpreter is in
[architecture.md](architecture.md#the-processor).

## Overview

Blue has a JIT to AArch64 and x86-64, on Linux and macOS (`src/jit.c`, the
runtime both share; `src/jit_a64.c` and `src/a64.h`, `src/jit_x86.c` and
`src/x86.h`, the translators; `--interp` turns it off). What follows is
the AArch64 JIT; the x86-64 one works the same way, with differences
[below](#x86-64). Blocks of up to 128
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

## The runtime and the translators

`src/jit.c` is the runtime both hosts share: the block table, the pages
blocks were translated from and their invalidation, links, fastmem and
its fault sites and handler, the C functions generated code calls, the run
loop, statistics and the profiler. A translator for each host makes the
code (`src/jit_a64.c`, built only on AArch64; `src/jit_x86.c`, only on
x86-64), and `src/jit_int.h` is what they share: the runtime offers
translators pages, blocks and fault sites, and a translator offers the
runtime its code buffer, its stubs, translation, links, retiring a block,
and the pc in a signal's context (`arch_*`). One translator over an
abstract emitter or an intermediate form would have meant rewriting the
AArch64 functions, which are written to that architecture's strengths
(bitfield moves for `rlwinm` and `rlwimi`, CSEL, TBZ and CBZ, w5 holding
the FPSCR); a copy of the runtime would have meant every fix to its subtle
parts made twice. When the runtime moved out of `jit.c`, AArch64's
generated code stayed byte for byte the same (checked by hashing every
block over the fuzzer's programs and the CD boots).

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

The x86-64 translator has the same blocks, links, budget, exits, hooks
and time base, with integer, compare, condition-register and branch
instructions translated, loads and stores (fastmem, as above, with MOVBE;
checked once one faults; stores to pages with translated code in place),
floating point as above (with FMA3), and kernel calls, the quick `sc` and
device registers in place as above. String instructions call the
interpreter from inside the block, as on AArch64 (`lswx` and `stswx` are
99% of them, from the 68k emulator's `_BlockMove`, and 0.01-0.02% of a
boot's instructions). Its code takes about as many host instructions per
guest instruction as AArch64's: in the CD boots, everything included,
1.04-1.10 times AArch64's ([below](#x86-64-against-aarch64)).

### The processor it needs

The translator needs x86-64-v3: AVX2, FMA3, BMI1 and BMI2, LZCNT and
MOVBE. FMA3 does the fused multiply-adds and the error-free
transformations, the encoder (`src/x86.h`) uses VEX three-operand forms
throughout, MOVBE does byte-swapped accesses, and BMI2's SHLX, SHRX, SARX,
RORX, BZHI and MULX and LZCNT do shifts, rotates and counts; there are no
fallback paths inside generated code. Blue checks for them at run time
with CPUID and XGETBV (the operating system must save the YMM state), and
without them it interprets, with one line saying what's missing (under
Rosetta, also that `ROSETTA_ADVERTISE_AVX=1` in the environment has
Rosetta offer them). `BLUE_JIT_HIDE` takes features as missing, for the
tests. The C code stays built for base x86-64, so one binary runs
everywhere. Processors without x86-64-v3 (Sandy and Ivy Bridge, Pentium
and Celeron parts up to 2020, older Atoms, AMD's before Excavator, the
2013 Mac Pro) interpret.

### Host registers

| Host | Role |
|---|---|
| rbx | `struct ppc_cpu *` |
| r14 | instructions left in the budget |
| r15 | the fastmem view's base, guest address 0 |
| rbp, r12, r13 | CR, r24, r3 (callee-saved) |
| r10, r11 | r4, LR (written back around calls into C) |
| rsi, rdi, r8, r9 | the register cache: four slots, as on AArch64 |
| rax, rcx, rdx | scratch (and division's and MULX's implicit operands) |
| xmm6-xmm15 | the FPR cache: ten slots |
| xmm0-xmm5 | FP scratch |

The five fixed guest registers are among the most used: counted over 3
billion interpreted instructions of the 8.5 CD's boot, CR is 15.8% of
register references, r3 9.2%, r4 7.7%, r24 7.5%, r1 6.2%, LR 5.5%; the
AArch64 translator's eight fixed (CR, LR, r1, r3, r4, r24, r27, r29) are
all among the top ten in the 8.5 and 9.1 boots. Instructions that need
four registers at once are why the cache keeps four slots rather than
giving one to a sixth fixed register. `struct jit` (with
`jit_state`) takes the first 64 KB of the code mapping, read-write and
not executable, so generated code reaches it RIP-relative, without a
register; the pointers it loads from there (the block table, page flags)
don't depend on the guest, so the processor loads them early. Upper halves
are kept zero (32-bit operations zero-extend), so a guest value is always
good as a 64-bit index.

### The condition register and XER

- **CR is one 32-bit value** in a host register, as in `struct ppc_cpu`.
  A compare into field f is a `cmp`, a `mov` of EQ in field f's place, and
  `cmovg` and `cmovl` from constants in memory (GT and LT in each field's
  place); SO, almost never set, is a test of XER's sign and a jump out of
  line; `and` and `or` merge the field into CR: eight instructions, as on
  AArch64 (CSEL, BFI). `Rc=1` uses `test`.
- **Branches on a CR bit** are `test cr, imm32` and a conditional jump,
  which the processor fuses into one operation; `bdnz` and the like `dec`
  and a jump. CR logical operations are `bt`/`setc` or shifts, `crset`
  and `crclr` `bts` and `btr`.
- **XER's CA in** is `bt xer, 29`, into the carry flag for `adc`; the
  subtract-from forms invert it (`cmc`) for `sbb`, since x86's borrow is
  PowerPC's carry inverted. **CA out** comes from `sbb` (all ones from the
  carry); **OV**, rare, out of line on `jo`. `addco.` takes 17
  instructions, AArch64's 16.
- **The emitter's rules:** nothing that changes flags goes between a
  flag's producer and its consumer, and there are no 8- or 16-bit
  partial register writes (MOVZX instead).

### Integer instructions

- **`rlwinm`** is RORX and an AND, or SHL, SHR, MOVZX or BZHI where the
  mask is a shift or a zero extension. **`rlwimi`** is `rorx t; xor t, d;
  and t, mask; xor d, t`; a byte or halfword into place one
  partial-register MOV, and a byte or halfword times 2, 4 or 8 (the 68k
  emulator's opcode times 8) `and`, MOVZX and LEA. It's most of what x86
  spends beyond AArch64, whose BFI is one instruction.
- **`slw`, `srw`, `sraw`** are 64-bit SHLX, SHRX and SARX of the zero- or
  sign-extended value: their 6-bit count is PowerPC's `rB & 63`, so shifts
  by 32-63 give 0, or the sign, with no test. The carry of `srawi` and
  `sraw` comes from BZHI of the bits shifted out.
- **`cntlzw`** is LZCNT (32 for zero), **`extsb`** and **`extsh`** MOVSX,
  **`mulhwu`** MULX, **`mulhw`** a 64-bit IMUL of the sign-extended
  operands; **`mullwo`**'s overflow is IMUL's OF exactly.
- **`divw` and `divwu`** never fault on the host: a divisor of 0 gives rA
  (the interpreter's choice), and `divw` by -1 is a negation, which also
  gives INT_MIN / -1 = INT_MIN. The `o` forms go to the interpreter, as on
  AArch64. 32-bit division keeps clear of 64-bit IDIV's latency on older
  processors.

### Memory

- **Addresses:** a D-form access that doesn't update rA, with a
  displacement of 0 or more, is `[r15 + rA + d]`, with no address
  computed: the guest register's upper half is zero, and an address that
  runs past 4 GB rather than wrapping lands in the view's slack (never
  accessible), and the interpreter does it. Otherwise a 32-bit LEA, which
  wraps as PowerPC does, and `[r15 + rax]`. Negative displacements take the
  LEA: `[r15 + rA + d]` would need a guard below the view.
- **Loads and stores** are MOVBE (then MOVZX or MOVSX for halfwords),
  straight from or to the guest's register; `lfd` and `stfd` a 64-bit
  MOVBE; `lfs` widens with VCVTSS2SD but at exponent 255, where infinities
  and NaNs keep their bits as on AArch64; `stfs` uses the AArch64
  translator's integer conversion. The byte-reversed forms (`lwbrx`,
  `stwbrx` and the halfwords) are translated too, so device registers
  reached by them are made in place, where AArch64 leaves them to the
  interpreter: that costs AArch64 198, 461 and 594 exits in the 8.5, 9.1
  and 9.2.2 CD boots (0.4-1.9% of their exits to the interpreter), too
  few to change. `dcbz` is two 16-byte stores of a zeroed register (no
  256-bit stores, so no dirty upper state).
- **Faults:** the site table holds each access's first byte's offset,
  prefixes included, and its side exit; the handler reads RIP from the
  signal's context, finds the site and moves RIP there. Linux sends
  SIGSEGV; macOS SIGBUS for a protected page and SIGSEGV for an unmapped
  one. x86's faults are precise (a store that runs into a protected page
  writes nothing), and host pages are 4 KB, so the view's protection is
  exactly the guest page's.
- **Checked accesses** (after a fault there, or all with
  `BLUE_JIT_FASTMEM=0`) test the page's flags and go through C's base,
  with the slow path for stores to code pages and the mmio hook's path, as
  on AArch64. A load that `MEM_FAST_R` turns away (it asks for the next
  page too, for an access that might cross) but that stays in a readable
  page loads after all, out of line, on both hosts (on AArch64 only
  without fastmem, so that its code with fastmem stays as it was); without
  that, the 9.1 and 9.2.2 CD boots without fastmem left for the
  interpreter 147 and 48 million times.

### Floating point on x86-64

As on AArch64: arithmetic is native only where the interpreter's fast path
would take it (round to nearest, no exceptions enabled, operands and
results well inside the normal range), with the inexact test an
error-free transformation by FMA3; the mode is checked once a block; the
FPSCR is lazy, its record left for a settle stub or `fp_settle()`; the
FPSCR instructions are native; everything else goes out of line to the
interpreter. Scalar doubles are VEX.128 throughout, so there's no AVX-SSE
transition cost against C's legacy SSE. `fnmadd` and `fnmsub` negate the
rounded `fmadd`/`fmsub`, as the interpreter does: VFNMADD and VFNMSUB
would give +0 where it gives -0 for an exact zero. `fctiw[z]` is VROUNDSD,
VCVTTSD2SI and a compare after converting back; `fsel` VCMPSD and
VBLENDVPD; `fcmpu` and `fcmpo` UCOMISD, whose parity flag means unordered
(out of line then). The FPSCR's mode and XX are tested in memory, not kept
in a register as w5 is on AArch64. Generated code takes MXCSR to be round
to nearest with every exception masked and no FTZ or DAZ, as the
interpreter's fast path does; the interpreter puts round to nearest back
after an operation in another mode.

The interpreter's NaNs were the host's: x86 gives 0xFFF8000000000000 for
an invalid operation where AArch64 and a 750 give 0x7FF8000000000000, and
with several NaN operands x86 takes the first operand, AArch64 signalling
NaNs first and the fused multiply-add's addend first, and PowerPC frA,
then frB, then frC. They're a 750's on every host now
([architecture.md](architecture.md#the-processor)); the JIT sends NaNs to
the interpreter, so it agrees either way. Tininess for underflow may still
differ (x86 detects it after rounding, AArch64 before), but only the
interpreter sees underflows.

### Code layout, links and invalidation

- **The mapping:** `struct jit`'s 64 KB, then 64 MB of code, read, write
  and execute (`MAP_JIT` on macOS, so a hardened build would need only the
  allow-jit entitlement; Intel Macs need it only under the hardened
  runtime). x86 keeps instruction fetch coherent with a thread's own
  stores, so there's no cache maintenance.
- **Blocks:** each starts with `cmp r14, imm32` and a `jb` to the budget
  stub, on a 16-byte boundary after its 8-byte block pointer. The blocks
  area starts at a 4 KB boundary after the stubs (see [under
  Rosetta](#under-rosetta)).
- **Hot and cold apart:** blocks' main lines are packed together, and
  their out-of-line code (side exits, slow paths, branch exits, link
  stubs, the budget and redispatch code; about 40% of the code) goes in an
  area of its own, but for the taken paths of conditional branches, which
  run as often as guest branches are taken and stay beside their block.
  The Zen 5's small instruction caches gain from that: L1
  instruction-cache misses -3%, cycles -1.3% (gcc) and -2% (clang). With the
  taken paths moved away too it changed nothing. AArch64 can't easily do
  the same: its branches to out-of-line code (TBZ ±32 KB, B.cond ±1 MB)
  would need veneers.
- **Links** are `jmp rel32`, straight to the block at the target if it
  exists when the block is translated (or is the block itself), else first
  to a stub that asks C, which then patches the displacement. Everything is
  within rel32 of everything else, so there are no range limits.
  **Invalidation** makes a block's entry a `jmp rel32` to its redispatch
  code. It's one thread, so patches need no atomicity.
- **Indirect branches** look up the block table inline, with an indirect
  `jmp` of their own at each exit, for the predictor. The first level holds
  each page's table less twice the page's address (`l1_entry()` in
  `jit_int.h`), so the lookup is `l1[pc >> 12]`, then the entry at that
  plus 2 × pc, with no masking.

### Calls into C

Arguments go in rdi, rsi, rdx, rcx, r8 and r9, which are cache slots or
scratch, so they're set up after the write-back; calls go through
`jit_state`'s pointers (`call [rip+x]`), since C may be more than 2 GB
away. Dirty registers are written back first, the caller-saved fixed ones
stored and reloaded, and every cached FPR reloaded after, as System V
keeps no XMM register across a call (AArch64 keeps d8-d15's low halves).
Generated code's calls and returns are paired (`enter` is called and
`exit` returns), so CET shadow stacks wouldn't object. Windows isn't a
target: its calling convention and exception handlers would change only
`jit_x86.c`'s calls and the fault hook.

### Under Rosetta

Rosetta 2 offers only x86-64-v2 by default; with `ROSETTA_ADVERTISE_AVX=1`
in the environment it offers all of x86-64-v3 (FMA3 results matched
AArch64's `fmadd` bit for bit over 4 million random cases, MOVBE, RORX,
LZCNT and SHLX over 2 million). Faults on a protected page come as
SIGBUS, and moving RIP in the handler works; Rosetta enforces 4 KB
protections inside its 16 KB host pages. Two costs real x86 processors
don't have:

- **Writing to code that has run costs about 13 µs a write**, as Rosetta
  translates the page again. The profiler put 30% of the 8.5 CD boot's
  samples on three stores, each the first write to a page of code: a new
  block's first instruction (53,000 blocks), a link's patch (49,000
  links) and a retired block's entry. Linking when translating, where the
  target exists (which real processors gain from too), cut the links'
  share from 14% to 5%; the rest, 29-32% of a boot, remains. `struct jit`'s
  64 KB keeps `jit_state` out of the 16 KB pages of code.
- **A hot loop that crosses a 4 KB boundary runs at 60-80% speed:** the
  synthetic benchmark's integer loop fell from 7,200 to 5,900 million
  instructions a second, and its copy loop from 10,450 to 5,400, with
  byte-identical code that had moved. Hence the blocks area's 4 KB
  alignment (one, not one per block), which keeps the benchmark's blocks
  clear of a boundary; a boot's blocks fall where they fall.

Under Rosetta on an M5 the 8.5, 9.1 and 9.2.2 CD boots take 5.47, 4.11
and 3.99 s (interpreted: 21.2, 21.0 and 17.3 s), against AArch64's 1.00,
1.38 and 1.39 natively.

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
([findings.md](findings.md), "BlockMove on the host isn't worth it").

On an AMD Ryzen Threadripper PRO 9975WX (Zen 5, Linux, gcc; medians of
5), the x86-64 translator against the interpreter:

| | 8.5 CD | 9.1 CD | 9.2.2 CD |
|---|---|---|---|
| interpreter | 14.71 s | 14.49 s | 10.22 s |
| JIT | 1.460 s | 1.543 s | 1.511 s |
| cycles per guest instruction | 1.85 | 2.01 | 2.62 |
| host instructions per guest instruction | 6.02 | 6.98 | 6.24 |

That's 10.1, 9.4 and 6.8 times the interpreter (clang's builds: 1.475,
1.587 and 1.533 s, against 16.0, 15.5 and 10.9). The synthetic benchmark,
in millions of guest instructions a second, JIT against interpreter:
dispatch 1,077 and 358, integer 8,266 and 480, copy 10,645 and 767, FP
1,631 and 206. Those were measured when the translator was complete;
with hot and cold code apart and the kernel's context switch in vectors
(below), the 9.2.2 CD's boot takes 1.21 s.

### Where the time goes

Of a boot's host time on an M5, translated code takes 62-78%; the rest is
mostly the kernel's:

- host calls (the disk and display drivers: 5-10% of a boot, 1-2.7 µs
  each) and Multiprocessing Services' calls (9.1's CD: 95 ms of 1.5 s);
- kernel calls made in place, about 17 ns each, the context switch's
  copies of 32 GPRs and 32 FPRs each way in vectors (NEON; AVX2 where the
  processor has it). Copying less isn't possible cheaply: context blocks
  are guest memory, which the guest reads and writes, so switching by
  pointer or lazily would take page protections that fault all the time;
- bringing the devices up to date, which is guest-visible (the USB
  controller writes the frame number into guest memory), so it stays at
  the instruction where it falls; a kernel call that finds devices to
  bring up to date does that itself rather than exit. Quick system calls
  could do the same, but it saved too little (9.1's host instructions
  -0.6%, its cycles within the noise) to be worth a hook in both
  translators.

In the JIT itself, code runs in the interpreter until it's hot (3-7%: 6-8
ns an instruction, against 3-4 for warm code; translating sooner costs as
much as it saves, `BLUE_JIT_HOT` from 4 to 16 changing the CD boots' cycles
by under 2%), translating takes 1-4%, and instruction-cache upkeep on
AArch64 (`ic ivau` and the barriers, 0.3-0.6%; macOS's write protection
switches, 0.5-0.9%) too little to batch. Of the instructions the
interpreter runs, 16-41% are hot code, at the device sync points (every
20,000 instructions), where the next block is longer than what's left of
the budget. Measured breakdowns, run by run, are in
[findings.md](findings.md) ("Where a boot's host time goes" and after).

### x86-64 against AArch64

Host instructions per guest instruction, counted by QEMU (Linux;
[below](#counting-host-instructions-without-the-host)): all of the
process, then translated code and Blue's own C in brackets:

| | 8.5 CD | 9.1 CD | 9.2.2 CD |
|---|---|---|---|
| x86-64 | 6.30 (4.57, 1.59) | 7.21 (4.54, 2.59) | 6.43 (4.68, 1.67) |
| AArch64 | 5.98 (4.51, 1.46) | 6.91 (4.47, 2.44) | 5.84 (4.27, 1.56) |

By kind of guest instruction in 8.5's boot, x86-64's and AArch64's: ALU
4.70 and 4.15, rotates 3.79 and 2.22, compares 8.09 and 8.78,
`bclr`/`bcctr` 11.38 and 9.92, loads 2.31 and 3.71, stores 3.51 and 4.25,
`bc` 2.18 and 1.19 (a test and a jump, which x86 fuses into one
operation). What x86 spends beyond AArch64 is mostly `rlwimi`, the inline
lookup and guest registers in memory (five fixed, against eight).

Fewer instructions aren't always faster on a real processor: r29 (the
guest register translated code loads and stores most in the 9.1 and 9.2.2
boots) as a sixth fixed register, with the budget moved to memory, cut
0.07-0.15 host instructions per guest instruction, but the boots took 2-4%
longer on the Zen 5 and the benchmark's integer and copy loops 7% and 18%
(each block's exit and the next block's entry then depend on each other
through a store and a load), so the budget stays in r14.

### The Zen 5's front end

What limits these boots on the Zen 5 is its front end. In the 9.2.2 CD's
boot (its counters, the whole process, user space): 6.5 billion cycles;
of the dispatch slots (8 a cycle), 54% got nothing from the front end and
11% waited on the back end; 6.2 of 17.8 billion operations came from the
decoders rather than the op cache, after 1.4 billion op cache misses; 149
million L1 instruction-cache misses; 43.5 million branch mispredictions,
31.8 million of them indirect (of 232 million indirect branches). Per
guest instruction, 8.5's boot is 1.85 cycles, 0.36 of them with the front
end stalled, and 9.2.2's 2.62, 0.89 stalled. The M5 runs the same boots
at 3.2 instructions a cycle, the Zen 5 at 2.4.

- **Most `bclr`s aren't returns:** 9.2.2's boot runs 193 million, with
  only 20.8 million calls. The rest is the 68k emulator dispatching
  through LR (`lhau r27,2(r24)`, `rlwimi r29,r27,3,13,28`, `mtlr r29`,
  `lhau`, `bflr cr2_lt`; that `bflr` alone runs 30.6 million times); 8.5's
  emulator dispatches through CTR instead (78.5 million `bcctr`).
- **Guest calls and returns as the host's** (a `bl` CALLing the block at
  its target, an unconditional `blr` RETurning when the guest address on
  top matched LR) gave no gain: indirect mispredictions fell by 1.5
  million, return mispredictions rose by 0.94 million (stacks dropped at
  exits), and cycles by 0.6%. The indirect predictor already had those
  returns.
- **The emulator's dispatch is spread already:** each handler's block
  follows its `b` into the emulator's dispatch code, so it ends with its
  own indirect jump (9.2.2's busiest dispatch `bflr` is reached from 665
  handlers, none with more than 8% of its runs). Spreading further would
  take blocks specialized by more history, which the block table (one
  block per guest address) doesn't have.
- **What's left is code size:** the hot main line is about 210 bytes a
  block. The SO test in every compare (`cmp dword [rbx+xer], 0` and `js`,
  13 bytes, as XER is beyond a byte displacement from rbx), rel32 jumps
  where rel8 would reach and the block entry's `cmp r14, imm32` are where
  bytes could go.

## Measuring

### Counting host instructions without the host

QEMU's user mode with a TCG plugin that counts each translated block's
executions gives the instructions an x86-64 (or AArch64) processor would
retire, exactly and the same every run, for the whole process: about 30 s
for the 8.5 CD's boot on an M5. AArch64's count under QEMU (23.9 billion
for that boot) is within 4% of the M5's own counters (24.7 billion with
the process's other threads). To divide it by guest instruction, a
measuring build of Blue writes a map (each block's host range, each guest
instruction's start, kind and word, where the main line ends, the stubs,
the text's address, the process's mappings), an interpreted run counts
guest instructions by kind, and a small program puts them together. That
needs QEMU built with `--enable-plugins` for the x86_64-linux-user and
aarch64-linux-user targets; none of it is in the tree. One thing QEMU
counts differently: each iteration of `rep movsb` is an instruction to it
and not to a processor, so glibc's large copies (`memmove`'s ERMS path)
show as 0.08-0.15 host instructions per guest instruction on x86-64,
against 0.01 for AArch64's NEON copies.

### Timing on real hardware

`tools/jit-timing.sh [runs]`, from the top of a checkout on the machine to
time (Linux or macOS, x86-64 or AArch64, with a C compiler and make),
builds Blue and writes what it finds to `jit-timing-<host>-<date>.txt`:

- the processor and the commit;
- the synthetic benchmark (`BLUE_JIT_BENCH=1`);
- each CD boot whose files it finds, in instruction time (the same guest
  work on every host): `runs` times with the JIT (3 by default) and once
  interpreted, with the JIT's statistics; and the processor's counters,
  host instructions and cycles per guest instruction: on Linux for the
  emulation thread (`BLUE_PERF=1`, where `perf_event_paranoid` allows), on
  macOS for the whole process (`/usr/bin/time -l`; under Rosetta, the ARM
  instructions it runs).

The ROM and CD images are looked for at the top of the tree (`MOS.rom`,
`85.iso`, `91.iso`, `922.iso`) or where `BLUE_ROM`, `BLUE_85_ISO`,
`BLUE_91_ISO` and `BLUE_922_ISO` say; without them only the benchmark
runs. A full run takes a few minutes, most of it the interpreted boots.
`BLUE_PERF`'s counts compare with QEMU's above: QEMU counts the whole
process, `BLUE_PERF` the emulation thread, which is nearly all of it.

The Zen 5's front-end figures above came from its raw AMD events, counted
for the whole process with `perf_event_open()` from a small wrapper (user
space only, which `perf_event_paranoid` 2 allows).

### Comparing runs

- **Hosts' clocks:** the guest's clock is the host's local time, so runs
  compared across hosts need the same time zone (`TZ`): a container in UTC
  ran the 8.5 CD otherwise than macOS in Sydney.
- **AArch64's link counts vary between identical runs** (by about 3%; the
  guest's results don't): `arch_link()` turns a B into another B without
  the barriers that would make the core fetch it anew, and the
  architecture lets a core go on executing the old one for a while (both
  are right), so the old one sometimes goes to the link stub again, which
  links it again and counts another link. With barriers after each link,
  four 9.1 CD runs made exactly 23,057 links each. x86 sees its own stores
  to code at once: its counts are the same every run.
- **The interpreter's benchmark numbers move with code placement:**
  `BLUE_JIT_BENCH=1`'s interpreted kernels ran 20-30% faster or slower
  between builds that don't change the interpreter (on the Zen 5 with
  clang, not gcc); the translated kernels don't.

## Testing

- **The fuzzer** (`tests/test_jit.c`) runs random programs on both
  engines and compares everything: integer, CR and XER, rotates, shifts by
  0-63, division by 0 and -1; loads and stores of every form, page
  crossings, a read-only page, a device, stores into code, watched pages;
  FP with special values and the FPSCR instructions; branches, `b` into
  ROM-like code, hooks; kernel calls and `sc` in place. Its programs go
  into zeroed memory, so a seed always makes the same run. CI runs 100,000
  on all four targets; `BLUE_JIT_FAR=1` and `BLUE_JIT_FASTMEM=0` test the
  long-branch and checked paths. Checks require the JIT to have made kernel
  calls, quick `sc`s and device accesses itself, without the interpreter.
- **The x86-64 encoder** is checked against LLVM's assembler: 174
  encodings in `test_jit.c` (REX, rbp and r13 as a base, rsp and r12 with
  SIB, RIP-relative operands, displacements and immediates of each size,
  two- and three-byte VEX, MOVBE, LZCNT, BMI1 and 2, FMA3), as
  `test_logic_imm()` checks `a64.h`.
- **The processor check:** `BLUE_JIT_HIDE` takes features as missing, and
  the JIT must decline with its message; `BLUE_TEST_JIT=1` makes no JIT a
  failure rather than a skip (CI).
- **The CD boots** in instruction time end exactly as on the other
  architecture and as the interpreter's (screenshots, instruction counts,
  statistics but the JIT's own line), with and without fastmem.
- **On a Mac with Apple silicon**, an x86-64 build runs under Rosetta:
  `make BUILD=build/x86 CC='cc -arch x86_64' CXX='c++ -arch x86_64' all
  build/x86/blue-tests`, then `ROSETTA_ADVERTISE_AVX=1 arch -x86_64
  build/x86/blue-tests jit` (and boots likewise). Its timings exaggerate
  translation's costs (above). A Linux x86-64 container under podman runs
  under QEMU (its CPU model "max" offers x86-64-v3), several times slower
  than Rosetta.
