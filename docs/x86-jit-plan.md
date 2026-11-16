# Plan: a JIT for x86_64 hosts

Guest PowerPC code runs in Blue's interpreter on every host, and on
AArch64 hosts in a JIT (`src/jit.c`, `src/a64.h`): about 200 million guest
instructions a second against the JIT's 2.5-5 billion. x86_64 hosts, where
Blue will be used most, only interpret. This plan adds an x86-64 backend
to the JIT, as exact as the AArch64 one (results, instruction counts, exits
and faults bit for bit the interpreter's) and as fast as x86 allows,
sharing everything that isn't about the host's instructions.

## Status

The x86-64 JIT is complete: milestone 5 (performance) done.

- **Measured**: on a real x86 processor (an AMD Ryzen Threadripper PRO
  9975WX, Zen 5, CachyOS Linux, the user's: build, tests, fuzzing,
  timings, the processor's counters), and by QEMU, which counts exactly
  the instructions an x86 processor would run (user mode, a TCG plugin;
  the same for the AArch64 build under `qemu-aarch64`, whose 8.5 count,
  23.9 billion, is within 4% of the M5's own counters, 24.7 billion with
  the process's other threads). A map from each translated block (each
  guest instruction's host code and kind) and an interpreted run's guest
  instructions by kind divide QEMU's count by guest instruction, kind,
  out-of-line code, stubs and Blue's functions (Findings: the method).
  Instruction counts steered; the real processor's times decided.
- **What changed**: CR fields from CMOVs of constants in memory, XER's SO
  tested out of line, CA from SBB, OV out of line on JO (`addco.`, 96
  million times in the 8.5 boot, 28 → 17 instructions); D-form accesses
  `[r15 + rA + d]` with no EA computed; `rlwimi` of a byte or halfword by
  a partial-register MOV or AND, MOVZX, LEA; the block table's first level
  biased per page so lookups don't mask; exits to a block that exists
  linked when translated; the context switch's register copies unrolled
  on x86 (GCC kept them loops: `nk.c`; AVX2 vectors since, where the
  processor has it). Tried and reverted: r29 in a
  host register with the budget in memory (Findings).
- **On the Zen 5** (Linux, gcc, medians of 5; the same boots end exactly
  as on AArch64, logs and screenshots, with and without fastmem):

  | | 8.5 | 9.1 | 9.2.2 |
  |---|---|---|---|
  | interpreter | 14.71 s | 14.49 s | 10.22 s |
  | JIT, milestone 4 | 1.597 s | 1.839 s | 1.682 s |
  | JIT, now | 1.460 s | 1.543 s | 1.511 s |
  | cycles per guest instruction, milestone 4 → now | 2.12 → 1.85 | 2.40 → 2.01 | 2.89 → 2.62 |
  | instructions per guest instruction, milestone 4 → now | 7.25 → 6.02 | 8.56 → 6.98 | 7.46 → 6.24 |

  The JIT is 10.1, 9.4 and 6.8 times the interpreter (clang's builds:
  1.475, 1.587 and 1.533 s, against 16.0, 15.5 and 10.9). The benchmark
  (guest instructions a second, millions): dispatch 1,077, integer 8,266,
  copy 10,645, FP 1,631, against the interpreter's 358, 480, 767 and 206.
- **Host instructions per guest instruction** (QEMU, Linux; libraries
  mostly QEMU's counting of `rep movsb`, Findings):

  | | 8.5 | 9.1 | 9.2.2 |
  |---|---|---|---|
  | x86, milestone 4 | 7.53 (translated 5.53, Blue 1.85) | 8.79 (5.30, 3.42) | 7.65 (5.56, 2.01) |
  | x86, now | 6.30 (4.57, 1.59) | 7.21 (4.54, 2.59) | 6.43 (4.68, 1.67) |
  | AArch64 | 5.98 (4.51, 1.46) | 6.91 (4.47, 2.44) | 5.84 (4.27, 1.56) |

  x86 against AArch64: 1.26, 1.27 and 1.31 before, 1.05, 1.04 and 1.10
  now (the plan's aim: 1.3). By kind, 8.5's boot (host instructions per
  guest instruction of that kind, x86 before → now, AArch64): ALU 6.93 →
  4.70 (4.15), rotates 4.53 → 3.79 (2.22), compares 10.75 → 8.09 (8.78),
  `bclr`/`bcctr` 13.07 → 11.38 (9.92), loads 2.94 → 2.31 (3.71), stores
  3.97 → 3.51 (4.25), `bc` 2.18 (1.19: test and jump, which x86 fuses
  into one operation). What's left is mostly `rlwimi` (AArch64's BFI is
  one instruction), the inline lookup and the guest registers in memory
  (x86 has five fixed, AArch64 eight).
- **Fastmem off** (a commit of its own, both translators): a load from a
  readable page below an unreadable one no longer goes to the
  interpreter. With `BLUE_JIT_FASTMEM=0` the 9.1 and 9.2.2 boots' exits
  to the interpreter fell from 146.6 and 48.2 million to 31,609 and
  24,652 on AArch64 (x86: 31,148 and 24,058), the 9.1 boot from 7.8 to
  1.6 s. AArch64 does it only without fastmem: with fastmem its code is
  byte for byte as before (the hashes over the boots and the fuzzer's
  fastmem runs).
- **AArch64's byte-reversed device accesses**: 198, 461 and 594 exits in
  the three boots (0.4-1.9% of their interpreter exits) and 186 in Unreal
  Tournament's 30 billion instructions: left as they are.
- **Under Rosetta** on the M5 (medians of 5): the 8.5, 9.1 and 9.2.2 boots
  take 5.47, 4.11 and 3.99 s (milestone 4: 5.91, 4.47 and 4.45;
  interpreted: 21.2, 21.0 and 17.3), against AArch64's 1.00, 1.38 and
  1.39 natively (interpreted: 14.1, 14.1 and 12.8). Writes to code that
  has run (a new block, a link, an invalidation) take about 30% of that
  (the profiler's samples on those stores); linking at translation time
  cut the links' share from 14% to 5%. Real processors don't pay it.

After milestone 5, the front end (the JIT speed work; Findings have the
measurements):

- **Where the Zen 5's cycles go** in the 9.2.2 CD boot: 54% of dispatch
  slots get nothing from the front end; 35% of operations come from the
  decoders rather than the op cache (1.4 billion op cache misses), with
  149 million L1 instruction-cache misses; 43.5 million branch misses, 31.8
  million of them indirect (of 232 million indirect branches). Guest
  returns are few of those: most of the boot's `bclr`s are the 68k
  emulator dispatching through LR.
- **Changed**: blocks' main lines pack together, their out-of-line code
  in an area of its own (the taken paths of conditional branches stay
  beside them): instruction-cache misses -3%, cycles -1.3% (gcc) and -2%
  (clang). On both hosts: `lwarx`/`stwcx.` natively, and `lmw`/`stmw`
  running into the next page without the interpreter. The 9.2.2 CD boot
  (medians of 11 alternating runs, wall time): gcc 1.217 → 1.210 s, clang
  1.259 → 1.233 s; front-end stalls (cycles with the op queue empty) -4%
  and -7%. It ends exactly as before (screenshot and log, with and
  without fastmem, gcc and clang); the fuzzer passes 200,000 programs with
  each compiler (and 50,000 each with `BLUE_JIT_FASTMEM=0` and
  `BLUE_JIT_FAR`).
- **Tried and dropped**: guest calls and returns as the host's, for its
  return stack (no fewer branch misses: the indirect predictor already had
  the returns). **Measured, not needed**: spreading the 68k emulator's
  dispatch, which each handler's block already has a copy of.
- **The kernel's costs** (both hosts; [plan.md](plan.md), Findings): the context
  switch's register copies in AVX2 vectors where the processor has it
  (found at run time; a third of the instructions), kernel calls made in
  place while devices are to be brought up to date (the update made in
  the call, at the same instruction), and stores to the info records made
  by the mmio hook. The 9.2.2 CD boot (medians of 11 alternating runs,
  wall time; the machine was shared): gcc 1.319 → 1.309 s, clang 1.281 →
  1.237 s; host instructions -2.9% and -2.8%. It ends exactly as before
  (screenshot and log, with and without fastmem, gcc and clang); the
  fuzzer passes 200,000 programs with each compiler and 50,000 with
  `BLUE_JIT_FASTMEM=0`.

Milestone 4 done (the rest):

- **Kernel calls and `sc`** as on AArch64: `twi 31,r31,n` made in place
  through the kernel's `kcall()`, going on through the block table;
  NanoKernel 2's quick `sc` answered in place; other system calls, host
  calls and unconditional traps leave straight for the kernel instead of
  through the interpreter.
- **Device registers in place**: GPR loads and stores that page flags turn
  away (after a fastmem fault on a device's page, or all without
  fastmem) offer the access to the mmio hook through `mmio()`, by way of
  an mmio stub that saves and reloads the fixed registers; then the
  block goes on, or leaves for the kernel or the interpreter as the hook
  says. Byte-reversed ones too, which AArch64 leaves to the interpreter
  (Findings).
- Done before: stores to code pages (`code_store()`, milestone 2), the
  hook and the match hook (milestone 1), string instructions through the
  interpreter (as on AArch64), the profiler (now with marks for the mmio
  and settle stubs). The shared runtime didn't change; `jit_int.h` gains
  x86's `mmio_call`. AArch64's generated code is byte for byte as before.
- **Exits** in the CD boots, x86 under Rosetta before and after, and
  AArch64 (interpreter, kernel, device):

  | | 8.5 | 9.1 | 9.2.2 |
  |---|---|---|---|
  | milestone 3 | 4,109,526, 0, 0 | 13,319,456, 0, 0 | 4,026,514, 0, 0 |
  | milestone 4 | 48,489, 1,131,150, 21,340 | 31,866, 1,664,358, 3,013 | 24,555, 619,476, 3,105 |
  | AArch64 | 49,299, 1,131,150, 21,340 | 33,171, 1,664,358, 3,013 | 31,645, 619,468, 3,105 |

  Milestone 3's interpreter exits were kernel calls (2.7, 8.4 and 2.6
  million), `sc` (none, 4.8 and 1.4 million), device registers (1.3
  million, 41,000 and 34,000) and host calls (about 60,000 each). What's
  left is of AArch64's kinds and about its size: loads and stores the
  hook doesn't take (44,000 in 8.5's), `dcb*` and FP-mode exits, fastmem
  faults.
- **Checks**: the quick-`sc` and kernel-call tests now also require the
  JIT to have made them itself (no interpreter exits), and a new test runs
  device registers in a loop by both engines, the JIT going to the
  interpreter at most once per access instruction (its first fastmem
  fault); all three fail with milestone 3's translator. The fuzzer passes
  400,000 programs under Rosetta (100,000 with `BLUE_JIT_FASTMEM=0`) and
  30,000 under QEMU; all of `make test` on macOS and Linux, both
  architectures. The three CD boots end exactly as on AArch64 (under
  Rosetta with `BLUE_JIT_FASTMEM=0` too; the 8.5 CD under QEMU), and so
  do (by hand, under Rosetta, from APFS clones of the user's disks) an
  installed 9.2.2's start to the Finder (6 billion instructions) and
  Unreal Tournament launched from the 8.6 disk through its intro (30
  billion: four screenshots, the output and the disk image afterwards
  all the same).
- **Speed** under Rosetta on the M5, milestone 3's build against this
  one, alternating (medians of 5): the 8.5 CD 6.0 → 6.0 s, 9.1's 4.7 →
  4.5 s, 9.2.2's 4.7 → 4.4 s; Unreal Tournament's 30 billion instructions
  36.7 → 34.9 s (interpreter exits 17.1 million → 570,000). Little: the
  kernel calls and device accesses themselves cost what they did, and
  what went is the round trip through the interpreter around each; the
  kernel's own exits (1.1 million in 8.5's boot, 8 million in Unreal
  Tournament's run) remain, as on AArch64. Under Rosetta about 30% of a
  boot goes on its handling of Blue's writes to code that has run
  (Findings).

Milestone 3 done (floating point):

- **The interpreter's NaNs** first, in a commit of their own: NaN results
  are a 750's on every host (the first NaN of frA, frB and frC, quieted,
  else 0x7FF8000000000000; never negated by `fnmadd`/`fnmsub`; cut to
  single precision by single-precision operations and `frsp`), and
  ∞ × 0 + a quiet NaN is VXIMZ everywhere. The new tests fail with the
  old code on both hosts (different cases on each) and pass on macOS and
  Linux, AArch64 and x86_64. AArch64's boots (output and screenshots),
  its fuzzing (100,000 programs: the JIT sends NaNs to the interpreter,
  so both sides changed alike) and its generated code (the hashes) are as
  before.
- **FP natively**, the AArch64 design: FPRs cached in xmm6-xmm15;
  A-form arithmetic with FMA3 where the interpreter's fast path would
  take it, inexactness by error-free transformations, XX and FX at once,
  FR, FI and FPRF left pending for the settle stub (a routine for each
  operation, `fp_settle()` for the rest); `fnmadd`/`fnmsub` negate the
  rounded fma, as the interpreter does; `fsel` (VCMPSD and VBLENDVPD),
  the moves, `fcmpu`/`fcmpo`, `frsp`, `fctiw[z]`, the FPSCR instructions;
  `lfs`/`lfd`/`stfs`/`stfd` in every form; `lfd` of two words just stored
  with `stw` put together from the registers. Anything else goes out of
  line to the interpreter.
- **Checks**: the fuzzer passes 400,000 programs under Rosetta (100,000
  with `BLUE_JIT_FASTMEM=0`) and 30,000 under QEMU; all of `make test` on
  macOS and Linux, both architectures. The three CD boots end exactly as
  on AArch64 under Rosetta (with `BLUE_JIT_FASTMEM=0` too), and the 8.5
  CD under QEMU. AArch64's generated code is byte for byte as before.
- **Speed** under Rosetta on the M5, milestone 2's build against this
  one, alternating (medians of 5): the 8.5 CD 5.8 → 5.9 s, 9.1's 4.6 →
  4.25 s, 9.2.2's 4.5 → 4.5 s. The boots change little: FP is a small part
  of them, and they still leave the JIT for the interpreter 4 to 13
  million times (kernel calls and device registers, which AArch64 makes in
  place: milestone 4). The benchmark's FP loop: 95 → 895 million
  instructions a second (AArch64's JIT natively: 1,680; the code follows
  AArch64's instruction for instruction, so the gap is most likely
  Rosetta's cost for scalar AVX, while x86's integer loop runs as fast as
  AArch64's); the dispatch, integer and copy loops are as before (830,
  7,000-8,000 and 10,500), once the blocks area starts at a 4 KB boundary
  (Findings).

Milestone 2 done (memory):

- **Loads and stores** of bytes, halfwords and words, D-form and indexed,
  with update, the byte-reversed ones (`lwbrx`, `lhbrx`, `stwbrx`,
  `sthbrx`, which AArch64 leaves to the interpreter), `lmw`/`stmw` and
  `dcbz`: fastmem through the view with MOVBE, the faulting access's side
  exit found by the shared fault handler (SIGSEGV on Linux, SIGBUS on
  macOS, RIP moved to the stub), the instruction checked from then on;
  without fastmem, or once checked, page flags first. Stores to pages
  with translated code take the slow path AArch64's do (a plain store
  unless the word is translated code, else `code_store()`): planned for
  milestone 4, but the 68k emulator stores next to its code all the time.
  Device registers still leave for the interpreter (milestone 4), and
  string instructions call it, as on AArch64.
- **Checks**: the fuzzer (now requiring fastmem faults on x86-64 too)
  passes 100,000 programs under Rosetta and 30,000 more under QEMU; all of
  `make test` on both; the three CD boots end exactly as on AArch64,
  under Rosetta, with `BLUE_JIT_FASTMEM=0` too, and the 8.5 CD under
  QEMU (1,273 fastmem faults, fewer than AArch64's 1,893: 4 KB host
  pages). AArch64's generated code is still byte for byte what it was
  before the split (the hashes over the fuzzer and the boots).
- **Speed** under Rosetta on the M5, before and after: the 8.5 CD 13.4 →
  6.3 s, 9.1's 12.2 → 4.7 s, 9.2.2's 11.2 → 4.6 s; the benchmark's
  dispatch loop 382 → 830 million instructions a second, the copy loop
  566 → 10,450 million (AArch64's JIT natively: 1,180 and 10,600).

Milestone 1 done (the split, then the integer core):

- **The split**: `jit.c` holds the runtime, `jit_a64.c` the AArch64
  translator, `jit_int.h` what they share. AArch64's generated code is
  byte for byte the same: a hash of every block over the fuzzer's programs
  (with `BLUE_JIT_FAR` and `BLUE_JIT_FASTMEM=0` too) and over the 8.5,
  9.1 and 9.2.2 CD boots came out equal, and so did timings. The fuzzer's
  programs went into guest memory with stack garbage after them, which
  blocks translated past the final `sc` read, so a seed didn't always make
  the same run; they're zeros now.
- **`x86.h`**: checked in `test_jit.c` against LLVM's assembler, 174
  encodings (two of them another form as good, noted).
- **`jit_x86.c`**: everything in milestone 1 below. The fuzzer passes
  100,000 programs under Rosetta and 3,000 under QEMU (Linux), and all of
  `make test` passes on both. The 8.5, 9.1 and 9.2.2 CD boots in
  instruction time end exactly as on AArch64 (output and screenshots).
  Under Rosetta on the M5: 13.2, 14.0 and 12.9 s (interpreted: 24 s for
  the 8.5 CD); the synthetic benchmark, guest instructions a second, JIT
  against interpreter: integer 7,200 million against 281 (AArch64's JIT on
  the M5: 6,250), the dispatch loop 382 against 231, the copy loop 566
  against 374, FP 63 against 98 (memory and FP still call the
  interpreter).
- **Elsewhere**: `jit_unavailable()` gives the reason there's no JIT, and
  Blue says it; CI fuzzes and benchmarks the JIT on all four targets
  (`BLUE_TEST_JIT=1`); `tools/jit-timing.sh` (below) times a machine.

Checked on the development machine before starting (Apple M5, macOS 27.2, with Rosetta 2
and podman):

- **Rosetta** advertises only x86-64-v2 by default (SSE4.2, POPCNT; no
  AVX, FMA, BMI, MOVBE or LZCNT). With `ROSETTA_ADVERTISE_AVX=1` in the
  environment it advertises all of x86-64-v3 (AVX, AVX2, FMA, F16C, BMI1,
  BMI2, LZCNT, MOVBE; XCR0 = 7). Under it, FMA3 results matched AArch64's
  `fmadd` bit for bit over 4 million random cases, and MOVBE, RORX, LZCNT
  and SHLX were right over 2 million. Faults on a protected page come as
  SIGBUS, and a handler that moves RIP to a stub works (about 15 µs a
  fault). Rosetta enforces 4 KB protections inside its 16 KB host pages.
  RWX memory works with or without `MAP_JIT`, and there's no write
  protection to switch (`pthread_jit_write_protect_supported_np()` is 0).
  Changing code that has already run costs about 14 µs a change, because
  Rosetta translates the page again. So no test-only override is needed:
  tests under Rosetta set the variable.
- **podman**: the machine (libkrun, Rosetta off) runs amd64 containers
  under QEMU 10.2.2 user mode on 4 KB pages. Its CPU model ("max",
  AuthenticAMD) advertises x86-64-v3 (AVX-512 state off). A memfd mapped
  twice, SIGSEGV with RIP moved to a stub (1.9 µs a fault) and code
  patching (3.5 µs) all work. FMA3 matched AArch64 bit for bit.
  Installing gcc in an amd64 Ubuntu 24.04 container and running these
  checks took 33 s.
- **Baselines** (AArch64, this worktree): the 8.5 CD to 4 billion
  instructions takes 1.36 s of wall time (1.00 s user) and 24.8 billion
  host instructions (6.2 per guest instruction, everything included). It
  translates 53,714 blocks (21 MB of code) and makes 51,411 links,
  24,287 invalidations and 1,897 fastmem faults. The fuzzer takes 1.0 s
  for its default 3,000 programs (4.8 s for 20,000). The supervisor's
  x86_64 build (interpreter) under Rosetta takes 22.2 s for the same boot,
  with the same screenshot.
- **Which guest registers are used** (each reference by an executed
  instruction counted once, interpreted, 3 billion instructions):
  - 8.5 CD: CR 15.8%, r3 9.2, r4 7.7, r24 7.5, r1 6.2, r5 5.8, LR 5.5,
    r0 5.3, r27 4.5, r29 3.9, CTR 3.0.
  - 9.1 CD: CR 14.3%, r4 7.2, r29 7.1, LR 6.6, r3 6.5, r27 6.3, r0 5.7,
    r1 5.3, r24 4.7.

  The AArch64 JIT's eight fixed registers (CR, LR, r3, r4, r1, r24, r27,
  r29) are among the top ten of both lists (with r0, and r5 on 8.5).
- **The interpreter's NaNs depend on the host** (a finding; see Floating
  point). x86 gives 0xFFF8000000000000 for an invalid operation (inf -
  inf, 0 × inf, 0/0) where AArch64 and a 750 give 0x7FF8000000000000.
  When several operands are NaNs, x86 takes the first operand, AArch64
  takes signalling NaNs first and fma's addend first, and PowerPC takes
  frA, then frB, then frC.

## Decisions

- **Structure: a second translator beside the AArch64 one, sharing one
  runtime.** `jit.c` becomes the architecture-neutral runtime: the block
  table, pages, invalidation, link bookkeeping, fault sites and the
  handler, the fastmem view, the C helpers generated code calls, the run
  loop, statistics and the profiler. The AArch64 translator moves as it is
  into `jit_a64.c`, and the new one goes in `jit_x86.c` with its encoder
  `x86.h`. Splitting `jit.c` into a front end and backends was rejected,
  and so was copying it (see Structure).
- **No change for AArch64.** The split comes first, as a change of its
  own. Its check is that the generated code is byte for byte the same,
  shown by hashing every block over the fuzzer's programs and the CD
  boots (instruction-time runs are deterministic). Fuzz and boot timings
  must also be the same.
- **Baseline x86-64-v3, checked at run time** with CPUID and XGETBV (the
  OS must save YMM state). Processors without it keep interpreting, with
  one line saying what's missing. Under Rosetta the line also says that
  `ROSETTA_ADVERTISE_AVX=1` enables it. FMA3 does the fused multiply-adds
  and the error-free transformations, and the encoder uses VEX three-operand
  forms throughout. It also uses MOVBE for byte-swapped accesses, BMI2's
  SHLX, SHRX, SARX, RORX, BZHI and MULX, and LZCNT. There are no fallback
  paths inside generated code. The C code stays built for base x86-64 (no
  `-march`), so one binary runs everywhere.
- **Exactness by the same rules as AArch64.** The interpreter on the same
  host is the reference. Blocks run only when the budget covers them. Every
  instruction makes its checks before it changes anything. Side exits write
  back what's dirty at their point. Anything unusual goes to the
  interpreter for that instruction. The fuzzer (`tests/test_jit.c`, which
  needs no change) is the judge on both architectures.
- **Registers:** three for the run's own state (the processor, the budget,
  the fastmem view), five fixed guest registers (CR, r3, r4, LR and r24 to
  start with), four LRU cache slots and three scratch. On AArch64 it's
  seven, eight, five and about ten. `struct jit` (with `jit_state`) lives at
  the start of the code mapping, so generated code reaches it RIP-relative,
  without a register.
- **CR stays one 32-bit value in a host register**, as in `struct
  ppc_cpu`. Compares make the field with CMOV. Branches use TEST with a
  32-bit immediate and a conditional jump, which the processor fuses into
  one operation. Fusing a compare with the branch after it, and a lazy
  CR0, wait for measurements (milestone 5).
- **Fastmem as on AArch64**: the same second view, the same site table
  keyed by host code offset (bytes here), SIGSEGV on Linux and
  SIGBUS/SIGSEGV on macOS. Each guest access is one host instruction whose
  address is in the table, so nothing needs decoding.
- **Code buffer**: RWX, with `MAP_JIT` on macOS so that a hardened build
  would need only the allow-jit entitlement. x86 needs no cache
  maintenance. Links and invalidations patch `jmp rel32`.
- **Testing**: Rosetta for macOS x86_64 (correctness, rough timing),
  podman (QEMU) for Linux x86_64, and CI's x86_64 runners, where the JIT
  fuzz job is added from milestone 1 on. Real speed needs real x86
  hardware. Until then, a synthetic benchmark on CI's x86 runners gives
  real numbers, and counts of host instructions per guest instruction
  stand in for timings.

## What the AArch64 JIT is made of

| Part of today's `jit.c` | Lines | Where it goes |
|---|---|---|
| Blocks, pages, the two-level table, flush, hot counts (`find()`), `rom_like()` | 250 | shared |
| Invalidation: `code_changed()`, `retire()`, `drop_block()`, word bitmaps | 150 | shared (the entry patch is the translator's) |
| C helpers for generated code: `step`, `settle`/`fp_settle`, `kcall`, `code_store`, `mmio` | 110 | shared |
| Fastmem: view, protections, fault handler, sites, `slow_down()` | 200 | shared (the context's pc is the translator's) |
| `jit_create`, `jit_free`, `jit_run` (links, exits), statistics | 220 | shared (code allocation and link patching are the translator's) |
| Profiler | 170 | shared (context pc, stub names) |
| Emission helpers, register cache, side exits, fixups, stored words | 600 | AArch64 translator |
| Loads and stores, slow stores, the mmio path | 400 | AArch64 translator |
| Floating point, the settle stub | 900 | AArch64 translator |
| Integer, branches, kernel calls, `translate_upto()`, stubs | 1,150 | AArch64 translator |

About 1,100 lines are architecture-neutral and are shared.

## Structure

Three ways were weighed:

1. **A front end and backends**: one translator over an abstract emitter
   or an IR. AArch64's instruction functions are written to that
   architecture's strengths: bitfield moves for `rlwinm`/`rlwimi`, CSEL,
   TBZ/CBZ, three-operand forms, w5 holding the FPSCR. Putting them behind
   an interface means rewriting all of them, which is churn and a risk to
   AArch64's code, against the constraint. The level at which one
   interface serves both hosts well is about "translate this PowerPC
   instruction", and that is a translator per host. An IR would mean a new
   JIT for both.
2. **A copy of `jit.c` adapted to x86**: quickest to start, but it would
   leave two copies of the subtle runtime (invalidation, fastmem, faults,
   links, hooks), and every fix would have to be made twice. Rejected.
3. **A translator per host and one shared runtime** (chosen). The runtime
   moves out of `jit.c` unchanged, and the AArch64 translator keeps its
   code generation untouched.

### Files

- `src/jit.h`: the public interface, unchanged.
- `src/jit_int.h` (new): what the runtime and a translator share. It has
  `struct jit_state` (read and written by generated code, layout as now),
  `jit_block`, `jit_page`, `jit_site` and `struct jit` with a per-host
  `struct jit_arch` inside it for the translator's own state (stub
  addresses, CPU features). It also has the `EXIT_*` kinds,
  `FP_PENDING()` and the prototypes of both sides.
- `src/jit.c`: the shared runtime, plus the interpreter-only stubs for
  hosts with no translator.
- `src/jit_a64.c`: the AArch64 translator (today's `jit.c` minus the
  runtime), compiled only `#if defined(__aarch64__)`.
- `src/jit_x86.c`, `src/x86.h` (new): the x86-64 translator and encoder,
  compiled only `#if defined(__x86_64__)`. Each translator file declares
  something in its `#else` branch, so `-Wpedantic` doesn't see an empty
  translation unit.
- `Makefile`: the two new sources in `LIB_SRC`.

### The interface

The runtime offers translators:

- `jit_get_page()`, `jit_rom_like()` (the rule for following `b` into
  the ROM), `jit_flush()`;
- `jit_add_block()`: registers a translated block in its page, the
  table and the word bitmap, as the end of `translate_upto()` does now;
- `jit_add_site()`: a fault site;
- the C helpers, through `jit_state` as now.

A translator offers the runtime:

```c
bool arch_init(struct jit *j);          /* CPU check, stubs; false: interpret (having said why) */
struct jit *arch_alloc(void);           /* struct jit and the code buffer (x86: one mapping) */
struct jit_block *arch_translate(struct jit *j, uint32_t pc);
void arch_link(struct jit *j, void *site, const void *target);    /* an exit to a known pc, now direct */
void arch_retire(struct jit *j, struct jit_block *b);              /* entry → redispatch */
void arch_code_write(bool begin);       /* macOS arm64's MAP_JIT switch; nothing elsewhere */
void arch_sync(struct jit *j);          /* before generated code runs again (arm64: icache barriers) */
uintptr_t arch_context_pc(void *uc);    /* fault handler and profiler */
void arch_set_context_pc(void *uc, const void *pc);
```

Code addresses in `struct jit` and the site table become byte addresses
and byte offsets. The AArch64 translator converts at its edges: a few
lines, and the instructions it emits are unchanged.

### Keeping AArch64 as it is

For the split, a temporary hash (FNV over each block's code, printed with
the JIT's statistics, not committed) must come out equal before and after.
It's checked over the fuzzer's default programs (fixed seeds), with
`BLUE_JIT_FAR` and `BLUE_JIT_FASTMEM=0`, and over the 8.5, 9.1 and 9.2.2
CD boots in instruction time. Generated code holds no host addresses, so
the hashes don't depend on where things are mapped. Timings, medians of
alternating runs before and after: the 8.5 CD to 4 billion, the 9.1 and
9.2.2 CDs, and `BLUE_JIT_PROGRAMS=20000`. After the split,
`jit_a64.c` changes only if a later milestone finds a shared bug.

## x86-64 code generation

### Host registers (System V)

| Host | Role |
|---|---|
| rbx | `struct ppc_cpu *` (callee-saved) |
| r14 | instructions left in the budget (callee-saved) |
| r15 | fastmem view base, guest address 0 (callee-saved) |
| rbp, r12, r13 | fixed guest registers CR, r24, r3 (callee-saved) |
| r10, r11 | fixed guest registers r4, LR (written back around calls into C) |
| rsi, rdi, r8, r9 | register cache: four LRU slots, as on AArch64 |
| rax, rcx, rdx | scratch (and division's, MULX's implicit operands) |
| rsp | stack, 16-byte aligned at every call |
| xmm6-xmm15 | FPR cache: ten slots |
| xmm0-xmm5 | FP scratch |

Notes on the table:

- **`jit_state` needs no register.** `struct jit` takes the first 64 KB of
  the code mapping, read-write and not executable, so generated code
  reaches it RIP-relative. The pointers it loads from there (the block
  table, page flags, C's base for checked accesses) don't depend on the
  guest, so out-of-order execution runs those loads early. The 64 KB also
  keeps `jit_state` out of the 16 KB host pages where Rosetta would
  retranslate code after each write.
- **The fixed guest registers** are the five most used in the counts
  above. The cache catches reuse within a block. Milestone 5 tried r29
  as a sixth (the guest register translated code loads and stores most in
  the 9.1 and 9.2.2 boots, 0.12 and 0.15 times per guest instruction),
  with the budget moved to memory for r14: fewer instructions, but slower
  on a real processor (Findings), so it was reverted. r29 in a cache
  slot's register instead leaves three slots, too few for instructions
  that need four registers at once.
- **Upper halves are kept zero.** 32-bit operations zero-extend on x86, so
  a guest value is always usable as a 64-bit index.

### Condition register and XER

- **Compare into field f**: `cmp`, `mov` of EQ in field f's place, then
  `cmovg` and `cmovl` from constants in memory (GT and LT in each field's
  place, by the stubs); SO, almost never set, by a test of XER's sign and
  a jump to out-of-line code (`ools()`); `and` and `or` merge it into CR.
  Eight instructions, as on AArch64 (CSEL, BFI). `Rc=1` uses `test`.
- **Branch on a CR bit**: `test cr, imm32` and a conditional jump, fused
  into one operation. `bdnz` and the like use `dec` with a conditional
  jump.
- **CR logical operations**: `bt`/`setc` or shifts; `crset` and `crclr`
  are `bts` and `btr`.
- **XER[CA] in**: `bt xer,29` moves it into the carry flag for `adc`. The
  subtract-from forms invert it (`cmc`) for `sbb`, since x86's borrow is
  PowerPC's carry inverted.
- **XER[CA] and [OV] out**: CA by `sbb` (all ones from the carry), `and`
  and `or` into XER; OV, rare, out of line on `jo` (with SO). `addco.`
  takes 17 instructions, AArch64's 16.
- **Rules for the emitter**: nothing that changes flags goes between a
  flag's producer and its consumer (debug builds track live flags and
  assert), and there are no 8- or 16-bit partial register writes (MOVZX
  instead).

### Integer instructions

- **`rlwinm`**: RORX and an AND. Masks that amount to a shift or a zero
  extension become SHL, SHR, MOVZX or BZHI. **`rlwimi`**: `rorx t; xor t,
  d; and t, mask; xor d, t`; a byte or halfword into place is one
  partial-register MOV, and a byte or halfword times 2, 4 or 8 (the 68k
  emulator's opcode times 8) `and`, MOVZX and LEA.
- **`slw`, `srw`, `sraw`**: 64-bit SHLX, SHRX and SARX of the
  zero- or sign-extended value. Their 6-bit count is PowerPC's `rB & 63`,
  so shifts by 32-63 give 0, or the sign, in the low half with no test.
  The carry of `srawi` and `sraw` comes from BZHI of the bits shifted out.
- **`cntlzw`**: LZCNT, which gives 32 for zero. **`extsb`, `extsh`**:
  MOVSX. **`mulhwu`**: MULX. **`mulhw`**: a 64-bit IMUL of the
  sign-extended operands. **`mullwo`**: IMUL's OF is PowerPC's overflow
  exactly.
- **`divw`, `divwu`**: never a host divide fault. A divisor of 0 gives rA
  (the interpreter's choice). `divw` by -1 is a negation, which also gives
  INT_MIN / -1 = INT_MIN = rA. The `o` forms go to the interpreter, as on
  AArch64.
- **`mftb`**: from the entry time base and the budget, as on AArch64.
- **Branches, the inline table lookup** for `bclr`/`bcctr` (its own
  indirect `jmp [rdx + rax*2]` at each exit, for the predictor), **links,
  side exits, branch exits** (the taken path out of line, the block going
  on along the not-taken one), the **hook**'s table entry and the **match
  hook**'s guard: all as on AArch64. There are no range limits:
  everything is within rel32 of everything else, so the `too_far` retries
  go away.

### Memory

- **EA**: a D-form access through the view that doesn't update rA, with
  d 0 or more, is `[r15 + rA + d]`, with no EA computed: the guest
  register's upper half is zero, and an address that runs past 4 GB
  instead of wrapping lands in the view's slack (MEM_SLACK, never
  accessible), and the interpreter does it. Otherwise a 32-bit LEA (`lea
  eax, [rA+d]` or `[rA+rB]`), which wraps and zero-extends as PowerPC
  wraps, and `[r15 + rax]`.
- **Loads**: MOVBE, then MOVZX or MOVSX for halfwords; MOVZX for bytes.
  `lfd` is a 64-bit MOVBE and VMOVQ. `lfs` widens with VCVTSS2SD, except at
  exponent 255, where infinities and NaNs keep their bits as on AArch64.
- **Stores**: MOVBE straight from the source register, with no temporary.
  `stfs` uses the AArch64 JIT's integer conversion (bit for bit
  `ppc_fp_double_to_single()`), and `stfd` a 64-bit MOVBE.
- **Update forms, `lmw`/`stmw`** (with the page check), and **`dcbz`**
  (two 16-byte stores of a zeroed XMM register; no 256-bit stores, so no
  dirty upper state) are as on AArch64. So are `stw, stw, lfd` built from
  the stored registers: x86 can't forward two stores into one load either.
- **Checked accesses**: after a fault at that instruction, or always with
  `BLUE_JIT_FASTMEM=0`. They test the page-flag byte (`MEM_FAST_R`,
  `MEM_FAST_W`, `MEM_CODE`) and go through C's base, with slow stores to
  code pages and the mmio hook's path as on AArch64. A load that
  `MEM_FAST_R` turns away (it asks for the next page too) and that stays
  in a readable page loads after all, out of line (`slow_loads()`).
- **Faults**: the site table holds the byte offset of each access's first
  byte, prefixes included, and its stub. The handler reads RIP
  (`gregs[REG_RIP]` on Linux, `uc_mcontext->__ss.__rip` on macOS), finds
  the site and moves RIP to the stub. Linux sends SIGSEGV; macOS sends
  SIGBUS for a protected page and SIGSEGV for an unmapped one. x86 faults
  are precise: a store that straddles into a protected page writes
  nothing (better than AArch64's architecture promises). Host pages are
  4 KB, so the view's protection is exactly the guest page's.
- **Negative displacements** still take the LEA: `[r15 + rA + d]` would
  need a guard below the view (a change in `mem.c`).

### Code buffer, links, invalidation

- **The mapping**: 64 MB RWX, as on AArch64 Linux, after `struct jit`'s
  64 KB. macOS x86_64 adds `MAP_JIT`. Intel Macs need it only under the
  hardened runtime (Blue's builds aren't hardened), and then the
  allow-jit entitlement is all that's missing. Intel has no per-thread
  write protection to switch.
- **No cache maintenance**: x86 keeps instruction fetch coherent with the
  same thread's stores, so `arch_sync()` does nothing.
- **Block entry**: `cmp r14, imm32` (7 bytes) and `jb` to the budget stub.
  The 8-byte block pointer stays in front of the code, aligned so the
  code starts on 16 bytes (milestone 5 measures whether that helps).
- **Links**: `jmp rel32`, straight to the block at the target if it
  exists when the block is translated (or is the block itself), else first
  aimed at a stub that asks C, which then patches the displacement.
  **Invalidation**: the entry's `cmp` becomes a `jmp rel32` to the block's
  redispatch code. It's one thread, so the patches need no atomicity.
- **Indirect branches** look up the block table inline: `l1[pc >> 12]`,
  then the entry at that plus 2 × pc, as x86-64's first level holds each
  page's table less twice the page's address (`l1_entry()` in
  `jit_int.h`; AArch64's table is as it was), so no masking.

### Calls into C

- **Arguments**: in rdi, rsi, rdx, rcx, r8 and r9. Those registers are
  cache slots or scratch, so the arguments are set up after the
  write-back.
- **Through `jit_state`'s pointers** (`call [rip+x]`), since C may be
  more than 2 GB away.
- **Saved and reloaded**: dirty registers are written back first.
  Caller-saved fixed registers are stored and reloaded. Every cached FPR
  is reloaded afterwards, because System V keeps no XMM register across a
  call (AArch64 keeps d8-d15's low halves). The settle stub saves the FPR
  cache around its call into `fp_settle()`.
- **Calls and returns stay paired**: `enter` is called and `exit`
  returns, so CET shadow stacks wouldn't object. Windows isn't a target.
  Its ABI (rcx/rdx/r8/r9, shadow space, xmm6-15 callee-saved, vectored
  exception handlers) would change only `jit_x86.c`'s calls and the fault
  hook.

### Floating point

- **Same design as AArch64.** Arithmetic is native only where the
  interpreter's fast path would take it: round to nearest, no exceptions
  enabled, operands and results tame. The inexact test is an error-free
  transformation with FMA3 (`fma(-r, b, a)`, ErrFma, TwoSum). Each block
  checks the mode once. The FPSCR is lazy: XX and FX at once, while FR, FI
  and FPRF wait as a record in `jit_state` for the settle stub or
  `fp_settle()`. The FPSCR instructions are native, compares go out of
  line when unordered, and everything else goes to the interpreter out of
  line.
- **Encoding**: VEX (AVX) scalar doubles throughout, with three-operand
  forms. VEX.128 only, so there's no AVX-SSE transition cost against C's
  legacy SSE.
- **`fnmadd`/`fnmsub`**: `fmadd`/`fmsub`, then a negation, as the
  interpreter writes it. VFNMADD and VFNMSUB would give +0 where it gives
  -0: -(a·c) - b against -(a·c + b) for an exact zero.
- **`fctiw[z]`**: VROUNDSD (nearest-even or toward zero), VCVTTSD2SI and
  a compare after converting back, like AArch64's FRINTN/FCVTZS/SCVTF.
  Out of range or NaN goes out of line.
- **`fsel`**: VCMPSD (0 ≤ a, false for a NaN) and VBLENDVPD.
- **`fcmpu`/`fcmpo`**: UCOMISD, whose parity flag means unordered (out of
  line then, for VXSNAN and VXVC).
- **The FPSCR's mode and XX** are tested in memory (`test byte
  [rbx+fpscr+k]`), not kept in a register as w5 is on AArch64.
- **MXCSR**: generated code assumes round to nearest, all exceptions
  masked, no FTZ or DAZ, as the interpreter's fast path already does; the
  interpreter puts round to nearest back after each operation in another
  mode (`nearest_rounding()`), which the fuzzer's programs exercise.
- **NaNs, infinities, denormals, directed rounding** never come out of
  the native paths; they go to the interpreter, as on AArch64. So the JIT
  equals the interpreter on any host, whatever the interpreter does.
- **The interpreter's NaNs** were the host's; since milestone 3 they're
  a 750's on every host (Status). Tininess for UX may still differ: x86
  detects it after rounding, AArch64 before (PowerPC's rule to be
  checked); only the interpreter sees it, as underflows never take the
  native paths.

### Kernel calls, hooks, device registers, time base, profiler

Ported as they are on AArch64:

- `twi 31,r31,n` in place through `kcall`, then on through the block table
  but not as a branch;
- the quick `sc` answered in place below `sc_until`;
- device registers in place through the mmio stub, with all registers
  written back and reloaded;
- stores to code pages through `code_store()`;
- the match hook's guard at its block's start;
- `mftb` natively;
- the profiler's samples, with RIP from the context.

String instructions go to the interpreter, as on AArch64 (`lswx`/`stswx`
are 99% of them, from `_BlockMove`). Doing them natively is a milestone 5
candidate if profiles ask for it.

## Testing and measuring

### macOS x86_64 under Rosetta

- **Build**: `make BUILD=build/x86 CC='cc -arch x86_64' CXX='c++ -arch
  x86_64' all build/x86/blue-tests` (inside `build/`, so it's ignored;
  native objects stay in `build/`).
- **Run**: `ROSETTA_ADVERTISE_AVX=1 arch -x86_64 build/x86/blue-tests
  jit`, and the boots likewise.
- **Uses**: correctness (fuzzer, `make test`, boots in instruction time)
  and rough timing. Rosetta translates generated code too, and charges
  about 14 µs for each code patch (about 75,000 links and invalidations a
  boot), so its timings exaggerate those costs.

### Linux x86_64 under podman (QEMU)

- **Image**: amd64 Ubuntu 24.04 with gcc and make (the CI runners' system).
- **Run**: `podman run --rm --platform linux/amd64 --memory 2g`. Sources
  are mounted read-only, objects go to the scratch directory, and the ROM
  and CDs are mounted read-only.
- **Uses**: the fuzzer (several times slower than Rosetta), `make test`
  and boots with patience. No container is left running.
- Turning on the podman machine's Rosetta would be faster, but it restarts
  the machine other agents use, so this plan doesn't.

### CI

- **The JIT fuzz job runs on all four targets**, x86_64 too, from
  milestone 1 on.
- **`BLUE_TEST_JIT=1`** (new, like `BLUE_TEST_GPU`) makes "no JIT" a
  failure rather than a skip.
- **The Toolchain step prints the CPU's features**, so a runner without
  x86-64-v3 shows why.
- **The x86 runners are real hardware**: Intel for `macos-26-intel`,
  Azure's AMD or Intel for Ubuntu. Their "JIT speed" step (the synthetic
  benchmark) runs on every push to main and every pull request; this
  checkout has no remote, so its numbers come from the user's push.

### Speed without x86 hardware

- **A synthetic benchmark** (`BLUE_JIT_BENCH=1 build/blue-tests jit`,
  not part of `make test`) runs a few PowerPC kernels for a fixed number of
  instructions, interpreted and translated, and prints guest instructions
  a second:
  - a 68k-emulator-like dispatch loop (`lhzu` from r24, `rlwinm`, `lwzx`,
    `mtctr`, `bctr` to handlers);
  - integer compare-and-branch;
  - a copy loop;
  - an FP loop (`fmadd`, `lfd`/`stfd`, MathLib's `mffs`/`mtfsf`).

  CI runs it on all four targets (non-gating). The x86 runners give real
  numbers for the JIT against the interpreter, to set beside AArch64's
  ratio and boot speed.
- **Host instructions per guest instruction**: counted by QEMU (a TCG
  plugin) for both builds over the same instruction-time boots, and
  divided by guest instruction kind with a map the translators write in a
  measuring build (Findings: counting host instructions without the
  host). That replaced the counters in generated code planned here
  (`BLUE_JIT_COUNT=1`): exact for every instruction, C's included, and
  nothing in the tree.
- **Rosetta timings** of the CD boots per milestone, against the
  interpreter's 22.2 s.
- **Real hardware**: if the user has an x86 machine (an Intel Mac or a
  Linux PC), `BLUE_PERF=1` on Linux gives the real instructions and
  cycles per guest instruction.

## Findings

- **AArch64's link counts vary between identical runs** (by about 3% of
  links; the guest's results don't): `arch_link()` turns a B into another
  B without the barriers that would make the core fetch it anew (the
  architecture allows a core to go on executing the old one for a while,
  and both are right), so the old one sometimes goes to the link stub
  again, which links it again and counts another link. With the barriers
  after each link, four 9.1 CD runs made exactly 23,057 links each (without
  them 23,901 to 23,930). x86 sees its own stores to code at once: its
  counts are the same every run.
- **Comparing runs across hosts** needs the same time zone: the guest's
  clock is the host's local time, so a container in UTC ran the 8.5 CD
  differently from macOS in Sydney until `TZ` matched.
- **Under Rosetta, a hot loop that crosses a 4 KB boundary runs at 60-80%
  speed** (the x86 page size it emulates): with milestone 3's larger stubs
  the benchmark's blocks moved, and its integer loop fell from 7,200 to
  5,900 million instructions a second, the copy loop from 10,450 to
  5,400, with byte-identical code. Padded to cross the boundary or not,
  the same blocks gave 6,100 against 7,500 and 6,300 against 10,550.
  The blocks area now starts at a 4 KB boundary after the stubs (one
  alignment, not one per block), which keeps the benchmark's blocks clear
  of a boundary. Real x86 processors don't care; a boot's blocks cross
  boundaries as they fall, as they always have.
- **Under Rosetta, writing to code that has run costs about 13 µs a
  write**, as the plan's first checks found (14 µs): the profiler
  (`BLUE_JIT_PROFILE=1`) on the 8.5 CD boot put 30% of its samples on
  three stores, each the first write to a page of code: a new block's
  first instruction (2,740 samples, 14%; 53,000 blocks, 13 µs each), a
  link's patch (2,642; 49,000 links) and a retired block's entry (375).
  x86 processors make no such charge. Milestone 5 made only the change
  that real processors gain from too: blocks are linked when translated
  where the target exists, which cut the links' samples to 899 (8.5's
  boot). The rest (new blocks, 4,116 samples; invalidations, 448) remains:
  29-32% of a boot under Rosetta, and comparisons with real x86 hardware
  should allow for it.
- **AArch64 leaves byte-reversed device accesses to the interpreter**:
  it runs `lwbrx`/`stwbrx` through `call_step()`, and `ppc_step()` gives
  up at a fault rather than offer the access to the hook, so each such
  access leaves the block (80 interpreter exits in 40 turns of a loop
  with one of each). x86 translates them, so they take the mmio hook's
  path in place. Counted: 198, 461 and 594 exits in the 8.5, 9.1 and
  9.2.2 CD boots (0.4-1.9% of their exits to the interpreter) and 186 in
  Unreal Tournament's 30 billion instructions, so AArch64 stays as it is.
- **Without fastmem, loads from a readable page below an unreadable one
  went to the interpreter**, on both hosts: the check is MEM_FAST_R, which
  asks for the next page too (for an access that might cross), and
  `mmio()` then declined an access that was fine. With
  `BLUE_JIT_FASTMEM=0`, 9.1's and 9.2.2's CD boots left 147 and 48
  million times for the interpreter (47 million of 9.2.2's: D-form loads
  with the next page unreadable). Fixed in milestone 5 (`slow_loads()`
  in both translators; on AArch64 only without fastmem, so that its code
  with fastmem stays as it was).
- **On a real x86 processor, fewer instructions weren't always faster**:
  r29 in r14, with the budget moved to `jit_state.remaining` (block
  entries comparing it in memory, exits subtracting from it there), cut
  0.07-0.15 host instructions per guest instruction from the boots, but
  on the Zen 5 the 8.5, 9.1 and 9.2.2 boots took 1.469, 1.594 and 1.549 s
  with it against 1.436, 1.533 and 1.485 s without, and the benchmark's
  integer and copy loops ran 7% and 18% slower: each block's exit and
  the next block's entry depend on each other through a store and a load.
  Reverted. Rosetta had shown no difference.
- **Where the Zen 5's cycles go** (its counters, per guest instruction,
  read by a measuring build): the 8.5 boot 1.85 cycles, 0.36 of them with
  the front end stalled, 0.0052 branch misses, 0.051 L1 instruction-cache
  misses; 9.2.2's 2.62 cycles, 0.89 stalled in the front end, 0.0175
  branch misses (the 68k emulator's dispatch through `bctr`, returns
  through `blr`), 0.065 instruction-cache misses. Translated code takes
  71% (8.5) and 81% (9.2.2) of the profiler's samples. What would help
  next is in the front end: returns predicted (a shadow return stack for
  `bl`/`blr`), fewer bytes of code per guest instruction.
- **The Zen 5's front end, by its counters** (the 9.2.2 CD boot, gcc;
  counted for the whole process with `perf_event_open()` from a small
  wrapper, user space only, which `perf_event_paranoid` 2 allows; AMD's
  raw events): 6.5 billion cycles; of the dispatch slots (8 a cycle), 54%
  got nothing from the front end and 11% waited on the back end; 6.2 of
  17.8 billion operations came from the decoders, after 1.4 billion op
  cache misses; 149 million L1 instruction-cache misses (fills from L2:
  0.9 billion); 2.28 billion branches, 43.5 million mispredicted, 31.8
  million of them indirect (of 232 million indirect branches); 27.7
  million returns, all C's (generated code had none), 11,000 of them
  mispredicted.
- **Most `bclr`s aren't returns** (the interpreter counting, with a
  32-entry return stack and a last-target predictor as models): 9.2.2's
  boot ran 193 million `bclr`, with only 20.8 million calls; 17.6 million
  went where the last call would return. The rest is the 68k emulator's
  dispatch, which goes through LR: `lhau r27,2(r24)`, `rlwimi
  r29,r27,3,13,28`, `mtlr r29`, `lhau`, `bflr cr2_lt` (`0x68066080`; that
  `bflr` alone runs 30.6 million times). 8.5's: 145 million `bclr` (38.7
  million returns) and 78.5 million `bcctr` (its emulator dispatches
  through CTR); 9.1's: 209 million (44.3 million).
- **Returns through the host's return stack: no gain, dropped.** A `bl`
  pushed its return address and CALLed the block at its target (linked as
  a jump would be), the host's return address pointing at a link to the
  block at pc + 4; an unconditional `blr` compared the guest address on
  top with LR and RETurned (`ret 8`) if they matched, else looked up the
  block table as before; the stack, generated code's own from a sentinel
  at the bottom, was dropped at exits and when a call would take it past
  64 KB. In 9.2.2's boot 10 million returns took the RET: indirect
  mispredictions fell by 1.5 million, but return mispredictions rose by
  0.94 million (stacks dropped at exits, unwinding past frames), and host
  instructions by 0.45%: cycles +0.6%. The indirect predictor had been
  predicting those returns' jumps already.
- **The 68k emulator's dispatch is spread already**: each handler's block
  follows its `b` into the emulator's dispatch code (the ROM-like rule),
  so it ends with its own indirect jump. 9.2.2's busiest dispatch `bflr`
  is reached from 665 handler entries, none with more than 8% of its runs;
  with a site per handler a last-target predictor misses 14.7 million
  times instead of 28.3. Spreading further would take blocks specialized
  by more history (the handler before), which the block table (one block
  per guest address) doesn't have.
- **Hot and cold code apart**: a block's main line was 56% of its bytes,
  the out-of-line code (side exits, slow paths, branch exits, link stubs,
  the budget and redispatch code) 44%, right after it. Moving it all to an
  area of its own changed nothing (cycles +1%): the taken paths of
  conditional branches are out of line too and run as often as taken
  guest branches. With those kept beside the main line (about 40% of the
  code apart), L1 instruction-cache misses fell 3% and cycles 1.3% (gcc;
  clang 2%); the slow path of stores to code pages (the 68k emulator's,
  next to its own code) made no difference either way. AArch64 can't
  easily do the same: its branches to out-of-line code (TBZ ±32 KB, B.cond
  ±1 MB) would need veneers, and the M5 runs these boots at 3.2
  instructions a cycle, against the Zen 5's 2.4.
- **What's left in the front end** is code size: the hot main line is
  about 210 bytes a block. Candidates: the SO test in every compare (`cmp
  dword [rbx+xer], 0` and `js`, 13 bytes: XER is beyond a byte
  displacement from rbx), rel32 jumps where rel8 would reach, the block
  entry's `cmp r14, imm32`.
- **Counting host instructions without the host**: QEMU's user mode
  with a TCG plugin that counts each translated block's executions (and
  so every instruction's) gives the instructions an x86 processor would
  retire, exactly and the same every run, for the whole process: about
  30 s for an 8.5 CD boot on the M5. The tools (in the scratch area, not
  the tree): QEMU 9.2.2 built with `--enable-plugins` for the
  x86_64-linux-user and aarch64-linux-user targets, the plugin (per
  instruction address: the block's count), a measuring build of Blue
  writing a map (each block's host range, each guest instruction's
  start, kind and word, where the main line ends, the stubs, the text's
  address, the process's mappings), an interpreted run counting guest
  instructions by kind, and a program putting them together. One thing
  it counts differently: each iteration of `rep movsb` is an
  instruction to QEMU, one instruction to a processor, so glibc's large
  copies (`memmove`'s ERMS path) show as 0.08-0.15 host instructions per
  guest instruction on x86, against 0.01 for AArch64's NEON copies.

## Timing on real hardware

`tools/jit-timing.sh [runs]`, from the top of a checkout on the machine
to time (Linux or macOS, x86_64 or AArch64, with a C compiler and make),
does everything and writes it to `jit-timing-<host>-<date>.txt` to send
back:

- the processor and the commit;
- the synthetic benchmark (`BLUE_JIT_BENCH=1`);
- each CD boot whose files it finds, in instruction time (the same guest
  work on every host): `runs` times with the JIT (3 by default) and once
  interpreted, with the JIT's statistics; and the processor's counters,
  host instructions and cycles per guest instruction: on Linux for the
  emulation thread (`BLUE_PERF=1`, where `perf_event_paranoid` allows),
  on macOS for the whole process (`/usr/bin/time -l`; under Rosetta, the
  ARM instructions it runs).

The counts compare with Status's (QEMU's, Linux x86_64): 6.24, 7.09
and 6.28 host instructions per guest instruction for the 8.5, 9.1 and
9.2.2 boots. QEMU counts the whole process; `BLUE_PERF` counts the
emulation thread, which is nearly all of it.

The ROM and CD images are looked for at the top of the tree (`MOS.rom`,
`85.iso`, `91.iso`, `922.iso`) or where `BLUE_ROM`, `BLUE_85_ISO`,
`BLUE_91_ISO` and `BLUE_922_ISO` point; without them only the benchmark
runs. A full run takes a few minutes, most of it the interpreted boots.

## Milestones

Each one is reviewed and merged on its own. Each leaves `make test` and
the fuzzer passing on AArch64 and x86_64 (Rosetta, podman, CI). The 8.5,
9.1 and 9.2.2 CD boots in instruction time must end exactly as the x86
interpreter's and the AArch64 build's do: screenshot, instruction count,
statistics (the JIT's own line aside).

1. **The split, then the integer core.**
   1. The runtime moves into `jit.c` and the AArch64 translator into
      `jit_a64.c`, with byte-identical AArch64 code and the same timings
      (a commit of its own).
   2. `x86.h` with encoding tests.
   3. `jit_x86.c`:
      - the CPU check and its message;
      - the mapping;
      - stubs: enter, exit, side exit, miss, hook, lookup, link;
      - blocks: budget check, header, redispatch;
      - the register cache;
      - all integer instructions, CR and XER, `mftb`;
      - branches, links, branch exits, the inline lookup;
      - the hook and the match hook.

      Kernel-bound instructions (`sc`, `twi`, host calls) go to the
      interpreter (always exact, as it makes calls in place itself), and so
      do loads, stores and FP, through `call_step()`. `test_jit.c`'s
      fastmem check is relaxed for x86 until milestone 2. The CI fuzz job
      on x86_64. The benchmark.
   4. Boots work from here, slowly: every load and store calls the
      interpreter.
2. **Memory.**
   - Fastmem loads and stores, all sizes and forms; `lmw`/`stmw`, `dcbz`.
   - The fault handler on Linux and macOS (with the profiler's context
     pc).
   - Checked accesses after faults and with `BLUE_JIT_FASTMEM=0`.
   - Device registers and stores to code pages leave for the interpreter
     at the instruction.

   Boots faster than interpreting from here.
3. **Floating point.**
   - The FPR cache.
   - `lfs`/`lfd`/`stfs`/`stfd` and the `stw, stw, lfd` forwarding.
   - Moves, `fsel`, compares, `frsp`, `fctiw[z]`.
   - The A-form fast path with FMA3, the lazy FPSCR and settle stub, the
     FPSCR instructions, slow paths.
   - With the supervisor's agreement, `ppc_fp.c`'s NaNs made PowerPC's on
     every host (a separate commit, with its own tests).
4. **The rest.**
   - Kernel calls and the quick `sc` in place.
   - Device registers in place (the mmio stub).
   - `code_store()` for stores to code pages.
   - String instructions through the interpreter, as on AArch64.
   - README (the JIT section: hosts, x86 specifics, Rosetta), [plan.md](plan.md)
     Findings.

   Kernel and device exit counts as AArch64's.
5. **Performance**, by measurement (done; Status), the list as planned:
   - register assignment: r29 as a sixth fixed register, the budget in
     memory: tried, and reverted on the real processor's times;
   - compare-and-branch fusion and a lazy CR0: not needed once a CR field
     took eight instructions (AArch64's count); a branch's `test` and jump
     fuse in the processor anyway;
   - D-form addressing without LEA: for displacements 0 or more, with no
     guard below the view (negative ones would need it);
   - block alignment: blocks start on 16 bytes, as planned; instruction
     counts can't say more, real hardware could;
   - memory operands for read-once guest registers: not done; what
     translated code loads and stores is mostly registers used again in
     the next block (r29, r27, XER, r1);
   - native `lswx`/`stswx`: not needed (0.01-0.02% of the boots'
     instructions);
   - the 68k emulator's dispatch jump: the inline lookup no longer masks
     (`l1_entry()`);
   - under Rosetta, fewer writes to code that has run: links made when
     translating, which real processors gain from too;
   - FP records that the next arithmetic would replace unread: not done;
     the boots run almost no FP (0.1% of instructions).

   The aim was host instructions per guest instruction within about 1.3
   times AArch64's: 1.04-1.10 in the CD boots.

## Tests

- **`tests/test_jit.c`'s fuzzer, unchanged.** It already covers:
  - integer, CR, XER, rotates, shifts by 0-63, division by 0 and -1;
  - loads and stores of every form, page crossings, a read-only page, an
    mmio device, stores into code;
  - FP with special values, FPSCR instructions, `stw, stw, lfd`;
  - branches, plain `b` into ROM-like code, hooks;
  - kernel calls and `sc` in place.

  It must pass 100,000 programs on each target, `BLUE_JIT_FAR` and
  `BLUE_JIT_FASTMEM=0` included.
- **The encoder**: a table of instructions and their bytes, from clang's
  x86-64 assembler, checked into `test_jit.c` (as `test_logic_imm()`
  checks `a64.h`). It covers:
  - REX.R/X/B;
  - rbp and r13 as a base with no displacement, rsp and r12 as a base
    (SIB);
  - RIP-relative operands;
  - 8- and 32-bit displacements and immediates, the 16-bit prefix;
  - two- and three-byte VEX with each map and W;
  - MOVBE, LZCNT, BMI1/2 and FMA3 forms.
- **`BLUE_TEST_JIT=1`**: fail rather than skip without a JIT (CI).
- **The CPU check**: an environment variable that can only hide features
  (never add them) has the JIT decline, with its message.
- **By hand, per milestone**:
  - the three CD boots in instruction time under Rosetta (and podman by
    milestone 2), against the interpreter and AArch64;
  - a window run of 9.2.2;
  - MacBench's FP test under Rosetta for milestone 3;
  - Unreal Tournament from a fresh APFS clone of the user's 8.6 disk for
    milestone 4.

## Risks

- **Register pressure**: nine host registers for guest values against
  AArch64's thirteen, and two-operand forms. Mitigated by memory
  operands, BMI2's non-destructive forms, tuning by counts, and the cache
  being the same design.
- **Instructions x86 lacks**: no bitfield insert (CR fields, `rlwimi`) and
  flag-based CA/OV mean more host instructions per guest instruction than
  AArch64. The compare and CR work is the place to watch.
- **Encoding and flag bugs**: the encoder table, the emitter's live-flags
  assertion, and the fuzzer.
- **Speed is measured only under emulation here**: CI's synthetic numbers
  and the counts are the estimate until real hardware is available.
  Rosetta's and QEMU's costs for code patching distort translation-heavy
  phases.
- **Processors below x86-64-v3 interpret**: Sandy and Ivy Bridge, Pentium
  and Celeron parts up to 2020, older Atoms, AMD before Excavator, the
  2013 Mac Pro, Rosetta without its variable. That's the user's decision,
  and the message says why.
- **The split could disturb AArch64**: the byte-identical hash and the
  timing comparison guard it.
- **Hardened runtime** if Blue is ever notarized: `MAP_JIT` is already
  used; the entitlement would be added then.
- **The interpreter's host-dependent NaNs** (above): a difference between
  hosts and from a 750, not between the JIT and the interpreter.
- **Older x86 costs**: 64-bit IDIV latency (avoided: 32-bit with the
  divisor checked), slow PDEP/PEXT on Zen 1/2 (not used), and the
  Skylake JCC erratum (alignment, milestone 5 if it shows).

## Open questions

1. **File layout**: `jit.c` as the shared runtime (keeping the public
   interface and the name the README uses), and `jit_a64.c`/`jit_x86.c`
   as translators. Or keep `jit.c` as the AArch64 translator and put the
   runtime in a new file?
2. **NaNs in `ppc_fp.c`**: make them PowerPC's on every host in milestone
   3? That changes AArch64 hosts' results for NaN-producing operations
   only.
3. **Shared files this touches**:
   - `Makefile` (two sources);
   - `nk.c` (its "no JIT" line gives way to the JIT's reason);
   - `tests/test_jit.c`;
   - `.github/workflows/ci.yml`;
   - README and [plan.md](plan.md);
   - `mem.c` (milestone 5's guard below the view).

   Any objection?
4. **`BLUE_TEST_JIT=1` on the x86 CI runners** fails the job if a runner
   lacks x86-64-v3. Acceptable, or skip there?
5. **Real hardware** (for the user): is there an x86 machine (Intel Mac
   or Linux PC) to time boots on later?
6. **Rosetta in the podman machine**: leave it off (QEMU, slower, other
   agents undisturbed), as planned?

## Size

Rough, with tests:

| Part | Lines |
|---|---|
| The split: `jit_int.h`, interface edits in `jit.c`/`jit_a64.c` (1,100 lines moved) | 300 |
| `x86.h` (encoder) | 700 |
| `jit_x86.c`: stubs, blocks, register cache, integer, branches | 1,800 |
| Memory, faults, checked paths | 600 |
| Floating point, settle stubs | 1,100 |
| Kernel calls, `sc`, mmio, code stores, match hook, profiler | 400 |
| Tests (encoder table, benchmark, `BLUE_TEST_JIT`) | 600 |
| CI, Makefile, README, [plan.md](plan.md) | 150 |

About 5,500 lines, nearly all new. Milestone 1 is about half of that.
