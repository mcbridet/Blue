# Third-party notices

Blue is Copyright (C) 2026 the Blue authors, and free software under the
GNU General Public License, version 2 only (GPL-2.0-only): see
[LICENSE](LICENSE). It comes with no warranty.

Blue includes no Apple software. Mac OS, Macintosh, Power Mac, QuickDraw
and Pippin are trademarks of Apple Inc.; Blue is not affiliated with Apple.

## In Blue's source

### Dear ImGui (`third_party/imgui/`)

The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

(Dear ImGui includes stb_truetype, stb_rect_pack and stb_textedit by Sean
Barrett, which are public domain or MIT, at the user's choice.)

### ChicagoFLF (`third_party/fonts/`, `src/font_data.inc`)

ChicagoFLF 2.1, by Robin Casady, a TrueType revival of Susan Kare's Chicago.
Public domain; its author's statement, from the font's own name table:

> The FLF were created by various authors including myself and Mike Wright.
> When Casady & Greene, Inc. closed the rights went back to the authors. I
> created ChicagoFLF and CheckboxFLF. You are free to use them as you wish.
> I consider those two in the public domain.

### OpenGL Utility library (`src/glu.c`)

Blue's OpenGLUtility follows SGI's GLU (1.2, and 1.3 as in Mesa's libGLU) in
its algorithms and results: its matrices, image scaling and mipmaps, and
quadrics. SGI's GLU is under this licence:

SGI FREE SOFTWARE LICENSE B (Version 2.0, Sept. 18, 2008)
Copyright (C) 1991-2000 Silicon Graphics, Inc. All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice including the dates of first publication and
either this permission notice or a reference to
http://oss.sgi.com/projects/FreeB/
shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
SILICON GRAPHICS, INC. BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

Except as contained in this notice, the name of Silicon Graphics, Inc.
shall not be used in advertising or otherwise to promote the sale, use or
other dealings in this Software without prior written authorization from
Silicon Graphics, Inc.

## References (no code used)

Blue's kernel follows the interfaces and behaviour documented by these, as
facts needed for compatibility; none of their code is in Blue:

- Elliot Nunn's reverse-engineered NanoKernel 1
  (https://github.com/elliotnunn/NanoKernel) and NanoKernel 2
  (https://github.com/elliotnunn/powermac-rom): the kernel's data
  structures' offsets, Multiprocessing Services' call numbers, arguments
  and objects, and how tasks (the blue task among them) are scheduled.
- tbxi (https://github.com/elliotnunn/tbxi, MIT): the ROM file's parcels'
  layout and flags.
- Apple's `PPCInfoRecordsPriv.h`, published with Darwin: the info records'
  offsets.
- QEMU and OpenBIOS: reference runs of Apple's own NanoKernel 2
  (`tools/qemu-ref/`, `docs/nk2-reference.md`).
- The x86-64 encoder's tests compare with encodings LLVM's assembler
  produces.

## Bundled in Blue.app and the AppImages

Built from source, Blue loads these from the system at run time. The app
and the AppImages carry them as separate, replaceable libraries, with their
licence texts and each one's version and source (in Blue.app,
`Contents/Resources/Licenses/`; in an AppImage, `usr/share/doc/blue/Licenses/`):

| Library | Licence | In | Source |
|---|---|---|---|
| SDL2 | zlib | both | https://github.com/libsdl-org/SDL |
| libslirp | BSD-3-Clause | both | https://gitlab.freedesktop.org/slirp/libslirp |
| GLib | LGPL-2.1-or-later | both | https://gitlab.gnome.org/GNOME/glib |
| proxy-libintl (GLib's libintl) | LGPL-2.0-or-later | Blue.app | https://github.com/frida/proxy-libintl |
| PCRE2 (built into GLib) | BSD-3-Clause (with the PCRE2 exception) | Blue.app | https://github.com/PCRE2Project/pcre2 |
| PCRE (GLib's) | BSD-3-Clause | the AppImages | https://www.pcre.org (Ubuntu 22.04's libpcre3) |

The released Blue.app's are built from their sources by
`tools/macos-deps.sh`; the AppImages' GLib, libslirp and PCRE are Ubuntu
22.04's packages, their SDL2 built from source by `tools/package-linux.sh`.
A Blue.app made with Homebrew's libraries (`make app`) carries Homebrew's
instead: SDL2 as sdl2-compat with SDL3 (zlib), and gettext's libintl
(LGPL-2.1-or-later, https://www.gnu.org/software/gettext/) and PCRE2 as
libraries of their own.
