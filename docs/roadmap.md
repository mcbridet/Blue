# Roadmap

What's next for Blue. What was found along the way, and how Mac OS was
brought up, are in [findings.md](findings.md) and [bring-up.md](bring-up.md).

## Next steps

1. A launcher: a settings window for the disks, CDs, RAM, ROM and shared
   folder, remembered between launches (in Blue's settings file, which
   keeps the game controllers' mappings already: see
   [input.md](input.md#the-settings-file)), for Blue.app and the AppImages
   opened from the desktop (which for now show a window saying how to start
   Blue from the command line).
2. Host ports forwarded to the guest, so that it can serve.

## When a program needs them

- What Blue's OpenGL leaves out ([3d.md](3d.md#what-it-leaves-out)):
  OpenGLUtility's tessellators and NURBS renderers, antialiased polygons,
  auxiliary buffers and colour index.
- OpenGL faster for games short of time: frames read back from the GPU
  asynchronously while the guest waits for the frame's turn, and a vertex
  cache in `glDrawElements` (about 0.25 ms a frame in Quake III).
- Exceptions in Multiprocessing Services tasks other than the blue one,
  sent to the task's exception queue as NanoKernel 2 does (with
  `MPSetExceptionHandler` and `MPThrowException`). Such a fault stops the
  run; none of Mac OS 9.1's or 9.2.2's tasks makes one.
- USB ([devices.md](devices.md#usb)): a USB 1.1 floppy drive's command
  set (UFI, over control/bulk/interrupt) for the test disk, to test Mac
  OS's floppy support as its bulk-only disk tests its mass storage; the
  host's high-speed bulk-only devices presented as full speed (their
  descriptors' packet sizes rewritten, transfers split and joined), if a
  device of the time is found that only connects at high speed; and the
  devices passed through remembered in the settings file, if connecting
  them each time proves a chore.
