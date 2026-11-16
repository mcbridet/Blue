# Notes on programs and games

What particular programs and games needed, and how they were set up and
checked under Blue.

## Bugdom and Unreal Tournament

Both draw with the guest tools' [3D engine](guest-tools.md#blue-3d-engine);
Unreal Tournament's OpenGL build draws with Blue's
[OpenGL libraries](guest-tools.md#blue-opengl-library-memory-and-utility).

Bugdom's setup, for the record:
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
paced).

UT's OpenGL build (the application and `OpenGLDrv.ini` from the CD's
`Extras:OpenGL UT.sea`, in place of UT's application, and
`GameRenderDevice=OpenGLDrv.OpenGLRenderDevice` in `UnrealTournament.ini`)
draws with Blue's OpenGL libraries, on 8.6 and on 9.2.2: the same
pictures as with RAVE, at 60 frames a second on the GPU and about 50 in
software (on 8.6, an M5 Mac).

## MacBench

ZD MacBench 5.0 runs (tested from a shared folder, on an installed 8.5):
it registers, opens its main window and runs its Processor and Floating
Point tests (the others are untried). Right after registering it uses
Gary Davidian's skeleton key, a kernel call that gives a program
supervisor mode, to read the processor's L2CR and HID0 (see the kernel
calls and privileged instructions in [architecture.md](architecture.md#the-kernel)); without it, MacBench quit with
"an error of type 1". Its scores are relative to its reference machine, a
Power Macintosh G3/300 (1000 in every test). Benchmark in real time, in a
window: in instruction time, guest time follows the instruction count, so
the guest processor looks like a 16.6 MIPS one (Processor: 62, 6% of the
G3/300); in one real-time run on an M5, with other work on the host,
Processor scored 18,324 (the test took about 25 s). MacBench times things
by polling the VIA's timer in a tight loop; in real time, a loop whose
device register read fell on the last instruction before a sync point
kept Blue from ever reaching one (guest time stood still, and the window
froze); it no longer does. Its Floating Point test runs for about 50 s
of guest time (five parts of about 10 s that count iterations: an FFT, an
LU decomposition, MathLib's functions, NaNs and infinities among their
arguments, a table of log factorials from MathLib's log and exp,
arithmetic on special values); the score weighs them like a harmonic
mean, so the log factorials (60% of it on Blue) count most. Headless in
real time on an M5 (`--realtime`, a script in seconds; medians of 4 runs,
interleaved) it scored 7,085 before the JIT's [floating-point work](jit.md#floating-point)
and 16,616 after (in a later session, 6,250 and 14,700: the host's speed
varies), while the Processor test's code runs as fast as before; see
Findings in [plan.md](plan.md#findings).

Its CD-ROM tests run with the MacBench 5.0 CD (`MacBench5.toast`) in a
CD-ROM drive, on an installed 8.5 and 9.2.2: the CD-ROM test (148 MB of
File Manager reads replayed from five CD titles; 1,442 on 8.5 in
instruction time, 4,261 on 9.2.2 with `clock 400`, 5,366-5,383 in real
time, a window or not) and the CD-ROM inspection tests (sequential and
random reads of 512 bytes and 1 MB). They read through the File Manager
only; below it MacBench just takes an inventory of the drives (see
Findings in [plan.md](plan.md#findings), "MacBench's CD-ROM test"). A report of the test
freezing MacBench on 9.2.2 in a window (the Memory Manager faulting on a
broken system heap, then the Toolbox) didn't reproduce here.

## Pippin titles

Pippin titles that link to the Pippin's own library need Blue Pippin
Library, one of the guest tools: see
[guest-tools.md](guest-tools.md#blue-pippin-library).

Super Marathon (Bungie, 1996: Marathon and Marathon 2 for the Pippin)
plays from its disc on Mac OS 8.5 and 9.2.2 installed on disks, with the
library installed. Open `M1:marathon.ppc` or `M2:marathon2.ppc`; the
Pippin's menu offers Begin New Game, Continue Saved Game, Delete Saved
Game, Preferences and Quit. The Pippin builds don't save at pattern
buffers: they ask "Save game as:" when a level ends, and the saves land
in `Preferences:Pippin:marathon.ppc` or `marathon2.ppc`; Continue Saved
Game opens them there, and Quit goes back to the Finder. The keyboard
works: in Marathon 2 the up arrow moves forward, Space fires, Tab acts (terminals),
Return pages through a terminal, Command-Q asks to end the game; Marathon
1 took Tab and Return held for a second in scripted tests, and its
movement keys weren't found there (the Preferences screen sets them). The
mouse works in the menus and dialogs. The Pippin's AppleJack controller
isn't emulated (its buttons are keys: the titles' `pipp` resources map
them onto the keyboard).

"1995.1.17 Hanshin Daishinsai" (a Macromedia Director projector, `JISIN`,
68k, without PippinPPC) runs on 8.5 and 9.2.2: open its disc (its name is
Japanese, shown in Mac OS Roman without a Japanese system; typing "c" in the
Finder selects it), then JISIN. It shows its title, then a photo at a
time, the next at a click. A scripted click doesn't select a desktop icon
(see [plan.md](plan.md#findings), Findings), which is why opening the disc that way seemed to
do nothing.
