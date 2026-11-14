# syshm

Run classic Mac OS on arbitrary hardware by replacing the bottom of the OS
instead of emulating a whole Power Mac.

On a real Power Mac only the NanoKernel runs in supervisor mode; the 68k
emulator, the Toolbox, applications and drivers all run in PowerPC user mode
and reach the kernel through a small interface. syshm keeps all of that Apple
code unmodified and supplies the rest natively:

```
68k apps, 68k Toolbox code    → Apple's own 68k emulator (from the ROM, unchanged)
PPC apps, Toolbox, drivers    → syshm's user-mode PowerPC JIT and interpreter
══════ NanoKernel interface: kernel calls (twi), exceptions, interrupts ══════
Native kernel + HAL drivers + loader   ← syshm, portable C
Linux today; other hosts / bare metal later
```

## Target

- **ROM:** `MOS.rom`, a 1.x NewWorld `Mac OS ROM` file (NanoKernel ID
  `NewWorld v1.0`, compatible with `iMac,1 PowerMac1,1 PowerBook1,1`). Same
  family SheepShaver uses; ships with Mac OS 8.5–8.6.
- **OS:** the retail Mac OS 8.5 CD (`85.iso`). It boots, and carries its own
  older ROM file (1.1.x, no `<COMPATIBLE>` list) which we don't use.
- **Machine identity:** to be decided by the universal info tables syshm
  builds. Retail 8.5 predates the Blue & White G3, so an iMac identity is the
  safer first choice; B&W G3 (`PowerMac1,1`) for 8.5.1 and later.
- **OS ladder:** 8.5 → 8.6 → 9.0.4 on the NanoKernel v1 interface, then the
  v2 interface for 9.2.2.

## Status

**Retail Mac OS 8.5 installs onto a disk image and boots from it, and so
does Mac OS 9.0 (see below).** Both also boot straight from their CDs.
Mac OS 8.6 works too, on the same ROM file and NanoKernel interface. Mouse
and keyboard work in a window or from scripted input, and sound plays
through the host (in real time). Games run accelerated by the guest tools'
3D engine, on the host's GPU: Unreal Tournament plays, and Bugdom (QuickDraw
3D, Game Sprockets) reached about 100 frames per second in play on an M1
Max, with music (it needs URL Access, which comes with Mac OS 8.6 and
later). The built-in Ethernet reaches the host's
network, so Internet Explorer loads web pages.

```
build/syshm disk create hd.img 2G "Macintosh HD"      # an empty HFS volume
build/syshm boot MOS.rom --disk hd.img --cdrom 85.iso --window --nvram syshm.nvram
```

Run "Mac OS Install" from the CD (about 30 billion instructions, about a
minute). In its Options, turn off "Update Apple Hard Disk Drivers": Drive
Setup only knows Apple's own ATA/SCSI drives and quits on ours. Then boot the
installed system with just `--disk hd.img`. Drives are tried in command-line
order. In the window, the GUI/Super key is Command and Alt is Option.

Restart starts the machine over in the same window, from the ROM with
memory and devices reset as on a real Mac (NVRAM is kept, and so are guest
time and the instruction count, which input scripts and `--max` go by).
Shut Down quits.

### Mac OS 9

Mac OS 9.0 installs and boots too, on the same ROM file and NanoKernel
interface. The "Mac OS 9" volume of Apple's 9.0.4 CD (9.0, with the 9.0.4
update alongside) starts up from the CD; its installer puts 9.0 on an empty
disk image in under a minute in real time (turn off "Update Apple Hard Disk
Drivers" again), and the installed system starts up to the Setup Assistant.
The 9.0.4 update on the same CD installs too, and 9.0.4 boots to the Finder
in under 3 s in real time, with fast start (see below). Without it, that's
13 s, or 5 s with AppleTalk inactive (Chooser): with AppleTalk active on
Ethernet, startup waits out its timeouts, as on a real Mac with no other
AppleTalk machine around, and behind syshm's NAT (IP only) there never is
one. That's 5 s for a node address and a router (AARP probes, then ZIP
GetNetInfo) and 3 s for Apple Remote Access's personal server to make sure
its name is unique (NBP lookups). Two things were in the way of those
13 s:

- Mac OS 9 waits up to 10 s during "Starting Up…" for the root hubs of the
  machine's USB controllers, and without any it waits it all out. syshm has
  an empty USB controller (below), whose root hub the ROM's driver finds at
  once.
- The input component of Mac OS 9's AWACS sound driver ('sind' 'awgc') has a
  bug in its close routine, which runs at every startup (the Component
  Manager opens and closes the component to register it): it unlocks its
  globals, passes a pointer into them to a call that allocates memory, which
  can move them, and afterwards writes through that pointer. With some
  layouts of the system heap that corrupts its free list, and startup
  stopped with an address error or hung (with the USB controller, every
  time). The sound node now says the chip has output hardware only
  (`hardware-types` 2; input only ever recorded silence), so the input
  component never claims it.

Sound works in Mac OS 9 too: its DMA plug-in for the sound chip needs the
chip's interrupts as a Driver Services interrupt set, and those sets' DMA
interrupts need acknowledging by the kernel (see interrupt sets below).

Mac OS 8.6 and later carry a newer NanoKernel in the System file (a `krnl`
resource) and replace the running kernel with it at startup unless that one
is at least as new. syshm's kernel reports NanoKernelInfo version 2.28, with
the multiprocessing fields empty (no "blue task"), so those Systems keep it;
their MPLibrary then doesn't use the version 2 interfaces, as on a machine
without them. One call does come through, though: 8.6's Process Manager
(its ProcessMgrSupport fragment), when nothing needs to run, sleeps until
the next process's wake time with Multiprocessing Services call 55
(`MPDispatch`, kernel call 8, with an AbsoluteTime in r3:r4). syshm naps
then until that time or an interrupt, as for `PowerDispatch`; any other
`MPDispatch` call stops the run, naming it.

### Mac OS 9.1 (not yet)

9.1's System won't start with `MOS.rom` ("This startup disk will not work
on this Macintosh model"). It needs the ROM file from its own CD (Mac OS ROM
6.1f9; `MOS91.rom` here is the data fork of "Mac OS ROM" from the CD's
System Folder), which keeps its contents in parcels (`src/parcels.h`; `rom
parcels` lists them): the Toolbox image; libraries the ROM prepares and
registers at startup (MotherBoardHAL, TimeManagerLib, NVRAMServicesLib,
RTCServicesLib, NativePowerMgrLib, StartLib), which earlier ROMs had inside
the image; and drivers for machines' devices. syshm loads it and puts the
libraries, and the drivers that match its devices (Cuda, Grackle, the
RTC), in the device tree as the Trampoline would. Its ConfigInfo no longer
describes the address space (the Trampoline in the same file hands the
kernel a page map instead), so syshm lays it out as earlier ROMs describe
it.

The 68k emulator then starts, and the ROM starts Multiprocessing Services,
which want a NanoKernel 2 underneath: one that runs Mac OS as its "blue"
task. For these ROM files syshm's kernel reports one (NanoKernelInfo's
version 2 fields; the ROM checks that the blue process has ID 1, the first
one a NanoKernel 2 makes), and takes their calls (`sc`, or kernel call 8,
with the call number in r0; `src/mp.c`): queues, semaphores, critical
regions, event groups (setting one that raises a 68k software interrupt
stops the run, as it isn't delivered yet), notifications, and the areas
startup's address space is made of (Mac OS's RAM and the page map's
regions). `SYSHM_LOG_MP=1` logs them. Startup then gets as far as creating
a preemptive task (call 7), from the ROM's Multiprocessing Services code,
and stops there: running tasks besides the blue one is next.

### Networking

In real time (`--window`, `--realtime`), the guest's built-in Ethernet is
connected to the host's network through libslirp, the user-mode NAT QEMU
uses (loaded at run time if installed, so not a build dependency;
`--no-net` leaves the Ethernet unconnected). DHCP gives the guest
10.0.2.15, with the gateway at 10.0.2.2 and DNS at 10.0.2.3. The guest can
open connections out; nothing outside can connect in. Only IP gets through,
so AppleTalk over Ethernet finds nobody (and makes startup wait, though
fast start mostly hides that; see Mac OS 9).

A fresh Mac OS 8.5 has TCP/IP set up for Ethernet and DHCP but inactive:
open the TCP/IP control panel, answer Yes to make it active, then close it
and save. Browsers of the time can't load most of today's HTTPS sites;
plain-HTTP sites work.

`SYSHM_NET=1` connects the Ethernet in instruction-time runs too (the
host network's timing then makes them unreproducible), and
`SYSHM_LOG_NET=1` logs the driver's calls. Frame counts are printed at
exit.

### Guest tools

Some things only code inside Mac OS can do. `--tools` attaches a small
read-only volume, "syshm Tools", holding it (`syshm tools create FILE` writes
the same volume to a file):

- **syshm Enhancements**: drag it onto the System Folder (the Finder puts it in
  Extensions) and restart. From then on, resizing the window changes the
  screen's resolution to match once the window has stopped changing (while
  it's being dragged, the window scales the screen), and the Finder lays out
  the desktop again. A
  resolution chosen in Mac OS (Monitors & Sound, or the Control Strip)
  resizes the window instead. Without it, the window scales the
  screen. On high-density displays a screen pixel is a point, so Mac OS
  appears at the size of other programs; with `--integer-scale` it is a
  whole number of pixels instead (2 at 1.7x), for the crispest pixels.
- **syshm 3D Engine**: a QuickDraw 3D RAVE drawing engine (put it in
  Extensions). QuickDraw 3D's interactive renderer and RAVE games then find
  an accelerator: it reports itself as an ATI engine (vendor 1, engine 5),
  because games of the time, Bugdom among them, take nothing else for
  hardware. Contexts draw to the screen or to memory, with textures and
  bitmaps in the 16- and 32-bit formats, Z buffering, Gouraud shading,
  perspective-correct and mipmapped texturing (modulate, decal, highlight),
  fog, alpha test, the blend modes including OpenGL-style factors, and
  depth-sorted transparency (`kQATag_ZSortedHint`). The host side
  (`src/rave.c`) records each frame's drawing, a batch at a time, and while
  the guest goes on recording the next, the GPU or a software rasterizer
  draws it:
  - In real time (`--window`, `--realtime`) the host's GPU draws, if it has
    hardware OpenGL 3.3 (`src/gpu.c`): in an off-screen context of its own
    (CGL on macOS, EGL without a window system elsewhere; `src/gl.c`,
    loaded at run time, so not a build dependency), on a thread of its own,
    and each frame is read back into the context's device in guest memory as
    before. The first context says which (`3d: drawing with OpenGL 4.1
    Metal - 91.7 (Apple M5)`). The pixels are those of the software
    rasterizer but for rounding at triangle edges and in filtering, and
    mipmapping, which is per pixel (trilinear with the Best texture filter,
    with anisotropic filtering where there is some). If OpenGL fails, the
    engine says so and goes on in software.
  - Otherwise, a software rasterizer (`src/raster.c`) draws it in bands of
    rows on helper threads; the pixels are always the same, however many
    threads draw them, so instruction-time runs stay the same on every host.
    `SYSHM_RASTER_THREADS` sets the number (default 3, counting the
    emulated processor's).

  `--no-gpu` (or `SYSHM_GPU=0`) draws in software in real time too;
  `SYSHM_GPU=1` draws with OpenGL in any run, a software renderer such as
  Mesa's llvmpipe included. `SYSHM_RAVE_FPS=1` prints the frame rate every
  second, and `SYSHM_LOG_RAVE=1` logs every call. docs/gpu-plan.md
  describes the GPU backend in full.

Like the drivers, these are generated code fragments whose logic runs in
syshm through host calls (`src/tools.c`). Bugdom's setup, for the record:
QuickDraw 3D 1.6, Game Sprockets, URL Access (part of Mac OS 8.6 and later;
Bugdom won't load without it) and the 3D engine in
Extensions; it asks for a registration code, which shareware-era copies
came with. Unreal Tournament (the Mac CD, 4.32) needs only the 3D engine in
Extensions: its first-time configuration offers RAVE only for an ATI engine
that answers ATI's engine-specific tag 1011 (the hardware's generation;
0x304 and up a Rage Pro, 0x500 and up a Rage 128). The engine answers
0x500, and the game picks its "RAVE (Rage 128)" settings, with which it
also asks for ATI's table of extensions (tag 1021, a pointer) and calls
them without checking: clearing the draw and z buffers, and replacing a
texture's pixels (for its animated textures). It lights surfaces with the
Rage 128's multitexturing: a second texture (the lightmap; tag 1018, a
pointer) modulates the first while tag 1012 is on, at coordinates in the
vertices' specular fields (ks: u/w, v/w, 1/w); both renderers do that,
filtering the lightmap bilinearly, clamped (the other tags UT sets then,
always the same, are taken to mean that). The engine's data carries
its version, so one installed by an older syshm isn't asked for what it
lacks: syshm says to install it again from the tools volume.

- `syshm rom` decodes NewWorld bootinfo files and raw 4 MB ROMs, verifies the
  checksum, extracts the Toolbox image and Trampoline, and decodes the
  NanoKernel ConfigInfo (memory map, low-memory init list).
- User-mode PowerPC interpreter (integer, FP, string/multiple, reservations)
  with kernel exits for `sc`, traps, privileged instructions, faults and a
  host-call opcode (primary opcode 6). AltiVec is not implemented.
- A JIT to AArch64, on Linux and macOS (`src/jit.c`, `src/a64.h`;
  `--interp` turns it off, and x86_64 hosts interpret). Blocks of up to 128
  instructions are translated
  natively once they have run 16 times (`SYSHM_JIT_HOT`). A block ends at
  an unconditional or computed branch; past a conditional one it goes on
  along the not-taken path, and the taken path leaves through out-of-line
  code. Rarer instructions call the interpreter from inside the block;
  traps, system calls and host calls go straight to the kernel; anything
  that faults or touches a device page leaves the block so the interpreter
  executes it. The time base is read natively.
  - Exactness: blocks run only when the instruction budget covers them, and
    each instruction checks before it changes anything, so instruction
    counts, device timing and faults are exactly the interpreter's (boots,
    the install and `--fixed-time` runs give identical results either
    way; `tests/test_jit.c` compares random programs).
  - Registers: CR, LR, r1, r3, r4, r24, r27 and r29 (the hottest across the
    68k emulator and native code) live in host registers while generated
    code runs; others are cached within a block and written back at exits
    (each side exit stores what is dirty at its point).
  - Control flow: exits to known addresses are patched into direct branches
    between blocks; computed ones look the target up in a two-level table
    (guest page, word) inline. Blocks follow unconditional branches into the
    ROM, so each 68k opcode handler gets its own copy of the emulator's
    dispatch code and of its indirect branch.
  - Memory ("fastmem"): guest memory is mapped a second time (a memfd on
    Linux, `mach_vm_remap()` on macOS) for generated code with host page
    protections that follow the guest's page flags (and write-protect pages
    holding translated code). Loads and stores then need no checks; one
    that faults (a device register, an unmapped or read-only page) is sent
    by a SIGSEGV handler (SIGBUS on macOS) to the interpreter, and
    translated with flag checks from then on. A few thousand do so in a
    boot. `SYSHM_JIT_FASTMEM=0` checks every access.
  - Code changes are tracked per instruction word: stores (by either engine,
    or the host, e.g. disk reads) to a page with translated code drop only
    the blocks translated from those words, which matters because the 68k
    emulator rewrites its own translation cache all the time. Rewritten
    code must run 16 times again before it's translated: most of the 68k
    emulator's translations are replaced after a few runs (a boot
    translated 222,000 blocks before this, 140,000 after).
  - Floating point: arithmetic, conversions, compares, moves and
    single-precision loads and stores run natively where the interpreter's
    fast path would apply (round to nearest, no exceptions enabled, operands
    and results well inside the normal range, so that inexact is the only
    possible exception and an error-free transformation finds it). The
    FPSCR comes out exactly as the interpreter's; anything else calls the
    interpreter for that instruction from out-of-line code. The FPSCR is
    kept lazily: XX and FX at once (they change only while XX is clear),
    but an operation's FR, FI and result class, which the next one
    replaces, wait with its operands until C or the interpreter might look,
    when the interpreter replays the operation to work them out. Keeping
    them exactly at every operation cost QuickDraw 3D's code (Bugdom) 13%.
  - Hosts: generated code is the same on Linux and macOS. It never uses
    x18, which macOS reserves; on macOS the code buffer is `MAP_JIT`
    memory, which the emulation thread makes writable only while it writes
    code (`pthread_jit_write_protect_np()`, about 25 ns a switch). What's
    left that only macOS pays (those switches, its slower fault signals) is
    under 1% of a boot.
  - `SYSHM_JIT_PROFILE=1` samples where host time goes (translated blocks
    by guest address, stubs, C functions) and prints it at the end.
    `SYSHM_PERF=1` reads the host processor's counters for the emulation
    thread (Linux): every second, guest instructions per second and host
    instructions and cycles per guest instruction; a total at the end.
- Idle: the Process Manager calls `_SynchIdleTime` when no process can run,
  and in real time the kernel then sleeps until the next VIA timer
  interrupt, window frame or input. An idle Finder costs a few percent of
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
  the Finder in under 3 s instead of 13, the 8.5 CD in about 1.5 s. Most of
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
  up while the guest idles, as for disk transfers (below): the Mac's clock
  is that much ahead at first, and right after a minute or two of idling.
  A restart starts guest time over from the host clock. `--no-fast-start`
  turns it off.
- `syshm boot` builds the user address space from ConfigInfo, does what the
  Trampoline would (hardware info record, universal tables, interrupt vector
  tables, device tree), fills in the kernel's info records and starts Apple's
  68k emulator from the ROM.
- Native kernel calls: `ReturnFromException` (all verdicts), `RunAlternateContext`,
  `ResetSystem`, `VMDispatch` (VM off; `VMAllocateMemory` supported),
  `PrioritizeInterrupts` and `RTASDispatch`.
- Exceptions go the NanoKernel way: to the running context's handler if
  enabled, otherwise ending the native (alternate) context. An interrupt that
  arrives while native code runs ends that context with exception 0, so the
  68k emulator takes it and resumes the native code afterwards.
- Interrupts reach the emulator the way the NanoKernel's Heathrow handler
  (kind 10) does it: `PendingInts` in HWInfo, the 68k level at
  EmulatorData`+0x70`, and the CR post/clear masks. Latched pulses (DMA
  channels) are acknowledged in the controller when delivered and stay in
  `PendingInts` until the next PrioritizeInterrupts (see interrupt sets).
- Devices: RTAS (clock, NVRAM, power), a 6522 VIA with timers, the Cuda (time,
  PRAM, power, ADB with autopoll), an ADB keyboard and mouse, a Heathrow-style
  interrupt controller and a Z8530 SCC with nothing attached (LocalTalk sees an
  idle network). Device registers are emulated by decoding the faulting
  load/store (`src/mmio.c`).
- DBDMA (`src/dbdma.c`): Mac I/O's 16 descriptor-based DMA channels, after
  the CHRP I/O Device Reference (chapter 15): input/output with the MORE and
  LAST forms and streams, STORE_QUAD/LOAD_QUAD (memory, device registers,
  channel registers), NOP, STOP; conditional interrupt, branch and wait on
  the channel's status bits; run, pause, flush, wake and dead. Devices plug
  into a channel's streams and may stall it until they have data or room;
  transfers take no time of their own, so DMA happens at deterministic points.
  Channel interrupts are pulses that the interrupt controller latches until
  cleared, or until the kernel takes them when it delivers them (see
  interrupts above).
- Sound (`src/awacs.c`): an AWACS-family codec on the Mac I/O "davbus", which
  Mac OS drives with its own driver stack from the System file (`.DAVAudio`,
  DAVAudioLib, the GCAwacs hardware plug-in and the 'awgc' sound output and
  port components; Mac OS 9's has a DMA plug-in in DAVAudioLib's place). Output takes 16-bit stereo frames from DMA channel 8 at
  the sample rate of guest time, so guest-visible behaviour never depends on
  the host; input (channel 9) records silence. The codec's attenuation and
  mute bits are applied before the frames go to the host, so the volume
  slider and Mute in Monitors & Sound work. What it took for Mac OS to use
  it: a DecoderInfo address for the chip (Gestalt `snhw` is then `awgc`),
  the device-tree node with `AAPL,interrupt-vectors` (which Mac OS 8.5's
  DAVAudioLib uses) and an interrupt set (`driver-ist`, which Mac OS 9's
  DMA plug-in needs: without one it fails to start, and Mac OS 9 had no
  sound output at all), and a `sound` child whose `vendor-id`/`device-id`
  (Apple, 1) pick a port layout the 'soph' component knows; without them the
  output device opens but never plays. The node declares output hardware
  only (`hardware-types` 2, bit 0 being input), which keeps Mac OS 9's buggy
  input component away (see Mac OS 9).
- Device tree (`src/devtree.c`, `src/bootinfo.c`): an `iMac,1` with a Grackle
  PCI bridge, Mac I/O, a framebuffer display and a disk, flattened into the
  format the ROM's Name Registry reads.
- Native drivers: syshm generates PEF NDRVs (`src/ndrv.c`) and puts them in
  the device tree, so Mac OS loads them like any PCI card's driver. Their code
  is a loop around a host call; the driver logic is C (`src/video.c`,
  `src/disk.c`), and the stub can call Mac OS services on its behalf
  (`IOCommandIsComplete`, `SetInterruptTimer`, Video Services' VBL service).
  - Display: 640x480 to 1280x1024 at 1-32 bits per pixel, CLUT and gamma
    calls, timer-driven VBL interrupts (which the cursor needs). Two more
    modes take the host window's size on request (up to what the 16 MB
    framebuffer holds at 32 bits per pixel), for syshm Enhancements (a guest
    tool) to switch between; each is listed only while in use.
  - Disk: host files as drives (`--disk`, writable; `--cdrom`, read-only;
    the 8.5 CD's System refuses to start from a writable drive, as from a
    copy of the CD).
    An Apple partition map gives one drive per HFS partition; otherwise the
    file is one volume. (When there's a map entry at offset 512, entries are
    512 bytes apart and count 512-byte blocks even if the driver descriptor
    gives larger ones, as on CDs with 2048-byte blocks.) A timer posts
    disk-inserted events until each volume has been mounted (the driver
    loads before events are enabled). The
    drives decline to hold virtual memory backing store (DriverGestalt
    `vmop`), so Mac OS boots with VM off. The kernel doesn't page; give the
    guest more RAM with `--ram` instead.
    Transfers take guest time, at 20 MB/s on disks and 4 MB/s on CDs (at
    most a tick each, so that a big one doesn't jump past what interrupts
    keep going, such as sound). Without it, a program's launch or a file
    copy took no guest time at all, and Mac OS counts a process's processor
    time in whole ticks: the Mac OS 9 installer waits until its engine has
    used a tick, which on a fast guest it could take forever to do. In real
    time, guest time runs ahead of the host clock by what transfers took,
    and catches up while the guest idles (running at 3/4 speed, unless sound
    plays).
- Ethernet (`src/net.c`): no chip is emulated. On New World Macs the
  firmware supplies the hardware layer of Mac OS's Ethernet driver, as a
  code fragment in the `ethernet` node's `lanLib,AAPL,MacOS,PowerPC`
  property, which the ROM registers as the shared library lanLib; Open
  Transport then loads "Ethernet (Built-In)", a DLPI driver around it.
  syshm puts its own generated lanLib there (the interface follows Apple's
  `bmac+` one, in later Mac OS ROM files), whose routines are host calls:
  sending passes frames to libslirp; for receiving, its initialization
  (which gets the node's RegEntryID) installs an interrupt handler for the
  receive DMA channel in the node's interrupt set, and each frame from the
  host pulses that channel's interrupt. The handler queues a secondary
  interrupt handler, which hands the frames to the driver's receive
  routine. (Without an interrupt set, a Driver Services timer polls
  instead: every 2 ms while there has been traffic in the last second and
  every 10 ms otherwise.) Mac OS Setup Assistant needs a network interface;
  without one it quit with error type 2.
- USB (`src/ohci.c`): an OpenHCI controller (an OPTi 82C861's PCI IDs, class
  0c0310) with two empty ports, driven by the ROM's own OHCIUIM. While it
  runs, frames are counted from guest time in 12 MHz bit times: each
  advances the frame number, writes it to the HCCA in guest memory and
  raises start-of-frame (and, every 32768, frame-number overflow). With
  nothing connected, there are no transfers, so the lists aren't walked.
  What it took for the ROM to use it: PCI configuration space
  (`src/pci.c`), through Grackle's `CONFIG_ADDR`/`CONFIG_DATA` registers
  and RTAS's `read-pci-config`/`write-pci-config`; an interrupt set for the
  controller (`driver-ist`, without which the UIM gives up with
  nrNotFoundErr; see interrupt sets below); and a ProductInfo flag (bit 26
  at `+0x28`) for which the ADB Manager sets the hook that the USB keyboard
  and mouse shims call (without it they call a null hook once the
  controller is found).
- Interrupt sets: Driver Services interrupt sets for devices (their
  `driver-ist`, whose members drivers install handlers in) come from the
  ROM's InterruptTreeNew, which builds them from every node with
  `AAPL,interrupt-priorities`: parallel arrays of up to 3 interrupts each,
  `AAPL,interrupts` (numbers, which the device names in its own
  `AAPL,interrupts`; here the sources), `AAPL,interrupt-priorities` (68k
  levels) and `AAPL,interrupt-vectors` (sources). The USB controller, the
  sound chip and the Ethernet node have them (the Ethernet node only for
  its receive DMA channel, lanLib's: with all three of its interrupts, Open
  Transport stalled for half a minute or more in some startups of Mac OS 9,
  busy in its deferred tasks, for reasons not yet known). Their handlers mask and
  unmask the source but never clear a latched pulse in the controller, so
  the kernel takes pulses itself when it delivers them, as the NanoKernel's
  handler would: they stay in `PendingInts` until Mac OS has handled them
  and calls PrioritizeInterrupts (which re-evaluates them). Otherwise a DMA
  interrupt fired forever.
- Host side: an SDL2 window loaded at run time (`src/display.c`; no build
  dependency, headless without it; on macOS also found in Homebrew's and
  MacPorts' directories, which aren't searched for bare library names;
  redrawn only when the screen or the status bar changes;
  drawn at the display's full resolution, so high-density displays don't
  blur it). The screen is scaled to fill the window above the status bar,
  keeping its shape:
  whole multiples exactly, other sizes by drawing it at the multiple above
  on the GPU and filtering that down, so every screen pixel comes out the
  same size with edges blended over at most one window pixel.
  `--integer-scale` uses whole multiples only (with a border if need be)).
  The Mac's cursor goes where the host pointer is (the low-memory mouse
  globals are set, as SheepShaver does), until Mac OS has had its cursor
  hidden for 0.3 s, as games hide it: then the pointer is captured and its
  motion goes to the ADB mouse, which is where InputSprocket and games read
  it (Unreal Tournament's mouse didn't move otherwise), until the cursor
  shows again. Control-Option (Ctrl+Alt) lets the pointer go, as does
  switching away from the window; a click in the window captures it again
  (the guest doesn't get that click), and so does the cursor showing and
  hiding again. The window's title says so while it's captured.
  F12 opens the menu (Dear ImGui's, from `third_party/imgui`, drawn over
  the screen through the same run-time SDL2 by `src/ui.cpp`; SDL 2.0.18 or
  later), and the guest is paused while it's open. In it: Resume (F12 and
  Esc too); Restart and Shut down (straight away: they're the Mac's own
  restart and power switch, so work not saved in Mac OS is lost; Shut down
  keeps NVRAM, as closing the window does); a screenshot of the Mac's screen
  (a PNG on the Desktop, or in the current directory without one, named
  for when it was taken); fullscreen (at the desktop's resolution); the
  status bar; and whether the pointer is captured while the Mac hides its
  cursor. In real time guest time doesn't count the pause, so the Mac's
  clock is that much behind afterwards.
  The status bar (shown unless `--no-status-bar`; the menu toggles it) sits
  below the screen, so the window grows by its height rather than covering
  anything (fullscreen, or maximized, the screen fitted to the window
  shrinks instead). It has lights for the disks, CD-ROMs and network, lit
  for a tenth of a second whenever they move data (green, amber while a
  disk is written; a light is dimmed if there's no such device), each with
  what it read and wrote over the last second (the network's what it
  received and sent, in bits; the disks' in bytes; a CD-ROM only reads),
  and one for the processor, green as bright as the share of the time the
  guest isn't napping, and softly blue for the rest, while it naps (with
  idle sleep). Beside that is its speed: millions of instructions a second of
  the time actually spent running them, as a clock rate (the Mac thinks it
  has a 266 MHz G3, which runs about one instruction a cycle), worked out
  once a second from a random 1 in 32 runs (timing each costs too much)
  and left as it was when the guest is busy less than a tenth of the time.
  In narrow windows (640 wide, say) the names are shortened and the text
  made smaller to fit.
  SDL2 sound output (`src/audio.c`; in real time, `--no-sound` turns it
  off; a small queue is kept so the guest's clock and the device's can't
  drift into growing delay),
  PNG screenshots (`src/png.c`), NVRAM kept
  in a file, real-time guest time with `--window`/`--realtime`, and
  `syshm disk create`, which lays out an empty HFS volume (`src/hfs.c`) the
  way Disk Initialization does (`hfsck` from hfsutils accepts it).
- Speed on an M1 Max: the boot to the Finder (about 3 billion instructions)
  takes 1.1-1.8 s with the JIT, which runs the 68k emulator's code at about
  2.5 billion PowerPC instructions per second (about 5 host instructions
  and 1.2 cycles each; much of the rest is mispredicted indirect jumps,
  which Apple's emulator makes by design); the interpreter manages about
  200 million. The Mac OS 8.5 install takes under 25 s. On an M5 under
  macOS, the same boot takes 0.8 s with the JIT and 11 s interpreted.

## Virtual machine description

What the Trampoline replacement (`src/bootinfo.c`) tells the ROM:

- Model `iMac,1`, a 266 MHz PowerPC 750 on a 66 MHz bus, timebase 16.6 MHz.
- Grackle PCI host bridge; on it the display (device 1, framebuffer at
  `0x82000000`), up to four disks (devices 2-5), the USB controller (device
  6, registers at `0x81000000`) and Mac I/O (device 16) at `0x81080000`:
  Heathrow-style interrupt controller (`+0x10`), DMA channels (`+0x8000`,
  0x100 bytes each), SCC (`+0x12000`), VIA1 with Cuda (`+0x16000`).
  Interrupt sources use Heathrow numbers (DMA channel n is source n,
  Ethernet 14, SCC A 15, SCC B 16, sound 17, VIA1 18, USB 28); levels: VIA1
  1, sound, Ethernet, USB and their DMA 2, SCC 4. Configuration space is at
  `0xFEC00000` (`CONFIG_ADDR`) and `0xFEE00000` (`CONFIG_DATA`).
- Sound: `davbus` at Mac I/O `+0x14000` (DMA channels 8 and 9), with a
  `sound` child; DecoderInfo long 39 holds the chip's address.
- Ethernet: `ethernet` at Mac I/O `+0x11000` (`bmac+`, DMA channels 2 and 3;
  nothing answers at those addresses, lanLib being the driver's way to the
  hardware), station address 52:54:00:12:34:56.
- ADB/power-manager kind 0 in the universal flags, so the early ROM code uses
  RTAS (tokens in `NKHWInfo`) for clock, NVRAM and power. A flag in
  ProductInfo (bit 19 at `+0x28`) has the ROM restart through RTAS
  `system-reboot`; otherwise it restarts in place, jumping back to its start
  with the devices still running, and hangs on the first interrupt. Bit 26
  there is for USB (see above).
- The ROM installs SCSI Manager 4.3 (no buses), as on real machines; the
  Apple CD/DVD Driver extension needs `_SCSIAtomic` to exist.
- The boot-info area (`0xFF800000`) holds the tables and the device tree.
- The ROM maps a few regions as single 4 KB pages (the kernel's and the
  emulator's data, the info records). The pages next to them, up to 16 KB
  boundaries, read as zeros rather than faulting, so that hosts with 16 KB
  pages can protect guest memory per host page (see fastmem). Only stray
  accesses could tell, and the map is the same on every host.

## Next steps

1. 3D: Glide, and OpenGL with Mac OS 9. (The RAVE engine draws with the
   host's GPU; what's left there, such as reading back less of each frame,
   is in docs/gpu-plan.md.)
2. Networking: host ports forwarded to the guest (so it can serve).
3. Startup-disk selection; Open Transport's stall with a full
   interrupt set for the Ethernet node; then the NanoKernel v2 interface,
   which the ROM files from Mac OS ROM 1.7.1 on use, and Mac OS 9.1's needs
   (see Mac OS 9.1).
4. More JIT speed for disk boots: device register accesses (1.3 million in
   a boot, mostly the Time Manager programming VIA timer 2) each leave
   generated code for the interpreter and the kernel's loop, and could be
   done from inside it (perhaps 3-5%); the interpreter, which runs code
   until it's hot at about 10 ns an instruction, mostly in overheads per
   block (about 8%); translating (4%), with the instruction-cache upkeep
   that follows (4%).

## Build and test

```
make            # build/syshm (C11, and C++17 for the menu: Dear ImGui is in third_party/imgui)
make test       # unit tests; the ROM test uses ./MOS.rom or $SYSHM_TEST_ROM if present,
                # the GPU test any OpenGL 3.3 there is (else it's skipped)
SYSHM_JIT_PROGRAMS=200000 build/syshm-tests jit    # longer JIT-vs-interpreter fuzzing
```

CI (`.github/workflows/ci.yml`) builds and tests on Linux and macOS, each on
x86_64 and AArch64, with warnings as errors; on AArch64 (Linux and macOS)
it also fuzzes the JIT with 100,000 programs, and on Linux the GPU backend
is tested on Mesa's
software OpenGL (llvmpipe; `SYSHM_TEST_GPU=1` makes the test fail rather than
skip without a context). Each run uploads a `syshm-<os>-<arch>.tar.gz`, and
pushing a `v*` tag publishes them as a GitHub release.

```
build/syshm rom info MOS.rom
build/syshm rom config MOS.rom                   # ConfigInfo and user address space
build/syshm rom parcels MOS91.rom                # a Mac OS 9.1 ROM file's parcels
build/syshm rom extract MOS.rom build/toolbox.rom
build/syshm rom trampoline MOS.rom build/trampoline.elf
build/syshm boot MOS.rom --cdrom 85.iso --window   # interactive
build/syshm boot MOS.rom --cdrom 85.iso --max 3000000000 --screenshot finder.png
build/syshm run-raw code.bin 1000                # SYSHM_TRACE=1 logs every instruction
```

Without `--window` or `--realtime`, guest time is counted in instructions
(one timebase tick each). `--seconds S` stops after S seconds of host time. With `--fixed-time <unix time>` the clock doesn't depend on the host
either, so a run is exactly reproducible, including faults at a given
instruction count. The boot reaches the Finder within about 3 billion
instructions (15 s). `--input FILE` replays input at given instruction
counts, for example:

```
3350000000 key 24                # Return (ADB key code, hex)
3400000000 dblclick 751 52       # open the disk
3500000000 motion 40 -10         # the mouse moved 40 right, 10 up (through the ADB mouse, as games read it)
3600000000 screenshot after.png
3700000000 clock 800             # guest time at 800 million instructions a second from here
```

`clock N` keeps a run reproducible while making guest time pass as on a
guest processor of N MIPS, so that work paced by guest time (sound, timer
tasks, a game's frame timing) is in real-time proportions: one timebase
tick per instruction is 16.6 MIPS, which makes such work look 50 times
bigger than it is. A scripted click's press and release are 3 million
instructions apart, which at a high `clock` can be too short for the guest
to notice; change the clock after the clicks that matter.

Devices and interrupts are serviced every 20,000 instructions and at kernel
exits, never at exits made only for debugging, so tracing and trap logging
don't change what the guest does. (At exits they are updated only when that
could change anything: when a VIA timer runs out or a USB frame starts with
its interrupt enabled, while sound DMA moves, or after something touched
them.) Tracing and trap logging use the
interpreter.

Debugging `boot`: `--trace-from N --trace-reg 24` logs instructions with the
68k PC (the emulator keeps it in `r24`), `--watch <addr>` logs every write
the guest makes to a page (the kernel's own writes there, such as disk
reads, go through unseen, so a watched run goes as any other), `--traps N
[--traps-after M]` logs 68k Toolbox/OS traps, interpreted or recompiled
(see Idle; with the driver name for `_Open`), `--log-hostcalls` logs the native drivers'
requests, `--dump-ram FILE` saves RAM at the end, `--max N` limits the run.
`--trace-range LO:HI [--trace-insn WORD]` logs only instructions in an
address range (optionally only one instruction word), with instruction
counts and r3-r5/LR, cheaply enough for whole boots. `SYSHM_LOG_SOUND=1`
logs the sound chip's register accesses, and `SYSHM_SOUND_WAV=FILE` records
what it plays (also in instruction time). `SYSHM_LOG_USB=1` logs the USB
controller's register accesses (not the frame number, which drivers poll),
`SYSHM_LOG_PCI=1` configuration space accesses.
`--io-ram` backs unmodelled I/O pages with plain RAM. Under gdb, `handle
SIGSEGV nostop noprint pass` lets the JIT's memory faults through; under lldb
on macOS, `settings set platform.plugin.darwin.ignored-exceptions
EXC_BAD_ACCESS` (before `run`) and `process handle -s false -n false -p true
SIGBUS` (or set `SYSHM_JIT_FASTMEM=0`).

## QEMU reference

Stock QEMU can't boot a 1.x ROM: its machines aren't models the ROM supports.
`tools/qemu-ref/run.sh` puts the ROM on a small bootable disk and patches the
g3beige device tree (`tools/qemu-ref/bw-g3.fs`) so the Trampoline accepts it.
Needs `hfsutils` and `qemu-system-ppc`.

```
tools/qemu-ref/run.sh [MOS.rom]                  # QEMU_DISPLAY=none for headless
tools/qemu-ref/qmon.py sample 10                 # PowerPC pc and 68k pc
tools/qemu-ref/qmon.py screenshot shot.png
tools/qemu-ref/qmon.py cmd 'x /16wx 0x5fffefc0'  # any monitor command
tools/qemu-ref/snapshot.sh                       # needs gdb; state at 68k emulator entry
```

With this ROM the Trampoline and NanoKernel boot and the 68k emulator runs ROM
code, then spins at 68k `0xFFC18316`. The Trampoline didn't resolve any
interrupt sources from the patched tree, so every vector in `NKHWInfo` is
`0x0800`, and the ROM's SCC setup loops forever when SCC B has no vector. It
is still the reference for what the Trampoline and kernel produce: the
snapshot gave syshm its info records, universal tables and low-memory setup.

## Assets

ROMs and OS images are user-supplied and ignored by git: `MOS.rom` (the ROM
above) and `85.iso` (retail Mac OS 8.5 CD, a bare bootable HFS volume);
`MOS91.rom` and `91.iso` for Mac OS 9.1 (see above).

## Layout

```
src/      be.h, lzss.c, rom.c + parcels.c, mem.c (flat 4 GB guest space),
          ppc.c + ppc_exec.inc + ppc_fp.c (interpreter), jit.c + a64.h
          (AArch64 JIT), nk.c (native kernel), mp.c (its Multiprocessing
          Services, NanoKernel 2's),
          bootinfo.c + devtree.c (Trampoline replacement, device tree),
          rtas.c (clock/NVRAM/power/PCI configuration), mmio.c, macio.c,
          dbdma.c, via.c, cuda.c, adb.c, scc.c, awacs.c, ohci.c (USB), pci.c
          (configuration space) (devices), ndrv.c (PEF driver generator),
          video.c, disk.c (native drivers), net.c (Ethernet's lanLib,
          libslirp), hfs.c (volume formatter),
          rsrc.c (resource forks), tools.c (guest tools), rave.c + raster.c
          + gpu.c + gl.c (3D engine: software and OpenGL), display.c (SDL2
          window), ui.cpp (its menu, with Dear ImGui), audio.c (SDL2 sound), png.c,
          perf.c (host processor counters), main.c
tests/    unit tests with a small instruction encoder (ppcasm.h); test_jit.c
          runs random programs on both engines and compares the results
tools/    qemu-ref/ reference-boot scripts, pef.py (PEF inspector),
          devtree.py (device-tree copy dumper)
docs/     gpu-plan.md (the 3D engine's OpenGL backend)
third_party/imgui/  Dear ImGui 1.93.0 (MIT; its core files only, as released)
```
