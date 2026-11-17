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

The sound chip (`src/burgundy.c`) is a Burgundy, the codec of the original
iMac, on the Mac I/O "davbus", which Mac OS drives with its own driver stack
from the System file (`.DAVAudio`, DAVAudioLib, SndHWLib and its
BurgundyPlugin, and the 'awgc' sound output, input and port components;
Mac OS 9's has a DMA plug-in in DAVAudioLib's place). Output takes 16-bit
stereo frames from DMA channel 8, and input puts them in channel 9's
buffers, at the sample rate of guest time, so guest-visible behaviour never
depends on the host. Mac OS turns on two of the davbus's subframes, so each
sample period DMA moves two frames each way: the codec's, and silence.

The codec's own registers (up to four bytes each) are reached through the
davbus's codec control and status registers, a byte a command: writes
carry the register, the byte and the register's length; a read shows the
byte in the status with which byte it is and a count that changes with each
one, which BurgundyPlugin waits for. Blue keeps the whole register file, so
what's read is what was written. Its mixer model follows what Mac OS sets:

- **Inputs:** four channels, each with a source, a coarse gain (1.5 dB
  steps) and a fine volume (0.375 dB steps; Mac OS's 0xDF is 0 dB). Channel
  1 is the CD, 2 the sound-in jack (External Mic and Sound In), 3 the
  built-in microphone (the host's sides mixed: it's one microphone), 4 the
  modem (silent). The gain applies to what's recorded.
- **Recording:** the channels the capture selects have, mixed. The
  microphone channels take the host's microphone (`src/mic.c`, below), the
  CD channel the CD's frames as the codec takes them.
- **Output:** the DMA's frames, and the channels the output selects have
  (the CD, when Mac OS monitors it), mixed; then the master volume (0.375 dB
  steps) and the louder of the enabled outputs' attenuation (speaker, line
  out, headphones; 1.5 dB steps), so the volume slider and Mute work.
  Nothing is plugged into the headphone or sound-in jacks.
- **SRS 3D Surround:** Mac OS writes its filter coefficients and turns it
  on (it's on by default); Blue keeps them and plays without it.

What it took for Mac OS to use the chip: a DecoderInfo address for it
(Gestalt `snhw` is then `awgc`), the device-tree node with
`AAPL,interrupt-vectors` (which Mac OS 8.5's DAVAudioLib uses) and an
interrupt set (`driver-ist`, which Mac OS 9's DMA plug-in needs: without
one it fails to start, and Mac OS 9 had no sound output at all), and a
`sound` child as the iMac's: `compatible` "burgundy" and "awacs" (Mac OS
8.5 tries GCAwacsPlugin first, which declines a codec that isn't an AWACS
or a Screamer, then BurgundyPlugin; its sound input driver, `.AppleSoundInput`,
is matched by "awacs"), `model` "343S0177" (Mac OS 9 picks BurgundyPlugin
by it), and `vendor-id`/`device-id` (Apple, 1), which pick the iMac's port
layout in the 'soph' component; without them the output device opens but
never plays. The codec's status says it's valid (bit 22), which Mac OS's
sound plug-in polls for (see [Mac OS 9.2.2](bring-up.md#mac-os-922-from-its-cd)).
On Mac OS 9.0.x (the System file's version on the startup disk) the node
says the chip has output only (`hardware-types` 2), which keeps that
version's buggy sound input component away (see [Mac OS 9](bring-up.md#mac-os-9)):
there, Mac OS records nothing. [findings.md](findings.md) has what
Mac OS sets for each input.

CD audio from the CD-ROM drives is described in
[drives.md](drives.md#cds-audio-and-cue-sheets).

On the host, SDL2 plays the sound (`src/audio.c`; in real time, `--no-sound` turns it
off; a small queue is kept, its frames resampled to play up to 0.5%
faster or slower as it's fuller or emptier than its goal, so that the
guest's clock and the device's can't drift apart into growing delay or
gaps, and a stall of the host is made up within seconds).

The microphone is the host's default sound input, through SDL2's capture
(`src/audio.c`, `src/mic.c`), in real time only. Blue opens it only while
Mac OS records from the built-in microphone or the sound-in jack (input DMA
running with one of their channels captured, for a tenth of a second), and
closes it two seconds after Mac OS stops; the status bar's Microphone
light is on while it's open, and the F12 menu says so and turns sound
input off (Mac OS then records silence; the choice is kept in the settings
file), as `--no-mic` does. On macOS the system asks the user's permission
the first time (Info.plist's `NSMicrophoneUsageDescription`); until it's
given, or if it's refused, Mac OS records silence and Blue says why on one
line. The host's frames wait in a ring of 16384 (the oldest dropped when
it's full; silence when it runs dry, until 1024 are waiting again) and
are resampled to the codec's rate in a straight line between the frames on
either side, taken up to 0.5% faster or slower as the ring is fuller or
emptier than 2048, so the host's clock and guest time can't drift apart.
Guest time takes them at its own pace: the host only fills the ring.
Instruction-time runs never open the microphone; they record silence, or
a WAV file given with `--mic-file` (over and over) or scripts' `mic play
FILE` (once; `mic stop` stops it), which is read as Mac OS records, so runs
stay reproducible.

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

Blue's USB controller (`src/ohci.c`) is an OpenHCI 1.0a controller (an
OPTi 82C861's PCI IDs, class 0c0310) with a root hub of two ports, driven
by the ROM's own OHCIUIM. What it took for the ROM to use it: PCI
configuration space (`src/pci.c`), through Grackle's
`CONFIG_ADDR`/`CONFIG_DATA` registers and RTAS's
`read-pci-config`/`write-pci-config`; an interrupt set for the
controller (`driver-ist`, without which the UIM gives up with
nrNotFoundErr; see [interrupt sets](#interrupt-sets)); and a ProductInfo
flag (bit 26 at `+0x28`) for which the ADB Manager sets the hook that the
USB keyboard and mouse shims call (without it they call a null hook once
the controller is found).

### Frames

While the controller is operational, frames are counted from guest time
in 12 MHz bit times (FmInterval + 1 each): each advances the frame
number, writes it to the HCCA in guest memory and raises start-of-frame
(and, every 32768, frame-number overflow). Until a device has been
connected, nothing more happens: there are no transfers, the lists aren't
walked, and Mac OS keeps overflow enabled and start-of-frame only while it
waits for a frame to pass, so an idle host sleeps until the overflow
(every 33 s) rather than waking at every frame.

Once one has been connected, each frame that ends does the frame's work,
as the hardware would within it, and then the next one starts:

1. **The periodic list:** the HCCA's interrupt table entry for the frame's
   number (its low five bits), and the EDs from there on: interrupt EDs,
   one TD each, then (with IsochronousEnable) isochronous ones, a packet
   each.
2. **The control list,** if ControlListEnable and ControlListFilled are
   set, then **the bulk list** likewise: ListFilled is cleared as the list
   starts, and set again if any ED on it had a TD, so a list is looked at
   again while it has work and not otherwise, as the specification has it.
   An ED's TDs are serviced one after another (at most 64 a frame on an
   ED, 512 in all), stopping at one that waits.
3. **The done queue:** retired TDs are put at its head (NextTD pointing at
   the previous head). Its interrupt delay counter takes the smallest
   DelayInterrupt of the TDs retired (0 for one with an error); at a
   frame's end, when it's 0 and WritebackDoneHead is clear, HcDoneHead
   goes to HccaDoneHead (bit 0 set if another enabled interrupt is
   pending), HcDoneHead is cleared and WDH set; otherwise the counter
   counts down, unless it's 7 (no interrupt asked for).
4. **The root hub's port resets,** which last 10 frames.

Bandwidth isn't accounted (PeriodicStart, the control/bulk service ratio
and LSThreshold are kept but don't limit anything): a frame moves all its
lists can, which for the test disk is much more than USB 1.1's 12 Mbit/s.
After a long gap in guest time (real time, with the host busy), the last
64 frames' lists are serviced and the rest only counted.
`ohci_next_event()` then makes every frame boundary an event, so frames
are serviced on time in instruction time too; in real time, the host
naps until a frame with work to do (TDs that aren't merely waiting on a
device, a done queue to write back, a port being reset) or with an
interrupt, waking every millisecond while transfers are with the host's
devices.

### Transfers

A **general TD** moves its whole buffer (CBP to BE, over two pages at
most: the second is BE's) at once: a TD of SETUP, OUT or IN, as the ED's
direction says (or the TD's, where the ED leaves it to them). The data
toggle is the TD's (T's high bit set) or the ED's carry, and changes with
each packet moved (MaxPacketSize from the ED), so a TD's T and the ED's
carry are left as the packets would leave them. When the device answers:

- **all moved:** NoError, CBP 0;
- **an IN short of the buffer** (the last packet short, or empty): with
  bufferRounding, NoError; without, DataUnderrun, and the ED halts; CBP
  points after the last byte received either way;
- **a stall:** Stall, and the ED halts (H in HeadP);
- **more than there was room for:** DataOverrun, and the ED halts;
- **NAK:** nothing changes, and the TD is tried again at the next frame
  (ListFilled stays set);
- **no device at the ED's address** (on an enabled, unsuspended port, not
  being reset): ErrorCount goes up and DeviceNotResponding goes in its
  ConditionCode, a frame each try; at three it's retired, halting the ED.
  A device lost on the way (the host's transfer failed) is retired at
  once, as CRC.

Retiring a TD writes its ConditionCode, ErrorCount, T and CBP, links it
into the done queue, and moves the ED's HeadP on (with the carry, and H
for an error).

An **isochronous TD** has a packet for each frame from its StartingFrame
for FrameCount + 1 frames: in its frame, a packet's offset (13 bits, the
top one choosing BP0's page or BE's) and the next one's give its buffer,
and its packet status word gets the ConditionCode (DataUnderrun for an IN
shorter than its buffer, as the specification has it for isochronous INs)
and the size received (0 for OUT). After its last packet the TD is
retired, NoError; a TD whose frames have all gone by is retired
DataOverrun without a packet moved, and the next one looked at. Isochronous
EDs don't halt.

An ED with sKip set, halted, or with HeadP equal to TailP is passed by.

### Devices

Devices (`src/usb.h`) take transfers on their endpoints: a whole control
request on endpoint 0, a TD's worth of a bulk or interrupt transfer, or
an isochronous packet. `usb_control()` turns endpoint 0's stages into
requests: a SETUP starts one (a request for data in is given to the
device then; one with data out gathers it from the OUT TDs that follow,
and goes to the device at the status stage, as one with no data does);
the data stage's INs take what the device answered, and NAK until it has;
the status stage completes, or stalls if the device did. SET_ADDRESS is
done there: the address changes after its status stage, and a bus reset
puts it back to 0.

A device answers at once, or later (bulk and interrupt endpoints and
control requests: the host's devices, whose transfers go through libusb).
A transfer that waits keeps its TD at the ED's head, NAKed at each frame,
until the answer is there (one transfer in flight per ED, 32 at once);
then the TD is retired with it. One whose ED is skipped, halted or given
another head TD, or that isn't seen for 64 frames, is given up (the
device cancels it, and won't touch it again). A device unplugged has its
transfers given up; TDs still meant for it then get no answer.

The root hub's ports: plugging a device in sets CurrentConnectStatus,
ConnectStatusChange and, for a low-speed one, LowSpeedDeviceAttached, and
RootHubStatusChange; SetPortReset holds PortResetStatus for 10 frames,
then enables the port (PortEnableStatus, PortResetStatusChange) and resets
the device. SetPortEnable, ClearPortEnable, SetPortSuspend and
ClearSuspendStatus (which resumes it: PortSuspendStatusChange) work on a
connected port; on an empty one, they only report a connect status change,
so that the driver looks again. Unplugging clears connection and enable
and reports both changes. The ports are always powered. The controller
going to UsbReset resets the bus: connected devices are reset, connected
but not enabled, with a connect status change, as Mac OS's driver expects
when it starts.

**The test disk** (`src/usbmsd.c`): a full-speed USB mass storage device
(class 8, SCSI transparent subclass 6, bulk-only transport 0x50; ID
1209:0001, pid.codes' test ID) whose disk is an image file. Bulk-only:
the CBW (31 bytes, `USBC`) on the OUT endpoint, then the data, then the
13-byte CSW (`USBS`) on the IN endpoint; a bad CBW stalls both endpoints
until Bulk-Only Mass Storage Reset and CLEAR_FEATURE(ENDPOINT_HALT); with
less data than the host asked for, it sends zeros to make up the length
and says so in the residue. Its SCSI commands are in `src/usbmsd.h`;
it's removable, so Mac OS ejects it with START STOP UNIT, after which
TEST UNIT READY reports no medium. It answers at once, so it works in
instruction time (see [debugging.md](debugging.md#the-usb-test-disk)).

**The host's devices** (`src/usbhost.c`) go through libusb-1.0, loaded at
run time (`src/hostlib.c`) with its functions taken by `dlsym` and the
structures Blue uses declared in Blue (libusb's ABI is stable). libusb's
events are handled on a thread of its own; a transfer's answer is copied
to the controller's buffer there, under the device's lock, and `done` set
last, and the controller takes it at a frame. Opening a device claims all
the interfaces of the configuration it's in, first detaching the host's
driver from each that has one (Linux: usbfs's disconnect, which needs the
device node to be writable; macOS: libusb's capture of the whole device,
which needs root, 1.0.25 or later), and reattaches them when it's let go.
Mac OS's requests go to the device as control transfers, except these,
which libusb's own calls carry out so that the host's view of the device
stays right: SET_CONFIGURATION (the interfaces released, the configuration
set, the new one's interfaces claimed, kernel drivers detached again),
SET_INTERFACE, and CLEAR_FEATURE(ENDPOINT_HALT) (which resets the host's
toggle too). A bus reset goes to the device (libusb's reset) once Mac OS
has configured it, not on the first enumeration. Each endpoint's type
comes from its descriptors (Mac OS's OHCIUIM puts bulk endpoints on the
control list, so the list doesn't say). Isochronous endpoints run as
streams: IN keeps three transfers of eight packets going and hands Mac OS
the packets received, one a frame (none: an empty packet); OUT gathers
Mac OS's packets into transfers of eight. libusb's hot-plug events (or a
transfer finding the device gone) mark a device unplugged; Blue then
unplugs it from its port, and puts a device with the same IDs plugged in
again back in the same port.

**High speed:** the iMac's OHCI is USB 1.1, and a device the host has
connected at high speed (or faster) isn't taken, as QEMU's OHCI doesn't
take one: presenting it as full speed would mean rewriting its
descriptors' packet sizes and polling intervals, and splitting and joining
transfers, where USB's short packets end transfers at 64 bytes on one side
and 512 on the other, and a device's protocol may depend on them. Through
a USB 1.1 hub, a USB 2.0 device connects at full speed, with full-speed
descriptors of its own, and passes through as it is.
