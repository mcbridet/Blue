# Bringing up Mac OS 8.5 to 9.2.2

How each version of Mac OS was made to start up on Blue's kernel, in the
order it happened: Mac OS 8.5 and 8.6 on the NanoKernel 1 interface, then
9.0, then 9.1 and 9.2.2 on NanoKernel 2's. It was the Status section of
Blue's README until November 2026, and is kept here as it was written:
"now" means then. How to install and start Mac OS today is in the
[README's Quick start](../README.md#quick-start); what Blue emulates is in
[architecture.md](architecture.md) and [devices.md](devices.md).

## Mac OS 8.5

**Retail Mac OS 8.5 installs onto a disk image and boots from it, and so
does Mac OS 9.0 (see below).** Both also boot straight from their CDs.
Mac OS 8.6 works too, on the same ROM file and NanoKernel interface. Mouse
and keyboard work in a window or from scripted input, and sound plays
through the host (in real time). Games run accelerated by the guest tools'
3D engine, on the host's GPU: Unreal Tournament plays, and Bugdom (QuickDraw
3D, Game Sprockets) reached about 100 frames per second in play on an M1
Max, with music (it needs URL Access, which comes with Mac OS 8.6 and
later; frames for the screen now come at most 60 a second, see
[guest-tools.md](guest-tools.md#blue-3d-engine)). The built-in Ethernet reaches the host's
network, so Internet Explorer loads web pages. CD-ROM drives play audio CDs
and games' CD soundtracks (cue sheets) through the sound chip, as the iMac's
drive did.

```
build/blue disk create hd.img 2G "Macintosh HD"      # an empty HFS volume
build/blue boot --disk hd.img --cdrom 85.iso --window --nvram blue.nvram
```

Run "Mac OS Install" from the CD (about 30 billion instructions, about a
minute). In its Options, turn off "Update Apple Hard Disk Drivers": Drive
Setup only knows Apple's own ATA/SCSI drives and quits on ours. Then boot the
installed system with just `--disk hd.img`. Drives are tried in command-line
order. In the window, the GUI/Super key is Command and Alt is Option.

### The startup disk and its ROM file

As a New World Mac's firmware does, Blue starts the "Mac OS ROM" file from
the startup disk: the first drive, in command-line order (an image's HFS
partitions in partition-map order), whose blessed System Folder holds a file
of type `tbxi`. Above, the new disk is empty, so that's the CD's; once 8.5
is installed, the disk's own. `src/hfsread.c` reads HFS and HFS+ volumes
(HFS+ also inside an HFS wrapper), only the blocks it needs. Blue says where
the file came from:

```
rom: "Mac OS 9.1:System Folder:Mac OS ROM" from 91.iso, partition "Toast 4.0 PPC Partition" (Mac OS ROM 6.1f9, 2448470 bytes)
```

`--rom FILE` starts a ROM file (or a raw 4 MB ROM image) of your own
instead. `blue rom info` and the other `rom` commands take a disk or CD
image too, and show the ROM file that would be started from it.

Restart starts the machine over in the same window, from the ROM with
memory and devices reset as on a real Mac (NVRAM is kept, and so are guest
time and the instruction count, which input scripts and `--max` go by).
Shut Down quits.

The Startup Disk control panel of Mac OS 9.1 and later chooses the startup
disk, as on an iMac: it sets Open Firmware's `boot-device`, which Mac OS
keeps in NVRAM (as `boot-device=/pci@80000000/blue,disk@3@3:8,\\:tbxi`:
the disk's PCI device number, its SCSI target, the partition), and a
restart, or the next launch with the same `--nvram` file, starts the ROM
file in that disk's System Folder, whichever system it is:

```
build/blue boot --disk hd91.img --disk hd922.img --cdrom 922.iso --cdrom 85.iso --window --nvram blue.nvram
rom: "MacOS9Lives:System Folder:Mac OS ROM" from hd922.img (Mac OS ROM 10.2f1, 2796822 bytes), which boot-device names
```

If that disk isn't attached (or has no ROM file there now), Blue starts up
from the first drive as before, and says why; `--rom FILE` overrides it.
For this, a ROM file with parcels (9.1 and later) gets Open Firmware's
settings in the device tree (`/options` with `boot-device`, `/openprom`
with `supports-bootinfo`, without which StartLib doesn't set it) and their
NVRAM partition (`common`, which Blue makes in NVRAM's first 2 KB the first
time; Mac OS's PRAM stays at `0x1400`), when NVRAM is kept (`--nvram`) or
in real time: Mac OS reads them at startup, so instruction-time runs
without `--nvram` are as they were. Mac OS 8.5 and 8.6's Startup Disk
lists only the current startup disk (it asks drives what to put in PRAM,
which Blue's don't answer), and their ROM files' Start Manager starts up
from the first drive with a System Folder: when `boot-device` (set by 9.x)
names such a disk, Blue puts that drive first, so it is (choosing the 8.5
CD in 9.1's Startup Disk starts 8.5 from it). From those systems, start
with `--rom` or without the disk to go back.

### Mac OS 8.5's own ROM file

Mac OS 8.5 also starts up with the ROM file from its own System Folder
(the data fork of "Mac OS ROM", release 0x44F3), from the CD or from a
system installed from it. That ROM has the DAV audio component's 'thng'
resource in it, and registering the component copies the resource's
description onto itself, in the ROM. The NanoKernel drops such stores
(quiet writes; see [architecture.md](architecture.md#the-virtual-machine)); until Blue's kernel did
too, startup stopped there with a bus error.

## Mac OS 9

Mac OS 9.0 installs and boots too, on the same ROM file and NanoKernel
interface. The "Mac OS 9" volume of Apple's 9.0.4 CD (9.0, with the 9.0.4
update alongside) starts up from the CD; its installer puts 9.0 on an empty
disk image in under a minute in real time (turn off "Update Apple Hard Disk
Drivers" again), and the installed system starts up to the Setup Assistant.
The 9.0.4 update on the same CD installs too, and 9.0.4 boots to the Finder
in under 3 s in real time, with fast start (see [architecture.md](architecture.md#time)). Without it, that's
13 s, or 5 s with AppleTalk inactive (Chooser): with AppleTalk active on
Ethernet, startup waits out its timeouts, as on a real Mac with no other
AppleTalk machine around, and behind Blue's NAT (IP only) there never is
one. That's 5 s for a node address and a router (AARP probes, then ZIP
GetNetInfo) and 3 s for Apple Remote Access's personal server to make sure
its name is unique (NBP lookups). Two things were in the way of those
13 s:

- Mac OS 9 waits up to 10 s during "Starting Up…" for the root hubs of the
  machine's USB controllers, and without any it waits it all out. Blue has
  an empty USB controller ([devices.md](devices.md#usb)), whose root hub the ROM's driver finds at
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
interrupts need acknowledging by the kernel (see [interrupt sets](devices.md#interrupt-sets)).

Mac OS 8.6 and later carry a newer NanoKernel in the System file (a `krnl`
resource) and replace the running kernel with it at startup unless that one
is at least as new. Blue's kernel reports NanoKernelInfo version 2.28, with
the multiprocessing fields empty (no "blue task"), so those Systems keep it;
their MPLibrary then doesn't use the version 2 interfaces, as on a machine
without them. One call does come through, though: 8.6's Process Manager
(its ProcessMgrSupport fragment), when nothing needs to run, sleeps until
the next process's wake time with Multiprocessing Services call 55
(`MPDispatch`, kernel call 8, with an AbsoluteTime in r3:r4). Blue naps
then until that time or an interrupt, as for `PowerDispatch`; any other
`MPDispatch` call stops the run, naming it.

## Mac OS 9.1 (from its CD)

9.1's System won't start with `MOS.rom` ("This startup disk will not work
on this Macintosh model"). It needs the ROM file from its own CD (Mac OS ROM
6.1f9, which Blue takes from the CD's System Folder; `MOS91.rom` here is a
copy of its data fork, for `--rom` and the tools), which keeps its contents
in parcels (`src/parcels.h`; `rom parcels` lists them): the Toolbox image;
libraries the ROM prepares and registers at startup (MotherBoardHAL,
TimeManagerLib, NVRAMServicesLib, RTCServicesLib, NativePowerMgrLib,
StartLib), which earlier ROMs had inside
the image; and drivers for machines' devices. Blue loads it and puts the
libraries, and the drivers that match its devices (Cuda, Grackle, the
RTC, NVRAM), in the device tree as the Trampoline would. Its ConfigInfo no longer
describes the address space (the Trampoline in the same file hands the
kernel a page map instead), so Blue lays it out as earlier ROMs describe
it.

The 68k emulator then starts, and the ROM starts Multiprocessing Services,
which want a NanoKernel 2 underneath: one that runs Mac OS as its "blue"
task. For these ROM files Blue's kernel reports one (NanoKernelInfo's
version 2 fields; the ROM checks that the blue process has ID 1, the first
one a NanoKernel 2 makes), and takes their calls (`sc`, or kernel call 8,
with the call number in r0; `src/mp.c`): queues, semaphores, critical
regions, event groups (setting one that's marked for it raises a 68k
software interrupt, which the emulator takes much as it does a device's),
notifications, the areas startup's address space is made of (Mac OS's RAM
and the page map's regions), and timers, which fire on the guest timebase.
`BLUE_LOG_MP=1` logs them, the switches between tasks, timers firing and
software interrupts raised.

Mixed Mode asks with every call whether the current task is the blue one
(`sc` with r0 -1; -2 asks for the task's word too), about a million times
per billion instructions. Generated code and the interpreter answer those
themselves, from values the kernel gives them before each run (`sc_*` in
`src/ppc.h`), unless the kernel would have more to do at that exit (see
the JIT's [kernel calls in place](jit.md#kernel-calls-in-place)); in a 4-billion-instruction run from the
CD, 3.3 of 4.5 million `sc` no longer leave generated code.

Tasks besides the blue one run on the same processor, one at a time: each
has its registers saved while another runs, and the blue task's stay as
its NanoKernel 1 contexts left them. They're scheduled as NanoKernel 2
does it: by priority (a task woken by what it waited for goes ahead of the
blue task; the blue task goes ahead of all once an interrupt is posted to
it, or while its 68k code has interrupts masked, and gives that up when
the emulator yields once its 68k code unmasks them), and in time slices
shared by weight, measured on the guest timebase so that runs stay
reproducible. A task that waits blocks, and makes its call again once
what it waits for comes, or its timeout; tasks can also wait for a time.
The blue task may not wait, and polls instead, letting other tasks run,
or naps while they do. Startup makes three tasks, which first run once the
68k code has unmasked interrupts: one that waits for other tasks to end,
one for the debugger's queue, and the page ager, which then waits for 3
seconds. Meanwhile the Time Manager's timers fire, each setting an event
group that raises a level-1 software interrupt; the 68k handler takes the
group's flags and the Time Manager carries on, and so does startup. Every
3 seconds the page ager looks through the areas for pages to age: it asks
for each area's details, skips the contiguous ones (all but Mac OS's RAM),
and makes some of the RAM's pages older (with no virtual memory, nothing
makes them younger again).

Later in startup Mac OS looks for a CPU plugin, code that knows the
machine's processors, and registers the one for G3 machines with Grackle;
the kernel runs its routines when asked, there and then, with interrupts
off (it says there's one processor). Then Mac OS makes a process of its
own and moves the blue task into it.

The CD then starts up to the Finder ("Mac OS 9.1", Starting Up with the
extensions' icons, then the CD's window), within about a billion
instructions (under a second):

```
build/blue boot --cdrom 91.iso --max 3000000000 --screenshot finder.png
```

Startup used to wait 25 s first. 9.1's Start Manager, unlike earlier ones,
starts up only from the drive Open Firmware booted from until the internal
disk's wait time (25 s unless set in PRAM) has passed, and only then from
any drive. It learns which drive that is from StartLib (one of the ROM
file's libraries), which reads the path Open Firmware leaves in the device
tree (`/chosen`'s `bootpath`: the device, the partition, and the System
Folder's ROM file) and asks each disk driver, through DriverGestalt, which
device-tree node, unit and partition its drives are. Blue's tree had no
such path. For ROM files with parcels it now names the partition the ROM
file came from (if it has boot blocks; otherwise, or with `--rom`, the
first partition with boot blocks, in command-line order), and Blue's disk
driver answers those questions, its drives being a SCSI target at the
disk's PCI device number (StartLib knows units only for SCSI, ATA, USB
and FireWire drives). In real time with fast start the CD is at the
Finder after 16 s of guest time (1.3 s on the host) instead of 41 (3.8 s);
what's left is mostly AppleTalk on Ethernet finding no one (5 s, as with
9.0.4) and the CD's transfers.

The clock and NVRAM work as on an iMac. 9.1 reaches neither through RTAS,
as 8.5 does: its ROM prepares the libraries in order and stops at the
first one that fails, and NVRAMServicesLib failed for want of an NVRAM
device, so RTCServicesLib, the next, never ran (the clock said 12:00 AM
and never moved). For these ROM files the device tree has the NVRAM an
iMac has, `nvram` under Mac I/O at `+0x60000` (8 KB, a byte every 16 bytes
of address space; `src/nvram.c`), which the ROM file's own driver for it
drives. RTCServicesLib then finds the RTC under via-cuda, whose driver
(also the ROM file's) asks the Cuda for the time; setting the time in Date
& Time sets the Cuda's clock, until the next restart (with 8.5, whose RTAS
clock ignores it, a restart goes back to the host's time as well). The
NVRAM is the same 8 KB that RTAS serves 8.5–9.0.4, kept in the same
`--nvram` file, and both keep PRAM in the same place, at `0x1400`, so a
file goes with an install whichever ROM starts it. 9.1 reads NVRAM at
startup and keeps its changes in memory until it shuts down or restarts,
when it writes them to the chip, as on an iMac: changes since startup are
lost when Blue quits otherwise (Turn off, in its menu or when it asks on
quitting). The end-of-run statistics count the chip's reads and writes.

9.1 installs onto a disk image with its own installer, as 9.0 does
(`build/blue disk create hd91.img 2G "Macintosh HD"`, then the CD and the
disk; Mac OS Install from the CD's window: Continue, Select with Macintosh
HD as the destination, Continue, Continue, Agree, then Options... to turn
off "Update Apple Hard Disk Drivers", and Start). A scripted run installed
it (268 MB written) by instruction 40 billion; the installed system starts
up from the disk alone (`rom: "Macintosh HD:System Folder:Mac OS ROM"`) to
the Finder with the Mac OS Setup Assistant, and takes the guest tools (see
[guest-tools.md](guest-tools.md)). By then the screen has gone to sleep, as Energy Saver puts
it after a while without input, and the first click or key press only
wakes it: the installer's last dialog takes the second.

## Mac OS 9.2.2 (from its CD)

The MacOS9Lives 9.2.2 CD (`922.iso`, "Mac OS 9.2.2 Universal", for Macs
Apple's 9.2.2 doesn't support; its ROM file is Mac OS ROM 10.2f1, a
modified 10.2.1 compatible with any `MacRISC` model) starts up to the
Finder on the same kernel, and opens its Read Me in SimpleText, as under
QEMU:

```
build/blue boot --cdrom 922.iso --max 3000000000 --screenshot finder.png
```

The Finder is up by instruction 1.5 billion; in real time with fast start
after 13-14 s of guest time (about 1 s on the host; how far guest time
gets in that second depends on how busy the host is), and then the host
sleeps most of the time. Its ROM file carries NanoKernel 2.28 (the version
Blue's kernel reports), and wants a few things 9.1's doesn't:

- The info records as NanoKernel 2.28 has them: HWInfo version 0x0108 (0xC0
  bytes), ProcessorInfo 0x0112 (0xA0 bytes, with the processor's clock
  rates) and SystemInfo 0x0107. The ROM's 68k startup reads HWInfo's new
  CascadeInfo pointer (`+0xA8`: interrupt controllers cascaded from an
  OpenPIC, none on an iMac); with the old, shorter record it read the end of
  RAM instead and stopped. Earlier ROMs still get NanoKernel 1's records.
- Multiprocessing Services calls 62 (`MPGetNextCpuID`), 44 and 45
  (`MPStartScheduling`, `MPStopScheduling`: the power manager stops the
  other processors before it sleeps; the one the kernel started on can't
  be) and 132 (`NKSetClockDriftCorrection`). The kernel now has the
  machine's coherence group and processor, as a NanoKernel 2 has them
  before its first call, so the blue task, page queue and address space
  have the real kernel's IDs.
- A task ('accu') that measures the timebase's drift against KeyLargo's
  counter at Mac I/O `+0x15038`. On the iMac's Mac I/O that's the floppy
  controller's cell, which now reads as zeros (it faulted before): the task
  finds the counter stopped and corrects nothing.
- Its ROM file has a lanLib for the iMac's own Ethernet controller (BMAC+),
  which the Trampoline puts in the Ethernet node in place of the firmware's.
  Blue has no BMAC, so its own lanLib stays.
- Kernel call 7 (CacheDispatch) with a selector for no cache (4), from the
  power manager as it dozes after half an hour of idling: -2, as both
  NanoKernels answer.
- `AAPL,interrupt-index` in the nodes with interrupt sets. Its
  DriverServicesLib makes a node's interrupt set only if the node has one
  (its Trampoline gives every node with interrupts one: each interrupt's
  slot in HWInfo's VectorLookupTable, here the source), so without it the
  sound chip, the USB controller and the Ethernet node had no sets: no
  sound, USB's driver waiting for an interrupt that never came, and lanLib
  polling. Blue gives them one for ROM files whose Trampoline makes it
  (9.2's; earlier ROMs' trees are as they were; the display's set has
  always had one). The sound plays (alerts; the Sound control panel's
  volume and Mute), USB's root hub runs with its interrupts (start of
  frame, frame number overflow), and lanLib receives by interrupt (TCP/IP
  gets its address by DHCP in real time).
- The sound codec's status says it's valid (bit 22), as a running codec's
  does. The sound plug-in (two start on 9.2.2) polls for it after setting
  the codec up, and gives up after a thousand polls: 2 s of guest time in
  instruction time, 17 s in real time with fast start, whose slower guest
  processor stretches each poll (the CD's Finder came at 22-30 s of guest
  time). It's set for every system: 8.5's and 9.1's plug-ins wait for it
  too (with fast start, the 8.5 CD's Finder came 0.9 s later without it,
  9.1's 1.4 s).

The CD installs with Apple Software Restore (its Read Me says how), which
copies the CD's disk image onto a volume, erasing it. Drive Setup, which
opens too, lists no drives (it knows Apple's own ATA and SCSI drives only)
and isn't needed:

```
build/blue disk create hd922.img 2G "Macintosh HD"
build/blue boot --disk hd922.img --cdrom 922.iso --window --nvram blue.nvram
```

In Apple Software Restore (open it from the CD's window), click Switch Disk
until the destination disk is "Macintosh HD", then Restore, and OK. A
scripted run restored 671 MB in under 20 billion instructions; the volume is
then called MacOS9Lives. Click Quit when it says so: Apple Software Restore
quits the Finder while it works, and the Finder, started again then, makes
the new volume's Desktop Folder and Trash. Then `build/blue boot --disk
hd922.img --window --nvram blue.nvram` starts it, to the Finder (the first
time, with the Setup Assistant over it), in real time and in instruction
time. In
instruction time its startup used to stop short of the Finder until a key
was pressed: a Time Manager software interrupt the 68k emulator never took,
and nothing else to interrupt the native code it then napped in. The
display's vertical blank interrupts it 60 times a second now, as an iMac's
does ([plan.md](plan.md#findings), Findings).

A volume Apple Software Restore has restored but not yet given back (Blue
closed with its last dialog up) has neither folder, and on its first
startup the Setup Assistant, quit, leaves the desktop without a menu bar:
it puts its Register with Apple alias in the Desktop Folder as it quits,
and without one it quits without showing the menu bar again. The Finder
makes the folder then, so the next startup is as it should be. That's Mac
OS's own doing, with that disk, on any Mac. What led there: Mac OS 9.2.2's
power manager dozes after half an hour without user activity (in a
scripted restore, half an hour after the last click, its last dialog up),
and it learns of activity from the ADB keyboard's key-downs but not from an
ADB mouse (the iMac's mouse is USB, and its driver tells the power manager
itself), so the dialog's Quit took no click until a key was pressed. While
Mac OS dozes, Blue now has mouse input go after a tap of Option, which
wakes it (`src/cuda.c`; [plan.md](plan.md#findings), Findings): clicks and moves work after
half an hour's idling too.

The CD's system and the restored one start up as quickly as before in
real time, give or take the half second USB's root hub and the sound
plug-ins now take at startup (measured without fast start too), and the
host sleeps while they idle: their sound output's DMA stops when nothing
plays, and a USB controller with only frame number overflow enabled wakes
the host when a frame overflows (every 33 s), not at every frame (1 ms).
The restored system, set up and idle at the Finder, left the host asleep
37 s of 40.
