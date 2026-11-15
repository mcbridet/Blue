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
- **The startup disk** (startup-wait): for ROM files with parcels the
  device tree has Open Firmware's `/chosen` `bootpath` (the partition the
  ROM file came from, else the first partition with boot blocks), and the disk driver answers the DriverGestalt
  selectors by which StartLib finds that drive, so the Start Manager takes
  it at once instead of after 25 s. See Findings, "The Start Manager's
  wait".
- **Floppies** (`--floppy`, `--floppy-drive`; `src/floppy.c`, the
  removable drive in `src/disk.c`): a floppy drive of syshm's own disk
  driver at PCI device 7, for raw and Disk Copy 4.2 images, whose disk
  comes and goes: put in from the window's menu, by dropping an image on
  the window, or from scripts (`floppy insert`, `floppy eject`); ejected
  by Mac OS. Mounts, writes, Eject, Erase Disk and a restart work on the
  8.5 and 9.1 CDs, FAT disks on an installed 8.5 (File Exchange). See
  Findings, "Media changes".
- **Mac OS 9.2.2** (Task 10): the MacOS9Lives 9.2.2 CD starts up to the
  Finder. For ROM files with parcels the info records are NanoKernel
  2.28's (HWInfo 0x0108 with CascadeInfo, ProcessorInfo 0x0112, SystemInfo
  0x0107); the kernel has the coherence group and processor objects, and
  calls 44, 45, 62 and 132; kernel call 7 answers unknown selectors; the
  floppy controller's Mac I/O cell reads as zeros; syshm's lanLib stays in
  the Ethernet node. See Findings, "Mac OS 9.2.2: the CD" and after.
- **CD swapping** (`--cdrom`, `--cdrom-drive`; removable CD-ROM drives in
  `src/disk.c`, where images go and the menu in `src/drives.c`): CD-ROM
  drives' discs come and go like the floppy's, through the same media
  changes, wherever they can be swapped (a window or real time, an empty
  drive, a script with `cdrom` commands); dropped images go by size, a
  floppy's to the floppy drive, others to a CD-ROM drive. The menu shows
  each drive's disc with Eject; its list of images is gone, and the
  `--floppy` images after the first go in as Mac OS ejects. Eject, Put
  Away and the Trash work on an installed 8.5 with a second CD-ROM drive
  (`ut.iso` out, `86.iso` in, by script), 9.1 and 8.5 still start up from
  ejectable CDs, and runs where discs can't be swapped are the same as
  before. See Findings, "Ejectable drives".
- **CD audio** (Task 16; `src/cdimage.c`, `src/cdaudio.c`, `src/cdrom.c`):
  CD-ROM drives answer as Apple's CD-ROM driver, with CD icons; cue sheets
  with audio tracks; the drive's audio played through the sound chip's
  input and read digitally. See Findings, "CD audio".
- **The display's VBL interrupt** (Task 17; `src/video.c`, `src/macio.c`,
  `src/bootinfo.c`, `src/nk.c`): a 60 Hz vertical-blank interrupt on the
  iMac's ATI line (source 22), through the display node's interrupt set,
  in place of the Driver Services timer. The restored 9.2.2 reaches the
  Finder in instruction time without a key. See Findings, "The display's
  interrupt".

Where it stops:

```
build/syshm boot --cdrom 91.iso --max 20000000000
stopped: instruction limit reached (system context)
```

The CD's Finder is up by instruction 1.1 billion (under 1 s on the host):
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
image as the destination. In real time with fast start the Finder is up
after 16 s of guest time (1.3 s on the host; it was 41 s before the
startup disk was found at once), against 13.8 s for the 8.5 CD: what's
left is mostly AppleTalk on Ethernet (5 s) and the CD's transfers.

The 9.2.2 CD (`build/syshm boot --cdrom 922.iso --max 4000000000`) has the
Finder up by instruction 1.5 billion, with the CD's Read Me open in
SimpleText (as under QEMU), and idles there; two runs are identical. In
real time with fast start the Finder is up after 13.4-13.7 s of guest time
(about 1 s on the host), and the host then sleeps 25 s of 30. It has
sound, USB's interrupts and interrupt-driven Ethernet (Findings, "9.2.2's
interrupt sets"). Its installer, Apple
Software Restore, restores the CD's image onto a blank disk from `disk
create` (671 MB, by instruction 40 billion of a scripted run), and the
restored system starts up to the Finder (the Setup Assistant over it the
first time), in real time and, since the display has its vertical-blank
interrupt, in instruction time too (Findings, "The restored 9.2.2 waits
for a key"). Set up, it's at the Finder by instruction 1.9 billion, and
after 13 s of guest time in real time. After half an hour of idling the power manager
dozes (PowerDispatch
1, CacheDispatch 4), and at 55 minutes its 'powr' task stops and restarts
scheduling around a sleep that doesn't happen; the system carries on.

## Ground rules for every task

- **Build and test:** `make -j8`, then `make test` (all suites must pass).
  C11 for the emulator, tabs, the style of the code around you. Comments
  only where the code doesn't say it; describe what is, not the history.
- **Don't change 8.5–9.0.4.** Everything new is for NanoKernel 2 ROMs only,
  behind `m->mp_kernel` (true for ROM files with parcels). Before handing
  back, check that the 8.5 CD still reaches the Finder:
  `build/syshm boot --rom MOS.rom --cdrom 85.iso --max 4000000000 --screenshot
  /tmp/85.png` (only the menu-bar clock may differ between runs).
- **Stay reproducible.** Runs in instruction time (no `--window` or
  `--realtime`) must stay exactly reproducible: guest tasks run on the one
  host thread, scheduled at instruction counts and guest timebase values,
  never by host time.
- **The 9.1 check:** `build/syshm boot --cdrom 91.iso --max
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

- **Status:** the MacOS9Lives 9.2.2 CD (`922.iso`) starts up to the Finder,
  and its Apple Software Restore installs onto a blank disk image. Done:
  NanoKernel 2.28's info records for ROM files with parcels
  (`init_info_records()` in `nk.c`, `bootinfo_hwinfo()`, `bootinfo_build()`
  in `bootinfo.c`); the coherence group and processor objects
  (`mp_init()`), `MPGetNextCpuID` (62), `MPStartScheduling` (44),
  `MPStopScheduling` (45), `NKSetClockDriftCorrection` (132) in `mp.c`;
  CacheDispatch (kernel call 7) for selectors above 2 (`kernel_call()`);
  the floppy controller's cell (`empty_cell_ops`, `setup_machine()`);
  syshm's lanLib kept over the ROM file's `bmac+` one (`build_tree()`).
  `tests/test_mp.c` and `tests/test_nvram.c` cover them. Open: Apple's own
  9.2/9.2.2 CDs (not tried: none here). Interrupt sets for the USB
  controller, the sound chip and lanLib on 9.2.2 (`AAPL,interrupt-index`
  for ROM files whose Trampoline makes it, and the codec's valid bit: see
  Findings, "9.2.2's interrupt sets") are done: sound, USB's interrupts and
  interrupt-driven Ethernet. The
  installed system's wait for a key in instruction time is gone (Task 17).
- **Goal:** their ROM files (newer parcels, possibly newer kernel
  features) on the same path. Find out first whether they list `iMac,1`
  in `<COMPATIBLE>`.

### 11. ROM from the startup disk

- **Status:** done. Without `--rom`, `syshm boot` takes the "Mac OS ROM"
  file from the startup disk, as a New World Mac's firmware does: the first
  drive in command-line order (partitions in map order) whose blessed
  System Folder (Finder info word 0) holds a file of type `tbxi`, the first
  in catalog order (`src/hfsread.c`: HFS, HFS+, HFS+ in an HFS wrapper;
  extents overflow). It prints where from (`rom: "Mac OS 9.1:System
  Folder:Mac OS ROM" from 91.iso, ...`); the run is otherwise as with
  `--rom MOS91.rom`. `rom info 91.iso` (and the other `rom` commands) read
  a disk image's the same way. `tests/test_hfsread.c` covers the reader.

### 12. Shared folder

- **Status:** done. `--share DIR` attaches a host folder as an HFS volume
  (`src/share.c`): metadata made in memory by `hfs_build()` (`src/hfs.c`,
  now with folders, B-trees of any depth, map nodes and Mac OS's name
  order), file data read from the host's files on demand, Mac OS's writes
  in an overlay; written back at the end of the run if Mac OS put the
  volume away, the host's deleted or replaced files moved to
  `DIR/.syshm-recovery/<date>/`, else the volume kept there as an image.
  The disk driver takes a `struct disk_backend` (read and write callbacks)
  in place of a file. `hfsread` reads through a callback too, and walks
  the whole catalog (`hfsread_walk()`). `tests/test_share.c` covers names,
  volumes (checked with hfsutils, and on Linux Apple's `fsck_hfs` from
  hfsprogs), dates, and writing back. 8.5 and 9.1 from their CDs and 8.6
  from a disk show the volume; Finder copies, new folders, renames, moves,
  deletions and a SimpleText save came back to the folder. Open: writing
  back when the volume is ejected, and offering the folder again as new
  media (needs the disk driver's removable media).

### 13. Installing the guest tools

- **Status:** done (8.5, 8.6, 9.2.2; 9.1 by the same path, tried as far
  as its CD allows: no installed 9.1 image). In
  window runs, syshm looks for the guest tools in the startup disk's
  System Folder before Mac OS starts (`install_check()` in
  `src/install.c`: the System Folder's folders, whatever their names, for
  files of the tools' type and creator whose code makes the tool's host
  call; current if both forks are the bytes this syshm makes). Once the
  Finder is up, if one is missing or differs (and the disk is writable
  and not locked), a box over the screen offers to install or update them;
  Mac OS runs on meanwhile. On yes, Mac OS writes them itself: syshm
  diverts the Process Manager's next idle call (`_SynchIdleTime` before
  Mac OS 9, trap AAAA with $82F0 in D0 on 9.x) to a 68k bootstrap that
  prepares a
  generated installer fragment with the Code Fragment Manager and calls it
  (see Findings). Input scripts answer with `tools install` or `tools
  later` (and turn the check on without a window). `tests/test_install.c`
  covers the check on HFS volumes made with `hfs_build()`, the offer, and
  the installer against a File Manager of its own. `--tools` is gone:
  `syshm tools create FILE` makes the volume for installing them by hand
  (`--cdrom FILE`). Open: "don't ask again"; 9.1 on an installed disk.

### 14. Clipboard

- **Status:** done. Text is shared by Mac OS and the host's clipboard in
  window runs in real time: `src/clip.c` (conversions; which side's text
  is newer), syshm Enhancements in `src/tools.c` (the guest side), and
  SDL2's text clipboard in `src/display.c`, on the window's thread, which
  is the emulation's. Enhancements (version 2 in its data: the installer
  tells an older copy on disk by its bytes, the version tells syshm where
  a running older copy's data is, which still switches modes) writes
  routine descriptors for ZeroScrap and PutScrap into the trap table and
  puts a GetNextEvent filter in `jGNEFilter`; when the host's text changes
  it posts the front application a resume event with convertClipboard, and
  keys typed in the window wait for that (at most 0.5 s). Input scripts'
  `clipboard` and `print-clipboard` stand in for the host's clipboard.
  `tests/test_clip.c` covers the conversions, echoes and held keys,
  `tests/test_tools.c` the patches. Scripted both ways on the 8.5 and 9.1
  CDs (SimpleText; `SYSHM_TOOLS_APPL=1` lets the Finder open Enhancements
  from the tools volume) and on a copy of an 8.5 install (Note Pad,
  Scrapbook, the Finder's names, Internet Explorer 4.01's address field);
  there `tools install` replaced the older Enhancements (`helper.quit`
  takes the patches out too) and the new one pasted in Note Pad. Open:
  pictures (see Findings), styles, applications that put their copy in the
  scrap only at a suspend, and the real host clipboard, which only a
  window run by hand uses.

### 15. Showing the screen

- **Status:** done. Three changes to what the window costs the emulation
  thread (which stops the Mac meanwhile): only the rows that changed are
  shown (`video_changed_rows()`: bands of them, compared with the copy last
  shown; whole frames still for mode and CLUT changes, the window's
  exposes and once a second), each band converted once, straight into a
  locked ARGB8888 streaming texture (`video_palette()`, `video_convert()`;
  `display_rows()` in `src/display.c`) and drawn again only there in the
  prescaled copy; and a hardware cursor (`src/video.c`), which the window
  draws over the screen (`display_cursor()`), in window runs in real time
  only. `video_to_rgb()` stays for screenshots, which draw the hardware
  cursor in. `tests/test_video.c` checks the conversion against
  `video_to_rgb()` at every depth, the bands, and the cursor calls with a
  stand-in Video Services Library. Checked in 8.5 and 9.1 from their CDs
  and 8.6 from a disk: the arrow, the I-beam (all inverting pixels), the
  watch (animated, with inverting pixels), hiding while typing; with SDL's
  software renderer too (inverting pixels are black there). Instruction-time
  runs are unchanged. Measurements in Findings. Open: presenting on a
  thread of its own; tracking framebuffer writes instead of comparing.

### 16. CD audio

- **Status:** done (8.5; 9.1 has no installed image here to try
  AppleCD Audio Player on). CD images with audio as cue sheets
  (`src/cdimage.c`: `.bin` in raw, 2048- or 2336-byte sectors, `MOTOROLA`,
  WAVE and AIFF tracks, `INDEX 00` and `PREGAP`/`POSTGAP`, sessions; the
  table of contents, the data track's 2048 bytes a sector as a
  `disk_backend`, raw sectors, the Q and P-W subchannel; `syshm disk toc`
  lists it). The drive's player (`src/cdaudio.c`): ranges or the driver's
  play lists, pause, stop, scan, volume, channel routing, its position a
  function of guest time; frames in a FIFO taken by the sound chip's analog
  input (`struct awacs_input` in `src/awacs.c`), mixed with the DMA's or
  alone, resampled to the codec's rate, through when the codec selects the
  CD with loop-through (or nothing). Every CD-ROM drive answers as Apple's
  CD-ROM driver (`src/cdrom.c`, after Technical Note DV22: ReadTOC 1-6,
  ReadTheQSubcode, ReadHeader, AudioTrackSearch, AudioPlay, AudioPause,
  AudioStop, AudioStatus, AudioScan, AudioControl, ReadMCN, ReadISRC,
  ReadAudioVolume, spindle speed, ReadAudio, the play lists 122-126;
  GetDriveType, GetBlockSize, ReturnDeviceIdent, GetCDFeatures,
  Get2KOffset), named `.AppleCD`, with DriverGestalt `devt` `cdrm`,
  `intf`, `dvrf`, `minf`, and a CD and CD-ROM drive icon (Control 21/22,
  `mics`/`dics`/`mnam`; drawn in `src/cdrom.c` with the floppy's palette).
  Tests: `tests/test_cd.c` (synthetic discs: an HFS data track made with
  `hfs_format()` read back with hfsread, audio tracks of patterns in each
  kind of file; the player's positions, pauses, lists, volume, pacing by
  guest time, resampling, through the codec), `tests/test_awacs.c` (the
  analog input with and without DMA), `tests/test_drives.c` (the driver
  calls on a cue sheet's disc). Checked on an installed 8.5 with the real
  Quake CD (`Quake [1996].cue`): the ISO 9660 data track mounts
  (QUAKE106), AppleCD Audio Player lists the 10 audio tracks with their
  lengths and plays, shuffles and scans them; `SYSHM_SOUND_WAV` holds
  track 2's samples bit for bit (scaled by Mac OS's volume), in
  instruction time (reproducibly) and in real time; a synthetic audio disc
  mounts as "Audio CD 1", and MoviePlayer's conversion of a track to AIFF
  holds its samples bit for bit. Instruction-time runs of the 8.5 and 9.1
  CDs reach the Finder as before, but for the CD icon (on the desktop and
  in the window's title). Open: see future.md, "More CD formats".

### 17. The display's VBL interrupt

- **Status:** done. The display interrupts at each vertical blank, as the
  iMac's ATI chip does, instead of the driver's Driver Services timer
  (which ran on the Time Manager, so stopped with it). The chip
  (`src/video.c`: `video_set_time()`, `video_next_vbl()`, `video_irq()`)
  counts vertical blanks on the timebase, n at n/60 s rounded up, so 60
  a guest second exactly and the same in every run; each latches its VBL
  status, which holds the display's line (source 22, level 2: `bootinfo.h`,
  `bootinfo_level_masks()`) while the interrupt is on, until the driver
  acknowledges it. The kernel brings it up to date with the other devices
  (`update_device_time()`, `device_next_tb()`, `update_interrupts()`, and
  `idle_wait()` wakes for it in real time). The display node has
  `interrupts` and an interrupt set (`interrupt_set()` in `bootinfo.c`, with
  `AAPL,interrupt-index` for 9.2's ROM files). The driver
  (`video_driver_call()`, `open_step()`) at Open: RegistryPropertyGet of
  `driver-ist`, GetInterruptFunctions, InstallInterruptFunctions with its
  handler (entry 1) and the set's refCon, enabler and disabler,
  VSLNewInterruptService, the chip's interrupt on and the set's enabler;
  without an interrupt set, no VBL service. The handler acknowledges the
  vertical blank (kIsrIsNotComplete if there was none) and queues the
  secondary handler (entry 2) unless it's queued already; that calls
  VSLDoInterruptService. SetInterrupt (Control 7) turns the chip's
  interrupt off and on (a vertical blank from while it was off doesn't
  count). The timer path is gone: every system here has interrupt sets.
  With more level-2 interrupts, latched DMA pulses got lost (see Findings,
  "Pulses lost at PrioritizeInterrupts"): `macio_release()` keeps one Mac
  OS hasn't handled for one more re-evaluation. Tests: `tests/test_video.c`
  (the timing: 60 in a guest second, 276,666 or 276,667 ticks apart, one
  interrupt for several unacknowledged; the driver's Open with and without
  a set, the handler, the secondary handler, SetInterrupt),
  `tests/test_nvram.c` (the display node's properties),
  `tests/test_dbdma.c` (pulses kept and released). Checked (instruction
  time, against main): the 8.5 CD, installed 8.5 and 8.6, the 9.1 and
  9.2.2 CDs reach the Finder as before, two runs identical but for the JIT's
  links; VBLs 59 a guest second (14,298 in the 8.5 CD's 4 billion
  instructions, 241 s; main's timer: 13,750), fewer where Mac OS keeps
  interrupts masked longer than a frame (Unreal Tournament: 52); the restored
  9.2.2 reaches the Setup Assistant over the Finder in all of 6 runs with
  different `--fixed-time`s (main: stuck in 3, stalled 17-25 s in the
  others); Unreal Tournament (8.6, with its CD) plays its intro with sound
  (main, merged: black screen, sound stopped, waiting for it); the hardware
  cursor follows the pointer in window runs (8.5 and 9.2.2 CDs); fast start
  in real time as main's (8.5 CD 13.8 s of guest time, 9.1 16.2 s, 9.2.2
  13.1 s, 8.6 installed 18.5-18.7 s); host time the same within noise.
- **Open:** on the restored 9.2.2's first startup (a disk Apple Software
  Restore left unmounted), quitting the Setup Assistant leaves the desktop
  without a menu bar, idle (with the VBL interrupt kept off too, and with
  the old timer); quitting it on a cleanly unmounted restore works.

## Findings

Facts later tasks need, added as they are found.

- **CD audio, Apple's CD-ROM driver** (Technical Note DV22, "CD-ROM Driver
  Calls", developer.apple.com/library/archive/technotes/dv/dv_22.html):
  csParam layouts as in the note; addresses are a type word and a long (0
  LBA, 1 MSF `0x00MMSSFF` in BCD, 2 a track in BCD, 3 a play-list entry in
  BCD, from 0); a track number as a stop address means the end of that
  track (AppleCD Audio Player stops "at" the last track, 0x11 on the Quake
  CD; AudioStop with track 0, which it sends at launch, means none);
  AudioStop with an address sets the stop for the next play too (the
  Control Strip's module sets it, then plays). DriveStatus's disc-in-place
  byte is 1 with a disc, as the note has it, whatever the drive queue's
  flags say (AppleCD Audio Player polls DriveStatus with drive 0 and wants
  exactly 1; a fixed drive's flags say 8). ReadAudio returns samples
  little-endian, as drives do; QuickTime's import reads 75 sectors and
  moves on 68 (its jitter correction finds the overlap by content, so a
  periodic test tone fools it: test with aperiodic audio).
- **CD audio, who asks what (8.5):** AppleCD Audio Player 2.2 walks the unit
  table from unit 32; for drivers with `dCtlFlags` bit 2 (DriverGestalt)
  it asks `devt` (`cdrm` or `dvdr`) and ReturnDeviceIdent (120), for others
  it compares the DRVR name with `.AppleCD`; its parameter block's
  ioVRefNum is 0. It numbers a mixed disc's audio tracks from 1. The
  Control Strip's CD module (CDStrip) asks `devt` of every DriverGestalt
  driver (drive 0) about every second, `intf` (`atpi` or `scsi`, then 120
  for "SCSI ID n"), reads the table of contents at startup and, with its
  AutoPlay, plays from the first audio track when a disc mounts, with the
  table it read at startup. Foreign File Access tries its file systems
  when MountVol fails (reads of an audio-only disc must succeed: with
  ioErr it gives up and the Finder offers to initialize); Audio CD Access
  4.0 then reads ReadTOC 5, 3, 1, 2, 3 and mounts "Audio CD n" with
  'trak'/'aucd' documents. ISO 9660 File Access reads 2048-byte sectors at
  32768. The Device Manager shows a native driver's name at
  `dCtlDriver + 18` (unit 49 `.AppleCD`), so `OpenDriver(".AppleCD")`
  finds it; Apple CD/DVD Driver 1.0.1 (68k DRVRs `.AppleCD` for SCSI and
  ATAPI) finds no drives of its own and leaves it alone.
- **CD audio, the codec:** Mac OS 8.5's sound driver writes register 0 =
  0x233 (input: the CD, gain 3) and register 1 = 0x8C0 (loop-through on,
  the speaker muted); Mac OS 9.1 here writes 0x000 and 0x880 (no input
  selected, no loop-through: its input component isn't loaded, see Mac OS
  9), so the CD plays through when nothing is selected too. Output
  attenuation 0x249 (9 steps, 13851/65536) applies to the CD as to DMA
  sound. In instruction time CD audio costs about 1 s per 8 billion
  instructions with 385 s of it playing (an installed 8.5 boot: 4.85 s
  against 3.83 s); the player reads 16 sectors at a time.

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
- **Quiet writes** (Mac OS 8.5's own ROM file): ConfigInfo maps the ROM
  (BAT lower `0xFFCF3253`, PMDT attributes 0x353; the 9.1 Trampoline's
  BAT too, WIMG 0b1010) write-through and PP 3, which NanoKernel 1 and 2
  treat as quiet: MemRetry skips a store to such a page and counts it in
  NanoKernelInfo.QuietWriteCount (+0xD0) (`MRDataStorageInt` in
  NanoKernel 1's `MRInts.s`, `MemRetryDSI` in `NKInterrupts.s`). The 8.5
  CD's ROM file (release 0x44F3) has a 'thng' resource in the ROM (-16600
  "DAV Audio Component", 'daud' 'dav ' 'appl', data at `0xFFDD4290`) that
  `MOS.rom` doesn't; the Component Manager registers it (the 'thng' loop at
  `0xFFC517E0`, then `0xFFC4F87E`) and copies its ComponentDescription onto
  itself: five stores at 68k pc `0xFFC4F8A6`, from instruction 321,370,799
  of a `--fixed-time` CD boot. syshm faulted on the first (exception 21, a
  68k bus error during Starting Up). `quiet_store()` in `nk.c` now runs
  such a store with the page writable and puts the page back, so the rest
  of the instruction (update forms, other pages) happens as under MemRetry.
- **The Start Manager's wait** (startup-wait): in real time with fast
  start the 9.1 CD took 41 s of guest time to the Finder (3.8 s on the
  host; the 8.5 CD 13.8 s), 25 of them in the Start Manager's
  FindStartupDevice (68k `0xFFC035EE`), spinning on Ticks 15 at a time
  (`0xFFC03680`). Until the internal disk's wait time has passed since it
  began (`_InternalWait` selector 0, PRAM's seconds, 25 when 0;
  `0xFFC03664`) it accepts only the drive whose number StartLib's
  GetStartupDevice gives (the test at `0xFFC03A7E`, the call at
  `0xFFC04190`), then any drive. 8.5's ROM (`0xFFC0295E`) has no such
  phase. GetStartupDevice walks the drive queue and takes the first drive
  that matches /chosen's `bootpath` (Open Firmware's: device path,
  `:partition`, then `,\\:tbxi`, the blessed folder's file of type tbxi):
  it resolves the device path in the Name Registry from
  `Devices:device-tree`, by node names and unit addresses (under a `pci`
  node `@dev[,fn]` is compared with `reg`'s first word; a node of type
  scsi ends the walk, the rest being the drive's), and asks the drive's
  driver DriverGestalt 'nmrg' (a pointer to its node's RegEntryID), 'dvrf'
  and 'intf' (its unit: SCSI's target ID, bits 8-15 of 'dvrf'; ATA's is 0;
  also USB and FireWire; other interfaces, a PCI card's 'card' among them,
  fail) and 'ofpt' (qualifier 2 and the map entry counted from 0, to which
  StartLib adds 1; any other qualifier: any partition). The drive matches
  when the node is the same, the bootpath's last unit address is the
  drive's unit, and the partitions agree. The driver needs the
  DriverGestalt bit in its DCE's flags, which NDRVs have. syshm's tree had
  no /chosen, so nothing matched until the 25 s were up. Now, for ROM
  files with parcels, `/chosen` has `bootpath`
  `/pci/syshm,disk@<PCI device>:<map entry>,\\:tbxi` (no number for a
  disk without a map) for the partition the ROM file came from if it has
  boot blocks ('LK'), else the first partition with them, disks in
  command-line order (`build_tree()` in `bootinfo.c`, chosen in
  `setup_machine()`), and the disk driver answers 'nmrg' (the RegEntryID
  from Initialize), 'intf' 'scsi', 'dvrf' target = its PCI device number,
  and 'ofpt' (`status()` in `disk.c`); earlier ROMs see none of it. A path
  ending in a unit-only node under the disk's (`/pci/syshm,disk@2/@0:3`,
  with 'intf' 'ata ') didn't match (the disk nodes have no children). The
  Finder is then up at instruction 1.05 billion instead of 1.68 (with
  `--fixed-time`), and in real time after 16 s of guest time (1.3 s). Of
  what's left (13 s with `clock 400` from the start, reproducibly; 8.5:
  10.5 s), 5 s are AppleTalk on Ethernet finding no one, as with 9.0.4
  (Open Transport busy: ten AARP probes 0.2 s apart, then six ZIP
  GetNetInfo 0.5 s apart, then NBP), and the CD's 18 MB of transfers take
  4.5 s at 4 MB/s. When the ROM file comes from a startup disk, its
  partition is the one named (if it has boot blocks).
- **BlockMove on the host isn't worth it.** InterfaceLib has BlockMove,
  BlockMoveData, BlockZero and their Uncached forms (`MOS.rom`
  `0xFFD2CCA0`-`0xFFD2D0EB`; the same code at the same code-section
  offsets in 8.5's and 8.6's ROM files; 9.1's at `0xFFCFFE74` and
  `0xFFD272BC`, where the first BlockMove call asks Gestalt `ppcf` for
  AltiVec and points the TOC's vectors at the G3 or AltiVec versions).
  BlockMoveData copies backwards when the destination is above the
  source; the cached forms clear each destination line with `dcbz` first,
  the Uncached ones don't (BlockZeroUncached stores with `stfd`); BlockMove
  above 12 bytes then calls FlushEmulatorCacheRange (about 80
  instructions). The 68k `_BlockMove` (A02E, A22E for -Data) never reaches
  them: its ROM routine (`0xFFC3DAF0`) is just F-line `$FC12` (flushes)
  or `$FC14`, which the emulator does natively (emulator `+0xA9AC`,
  `lswx`/`stswx` up to 20 bytes, a `dcbz` loop from 0x5F). Counted in the
  interpreter (calls, bytes, instructions until return): the 8.5 CD boot
  (4 billion) 88,000 calls, 22 MB, 0.34% of instructions (68k 0.17%); the
  9.1 CD boot (3 billion) 132,000 calls, 39 MB, 0.60%; Unreal Tournament
  starting from the 8.6 disk (31.5 billion) 11.7 million calls, 1.16%, of
  which 0.86% is 11.3 million BlockMoveData calls of about 1 byte; a
  Finder copy of 7 MB 0.1% (large file transfers go to the disk driver on
  the host). Calls over 256 bytes, the only ones a kernel exit could
  speed up, are 0.25-0.45%. Sampled host time in their translated blocks
  is 1.1% of the 9.1 boot (8 runs; single runs 1.0-2.4%), 1.3% of the 8.5
  boot and 1.9% of UT's startup (four fifths in the 1-byte copies);
  touching guest RAM in advance doesn't change it. The 68k routine's
  `lswx`/`stswx`, which the JIT hands to the interpreter, are 99% of the
  200,000-275,000 string instructions in a boot.
- **Quick `sc` in the engines:** 4.5 million of a 4-billion-instruction
  CD run's 12.6 million kernel exits were `sc`, 4 million of them Mixed
  Mode's r0 -1 (`0xFFCF8DB8`). The kernel's answer (`system_call()`)
  never counted the `sc` as retired, and an exit for it only services
  devices and Multiprocessing Services when they're due (or something
  touched the devices since the last update); `in_place()` gives the
  engines the answers and a time-base limit (the earlier of the next
  device event and `mp->next_tb`; 0 when devices are dirty, a switch,
  software interrupt or preemption waits, with `clock`, or before trap
  logging starts) for each main-loop run, so the run stays the same
  instruction for instruction. 0.56 million still exit because the devices
  were dirty (after each sync point and each real MP call), 0.07 million
  because the limit had passed. Kernel exits 11.66 → 8.33 million, host
  time 1.53 → 1.44 s.
- **Kernel calls 0 and 1 in place:** most of the rest were the 68k
  emulator's switches to native code and back (3.5 million
  RunAlternateContext, 5 of them with an interrupt pending, and 3.5
  million ReturnFromException, all but one with verdict 32-255; 8.5: 1.4
  and 1.4 million). The engines call the
  kernel's `kcall_in_place()` for `twi 31,r31,n` while the time base is
  below the same limit and the blue task runs; it makes the call with
  `kernel_call()` itself (the same code as at an exit) when it can't fail
  and there's no interrupt pending (RunAlternateContext's `r4 & cr`), and
  declines anything else (verdict 1, bad verdicts, unusable context
  blocks, other calls), which then exits as before. Generated code
  settles the lazy FPSCR first, and goes on through the block table, but
  not as a branch (the `_SynchIdleTime` hook doesn't stop there, as it
  doesn't after an exit). Counts in the end-of-run statistics are as
  before. 0.28 million still exit (devices dirty, or the limit passed).
  9.1: kernel exits 8.33 → 1.53 million, host time 1.43 → 1.32 s
  (main 1.50); 8.5: 2.83 → 1.11 million, 1.03 → 1.02 s.
- **Device registers in place:** a 4-billion-instruction 8.5 CD run made
  1.64 million device register accesses, each a DSI exit: 874,000 reads
  of the USB controller's HcFmNumber (`lwz` at `0xFFE22A4C`, MOS.rom's
  OHCI driver polling the frame number, from 700 million on), 720,000 of
  VIA1 (the Time Manager, through the 68k emulator's `lbz(u)x`/`stbx`:
  T2CL and T2CH read 163,000 and 161,000 times, written 80,000 and
  160,000, IFR read 117,000, IER 33,000, port B 23,000) and a few
  thousand of SCC, DBDMA and sound registers; once idle, 18,000 VIA and
  27,000 USB per 100 million instructions (instruction time makes those
  rates about 150 times real time's). 8.5 and 8.6 installed: 0.71 and
  0.77 million, nearly all VIA; 9.1's CD 17,000. On an M5 an access cost
  about 50 ns with its exit. The engines now offer a faulting load or
  store to the kernel's `device_hook()` (`cpu->mmio`), which does what
  the loop does at that exit (devices brought up to date as at any exit,
  then for the access; the access; interrupts; the next device event)
  and goes on, unless the loop would do more: interrupts changed (pending,
  posted or cleared), a preemption, software interrupt or task switch
  waiting, the MP tick due, a Cuda shutdown or restart. Then the run
  stops right after the access (`PPC_EXIT_MMIO`, 2% of accesses) and the
  loop does the rest, without the exit's own device update. Two things
  had to be exactly as at the exit: an access on a run's last
  instruction still exits, because after that exit the loop computed its
  next sync point from the count past it and skipped the one there (an
  old quirk, since fixed: see "MacBench's freeze"; done in place, the
  access would leave the sync point to be serviced, though the boots came
  out the same that way too); and
  `in_place()` runs again after each access, as before the loop's next
  run (without it, kernel calls in place ran past the next device event
  as the access had moved it, and the boot changed). Off with `clock`
  (each exit sets the time base). Generated code has the hook's path only
  for instructions whose fastmem fault was on an unmapped page (every
  checked access with fastmem off): with it for every checked access,
  code grew 6-10%. The bookkeeping was 30 ns an access; looking the
  region up and decoding once, no DBDMA poll with nothing to poll, no
  scan of interrupt levels with nothing pending, and inline line and
  pending helpers (VIA, USB, Mac I/O) brought it to 23 ns. 8.5 CD:
  device exits 1.64 million → 36,000, host time 1.00 → 0.96 s; 8.5
  installed 0.87 → 0.86 s, 8.6 1.24 → 1.21 s, 9.1 CD 1.04 → 1.03 s
  (medians of 9 alternating runs on a shared machine, ±1-2%).
- **Mac OS and a shared volume's MDB:** 8.5, 8.6 and 9.1 write the MDB
  with the unmounted attribute (`drAtrb` bit 8) clear as their first write
  after mounting a volume, before anything else, and with it set when they
  put it away (Shut Down, Restart, Put Away). A volume that was written
  but whose MDB doesn't say so wasn't put away (the run ended under it).
  The Finder makes `Desktop DB` and `Desktop DF` on every writable volume
  it mounts, and Desktop Printing (8.6) an invisible `DesktopPrinters DB`;
  the Trash and the `Desktop Folder` come when used.
- **Norton Utilities and the shared volume's dates:** Norton FileSaver
  (Norton Utilities 6, scanning every disk at startup after a crash) said
  "There is damage to the Master Directory Block. (Error 16,005)" of the
  shared volume, whose MDB's creation and modification dates were the
  folder's (2026) and so later than the guest's clock in instruction-time
  runs (`--fixed-time 1000000000`, 2001). Disk Doctor names it: "The
  creation date is invalid" (5,1,3), then the modification date (5,1,4),
  the alternate MDB's (5,2,3 and 5,2,4), and every file's "bad creation
  dates" (15,2,8, minor). Norton allows a minute or so in the future, not
  an hour (scanned with the guest's clock 60 s, 1 h, 1 day, 1 and 20 years
  behind the folder's dates; FileSaver's 16,005 is any MDB damage). It also
  takes a file modified before it was created for "bad modification dates"
  (15,2,9), as macOS's copies and unpacked files are (born anew, modified
  when they were): 316 files of a copy of the user's folder, with the
  guest's clock at the host's time. `share_open()` now takes the guest's
  clock at the start (`--fixed-time`, else the host's) and no date is
  later, nor created after modified. With that, FileSaver, Disk Doctor
  ("No Problems Found"), Disk First Aid 8.5 and Linux's `fsck.hfs` find
  nothing wrong, on that copy too, in instruction time and real time; but
  for the dates they found nothing before either. A Macintosh HD made later
  than such a run's clock is flagged the same way (`syshm disk create`
  dates it then). Mac OS 8.5's Disk Initialization of a volume the share's
  size gives the same geometry (63,569 blocks of 16,896 bytes from sector
  19, the alternate MDB right after the last), but 1 MB B-trees. FileSaver
  scans after a crash: a run ended by `--max` leaves an image as one.
  macOS's own `fsck_hfs` (hfs-753.41.1) can't check HFS volumes with
  files: it stops at a bounds check (`brk #0x5512`) in
  `hfs_swap_HFSBTInternalNode`, which swaps a file record's six extents
  through its three-entry `dataExtents`; it does the same on a Mac OS-made
  HD.
- **The catalog's order of names** is Apple's FastRelString (the HFS code
  in Apple's open-source `hfs`, `UCStringCompareData.h`'s `gCompareTable`):
  case-insensitive, each accented letter after its plain one, a
  non-breaking space equal to a space. hfsutils' `hfs_charorder` and
  Linux's `caseorder` order the same (checked for all 65536 pairs);
  `hfs.c` keeps the ranks. Linux's hfsprogs `fsck.hfs` (Apple's
  fsck_hfs 540) checks HFS volumes, map nodes and order included;
  hfsutils' `hfsck -n` fails to read any image, so the test runs it
  without `-n` on a copy.
- **Keys in input scripts:** in 9.1's Finder, Return held for 10 million
  instructions didn't start editing an icon's name; held for 1 million it
  does (8.5 took Command-D held 20 million).
- **Running code of syshm's in Mac OS on demand** (installing the guest
  tools): the Toolbox trap table is in low memory as on 68k Macs (entry
  `0x0E00 + 4 × (trap & 0x3FF)`), and the 68k emulator dispatches through
  it on every call, interpreted or recompiled. `_SynchIdleTime` (ABF7),
  where the idle hook stops, is called from a `_SystemTask` patch in the
  system heap (r24 points 8 bytes into `_SystemTask`'s entry, on 8.5 and
  8.6): application time, in the current application's context (often
  the Finder's), inside its WaitNextEvent. At that hook syshm points the
  entry at 68k code of its own, in 64 KB it maps at `0x5E000000`, outside
  everything Mac OS knows of; the dispatch under way goes there. The code
  puts the entry back first, saves d0-d7/a0-a6, and calls
  `_CodeFragmentDispatch` (AA5A, a routine descriptor to the native CFM;
  selector 3 GetMemFragment, 4 CloseConnection, pushed as a word, Pascal
  conventions) on a PEF container made like the drivers, with
  `kPrivateCFragCopy`: its InterfaceLib imports resolve, and memory outside
  RAM is fine for the container. Its main routine is called through a
  routine descriptor (procInfo 0, ISA 1, the TVector from GetMemFragment
  written in by the 68k code) by a plain `jsr`, so Mixed Mode switches
  to native code as for any 68k caller, and back for each File Manager
  trap. Then CloseConnection, the registers, and a `jmp` to the trap's
  own routine. FindFolder, PBHGetVInfoSync, PBGetCatInfoSync, the FSp
  calls, FSWrite and FSClose all work there, and so does
  LaunchApplication (`launchContinue | launchNoFileFlags |
  launchDontSwitch`) of a background-only application: Enhancements runs
  at once (its launches count 2 in the run's statistics). Disk First Aid
  finds the volume sound afterwards. (hfsutils' `hdel` on an installed
  8.6 disk leaves something it can't repair; `hattrib` doesn't.)
- **A running Enhancements can't be replaced** (its resource fork is open:
  `fBsyErr`), so syshm has it quit first (`helper.quit`: its main
  routine returns at its next WaitNextEvent, as for a quit Apple event)
  and goes on once it's gone (0.1 s later; 5 s at most), and starts it
  again if it couldn't be replaced after all.
- **RAVE finds engines per program:** QuickDraw 3D RAVE looks through the
  Extensions folder when a program loads it, so programs opened after the
  3D engine is installed find it without a restart, even after an earlier
  program loaded RAVE (SimpleText opening the Teapot from the QuickDraw 3D
  folder, quit, the engine installed, opened again: 19 calls to the
  engine). Programs already running don't.
- **Mac OS 9 doesn't call `_SynchIdleTime`** (9.1 and 9.2.2 alike): its
  Process Manager idles natively, napping with `MPDelayUntil` (call 55),
  and its idle routine (68k, in the system heap) calls trap AAAA with
  $82F0 in D0.W where 8.x's `_SystemTask` patch called ABF7 (both then go
  on to the ROM's idle code, which calls A0DD and `_PowerDispatch` A09F
  with D0 3). ABF7's entry is a native routine nobody calls at idle, so
  the idle hook never stopped there, and the offer never came on 9.2.2:
  the installer only ever looked for the Finder and went in at that hook.
  Now the Multiprocessing naps are idle calls for the installer too
  (`tools_idle()` in `nk.c`, from `kcall_mp_dispatch()` and PowerDispatch's
  nap), the Finder is noticed between idle calls as well (at idle calls
  9.2.2's current application is as often a background one: "DVD
  AutoLauncher"), and once the answer is yes on a system whose
  `_SynchIdleTime` the hook hasn't seen, the hook watches AAAA instead
  (`watch_trap()`; the JIT translates again for it) and the installer goes
  in at its dispatch with $82F0 in D0, in the same idle context. The
  bootstrap, GetMemFragment (AA5A is there too), Mixed Mode, the File
  Manager on HFS+, LaunchApplication and the memory at `0x5E000000`
  (free in 9.2.2's map too) needed nothing new. On 9.2.2 (MacOS9Lives),
  Enhancements switched modes for a `resize` (1024x768), passed text both
  ways (a paste in SimpleText of "Hello from the host – Olé", and its
  copy back), and QuickDraw 3D 1.6's viewer drew the Teapot through the
  engine (one context, 5,452 triangles), also in the session it was
  installed in. 9.1's CD idles the same way (AAAA, $82F0); its system
  won't run from a writable copy, but offered (a test build) on the CD
  itself the installer went in, prepared, and stopped at Mac OS's own
  check of the volume: locked.
- **Media changes** (the floppy drive): Mac OS learns of a disk in a
  drive from the drive's flags, the 4 bytes before its drive queue
  element (byte 1: 0 none, 1 or 2 an ejectable disk in place, 8 a
  non-ejectable one; byte 0 bit 7 locked), and from a disk-inserted event
  (`PostEvent(diskEvt, drive)`), which GetNextEvent/WaitNextEvent turn
  into a MountVol for whatever application is in front (Disk Copy, in
  front, offered to image a floppy put in). The driver's Driver Services
  timer, already there to post the disks' events, keeps running once a
  second for the floppy drive: it sets the flags from the host's state and
  posts the event, as the Sony driver's VBL task did, from secondary
  interrupt level. Taking a disk out is Mac OS's (Control 7, Eject, after
  UnmountVol or with the volume left off line): the driver closes the
  image and clears the flags. Put Away, the Finder's Eject (which unmounts
  in 8.5 and 9.1: no grey icon is left) and an Initialize dialog's Eject
  all come there; a non-ejectable drive (flags 8) gets no call at all.
  Whether Mac OS has a volume on the drive is in its VCB queue (`VCBQHdr`
  at `0x356`, `vcbDrvNum` at +72, 0 off line). The startup disk could be
  taken from any drive with a disk in it, so the drive says it's empty
  until an application has run: `CurApName` (`0x910`) holds a name of 1-31
  characters (8.5 leaves 0xFF bytes there during startup, so non-zero
  isn't enough). Mac OS asks for the drive's icons (Control 21 and 22,
  DriverGestalt 'mics', 'dics': 'icns' data), its 'devt' ('flop'), 'minf',
  'flus' (it then sends DriverConfigure 'flus' at FlushVol: the DC42
  checksums' moment), the format list (Status 6) and Control 23 on mount
  and for Erase Disk (Format, then Verify, then it writes a new volume),
  and 'ejec' at restart (kRestartDontEject keeps the disk in). Disk Copy
  6.3.1 can't make floppies on it (the 8.5 CD's "Make Disk Tools
  floppies" says it needs a drive that can make 720K/1440K disks, without
  a call to the driver: it looks for other drives, the Sony driver's
  presumably), though it offers to make an image of one put in.
- **Ejectable drives** (CD swapping): the flags' "ejectable" (1) rather
  than "non-ejectable" (8) changes what Mac OS does from early on: with
  only that changed, the 8.5 CD's 4-billion-instruction run ended
  elsewhere (its screen the same) and 9.1's screen differed, as Mac OS
  reads the flags as it runs; set only once the CD was mounted, the runs
  still diverged. With flags 8, the Finder's Eject is disabled and Put
  Away asks "Do you really want to remove the disk ... It will reappear on
  the desktop when the computer is restarted", then unmounts it without a
  call to the driver (only DriverGestalt 'flus' queries), so nothing could
  tell the driver to release the image or start its timer again (it stops
  once its volumes are mounted). Hence CD-ROM drives are ejectable only
  where discs can be swapped. With flags 1, Put Away, the Trash and the
  Finder's Eject unmount the volume and call Control 7; the Finder refuses
  for the startup disk. What 8.5 asks of an ejectable CD drive is what it
  asks of the disks (DriverGestalt 'flus', 'intf', 'minf', 'mics', Control
  20, 21, 22, 23 at mount), all declined as before, so a CD keeps its
  look: the generic disk's icon or the disc's own, the same in at startup
  or put in later. 'ejec' (kRestartDontEject) keeps the startup CD in at a
  restart.
- **Each application has its own scrap:** its scrap handle is in its
  heap, and the low-memory scrap globals (`0x960`) are switched with it;
  the Process Manager hands the front application's scrap to the next one
  brought to the front. A background application never sees another's
  copy: after SimpleText's Copy on the 8.5 CD, syshm Enhancements'
  InfoScrap count and size were as before. So Enhancements can't poll the
  scrap: it sees copies in PutScrap, and puts the host's text into the
  front application's scrap in that application's context.
- **An application's patches are its own:** NSetTrapAddress from an
  application patched the trap only while that application ran (the
  Process Manager swapped the trap table entry at every switch, even with
  the routine descriptor in the system heap). Written into the trap table
  (`0x0E00` + 4 × the Toolbox trap's number), as an extension's patch is
  at startup, it's everyone's and stays. InterfaceLib's glue for the Scrap
  Manager reads the trap table at each call (`lwz r0,0x15F4(0)`, then
  CallUniversalProc), so PowerPC applications go through it too.
  `jGNEFilter` (`0x29A`) isn't switched either.
- **Applications that don't ask:** Note Pad (68k) pastes from TextEdit's
  private scrap, which it fills from the desk scrap on a resume event with
  convertClipboard; at a paste it only asks `GetScrap(nil, 'TEXT')` (the
  length). Scrapbook (68k) makes no Scrap Manager call after it starts: it
  reads the scrap's size in low memory to enable Paste. Hence the filter
  (the host's text is in the front application's scrap before its next
  event) and the posted resume event (an `osEvt` with
  suspendResumeMessage, resumeFlag and convertClipboardFlag, which
  PostEvent puts in the queue, ahead of keys typed after it). Patching
  WaitNextEvent and GetNextEvent instead, calling the Process Manager's
  (which switches processes inside them) through CallUniversalProc, left
  applications without their windows.
- **Pictures (not done):** Mac OS to host, the scrap's `PICT` is QuickDraw
  drawing: simplest to have Mac OS draw it (DrawPicture into a GWorld,
  from Enhancements) and hand the host the pixels. Host to Mac OS: decode
  PNG (needs an inflater: zlib, or stb_image in third_party) and make a
  PICT of one DirectBitsRect (32-bit pixels), mind the 4 MB limit. The host
  side is the bigger part: SDL2's clipboard is text only. SDL3's
  (`SDL_SetClipboardData` and MIME types) would do X11, Wayland and macOS
  alike; without it, NSPasteboard through the Objective-C runtime on
  macOS, and on Linux the X11 CLIPBOARD selection (an `image/png` target,
  answering SelectionRequest events) and a Wayland data source, each
  separately.
- **Showing the screen** (Task 15; M5, window runs, the pointer moved in
  circles by a timed script, 50 moves a second; per presented frame, and
  the share of the emulation thread's time in `display_frame()`; medians
  of 3-5 runs, their range in brackets. The machine was shared and the
  thread's core and clock vary: the same run gave 1.4% and 7.5%.)
  At 800x600, 8 bits (the 8.5 CD): main showed 45 frames a second,
  converting each in 0.57 ms [0.26-1.01], uploading it in 0.62 [0.28-1.03]
  and drawing in 0.21, 6.5% [2.7-11.0] of the thread; showing changed rows
  only, 44 a second of 31 rows, 0.05, 0.08 and 0.14 ms, 1.2% [0.7-2.8];
  with the hardware cursor nothing in the framebuffer changes: 46 draws a
  second at 0.34 ms and a whole frame a second, 2.0% [0.5-2.6]. At
  1920x1080, 8 bits (8.6 from a disk): 48 a second at 1.82, 1.68 and 0.18
  ms, 17.8% [10.7-19.0]; changed rows, 46 a second of 44 rows at 0.27,
  0.13 and 0.25 ms, 3.2% [1.2-6.0]; the hardware cursor, 48 draws a second
  at 0.34 ms, 3.3% [2.2-3.9]. At 1920x1080, 32 bits: 47 a second at 0.71,
  1.32 and 0.17 ms, 10.8% [7.8-17.2]; changed rows, 1.06 (nearly all of it
  comparing), 0.18 and 0.34 ms, 8.7% [1.9-10.2]; the hardware cursor, 46
  draws a second at 0.14 ms, 2.0% [1.8-2.1].
  On the 8.6 desktop at 1920x1080, 32 bits: idle (the menu-bar clock
  changes once a minute), main 3.7% [1.7-7.3], now 3.3% [3.1-7.1]; typing
  in Sherlock's field, 10 keys a second (7 frames a second change),
  main 0.67 + 1.74 ms a frame and 3.2% [2.3-10.2], now 12 rows a frame,
  0.33 + 0.08 ms and 1.7% [1.3-7.5]. Pixels converted a second while the
  pointer moves: 21.6 million at 800x600 before, 1.1 million in bands, 0.5
  million with the hardware cursor (the frame a second); 99.5, 3.9 and 2.1
  million at 1920x1080. What's left is mostly comparing the framebuffer
  with the copy each frame (2 x 8 MB at 32 bits: 0.23 ms in a cold-cache
  benchmark, up to 1 ms in the emulator), which main did too for frames
  that hadn't changed; row by row costs what one memcmp does. Standalone,
  converting 1920x1080 takes 0.13 ms at 32 bits (a byte swap), 0.37 ms at
  16 (shifts and masks: a 32K-entry table took 0.56), 0.39 ms at 8 (a
  table of 256); in the emulator a whole frame takes 0.7-2.3 ms with the
  copy and the first writes to the buffer SDL locks (Metal's renderer
  allocates one for each lock). SDL's renderers take ARGB8888 as it is
  (Metal's, here, lists it first; so do OpenGL's and the software one);
  RGB24 was converted again by SDL on the CPU.
- **The hardware cursor** (Task 15; Designing PCI Cards and Drivers, the
  Display Device Driver Guide; Apple's IOGraphics, whose NDRV support
  implements the same library on Mac OS X). Control 22 SetHardwareCursor
  (VDSetHardwareCursorRec: csCursorRef, two reserved words), Control 23
  DrawHardwareCursor (csCursorX, csCursorY, csCursorVisible, two
  reserved); Status 22 SupportsHardwareCursor (csSupportsHardwareCursor),
  Status 23 GetHardwareCursorDrawState (x, y, visible, set). Mac OS 8.5 to
  9.1 call SetHardwareCursor with each new cursor, immediate, without
  asking Status 22 first (never called in these runs); before, it failed
  (controlErr) and Mac OS drew the cursor itself, as it still does in
  headless and instruction-time runs. Once one is taken, DrawHardwareCursor
  comes from the cursor's VBL task as the pointer moves, asynchronous
  (kind 2: completed with IOCommandIsComplete); x and y are the image's top
  left, the hot spot already taken off (an arrow at 100,100 comes as
  99,99), so the window needs no hot spot. The same arrow is handed over
  again and again (21 times as the 8.5 CD starts). csCursorRef points at a
  CursorImageRec (version 1.0, a PixMapHandle and a BitMapHandle: the
  cursor's 1- or 8-bit 16x16 pixmap and its mask), opaque to drivers:
  `VSLPrepareCursorForHardwareCursor(cursorRef, &descriptor, &info)`
  converts it. HardwareCursorDescriptorRec (version 1.0): height, width,
  bitDepth, maskBitDepth, numColors, colorEncodings (each colour's pixel
  value), flags, supportedSpecialEncodings (1 transparent, 4 inverting),
  specialEncodings[16] (their values). HardwareCursorInfoRec (version 1.0):
  cursorHeight and cursorWidth (filled in), colorMap (a ColorTable of the
  driver's: the library fills the ColorSpecs after its 8-byte header, not
  the header) and hardwareCursor (the driver's buffer, rows of the
  descriptor's width). The library in 8.5-9.1 converts to 2, 4 and 8 bits
  a pixel; 1 and 32 (direct) fail. syshm asks for 8: 254 colours (the
  value is the colour's index), 254 transparent, 255 inverting. The
  I-beam is all inverting pixels; the watch has two (its crown) and
  animates through SetHardwareCursor; ObscureCursor (typing) hides it
  with DrawHardwareCursor. With the library's import, the descriptor, the
  colour table and the buffer the driver is 3.8 KB bigger, so it's built
  so only for window runs in real time. (SheepShaver reads the
  CursorImage's pixmap itself, 1-bit cursors only, and guesses the hot
  spot.)
- **Mac OS 9.2.2: the CD** (Task 10): MacOS9Lives' "Mac OS 9.2.2
  Universal" (`922.iso`, volume MacOS9Lives in the partition "Toast 5.2 HFS
  Optimizer"), for Macs Apple's 9.2.2 doesn't support. Its ROM file is
  10.2f1 (its Read Me: "Modified Mac ROM 10.2.1MR"), `<COMPATIBLE>` MacRISC
  to MacRISC4 (no model check stops it), Toolbox release 0x45F6, 31 parcels
  (no MotherBoardHAL among CodePrepare's; AGPLib and EtherPrintfLib in
  CodeRegister; drivers for `bmac+`, KeyLargo, Kauai, UniNorth...), and
  NanoKernel 2.28. Under QEMU (`tools/qemu-ref/run91.sh 922.iso`, `mac99`)
  it reaches the Finder with its Read Me open in SimpleText within a
  minute. Its installer is Apple Software Restore 2.2.5, which restores
  `MacOS9LivesMR.img` from the CD onto a volume (erasing it, as Mac OS
  Extended); Drive Setup opens too and lists no drives.
- **NanoKernel 2.28's info records** (QEMU, 9.2.2 at the Finder): the
  InfoRecord page (`0x5FFFE000`) is a page of its own (10 pages below the
  KDP), no longer the KDP. ProcessorState `0x68FFDB30` (PSA) 0x0100/0x80;
  HWInfo `0x5FFFEF00` 0x0108/0xC0; ProcessorInfo `0x68FFEF20` (KDP)
  0x0112/0xA0; NanoKernelInfo `0x68FFEDC0` 0x0228/0x160; DiagInfo
  `0x68FFDA30` 0x0100/0x100; SystemInfo `0x5FFFEDC0` 0x0107/0x140. HWInfo's
  0x0108 fields, from the Trampoline (`rom trampoline 922.iso`): +0x9C a
  halfword, 0x41 with an OpenPIC (which also gets spurious vector 0x40 at
  +0x74; nothing without one); +0xA0 the device tree's checksum over the
  `psum` parcel's properties (0xDA424865 there); +0xA8 CascadeInfo, a
  pointer to a 0x204-byte table the Trampoline copies into the ConfigInfo
  page (`0x68FEFCFC`): the count of interrupt controllers cascaded from the
  OpenPIC first (0 on QEMU), their entries from +0x148 (0x20 bytes each).
  SystemInfo 0x0107 adds bytes from NVRAM at +0x128 and +0x129 (Mac OS
  offset +0xAF, the RAM disk's size, and +0xB1) and a long at +0x12A for the
  RAM disk. ProcessorInfo 0x0112 adds ClockRates at +0x60 (CPU, bus,
  decrementer Hz; the kernel copies them from +0x04-+0x0C; call 108 sets
  other steps). For ROM files with parcels syshm now gives those versions
  and lengths, still in its one page (HWInfo at +0xE60, ProcessorInfo at
  +0xF20, the rest where NanoKernel 1's are), CascadeInfo with no
  controllers (boot-info area +0x51100), the checksum and RAM disk fields 0,
  and the clock rates. The 9.1 CD's run is unchanged by it.
- **What 9.2.2's ROM reads there:** its 68k startup (`0xFFC0F0AC`, the
  only new use of NKHWInfoPtr) does `movea.l ([$68FFEFD0],$A8),a1; tst.l
  (a1)`, and with a count patches handlers from a table ProductInfo +0x50
  leads to. With syshm's HWInfo 0x0103/0xA0, +0xA8 was SystemInfo's
  logical RAM size, read as a pointer: exception 18 at the end of RAM at
  instruction 954,597. A System fragment checks ProcessorInfo's version
  (at least 0x0111) before starting 'accu' (below).
- **Calls 9.2.2 makes that 9.1 doesn't** (QEMU, `-d int`, 75 s): 62
  `MPGetNextCpuID` (from MPLibrary, `0xFFCD6ED0`: coherence group r3, 0 the
  current processor's; previous CPU r4 → next r4; kMPInvalidIDErr after the
  last, r4 left), 132 `NKSetClockDriftCorrection` (parts per billion r3; 0
  turns it off: then nothing but noErr; else the kernel's drift timer steps
  the timebase by one tick every so often; kMPInsufficientResourcesErr
  without that timer or with more than one processor scheduled. syshm's
  timebase keeps time: noErr), 95 `MPFindVictimPage` (86 times: QEMU's free
  page count went low; syshm's doesn't), and `sc` -3 (16,815 times, all
  from the kernel's own code at physical `0x00F15920`). No 11
  (`MPIsTaskBlue`): Mixed Mode's `sc` -1 does it. Later, idle, 44 and 45.
- **The startup objects** (`NKInit.s`, and QEMU's index): process 1,
  coherence group 0x00010001, CPU 0x00020001 (Flags 0xF: scheduled, and
  bit 31, the processor the kernel started on), address space 0x00030001,
  page queue 0x00040001, blue task 0x00050001, then semaphores, the idle
  task, another address space and the areas. syshm now makes the first six
  so (`mp_init()`); MPRegisterCpuPlugin takes the group's ID as well as 0.
  `MPStopScheduling` (45) on the starting processor is
  kMPInsufficientResourcesErr; `MPStartScheduling` (44) on a scheduled one
  noErr. The 9.1 CD's run to 4 billion is the same with the new IDs (its
  screen and every count but the JIT's links).
- **'accu', the clock's drift** (9.2.2): a PEF fragment the System loads
  (with a 68k loader; imports NameRegistryLib and MPLibrary) calls
  `NKSetClockDriftCorrection(0)`, checks ProcessorInfo's version, finds the
  Name Registry node named `mac-io` and its `AAPL,address`, and starts task
  'accu'. Every 120 s (an `MPWaitOnSemaphore` timeout; the semaphore starts
  at 1, so the first pass is at once) it reads the timebase and the 64-bit
  counter at mac-io +0x15038 (`lwbrx`, KeyLargo's timer), and when the
  counter has moved, sets a correction in ppb against ProcessorInfo's
  decrementer rate; else it sets 0. On an iMac's Paddington that address is
  in the floppy controller's (SWIM3) cell. syshm had nothing there: the
  task faulted (exception 18 in a task: Task 4). The cell now reads zeros
  (`setup_machine()`, `empty_cell_ops`, for ROM files with parcels).
- **The ROM file's lanLib for `bmac+`** (9.2.2's parcel flags 0xC, blob
  flags 6, not keep-existing) took the place of syshm's in the Ethernet
  node, and its first store to the BMAC registers (Mac I/O +0x11180) made
  the Finder quit. `build_tree()` puts syshm's back after the parcels.
- **The power manager when idle** (9.2.2's CD): after half an hour of guest
  time without input, PowerDispatch 9 once, then PowerDispatch 1 (doze
  until an interrupt) in a loop, with CacheDispatch (kernel call 7) and r3
  = 4 now and then. Both NanoKernels (`MOS.rom` `0xFFF15F78`, 10.2f1
  `0xFFF1A8CC`) answer a selector above 2 with -2 in the low half, the high
  half being the caller's r23's (their return path ANDs it in); syshm does
  the same, and still stops for 0-2 (the caches; not seen). At 55 minutes
  the 'powr' task calls 62, 45 (refused), 62, 44, as around a sleep; no
  PowerDispatch 8 comes, and the system carries on (input brings it back
  from dozing). 9.1's CD doesn't doze in 70 minutes.
- **The restored 9.2.2 waits for a key, in instruction time:** started from
  the disk Apple Software Restore made, it shows "Mac OS 9.2" and the
  desktop by instruction 2.1 billion, then no 68k code runs: the blue task
  stays in native code (MPLibrary `0xFFCD696C`), napping with call 55, the
  menu-bar clock stopped and the watch cursor up. A key (or Command held
  from startup) and it goes on to the Finder; in real time it starts up
  normally (13.5 s of guest time). The last level-1 software interrupt
  (Time Manager) was posted at a sync point while the emulator ran (68k mask
  0, `0x6806E44C`, in its path that saves the context block on the stack
  around a call and restores it, `0x6806E2A0`-`0x6806E454`); the emulator
  yielded (call 13) and went on into native code without taking it. ECB
  +0xD0 stays 1 and the table's level-1 entry stays taken, so later timers
  raise nothing (SetEvent's "already set"), the Time Manager stops, and the
  native code waits for time that never passes. RunAlternateContext's test
  mask (TestIntMaskInit 0x00200000, cr2.eq) doesn't stop it for a software
  interrupt (cr2.lt), on NanoKernel 2 too, and NanoKernel 2 delivers the
  same way (SetEvent flags the blue task; SchEval ORs the bits into the
  running emulator's CR). What differs is likely when timers fire (syshm:
  at sync points, every 20,000 instructions) and that a real iMac has a
  periodic external interrupt, the display's VBL, which would end the
  native code within 16 ms, while syshm's VBL is a Driver Services timer
  that itself needs the Time Manager. Options: a VBL interrupt from the
  display device (`video.c`, its node's interrupt set), or timers firing at
  their exact time in instruction time. **Resolved** by the first (Task
  17); see the next.
- **The display's interrupt** (Task 17): on the iMac the ATI chip's PCI
  interrupt goes to Paddington's source 22 (MAME's `imacg3.cpp` wires its
  Rage IIc there; its OHCI is source 28, as syshm's, and NetBSD on a Bondi
  iMac says the same of the OHCI; QEMU's g3beige wires Grackle's four PCI
  lines to Heathrow's 21-24). Real NDRVs install their handler in the
  node's `driver-ist` set and either call VSLDoInterruptService there
  (QemuMacDrivers' QemuVga, `USE_PCI_IRQ`) or from a secondary handler, as
  syshm's does; VSLDoInterruptService runs the screen's VBL tasks (the
  cursor's) and was called from a Driver Services timer's secondary
  handler before, so either level works. Mac OS 9.2's ROM file (10.2f1)
  has no InterruptTreeNew: DriverServicesLib builds the sets itself
  (`0xFFD860F4`: a RegistryEntrySearch for `AAPL,interrupt-priorities`,
  then four RegistryPropertyGets, each failing the node:
  `AAPL,interrupt-priorities`, `-vectors`, `-index`, `AAPL,interrupts`),
  one set per 68k level with members numbered in order, adding each to the
  node's `driver-ist` (`0xFFD85E24`). `AAPL,interrupt-index` is the
  source's entry in its table of 32-byte entries (above 0x3F: a cascaded
  controller's, from CascadeInfo; the real Trampoline makes the property).
  Without it no node had a set on 9.2.2: the display's Open got
  nrNotFoundErr, USB's UIM never enables its interrupts, the sound DMA
  plug-in doesn't start (no codec writes) and lanLib polls. With it on all
  of them, 9.2.2 plays sound, but the CD's Finder came at 22 s of guest
  time instead of 13 in real time with fast start, and the host idled
  less, so only the display had it at first; see "9.2.2's interrupt sets"
  for why, and the rest.
- **The restored 9.2.2's hang, resolved:** reproduced with a restore whose
  run was stopped without quitting Apple Software Restore (its first
  startup then differs; a cleanly unmounted one didn't hang). On main, of
  6 runs to 8 billion with `--fixed-time` 1.0-1.5 billion, 3 stopped short
  of the Finder for good (a level-1 software interrupt never taken; 1679
  later raises dropped as "already set") and 3 stalled 17-25 s the same
  way before going on. With the vertical blank: none, the longest a
  software interrupt waited 18 ms, and all 6 reach the Setup Assistant (the
  restore's first startup). The same build with the chip's interrupt kept
  off stalls as main does, so it's the VBL that ends them: each one ends
  the native code the blue task naps in (exception 0), and the emulator,
  taking it, takes the waiting software interrupt too. A software
  interrupt dropped on purpose (its CR bits never posted) at 250 s of
  guest time waited 22.5 s without the VBL, under 18 ms with it; at 30 s,
  forever without.
- **Pulses lost at PrioritizeInterrupts** (Task 17): `macio_release()`
  dropped every taken pulse at each PrioritizeInterrupts, but Mac OS calls
  that not only once it has handled what it dispatched: also when it
  lowers its 68k interrupt mask, and at the end of a dispatch that began
  before the pulse came. A DMA pulse taken in between was dropped unseen.
  With the display's level-2 interrupts it happened often enough that
  Unreal Tournament's sound DMA (8.6, its CD) stopped after 12 s, and the
  game waited for it in SndChannelStatus at a black screen (merged main
  does that too, by its timing). Mac OS 8.5-8.6's sound driver
  clears its pulse in the controller and masks and unmasks its source;
  Mac OS 9's interrupt sets mask and unmask. So a taken pulse now goes once
  it's been cleared or masked, or at the PrioritizeInterrupts after the one
  that found it unhandled. In that UT run a PrioritizeInterrupts found one
  not handled yet 4,435 times (of 44,568 sound interrupts), none stayed
  unhandled to the next, and the sound plays on (22.8 million frames by
  instruction 20 billion, against 0.57 million before it stopped).
- **The clock with `--fixed-time 1000000000`** reads an hour later on the
  9.2.2 CD than on 9.1's or 8.5's (12:50 PM against 11:50 AM): September
  2001, summer time in the northern hemisphere but not where this host is
  (Australia). The CD's Date & Time sets daylight-saving time itself; the
  restored system shows the host's local time like the others.
- **Gary Davidian's skeleton key** (`src/priv.c`, `priv.h`; ZD MacBench
  5.0): ResetSystem (kernel call 2, the 68k RESET) with r3 'Gary' and r4
  0x05051956 doesn't reset: both NanoKernels (1: `KCallResetSystem` in
  `SoftInts.s`; 2: `kcResetSystem` in `NKInterrupts.s`, which logs
  "Skeleton key inserted at") return to the caller (LR from the
  emulator's trap table, else the next instruction) with SRR1 = (MSR &
  ~r5) | r7, r7 the caller's. MacBench uses it once, right after its
  registration, from code it puts in RAM and runs in the System context:
  r5 0xC010 (EE PR DR off), r7 0; then `mtmsr` DR on, a subroutine that
  stores L2CR and HID0 at 0xA78 and 0xA7C, `mtmsr` DR off, `mtmsr` with
  0xC010 ORed back in, and back into the emulator (r30 | 0x6084). syshm's
  processor still runs it in user mode: the kernel keeps the blue task's
  MSR (`m->priv`; 0xD032 otherwise, and always for other tasks) and,
  while it has PR clear, does the privileged instructions that exit to it
  (`privileged()` in `nk.c`, `priv_insn()`): mfmsr, mtmsr (POW never
  kept: no nap), rfi (MSR = SRR1, pc = SRR0), mfspr/mtspr of the 750's
  supervisor SPRs, mfsr, mtsr, mfsrin, mtsrin, and tlbie, tlbsync and
  dcbi, which do nothing; tlbia (the 750 has none) and SPRs it hasn't are
  an illegal instruction exception, as on the 750 (in user mode,
  privileged instruction, as before). In user mode only mfmsr is done.
  The values: PVR 0x00080301 (ProcessorInfo's); HID0 0x8010C0A4 (EMCP DPM
  ICE DCE SGE BTIC BHT); HID1 0xA0000000 (PLL_CFG 1010, 4x: table 17 of
  the MPC750 hardware specifications; read-only, as are PVR and the time
  base's write SPRs, 284/285, whose writes do nothing); L2CR 0xA9000000
  (L2E, 512 KB, the core clock / 2, pipelined burst SRAM; L2IP reads 0, so
  a global invalidate is over at once); ICTC 0; THRM1 and THRM2 compare
  with 40 °C while THRM3 has E and they have V (TIV at once; TIN when
  above the threshold, bits 2-8, or below it with TID), as Linux's
  tau_6xx.c and successive approximation read it; DEC counts down with
  the time base from what was last written (0 at startup); SDR1 (RAM
  size - 1 MB) | 0xF (SystemInfo's 1 MB hash table at the top of RAM;
  NanoKernel 1 sizes it for 2 PTEs a page); the BATs as SetSpace loads
  them from ConfigInfo's user BAT map (ConfigInfo-relative lower words +
  ConfigInfo's physical address, `ROM_PHYS_BASE` + 0x30D000, as `Reset.s`
  makes them; no G in instruction BATs; all 0 for ROM files without a
  map, from 9.1 on), the segment registers its user segment map's; SPRG0-3,
  SRR0/1, DAR, DSISR, EAR, IABR, DABR and the performance monitor's
  (952-958) start at 0 and keep what's written (the counters don't count).
  Real mode (IR or DR off) uses the same address space: RAM is at its
  physical address there, and so is I/O; elsewhere it's Mac OS's map (the
  ROM's physical copy at 0x00C00000 isn't there: that's RAM here).
  Exceptions keep the MSR, as NanoKernel 1 keeps SRR1 through its
  exception and context-switch paths. With EE off nothing interrupts:
  `update_interrupts()` returns at once (sources stay pending, PendingInts
  as it was), and preemption, software interrupts, Multiprocessing
  Services' tick and task switches wait (`blue_interruptible()`; the blue
  task never blocks in a nap then), so supervisor code only ever runs as
  the blue task. MacBench's window has 13 instructions with EE off. Not
  done: the CPU plugin's routines (`cpu_plugin_run()`, 9.1's
  MPCpuPlugin) still have only mfmsr; they could use `priv_insn()` with a
  supervisor MSR.
- **MacBench's freeze** (real time only): after its registration MacBench
  times something by reading VIA1's T1C-L in a 4-instruction loop (`lbz
  r30,0(r31)`, r31 0x81096800, in its heap). In real time the time base
  only moves at sync points and some exits (the processor's step is 0),
  and a device register access on a run's last instruction exits and is
  done by the loop, which then computed its next sync point from the
  count past it, skipping the one the access ended at. SYNC_INSNS is a
  multiple of 4, so once the read fell on a sync point it fell on every
  one: no sync point was ever serviced again, so the time base, the
  timer and the loop stood still, and so did the window, input and
  `--seconds` (100% of a host core in `device_hook()`). The loop now
  services the sync point an access ends (as for quiet and info-record
  stores). Instruction-time runs are unchanged but for the JIT's count of
  kernel exits (the 8.5 CD's 4-billion check: 1,106,623 → 1,106,673:
  the sync point sets `dev_dirty`, so the next run makes no kernel calls
  in place). Main froze the same way: the user's freeze was this, not
  the skeleton key (which without it quit MacBench with "an error of type
  1", in instruction time).
- **9.2.2's interrupt sets** (`src/bootinfo.c`, `src/rom.c`): 10.2f1's
  Trampoline names `AAPL,interrupt-index` (`rom trampoline 922.iso`);
  9.1's (6.1f9) and `MOS.rom`'s don't. Under QEMU (`mac99`, 922.iso at
  the Finder, the device-tree copy from a 128 MB RAM dump with `nk2dump.py
  --tree`) every node with `interrupts` has all four properties:
  `AAPL,interrupt-vectors` the controller's sources (OpenPIC's there),
  `-priorities` 68k levels, `AAPL,interrupts` numbers counted through the
  tree (escc-legacy's channels get their own though they share escc's
  sources), and `AAPL,interrupt-index` each source's slot in HWInfo's
  VectorLookupTable, counted from 0 as sources are first met (escc and
  escc-legacy share 3-5; the table: 0x2F, 0x37, 0x19, 0x25, 4, 5, 0x24, 6,
  7, 0xD, 2, 0xE, 3, 0x1C, 0x1E; usb's source 0x1C is slot 0xD, the
  ethernet's 0x1E slot 0xE). syshm's table maps slot n to source n, so its
  index is the source. The sound chip (17, 8, 9), the USB controller (28)
  and the Ethernet node (its receive DMA channel, 3, as before) now have
  it when `rom_interrupt_index()` finds the name in the ROM file's
  Trampoline (`bootinfo_machine.interrupt_index`); earlier ROMs' trees
  are as they were (the display's index stays for every ROM: without it
  the 8.5 and 9.1 checks' counts change, though not their screens).
  QEMU's `mac99` has no sound device, so it can't show what 9.2.2's sound
  does there.
- **The codec's valid bit** (`src/awacs.c`, `setup_machine()`): what made
  startup slow with the sound chip's set. Mac OS 9's sound plug-in, once
  it has set the codec up, runs an event (`0x003203C0` in a 9.2.2 CD run
  to a billion instructions) that reads the codec's status for a subframe
  (`0x0031B9D8`: control's status-subframe field set, then +0x20) and
  tests bit 22 (MASK_VALID in Linux's `awacs.h`, whose
  `snd_pmac_screamer_wait()` waits for it, 1 ms at a time); while it's
  clear the event is queued again, about a thousand times, then dropped.
  syshm's codec never set it. Two plug-in instances poll on 9.2.2 (1,025
  and 1,015 polls, from instructions 0.90 and 0.98 billion), each 1.3 ms
  of guest time apart in instruction time; in real time with fast start
  (a guest processor ten times slower in guest time) about 8 ms apart, 17
  s in all: the CD's Finder at 29.7 s of guest time with only the sound
  chip's set, 22.2 s with all three. With the bit (`awacs.valid`) the plug-in
  goes on at once. 9.1's plug-in has the same code (`0x003093F8`, 1,000
  polls at startup, 2 s in instruction time), and 8.5's polls too; the bit
  is set for every system (real time, fast start, guest time to the
  Finder: 8.5 CD 14.6 → 13.7 s, 9.1 CD 17.1 → 15.7 s; the screens at 4
  billion instructions are the same).
- **9.2.2's sound with its interrupt set:** two plug-in instances (one in
  a fragment that makes a `sound-root` node, "VirtualHAL", with
  `AAPL,sndhw-plugin-id`) set the codec up with fades of both outputs'
  attenuation around a mute (16 steps 10 ms apart: about 0.3 s each). The
  output DMA plays 2,048 frames of silence at startup and stops; after
  that it runs only while something plays: an alert (a click outside
  SimpleText's Open dialog: 0.95 s, peak 6,925), the Sound control panel's
  Play (peak 5,393 at its volume then, 18,066 with Main Volume at the top,
  nothing with Mute, which sets both outputs' mute bits, register 1
  0xA80); the volume slider writes the attenuation (registers 2 and 4).
  The output never ran on by itself (the CD, the restored install with and
  without the Setup Assistant, after alerts), so idle real-time runs
  sleep; that it ran continuously, as first reported, didn't reproduce.
- **USB with its interrupt set (9.2.2):** without it the UIM enabled start
  of frame without MasterInterruptEnable and waited for a frame that never
  interrupted. With it: 0x8000007B (MIE, RHSC, FNO, UE, RD, WDH, SO), start
  of frame enabled for a frame at a time about 15 times during setup (each
  handled: cleared and disabled), both ports powered and read (0x100,
  nothing connected), then only frame number overflow, every 32.8 s of
  guest time. `ohci_next_event()` counts every frame while overflow is
  enabled (the HCCA's frame number moves on), and `idle_wait()` used it,
  so idle real-time runs woke the host every 1 ms: 9.2.2 now, and 8.5 and
  9.1 all along (their UIMs enable overflow too: 0x80000032, 0x8000007B).
  `ohci_next_interrupt()` gives the frame that interrupts, and
  `idle_wait()` sleeps to it (9.1's idle calls in 40 s of real time:
  94,768 → 10,862); `device_next_tb()` still sees every frame, so
  instruction-time runs are as before.
- **Ethernet by interrupt (9.2.2):** `net: receiving by interrupt` (main:
  `no receive interrupt (step 3, status -2539): polling`). In a real-time
  run of the restored system, the TCP/IP control panel shows 10.0.2.15,
  router 10.0.2.2, name server 10.0.2.3 from DHCP (28 frames sent, 2
  received).
- **Startup times with the sets, and what fast start's guest time
  measures:** the 9.2.2 CD's fast start ended at 13.4-13.7 s of guest time
  with a quiet host (main 12.9-13.1), and at 13.6-15.2 s for both when the
  host was busy (load 2.6): fast start runs guest time ten times as fast
  as the host's, so CPU-bound startup costs ten times its host time in
  guest time, which moves with the host's load. Without fast start
  (`--realtime --no-fast-start`) the Finder is current at 6.2-6.4 s
  against 5.8-6.1: the half second is the guest's own, the first CD read
  0.27 s later with USB's set (the USB Manager after the root hub's setup)
  and the sound plug-ins' fades 0.1-0.2 s more. The restored system, set
  up: 13.0-13.1 s (main 12.8-13.0), and the host asleep 36.4-37.0 s of 40
  (main 35.4-35.8).
- **Scripting the 9.2.2 restore** (instruction time): scripted clicks and
  double-clicks didn't select the desktop's icons (main too); typing a name
  in the Finder does (`MacO`, then Command-O opens the CD's window; `apple
  s`, Command-O, opens Apple Software Restore, whose destination is already
  Macintosh HD). Return, then a click on OK, restores (done by 39
  billion). Its last dialog's Quit took a click only after a key press;
  Special > Shut Down (y 125 in the CD's Finder at 800x600, 136 in the
  restored one's, whose Special menu has Burn CD) then powers off with
  the volume unmounted. The Setup Assistant's first screen quits with
  Command-Q and a click on Quit.
- **MacBench's Floating Point test** (M5; profiles in instruction time
  with `clock 1000`, which runs the same code, `SYSHM_JIT_PROFILE=FROM:TO`
  over one part): five parts of about 10 s of guest time, each counting
  iterations: an FFT (MacBench's code at 0x06D17xxx in that run), an LU
  decomposition (0x06D18xxx), MathLib's functions with NaNs and infinities
  among the arguments (0x0760xxxx-0x0761xxxx), a table of log factorials
  (`build_logfact`: MathLib's log and exp, from a loop turning integers
  into doubles the 0x43300000 way) and arithmetic on special values. The
  score is homogeneous in guest speed (clock 1000: 3,894; 2000: 7,776) and
  weighs the parts like a harmonic mean: doubling the clock in one part
  alone raised it 12.6%, 3.5%, 1.6%, 43% and 4.1%, so their shares of the
  "time" are about 22%, 7%, 3%, 60% and 8%. Host speed per part before
  (guest instructions per second): 3.2, 5.7, 1.0, 1.35 and 3.3 billion
  (the Processor test's code: 5.2), which predicts about 6,700 in real time
  (7,085 measured; the user's window: about 7x). What was slow: MathLib
  saves the FPSCR with mffs, clears it and restores it (XX ORed in) with
  mtfsf 0xFF in every call, and those went to the interpreter through
  `call_step()` (every register written back, the cache dropped, the
  pending FP operation replayed in C first): 120 million in 19 billion
  instructions of the first parts, 79 million replays. Every FP operation
  then loaded its operands from struct ppc_cpu, stored its result there
  (the next one loading it back, on the dependency chain), checked the
  rounding mode and stored its pending record (operands, instruction):
  about 22 host instructions. Special values (NaN, infinity, x/0) go to
  the interpreter (13.6 million in those 19 billion), as before: they are
  most of the MathLib and special-value parts (3% and 8%). In `src/jit.c`
  now: the FPSCR instructions natively (`fp_fpscr()`; mtfsf 0xFF replaces
  what's pending, so only mffs and partial writes settle it, and
  `fp_settle()` replays in the mode the operation ran in and takes only
  FR, FI and FPRF from the replay, so mode and exception bits may change
  in between); FPRs in host registers within a block (16 slots, part of
  the register cache's state, so every exit and call into C writes them
  back); the mode checked once a block (`fp_mode()`, a side exit
  otherwise); the pending record deferred in the cache state
  (`fp_record()`) and settled natively (`build_settle()`: a routine per
  operation, with the fast path's checks, else `fp_settle()`; a slow path
  that rejoins leaves its own record to come, so the routine must check);
  checks only the inexact test needs skipped while XX is set
  (`fp_lazy_checks()`); w5 kept as the FPSCR between FP instructions
  (`fp_keeps_w5()`); stw, stw, lfd of those words built from the registers
  (`forward_double()`). Per part after: 5.4, 6.2, 1.7, 3.4 and 5.1
  billion; real time 7,085 → 16,616 (medians of 4 headless runs,
  interleaved; in a later session 6,250 → 14,700, main merged; single
  runs on a loaded host ranged from 4,660 to 8,826 for the same binary). The 8.5 and 9.1 CD checks, and the test's own
  instruction-time run to 70 billion, end exactly as before. Left: the log
  factorials are still two thirds of what remains (MathLib code at 8-15
  host instructions an FP operation, the range checks and the XX test
  most of them; a settle and two mtfsf a call); special values through the
  interpreter; FPRs in host registers across blocks. The interpreter's
  mffs leaves the upper word 0 (`FP_INT_HI`'s comment says otherwise; the
  750's is undefined, 0xFFF80000 on the Gekko): both engines agree, as
  before.
- **Unreal Tournament at double speed** (real time, the guest tools' 3D
  engine; the Mac CD's 4.0 application, "Version 400", the same file on
  8.6 and 9.2.2): what it times with, from its code fragment (one PEF
  container, imports Microseconds, TickCount, LMGetTicks, Gestalt, no
  DriverServicesLib): `appSeconds()` (code section +0x361E0) adds up
  Microseconds deltas times 1e-6; `appCycles()` (+0x1635C8) is mftb's low
  word, for its stats only, its "CPU cycle test" (in the log: "secs/cycle")
  timing Delay(30) with it; `appSleep()` spins on Microseconds. The main
  loop (+0x1511E0) ticks the engine with appSeconds' difference. A probe
  reading appSeconds' total in guest memory once a second of host time
  found it right: with guest time on 8.6 (Microseconds there is the Time
  Manager's, from VIA timer 2) and 0.6% fast on 9.2.2 (below), in the intro,
  the menus and play, headless and in a window, with the tools installed
  at startup or during the session; and guest time kept the host clock's
  pace (the lead fast start and disk transfers left stays as it was while
  UT runs: it never idles, and it reads no disk in play). The speed is
  UT's own: `ULevel::Tick` (+0xB7120) multiplies the tick's time by
  TimeDilation, adds it to the level's TimeSeconds, then clamps it to
  0.005-0.4 s (`Clamp` with the TOC's 0.005f and 0.4f, +0xB716C) for the
  actors' ticks, so above 200 frames a second actors, their timers (the
  game's one-second timer counts the match's Elapsed Time and time limit
  down) and everything moving run fast: 400 frames a second, twice the
  speed. Measured (M5, headless, practice DeathMatch on DM-Barricade, the
  scoreboard's Elapsed Time over 30 s of host time, `SYSHM_RAVE_FPS`):
  9.2.2 at 640x480 490 frames a second, 66 s; at 1024x768 (the user's
  ini later) 290, 40 s; 8.6 at 1024x768 340, 46 s. A real iMac drew UT at
  well under 60. Now (`frame_shown()`, `frame_wait()` in `src/nk.c`,
  `video_frame_shown()` in `src/video.c`) a context drawing to a screen
  shows a frame at most once a vertical blank interval after its last, and
  the call that ended it (the first QARenderEnd or QASwapBuffers after a
  QARenderStart) returns then: in real time it leaves the host call without
  moving the PC, so the guest takes interrupts and makes the call again,
  which naps (`blue_naps()`, Multiprocessing Services' tasks first) until
  the frame's time, without making up guest time's lead as idling does;
  in instruction time guest time moves on, as for disk transfers. After:
  60.0 frames a second, Elapsed Time 30 s in 30 s (9.2.2 and 8.6, both at
  1024x768; in a window too), and the host sleeps about half the time or
  more while it plays. The 8.5 and 9.1 CD checks (4 billion instructions),
  the 9.2.2 CD's (3 billion) and installed 8.6's (5 billion) end exactly
  as before (no 3D frames there). Tests: `tests/test_video.c`.
- **Mac OS 9.1 and later take the timebase to be a quarter of the bus
  clock:** 9.2.2's Microseconds (trap A193, which InterfaceLib's calls) is
  native, UpTime times a factor its fragment sets up from
  GetTimeBaseInfo's (`0xFFD1C810` in 10.2f1's image), 4,000,000,000 over
  the bus clock: that from Gestalt `bclk` (`0xFFD1C794`; ProcessorInfo's
  BusClockRateHz without it), the RTC's for a 601. 9.1's image has the
  same code (`0xFFD276B0`); 8.5's and 8.6's don't, and their Microseconds
  is the Time Manager's, from VIA timer 2. syshm's bus was 66 MHz and its
  timebase 16.6 MHz, so 9.x's Microseconds and AbsoluteToNanoseconds ran
  0.6% fast (factor 1/16.5; Unreal Tournament's clock 1.006 of guest time,
  measured in real time). The bus is now four times the timebase, 66.4 MHz
  (device tree, ProcessorInfo; the processor stays 266 MHz): the factor is
  1/16.6 and UT's clock 1.0006 of guest time over 25 s. The 8.5 CD and
  installed 8.6 checks end exactly as before; 9.1's CD (4 billion) ends
  elsewhere in its startup's details (its timers' durations changed by
  0.6%) with the same screen, and 9.2.2's CD (3 billion) with the same
  screen but SimpleText's caret in the other phase of its blink.
