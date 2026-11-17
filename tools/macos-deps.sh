#!/bin/sh
# The libraries Blue.app bundles, built from their sources for an older macOS
# than Homebrew's (whose minimum is the macOS they were built on): SDL2,
# libslirp with GLib (PCRE2 built into it, and proxy-libintl for its libintl),
# and libusb.
# Into PREFIX, for MACOSX_DEPLOYMENT_TARGET (11.0 unless set) and this Mac's
# architecture or ARCH's (x86_64 on Apple silicon runs its checks in
# Rosetta), with their licences in PREFIX/share/licenses. Needs curl, cmake,
# meson, ninja, pkg-config and make; `make app DEPS=PREFIX` bundles them.
#
# Their exact sources stay in PREFIX/share/sources (so a cache of PREFIX
# keeps them): each tarball, checked by SHA-256 and fetched only if it isn't
# there already; those of GLib's subprojects that go into the libraries
# (PCRE2, proxy-libintl), as Meson fetched them for GLib's wraps (which give
# their SHA-256); Blue's change to GLib, as a patch; and this script. Its
# SOURCES says which library each is for (`tools/package-macos.sh sources`
# takes those of the libraries an app carries), SHA256SUMS their sums.
set -eu

SDL2_VERSION=2.32.10
SDL2_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
GLIB_VERSION=2.90.1
GLIB_SHA256=93c941aa17d5eb1d53fe838365f29a8b4e539c222a256d974ec8f30fc413e396
SLIRP_VERSION=4.9.7
SLIRP_SHA256=d2567fa06a6357c7755d61c67293b2c0bf069b3f2173bde6b76cf768529a1938
USB_VERSION=1.0.30
USB_SHA256=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf

if [ $# -ne 1 ]; then
	echo "usage: $0 PREFIX" >&2
	exit 2
fi
mkdir -p "$1"
prefix=$(cd "$1" && pwd)
: "${MACOSX_DEPLOYMENT_TARGET:=11.0}"
export MACOSX_DEPLOYMENT_TARGET
# (what's built here only, not Homebrew's)
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
unset PKG_CONFIG_PATH CPATH LIBRARY_PATH 2>/dev/null || true
jobs=$(sysctl -n hw.ncpu)
arch=${ARCH:-$(uname -m)}
# (a function newer than MACOSX_DEPLOYMENT_TARGET would be missing on the older macOS, where calling it crashes)
flags="-Werror=unguarded-availability-new"
[ "$arch" = "$(uname -m)" ] || flags="$flags -arch $arch"
# Only these flags: the libraries aren't Blue's, and an inherited -Werror (CI builds Blue with it) would stop
# them on any warning another compiler raises
export CFLAGS="$flags" CXXFLAGS="$flags" OBJCFLAGS="$flags"
[ "$arch" = "$(uname -m)" ] || export LDFLAGS="-arch $arch ${LDFLAGS:-}"
work=$(mktemp -d "${TMPDIR:-/tmp}/blue-deps.XXXXXX")
trap 'rm -rf "$work"' EXIT

# For another architecture, Meson needs telling (else it takes this Mac's for the libraries' own code)
cross=
if [ "$arch" != "$(uname -m)" ]; then
	case $arch in arm64) family=aarch64 ;; *) family=$arch ;; esac
	cat > "$work/cross.ini" <<- EOF
		[binaries]
		c = ['cc', '-arch', '$arch']
		cpp = ['c++', '-arch', '$arch']
		objc = ['cc', '-arch', '$arch']
		objcpp = ['c++', '-arch', '$arch']
		ar = 'ar'
		strip = 'strip'
		pkg-config = 'pkg-config'
		[built-in options]
		c_args = ['-Werror=unguarded-availability-new']
		objc_args = ['-Werror=unguarded-availability-new']
		[properties]
		needs_exe_wrapper = false
		[host_machine]
		system = 'darwin'
		subsystem = 'macos'
		kernel = 'xnu'
		cpu_family = '$family'
		cpu = '$arch'
		endian = 'little'
	EOF
	cross=--cross-file=$work/cross.ini
fi

die() {
	echo "macos-deps: $*" >&2
	exit 1
}

src=$prefix/share/sources
mkdir -p "$src"
cmp -s "$0" "$src/macos-deps.sh" || cp "$0" "$src/macos-deps.sh"
: > "$work/SOURCES"

# Is FILE in share/sources, with the SHA-256 given?
have() {
	echo "$2  $src/$1" | shasum -a 256 -c - > /dev/null 2>&1
}

# A tarball from URL into share/sources as FILE (unless it's there already), checked by SHA-256, and unpacked
fetch() {
	if ! have "$2" "$3"; then
		curl -sSfL -o "$work/$2" "$1"
		echo "$3  $work/$2" | shasum -a 256 -c - > /dev/null || die "$2: not the file expected (SHA-256)"
		mv "$work/$2" "$src/$2"
	fi
	tar -xf "$src/$2" -C "$work"
}

# A line of share/sources/SOURCES: the dylib it goes into, library, version, licence (SPDX), file, origin
source_of() {
	printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >> "$work/SOURCES"
}

licence() {
	to=$prefix/share/licenses/$1
	shift
	mkdir -p "$to"
	cp "$@" "$to/"
}

recipe="Blue's tools/macos-deps.sh, which built it"

echo "macos-deps: SDL2 ($(date +%H:%M:%S))"
sdl2_url=https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz
fetch "$sdl2_url" "SDL2-$SDL2_VERSION.tar.gz" "$SDL2_SHA256"
cmake -S "$work/SDL2-$SDL2_VERSION" -B "$work/sdl2" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
      -DCMAKE_OSX_ARCHITECTURES="$arch" \
      -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
cmake --build "$work/sdl2" -j "$jobs"
cmake --install "$work/sdl2"
licence SDL2 "$work/SDL2-$SDL2_VERSION/LICENSE.txt"
source_of libSDL2-2.0.0.dylib SDL2 "$SDL2_VERSION" Zlib "SDL2-$SDL2_VERSION.tar.gz" "$sdl2_url"
source_of libSDL2-2.0.0.dylib SDL2 "$SDL2_VERSION" Zlib macos-deps.sh "$recipe"

echo "macos-deps: GLib ($(date +%H:%M:%S))"
glib_url=https://download.gnome.org/sources/glib/${GLIB_VERSION%.*}/glib-$GLIB_VERSION.tar.xz
glib=$work/glib-$GLIB_VERSION
fetch "$glib_url" "glib-$GLIB_VERSION.tar.xz" "$GLIB_SHA256"
# GLib uses pipe2() if the SDK has it (macOS 27's does), which older macOS hasn't: pipe() instead
# (the change kept as a patch too)
patch=glib-$GLIB_VERSION-blue.patch
cp "$glib/meson.build" "$work/meson.build.orig"
sed "/^  'pipe2',$/d" "$work/meson.build.orig" > "$glib/meson.build"
! grep -q "'pipe2'" "$glib/meson.build" || die "GLib's build still looks for pipe2()"
cat > "$src/$patch" << EOF
Blue's change to GLib $GLIB_VERSION, which its tools/macos-deps.sh makes: no check for pipe2(),
which macOS 11 lacks though newer SDKs declare it (GLib would call it, weakly linked, and crash
on an older macOS); GLib uses pipe() instead. In glib-$GLIB_VERSION: patch -p1 < $patch

EOF
status=0
diff -u -L a/meson.build -L b/meson.build "$work/meson.build.orig" "$glib/meson.build" >> "$src/$patch" || status=$?
[ "$status" -eq 1 ] || die "GLib's meson.build: no change made, or diff failed"

# PCRE2 (built into libglib) and proxy-libintl (its libintl) are GLib's subprojects: Meson fetches
# their files into its packagecache as GLib's wraps say, checked by the SHA-256 they give (those
# already in share/sources are put there first, and kept there after)
wrap() {
	sed -n "s/^$2 *= *//p" "$glib/subprojects/$1.wrap"
}
mkdir -p "$glib/subprojects/packagecache"
for w in pcre2 proxy-libintl; do
	for what in source patch; do
		f=$(wrap "$w" "${what}_filename")
		if [ -n "$f" ] && have "$f" "$(wrap "$w" "${what}_hash")"; then
			cp "$src/$f" "$glib/subprojects/packagecache/"
		fi
	done
done
meson setup ${cross:+"$cross"} "$work/glib" "$glib" --prefix="$prefix" --libdir=lib \
      --buildtype=release \
      -Ddefault_library=shared --force-fallback-for=pcre2,proxy-libintl,libffi \
      -Dtests=false -Dintrospection=disabled -Dnls=disabled -Ddocumentation=false -Dman-pages=disabled \
      -Dsysprof=disabled -Dglib_debug=disabled -Dlibelf=disabled
for w in pcre2 proxy-libintl; do
	for what in source patch; do
		f=$(wrap "$w" "${what}_filename")
		[ -z "$f" ] || { cp "$glib/subprojects/packagecache/$f" "$src/" && chmod a+r "$src/$f"; }
	done
done
meson compile -C "$work/glib" -j "$jobs"
meson install -C "$work/glib"
pcre2=$(wrap pcre2 directory)
intl=$(wrap proxy-libintl directory)
licence GLib "$glib/COPYING"
licence GLib/PCRE2 "$glib/subprojects/$pcre2"/LICENCE*       # (built into GLib)
licence proxy-libintl "$glib/subprojects/$intl"/COPYING*    # (GLib's libintl, a stand-in for gettext's)
lgpl=LGPL-2.1-or-later
source_of libglib-2.0.0.dylib GLib "$GLIB_VERSION" "$lgpl" "glib-$GLIB_VERSION.tar.xz" "$glib_url"
source_of libglib-2.0.0.dylib GLib "$GLIB_VERSION" "$lgpl" "$patch" "Blue's change to its meson.build (patch -p1)"
source_of libglib-2.0.0.dylib GLib "$GLIB_VERSION" "$lgpl" macos-deps.sh "$recipe"
pcre="PCRE2 (built into GLib)" bsd="BSD-3-Clause WITH PCRE2-exception"
source_of libglib-2.0.0.dylib "$pcre" "${pcre2##*-}" "$bsd" "$(wrap pcre2 source_filename)" "$(wrap pcre2 source_url)"
source_of libglib-2.0.0.dylib "$pcre" "${pcre2##*-}" "$bsd" "$(wrap pcre2 patch_filename)" \
          "$(wrap pcre2 patch_url) (Meson's build files for it, from WrapDB)"
source_of libglib-2.0.0.dylib "$pcre" "${pcre2##*-}" "$bsd" "glib-$GLIB_VERSION.tar.xz" \
          "GLib's subprojects/pcre2.wrap, which names these, and the patch in subprojects/packagefiles/pcre2"
source_of libglib-2.0.0.dylib "$pcre" "${pcre2##*-}" "$bsd" macos-deps.sh "$recipe"
source_of libintl.8.dylib proxy-libintl "${intl##*-}" LGPL-2.0-or-later "$(wrap proxy-libintl source_filename)" \
          "$(wrap proxy-libintl source_url)"
source_of libintl.8.dylib proxy-libintl "${intl##*-}" LGPL-2.0-or-later "glib-$GLIB_VERSION.tar.xz" \
          "GLib's subprojects/proxy-libintl.wrap, which names it"
source_of libintl.8.dylib proxy-libintl "${intl##*-}" LGPL-2.0-or-later macos-deps.sh "$recipe"

echo "macos-deps: libslirp ($(date +%H:%M:%S))"
slirp_url=https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v$SLIRP_VERSION/libslirp-v$SLIRP_VERSION.tar.gz
fetch "$slirp_url" "libslirp-v$SLIRP_VERSION.tar.gz" "$SLIRP_SHA256"
meson setup ${cross:+"$cross"} "$work/slirp" "$work/libslirp-v$SLIRP_VERSION" --prefix="$prefix" --libdir=lib \
      --buildtype=release \
      -Ddefault_library=shared
meson compile -C "$work/slirp" -j "$jobs"
meson install -C "$work/slirp"
licence libslirp "$work/libslirp-v$SLIRP_VERSION/COPYRIGHT"
source_of libslirp.0.dylib libslirp "$SLIRP_VERSION" BSD-3-Clause "libslirp-v$SLIRP_VERSION.tar.gz" "$slirp_url"
source_of libslirp.0.dylib libslirp "$SLIRP_VERSION" BSD-3-Clause macos-deps.sh "$recipe"

echo "macos-deps: libusb ($(date +%H:%M:%S))"
usb_url=https://github.com/libusb/libusb/releases/download/v$USB_VERSION/libusb-$USB_VERSION.tar.bz2
fetch "$usb_url" "libusb-$USB_VERSION.tar.bz2" "$USB_SHA256"
host=
[ "$arch" = "$(uname -m)" ] || host=--host=$family-apple-darwin      # (family: see the cross file above)
# (pipe2() isn't on older macOS, though newer SDKs declare it: libusb uses pipe() without it, as GLib does above)
(cd "$work/libusb-$USB_VERSION" && ./configure ${host:+"$host"} ac_cv_func_pipe2=no --prefix="$prefix" --disable-static \
	--disable-examples-build --disable-tests-build > /dev/null && make -j "$jobs" > /dev/null && make install > /dev/null)
licence libusb "$work/libusb-$USB_VERSION/COPYING"
lgpl=LGPL-2.1-or-later
source_of libusb-1.0.0.dylib libusb "$USB_VERSION" "$lgpl" "libusb-$USB_VERSION.tar.bz2" "$usb_url"
source_of libusb-1.0.0.dylib libusb "$USB_VERSION" "$lgpl" macos-deps.sh "$recipe"

echo "macos-deps: done ($(date +%H:%M:%S))"

# Each library's package (its licences' directory), version and source, for tools/package-macos.sh
cat > "$prefix/share/licenses/LIBRARIES" << EOF
libSDL2-2.0.0.dylib SDL2 $SDL2_VERSION https://github.com/libsdl-org/SDL/releases/tag/release-$SDL2_VERSION
libglib-2.0.0.dylib GLib $GLIB_VERSION $glib_url
libintl.8.dylib proxy-libintl ${intl##*-} https://github.com/frida/proxy-libintl/releases/tag/${intl##*-}
libslirp.0.dylib libslirp $SLIRP_VERSION https://gitlab.freedesktop.org/slirp/libslirp/-/tags/v$SLIRP_VERSION
libusb-1.0.0.dylib libusb $USB_VERSION https://github.com/libusb/libusb/releases/tag/v$USB_VERSION
EOF

# The sources' list, and their sums
mv "$work/SOURCES" "$src/SOURCES"
cut -f5 "$src/SOURCES" | sort -u | (cd "$src" && xargs shasum -a 256) > "$src/SHA256SUMS"
