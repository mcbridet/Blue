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
  `MPYieldWithHint` (13), `MPGetNextID` (37), areas (77, 80; one per
  startup region plus Mac OS's RAM; 78 their records, 93/94 page ages),
  the page ager's (82, 100), timers (40, 41, 30, 31, 32), delays (33, and
  55 for tasks), processes (3, 4, 5, 6, 99, 116), the CPU plugin (2, 46,
  in `nk.c`), and `sc` with r0 -1 and -2 (`system_call()`: is the current
  task the blue one). `SYSHM_LOG_MP=1` logs each, the task switches,
  timers firing and software interrupts raised.
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
- **Software interrupts** (Task 5): setting an event group marked by
  `MPSetSWIEvent` puts it in the emulator's table and raises its 68k level
  (`raise_swi()` in `mp.c`), posted to the emulator like an external
  interrupt (`post_software_interrupt()` in `nk.c`); the 68k handler's
  `MPWaitForEvent` takes it out (`swi_taken()`). The emulator's
  PriorityShifty bits (0x100, the top bit) are kept as NanoKernel 2 keeps
  them, so the emulator yields its boost when it unmasks interrupts.
- **The rest of startup** (Task 8): the page ager's area records (78) and
  page ages (93, 94); the CPU plugin (2, 46: Mac OS registers its G3
  plugin, whose routines run as NanoKernel 2's SIGP runs them);
  `MPProcessors` (3); processes (4, 5, 6, 99, 116). The 9.1 CD boots to
  the Finder.
- **The clock and NVRAM** (Task 8): for ROM files with parcels the device
  tree has the iMac's NVRAM chip (`nvram` under mac-io, `src/nvram.c`),
  backed by RTAS's 8 KB and the same `--nvram` file; with it
  NVRAMServicesLib prepares, and so does RTCServicesLib after it, whose
  Cuda RTC driver asks the Cuda for the time. See Findings.

Where it stops:

```
build/syshm boot MOS91.rom --cdrom 91.iso --max 20000000000
stopped: instruction limit reached (system context)
```

The CD's Finder is up by instruction 2 billion (about 1 s on the host):
"Mac OS 9.1 / Starting Up..." with the extensions' icons, then the desktop
with the CD's window open (9 items: Before You Install, Installing Mac OS
9.1.pdf, Mac OS Install, Adobe Software, CD Extras, Utilities...), and it
idles there (MP calls 11, 13, 18, 31, 32, 52 as the Time Manager runs; the
page ager every 3 s). Two runs are identical. The menu-bar clock shows the
guest time (`--fixed-time` and the guest time since, in the host's time
zone, as for 8.5), and 9.1 reads NVRAM through its own driver for the
`nvram` node (`nvram: 320 reads, 0 writes` at the end; it writes what it
changed when it shuts down or restarts). Installing 9.1 from the CD
onto a disk image is left to the user (in a window); scripted, its
installer got as far as its Important Information page with a new 2 GB
image as the destination.

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
- **Go easy on the host's memory.** Never load a whole disk or CD image, or
  a large RAM dump, into a Python process (machfs `Volume.read()` on a 2 GB
  image, `f.read()` of a whole image): read only the blocks needed, or mmap.
  Run one heavy process at a time, leave none running when a step is done,
  and delete big scratch files as soon as they're no longer needed.
- **Parallel work:** the project is not a git repository, so two subagents
  editing the same file will clobber each other. Tasks below list the
  files they own; run tasks that share files one after another. (Making
  the project a git repository would allow a worktree per task.)
- **Hand back:** a short report (what changed, what was found, the new
  stopping point), and update this file: the task's **Status** line, and
  anything learned that later tasks need under **Findings** at the end.
  Update the README's "Mac OS 9.1 (from its CD)" section in the README's
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
  group as call 30 set, then deleted unless kept (an event group may raise
  a software interrupt: Task 5). `idle_wait()` wakes for them in real
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

- **Status:** done. `raise_swi()` (in `set_event()`) and `swi_taken()` (in
  `MPWaitForEvent`) in `mp.c` keep the emulator's table and level as
  `SetEvent` and `MPWaitForEvent` do (with or without the table); a napping
  blue task is woken critical; `post_software_interrupt()` in `nk.c` posts
  it (CR, `preempt`) as `update_interrupts()` does external ones. The
  PriorityShifty bits are modelled (`blue_boost()`, `set_shifty()`; see
  Findings), and `MPGetNextID` (37) added for the page ager.
  `tests/test_mp.c` covers the bookkeeping. The 68k handler runs (each
  software interrupt is followed by its `MPWaitForEvent` through kernel
  call 8, the `FE1F` glue). See Findings.
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

- **Status:** the 9.1 CD reaches the Finder; installing from it is left to
  the user (in a window). Done: the page ager's area records (78) and page
  ages (93, 94); the CPU plugin (2, 46: `cpu_plugin_register()`,
  `cpu_plugin_call()` in `nk.c`); `MPProcessors` (3); processes (4, 5, 6,
  99, 116, with every object's owning process). `tests/test_mp.c` covers
  areas and processes. The clock and NVRAM work (the iMac's `nvram` node
  and chip for these ROM files; `tests/test_nvram.c`). Open: whether an
  installed 9.1 boots. Task 4 wasn't needed: no task faulted. See Findings.
- **Goal:** take the 9.1 check from wherever it stops to the Finder,
  fixing one stop at a time. Expected along the way:
  - More MP calls (areas and pages: `MPCreateArea` 72, `MPGetPhysicalAddress`
    81, `MPHoldPages` 89 and friends, `MPGetPageSize` 69, `NKLocateInfoRecord`
    107; follow `NKAddressSpaces.s` and `NKPaging.s`). Virtual memory stays
    off: every page is mapped, so page faults to a backing provider
    shouldn't happen. First: the page ager's area record (78) and page ages
    (93, 94), every 3 s (see Findings, "The page ager's pass").
  - The parcels' libraries being prepared and registered (the 68k code at
    `0xFFC015E0`/`0xFFC016B0` walks `Devices:device-tree:AAPL,CodePrepare`
    and `AAPL,CodeRegister`): check they load.
  - The parcels' drivers against syshm's devices: the Cuda library, the
    RTC driver under via-cuda, and NVRAM (done: the ROM's driver for
    `mac-io/nvram` drives syshm's `nvram` node; see Findings).
  - The display, disk and Ethernet drivers syshm makes (NDRVs, lanLib)
    under 9.1, and idle (does 9.1's Process Manager still nap through
    call 55 and `_SynchIdleTime`?), fast start.
- **Done when:** the 9.1 CD reaches the Finder; then its installer puts
  9.1 on an empty disk image and that boots.

### 9. Regression and documentation

- **Status:** ongoing. Run after every task that changes behaviour.
- **Checks:** `make test`; the 8.5 CD to the Finder (above); 8.6 and
  9.0.4 if their images are available (copy writable disks first).
- **Documentation:** the README's "Mac OS 9.1 (from its CD)" section and Next
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
  when they fire (Task 5 delivers the interrupt).
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
  nybble; 0 in syshm, so 2; nothing in the run writes it): latency-protected.
  In syshm's run the mask is 7 from instruction 1.1 million to about 53.88
  million, right after TimeManagerLib arms its timers. No kernel call 4
  comes with it (the real one comes because an interrupt is pending).
- **PriorityShifty's other bits** (Task 5, `set_shifty()` in `nk.c`): the
  kernel sets 0x100 whenever it boosts the blue task for its mask, and when
  it posts an interrupt to it (external or software). The emulator's
  set-SR routine (`0x6806C410`, which MOVE to SR, RTE and the like go
  through) then calls `MPYieldWithHint(1)` (`sc` at `0x6806C480`) when the
  new mask is 0, unless an external interrupt above it is pending (that
  path makes kernel call 4 instead). `MPYieldWithHint` and `MPDelayUntilSys`
  from the blue task clear 0x300 and set the top bit, which has the next
  evaluation clear it and leave the blue task's priority alone.
  Evaluations are `mp_next()` (SchEval) and an `mp_tick()` that ends
  something (`mp_fires()`: the decrementer), not every `mp_tick()` (every
  MP call makes one due). syshm's run makes call 13 about as the real one
  does: twice at the start, then once per Time Manager interrupt (after its
  `MPArmTimer`). The yield when the mask drops to 0 at 53.88 million leaves
  the blue task nominal at the front, so it keeps the processor to the end
  of its slice and the tasks first run at 54,320,000. 0x200
  (latency-protected whatever the mask) isn't modelled: nothing sets it.
- **Software interrupts** (Task 5): `MPSetSWIEvent` (54) marks a group
  0x10 | level (1-8, else 1). The emulator puts its table's offset, 0xAE0,
  at ECB+0xC8 itself at startup (`0x6806DDFC`): entries are at
  EmulatorData+0xAE0 + 4 × level. Setting the group (`SetEvent`,
  `raise_swi()`): if its level's entry is taken, nothing more (the flags
  are just ORed in); else the group goes there and, unless a higher level
  (7 down to 1) is in the table, ECB+0xD0 = its level and the blue task is
  to be interrupted (napping: critical, at the front; else its priority
  stays). The post ORs ConfigInfo's Post & Clear masks, 0x00800000 (cr2.lt
  only; an external interrupt also sets 0x00600000), into the System
  context's CR. The emulator (`0x6806D114` when cr2.lt is set, then
  `0x6806D3E4`): if ECB+0xD0's level is above its mask and no higher
  external interrupt is pending (cr2.gt, level at EmulatorData+0x71), it
  reads the table at that level, counts in EmulatorData+0xC0, raises its
  mask to the level and takes vector 0x58 (format 2) with the group pushed.
  It never clears the table or ECB+0xD0; whenever its SR is set with
  ECB+0xD0 nonzero it sets cr2.lt again, so a lower level waiting is taken
  once the mask drops. The ROM's handler (vector 0x58 = `0xFFC04A64`,
  dispatcher `0xFFC0FCD0`) calls `MPWaitForEvent(group, 0)` through the
  `FE1F` glue (kernel call 8, 68k pc `0xFFC10BB0`): with flags set, the
  kernel clears the group's entry (if it's that group's) and puts the
  highest level left, or 0, in ECB+0xD0 (`swi_taken()`). Without a table
  (ECB+0xC8 0), ECB+0xD0 holds the group itself, one at a time. Deleting a
  group leaves its entry (as NanoKernel 2 does).
- **The page ager's pass** (every 3 s; Task 8, done): it walks the areas
  with `MPGetNextID` (37: class r3, 0 any; the previous ID r4, 0 from the
  first; the next ID in r4, `kMPInvalidIDErr` after the last, when it
  starts again from 0: with no areas it would spin) and asks for each
  one's record with call 78 (`MPCall_78` in `NKAddressSpaces.s`: area r3,
  r4 must be 1, r5 the offset, 0x00/0x10/0x20/0x30/0x3C; four words back
  in r6-r9, three at 0x30, none at 0x3C, their byte count in r10). The
  record, from the Area structure (`NKOpaque.a`): 1, ProcessID,
  AddressSpaceID, ParentAreaID; BackingProviderID, PTEConfig,
  FlagsAndMinAlign, LogicalBase; Length, LogicalSeparation, Counter (page
  faults), BytesMapped; PageSize, +0x80, BackingProviderMisc. The real
  kernel's RAM area: 1, 1, 0x00030001, 0; 0x001A0001, 7, 0x600C, 0;
  0x0FA1C000, 0, 0, 0x0FA1C000; 0x1000, 0, 0 (`mptrace.py` in the
  reference run). The kernel's other startup areas are contiguous
  (FlagsAndMinAlign 0xE00C; PTEConfig from the page map's attributes: 4,
  |1 unless PP 1, |2 for PP 2 (0 counts as 2), |8 for PP 1, |0x40 W,
  |0x20 I, |0x80 G: 7 read-write, 5 read-only, 0xA7 I/O). The ager skips
  aliases (ParentAreaID) and areas with 0x8000, and in each pass ages a
  number of random pages (40 here) of the next paged area it comes to:
  `MPGetPageAge` (93: area r3, address r4 → r5) and `MPSetPageAge` (94:
  age r5, 15 bits), age × 1.25 + 1, at most 0x7FFF. Ages are a halfword
  per page of a paged area (its FaultCtrArray), 0x7FFF to start with; with
  0x8000 the page is held (get says 0, set fails with
  `kMPInsufficientResourcesErr`). NanoKernel 2's 94 also takes the page out
  of the hash table, and the next use of the page (a hash-table miss)
  halves its age; syshm has no hash table, so ages only grow.
  `MPFindVictimPage` (95) takes the oldest; nothing calls it while the free
  page count stays at `mp->free_pages`.
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
- **The CPU plugin** (Task 8; `nk.c`): code Mac OS registers for the
  machine's processors (starting others, their clocks, power and
  temperature). Before one is registered, `MPCpuPlugin` (46: selector r3,
  arguments r4-r10) returns -29286; Mac OS's probes make it with selector 0
  (two of them on syshm, from candidate plugins: a 9500/9600 one looking
  for Hammerhead, then the one that registers). `MPRegisterCpuPlugin` (2:
  coherence group r3, 0 the machine's; code r4 and size r5, page-aligned;
  descriptor r6 inside it: +0 a table of stacks by processor, +0x1C the
  routine count, at most 64, +0x20 their transition vectors) then takes
  "G3Plugin", which finds Grackle (on QEMU's mac99 a 52 KB one registers
  instead). Its routines: 0 CountProcessors (it says 1), 1-5 start,
  resume, stop, reset, signal a processor, 6-7 processor status, 8
  SynchClock, 9 GotSignal, 10 ProcessorState, 11 RunSIGPRun, 12 PowerMgt
  (temperature, power: the one touching SPRs), 13-16 empty (0). NanoKernel
  2's SIGP runs a routine at once, in supervisor mode with interrupts off,
  on the processor's stack, r3 the processor's index, returning to the
  emulator's ReturnFromException trap; the routine's r3/r4 are the call's.
  An exception in it gives -29285, a selector out of range -29287. syshm
  runs it in an inner loop (`cpu_plugin_run()`): no interrupts, timers or
  task switches meanwhile; mfmsr and device registers work, any other
  privileged instruction counts as an exception. The kernel's own SIGPs
  (12 from the idle task, 4060 per boot on QEMU; those for other
  processors) don't happen in syshm (no idle task, one processor). After
  registering, Mac OS calls selector 0 once more, then `MPProcessors` (3:
  r3 0 all, else scheduled; the count in r3, 1).
- **Processes** (Task 8): `MPCreateProcess` (4: address space r3, 0 the
  system's → r5) makes one late in startup (0x01B40001 in syshm,
  0x01C00001 on QEMU), and `MPSetBlueProcessID` (99: process r3, word r4;
  blue task only, else `kMPInsufficientResourcesErr`) moves the blue task
  into it, twice (r4 0x00AAF520, r5 'MACS', then a pointer): it sets the
  blue task's process and its call-63 word. NanoKernelInfo's
  blueProcessID stays 1. `MPTerminateProcess` (5) only marks it (no tasks
  can be made in it then; `kMPInsufficientResourcesErr` the second time);
  `MPDeleteProcess` (6) wants it terminated with no tasks left. Every
  object belongs to the process of the task that made it (a task to its
  own; startup's to the blue process), for `MPGetNextIDOwnedByProcess`
  (116: process r3, class r4, previous ID r5 → the next r5 of that class
  that belongs to it; class process, CPU or coherence group: paramErr).
- **Preparing `AAPL,CodePrepare`** (Task 8): the 68k code at `0xFFC015E0`
  has `0xFFC01300` walk the node's properties (those `AAPL,prepare_order`
  names first: here only "Placeholder", which isn't there; then the rest
  in order: MotherBoardHAL, TimeManagerLib, NVRAMServicesLib,
  RTCServicesLib) and prepare each library (`0xFFC01250`), and it stops
  at the first one that fails (its init returns an error). So a library
  that can't find its device keeps every later one from running.
- **The clock** (Task 8, done): 9.1 showed 12:00 AM because RTCServicesLib
  never ran: NVRAMServicesLib, before it, failed (`nrNotFoundErr`, -2539:
  no node with `device_type` nvram), and the walk above stopped. The ROM's
  ReadDateTime (`0xFFC0C572`) calls the platform's primitive (ProductInfo
  +0x44, table +0x1C: `0xFFC080F6` → `0xFFC08300`), which calls RTCGetTime
  through the table RTCServicesLib's init puts at ExpandMem +0x300 (+8
  RTCGetTime, +0xC RTCSetTime, routine descriptors; SetDateTime goes
  through table +0x20, `0xFFC08350`); with no table it returns -2405 and
  Time (0x20C) stayed 0xFFFFFFFF. RTCServicesLib's init finds the node with
  `device_type` rtc, prepares its `driver,AAPL,MacOS,PowerPC` (the
  via-cuda/rtc parcel's "Cuda RTC Plugin") and takes its
  `ThePluginDispatchTable` (version 1; +0x10, +0x14 init, +0x24 get time,
  +0x28 set time), sets Time from it, and installs a Time Manager task that
  adds 1 to Time every second. The plugin calls `_EgretDispatch` (OS trap
  0x92, through the trap table at 0x648 with `CallOSTrapUniversalProc`)
  with a pseudo packet, 3 to get the time and 9 to set it (seconds since
  1904, local time); the ROM's 68k Cuda Manager (`0xFFC09C00`) sends it
  over the VIA, and syshm's Cuda answers from the guest clock (SET_TIME
  moves its offset until the next restart). With the `nvram` node below,
  the run makes one more Cuda packet (the get) and the clock is right;
  Date & Time (8.6's, from a second disk) sets it with one more.
- **NVRAM** (Task 8, done): NVRAMServicesLib's init finds the node with
  `device_type` nvram and prepares its driver, which for an `nvram` under a
  node named mac-io is the ROM file's "Cuda NVRAM Plugin" (`nvram,cuda`;
  parcel flags: device_type and parent's name): it reads `AAPL,address`
  and moves byte k of NVRAM at that address + 16k (`lbzu`/`stbu`, one byte
  at a time), and says NVRAM is 8 KB, kind 2. Any kind but 1 has the
  library keep a copy in RAM in 32-byte blocks, each read from the chip
  when first used, and written back only by NVRAMFlush (kind 4: after every
  write): on the sleep queue's sleep and doze demands (installed through a
  Notification Manager task), and from the Shutdown Manager on Shut Down
  and Restart (`0xFFC2E7F0`, from `0xFFC2DEF8` and `0xFFC2E028`). Its
  table at ExpandMem +0x2FC: +0xC NVRAMRead, +0x10 NVRAMWrite, +0x14
  NVRAMFlush. The 68k PRAM routines (`0xFFC08230` read, `0xFFC082A0` write)
  use offset NKHWInfo +0x3C (0x1400, as syshm gives 8.5's ROM) plus the
  PRAM address, so 9.1 keeps PRAM where 8.5 does through RTAS, and one
  `--nvram` file serves both (each started from the other's file without
  reinitializing PRAM). A boot reads 0x1400-0x151F and 0x0000-0x001F (the
  partition header NVRAMFindPartition looks at; syshm's NVRAM has no
  partitions, and nothing seen depends on them). A first boot on a blank
  file writes PRAM's 256 bytes at shutdown; later ones the blocks that
  changed. The real node (a beige G3's Open Firmware listing; the iMac's
  Paddington is a Heathrow): `nvram@60000`, compatible nvram; Linux's
  powermac nvram.c
  takes the stride from the one `reg` range (0x20000 / 8 KB), QEMU's
  macio NVRAM is `addr >> 4 & (size - 1)`, and OpenBIOS gives `#bytes`
  0x2000, `reg` 0x60000 0x20000. syshm's node and chip are only in trees
  for ROM files with parcels; RTAS keeps the bytes (`src/nvram.c` points
  at them).
- **Task 4** (exceptions in tasks other than the blue one) wasn't needed
  to reach the Finder: no task faulted.
