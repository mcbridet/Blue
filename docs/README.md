# Blue's documentation

The [README](../README.md) tells how to install and use Blue. These
documents go further: how Blue works, how it was made, and how to build,
test and debug it.

**How Blue works**

- [architecture.md](architecture.md): the design: the NanoKernel interface,
  the loader, the kernel, time, and the machine Mac OS sees.
- [devices.md](devices.md): the iMac's devices and Blue's drivers: interrupts,
  DMA, the display, sound, networking and USB.
- [drives.md](drives.md): disks, CD-ROM and floppy drives, cue sheets and CD
  audio.
- [shared-folder.md](shared-folder.md): a host folder shown to Mac OS as a
  disk, and how its changes go back.
- [window.md](window.md): the window, its scale, the menu, the prompts and the
  status bar.
- [guest-tools.md](guest-tools.md): the guest tools and their installer: Blue
  Enhancements, the clipboard, the 3D engine, OpenGL and the Pippin library.
- [3d.md](3d.md): 3D: the RAVE engine and QuickDraw 3D, Blue's OpenGL, the
  software rasterizer and the GPU backend.
- [jit.md](jit.md): the PowerPC JIT for AArch64 and x86-64, and its
  measurements.
- [bring-up.md](bring-up.md): how Mac OS 8.5, 9.0, 9.1 and 9.2.2 were made to
  start up.
- [findings.md](findings.md): what was found along the way, in detail:
  Apple's ROMs and NanoKernels, Mac OS, programs, hosts, and measurements of
  Blue; with references.
- [software.md](software.md): notes on particular programs and games:
  Bugdom, Unreal Tournament, MacBench, Pippin titles.

**Working on Blue**

- [building.md](building.md): building, the tests, CI, packaging and
  releases.
- [debugging.md](debugging.md): instruction time, input scripts, tracing,
  the `BLUE_*` environment variables, and the tools.
- [layout.md](layout.md): what's where in the source.
- [qemu-reference.md](qemu-reference.md): QEMU as the reference for Apple's
  Trampoline and NanoKernel.
- [nk2-reference.md](nk2-reference.md): what Apple's NanoKernel 2 does,
  found under QEMU.

**What's next**

- [roadmap.md](roadmap.md): the next steps.
- [future.md](future.md): ideas further off: a modem, a serial port,
  infrared, 3D sound, sound input, AltiVec, 3dfx Glide, pictures on the
  clipboard, printing, game controllers and the CD formats still missing.
