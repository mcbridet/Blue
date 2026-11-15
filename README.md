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

- **ROM:** the "Mac OS ROM" file from the startup disk's System Folder, as
  a New World Mac's firmware starts it (see Status); `--rom` for one of your
  own. Tested: 8.5's own (Toolbox release 0x44F3), 8.6's (0x44B5), 9.1's
  (6.1f9, in parcels), MacOS9Lives 9.2.2's (10.2f1, in parcels, 0x45F6),
  and `MOS.rom`, a 1.x file (0x44F1, compatible with
  `iMac,1 PowerMac1,1 PowerBook1,1`; the family SheepShaver uses).
- **OS:** the retail Mac OS 8.5 CD (`85.iso`). It boots with the ROM file it
  carries itself (1.1.x, no `<COMPATIBLE>` list) or with `MOS.rom`.
- **Machine identity:** to be decided by the universal info tables syshm
  builds. Retail 8.5 predates the Blue & White G3, so an iMac identity is the
  safer first choice; B&W G3 (`PowerMac1,1`) for 8.5.1 and later.
- **OS ladder:** 8.5 → 8.6 → 9.0.4 on the NanoKernel v1 interface, then the
  v2 interface for 9.1 and 9.2.2 (both CDs start up to the Finder; see
  below).

## Status

**Retail Mac OS 8.5 installs onto a disk image and boots from it, and so
does Mac OS 9.0 (see below).** Both also boot straight from their CDs.
Mac OS 8.6 works too, on the same ROM file and NanoKernel interface. Mouse
and keyboard work in a window or from scripted input, and sound plays
through the host (in real time). Games run accelerated by the guest tools'
3D engine, on the host's GPU: Unreal Tournament plays, and Bugdom (QuickDraw
3D, Game Sprockets) reached about 100 frames per second in play on an M1
Max, with music (it needs URL Access, which comes with Mac OS 8.6 and
later; frames for the screen now come at most 60 a second, see Guest
tools). The built-in Ethernet reaches the host's
network, so Internet Explorer loads web pages. CD-ROM drives play audio CDs
and games' CD soundtracks (cue sheets) through the sound chip, as the iMac's
drive did.

```
build/syshm disk create hd.img 2G "Macintosh HD"      # an empty HFS volume
build/syshm boot --disk hd.img --cdrom 85.iso --window --nvram syshm.nvram
```

Run "Mac OS Install" from the CD (about 30 billion instructions, about a
minute). In its Options, turn off "Update Apple Hard Disk Drivers": Drive
Setup only knows Apple's own ATA/SCSI drives and quits on ours. Then boot the
installed system with just `--disk hd.img`. Drives are tried in command-line
order. In the window, the GUI/Super key is Command and Alt is Option.

As a New World Mac's firmware does, syshm starts the "Mac OS ROM" file from
the startup disk: the first drive, in command-line order (an image's HFS
partitions in partition-map order), whose blessed System Folder holds a file
of type `tbxi`. Above, the new disk is empty, so that's the CD's; once 8.5
is installed, the disk's own. `src/hfsread.c` reads HFS and HFS+ volumes
(HFS+ also inside an HFS wrapper), only the blocks it needs. syshm says where
the file came from:

```
rom: "Mac OS 9.1:System Folder:Mac OS ROM" from 91.iso, partition "Toast 4.0 PPC Partition" (Mac OS ROM 6.1f9, 2448470 bytes)
```

`--rom FILE` starts a ROM file (or a raw 4 MB ROM image) of your own
instead. `syshm rom info` and the other `rom` commands take a disk or CD
image too, and show the ROM file that would be started from it.

Restart starts the machine over in the same window, from the ROM with
memory and devices reset as on a real Mac (NVRAM is kept, and so are guest
time and the instruction count, which input scripts and `--max` go by).
Shut Down quits.

Mac OS 8.5 also starts up with the ROM file from its own System Folder
(the data fork of "Mac OS ROM", release 0x44F3), from the CD or from a
system installed from it. That ROM has the DAV audio component's 'thng'
resource in it, and registering the component copies the resource's
description onto itself, in the ROM. The NanoKernel drops such stores
(quiet writes; see Virtual machine description); until syshm's kernel did
too, startup stopped there with a bus error.

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

### Mac OS 9.1 (from its CD)

9.1's System won't start with `MOS.rom` ("This startup disk will not work
on this Macintosh model"). It needs the ROM file from its own CD (Mac OS ROM
6.1f9, which syshm takes from the CD's System Folder; `MOS91.rom` here is a
copy of its data fork, for `--rom` and the tools), which keeps its contents
in parcels (`src/parcels.h`; `rom parcels` lists them): the Toolbox image;
libraries the ROM prepares and registers at startup (MotherBoardHAL,
TimeManagerLib, NVRAMServicesLib, RTCServicesLib, NativePowerMgrLib,
StartLib), which earlier ROMs had inside
the image; and drivers for machines' devices. syshm loads it and puts the
libraries, and the drivers that match its devices (Cuda, Grackle, the
RTC, NVRAM), in the device tree as the Trampoline would. Its ConfigInfo no longer
describes the address space (the Trampoline in the same file hands the
kernel a page map instead), so syshm lays it out as earlier ROMs describe
it.

The 68k emulator then starts, and the ROM starts Multiprocessing Services,
which want a NanoKernel 2 underneath: one that runs Mac OS as its "blue"
task. For these ROM files syshm's kernel reports one (NanoKernelInfo's
version 2 fields; the ROM checks that the blue process has ID 1, the first
one a NanoKernel 2 makes), and takes their calls (`sc`, or kernel call 8,
with the call number in r0; `src/mp.c`): queues, semaphores, critical
regions, event groups (setting one that's marked for it raises a 68k
software interrupt, which the emulator takes much as it does a device's),
notifications, the areas startup's address space is made of (Mac OS's RAM
and the page map's regions), and timers, which fire on the guest timebase.
`SYSHM_LOG_MP=1` logs them, the switches between tasks, timers firing and
software interrupts raised.

Mixed Mode asks with every call whether the current task is the blue one
(`sc` with r0 -1; -2 asks for the task's word too), about a million times
per billion instructions. Generated code and the interpreter answer those
themselves, from values the kernel gives them before each run (`sc_*` in
`src/ppc.h`), unless the kernel would have more to do at that exit (see
the JIT's kernel calls in place); in a 4-billion-instruction run from the
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
build/syshm boot --cdrom 91.iso --max 3000000000 --screenshot finder.png
```

Startup used to wait 25 s first. 9.1's Start Manager, unlike earlier ones,
starts up only from the drive Open Firmware booted from until the internal
disk's wait time (25 s unless set in PRAM) has passed, and only then from
any drive. It learns which drive that is from StartLib (one of the ROM
file's libraries), which reads the path Open Firmware leaves in the device
tree (`/chosen`'s `bootpath`: the device, the partition, and the System
Folder's ROM file) and asks each disk driver, through DriverGestalt, which
device-tree node, unit and partition its drives are. syshm's tree had no
such path. For ROM files with parcels it now names the partition the ROM
file came from (if it has boot blocks; otherwise, or with `--rom`, the
first partition with boot blocks, in command-line order), and syshm's disk
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
lost when syshm quits otherwise (closing the window, or Turn off in its
menu). The end-of-run statistics count the chip's reads and writes.

Not done yet: installing 9.1 onto a disk image with its installer and
starting up from that.

### Mac OS 9.2.2 (from its CD)

The MacOS9Lives 9.2.2 CD (`922.iso`, "Mac OS 9.2.2 Universal", for Macs
Apple's 9.2.2 doesn't support; its ROM file is Mac OS ROM 10.2f1, a
modified 10.2.1 compatible with any `MacRISC` model) starts up to the
Finder on the same kernel, and opens its Read Me in SimpleText, as under
QEMU:

```
build/syshm boot --cdrom 922.iso --max 3000000000 --screenshot finder.png
```

The Finder is up by instruction 1.5 billion; in real time with fast start
after 13-14 s of guest time (about 1 s on the host; how far guest time
gets in that second depends on how busy the host is), and then the host
sleeps most of the time. Its ROM file carries NanoKernel 2.28 (the version
syshm's kernel reports), and wants a few things 9.1's doesn't:

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
  syshm has no BMAC, so its own lanLib stays.
- Kernel call 7 (CacheDispatch) with a selector for no cache (4), from the
  power manager as it dozes after half an hour of idling: -2, as both
  NanoKernels answer.
- `AAPL,interrupt-index` in the nodes with interrupt sets. Its
  DriverServicesLib makes a node's interrupt set only if the node has one
  (its Trampoline gives every node with interrupts one: each interrupt's
  slot in HWInfo's VectorLookupTable, here the source), so without it the
  sound chip, the USB controller and the Ethernet node had no sets: no
  sound, USB's driver waiting for an interrupt that never came, and lanLib
  polling. syshm gives them one for ROM files whose Trampoline makes it
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
build/syshm disk create hd922.img 2G "Macintosh HD"
build/syshm boot --disk hd922.img --cdrom 922.iso --window --nvram syshm.nvram
```

In Apple Software Restore (open it from the CD's window), click Switch Disk
until the destination disk is "Macintosh HD", then Restore, and OK. A
scripted run restored 671 MB in under 20 billion instructions; the volume is
then called MacOS9Lives. Then `build/syshm boot --disk hd922.img --window
--nvram syshm.nvram` starts it, to the Finder (the first time, with the
Setup Assistant over it), in real time and in instruction time. In
instruction time its startup used to stop short of the Finder until a key
was pressed: a Time Manager software interrupt the 68k emulator never took,
and nothing else to interrupt the native code it then napped in. The
display's vertical blank interrupts it 60 times a second now, as an iMac's
does (plan.md, Findings).

The CD's system and the restored one start up as quickly as before in
real time, give or take the half second USB's root hub and the sound
plug-ins now take at startup (measured without fast start too), and the
host sleeps while they idle: their sound output's DMA stops when nothing
plays, and a USB controller with only frame number overflow enabled wakes
the host when a frame overflows (every 33 s), not at every frame (1 ms).
The restored system, set up and idle at the Finder, left the host asleep
37 s of 40.

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

### Shared folder

`--share DIR` shows a host folder to Mac OS as a volume of its own, named
after the folder, with the files and folders in it: the way to move files
between the host and Mac OS, such as a program downloaded on the host to
install. It's an ordinary HFS volume, so everything works on it (the
Finder, installers, programs run from it), and what Mac OS changes on it
goes back to the folder when Mac OS shuts down.

```
build/syshm boot --disk hd.img --share ~/Mac --window --nvram syshm.nvram
```

The volume (`src/share.c`) is made when syshm starts: its catalog and the
rest of its metadata in memory (`src/hfs.c` lays volumes out), its files'
contents read from the host's files only as Mac OS reads them. It has half
as much free space as there is in it, at least 1 GB but under 2 GB (older
installers count free space in 31 bits). It is attached after the other
drives, as one of the four, and is never the startup disk (no boot blocks,
no System Folder). Names become Mac OS Roman, with `/` for `:` as macOS
shows them; one longer than 31 characters, or the same as another's to
Mac OS (which ignores case), is shortened with the item's ID, as Mac OS 9
does (`A very long file name#1A.txt`). Dot files, symbolic links to
folders and files over 2 GB (HFS's limit) are left out, and syshm says so.
Dates are the host's, but none later than the guest's clock as the run
starts (`--fixed-time`'s, else the host's), and none created after it was
modified (as a file macOS copied or unpacked is: it's created when it was
modified): Norton Utilities takes either for damage, a volume's creation
date in the future for "damage to the Master Directory Block (Error
16,005)". The volume was created when the folder was (on Linux, when it
last changed), so Mac OS knows it from run to run.
Resource forks and Finder info (type, creator, flags) are the host's: on
macOS the file's own (`..namedfork/rsrc`, `com.apple.FinderInfo`), and
otherwise an AppleDouble file, `._name` beside it or in a `__MACOSX` folder
(as zip archives keep them). A file without Finder info gets a type and
creator from its extension (`.sit`, `.hqx`, `.bin`, `.zip` for StuffIt
Expander; `.txt`; `.jpg`, `.gif`, `.png` for PictureViewer; `.pdf`; `.img`,
`.dmg`, `.toast`, `.dsk` for Disk Copy; `.mov`, `.mp3`, `.aiff`, `.html`
and a few more), else SimpleText's if it's plain text, else `????`.
Archives in the StuffIt 5 format (from 1998 on; `lsar` calls them "StuffIt
5") need StuffIt Expander 5 or later; Mac OS 8.5 and 8.6 come with 4.5,
which quits without a word on them. Either install Expander 5.5 (as a
`.hqx` or `.bin`, which 4.5 opens), or expand on the host with The
Unarchiver's `unar` (Homebrew's `unar`), which keeps resource forks and
Finder info on macOS, so they reach Mac OS through the share.

What Mac OS writes goes to a temporary file (in `$TMPDIR`, gone when
syshm exits), never to the folder during the run. At the end of the run,
if Mac OS put the volume away (Shut Down, or Put Away), syshm writes back
what changed: new, changed, renamed, moved and deleted files and folders,
with their resource forks and Finder info as the host keeps them (on
macOS the file's own; otherwise an AppleDouble `._name` file, only for a
file with a resource fork or Finder info its name and contents wouldn't
give it) and their dates. Nothing of the host's is destroyed: a file or
folder Mac OS deleted (emptying the Trash or not) or replaced is moved to
`DIR/.syshm-recovery/<date and time>/`, under the path it had, and a file
that changed on the host as well during the run is kept, with Mac OS's
version beside it as `name (Mac OS).ext`. What Mac OS and its extensions
keep on a volume for themselves (the desktop database, the Trash, invisible
files at its top) isn't written back. syshm lists what it did:

```
share: writing back what Mac OS changed on "Shared" to /Users/me/Shared
share:   changed Read Me.txt (the host's was put in .syshm-recovery/2026-11-15 00.13.05)
share:   new untitled folder
share: 1 new, 1 changed, 0 moved or renamed, 0 deleted; the host's old ones are in /Users/me/Shared/.syshm-recovery/2026-11-15 00.13.05
```

If the run ends without Mac OS having put the volume away (the window
closed, or Turn off in syshm's menu, rather than Mac OS shutting down),
nothing is written back, as the volume may be half-written; if anything
of the user's had changed on it, it is saved as a disk image in the
recovery folder instead (attach it with `--disk` to get at what's on it;
Mac OS may check it first, as after a crash). Mac OS marks the volume in use
in its MDB as soon as it mounts it (its first write) and put away when it
unmounts it; that's how syshm tells. A restart keeps the volume as Mac OS
left it. `SYSHM_LOG_SHARE=1` logs Mac OS's writes.

Changes cross only once a run: files added on the host during the run
appear in Mac OS the next time, and changes made in Mac OS reach the host
at the end. Text files keep their line endings (a host's LF shows as boxes
in SimpleText). Folders' Finder info (window positions, custom icons) and
the Locked checkbox aren't written back.

### Floppies and CDs: removable drives

The floppy drive (`--floppy`) and the CD-ROM drives (`--cdrom`) take disc
images that come and go while Mac OS runs, mainly to install software that
came on floppies or on several CDs. Mac OS ejects a disc as on any Mac: Put
Away or dragging it to the Trash, Eject in the Finder's Special menu, an
installer's own eject, an Initialize dialog's Eject. The drive is then
empty, and syshm says so (`cdrom: Mac OS ejected ut.iso`). To put an image
in, drop it on the window: one of a floppy's size (2880K or less) goes in
the floppy drive, any other in a CD-ROM drive, the first that's empty (else
one whose disc Mac OS isn't using). Mac OS learns of a disc going in as
from a real drive, by a disk-inserted event, so whatever application is in
front and waiting for one (an installer, Disk Copy) gets it, and mounts it.

```
build/syshm boot --disk hd.img --cdrom "Game CD 1.iso" --window
build/syshm boot --disk hd.img --cdrom-drive --floppy "Install 1.image" --floppy "Install 2.image" --window
```

The window's menu (F12) shows what's in each drive, with an Eject button.
syshm won't take a disc out that Mac OS has mounted (it's using it: the
button is disabled, and the menu says to put it away in Mac OS); one that
isn't mounted (unreadable, say, or just put in) it can. If an image can't
go in (no empty drive, a drive Mac OS is using, not an image syshm takes),
the menu opens to say why.

`--floppy IMAGE` puts a floppy disk image in the floppy drive; given
several times, for a set of disks, the first is in the drive at startup
and the others go in, in turn, as Mac OS ejects the one in (a second
later, as from a quick hand): a multi-disk installer that ejects disk 1 and
asks for disk 2 gets it with nothing to do, and the menu lists what's still
to come. Dropping an image puts that one in instead (when the drive is
free), and the rest follow as before. `--floppy-drive` gives a floppy drive
with nothing in it, `--cdrom-drive` an empty CD-ROM drive (one of the four
disks). Input scripts have `floppy insert FILE` and `cdrom insert FILE` (the
rest of the line), `floppy eject` and `cdrom eject` (the first CD-ROM drive
whose disc Mac OS isn't using), which follow the same rules.

CD-ROM drives answer as CD-ROM drives (see CDs: audio and cue sheets
below), and the swappable ones say their disc is ejectable, and that a
restart keeps it in. A CD-ROM drive gives
Mac OS a drive for each HFS partition of the disc in it at startup (one if
it's empty); a disc put in later with more shows its first ones, and a
disc comes out once none of its volumes is in use. A CD put in while Mac OS runs is never
the startup disk (it's shown to Mac OS once applications run); one in at
startup may be, as before, and Mac OS won't eject the startup disk ("The
startup disk cannot be removed from the desktop"). CD images are raw (an
`.iso`, `.toast`, `.cdr` or `.img`, with or without a partition map) or cue
sheets (below); compressed ones and UDIF `.dmg` (`hdiutil convert -format
UDTO` makes a raw `.cdr`) are refused, saying what they are, and so is a
`.bin` of raw sectors given without its `.cue` (when it starts with a data
track; one that starts with audio can't be told from a plain image). Ejectable drives change what Mac OS does all along (it looks at
the drives' flags as it runs), so CD-ROM drives are ejectable only where
discs can be swapped: in a window or real time, with `--cdrom-drive`, or
with an input script that swaps CDs (`cdrom` commands); other
instruction-time runs keep them fixed, as before, the same instruction for
instruction.

#### CDs: audio and cue sheets

A CD-ROM drive (every one, swappable or not) answers Mac OS as Apple's
CD-ROM driver does on a real Mac (`src/cdrom.c`, after Apple's Technical
Note DV22): its driver is named `.AppleCD`, DriverGestalt says `cdrm`, and
it has the calls that AppleCD Audio Player, the Control Strip's CD module,
Audio CD Access, QuickTime and games make: the table of contents
(ReadTOC), where the drive is (AudioStatus, the Q subchannel), playing
(AudioPlay, AudioTrackSearch, AudioPause, AudioStop, AudioScan), the
drive's volume and channels (AudioControl, ReadAudioVolume), the track
play lists the driver plays itself (AppleCD Audio Player's shuffle and
programs, with repeat), reading audio digitally (ReadAudio, with the Q or
all of the subchannel). The Finder shows a CD and a CD-ROM drive icon (a
disc's own volume icon, if it has one, goes before them), and Get Info says
"syshm CD-ROM drive".

Images with audio come as cue sheets (`.cue`, with `.bin` files, as most
CD rips are): tracks in raw 2352-byte sectors (audio; mode 1 or mode 2
data), in 2048- or 2336-byte data sectors, or in WAVE or AIFF files (44.1
kHz, 16-bit stereo); `BINARY` and `MOTOROLA` files, one for the disc or one
a track; `INDEX 00` pregaps in the file and `PREGAP`/`POSTGAP` silence that
isn't; several sessions (`REM SESSION`, as on CD-Extra discs); `CATALOG` and
`ISRC`. The disc's own addresses are worked out as a drive has them (track 1
at 00:02:00). `syshm disk toc FILE` lists what the drive will say:

```
$ build/syshm disk toc "Quake [1996].cue"
Quake [1996].cue: 11 tracks, 1 session, lead-out at 61:47:11 (LBA 277886)
   1  mode 1  00:02:00  LBA      0  02:47:20  (Mac OS sees its data)
   2  audio   02:51:20  LBA  12695  05:08:38  (pregap 00:02:00)
   3  audio   07:59:65  LBA  35840  02:25:60  (pregap 00:00:07)
   ...
```

Mac OS mounts the data track (the first one): an HFS volume, with or
without a partition map, or ISO 9660 with ISO 9660 File Access (in an
installed system: the PC Quake CD shows up as QUAKE106). A disc of audio
alone reads as blank data, and Foreign File Access with Audio CD Access
mounts it as "Audio CD 1", its tracks as documents (on 8.5; AppleCD Audio
Player opens them, QuickTime's MoviePlayer converts them to AIFF files,
reading them digitally).

The drive plays audio as the iMac's did: into the sound chip's analog input,
which Mac OS sets to play the CD through to the outputs (8.5 selects the CD
and turns loop-through on; with no input selected, as Mac OS 9 leaves it
here, it plays through too). So CD audio is mixed with Mac OS's own sound
and follows its volume and mute (Monitors & Sound), as on the real machine,
and the drive's own volume (AudioControl) comes before. It's paced by guest
time: where the drive is follows from where and when play started, so
instruction-time runs are reproducible, and `SYSHM_SOUND_WAV` records it.
To play a music CD's or a game's soundtrack, start Mac OS with the cue
sheet, or drop it on the window:

```
build/syshm boot --disk hd.img --cdrom "Quake [1996].cue" --window    # then AppleCD Audio Player, in the Apple menu
```

On an installed 8.5, the Control Strip's CD module plays a disc that's in at
startup by itself (its AutoPlay); for one put in later it keeps the table of
contents it read at startup (none) and doesn't, while AppleCD Audio Player
plays either. AppleCD Audio Player numbers a mixed disc's audio tracks from
1 (on the Quake CD, its Track 1 is the disc's track 2).

#### Floppies

Images: raw ones of 400K, 720K, 800K, 1440K and 2880K disks (`.img`,
`.dsk`, `.ima`, `.flp`), and Disk Copy 4.2 ones (`.image`: an 84-byte
header, the blocks, then the 400K and 800K disks' tags; also in
MacBinary), whose checksums syshm keeps right as Mac OS writes (they're
worked out again a second after the last write, and when the disk is
ejected or flushed). They're told apart by their contents and size, not
their names. Compressed images and archives (Disk Copy 6's NDIF, DART,
UDIF `.dmg`, StuffIt, zip, gzip, BinHex) aren't taken: syshm says what
they are, or that their size is no floppy's. Convert them to a raw or Disk
Copy 4.2 image first (with Disk Copy in Mac OS, or a tool on the host). An
image is read-only (Mac OS shows it locked) if its file can't be written,
or is locked in the Finder; lock an image to keep it as it was, as Mac OS
writes to a writable disk as soon as it mounts it (its volume information,
the desktop database). Mac OS mounts what it can read: HFS, FAT (PC
floppies) with File Exchange, which a full install has (the 8.5 CD's
System doesn't: it offers to initialize them). MFS disks (System 1-6 era,
400K) are refused: Mac OS 8.5 and later can't read them. A blank image (or
one Mac OS can't read) brings up Mac OS's "unreadable, initialize?"
dialog, and Erase Disk works on floppies (Format, Verify, then a new
volume; the format offered is the image's size). `syshm disk create
blank.img 1440K "Untitled"` makes a blank formatted floppy (400K, 720K,
800K or 1440K).

The iMac has no floppy controller, so none is emulated: the drive is a
third kind of drive of syshm's own disk driver (`src/disk.c`, images in
`src/floppy.c`, which drive an image goes in `src/drives.c`), at PCI
device 7, much as a USB floppy drive is on a real iMac, and needs nothing
installed in Mac OS. It answers as a floppy drive
(DriverGestalt 'devt' 'flop', a floppy disk's icon and the drive's, in
color, the 'where' of Get Info, the format list) and says its disk is
ejectable, and Mac OS keeps the disk in at a restart. A floppy is never the
startup disk: the drive is empty as far as Mac OS knows until applications
run (see plan.md, Findings), and syshm doesn't look for a ROM file on it.
Transfers take guest time at the disks' rate. The status bar's floppy light
(with a floppy drive only) is dimmed while it's empty. Disk Copy can't
make floppies from images in this drive (it looks for Apple's floppy
drives): put the image in instead, or a copy of it.

### MacBench

ZD MacBench 5.0 runs (tested from a shared folder, on an installed 8.5):
it registers, opens its main window and runs its Processor and Floating
Point tests (the others are untried). Right after registering it uses
Gary Davidian's skeleton key, a kernel call that gives a program
supervisor mode, to read the processor's L2CR and HID0 (see the kernel
calls and privileged instructions below); without it, MacBench quit with
"an error of type 1". Its scores are relative to its reference machine, a
Power Macintosh G3/300 (1000 in every test). Benchmark in real time, in a
window: in instruction time, guest time follows the instruction count, so
the guest processor looks like a 16.6 MIPS one (Processor: 62, 6% of the
G3/300); in one real-time run on an M5, with other work on the host,
Processor scored 18,324 (the test took about 25 s). MacBench times things
by polling the VIA's timer in a tight loop; in real time, a loop whose
device register read fell on the last instruction before a sync point
kept syshm from ever reaching one (guest time stood still, and the window
froze); it no longer does. Its Floating Point test runs for about 50 s
of guest time (five parts of about 10 s that count iterations: an FFT, an
LU decomposition, MathLib's functions, NaNs and infinities among their
arguments, a table of log factorials from MathLib's log and exp,
arithmetic on special values); the score weighs them like a harmonic
mean, so the log factorials (60% of it on syshm) count most. Headless in
real time on an M5 (`--realtime`, a script in seconds; medians of 4 runs,
interleaved) it scored 7,085 before the JIT's floating-point work above
and 16,616 after (in a later session, 6,250 and 14,700: the host's speed
varies), while the Processor test's code runs as fast as before; see
Findings in plan.md.

Its CD-ROM tests run with the MacBench 5.0 CD (`MacBench5.toast`) in a
CD-ROM drive, on an installed 8.5 and 9.2.2: the CD-ROM test (148 MB of
File Manager reads replayed from five CD titles; 1,442 on 8.5 in
instruction time, 4,261 on 9.2.2 with `clock 400`, 5,366-5,383 in real
time, a window or not) and the CD-ROM inspection tests (sequential and
random reads of 512 bytes and 1 MB). They read through the File Manager
only; below it MacBench just takes an inventory of the drives (see
Findings in plan.md, "MacBench's CD-ROM test"). A report of the test
freezing MacBench on 9.2.2 in a window (the Memory Manager faulting on a
broken system heap, then the Toolbox) didn't reproduce here.

### Guest tools

Some things only code inside Mac OS can do: syshm Enhancements, syshm
3D Engine and syshm's OpenGL libraries (below). In a window, syshm offers to install them: before Mac OS
starts it looks in the startup disk's System Folder (its folders,
Extensions whatever its name and Extensions (Disabled) among them) for
them, and once the Finder is up, if one is missing or isn't this syshm's
(both forks compared with what it makes), a box over the screen says so,
with Install (or Update) and Not Now; Mac OS runs on meanwhile. On
Install, Mac OS writes the files itself (see Findings in plan.md): each
goes to a temporary file in the folder it belongs in, which then takes the
old copy's place, so an error (a full or locked disk) leaves nothing half
written; a disabled copy is updated where it is and stays disabled.
syshm Enhancements starts at once (an older one running is asked to quit
first), and programs opened from then on find the 3D engine, without a
restart. This works on Mac OS 8.5, 8.6 and 9.2.2 installed on disk images
(9.1 by the same path; there was no installed 9.1 to try). The outcome is
shown and printed:

```
tools: installed syshm Enhancements, syshm 3D Engine, syshm OpenGL Library, syshm OpenGL Memory and syshm OpenGL Utility in "Macintosh HD:System Folder:Extensions". syshm Enhancements is running. Programs opened from now on find the 3D engine and syshm's OpenGL.
```

Mac OS 9 has OpenGL libraries of Apple's with the names syshm's have
(OpenGLLibrary, OpenGLMemory, OpenGLUtility), so only one set can be
enabled. Once syshm's are in Extensions, the installer moves Apple's to
Extensions (Disabled) and says so (the offer says it will). If syshm's
are gone (deleted by hand) while Apple's are disabled, the offer is to put
Apple's back (Put Back) instead of installing syshm's. Checked on 9.2.2:

```
tools: installed syshm OpenGL Library, syshm OpenGL Memory and syshm OpenGL Utility in "MacOS9Lives:System Folder:Extensions"; moved Apple's OpenGLLibrary, OpenGLMemory and OpenGLUtility to "MacOS9Lives:System Folder:Extensions (Disabled)". Programs opened from now on find syshm's OpenGL.
```

Nothing is offered when the startup disk is a CD, an image attached
read-only, or a locked volume, nor without a window: an input script can
answer instead (`tools install`, `tools later`), and that turns the check
on. The startup disk is the partition the ROM file came from (with
`--rom`, the first with boot blocks).

They can still be installed by hand: `syshm tools create FILE` writes a
small volume, "syshm Tools", holding them, which `--cdrom FILE` attaches
(read-only):

- **syshm Enhancements**: drag it onto the System Folder (the Finder puts it in
  Extensions) and restart (or let syshm install it). From then on, resizing
  the window changes the
  screen's resolution to match once the window has stopped changing (while
  it's being dragged, the window scales the screen), and the Finder lays out
  the desktop again. A
  resolution chosen in Mac OS (Monitors & Sound, or the Control Strip)
  resizes the window instead. Without it, the window scales the
  screen. On high-density displays a screen pixel is a point, so Mac OS
  appears at the size of other programs; with `--integer-scale` it is a
  whole number of pixels instead (2 at 1.7x), for the crispest pixels. It
  also shares the clipboard (below). One installed by a syshm from before
  the clipboard isn't this syshm's (so in a window syshm offers to update
  it), and it still switches modes until then: its data carries its
  version, as the 3D engine's does, so that syshm finds the older copy's
  data where it is, and says so. An `appe` only starts from Extensions, so
  a CD's system can't run it; to try it there, `SYSHM_TOOLS_APPL=1 syshm
  tools create FILE` makes the volume's copy an application the Finder
  opens with a double-click.
- **Clipboard** (with syshm Enhancements, in a window in real time): text
  copied in Mac OS is on the host's clipboard, and text copied on the host
  is what Command-V pastes in Mac OS, in the front application, without
  switching applications (tested: SimpleText, Note Pad, Scrapbook, the
  Finder's names and Internet Explorer 4's fields on 8.5, SimpleText on
  9.1). Mac OS keeps text in Mac OS Roman with CR line ends (`TEXT`), and
  applications that know Unicode (Mac OS 9's MLTE) in UTF-16 too (`utxt`);
  the host has UTF-8 with LF. Accents come out precomposed (macOS often
  decomposes them), and characters Mac OS Roman lacks become something
  close where there is one (½ "1/2", → "->", other spaces a space) or
  else "?"; `utxt`, which syshm puts in the scrap too, has them all, for
  applications that read it, and is what the host gets when an
  application puts both. Styles (`styl`) and pictures aren't passed, nor
  texts over 4 MB (syshm says so). How: each application has a scrap of
  its own (the Process Manager hands it to the next one brought to the
  front), and many keep a private one besides, such as TextEdit's, which
  they take from the scrap only when resumed with "the clipboard has
  changed". Enhancements patches ZeroScrap and PutScrap to see what
  applications copy; puts the host's new text in the front application's
  scrap just before it gets its next event (a GetNextEvent filter: some
  read the scrap's size in low memory rather than asking); and posts it a
  resume event saying the clipboard has changed, as the Process Manager
  does when one switches applications. Keys typed in the window meanwhile
  wait for that (at most half a second), so that a quick Command-V pastes
  the new text. An application that puts its copy in the scrap only when
  switched out (from a private scrap, at a suspend) reaches the host once
  another Mac application comes to the front. Scripted runs have a
  stand-in for the host's clipboard (`clipboard` and `print-clipboard`,
  see Debugging boot); other runs without a window, or in instruction
  time, leave the clipboard alone.
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
    threads draw them, and it's built without the fused multiply-adds
    compilers use on some hosts (Apple silicon's), so instruction-time runs
    stay the same on every host. `SYSHM_RASTER_THREADS` sets the number
    (default 3, counting the emulated processor's).

  The rasterizer and the GPU take what Mac OS 9's OpenGL will need too
  (docs/opengl-plan.md): two texture units combined in stages as OpenGL's
  texture environments combine them, blend equations, color masks, fog by
  a fog coordinate; RAVE's texture operations are such stages.

  `--no-gpu` (or `SYSHM_GPU=0`) draws in software in real time too;
  `SYSHM_GPU=1` draws with OpenGL in any run, a software renderer such as
  Mesa's llvmpipe included. `SYSHM_RAVE_FPS=1` prints the frame rate every
  second, and `SYSHM_LOG_RAVE=1` logs every call. docs/gpu-plan.md
  describes the GPU backend in full.

  Frames for the screen come at most one a vertical blank of the display's
  (60 a second of guest time), which shows no more; the iMac's 3D hardware
  drew games no faster. The call that ends one (QARenderEnd) returns once
  the display shows it: in real time the guest waits in it, taking
  interrupts meanwhile; in instruction time guest time moves on instead.
  Games of the time go wrong at the frame rates the host's GPU reaches:
  Unreal Tournament's engine ticks its actors at least 5 ms at a time, so
  at 400 frames a second its game, timers and match clock included, ran
  at twice the speed (see Findings in plan.md). Waiting, the host sleeps,
  as when the guest idles (but guest time ahead of the host clock isn't
  made up then: the game would show it).
- **syshm OpenGL Library, Memory and Utility** (under way:
  docs/opengl-plan.md): Mac OS 9's OpenGL libraries, as syshm's own, under
  Apple's fragment names (OpenGLLibrary, OpenGLMemory, OpenGLUtility) with
  Apple's exports, put in Extensions; on Mac OS 9 Apple's go to Extensions
  (Disabled) meanwhile (above; installing by hand, move them there too). Their
  calls are host calls (`src/opengl.c`), but for most of gl's: the
  library's own code records those in a buffer in the program's memory,
  done on the host all together at its next host call (a swap,
  `glGetError`). A front end on the host (`src/glfront.c`, `src/gltex.c`)
  does OpenGL's
  fixed function, and it draws with the 3D engine's backend, on the GPU in
  real time and in software otherwise, into the context's window (clipped
  to its regions), the screen or an offscreen buffer, when it's swapped.
  So far: AGL (one accelerated renderer, ATI's "Rage 128 OpenGL Engine" as
  on Mac OS 9, its pixel formats and renderer info; contexts laid out as
  Apple's, with their dispatch tables; drawables, swaps, swap and buffer
  rectangles) and, of OpenGL, the matrices and their stacks, `glBegin` and
  `glEnd`'s points, lines, triangles, quads and polygons with their colors,
  transformed, clipped, culled, flat or smooth shaded, depth tested,
  blended, alpha tested, masked and scissored in the viewport; textures
  (objects, 1D and 2D images in every format and type, parameters, two
  units with their texture environments, the texture matrices);
  `glReadPixels`; clears, strings, errors and queries; OpenGLMemory's
  allocations. Not yet: lighting, fog, vertex arrays, display lists,
  OpenGLUtility. Unreal Tournament's OpenGL build (on the UT CD,
  `Extras:OpenGL UT.sea`) plays with them, on the GPU and in software.
  Swaps to the screen or a window are paced as the 3D engine's frames are
  (above). `SYSHM_GL_FPS=1` prints the frame rate every second, and
  `SYSHM_LOG_GL=1` logs every call.

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
always the same, are taken to mean that). It plays at 60 frames a second,
in time (a match's Elapsed Time 30 s in 30 s of host time, on 8.6 and
9.2.2; 66 s at 640x480 and 40-46 s at 1024x768 before the frames were
paced). The engine's data carries
its version, so one installed by an older syshm isn't asked for what it
lacks: syshm says to install it again (and a window offers to update it).
UT's OpenGL build (the application and `OpenGLDrv.ini` from the CD's
`Extras:OpenGL UT.sea`, in place of UT's application, and
`GameRenderDevice=OpenGLDrv.OpenGLRenderDevice` in `UnrealTournament.ini`)
draws with syshm's OpenGL libraries, on 8.6 and on 9.2.2: the same
pictures as with RAVE, at 60 frames a second on the GPU and about 50 in
software (on 8.6, an M5 Mac).

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
  traps, system calls and host calls go straight to the kernel (but for
  those made in place, below); anything
  that faults leaves the block so the interpreter executes it (but for
  device registers, below). The time base is read natively.
  - Exactness: blocks run only when the instruction budget covers them, and
    each instruction checks before it changes anything, so instruction
    counts, device timing and faults are exactly the interpreter's (boots,
    the install and `--fixed-time` runs give identical results either
    way; `tests/test_jit.c` compares random programs).
  - Kernel calls in place: the 68k emulator's switches to native code and
    back (kernel calls 1 and 0, RunAlternateContext and
    ReturnFromException: 7 million in 4 billion instructions of 9.1 from
    its CD, 2.8 million in 8.5's) and NanoKernel 2's quick `sc` (see Mac
    OS 9.1) don't leave generated code or the interpreter. For the
    switches, generated code calls the kernel's function from inside the
    block (`kcall` in `src/ppc.h`), which makes the call in its common
    cases (no interrupt pending; verdict 0 or 32-255) with the same code
    as at an exit, then goes on through the block table. The kernel allows
    it before each run only while an exit would do nothing more (no
    devices to update, Multiprocessing Services tick due, task switch,
    software interrupt or preemption waiting: `in_place()` in
    `src/nk.c`), and as at an exit the call retires nothing, so runs are
    the same instruction for instruction. Kernel exits in that 9.1 run
    went from 11.7 to 1.5 million (8.5: 2.8 to 1.1), host time from 1.50
    to 1.32 s (8.5: 1.03 to 1.02 s).
  - Device registers in place: loads and stores to device registers (1.6
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
    instruction (see devices below), nor with `clock`. Generated code gets
    the hook's path, out of line, for instructions whose fastmem access
    faulted on a page that isn't mapped; it writes all registers back and
    reloads them afterwards (the hook may change any, CR when it posts an
    interrupt). In the 8.5 CD boot device-register exits went from 1.64
    million to 36,000 and host time from 1.00 to 0.96 s (8.5 installed:
    0.87 to 0.86 s; 8.6: 1.24 to 1.21 s; 9.1's CD, with 17,000, as before)
    on an M5, where an access cost about 50 ns with its exit and costs
    about 25 now, nearly all of it the devices' own bookkeeping.
    `SYSHM_DEVICE_EXITS=1` has every access exit as before, for
    comparisons.
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
  - Floating point: arithmetic, conversions, compares, moves, FPR loads
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
  - Hosts: generated code is the same on Linux and macOS. It never uses
    x18, which macOS reserves; on macOS the code buffer is `MAP_JIT`
    memory, which the emulation thread makes writable only while it writes
    code (`pthread_jit_write_protect_np()`, about 25 ns a switch). What's
    left that only macOS pays (those switches, its slower fault signals) is
    under 1% of a boot.
  - `SYSHM_JIT_PROFILE=1` samples where host time goes (translated blocks
    by guest address, stubs, C functions) and prints it at the end;
    `SYSHM_JIT_PROFILE=FROM:TO` samples only from instruction FROM to TO.
    `SYSHM_PERF=1` reads the host processor's counters for the emulation
    thread (Linux): every second, guest instructions per second and host
    instructions and cycles per guest instruction; a total at the end.
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
  up while the guest idles, as for disk transfers (below): the Mac's clock
  is that much ahead at first, and right after a minute or two of idling.
  A restart starts guest time over from the host clock. `--no-fast-start`
  turns it off.
- `syshm boot` takes the Mac OS ROM file from the startup disk (see above;
  `src/hfsread.c`: the blessed System Folder is word 0 of the volume's
  Finder info, its files follow its thread record in the catalog B-tree, and
  a fragmented file's extents go on in the extents overflow file), or the
  one `--rom` names. It builds the user address space from ConfigInfo, does what the
  Trampoline would (hardware info record, universal tables, interrupt vector
  tables, device tree), fills in the kernel's info records and starts Apple's
  68k emulator from the ROM.
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
  (see interrupt sets).
- Devices: RTAS (clock, NVRAM, power), a 6522 VIA with timers, the Cuda (time,
  PRAM, power, ADB with autopoll), an ADB keyboard and mouse, a Heathrow-style
  interrupt controller and a Z8530 SCC with nothing attached (LocalTalk sees an
  idle network), and for ROM files with parcels the NVRAM chip behind Mac I/O
  (`src/nvram.c`; see Mac OS 9.1). Device registers are emulated by decoding the faulting
  load/store (`src/mmio.c`), without an exit where it would do nothing more
  (see the JIT's device registers in place).
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
  the host; input (channel 9) records silence. The codec's analog input
  carries the CD-ROM drives' audio (`src/cdaudio.c`), played through to
  the outputs when the input selector has the CD and loop-through is on
  (or no input was selected), mixed with the DMA's frames or alone,
  resampled to the codec's rate. The codec's attenuation and
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
  input component away (see Mac OS 9). The codec's status says it's valid
  (bit 22), which Mac OS's sound plug-in polls for (see Mac OS 9.2.2).
- Device tree (`src/devtree.c`, `src/bootinfo.c`): an `iMac,1` with a Grackle
  PCI bridge, Mac I/O, a framebuffer display and a disk, flattened into the
  format the ROM's Name Registry reads.
- Native drivers: syshm generates PEF NDRVs (`src/ndrv.c`) and puts them in
  the device tree, so Mac OS loads them like any PCI card's driver. Their code
  is a loop around a host call; the driver logic is C (`src/video.c`,
  `src/disk.c`), and the stub can call Mac OS services on its behalf
  (`IOCommandIsComplete`, `SetInterruptTimer`, Driver Services' interrupt
  sets, Video Services' VBL service).
  - Display: 640x480 to 1280x1024 at 1-32 bits per pixel, CLUT and gamma
    calls, and a vertical-blank (VBL) interrupt, as the iMac's ATI chip
    has: 60 times a second of guest time, a device event on the timebase
    (vertical blank n at n/60 s), on the chip's line into the interrupt
    controller (source 22, level 2) while the driver has it on. At Open the
    driver installs its handler in the display node's interrupt set (its
    `driver-ist`), keeping the set's enabler and disabler, makes the Video
    Services Library's VBL service and turns the interrupt on; SetInterrupt
    (Control 7) turns it off and on. The handler acknowledges the vertical
    blank in the chip and queues a secondary interrupt handler, which runs
    Mac OS's VBL tasks for the screen (`VSLDoInterruptService`: the cursor's,
    games' page flips). Vertical blanks the guest doesn't get to in time
    are one interrupt. It used to be a Driver Services timer, which ran on
    the Time Manager and so stopped whenever that did. Two more
    modes take the host window's size on request (up to what the 16 MB
    framebuffer holds at 32 bits per pixel), for syshm Enhancements (a guest
    tool) to switch between; each is listed only while in use.
    In window runs in real time it has a hardware cursor: Mac OS hands it
    each new cursor (SetHardwareCursor, which Video Services'
    `VSLPrepareCursorForHardwareCursor` converts to 8 bits a pixel, with
    values for transparent and inverting pixels) and where it is, as it
    moves (DrawHardwareCursor), and the window draws it over the screen, so
    the pointer moving changes nothing in the framebuffer. Cursors the
    library can't convert, Mac OS draws itself. Headless and
    instruction-time runs have none (the driver is as it was without it),
    so Mac OS draws the pointer into the framebuffer as before; screenshots
    show it either way.
  - Disk: host files as drives (`--disk`, writable; `--cdrom`, read-only;
    the 8.5 CD's System refuses to start from a writable drive, as from a
    copy of the CD), and a host folder (`--share`; see Shared folder),
    whose blocks come from `src/share.c` rather than a file; and a floppy
    drive (`--floppy`): the floppy and CD-ROM drives' discs come and go
    (see Floppies and CDs). CD-ROM drives answer as Apple's CD-ROM driver
    (`src/cdrom.c`: the table of contents, audio, icons), their discs
    plain images or cue sheets (`src/cdimage.c`).
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
    plays). While sound plays, though, guest time ahead would be sound
    played ahead, which the host can't play: there a transfer takes host
    time, all of it, as a real drive's DMA does while the processor goes
    on. The guest waits in the driver's call until the transfer is over,
    taking interrupts meanwhile, so the sound keeps the host clock's pace (a
    movie streaming from disk used to run guest time 1.1 to 2.3 times as
    fast, its sound dropped in pieces; plan.md, Findings).
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
  Mac OS keeps overflow enabled and start-of-frame only while it waits for
  a frame to pass, so an idle host sleeps until the overflow (every 33 s)
  rather than waking at every frame.
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
  levels) and `AAPL,interrupt-vectors` (sources). Mac OS 9.2's ROM files
  build them in DriverServicesLib instead, and only for nodes that also
  have `AAPL,interrupt-index`, which their Trampoline makes (each
  interrupt's slot in HWInfo's VectorLookupTable, whose slot n is source n
  here: the source again). The display, the USB controller, the
  sound chip and the Ethernet node have them (the Ethernet node only for
  its receive DMA channel, lanLib's: with all three of its interrupts, Open
  Transport stalled for half a minute or more in some startups of Mac OS 9,
  busy in its deferred tasks, for reasons not yet known), with
  `AAPL,interrupt-index` when the ROM file's Trampoline makes it (the
  display's always: earlier ROMs ignore it). Their handlers mask and
  unmask the source but never clear a latched pulse in the controller, so
  the kernel takes pulses itself when it delivers them, as the NanoKernel's
  handler would: they stay in `PendingInts` until Mac OS has handled them
  (cleared them in the controller, as Mac OS 8.5's sound driver does, or
  masked their source) and calls PrioritizeInterrupts (which re-evaluates
  them). Otherwise a DMA interrupt fired forever. One it hasn't handled
  stays through one PrioritizeInterrupts more: Mac OS also calls it when it
  lowers its 68k interrupt mask, and at the end of a dispatch that began
  before the pulse came, and dropping the pulse there lost it (Unreal
  Tournament's sound stopped and the game waited for it forever).
- Host side: an SDL2 window loaded at run time (`src/display.c`; no build
  dependency, headless without it; on macOS also found in Homebrew's and
  MacPorts' directories, which aren't searched for bare library names;
  redrawn only when the screen, the pointer or the status bar changes;
  drawn at the display's full resolution, so high-density displays don't
  blur it). Up to 60 times a second the framebuffer is compared with a
  copy of what was last shown, row by row, and only the bands of rows that
  changed are converted, straight into a streaming texture in the 32-bit
  format renderers take as it is (ARGB8888: a byte swap at 32 bits per
  pixel, the 5-bit channels widened at 16, the CLUT expanded to 32-bit
  colours at 8 and fewer), and drawn again in the scaled copy below (all
  of it when the mode or colours change, and once a second if nothing
  has). The screen is scaled to fill the window above the status bar,
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
  Esc too); Restart (straight away: the Mac's own reset, so work not saved
  in Mac OS is lost); Shut down, which presses the keyboard's power key, so
  Mac OS asks "Are you sure you want to shut down your computer now?" and
  shuts down properly if told to (applications ask to save, NVRAM and a
  shared folder are written, and the run ends when the Mac turns itself
  off); Turn off, the power switch, for a Mac that doesn't answer (straight
  away; it keeps NVRAM, as closing the window does); a screenshot of the Mac's screen
  (a PNG on the Desktop, or in the current directory without one, named
  for when it was taken); fullscreen (at the desktop's resolution); the
  status bar; whether the pointer is captured while the Mac hides its
  cursor; and what's in the floppy and CD-ROM drives, each with Eject (see
  Floppies and CDs). In real time guest time doesn't count
  the pause, so the Mac's clock is that much behind afterwards.
  The status bar (shown unless `--no-status-bar`; the menu toggles it) sits
  below the screen, so the window grows by its height rather than covering
  anything (fullscreen, or maximized, the screen fitted to the window
  shrinks instead). It has lights for the disks, CD-ROMs, the floppy drive
  (if there's one) and network, lit
  for a tenth of a second whenever they move data (green, amber while a
  disk is written; a light is dimmed if there's no such device), each with
  what it read and wrote over the last second (the network's what it
  received and sent, in bits; the disks' in bytes; a CD-ROM only reads),
  and one for the processor, green as bright as the share of the time the
  guest isn't napping, and softly blue for the rest, while it naps (with
  idle sleep). Beside that is its speed: millions of instructions a second of
  the time actually spent running them, as a clock rate (the Mac thinks it
  has a 266 MHz G3, which runs about one instruction a cycle), worked out
  once a second from a random 1 in 32 runs (timing each costs too much),
  and not shown while the guest naps (busy less than a tenth of the time)
  or the menu pauses it.
  In narrow windows (640 wide, say) the names are shortened and the text
  made smaller to fit.
  SDL2 sound output (`src/audio.c`; in real time, `--no-sound` turns it
  off; a small queue is kept, its frames resampled to play up to 0.5%
  faster or slower as it's fuller or emptier than its goal, so that the
  guest's clock and the device's can't drift apart into growing delay or
  gaps, and a stall of the host is made up within seconds),
  PNG screenshots (`src/png.c`), NVRAM kept
  in a file, real-time guest time with `--window`/`--realtime`, and
  `syshm disk create`, which lays out an empty HFS volume (`src/hfs.c`, which
  also lays out the shared folder's, with B-trees of any size) the
  way Disk Initialization does (`hfsck` from hfsutils accepts it).
- Speed on an M1 Max: the boot to the Finder (about 3 billion instructions)
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
  (plan.md, Findings).

## Virtual machine description

What the Trampoline replacement (`src/bootinfo.c`) tells the ROM:

- Model `iMac,1`, a 266 MHz PowerPC 750 on a 66.4 MHz bus, timebase 16.6 MHz:
  a quarter of the bus clock, as the 750's is, and as Mac OS 9.1 and later
  take it to be (their AbsoluteTime is converted with Gestalt's bus clock;
  with a 66 MHz bus, their Microseconds ran 0.6% fast).
- Grackle PCI host bridge; on it the display (device 1, framebuffer at
  `0x82000000`), up to four disks (devices 2-5; a shared folder is one),
  the USB controller (device 6, registers at `0x81000000`), the floppy
  drive if there's one (device 7, `syshm,floppy`) and Mac I/O
  (device 16) at `0x81080000`:
  Heathrow-style interrupt controller (`+0x10`), DMA channels (`+0x8000`,
  0x100 bytes each), SCC (`+0x12000`), VIA1 with Cuda (`+0x16000`).
  Interrupt sources use Heathrow numbers (DMA channel n is source n,
  Ethernet 14, SCC A 15, SCC B 16, sound 17, VIA1 18, the display 22, as the
  iMac's ATI chip, USB 28); levels: VIA1
  1, the display, sound, Ethernet, USB and their DMA 2, SCC 4. Configuration space is at
  `0xFEC00000` (`CONFIG_ADDR`) and `0xFEE00000` (`CONFIG_DATA`).
- Sound: `davbus` at Mac I/O `+0x14000` (DMA channels 8 and 9), with a
  `sound` child; DecoderInfo long 39 holds the chip's address.
- Ethernet: `ethernet` at Mac I/O `+0x11000` (`bmac+`, DMA channels 2 and 3;
  nothing answers at those addresses, lanLib being the driver's way to the
  hardware), station address 52:54:00:12:34:56. Its lanLib is syshm's even
  when the ROM file has one for `bmac+` (9.2.2's).
- Mac I/O's floppy controller cell (`+0x15000`), for ROM files with parcels
  only: reads as zeros, writes are ignored (the iMac has no floppy drive).
- NVRAM, for ROM files with parcels only: `nvram` at Mac I/O `+0x60000`
  (`device_type` and `compatible` `nvram`, `reg` 0x60000 0x20000, `#bytes`
  0x2000, `AAPL,address`), as on Heathrow and Paddington Macs: 8 KB, byte k
  at `+16k`.
- Where Open Firmware booted from, for ROM files with parcels only:
  `/chosen`'s `bootpath`, such as `/pci/syshm,disk@2:8,\\:tbxi` (a disk's
  node is at its PCI device number, the partition is its map entry, none
  without a map), for the partition the ROM file came from, or else the
  first partition with boot blocks. The disk
  driver says, through DriverGestalt, which node ('nmrg'), unit ('intf'
  'scsi', 'dvrf' with the PCI device number as target ID) and partition
  ('ofpt') each drive is.
- ADB/power-manager kind 0 in the universal flags, so the early ROM code uses
  RTAS (tokens in `NKHWInfo`) for clock, NVRAM and power (`NKHWInfo`'s Mac OS
  NVRAM offset, `0x1400`, is where PRAM goes; a ROM file with parcels uses the
  Cuda's RTC and the `nvram` node instead, through its libraries). A flag in
  ProductInfo (bit 19 at `+0x28`) has the ROM restart through RTAS
  `system-reboot`; otherwise it restarts in place, jumping back to its start
  with the devices still running, and hangs on the first interrupt. Bit 26
  there is for USB (see above).
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
  QuietWriteCount). syshm's kernel does the same; a run's summary says
  `quiet writes: N` when there were any.
- The ROM maps a few regions as single 4 KB pages (the kernel's and the
  emulator's data, the info records). The pages next to them, up to 16 KB
  boundaries, read as zeros rather than faulting, so that hosts with 16 KB
  pages can protect guest memory per host page (see fastmem). Only stray
  accesses could tell, and the map is the same on every host.

## Next steps

1. 3D: OpenGL with Mac OS 9, then Glide: docs/opengl-plan.md (what Mac OS
   9.1's OpenGL is and what games link to, the design; under way: textures
   are done and Unreal Tournament's OpenGL build plays; vertex arrays, fog
   and lighting are next) and docs/glide-plan.md. (The
   RAVE engine draws with the host's GPU; what's left there, such as
   reading back less of each frame, is in docs/gpu-plan.md.)
2. Networking: host ports forwarded to the guest (so it can serve).
3. Shared folder: writing back when Mac OS ejects the volume, then offering
   the folder again as new media, so that changes cross during a run (it
   needs the disk driver's removable media).
4. Startup-disk selection; Open Transport's stall with a full
   interrupt set for the Ethernet node; then the NanoKernel v2 interface,
   which the ROM files from Mac OS ROM 1.7.1 on use, and Mac OS 9.1's needs
   (see Mac OS 9.1).
5. More JIT speed for disk boots: the interpreter, which runs code
   until it's hot at about 10 ns an instruction, mostly in overheads per
   block (about 8%); translating (4%), with the instruction-cache upkeep
   that follows (4%); device register accesses, though made in place, still
   cost about 25 ns each in the devices' bookkeeping (about 4% of the 8.5
   CD boot).
6. The window: drawing and presenting on a thread of their own, so the
   Mac doesn't stop for them; and knowing which framebuffer pages were
   written (by protecting them) rather than comparing it with a copy each
   frame, which is most of what showing a 32-bit screen still costs (plan.md,
   Findings, "Showing the screen").

future.md has ideas not planned yet: a modem, infrared, 3D sound, sound
input, AltiVec, printing, game controllers, and more CD formats.

## Build and test

```
make            # build/syshm (C11, and C++17 for the menu: Dear ImGui is in third_party/imgui)
make test       # unit tests; the ROM test uses ./MOS.rom or $SYSHM_TEST_ROM if present,
                # the GPU test any OpenGL 3.3 there is (else it's skipped)
SYSHM_JIT_PROGRAMS=200000 build/syshm-tests jit    # longer JIT-vs-interpreter fuzzing
```

The guest's OpenGL is checked in the guest too, by a test application of
syshm's own, "syshm GL Test" (`src/gltest.c`: its logic is C in syshm
calling the guest's OpenGLLibrary through the Code Fragment Manager's
linking, as a game would). `SYSHM_TOOLS_GL_TEST=1 build/syshm tools create
FILE` puts it on the tools volume; with the OpenGL libraries in a disk
image's Extensions folder and it in Startup Items (hfsutils' `hcopy -m`
copies them), a run in instruction time shows its window with the scene
`SYSHM_GL_TEST_SCENE` names (`clear`: a clear, and a scissored one;
`triangle`: in perspective, a floor cut by the near plane, a triangle and a
quad crossing in depth, a flat-shaded fan) at the depth
`SYSHM_GL_TEST_DEPTH` sets (16 or 32), the same on every host. On Linux,
`make test` draws scenes through the libraries and compares them with Mesa's
own OpenGL (its compatibility profile, which still has `glBegin`):
`SYSHM_TEST_VERBOSE=1` says how far apart they are, and
`SYSHM_TEST_SAVE=DIR` saves them as PNGs.
`tools/gltable.py` made `src/opengl_table.h`, the libraries' functions,
from Apple's (Mac OS 9.2.2's) and the macOS SDK's OpenGL headers.

CI (`.github/workflows/ci.yml`) builds and tests on Linux and macOS, each on
x86_64 and AArch64, with warnings as errors; on AArch64 (Linux and macOS)
it also fuzzes the JIT with 100,000 programs, and on Linux the GPU backend
is tested on Mesa's
software OpenGL (llvmpipe; `SYSHM_TEST_GPU=1` makes the test fail rather than
skip without a context). Each run uploads a `syshm-<os>-<arch>.tar.gz`, and
pushing a `v*` tag publishes them as a GitHub release.

```
build/syshm rom info MOS.rom
build/syshm rom info 91.iso                      # the Mac OS ROM file in a disk's System Folder
build/syshm rom config MOS.rom                   # ConfigInfo and user address space
build/syshm rom parcels MOS91.rom                # a Mac OS 9.1 ROM file's parcels
build/syshm rom extract MOS.rom build/toolbox.rom
build/syshm rom trampoline MOS.rom build/trampoline.elf
build/syshm boot --rom MOS.rom --cdrom 85.iso --window   # interactive
build/syshm boot --rom MOS.rom --cdrom 85.iso --max 3000000000 --screenshot finder.png
build/syshm boot --rom MOS.rom --cdrom 85.iso --share ~/Mac --window    # with a shared folder
build/syshm boot --rom MOS.rom --cdrom 85.iso --floppy disk1.img --floppy disk2.img --window   # a floppy drive
build/syshm boot --disk hd.img --cdrom-drive --window       # an empty CD-ROM drive: drop CD images on the window
build/syshm boot --disk hd.img --cdrom "Game.cue" --window  # a CD with audio tracks
build/syshm disk toc "Game.cue"                             # its tracks, as its CD-ROM drive gives them
build/syshm disk create blank.img 1440K "Untitled"          # a blank floppy
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
3800000000 power                 # the keyboard's power key: Mac OS asks whether to shut down
0 tools install                  # the answer to the offer to install the guest tools (or "later")
3900000000 floppy insert Disk 2.img
3950000000 cdrom insert Game CD 2.iso
4000000000 clipboard Olé “two”\nlines   # the host's clipboard has this text now (\n: a newline)
4100000000 print-clipboard       # prints the host's clipboard, with what Mac OS copied since
```

(`floppy insert` and `cdrom insert` take the rest of their line as the
image's path, comments included; `floppy eject` and `cdrom eject` take one
out: see Floppies and CDs.)

`clipboard` and `print-clipboard` stand in for the host's clipboard (a
scripted run leaves the window's alone), for syshm Enhancements to share
(see Guest tools): the text is as if copied on the host with the window
then brought to the front.

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
them. Kernel calls made in place, without an exit, only happen while an
exit would do none of that; device register accesses made in place update
devices as their exits would. An access on the last instruction before a
sync point still exits, and the loop then services that sync point.) Tracing and trap logging use the
interpreter.

Debugging `boot`: `--trace-from N --trace-reg 24` logs instructions with the
68k PC (the emulator keeps it in `r24`), `--watch <addr>` logs every write
the guest makes to a page (the kernel's own writes there, such as disk
reads, go through unseen, so a watched run goes as any other), `--traps N
[--traps-after M]` logs 68k Toolbox/OS traps, interpreted or recompiled
(see Idle; with the driver name for `_Open`), `--log-hostcalls` logs the native drivers'
requests (a CD-ROM drive's calls with their parameters in and out, and what it plays), `--dump-ram FILE` saves RAM at the end, `--max N` limits the run.
`--trace-range LO:HI [--trace-insn WORD]` logs only instructions in an
address range (optionally only one instruction word), with instruction
counts and r3-r5/LR, cheaply enough for whole boots. `SYSHM_LOG_SOUND=1`
logs the sound chip's register accesses, and `SYSHM_SOUND_WAV=FILE` records
what it plays (also in instruction time; CD audio included).
`SYSHM_LOG_AUDIO=1` prints, every second while sound plays in real time,
how the host's sound queue fared (its depth, how much faster or slower it
played, chunks dropped, silence added) and what paces it (the frames the
guest's sound DMA gave, how far guest time is ahead of the host clock, the
disks' transfers and the guest time they took, how much the host's sleeps
overran). `SYSHM_LOG_USB=1` logs the USB
controller's register accesses (not the frame number, which drivers poll),
`SYSHM_LOG_PCI=1` configuration space accesses.
`--io-ram` backs unmodelled I/O pages with plain RAM. Under gdb, `handle
SIGSEGV nostop noprint pass` lets the JIT's memory faults through; under lldb
on macOS, `settings set platform.plugin.darwin.ignored-exceptions
EXC_BAD_ACCESS` (before `run`) and `process handle -s false -n false -p true
SIGBUS` (or set `SYSHM_JIT_FASTMEM=0`).

`tools/disasm.py` disassembles guest code (PowerPC or 68k, with Python's
capstone) from a ROM file's image or a `--dump-ram` file, and names the code
fragment an address is in: the image's PEF containers, the parcels' libraries
and their copies in RAM, with their exports, entry points and imports.
`tools/disasm.py MOS91.rom 0xFFCE2700`, `tools/disasm.py ram.bin MOS91.rom
0x28D70`; `--list` shows what is where.

`tools/heap.py ram.bin` walks the system heap and the current application's
heap in a `--dump-ram` file block by block (reading headers, not the whole
file) and reports blocks whose back link or size is broken, where a zone was
written over; `--find ADDR` names the system heap block an address is in.

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

For Mac OS 9.1, QEMU's `mac99` runs Apple's real Trampoline and NanoKernel 2
unpatched, to the Finder: `tools/qemu-ref/run91.sh [91.iso]` boots the CD,
`mptrace.py` logs every Multiprocessing Services call with its results
through QEMU's gdbstub (`rsp.py`, no gdb needed), `nk2dump.py` decodes the
kernel's state and the device tree from a RAM dump, and `shots.py` takes
screenshots. What they found is in `docs/nk2-reference.md`.

## Assets

ROMs and OS images are user-supplied and ignored by git: `MOS.rom` (the ROM
above) and `85.iso` (retail Mac OS 8.5 CD, a bare bootable HFS volume);
`MOS91.rom` and `91.iso` for Mac OS 9.1 (see above). The ROM files are only
needed with `--rom` (and for the ROM test and tools): otherwise syshm takes
the one in the startup disk's System Folder.

## Layout

```
src/      be.h, lzss.c, rom.c + parcels.c, mem.c (flat 4 GB guest space),
          ppc.c + ppc_exec.inc + ppc_fp.c (interpreter), jit.c + a64.h
          (AArch64 JIT), nk.c (native kernel), mp.c (its Multiprocessing
          Services, NanoKernel 2's), priv.c (supervisor mode: the skeleton key),
          bootinfo.c + devtree.c (Trampoline replacement, device tree),
          rtas.c (clock/NVRAM/power/PCI configuration), mmio.c, macio.c,
          dbdma.c, via.c, cuda.c, adb.c, scc.c, awacs.c, ohci.c (USB), pci.c
          (configuration space), nvram.c (devices), ndrv.c (PEF fragment
          generator: drivers, the guest tools),
          video.c, disk.c (native drivers), floppy.c (floppy images),
          drives.c (removable drives: where images go, the menu),
          cdrom.c (the CD-ROM driver's calls), cdimage.c (CD images: cue
          sheets, the table of contents), cdaudio.c (the drive's player),
          net.c (Ethernet's lanLib,
          libslirp), hfs.c (HFS volume writer), hfsread.c (HFS and HFS+
          reader: the startup disk's ROM file), share.c (shared folder),
          rsrc.c (resource forks), tools.c (guest tools), install.c
          (installing them from inside Mac OS), clip.c (the clipboard they
          share with the host), rave.c (3D engine), opengl.c +
          opengl_table.h (the guest's OpenGL libraries), glfront.c + gltex.c (their
          fixed-function front end; gltex.c its textures), gltest.c (their test application), surface.c + raster.c + gpu.c + gl.c (the 3D backend:
          drawing, in software and with OpenGL, and presenting), display.c (SDL2
          window), ui.cpp (its menu and prompts, with Dear ImGui), audio.c (SDL2 sound), png.c,
          perf.c (host processor counters), main.c
tests/    unit tests with a small instruction encoder (ppcasm.h); test_jit.c
          runs random programs on both engines and compares the results
tools/    qemu-ref/ reference-boot scripts, pef.py (PEF inspector),
          devtree.py (device-tree copy dumper), disasm.py (disassembler),
          heap.py (heap zones in a RAM dump, checked),
          gltable.py (the OpenGL libraries' functions, from Apple's)
docs/     gpu-plan.md (the 3D engine's OpenGL backend), opengl-plan.md and
          glide-plan.md (Mac OS 9's OpenGL, under way, and 3dfx Glide),
          nk2-reference.md (Mac OS 9.1 under QEMU: what Apple's NanoKernel 2
          does)
third_party/imgui/  Dear ImGui 1.93.0 (MIT; its core files only, as released)
```
