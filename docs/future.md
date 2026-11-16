# Future features

Ideas worth doing some day, beyond the [roadmap](roadmap.md). Each says
what the real iMac had, what it would be good for under Blue, and what it
would take.

The modem needs something first: a serial controller that carries
data. Blue's Z8530 SCC (`src/scc.c`) has
nothing attached today: what Mac OS sends vanishes, the receiver always sees
an idle line, and it raises no interrupts. It would need data both ways, its
interrupts (receive, transmit, status changes), the handshake and
carrier-detect lines the drivers watch, timing at the chosen bit rate, and
perhaps Mac I/O's DMA channels for the SCC if Mac OS's serial driver uses
them on the iMac. Runs that use the network or a host device stay real-time
only, as with Ethernet, with input-script commands for reproducible tests.

## Modem

- **The iMac's:** a built-in 56K modem (V.90 and K56flex; fax at 14.4K) that
  "appears to the system as a serial port that responds to the typical AT
  commands" (Apple's iMac Developer Note). Mac OS has a modem script for it
  ("iMac Internal 56K"), so the Modem control panel, Remote Access and
  serial programs find it as they would on the real machine.
- **What for:** not the Internet (Ethernet already reaches it, far faster),
  but software that only knows modems: terminal programs such as ZTerm
  calling telnet BBSes (many still run), head-to-head modem games of the
  90s between two Blue windows or over the Internet, and old dial-up
  clients.
- **How:** as DOSBox and 86Box do, the modem talks over TCP. `ATDT
  host:port` (or a number from a small phonebook) connects there and answers
  `CONNECT 56000`; hanging up closes the connection. An option such as
  `--modem-listen PORT` makes an incoming connection ring, for `ATA` or
  auto-answer (S0), so two Blue instances can call each other. The usual
  Hayes behaviour: the command set Mac OS's script sends, `+++` with its
  guard time, result codes, and carrier detect following the connection.
- **For fun:** the real modem fed its speaker (dialling, the handshake) into
  the sound chip; Blue could play that while connecting.
- **Size:** moderate, nearly all of it in the serial controller; the modem
  and TCP parts are small.

## Sound input (microphone and line in)

- **The iMac's:** a built-in microphone above the screen and a stereo
  sound-in jack, both into the sound chip's input, chosen in Monitors &
  Sound (Sound control panel in Mac OS 9).
- **What for:** recording in Mac OS's own programs (SimpleSound, alert
  sounds, sound editors), PlainTalk speech recognition and Speakable Items
  with the host's microphone, and games or programs that listen.
- **Where Blue is:** the codec's input DMA channel (9) runs at the right
  pace but records silence, and the sound node declares output hardware
  only (`hardware-types` 2), because Mac OS 9's AWACS input component
  ('sind' 'awgc') has a bug in its close routine that corrupts the system
  heap at startup ([bring-up.md](bring-up.md#mac-os-9), Mac OS 9). Declaring input again brings that
  back, so that comes first. Options: declare input only for systems
  without the bug (8.5 and 8.6 use another sound stack; check which have
  it), make the bug harmless (a RAM patch of the component, with the
  injection the guest-tools installer brings), or give Mac OS 9 its input
  another way, as an emulated USB audio device on Blue's USB controller
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

- **The iMac's:** none. Its PowerPC 750 (G3) has no vector unit, and Blue
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
  and Mac OS 9.1's own BlockMove 2-4 times faster or more. Blue already
  runs scalar code at about 2.5 billion instructions a second (about ten
  times the iMac, in the range of the fastest G4s that ran Mac OS 9 before
  their AltiVec), so this would mostly speed up vector-heavy work.
- **Identity:** no G3 iMac had AltiVec. Either Blue reports a G4 (a 7400
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
    compares, shifts, vperm with TBL; ARMv8.2's dot products (UDOT, SDOT)
    could speed up the multiply-sums (vmsum*) where the host has them, with
    ARMv8.0 still the baseline. On x86-64 the JIT's baseline (x86-64-v3)
    has the same in SSE and AVX2: saturating adds, PSHUFB for vperm, FMA3
    for vmaddfp. Vector registers in the register cache; the fuzz tests
    cover them.
  - The kernel: vector registers saved and restored with contexts and MP
    tasks (VRSAVE to skip unused ones), the AltiVec Unavailable exception,
    under both the NanoKernel 1 and 2 interfaces.
  - Hosts without a JIT (x86-64 processors older than x86-64-v3) would run
    AltiVec slowly; one more reason for an option.
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
- **How:** Blue's network gateway (10.0.2.2) answers LPR (port 515);
  Mac OS's LaserWriter 8 sends it PostScript, and Blue writes the job out
  as a PDF in a folder (an option, the Desktop by default) with
  Ghostscript's pdfwrite (found at run time; recent macOS no longer
  converts PostScript itself), or hands it to the host's printing system
  (`lp`: CUPS, on macOS and Linux). Without Ghostscript the PostScript file
  is kept and Blue says so. The guest tools carry a PPD for a "Blue PDF
  Printer" (paper sizes, colour), and the install prompt could set up the
  desktop printer. Programs that draw with QuickDraw print too: LaserWriter
  8 turns QuickDraw into PostScript. The NAT has to accept connections to
  the gateway's own address for this. Real-time runs only, as for all
  networking.
- **Alternatives:** a LaserWriter on AppleTalk (PAP) at the gateway, which
  needs AppleTalk to get past the NAT (only IP does today), or a printer
  driver of Blue's own in the Chooser (far more work).
- **Size:** moderate: an LPD listener in the NAT, the job handling, the PPD.

## Game controllers

- **Where Blue is:** the host's pads are the Mac's keyboard and mouse, a
  Pippin title's AppleJack, and game controllers of their own in games
  that use InputSprocket ([input.md](input.md)).
- **What's left:**
  - Mappings of one's own for the keyboard and mouse, kept between runs
    (with the launcher).
  - Two AppleJacks, for two-player Pippin titles.
  - The scroll wheel as scrolling (the guest tools could scroll the window
    under the pointer) rather than arrow keys.
- **Size:** small.
