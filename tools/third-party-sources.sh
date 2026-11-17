#!/bin/sh
# Blue-third-party-sources.tar.gz, beside a release's packages: the exact
# sources of the libraries they bundle, as each package's build kept them
# (`tools/package-macos.sh sources`, tools/package-linux.sh with SOURCES): a
# directory of files each, with SOURCES, a line for each file of each library
# (package, the library's file in it, library, version, licence, file,
# origin; tab-separated). Each file goes in once (one of the same name from
# another build must be the same file), with a README.txt naming each library,
# its version and licence, the packages it's in and its files, and a
# SHA256SUMS of them.
#
#   tools/third-party-sources.sh OUT.tar.gz DIR...
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)

die() {
	echo "third-party-sources: $*" >&2
	exit 1
}

sha256() {
	if command -v sha256sum > /dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

[ $# -ge 2 ] || die "usage: $0 OUT.tar.gz DIR..."
case $1 in /*) out=$1 ;; *) out=$PWD/$1 ;; esac
mkdir -p "$(dirname "$out")"
shift
version=$(sed -n 's/^#define BLUE_VERSION "\(.*\)"/\1/p' "$root/src/version.h")
[ -n "$version" ] || die "no BLUE_VERSION in src/version.h"
name=Blue-third-party-sources
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
dir=$work/$name
mkdir "$dir"
tab=$(printf '\t')

: > "$work/SOURCES"
for d in "$@"; do
	[ -s "$d/SOURCES" ] || die "$d: no SOURCES"
	while IFS=$tab read -r _ _ _ _ _ file origin; do
		[ -n "$origin" ] || die "$d/SOURCES: a line without all its fields"
		case $file in
		*/* | .* | README.txt | SHA256SUMS) die "$d/SOURCES: a file named $file" ;;
		esac
		[ -f "$d/$file" ] || die "$d/$file: not there"
		if [ -f "$dir/$file" ]; then
			cmp -s "$d/$file" "$dir/$file" || die "$file: $d's isn't the same as another's"
		else
			cp "$d/$file" "$dir/$file"
		fi
	done < "$d/SOURCES"
	cat "$d/SOURCES" >> "$work/SOURCES"
done

{
	cat << EOF
Blue $version: the sources of the libraries in its packages

Blue's packages (Blue-arm64.dmg, Blue-x86_64.dmg, Blue-x86_64.AppImage and
Blue-aarch64.AppImage) carry the libraries below, each a separate shared
library that can be replaced by another build of it. These are the exact
sources they were built from, as kept by the builds that made the packages;
SHA256SUMS has their SHA-256 sums.

On macOS, Blue builds them from the upstream tarballs here, checked by their
SHA-256, with tools/macos-deps.sh (macos-deps.sh here), which also makes
GLib's one change (glib-*-blue.patch here) and configures libusb without
pipe2(), which macOS 11 lacks; tools/package-macos.sh then
bundles them. To build them again from these files (macos-deps.sh uses them
rather than fetching them; only libffi, for GLib's GObject, which Blue
doesn't carry, it still fetches):

    mkdir -p PREFIX/share/sources && cp * PREFIX/share/sources/
    sh macos-deps.sh PREFIX

The AppImages carry Ubuntu's own builds of the libraries named for an Ubuntu
source package below, unchanged but for their run path (patchelf, by
tools/package-linux.sh: package-linux.sh here); their source packages are
here, of the versions bundled (dpkg-source -x NAME.dsc unpacks one, with
Ubuntu's changes; dpkg-buildpackage builds it). SDL2 is built from its
tarball here by package-linux.sh.

Blue itself is under the GPL, version 2 only; its source is
https://github.com/mcbridet/Blue (the tag v$version).

EOF
	# (the libraries in order of name, each one's files in the order given)
	awk '{ print NR "\t" $0 }' "$work/SOURCES" | sort -t "$tab" -k4,4f -k1,1n | cut -f2- | awk '
		BEGIN { FS = "\t" }
		!seen[$0]++ {
			key = $3 FS $4 FS $5
			if (!(key in files)) { keys[++nk] = key; files[key] = ""; ins[key] = "" }
			in_ = $1 ": " $2
			if (!((key, in_) in inside)) { inside[key, in_] = 1; ins[key] = ins[key] "\n" in_ }
			if (!((key, $6) in from)) { from[key, $6] = ""; files[key] = files[key] "\n" $6 }
			if (!((key, $6, $7) in origin)) { origin[key, $6, $7] = 1; from[key, $6] = from[key, $6] "\n" $7 }
		}
		END {
			for (i = 1; i <= nk; i++) {
				key = keys[i]
				split(key, k, FS)
				print k[1]
				print "  Version:  " k[2]
				print "  Licence:  " k[3]
				n = split(substr(ins[key], 2), a, "\n")
				for (j = 1; j <= n; j++)
					print (j == 1 ? "  In:       " : "            ") a[j]
				n = split(substr(files[key], 2), a, "\n")
				for (j = 1; j <= n; j++) {
					print (j == 1 ? "  Files:    " : "            ") a[j]
					m = split(substr(from[key, a[j]], 2), b, "\n")
					for (l = 1; l <= m; l++)
						print "              " b[l]
				}
				print ""
			}
		}'
} > "$dir/README.txt"

(cd "$dir" && for f in *; do [ "$f" = SHA256SUMS ] || sha256 "$f"; done) > "$work/SHA256SUMS"
mv "$work/SHA256SUMS" "$dir/SHA256SUMS"
chmod -R a+rX "$dir"
# (macOS's tar: no ._ files for extended attributes)
COPYFILE_DISABLE=1 tar -czf "$out" -C "$work" "$name"
set -- "$dir"/*
echo "$out: $(du -h "$out" | cut -f1), $# files"
