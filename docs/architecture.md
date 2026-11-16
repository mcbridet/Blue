# How Blue works

Blue runs classic Mac OS by replacing the bottom of the system, the
Power Mac's NanoKernel, rather than emulating a whole Power Mac. This
document describes the design: what runs where, how Mac OS is started,
what the kernel does, and the machine Mac OS sees. Other documents go
further into parts of it:

- [bring-up.md](bring-up.md): how Mac OS 8.5, 9.0, 9.1 and 9.2.2 were made
  to start up, step by step;
- [devices.md](devices.md): the iMac's devices and Blue's drivers;
- [drives.md](drives.md) and [shared-folder.md](shared-folder.md): disks,
  CDs, floppies and the shared folder;
- [window.md](window.md): the window, its menu and status bar;
- [guest-tools.md](guest-tools.md): the code Blue installs inside Mac OS;
- [jit.md](jit.md): the PowerPC JIT.

## The NanoKernel interface

On a real Power Mac only the NanoKernel runs in supervisor mode; the 68k
emulator, the Toolbox, applications and drivers all run in PowerPC user mode
and reach the kernel through a small interface. Blue keeps all of that Apple
code unmodified and supplies the rest natively:

```
68k apps, 68k Toolbox code    → Apple's own 68k emulator (from the ROM, unchanged)
PPC apps, Toolbox, drivers    → Blue's user-mode PowerPC JIT and interpreter
══════ NanoKernel interface: kernel calls (twi, sc), exceptions, interrupts ══════
Native kernel + an iMac's devices + drivers + loader   ← Blue, portable C
macOS and Linux hosts, AArch64 and x86-64
```

## What Blue targets

- **ROM:** the "Mac OS ROM" file from the startup disk's System Folder, as
  a New World Mac's firmware starts it (see [bring-up.md](bring-up.md)); `--rom` for one of your
  own. Tested: 8.5's own (Toolbox release 0x44F3), 8.6's (0x44B5), 9.1's
  (6.1f9, in parcels), MacOS9Lives 9.2.2's (10.2f1, in parcels, 0x45F6),
  and `MOS.rom`, a 1.x file (0x44F1, compatible with
  `iMac,1 PowerMac1,1 PowerBook1,1`; the family SheepShaver uses).
- **OS:** Mac OS 8.5 to 9.2.2. The first target was the retail Mac OS 8.5
  CD (`85.iso`), which boots with the ROM file it carries itself (1.1.x, no `<COMPATIBLE>` list) or with `MOS.rom`.
- **Machine identity:** the original iMac (`iMac,1`): its device tree,
  devices and drivers, as the universal info tables Blue builds describe
  it. Retail 8.5 predates the Power Mac G3 (Blue and White), so the iMac
  covers the whole range from 8.5 on.
- **OS ladder:** 8.5 → 8.6 → 9.0.4 on the NanoKernel v1 interface, then the
  v2 interface for 9.1 and 9.2.2 (both CDs start up to the Finder, and install; see
  [bring-up.md](bring-up.md)). 9.2.2 is MacOS9Lives' CD; Apple's own 9.2
  and 9.2.2 CDs haven't been tried.

## Starting Mac OS: the loader

Blue does what a New World Mac's firmware and the Trampoline in the ROM
file do before the NanoKernel starts. Which disk it starts up from, and
how the Startup Disk control panel chooses it, is in
[bring-up.md](bring-up.md#the-startup-disk-and-its-rom-file).

- `blue rom` decodes NewWorld bootinfo files and raw 4 MB ROMs, verifies the
  checksum, extracts the Toolbox image and Trampoline, and decodes the
  NanoKernel ConfigInfo (memory map, low-memory init list).
- `blue boot` takes the Mac OS ROM file from the startup disk (see
  [bring-up.md](bring-up.md#the-startup-disk-and-its-rom-file);
  `src/hfsread.c`: the blessed System Folder is word 0 of the volume's
  Finder info, its files follow its thread record in the catalog B-tree, and
  a fragmented file's extents go on in the extents overflow file), or the
  one `--rom` names. It builds the user address space from ConfigInfo, does what the
  Trampoline would (hardware info record, universal tables, interrupt vector
  tables, device tree), fills in the kernel's info records and starts Apple's
  68k emulator from the ROM.

## The processor

Blue has a user-mode PowerPC interpreter (integer, FP, string/multiple, reservations)
with kernel exits for `sc`, traps, privileged instructions, faults and a
host-call opcode (primary opcode 6). AltiVec is not implemented. Floating
point is the host's IEEE arithmetic, but NaN results are a 750's on
every host (the first NaN of frA, frB and frC, quieted, else
0x7FF8000000000000; never negated by `fnmadd`/`fnmsub`; cut to single
precision by single-precision operations and `frsp`), where hosts
differed (x86's default NaN is negative; each picked among NaN operands
its own way).

A JIT translates the hot code to AArch64 or x86-64, with exactly the
interpreter's results: see [jit.md](jit.md).

## The kernel

Blue's kernel takes the kernel calls, exceptions and interrupts that the
NanoKernel would, for both versions of its interface: NanoKernel 1's for
Mac OS 8.5 to 9.0.4, and NanoKernel 2's, with Multiprocessing Services and
its tasks, for 9.1 and later (`src/mp.c`; see
[bring-up.md](bring-up.md#mac-os-91-from-its-cd)).

- Native kernel calls: `ReturnFromException` (all verdicts), `RunAlternateContext`,
  `ResetSystem`, `VMDispatch` (VM off; `VMAllocateMemory` supported),
  `PrioritizeInterrupts` and `RTASDispatch`. `ResetSystem` with Gary
  Davidian's skeleton key (r3 'Gary', r4 0x05051956) doesn't reset: its
  caller goes on with MSR = (MSR & ~r5) | r7, as with both NanoKernels.
- Privileged instructions: in user mode, `mfmsr` reads the user MSR and the
  rest raise the privileged instruction exception. In supervisor mode (the
  skeleton key's MSR with PR off) the kernel does them (`src/priv.c`):
  `mfmsr`, `mtmsr` (PR on again ends it), `rfi`, the 750's supervisor SPRs
  with an iMac's values (PVR, HID0, HID1 for 266 MHz on a 66 MHz bus, L2CR
  for its 512 KB L2 at half speed, the thermal assist unit reading 40 °C,
  the decrementer, SPRGs, SRR0/1, BATs...), segment registers, and `tlbie`,
  `tlbsync` and `dcbi`, which do nothing; writes are kept, without effect.
  Real mode is the same address space (identity for RAM). While the MSR has
  EE off, interrupts wait.
- Exceptions go the NanoKernel way: to the running context's handler if
  enabled, otherwise ending the native (alternate) context. An interrupt that
  arrives while native code runs ends that context with exception 0, so the
  68k emulator takes it and resumes the native code afterwards.
- Interrupts reach the emulator the way the NanoKernel's Heathrow handler
  (kind 10) does it: `PendingInts` in HWInfo, the 68k level at
  EmulatorData`+0x70`, and the CR post/clear masks. Latched pulses (DMA
  channels) are acknowledged in the controller when delivered and stay in
  `PendingInts` until a PrioritizeInterrupts after Mac OS has handled them
  (see [interrupt sets](devices.md#interrupt-sets)).

### The kernel's loop

`nk_boot()`'s loop in `src/nk.c` runs guest code in chunks (`ppc_run()`:
the JIT, or the interpreter) and handles each exit: kernel calls (`twi`,
`kernel_call()`), `sc` (`kcall_mp_dispatch()` under NanoKernel 2's
interface), faults, and the host calls of Blue's drivers and guest tools.
Devices and interrupts are brought up to date every 20,000 instructions
(`SYNC_INSNS`) and at exits that may have touched them. The common kernel
calls, NanoKernel 2's quick system calls and device register accesses are
made without leaving the engines (see the JIT's [kernel calls in
place](jit.md#kernel-calls-in-place)).

- Mac OS runs as NanoKernel 1 contexts: the System context (Apple's 68k
  emulator) and an alternate context (native code), switched by
  `switch_context()`, which saves and loads the registers to and from the
  context blocks in guest memory (in vectors: NEON, or AVX2 where the
  processor has it). `m->system`, `m->context`, `m->flags` and `m->enables`
  describe the running one.
- Interrupts: `update_interrupts()` posts the 68k level to the emulator
  through EmulatorData and the System context's CR; native code running
  then is ended with exception 0 (`m->preempt`), so the emulator takes it.
- Idle: `idle_wait()` (PowerDispatch's nap, the `_SynchIdleTime` hook,
  Multiprocessing Services' call 55) sleeps in real time until the next
  timer, interrupt or input, and returns at once in instruction time; with
  Multiprocessing Services' tasks ready, they run instead (`blue_naps()`).

### Multiprocessing Services

For ROM files with parcels (9.1 and later: `m->mp_kernel`), the kernel is
a NanoKernel 2, with Multiprocessing Services built in, running Mac OS as
one task (the "blue task") among others it schedules. Everything it does
is behind `mp_kernel`, so 8.5 to 9.0.4 see NanoKernel 1 as before.
`src/mp.c` holds its objects (`struct mp`): IDs as NanoKernel 2 makes them
(the slot << 16 | the slot's use count; the blue process is the first, 1,
which the ROM checks), the blue process, task, page queue and address
space, the tasks' saved registers and the ready queue, and `mp_call()`,
which makes a call for the current task. Calls are there as Mac OS makes
them, each after NanoKernel 2's source; one that isn't stops the run,
naming it. They are: tasks (7-10, 126), queues (15-18, 39), semaphores
(20-23), critical regions (25-28), event groups (49-52, and 54, which
marks one to raise a 68k software interrupt when set), notifications
(64-67, 128), task queries (11, 12, 14, 56, 59, 63), `MPYieldWithHint`
(13), `MPGetNextID` (37), areas (77, 80: one for each startup region and
Mac OS's RAM; 78 their records, 93 and 94 page ages), the page ager's (82,
100), timers (30, 31, 32, 40, 41), delays (33, and 55), processes (3-6,
99, 116), processors and their scheduling (44, 45, 62, 132), the CPU
plugin (2, 46: in `nk.c`), and `sc` with r0 -1 and -2 (whether the
current task is the blue one: `system_call()`). Its rules are NanoKernel
2's:

- **Tasks share the one processor and host thread.** Each task other than
  the blue one has its registers (GPRs, FPRs, CR, LR, CTR, XER, FPSCR,
  PC) saved in `struct mp` while another runs; after an exit, when
  `mp.yield` says the choice may change, the loop switches
  (`switch_task()`) to `mp_next()`'s. The blue task's state is whatever
  its NanoKernel 1 contexts hold. `mp.current` is the task on the
  processor, and `blue_running()` says whether that's the blue task.
- **Blocking:** a task that must wait is taken off the processor with its
  PC on the `sc`; when what it waits for comes, or its timeout, it's made
  ready and makes the call again.
- **The blue task never blocks:** NanoKernel 2 answers it
  `kMPBlueBlockingErr` (-29293), and Mac OS polls. While it polls or naps,
  ready tasks run.
- **Interrupts belong to the blue task:** one posted while another task
  runs has the blue task run next.
- **Time is the guest timebase:** timeouts, timers and time slices are in
  timebase ticks, so instruction-time runs stay the same every time.
- **Exceptions in other tasks** stop the run, as do kernel calls from them
  other than 8: NanoKernel 2 sends them to the task's exception queue, but
  none of 9.1's or 9.2.2's tasks makes one.

How 9.1 and 9.2.2 use it is in
[bring-up.md](bring-up.md#mac-os-91-from-its-cd); the calls, their
registers, and NanoKernel 2's scheduling as Blue follows it are in
[findings.md](findings.md), and what Apple's does under QEMU in
[nk2-reference.md](nk2-reference.md).

## The device tree and native drivers

- Device tree (`src/devtree.c`, `src/bootinfo.c`): an `iMac,1` with a Grackle
  PCI bridge, Mac I/O, a framebuffer display and a disk, flattened into the
  format the ROM's Name Registry reads.
- Native drivers: Blue generates PEF NDRVs (`src/ndrv.c`) and puts them in
  the device tree, so Mac OS loads them like any PCI card's driver. Their code
  is a loop around a host call; the driver logic is C (`src/video.c`,
  `src/disk.c`), and the stub can call Mac OS services on its behalf
  (`IOCommandIsComplete`, `SetInterruptTimer`, Driver Services' interrupt
  sets, Video Services' VBL service).

The display driver is described in [devices.md](devices.md#display-and-the-vertical-blank),
the disk driver in [drives.md](drives.md#the-disk-driver), and the
Ethernet driver's lanLib in [devices.md](devices.md#networking).

## Time

Guest time either follows the host clock (real time: the window, or
`--headless --realtime`) or is counted in instructions, one timebase tick each
(instruction time: `--headless`'s default), which makes a run the
same every time; see [debugging.md](debugging.md#instruction-time-and-real-time).
In real time, Blue sleeps while Mac OS is idle, and hurries through the
timeouts of startup:

- Idle: the Process Manager calls `_SynchIdleTime` when no process can run,
  and in real time the kernel then sleeps until the next VIA timer
  interrupt or vertical blank, window frame or input. An idle Finder costs a few percent of
  one core (`--no-idle` turns it off). The 68k emulator dispatches traps
  it interprets through its dispatch table, so the kernel watches the entry
  for `_SynchIdleTime` there; but code its dynamic recompiler has
  translated (the hot code, such as the Process Manager's idle loop) calls
  one entry of the recompiler's for every A-line trap, with `r24` pointing
  at the trap word. There a conditional hook stops only for
  `_SynchIdleTime` (`hook_match_*` in `src/ppc.h`: the JIT checks the trap
  word at the start of its block for that entry, which costs nothing
  measurable; stopping for every trap cost 30%). The kernel finds the entry
  by its first instructions. The NanoKernel's PowerDispatch nap call
  idles too, though neither Mac OS 8.5 nor 9.0.4 makes it here. The Mac OS
  Setup Assistant never idles: while it's in front, it keeps a core busy,
  as it kept a real Mac's processor.
- Fast start: in real time, from startup until the Finder is up, guest time
  runs 10 times as fast as the host clock, so that the timeouts startup
  waits out (AppleTalk on Ethernet looking for other machines, Apple Remote
  Access making sure of its name, USB, DHCP) pass in a tenth of the time;
  meanwhile the guest sees a processor a tenth as fast. Mac OS 9.0.4 is at
  the Finder in under 3 s instead of 13, the 8.5 CD in about 1.5 s, the 9.1
  CD in about 1.3 s. Most of
  those waits are busy loops, which don't call `_SynchIdleTime`, so guest
  time runs faster throughout. The Finder counts as up once it has run (the
  Process Manager names the current application in `CurApName`, which stays
  unset until the first one runs) and the guest has then idled for 1.5 s of
  guest time without disk transfers (network traffic doesn't count: what's
  left may be looking for printers in the background), so a dialog before
  the Finder can't end it. Input (keys, the mouse button) ends it too once
  the Finder has run; before that (a dialog during startup, such as the one
  after a crash, or keys held down) it only holds it back while a key or
  the button is down and for the Mac's double-click time after (Mac OS
  times clicks, double-clicks and key repeat in its own ticks), and
  startup then goes on faster. Two minutes of guest time after the Finder
  has run end it as well (the Setup Assistant never idles), or ten from the
  start if the Finder never runs. Guest time is then ahead of the host
  clock by what it gained, 15-25 s (about two minutes after the startup
  disk check that follows a crash), which it makes
  up while the guest idles, as for disk transfers ([drives.md](drives.md#the-disk-driver)): the Mac's clock
  is that much ahead at first, and right after a minute or two of idling.
  A restart starts guest time over from the host clock. `--no-fast-start`
  turns it off.

## The virtual machine

What the Trampoline replacement (`src/bootinfo.c`) tells the ROM:

- Model `iMac,1`, a 266 MHz PowerPC 750 on a 66.4 MHz bus, timebase 16.6 MHz:
  a quarter of the bus clock, as the 750's is, and as Mac OS 9.1 and later
  take it to be (their AbsoluteTime is converted with Gestalt's bus clock;
  with a 66 MHz bus, their Microseconds ran 0.6% fast).
- Grackle PCI host bridge; on it the display (device 1, framebuffer at
  `0x82000000`), up to four disks (devices 2-5; a shared folder is one),
  the USB controller (device 6, registers at `0x81000000`), the floppy
  drive if there's one (device 7, `blue,floppy`) and Mac I/O
  (device 16) at `0x81080000`:
  Heathrow-style interrupt controller (`+0x10`), DMA channels (`+0x8000`,
  0x100 bytes each), SCC (`+0x12000`; the escc layout at `+0x13000`), VIA1
  with Cuda (`+0x16000`).
  Interrupt sources use Heathrow numbers (DMA channel n is source n,
  Ethernet 14, SCC A 15, SCC B 16, sound 17, VIA1 18, the display 22, as the
  iMac's ATI chip, USB 28); levels: VIA1
  1, the display, sound, Ethernet, USB and their DMA 2, SCC 4 (and its DMA
  channels 4-7, with the modem). Configuration space is at
  `0xFEC00000` (`CONFIG_ADDR`) and `0xFEE00000` (`CONFIG_DATA`).
- Sound: `davbus` at Mac I/O `+0x14000` (DMA channels 8 and 9), with a
  `sound` child; DecoderInfo long 39 holds the chip's address.
- The SCC, with the internal modem (see
  [devices.md](devices.md#the-serial-controller)): `escc` and
  `escc-legacy`, each with `ch-a` (the modem's: DMA channels 4 and 5) and
  `ch-b` (the infrared port's: 6 and 7).
- Ethernet: `ethernet` at Mac I/O `+0x11000` (`bmac+`, DMA channels 2 and 3;
  nothing answers at those addresses, lanLib being the driver's way to the
  hardware), station address 52:54:00:12:34:56. Its lanLib is Blue's even
  when the ROM file has one for `bmac+` (9.2.2's).
- Mac I/O's floppy controller cell (`+0x15000`), for ROM files with parcels
  only: reads as zeros, writes are ignored (the iMac has no floppy drive).
- NVRAM, for ROM files with parcels only: `nvram` at Mac I/O `+0x60000`
  (`device_type` and `compatible` `nvram`, `reg` 0x60000 0x20000, `#bytes`
  0x2000, `AAPL,address`), as on Heathrow and Paddington Macs: 8 KB, byte k
  at `+16k`.
- Where Open Firmware booted from, for ROM files with parcels only:
  `/chosen`'s `bootpath`, such as `/pci/blue,disk@2:8,\\:tbxi` (a disk's
  node is at its PCI device number, the partition is its map entry, none
  without a map), for the partition the ROM file came from, or else the
  first partition with boot blocks. The disk
  driver says, through DriverGestalt, which node ('nmrg'), unit ('intf'
  'scsi', 'dvrf' with the PCI device number as target ID) and partition
  ('ofpt') each drive is.
- Open Firmware's settings, for ROM files with parcels when NVRAM is kept
  or in real time: `/options` (`boot-device`, from NVRAM's `common`
  partition, else the bootpath) and `/openprom` (`model` "OpenFirmware 3",
  `supports-bootinfo`); in NVRAM, the `common` partition (CHRP: signature
  0x70, 2 KB at offset 0), which Blue makes if those bytes are blank
  (`src/ofnvram.c`). The disks' nodes are in command-line order, but for a
  startup disk `boot-device` chose for a ROM file without parcels, which
  goes first.
- ADB/power-manager kind 0 in the universal flags, so the early ROM code uses
  RTAS (tokens in `NKHWInfo`) for clock, NVRAM and power (`NKHWInfo`'s Mac OS
  NVRAM offset, `0x1400`, is where PRAM goes; a ROM file with parcels uses the
  Cuda's RTC and the `nvram` node instead, through its libraries). A flag in
  ProductInfo (bit 19 at `+0x28`) has the ROM restart through RTAS
  `system-reboot`; otherwise it restarts in place, jumping back to its start
  with the devices still running, and hangs on the first interrupt. Bit 26
  there is for USB (see [devices.md](devices.md#usb)).
- The ROM installs SCSI Manager 4.3 (no buses), as on real machines; the
  Apple CD/DVD Driver extension needs `_SCSIAtomic` to exist.
- The boot-info area (`0xFF800000`) holds the tables and the device tree.
- Info records: NanoKernel 1's versions and lengths, or for ROM files with
  parcels NanoKernel 2.28's (HWInfo 0x0108 with CascadeInfo, a table of no
  cascaded interrupt controllers, ProcessorInfo 0x0112 with clock rates,
  SystemInfo 0x0107), all in the one InfoRecord page.
- Quiet writes: ConfigInfo maps the ROM write-through and read-only (PP 3),
  which the NanoKernel takes to mean that stores to it are dropped rather
  than raising an exception (and counted in NanoKernelInfo's
  QuietWriteCount). Blue's kernel does the same; a run's summary says
  `quiet writes: N` when there were any.
- The ROM maps a few regions as single 4 KB pages (the kernel's and the
  emulator's data, the info records). The pages next to them, up to 16 KB
  boundaries, read as zeros rather than faulting, so that hosts with 16 KB
  pages can protect guest memory per host page (see [fastmem](jit.md#memory-fastmem)). Only stray
  accesses could tell, and the map is the same on every host.
