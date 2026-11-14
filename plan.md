# Plan: Mac OS 9.1 and later

Mac OS 9.1, 9.2 and 9.2.2 need the "Mac OS ROM" file from their own CD,
and that ROM wants a NanoKernel 2 underneath: a kernel with
Multiprocessing Services built in, that runs Mac OS as one task (the "blue
task") among others it schedules. syshm implements the NanoKernel v1
interface natively (`src/nk.c`). This plan adds the parts of the NanoKernel
2 interface Mac OS 9.1 uses, one piece at a time, without changing what
8.5–9.0.4 see.

The work is broken into tasks that a subagent can pick up with no other
context than this file. Each task says what it needs, what it touches, how
to check it, and what to hand back.

## Status

Done:

- **Parcels** (`src/parcels.c`, `src/rom.c`): 9.1's ROM file keeps its
  contents in parcels. syshm loads them, takes the 4 MB Toolbox image from
  the `'rom '` parcel, and puts the `'node'` parcels' libraries
  (`AAPL,CodePrepare`: MotherBoardHAL, TimeManagerLib, NVRAMServicesLib,
  RTCServicesLib; `AAPL,CodeRegister`: NativePowerMgrLib, StartLib) and the
  `'prop'` parcels that match its devices (Cuda, Grackle, the RTC under
  via-cuda, `/rom/macos`'s version) in the device tree. `rom parcels
  MOS91.rom` lists them.
- **Address space** (`default_regions()` in `src/nk.c`): 9.1's ConfigInfo
  has no page map (the Trampoline in the same file hands the kernel one),
  so syshm lays out the 8.5 ROM's map at the logical addresses ConfigInfo
  still gives.
- **A blue task** (`init_info_records()`): for ROM files with parcels
  (`m->mp_kernel`), NanoKernelInfo's version 2 fields name a blue process
  (ID 1, which the ROM checks), a blue task and a page queue.
- **Multiprocessing Services calls** (`src/mp.c`, `kcall_mp_dispatch()`):
  `sc` (only when `mp_kernel`) or kernel call 8, call number in r0. Done so
  far: tasks (7-10, 126; see below), queues (15-18, 39), semaphores
  (20-23), critical regions (25-28), event groups (49-52, 54),
  notifications (64-67, 128), task queries (11, 12, 14, 56, 59, 63),
  `MPYieldWithHint` (13), areas (77, 80; one per startup region plus Mac
  OS's RAM), the page ager's (82, 100, and 116 for an invalid process),
  timers (40, 41, 30, 31, 32), delays (33, and 55 for tasks), and `sc`
  with r0 -1 and -2 (`system_call()`: is the current task the blue one).
  `SYSHM_LOG_MP=1` logs each, the task switches and timers firing. Setting
  an event group that raises a 68k software interrupt stops the run (not
  delivered yet).
- **Tasks** (Task 1): tasks other than the blue one are made, started,
  terminated and deleted, and run on the one processor: registers saved
  in `struct mp`, swapped by `switch_task()`.
- **Scheduling** (Task 2): NanoKernel 2's priorities, ready queue and time
  slices (`mp_next()`, `mp_switch()`, `mp_tick()`), waits that block and
  wake (see Findings), the blue task's polls (yield) and naps (tasks run
  meanwhile), interrupts making the blue task critical
  (`mp_interrupt()`), and its 68k interrupt mask setting its priority
  (`blue_boost()`). NanoKernelInfo's task count starts at 2, as with the
  real kernel's idle task.
- **Timers and delays** (Task 3): timeouts on waits, `MPDelayUntil`, and
  timer objects firing (`mp_tick()`), all on the guest timebase.

Where it stops:

```
build/syshm boot MOS91.rom --cdrom 91.iso --max 20000000000
MPDispatch: a timer set event group 0x00160001, which raises a software interrupt
stopped: software interrupts from Multiprocessing Services aren't delivered yet (system context)
```

Startup's three tasks first run at instruction 53,900,000, once the 68k
code unmasks interrupts (as with the real kernel, `docs/nk2-reference.md`):
'deth' and 'dbgr' block on their queues; 'ager' sets itself up (100, 64,
15, 39, 49, 66, 66, 82, 116, 100, the real kernel's sequence) and blocks in
`MPWaitForEvent` with a 3 s timeout. The first of the Time Manager's timers
armed for real (0x00450001, due at timebase 54,143,452) then fires at
instruction 54,160,000 and sets the level-1 software interrupt group:
that's Task 5's (on the real kernel too the first software interrupt comes
17 ms after the tasks first run). Nothing is on the screen yet. Without
Task 3's timers the blue task idled to the limit (21 million naps) with
the grey desktop pattern.

## Ground rules for every task

- **Build and test:** `make -j8`, then `make test` (all suites must pass).
  C11 for the emulator, tabs, the style of the code around you. Comments
  only where the code doesn't say it; describe what is, not the history.
- **Don't change 8.5–9.0.4.** Everything new is for NanoKernel 2 ROMs only,
  behind `m->mp_kernel` (true for ROM files with parcels). Before handing
  back, check that the 8.5 CD still reaches the Finder:
  `build/syshm boot MOS.rom --cdrom 85.iso --max 4000000000 --screenshot
  /tmp/85.png` (only the menu-bar clock may differ between runs).
- **Stay reproducible.** Runs in instruction time (no `--window` or
  `--realtime`) must stay exactly reproducible: guest tasks run on the one
  host thread, scheduled at instruction counts and guest timebase values,
  never by host time.
- **The 9.1 check:** `build/syshm boot MOS91.rom --cdrom 91.iso --max
  20000000000` (a few seconds). Add `SYSHM_LOG_MP=1` for the calls, and
  `--screenshot f.png` once something may be on screen. Report the new
  stopping point (the last lines of output).
- **Leave the user's files alone.** `hd.img`, `hd86.img`, `86update.img`
  and the NVRAM files are the user's installs. CDs go in with `--cdrom`
  (read-only). For a writable disk, copy one to a temporary directory.
- **Parallel work:** the project is not a git repository, so two subagents
  editing the same file will clobber each other. Tasks below list the
  files they own; run tasks that share files one after another. (Making
  the project a git repository would allow a worktree per task.)
- **Hand back:** a short report (what changed, what was found, the new
  stopping point), and update this file: the task's **Status** line, and
  anything learned that later tasks need under **Findings** at the end.
  Update the README's "Mac OS 9.1 (not yet)" section in the README's
  style when behaviour changes.

## References

- **NanoKernel 2, reverse-engineered:** `git clone --depth 1
  https://github.com/elliotnunn/powermac-rom`. The kernel is in
  `NanoKernel/` (`NKMPCalls.s` dispatch and return paths, `NKTasks.s`
  tasks, `NKSync.s` queues, semaphores, critical regions, event groups,
  notifications, timers, `NKAddressSpaces.s` areas, `NKScheduler.s`,
  `NKInit.s` startup, `NKInterrupts.s`). Structures are in `Internal/`
  (`NKOpaque.a` objects and ID classes, `InfoRecords.a` NanoKernelInfo,
  `EmulatorPublic.a` the emulator's context block). Each MP call is
  `DeclareMPCall <n>, <name>`; arguments come in r3 up, the status goes
  back in r3, results in the registers after it (some calls differ: look).
- **NanoKernel 1** (what `nk.c` follows): `git clone --depth 1
  https://github.com/elliotnunn/NanoKernel`.
- **Parcels and ROM files:** tbxi (`github.com/elliotnunn/tbxi`),
  `tbxi/parcels_dump.py` for the flags.
- **Disassembling:** `tools/disasm.py MOS91.rom 0xFFCE2700 [COUNT]` (or
  `ADDR:END`, or a symbol such as `MPCreateTask`) disassembles PowerPC or 68k
  code from a ROM file's image and/or a `--dump-ram` file (`disasm.py
  ram.bin MOS91.rom 0x28D70`), naming the fragment an address is in (the
  image's PEF containers, the parcels' libraries, their copies in RAM) with
  exports, imports and MP call numbers; `--list` shows what is where. It
  needs capstone: a Python virtual environment with `pip install capstone
  machfs` in a temporary directory (machfs reads the CDs' HFS volumes).
  `build/syshm rom extract MOS91.rom img91.bin` writes the 4 MB image
  (address `0xFFC00000 + offset`), `--dump-ram FILE` saves RAM.
- **Tracing a run:** `--trace-from N --trace-reg 24 --interp` (r24 is the
  68k PC), `--traps N` (68k traps), `--watch ADDR`, `--trace-range LO:HI`.
  Instruction counts are the same with and without the JIT.

## How syshm's kernel works (what the tasks build on)

- `nk_boot()`'s loop in `src/nk.c` runs guest code with `ppc_run()` in
  chunks and handles each exit: kernel calls (`twi` → `kernel_call()`),
  `sc` (→ `kcall_mp_dispatch()` when `mp_kernel`), faults, host calls for
  syshm's drivers. Devices and interrupts are updated every `SYNC_INSNS`
  instructions and at real exits.
- Mac OS (the blue task) runs as NanoKernel 1 contexts: the System context
  (Apple's 68k emulator) and an alternate context (native code), switched
  by `switch_context()` with `save_context()`/`load_context()` to context
  blocks in guest memory. `m->system`, `m->context`, `m->flags`,
  `m->enables` describe the running one.
- Interrupts: `update_interrupts()` posts the 68k level to the emulator
  through EmulatorData and the System context's CR; native code running
  then is ended with exception 0 (`m->preempt`) so the emulator takes it.
- Idle: `idle_wait(m, deadline)` (PowerDispatch naps, the `_SynchIdleTime`
  hook, MP call 55) sleeps in real time; in instruction time it returns at
  once. With tasks ready, they run instead (`blue_naps()`).
- `src/mp.c` holds the Multiprocessing Services objects (`struct mp`):
  IDs as NanoKernel 2 makes them (slot << 16 | the slot's use count), the
  blue process, task, page queue and address space, the tasks' saved
  registers and the ready queue, and `mp_call()`, which does a call for
  the current task and returns false for ones it doesn't have.
- Tasks: `mp.current` is on the processor (its registers in `m->cpu`);
  `blue_running()` says whether that's the blue task. After an exit,
  `nk_boot()`'s loop switches (`switch_task()`) to `mp_next()` when
  `mp.yield` is set.

## Decisions

- **Tasks share the one processor and host thread.** Each task other than
  the blue one has a saved register set (GPRs, FPRs, CR, LR, CTR, XER,
  FPSCR, PC) in `struct mp`. Switching tasks swaps them with `m->cpu`'s;
  the blue task's state is whatever its NanoKernel 1 contexts hold.
- **Blocking, as NanoKernel 2 does it:** a task that must wait is taken
  off the processor with its PC on the `sc`; when what it waits for comes
  (or its timeout), it is made ready and runs the `sc` again.
- **The blue task never blocks** (NanoKernel 2 returns
  `kMPBlueBlockingErr`, -29293, and Mac OS polls). While it polls, or
  naps, ready tasks run.
- **Interrupts belong to the blue task.** When one is posted while another
  task runs, the blue task runs next.
- **Time is the guest timebase.** Timeouts, timers and time slices are in
  timebase ticks, so instruction-time runs stay reproducible.
- **Calls are added as Mac OS makes them**, each after NanoKernel 2's
  source, rather than all at once.

## Tasks

Dependencies: 1 → 2 → 3 → 4; 5 after 2; 6 and 7 any time (they don't touch
the kernel); 8 onwards as the boot progresses.

### 1. Task contexts and task creation

- **Status:** done. Calls 7 (create), 8 (start), 9 (terminate), 10
  (delete), 126 (`KCSetTaskType`), 59 (the debugger's queue) in `mp.c`;
  per-task state and saved registers in `struct mp` (`mp_regs()`), a FIFO
  ready queue, `mp_next()`/`mp_switch()`; `switch_task()`,
  `blue_yields()`, `system_call()` in `nk.c`. A returning entry point ends
  the task (its LR starts at the kernel data page; the ISI there
  terminates it with r3). To get to a nap, timers are made, set and armed
  (40, 30, 31, 32) without firing: Task 3 makes them fire. `tests/test_mp.c`
  covers the task calls. See Findings.
- **Owns:** `src/mp.c`, `src/mp.h`, the task-switching parts of `src/nk.c`.
- **Goal:** tasks other than the blue one exist and can run.
- **Steps:**
  1. Read `MPCall_7` (create), `MPCall_8` (start: parameter, priority),
     `MPCall_9` (terminate), `MPCall_10`, and `CreateTask` in
     `NKTasks.s`, and the Task structure in `NKOpaque.a`. Note which
     registers carry what (call 7 returns the ID in r10).
  2. Give `struct mp` per-task state: registers, state (ready, running,
     blocked, terminated), weight/priority, termination queue, exception
     queue. Keep `mp.c` free of `struct machine`: `nk.c` asks it which
     task runs next and swaps registers.
  3. A switch primitive in `nk.c`: save the running task's registers,
     load another's; for the blue task, save and restore `m->cpu` plus the
     NanoKernel 1 context fields, so the blue task resumes exactly where
     it was (System or alternate context).
  4. A task whose entry returns: point its LR at a stub that ends it
     (NanoKernel 2 sends the termination message to its queue with the
     task's result: see `MPCall_9` and the exit path).
  5. Until task 2 exists, run a new task when the blue task next polls
     (gets `kMPBlueBlockingErr`) or naps, until it blocks or ends.
- **Done when:** the 9.1 check gets past call 7 (and the start call), the
  new task runs (log the switches with `SYSHM_LOG_MP`), and the boot stops
  somewhere later. Note what the task does first (likely waiting on a
  queue).

### 2. Scheduler, blocking and waking

- **Status:** done. Priorities, ready queue and time slices after
  `NKScheduler.s` (`mp_next()`, `mp_switch()`, `mp_tick()` in `mp.c`;
  `switch_task()`, `blue_naps()`, `blue_boost()` and the run loop in
  `nk.c`); waits on queues, semaphores, event groups and critical regions
  block and are woken (and deletions wake them with `kMPDeletedErr`);
  `MPYieldWithHint` (13); the page ager's calls (82, 100, 116). Timeouts
  are Task 3's. `tests/test_mp.c` covers it. See Findings.
- **Owns:** `src/mp.c`, `src/mp.h`, the run loop in `src/nk.c`.
- **Goal:** waits block MP tasks and wake them; the blue task and MP
  tasks share the processor fairly and deterministically.
- **Steps:**
  1. Blocking waits for MP tasks on queues (18), semaphores (23), event
     groups (52), critical regions (27): block with the PC on the `sc`;
     notify, signal, set and exit wake the first waiter, which reruns the
     call (`EnqueueMessage`, `SignalSemaphore`, `SetEvent` show how).
  2. The ready queue and choosing what runs: NanoKernel 2 has priorities
     (critical, latency-protected, normal, idle) and weights
     (`NKScheduler.s`); start with what Mac OS uses, simplest that is
     faithful.
  3. Switch points: a task blocks or ends; the blue task polls (blue
     blocking) or naps (`idle_wait` callers: give ready tasks the time
     first, nap only if none); a time slice ends (a timebase deadline,
     checked at chunk boundaries); an interrupt is posted while a task
     other than the blue one runs (the blue task runs next).
  4. Instruction-time runs: confirm two runs give identical output.
- **Done when:** blocked tasks don't spin, the 9.1 check gets further, and
  8.5 still boots.

### 3. Timers and delays

- **Status:** done. Waits on queues (18), semaphores (23), event groups
  (52) and critical regions (27) time out (a Duration: milliseconds, or
  negative, microseconds; `kDurationForever` doesn't), their calls
  returning `kMPTimeoutErr`; `MPDelayUntil` (33, and 55 for tasks) blocks
  until a time; timers (40 records the time made, 41 deletes, 31 arms with
  NanoKernel 2's options) fire in `mp_tick()`: queue, semaphore and event
  group as call 30 set, then deleted unless kept. The run loop stops when
  one sets a software interrupt group. `idle_wait()` wakes for them in real
  time (`mp_next_due()`). `tests/test_mp.c` covers it. See Findings.
- **Owns:** `src/mp.c`, `src/mp.h`.
- **Goal:** timeouts on waits, `MPDelayUntil` (33) for tasks, timer
  objects (`NKCreateTimer` 40, `NKDeleteTimer` 41, `NKSetTimerNotify` 30,
  `MPArmTimer` 31, `MPCancelTimer` 32), all on the guest timebase.
- **Notes:** call 55 (`MPDelayUntilSys`) already exists for the blue task
  (it naps); keep that behaviour for the blue task. Timers fire by causing
  their notification (queue, semaphore or event group).
- **Done when:** a task's delay and a wait with a timeout end on time in
  instruction time.

### 4. Exceptions in tasks

- **Status:** not started. **Needs:** 1.
- **Owns:** `src/mp.c`, the fault paths in `src/nk.c`.
- **Goal:** a fault, illegal instruction or trap in a task other than the
  blue one goes where NanoKernel 2 sends it (the task's exception queue,
  `MPSetExceptionHandler` 56, with the message format in `NKTasks.s`;
  else the task is terminated), never to the blue task's contexts.
  `MPThrowException` (57) too.
- **Done when:** a test task that faults is reported on its queue (a unit
  test in `tests/`), and the blue task is unaffected.

### 5. Software interrupts to the 68k emulator

- **Status:** not started. **Needs:** 2 (a task usually sets the event).
- **Owns:** `set_event()` in `src/mp.c`, `update_interrupts()` and nearby
  in `src/nk.c`.
- **Goal:** setting an event group marked by `MPSetSWIEvent` (54) raises
  its 68k software interrupt, as NanoKernel 2's `SetEvent` does: the
  group's ID goes in the emulator's context block (`SWIEventGroupID`,
  `+0xD0`) or, when `EDPOffsetSWIRelated` (`+0xC8`) is set, in that table
  of EmulatorData at the interrupt's level; then the interrupt is posted
  the way external interrupts are. Work out from the emulator's code in
  the ROM image how it takes and acknowledges them.
- **Notes:** at startup the ROM marks two groups, levels 7 and 1; the
  level-7 one is a notification target of the RAM area's page faults.
- **Done when:** the run no longer stops with "software interrupts from
  Multiprocessing Services aren't delivered yet", and the 68k handler runs
  (trap log or trace shows it).

### 6. Reference run under QEMU (optional, research only)

- **Status:** done. `docs/nk2-reference.md` answers the questions; 9.1
  boots to the Finder on QEMU's `mac99` unpatched. Tools:
  `tools/qemu-ref/run91.sh`, `shots.py`, `rsp.py` (gdbstub client, no gdb
  needed), `mptrace.py` (MP call trace), `nk2dump.py` (decodes RAM dumps).
- **Owns:** `tools/qemu-ref/` (new files only), a notes file
  `docs/nk2-reference.md`.
- **Goal:** what Apple's 9.1 Trampoline and NanoKernel 2 really do, to
  check syshm against: the page map they build (and the areas from it),
  the device tree after the Trampoline (parcel properties applied), the
  first IDs and NanoKernelInfo, the sequence of MP calls during startup
  (a breakpoint on the system call vector, `0xC00`). `tools/qemu-ref/`
  boots a 1.x ROM file on `g3beige`; 9.1's ROM may need `mac99`.
- **Done when:** the notes file answers those questions, or says what
  stood in the way.

### 7. A disassembly helper (optional)

- **Status:** done: `tools/disasm.py` (usage in its docstring and under
  References).
- **Owns:** new files in `tools/`.
- **Goal:** one command to disassemble PowerPC or 68k code at a guest
  address, from a ROM file (its image) or a RAM dump, naming the code
  fragment an address is in (the ROM image's PEF containers, the parcels'
  libraries). Python with capstone as an optional dependency, like the
  other `tools/` scripts; usage in its docstring.
- **Done when:** it disassembles `0xFFCE2700` (the MP call stubs) and
  `0xFFC015E0` (the 68k code that prepares `AAPL,CodePrepare`'s
  libraries) from `MOS91.rom`, with fragment names.

### 8. The rest of startup

- **Status:** not started. **Needs:** 2, then whatever the boot needs.
- **Goal:** take the 9.1 check from wherever it stops to the Finder,
  fixing one stop at a time. Expected along the way:
  - More MP calls (areas and pages: `MPCreateArea` 72, `MPGetPhysicalAddress`
    81, `MPHoldPages` 89 and friends, `MPGetPageSize` 69, `NKLocateInfoRecord`
    107; follow `NKAddressSpaces.s` and `NKPaging.s`). Virtual memory stays
    off: every page is mapped, so page faults to a backing provider
    shouldn't happen.
  - The parcels' libraries being prepared and registered (the 68k code at
    `0xFFC015E0`/`0xFFC016B0` walks `Devices:device-tree:AAPL,CodePrepare`
    and `AAPL,CodeRegister`): check they load.
  - The parcels' drivers against syshm's devices: the Cuda library, the
    RTC driver under via-cuda, and NVRAM (the ROM has a driver for
    `mac-io/nvram`; syshm has no such node and keeps NVRAM behind RTAS).
  - The display, disk and Ethernet drivers syshm makes (NDRVs, lanLib)
    under 9.1, and idle (does 9.1's Process Manager still nap through
    call 55 and `_SynchIdleTime`?), fast start.
- **Done when:** the 9.1 CD reaches the Finder; then its installer puts
  9.1 on an empty disk image and that boots.

### 9. Regression and documentation

- **Status:** ongoing. Run after every task that changes behaviour.
- **Checks:** `make test`; the 8.5 CD to the Finder (above); 8.6 and
  9.0.4 if their images are available (copy writable disks first).
- **Documentation:** the README's "Mac OS 9.1 (not yet)" section and Next
  steps; the site's Status window (`site/index.html`, "Mac OS 9.1–9.2.2"
  row) once 9.1 works.

### 10. Mac OS 9.2–9.2.2

- **Status:** not started. **Needs:** 8, and their CDs from the user.
- **Goal:** their ROM files (newer parcels, possibly newer kernel
  features) on the same path. Find out first whether they list `iMac,1`
  in `<COMPATIBLE>`.

## Findings

Facts later tasks need, added as they are found.

- **ID format:** slot << 16 | the slot's use count; the first ID made is
  1 (the blue process). Classes (`NKOpaque.a`): process 1, task 2, timer
  3, queue 4, semaphore 5, critical region 6, CPU 7, address space 8,
  event group 9, coherence group 10, area 11, notification 12.
- **Status codes:** `kMPInvalidIDErr` -29299, `kMPInsufficientResourcesErr`
  -29298, `kMPTaskAbortedErr` -29297, `kMPTimeoutErr` -29296,
  `kMPDeletedErr` -29295, `kMPBlueBlockingErr` -29293, `paramErr` -50,
  unimplemented call -4.
- **NanoKernelInfo v2** (`+0x100`): blue process ID, blue task ID, page
  queue ID, task count, free-pool extend count.
- **The ROM's Multiprocessing Services code** is a PEF container in the
  image at offset `0xDB0E0`-`0xE6E80` (imports `MPLibrary`'s `gMPSys…`
  globals); its call stubs (`li r0,N; sc; blr`) are around `0xFFCE2700`.
  Some calls come from the 68k emulator through kernel call 8 instead
  (`MPCreateEvent`, `MPSetSWIEvent`: NanoKernel 2's FE1F trap).
- **Startup's calls so far, in order:** 63 (blue task, 0x36180), 25, 20
  (maximum 0, initial 0), 25, 15, 39 (queue, 1), 64, 66 (notification →
  queue), 49, 54 (level 7), 66 (notification → that event group, flags
  0x80000000), 80 (area of address 0), 77 (RAM area's backing provider =
  that notification), 15, 15, 49, 54 (level 1), 25, 12, 25, 11, 20 (1, 0),
  20 (1, 0), 25, 15, 15, 25, 12, 12, 12, 27, 12, 28, then 7. Then, for each
  of three tasks: 7, 12, 15 (its queue), 39 (1), 8, 39 (queue 0x110001, 2,
  3, 4), 126 (its name); with 59 (debugger queue 0x1D0001) after the
  second. Then 11 ×4, 15 (0x250001), 39 (64), and 32 timers (40, 30 →
  queue 0x250001, 30 → event group 0x160001 flags 8, 31 at "never"
  0x7FFFFFFF_FFFFFFFF, 32), three of them then armed for real (31 at
  absolute times about 16 ms, 1.4 s and 3 s on, at instruction 53.9
  million). From TimeManagerLib (code at `0x0001EE00`, TOC `0x70538`). Then
  no calls until the blue task's first nap, call 55 (stub `0xFFCE2A28`), at
  instruction 116,381,084 (7 s of guest time, so with firing timers the
  path will differ), when the three tasks run.
- **Results in odd registers:** `MPCreateSemaphore` returns the ID in r5,
  `MPCreateTask` in r10; call 12 returns the current task's ID in r3 (not a
  status) and its call-63 word in r4; `MPWaitOnQueue` takes its timeout
  in r7 and returns the message in r4-r6.
- **Task calls (`NKTasks.s`):** 7 `MPCreateTask`(process r3, termination
  queue r4, options r5: anything but 2 and 4 is
  `kMPInsufficientResourcesErr`, so `kMPCreateTaskSuspendedMask` too; r6 →
  the task's call-12 word; r7 → its r12; r8 → its PC; r9 → its r2) → ID
  in r10. 8 (start: task r3, name r4, r5 → its r3, termination message
  words 1 and 2 r6 r7, r8 → its r1, r9 the stack's low end): only a task
  never started. 9 `MPTerminateTask`(r3, result r4 → message word 3): the
  message goes to the termination queue; `kMPInsufficientResourcesErr` if
  already terminated. 10 deletes a terminated task. 126 sets the name
  (`KCSetTaskType`). 59 sets the debugger's queue (where
  `ThrowTaskToDebugger` sends exceptions; Task 4). NanoKernel 2's `Task.State`
  is 0 (not queued: new, blocked, terminated), 1 ready, 2 running;
  terminated is a flag. New tasks have weight 100, priority nominal (2),
  like the blue task (weight 200), so starting one doesn't preempt the blue
  task. NanoKernel 2's `CreateTask` also makes a page-fault semaphore (an ID)
  per task; syshm doesn't, so later IDs differ from NanoKernel 2's.
- **MPLibrary's tasks:** the ROM's internal create (`0xFFCE03EC`, behind the
  exported `MPCreateTask` at `0xFFCE061C`) puts a 0x14C-byte record at the
  top of the stack block (the task's r1 and r3, and its call-12 word) and
  starts every task at a wrapper, `0xFFCE03A0`, that calls the entry point
  (record +0x28, parameter +0x2C) and then `MPTerminateTask` (`0xFFCE0230`,
  call 9) on itself with the result. So entry points don't return to their
  LR; syshm still starts LR at the kernel data page (`mp.task_return`),
  where a return faults and ends the task.
- **Startup's tasks:** 0x001F0001 'deth' waits on queue 0x001C0001 (the
  blue process's termination queue, every task's); 0x00210001 'dbgr' waits
  on the debugger's queue 0x001D0001; 0x00230001 'ager' (page ager) calls
  100 first. 'deth' and 'dbgr' block (forever, until Task 2 wakes them).
  In an experiment where 'ager' returned at once (which checked the
  return path: it ended, the blue task carried on), the blue task then
  idled to the 20-billion limit (21 million naps, 68k traps AA7F and AAFE
  in turn, nothing on screen but the grey pattern, 0 VBLs): most likely
  waiting for timers to fire.
- **`sc` specials (`IntSyscall` in `NKInterrupts.s`):** r0 -1: is the
  current task blue (r0 = 0x00200000 or 0, CR0 GT or EQ); -2: the same CR0,
  r0 = the current task's call-12 word; -3: the FDP's way back to
  supervisor mode (not seen). Any other r0 is an MP call; out-of-range ones
  return -4. MixedMode uses `sc` -1 (`0xFFCF8DB4`, after
  `MPCallUniversalProc`).
- **Task switching in syshm (`nk.c`):** while a task other than the blue
  one runs, the blue task's NanoKernel 1 state (`m->context`, `m->system`,
  `m->flags`, `m->enables`) stays as it was; `update_interrupts()` posts to
  the System context's CR wherever it is (`system_cr()`: the blue task's
  saved registers when it was in the System context), and `preempt` waits
  until the blue task is back. Kernel calls other than 8 and exceptions in
  other tasks stop the run (Task 4). The switch happens in `nk_boot()`'s
  loop after an exit is handled, when `mp.yield` is set (see the
  scheduler's finding below).
- **Timers:** 40 makes one (ID in r4), 30 sets what firing does (as 66
  does for notifications), 31 arms it (options: see "Timers in syshm"
  below), 32 returns the time left in r4:r5 and disarms. TimeManagerLib's
  timers set event group 0x160001, the level-1 software interrupt group,
  when they fire: Task 5 is needed for that.
- **The scheduler in syshm (Task 2, `mp.c`):** one ready queue in arrival
  order (`mp->ready`, the blue task included when it's ready but not
  running); `mp_next()` (NanoKernel 2's `SchEval`) keeps the running task
  unless a ready one is strictly more urgent, else takes the first of the
  most urgent priority, else (all blocked) the blue task. Priorities 0
  critical, 1 latency-protected, 2 nominal (new tasks and the blue task),
  3 idle. A task woken by what it waits for becomes latency-protected and
  goes to the back (`SchRdyTaskNow`); a preempted or yielding one goes to
  the back too. Time slices (`start_slice()`): the task's weight's share of
  its priority's cake, with the ready tasks of that priority (cakes:
  `TimebaseTicksPerPeriod(-1042)` = 17193 ticks, times 8 per priority);
  at its end (`mp_tick()`, the decrementer) a critical or latency-protected
  task becomes nominal and yields. A running nominal task keeps its slice
  when re-evaluated; more urgent ones get a new one (as NanoKernel 2's
  `major_0x148ec`). `mp->yield` is NanoKernel 2's SchEvalFlag: set by
  anything that readies, blocks, ends or yields; the loop then calls
  `mp_next()` and `switch_task()` (which also settles a task that yielded
  and was chosen again). `mp_tick()` runs when `mp_due()` (`mp->next_tb`;
  every call sets it to 0), only at exits that aren't for debugging.
- **Blocking and waking:** a task that waits blocks with its PC on the `sc`
  (`waiting` = the object, `since` = the order it blocked in; the first to
  block is woken first). Queues, event groups and critical regions wake
  their first waiter to make the call again (`EnqueueMessage`, `SetEvent`,
  `MPExitCriticalRegion` rewind the PC by 4); a semaphore passes straight
  to its first waiter, whose call then returns noErr without taking it
  again (`SignalSemaphore` leaves the value alone); deleting an object
  wakes every waiter with `kMPDeletedErr` (priority unchanged). Such
  statuses are kept in `task.resume`: the call made again returns it at
  once (`mp_call()`), which is also how timeouts end waits (Task 3).
- **The blue task:** a poll (`kMPBlueBlockingErr`) yields to tasks as
  urgent and records the object (`mp->polled`, NanoKernel 2's
  `BlueSpinningOn`); when that object becomes available, a napping blue
  task wakes (latency-protected), else its next nap is skipped
  (`UnblockBlueIfCouldBePolling`, `MPDelayUntilSys`). A nap (call 55, the
  `_SynchIdleTime` hook, PowerDispatch: `blue_naps()`) with tasks ready
  blocks the blue task until its time (then latency-protected, like
  `TimerFire1`), an interrupt, or nothing else is ready (then the rest of
  the nap is `idle_wait()`); with none ready it's `idle_wait()` as before;
  with an interrupt pending, neither. A posted interrupt
  (`update_interrupts()` → `mp_interrupt()`) makes the blue task critical
  and puts it at the front (the PIH's `SchRdyTaskLater`).
- **The 68k interrupt mask** (`blue_boost()`, NanoKernel 2's
  `SchFiddlePriorityShifty` and `IntDecrementer`): the emulator keeps the
  68k SR's upper byte in its context's r25 (0x27 masked, 0x20 not; read
  live while the System context runs, from the ECB otherwise). Mask 7: the
  blue task is critical at every evaluation, and `mp_tick()` does nothing
  but come back a critical slice later (the decrementer skips
  `TimerDispatch`: no time slices or timers while the 68k has interrupts
  off). Mask at or above the ECB's PriorityShifty level (+0xCC, low
  nybble; 0 in syshm, so 2): latency-protected. In syshm's run the mask is
  7 from instruction 1.1 million to about 53.88 million, right after
  TimeManagerLib arms its timers; the tasks then run at the next sync. No
  kernel call 4 comes with it (the real one comes because an interrupt is
  pending). Not modelled: PriorityShifty's 0x100 bit (the kernel sets it
  when it boosts the blue task; the emulator then calls `MPYieldWithHint`
  on its wait path) and high bit, so syshm doesn't see call 13 from the
  emulator; call 13 is implemented after `MPYieldWithHint` (r3 returned
  unchanged).
- **The page ager:** `MPGetFreePageCount` (100) returns `mp->free_pages`,
  NanoKernel 2's primed free list size, 18 + RAM pages / 2048 (34 at
  128 MB; the real kernel said 16 at 256 MB, its pool having taken some).
  The ager's thresholds are 5 and 10 pages (MPLibrary globals, at
  0x35978, which its TOC points to at -0x1C, and its TOC + 0x774 =
  0x3597C): below 10 it looks for pages to take (the routine at
  `0xFFCE220C`; probably `MPFindVictimPage` 95, then 128 and 67 on the RAM
  area's backing provider, which the real kernel's boot makes 45 times
  each), and below 5 its wait shrinks by 0.1 s a pass, down to 0.3 s
  (growing back to 3 s at 10 or more). `MPRegisterAger` (82)
  keeps the notification (`mp->ager`; `MPGetFreePage` would set its event
  group when pages run out). `MPGetNextIDOwnedByProcess` (116) is called
  with process 0 and returns `kMPInvalidIDErr`, as on the real kernel;
  other uses are unimplemented. NanoKernelInfo.TaskCount starts at 2
  (`mp->tasks`), so the page-fault queue's reserves are 2, 3, 4, 5.
- **Timers in syshm (Task 3):** timer objects and blocked tasks' deadlines
  (`task.deadline`: a timeout, a delay's end, the blue task's nap) end in
  `mp_tick()`, earliest first and, among those due together, in the order
  they were armed or blocked (`mp->queued`), as NanoKernel 2's sorted timer
  queue does (`EnqueueTimer`, `TimerDispatch`). A timeout wakes the task
  with its priority unchanged, its call returning `kMPTimeoutErr`
  (`TimerFire2`); a delay's end latency-protected, its call returning noErr
  (`TimerFire1`). A timer (`TimerFire3`) sends its message, signals its
  semaphore and sets its event group (each only if that object still
  exists), and is deleted unless armed with `kMPPreserveTimerIDMask`
  (1). `MPArmTimer`'s options as NanoKernel 2 takes them: 4 converts r4, a
  Duration, to ticks; 2 adds the timer's last time (the time it was made,
  for a new one); without 2 the result is an absolute time, even for a
  Duration. TimeManagerLib arms all its timers with option 1 (kept). The
  first timer of syshm's run fires at instruction 54,160,000, its time
  54,143,452: deadlines are seen at the next sync point or exit (every
  20,000 instructions at most, about 1.2 ms of guest time), never while the
  blue task runs with its 68k interrupts masked.
- **Reference run** (`docs/nk2-reference.md`, QEMU `mac99`): up to call 7
  the real sequence is syshm's, plus a first MPYieldWithHint (13) from the
  emulator. Right after the third task is named, Mixed Mode starts making
  `sc` with r0 = -1 (53k per boot): not an MP call; the kernel sets r0 =
  0x00200000 (CR0 GT) for the blue task, 0 (CR0 EQ) otherwise, and changes
  nothing else. NanoKernelInfo.TaskCount is 2 at startup (blue + idle);
  MPLibrary sets the page-fault queue's reserve from it. The real kernel
  holds 22 objects before the first call (blue task 0x00050001, page queue
  0x00040001, RAM area 0x000A0001; first MP result 0x00160001). The new
  tasks first run at the blue task's first kernel call 4 and block at once
  ('deth', 'dbgr': MPWaitOnQueue forever; 'ager': MPWaitForEvent, 3 s).
  Level-1 software interrupts (Time Manager timers) fire from 0.2 s after
  call 63; the emulator takes them as 68k vector 0x58 with the event
  group's ID pushed, and the ROM's handler calls MPWaitForEvent(group, 0).
  The Trampoline puts AAPL,CodePrepare and AAPL,CodeRegister first among
  `/`'s children.
