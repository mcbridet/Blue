# Game controllers

The host's game controllers reach Mac OS three ways: as the Mac's ADB
keyboard and mouse, by Blue's mapping; while a Pippin title runs in front,
as the Pippin's AppleJack controller, by the title's own mapping; and in
games that use InputSprocket (Apple Game Sprockets' input library), as
game controllers of their own, through a driver among the guest tools.
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
off (the menu's Controllers checkbox), Blue's, or a Pippin title's; and,
for each pad, the controls a game reads itself through InputSprocket
(`pad_slot.taken`, below), which do nothing here. What a control does is
decided as it goes down and undone as it comes up, whatever the mapping
is by then, so a key never stays down; a key two controls hold goes up
when the last lets go. The left stick and the triggers are buttons too,
pressed past half way and let go below 3/8. While Blue's menu or a prompt
is up, presses do nothing.

Blue's mapping (`default_map`): the D-pad and left stick the arrow keys,
A Return, B Command-period, X Space, Y Tab, Start Esc, Back Command
(held), the shoulders Shift and Option (held), the right trigger the
mouse button and the left a Control-click, the right stick the mouse
(pressed: the button). The right stick moves the ADB mouse as guest time
passes, at up to `PAD_MOUSE_SPEED` (900) counts a second, growing as the
square of how far it's pushed past a dead zone of about 15%; it's worked
out at the machine's sync points in whole numbers, so that instruction-time
runs come out the same on any host.

## A Pippin title's AppleJack

The Pippin's controller, the AppleJack, was an ADB device with 18
switches, whose driver turned them into keys and mouse clicks by a
mapping a title can set: Blue Pippin Library holds that driver's data
(AJGlobalData), makes it findable as the AppleJack among the ADB devices,
and tells Blue where it is and which process the title is
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
switchStates as the pads have them, for titles that read them. Every pad
is the one AppleJack.

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

`tests/test_pad.c` covers the mapper (Blue's mapping, a Pippin title's,
the sticks, controls a game takes), `tests/test_pippin.c` the Pippin
library, and `tests/test_isp.c` the driver: its fragment and file, the
autoconfiguration of Unreal Tournament's needs and a flight game's, the
data, and the driver as InputSprocket drives it against a Mac OS of the
test's own (registration, a game's needs, the task's pushes, the state,
the panel, a copy of another version's). In a run, input scripts play
pads in instruction time (`pad 0 axis rightx 0.8`), and `BLUE_LOG_PADS=1`
logs every control with the mapping it went to and, for InputSprocket,
each call InputSprocket makes to the driver (but the task's and
Tickle's) and each of a game's needs with its kind, label and flags.
Unreal Tournament was played this way on Mac OS 8.6 (see
[findings.md](findings.md), "Unreal Tournament's needs"): run, turn,
look, fire, jump and slide, and its Options, Input Device dialog opened,
a control changed in the panel, OK, and back to the game.
