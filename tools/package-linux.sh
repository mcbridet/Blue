#!/bin/sh
# Blue's AppImage (Linux): build/Blue-<arch>.AppImage. Blue is built here
# (libstdc++ and libgcc linked in, so that glibc is all it needs of the
# system), and SDL2 and libslirp, with what they need (GLib, PCRE), go in its
# usr/lib, each with $ORIGIN as its run path, and their licences in
# usr/share/doc/blue. SDL2 then loads X11, Wayland, ALSA, PulseAudio,
# PipeWire and OpenGL from the system, as Blue does OpenGL and EGL; none of
# those, nor glibc, is bundled. Build it on an old distribution, so that it
# runs on newer ones: in CI, Ubuntu 22.04 (glibc 2.35) in a container,
#
#   docker run --rm -v "$PWD:/src" -w /src ubuntu:22.04 tools/package-linux.sh --setup
#
# where --setup (as root, on Ubuntu) first installs the tools, libslirp and
# the headers of what SDL2 loads, and builds SDL2 from its source into
# /opt/sdl2 (its backends all loaded at run time). Without it, SDL2 comes
# from SDL2_DIR (/opt/sdl2 if it's there) or the system, libslirp from the
# system, and it needs make, a C/C++ compiler, readelf, patchelf, mksquashfs
# and curl. BUILD (build/appimage) is where Blue is built, OUT (build) where
# the AppImage goes. The AppImage runs on the AppImage project's type2-runtime
# (static: it needs no libfuse, only fusermount, or --appimage-extract-and-run).
set -eu

SDL2_VERSION=2.32.10
SDL2_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
RUNTIME_RELEASE=20251108
runtime_sha256() {
	case $1 in
	x86_64) echo 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d ;;
	aarch64) echo 00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444 ;;
	esac
}

root=$(cd "$(dirname "$0")/.." && pwd)
arch=$(uname -m)
BUILD=${BUILD:-$root/build/appimage}
OUT=${OUT:-$root/build}

die() {
	echo "package-linux: $*" >&2
	exit 1
}

setup() {
	[ "$(id -u)" = 0 ] || die "--setup installs packages: run it as root, in a container"
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -q
	apt-get install -y -q --no-install-recommends build-essential gcc-12 g++-12 cmake ninja-build pkg-config curl \
		ca-certificates patchelf squashfs-tools file libslirp0 libglib2.0-0 \
		libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev \
		libxinerama-dev libxxf86vm-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
		libegl-dev libgl-dev libgles-dev libdrm-dev libgbm-dev libasound2-dev libpulse-dev \
		libpipewire-0.3-dev libudev-dev libdbus-1-dev libibus-1.0-dev
	work=$(mktemp -d)
	curl -sSfL -o "$work/sdl2.tar.gz" \
		"https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz"
	echo "$SDL2_SHA256  $work/sdl2.tar.gz" | sha256sum -c - > /dev/null || die "SDL2's source isn't the file expected"
	tar -xzf "$work/sdl2.tar.gz" -C "$work"
	cmake -S "$work/SDL2-$SDL2_VERSION" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=/opt/sdl2 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF > "$work/cmake.log" ||
		{ cat "$work/cmake.log"; die "SDL2 wouldn't configure"; }
	grep -E '^-- +SDL_(X11|WAYLAND|ALSA|PULSEAUDIO|PIPEWIRE|KMSDRM|IBUS|LIBUDEV|DBUS|OPENGL|OPENGLES)' \
		"$work/cmake.log" || true
	cmake --build "$work/build"
	cmake --install "$work/build" > /dev/null
	mkdir -p /opt/sdl2/share/licenses/SDL2
	cp "$work/SDL2-$SDL2_VERSION/LICENSE.txt" /opt/sdl2/share/licenses/SDL2/
	echo "SDL2 $SDL2_VERSION https://github.com/libsdl-org/SDL/releases/tag/release-$SDL2_VERSION" \
		> /opt/sdl2/share/licenses/SDL2/SOURCE
	rm -rf "$work"
}

[ "${1:-}" = --setup ] && setup
[ -z "${SDL2_DIR:-}" ] && [ -d /opt/sdl2/lib ] && SDL2_DIR=/opt/sdl2
for tool in make readelf patchelf mksquashfs curl; do
	command -v "$tool" > /dev/null || die "$tool is needed"
done

# What glibc is (none of it bundled), and what may be
system_lib() {
	case $1 in
	libc.so.* | libm.so.* | libdl.so.* | libpthread.so.* | librt.so.* | libresolv.so.* | ld-linux*.so.*) return 0 ;;
	*) return 1 ;;
	esac
}
bundled_lib() {
	case $1 in
	libSDL2-2.0.so.* | libslirp.so.* | libglib-2.0.so.* | libpcre.so.* | libpcre2-8.so.* | libintl.so.*) return 0 ;;
	*) return 1 ;;
	esac
}

# A library's file: SDL2_DIR's, the system's
find_lib() {
	if [ -n "${SDL2_DIR:-}" ] && [ -e "$SDL2_DIR/lib/$1" ]; then
		echo "$SDL2_DIR/lib/$1"
		return
	fi
	ldconfig -p | awk -v n="$1" '$1 == n { print $NF; exit }'
}

needed() {
	readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'
}

# (GCC 12 or later, or clang, for __builtin_shufflevector: Ubuntu 22.04's cc is GCC 11)
if [ -z "${CC:-}" ] && command -v gcc-12 > /dev/null && command -v g++-12 > /dev/null; then
	CC=gcc-12 CXX=g++-12
fi
make -C "$root" -j"$(getconf _NPROCESSORS_ONLN)" BUILD="$BUILD" CC="${CC:-cc}" CXX="${CXX:-c++}" \
	LDFLAGS="-g -static-libstdc++ -static-libgcc" "$BUILD/blue"
version=$(sed -n 's/^#define BLUE_VERSION "\(.*\)"/\1/p' "$root/src/version.h")

app=$BUILD/AppDir
rm -rf "$app"
mkdir -p "$app/usr/bin" "$app/usr/lib" "$app/usr/share/applications" "$app/usr/share/doc/blue/Licenses"
cp "$BUILD/blue" "$app/usr/bin/blue"
strip --strip-debug "$app/usr/bin/blue"
for lib in $(needed "$app/usr/bin/blue"); do
	system_lib "$lib" || die "blue needs $lib, which isn't glibc's"
done
ln -s usr/bin/blue "$app/AppRun"

sed -e '/^#/d' -e '/^NoDisplay=/d' "$root/art/blue.desktop" > "$app/blue.desktop"
cp "$app/blue.desktop" "$app/usr/share/applications/blue.desktop"
for n in 16 24 32 48 64 128 256 512; do
	mkdir -p "$app/usr/share/icons/hicolor/${n}x$n/apps"
	cp "$root/art/png/blue-$n.png" "$app/usr/share/icons/hicolor/${n}x$n/apps/blue.png"
done
cp "$root/art/png/blue-256.png" "$app/blue.png"
ln -s blue.png "$app/.DirIcon"

# SDL2, libslirp and what they need but glibc
list=$BUILD/libs
: > "$list"
for lib in libSDL2-2.0.so.0 libslirp.so.0; do
	f=$(find_lib "$lib")
	[ -n "$f" ] || die "$lib: not found (SDL2_DIR, or the system's)"
	echo "$lib $f" >> "$list"
done
n=0
while [ "$n" -lt "$(wc -l < "$list")" ]; do
	n=$((n + 1))
	line=$(sed -n "${n}p" "$list")
	name=${line%% *}
	for dep in $(needed "${line#* }"); do
		system_lib "$dep" && continue
		bundled_lib "$dep" || die "$name needs $dep, which shouldn't be bundled (the system's graphics, sound, ...?)"
		grep -q "^$dep " "$list" && continue
		f=$(find_lib "$dep")
		[ -n "$f" ] || die "$name needs $dep, not found"
		echo "$dep $f" >> "$list"
	done
done
while read -r name from; do
	cp -L "$from" "$app/usr/lib/$name"
	chmod u+w "$app/usr/lib/$name"
	# shellcheck disable=SC2016
	patchelf --set-rpath '$ORIGIN' "$app/usr/lib/$name"
done < "$list"

# Licences: Blue's, and each library's (SDL2_DIR's, or its Debian package's copyright and the licences that names)
doc=$app/usr/share/doc/blue
cp "$root/LICENSE" "$root/THIRD-PARTY-NOTICES.md" "$root/README.md" "$doc/"
{
	echo "Blue $version"
	echo
	echo "The AppImage's usr/lib holds these libraries, each a separate shared library that Blue"
	echo "loads at run time, so each can be replaced by another build of it with the same name"
	echo "(extract the AppImage with --appimage-extract, replace it in squashfs-root/usr/lib,"
	echo "and run squashfs-root/AppRun). Their licences are in the folders here, named for each;"
	echo "the source of each is at the address given."
	echo
} > "$doc/Licenses/README.txt"
distro=$(sed -n 's/^PRETTY_NAME="*\([^"]*\)"*$/\1/p' /etc/os-release 2> /dev/null)
while read -r name from; do
	if [ -n "${SDL2_DIR:-}" ] && [ "$from" = "$SDL2_DIR/lib/$name" ]; then
		cp -R "$SDL2_DIR/share/licenses/"* "$doc/Licenses/"
		read -r pkg ver src < "$SDL2_DIR/share/licenses/SDL2/SOURCE"
		rm "$doc/Licenses/SDL2/SOURCE"
		echo "$name: $pkg $ver, $src" >> "$doc/Licenses/README.txt"
		continue
	fi
	pkg=$(dpkg -S "*/$(basename "$(realpath "$from")")" 2> /dev/null | head -1 | cut -d: -f1) ||
		die "$name: no package owns $from, to find its licence in"
	[ -n "$pkg" ] || die "$name: no package owns $from, to find its licence in"
	mkdir -p "$doc/Licenses/$pkg"
	cp -L "/usr/share/doc/$pkg/copyright" "$doc/Licenses/$pkg/" || die "$pkg has no copyright file"
	grep -o '/usr/share/common-licenses/[A-Za-z0-9.+-]*' "/usr/share/doc/$pkg/copyright" | sort -u |
		while read -r l; do
			[ ! -f "$l" ] || cp "$l" "$doc/Licenses/$pkg/"
		done
	ver=$(dpkg-query -W -f '${Version}' "$pkg")
	srcpkg=$(dpkg-query -W -f '${source:Package}' "$pkg")
	case $distro in
	Ubuntu*) src=https://launchpad.net/ubuntu/+source/$srcpkg/$ver ;;
	Debian*) src=https://snapshot.debian.org/package/$srcpkg/$ver/ ;;
	*) src="apt-get source $srcpkg=$ver" ;;
	esac
	echo "$name: $pkg $ver, from ${distro:-the system}'s $srcpkg ($src)" >> "$doc/Licenses/README.txt"
done < "$list"
echo >> "$doc/Licenses/README.txt"
echo "Blue's own licence is ../LICENSE (GPL-2.0-only); what's built into it, ../THIRD-PARTY-NOTICES.md." \
	>> "$doc/Licenses/README.txt"

# The oldest glibc it all needs
glibc=$(for f in "$app/usr/bin/blue" "$app"/usr/lib/*; do readelf -V "$f"; done |
	grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)

runtime=$BUILD/runtime-$arch-$RUNTIME_RELEASE
sum=$(runtime_sha256 "$arch")
[ -n "$sum" ] || die "no AppImage runtime for $arch"
if ! echo "$sum  $runtime" | sha256sum -c - > /dev/null 2>&1; then
	curl -sSfL -o "$runtime" \
		"https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_RELEASE/runtime-$arch"
	echo "$sum  $runtime" | sha256sum -c - > /dev/null || die "the AppImage runtime isn't the file expected"
fi
rm -f "$BUILD/squashfs"
mksquashfs "$app" "$BUILD/squashfs" -root-owned -noappend -comp zstd -quiet > /dev/null
mkdir -p "$OUT"
image=$OUT/Blue-$arch.AppImage
cat "$runtime" "$BUILD/squashfs" > "$image"
chmod 755 "$image"
echo "$image: Blue $version, $(du -h "$image" | cut -f1), glibc $glibc or later;" \
     "$(cd "$app/usr/lib" && echo *)"
