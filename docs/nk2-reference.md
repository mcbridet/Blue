# NanoKernel 2 reference: Mac OS 9.1 under QEMU

What Apple's own 9.1 boot code (the "Mac OS ROM" 6.1f9 file from the 9.1
CD, the same file as `MOS91.rom`, with its Trampoline and NanoKernel 2.21)
does, observed under stock QEMU 11.1.3, to check Blue against. All
addresses are for QEMU's `mac99,via=pmu` machine with 256 MB unless noted.

How it was obtained (scripts in `tools/qemu-ref/`, usage in each file):

- `run91.sh` boots the CD (`-cdrom`-style, read-only) on `mac99,via=pmu`.
- `shots.py` boots and takes screenshots at given times.
- `rsp.py` is a small GDB remote-protocol client for QEMU's gdbstub (there
  is no PowerPC gdb here): breakpoints, registers, virtual and physical
  memory, monitor commands.
- `mptrace.py` boots under the gdbstub and logs every Multiprocessing
  Services call with its arguments, results, the running task and guest
  time, plus each event-group software interrupt, and saves RAM dumps.
- `nk2dump.py` decodes a RAM dump: info records, kernel globals, the page
  map, BATs, every object in the kernel's ID index (tasks, areas, event
  groups...), and with `--tree` the device-tree copy (via `tools/devtree.py`).
- QEMU's own `-d int -D log` gave the whole boot's `sc` calls (r0, r3-r8).

Kernel structure offsets are from `powermac-rom` (`Internal/*.a`); where
checked (object signatures, KDP page map, BATs, index layout) the 9.1
kernel agrees. Guest times are the timebase at 25 MHz (OpenBIOS's
`timebase-frequency`); the kernel starts it near 0.

## 1. Does 9.1 boot?

| machine            | result (host time, TCG on Apple silicon)                      |
|--------------------|---------------------------------------------------------------|
| `mac99,via=pmu`    | Finder at about 17 s; "Mac OS 9.1 / Starting Up..." at 10 s   |
| `mac99` (Cuda)     | Finder                                                        |
| `mac99,via=pmu-adb`| Finder                                                        |
| `g3beige`          | stops in OpenBIOS: "this image is not for this platform"      |

All three `mac99` variants reach the Finder with the CD's window open
("Mac OS 9.1", 9 items: Mac OS Install, Utilities, ...) and no patching.
With `via=pmu` (the others' trees weren't dumped) OpenBIOS presents
`model "PowerMac3,1"`, `compatible "PowerMac3,1", "MacRISC", "MacRISC2",
"Power Macintosh"`, a G4 (PVR `0x000C0209`, 7400), UniNorth PCI, KeyLargo
mac-io, OpenPIC, PMU, QEMU VGA (with OpenBIOS's own NDRV), gmac Ethernet,
USB. On `g3beige` the ROM file's boot script stops with "this image is not
for this platform" (none of its `<COMPATIBLE>` models: iMac,1 PowerMac1,1
PowerBook1,1 PowerMac2,1 PowerMac3,1 PowerBook2,1 PowerBook3,1 PowerMac2,2
PowerBook2,2 PowerMac3,3 PowerMac5,1); it would need a patch like
`bw-g3.fs`. Running under `mptrace.py`'s breakpoints the boot still
reaches the Finder.

## 2. Multiprocessing Services calls during startup

Three ways in, all seen:

- `sc`, call number in r0 (`IntSyscall` at physical `0x00F14580`; the
  vector at `0xC00` jumps through SPRG3+0x30). Most calls.
- `twi 31,r31,8` (kernel call 8, MPDispatch) at `0x6806E8E0`, the 68k
  emulator's kernel-trap table entry, reached by the 68k `FE1F` trap.
  The 68k glue is in the ROM: `0xFFC10B60` MPCreateEvent (d0 = 0x31),
  `0xFFC10B80` MPSetSWIEvent (0x36), `0xFFC10BA0` MPWaitForEvent (0x34).
- `sc` with r0 = -1 (not an MP call; see below), 53,071 times per boot,
  all from Mixed Mode's `MPCallUniversalProc` (`0xFFCF8DB8`, ROM
  MixedMode 'ncod' 1). The first one comes right after the third task is
  named (call 126 'ager'). r0 = -2 never happens.

**`sc -1` and `sc -2` in the 9.1 kernel** (`0xFFF14580`, disassembled):
`sc -1` sets r0 = EWA.Flags & `0x00200000` (the blue bit) with `rlwinm.`,
so CR0 is EQ when the caller is *not* the blue task (GT when it is), and
returns with r1 and LR restored, nothing else touched. `sc -2` does the same
CR0 test but sets r0 to the word at the current Task + `0xEC` (the word
MPCreateTask took in r6). There is no `sc -3` in 2.21.

**The first calls** (from `mptrace.py`; "kc8" = through the emulator's trap;
results after the arrow; IDs are slot << 16 | use count):

| #   | t (s) | task    | call | arguments → result |
|-----|-------|---------|------|--------------------|
| 1   | 0.013 | blue    | 13 MPYieldWithHint | r3=1, from the emulator (`0x6806C480`) → r3=1 |
| 2   | 0.121 | blue    | 63   | r3=0x00050001 (blue task) r4=0x68470 r5=0xF04D6163 → 0 |
| 3   |       | blue    | 25 MPCreateCriticalRegion | → 0x00160001 |
| 4   |       | blue    | 20 MPCreateSemaphore | max 0, initial 0 → r5=0x00170001 |
| 5   |       | blue    | 25 | → 0x00180001 |
| 6   |       | blue    | 15 MPCreateQueue | → 0x00190001 |
| 7   |       | blue    | 39 MPSetQueueReserve | 0x00190001, **2** |
| 8   |       | blue    | 64 MPCreateNotification | → 0x001A0001 |
| 9   |       | blue    | 66 MPModifyNotification | 0x001A0001 → queue 0x00190001 |
| 10  |       | blue kc8 | 49 MPCreateEvent | → 0x001B0001 (68k pc `0xFFC10B68`) |
| 11  |       | blue kc8 | 54 MPSetSWIEvent | 0x001B0001, level 7 |
| 12  |       | blue    | 66 | 0x001A0001 → event group 0x001B0001, flags 0x80000000 |
| 13  |       | blue    | 80 MPGetAreaFromAddress | address 0 → r5=0x000A0001 |
| 14  |       | blue    | 77 MPSetAreaBackingProvider | area 0x000A0001, notification 0x001A0001 |
| 15-16 |     | blue    | 15, 15 | → 0x001C0001, 0x001D0001 |
| 17-18 |     | blue kc8 | 49, 54 | → 0x001E0001; level 1 |
| 19-34 | 0.125 | blue  | 25, 12, 25, 11, 20 (1,0), 20 (1,0), 25, 15, 15, 25, 12, 12, 12, 27, 12, 28 | queues 0x00240001, 0x00250001; 12 returns 0x00050001 |
| 35  | 0.125 | blue    | 7 MPCreateTask | see below → r10=0x00270001 |
| 36-38 |     | blue    | 12, 15 (→ 0x00290001), 39 (0x00290001, 1) | |
| 39  | 0.125 | blue    | 8 MPStartTask | 0x00270001, see below |
| 40  |       | blue    | 39 | 0x00190001, **3** |
| 41  |       | blue    | 126 KCSetTaskType | 0x00270001, 'deth' |
| 42-54 | 0.126 | blue  | the same for task 0x002A0001: 39 (0x00190001, 4), 126 'dbgr', then 59 (0x00250001) | |
| 55-69 | 0.127 | blue  | the same for task 0x002D0001: 39 (0x00190001, 5), 126 'ager' | |
| 70-76 | 0.147 | blue  | 11 MPIsTaskBlue ×7 | → 1 |
| 77-78 | 0.177 | blue  | 15 (→ 0x00300001), 39 (0x00300001, 64) | |
| 79-238 | 0.18 | blue   | 32 × {40 NKCreateTimer, 30 → queue 0x00300001, 30 → event group 0x001E0001 flags 8, 31 forever, 32} | timers 0x00310001... (Time Manager's pool) |
| 239-240 | 0.29 | blue  | 31 MPArmTimer ×2 | real deadlines |
| 241 | 0.296 | deth    | 18 MPWaitOnQueue | 0x00240001, forever: blocks |
| 242 | 0.296 | dbgr    | 18 MPWaitOnQueue | 0x00250001, forever: blocks |
| 243-253 | 0.297 | ager | 100 (→16), 64, 15, 39 (8), 49, 66, 66, 82 MPRegisterAger, 116 (→ -29299), 100, 52 MPWaitForEvent (0x00530001, 3000 ms) | blocks, wakes every 3 s for page aging (93/94 on area 0x000A0001) |
| 254 | 0.304 | blue    | 13 | |
| 255 | 0.313 | blue kc8 | 52 MPWaitForEvent | 0x001E0001, timeout 0 → r4 = 8 (first level-1 software interrupt) |
| 256-259 |   | blue    | 18 (0x00300001, timeout 0) → message; 31; 13; 18 → -29296 | the Time Manager's loop |

Up to call 7 this is exactly the sequence Blue sees ([plan.md](plan.md), Findings),
with three differences: the very first call is 13 from the emulator
(Blue's log starts at 63); the first MPSetQueueReserve passes 2 where
Blue's run passes 1; and the IDs differ (section 3). The reserve on queue
0x00190001 (the RAM area's page-fault queue) is the task count: MPLibrary
(`0xFFCDC1B0`) reads NanoKernelInfo.TaskCount through the pointer at
0x68FFEFE0 (the kernel-data view of the InfoRecord) and passes it to
MPSetQueueReserve, then adds one per task made: 2, 3, 4, 5, and 6 at the
Finder. TaskCount is 2 at startup (the blue task and the idle task); Blue's
kernel reports 1.

**The first task.** MPCreateTask: r3 = 1 (blue process), r4 = 0x00240001
(termination queue), r5 = 0 (options), r6 = 0x0009BB38 (kept at Task +0xEC;
what `sc -2` returns), r7 = 0x0007E6D0 (the task's r12), r8 = `0xFFCE03A0`
(entry), r9 = 0x0007E3B8 (the task's r2/TOC); ID back in r10 = 0x00270001.
MPStartTask: r3 = 0x00270001, r4 = 0xF04D6163 (Task.Name, renamed 'deth' by
call 126), r5 = 0x0009BB38 (the task's r3: MPLibrary's record for it),
r6/r7 = 0x0009BB38, 0 (termination message words), r8 = 0x0009BB38 (the
task's r1), r9 = 0x0009B738. `0xFFCE03A0` is MPLibrary's task trampoline:
it calls the function in the record (+0x28, parameter +0x2C), then exits.
All four tasks of the boot share entry, r12 and termination queue.

What it does first: nothing for 170 ms. All three tasks first run at
t = 0.296 s, in creation order, right after the blue task's first kernel
call 4 (PrioritizeInterrupts; `--kcalls 4` shows two, 68k pc `0xFFC002B8`
and `0xFFC1082E`), which the emulator makes when the 68k boot code first
lowers its interrupt mask (`move.w #$2000,sr` at `0xFFC002B4`). Kernel
call 4 had not been made before, and the blue task made no MP call in
between: it neither blocked nor polled nor yielded. 'deth' and 'dbgr' then
block in MPWaitOnQueue (forever) on
0x00240001 (the termination queue) and 0x00250001; 'ager' sets itself up
and blocks in MPWaitForEvent with a 3 s timeout. Meanwhile the blue task
makes 7 MPIsTaskBlue calls, builds the Time Manager's 32 timers and arms
two; the first timer fires at 0.313 s (section 6). (Times vary by about
0.1 s between runs: QEMU's clock is real time.)

**Whole boot to the Finder** (45 s run, `-d int`; `sc` only, counts):
-1: 53071, 18: 13227, 11: 12509, 31: 9213, 13: 6648, 55: 4394, 32: 4270,
46: 4064 (MPCpuPlugin, mostly from the idle task's loop at `0x00F2338C`,
-29286 until MPRegisterCpuPlugin), 78: 950, 30: 773, 93/94: 520 each, 40:
389, 23: 329, 37: 157, 67/95/128: 45 each, 12: 37, 52: 17, 15/100: 15, 39:
11, 25: 10, 27/28: 8, 7/8/20/66/126: 4, 17/49/64/99: 2, and once each 2, 3,
4, 59, 62, 63, 77, 80, 82, 116. The fourth task is 'powr' (priority 1).
Later in the boot MPCreateProcess (4) makes process 0x01C00001 and
MPSetBlueProcessID (99, twice, r4 = 0x00B8E890, r5 = 'MACS' then a
pointer) makes it the blue process: the blue task's process is 0x01C00001
at the Finder (the ROM's check for blue process 1 is only at startup).
About 1.2 million `twi` kernel calls, mostly ReturnFromException and
RunAlternateContext.

## 3. IDs and NanoKernelInfo

The ID index (Index 'INDX' in the kernel pool, pages of 0x1FA 8-byte
entries {class, -, use count, pointer}) at the first call already holds 22
objects:

| ID         | object |
|------------|--------|
| 0x00000001 | process (the blue process; address space 0x00030001) |
| 0x00010001 | coherence group |
| 0x00020001 | CPU |
| 0x00030001 | address space (SRs 0x20n00000, BATs below) |
| 0x00040001 | queue: the page queue |
| 0x00050001 | task 'blue': weight 200, priority 0 at first, 2 by call 7 |
| 0x00060001 | semaphore |
| 0x00070001 | task 'idle': priority 3, weight 1, runs kernel code at `0x00F232C4` |
| 0x00080001 | semaphore |
| 0x00090001 | address space (SRs 0x20n00001, no BATs) |
| 0x000A0001-0x00150001 | the 12 areas of section 4 (0x00110002: created on the second try) |

So the first ID an MP call returns is 0x00160001 (Blue's kernel:
0x000E0001). Blue's kernel matches the blue process (1) and address space
(0x00030001); the blue task (0x00050001 vs 0x00010001 in Blue's kernel),
page queue (0x00040001 vs 0x00020001) and the RAM area (0x000A0001 vs
0x00040001) differ.

**NanoKernelInfo** (version 0x0221, length 0x140) at the first call:
blueProcessID 0x00000001, blueTaskID 0x00050001, pageQueueID 0x00040001,
TaskCount 2, FreePoolExtendCount 0, ConfigFlags 0x1B, IDCtr 22. At the
Finder: TaskCount 6, FreePoolExtendCount 10, 455 objects (389 timers).

**InfoRecord pointer table** (KDP+0xFC0, logical 0x5FFFEFC0):

| record          | logical    | version | length | Blue puts |
|-----------------|------------|---------|--------|------------|
| ProcessorState  | 0x68FFDB60 (PSA page) | 0x0100 | 0x80 | IR+0xB00 |
| HWInfo          | 0x5FFFEC20 | 0x0107  | 0xC0   | IR+0xB80, 0x0103/0xA0 |
| ProcessorInfo   | 0x5FFFEF60 | 0x0101  | 0x60   | same |
| NanoKernelInfo  | 0x5FFFEE20 | 0x0221  | 0x140  | IR+0x940, 0x0228/0x160 |
| DiagInfo        | 0x68FFDA60 (PSA page) | 0x0100 | 0x100 | IR+0xD60 |
| SystemInfo      | 0x5FFFECE0 | 0x0107  | 0x140  | IR+0xC20, 0x0105 |

**Where the pages are:** KDP is physical 0x0FBFE000 (SPRG0). It is the
InfoRecord page: logical 0x5FFFE000 maps to it (area 0x000B0001, PTE
template 0x0FBFE012: read/write), and 0x68FFE000 too (read-only, inside
area 0x000E0001, 0x68FF5000-0x68FFEFFF → 0x0FBF5000, which also maps the
kernel pool and the PSA page 0x68FFD000 → 0x0FBFD000). EmulatorData
0x68FFF000 → 0x0FBFF000 (ECB at +0x100).

HWInfo: MacROM_Base 0x00C00000, DeviceTreeBase 0x6400000C,
UniversalInfoTableBase 0x6404C0F0, ConfigInfoTableBase 0x00003000
(physical), VectorLookup/MaskTable 0x68FEFF80/0x68FEFF40 (in the ConfigInfo
page), OpenPICBaseAddr 0x80040000, MacOS_NVRAM_Offset 0x1400, no RTAS
tokens, VIA vector 0x19 bit 2, SCC A/B 0x25/0x24, NMI 0x37, flags 4.

## 4. Page map and areas

The Trampoline builds a ConfigInfo for the kernel at physical 0x3000
(logical 0x68FEF000): LA_InterruptCtl 0x80060000 (the ROM's own says
0x808E0000), InterruptHandlerKind 6, ROMImageBaseOffset → 0x00C00000,
KernelCodeOffset → 0x00F10000 (the NanoKernel at image +0x310000),
PageAttributeInit 0x12, a 0x138-byte page map, BAT ranges and BAT maps
(SupInit/UsrInit/OvlInit 0x13FF13FF, CPUInit 0xF3FFF3FF), Debug 0x0100,
BootstrapVersion "NewWorld v1.0". The kernel copies the page map to
KDP+0x920, fills in the RAM, InfoRecord, KDP and EmulatorData entries, and
makes areas from it.

**The page map in the kernel** (16 segment entries → PMDTs: logical,
pages, physical | attributes; 0xA00/0xA01 = nothing, 0xC00 = paged RAM,
else contiguous with WIMG << 3 | PP):

| logical | physical | attr | |
|---------|----------|------|-|
| 0x00000000-0x0FA1BFFF | paged | 0xC3E | Mac OS RAM (250 MB) |
| 0x5FFFE000 (1 page) | 0x0FBFE000 | 0x010 | InfoRecord = KDP |
| 0x64000000-0x6417FFFF | 0x0AB00000 | 0x012 | device-tree copy, universal info |
| 0x68FEF000 (1 page) | 0x00003000 | 0x013 | ConfigInfo |
| 0x68FF5000-0x68FFEFFF | 0x0FBF5000 | 0x093 | kernel pool, PSA, KDP |
| 0x68FFF000 (1 page) | 0x0FBFF000 | 0x010 | EmulatorData |
| 0x80060000 (1 page) | 0x80060000 | 0x028 | interrupt control |
| 0x80000000-0x8FFFFFFF | 0x80000000 | 0x02A | I/O |
| 0xF0000000-0xFFFFFFFF | 0xF0000000 | 0x02A | I/O |

**BATs** (the blue address space's, same as the ConfigInfo ranges): 
0xFFC00000-0xFFFFFFFF → 0x00C00000 (the 4 MB image, WIMG 0b1010, PP 3
read-only) and 0x68000000-0x680FFFFF → 0x00F00000 (image +0x300000:
NanoKernel, emulator, dispatch table), read-only. A third range
(0x00000000 4 MB → 0x00C00000) is in the table but no BAT map uses it.
The ROM is not an area.

**Areas** (all made by the kernel before the first call; none are added
by the Finder):

| ID | logical | length | physical / template | notes |
|----|---------|--------|---------------------|-------|
| 0x000A0001 | 0x00000000 | 0x0FA1C000 | paged (page list at 0x0FBB5000) | RAM; backing provider 0x001A0001 after call 77 |
| 0x000B0001 | 0x5FFFE000 | 0x1000 | 0x0FBFE012 | InfoRecord |
| 0x000C0001 | 0x64000000 | 0x180000 | 0x0AB00012 | |
| 0x000D0001 | 0x68FEF000 | 0x1000 | 0x00003013 | ro |
| 0x000E0001 | 0x68FF5000 | 0xA000 | 0x0FBF5013 | ro |
| 0x000F0001 | 0x68FFF000 | 0x1000 | 0x0FBFF012 | |
| 0x00100001 | 0x80060000 | 0x1000 | 0x8006003A | I, G |
| 0x00110002 | 0x80000000 | 0x60000 | 0x8000003A | the segment's area, split |
| 0x00120001 | 0x80061000 | 0x0FF9F000 | 0x8006103A | around 0x80060000 |
| 0x00130001 | 0xF0000000 | 0x10000000 | 0xF000003A | |
| 0x00140001 | 0x68F16000 | 0x1000 | no access | guard for 0x68F168F1 |
| 0x00150001 | 0xDEADB000 | 0x1000 | no access | guard for 0xDEADBEEF |

Physical layout (SystemInfo): physical 0x0FFFC000, usable and logical
0x0FA1C000, hash table 2 MB at 0x0FC00000 (SDR1 0x0FC0001F); RAM banks
0x00004000-0x00BFFFFF, 0x00E60000-0x00EFEFFF, 0x01000000-0x0FEE7FFF,
0x0FEEB000-0x0FFFFFFF (the ROM image 0x00C00000-0x00FFFFFF is cut out,
except the unused 0x00E60000-0x00EFEFFF). Kernel memory
0x0FBB5000-0x0FE00000. Logical page 0 is physical 0x4000
(PA_RelocatedLowMemInit).

**Against Blue's `default_regions()`** (`build/blue rom config
MOS91.rom`): the ROM (0xFFC00000), 0x68000000 megabyte, InfoRecord
(rw), kernel data (ro) and EmulatorData (rw) agree. Different:
Blue maps the interrupt-control page at 0x808E0000 (QEMU's Trampoline
uses its machine's 0x80060000), its boot-info region at 0xFF800000
(QEMU: device tree etc. at 0x64000000, ConfigInfo at 0x68FEF000), a page at
0x60C00000, and I/O 0xF0000000-0xFF00FFFF (QEMU: whole segments 8 and F;
0xFFC00000 up is covered by the BAT). Blue has no 0x68FFD000 (PSA) page;
it doesn't need one while its ProcessorState and DiagInfo are in the
InfoRecord page.

## 5. The device tree after the Trampoline

`nk2dump.py --tree` (DeviceTreeBase 0x6400000C, physical 0x0AB0000C).
Root's children in order: **AAPL,CodePrepare, AAPL,CodeRegister**, aliases,
openprom, options, chosen, builtin, packages, cpus, memory, rom, nvram, pci,
uni-n. So the two 'node' parcels become the *first* children of `/`
(Blue's `dt_add_node()` appends them last).

- `AAPL,CodePrepare`: AAPL,prepare_order "Placeholder", MotherBoardHAL,
  TimeManagerLib, NVRAMServicesLib, RTCServicesLib (in parcel order).
- `AAPL,CodeRegister`: NativePowerMgrLib, StartLib, then PMULib (from the
  `via-pmu-99 / power-mgt` 'prop' parcel, child flags 0x10096: special
  node 1, create under it). EtherPrintfLib (flag 0x100, debug only) is not
  there.
- `/rom/macos`: AAPL,toolbox-image,lzss (OpenBIOS's) and
  MacOSROMFile-version "6.1f9" (the `macos` parcel, flags 1: name match).

Properties from 'prop' parcels, as tbxi's flags say (and `src/parcels.c`
does):

| parcel | node | added |
|--------|------|-------|
| `nvram,flash / nvram` (0xC) | /nvram | driver,AAPL,MacOS,PowerPC (2780) |
| `uni-north / pci` (0xC) | /pci | pef,AAPL,MacOS,PowerPC,prepare (2026), code,AAPL,MacOS,name "UniNorth_PCICyclesLib" |
| `via-pmu / rtc` (0xA: type rtc, parent name via-pmu) | mac-io/via-pmu/rtc | driver (712) |
| `via-pmu-99 / power-mgt` (0xC) | mac-io/via-pmu/power-mgt | pef,...,register (40768), code,...,name "PowerMgrPlugin"; PMULib to CodeRegister |
| `keylargo-ata / ata` (0xC) | both mac-io/ata-3 | driver (22008) |
| `gmac / network` (0xC) | /pci/ethernet | lanLib,AAPL,MacOS,PowerPC (33074) |
| `macos` (0x1) | /rom/macos | MacOSROMFile-version |

Not applied (no such node): `pmu / power-mgt` (the node's compatible is
via-pmu-99), `mac-io / nvram`, `via-pmu / nvram`, cuda, grackle,
heathrow-ata, cmd646-ata, cofb, backlight, cardbus, ieee1394. QEMU VGA's
driver is OpenBIOS's own NDRV. This matches Blue's matching rules; only the
node placement differs.

## 6. Software interrupts from event groups

They fire, from the start. Two groups are marked at startup through the
emulator (calls 49/54 via `FE1F`): 0x001B0001 level 7 (the RAM area's
page-fault notification target, flags 0x80000000) and 0x001E0001 level 1
(each Time Manager timer's notification sets its flag 8). Level 1 fires
first at t = 0.313 s, when the first armed timer expires, and then 93 times
in the first 1.2 s (345 in the first 6000 calls), from timer expiry in
whatever task runs (blue or idle). Level 7 never fired in the traced
window (virtual memory is off).

Kernel side (`SetEvent`, break at physical `0x00F1BEF8`): ECB
EDPOffsetSWIRelated (ECB+0xC8) is 0xAE0, so the kernel stores the group ID
in EmulatorData+0xAE0 + 4 × level and the *level* (1) in ECB+0xD0
(SWIEventGroupID), flags the blue task (kFlagSchToInterruptEmu) and, when
the scheduler next runs it, switches it to its System (emulator) context
if needed, sets ECB+0xCC (PriorityShifty) |= 0x100 and ORs PSA.MCR
(PostIntMaskInit & ClearIntMaskInit, 0x00E00000 & 0xFF9FFFFF) into the
emulator's CR, as for external interrupts. 0xAE0 is not the kernel's: the
emulator stores it itself (`li r7,0xAE0; stw r7,0x1C8(r31)` at
`0x6806DDFC`, r31 = EmulatorData).

Emulator side (`0x6806D3E4`): when ECB+0xD0's level is above the 68k
interrupt mask, it reads the group ID from EmulatorData+0xAE0 + 4 × level,
counts in EmulatorData+0xC0, raises the SR mask to the level, builds a
format-2 exception frame for **vector offset 0x58** (vector 22, unused on
a real 68k) and pushes the group ID on the 68k stack. (`0x6806C438`: the
emulator's wait path yields with MPYieldWithHint(1), the boot's very first
call, when ECB+0xCC has 0x100 and no SWI is pending.) Vector 0x58 is
`0xFFC04A64` early and a RAM handler (0x00206CBE) at the Finder.

68k side: the ROM's dispatcher at `0xFFC0FCD0` (counts in ExpandMem
[$2B6]+$31C) calls MPWaitForEvent(group, timeout 0) through the `FE1F`
glue (`0xFFC10BA0`; seen as kc8 call 52 with 68k pc `0xFFC10BB0`, result
flags in r4, 8 for timers) and calls the handler registered for each set
bit. Handlers are registered by `0xFFC0FC00`, which makes the per-level
group on first use (49, 54) from a table at `0xFFC0FBD0`: kind 1 → level 1
flag 8, kind 3 → level 1 flag 4, kind 4 → level 1 flag 2, kind 2 → level 7
flag 0x80000000. After a level-1 interrupt the Time Manager reads its
queue 0x00300001 with MPWaitOnQueue (timeout 0) and re-arms with
MPArmTimer.

## For Blue

1. **`sc` with r0 = -1** comes right after the third task is made (Mixed
   Mode's CallUniversalProc, 53k times per boot). Answer as the kernel does
   (r0 = 0x00200000 and CR0 GT for the blue task, r0 = 0 and CR0 EQ
   otherwise; nothing else changes), not as an MP call. `sc -2`: r0 = the
   task's MPCreateTask r6 word, same CR0.
2. **MPYieldWithHint (13)** is the first call of all and frequent (6.6k per
   boot), all from the emulator at `0x6806C480`, which yields this way
   when ECB+0xCC (PriorityShifty) has 0x100 set; NanoKernel 2's scheduler
   sets that bit when it interrupts the blue task (it was 0x101 at the
   first call). Blue doesn't set the bit, so it may not see these calls;
   if it does, MPYieldWithHint just returns (r3 came back unchanged, 1).
3. **TaskCount** should be 2 at startup (blue and idle): MPLibrary sets
   the page-fault queue's reserve from it (QEMU 2, Blue 1).
4. The real kernel makes 22 objects before the first call (table above);
   matching them would make Blue's IDs equal QEMU's (first call result
   0x00160001, blue task 0x00050001, page queue 0x00040001, RAM area
   0x000A0001). Mac OS seems not to depend on the values.
5. Info record versions/lengths: 9.1's kernel says NanoKernelInfo 0x0221/0x140,
   HWInfo 0x0107/0xC0, SystemInfo 0x0107; Blue says 0x0228/0x160, 0x0103/0xA0,
   0x0105.
6. Node parcels go first among `/`'s children, not last.
7. New tasks don't run at once: here they first ran 170 ms after
   MPStartTask, when the blue task made its first PrioritizeInterrupts
   (kernel call 4, the 68k boot code enabling interrupts), with no blue MP
   call in between. Blue's kernel differs (it runs them when the blue
   task polls or naps); running ready tasks at kernel call 4 too would
   match.
   The first thing each does is block (or, for 'ager', wait 3 s).
8. Software interrupts are needed early (the first at t = 0.313 s, 0.19 s
   after call 63, from the Time Manager's first timer), and arrive at the
   68k as vector 0x58 with the group ID pushed (section 6); Task 5's
   description in [plan.md](plan.md) is right on the kernel side.

## 7. Mac OS 9.2.2 (the MacOS9Lives CD)

`tools/qemu-ref/run91.sh 922.iso` boots the MacOS9Lives 9.2.2 CD (Mac OS ROM
10.2f1, NanoKernel 2.28) to the Finder on `mac99,via=pmu` in under a minute,
with its Read Me open in SimpleText. NanoKernel 2.28 keeps the InfoRecord
page apart from the KDP (10 pages below it), and its info records (at the
Finder, from a RAM dump):

| record          | logical    | version | length |
|-----------------|------------|---------|--------|
| ProcessorState  | 0x68FFDB30 (PSA) | 0x0100 | 0x80 |
| HWInfo          | 0x5FFFEF00 | 0x0108  | 0xC0   |
| ProcessorInfo   | 0x68FFEF20 (KDP) | 0x0112 | 0xA0 |
| NanoKernelInfo  | 0x68FFEDC0 (KDP) | 0x0228 | 0x160 |
| DiagInfo        | 0x68FFDA30 (PSA) | 0x0100 | 0x100 |
| SystemInfo      | 0x5FFFEDC0 | 0x0107  | 0x140  |

HWInfo's new fields (+0x9C, +0xA0 the device tree's checksum, +0xA8
CascadeInfo), the `sc` calls of the boot (`-d int`) and what Blue does
with them are in [plan.md](plan.md)'s Findings ("Mac OS 9.2.2: the CD" and after).
