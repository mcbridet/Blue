# Game controllers

The host's game controllers reach Mac OS three ways: as the Mac's ADB
keyboard and mouse, by Blue's mapping (or one of the user's own); while a
Pippin title runs in front, as the Pippin's AppleJack controllers, by the
title's own mapping; and in games that use InputSprocket (Apple Game
Sprockets' input library), as game controllers of their own, through a
driver among the guest tools.
This document describes how. How to use them is in the
[README](../README.md#game-controllers); how the window takes the pads
from SDL2 is in [window.md](window.md#game-controllers).

## Pads and the mapper

A pad is up to four of SDL2's game controllers in the window, or of an
input script's (`pad N plug`, [debugging.md](debugging.md#input-scripts)).
Their buttons and axes come to the Mac's thread by SDL's names and layout
(A the bottom face button, B the right, X the left, Y the top, whatever
the pad's labels say) and go to `src/pad.c`, which keeps each pad's state
and turns controls into ADB keys and mouse button presses (`adb_key()`,
`adb_mouse_button()`) and motion (`adb_mouse_move()`).

Before each change `pads_ready()` in `src/nk.c` sets the mapping in force:
off (the menu's Controllers checkbox), Blue's (each pad's own, below), or
a Pippin title's; and,
for each pad, the controls a game reads itself through InputSprocket
(`pad_slot.taken`, below), which do nothing here. What a control does is
decided as it goes down and undone as it comes up, whatever the mapping
is by then, so a key never stays down; a key two controls hold goes up
when the last lets go. The sticks' four ways and the triggers are buttons
too, pressed past half way and let go below 3/8. While Blue's menu or a
prompt is up, presses do nothing.

Blue's mapping (`pad_default_map`): the D-pad and left stick the arrow
keys, A Return, B Command-period, X Space, Y Tab, Start Esc, Back Command
(held), the shoulders Shift and Option (held), the right trigger the
mouse button and the left a Control-click, the right stick the mouse
(pressed: the button). A stick that's the mouse moves the ADB mouse as
guest time passes, at up to `PAD_MOUSE_SPEED` (900) counts a second,
growing as the square of how far it's pushed past a dead zone of about
15%; it's worked out at the machine's sync points in whole numbers, so
that instruction-time runs come out the same on any host. Its four ways
do nothing then; a stick that's keys does what each of its ways is
mapped to instead.

## Mappings of one's own

A mapping (`struct pad_map`) says what each of the 31 controls does (the
21 buttons, the triggers, the sticks' ways) and whether each stick is the
mouse or keys. A control can be a key, with modifiers or not (Command,
Shift, Option, Control: pressed before it and let go after), held as long
as the control is; a modifier key alone; a click, with modifiers or not;
a double-click; Click Lock (the mouse button down until the control is
pressed again); or nothing. These are the AppleJack driver's functions,
and `struct pad_switch` its SwitchData. Each pad has its mapping in its
slot (`pad_set_map()`): its kind's own, if the user gave it one, else the
one for all controllers, which is Blue's until the user changes it. A
kind of controller is SDL's GUID for it (`SDL_JoystickGetGUID()`, as
`SDL_JoystickGetGUIDString()` writes it), or, for a script's pads
without a window, its name. A control down when its mapping changes goes
up as it went down. Mappings apply to Blue's mapping only: a Pippin
title's AppleJacks keep the title's, and a game's InputSprocket needs its
own controls dialog.

**The menu.** Mappings… under Controllers in Blue's menu shows a box in
the menu's place (`mappings()` in `src/ui.cpp`): a radio button for each
kind (All controllers, and each kind plugged in), a checkbox for whether
a kind has a mapping of its own (turned on, it starts as a copy of the
one for all; off, the kind takes that one again), and the controls on
three pages (Buttons, the D-pad and the rest, the Sticks), each with
what it does in a box like a Mac OS 8 pop-up menu. Its menu has Press a
Key…, the clicks (plain, with Shift, Command, Option or Control), Double-
Click, Click Lock and Nothing. Press a Key… waits for a key on the host's
keyboard (`ui_capture_key()`: the window hands the menu every key then,
Esc too, F12 aside): the first that isn't a modifier, with the modifiers
held, or a modifier let go of alone. Restore Defaults makes the mapping
shown Blue's. Each change goes to the machine's thread as a menu action
(`UI_PAD_EDIT`, `pad_edit()` in `src/nk.c`), which changes the settings,
writes the settings file, and maps the pads again. The machine logs each:

```
pads: Xbox Wireless Controller has a mapping of its own
pads: Xbox Wireless Controller: a is Command-Q
pads: all controllers: the left stick is the mouse
```

## The settings file

Blue's own settings, those the user chooses in its menu, are kept in a
file of their own (`src/settings.c`), apart from the Mac's NVRAM
(`--nvram`): for now, the mappings. It's where each platform keeps such
things: `~/Library/Application Support/Blue/settings.ini` on macOS,
`$XDG_CONFIG_HOME/blue/settings.ini` on Linux (`~/.config/blue/` without
it). It's text, as INI files are: `name = value` lines under a
`[section]`, names and sections in any case, `#` starting a comment line.
The launcher on the [roadmap](roadmap.md) can keep its settings there
too; sections and lines Blue doesn't know are kept as they are when it
writes the file. The mappings:

```
[controller]
a = Return
b = Command-Period
leftshoulder = Shift
lefttrigger = Control-Click
leftstickmode = Keys
leftstickup = Up Arrow
rightstickmode = Mouse
...

[controller 030000005e040000e002000003090000]
name = Xbox Wireless Controller
a = Command-Q
...
```

`[controller]` is the mapping for all controllers; `[controller ID]` a
kind's own, by its GUID (32 hex digits) or, without one, its name. The
controls go by SDL's names (`a`, `leftshoulder`, `dpup`, `misc1`,
`lefttrigger`), and the sticks' ways as `leftstickup` to
`rightstickright`; `leftstickmode` and `rightstickmode` are `Keys` or
`Mouse`. What a control does is a key by its name on a US keyboard (`A`,
`1`, `Period`, `Left Bracket`, `Return`, `Esc`, `Up Arrow`, `F5`, `Keypad
5`; a character such as `.` is taken too) after modifiers
(`Command-Shift-Z`), a modifier alone (`Shift`), `Click` (`Control-Click`),
`Double-Click`, `Click Lock` or `Nothing`. A control without a line keeps
Blue's default. A line Blue can't read is left out, and said once
(`settings: settings.ini, line 7: not a key, a click or Nothing (left
out)`); a file it can't read at all is left alone, and not written over.
Blue writes all its sections whole, each time something changes: to a
new file in the same folder (made if need be), synced, then renamed over
the old one, so the file is always one or the other.

Only runs in a window, in real time and without an input script, use the
file, so that reproducible runs and tests don't depend on (or change) the
user's: `--settings FILE` uses that file in any run, and `--settings none`
none. Blue's menu says where the mappings are kept, or that they're for
the run only.

## A Pippin title's AppleJacks

The Pippin's controller, the AppleJack, was an ADB device with 18
switches, whose driver turned them into keys and mouse clicks by a
mapping a title can set: Blue Pippin Library holds that driver's data
(AJGlobalData) for two, makes them findable as AppleJacks among the ADB
devices, and tells Blue where they are and which process the title is
([guest-tools.md](guest-tools.md#blue-pippin-library) has the detail,
[findings.md](findings.md), "The AppleJack", the driver's data). While
the title is the front process (Blue Enhancements asks Mac OS which is,
as it looks round), the pads are the AppleJack: the D-pad and left stick
its D-pad; A, B, X and Y its red, green, yellow and blue buttons (as they
sit); the shoulders and triggers its left and right fire buttons; Back,
Guide and Start its three system buttons; and the right stick its
trackball, the mouse. Blue reads a switch's mapping in guest memory as
the control goes down, so a title's changes (Super Marathon's menus and
its game have their own) apply at once, and keeps the data's
switchStates as the pads have them, for titles that read them.

**Two AppleJacks.** A Pippin had two ADB ports in front, and Apple's note
on the driver says to keep a pointer to each AppleJack found, four at
most. With one pad plugged in (or none), a title finds one AppleJack, as
before; with two or more, it finds two (the library's count, which Blue
sets as pads come and go: `pads_done()`), and the second pad (the second
slot in use, in Blue's numbering) is the second AppleJack, by its own
mapping and with its own switchStates; the others are the first. Each is
an ADB device first at address 3 (now 15 and 14), with a cursor device
record of its own whose refCon points at its AJGlobalData; a title that
goes through them (as Apple's note does, from the last to the first)
finds both, and sets each one's mapping. Blue logs the count and each
one's mapping:

```
pippin: marathon.ppc runs, with AppleJacks for game controllers (their data at 0x06F437FC and 0x06F43858)
pippin: marathon.ppc has two AppleJacks: the second pad is the second
pippin: marathon.ppc's second AppleJack mapping: the same
```

A copy of the library from before (one AppleJack) still works: Blue
finds no second AJGlobalData after the first, and every pad is its one
AppleJack.

## InputSprocket

InputSprocketLib loads every shared library of creator `insp` (or
Apple's own, `insi`) in the Extensions folder for each game that starts
it, and calls those its drivers. Blue Game Controllers is one, among the
guest tools (`src/isp.c`, a fragment built as the other tools are: every
entry a host call, its logic Blue's, calling InputSprocket and the
Toolbox through its imports in turn). InputSprocket itself is Apple's:
games that use it install it. It has been tried with InputSprocket 1.4.1
(Mac OS 8.6's era) and Unreal Tournament.

**Devices.** As the driver is loaded (its initialization routine, as
Apple's drivers of the time do; and when InputSprocket 1.7 calls its
`ISpDriver_FindAndLoadDevices`), each pad plugged in becomes a device of
class gamepad (`ISpDevice_New`), named as SDL names it, its permanent ID
its slot (so that a game's saved configuration finds it again), with 18
elements (`ISpElement_New`): its eleven buttons, the D-pad, the sticks'
four axes and the two triggers, named ("Left stick up/down", "Right
trigger") and labelled for what they usually are (the left stick's axes
`xaxi` and `yaxi`, the right's roll and pitch, the triggers brake and
gas, Start pause). A Time Manager task pushes the pads' changes to those
elements (`ISpElement_PushSimpleData`) every 10 ms of guest time, as does
the game's `ISpTickle`, for games that read devices' elements directly.
The meta handler given to `ISpDevice_New` hands InputSprocket a function
for each of its 22 selectors, each another entry of the fragment.

**A game's needs.** The game's `ISpInit` gives each device the game's
needs (Init): a name, a kind (button, axis, delta, D-pad, movement), a
label saying what it's for, and flags; and the virtual elements the game
reads them through. The driver meets those it can
(`isp_autoconfigure()`), and from then on pushes each one's data to its
virtual element as the pad changes. A need's label picks its control,
the way a shooter is played on a pad today, in this order:

1. Deltas (the mouse's motion: turning and looking) take the right stick.
2. Axes that are pedals or throttles take the triggers.
3. Buttons take theirs: fire the right trigger (or the right shoulder),
   secondary fire the left, jump A, pause Start, quit Back, previous and
   next the shoulders (or the D-pad's left and right), run and look the
   sticks' buttons; moving and sliding take the left stick's directions
   (or the D-pad's), even where the game has an axis for them, since a
   shooter's "move" axis looks up and down as well while mouse look is
   on; turning and looking take the right stick's, unless the game has an
   axis or a delta for them.
4. Other axes take a stick: x and y the left one's, roll, yaw and rudder
   the right one's left and right, pitch its up and down.
5. The rest, in the game's order: buttons A, B, X, Y, the shoulders, the
   triggers, the D-pad's directions, the sticks' buttons; axes a free
   stick axis (an asymmetric one a trigger); deltas the right stick.

Start, Back and Guide are left out of (5), for the game's menus. A
control serves one need, and a stick taken whole can't also be taken by
its directions. Needs another device met that may be met once only, the
game's utilities (a frame-rate key) and those it says not to configure
are left alone. Unreal Tournament's 26 come out as: the left stick runs
and slides, the right stick turns and looks, the right trigger fires,
the left fires the other way, A jumps, B crouches, X throws the weapon, Y
is mouse look, the shoulders change weapons, the D-pad's left, right and
up work the inventory and its down walks, and the sticks' buttons
sidestep and center the view. The log says so:

```
isp: Test Pad is an InputSprocket device for Unreal Tournament
isp: Unreal Tournament asked for 26 controls: Test Pad meets 20 of them: Turn (Mouse Left/Right) Right stick left/right, Look/Move (Mouse Up/Down) Right stick up/down, Fire/Click Right trigger, ...
isp: Unreal Tournament reads the pads through InputSprocket
isp: Unreal Tournament is in front: the pads' controls it uses are its own, the rest the keyboard and mouse
```

**The data.** Buttons are 0 or 1; an axis runs from 0 (left, down) to
0xFFFFFFFF (right, up), as games take them (SDL has up negative), a
trigger from 0; the D-pad is InputSprocket's eight directions. A delta is
the mouse's motion the stick makes as guest time passes, along the same
curve and up to the same speed as the right stick moves the mouse in
Blue's mapping, at `ISP_DELTA_COUNT` (128) a count. InputSprocket.h has a
delta as a Fixed and Apple's own mouse driver pushes plain counts, but
Unreal Tournament takes 1/128 of what it reads (it shifts it right by 7)
as the mouse's counts: so it turns about 280 degrees a second with the
stick all the way over, at its default sensitivity. Up is a positive
delta to it, as it is an axis's maximum. A delta's remainder is kept from
one push to the next.

**In use.** While a device of the driver's has a game's needs met and is
active (InputSprocket deactivates devices while their game is in the
background, and the game can), and the game is the front process (Blue
Enhancements says, as for a Pippin title), the pad's controls that meet
one of its needs are the game's only: Blue's mapping leaves them alone
(`isp_taken()`, `pad_slot.taken`; the right stick's mouse too), while the
rest still do what they do there, so Start is Esc for the game's menus.
The menu's Controllers row says so.

**The configuration dialog.** `ISpConfigure`'s dialog lists the devices;
for this one it shows the driver's panel in the rect InputSprocket gives
it (Show): a user item over that rect, drawn by the driver (in Geneva 9,
text cut to fit with `TruncString`), a row for each need a pad's control
can meet, each with its control in a box like a popup menu's. A click in
a box (InputSprocket passes the dialog's every event through
HandleEvent; a mouse-down in a box is handled there) pops up the
controls that can meet that need, with None, and the one chosen meets it
from then on. Dirty tells InputSprocket when it differs from what it was
as the dialog opened (it then says "Custom"); Cancel puts it back
(EndConfiguration). InputSprocket keeps what meets each need for the game
(GetState gives `BlGp`, a version, the needs' count and each one's
control; SetState takes it back, if it's for the same needs), and gives
it back when the game next starts. The device's icon in the list is the
driver file's own (the Finder's custom icon, resource ID -16455).

**Copies of other versions.** The host's logic has to match the fragment
the guest loaded: the fragment's data starts with `BlIs` and a hash of
its layout (imports, exports, entries, data); a copy whose hash isn't
this version's is left alone (each call returns at once: no devices),
and Blue says once that it's from another version of Blue, to update.
The guest tools' check finds the driver by its host call, as the others,
so other makers' `insp` drivers in Extensions are never taken for it.

**What isn't there.** A pad plugged in after the game started
InputSprocket becomes a device only if InputSprocket asks again (1.7
asks drivers through `ISpDriver_CheckConfiguration` whether their devices
are still what they were, which then says no); a pad unplugged stays a
device, its controls let go. Two games running InputSprocket at once
have a driver each, up to two. No force feedback: InputSprocket has none.
Unreal Tournament's menus move their pointer by the delta needs as well
as by the mouse, far faster with the stick; use the mouse there.

## Testing

`tests/test_pad.c` covers the mapper (Blue's mapping, mappings of one's
own, the sticks as keys or the mouse, a Pippin title's, two AppleJacks,
controls a game takes, the actions' names), `tests/test_settings.c` the
settings file (read and written, what Blue doesn't know kept, a kind's
mapping by GUID or name, written whole as a new file renamed over the
old, its place), `tests/test_pippin.c` the Pippin library (with the ADB
patches' 68k code run as a title calls them, for one AppleJack and two),
and `tests/test_isp.c` the driver: its fragment and file, the
autoconfiguration of Unreal Tournament's needs and a flight game's, the
data, and the driver as InputSprocket drives it against a Mac OS of the
test's own (registration, a game's needs, the task's pushes, the state,
the panel, a copy of another version's). In a run, input scripts play
pads in instruction time (`pad 0 axis rightx 0.8`; `pad 1 button a
down`, a second pad), and `BLUE_LOG_PADS=1` logs every control with the
mapping it went to and, for InputSprocket, each call InputSprocket makes
to the driver (but the task's and Tickle's) and each of a game's needs
with its kind, label and flags. Unreal Tournament was played this way on
Mac OS 8.6 (see [findings.md](findings.md), "Unreal Tournament's
needs"): run, turn, look, fire, jump and slide, and its Options, Input
Device dialog opened, a control changed in the panel, OK, and back to the
game. Super Marathon was played with two scripted pads on Mac OS 8.5
(see [findings.md](findings.md), "A second AppleJack"): it found both
AppleJacks and gave each its game's mapping, and the second pad's D-pad
moved the player forward through the second.
