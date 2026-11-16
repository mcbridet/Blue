#!/bin/sh
# The libraries Blue.app bundles, built from their sources for an older macOS
# than Homebrew's (whose minimum is the macOS they were built on): SDL2, and
# libslirp with GLib (PCRE2 built into it, and proxy-libintl for its libintl).
# Into PREFIX, for MACOSX_DEPLOYMENT_TARGET (11.0 unless set) and this Mac's
# architecture or ARCH's (x86_64 on Apple silicon runs its checks in
# Rosetta), with their licences in PREFIX/share/licenses. Needs curl, cmake,
# meson, ninja and pkg-config; `make app DEPS=PREFIX` bundles them.
set -eu

SDL2_VERSION=2.32.10
SDL2_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
GLIB_VERSION=2.90.1
GLIB_SHA256=93c941aa17d5eb1d53fe838365f29a8b4e539c222a256d974ec8f30fc413e396
SLIRP_VERSION=4.9.7
SLIRP_SHA256=d2567fa06a6357c7755d61c67293b2c0bf069b3f2173bde6b76cf768529a1938

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
export CFLAGS="$flags ${CFLAGS:-}" CXXFLAGS="$flags ${CXXFLAGS:-}" OBJCFLAGS="$flags ${OBJCFLAGS:-}"
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

fetch() {
	curl -sSfL -o "$work/$2" "$1"
	echo "$3  $work/$2" | shasum -a 256 -c - > /dev/null || die "$2: not the file expected (SHA-256)"
	tar -xf "$work/$2" -C "$work"
}

licence() {
	to=$prefix/share/licenses/$1
	shift
	mkdir -p "$to"
	cp "$@" "$to/"
}

fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz" \
      "SDL2-$SDL2_VERSION.tar.gz" "$SDL2_SHA256"
cmake -S "$work/SDL2-$SDL2_VERSION" -B "$work/sdl2" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
      -DCMAKE_OSX_ARCHITECTURES="$arch" \
      -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
cmake --build "$work/sdl2" -j "$jobs"
cmake --install "$work/sdl2"
licence SDL2 "$work/SDL2-$SDL2_VERSION/LICENSE.txt"

fetch "https://download.gnome.org/sources/glib/${GLIB_VERSION%.*}/glib-$GLIB_VERSION.tar.xz" \
      "glib-$GLIB_VERSION.tar.xz" "$GLIB_SHA256"
# GLib uses pipe2() if the SDK has it (macOS 27's does), which older macOS hasn't: pipe() instead
sed "/^  'pipe2',$/d" "$work/glib-$GLIB_VERSION/meson.build" > "$work/meson.build"
mv "$work/meson.build" "$work/glib-$GLIB_VERSION/meson.build"
! grep -q "'pipe2'" "$work/glib-$GLIB_VERSION/meson.build" || die "GLib's build still looks for pipe2()"
meson setup ${cross:+"$cross"} "$work/glib" "$work/glib-$GLIB_VERSION" --prefix="$prefix" --libdir=lib \
      --buildtype=release \
      -Ddefault_library=shared --force-fallback-for=pcre2,proxy-libintl,libffi \
      -Dtests=false -Dintrospection=disabled -Dnls=disabled -Ddocumentation=false -Dman-pages=disabled \
      -Dsysprof=disabled -Dglib_debug=disabled -Dlibelf=disabled
meson compile -C "$work/glib" -j "$jobs"
meson install -C "$work/glib"
licence GLib "$work/glib-$GLIB_VERSION/COPYING"
for d in "$work/glib-$GLIB_VERSION"/subprojects/pcre2-*/; do
	licence GLib/PCRE2 "$d"/LICENCE*                # (built into GLib)
done
for d in "$work/glib-$GLIB_VERSION"/subprojects/proxy-libintl-*/; do
	licence proxy-libintl "$d"/COPYING*             # (GLib's libintl, a stand-in for gettext's)
	intl=${d%/}
	intl=${intl##*-}
done

fetch "https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v$SLIRP_VERSION/libslirp-v$SLIRP_VERSION.tar.gz" \
      "libslirp-v$SLIRP_VERSION.tar.gz" "$SLIRP_SHA256"
meson setup ${cross:+"$cross"} "$work/slirp" "$work/libslirp-v$SLIRP_VERSION" --prefix="$prefix" --libdir=lib \
      --buildtype=release \
      -Ddefault_library=shared
meson compile -C "$work/slirp" -j "$jobs"
meson install -C "$work/slirp"
licence libslirp "$work/libslirp-v$SLIRP_VERSION/COPYRIGHT"

# Each library's package (its licences' directory), version and source, for tools/package-macos.sh
cat > "$prefix/share/licenses/LIBRARIES" << EOF
libSDL2-2.0.0.dylib SDL2 $SDL2_VERSION https://github.com/libsdl-org/SDL/releases/tag/release-$SDL2_VERSION
libglib-2.0.0.dylib GLib $GLIB_VERSION https://download.gnome.org/sources/glib/${GLIB_VERSION%.*}/glib-$GLIB_VERSION.tar.xz
libintl.8.dylib proxy-libintl $intl https://github.com/frida/proxy-libintl/releases/tag/$intl
libslirp.0.dylib libslirp $SLIRP_VERSION https://gitlab.freedesktop.org/slirp/libslirp/-/tags/v$SLIRP_VERSION
EOF
