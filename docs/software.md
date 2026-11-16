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

A game controller plays UT through InputSprocket (with Blue Game
Controllers, on 8.6 with InputSprocket 1.4.1, as UT installs it): the
left stick runs and slides, the right turns and looks, the triggers fire,
A jumps, and Start is Esc, for its menus; Options, Input Device shows the
pad in InputSprocket's dialog (see [input.md](input.md#inputsprocket)).
It wants its CD in the drive. A scripted run plays it in instruction
time: from the intro, Tab and `open DM-Tempest` (its console) start a
practice match, and `clock 300` makes it fast enough to play (see
[findings.md](findings.md), "Unreal Tournament's needs").

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
[findings.md](findings.md), "MacBench's Floating Point test".

Its CD-ROM tests run with the MacBench 5.0 CD (`MacBench5.toast`) in a
CD-ROM drive, on an installed 8.5 and 9.2.2: the CD-ROM test (148 MB of
File Manager reads replayed from five CD titles; 1,442 on 8.5 in
instruction time, 4,261 on 9.2.2 with `clock 400`, 5,366-5,383 in real
time, a window or not) and the CD-ROM inspection tests (sequential and
random reads of 512 bytes and 1 MB). They read through the File Manager
only; below it MacBench just takes an inventory of the drives (see
[findings.md](findings.md), "MacBench's CD-ROM test"). A report of the test
freezing MacBench on 9.2.2 in a window (the Memory Manager faulting on a
broken system heap, then the Toolbox) didn't reproduce here.

## Pippin titles

Pippin titles that link to the Pippin's own library need Blue Pippin
Library, one of the guest tools: see
[guest-tools.md](guest-tools.md#blue-pippin-library).

Pippin discs run inside Mac OS, from a CD-ROM drive (`blue boot --disk
hd.img --cdrom "Super Marathon (USA).cue"`): a Pippin's system is the
console's (its ROM, and the System Folder on the disc, with the `Pippin`
enabler, type `gbly`), and Blue starts Mac OS from a Mac OS ROM file,
which a Pippin disc hasn't. When `boot` finds no Mac OS startup disk and
one of the images is a Pippin disc (a `PippinAuthenticationFile` at its
root, or that enabler in its blessed System Folder: `startup_pippin()` in
`src/startup.c`), it says so by the disc's name, in the terminal and over
a black screen in the window (with `--headless`, only in the terminal),
with the command to copy: the arguments given with `--disk <your Mac OS
disk>` before the drives, the disc in a CD-ROM drive. It exits with
status 1 after Quit. With no startup disk and no Pippin disc, the same
window says why (no disks or CDs, or none with a System Folder holding a
Mac OS ROM file) and points to the README's Quick start.

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
1 took Tab and Return held for a second in scripted tests. The mouse
works in the menus and dialogs.

A game controller plays it as the Pippin's AppleJack, with the title's
own mappings (see [guest-tools.md](guest-tools.md#blue-pippin-library)):
in the menus the right stick moves the pointer and the triggers and
shoulders click; in the game the D-pad moves (O, K, ; and L: the keys
the game's own settings have, which is how Marathon 1's movement keys were
found), A, B, X and Y are Shift, Tab, Space and Option, the shoulders and
triggers Z and X, and Back, Guide and Start Command-Q, Control and P. An
instruction-time run on 8.5 (a disk with the guest tools, the disc's cue
sheet in a CD-ROM drive) opened the title with a scripted pad (Back and
the D-pad's down, Command-Down Arrow, in the Finder), moved the pointer
with its right stick and began a game with its right trigger, ended the
chapter screen with X, then walked forward with the D-pad's up and turned
with its left:

```
0 pad 0 plug Test Pad
4000000000 key 01                       # "super", the disc ...
4200000000 pad 0 button back down       # ... opened: Command-Down Arrow
4210000000 pad 0 button dpdown down
4220000000 pad 0 button dpdown up
4230000000 pad 0 button back up
...                                     # M1, then marathon.ppc, the same way
6400000000 clock 300
11000000000 pad 0 axis rightx 0.8       # the pointer, by the trackball
11000000000 pad 0 axis righty 0.5
11300000000 pad 0 axis rightx 0
11300000000 pad 0 axis righty 0
11900000000 move 250 310                # on Begin New Game
12000000000 pad 0 axis righttrigger 1   # the right fire button: a click
12100000000 pad 0 axis righttrigger 0
15500000000 pad 0 button x down         # the chapter screen ended
15600000000 pad 0 button x up
16100000000 pad 0 button dpup down      # forward (O), for two seconds of guest time
16700000000 pad 0 button dpup up
16900000000 pad 0 button dpleft down    # a turn to the left (K)
17100000000 pad 0 button dpleft up
```

The AppleJack isn't emulated as an ADB device: the library stands in for
its driver. Two AppleJacks, for two players, aren't there either: every
pad is the one AppleJack.

"1995.1.17 Hanshin Daishinsai" (a Macromedia Director projector, `JISIN`,
68k, without PippinPPC) runs on 8.5 and 9.2.2: open its disc (its name is
Japanese, shown in Mac OS Roman without a Japanese system; typing "c" in the
Finder selects it), then JISIN. It shows its title, then a photo at a
time, the next at a click. A scripted click doesn't select a desktop icon
(see [findings.md](findings.md), "Desktop icons from scripts"), which is why opening the disc that way seemed to
do nothing.
