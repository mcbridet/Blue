# The window

How Blue's window works: drawing the Mac's screen, its scale, the
keyboard and mouse, the menu (F12), the prompts and the status bar. How to
use them is in the [README](../README.md#using-blue).

## Drawing the screen

Blue's window is an SDL2 window, loaded at run time (`src/display.c`; no build
dependency, headless without it; Blue.app's or the AppImage's own SDL2
first, then the system's, on macOS also in Homebrew's and MacPorts'
directories, which aren't searched for bare library names: `src/hostlib.c`;
redrawn only when the screen, the pointer or the status bar changes;
drawn at the display's full resolution, so high-density displays don't
blur it). The window has a thread of its own, the process's main thread
(where SDL wants windows, events and drawing on macOS), and the Mac runs
on another: up to 60 times a second (and when each of a 3D game's frames
is due) the Mac hands over a frame, the rows of the screen that changed
copied into the window's copy of the framebuffer, and goes on while the
window converts, uploads, draws and presents it (about a millisecond
later); the window's input reaches the Mac through a queue. The Mac
doesn't compare the whole framebuffer to find what changed: its pages
are watched for writes (in the JIT's view of guest memory they're
write-protected, and the first store to one, whichever way it comes, is
seen and lets the page go; see [fastmem](jit.md#memory-fastmem)), so only the pages written since
the last frame are compared with the copy, a host page at a time (16 KB
on Apple silicon). A page written in most frames (a game's, a movie's) is
compared rather than watched, until it has stayed the same for 8 frames:
being seen written costs several times what comparing it does.
`BLUE_WINDOW_COMPARE=1` compares the whole screen instead. The rows that
changed are converted into the 32-bit format renderers take as it is
(ARGB8888: a byte swap at 32 bits per pixel, the 5-bit channels widened
at 16, the CLUT expanded to 32-bit colours at 8 and fewer), uploaded,
and drawn again in the scaled copy below (all of it when the mode or
colours change, and once a second if nothing has). A screen the window's
own size at its scale is drawn at exactly that (see [the scale](#the-scale));
others are scaled to fill the window above the status bar, keeping their
shape: whole multiples exactly, other sizes by drawing at the multiple
above on the GPU and filtering that down, so every screen pixel comes
out the same size with edges blended over at most one window pixel.
`--integer-scale` uses whole multiples only (with a border if need be).

## The scale

In a window, each of the Mac's pixels is a whole number of the display's
pixels, 1 to 4: the window's scale. Auto, the default, picks the number
that brings Mac OS nearest, in proportion, to 80 pixels per inch, as on a
1999 Mac's screen, from the display's own density: its physical size from
CoreGraphics on macOS; on Linux SDL 2's figure (the X server's or the
compositor's) or, under sdl2-compat, whose displays have no physical size,
the kernel's EDID of the display (`/sys/class/drm`). So 1x up to 113 pixels
per inch (a 27-inch 1440p display: 109), 2x up to 196 (a 27-inch 4K one:
163), 3x up to 277 (a 5K iMac: 218; a 14-inch MacBook Pro: 254), and 4x
above (a 13-inch MacBook Air made to look like 1710 x 1112: 299). Where the
density isn't known, or isn't plausible (a television giving its shape for
its size), it's the display's own scale: 1x, or 2x on a Retina display. It's
worked out again when the window goes to another display, and Blue says
what it found:

```
display: 218 pixels per inch: Auto's scale is 3
```

`--scale auto|1|2|3|4` sets it at startup, and F12's menu has it (Scale:
Auto (3x), 1x, 2x, 3x, 4x; Auto says what it chose). The menu, prompts and
status bar are in the window's own units (points), whatever the scale.

- With Blue Enhancements (see [guest-tools.md](guest-tools.md#blue-enhancements)) the screen follows the window:
  resized, the window asks Mac OS for its size in the display's pixels over
  the scale, rounded to what the display driver takes (the width down to a
  multiple of 8, at least 512 x 384, and smaller, keeping its shape, if 16
  MB wouldn't hold it at 32 bits a pixel), and that screen is drawn at
  exactly the scale, crisp, with a border of what the rounding left. A
  full-screen window on a 5K iMac is 1704 x 944 at 3x, with the status bar.
  A new scale changes the screen, the window staying as it is (as Mac OS
  resumes, chosen in the menu).
- Without it, the window shows the screen at the scale: it opens at the
  screen's size times the scale, within the display (smaller, keeping its
  shape, if that doesn't fit), and a new scale resizes it. Resized by hand,
  it scales the screen to fill it (see [Drawing the screen](#drawing-the-screen)).
- `--integer-scale` keeps to whole numbers wherever the window scales a
  screen that isn't its own size (without Enhancements; while the window is
  being resized; a resolution chosen in Mac OS that doesn't fit the
  display), leaving a border rather than filtering; a window that doesn't
  fit the display at the scale is the largest whole multiple that does.
  With the screen following the window it changes nothing: that screen is
  drawn at the scale anyway.

## Keyboard and mouse

In the window, the GUI key (Command on a Mac keyboard, the Windows or
Super key on a PC's) is Mac OS's Command key, Alt is Option, and Control
is Control. Only the left mouse button reaches Mac OS; F12 is Blue's own
(the menu), and isn't passed on.

Every Command-key combination is Mac OS's, on macOS too. SDL makes a menu
bar of its own when it starts (`CreateApplicationMenus()` in SDL 2.32.10's
`SDL_cocoaevents.m`, and SDL3's under sdl2-compat; whenever it makes the
application object, so not even an earlier menu bar stops it), whose key
equivalents acted on Blue as well as reaching Mac OS (SDL sees a key
before AppKit offers it to the menu bar): Command-Q (Quit, SDL_QUIT) and
Command-W (Close) ended the run at once, Command-H hid Blue, Command-M
minimised the window, Control-Command-F made it full screen, and AppKit
added Option-Command-W and -H. Once SDL is up, Blue puts its own in place
(`src/menubar_mac.m`): the application menu alone, About Blue, Hide Blue
and Quit Blue, without key equivalents, and no Window menu. Nothing else in
SDL acts on Command keys (its views' `keyDown:` do nothing, and no hint
changes that). The window without a command, which runs no Mac OS, gives
Hide Blue and Quit Blue their Command-H and Command-Q (`display_shortcuts()`).
macOS keeps its own (Command-Tab, Command-Space, the screenshot keys).

The Mac's cursor goes where the host pointer is (the low-memory mouse
globals are set, as SheepShaver does), until Mac OS has had its cursor
hidden for 0.3 s, as games hide it: then the pointer is captured and its
motion goes to the ADB mouse, which is where InputSprocket and games read
it (Unreal Tournament's mouse didn't move otherwise), until the cursor
shows again. Control-Option (Ctrl+Alt) lets the pointer go, as does
switching away from the window; a click in the window captures it again
(the guest doesn't get that click), and so does the cursor showing and
hiding again. The window's title says so while it's captured.

## The menu

F12 opens the menu (Dear ImGui's, from `third_party/imgui`, drawn over
the screen through the same run-time SDL2 by `src/ui.cpp`; SDL 2.0.18 or
later), and the guest is paused while it's open. In it: Resume (F12 and
Esc too); Restart (straight away: the Mac's own reset, so work not saved
in Mac OS is lost); Shut down, which presses the keyboard's power key, so
Mac OS asks "Are you sure you want to shut down your computer now?" and
shuts down properly if told to (applications ask to save, NVRAM and a
shared folder are written, and the run ends when the Mac turns itself
off); Turn off, the power switch, for a Mac that doesn't answer (straight
away; it keeps NVRAM); Install Guest
Tools... (the offer, see [guest-tools.md](guest-tools.md)); a screenshot of the Mac's screen
(a PNG on the Desktop, or in the current directory without one, named
for when it was taken); fullscreen (at the desktop's resolution); the
window's scale (radio buttons: Auto, saying what it chose, and 1x to 4x;
see [the scale](#the-scale)); the
status bar; whether the pointer is captured while the Mac hides its
cursor; and what's in the floppy and CD-ROM drives, each with Eject and
Insert... (see [drives.md](drives.md#removable-drives)). In real time guest time doesn't count
the pause, so the Mac's clock is that much behind afterwards.

## Quitting

SDL turns the window's close button (or the window manager's Close, on
Linux), Quit in the menu bar and the Dock, and logging out of macOS (a
Quit Apple event: SDL's application has `terminate:` send SDL_QUIT) into
SDL_QUIT, which the window passes on as `DISPLAY_EVENT_CLOSE`. Once Mac
OS has started up (the Finder has been the current application since the
Mac last started), Blue asks over the Mac's screen, as the guest tools'
offer does (in its place if that's showing, and with the menu closed): "Mac
OS is still running. Shut it down before Blue quits?", with Shut Down
(the default: the power key, as the menu's Shut down... presses, so Mac
OS asks and Blue quits when it turns itself off), Cancel (Esc: back to
the Mac, and to the offer if it was showing) and Turn Off (on the left:
at once). Asked again while it's asking, it goes on asking: Turn Off is
there, and a second click on the close button shouldn't lose anyone's
work. Before the Finder is up there's nothing to lose but a startup, so
Blue quits at once, as it does in scripted runs (whose input is the
script's) and with an SDL too old for the menu. Logging out of macOS
waits while Blue asks, and macOS gives up on the logout if Mac OS takes
too long. The run's last line says why it ended: `window closed`, `turned
off on quitting`, `turned off from the menu`, or `guest powered off`.

SIGINT and SIGTERM still end a window's run at once: SDL would make them
SDL_QUIT too, so while the window is open Blue has handlers of its own,
put in place before SDL's would be (SDL leaves alone a handler it finds,
as it does SIG_IGN, a background job's), and the window passes them on as
`DISPLAY_EVENT_QUIT` (`told to quit (SIGINT or SIGTERM)`).

## The look

The menu, the prompts (the guest tools' offer and news, and whether to
shut Mac OS down on quitting) and the status
bar look like Mac OS 8's Platinum, in the colours of Blue's site
(`site/style.css`), the same on macOS and Linux, in a window or
fullscreen: Platinum grey boxes in a black frame, bevelled (light top
and left edges, dark bottom and right), with a shadow and their title in
a striped title bar; bevelled, rounded push buttons, dark while pressed;
checkboxes whose check overhangs the box; radio buttons, round, with a
dot; etched lines between parts; and Bondi Blue for the checks, the
radio buttons' dots, the keyboard's focus ring and the processor's light
as it naps. A prompt has Blue's icon beside its text, as a Mac OS
alert has its icon, and its buttons below in Mac OS's order: the default
on the right, with Mac OS's thick ring around it, the other beside it,
and a third (Don't Ask Again) on the left. Return presses the default
button (Resume, in the menu), Esc the other (in the menu, Esc resumes);
Tab and the arrows move the focus ring, and Space presses the button it's
on. The text is ChicagoFLF (`third_party/fonts`: Robin Casady's
public-domain revival of Mac OS's Chicago), kept in the program
(`src/font_data.inc`, made by `tools/fonts.py`), so building needs
nothing more; it's drawn at 12 pixels to the em (24 on high-density
displays, rasterized at the display's density), Chicago's own grid, so
its strokes are whole pixels, as Mac OS's 12-point Chicago was.

## The status bar

The status bar (shown unless `--no-status-bar`; the menu toggles it) sits
below the screen, Platinum like a Mac OS 9 window's edge, so the window grows by its height rather than covering
anything (fullscreen, or maximized, the screen fitted to the window
shrinks instead). It has lights for the disks, CD-ROMs, the floppy drive
(if there's one) and network, lit
for a tenth of a second whenever they move data (green, amber while a
disk is written; a light is dimmed if there's no such device), each with
what it read and wrote over the last second (the network's what it
received and sent, in bits; the disks' in bytes; a CD-ROM only reads),
and one for the processor, green as bright as the share of the time the
guest isn't napping, and softly blue for the rest, while it naps (with
idle sleep). Beside that is its speed: millions of instructions a second of
the time actually spent running them, as a clock rate (the Mac thinks it
has a 266 MHz G3, which runs about one instruction a cycle), worked out
once a second from a random 1 in 32 runs (timing each costs too much),
and not shown while the guest naps (busy less than a tenth of the time)
or the menu pauses it.
In narrow windows (640 wide, say) the names are shortened and the text
made smaller to fit.

## The rest of the host side

The host side also has PNG screenshots (`src/png.c`), NVRAM kept
in a file, real-time guest time (the window's, or `--headless --realtime`), and
`blue disk create`, which lays out an empty HFS volume (`src/hfs.c`, which
also lays out the shared folder's, with B-trees of any size) the
way Disk Initialization does (`hfsck` from hfsutils accepts it).
