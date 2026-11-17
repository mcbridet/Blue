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

### CD and disk image formats (`third_party/`, but for `imgui/` and `fonts/`)

Blue reads CHD, compressed audio and compressed Mac images with these
libraries, built into it from their sources in `third_party/` (versions,
origins and the files kept: `third_party/README`). Their licences:

| Library | What Blue reads with it | Licence |
|---|---|---|
| libchdr | MAME's CHD images | BSD-3-Clause |
| the LZMA SDK's decoders (libchdr's copy, and `third_party/lzma/`) | CHD's cdlz codec; UDIF's xz chunks | public domain |
| miniz | CHD's cdzl and cdfl codecs; UDIF's zlib chunks | MIT |
| zstd's decoder | CHD's zstd and cdzs codecs | BSD-3-Clause |
| dr_flac (libchdr's copy), dr_mp3 (with minimp3) | CHD's cdfl codec; FLAC and MP3 tracks of cue sheets | public domain or MIT-0 (minimp3: CC0) |
| stb_vorbis | Ogg Vorbis tracks of cue sheets | public domain or MIT |
| bzip2 | UDIF's bzip2 chunks | bzip2's (BSD-style) |
| lzfse | UDIF's LZFSE chunks | BSD-3-Clause |

**libchdr** (`third_party/libchdr/`; parts derived from MAME, copyright Aaron
Giles, under the same terms):

Copyright Romain Tisserand
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

**The LZMA SDK** (`third_party/lzma/`, `third_party/libchdr/deps/lzma-26.02/`):
"LZMA SDK is written and placed in the public domain by Igor Pavlov."

**miniz** (`third_party/libchdr/deps/miniz-3.1.2/`):

Copyright 2013-2014 RAD Game Tools and Valve Software
Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC
All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

**zstd** (`third_party/libchdr/deps/zstd-1.5.7/`; zstd is also under the
GPL-2.0, at the user's choice):

BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta, nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

**dr_flac and dr_mp3** (`third_party/libchdr/include/dr_libs/dr_flac.h`,
`third_party/dr_libs/dr_mp3.h`), by David Reid, public domain or, at the
user's choice:

MIT No Attribution

Copyright 2023 David Reid

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

dr_mp3 is based on minimp3 (https://github.com/lieff/minimp3): "To the
extent possible under law, the author(s) have dedicated all copyright and
related and neighboring rights to this software to the public domain
worldwide. This software is distributed without any warranty." (CC0 1.0)

**stb_vorbis** (`third_party/stb/stb_vorbis.c`), by Sean Barrett, public
domain or, at the user's choice:

Copyright (c) 2017 Sean Barrett

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

**bzip2** (`third_party/bzip2/`):

This program, "bzip2", the associated library "libbzip2", and all
documentation, are copyright (C) 1996-2019 Julian R Seward.  All
rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. The origin of this software must not be misrepresented; you must
   not claim that you wrote the original software.  If you use this
   software in a product, an acknowledgment in the product
   documentation would be appreciated but is not required.

3. Altered source versions must be plainly marked as such, and must
   not be misrepresented as being the original software.

4. The name of the author may not be used to endorse or promote
   products derived from this software without specific prior written
   permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

**lzfse** (`third_party/lzfse/`):

Copyright (c) 2015-2016, Apple Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1.  Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

2.  Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

3.  Neither the name of the copyright holder(s) nor the names of any contributors
    may be used to endorse or promote products derived from this software without
    specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.

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
`Contents/Resources/Licenses/`; in an AppImage, `usr/share/doc/blue/Licenses/`).
(The libraries in Blue's source, above, are built into Blue itself, and
their source is Blue's, at the release's tag; the packages need nothing more
for them.)

| Library | Licence | In | Source |
|---|---|---|---|
| SDL2 | zlib | both | https://github.com/libsdl-org/SDL |
| libslirp | BSD-3-Clause | both | https://gitlab.freedesktop.org/slirp/libslirp |
| libusb | LGPL-2.1-or-later | both | https://github.com/libusb/libusb |
| GLib | LGPL-2.1-or-later | both | https://gitlab.gnome.org/GNOME/glib |
| proxy-libintl (GLib's libintl) | LGPL-2.0-or-later | Blue.app | https://github.com/frida/proxy-libintl |
| PCRE2 (built into GLib) | BSD-3-Clause (with the PCRE2 exception) | Blue.app | https://github.com/PCRE2Project/pcre2 |
| PCRE (GLib's) | BSD-3-Clause | the AppImages | https://www.pcre.org (Ubuntu 22.04's libpcre3) |

The released Blue.app's are built from their sources by
`tools/macos-deps.sh`; the AppImages' GLib, libslirp, libusb and PCRE are
Ubuntu 22.04's packages, their SDL2 built from source by
`tools/package-linux.sh` (their libusb loads the system's libudev, which
isn't bundled).
A Blue.app made with Homebrew's libraries (`make app`) carries Homebrew's
instead: SDL2 as sdl2-compat with SDL3 (zlib), and gettext's libintl
(LGPL-2.1-or-later, https://www.gnu.org/software/gettext/) and PCRE2 as
libraries of their own.

### libusb

Blue passes the host's USB devices through to Mac OS with libusb-1.0,
which it loads at run time (`src/usbhost.c` declares the parts of its
interface it uses; nothing of libusb is built into Blue). Blue.app and the
AppImages carry it as a separate library (`libusb-1.0.0.dylib`,
`libusb-1.0.so.0`), which can be replaced by another build of it; its
licence's full text goes with it (`Licenses/libusb/COPYING` in Blue.app;
`Licenses/libusb-1.0-0/` in an AppImage, with Debian's copyright file), and
is at https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html. Its source
is https://github.com/libusb/libusb (Blue.app's: the release tarball
https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2,
unchanged, configured without `pipe2()`; the AppImages': Ubuntu 22.04's
libusb-1.0 source package), and is in `Blue-third-party-sources.tar.gz`.

> Copyright © 2001 Johannes Erdfelt, © 2007-2009 Daniel Drake, © 2010-2012
> Peter Stuge, © 2008-2023 Nathan Hjelm, © 2009-2013 Pete Batard,
> © 2009-2013 Ludovic Rousseau, © 2010-2012 Michael Plante, © 2011-2013
> Hans de Goede, © 2012-2013 Martin Pieuchot, © 2012-2013 Toby Gray,
> © 2013-2018 Chris Dickens, and libusb's other authors (its `AUTHORS`).
>
> This library is free software; you can redistribute it and/or modify it
> under the terms of the GNU Lesser General Public License as published by
> the Free Software Foundation; either version 2.1 of the License, or (at
> your option) any later version.
>
> This library is distributed in the hope that it will be useful, but
> WITHOUT ANY WARRANTY; without even the implied warranty of
> MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser
> General Public License for more details.

### Their exact sources: `Blue-third-party-sources.tar.gz`

Every release carries `Blue-third-party-sources.tar.gz` beside the disk
images and AppImages (and in its `SHA256SUMS`): the exact corresponding
source of each library above, as the CI jobs that built the release's
packages used it. Each job keeps the sources of the libraries its package
carries (`tools/package-macos.sh sources`; `tools/package-linux.sh` with
`SOURCES`), and `tools/third-party-sources.sh` puts them in one archive,
each file once:

- **For Blue.app** (both disk images): the upstream tarballs
  `tools/macos-deps.sh` builds from, each checked by its SHA-256: SDL2
  2.32.10, GLib 2.90.1, libslirp 4.9.7 and libusb 1.0.30, and PCRE2 10.46 (with its Meson
  build files from WrapDB) and proxy-libintl 0.5 as GLib's own
  `subprojects` wraps fetch them (the wraps give their SHA-256); Blue's one
  change to GLib, as a patch (`glib-2.90.1-blue.patch`: its `meson.build`
  without the check for `pipe2()`, which macOS 11 lacks); and the scripts
  that built them and bundled them (`macos-deps.sh`, with every configure
  option, and `package-macos.sh`). `macos-deps.sh` builds them again from
  these files (the archive's `README.txt` says how). The cached build in CI
  keeps them in its `share/sources`.
- **For the AppImages:** Ubuntu 22.04's source packages (`.dsc`,
  `.orig.tar.*`, `.debian.tar.*`, by `apt-get source`, which checks them
  against Ubuntu's signed indexes) of exactly the versions bundled: glib2.0
  (for libglib2.0-0), libslirp (libslirp0), libusb-1.0 (libusb-1.0-0) and
  pcre3 (libpcre3); SDL2
  2.32.10's tarball; and `package-linux.sh`, which builds SDL2 and bundles
  them all (Ubuntu's libraries unchanged but for their run path). glibc,
  which provides libintl on Linux, is the system's, not bundled.

The archive's `README.txt` names each library, its version and licence, the
packages it's in (and as which file), and each source file with where it
came from; its `SHA256SUMS` sums them. Each project's own address is in the
table above.
