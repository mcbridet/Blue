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

## Sound input (microphone and line in)

- **The iMac's:** a built-in microphone above the screen and a stereo
  sound-in jack, both into the sound chip's input, chosen in Monitors &
  Sound (Sound control panel in Mac OS 9).
- **What for:** recording in Mac OS's own programs (SimpleSound, alert
  sounds, sound editors), PlainTalk speech recognition and Speakable Items
  with the host's microphone, and games or programs that listen.
- **Where syshm is:** the codec's input DMA channel (9) runs at the right
  pace but records silence, and the sound node declares output hardware
  only (`hardware-types` 2), because Mac OS 9's AWACS input component
  ('sind' 'awgc') has a bug in its close routine that corrupts the system
  heap at startup (README, Mac OS 9). Declaring input again brings that
  back, so that comes first. Options: declare input only for systems
  without the bug (8.5 and 8.6 use another sound stack; check which have
  it), make the bug harmless (a RAM patch of the component, with the
  injection the guest-tools installer brings), or give Mac OS 9 its input
  another way, as an emulated USB audio device on syshm's USB controller
  (like Griffin's iMic, which iMac owners used), where Mac OS 9's USB audio
  support handles it; whether 8.5 and 8.6 have USB audio support is to be
  checked.
- **How (host side):** capture with SDL2 (loaded at run time, as for
  output) from the host's default input, or one chosen with an option, in
  real-time runs only. Captured frames go into the input DMA buffers at the
  guest's rate (rate-matched as output is), with the codec's input source
  and gain bits applied. Instruction-time runs keep recording silence, or
  read a WAV file given with an option, so tests stay reproducible.
- **Privacy:** the host's microphone opens only while Mac OS is actually
  recording (input DMA running) and closes after; the status bar shows a
  light while it's open; on macOS the system asks the user's permission the
  first time.
- **Size:** moderate: the host capture and DMA side are small; the Mac OS 9
  input component is the uncertain part.

## AltiVec

- **The iMac's:** none. Its PowerPC 750 (G3) has no vector unit, and syshm
  reports that processor (PVR 0x00080301); an AltiVec instruction (primary
  opcode 4) stops with "unimplemented". Mac OS 9 software checks for AltiVec
  (Gestalt `ppcf`, or the processor type) and takes its ordinary code
  without it, as on a real G3 iMac.
- **What for:** little compatibility: nearly all Mac OS 9 software,
  professional programs included, ran on G3s, and AltiVec only made it
  faster on G4s. A few things needed it: Apple's DVD Player decoding in
  software (which would also need a DVD drive), Final Cut Pro's real-time
  effects, iDVD (with a SuperDrive), some late plug-ins and encoders. Speed:
  on a G4, AltiVec made Photoshop filters, QuickTime codecs, MP3 encoding
  and Mac OS 9.1's own BlockMove 2-4 times faster or more. syshm already
  runs scalar code at about 2.5 billion instructions a second (about ten
  times the iMac, in the range of the fastest G4s that ran Mac OS 9 before
  their AltiVec), so this would mostly speed up vector-heavy work.
- **Identity:** no G3 iMac had AltiVec. Either syshm reports a G4 (a 7400
  PVR; Mac OS 8.5 predates the G4, which shipped with 8.6) or it keeps the
  G3 and only says it has a vector unit where Mac OS asks (programs that go
  by the processor type then don't use it). Either way it's an option, off
  by default.
- **How:**
  - The interpreter: about 160 instructions and the vector loads and
    stores, with VSCR (saturation, non-Java mode flushing denormals to
    zero) and the estimates (vrefp, vrsqrtefp, vexptefp, vlogefp), whose
    results must be the same in the interpreter and the JIT (and close to
    the 7400's).
  - The JIT, which is required, not an extra: interpreted, AltiVec
    instructions cost about 10 ns each, so programs taking their AltiVec
    code would get slower than their scalar code. Most map one to one onto
    AArch64 NEON (also 128-bit): adds and saturating adds, multiply-adds,
    compares, shifts, vperm with TBL. Vector registers in the register
    cache; the fuzz tests cover them.
  - The kernel: vector registers saved and restored with contexts and MP
    tasks (VRSAVE to skip unused ones), the AltiVec Unavailable exception,
    under both the NanoKernel 1 and 2 interfaces.
  - x86_64 hosts have no JIT, so AltiVec would be slow there; one more
    reason for an option.
  - Once Gestalt says AltiVec, Mac OS 9.1 itself switches BlockMove to its
    AltiVec code and QuickTime to vector codecs, from the first boot, so the
    translation has to be right and fast from the start.
- **Size:** large, comparable to a big part of the JIT. Worth it when a
  program needs AltiVec, or vector-heavy work turns out too slow; try the
  program first.

## Printing

- **The iMac's:** no printer port. Printers were USB ones with their own
  drivers, or network printers: LaserWriters over AppleTalk, or any
  PostScript printer by LPR over TCP/IP (LaserWriter 8's Desktop Printer
  Utility makes an LPR desktop printer).
- **What for:** printing from Mac programs to a PDF on the host, or to the
  host's printer.
- **How:** syshm's network gateway (10.0.2.2) answers LPR (port 515);
  Mac OS's LaserWriter 8 sends it PostScript, and syshm writes the job out
  as a PDF in a folder (an option, the Desktop by default) with
  Ghostscript's pdfwrite (found at run time; recent macOS no longer
  converts PostScript itself), or hands it to the host's printing system
  (`lp`: CUPS, on macOS and Linux). Without Ghostscript the PostScript file
  is kept and syshm says so. The guest tools carry a PPD for a "syshm PDF
  Printer" (paper sizes, colour), and the install prompt could set up the
  desktop printer. Programs that draw with QuickDraw print too: LaserWriter
  8 turns QuickDraw into PostScript. The NAT has to accept connections to
  the gateway's own address for this. Real-time runs only, as for all
  networking.
- **Alternatives:** a LaserWriter on AppleTalk (PAP) at the gateway, which
  needs AppleTalk to get past the NAT (only IP does today), or a printer
  driver of syshm's own in the Chooser (far more work).
- **Size:** moderate: an LPD listener in the NAT, the job handling, the PPD.

## Game controllers

- **The iMac's:** USB only. Gamepads and joysticks (Gravis GamePad Pro USB,
  MacAlly iShock, Thrustmaster and others) were used through InputSprocket
  (Game Sprockets), with drivers from their makers or a generic USB HID
  driver.
- **Where syshm is:** the host's gamepads don't reach Mac OS; games see the
  keyboard and mouse only.
- **What for:** games with InputSprocket support, which most Mac action
  games of the late 90s have (Unreal Tournament among them).
- **Host side:** SDL2's game controller API (loaded at run time like the
  rest of SDL; macOS and Linux; plugging in and out; the standard layouts of
  Xbox, PlayStation and Switch pads), in window runs only, with script
  commands for tests.
- **Guest side, options:**
  - An InputSprocket driver among the guest tools, its logic in syshm
    through host calls like the 3D engine's: the pad appears in each game's
    InputSprocket configuration with named buttons, sticks and triggers,
    and no USB emulation is needed. How InputSprocket finds its drivers
    (plug-ins in the Extensions folder, to confirm) decides the packaging.
    The first choice.
  - A virtual USB gamepad (HID) on syshm's USB controller, which has no
    devices yet, for Mac OS's own USB drivers: more general (games that
    read USB HID themselves; a base for other USB devices), but it needs
    USB device emulation and depends on what Mac OS 8.5-9.1's USB HID
    support does with gamepads (to check).
  - A host-side mapping of the pad to keys and the mouse, for games without
    InputSprocket (set up in the F12 menu, say).
- **Related, small:** right-click as Control-click (Mac OS 8's contextual
  menus), and the scroll wheel (Mac OS 9 had no system-wide support; the
  guest tools could turn it into scrolling).
- **Size:** small to moderate for the InputSprocket driver and the key
  mapping; moderate for the USB gamepad.

## More CD formats

- **Where syshm is:** a CD image is a plain one (an Apple partition map with
  HFS partitions, a bare HFS or HFS+ volume, or ISO 9660, which an
  installed system mounts with ISO 9660 File Access) or a cue sheet with
  `.bin`, WAVE or AIFF tracks, whose audio the CD-ROM drive plays and reads
  as Apple's CD-ROM driver does (README, "CDs: audio and cue sheets").
  Hybrid Mac/PC discs usually carry a partition map too and should work
  (not yet tried).
- **Formats missing:**
  - Other track layouts: `.nrg`, `.mdf`/`.mds`, `.ccd`/`.img`/`.sub` and
    `.chd`: readers giving the same table of contents as the cue sheets'
    (`src/cdimage.c`); cue sheets with compressed audio (FLAC, MP3, Ogg),
    decoded as read.
  - Compressed images: UDIF `.dmg` (zlib, bzip2, ADC or LZFSE chunks,
    read-only, decompressed as read) and Disk Copy 6 NDIF `.img` (whose map
    is in the resource fork, so only where the fork survived, or from
    MacBinary). Meanwhile, macOS's `hdiutil convert` makes a plain image.
- **CD audio, what's left:** recording from the CD (the codec's input DMA
  records silence even with the CD selected); de-emphasis for tracks
  flagged with pre-emphasis (played as stored); CD-Text (titles); a check
  of Mac OS 9's Audio CD Access and its AIFF tracks, and of games that play
  their soundtracks from the CD (AppleCD Audio Player, the Control Strip's
  CD module, Audio CD Access and QuickTime's import were tried on 8.5).
- **Size:** small to moderate: each format is a self-contained reader behind
  the CD-ROM drive.
