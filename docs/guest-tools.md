# The guest tools

Some things only code inside Mac OS can do. Blue's guest tools are that
code: Blue Enhancements (the screen following the window, the
clipboard), Blue 3D Engine, Blue's OpenGL libraries and Blue Pippin
Library. This document describes how Blue offers and installs them, and
how each works. How to use them is in the
[README](../README.md#the-guest-tools).

## Offering and installing them

In a window, Blue offers to install them: before Mac OS
starts it looks in the startup disk's System Folder (its folders,
Extensions whatever its name and Extensions (Disabled) among them) for
them, and once the Finder is up, if one is missing or isn't this version's
(both forks compared with what it makes), a box over the screen says so,
with Install and Restart (or Update and Restart), Not Now and Don't Ask
Again (below); Mac OS runs on meanwhile. On Install, Mac OS writes the files itself (see
[findings.md](findings.md), "Running code of Blue's in Mac OS on demand"): each goes to a temporary file in the folder it belongs in, which
then takes the old copy's place, so an error (a full or locked disk) leaves
nothing half written; a disabled copy is updated where it is and stays
disabled. An older Blue Enhancements that's running is asked to quit
first (its file is in use while it runs). Mac OS opens its file some
seconds before it first runs (on 9.2.2 after the Finder is up), so if the
installer still finds it in use as it replaces it, the copy written is
deleted, Enhancements is asked to quit (it does as soon as it runs), and
the installer goes in again once it has. Once everything is in place,
Mac OS restarts as Special > Restart does (Blue's installer sends the
Process Manager the Restart Apple event): programs are asked to quit, and
can ask to save their work first, or stop the restart (the tools are in
place then, and load at the next restart). CDs and floppies stay in their
drives through the restart (see [drives.md](drives.md#removable-drives)). Afterwards the tools load as
any extension does: Blue Enhancements runs, programs find the 3D engine
and Blue's OpenGL, and nothing is offered. If anything goes wrong, Mac
OS doesn't restart, and the box says what. This works on Mac OS 8.5, 8.6,
9.1 and 9.2.2 installed on disk images. The outcome is shown (the box stays
through the restart, until OK) and printed:

```
tools: installed Blue Enhancements, Blue 3D Engine, Blue OpenGL Library, Blue OpenGL Memory, Blue OpenGL Utility and Blue Pippin Library in "Macintosh HD:System Folder:Extensions". Mac OS is restarting.
```

Mac OS 9 has OpenGL libraries of Apple's with the names Blue's have
(OpenGLLibrary, OpenGLMemory, OpenGLUtility), so only one set can be
enabled. Once Blue's are in Extensions, the installer moves Apple's to
Extensions (Disabled) and says so (the offer says it will). If Blue's
are gone (deleted by hand) while Apple's are disabled, the offer is to put
Apple's back (Put Back and Restart) instead of installing Blue's, and
Blue's aren't offered again for the rest of the run. Checked on 9.2.2:

```
tools: installed Blue OpenGL Library, Blue OpenGL Memory and Blue OpenGL Utility in "MacOS9Lives:System Folder:Extensions"; moved Apple's OpenGLLibrary, OpenGLMemory and OpenGLUtility to "MacOS9Lives:System Folder:Extensions (Disabled)". Mac OS is restarting.
```

Nothing is offered when the startup disk is a CD, an image attached
read-only, or a locked volume, nor without a window (`--headless`): an input script can
answer instead (`tools install`, `tools later`, `tools never`), and that
turns the check on; `tools install` restarts Mac OS too, and
instruction-time runs stay reproducible. The startup disk is the partition
the ROM file came from (with `--rom`, the first with boot blocks).

The offer has a third button, on its left: Don't Ask Again. Blue then
doesn't offer the tools by itself on that disk again, whatever version
they are: Mac OS writes a small preferences file, "Blue Preferences", in
the startup disk's Preferences folder (through the installer, as it writes
the tools; text: `Offer the guest tools: no`), which Blue finds there as it
looks for the tools before startup. The choice goes with the disk, so
another system Blue starts has its own. Install Guest Tools... in Blue's
menu (F12) makes the offer any time there's something to install (it's
disabled otherwise, saying why), Don't Ask Again or not; installing the
tools deletes the file, so newer versions of them are offered again.
Deleting the file by hand does the same. In scripts, `tools offer` is the
menu item.

```
tools: Blue won't offer its guest tools on "MacOS9Lives" again (Blue Preferences in "MacOS9Lives:System Folder:Preferences" says so). To install them later, choose Install Guest Tools... in Blue's menu (F12).
tools: not offered (Blue Preferences: Don't Ask Again; Blue's menu, F12, offers them)
```

They can still be installed by hand: `blue tools create FILE` writes a
small volume, "Blue Tools", holding them, which `--cdrom FILE` attaches
(read-only); drag them onto the System Folder (the Finder puts them in
Extensions) and restart, as Blue's installer does. The Finder shows each
tool with an icon of Blue's (custom icons: resource ID -16455 in each
file, with the Finder flag that says so), and the volume too (an
invisible file at its top, `Icon\r`). Each is described below.

Like the drivers, these are generated code fragments whose logic runs in
Blue through host calls (`src/tools.c`; but for the Pippin library's).

## Blue Enhancements

With Blue Enhancements installed, resizing the window changes the
screen's resolution to match once the window has stopped changing (its
size in the display's pixels over the window's scale, see [the window's
scale](window.md#the-scale); while it's being dragged, the window scales the screen), and the
Finder lays out the desktop again. A
resolution chosen in Mac OS (Monitors & Sound, or the Control Strip)
resizes the window instead. Without it, the window scales the
screen. It
also shares the clipboard ([below](#the-clipboard)). One installed by a version of Blue
from before the clipboard isn't this version's (so in a window Blue
offers to update it), and it still switches modes until then: its data carries its
version, as the 3D engine's does, so that Blue finds the older copy's
data where it is, and says so. An `appe` only starts from Extensions, so
a CD's system can't run it; to try it there, `BLUE_TOOLS_APPL=1 blue
tools create FILE` makes the volume's copy an application the Finder
opens with a double-click.

## The clipboard

In a window in real time, Blue Enhancements shares the clipboard: text
copied in Mac OS is on the host's clipboard, and text copied on the host
is what Command-V pastes in Mac OS, in the front application, without
switching applications (tested: SimpleText, Note Pad, Scrapbook, the
Finder's names and Internet Explorer 4's fields on 8.5, SimpleText on
9.1). Mac OS keeps text in Mac OS Roman with CR line ends (`TEXT`), and
applications that know Unicode (Mac OS 9's MLTE) in UTF-16 too (`utxt`);
the host has UTF-8 with LF. Accents come out precomposed (macOS often
decomposes them), and characters Mac OS Roman lacks become something
close where there is one (½ "1/2", → "->", other spaces a space) or
else "?"; `utxt`, which Blue puts in the scrap too, has them all, for
applications that read it, and is what the host gets when an
application puts both. Styles (`styl`) and pictures aren't passed, nor
texts over 4 MB (Blue says so). How: each application has a scrap of
its own (the Process Manager hands it to the next one brought to the
front), and many keep a private one besides, such as TextEdit's, which
they take from the scrap only when resumed with "the clipboard has
changed". Enhancements patches ZeroScrap and PutScrap to see what
applications copy; puts the host's new text in the front application's
scrap just before it gets its next event (a GetNextEvent filter: some
read the scrap's size in low memory rather than asking); and posts it a
resume event saying the clipboard has changed, as the Process Manager
does when one switches applications. Keys typed in the window meanwhile
wait for that (at most half a second), so that a quick Command-V pastes
the new text. An application that puts its copy in the scrap only when
switched out (from a private scrap, at a suspend) reaches the host once
another Mac application comes to the front. Scripted runs have a
stand-in for the host's clipboard (`clipboard` and `print-clipboard`,
see [debugging.md](debugging.md#input-scripts)); other runs without a window (`--headless`), or in
instruction time, leave the clipboard alone.

## Blue 3D Engine

Blue 3D Engine is a QuickDraw 3D RAVE drawing engine (put it in
Extensions). QuickDraw 3D's interactive renderer and RAVE games then find
an accelerator: it reports itself as an ATI engine (vendor 1, engine 5),
because games of the time, Bugdom among them, take nothing else for
hardware. Contexts draw to the screen or to memory, with textures and
bitmaps in the 16- and 32-bit formats, Z buffering, Gouraud shading,
perspective-correct and mipmapped texturing (modulate, decal, highlight),
fog, alpha test, the blend modes including OpenGL-style factors, and
depth-sorted transparency (`kQATag_ZSortedHint`). The host side
(`src/rave.c`) records each frame's drawing, a batch at a time, and while
the guest goes on recording the next, the GPU or a software rasterizer
draws it:

- In real time (the window, or `--headless --realtime`) the host's GPU draws, if it has
  hardware OpenGL 3.3 (`src/gpu.c`): in an off-screen context of its own
  (CGL on macOS, EGL without a window system elsewhere; `src/gl.c`,
  loaded at run time, so not a build dependency), on a thread of its own,
  and each frame is read back into the context's device in guest memory as
  before. The first context says which (`3d: drawing with OpenGL 4.1
  Metal - 91.7 (Apple M5)`). The pixels are those of the software
  rasterizer but for rounding in filtering (and at the odd triangle edge),
  and mipmapping, which is per pixel (trilinear with the Best texture filter,
  with anisotropic filtering where there is some). If OpenGL fails, the
  engine says so and goes on in software.
- Otherwise, a software rasterizer (`src/raster.c`) draws it in bands of
  rows on helper threads (with vertices snapped to 1/256 of a pixel, as
  GPUs snap them, and exact edges: triangles sharing an edge leave no
  gap and draw no pixel twice); the pixels are always the same, however many
  threads draw them, and it's built without the fused multiply-adds
  compilers use on some hosts (Apple silicon's), so instruction-time runs
  stay the same on every host. `BLUE_RASTER_THREADS` sets the number
  (default 3, counting the emulated processor's).

The rasterizer and the GPU take what Mac OS 9's OpenGL needs too: two
texture units combined in stages as OpenGL's texture environments combine
them, blend equations, colour masks, fog by a fog coordinate, a stencil
buffer and its test; RAVE's texture operations are such stages.

`--no-gpu` (or `BLUE_GPU=0`) draws in software in real time too;
`BLUE_GPU=1` draws with OpenGL in any run, a software renderer such as
Mesa's llvmpipe included. `BLUE_RAVE_FPS=1` prints the frame rate every
second, and `BLUE_LOG_RAVE=1` logs every call. [3d.md](3d.md) describes
the engine and both backends in full.

Frames for the screen come at most one a vertical blank of the display's
(60 a second of guest time), which shows no more; the iMac's 3D hardware
drew games no faster. The call that ends one (QARenderEnd) returns once
the display shows it: in real time the guest waits in it, taking
interrupts meanwhile; in instruction time guest time moves on instead.
Games of the time go wrong at the frame rates the host's GPU reaches:
Unreal Tournament's engine ticks its actors at least 5 ms at a time, so
at 400 frames a second its game, timers and match clock included, ran
at twice the speed (see [3d.md](3d.md#presenting-and-pacing)). Waiting, the host sleeps,
as when the guest idles (but guest time ahead of the host clock isn't
made up then: the game would show it). In a window in real time, the
window shows each such frame when it's due, so frames come at an even
pace.

The engine's data carries its version, so one installed by an older
version of Blue isn't asked for what it lacks: Blue says to install it
again (and a window offers to update it). How Bugdom and Unreal
Tournament use the engine is in [software.md](software.md#bugdom-and-unreal-tournament).

## Blue OpenGL Library, Memory and Utility

These are Mac OS 9's OpenGL libraries, as Blue's own, under
Apple's fragment names (OpenGLLibrary, OpenGLMemory, OpenGLUtility) with
Apple's exports, put in Extensions; on Mac OS 9 Apple's go to Extensions
(Disabled) meanwhile ([above](#offering-and-installing-them); installing by hand, move them there too). Their
calls are host calls (`src/opengl.c`), but for most of gl's: the
library's own code records those in a buffer in the program's memory,
done on the host all together at its next host call (a swap,
`glGetError`). A front end on the host (`src/glfront.c`, `src/gltex.c`)
does OpenGL's
fixed function, and it draws with the 3D engine's backend, on the GPU in
real time and in software otherwise, into the context's window (clipped
to its regions), the screen or an offscreen buffer, when it's swapped.
They do AGL (ATI's accelerated "Rage 128 OpenGL Engine" as on Mac OS 9,
and, for pixel formats with an accumulation buffer, which only Apple's
software renderer offered, that one, "Generic", drawn the same way but
with Apple's strings; their pixel formats and renderer info; contexts
laid out as Apple's, with their dispatch tables; drawables, swaps, swap
and buffer rectangles) and, of
OpenGL 1.2, the matrices and their stacks, `glBegin` and `glEnd`'s points,
lines, triangles, quads and polygons with their colours, transformed,
clipped, culled, flat or smooth shaded, depth tested, blended or by logic
ops, alpha tested, masked and scissored in the viewport; points and lines
by OpenGL's rules (wide, antialiased, stippled, textured), the polygon
stipple, the accumulation buffer, 16-bit drawables dithered, Apple's
specular vector, the texture LOD bias and texture environment combine
extensions; textures
(objects, 1D and 2D images in every format and type, parameters, two
units with their texture environments, the texture matrices);
client arrays (`glDrawArrays`, `glDrawElements`, interleaved, compiled);
lighting (`src/gllight.c`: 8 lights, materials, two sides, separate
specular), fog, generated texture coordinates, user clip planes,
stencil, polygon offset and modes, the attribute stacks
(`src/glarray.c`, `src/glattrib.c`); display lists (`src/gllist.c`);
the raster position, `glBitmap`, `glDrawPixels`, `glCopyPixels` and the
pixel transfer (`src/glpixel.c`); `glReadPixels`, `glCopyTexImage` and
`glGetTexImage`; 3D textures; clears, strings, errors and queries;
OpenGLMemory's allocations; and OpenGLUtility, done on the host as SGI's
GLU does it (`src/glu.c`: the matrices, `gluProject`, scaled images and
mipmaps, quadrics); selection and feedback (`src/glselect.c`);
evaluators (`src/gleval.c`); `aglSetFullScreen` (the Display Manager
switches the screen to the size asked for, and back when the drawable
goes) and `aglUseFont` (QuickDraw draws the glyphs, as with Apple's).
They leave out OpenGLUtility's tessellators and NURBS (they draw nothing),
antialiased polygons (`GL_POLYGON_SMOOTH` is kept, not drawn), auxiliary
buffers and colour index; S3TC and paletted textures aren't offered (nor
were they by Apple's for a Rage 128). An OpenGLLibrary installed by an older version of Blue is left
alone, and Blue says to update it.
Unreal Tournament's OpenGL build (on the UT CD, `Extras:OpenGL UT.sea`)
and Quake III Arena's demo play with them, on the GPU and in software.
Swaps to the screen or a window are paced as the 3D engine's frames are
([Blue 3D Engine](#blue-3d-engine)). `BLUE_GL_FPS=1` prints the frame rate every second, and
`BLUE_LOG_GL=1` logs every call. `BLUE_GL_PROFILE=FILE` writes a line
to FILE for each frame shown on the screen: the host time since the one
before and where it went (the libraries' host calls, waiting for the
backend, reading back from the GPU, writing the frame to guest memory,
waiting for the frame's turn), the calls, records and primitives, and
the GPU thread's time drawing and reading back; and a line for each
update of the window (see [3d.md](3d.md#where-a-frames-time-goes)).
How they work, and how they're tested, is in [3d.md](3d.md#blues-opengl).

## Blue Pippin Library

Blue Pippin Library is PippinPPC, the Bandai Pippin's library, for
Pippin titles (below; and [software.md](software.md#pippin-titles)), put in Extensions. Unlike the
others its code is its own, calling Mac OS through InterfaceLib and
AppearanceLib; it makes no host calls.

Discs for the Bandai Pippin (Apple's 1996 console design, System 7.5) are
HFS CDs, and many of their programs run on a Mac, but some link to the
Pippin's own library, PippinPPC, which Mac OS hasn't got ("could not be
opened because "PippinPPC" could not be found"). Blue Pippin Library is Blue's own PippinPPC, with the Pippin's
twelve functions mapped onto Mac OS 8.5 to 9.2.2 (`src/pippin.c`; [findings.md](findings.md), "PippinPPC
on a Pippin", has what each does on a Pippin):

- The Pippin's file dialogs are Standard File's, starting in the title's
  folder: on a Pippin that is at the root of its 120 KB of flash (or a
  floppy's), here `System Folder:Preferences:Pippin:` and the title's name
  (its `pipn` resource's, else the application's). Saves and preferences
  go there, never to the CD. The file chosen comes back with a working
  directory for its folder as its vRefNum, as the old Standard File's did,
  since titles such as Marathon take the folder from that.
  `PippinStandardDeleteFile`, the Pippin's "make room" dialog, is a file
  chosen in the title's folder, deleted once a caution alert is answered
  Delete. `MakeWritesPermanent` (writing the flash) flushes the volume;
  there's no floppy dock; the soft keyboard and the Pippin's extra PRAM do
  nothing.
- While a title that links to it runs, Gestalt answers `pipn` (titles
  check it, and Super Marathon quits without it: "Sorry, but this game is
  only for the Pippin."), and ShutDwnStart and ShutDwnPower return at once:
  a Pippin turns off when its title quits, which here would shut Mac OS
  down.

Nothing provides the Pippin's video calls (`PippinVideo.h`'s, which
aren't among PippinPPC's twelve) or its floppy dock, for titles that use
them.
