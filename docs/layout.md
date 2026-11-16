# Source layout

What's where in Blue's source tree.

```
src/      be.h, lzss.c, rom.c + parcels.c, mem.c (flat 4 GB guest space, its page
          flags, pages watched for writes),
          ppc.c + ppc_exec.inc + ppc_fp.c (interpreter), jit.c (the JIT's
          runtime), jit_a64.c + a64.h and jit_x86.c + x86.h (its AArch64
          and x86-64 translators), nk.c (native kernel), clock.c (the guest's wall clock), mp.c (its Multiprocessing
          Services, NanoKernel 2's), priv.c (supervisor mode: the skeleton key),
          bootinfo.c + devtree.c (Trampoline replacement, device tree),
          rtas.c (clock/NVRAM/power/PCI configuration), mmio.c, macio.c,
          dbdma.c, via.c, cuda.c, adb.c, scc.c, awacs.c, ohci.c (USB), pci.c
          (configuration space), nvram.c (devices), ndrv.c (PEF fragment
          generator: drivers, the guest tools),
          video.c, disk.c (native drivers), floppy.c (floppy images),
          drives.c (removable drives: where images go, the menu),
          cdrom.c (the CD-ROM driver's calls), cdimage.c (CD images: cue
          sheets, the table of contents), cdaudio.c (the drive's player),
          net.c (Ethernet's lanLib,
          libslirp), hfs.c (HFS volume writer), hfsread.c (HFS and HFS+
          reader: the startup disk's ROM file), startup.c (which disk is
          the startup disk), ofnvram.c (Open Firmware's variables in
          NVRAM: boot-device), share.c (shared folder),
          rsrc.c (resource forks), tools.c (guest tools), pippin.c (their
          PippinPPC, for Pippin titles), install.c
          (installing them from inside Mac OS), clip.c (the clipboard they
          share with the host), rave.c (3D engine), opengl.c +
          opengl_table.h (the guest's OpenGL libraries), glfront.c + gltex.c + gllight.c +
          glarray.c + glattrib.c (their fixed-function front end: textures, lighting and the rest of each
          vertex's work, client arrays, the attribute stacks), gllist.c (display lists),
          glpixel.c (the pixel paths), glselect.c (selection and feedback),
          gleval.c (evaluators), glu.c (OpenGLUtility, as SGI's GLU), gltest.c (their test application),
          surface.c + raster.c + gpu.c + gl.c (the 3D backend:
          drawing, in software and with OpenGL, and presenting), display.c (SDL2
          window, on the main thread, and what the Mac hands it), density.c (displays'
          pixels per inch, for its scale), ui.cpp (its menu and prompts, with Dear ImGui),
          chooser.c, chooser_mac.m (the host's file chooser, for the menu's Insert...), audio.c (SDL2 sound), png.c,
          perf.c (host processor counters), env.c (settings' old names),
          icon.c (the window's icon), font.c (its font), hostlib.c (where
          SDL2 and libslirp are loaded from), welcome.c (the window shown
          without a command), version.h, main.c
tests/    unit tests with a small instruction encoder (ppcasm.h); test_jit.c
          runs random programs on both engines and compares the results
tools/    qemu-ref/ reference-boot scripts, pef.py (PEF inspector),
          devtree.py (device-tree copy dumper), disasm.py (disassembler),
          heap.py (heap zones in a RAM dump, checked),
          gltable.py (the OpenGL libraries' functions, from Apple's),
          icons.py (the icons, from art/), fonts.py (the window's font,
          from third_party/fonts/), package-macos.sh (Blue.app and its
          disk image), macos-deps.sh (its libraries, from their sources),
          package-linux.sh (the AppImage), third-party-sources.sh
          (Blue-third-party-sources.tar.gz: the packages' libraries'
          sources, in one archive), jit-timing.sh (the JIT's speed
          here, into a log), check-links.py (the docs' links)
art/      blue.svg (the icon's master), finder-icons.txt (the Finder
          icons' pixel art), png/ (the icon at each size), Info.plist
          (macOS: the executable's and Blue.app's), blue.desktop (Linux)
docs/     the documentation (README.md lists it): how Blue works,
          building, debugging, the plans (plan.md, future.md, roadmap.md,
          x86-jit-plan.md, gpu-plan.md, opengl-plan.md, glide-plan.md),
          nk2-reference.md (Mac OS 9.1 under QEMU: what Apple's NanoKernel 2
          does), images/ (the README's screenshots)
site/     the website (index.html, style.css, main.js, its icons)
.github/  workflows/ci.yml (build, test, package, the sources archive,
          and release),
          workflows/pages.yml (publishes site/)
third_party/imgui/  Dear ImGui 1.93.0 (MIT; its core files only, as released)
third_party/fonts/  ChicagoFLF 2.1 (public domain; its notice in README there)
```

## Files you supply

ROMs and OS images are user-supplied and ignored by git: `MOS.rom` (a 1.x Mac OS ROM
file: see [architecture.md](architecture.md#what-blue-targets)) and `85.iso` (retail Mac OS 8.5 CD, a bare bootable HFS volume);
`MOS91.rom` and `91.iso` for Mac OS 9.1 (see
[bring-up.md](bring-up.md#mac-os-91-from-its-cd)); `922.iso` for 9.2.2. The ROM files are only
needed with `--rom` (and for the ROM test and tools): otherwise Blue takes
the one in the startup disk's System Folder.
