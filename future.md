# Future features

Ideas worth doing some day, not planned yet. Each says what the real iMac
had, what it would be good for under syshm, and what it would take.

The modem and infrared both need the same thing first: a serial controller
that carries data. syshm's Z8530 SCC (`src/scc.c`) has nothing attached
today: what Mac OS sends vanishes, the receiver always sees an idle line, and
it raises no interrupts. It would need data both ways, its interrupts
(receive, transmit, status changes), the handshake and carrier-detect lines
the drivers watch, timing at the chosen bit rate, and perhaps Mac I/O's DMA
channels for the SCC if Mac OS's serial driver uses them on the iMac. Runs
that use the network stay real-time only, as with Ethernet, with input-script
commands for reproducible tests.

## Modem

- **The iMac's:** a built-in 56K modem (V.90 and K56flex; fax at 14.4K) that
  "appears to the system as a serial port that responds to the typical AT
  commands" (Apple's iMac Developer Note). Mac OS has a modem script for it
  ("iMac Internal 56K"), so the Modem control panel, Remote Access and
  serial programs find it as they would on the real machine.
- **What for:** not the Internet (Ethernet already reaches it, far faster),
  but software that only knows modems: terminal programs such as ZTerm
  calling telnet BBSes (many still run), head-to-head modem games of the
  90s between two syshm windows or over the Internet, and old dial-up
  clients.
- **How:** as DOSBox and 86Box do, the modem talks over TCP. `ATDT
  host:port` (or a number from a small phonebook) connects there and answers
  `CONNECT 56000`; hanging up closes the connection. An option such as
  `--modem-listen PORT` makes an incoming connection ring, for `ATA` or
  auto-answer (S0), so two syshm instances can call each other. The usual
  Hayes behaviour: the command set Mac OS's script sends, `+++` with its
  guard time, result codes, and carrier detect following the connection.
- **For fun:** the real modem fed its speaker (dialling, the handshake) into
  the sound chip; syshm could play that while connecting.
- **Size:** moderate, nearly all of it in the serial controller; the modem
  and TCP parts are small.

## Infrared (IrDA)

- **The iMac's:** revision A and B iMacs have an infrared port that supports
  IrDA but not Apple's older IRTalk (Apple Tech Info Library 20928);
  revision C dropped infrared. Mac OS 8.5-9 drive it with the Infrared
  control panel. Which part of the I/O chip drives the transceiver, and how
  the device tree describes it, is to be checked in the developer note and
  the ROM's tree.
- **What for:** little today. macOS never had IrDA, and Linux removed its
  stack in 4.17, so there is nothing on the host to talk to. What could
  work is a virtual beam: two syshm instances joined over TCP, so the
  Infrared control panel's file exchange (and whatever runs over it) works
  between two emulated iMacs. Mostly for completeness and curiosity.
- **How:** the serial controller above in IrDA mode, with the frames
  (IrLAP) passed to the other instance as they are; Mac OS's own stack does
  the rest.
- **Size:** small once the serial controller carries data, but low value.

## 3D sound

- **SoundSprocket** (Apple Game Sprockets, Mac OS 8-9): software 3D sound
  for games, a listener and sources placed in space, filtered through the
  Sound Manager so two speakers or headphones give direction. It runs on the
  guest's processor before the sound reaches the sound chip, so it should
  already work with Game Sprockets installed (Bugdom's setup includes it).
  Nothing to do but confirm it with a game that uses it.
- **The iMac's SRS "3D Surround Sound":** a stereo-widening effect in the
  iMac's audio hardware, after the sound chip, turned on and off in
  Monitors & Sound (Sound, "Enabled"; Apple advised turning it off for
  headphones). Under syshm the switch does nothing, and it isn't known
  whether the checkbox even appears (that depends on what the emulated sound
  hardware reports). To do it: find which codec or GPIO bit the iMac's
  sound driver sets for it, then widen the host's output while it's on.
  SRS's processing was proprietary, so this would be an approximation, and
  today's headphones and host systems often add their own spatial audio, so
  it's low priority.
