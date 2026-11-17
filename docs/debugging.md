# Testing and debugging

Blue can run Mac OS the same way every time, down to the instruction,
which makes it a tool for testing Blue and for finding out what Mac OS
does. This document covers instruction time, scripted input, the
debugging options of `blue boot`, the `BLUE_*` environment variables and
the tools in `tools/`. Building and the unit tests are in
[building.md](building.md); notes on particular programs (MacBench among
them) in [software.md](software.md).

## Instruction time and real time

`blue boot` runs the Mac in a window, in real time: guest time follows
the host clock, as on a real Mac. With `--headless` there's no window, and
guest time is counted in instructions (one timebase tick each), unless
`--realtime` is given too; `--instruction-time` keeps instruction time in
the window. `--seconds S` stops after S seconds of host time, and `--max N`
after N instructions (500 million by default with `--headless`; no limit
in a window). With `--fixed-time <unix time>` the clock doesn't depend on
the host either, so a run is exactly reproducible, including faults at a
given instruction count:

```
build/blue boot --headless --rom MOS.rom --cdrom 85.iso --max 4000000000 --fixed-time 1000000000 --screenshot finder.png
```

The boot reaches the Finder within about 3 billion instructions (three
minutes of guest time; a second or so on the host with the JIT: see
[Speed](jit.md#speed)). Without a display to open a window on (SSH, a
container, CI), `blue boot` stops and says to use `--headless`.

## Input scripts

`--input FILE` replays input at given instruction
counts (`build/blue boot --headless --cdrom 85.iso --rom MOS.rom --input
script.txt --max 5000000000`), for example:

```
3350000000 key 24                # Return (ADB key code, hex)
3400000000 dblclick 751 52       # open the disk
3500000000 motion 40 -10         # the mouse moved 40 right, 10 up (through the ADB mouse, as games read it)
3550000000 wheel 0 -3            # the scroll wheel turned 3 lines down (0.25: a quarter of a line up)
3600000000 screenshot after.png
3700000000 clock 800             # guest time at 800 million instructions a second from here
3800000000 power                 # the keyboard's power key: Mac OS asks whether to shut down
0 tools install                  # the answer to the offer to install the guest tools (or "later", or "never": Don't Ask Again): Mac OS then restarts
5000000000 tools offer           # the offer, now (the menu's Install Guest Tools...)
3900000000 floppy insert Disk 2.img
3950000000 cdrom insert Game CD 2.iso
4000000000 clipboard Olé “two”\nlines   # the host's clipboard has this text now (\n: a newline)
4100000000 print-clipboard       # prints the host's clipboard, with what Mac OS copied since
4200000000 share file Added.txt From the host.\n
4300000000 share refresh         # as the menu's Refresh: Mac OS puts the shared folder's disk away, and it's offered again
```

(`floppy insert` and `cdrom insert` take the rest of their line as the
image's path, comments included; `floppy eject` and `cdrom eject` take one
out: see [drives.md](drives.md#removable-drives). `share file` writes a
file in the shared folder, the rest of its line, as if on the host.)

The commands, one a line after the instruction count (lines starting
with `#` are comments):

| Command | What it does |
|---|---|
| `move X Y` | moves the mouse to (X, Y) on the screen |
| `click X Y`, `dblclick X Y` | moves there, then clicks or double-clicks once the cursor has followed (a press and its release 3 million instructions apart) |
| `press X Y`, `down`, `up` | moves and presses the button; presses it; lets it go |
| `motion DX DY` | the mouse moved, as games read it (through the ADB mouse) |
| `wheel DX DY` | the scroll wheel turned, lines (or parts of one) right and up, as a trackpad's or a mouse's: Blue Enhancements scrolls the window under the pointer, or the arrow keys are pressed (see [guest-tools.md](guest-tools.md#the-scroll-wheel)) |
| `key HEX [down\|up]` | an ADB key code, pressed and released, or only pressed or let go |
| `power` | the keyboard's power key (Mac OS asks whether to shut down) |
| `resize W H` | as if the window were resized (for Blue Enhancements) |
| `screenshot FILE` | saves the screen as a PNG |
| `floppy insert FILE`, `floppy eject`, `cdrom insert FILE`, `cdrom eject` | swap discs |
| `tools install`, `tools later`, `tools never`, `tools offer` | the guest tools' offer: answered, or made now |
| `clipboard TEXT`, `print-clipboard` | the host's clipboard, stood in for |
| `clock N` | from here, guest time as for an N MIPS processor |
| `share eject`, `share refresh` | Mac OS puts the shared folder's volume away (whatever it has open on it, or as the menu's Refresh: not while files are open on it), and it's written back and offered again (see [shared-folder.md](shared-folder.md#put-away-and-offered-again)) |
| `share file NAME TEXT` | the shared folder has NAME holding TEXT (the rest of the line, as for `clipboard`), as if the host had written it |
| `pad N plug [NAME]`, `pad N unplug` | game controller N (0 to 3) plugged in or out |
| `pad N button NAME down\|up` | its button pressed or let go, by SDL's name: `a`, `b`, `x`, `y`, `back`, `guide`, `start`, `leftstick`, `rightstick`, `leftshoulder`, `rightshoulder`, `dpup`, `dpdown`, `dpleft`, `dpright`, `misc1`, `paddle1` to `paddle4`, `touchpad` |
| `pad N axis NAME VALUE` | its stick or trigger moved, from -1 to 1 (a trigger's from 0): `leftx`, `lefty` (down positive), `rightx`, `righty`, `lefttrigger`, `righttrigger` |
| `modem ring` | the script's far end calls the modem: it rings, for `ATA` or S0 |
| `modem pickup` | the script's far end answers the next number dialled (`CONNECT 56000`), rather than nobody (`NO CARRIER`) |
| `modem recv TEXT` | the far end sends TEXT (the rest of the line: `\r`, `\n`, `\\` and `\xHH` stand for a CR, a newline, a backslash and any byte) |
| `modem hangup` | the far end hangs up |
| `mic play FILE`, `mic stop` | the microphone hears a WAV file (the rest of the line), once from its start, as Mac OS records; or nothing more |
| `usb connect FILE`, `usb disconnect [PORT]` | the USB test disk, with the image FILE (the rest of the line), plugged into a free USB port; unplugged (the test disks, or the device in port 1 or 2) |

Times can be seconds of host time since the start instead, written with
an `s` (`12.5s click 100 200`), for scripts in real time (in the window,
or `--headless --realtime`);
a script's times are either all instruction counts or all seconds.

`clipboard` and `print-clipboard` stand in for the host's clipboard (a
scripted run leaves the window's alone), for Blue Enhancements to share
(see [guest-tools.md](guest-tools.md#the-clipboard)): the text is as if copied on the host with the window
then brought to the front.

`pad` plays a game controller (see
[input.md](input.md)): in a window, through one of
SDL's virtual game controllers (SDL 2.0.14 and later), as a real pad
would; otherwise, or without them, straight to the pads' mapping. A
scripted run ignores the host's own pads. A stick that's the mouse moves
it as guest time passes, so the same script moves it as far each time.
Each pad is numbered, so `pad 1 ...` is a second pad: with two plugged
in, a Pippin title has two AppleJacks, the second pad the second (see
[input.md](input.md#a-pippin-titles-applejacks)). A scripted run uses
Blue's mappings from a settings file only if given one (`--settings
FILE`, see [input.md](input.md#the-settings-file)); a window's pads have
SDL's GUID for a virtual controller, and a run without a window's only
their names (`pad 0 plug Test Pad`: a `[controller Test Pad]` section).

`wheel` turns the scroll wheel: with Blue Enhancements running (and its
patches in: a script with a `wheel` command has them, as one with
`clipboard` has), the lines wait for the application in front, which a
key wakes, to scroll by; otherwise they're the arrow keys at once.

`modem` plays the internal modem's far end (see
[devices.md](devices.md#the-modem)), so a call can be tested the same way
every time: in instruction time the modem has no TCP, and dialling gives
`NO CARRIER` unless the script picked up. What Mac OS sends to the
script's far end is printed (`modem: the far end got "..."`, with the same
escapes), and a script with `modem` commands has the modem in instruction
time too. For example, Remote Access dialling with a scripted far end:

```
0 modem pickup                  # the next call is answered
7300000000 click 486 368        # Remote Access's Connect, at 800x600
8000000000 modem recv ~\xFF}#\xC0!}!}!} }*}%}&}24Vxy} ~
8500000000 modem hangup
```

(`modem recv` takes the rest of its line, so a comment can't follow it: the
line above is an LCP Configure-Request.)

`mic play` stands in for the microphone (see [Sound](devices.md#sound)), as
`--mic-file` does for a whole run (over and over): the file is read only as
Mac OS records from the microphone or sound-in, so a recording comes out
the same each time. `BLUE_SOUND_IN_WAV=FILE` writes what Mac OS got, and
`BLUE_SOUND_WAV=FILE` what it played, to compare with the file. For
example, with SimpleSound open on an installed 8.5 at 800x600 (the counts
are a particular run's), it records five seconds of the file as a new
alert sound and plays it back:

```
5900000000 click 76 188         # SimpleSound's Add...
6200000000 click 272 170        # Record
6400000000 click 310 170        # Stop
6500000000 click 383 170        # Play
```

`clock N` keeps a run reproducible while making guest time pass as on a
guest processor of N MIPS, so that work paced by guest time (sound, timer
tasks, a game's frame timing) is in real-time proportions: one timebase
tick per instruction is 16.6 MIPS, which makes such work look 50 times
bigger than it is. A scripted click's press and release are 3 million
instructions apart, which at a high `clock` can be too short for the guest
to notice; change the clock after the clicks that matter.

## The USB test disk

`--usb-test-disk FILE` plugs a USB mass storage device into one of the
Mac's USB ports at startup, its disk the image FILE (read and written in
512-byte blocks; read-only if the file is), and `usb connect FILE` and
`usb disconnect` in an input script plug one in and out at a given
instruction count. It's built into Blue (`src/usbmsd.c`: bulk-only
transport, SCSI) and answers at once, so it works in instruction time,
where runs with it are reproducible: Mac OS's own USB stack, through the
ROM's OHCI driver and its mass storage driver, mounts it as a removable
disk. It's for testing the USB controller (see
[devices.md](devices.md#usb)), not a way to attach disks (`--disk` is
that). With Mac OS 9.2.2 installed on `hd.img`, for example:

```sh
blue disk create usb.img 20M "USB Test"
blue boot --headless --disk hd.img --usb-test-disk usb.img --input usb.txt --max 4400000000 --fixed-time 1000000000
```

with a script `usb.txt` that opens the disk, duplicates a file on it and
ejects it (the icon where Mac OS 9.2.2 puts the first removable disk, at
800 × 600):

```
3000000000 click 750 182          # select USB Test
3050000000 key 37 down            # Command-O: open it
3060000000 key 1F
3070000000 key 37 up
3350000000 key 04                 # select the file starting with H
3400000000 key 37 down            # Command-D: duplicate it
3410000000 key 02
3420000000 key 37 up
3850000000 click 750 182
3900000000 key 37 down            # Command-E: eject the disk
3910000000 key 0E
3920000000 key 37 up
4300000000 screenshot ejected.png
```

The host's devices go through libusb (`src/usbhost.c`); the tests run that
path with a stand-in for libusb (`build/libusb-mock.so`, from
`tests/libusb_mock.c`) whose one device is a test disk, its transfers
answered on libusb's thread a millisecond or more later. `BLUE_LIBUSB`
loads it in place of the real one, `BLUE_LIBUSB_MOCK_IMAGE` names its
disk's image, and creating a file named as that image with `.unplugged`
added unplugs it (removing it plugs it in again):

```sh
BLUE_LIBUSB=build/libusb-mock.so BLUE_LIBUSB_MOCK_IMAGE=usb.img \
	blue boot --headless --realtime --disk hd.img --usb 1209:0001 --seconds 70 --max 900000000000
```

On macOS, `BLUE_LIBUSB_MOCK_HELD=1` makes the stand-in's disk one the
host's driver has, as macOS's would: Blue can't take it, and only its USB
helper (`blue-usb-helper`) can, as root could. `BLUE_USB_HELPER_TEST=1`
starts the helper as you, without macOS's password dialog, so the whole
way through the helper runs without root:

```sh
BLUE_LIBUSB=build/libusb-mock.so BLUE_LIBUSB_MOCK_IMAGE=usb.img BLUE_LIBUSB_MOCK_HELD=1 \
	BLUE_USB_HELPER_TEST=1 blue boot --headless --realtime --cdrom 922.iso --usb 1209:0001 --seconds 60
```

## When devices are serviced

Devices and interrupts are serviced every 20,000 instructions and at kernel
exits, never at exits made only for debugging, so tracing and trap logging
don't change what the guest does. (At exits they are updated only when that
could change anything: when a VIA timer runs out or a USB frame starts with
its interrupt enabled, while sound DMA moves, or after something touched
them. Kernel calls made in place, without an exit, and device register
accesses made in place update devices as their exits would. An access on
the last instruction before a sync point still exits, and the loop then
services that sync point.) Tracing and trap logging use the
interpreter.

## Tracing and logging

Debugging `boot`: `--trace-from N --trace-reg 24` logs instructions with the
68k PC (the emulator keeps it in `r24`), `--watch <addr>` logs every write
the guest makes to a page (the kernel's own writes there, such as disk
reads, go through unseen, so a watched run goes as any other), `--traps N
[--traps-after M]` logs 68k Toolbox/OS traps, interpreted or recompiled
(see [Idle](architecture.md#time); with the driver name for `_Open`), `--log-hostcalls` logs the native drivers'
requests (a CD-ROM drive's calls with their parameters in and out, and what it plays), `--dump-ram FILE` saves RAM at the end, `--max N` limits the run.
`--trace-range LO:HI [--trace-insn WORD]` logs only instructions in an
address range (optionally only one instruction word), with instruction
counts and r3-r5/LR, cheaply enough for whole boots. `BLUE_LOG_SOUND=1`
logs the sound chip's register accesses, and `BLUE_SOUND_WAV=FILE` records
what it plays (also in instruction time; CD audio included).
`BLUE_LOG_AUDIO=1` prints, every second while sound plays in real time,
how the host's sound queue fared (its depth, how much faster or slower it
played, chunks dropped, silence added) and what paces it (the frames the
guest's sound DMA gave, how far guest time is ahead of the host clock, the
disks' transfers and the guest time they took, how much the host's sleeps
overran). `BLUE_WINDOW_PROFILE=1` prints, every second in a window, the
frames the Mac handed the window and how much of the screen changed in
them, and where showing them took the host's time: on the Mac's thread
(which stops the Mac meanwhile: finding and copying the changes, taking
the input) and on the window's (converting, uploading, drawing,
presenting, events), in milliseconds a frame of the host clock and of the
thread's processor time, the pages seen written a frame, and how long after
the hand-over a frame was on the screen. `BLUE_WINDOW_COMPARE=1` finds the
changes by comparing the whole screen, without watching the framebuffer;
`BLUE_WINDOW_CHECK=1` compares the watched pages too, and says where a
write went unseen. `BLUE_LOG_USB=1` logs USB's transfers (each TD
retired, each request a device was given, the test disk's SCSI commands,
the host's devices' answers), `BLUE_LOG_USB=2` the controller's register
accesses too (not the frame number, which drivers poll),
`BLUE_LOG_PCI=1` configuration space accesses, `BLUE_LOG_LIBS=1` the file
each run-time library (SDL2, libslirp, libusb) was loaded from.
`--io-ram` backs unmodelled I/O pages with plain RAM. Under gdb, `handle
SIGSEGV nostop noprint pass` lets the JIT's memory faults through; under lldb
on macOS, `settings set platform.plugin.darwin.ignored-exceptions
EXC_BAD_ACCESS` (before `run`) and `process handle -s false -n false -p true
SIGBUS` (or set `BLUE_JIT_FASTMEM=0`).

## The ROM and other commands

Examples of `blue`'s commands, as a developer uses them (`build/blue` is
what `make` builds; `MOS.rom` and the CD images are your own, see
[layout.md](layout.md#files-you-supply)):

```
build/blue rom info MOS.rom
build/blue rom info 91.iso                      # the Mac OS ROM file in a disk's System Folder
build/blue rom config MOS.rom                   # ConfigInfo and user address space
build/blue rom parcels MOS91.rom                # a Mac OS 9.1 ROM file's parcels
build/blue rom extract MOS.rom build/toolbox.rom
build/blue rom trampoline MOS.rom build/trampoline.elf
build/blue boot --rom MOS.rom --cdrom 85.iso            # interactive, in a window
build/blue boot --headless --rom MOS.rom --cdrom 85.iso --max 3000000000 --screenshot finder.png
build/blue boot --rom MOS.rom --cdrom 85.iso --share ~/Mac    # with a shared folder
build/blue boot --rom MOS.rom --cdrom 85.iso --floppy disk1.img --floppy disk2.img   # a floppy drive
build/blue boot --disk hd.img                              # with empty CD-ROM and floppy drives: drop images on the window
build/blue boot --disk hd.img --cdrom "Game.cue"           # a CD with audio tracks
build/blue disk toc "Game.cue"                             # its tracks, as its CD-ROM drive gives them
build/blue disk create blank.img 1440K "Untitled"          # a blank floppy
build/blue run-raw code.bin 1000                # BLUE_TRACE=1 logs every instruction
```

## Environment variables

Blue reads these from the environment. Those that take no value are on
when set (to anything; `BLUE_GPU`, `BLUE_JIT_FASTMEM`, `BLUE_TOOLS_APPL`
and `BLUE_TOOLS_GL_TEST` look at the value: see below).

Blue was called syshm until November 2026, and its settings were
`SYSHM_*`: if one of the old names is set, Blue says what it's called now,
and doesn't read it.

**`blue`:**

| Variable | What it does |
|---|---|
| `BLUE_DEVICE_EXITS=1` | every device register access leaves generated code, as before [device registers in place](jit.md#device-registers-in-place), for comparisons |
| `BLUE_GL_FPS=1` | prints the OpenGL libraries' frame rate every second |
| `BLUE_GL_PROFILE=FILE` | writes a line to FILE for each OpenGL frame shown, with where its host time went ([guest-tools.md](guest-tools.md#blue-opengl-library-memory-and-utility)) |
| `BLUE_GL_TEST_DEPTH=16` | Blue GL Test's screen depth, 16 or 32 (the default) ([building.md](building.md#testing-the-guests-opengl)) |
| `BLUE_GL_TEST_SCENE=NAME` | the scene Blue GL Test draws |
| `BLUE_GPU=0`, `BLUE_GPU=1` | 0: the 3D engine and OpenGL draw in software, as with `--no-gpu`; 1: with OpenGL in any run, instruction time and software renderers such as Mesa's llvmpipe included |
| `BLUE_GPU_DEBUG=1` | the GPU backend says what its shader compiler says, and more |
| `BLUE_GPU_FAIL=N` | the GPU backend fails at its Nth job, to check that drawing goes on in software |
| `BLUE_JIT_FAR=1` | (tests) puts translated blocks out of short branch range of the JIT's stubs |
| `BLUE_JIT_FASTMEM=0` | the JIT checks every load and store rather than using [fastmem](jit.md#memory-fastmem) |
| `BLUE_JIT_HIDE=LIST` | (tests, x86-64) processor features to take as missing, comma-separated |
| `BLUE_JIT_HOT=N` | how many times a block runs before it's translated (16; at most 255) |
| `BLUE_JIT_PROFILE=1`, `=FROM:TO` | samples where host time goes, and prints it at the end ([jit.md](jit.md#profiling)) |
| `BLUE_LOG_AUDIO=1` | how the host's sound queue fares, every second ([above](#tracing-and-logging)) |
| `BLUE_LOG_GL=1` | logs every OpenGL call |
| `BLUE_LOG_LIBS=1` | the file SDL2, libslirp and libusb were loaded from |
| `BLUE_LIBUSB=FILE` | loads this libusb-1.0 rather than Blue's own or the system's (the tests' stand-in for it: `build/libusb-mock.so`); never in the USB helper running as root |
| `BLUE_USB_HELPER_TEST=1` | (macOS) starts the USB helper as you, without asking for a password (it can then take only what you could: for the tests, with the stand-in's `BLUE_LIBUSB_MOCK_HELD`; see [above](#the-usb-test-disk)) |
| `BLUE_LOG_MODEM=1` | the modem's commands, results and calls |
| `BLUE_LOG_MP=1` | Multiprocessing Services' calls, task switches, timers and software interrupts |
| `BLUE_LOG_NET=1` | the Ethernet driver's calls |
| `BLUE_LOG_PADS=1` | the game controllers' buttons and axes, and the mapping each went to; InputSprocket's calls to Blue Game Controllers, and each of a game's needs ([input.md](input.md#testing)) |
| `BLUE_LOG_PCI=1` | PCI configuration space accesses |
| `BLUE_LOG_RAVE=1` | every call to the 3D engine |
| `BLUE_LOG_SERIAL=1` | the serial controller's register accesses (the modem's channel), and its DCD, CTS, DTR and RTS |
| `BLUE_LOG_SHARE=1` | Mac OS's writes to the shared folder |
| `BLUE_LOG_SOUND=1` | the sound chip's register accesses, and the codec's registers as they're written |
| `BLUE_LOG_USB=1` | USB's transfers and requests (`2`: the controller's register accesses too, not the frame number) |
| `BLUE_LOG_WHEEL=1` | where each of the scroll wheel's turns went: the scroll bar clicked, the lines its action procedure scrolled, or why not ([guest-tools.md](guest-tools.md#the-scroll-wheel)) |
| `BLUE_NET=1` | connects the Ethernet in instruction time too (then not reproducible) |
| `BLUE_PERF=1` | the host processor's counters for the emulation thread, every second (Linux) |
| `BLUE_RASTER_THREADS=N` | the software rasteriser's threads, counting the emulated processor's (3, or fewer processors if there are; 1 to 8) |
| `BLUE_RAVE_FPS=1` | prints the 3D engine's frame rate every second |
| `BLUE_SOUND_IN_WAV=FILE` | records what Mac OS records (the codec's frames input DMA delivers), as a WAVE file |
| `BLUE_SOUND_WAV=FILE` | records what the sound chip plays, CD audio included, as a WAVE file (in instruction time too) |
| `BLUE_TOOLS_APPL=1` | `tools create` makes the volume's Blue Enhancements an application, to try it from a CD's system |
| `BLUE_TOOLS_GL_TEST=1` | `tools create` puts Blue GL Test on the volume |
| `BLUE_TRACE=1` | `run-raw` logs every instruction |
| `BLUE_WINDOW_CHECK=1` | compares the watched framebuffer pages too, and says where a write went unseen |
| `BLUE_WINDOW_COMPARE=1` | finds the screen's changes by comparing all of it, without watching the framebuffer |
| `BLUE_WINDOW_PROFILE=1` | the window's frames and where their host time went, every second |

`BLUE_GPU=0` and `BLUE_JIT_FASTMEM=0` need the 0; `BLUE_GPU=1` the 1;
`BLUE_TOOLS_APPL` and `BLUE_TOOLS_GL_TEST` are on unless 0.

**`build/blue-tests`** ([building.md](building.md#tests)):

| Variable | What it does |
|---|---|
| `BLUE_GPU_DEBUG=1` | the GPU tests print the pixels that differ |
| `BLUE_JIT_BENCH=N` | the JIT's synthetic benchmark (N times 500 million instructions) instead of its test |
| `BLUE_JIT_PROGRAMS=N` | how many random programs the JIT test runs on both engines (3000) |
| `BLUE_JIT_SEED=N` | the first program's number |
| `BLUE_TEST_FB_DUMP=1` | prints the whole of a feedback buffer that differs from Mesa's |
| `BLUE_TEST_GLU=PATH` | Mesa's libGLU (`libGLU.so.1`), to compare OpenGLUtility's scene too (Linux) |
| `BLUE_TEST_GPU=1` | no OpenGL context is a failure, not a skip |
| `BLUE_TEST_JIT=1` | no JIT is a failure, not a skip |
| `BLUE_TEST_ROM=FILE` | the ROM file the ROM and startup tests use (else `./MOS.rom`, if there is one) |
| `BLUE_TEST_SAVE=DIR` | saves the OpenGL scenes as PNGs |
| `BLUE_TEST_VERBOSE=1` | says how far apart the OpenGL scenes are, and shows the shared folder test's checks (`hfsck`, `fsck`) |

**`tools/jit-timing.sh`:** `BLUE_ROM`, `BLUE_85_ISO`, `BLUE_91_ISO` and
`BLUE_922_ISO` say where the ROM file and the CD images are (else
`MOS.rom`, `85.iso`, `91.iso` and `922.iso` at the top of the tree).

## Tools

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

The rest of `tools/`:

- `tools/pef.py FILE [OFFSET]` inspects a PEF container (the Code
  Fragment Manager's format), such as the drivers and guest tools Blue
  generates, at an offset in a file.
- `tools/devtree.py FILE OFFSET` dumps a copy of the device tree in the
  format Mac OS reads (`NKHWInfo.DeviceTreeBase`), at an offset in a
  file such as a `--dump-ram` one.
- `tools/jit-timing.sh [runs]` builds Blue, runs the JIT's benchmark and
  the CD boots there are, and writes what it found to a log to send back
  (see [building.md](building.md#tests)).
- `tools/gltable.py` made `src/opengl_table.h` (see
  [building.md](building.md#testing-the-guests-opengl)).
- `tools/icons.py` and `tools/fonts.py` make the icons and the window's
  font (see [building.md](building.md#icons-and-desktop-files)).
- `tools/package-macos.sh`, `tools/macos-deps.sh`, `tools/package-linux.sh`
  and `tools/third-party-sources.sh` make the packages and the sources
  archive (see [building.md](building.md#packaging)).
- `tools/check-links.py` checks the links in the README and `docs/`.
- `tools/qemu-ref/`: see [qemu-reference.md](qemu-reference.md).
