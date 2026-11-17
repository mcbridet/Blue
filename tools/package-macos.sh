#!/bin/sh
# Blue.app and its disk image (macOS), for one architecture.
#
#   tools/package-macos.sh app BLUE INFO_PLIST DEPS OUT.app
#       Blue.app around the executable BLUE: SDL2, libslirp and libusb from DEPS/lib
#       (Homebrew's prefix, or tools/macos-deps.sh's) and the libraries they
#       need but the system's, in Contents/Frameworks (their install names
#       @rpath/NAME, found through the executable's @executable_path/../Frameworks
#       and their own @loader_path), their licences in
#       Contents/Resources/Licenses (Blue's LICENSE and THIRD-PARTY-NOTICES.md
#       beside it); Blue's USB helper, blue-usb-helper (BLUE's neighbour), in
#       Contents/MacOS too; LSMinimumSystemVersion raised to the newest any of
#       them needs; ad-hoc signed, the libraries and the helper first.
#   tools/package-macos.sh dmg APP OUT.dmg
#       A disk image with the app, a link to Applications, and Blue's LICENSE.
#   tools/package-macos.sh sources APP DEPS PACKAGE OUT
#       The exact sources of the libraries in APP's Contents/Frameworks, as
#       tools/macos-deps.sh keeps them in DEPS/share/sources (checked against
#       its SHA256SUMS), into the directory OUT, with OUT/SOURCES: a line for
#       each file of each library, PACKAGE (the disk image's name) first, for
#       tools/third-party-sources.sh.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)

die() {
	echo "package-macos: $*" >&2
	exit 1
}

# The oldest macOS a Mach-O file runs on (the newest of its slices')
minos() {
	otool -l "$1" | awk '
		/cmd LC_BUILD_VERSION/ { b = 1 } /cmd LC_VERSION_MIN_MACOSX/ { v = 1 }
		b && $1 == "minos" { print $2; b = 0 } v && $1 == "version" { print $2; v = 0 }' |
		sort -t. -k1,1n -k2,2n -k3,3n | tail -1
}

newer() {   # the newer of two versions
	printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1
}

# Libraries a Mach-O file loads, but the system's
deps() {
	otool -L "$1" | tail -n +2 | awk '{ print $1 }' | grep -v -E '^(/usr/lib/|/System/)' || true
}

sign() {
	app=$1
	for f in "$app"/Contents/Frameworks/*.dylib; do
		[ -L "$f" ] || codesign --force -s - "$f"
	done
	codesign --force -s - -i "$(plutil -extract CFBundleIdentifier raw "$app/Contents/Info.plist").usb-helper" \
		"$app/Contents/MacOS/blue-usb-helper"
	codesign --force -s - "$app"
	codesign --verify --deep --strict "$app"
}

# The newest minimum of the executable's and the libraries', in the Info.plist
set_minimum() {
	app=$1
	need=$(plutil -extract LSMinimumSystemVersion raw "$app/Contents/Info.plist")
	for f in "$app/Contents/MacOS/blue" "$app/Contents/MacOS/blue-usb-helper" "$app"/Contents/Frameworks/*.dylib; do
		[ -L "$f" ] || need=$(newer "$need" "$(minos "$f")")
	done
	plutil -replace LSMinimumSystemVersion -string "$need" "$app/Contents/Info.plist"
	echo "$need"
}

app() {
	[ $# -eq 4 ] || die "usage: $0 app BLUE INFO_PLIST DEPS OUT.app"
	blue=$1 plist=$2 deps=${3%/} out=$4
	[ -n "$deps" ] && [ -d "$deps/lib" ] || die "no libraries to bundle: DEPS ('$deps') should be Homebrew's prefix or tools/macos-deps.sh's"
	helper=$(dirname "$blue")/blue-usb-helper
	[ -x "$helper" ] || die "no $helper (make builds it)"
	rm -rf "$out"
	mkdir -p "$out/Contents/MacOS" "$out/Contents/Frameworks" "$out/Contents/Resources/Licenses"
	cp "$blue" "$out/Contents/MacOS/blue"
	cp "$helper" "$out/Contents/MacOS/blue-usb-helper"
	cp "$plist" "$out/Contents/Info.plist"
	printf 'APPL????' > "$out/Contents/PkgInfo"

	icons=$(mktemp -d)
	mkdir "$icons/Blue.iconset"
	for n in 16 32 128 256 512; do
		cp "$root/art/png/blue-$n.png" "$icons/Blue.iconset/icon_${n}x$n.png"
		cp "$root/art/png/blue-$((n * 2)).png" "$icons/Blue.iconset/icon_${n}x$n@2x.png"
	done
	iconutil -c icns -o "$out/Contents/Resources/Blue.icns" "$icons/Blue.iconset"
	rm -rf "$icons"

	# The libraries: SDL2 (and SDL3 under sdl2-compat, which opens it as @loader_path/libSDL3.dylib),
	# libslirp, libusb, and theirs; a list of "file name, where from" as they're copied
	fw=$out/Contents/Frameworks
	list=$(mktemp)
	for lib in "$deps/lib/libSDL2-2.0.0.dylib" "$deps/lib/libslirp.0.dylib" "$deps/lib/libusb-1.0.0.dylib"; do
		[ -f "$lib" ] || die "$lib: not found (Homebrew: brew install sdl2 libslirp libusb)"
		echo "$(basename "$lib") $lib" >> "$list"
	done
	if strings - "$deps/lib/libSDL2-2.0.0.dylib" | grep -q '@loader_path/libSDL3.dylib'; then
		[ -f "$deps/lib/libSDL3.0.dylib" ] || die "$deps/lib/libSDL3.0.dylib: not found (sdl2-compat needs it)"
		echo "libSDL3.dylib $deps/lib/libSDL3.0.dylib" >> "$list"
	fi
	n=0
	while [ "$n" -lt "$(wc -l < "$list")" ]; do
		n=$((n + 1))
		line=$(sed -n "${n}p" "$list")
		name=${line%% *} from=${line#* }
		cp "$(realpath "$from")" "$fw/$name"
		chmod u+w "$fw/$name"
		for ref in $(deps "$fw/$name"); do
			leaf=$(basename "$ref")
			[ "$leaf" = "$(basename "$(otool -D "$fw/$name" | tail -1)")" ] && continue    # (its own name)
			case $ref in
			@*) found=
			    for dir in "$(dirname "$from")" "$deps/lib"; do
				    [ -f "$dir/$leaf" ] && found=$dir/$leaf && break
			    done
			    [ -n "$found" ] || die "$name needs $ref, not found in $(dirname "$from") or $deps/lib" ;;
			*) found=$ref ;;
			esac
			grep -q "^$leaf " "$list" || echo "$leaf $found" >> "$list"
		done
	done

	for f in "$fw"/*.dylib; do
		name=$(basename "$f")
		install_name_tool -id "@rpath/$name" "$f" 2> /dev/null
		for ref in $(deps "$f"); do
			case $ref in @rpath/*) ;; *) install_name_tool -change "$ref" "@rpath/$(basename "$ref")" "$f" 2> /dev/null ;; esac
		done
		for r in $(otool -l "$f" | awk '/cmd LC_RPATH/ { r = 1 } r && $1 == "path" { print $2; r = 0 }'); do
			install_name_tool -delete_rpath "$r" "$f" 2> /dev/null
		done
		install_name_tool -add_rpath @loader_path "$f" 2> /dev/null
		for ref in $(deps "$f"); do
			case $ref in @rpath/*) [ -f "$fw/${ref#@rpath/}" ] || die "$name needs $ref, which isn't bundled" ;;
			*) die "$name still loads $ref" ;; esac
		done
	done
	for f in blue blue-usb-helper; do
		for ref in $(deps "$out/Contents/MacOS/$f"); do
			die "$f loads $ref (it should load only the system's libraries)"
		done
	done

	# Their licences: tools/macos-deps.sh's (share/licenses/LIBRARIES says whose), or the Homebrew keg's
	lic=$out/Contents/Resources/Licenses
	about=$lic/README.txt
	{
		echo "Blue $(plutil -extract CFBundleShortVersionString raw "$out/Contents/Info.plist")"
		echo
		echo "Blue.app's Contents/Frameworks holds these libraries, each a separate dylib that Blue"
		echo "loads at run time, so each can be replaced by another build of it with the same name"
		echo "(then sign the app again: codesign --force --deep -s - Blue.app). Their licences are"
		echo "in the folders here, named for each; the source of each is at the address given, and"
		echo "Blue's releases carry the exact sources they were built from, in"
		echo "Blue-third-party-sources.tar.gz (https://github.com/mcbridet/Blue/releases)."
		echo
	} > "$about"
	while read -r name from; do
		real=$(realpath "$from")
		line=$(grep "^$name " "$deps/share/licenses/LIBRARIES" 2> /dev/null || true)
		if [ -n "$line" ]; then
			read -r _ pkg version source <<- EOF
				$line
			EOF
			cp -R "$deps/share/licenses/$pkg" "$lic/"
		else
			case $real in
			*/Cellar/*/lib/*) keg=${real%/lib/*} ;;
			*) die "$name: no licence known (from $real)" ;;
			esac
			version=$(basename "$keg") pkg=$(basename "$(dirname "$keg")")
			source="Homebrew's $pkg formula (https://formulae.brew.sh/formula/$pkg)"
			mkdir -p "$lic/$pkg"
			for t in "$keg"/LICENSE* "$keg"/LICENCE* "$keg"/COPYING* "$keg"/COPYRIGHT* "$keg"/*GPL*.txt; do
				[ -f "$t" ] && cp "$t" "$lic/$pkg/"
			done
			[ -n "$(ls "$lic/$pkg")" ] || die "$name: no licence in $keg"
			if [ "$pkg" = gettext ]; then
				echo "libintl is under the GNU LGPL 2.1 or later; the GPL in COPYING is gettext's tools'," \
				     "which aren't here." > "$lic/$pkg/NOTE"
			fi
		fi
		echo "$name: $pkg $version, $source" >> "$about"
	done < "$list"
	rm -f "$list"
	echo >> "$about"
	echo "Blue's own licence is ../LICENSE (GPL-2.0-only); what's built into it, ../THIRD-PARTY-NOTICES.md." >> "$about"
	cp "$root/LICENSE" "$root/THIRD-PARTY-NOTICES.md" "$out/Contents/Resources/"

	need=$(set_minimum "$out")
	sign "$out"
	echo "$out: Blue $(plutil -extract CFBundleShortVersionString raw "$out/Contents/Info.plist")," \
	     "$(lipo -archs "$out/Contents/MacOS/blue"), macOS $need or later;" \
	     "$(cd "$fw" && echo *.dylib)"
}

dmg() {
	[ $# -eq 2 ] || die "usage: $0 dmg APP OUT.dmg"
	stage=$(mktemp -d)
	ditto "$1" "$stage/$(basename "$1")"
	ln -s /Applications "$stage/Applications"
	cp "$root/LICENSE" "$stage/LICENSE"
	# (hdiutil sometimes finds the device busy, or makes an image it can't open again, on CI's Macs
	# especially: a few tries, each checked)
	for try in 1 2 3; do
		rm -f "$2"
		hdiutil create -quiet -volname Blue -srcfolder "$stage" -fs HFS+ -format UDZO "$2" &&
			hdiutil verify -quiet "$2" && break
		[ "$try" -lt 3 ] || die "hdiutil couldn't make $2"
		sleep 5
	done
	rm -rf "$stage"
	echo "$2: $(du -h "$2" | cut -f1)"
}

sources() {
	[ $# -eq 4 ] || die "usage: $0 sources APP DEPS PACKAGE OUT"
	app=$1 from=${2%/}/share/sources package=$3 out=$4
	[ -f "$from/SOURCES" ] ||
		die "no sources in $from: tools/macos-deps.sh keeps them there (Homebrew's libraries have none)"
	mkdir -p "$out"
	: > "$out/SOURCES"
	tab=$(printf '\t')
	lines=$(mktemp)
	cp "$root/tools/package-macos.sh" "$out/"
	for f in "$app"/Contents/Frameworks/*.dylib; do
		name=$(basename "$f")
		grep "^$name$tab" "$from/SOURCES" > "$lines" || die "$name: no sources in $from/SOURCES"
		while IFS=$tab read -r _ lib version licence file origin; do
			(cd "$from" && awk -v f="$file" '$2 == f' SHA256SUMS | shasum -a 256 -c - > /dev/null 2>&1) ||
				die "$from/$file: missing, or not the file it was (SHA256SUMS)"
			cp "$from/$file" "$out/"
			printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$package" "Blue.app/Contents/Frameworks/$name" \
				"$lib" "$version" "$licence" "$file" "$origin" >> "$out/SOURCES"
		done < "$lines"
		# (and what this script did to it)
		cut -f2-4 "$lines" | sort -u | while IFS=$tab read -r lib version licence; do
			printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$package" "Blue.app/Contents/Frameworks/$name" \
				"$lib" "$version" "$licence" package-macos.sh \
				"Blue's tools/package-macos.sh, which bundled it (install names @rpath, signed ad hoc)"
		done >> "$out/SOURCES"
	done
	rm -f "$lines"
	echo "$out: $(cut -f6 "$out/SOURCES" | sort -u | tr '\n' ' ')"
}

[ $# -ge 1 ] || die "usage: $0 app|dmg|sources ..."
cmd=$1
shift
case $cmd in
app | dmg | sources) "$cmd" "$@" ;;
*) die "usage: $0 app|dmg|sources ..." ;;
esac
