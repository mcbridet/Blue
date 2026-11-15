# Plan: a JIT for x86_64 hosts

Guest PowerPC code runs in syshm's interpreter on every host, and on
AArch64 hosts in a JIT (`src/jit.c`, `src/a64.h`): about 200 million guest
instructions a second against the JIT's 2.5-5 billion. x86_64 hosts, where
syshm will be used most, only interpret. This plan adds an x86-64 backend
to the JIT, as exact as the AArch64 one (results, instruction counts, exits
and faults bit for bit the interpreter's) and as fast as x86 allows,
sharing everything that isn't about the host's instructions.

## Status

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
- **Checks**: the fuzzer passes 450,000 programs under Rosetta (100,000
  with `SYSHM_JIT_FASTMEM=0`) and 30,000 under QEMU; all of `make test` on
  macOS and Linux, both architectures. The three CD boots end exactly as
  on AArch64 under Rosetta, and the 8.5 CD under QEMU. AArch64's
  generated code is byte for byte as before.
- **Speed** under Rosetta on the M5, milestone 2's build against this
  one, alternating (medians of 4): the 8.5 CD 5.9 → 5.9 s, 9.1's 4.7 →
  4.5 s, 9.2.2's 4.5 → 4.4 s. The boots hardly use FP: their time goes to
  the interpreter, which they leave the JIT for 4 to 13 million times
  (kernel calls and device registers: milestone 4). The benchmark's FP
  loop: 98 → 900 million instructions a second (AArch64's JIT natively:
  1,680); its integer and copy loops are as before (7,200-7,900 and
  10,500), once blocks start where they did (Findings).

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
  under Rosetta, with `SYSHM_JIT_FASTMEM=0` too, and the 8.5 CD under
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
  (with `SYSHM_JIT_FAR` and `SYSHM_JIT_FASTMEM=0` too) and over the 8.5,
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
  syshm says it; CI fuzzes and benchmarks the JIT on all four targets
  (`SYSHM_TEST_JIT=1`); `tools/jit-timing.sh` (below) times a machine.

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
`SYSHM_JIT_FAR` and `SYSHM_JIT_FASTMEM=0`, and over the 8.5, 9.1 and 9.2.2
CD boots in instruction time. Generated code holds no host addresses, so
the hashes don't depend on where things are mapped. Timings, medians of
alternating runs before and after: the 8.5 CD to 4 billion, the 9.1 and
9.2.2 CDs, and `SYSHM_JIT_PROGRAMS=20000`. After the split,
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
  above. The cache catches reuse within a block. A guest register read
  once can stay in memory, since x86 instructions take memory operands
  (`add r8d, [rbx+off]`). Milestone 5 tunes this with dynamic counts:
  loads and stores of guest registers per guest instruction.
- **Upper halves are kept zero.** 32-bit operations zero-extend on x86, so
  a guest value is always usable as a 64-bit index.

### Condition register and XER

- **Compare into field f**: `mov` 2, 4 and 8 into the scratch registers,
  `cmp`, `cmovg` and `cmovl`, then `bt xer,31` and `adc` for SO. The
  field is then shifted into place, and `and` and `or` merge it into CR.
  That is about ten instructions, against about seven on AArch64 (CSEL,
  BFI). `Rc=1` uses `test`.
- **Branch on a CR bit**: `test cr, imm32` and a conditional jump, fused
  into one operation. `bdnz` and the like use `dec` with a conditional
  jump.
- **CR logical operations**: `bt`/`setc` or shifts; `crset` and `crclr`
  are `bts` and `btr`.
- **XER[CA] in**: `bt xer,29` moves it into the carry flag for `adc`. The
  subtract-from forms invert it (`cmc`) for `sbb`, since x86's borrow is
  PowerPC's carry inverted.
- **XER[CA] and [OV] out**: `setc`/`sbb` and `seto`, merged into XER.
- **Rules for the emitter**: nothing that changes flags goes between a
  flag's producer and its consumer (debug builds track live flags and
  assert), and there are no 8- or 16-bit partial register writes (MOVZX
  instead).

### Integer instructions

- **`rlwinm`**: RORX and an AND. Masks that amount to a shift or a zero
  extension become SHL, SHR, MOVZX or BZHI. **`rlwimi`**: `rorx t; xor t,
  d; and t, mask; xor d, t`.
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

- **EA**: a 32-bit LEA (`lea eax, [rA+d]` or `[rA+rB]`), which wraps and
  zero-extends as PowerPC wraps. The access is `[r15 + rax]`.
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
  `SYSHM_JIT_FASTMEM=0`. They test the page-flag byte (`MEM_FAST_R`,
  `MEM_FAST_W`, `MEM_CODE`) and go through C's base, with slow stores to
  code pages and the mmio hook's path as on AArch64.
- **Faults**: the site table holds the byte offset of each access's first
  byte, prefixes included, and its stub. The handler reads RIP
  (`gregs[REG_RIP]` on Linux, `uc_mcontext->__ss.__rip` on macOS), finds
  the site and moves RIP to the stub. Linux sends SIGSEGV; macOS sends
  SIGBUS for a protected page and SIGSEGV for an unmapped one. x86 faults
  are precise: a store that straddles into a protected page writes
  nothing (better than AArch64's architecture promises). Host pages are
  4 KB, so the view's protection is exactly the guest page's.
- **Later** (milestone 5, measured): `[r15 + rA + d]` without the LEA for
  D-form accesses that don't update rA. It needs a 64 KB guard below the
  view (a small change in `mem.c`). An address that wraps lands in the
  guard or in the view's top slack, both inaccessible, and the
  interpreter does it.

### Code buffer, links, invalidation

- **The mapping**: 64 MB RWX, as on AArch64 Linux, after `struct jit`'s
  64 KB. macOS x86_64 adds `MAP_JIT`. Intel Macs need it only under the
  hardened runtime (syshm's builds aren't hardened), and then the
  allow-jit entitlement is all that's missing. Intel has no per-thread
  write protection to switch.
- **No cache maintenance**: x86 keeps instruction fetch coherent with the
  same thread's stores, so `arch_sync()` does nothing.
- **Block entry**: `cmp r14, imm32` (7 bytes) and `jb` to the budget stub.
  The 8-byte block pointer stays in front of the code, aligned so the
  code starts on 16 bytes (milestone 5 measures whether that helps).
- **Links**: `jmp rel32`, first aimed at a stub that asks C, which then
  patches the displacement. **Invalidation**: the entry's `cmp` becomes a
  `jmp rel32` to the block's redispatch code. It's one thread, so the
  patches need no atomicity.

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
  x86_64' all build/x86/syshm-tests` (inside `build/`, so it's ignored;
  native objects stay in `build/`).
- **Run**: `ROSETTA_ADVERTISE_AVX=1 arch -x86_64 build/x86/syshm-tests
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
- **`SYSHM_TEST_JIT=1`** (new, like `SYSHM_TEST_GPU`) makes "no JIT" a
  failure rather than a skip.
- **The Toolchain step prints the CPU's features**, so a runner without
  x86-64-v3 shows why.
- **The x86 runners are real hardware**: Intel for `macos-26-intel`,
  Azure's AMD or Intel for Ubuntu.

### Speed without x86 hardware

- **A synthetic benchmark** (`SYSHM_JIT_BENCH=1 build/syshm-tests jit`,
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
- **Host instructions per guest instruction**:
  - *Statically*, per translated block, main path and out of line, for
    both translators over the same instruction-time boot. That needs no
    runtime cost: one statistics line.
  - *Dynamically* on x86 (`SYSHM_JIT_COUNT=1`, `jit_x86.c` only):
    counters at block entries and out-of-line paths, times the host
    instructions of each straight-line piece. This gives an exact count
    for generated code under any emulator. AArch64's real counts come
    from `/usr/bin/time -l` and `SYSHM_PERF`.
- **Rosetta timings** of the CD boots per milestone, against the
  interpreter's 22.2 s.
- **Real hardware**: if the user has an x86 machine (an Intel Mac or a
  Linux PC), `SYSHM_PERF=1` on Linux gives the real instructions and
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
  Blocks now start on a 4 KB page of their own, which keeps the
  benchmark's clear of a boundary. Real x86 processors don't care; a
  boot's blocks cross boundaries as they fall, as they always have
  (milestone 5 may keep small blocks from crossing, if Rosetta timings
  matter enough).

## Timing on real hardware

`tools/jit-timing.sh [runs]`, from the top of a checkout on the machine
to time (Linux or macOS, x86_64 or AArch64, with a C compiler and make),
does everything and writes it to `jit-timing-<host>-<date>.txt` to send
back:

- the processor and the commit;
- the synthetic benchmark (`SYSHM_JIT_BENCH=1`);
- each CD boot whose files it finds, in instruction time (the same guest
  work on every host): `runs` times with the JIT (3 by default) and once
  interpreted, with the JIT's statistics; on Linux also the processor's
  counters (`SYSHM_PERF=1`: host instructions and cycles per guest
  instruction), where `perf_event_paranoid` allows.

The ROM and CD images are looked for at the top of the tree (`MOS.rom`,
`85.iso`, `91.iso`, `922.iso`) or where `SYSHM_ROM`, `SYSHM_85_ISO`,
`SYSHM_91_ISO` and `SYSHM_922_ISO` point; without them only the benchmark
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
   - Checked accesses after faults and with `SYSHM_JIT_FASTMEM=0`.
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
   - README (the JIT section: hosts, x86 specifics, Rosetta), plan.md
     Findings.

   Kernel and device exit counts as AArch64's.
5. **Performance**, by measurement:
   - register assignment;
   - compare-and-branch fusion and a lazy CR0;
   - D-form addressing without LEA (the view's guard);
   - block alignment;
   - memory operands for read-once guest registers;
   - native `lswx`/`stswx` if profiles say so;
   - the 68k emulator's dispatch jump;
   - FP records that the next arithmetic would replace unread, which are
     stored at once when the result overwrites an operand (`fmadd f2,
     f2, f3, f1`): kept elsewhere, or not made, where nothing between can
     leave the block or read them.

   The aim is host instructions per guest instruction within about 1.3
   times AArch64's, and timings on real hardware where it can be had.

## Tests

- **`tests/test_jit.c`'s fuzzer, unchanged.** It already covers:
  - integer, CR, XER, rotates, shifts by 0-63, division by 0 and -1;
  - loads and stores of every form, page crossings, a read-only page, an
    mmio device, stores into code;
  - FP with special values, FPSCR instructions, `stw, stw, lfd`;
  - branches, plain `b` into ROM-like code, hooks;
  - kernel calls and `sc` in place.

  It must pass 100,000 programs on each target, `SYSHM_JIT_FAR` and
  `SYSHM_JIT_FASTMEM=0` included.
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
- **`SYSHM_TEST_JIT=1`**: fail rather than skip without a JIT (CI).
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
- **Hardened runtime** if syshm is ever notarized: `MAP_JIT` is already
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
   - README and plan.md;
   - `mem.c` (milestone 5's guard below the view).

   Any objection?
4. **`SYSHM_TEST_JIT=1` on the x86 CI runners** fails the job if a runner
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
| Tests (encoder table, benchmark, `SYSHM_TEST_JIT`) | 600 |
| CI, Makefile, README, plan.md | 150 |

About 5,500 lines, nearly all new. Milestone 1 is about half of that.
