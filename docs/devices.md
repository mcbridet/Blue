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
interrupt controller, a Z8530 SCC (its channel A the internal
[modem's](#the-modem); channel B, the infrared port's, has nothing
attached, so LocalTalk there sees an idle network), and for ROM files with
parcels the NVRAM chip behind Mac I/O
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
well, and Open Transport doesn't stall with that one either: [findings.md](findings.md),
"Open Transport's stall"), with
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

## The serial controller

The iMac's serial controller is a Zilog Z85C30 ESCC in Mac I/O
(`src/scc.c`), with no ports on the outside: its channel A is the internal
modem's and channel B the infrared port's. The firmware describes it to
Mac OS in the device tree, as an `escc` node (and `escc-legacy`, the same
channels at the old Macs' register layout) with a `ch-a` and a `ch-b`:
each with its control, data and enhancement registers, a DMA channel for
each direction (A's are 4 and 5, B's 6 and 7) and an interrupt for each of
the three (the SCC's own at level 4, as on every Mac, and the two DMA
channels'). Mac OS's serial driver, the ROM's SerialDMA, finds the channels
by `compatible` (`chrp,es2`, `chrp,es3`), and sends and receives by DMA:
it receives through a ring of buffers that it stops and restarts to see
what has come in, with the SCC raising its DMA request for the receiver,
and it transmits through a ring of its own, the SCC taking each character
as there's room in its transmit FIFO. The SCC's interrupts tell it of
errors, breaks and CTS changes (for hardware handshaking), through the
68k-style dispatch of Mac OS's SCC interrupt handler (RR3, and the vector
modified with the status through channel B). The modem's extension, which
names the port "Internal Modem" in the Modem control panel, finds the
modem by channel A's `slot-names` (Modem) on Mac OS 8.5 and by a channel
A whose `compatible` names `cobalt` on Mac OS 9 (Blue's legacy one does:
see [findings.md](findings.md), "The iMac's serial controller in the
device tree"). The nodes are in the tree only with the modem (see
[below](#the-modem)): other runs are as they were without it.

With the modem attached, channel A carries data both ways. What Mac OS
sends goes through the transmit FIFO (4 characters) to the shift register,
and leaves one character per character time: start, data, parity and stop
bits at the bit rate Mac OS programs (the baud-rate generator's time
constant, its clock, 3.6864 MHz, and the clock mode: 57,600 or 115,200
bits a second as the modem scripts set it). The modem's characters come
into the receive FIFO (8) at the same pace, and wait at the modem while
it's full. Everything is in guest time, so what Mac OS sees never depends
on the host: a device event at the next character's time, like the other
devices' (see [When devices are serviced](debugging.md#when-devices-are-serviced)).
The receive, transmit and external/status interrupts, DTR and RTS (to the
modem) and DCD and CTS (from it) are the SCC's, with RR0's CTS bit
inverted, as on every Power Mac (the bit is clear while CTS is
asserted). Channel B, with nothing attached, is as the whole SCC used to
be: what Mac OS sends vanishes at once, the receiver sees an idle line,
and it raises no interrupts.

## The modem

The iMac's built-in 56K modem "appears to the system as a serial port that
responds to the typical AT commands" (Apple's iMac Developer Note), and
Blue's is one: a Hayes-compatible modem (`src/modem.c`) on the SCC's
channel A, whose calls go over TCP (`src/modemnet.c`), as DOSBox's and
86Box's do. It's there in a window or real time (`--no-modem` leaves it
out), and in instruction-time runs that ask for it (`--modem-listen`,
`--modem-phonebook`, or an input script's `modem` commands): Mac OS sees
the escc nodes and the modem only then, so other runs stay the same
instruction for instruction.

- **Commands:** what Mac OS's modem scripts send (8.5's "iMac Internal
  56k" and 9's "Apple Internal 56K Modem (v.90)"), and what terminal
  programs use: `ATZ`, `AT&F`, `E`, `V`, `Q`, `X`, `M`, `L`, `H`, `O`, `A`,
  `D`, `S` registers (`S0` auto-answer, `S2` and `S12` for the escape, `S7`
  for how long dialling waits, `S25` for how long DTR has to drop), `&C`,
  `&D`, `I`, `&V` and `A/`, with verbose or numeric result codes; the rest
  (`\N`, `%C`, `&K`, `W` and such) are taken with OK. `ATI12` answers `D1`,
  by which Mac OS 9's script picks its settings.
- **Dialling:** `ATDT host:port` (or `ATDP`) connects there, `host` alone
  to port 23, and answers `CONNECT 56000` once connected, or `NO CARRIER`
  (no answer, or nobody there) or `BUSY` (refused; `NO CARRIER` too
  after `ATX0` to `ATX2`). A number of digits is
  looked up in the phonebook (`--modem-phonebook FILE`: lines of `NUMBER
  HOST:PORT`, with `telnet` or `raw` after it if need be, `#` starting a
  comment); a number dialled in parts (`ATDT555;`, then `ATDT1234`), as
  Mac OS's scripts do with long numbers, is put together. Names are looked
  up and connections made on a thread of their own, and a key pressed while
  dialling gives up, as on a real modem.
- **Answering:** with `--modem-listen [ADDRESS:]PORT`, a connection to that
  port rings (`RING` every six seconds of guest time, `S1` counting) until
  `ATA`, or the `S0`th ring when `S0` is set. A connection coming in
  during a call is closed at once.
- **The call:** data goes both ways while it's on; `+++` with a second
  (`S12`) of silence before and after returns to command mode, the call
  kept (`ATO` goes back); so does `+++` followed at once by a command and
  its CR (`+++ATH`, as Mac OS's scripts hang up). DCD follows the carrier
  (`&C1`); dropping DTR for `S25` (0.05 s) hangs up (`&D2`), but not the
  moment's drop of a serial driver resetting its channel. The far end
  hanging up gives `NO CARRIER`. Characters go at the serial port's rate,
  not a 56K line's.
- **Telnet:** to port 23, to a phonebook entry marked `telnet`, and to any
  far end that opens with a telnet command (as BBSes do), the modem speaks
  telnet (RFC 854), as DOSBox's does: a 0xFF sent is doubled and one
  received undoubled, and option negotiation is answered minimally (binary
  and suppress-go-ahead agreed to both ways, the far end's echo accepted,
  anything else refused); subnegotiations are skipped.
- **Real time only,** as Ethernet: in instruction time the modem answers
  its commands as ever, but dialling gives `NO CARRIER` and nothing rings,
  so runs are reproducible; an input script's `modem` commands stand in
  for a far end (see [Input scripts](debugging.md#input-scripts)).
- **Blue's menu** shows what the modem is doing ("Modem: connected to
  host:port", "Modem: idle, listening for calls on port N") with a Hang Up
  button. `BLUE_LOG_MODEM=1` logs its commands and calls,
  `BLUE_LOG_SERIAL=1` the SCC's registers.

Mac OS 9's extension for the modem resets it through a GPIO of Heathrow's
at the real iMac's Mac I/O address (`0x8000006D`), which it has written
in; Blue's Mac I/O is elsewhere, so with the modem there that byte is
answered there too (it's only kept: the modem needs no resetting).

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

In real time (the window, or `--headless --realtime`), the guest's built-in Ethernet is
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
