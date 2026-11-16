# Building, testing and packaging

How to build Blue from source, run its tests, and make the packages that
releases carry. To install a release instead, see the
[README](../README.md#download-and-install).

## Building

Ready-made builds are at https://github.com/mcbridet/Blue/releases:
`Blue-arm64.dmg` and `Blue-x86_64.dmg` (macOS 11 or later, on Apple
silicon and Intel), `Blue-x86_64.AppImage` and `Blue-aarch64.AppImage` (Linux with glibc 2.35
or later: Ubuntu 22.04, Debian 12, Fedora 36 and since), with `SHA256SUMS`
(see [Installing](#installing), below). To build it yourself:

```
git clone https://github.com/mcbridet/Blue
cd Blue
make            # build/blue (C11, and C++17 for the menu: Dear ImGui is in third_party/imgui)
```

Building needs only a C11 and C++17 compiler (Clang or GCC) and `make`:
Dear ImGui and the window's font are in the tree, and SDL2 and libslirp
aren't build dependencies, since Blue loads them when it runs
(`src/hostlib.c`). To run your build with a window, sound and the
network, install them:

| Host | Packages |
|---|---|
| macOS (Homebrew) | `brew install sdl2 libslirp` |
| Debian, Ubuntu | `sudo apt install libsdl2-2.0-0 libslirp0` |
| Fedora | `sudo dnf install SDL2 libslirp` |

The menu (F12) needs SDL 2.0.18 or later. On Linux, Insert... in the menu
uses zenity or kdialog, if one is installed; the 3D engine and OpenGL draw
on the GPU with OpenGL 3.3, and in software without it. `build/blue
--version` says which SDL2 and libslirp Blue finds, and where.

## Tests

```
make test       # unit tests; the ROM test uses ./MOS.rom or $BLUE_TEST_ROM if present,
                # the GPU test any OpenGL 3.3 there is (else it's skipped)
BLUE_JIT_PROGRAMS=200000 build/blue-tests jit    # longer JIT-vs-interpreter fuzzing
tools/jit-timing.sh     # times the JIT here (its benchmark, the CD boots there are) into a log
```

`build/blue-tests NAME` runs one suite of the tests (`jit`, `opengl`,
`share` and the rest: `tests/main.c` lists them).

### Testing the guest's OpenGL

The guest's OpenGL is checked in the guest too, by a test application of
Blue's own, "Blue GL Test" (`src/gltest.c`: its logic is C in Blue
calling the guest's OpenGLLibrary and OpenGLUtility through the Code
Fragment Manager's linking, as a game would). `BLUE_TOOLS_GL_TEST=1
build/blue tools create FILE` puts it on the tools volume; with the OpenGL
libraries in a disk image's Extensions folder and it in Startup Items
(hfsutils' `hcopy -m` copies them), a run in instruction time shows its
window with the scene `BLUE_GL_TEST_SCENE` names (`clear`: a clear, and a
scissored one; `triangle`: in perspective, a floor cut by the near plane, a
triangle and a quad crossing in depth, a flat-shaded fan; `lit`: a bump
drawn from vertex and normal arrays, lit, fogged, and a quad drawn through
the stencil; `lists`: quadrics made into display lists through
OpenGLUtility, called lit and as lines; `pixels`: an image zoomed, glyphs
by `glBitmap`, a copy by `glCopyPixels`; `fullscreen`: `triangle` drawn
on the whole screen, switched to 640 x 480 by `aglSetFullScreen`; `font`:
lines of text in two fonts by `aglUseFont`'s lists; `lines`: the polygon
stipple, a logic op, stippled, wide, antialiased and textured lines and
points; `accum`: a context of the generic renderer, its accumulation buffer
blurring a turning bar) at the depth
`BLUE_GL_TEST_DEPTH` sets (16 or 32), the same on every host (with the
same time zone: the guest's clock is the host's local time). On Linux,
`make test` draws scenes through the libraries and compares them with Mesa's
own OpenGL (its compatibility profile, which still has `glBegin`):
`BLUE_TEST_VERBOSE=1` says how far apart they are, and
`BLUE_TEST_SAVE=DIR` saves them as PNGs; `BLUE_TEST_GLU=PATH` names
Mesa's libGLU (`libGLU.so.1`), to compare OpenGLUtility's scene too.
`tools/gltable.py` made `src/opengl_table.h`, the libraries' functions,
from Apple's (Mac OS 9.2.2's) and the macOS SDK's OpenGL headers.

## CI and releases

CI (`.github/workflows/ci.yml`) builds and tests on Linux and macOS, each on
x86_64 and AArch64, with warnings as errors; it also fuzzes the JIT with
100,000 programs (`BLUE_TEST_JIT=1` makes a missing JIT a failure) and
prints its synthetic benchmark (`BLUE_JIT_BENCH=1 build/blue-tests jit`:
guest instructions a second for a few kernels, interpreted and
translated), and on Linux the GPU backend
is tested on Mesa's
software OpenGL (llvmpipe; `BLUE_TEST_GPU=1` makes the test fail rather than
skip without a context). Each run also packages Blue (see [Packaging](#packaging)): on
each Mac a Blue.app in its disk image (`Blue-arm64.dmg`, `Blue-x86_64.dmg`;
its libraries built by `tools/macos-deps.sh`, cached until the script
changes), and on each Linux runner an AppImage, built in an Ubuntu 22.04
container; each is run (`--version`, which must find its own libraries),
and the app must need no newer macOS than 11. Each build job also keeps
the exact sources of the libraries its package carries (an artifact,
`sources-<target>`), and a `sources` job puts them together into
`Blue-third-party-sources.tar.gz` on every run (see
[Their exact sources](#their-exact-sources)). Pushing a `v*` tag
publishes the two disk images, `Blue-x86_64.AppImage`,
`Blue-aarch64.AppImage`, `Blue-third-party-sources.tar.gz` and
`SHA256SUMS` (the sums of them all) as a GitHub release, once the tag
is `src/version.h`'s version (`v2026.11.1` for `2026.11.1`: year, month,
then the release within the month): the names have no
version in them, so `releases/latest/download/Blue-arm64.dmg` and the like
are always the newest.

## Icons and desktop files

Blue's icons come from `art/`: `art/blue.svg`, the master (the stack on its
tile), and `art/finder-icons.txt`, the Finder icons in Mac OS as pixel art
in Mac OS's palettes. `tools/icons.py` makes the rest from them (with
Python and headless Chrome, which building doesn't need: what it makes is
committed): the app icon as PNGs (`art/png/blue-16.png` to
`blue-1024.png`), the window's (`src/icon_data.inc`, 256 x 256, which SDL
on macOS shows in the Dock too) and the Finder's (`src/finder_icons.inc`,
previewed in `art/finder-icons.png`). On macOS the executable carries an
Info.plist (`art/Info.plist`, with the version from `src/version.h` and the
oldest macOS filled in by the Makefile: `build/Info.plist`, Blue.app's
too), so the menu bar and the Dock say Blue; its bundle identifier is
`io.github.mcbridet.Blue`, and Blue.app's icon is made from the PNGs by
`iconutil`. On Linux the window's class and app ID are `blue`, which
`art/blue.desktop` and the icons go with: the AppImages carry them, for
AppImage integration tools; for a build of your own, copy the desktop
entry to `~/.local/share/applications` and `art/png/blue-N.png` to
`~/.local/share/icons/hicolor/NxN/apps/blue.png`.

## Installing

**macOS** (11 or later): open `Blue-arm64.dmg` (Apple silicon) or
`Blue-x86_64.dmg` (Intel) and drag Blue to Applications. Blue.app is
signed ad hoc, not by a registered developer, and isn't notarised, so the
first time macOS won't open it: right-click it and choose Open (macOS 14
and earlier), or, once macOS has refused it, click Open Anyway in System
Settings > Privacy & Security (macOS 15 and later). After that it opens
from the Finder or Terminal. (`xattr -dr com.apple.quarantine
/Applications/Blue.app` does the same from Terminal.) Opened from the
Finder, Blue shows a small window: it's started from the command line for
now (a launcher is coming: see [roadmap.md](roadmap.md)), with the command for this copy
of it and a button that copies it. In Terminal, Blue is
`/Applications/Blue.app/Contents/MacOS/blue`, with the commands and
options in the [README](../README.md#command-line-reference); a link to it in your PATH works too (`ln -s
/Applications/Blue.app/Contents/MacOS/blue /usr/local/bin/blue`).

**Linux** (x86_64 or AArch64, glibc 2.35 or later): `chmod +x Blue-x86_64.AppImage` (or
`Blue-aarch64.AppImage`), then run it as Blue itself:
`./Blue-x86_64.AppImage boot --cdrom 85.iso --window`. It mounts itself with
FUSE, for which it needs `fusermount` (the `fuse` or `fuse3` package, which
most desktops have; not libfuse2); without FUSE, in a container say,
`./Blue-x86_64.AppImage --appimage-extract-and-run boot ...` unpacks it into
a temporary directory each time instead (or `--appimage-extract` once, then
`squashfs-root/AppRun boot ...`). Opened from a file manager it shows the
same window as the app on macOS. It holds Blue's desktop entry and icons,
for AppImage integration tools (AppImageLauncher, Gear Lever).

## What's in the packages

Blue, and the libraries it loads at run time
(`src/hostlib.c`): SDL2 (window, sound, input) and libslirp (networking),
with GLib, which libslirp needs, and what GLib needs (on macOS its libintl,
proxy-libintl, and PCRE2 built into it; on Linux PCRE), in Blue.app's
`Contents/Frameworks` and the AppImage's `usr/lib`. Blue looks for each
there first (`../Frameworks` or `../lib` from the executable's real
directory), then where the system finds it, so `build/blue` takes the
system's or Homebrew's; `blue --version` says which SDL2 and libslirp it
found, and where, and `BLUE_LOG_LIBS=1` logs it as they're loaded. Nothing
else is bundled: SDL2 loads X11 or Wayland, ALSA, PulseAudio or PipeWire
and OpenGL from the system, as Blue does OpenGL and EGL, and glibc is the
system's. The libraries' licences are in Blue.app's
`Contents/Resources/Licenses` and the AppImage's `usr/share/doc/blue/Licenses`
(beside Blue's `LICENSE` and `THIRD-PARTY-NOTICES.md`; also `LICENSE` in the
disk image), with a `README.txt` giving each one's version and source. They
stay separate, replaceable libraries, as the LGPL asks of GLib and
libintl: put another build of one in its place, under the same name (in
Blue.app, then sign it again: `codesign --force --deep -s - Blue.app`; an
AppImage, `--appimage-extract` it, replace the file in
`squashfs-root/usr/lib` and run `squashfs-root/AppRun`). Their sources, as
released: on macOS, built from source by `tools/macos-deps.sh`, SDL2
2.32.10 (https://github.com/libsdl-org/SDL/releases/tag/release-2.32.10),
GLib 2.90.1 (https://download.gnome.org/sources/glib/2.90/), with PCRE2
10.46 and proxy-libintl 0.5 from GLib's own `subprojects` wraps
(https://github.com/PCRE2Project/pcre2/releases/tag/pcre2-10.46,
https://github.com/frida/proxy-libintl/releases/tag/0.5), and libslirp
4.9.7 (https://gitlab.freedesktop.org/slirp/libslirp/-/tags/v4.9.7); on
Linux, SDL2 2.32.10 built by `tools/package-linux.sh`, and Ubuntu 22.04's
packages of the rest (libslirp0, libglib2.0-0, libpcre3: their sources by
`apt-get source`, or at https://launchpad.net/ubuntu/+source/, as
`README.txt` gives them). Blue's own source is this repository, at the
release's tag.

### Their exact sources

Each release also carries
`Blue-third-party-sources.tar.gz` (in its `SHA256SUMS` too), with the exact
source of every library in its packages, as the release's builds used it:
for Blue.app, the upstream tarballs its libraries are built from, Blue's
patch to GLib and `tools/macos-deps.sh`, which builds them; for the
AppImages, Ubuntu's source packages (`.dsc`, `.orig.tar.*`,
`.debian.tar.*`) of the versions they carry, and SDL2's tarball. Its
`README.txt` gives each library's version, licence and packages (see
[THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md#their-exact-sources-blue-third-party-sourcestargz)).

## Packaging

The version is `src/version.h`'s (`BLUE_VERSION`), which `blue --version`
and the usage show, and the Makefile puts in the Info.plist
(`CFBundleShortVersionString`, `CFBundleVersion`); a release's tag must be
it, with a `v`.

```
make app                         # build/Blue.app (macOS), with Homebrew's SDL2 and libslirp
make dmg                         # build/Blue-arm64.dmg (or x86_64): Blue.app, a link to Applications, LICENSE
tools/macos-deps.sh ~/blue-deps  # SDL2, GLib and libslirp from their sources, for macOS 11 (a few minutes)
make app DEPS=~/blue-deps        # ... Blue.app with those, as released
podman run --rm -v "$PWD:/src" -e BUILD=/tmp/appimage -e OUT=/src/build ubuntu:22.04 \
        /src/tools/package-linux.sh --setup          # build/Blue-<arch>.AppImage, built in Ubuntu 22.04
```

The exact sources of the libraries a package carries, for
`Blue-third-party-sources.tar.gz`:

```
tools/package-macos.sh sources build/Blue.app ~/blue-deps Blue-arm64.dmg OUT   # Blue.app's, as macos-deps.sh kept them
podman run ... -e SOURCES=/src/build/sources ubuntu:22.04 /src/tools/package-linux.sh --setup   # the AppImage's too
tools/third-party-sources.sh Blue-third-party-sources.tar.gz DIR...               # those directories in one archive
```

`tools/macos-deps.sh` keeps the sources it builds from in
`PREFIX/share/sources` (each tarball checked by its SHA-256, and fetched
only if it isn't there; PCRE2 and proxy-libintl as Meson fetched them for
GLib's wraps; Blue's change to GLib as a patch; and the script itself),
and `tools/package-macos.sh sources` takes those of the libraries an app
carries. `tools/package-linux.sh`, with `SOURCES` set to a directory,
keeps SDL2's tarball and Ubuntu's source packages of the versions it
bundles (by `apt-get source`, which needs `deb-src`: `--setup` adds it).
Each writes a `SOURCES` file listing them, and
`tools/third-party-sources.sh` puts the directories together, each file
once, with a `README.txt` and a `SHA256SUMS`.

On macOS the Makefile builds for `MACOSX_DEPLOYMENT_TARGET` (11.0 unless
set; a build directory from before needs `make clean` once, or the linker
warns of objects built for a newer macOS), and links the executable with
an `LC_RPATH` of `@executable_path/../Frameworks`. `make app` (`DEPS`:
`brew --prefix` unless given; `brew install sdl2 libslirp`) copies SDL2,
libslirp and every library they load but the system's into
`Contents/Frameworks`, as `@rpath/NAME` with an `LC_RPATH` of
`@loader_path`, and their licences (from `DEPS/share/licenses`, which
`tools/macos-deps.sh` writes, or the Homebrew kegs), sets
`LSMinimumSystemVersion` to the newest minimum of the executable and the
libraries (`otool -l`'s `LC_BUILD_VERSION`), and signs it ad hoc, the
libraries first (`codesign --force -s -`), checking it with `codesign
--verify --deep --strict`. Homebrew's libraries need the macOS they were
built for (26 or 27 on a Mac with macOS 27, so the app does too; `make app`
says), and Homebrew's SDL2 is sdl2-compat, over SDL3, which then goes in
too; `tools/macos-deps.sh` builds them for macOS 11 (`ARCH=x86_64` builds
Intel's on Apple silicon, its checks run in Rosetta), as CI does.
Without the hardened runtime, the JIT needs no entitlement. The disk image
is named for the executable's architecture (`lipo -archs`): `make dmg
BUILD=build/x86 CC='cc -arch x86_64' CXX='c++ -arch x86_64'
DEPS=~/blue-deps-x86_64` makes Intel's on Apple silicon. `tools/package-linux.sh` builds Blue with GCC 12
(Ubuntu 22.04's `gcc-12`), libstdc++ and libgcc linked in, so that glibc
is all it needs of the system; with `--setup` it first installs what it
needs and builds SDL2 2.32.10 into `/opt/sdl2`, its backends (X11, Wayland,
ALSA, PulseAudio, PipeWire, KMS/DRM) all loaded at run time. It bundles
SDL2, libslirp and what they load but glibc, and nothing outside a short
list (to keep the system's graphics and sound stacks out), each with a run
path of `$ORIGIN`, and puts them in a squashfs behind the AppImage
project's static type2-runtime (release 20251108, checked by SHA-256).
