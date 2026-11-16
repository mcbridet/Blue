# Plan: Mac OS 9.1 and later

Mac OS 9.1, 9.2 and 9.2.2 need the "Mac OS ROM" file from their own CD,
and that ROM wants a NanoKernel 2 underneath: a kernel with
Multiprocessing Services built in, that runs Mac OS as one task (the "blue
task") among others it schedules. Blue implements the NanoKernel v1
interface natively (`src/nk.c`). This plan adds the parts of the NanoKernel
2 interface Mac OS 9.1 uses, one piece at a time, without changing what
8.5–9.0.4 see.

The work is broken into tasks that a subagent can pick up with no other
context than this file. Each task says what it needs, what it touches, how
to check it, and what to hand back.

## Status

Done:

- **Parcels** (`src/parcels.c`, `src/rom.c`): 9.1's ROM file keeps its
  contents in parcels. Blue loads them, takes the 4 MB Toolbox image from
  the `'rom '` parcel, and puts the `'node'` parcels' libraries
  (`AAPL,CodePrepare`: MotherBoardHAL, TimeManagerLib, NVRAMServicesLib,
  RTCServicesLib; `AAPL,CodeRegister`: NativePowerMgrLib, StartLib) and the
  `'prop'` parcels that match its devices (Cuda, Grackle, the RTC under
  via-cuda, `/rom/macos`'s version) in the device tree. `rom parcels
  MOS91.rom` lists them.
- **Address space** (`default_regions()` in `src/nk.c`): 9.1's ConfigInfo
  has no page map (the Trampoline in the same file hands the kernel one),
  so Blue lays out the 8.5 ROM's map at the logical addresses ConfigInfo
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
  task the blue one). `BLUE_LOG_MP=1` logs each, the task switches,
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
  removable drive in `src/disk.c`): a floppy drive of Blue's own disk
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
  floppy controller's Mac I/O cell reads as zeros; Blue's lanLib stays in
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
- **Drives always there, and Insert...** (`src/main.c`, `src/chooser.c`,
  `src/chooser_mac.m`, the menu in `src/ui.cpp` and `src/display.c`): in a
  window or real time Blue adds an empty floppy drive, and an empty CD-ROM
  drive when none was given (`--cdrom` or `--cdrom-drive`) and one of the
  four disks is free (else it says so once), so an image can go in
  whatever Mac OS started with; headless instruction-time runs get only the
  drives given, as before. Each drive in the menu has Insert... beside
  Eject: the host's file chooser, opened on the window's thread and not
  waited for (macOS: an NSOpenPanel, `beginWithCompletionHandler:`, its
  events run by SDL's own event loop; elsewhere zenity's or kdialog's, on
  the PATH, run by `posix_spawn` and watched by a thread of its own, which
  wakes the window when it's done); the file chosen goes to the machine as
  the menu's action with a path, and in that drive through
  `drive_insert()` (`drives_pick_in()`: that drive's checks and messages);
  without a chooser, or if it fails, the menu says to drop an image on the
  window. Checked: a real-time headless run of an installed 8.5 with no
  drives given had both (`disk 1: a CD-ROM drive, empty`, the floppy drive
  as drive 10), and a script's floppy and `922.iso` went in them and
  mounted; with four disks, or three and `--share`, no CD-ROM drive and the
  one line saying so; a window run of it, F12's menu with both drives
  empty, Eject disabled and Insert... (captured by window ID); the Open
  panel shown by a test harness running an event loop as SDL's does (not
  blocking it, "Choose a disk image to put in the floppy drive", Insert),
  cancelled (waking the window, nothing chosen) and closed unfinished.
  Clicks and keys posted to the window's process didn't reach the menu
  while another application was in front, so a file wasn't chosen there.
  `tests/test_chooser.c` runs the zenity/kdialog chooser with stand-ins
  (a file chosen, cancelled, failing, kdialog's without zenity, none, closed
  unfinished); `test_routing()` in `tests/test_drives.c` has
  `drives_pick_in()`. Not built on Linux in this session (no Linux host):
  the zenity/kdialog chooser is plain POSIX, built and tested on macOS
  too, and only `chooser_open()` differs.
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
- **OpenGL** (Task 18; milestones 0 to 8 of [opengl-plan.md](opengl-plan.md)): the
  libraries, AGL and clears, checked by a test application in the guest;
  gl calls recorded in guest memory by the library's own code, and a
  fixed-function front end on the host drawing triangles (matrices,
  clipping, culling, depth, blending, logic ops), points and lines by
  OpenGL's rules (wide, antialiased, stippled, textured), textures, client
  arrays, lighting, fog, texture coordinate generation, clip planes,
  stencil, polygon offset, modes and stipple, the attribute stacks,
  display lists, the raster position and pixel rectangles, 3D textures,
  the accumulation buffer (as Apple's generic renderer's), 16-bit
  drawables dithered, and OpenGLUtility on the host, checked against
  Mesa's OpenGL. Unreal Tournament's OpenGL build plays on the GPU
  and in software, and so does Quake III Arena's demo; the installer
  offers the libraries, and on Mac OS 9 moves Apple's to Extensions
  (Disabled) while Blue's are there.
  The groundwork (milestone 0):
  the 3D backend's texture units and stages, blend equations and color
  masks, with RAVE drawing through them, pixels unchanged; the software
  rasterizer without fused multiply-adds; the fragment builder's many
  exports, own code and zero-filled data; host calls with floats. See
  Findings, "Fused multiply-adds".
- **The window on a thread of its own** (Task 15, window-speed;
  `src/display.c`, `src/mem.c`, `src/video.c`, the JIT's fault handler
  and checked stores): the window draws and presents on the main thread
  while the Mac runs on another, and the framebuffer's pages are watched
  for writes (`MEM_WATCH`) instead of compared with a copy each frame. The
  Mac stops 0.01-0.2 ms a frame for the window where it stopped 0.2-1.2
  ms. See Findings, "The window on a thread of its own".
- **Pippin titles** (Task 19; `src/pippin.c`): Blue Pippin Library, a guest
  tool, is Blue's own PippinPPC (the Bandai Pippin's library) with Apple's
  twelve exports on Mac OS's calls. Super Marathon's two games play from
  their disc on installed 8.5 and 9.2.2, and save, load and delete saves
  through it. See Findings, "PippinPPC" and after.
- **The window's look** (ui-platinum; `src/ui.cpp`, `src/font.c`,
  `third_party/fonts`, `tools/fonts.py`): the menu (F12), the prompts and
  the status bar look like Mac OS 8's Platinum in the site's colours
  (Platinum greys, Bondi Blue for checks and the keyboard's focus), with
  bevelled boxes and buttons, a striped title bar, the default button's
  ring (Return presses it), and the text in ChicagoFLF, kept in the
  program and drawn on Chicago's own pixel grid. The same Dear ImGui menu
  on macOS and Linux (no GTK, no native menus). See Findings, "The
  window's look".
- **The window's scale** (screen-scale; `src/display.c`, `src/density.c`,
  `src/ui.cpp`): each Mac pixel is a whole number of the display's pixels,
  1 to 4. Auto, the default, picks the one that brings Mac OS nearest to 80
  pixels per inch, from the display's physical size (CoreGraphics on
  macOS; SDL 2's figure or the kernel's EDID on Linux), else the display's
  own scale; `--scale` and the menu (Scale: Auto (3x), 1x to 4x, radio
  buttons) set it. With Blue Enhancements a resized window asks for its
  pixels over the scale, and that screen is drawn at exactly the scale;
  without, the window opens at the screen's size times the scale, within
  the display, and a new scale resizes it. `--integer-scale` now only keeps
  the window from filtering a screen that isn't its own. See Findings, "The
  window's scale".
- **Packaging** (packaging; `src/version.h`, `src/hostlib.c`,
  `src/welcome.c`, `tools/package-macos.sh`, `tools/macos-deps.sh`,
  `tools/package-linux.sh`): releases are `Blue-arm64.dmg` and
  `Blue-x86_64.dmg` (a Blue.app each, macOS 11 or later, signed ad hoc)
  and `Blue-x86_64.AppImage`, `Blue-aarch64.AppImage` (built in Ubuntu 22.04:
  glibc 2.35), carrying SDL2 and libslirp with GLib and what it needs,
  separate and replaceable, with their licences, and each release has
  their exact sources in `Blue-third-party-sources.tar.gz`. Blue loads
  them from beside itself first. The version is `src/version.h`'s
  (`--version`, the usage, the Info.plist; a release's tag must match it).
  Opened from the desktop with nothing to do, Blue shows a Platinum window
  with the command line for it, until the launcher ([roadmap.md](roadmap.md)).
  See Findings, "Packaging".

Where it stops:

```
build/blue boot --headless --cdrom 91.iso --max 20000000000
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
changed when it shuts down or restarts). 9.1 installs from the CD onto a
blank disk image with its installer, scripted (see Findings, "Installing
9.1"), and the installed system starts up and takes the guest tools. In
real time with fast start the Finder is up
after 16 s of guest time (1.3 s on the host; it was 41 s before the
startup disk was found at once), against 13.8 s for the 8.5 CD: what's
left is mostly AppleTalk on Ethernet (5 s) and the CD's transfers.

The 9.2.2 CD (`build/blue boot --headless --cdrom 922.iso --max 4000000000`) has the
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
  `build/blue boot --headless --rom MOS.rom --cdrom 85.iso --max 4000000000
  --screenshot /tmp/85.png` (only the menu-bar clock may differ between runs).
- **Stay reproducible.** Runs in instruction time (`--headless`, without
  `--realtime`) must stay exactly reproducible: guest tasks run on the one
  host thread, scheduled at instruction counts and guest timebase values,
  never by host time.
- **The 9.1 check:** `build/blue boot --headless --cdrom 91.iso --max
  20000000000` (a few seconds). Add `BLUE_LOG_MP=1` for the calls, and
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
  Update the documentation in its style when behaviour changes: the
  README for what users see, and docs/ for the rest ([bring-up.md](bring-up.md)'s
  "Mac OS 9.1 (from its CD)" section was the README's until November 2026).

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
  exports, imports and MP call numbers; `--list` shows what is where.
  `tools/heap.py ram.bin` checks the system and application heaps' blocks
  in a dump (`--find ADDR`: the block an address is in). disasm.py
  needs capstone: a Python virtual environment with `pip install capstone
  machfs` in a temporary directory (machfs reads the CDs' HFS volumes).
  `build/blue rom extract MOS91.rom img91.bin` writes the 4 MB image
  (address `0xFFC00000 + offset`), `--dump-ram FILE` saves RAM.
- **Tracing a run:** `--trace-from N --trace-reg 24 --interp` (r24 is the
  68k PC), `--traps N` (68k traps), `--watch ADDR`, `--trace-range LO:HI`.
  Instruction counts are the same with and without the JIT.

## How Blue's kernel works (what the tasks build on)

- `nk_boot()`'s loop in `src/nk.c` runs guest code with `ppc_run()` in
  chunks and handles each exit: kernel calls (`twi` → `kernel_call()`),
  `sc` (→ `kcall_mp_dispatch()` when `mp_kernel`), faults, host calls for
  Blue's drivers. Devices and interrupts are updated every `SYNC_INSNS`
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
  new task runs (log the switches with `BLUE_LOG_MP`), and the boot stops
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

- **Status:** done. [nk2-reference.md](nk2-reference.md) answers the questions; 9.1
  boots to the Finder on QEMU's `mac99` unpatched. Tools:
  `tools/qemu-ref/run91.sh`, `shots.py`, `rsp.py` (gdbstub client, no gdb
  needed), `mptrace.py` (MP call trace), `nk2dump.py` (decodes RAM dumps).
- **Owns:** `tools/qemu-ref/` (new files only), a notes file
  [nk2-reference.md](nk2-reference.md).
- **Goal:** what Apple's 9.1 Trampoline and NanoKernel 2 really do, to
  check Blue against: the page map they build (and the areas from it),
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
  - The parcels' drivers against Blue's devices: the Cuda library, the
    RTC driver under via-cuda, and NVRAM (done: the ROM's driver for
    `mac-io/nvram` drives Blue's `nvram` node; see Findings).
  - The display, disk and Ethernet drivers Blue makes (NDRVs, lanLib)
    under 9.1, and idle (does 9.1's Process Manager still nap through
    call 55 and `_SynchIdleTime`?), fast start.
- **Done when:** the 9.1 CD reaches the Finder; then its installer puts
  9.1 on an empty disk image and that boots.

### 9. Regression and documentation

- **Status:** ongoing. Run after every task that changes behaviour.
- **Checks:** `make test`; the 8.5 CD to the Finder (above); 8.6 and
  9.0.4 if their images are available (copy writable disks first).
- **Documentation:** the "Mac OS 9.1 (from its CD)" section (README's
  then, [bring-up.md](bring-up.md#mac-os-91-from-its-cd)'s now) and Next steps
  ([roadmap.md](roadmap.md)); the site's Status window (`site/index.html`, "Mac OS 9.1–9.2.2"
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
  Blue's lanLib kept over the ROM file's `bmac+` one (`build_tree()`).
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

- **Status:** done. Without `--rom`, `blue boot` takes the "Mac OS ROM"
  file from the startup disk, as a New World Mac's firmware does: the first
  drive in command-line order (partitions in map order) whose blessed
  System Folder (Finder info word 0) holds a file of type `tbxi`, the first
  in catalog order (`src/hfsread.c`: HFS, HFS+, HFS+ in an HFS wrapper;
  extents overflow). It prints where from (`rom: "Mac OS 9.1:System
  Folder:Mac OS ROM" from 91.iso, ...`); the run is otherwise as with
  `--rom MOS91.rom`. `rom info 91.iso` (and the other `rom` commands) read
  a disk image's the same way. `tests/test_hfsread.c` covers the reader.
  The Startup Disk control panel's choice (startup-disk): Mac OS 9.1 and
  later set Open Firmware's boot-device, which Blue reads from NVRAM at
  launch and at each restart (`src/startup.c`, `src/ofnvram.c`), and starts
  the ROM file of the disk and partition it names (see Findings, "The
  Startup Disk control panel"). `tests/test_startup.c` covers the NVRAM
  partition and variables, the paths, and which drive starts;
  `tests/test_nvram.c` the tree's `/options`, `/openprom` and disk order.

### 12. Shared folder

- **Status:** done. `--share DIR` attaches a host folder as an HFS volume
  (`src/share.c`): metadata made in memory by `hfs_build()` (`src/hfs.c`,
  now with folders, B-trees of any depth, map nodes and Mac OS's name
  order), file data read from the host's files on demand, Mac OS's writes
  in an overlay; written back at the end of the run if Mac OS put the
  volume away, the host's deleted or replaced files moved to
  `DIR/.blue-recovery/<date>/`, else the volume kept there as an image.
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

- **Status:** done (8.5, 8.6, 9.1, 9.2.2). In
  window runs, Blue looks for the guest tools in the startup disk's
  System Folder before Mac OS starts (`install_check()` in
  `src/install.c`: the System Folder's folders, whatever their names, for
  files of the tools' type and creator whose code makes the tool's host
  call; current if both forks are the bytes this version of Blue makes).
  Once the Finder is up, if one is missing or differs (and the disk is writable
  and not locked), a box over the screen offers to install or update them
  ("Install and Restart", "Update and Restart"; Mac OS 9's "Move and
  Restart" and "Put Back and Restart"); Mac OS runs on meanwhile. On yes,
  Mac OS writes them itself: Blue
  diverts the Process Manager's next idle call (`_SynchIdleTime` before
  Mac OS 9, trap AAAA with $82F0 in D0 on 9.x) to a 68k bootstrap that
  prepares a
  generated installer fragment with the Code Fragment Manager and calls it
  (see Findings). Once all is in place (and only then) the installer
  flushes the volume and sends the Process Manager the Restart Apple event
  (`aevt` `rest`), and Mac OS restarts as Special > Restart does; the
  tools load as any extension at startup, and the check after the
  restart finds them current. Input scripts answer with `tools install` or `tools
  later` (and turn the check on without a window); `tools install`
  restarts Mac OS too. `tests/test_install.c`
  covers the check on HFS volumes made with `hfs_build()`, the offer, and
  the installer against a File Manager of its own, the restart included.
  `--tools` is gone:
  `blue tools create FILE` makes the volume for installing them by hand
  (`--cdrom FILE`). Don't Ask Again (the offer's third button; `tools
  never`): Mac OS writes "Blue Preferences" (type 'pref', Enhancements'
  creator, CR-ended text: `Blue Preferences`, `Offer the guest tools: no`)
  in its Preferences folder, by the same installer (job
  `INSTALL_JOB_NEVER`: FindFolder 'pref', PBGetCatInfo for the folder's
  name, FSpDelete of an older one, FSpCreate, FSpOpenDF, FSWrite, FSClose,
  FlushVol; no restart); the check finds it among the System Folder's
  folders, and the offer then isn't made by itself. The window's menu has
  Install Guest Tools... (`tools offer`), which makes it whenever there's
  something to install (disabled otherwise, with why), and installing
  deletes the file (FSpDelete before the FlushVol), so later versions are
  offered again. The choice goes with the disk. `tests/test_install.c`
  covers the file's contents, the check, the menu's offer and both jobs.
  Checked on the restored 9.2.2 (set up): Don't Ask Again wrote the file,
  the next startup offered nothing, `tools offer` and Install installed
  the tools, deleted the file and restarted. 9.1, installed from its CD
  (Findings, "Installing 9.1"): offered once the Finder was up (the Setup
  Assistant in front), Install and Restart put the five tools in
  Extensions and Apple's OpenGL libraries in Extensions (Disabled), Mac OS
  restarted (through the Cuda), the check then found them current;
  Enhancements ran and switched to 1024x768 for a `resize`, and Blue GL
  Test (from a tools volume made with `BLUE_TOOLS_GL_TEST=1`) drew its
  triangle scene through Blue's OpenGL. An Enhancements that Mac OS has
  launched but that hasn't run yet is found in use as it's replaced: it's
  asked to quit, and the installer goes in again once it has (Findings,
  "Enhancements is in use before it runs"). Open: none.

### 14. Clipboard

- **Status:** done. Text is shared by Mac OS and the host's clipboard in
  window runs in real time: `src/clip.c` (conversions; which side's text
  is newer), Blue Enhancements in `src/tools.c` (the guest side), and
  SDL2's text clipboard in `src/display.c`, on the window's thread, which
  is the emulation's. Enhancements (version 2 in its data: the installer
  tells an older copy on disk by its bytes, the version tells Blue where
  a running older copy's data is, which still switches modes) writes
  routine descriptors for ZeroScrap and PutScrap into the trap table and
  puts a GetNextEvent filter in `jGNEFilter`; when the host's text changes
  it posts the front application a resume event with convertClipboard, and
  keys typed in the window wait for that (at most 0.5 s). Input scripts'
  `clipboard` and `print-clipboard` stand in for the host's clipboard.
  `tests/test_clip.c` covers the conversions, echoes and held keys,
  `tests/test_tools.c` the patches. Scripted both ways on the 8.5 and 9.1
  CDs (SimpleText; `BLUE_TOOLS_APPL=1` lets the Finder open Enhancements
  from the tools volume) and on a copy of an 8.5 install (Note Pad,
  Scrapbook, the Finder's names, Internet Explorer 4.01's address field);
  there `tools install` replaced the older Enhancements (`helper.quit`
  takes the patches out too) and the new one pasted in Note Pad (then
  started at once; installs restart Mac OS now). Open:
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
  runs are unchanged. Measurements in Findings. Presenting on a thread of
  its own and watching the framebuffer for writes followed (window-speed):
  see "The window on a thread of its own" in Findings.

### 16. CD audio

- **Status:** done (8.5; 9.1 has no installed image here to try
  AppleCD Audio Player on). CD images with audio as cue sheets
  (`src/cdimage.c`: `.bin` in raw, 2048- or 2336-byte sectors, `MOTOROLA`,
  WAVE and AIFF tracks, `INDEX 00` and `PREGAP`/`POSTGAP`, sessions; the
  table of contents, the data track's 2048 bytes a sector as a
  `disk_backend`, raw sectors, the Q and P-W subchannel; `blue disk toc`
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
  lengths and plays, shuffles and scans them; `BLUE_SOUND_WAV` holds
  track 2's samples bit for bit (scaled by Mac OS's volume), in
  instruction time (reproducibly) and in real time; a synthetic audio disc
  mounts as "Audio CD 1", and MoviePlayer's conversion of a track to AIFF
  holds its samples bit for bit. Instruction-time runs of the 8.5 and 9.1
  CDs reach the Finder as before, but for the CD icon (on the desktop and
  in the window's title). Open: see [future.md](future.md), "More CD formats".

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
- **Open:** none. On the restored 9.2.2's first startup (a disk Apple
  Software Restore left unmounted), quitting the Setup Assistant left the
  desktop without a menu bar: not the VBL's doing (see Findings, "The
  restored 9.2.2's first startup without a menu bar").

### 18. OpenGL for Mac OS 9 games

- **Status:** milestones 0 to 8 done ([opengl-plan.md](opengl-plan.md) has the plan,
  its milestones and what's built). Milestone 8, the rest of OpenGL 1.2:
  logic ops (OpenGL's own on the GPU); line stipple (the counter going on
  along strips and loops, starting again for separate lines) and polygon
  stipple (through the pixel store); points and lines by OpenGL's rules
  (`raster_gl_fragments()`, shared by both backends: the diamond-exit
  rule, wide ones as columns or rows, points as squares), antialiased (each
  fragment's alpha times the pixel's area covered; the hints change
  nothing, as with Apple's ATI renderer, which always asked RAVE's best
  antialiasing) and textured (a line's level of detail its texels a pixel);
  16-bit frames dithered by a 4 x 4 ordered matrix while `GL_DITHER` is on;
  and the accumulation buffer, which on Mac OS 9.2.2 only Apple's generic
  software renderer had: pixel formats asking for one are that renderer's
  (ID 0x00020200, not accelerated), drawn as ATI's but with Apple's
  strings ("Apple", "Generic", "1.1" and the extensions 9.2.2's engine
  made of the renderer's). Both extensions that list has and Blue hadn't
  are done, and in every context's list, as on a Rage 128 with 9.2.2:
  `GL_APPLE_specular_vector` (the specular term by the light's reflection,
  as Apple's specification and engine have it) and
  `GL_EXT_texture_lod_bias` (per unit, clamped to 10, the deepest level,
  as 9.2.2 gave no maximum; in software added to triangles' and lines'
  levels of detail, on the GPU `texture()`'s bias). Then
  `GL_TEXTURE_MIN_LOD` and `_MAX_LOD` applied (in software clamping the
  level of detail, on the GPU a sampler's), and `GL_EXT_texture_env_combine`
  as Apple's engine takes it (five functions, four sources, the operands,
  scales of 1, 2 and 4; each unit's combine a stage of the backend's),
  offered in ATI's list as a Rage 128's had it, not in the generic one's.
  Quake III's demo and UT, run again, take up none of the three (their
  calls logged): new instruction-time references for both, and on
  milestone 8's runs the code costs 0.06% more host instructions in Quake
  III, none measurable in UT (a first build cost 0.2%: new fields in the
  middle of the state put the per-pixel loads out of AArch64's reach; they
  are at the ends now). On radeonsi (x86-64 Linux, gcc and clang) all
  tests pass, the new scenes within 1 of Blue's software; three older
  scenes were moved off ties first (see Findings).
  Nine more scenes against Mesa (logic ops, the LOD bias and range
  exactly, combine within 2; the rest where llvmpipe's stipple counter,
  smooth coverage and line texturing depart),
  two in the test application (the same on macOS and Linux, the GPU
  within 1). RAVE's pixels, the boots and, built with the library's old
  data, the games' instruction-time runs are main's (but for Quake III's
  16-bit frame's dither). Not done: GLU's tessellators and NURBS,
  antialiased polygons, auxiliary buffers, color index. Milestone 7, performance: measured
  first (`BLUE_GL_PROFILE`: where each frame's host time goes), in real
  time on an M5 at 640 x 480. On the GPU, Quake III's demo and UT run at
  60 frames a second with 7-10 ms of each frame to spare (the guest 3-5
  ms, the front end 0.3-1.8, reading back 1.6-2.3, mostly the GPU
  finishing at low clocks); in software the rasterizer is the cost (7-12
  ms a frame waited for). The rasterizer is about 30% faster, its pixels
  the same bit for bit (pixels unpacked and packed as vectors, vector
  blend factors, blending by one and zero skipped, bilinear texel
  addresses without float clamping where they're exact): Quake III in
  software 59.2 to 59.9 frames a second, UT's intro 52.3 to 56.8. In a
  window in real time, frames are shown when they're due, at an even pace
  (before, the window's own updates ran slower than 60 a second, and a
  frame was shown twice and the next lost every few seconds). The games'
  instruction-time runs, the 3D engine's pixels (software and GPU) and the
  8.5 and 9.1 boots are as before. Two test fixes: `opengl` crashed on a
  Linux without libEGL (feedback compared against Mesa without Mesa), and
  on AMD's radeonsi `gpu` and `opengl` failed at ties the tests set up
  (pixel centers exactly on texel edges, an alpha exactly the alpha
  test's, a depth exactly a quad's), now moved off, and at radeonsi's
  `glCopyPixels` of depths (see Findings), not compared there. Milestone
  6, completeness: selection
  and feedback (`src/glselect.c`: hit records and every feedback type, the
  primitives clipped to the view volume, in Mesa's order), evaluators
  (`src/gleval.c`: one- and two-dimensional maps of every kind, grids,
  meshes, made normals), `aglSetFullScreen` (the Display Manager switches
  the screen to the size asked for and back when the drawable goes, by the
  library's callouts) and `aglUseFont` (as Apple's: QuickDraw draws each
  glyph into an offscreen world, the host makes it a list's `glBitmap`).
  OpenGLLibrary now imports those calls from InterfaceLib, so its data
  moved: an older version's copy is left alone, with a message to update
  it. S3TC and paletted textures are left out: Mac OS 9.2.2's engine and
  ATI renderer don't offer them. Feedback and selection give Mesa's
  records, the evaluators' two new scenes Mesa's pixels (within 2), and
  the test application's `fullscreen` and `font` scenes are the same
  screenshots on macOS and Linux. Quake III's demo and UT play, but at
  other moments than main's in instruction time: the library's imports
  shift when they start (built without them, the milestone's Blue gives
  main's runs exactly). Milestone 5: display lists (kept with
  the bytes their calls' pointers pointed to, arrays' draws as the
  attribute calls they make; nested, shared), the raster position,
  `glBitmap`, `glDrawPixels` and `glCopyPixels` of colors, depth and
  stencil (zoomed, through the pixel transfer and maps), `glCopyTexImage`
  and `glCopyTexSubImage`, `glGetTexImage`, 3D textures in both backends,
  and OpenGLUtility on the host (`src/glu.c`: the matrices, projections,
  scaled images and mipmaps, quadrics, as SGI's GLU 1.2 does them; Apple's
  strings; tessellators and NURBS renderers kept but drawing nothing).
  Four scenes against Mesa differ by 1 at most but in lines at a slope
  (Mesa's libGLU drawing "glu"'s quadrics), every scene made into a list
  draws the same pixels, and the test application's new scenes (lists of
  quadrics made through OpenGLUtility; pixel rectangles) are the same
  screenshots on macOS and Linux. Quake III Arena's demo (id's, from
  archive.org) installs with its installer and plays `demo001` on a clone
  of 8.6, the same frame on macOS and Linux in instruction time (with
  `TZ=UTC`), and Unreal Tournament's runs are main's. Milestone 4: client
  arrays of every
  type (interleaved, `glDrawElements`, `glDrawRangeElements`, compiled
  arrays), lighting (8 lights, two sides, separate specular, the color
  material, normalizing and rescaling), fog, texture coordinate generation
  (with `GL_NV_texgen_reflection`'s), user clip planes, stencil in both
  backends (RAVE's pixels and speed unchanged), polygon offset and modes,
  `glPushAttrib` and `glPushClientAttrib`, `glGet*` for all of it; eight
  scenes against Mesa differ by 2 at most but in one face lit from behind
  (by 3), and the test application's lit, fogged, stenciled scene is the
  same screenshot on macOS and Linux. The user has no Quake III (the
  plan's check), so the scenes and Unreal Tournament were the checks. Mac
  OS 9's OpenGLLibrary has no fog coordinates. Milestone 3: textures (`src/gltex.c`:
  objects shared between contexts made to share, 1D and 2D images and
  subimages in every format and type, parameters, completeness, two units
  with their environments as the backend's stages, the texture matrices),
  `glReadPixels`, the extensions string; swaps to a port paced by the
  display's refresh as the 3D engine's frames are; the installer offers
  the libraries, and on Mac OS 9 moves Apple's to Extensions (Disabled)
  once Blue's are in Extensions, or offers to put them back if Blue's
  are gone (checked on a clone of 9.2.2). Unreal Tournament's OpenGL build
  plays (the intro and a match on 8.6, the intro on 9.2.2), on the GPU (60
  frames a second in real time) and in software (about 50), its pictures
  the RAVE build's; four textured scenes against Mesa differ only where Mesa's
  `GL_CLAMP` blends with the border color and at three pixel centers on a
  shared edge. Milestone 2: most gl calls recorded by
  code of the library's own into a buffer in the process's memory, done on
  the host at its next host call; the fixed-function front end
  (`src/glfront.c`): matrices, `glBegin`/`glEnd`'s primitives, clipping
  with a guard band, culling, flat and smooth shading, depth, blending,
  the alpha test, masks, scissor and viewport, the same numbers on every
  host; where OpenGL leaves room, the Rage 128's way (colors linear in
  screen space). Six scenes drawn through the libraries match `gpu.c` and
  Mesa's compatibility profile but for pixels whose centers are on edges
  (and, in a perspective scene, Mesa's perspective-corrected colors); a
  perspective scene in the test application gives the same screenshot on
  macOS and Linux. `gpu.c`'s varyings are smooth now (llvmpipe got
  noperspective ones wrong where it clips). Milestone 1: the three libraries
  (`src/opengl.c`, from the table `tools/gltable.py` made of Apple's),
  AGL, contexts laid out as Apple's with their dispatch tables, swaps into
  a window's port clipped to its regions (`src/surface.c`), clears; a test
  application of Blue's own (`src/gltest.c`) whose window, cleared through
  the guest's OpenGL on installed 8.5 and 8.6, gives the same screenshot on
  macOS and Linux, in software and on the GPU. Milestone 0: Blue's own OpenGLLibrary, OpenGLMemory and
  OpenGLUtility, recording calls in guest memory, with OpenGL's fixed
  function in C on the host, drawn by `raster.c` and `gpu.c`. Done: the
  3D backend's richer vertices and states (three colors, a fog coordinate,
  two texture units combined in up to four stages as `GL_COMBINE`, blend
  equations and constant colors, no blending, color masks, fragments
  without alpha drawn too), RAVE's texture operations as stages
  (`raster_rave_stages()`), a GPU program for each configuration of stages;
  the rasterizer built without fused multiply-adds; `ndrv_build()` with
  any number of exports through a PEF hash table, data exports, code of a
  fragment's own, zero-filled data, termination routines and versions; host
  calls with f1 to f13 and `ndrv_args`. Tests: `tests/test_raster.c` (each
  stage operation and kind of argument, blend equations, constant colors,
  color masks drawn and cleared, the fog coordinate, fragments without
  alpha), `tests/test_gpu.c` (the same against `raster.c`, a projective
  unit, minifying and magnifying filters), `tests/test_tools.c` (300
  exports found as the Code Fragment Manager finds them, the rest of the
  builder), `tests/test_drivers.c` (`ndrv_args`), `tests/test_jit.c`
  (f1 to f13 at a host call, both engines). Checked: 20 random streams of
  RAVE calls (6,000 frames) give the same pixels as before in software,
  on macOS (with the next item) and Linux; on the GPU, 4 frames differ,
  each in one pixel's channel by one step. The 8.5 and 9.1 CDs boot to the same screenshots as
  main's.
- **Next:** the plan's milestones are done. Candidates for speed, if a
  game needs them: reading back asynchronously while the guest waits for
  its frame's turn, a vertex cache in `glDrawElements`. Open items: OpenGLUtility's
  tessellators and NURBS renderers keep their objects and properties but
  draw nothing (until a program needs them); antialiased polygons
  (`GL_POLYGON_SMOOTH` is kept, not drawn), auxiliary buffers and color
  index aren't offered. (`raster.c`'s pixels on edges two
  triangles share: fixed, see Findings, "Shared edges in the software
  rasterizer".)

### 19. Pippin titles

- **Status:** done. Blue Pippin Library (`src/pippin.c`, file "Blue
  Pippin Library", 'shlb' 'syPP', fragment `PippinPPC`, host call number
  0x108 in its unused stubs only, to tell Blue's copy) is offered and
  installed with the other guest tools (the Finder icon: the stack with a
  "P!" puzzle piece). Its code is its own: each export calls InterfaceLib
  (FindFolder, PBGetCatInfoSync, DirCreate, Standard File, FSpDelete,
  FlushVol, OpenWD, the Gestalt value calls, the trap table) or
  AppearanceLib (StandardAlert); `tests/test_pippin.c` runs it in the
  interpreter against a Mac OS of the test's own (folders, dialogs,
  Gestalt, the trap table), and checks the exports by the PEF hash table.
  Super Marathon's Marathon and Marathon 2 launch from the disc and play
  on 8.5 and 9.2.2 installed on disks (scratch installs: 8.5 from its CD,
  9.2.2 restored); a game saved at a level's end lands in
  `Preferences:Pippin:marathon2.ppc` (or `marathon.ppc`), loads with
  Continue Saved Game, is deleted with Delete Saved Game, and Quit returns
  to the Finder. The saves were tested with copies of the disc whose first
  level's player start is at its exit terminal (no monsters, no mission),
  made in scratch: playing through a level isn't scriptable. The other
  Pippin disc here, "1995.1.17 Hanshin Daishinsai" (a Director 4
  projector, 68k), runs on both.
- **Open:** titles that use the Pippin's video calls (PippinVideo.h's)
  or its floppy dock. (The AppleJack controller: the host's game
  controllers play as one, by each title's mapping; see Findings, "The
  AppleJack".)

## Findings

Facts later tasks need, added as they are found.

- **Fused multiply-adds** (OpenGL, milestone 0): clang contracts `a * b +
  c` into one fused operation within an expression by default, and does on
  Apple silicon (345 in `raster.c`), where it rounds once instead of twice;
  so the software rasterizer's pixels on Apple silicon Macs differed from
  Linux's and Intel Macs'. `#pragma STDC FP_CONTRACT OFF` (clang's; GCC
  ignores it and warns) stops that; GCC contracts nothing in ISO C mode
  (`-std=c11`), but does in its default GNU mode: `CFLAGS` given on make's
  command line replace the Makefile's, `-std=c11` with them, so give them
  in the environment, as CI does. Apple's OpenGL driver: setting the blend
  equation for every draw call, even to the same value, cut a test of many
  small draws to a fifth of its frame rate.
- **Shared edges in the software rasterizer** (OpenGL, after milestone 3):
  `raster.c`'s edge functions were floats, with the top-left rule on top;
  at a pixel center exactly on an edge two triangles share, rounding could
  leave both just below zero, so neither drew it (3 pixels on a quad's
  diagonal in a textured scene, which the GPU and Mesa drew), and near
  such edges which triangle got a pixel was rounding's choice. Now the
  vertices snap to 1/256 of a pixel, as GPUs and Mesa snap them, and the
  edge functions are 64-bit integers there, with the top-left rule as a
  bias of 1 (vertices beyond 2^21 pixels aren't drawn); each row's span
  comes out exactly by integer division, so pixels aren't tested one by
  one. Coverage now matches Mesa's in every scene `tests/test_opengl.c`
  compares (the pixels one covers and the other doesn't went from 1, 11,
  11 and 3 to none), and Metal's in them; `test_shared_edges` in
  `tests/test_raster.c` checks a mesh with edges through pixel centers
  (4 pixels wrong with the old code). RAVE's software pixels changed only
  at edges: in the 20 random streams (6,000 frames, 36.8 million pixels)
  13,047 pixels, at most 31 in a frame (of the rasterizer's changes before
  overdraw, all were within 1/256 of a pixel of an edge, 13% within
  10^-4: ties the floats decided by noise; the rest moved by snapping); in
  Unreal Tournament's intro in software, at most 19 pixels of 786,432 in
  each of eight frames at fixed instruction counts. Interpolation still starts a pixel
  before each row's span, where the float estimate started, so pixels
  covered as before round as before (starting at the span instead changed
  8,000 more in the streams by rounding, most in slivers whose gradients
  are huge). It's faster: 5-27% on 3,000 60-pixel triangles, 12-27% on
  100,000 4-pixel ones. The GPU's pixels are unchanged.
- **No fog coordinates on Mac OS 9** (OpenGL, milestone 4): Mac OS 9.2.2's
  OpenGLLibrary exports `glSecondaryColor*EXT` and
  `glSecondaryColorPointerEXT`, but no `glFogCoord*` or
  `glFogCoordPointerEXT`, and its engine lists neither
  `GL_EXT_fog_coord` nor `GL_EXT_secondary_color` (the secondary color is
  there for separate specular and `GL_COLOR_SUM_EXT`). Fog goes by the
  eye-space depth alone, and `GL_FOG_COORDINATE_SOURCE` is an invalid enum.
- **Pixel rectangles in Mesa** (OpenGL, milestone 5): Mesa 25.2 (llvmpipe)
  draws an image from the raster position rounded to the nearest pixel
  corner (`lroundf`), where OpenGL 1.2's 3.6.4 takes the position as it is
  (the same pixels at a zoom of 1 but at half-way positions); Blue rounds
  as Mesa does. For fractional and negative zooms, Mesa's rows and columns
  fall where neither OpenGL 1.2's rectangles nor that rounding put them:
  Blue keeps OpenGL 1.2's rectangles (checked in `tests/test_glfront.c`),
  and the scenes compared with Mesa zoom by whole numbers. Mesa's pixel
  maps don't work: see the next item.
- **Mesa 25.2's pixel maps corrupt its heap** (OpenGL, milestone 5): its
  `glDrawPixels` with `GL_MAP_COLOR` on frees a 40-byte block it goes on
  using. Its blue and alpha come out as garbage, even through identity
  maps, and when the context goes, Mesa reads the block and writes 4 bytes
  into it (valgrind: freed in `glDrawPixels`, read and written in
  `eglDestroyContext`). A program of a few calls to Mesa alone, without
  Blue, aborts every time ("corrupted double-linked list"). In
  `tests/test_opengl.c` it was the intermittent crash during milestone 5
  (an abort or a segfault in about a third of the runs, depending on how
  the threads' allocations fell; a sanitized build and `MALLOC_CHECK_`
  changed that, and AddressSanitizer found nothing in Blue's code). It
  stopped once the scene compared with Mesa used no maps, and wasn't
  understood until it was reproduced, after the milestone, by putting the
  maps back (10 crashes in 30 runs; none with a negative zoom but no
  maps). Blue's GPU backend never uses Mesa's maps (the pixel transfer
  is the front end's, on the host). The Mesa harness now fails a scene that turns
  `GL_MAP_COLOR` on, rather than run it.
- **SGI's GLU, 1.2 and 1.3** (OpenGL, milestone 5): Apple's OpenGLUtility
  is SGI's 1.2 ("1.2 MacOS 8.0"; it imports OpenGLLibrary's `glc_cc` as
  data, `NewPtr` and `DisposePtr`). 1.2 makes every mipmap level through
  16-bit images, each the last scaled (`scale_internal`); 1.3, Mesa's
  libGLU, keeps 2D images in their own type, halving 8-bit ones directly
  and scaling those that aren't powers of two by another box filter, up
  to 35 levels apart from 1.2's at the first level of a 3 x 7 image (and
  within a level where there's no scaling). `gluScaleImage` and 1D
  mipmaps are the same in both. Both read normals they never computed for
  quadrics drawn as points or silhouettes with flat normals (stale stack
  memory: Mesa's libGLU gave a previous call's); Blue gives the smooth
  ones there.
- **Mesa 25.2's feedback and evaluators** (OpenGL, milestone 6): its
  feedback gives `GL_LINE_TOKEN` where OpenGL 1.2's 5.3 has
  `GL_LINE_RESET_TOKEN` (each separate line, each strip's first segment,
  each polygon's outline's), and, flat shaded, each vertex's own color
  rather than the provoking vertex's; it gives polygons as triangles
  (v[i], v[i + 1], v[0]), which Blue follows (the order is OpenGL's to
  choose). Its evaluators: once a color map has been evaluated, the
  current color loses the components the last `glColor` didn't set (all
  four if it was never called), so later vertices come out (0, 0, 0, 0),
  or with alpha 0 after `glColor3`; filled meshes are triangle strips (the
  quad strips of 5.1 split along the other diagonal: other colors inside
  a quad, where they aren't linear in u and v); `glGetMap`'s coefficients
  come v fastest (its manual page: u fastest); `GL_AUTO_NORMAL`'s
  derivatives are in u' and v', so a domain run backwards doesn't turn the
  normal round as 5.1's dp/du x dp/dv does. Blue keeps OpenGL's way in
  each; the scenes compared with Mesa avoid them.
- **Apple's `aglUseFont` and `aglSetFullScreen`** (OpenGL, milestone 6,
  disassembled from Mac OS 9.2.2's OpenGLLibrary): `aglUseFont` fails with
  `AGL_BAD_STATE` while a list is being made and `AGL_BAD_VALUE` for a font
  `RealFont` doesn't have; it measures the font in a 4 x 4 1-bit GWorld,
  then draws each glyph in one of (widMax + 2e) x (ascent + descent + 2e)
  (e: half the size if the face isn't plain), the pen at (e, ascent + e),
  copies its rows bottom up and makes list base + i of one `glBitmap`:
  (CharWidth + 2e) wide, from (e, descent + e - 1), moving CharWidth on,
  with the unpacking it sets (and puts back). It draws white on black
  after `SetPort` (not `SetGWorld`, so the colors are the screen's
  device's); Blue uses `SetGWorld` and the world's own black on white, so
  the glyphs are its 1 bits whatever the screen's depth. `aglSetFullScreen`
  keeps the size, frequency and
  device, attaches the drawable through its engine and sets the viewport
  to the size; neither the library, the engine nor the ATI renderer
  imports a Display Manager call.
- **radeonsi's `glCopyPixels` of depths** (OpenGL, milestone 7; Mesa
  26.2 on a Radeon RX 6800 XT, surfaceless EGL, compatibility profile):
  copying `GL_DEPTH` writes no color at all (OpenGL 1.2's 4.3.3 has the
  fragments take the raster color) and writes the depths with the depth
  test disabled too (with it off, nothing should reach the depth buffer);
  llvmpipe does as OpenGL says. A 64 x 32 framebuffer object, a quad at
  depth 0.5 in red, then `glCopyPixels(0, 0, 16, 16, GL_DEPTH)` in blue:
  radeonsi leaves black with depth 0.5 there, with the test `GL_LESS`,
  `GL_ALWAYS` or off. `tests/test_opengl.c` leaves those pixels out on
  radeonsi.
- **Ties in the tests** (OpenGL, milestone 7): a test whose numbers put a
  pixel center exactly on a texel's edge, or a value exactly at a test's
  reference, passes on one GPU and fails on another, since there the last
  bit of a division or interpolation decides (radeonsi rounded otherwise
  than llvmpipe and Metal at 14 texel edges of a projective square, 11
  pixels of an alpha test against 0.5, a depth image's 0.6 against a
  quad's). Exact rational numbers make them: perspective with q of 1 to
  2.5 across 32 pixels and 16 texels put many pixel centers on edges.
  The tests keep a margin now (a thousandth of a texel; an alpha's 0.5
  between pixel centers). After milestone 8, the same with colors: a
  clear of 0.1 (25.5 of 255: radeonsi rounds it down, llvmpipe and Blue
  up) and logic ops over a gradient (radeonsi's interpolated colors a
  step from llvmpipe's at some pixels, which XOR and the like turn into
  differences in high bits); the scenes' colors are whole 255ths, flat,
  where logic ops take them.
- **radeonsi's aliased points** (OpenGL, after milestone 8; Mesa 26.2.6
  on a Radeon RX 6800 XT, compatibility profile): a point of size 2.6
  without `GL_POINT_SMOOTH` is a square 2.6 pixels across (as core
  profiles have it), where OpenGL 1.2's 3.3 rounds the size to the
  nearest whole number, 3 (llvmpipe and Blue do). Whole sizes are drawn
  alike; `tests/test_opengl.c` leaves the 2.6 point out on radeonsi.
- **Apple's renderers' info on Mac OS 9.2.2** (OpenGL, milestone 8,
  disassembled): `gldGetRendererInfo` of the ATI renderer gives no
  accumulation buffer, no stencil and no offscreen drawables (accelerated,
  full screen, compliant); the generic software renderer's gives
  accumulation buffers RGB 888, ARGB 8888, RGB 16-16-16 and ARGB
  16-16-16-16, stencil 0 or 8, 4 auxiliary buffers, window, offscreen,
  backing store, MP safe, robust, multiscreen and compliant, not
  accelerated or full screen. Its `gldGetString` gives "Apple", "Generic",
  "1.1" and an extension list ending in `GL_ARB_multitexture` (left out of
  [opengl-plan.md](opengl-plan.md) before). The engine's `glGetString` passes the
  vendor, renderer and version on as they are, and makes the extensions
  its own seven followed by those of the renderer's it knows (13), in the
  renderer's order, each with a space after it. ATI's
  `glrUpdatePointSettings` and `glrUpdateLineSettings` set RAVE's
  `kQATag_Antialias` to `glrAntiAliasHint(GL_NICEST)`, best, whenever
  smooth points or lines are on, whatever the hint. ATI's `gldGetString`
  picks its renderer's name and extensions by the chip; a Rage 128's
  list has `GL_EXT_texture_env_combine`, `GL_EXT_texture_lod_bias` and
  `GL_SGIS_texture_edge_clamp` ([opengl-plan.md](opengl-plan.md) had guessed, from the
  strings' order, a shorter one). The engine keeps `glTexEnv`'s LOD bias as
  given, per unit, but answers no `GL_MAX_TEXTURE_LOD_BIAS_EXT`
  (`GL_INVALID_ENUM`); its specular vector is the specification's formula.
  Its `glTexEnv` takes `GL_EXT_texture_env_combine`'s values only (no
  `GL_SUBTRACT_ARB`, no other unit's texture as a source), and answers a
  scale other than 1, 2 or 4 with `GL_INVALID_ENUM` (the extension:
  `GL_INVALID_VALUE`).
- **Where new state goes in `raster_state` and `gl_state`** (OpenGL,
  after milestone 8): adding 16 bytes to each texture unit's
  `struct raster_unit` (which comes early in `raster_state`) cost Quake
  III's run 0.15% more host instructions, with no new work per pixel: the
  fields the row loops read moved past where AArch64's paired and
  unscaled loads reach from the state's start (252 and 255 bytes), so
  each loop variant loaded them in more instructions (`otool -tv`: `ldp`
  and `ldur` became two `ldr`s, and a spill appeared). The same in
  `gl_state` (24 KB) for its per-unit environment. New state that isn't
  read per pixel or per vertex goes at the end of those structures, and a
  host instruction count (`time -l`, one rasterizer thread, on runs that
  are otherwise identical) shows it.
- **Mesa's line stipple and smooth points and lines** (OpenGL, milestone
  8; Mesa 25.2's llvmpipe): a strip's stipple counter goes on from segment
  to segment by their lengths, not by the fragments drawn (OpenGL 1.2's
  3.4.2), so its dashes may end a pixel sooner or later; smooth lines'
  alpha is a ramp by distance from the line, and smooth points are drawn
  as round aliased ones (alpha 1). Its aliased lines and points are
  OpenGL's diamond-exit rule and squares exactly, but at one tie: where an
  x-major line going up crosses a pixel row's edge exactly at a column's
  center, it takes the row below, and OpenGL's perturbed endpoints the one
  above. Blue follows
  OpenGL 1.2 in each; the scenes compared with Mesa allow for them.
- **S3TC and paletted textures on Mac OS 9.2.2** (OpenGL, milestone 6):
  the extension strings of 9.2.2's OpenGLEngine, OpenGLRendererATI and
  the software OpenGLRenderer have neither S3TC nor
  `GL_EXT_paletted_texture`; only the "NVIDIA OpenGL" engine on the same
  disk (GeForce cards') lists them.

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
  per task; Blue doesn't, so later IDs differ from NanoKernel 2's.
- **MPLibrary's tasks:** the ROM's internal create (`0xFFCE03EC`, behind the
  exported `MPCreateTask` at `0xFFCE061C`) puts a 0x14C-byte record at the
  top of the stack block (the task's r1 and r3, and its call-12 word) and
  starts every task at a wrapper, `0xFFCE03A0`, that calls the entry point
  (record +0x28, parameter +0x2C) and then `MPTerminateTask` (`0xFFCE0230`,
  call 9) on itself with the result. So entry points don't return to their
  LR; Blue still starts LR at the kernel data page (`mp.task_return`),
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
- **Task switching in Blue (`nk.c`):** while a task other than the blue
  one runs, the blue task's NanoKernel 1 state (`m->context`, `m->system`,
  `m->flags`, `m->enables`) stays as it was; `update_interrupts()` posts to
  the System context's CR wherever it is (`system_cr()`: the blue task's
  saved registers when it was in the System context), and `preempt` waits
  until the blue task is back. Kernel calls other than 8 and exceptions in
  other tasks stop the run (Task 4). The switch happens in `nk_boot()`'s
  loop after an exit is handled, when `mp.yield` is set (see the
  scheduler's finding below).
- **Timers:** 40 makes one (ID in r4), 30 sets what firing does (as 66
  does for notifications), 31 arms it (options: see "Timers in Blue"
  below), 32 returns the time left in r4:r5 and disarms. TimeManagerLib's
  timers set event group 0x160001, the level-1 software interrupt group,
  when they fire (Task 5 delivers the interrupt).
- **The scheduler in Blue (Task 2, `mp.c`):** one ready queue in arrival
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
  nybble; 0 in Blue, so 2; nothing in the run writes it): latency-protected.
  In Blue's run the mask is 7 from instruction 1.1 million to about 53.88
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
  MP call makes one due). Blue's run makes call 13 about as the real one
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
  halves its age; Blue has no hash table, so ages only grow.
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
- **Timers in Blue (Task 3):** timer objects and blocked tasks' deadlines
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
  first timer of Blue's run fires at instruction 54,160,000, its time
  54,143,452: deadlines are seen at the next sync point or exit (every
  20,000 instructions at most, about 1.2 ms of guest time), never while the
  blue task runs with its 68k interrupts masked.
- **Reference run** ([nk2-reference.md](nk2-reference.md), QEMU `mac99`): up to call 7
  the real sequence is Blue's, plus a first MPYieldWithHint (13) from the
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
  (two of them on Blue, from candidate plugins: a 9500/9600 one looking
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
  An exception in it gives -29285, a selector out of range -29287. Blue
  runs it in an inner loop (`cpu_plugin_run()`): no interrupts, timers or
  task switches meanwhile; mfmsr and device registers work, any other
  privileged instruction counts as an exception. The kernel's own SIGPs
  (12 from the idle task, 4060 per boot on QEMU; those for other
  processors) don't happen in Blue (no idle task, one processor). After
  registering, Mac OS calls selector 0 once more, then `MPProcessors` (3:
  r3 0 all, else scheduled; the count in r3, 1).
- **Processes** (Task 8): `MPCreateProcess` (4: address space r3, 0 the
  system's → r5) makes one late in startup (0x01B40001 in Blue,
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
  over the VIA, and Blue's Cuda answers from the guest clock (SET_TIME
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
  use offset NKHWInfo +0x3C (0x1400, as Blue gives 8.5's ROM) plus the
  PRAM address, so 9.1 keeps PRAM where 8.5 does through RTAS, and one
  `--nvram` file serves both (each started from the other's file without
  reinitializing PRAM). A boot reads 0x1400-0x151F and 0x0000-0x001F (the
  partition header NVRAMFindPartition looks at; Blue's NVRAM has no
  partitions, and nothing seen depends on them). A first boot on a blank
  file writes PRAM's 256 bytes at shutdown; later ones the blocks that
  changed. The real node (a beige G3's Open Firmware listing; the iMac's
  Paddington is a Heathrow): `nvram@60000`, compatible nvram; Linux's
  powermac nvram.c
  takes the stride from the one `reg` range (0x20000 / 8 KB), QEMU's
  macio NVRAM is `addr >> 4 & (size - 1)`, and OpenBIOS gives `#bytes`
  0x2000, `reg` 0x60000 0x20000. Blue's node and chip are only in trees
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
  of a `--fixed-time` CD boot. Blue faulted on the first (exception 21, a
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
  DriverGestalt bit in its DCE's flags, which NDRVs have. Blue's tree had
  no /chosen, so nothing matched until the 25 s were up. Now, for ROM
  files with parcels, `/chosen` has `bootpath`
  `/pci/blue,disk@<PCI device>:<map entry>,\\:tbxi` (no number for a
  disk without a map) for the partition the ROM file came from if it has
  boot blocks ('LK'), else the first partition with them, disks in
  command-line order (`build_tree()` in `bootinfo.c`, chosen in
  `setup_machine()`), and the disk driver answers 'nmrg' (the RegEntryID
  from Initialize), 'intf' 'scsi', 'dvrf' target = its PCI device number,
  and 'ofpt' (`status()` in `disk.c`); earlier ROMs see none of it. A path
  ending in a unit-only node under the disk's (`/pci/blue,disk@2/@0:3`,
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
- **The profiler on macOS** (JIT speed): macOS delivers SIGPROF at the
  thread's next system call or fault, not where the time went: a loop
  spending 1.25% of its time in `getppid()` got 99.3% of its samples there.
  So `BLUE_JIT_PROFILE` had put the 8.5 CD boot's time in `pread`,
  translation (which faults on fresh code pages) and signal handling, with
  36-54% in translated blocks; timing each part directly says 68%, and so
  does the profiler now that a thread of its own samples the emulation
  thread (`thread_suspend()`, `thread_get_state()`; Linux keeps SIGPROF,
  which is fair there). Earlier estimates from it (translation 4%, the
  instruction-cache upkeep after it 4%) were mostly that bias.
- **Where a boot's host time goes** (JIT speed; M5, a measuring build
  timing each part with the counter register, ms, before this work):

  | | 8.5 CD | 9.1 CD | 9.2.2 CD | 8.5 disk | 9.2.2 disk |
  |---|---|---|---|---|---|
  | all | 1,179 | 1,496 | 1,209 | 1,384 | 2,318 |
  | translated code | 802 (68%) | 922 (62%) | 948 (78%) | 919 (66%) | 1,547 (67%) |
  | kernel calls in place | 41 | 191 (13%) | 44 | 27 | 192 (8%) |
  | device registers in place | 41 (3.5%) | 0.6 | 0.6 | 12 | 0.6 |
  | the interpreter from inside blocks | 11 | 13 | 7 | 15 | 15 |
  | interpreting code not yet hot | 66 (5.6%) | 43 | 43 | 99 (7.1%) | 82 |
  | translating | 34 (2.8%) | 19 | 27 | 61 (4.4%) | 28 |
  | links; instruction-cache barriers | 11; 3.7 | 6; 2.1 | 7; 2.2 | 20; 7.7 | 9; 3.5 |
  | the kernel's loop, between runs | 151 (13%) | 277 (19%) | 115 (10%) | 208 (15%) | 411 (18%) |

  The kernel's share is mostly host calls (the disk and display drivers:
  75, 75, 114, 147 and 191 ms, 1-2.7 µs each), Multiprocessing Services'
  calls (9.1: 95 ms) and kernel calls that exit (below). A kernel call in
  place costs 23 ns, nearly all of it the context switch's copies of 32
  GPRs and 32 FPRs each way (`switch_context()`): the next step there is
  the kernel's, not the JIT's (switching FPRs only when they're used, as
  the NanoKernel does). The 8.5 CD's 872,000 idle-hook exits each mark the
  devices dirty, so the 68k emulator's next RunAlternateContext can't be
  made in place (963,000 kernel exits, about 5% of the boot); not changed,
  as when devices are brought up to date is guest-visible (the USB
  controller writes the frame number into guest memory). (Both since
  done, below: "The context switch", "Kernel calls while devices are
  dirty".)
- **Code that isn't hot yet** (JIT speed): the interpreter runs it at 6-8
  ns an instruction (46 ns for a block of 6.5), against 3-4 ns for warm
  code: guest code it hasn't seen, and the interpreter's dispatch
  mispredicted. `BLUE_JIT_HOT` from 4 to 16 changed the three CD boots'
  cycles by under 2% (1, 2 and 32 were worse, by up to 7%): translating
  sooner costs what it saves. Going on from block to block inside the
  interpreter while the next one is cold (no return to `jit_run()` and
  `find()` between them) halved the calls but saved nothing (8.5 CD and
  disk), so it was dropped.
  16-41% of these instructions are hot code, run by the interpreter at the
  device sync points (every 20,000 instructions), where the next block is
  longer than what's left (1.1-3.6 million instructions a boot, about 1% of
  it): a block runs only when the budget covers it.
- **Instruction-cache upkeep** (JIT speed, AArch64): `ic ivau` over new
  code and the barriers before generated code runs cost 0.3-0.6% of a boot
  (timed directly), so there's nothing worth batching; macOS's write
  protection switches around each translation, link and invalidation
  (`pthread_jit_write_protect_np()`, about 20 ns) cost 0.5-0.9%, and
  merging those that come together would save under half of that.
- **Device registers in place, read only** (JIT speed): an access cost 25
  ns: devices brought up to date 9.5, the access 4, interrupts 5.5, the
  next device event 6.5. A read that changes nothing (the VIA's registers
  but for T1CL, T2CL and SR; all of the USB controller's), with the
  devices not due to change by themselves, now skips the last two and
  `in_place()`: they would find everything as it was (checked at every
  such access of the 8.5 CD boot, a million). Devices are still brought up
  to date, as that is guest-visible (above). 8.5 CD: host instructions
  -1.4%.
- **`lwarx`/`stwcx.`** (JIT speed): 1.8 of the 2.4 million calls into the
  interpreter from inside blocks in the 8.5 CD boot, 1.7 of 2.3 million in
  9.1's (the rest: `lswx`/`stswx`, `lswi`/`stswi`), each writing back and
  reloading every register. Now native on both hosts; a `stwcx.` whose
  reservation isn't there still goes to the interpreter. Host instructions
  -1.2% (8.5 and 9.1 CDs).
- **`lmw`/`stmw` running into the next page** (JIT speed) left the block
  for the interpreter: in the installed 9.2.2's start, 178,000 of each (a
  stack frame's register saves across a page boundary), 357,000 of its
  416,000 exits to the interpreter. Both hosts now check the next page's
  flags too, as `mem_ok()` does (and not across 4 GB): 44,000 exits left;
  the CD boots' fell by 22-34%. Most of what's left (8.5 CD: 24,700 of
  the first 30,000) are the 68k emulator's `stb` into the NanoKernel's info
  records at `0x5FFFEC0E`, which the kernel keeps read-only to see them
  (`kdp_pull()`): the kernel's to make in place, if anyone (since done:
  "Stores to the info records", below).
- **JIT speed, before and after** (medians of 5 alternating runs; M5 user
  time, the Zen 5's wall time; cycles and host instructions alongside):

  | | before | after |
  |---|---|---|
  | M5, 8.5 CD | 1.02 s, 4.38 G cycles, 24.70 G instructions | 0.98 s, 4.28 G, 24.10 G |
  | M5, 9.1 CD | 1.34 s, 5.89 G, 30.68 G | 1.33 s, 5.84 G, 30.34 G |
  | M5, 9.2.2 CD | 1.10 s, 4.88 G, 15.85 G | 1.09 s, 4.83 G, 15.82 G |
  | M5, 8.5 disk (4 billion) | 1.26 s, 5.54 G, 23.87 G | 1.25 s, 5.46 G, 23.54 G |
  | M5, 9.2.2 disk (6 billion) | 2.18 s, 9.45 G, 40.60 G | 2.17 s, 9.30 G, 40.30 G |
  | Zen 5, 9.2.2 CD, gcc | 1.217 s, 6.36 G, 15.63 G | 1.210 s, 6.33 G, 15.59 G |
  | Zen 5, 9.2.2 CD, clang | 1.259 s, 6.60 G, 15.96 G | 1.233 s, 6.45 G, 15.92 G |

  Exits to the interpreter fell (8.5 CD 49,076 → 38,549; 9.1 33,535 →
  22,386; 9.2.2 28,944 → 22,713; 8.5 disk 57,209 → 41,546; 9.2.2 disk
  415,068 → 44,271); the other kinds are as they were. The profiler's
  split (blocks, stubs, Blue, elsewhere) on the M5: 8.5 66.9/1.4/28.5/3.2%
  → 69.0/1.3/26.6/3.1%, 9.1 62.4/0.9/34.6/2.1% → 63.1/1.0/34.3/1.6%, 9.2.2
  77.5/0.7/19.2/2.6% → 76.2/0.8/20.0/3.0% (the old macOS profiler had said
  53.8% blocks for 8.5). The synthetic benchmark is as it was on both (its
  loops use none of what changed). The Zen 5 had only the 9.2.2 CD (one CD
  copied over the link), which gains least: it runs few `lwarx`, device
  registers or `lmw` across pages.
- **The context switch** (kernel speed): `switch_context()` copied the 32
  GPRs and 32 FPRs each way one at a time (nk.c said they vectorized; they
  didn't), about 450 host instructions a switch: of the 23 ns a kernel
  call in place cost, 9 went to saving and 9 to loading. NEON now copies
  four GPRs or two FPRs at a time on AArch64 (lanes byte-reversed; a GPR's
  doubleword in the context block is written back whole, its high word as
  it was), and AVX2 four of either on x86-64 where the processor has it
  (found at run time: the C stays baseline, and without AVX2 the copies
  are as they were): 6 ns each way, 17 a call. Copying less was ruled out:
  context blocks are guest memory, which the guest reads and writes (the
  system context's is on the 68k emulator's hot data page), so switching
  by pointer or lazily would take page protection that faults all the
  time, and finding what changed costs what copying does. 9.1 CD on the
  M5: host instructions 30.34 → 28.41 billion, user time 1.34 → 1.27 s;
  the 9.2.2 CD on the Zen 5: instructions -3%, cycles -1% (gcc) and -2%
  (clang).
- **Kernel calls while devices are dirty** (kernel speed): an exit that
  may have touched the devices (the idle hook's, a host call, a kernel
  call that exits, a sync point) leaves them to be brought up to date at
  the next exit, and until then no kernel call was made in place, so the
  68k emulator's next RunAlternateContext exited for that alone. Bringing
  them up to date is guest-visible (above), so it still happens at that
  instruction, now in the call: `kcall()` gets the instructions the run
  hasn't counted yet (`done`, as the mmio hook does; the fuzzer checks
  that both engines say the same), and `kcall_in_place()` brings the
  devices up to date at that instruction count and time base, then makes
  the call unless an interrupt changed or the loop has more to do (then it
  exits, the devices as that exit would have had them, and not due
  again); not while DMA or sound moves (their next event is now). Kernel
  exits: 8.5 CD 1,135,540 → 153,052, 8.5 disk 833,639 → 219,317, 9.1 CD
  1,666,186 → 1,480,003. The 8.5 CD's host instructions fell 0.8% and its
  time about 2%, not the 5% estimated above: that counted the device
  updates, which stay; only the exits' own cost went. The update is out of
  line (`kcall_dirty()`): inlined, it gave every call a frame of 12
  registers (9.1: +0.6% instructions).
- **Stores to the info records** (kernel speed): most of what still went
  to the interpreter were the 68k emulator's byte stores (`stb` at
  `0x6806628C`) to `0x5FFFEC0D` and `0x5FFFEC0E` in the InfoRecord page,
  which the kernel keeps read-only to see them, making each at an exit
  with the page writable and pulling the change into its copy of
  KernelData (`kdp_pull()`). The mmio hook makes them the same way now
  (`info_store()`: the store, the pull, the devices marked dirty as that
  exit left them), unless a software interrupt, task switch or preemption
  waits, or it's a store of another form or across a page; generated code
  offers the hook stores that fault on a page that isn't writable, not
  only accesses to pages that aren't readable. Exits to the interpreter:
  8.5 CD 38,549 → 5,450, 9.1 22,386 → 7,493, 9.2.2 22,713 → 13,120, 8.5
  disk 41,546 → 9,065, 9.2.2 disk 33,241 → 11,061.
- **Quick system calls while devices are dirty** (kernel speed; tried and
  dropped): most of NanoKernel 2's quick `sc`s that still exit are the
  first after a real one, which leaves the devices dirty (9.1: 560,000 of
  660,000). Answering them in place the same way (a hook that brings the
  devices up to date, then generated code going on through the block
  table) took 9.1's kernel exits from 1.48 to 0.89 million (the 9.2.2
  disk's from 0.95 to 0.69) and its host instructions down 0.6%, but
  cycles moved less than the noise (9.1 -0.1%, medians of 5; the 9.2.2 CD
  on the Zen 5 +0.3% with gcc, -0.7% with clang): the device update, which
  stays, is most of what such an exit costs. Not worth a hook in both code
  generators.
- **The interpreter's synthetic benchmark moves with code placement**:
  `BLUE_JIT_BENCH=1`'s interpreted kernels ran 20-30% faster or slower
  between builds that don't change the interpreter (on the M5 the copy
  kernel ran about 500 million instructions a second after the context
  switch commit, which changes only nk.c, and about 700 before it and
  after the next one); the Zen 5 shows it with clang, not gcc. The
  translated kernels don't move. Its interpreter numbers say little about
  changes elsewhere.
- **Kernel speed, before and after** (main as of 5e5e1bf, then with the
  three changes above; medians of 5 alternating runs on the M5, user time;
  of 11 on the Zen 5, wall time; cycles and host instructions alongside):

  | | before | after |
  |---|---|---|
  | M5, 8.5 CD | 0.94 s, 4.19 G cycles, 24.09 G instructions | 0.91 s, 4.08 G, 23.29 G |
  | M5, 9.1 CD | 1.32 s, 5.83 G, 30.46 G | 1.26 s, 5.56 G, 28.56 G |
  | M5, 9.2.2 CD | 1.08 s, 4.84 G, 15.84 G | 1.06 s, 4.75 G, 15.38 G |
  | M5, 8.5 disk (4 billion) | 1.19 s, 5.36 G, 23.52 G | 1.18 s, 5.30 G, 22.96 G |
  | M5, 9.2.2 disk (6 billion) | 1.78 s, 7.91 G, 34.72 G | 1.76 s, 7.82 G, 34.07 G |
  | Zen 5, 9.2.2 CD, gcc | 1.319 s, 6.85 G, 15.59 G | 1.309 s, 6.80 G, 15.13 G |
  | Zen 5, 9.2.2 CD, clang | 1.281 s, 6.68 G, 15.92 G | 1.237 s, 6.45 G, 15.48 G |

  Kernel exits: 8.5 CD 1,135,540 → 153,052, 9.1 1,666,186 → 1,480,003,
  9.2.2 492,580 → 380,048, 8.5 disk 833,639 → 219,317, 9.2.2 disk
  1,141,014 → 949,367; exits to the interpreter as above; the other kinds
  as they were. The profiler's split (blocks, stubs, Blue, elsewhere) on
  the M5: 8.5 69.7/1.6/26.1/2.6% → 71.8/0.7/25.3/2.2%, 9.1
  64.3/0.8/33.0/1.9% → 67.5/0.9/30.0/1.6%, 9.2.2 77.3/0.7/18.8/3.1% →
  78.2/0.6/18.8/2.4%. The synthetic benchmark's translated kernels are as
  they were. The Zen 5 was shared (wall times spread ±5%; a run under
  heavier load gave gcc -3.2%, clang -3.6%).
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
  than such a run's clock is flagged the same way (`blue disk create`
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
- **Running code of Blue's in Mac OS on demand** (installing the guest
  tools): the Toolbox trap table is in low memory as on 68k Macs (entry
  `0x0E00 + 4 × (trap & 0x3FF)`), and the 68k emulator dispatches through
  it on every call, interpreted or recompiled. `_SynchIdleTime` (ABF7),
  where the idle hook stops, is called from a `_SystemTask` patch in the
  system heap (r24 points 8 bytes into `_SystemTask`'s entry, on 8.5 and
  8.6): application time, in the current application's context (often
  the Finder's), inside its WaitNextEvent. At that hook Blue points the
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
  calls, FSWrite and FSClose all work there, and so did
  LaunchApplication (`launchContinue | launchNoFileFlags |
  launchDontSwitch`) of a background-only application: Enhancements ran
  at once (its launches counted 2 in the run's statistics), until installs
  restarted Mac OS instead (below). Disk First Aid
  finds the volume sound afterwards. (hfsutils' `hdel` on an installed
  8.6 disk leaves something it can't repair; `hattrib` doesn't.)
- **A running Enhancements can't be replaced** (its resource fork is open:
  `fBsyErr`), so Blue has it quit first (`helper.quit`: its main
  routine returns at its next WaitNextEvent, as for a quit Apple event)
  and goes on once it's gone (0.1 s later; 5 s at most). The restart
  that follows an install doesn't change that (the file has to be
  replaced before it): on 8.6, without the quit, FSpDelete of the running
  older copy gave `fBsyErr` ("it's in use"), nothing was installed and
  Mac OS didn't restart. 9.2.2 starts Enhancements only after the Finder
  is up, so an input script's early `tools install` found it not yet
  running at the answer, and it was running by the time the installer
  went in (`fBsyErr` again); Blue now asks it again just before going
  in. If it can't be replaced after it quit, it isn't started again
  (Mac OS doesn't restart either): it starts with Mac OS next time.
- **Enhancements is in use before it runs:** the Process Manager opens
  its file as it launches it, seconds of guest time before its main
  routine first runs (when `helper.running` becomes true): on an
  installed 8.5, from the first idle call (67.5 s of guest time) to its
  main routine (74.4 s); on 9.2.2 (MacOS9Lives) from 118.6 s to 137.7 s,
  and there the Finder is up, and the offer made, about 15 s before it
  runs. An answer in that gap found Enhancements "not running", and the
  installer's FSpDelete of the old copy gave `fBsyErr`: "couldn't update
  Blue Enhancements (replacing the old copy): it's in use", nothing
  installed, no restart (the reported failure). Reproduced every time on
  copies of an 8.5 and a 9.2.2 with an older Enhancements (installed by
  an older Blue): the menu's offer answered at instruction 1.0-1.195
  billion on 8.5 (Enhancements first ran at 1.205 billion), and on 9.2.2
  the Finder's offer answered at 2.0 billion (it ran at 2.22 billion).
  Now "running?" is decided where it matters, as the old copy is
  replaced: on `fBsyErr` there (Enhancements only, once) the installer
  deletes its temporary file and stops, leaving everything as it was
  (Enhancements goes first, so nothing else is done yet); Blue sets
  `helper.quit`, which one that's being launched sees at its first
  WaitNextEvent, waits for it to have quit (`helper.quits`, a count; up
  to 60 s for one that hasn't started, 5 s for one that runs), and the
  installer goes in again and starts over. One that had quit already
  (asked before the installer went in) isn't asked again: its process
  just hadn't gone. Still in use the second time: said as before, no
  restart. Both reproductions now update everything and restart, as do
  answers at every point tried around Enhancements' start (8.5: 1.0 to
  1.3 billion; 9.2.2: 1.8 to 2.21 billion). Test:
  `test_installer_busy()` in `tests/test_install.c`.
- **Restarting Mac OS after an install** (all three of 8.5, 8.6 and
  9.2.2): the Process Manager takes the Restart Apple event (`aevt`
  `rest`, kCoreEventClass and kAERestart) sent to the system process
  (`typeProcessSerialNumber` {0, kSystemProcess}, `kAENoReply`), from the
  installer in the idle trap's context, and restarts as Special > Restart
  does: each program is asked to quit (SimpleText with a changed document
  asked "Save changes to the document … before closing?"; Don't Save, and
  the restart went on; Cancel, and Mac OS went on, the tools in place).
  The Finder ignores `aevt` `rest` (nothing happened on any of the three),
  but takes its own, `fndr` `rest` (its `aete`'s "restart", what
  AppleScript sends) and the System 7 Finder Events' `FNDR` `rest`; the
  Process Manager's doesn't need the Finder. 8.5 and 8.6 then restart
  through RTAS `system-reboot`, 9.2.2 through the Cuda, and Blue starts
  the machine over (`restart_machine()`): the check finds the tools
  current, Enhancements runs (one launch), QuickDraw 3D's Teapot in
  SimpleText calls the engine (19 calls on 8.5 and 8.6) and Blue's GL
  Test draws with Blue's OpenGL (8.5, 8.6, 9.2.2). The installer flushes
  the volume (FlushVol) before it asks, and asks only if everything
  asked for was done. The box with the outcome stays through the restart,
  until OK (a question is taken down: it belonged to that session). After
  a Put Back, Blue's OpenGL isn't offered again in that run (the disk
  looks just like a 9.2.2 whose tools came before Blue's OpenGL did), so
  the restart doesn't bring the opposite offer straight back. CDs stay in
  their drives through the restart, on 8.5, 8.6, 9.1 and 9.2.2, and so do
  floppies (see "A restart or shutdown keeps CDs and floppies in").
- **RAVE finds engines per program:** QuickDraw 3D RAVE looks through the
  Extensions folder when a program loads it, so programs opened after the
  3D engine is installed find it without a restart, even after an earlier
  program loaded RAVE (SimpleText opening the Teapot from the QuickDraw 3D
  folder, quit, the engine installed, opened again: 19 calls to the
  engine). Programs already running don't. (Installs restart Mac OS now
  anyway, for Enhancements and OpenGL.)
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
  and 'ejec' at a restart or shutdown (which the drive answers as the
  CD-ROM drives do, so the disk stays in: see "A restart or shutdown
  keeps CDs and floppies in").
  Disk Copy
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
  or put in later.
- **A restart or shutdown keeps CDs and floppies in** (DriverGestalt 'ejec'): a
  restart (Special > Restart, the Restart Apple event the guest tools'
  installer sends) or a shutdown runs the ShutDown Manager's queue. Two of
  its procedures, ROM code, go through the volume queue: one for
  sdOnBootVolUnmount (every volume but the startup disk's), then one for
  sdOnRestart or sdOnPowerOff (the rest). Each unmounts the volume (it may
  stay, busy) and asks its drive's driver DriverGestalt 'ejec' (ioVRefNum
  0, so the same answer for every drive of a driver; only of one that's
  open, with Driver Gestalt on in dCtlFlags), then calls Eject (`_Eject`,
  which sends Control 7) unless the answer says not to. Nothing else asks
  'ejec': Cmd-E, Put Away and the Trash send Control 7 without it (8.5 and
  9.2.2). Which bit stops it depends on the ROM: 8.5's (the CD's and
  `MOS.rom`), 8.6's and 9.1's test the one Apple's headers name for what's
  happening (bit 0, kRestartDontEject, at a restart; bit 1,
  kShutDownDontEject, at shutdown; either, in the first procedure),
  9.2.2's (10.2f1) the other one, and 9.2.2's System replaces the first
  procedure with one that tests bit 1 whichever it is. Blue answered
  kRestartDontEject alone, for swappable CD-ROM drives (and the floppy
  drive), so 9.2.2 ejected a CD at a restart (the reported bug: the drive
  was empty after it; seen with the guest tools' restart and Special >
  Restart) and 8.5 at shutdown (the startup CD too). Every CD-ROM drive
  now answers both bits (3): on 8.5, 8.6, 9.1 and 9.2.2 Mac OS no longer
  calls Eject for a CD at a restart or shutdown, and it's mounted again
  after the restart. As an iMac does: one owner (an iMac DV, Mac OS 9)
  asked on Info-Mac whether it was normal that "the disc is not ejected
  before a restart or after closing the computer, like it used to be".
  Fixed drives answer the same (their driver refused Control 7 anyway).
  For a while the floppy drive didn't answer, so Mac OS ejected a floppy
  at every restart and shutdown, as from a Mac's own floppy drive (Inside
  Macintosh's Shutdown Manager ejects every volume's disk), and the next
  of several `--floppy` images went in; before that it answered
  kRestartDontEject alone (8.x kept the floppy in at a restart, 9.2.2
  didn't). It now answers both bits too, so a floppy stays in and is
  mounted again after a restart, and nothing of the `--floppy` queue goes
  in (a disk set's installer that restarts finds its disk still there).
  Checked headless with two `--floppy` images: Special > Restart on an
  installed 8.5, 8.6, 9.1 and 9.2.2 (Mac OS asked the floppy drive
  'ejec', made no Control 7, and the first disk was mounted again after
  the restart, the second not in); Command-E on 8.5 and 9.2.2 (ejected,
  the second disk in a second later), then Special > Shut Down (it stayed
  in). Checked headless
  with ejectable drives (a `cdrom` command in the script, never reached):
  the tools' install on 9.2.2 (MacOS9Lives, `91.iso` in a CD-ROM drive),
  an installed 8.5, 8.6 and 9.1 (`922.iso`); Special > Restart on 9.2.2
  and on the 8.5 CD (`922.iso` in a second drive); Special > Shut Down on
  the 8.5 CD; Cmd-E still ejecting on 8.5 and 9.2.2. The menu's Restart
  doesn't involve Mac OS: `restart_machine()` starts the machine over with
  the drives' discs as they are. Test: `test_shutdown()` in
  `tests/test_drives.c` (both ROMs' tests on each drive's answer).
- **Each application has its own scrap:** its scrap handle is in its
  heap, and the low-memory scrap globals (`0x960`) are switched with it;
  the Process Manager hands the front application's scrap to the next one
  brought to the front. A background application never sees another's
  copy: after SimpleText's Copy on the 8.5 CD, Blue Enhancements'
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
  a pixel; 1 and 32 (direct) fail. Blue asks for 8: 254 colours (the
  value is the colour's index), 254 transparent, 255 inverting. The
  I-beam is all inverting pixels; the watch has two (its crown) and
  animates through SetHardwareCursor; ObscureCursor (typing) hides it
  with DrawHardwareCursor. With the library's import, the descriptor, the
  colour table and the buffer the driver is 3.8 KB bigger, so it's built
  so only for window runs in real time. (SheepShaver reads the
  CursorImage's pixmap itself, 1-bit cursors only, and guesses the hot
  spot.)
- **The window on a thread of its own** (Task 15, window-speed; M5,
  macOS, window runs in real time; `BLUE_WINDOW_PROFILE=1` measures it).
  Before, the emulation thread did all the window's work at each frame:
  compared the framebuffer with a copy of what was last shown, row by row,
  converted the changed rows into a locked SDL texture, drew and presented,
  and polled SDL's events, the Mac stopped meanwhile. Now:
  - **Threads.** `display_main()` (from `main.c`, for a window) runs
    `nk_boot()` on a thread of its own, with the main thread's stack size,
    and the main thread runs the window, as SDL wants on macOS: it opens
    and closes it when the machine asks (`display_open()`,
    `display_close()` wait for it), waits in `SDL_WaitEventTimeout()`, and
    is woken by an SDL user event the machine pushes (at most one in
    flight). Under one lock the machine hands over a frame
    (`display_frame_begin()`/`_end()`: the changed rows copied into the
    window's copy of the framebuffer, a byte a row marking them, and the
    palette), the pointer, the status bar, the menu's and prompts' texts,
    the drives; asks for the pointer captured or let go, the status bar
    shown or hidden, text put on the host's clipboard; and asks for the
    host's clipboard (it waits for that one, as SDL reads it on the main
    thread). The window converts the marked rows into a 32-bit copy of its
    own under the lock (0.06-0.2 ms a frame of a game at 1024 x 768 x 32),
    then uploads them in bands (`SDL_UpdateTexture`), draws and presents.
    Its events go to the machine through a queue of 1,024 (moves one after
    another merged into one, as they'd have come to the same place; a
    prompt's answer dropped if that prompt has gone since); the machine
    takes them at its frames, as before. The menu and prompts are the
    window's: it draws the menu at 60 a second while it's open (the
    machine is paused and sends no frames), a prompt with each frame the
    machine sends and as it answers input, and fullscreen and the status
    bar are its own as before. Exposes and lost render targets no longer
    go to the machine: the window has the whole screen converted.
  - **Napping precisely.** The machine naps waiting for input (in
    `nap()`, and while paused). With SDL that was `SDL_WaitEventTimeout()`
    on the window's thread; a condition variable's timed wait (and
    `nanosleep`, `mach_wait_until`) on a thread of a process that isn't in
    front came 15-25% late on this Mac (16 ms became 18.5-19.5; with the
    thread's QoS user-interactive and latency tier 0, about 5%), and the
    window's frames fell from 59.8 to 58.5 a second. A kqueue timer with
    `NOTE_CRITICAL` (which may not be put off to coalesce) came within
    0.02-0.05 ms, so on macOS the machine waits in a kqueue, for that timer
    or an `EVFILT_USER` the window triggers as it queues input; elsewhere on
    a condition variable on the monotonic clock. Frames now come 59.9-60.0
    a second, and a 3D game's (handed over when due) 16.62-16.72 ms apart
    (1st to 99th percentile; main 16.24-17.13).
  - **The framebuffer watched** (`mem_watch()`, `MEM_WATCH` in `mem.h`).
    A watched page is writable but its next write is seen first: it isn't
    `MEM_FAST_W`, so the interpreter's and checked stores take the slow
    way (`mem_ok_w()` → `mem_write_slow()`, or the JIT's `code_store()`
    call, beside its translated-code check, in both back ends; `stmw` and
    `dcbz` go to the interpreter); the host's writes go through
    `mem_ok_w()` or `mem_invalidate()` as for translated code; and the
    JIT's view has the page read-only, where the fault handler, seeing a
    watched page, unwatches it (with the others sharing its host page) and
    returns to the store, which goes ahead, the instruction left as it was
    (fastmem's other faults make an instruction checked from then on, which
    for the framebuffer would have slowed every store to it). Unwatching
    clears the flag on the host page's guest pages and changes the view's
    protection. So a page still watched hasn't been written since it was
    watched, and `video_copy_changes()` compares only the pages that
    aren't, a host page (16 KB here) at a time, with the window's copy,
    copying those that differ. A page that's stayed the same for 8 frames
    is watched again. Costs, measured alone: a fault with its
    `mprotect()` 6.2-7.6 µs per 16 KB page, an `mprotect()` of one page
    0.7-1.8 µs, comparing or copying 16 KB about 1.7 µs; so a page written
    in most frames costs less compared than watched. At first a page was
    watched again as soon as it had stayed the same for one frame: in
    Unreal Tournament every frame the game skipped (the screen the same
    when the window looked) had all 192 of its host pages watched, then
    seen written again by RAVE's frame (a protection change each, with the
    process's other threads to tell): 20-29 a frame. RAVE's and OpenGL's
    own `writable()` invalidated only pages with translated code: their
    frames for the screen went unseen (the window showed a game's screen
    once a second) until they took watched pages too.
  - **Checking.** `BLUE_WINDOW_CHECK=1` compares the watched pages too and
    says where a write went unseen: none in the Finder scenarios below,
    Unreal Tournament's intro (RAVE on the GPU), and a restart. A
    throwaway build also write-protected the framebuffer in the host's own
    mapping while it was watched, so that any write by the host's code
    there faulted with its backtrace: none either. `tests/test_jit.c` watches the data's pages in a quarter
    of its programs (a page still watched must be as it was, either way;
    fastmem must have seen watched writes); `tests/test_video.c` keeps the
    copy by comparing and by watching.
  - **Measured.** Medians of three runs (two to five for the games),
    alternating main (with the profile added) and this; the machine was
    shared with other agents' builds and tests (load average 4-8), and the
    same run gave twice the time at times. Per frame the Mac was handed
    (about 60 a second); "Mac" is the time the Mac stopped for the window
    and its share of the time; "window" the window's thread's time
    (main: none). The Finder: the installed 8.5's disk window dragged
    about by a script (a drag every 0.9 s, 50 moves a second, about a
    third of the screen redrawn when it drops), then idle (the menu-bar
    clock), then the pointer moved in circles (the hardware cursor: the
    framebuffer doesn't change).

  | scene | changed frames a second | main: Mac | now: Mac | now: window | pages seen written a frame |
  |---|---|---|---|---|---|
  | Finder 640 x 480, 8 bits, drags | 24 | 0.141 (0.84%) | 0.022 (0.13%) | 0.13 [0.24] | 0.5 |
  | ... idle | 1 | 0.066 (0.39%) | 0.018 (0.11%) | 0.04 [0.22] | 0 |
  | ... the pointer moving | 1 | 0.328 (1.96%) | 0.024 (0.14%) | 0.12 [0.22] | 0 |
  | Finder 640 x 480, 32 bits, drags | 25 | 0.184 (1.10%) | 0.028 (0.17%) | 0.11 [0.23] | 1.8 |
  | ... idle | 1 | 0.177 (1.05%) | 0.014 (0.09%) | 0.02 [0.11] | 0 |
  | ... the pointer moving | 1 | 0.273 (1.63%) | 0.018 (0.11%) | 0.14 [0.39] | 0 |
  | Finder 1920 x 1080, 32 bits, drags | 25 | 0.514 (3.08%) | 0.076 (0.46%) | 0.20 [0.31] | 5.4 |
  | ... idle | 1 | 1.015 (6.06%) | 0.045 (0.27%) | 0.07 [0.31] | 0 |
  | ... the pointer moving | 1 | 1.174 (7.00%) | 0.051 (0.31%) | 0.28 [0.53] | 0 |
  | UT intro, RAVE on the GPU, 1024 x 768, 32 bits | 60 | 0.756 (4.54%) | 0.205 (1.23%) | 0.90 [1.05] | 0 |
  | UT intro, OpenGL on the GPU, the same | 60 | 0.52 (3.1%) | 0.088 (0.53%) | 0.63 [0.76] | 0 |

  Milliseconds a frame of the host clock; in brackets the window's
  thread's processor time, which counts what SDL does while the thread
  waits for the next frame (it pumps Cocoa's events, 0.1-0.3 ms a wake),
  and in parentheses the share of the time the Mac stopped. Main's Mac
  time is all the window's work, its processor time within 0.05 ms of it;
  the Mac's now is finding and copying the changes (the input takes under
  0.002 ms), and seeing pages written adds their faults (about 7 µs each)
  in the guest's own time. Main's idle and pointer rows at 1920 x 1080
  were mostly comparing 8.3 MB a frame (0.9-1.2 ms); its drags, comparing
  and converting a third of the screen. OpenGL's main row is of two runs.
  The Finder scenes are 1920 x 1080 and 640 x 480 screens in windows their
  size (the window doesn't scale them).

  In the games the screen changes every frame (1024 x 768, 32 bits, 3.1 MB
  a frame: Unreal Tournament's intro, in a window the size of the screen,
  on the GPU): the Mac stops for comparing and copying what changed,
  0.1-0.2 ms, where main also converted, uploaded, drew and presented
  (0.6-1.2 ms); the window takes 0.8-1.1 ms of its own thread, and shows
  each frame 0.7-1.1 ms after it's handed over (at most 1.8 ms). RAVE on
  the GPU: 60.0 frames a second against main's 59.6 (53.1-60.0), the
  frames handed over 16.67 ms apart with a standard deviation of 0.35-0.54
  ms (main 0.36-11.3); UT's OpenGL build: 60.0 against 60.0, OpenGL's
  frames for the screen 16.67 ms apart (deviation 0.9-1.2 ms; main
  1.2-1.4). In software (`--no-gpu`, one run each) the game's frames rose
  from 36.1 to 39.4 a second, the Mac's processor no longer shared with
  the window's work. A first version also drew a prompt on a 16 ms timer
  of its own, between the frames: with the news of the guest tools'
  update over the game (it stays through the restart), 75-83 draws a
  second where frames came 50-60, and the game's GPU frames fell to 49-55
  a second, the window's presenting taking the GPU from them; drawn with
  the frames, as main drew it, they're at 60.
  - **Sound** in those runs (UT's music, in real time): no frames
    dropped, and 2,816 frames of silence added at the start, as in main's
    runs (one of main's added 30,976, in a stall of the host).
  - **The window's own costs** (its thread): `SDL_UpdateTexture` of a
    whole 1024 x 768 frame takes 0.4-0.8 ms with Metal (a buffer made and
    filled each time), against the old locked texture's 0.04-0.08 (where
    the conversion's 0.35-0.6 ms included the locked buffer's first
    writes). The window's thread waits in `SDL_WaitEventTimeout`, which
    pumps Cocoa's events for 0.04-0.3 ms of processor time a wake.
  - **Instruction-time window runs** go as main's: the 8.5 CD's
    `--window --instruction-time --input` boot to 4 billion instructions
    has main's log (but the `jit:`, `idle:` and screen lines) and
    screenshots, with the pages watched and the JIT's faults on them
    (unscripted, the host pointer's moves over the window are input, and
    no two runs are alike, main's either). Headless runs watch nothing.
  - **Window captures** (`screencapture -l`, the window alone) of the same
    scripted runs at the same moments, main's and this: the 8.5 CD's Finder
    with and without the status bar and with `--integer-scale`, the
    installed 8.5's guest tools offer over the screen, and its disk's
    window at 1280 x 1024 in millions of colours are the same to the
    pixel, but the status bar's processor light (whose brightness is the
    share of the time the Mac was busy, less now).
  - **Linux** (arm64, in a podman container: Ubuntu 24.04's SDL 2, gcc
    13, Xvfb without a window manager, xdotool and xclip for input): no
    warnings in Blue's files (gcc's `-Wformat-truncation` in `install.c`,
    main's, is gone too: `report_never()` writes its news with `add()`, as
    `report()` does), the tests pass, and in windows: the 8.5 CD's Finder; F12's menu (with its
    Install Guest Tools...), driven by the mouse and the keyboard
    (Screenshot and its news, Fullscreen on and off, Resume, Turn off,
    which ends the run as before); clicks and drags in the guest; the
    window resized, the screen scaled to it; on the installed 8.5 the
    guest tools' offer with Don't Ask Again, answered Not Now, and Update
    and Restart with its news after; a resize that Enhancements followed
    (to 1024 x 768); and the clipboard both ways (the host's text into Mac
    OS at startup; a Finder copy, after the menu's Restart, on X11's
    clipboard as "Macintosh HD"). That last found a restart turning the
    clipboard off for the rest of the run (`restart_machine()` didn't keep
    it, on main too): it's kept now. Fullscreen without a window manager
    leaves the X window its size (the renderer draws for the screen's);
    dropped files weren't tried (xdotool can't drag), nor sound. Frames
    came 57-58 a second there (the container's timers).
  - **ThreadSanitizer** (macOS, `make BUILD=build/tsan CC='cc
    -fsanitize=thread -g' CXX='c++ -fsanitize=thread -g'`) found no races
    in three window runs: the 8.5 CD's `--window --instruction-time` boot
    with scripted clicks, a window drag and keys (8 s); the installed 8.5
    in real time for 24 s with the status bar, the guest tools' offer
    answered Not Now, a click, a drag and a key, and F12's menu opened and
    closed twice with its Screenshot taken in between (that input made in
    the window's thread as SDL's events would be, by a scratch build, as
    the host's own input was the user's); and Unreal
    Tournament's intro with RAVE on the GPU (the guest tools installed and
    a restart first, 52 s; 35-38 frames a second under TSan). (A small
    racy program gets its report, so TSan works there.) SDL 2 on macOS is
    Homebrew's sdl2-compat, whose constructor `dlopen()`s
    `libSDL3.dylib`, a bare name that dyld finds through the caller's
    rpath (sdl2-compat's leads to Homebrew's sdl3); under TSan the caller
    is TSan's `dlopen()`, so SDL 3 isn't found and sdl2-compat aborts:
    `DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib` finds it (macOS drops
    `DYLD_` variables for its own programs, so with `/usr/bin/timeout` it
    goes after: `timeout 60 env DYLD_FALLBACK_LIBRARY_PATH=...
    build/tsan/blue ...`). TSan doesn't see what the JIT's code does, but
    the machine's thread hands everything to the window's in C.
- **The window's look** (ui-platinum; macOS, M-series, a 1x display, and
  Linux x86-64). The menu, prompts and status bar are drawn as Mac OS 8's
  Platinum, in `site/style.css`'s colours, still with Dear ImGui:
  - **Drawn.** Boxes are Dear ImGui windows without their title bars: the
    frame is its border, and the bevel, the 2-unit shadow, the title bar's
    stripes and the title go on the window's draw list. Buttons and
    checkboxes are `InvisibleButton`s (behaviour, hovering, keyboard
    navigation with `ImGuiButtonFlags_EnableNav`) drawn on the draw list:
    gradients by shading the vertices of a filled shape, the default
    button's ring as nested filled rounded rectangles (black, grey, white:
    exact bands, where strokes are centred on half units), the focus ring
    in Bondi Blue (Dear ImGui's own nav cursor made transparent). Disabled
    items fade with `DisabledAlpha` 0.4: black on Platinum comes out
    #8B8B8B, Mac OS's grey. Lines are a whole number of pixels thick at
    any density (`hair()`), positions snapped to pixels, and windows'
    sizes whole units (a size callback rounding up), so edges are sharp.
  - **Return** is the default button's: it no longer goes to Dear ImGui's
    navigation, which pressed whichever item had the focus, and only once
    the keyboard had shown the focus. Space presses the focused one, Tab
    and the arrows move it.
  - **The font.** ChicagoFLF 2.1 (FontForge's build, 570 glyphs: Latin-1
    and more, … “ ” ’ – — ✓ ⌘ among them; Dear ImGui's own font merged
    for the rest). Its licence is in its own name table (copyright, name
    0: Robin Casady's "You are free to use them as you wish. I consider
    those two in the public domain."; licence, name 13: "Public Domain"),
    copied into `third_party/fonts/README`. It's packed as the icon is
    (LZSS, 50,472 bytes to 31,151). Its glyphs are drawn on a grid of
    1000/12 units (`l`'s stem from 84 to 250, capitals 750 high: 2 and 9
    squares), so at 12 pixels to the em every edge is a pixel's. Dear
    ImGui sizes a font by its ascent to descent
    (`stbtt_ScaleForPixelHeight`: 988 + 200 = 1,188 units), so size 12
    was an em of 10.1 pixels, its stems straddling pixels (and at 13, the
    first try, grey and soft). `ExtraSizeScale` 1.188 (`FONT_EM_SCALE`,
    from the font's `head` and `hhea`) makes size 12 an em of 12 pixels,
    lines still 12 apart, and `PixelSnapH` without oversampling puts each
    glyph on whole pixels: black, sharp Chicago 12, as Mac OS drew it (the
    guest's own menus beside it match), and at density 2 an em of 24.
    Wrapped text has 3 units between lines (15 to a line).
  - **Checked.** Captures by window ID of a clone of the installed 8.5
    without the tools (Install and Restart offered) in real time, before
    and after, with a scratch build that made input in the window's thread
    (the menu opened, the focus moved, Return pressed: the menu resumed;
    the offer answered Not Now). A scratch harness drew the menu, the
    offer and a busy status bar with `ui.cpp` into SDL's software renderer
    (no window) at densities 1, 1.5 and 2: at 2 the text is 24 pixels to
    the em and as sharp; the same harness on Linux (x86-64, gcc 16,
    sdl2-compat 2.32.74 over SDL 3.4) made images identical to macOS's
    (arm64, the same sdl2-compat) to the pixel. The menu is 438 units tall where it was about 510, so it
    fits a 640 x 480 screen's window now. Fullscreen wasn't tried (it
    takes the screen's Space from the user), nor a high-density display.
  - **Cost** (`BLUE_WINDOW_PROFILE`, the installed 8.5 idle in the Finder
    with the status bar, 15 s of two 25 s runs each, alternating): drawing
    0.085-0.093 ms a draw before, 0.090-0.094 after; presenting 0.23-0.25
    ms both.
- **The window's scale** (screen-scale; macOS 27, M5, with a 49-inch 5120
  x 1440 display at 1x and the built-in 13-inch at 2x, made to look like
  1710 x 1112; Homebrew's sdl2-compat over SDL 3):
  - **Densities.** CoreGraphics on this Mac: the 49-inch 1193.1 x 338.7 mm
    (`CGDisplayScreenSize`) and 5120 x 1440 pixels, 109 pixels per inch
    (Auto: 1x); the built-in 290.5 x 188.9 mm and 3420 x 2224 pixels in its
    scaled mode (`CGDisplayModeGetPixelWidth`: the backing store's, which
    is what the window draws into, not the panel's 2560), 299 (4x). SDL's
    display bounds on macOS are CoreGraphics' (`CGDisplayBounds`), so the
    display is found by the middle of its bounds
    (`CGGetDisplaysWithPoint`). A display without an EDID size gets 72
    dots per inch of its points from CoreGraphics: its own scale again, as
    Auto's fallback would give. sdl2-compat's `SDL_GetDisplayDPI` is made
    up (96 times the content scale and the pixel density), so on Linux
    SDL's figure is taken only from SDL 2 itself (sdl2-compat says it's
    2.x.50 or later in `SDL_GetVersion`); otherwise the kernel's EDID
    (`/sys/class/drm/card*-*/edid`, its first detailed timing's
    millimetres, else its centimetres), of the only display connected or
    the one of the window's display's size, for local X11 and Wayland only.
    On the user's Threadripper (Linux, a 3840 x 1600 display on DP-3) it
    reads 880 x 367 mm: 110.8 pixels per inch (1x).
  - **Auto's rule:** the scale nearest in proportion to 80 pixels per
    inch, s rather than s + 1 up to 80 sqrt(s (s + 1)): 1x to 113, 2x to
    196, 3x to 277, 4x above. Nearest by difference would put a 27-inch
    1440p display (109) at 2x (54.5 is nearer 80), where 1x is meant.
    Plausible sizes: pixels square to 10% (turned displays allowed for), a
    diagonal of 10 to 150 inches (a television giving 16 x 9 cm for its
    shape is 7 inches), 20 to 1000 pixels per inch.
  - **The screen asked for** is the window's pixels (above the status
    bar) over the scale, rounded down; the driver rounds that
    (`video_fit_size()`, `video_request_size()`'s rule as before): the
    width down to a multiple of 8, at least 512 x 384, and smaller keeping
    its shape until 16 MB holds it at 32 bits (a 4K window at 1x: 2584 x
    1464, filled). A screen that is the window's own (what's left over under
    8 x the scale across and the scale down) is drawn at exactly the scale;
    any other is filled as before, so while the window is dragged, or
    without Enhancements once resized by hand, it's scaled. The window
    knows whether the screen follows it (`display_follow()`, from the
    helper's running): a new scale, or density, then asks for a screen;
    otherwise the window takes the screen's size at it.
  - **The window's frame:** `SDL_GetWindowBordersSize` fails on macOS
    (sdl2-compat, SDL 3), hidden or shown, so a window's room is its
    display's usable part less a title bar of 32 units, macOS 27's (a
    window 1377 high has a frame 1409 high). Without it a 3x window of an
    800 x 600 screen on the 1440-high display came out 1879 x 1377: macOS
    cut its height, not its width. Now 1804 x 1377 (the screen at 2.25x,
    filled, its shape kept), the status bar's 24 taken from the screen's
    part when it comes. The window is made hidden, sized, centred on its
    display and then shown, rather than shown and resized.
  - **Checked** with window captures by ID, a scratch build stepping the
    window's thread (resizing the window with the event SDL sends for a
    user's resize, choosing in the menu, moving the window): the installed
    8.5 (a clone of base85t, its tools current), Auto 1x on the 49-inch: a
    2400 x 1224 window asked for 2400 x 1200; 2x then 1200 x 600, 3x 800 x
    400, each drawn at exactly that (every 2 x 2 or 3 x 3 block of the
    capture one colour, but the frame's line over the first row);
    `--scale 2` opened the window at 1600 x 1224 for 800 x 600, and Auto
    chosen in the menu (paused) asked for 1600 x 1200 once resumed; the
    menu's Scale row (Auto (1x), 2x chosen). The 8.5 CD (no Enhancements)
    with `--scale 3`: the window 1804 x 1377 within the display; 1x 800 x
    624, 2x 1600 x 1224, resized by hand filled, 3x within the display
    again; with `--integer-scale` that 3x window opened at 2x (1600 x
    1224) and, resized by hand to 1000 x 700, showed the screen at 1x with
    a border. Moved onto the built-in display: 299 pixels per inch, Auto 4x,
    so 512 x 384 (the driver's smallest) at 4x in a window of 1024 x 792
    points, the menu at density 2 with Auto (4x); 3x 680 x 512; back on
    the 49-inch (3x chosen) 512 x 384 at 3x.
  - **Not checked:** a 5K iMac's and a 4K display's own figures (Auto's
    choice for their densities is in the tests), Linux at run time (built
    with gcc 16 `-Werror`, the tests passed, and the EDID read on the
    Threadripper; no window), Wayland's and X11's SDL 2 figures, a display
    without an EDID size, fullscreen.
  - **Cost** (`BLUE_WINDOW_PROFILE`, the installed 8.5 idle at the Finder
    at 1x, 10 s of three 20 s runs each, alternating): drawing 0.037-0.100
    ms a draw (medians) both, presenting 0.09-0.25 ms both, events
    0.001-0.003 ms a frame both (the same build gave either). The
    display's density is looked at as the window moves or changes size or
    display, and once a second (1.5-10 µs: sdl2-compat's
    `SDL_GetWindowDisplayIndex` is most of it).
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
  other steps). For ROM files with parcels Blue now gives those versions
  and lengths, still in its one page (HWInfo at +0xE60, ProcessorInfo at
  +0xF20, the rest where NanoKernel 1's are), CascadeInfo with no
  controllers (boot-info area +0x51100), the checksum and RAM disk fields 0,
  and the clock rates. The 9.1 CD's run is unchanged by it.
- **What 9.2.2's ROM reads there:** its 68k startup (`0xFFC0F0AC`, the
  only new use of NKHWInfoPtr) does `movea.l ([$68FFEFD0],$A8),a1; tst.l
  (a1)`, and with a count patches handlers from a table ProductInfo +0x50
  leads to. With Blue's HWInfo 0x0103/0xA0, +0xA8 was SystemInfo's
  logical RAM size, read as a pointer: exception 18 at the end of RAM at
  instruction 954,597. A System fragment checks ProcessorInfo's version
  (at least 0x0111) before starting 'accu' (below).
- **Calls 9.2.2 makes that 9.1 doesn't** (QEMU, `-d int`, 75 s): 62
  `MPGetNextCpuID` (from MPLibrary, `0xFFCD6ED0`: coherence group r3, 0 the
  current processor's; previous CPU r4 → next r4; kMPInvalidIDErr after the
  last, r4 left), 132 `NKSetClockDriftCorrection` (parts per billion r3; 0
  turns it off: then nothing but noErr; else the kernel's drift timer steps
  the timebase by one tick every so often; kMPInsufficientResourcesErr
  without that timer or with more than one processor scheduled. Blue's
  timebase keeps time: noErr), 95 `MPFindVictimPage` (86 times: QEMU's free
  page count went low; Blue's doesn't), and `sc` -3 (16,815 times, all
  from the kernel's own code at physical `0x00F15920`). No 11
  (`MPIsTaskBlue`): Mixed Mode's `sc` -1 does it. Later, idle, 44 and 45.
- **The startup objects** (`NKInit.s`, and QEMU's index): process 1,
  coherence group 0x00010001, CPU 0x00020001 (Flags 0xF: scheduled, and
  bit 31, the processor the kernel started on), address space 0x00030001,
  page queue 0x00040001, blue task 0x00050001, then semaphores, the idle
  task, another address space and the areas. Blue now makes the first six
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
  in the floppy controller's (SWIM3) cell. Blue had nothing there: the
  task faulted (exception 18 in a task: Task 4). The cell now reads zeros
  (`setup_machine()`, `empty_cell_ops`, for ROM files with parcels).
- **The ROM file's lanLib for `bmac+`** (9.2.2's parcel flags 0xC, blob
  flags 6, not keep-existing) took the place of Blue's in the Ethernet
  node, and its first store to the BMAC registers (Mac I/O +0x11180) made
  the Finder quit. `build_tree()` puts Blue's back after the parcels.
- **The power manager when idle** (9.2.2's CD): after half an hour of guest
  time without input, PowerDispatch 9 once, then PowerDispatch 1 (doze
  until an interrupt) in a loop, with CacheDispatch (kernel call 7) and r3
  = 4 now and then. Both NanoKernels (`MOS.rom` `0xFFF15F78`, 10.2f1
  `0xFFF1A8CC`) answer a selector above 2 with -2 in the low half, the high
  half being the caller's r23's (their return path ANDs it in); Blue does
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
  running emulator's CR). What differs is likely when timers fire (Blue:
  at sync points, every 20,000 instructions) and that a real iMac has a
  periodic external interrupt, the display's VBL, which would end the
  native code within 16 ms, while Blue's VBL is a Driver Services timer
  that itself needs the Time Manager. Options: a VBL interrupt from the
  display device (`video.c`, its node's interrupt set), or timers firing at
  their exact time in instruction time. **Resolved** by the first (Task
  17); see the next.
- **The display's interrupt** (Task 17): on the iMac the ATI chip's PCI
  interrupt goes to Paddington's source 22 (MAME's `imacg3.cpp` wires its
  Rage IIc there; its OHCI is source 28, as Blue's, and NetBSD on a Bondi
  iMac says the same of the OHCI; QEMU's g3beige wires Grackle's four PCI
  lines to Heathrow's 21-24). Real NDRVs install their handler in the
  node's `driver-ist` set and either call VSLDoInterruptService there
  (QemuMacDrivers' QemuVga, `USE_PCI_IRQ`) or from a secondary handler, as
  Blue's does; VSLDoInterruptService runs the screen's VBL tasks (the
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
  0xC010 ORed back in, and back into the emulator (r30 | 0x6084). Blue's
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
- **MacBench's CD-ROM test** (macbench-cd; 8.5 and 9.2.2 installed, the
  MacBench 5.0 CD in a drive, MacBench in the shared folder): the test
  reads its CD only through the File Manager: 14,673 synchronous PBReads
  (`_Read`, none async), 148 MB, 512 bytes to 64 KB each, replaying five
  CD titles; the inspection tests read one big file (Random Read 1024K:
  554 reads of 1 MB). Below the File Manager MacBench only takes an
  inventory of the volumes, at each test's start and for System
  Information's Storage Devices (68k code in its heap): PBHGetVInfo for
  each, DriverGestalt `devt` and `intf` of its driver, Control 23,
  OpenDriver(".AppleCD") and Status 97 (WhoIsThere: statusErr). By
  `devt`/`intf` it picks how to identify the drive: `disk`/`ata ` and
  `cdrm`/`atpi` through the ATA Manager, `scsi` through SCSIAction 0x86
  (GetVirtualIDInfo) and then the old SCSI Manager, bus 0 ID 0 when that
  fails: SCSIGet and SCSISelect (5 tries) for an INQUIRY, a MODE SENSE of
  pages 3 and 4 (geometry), the capacity from PBXGetVolInfo. Every select
  fails at once on Blue (no bus), so the drives show as SCSI 0.0.0 with
  no vendor or model; a real iMac's CD answers `atpi` (StartLib needs
  `scsi`, see "The Start Manager's wait"). It finds its CD by a file on
  it. The test completes: 8.5 1,442 (16.6 MIPS), 9.2.2 1,185 (16.6 MIPS),
  4,261 (`clock 400`), 5,366-5,383 in real time; also with `clock`
  100-3000 and other `--fixed-time`s, fixed and ejectable drives, in a
  window with sound, six times in a row, after the Disk and Video tests,
  after QuickTime Player played a movie and while it kept playing (main
  before the transfer waits), cancelled, across a mode switch, with the
  host's clipboard coming in, with the process stopped for seconds
  (SIGSTOP), the user's own shared folder, MacBench run from the CD; the
  heaps intact after each (`tools/heap.py`), and a debugging build
  checking every disk and CD read and DBDMA store against the heap's
  blocks found none outside an allocated block. The user's freeze (9.2.2,
  a window) didn't reproduce. Its faults decode, matched by call sites
  (`lwz r9,4(r31)` with calls 0xD00, 0x1090 and -0x4B00 away), to the
  Memory Manager's block walk (the Modern Memory Manager's zone: blocks
  from + 0x30 to the last one at + 0x40, the walk from the rover at
  + 0x2C, a block's size at + 8), from `__NewPtr` (r24 0xA71E,
  NewPtrSysClear: the system heap), `__MoveHLow` and `__ReserveMem`, then
  `IterateHashTable` in the Toolbox's HLTB library: the system heap had a
  block header (or the rover) written over. That boot laid memory out
  otherwise: the Memory Manager 0xB70 lower than here (0x307E38 or
  0x3070B8 here, moving with the drives, the NVRAM, the shared folder) and
  MacBench's code at 0x00FFC51C (here 0x00F6xxxx-0x0119xxxx), so the
  session had a history not identified. Open.
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
  ethernet's 0x1E slot 0xE). Blue's table maps slot n to source n, so its
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
  Blue's codec never set it. Two plug-in instances poll on 9.2.2 (1,025
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
- **Open Transport's stall with a full interrupt set for the Ethernet
  node, gone:** with all three of the node's interrupts in its set (MAC,
  transmit DMA, receive DMA, as on the iMac; a scratch build), lanLib put
  its handler in the set's first member, the MAC's, which Blue never
  raises: the receive channel's pulses reached no handler, so no frame
  arrived (DHCP unanswered), though startup didn't stall: an unhandled
  pulse is now released after one more re-evaluation ("Pulses lost at
  PrioritizeInterrupts"), and one taken again and again may have been the
  old stall (not reproduced). lanLib now takes the last member
  (`interrupt_step()` in `src/net.c`): RegistryPropertyGet leaves the size
  as given (24, for a set of one too: logged on the 8.5 and 9.1 CDs), so
  the last entry that isn't empty, the buffer cleared first. Real-time
  startups with networking, fast start's guest time to the Finder, 3 runs
  each, receive channel only against all three: 9.1 CD 15.9-17.1 s against
  16.0-16.7, 9.2.2 CD 13.9-16.2 (and 25.1, the host busy) against
  14.1-15.4, installed 9.1 24.7-24.8 against 25.3-28.2, the restored 9.2.2
  13.3-13.5 against 13.4-14.2; every run received by interrupt, DHCP's 2
  frames each, nothing dropped. No stall either way; the tree keeps the
  receive channel's alone (the only one Blue raises), so the CD checks end
  exactly as before. Tests: `tests/test_net.c` (sets of three and of one).
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
- **Installing 9.1** (instruction time, `--fixed-time 1000000000`): a
  blank 2 GB disk (`disk create`) first, the CD second. In the CD's
  Finder, typing `mac os i` (3 million instructions apart) and Command-O
  opens Mac OS Install (by instruction 5 billion). Then, at 800x600:
  Continue (594 443), Select (594 443; Macintosh HD is already the
  destination, "None Installed", 2046 MB available, 368 MB needed),
  Continue on Important Information and on the license (594 443), Agree
  (530 366), Options... (314 443), the "Update Apple Hard Disk Drivers"
  checkbox (217 226), OK (551 389), Start (594 443) at 11.2 billion. Mac
  OS 9.1 then Internet Access, and "The installation process has
  finished" by 40 billion (268 MB written, 52 MB read). Its Quit (532 348)
  took no click, nor Return, at 40-45 billion, but took the second of two
  clicks 0.3 billion apart, and Return at 50 billion: Energy Saver had put
  the screen to sleep (no doze: 9.1 makes no PowerDispatch calls), and the
  first input only wakes it, as on a Mac. `power`, then Shut Down (541
  188), and the volume is unmounted cleanly (HFS, attributes 0x0100). The
  installed disk starts up alone, with its own ROM file, to the Finder and
  the Mac OS Setup Assistant (Command-Q, then Quit at 541 197).
- **The Startup Disk control panel** (startup-disk; 9.1 installed and
  the restored 9.2.2, with CDs): Startup Disk 9.x lists the volumes with a
  System Folder (Blue's disks and CDs, 8.5's CD too) and on a choice calls
  StartLib's SetSelectedStartupDevice (MOS91.rom's StartLib, code offset
  0x21A8): the drive's DriverGestalt ('boot' fails, 'nmrg', 'dvrf',
  'intf' 'scsi', 'ofpt'), a path built from them, then
  WriteSelectedStartupDevice: /options' `boot-device` set (created if need
  be) with RegistryPropertySetMod's kRegPropertyValueIsSavedToNVRAM (0x20),
  and /chosen's AAPL,boot-device-driveNumber and -interfaceType. Before
  that it checks Gestalt 'nreg' and /openprom's `supports-bootinfo` (code
  0xF0C): without /openprom it took another path whose length came out
  uninitialized (NewPtr of 167 MB: -108), and without /options it couldn't
  set the property. With both, setting it failed with -116: the Name
  Registry writes such properties at once through the ROM's 68k code
  (10.2f1 `0xFFC476D8`, 6.1f9 `0xFFC48A90`), which finds the CHRP
  partition with signature 0x70 named "common" (NVRAMServicesLib's
  NVRAMFindPartition walks 16-byte headers from offset 0, each checked
  with its checksum; Blue's blank NVRAM ended the walk with -116) and
  rewrites its "name=value" strings. Mac OS's PRAM stays at `0x1400`
  (NVRAMServicesLib doesn't find "APL,MacOS75" and falls back there), and
  NVRAM's first 2 KB were unused in every NVRAM file here, so Blue makes an
  empty "common" there (`ofnvram_format()`). Mac OS then wrote
  `boot-device=/pci@80000000/blue,disk@3@3:,\\:tbxi` (after an empty first
  string; its reader skips one), `disk@3@3:8` for the 9.2.2 CD's map entry
  8, the same numbering as Blue's bootpath. Blue reads it at launch (from
  `--nvram`) and at each restart (the NVRAM in memory), maps the PCI device
  to a drive, and starts the ROM file of that partition's blessed folder
  (`hfsread_system_file_in()`), else the first drive's as before; the tree
  has /options (`boot-device` from NVRAM, else the bootpath, so the panel
  shows the current choice) and /openprom. Checked: 9.1 → the 9.2.2 disk
  and back (from 9.2.2's panel), the next launch on the chosen disk, the
  9.2.2 CD among a disk and two CDs; an absent disk falls back with a
  note, `--rom` ignores it. The tree's nodes and the partition change what
  9.1 and 9.2.2 do at startup (9.2.2's CD read NVRAM 352 times instead of
  320, other counts moved; the screens the same but for the caret's
  blink), so they're there only when NVRAM is kept or in real time, and
  the CD checks are unchanged. 8.5's ROM image has no such code; its
  Startup Disk (7.x) lists only the current startup disk (DriverGestalt
  'boot', for PRAM's DefaultStartup, which Blue's driver doesn't answer).
  Choosing the 8.5 CD in 9.1's panel loaded 8.5's ROM file, whose Start
  Manager took the first drive with a System Folder (9.1's: "This startup
  disk will not work on this Macintosh model"); Mac OS numbers drives in
  the tree's order, so the chosen disk's node now goes first for ROM files
  without parcels, and 8.5 starts from the CD.
- **Scripting the 9.2.2 restore** (instruction time): scripted clicks and
  double-clicks didn't select the desktop's icons (main too); typing a name
  in the Finder does (`MacO`, then Command-O opens the CD's window; `apple
  s`, Command-O, opens Apple Software Restore, whose destination is already
  Macintosh HD); the letters must come close together (3 million
  instructions apart; 20 million apart each starts a new name). Return,
  then a click on OK (551 364), restores (done by 39 billion). Its last
  dialog's Quit (494 342) took a click only after a key press, because Mac
  OS was dozing by then (see "Mac OS 9.2.2's doze and the mouse"); now a
  `move` there, then a `click` 30 million instructions later, quits it.
  Special > Shut Down (y 125 in the CD's Finder at 800x600, 136 in the
  restored one's, whose Special menu has Burn CD) then powers off with
  the volume unmounted; or `power`, then a click on Shut Down (541 188).
  The Setup Assistant's first screen quits with Command-Q and a click on
  Quit (569 487 at 1024x768).
- **The restored 9.2.2's first startup without a menu bar** (Task 17's
  open item): reproduced from a blank disk (a scripted restore, the run
  stopped with Apple Software Restore's last dialog up, then the disk
  started and the Setup Assistant quit at its first screen: the desktop and
  its icons, no menu bar (MBarHeight 0), no Control Strip, the Finder
  current and answering clicks). It isn't the volume's unmounted bit:
  setting it on that disk changed nothing, and clearing it on a disk that
  works broke nothing. The disks differed in 12 blocks: the working one had
  been mounted by the CD's Finder (after Apple Software Restore's Quit),
  which made a Desktop Folder and a Trash at its top. Apple Software
  Restore quits the Finder while it works (the restored volume is mounted
  meanwhile, but no Finder sees it) and starts it again as it quits; at the
  first startup the Setup Assistant runs before the Finder (its Launcher
  extension), and on Quit it puts a Register with Apple alias in the
  Desktop Folder ("you can register later by double-clicking Register with
  Apple on the desktop"). Without the folder it quits without showing the
  menu bar it hid, and makes no alias; the Finder then makes the folder.
  The working disk with its Desktop Folder renamed (the catalog's key and
  thread, in place) does the same, so that's the cause; a real Mac would
  do it too with that disk, and its next startup is fine (checked: the
  assistant again, quit, menu bar and alias). The VBL was on throughout
  (no SetInterrupt). Blue's part was the stopped run: see the next.
- **Mac OS 9.2.2's doze and the mouse:** at Apple Software Restore's last
  dialog the power manager had been dozing since instruction 41.45 billion
  (half an hour after the OK click; PowerDispatch 9, then 1 in a loop,
  CacheDispatch 4, the cursor hidden twice, the display's sync off by
  SetSync): mouse clicks (the ADB autopoll packets arrived: `00 80`, `80
  80`) and ADB moves changed nothing, not even MBState; a key woke it at
  once (PowerDispatch 9 within 40,000 instructions) and clicks worked
  after. The same on the CD's Finder after 30 minutes' idling (a menu
  wouldn't open). The ADB Manager's handler for autopoll data (10.2f1's
  image `0xFFC2B4EA`) tells the power manager of activity (68k
  `_PowerDispatch` with D0 `$00010001`, UsrActivity) if low memory `$DD5`
  bit 0 is set: for the keyboard (its device's original address 2) when
  the packet's first key is a key-down, and for the mouse (original address
  3) only unless the power manager's globals (PMgrBase `$D18`, + `$112`)
  say 'SARA', which they do here. So the ADB mouse never counts; on the
  iMac the mouse is USB, and its HID driver tells the power manager itself.
  Setting the globals' last-activity time (+ `$1A`) from the host didn't
  wake it. Now the Cuda (`cuda_poll()`), when it has mouse data (or the
  cursor was put where the host pointer is) while Mac OS dozes (it napped
  through PowerDispatch in the last 250 ms of guest time: `cuda_doze()`),
  first sends a keyboard packet with Option down and up, at most once a
  second; a modifier alone posts no event. A click 0.3 s after the tap
  still went unseen (Mac OS was still waking), one 1.8 s after worked:
  window users move the mouse first. Runs that never doze are unchanged
  (the CD checks), and a scripted restore with a plain `move` and `click`
  quits Apple Software Restore; its disk then starts and quits the Setup
  Assistant to the Finder with its menu bar, the Register with Apple alias
  and the Control Strip.
- **MacBench's Floating Point test** (M5; profiles in instruction time
  with `clock 1000`, which runs the same code, `BLUE_JIT_PROFILE=FROM:TO`
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
  scoreboard's Elapsed Time over 30 s of host time, `BLUE_RAVE_FPS`):
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
  is the Time Manager's, from VIA timer 2. Blue's bus was 66 MHz and its
  timebase 16.6 MHz, so 9.x's Microseconds and AbsoluteToNanoseconds ran
  0.6% fast (factor 1/16.5; Unreal Tournament's clock 1.006 of guest time,
  measured in real time). The bus is now four times the timebase, 66.4 MHz
  (device tree, ProcessorInfo; the processor stays 266 MHz): the factor is
  1/16.6 and UT's clock 1.0006 of guest time over 25 s. The 8.5 CD and
  installed 8.6 checks end exactly as before; 9.1's CD (4 billion) ends
  elsewhere in its startup's details (its timers' durations changed by
  0.6%) with the same screen, and 9.2.2's CD (3 billion) with the same
  screen but SimpleText's caret in the other phase of its blink.
- **A movie's sound stuttered while the processor idled** (real time; the
  user's "240x180 Uncompressed", a 15 s QuickTime movie of 61 MB, 4 MB/s,
  44.1 kHz sound): disk transfers moved guest time on at once (20 MB/s, at
  most a tick each), putting it ahead of the host clock, and idling makes
  that up only while no sound plays. While sound plays, then, guest time
  ran ahead for good, and the sound DMA, paced by guest time, gave the host
  more frames than its device played: the queue filled to its 250 ms and
  `audio.c` dropped whole chunks (256 frames, 5.8 ms each). Measured with
  `BLUE_LOG_AUDIO` (M5, in a window, 45 s runs, the movie played with the
  space bar): 9.2.2's QuickTime Player from the shared folder read 4 MB/s
  in 1 MB reads, each charged a tick, 83 ms of guest time a second: guest
  time 8% fast, the queue full 2 s in, then 12 to 17 chunks dropped every
  second (47,872 frames in all), the processor mostly idle. 8.5's
  MoviePlayer (QuickTime 3) read 30 MB/s of host time from the disk image
  and from the shared folder alike: QuickTime 3, or the File Manager under
  it, reads the same 32 KB again and again (one block 136 times in turn,
  into a ring of eight 32 KB buffers at 0x0708AD10, between 0.8 and 1 MB
  reads of the frames), 13 MB/s of guest time for a 4 MB/s movie, so
  transfers took 1.27 s of guest time a second: guest time at 2.27 times
  the host clock, 100,000 frames a second from the guest, 56% of chunks
  dropped (315,648 frames), and the movie over in 7 s instead of 15. Not
  the host's sleeps: they overran by 0.1-0.25 ms on average, under 1 ms
  nearly always (SDL_WaitEventTimeout overran by 10-77 ms a few times a
  run, mostly as windows opened and applications started; the queue's
  depth covers most of them).
  Now (`drive_call()`, `transfer_wait()` in `src/nk.c`) while sound plays
  in real time a transfer takes host time, all of it: the call leaves the
  host call without moving the PC, the guest takes interrupts and makes
  the call again, which naps until the transfer's time (as for the 3D
  engine's frames); a call made while another waits (from an interrupt) or
  by another task than the blue one moves guest time on as before
  (interrupts are taken in the waits: 633 of 5,500 in the 9.2.2 run's,
  1,322 of 18,500 in 8.5's). After:
  guest time's lead stays put while the movie plays, 44,100 frames a
  second, nothing dropped and no silence added during it, the movie in
  15 s (its timecode at 4 s 22 frames 5 s after the space bar); 9.2.2
  waited out 331 transfers (3.1 s), 8.5 4,052 (7.8 s; QuickTime 3's
  re-reads take half of each second, and it keeps up). And the host's
  queue (`audio_pace()` in `src/audio.c`) now resamples the guest's frames
  to play up to 0.5% faster or slower as the queue, smoothed, is fuller or
  emptier than its goal (60 ms plus half the device's 1024-frame buffer):
  0.1% for each 10 ms off, so a device clock that drifts from the host's
  (a fraction of a percent at most) no longer empties or fills it in time,
  and the silence added after a stall of the host is played away within
  seconds rather than kept as delay. Measured: within ±0.2% of real speed,
  the queue 52-87 ms before each chunk (before: 41-70 on 9.2.2, 81-104 on
  8.5, as each happened to start). Instruction-time runs are unchanged
  (the wait is only in real time): the 8.5 and 9.2.2 CDs (3 billion
  instructions) and 9.1's (4 billion) end exactly as before, and the 8.5
  and 9.2.2 CDs' real-time startups take as long (1.2 and 1.0 s with fast
  start; little sound plays then).
  Tests: `tests/test_audio.c` (pass-through at the goal, the 0.5% bound, a
  device 0.3% fast or slow for 5 minutes, recovery from 150 ms over).
- **PippinPPC on a Pippin** (Task 19): the Pippin's System Folder has an
  enabler ('gbly' "Pippin") whose 'nlib' 777, "PippinPPC" (1,901 bytes),
  is the library. Its twelve exports import only GetToolboxTrapAddress and
  CallUniversalProc: each calls the trap $AA6F (Pippin.h's
  kPippinDispatch) with a D0-dispatched Pascal procInfo, the selector word
  being the parameters' words << 8 | the selector:
  `OSErr SavePRAMtoFlash(void)` 0x0000, `OSErr MakeWritesPermanent(void)`
  0x0001, `OSErr ReadNVExtraPRAM(void *buffer, short count, short offset)`
  0x0406 and `WriteNVExtraPRAM` 0x0407 (the order from the enabler's own
  call: 2 bytes at 12, the AppleTalk setting), `void
  InvokeSoftKeyboard(void)` 0x0009, `OSErr PippinFindFolder(short vRefNum,
  OSType folderType, Boolean create, short *foundVRefNum, long
  *foundDirID)` 0x080A (FindFolder's), `PippinStandardGetFile` 0x070B and
  `PippinStandardPutFile` 0x060C (StandardGetFile's and StandardPutFile's),
  `OSErr PippinStandardDeleteFile(long *bytesRequested)` 0x020D, `Boolean
  HasFloppyDock(void)` 0x000E, `Boolean HasMountedFloppy(short *vRefNum)`
  0x020F, `short GetDriveRefNum(short drive)` 0x0110 (1
  kPippinStorageDevice, 2 kFloppyStorageDevice: the volume's vRefNum).
  Pippin.h (the WWDC 1996 CD's "Pippin Video Sources") declares all but 0,
  6 and 7, and two that aren't exported (Read/WriteAppleTalkBootSetting,
  0x0011 and 0x0112); the enabler's 'gpch' 777 patches $AA6F for 1, 9 to
  16, 0x11 and 0x112 and leaves 0, 6 and 7 to the Pippin ROM's. What they
  do (Pippin Technical Notes "Pippin Standard File" and "Pippin Flash
  Access", Dev CD Dec 96): a Pippin has 128 KB of flash (120 for titles),
  a small HFS volume, copied to a RAM disk at startup; MakeWritesPermanent
  writes it back (up to 8 s; Restart and Shut Down do too). Each title
  keeps its files in one folder at the root of the flash or a floppy,
  named by its 'pipn' resource: the enabler's code takes 'pipn' 1's Pascal
  string at offset 12 (63 characters at most), else CurApName.
  PippinStandardDeleteFile asks the user to delete files to free
  *bytesRequested, and returns the bytes deleted there.
- **Super Marathon on Mac OS** (Task 19): `marathon.ppc` and
  `marathon2.ppc` import six of the functions, strongly, and at startup
  ask Gestalt for 'pipn' (gestaltPippinAttr): without it, a fatal "Sorry,
  but this game is only for the Pippin." With it, preferences go through
  PippinFindFolder(GetDriveRefNum(kPippinStorageDevice), 'pipn', true, …)
  rather than FindFolder's 'pref', the dialogs are the Pippin's,
  MakeWritesPermanent follows a save, Delete Saved Game works (else a
  beep), the 'pipp' resources 1000 and 1001 are copied into the AppleJack's
  ADB device's data (there's no such device: nothing), and quitting calls
  MakeWritesPermanent and ShutDwnStart. Their change_panel_state sends
  pattern buffers (panel class 7) to its default case, which does nothing;
  the game is saved when a level changes instead (the shell's state 13,
  `_change_level`: after the level loads, "Save game as:"; 550 bytes for
  Marathon 2, 278 for Marathon). Marathon (1) takes the chosen file's
  folder from sfFile.vRefNum with directory 0, saving and loading, and
  ignores parID: with an FSSpec's own vRefNum it saved at the volume's
  root and found nothing to load. The library's dialogs now return a
  working directory for the file's folder as the vRefNum (OpenWD, procID
  'ERIK', Standard File's), as SFGetFile's did; Marathon 2 uses the FSSpec
  either way. No ROM's InterfaceLib has the Gestalt value calls; the
  System of 8.5 and 9.2.2 adds them ('fovr' 50 "InterfaceLib":
  NewGestaltValue, SetGestaltValue, DeleteGestaltValue). The value is 0:
  the Pippin's isn't known, and these titles only look for the selector.
- **A library's initialization runs in the application's context**: the
  Code Fragment Manager prepares an application's import libraries in the
  new process, so NSetTrapAddress from an initialization routine is the
  application's own patch (see "An application's patches are its own"):
  the Pippin library's _ShutDown patch is in force only while the title
  runs. The patch is 68k code in the library's data, put in place with
  BlockMove (which flushes the 68k emulator's cache, as BlockMoveData
  doesn't). Quit in Super Marathon's menu goes back to the Finder.
- **StandardAlert's buttons** (AppearanceLib, Mac OS 8.5): a parameter
  block with kAlertStdAlertCancelButton as both the default and the cancel
  button returned paramErr, without an alert; Delete (the OK button) as
  default and Cancel as the cancel button works.
- **Desktop icons from scripts**: a scripted click on a disc's icon
  deselected the startup disk and selected nothing (on the Hanshin disc,
  as the restored 9.2.2's finding says), so Command-O opened nothing.
  Typing selects: "c" for that disc, whose Japanese name shows in Mac OS
  Roman as "ç„ê_ëÅèç-" (the Finder's order ignores the cedilla).
- **Marathon's keys in instruction time** (with `clock 300`): Marathon 1
  missed Tab held for 0.1 billion instructions (a third of a second of
  guest time) and took it held 0.3 billion; Marathon 2 took 0.1 billion.
- **hfsutils writes images it mounts**: `hmount` opens a writable image
  read-write, and on a volume whose MDB lacks the "unmounted" bit (drAtrb
  0x0100) libhfs scavenges: drWrCnt + 1, drLsMod set to the time, the bit
  set, the volume bitmap written again (drFreeBks and drNxtCNID if they
  disagreed). Mastered CDs often lack the bit (85.iso did, and its 8.5 CD
  boot then read 924 fewer blocks and ended elsewhere than its
  references). Use hfsutils only on APFS clones (`cp -c`), made read-only
  (`chmod a-w`) when only reading: libhfs then mounts read-only.
- **Packaging** (packaging; macOS 27 on an M-series Mac, Ubuntu 22.04 and
  Debian 12 arm64 in podman):
  - **Homebrew's libraries need the macOS they were built on**
    (`LC_BUILD_VERSION` minos: 26.0 for sdl2-compat, SDL3, gettext's
    libintl and PCRE2, 27.0 for GLib and libslirp here), so an app made
    with them does too (`make app` raises `LSMinimumSystemVersion` to the
    newest and says so). `tools/macos-deps.sh` builds SDL2 2.32.10, GLib
    2.90.1 and libslirp 4.9.7 for macOS 11 in about a minute each way on
    an M-series Mac (cached in CI). Homebrew's SDL2 is sdl2-compat, which
    opens SDL3 as `@loader_path/libSDL3.dylib`, so that goes in too.
  - **An SDK newer than the target hides missing functions**: GLib's
    `cc.has_function('pipe2')` declares its own prototype and finds
    `pipe2` in macOS 27's SDK, then calls it (weakly linked: NULL, a crash,
    before macOS 27). The deps are built with
    `-Werror=unguarded-availability-new`, and GLib's check for pipe2 is
    taken out (it uses pipe() then).
  - **dlopen and @rpath**: the bundled dylibs are `@rpath/NAME` with an
    `LC_RPATH` of `@loader_path` each, so they find each other wherever
    the app is (moved, renamed, run through a symbolic link); the
    executable's `@executable_path/../Frameworks` is there for the same.
    Blue opens them by full path, found from `_NSGetExecutablePath` or
    `/proc/self/exe` with links resolved.
  - **Ubuntu 22.04's GCC 11 lacks `__builtin_shufflevector`** (raster.c),
    so the AppImage's Blue is built with `gcc-12`, with libstdc++ and
    libgcc linked in: glibc (2.35) is all it needs. GCC 12 also warned of
    `id` maybe used uninitialized in `mp_tick()` (inlined `fire()`), which
    GCC 13 and clang didn't: `earliest()` set `*id` only when it found a
    timer or task. It now sets it to 0 (none: no object's ID is 0) at the
    start, as it sets `*time` to `UINT64_MAX`, so its results are defined
    either way; GCC 12 builds Blue without a warning. Ubuntu 22.04's GLib
    2.72 uses PCRE 8.39 (`libpcre.so.3`), not PCRE2, and gettext's
    functions are glibc's: the AppImage carries SDL2, libslirp, GLib and
    PCRE.
  - **The bundled libraries' exact sources** go with each release, in
    `Blue-third-party-sources.tar.gz` ([THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md#their-exact-sources-blue-third-party-sourcestargz)). Meson
    fetches PCRE2 and proxy-libintl for GLib's wraps into
    `subprojects/packagecache` (PCRE2's with a WrapDB zip of its Meson
    files, and a patch from GLib's own `packagefiles`), checking the
    SHA-256 the wraps give; files already there are used, so
    `tools/macos-deps.sh` puts in those it kept and keeps what Meson
    fetched (libffi, for GObject only, isn't carried and isn't kept). On
    Ubuntu, `${source:Version}` is the source package's version (the
    binary's can differ), `apt-get source --print-uris` lists its files,
    and `apt-get source --download-only` checks them against the signed
    `Sources` indexes, which need `deb-src` (commented out in the
    ubuntu:22.04 image). Launchpad keeps every version's files
    (`+archive/primary/+sourcefiles/PKG/VERSION/FILE`, the epoch included),
    where mirrors drop superseded ones. In podman on macOS the files
    apt-get writes as root look 0600 from the Mac (they're 0644 in the
    container); the archive's files are made readable anyway.
  - **The AppImage runtime**: the AppImage project's static type2-runtime
    needs no libfuse2, only `fusermount`; without FUSE (a container) it
    says so, and `--appimage-extract-and-run` works anywhere. Bundled
    libraries get a run path of `$ORIGIN` (no `LD_LIBRARY_PATH`, which
    would reach the host's zenity and kdialog too).
  - **Opened from the desktop**: a Finder launch has launchd (pid 1) as
    its parent and no terminal on stdin, stdout or stderr; an AppImage
    has `APPIMAGE` set. Only then does Blue without a command show its
    window: a script or tool running `blue` with its output redirected
    still gets the usage. A prompt is drawn as the machine hands frames
    over, and Dear ImGui sizes a box in its first frame without showing
    it, so the window hands over the same frame a few times after each
    change. Its box is in the window's units, so the window opens at
    `DISPLAY_SCALE_OWN` (the display's own scale: a screen pixel to a
    unit, the window 600 x 360 units on any display) rather than Auto, and
    follows the window's size with a desktop of that size.
- **Quitting, and SDL's menu bar on macOS** (the window; macOS 27, SDL
  2.32.10 as the app carries it, and Homebrew's sdl2-compat 2.32.74 on
  SDL 3.4.18): SDL makes a menu bar whenever it makes the application
  object (`Cocoa_RegisterApp()`, if `NSApp` is nil: then the menu bar is
  always nil too, so one made before can't stop it; and making `NSApp`
  first would leave out SDL's application class, which dispatches its
  events, and its `finishLaunching`). Logged with a probe injected into
  `blue` (`DYLD_INSERT_LIBRARIES`; it posts key events into the
  application's queue, clicks the close button, sends a Quit Apple event
  to the process): Settings… ⌘, Hide ⌘H, Hide Others ⌥⌘H, Quit ⌘Q;
  Window: Close ⌘W, Close All ⌥⌘W (AppKit's), Minimize ⌘M, Toggle Full
  Screen fn-F and ⌃⌘F. SDL sees a key event before `-[NSApp sendEvent:]`
  offers it to the menu bar, so Mac OS got ⌘Q and Blue ended at once
  ("stopped: window closed"); ⌘H hid Blue, ⌘M minimised it, and the close
  button and a Quit Apple event (the Dock's Quit, logging out) ended the
  run at once too. Blue now replaces the menu bar once SDL is up (About
  Blue, Hide Blue, Quit Blue, no key equivalents, no Window menu), and
  asks before quitting once the Finder has run (`quit_asked()` in nk.c):
  ⌘Q in Mac OS Setup Assistant asked "Are you sure you want to quit?",
  ⌘H, ⌘M and ⌘W left Blue as it was, the close button, a Quit Apple event
  and Quit Blue asked (twice asked, once shown), Shut Down brought Mac OS
  8.5's "Are you sure you want to shut down your computer now?" and then
  "stopped: guest powered off", Turn Off "stopped: turned off on
  quitting", Cancel brought back the guest tools' offer it had covered,
  and closing before the Finder was up quit at once. Under sdl2-compat,
  key events posted to a window that isn't key don't reach the program
  (SDL 2.32.10's do): a test harness's artefact. SIGINT and SIGTERM: SDL
  installs handlers (they become SDL_QUIT) only where it finds SIG_DFL,
  so Blue's own, installed first, keep them quitting at once (a
  background job's SIGINT is SIG_IGN, and stays so).
- **A window by default** (`blue boot`): the window is what people run,
  so `boot` opens one, in real time, and `--headless` runs without
  (instruction time unless `--realtime`, 500 million instructions unless
  `--max`); `--window` is still taken, and does nothing. With no display
  (SSH, a container, CI: `SDL_VIDEODRIVER=none` stands in for one here)
  the run stops with "boot: no window can be opened (SDL's reason): run
  with --headless to start Mac OS without one" and exit status 1, rather
  than going on without the window, which would be real time with
  nothing to see (`display_failure()` says why `display_open()` failed;
  display.c no longer prints it). Reproducible runs, the reference boots
  among them, are `build/blue boot --headless ...`: the same logs and
  screenshots as before.
- **A Pippin disc on its own** (Task 19): Super Marathon's disc (one HFS
  volume, "Super Marathon") has a `PippinAuthenticationFile` ('PpnV',
  'PpnA') at its root and a blessed System Folder with Finder, System
  ('zsys'), the `Pippin` enabler ('gbly' 'MACS'), Pippin Launch and a
  StartupScreen, but no Mac OS ROM file: a Pippin's ROM is the console's.
  Given only it, `boot` used to say "no drive has a System Folder with a
  Mac OS ROM file in it: give one with --rom FILE" in the terminal (and
  nothing in a window). Now when there's no startup disk it looks for a
  Pippin disc among the images (`startup_pippin()`: the authentication
  file at the root, or the enabler in the blessed System Folder;
  `hfsread_lookup()` finds a name in a folder, ASCII letters in either
  case, and `hfsread_volumes()` goes through an image's volumes) and says
  by the volume's name that Pippin titles run inside Mac OS, with the
  command: `boot`'s arguments, quoted for the shell, with `--disk <your
  Mac OS disk>` before the drives and the disc as a CD. Otherwise it says
  why there's no startup disk, with the README's Quick start. In the
  terminal, and (but with `--headless`) over a black screen in a 640 x
  480 window at the display's own scale, with Copy Command (the Pippin's)
  and Quit; exit status 1.
- **The AppleJack** (game controllers): Apple's Pippin Technical Note
  "Applejack Input Device Driver" (PPBD-JHACEF003, 4/11/96) gives its
  driver's data, AJGlobalData: `pipp`, MyTalkR0, handlerID, ourMBState,
  ourLockState, switchStates (a long, a bit per switch, 0 while down),
  SwitchData switchMappingArray[18] (function, modifiers, keyCode,
  charCode: functions nothing, mouse, double-click, mouse lock, keyboard,
  front panel (unused), modifier; modifiers bits Command, Shift, Caps
  Lock, Option, Control), nextjADBProc and cursorHandler: 92 bytes, as a
  `pipp` resource is. The switch numbers are the bits of its ADB register
  0: 0-2 the three system buttons below (left, middle, right), 3-7 none,
  8 blue, 9 yellow, 10-13 the D-pad's up, left, right and down, 14 red,
  15 green, 16 the right fire button, 17 the left. The key codes are
  virtual ones (the KeyMap's, an event's): Super Marathon's have 0x7E for
  up and 0x3B for Control, where the note's own constants say 0x36 (the
  ADB keyboard's raw code); the System's `KMAP` maps the ADB keyboard's
  codes to them (raw 0x36 Control to 0x3B, the arrows 0x3B-0x3E to
  0x7B-0x7E, the right-hand modifiers 0x7B-0x7D to 0x3C-0x3E). The driver
  sits in a Pippin's ROM; Super Marathon's disc has no Applejack extension
  (its System Folder's Extensions folder has only "Pippin 1.2 ROM Init"),
  and the enabler's own `pipp` resources (0, and 128: the startup
  picture) aren't mappings. `marathon.ppc` and `marathon2.ppc` each have
  `pipp` 1000 (the menus: the D-pad the arrows, the fire buttons the mouse
  button, the system buttons Command-Q, mouse lock and double-click) and
  1001 (the game: O, K, ; and L on the D-pad, Z and X the fire buttons,
  Option, Space, Shift and Tab the coloured buttons, Command-Q, Control
  and P the system buttons). Their routine at code offset 0x38174 (in
  Marathon's) goes through CountADBs down to 1 with GetIndADB, and for a
  device whose origADBAddr is 3 and dbDataAreaAddr isn't nil, takes that
  as a cursor device record, its refCon (offset 8) as AJGlobalData, and if
  that begins `pipp`, copies the resource's 18 SwitchData into it (nothing
  else). It's called with 1000 as menus come up, and with 1001 by
  "LoadGameAppleJackSettings" only if a byte the game sets from Gestalt
  'pipn' says it's a Pippin. The ADB Manager's traps: _CountADBs $A077,
  _GetIndADB $A078 and _GetADBInfo $A079 (Blue Pippin Library's patch
  first went on $A079 by mistake: Marathon then found no AppleJack, and
  GetADBInfo for the mouse's address 3 gave the AppleJack's record).
