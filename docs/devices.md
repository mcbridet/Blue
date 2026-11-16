# The iMac's devices

Blue gives Mac OS the devices of the original iMac (`iMac,1`) that Mac
OS's own drivers drive, and native drivers of its own, in the device tree,
for the rest: the display, the disks and the Ethernet. Where each device
sits in the machine is in [architecture.md](architecture.md#the-virtual-machine);
the drives are in [drives.md](drives.md), and the window, which shows the
screen and takes the keyboard and mouse, in [window.md](window.md).

## The devices

Blue emulates RTAS (clock, NVRAM, power), a 6522 VIA with timers, the Cuda (time,
PRAM, power, ADB with autopoll), an ADB keyboard and mouse, a Heathrow-style
interrupt controller and a Z8530 SCC with nothing attached (LocalTalk sees an
idle network), and for ROM files with parcels the NVRAM chip behind Mac I/O
(`src/nvram.c`; see [Mac OS 9.1](bring-up.md#mac-os-91-from-its-cd)). Device registers are emulated by decoding the faulting
load/store (`src/mmio.c`), without an exit where it would do nothing more
(see the JIT's [device registers in place](jit.md#device-registers-in-place)).

The keyboard and mouse are ADB devices behind the Cuda, as on the iMac's
predecessors; how the window feeds them is in
[window.md](window.md#keyboard-and-mouse).

## Interrupt sets

Driver Services interrupt sets for devices (their
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
its receive DMA channel, the one Blue raises; lanLib takes its set's
last member, which is that channel's in the iMac's set of all three as
well, and Open Transport doesn't stall with that one either: [plan.md](plan.md#findings),
Findings), with
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

## DBDMA

Blue's DBDMA (`src/dbdma.c`) has Mac I/O's 16 descriptor-based DMA channels, after
the CHRP I/O Device Reference (chapter 15): input/output with the MORE and
LAST forms and streams, STORE_QUAD/LOAD_QUAD (memory, device registers,
channel registers), NOP, STOP; conditional interrupt, branch and wait on
the channel's status bits; run, pause, flush, wake and dead. Devices plug
into a channel's streams and may stall it until they have data or room;
transfers take no time of their own, so DMA happens at deterministic points.
Channel interrupts are pulses that the interrupt controller latches until
cleared, or until the kernel takes them when it delivers them (see
[interrupts](architecture.md#the-kernel)).

## Display and the vertical blank

The display driver (`src/video.c`) offers 640x480 to 1280x1024 at 1-32 bits per pixel, CLUT and gamma
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
framebuffer holds at 32 bits per pixel), for Blue Enhancements (a guest
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

## Sound

The sound chip (`src/awacs.c`) is an AWACS-family codec on the Mac I/O "davbus", which
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
input component away (see [Mac OS 9](bring-up.md#mac-os-9)). The codec's status says it's valid
(bit 22), which Mac OS's sound plug-in polls for (see [Mac OS 9.2.2](bring-up.md#mac-os-922-from-its-cd)).

CD audio from the CD-ROM drives is described in
[drives.md](drives.md#cds-audio-and-cue-sheets).

On the host, SDL2 plays the sound (`src/audio.c`; in real time, `--no-sound` turns it
off; a small queue is kept, its frames resampled to play up to 0.5%
faster or slower as it's fuller or emptier than its goal, so that the
guest's clock and the device's can't drift apart into growing delay or
gaps, and a stall of the host is made up within seconds).

## Networking

In real time (`--window`, `--realtime`), the guest's built-in Ethernet is
connected to the host's network through libslirp, the user-mode NAT QEMU
uses (Blue.app and the AppImages carry it; a build of your own loads it
at run time if it's installed, so it isn't a build dependency;
`--no-net` leaves the Ethernet unconnected). DHCP gives the guest
10.0.2.15, with the gateway at 10.0.2.2 and DNS at 10.0.2.3. The guest can
open connections out; nothing outside can connect in. Only IP gets through,
so AppleTalk over Ethernet finds nobody (and makes startup wait, though
fast start mostly hides that; see [Mac OS 9](bring-up.md#mac-os-9)).

A fresh Mac OS 8.5 has TCP/IP set up for Ethernet and DHCP but inactive:
open the TCP/IP control panel, answer Yes to make it active, then close it
and save. Browsers of the time can't load most of today's HTTPS sites;
plain-HTTP sites work.

`BLUE_NET=1` connects the Ethernet in instruction-time runs too (the
host network's timing then makes them unreproducible), and
`BLUE_LOG_NET=1` logs the driver's calls. Frame counts are printed at
exit.

For Ethernet (`src/net.c`), no chip is emulated. On New World Macs the
firmware supplies the hardware layer of Mac OS's Ethernet driver, as a
code fragment in the `ethernet` node's `lanLib,AAPL,MacOS,PowerPC`
property, which the ROM registers as the shared library lanLib; Open
Transport then loads "Ethernet (Built-In)", a DLPI driver around it.
Blue puts its own generated lanLib there (the interface follows Apple's
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

## USB

Blue's USB controller (`src/ohci.c`) is an OpenHCI controller (an OPTi 82C861's PCI IDs, class
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
nrNotFoundErr; see [interrupt sets](#interrupt-sets)); and a ProductInfo flag (bit 26
at `+0x28`) for which the ADB Manager sets the hook that the USB keyboard
and mouse shims call (without it they call a null hook once the
controller is found).
