#!/bin/bash
# Reference run: boot a 1.x NewWorld "Mac OS ROM" file on stock qemu-system-ppc
# (g3beige) with the device tree patched to pass for a B&W G3. This runs
# Apple's real Trampoline and NanoKernel, so it shows what Blue has to reproduce.
#
# usage: tools/qemu-ref/run.sh [rom-file] [extra qemu args...]
#   QEMU_DISPLAY=gtk|sdl|none (default: gtk when a desktop session is present)
# Inspect a running session with tools/qemu-ref/qmon.py.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
rom=${1:-$root/MOS.rom}
[ $# -gt 0 ] && shift

out=$root/build/qemu-ref
disk=$out/boot.img
mkdir -p "$out"

if [ ! -e "$disk" ] || [ "$rom" -nt "$disk" ]; then
	hfs=$out/boot.hfs
	dd if=/dev/zero of="$hfs" bs=1M count=8 status=none
	hformat -l "Blue boot" "$hfs" >/dev/null
	hmkdir "System Folder"
	hcopy -r "$rom" ":System Folder:Mac OS ROM"
	hattrib -t tbxi -c chrp ":System Folder:Mac OS ROM"
	hattrib -b ":System Folder"
	humount >/dev/null
	python3 "$here/mkapm.py" "$hfs" "$disk"
	rm -f "$hfs"
fi

if [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
	display=${QEMU_DISPLAY:-gtk}
else
	display=${QEMU_DISPLAY:-none}
fi

patch=$(grep -v '^\\' "$here/bw-g3.fs" | tr '\n' ' ')
boot='/pci@80000000/mac-io@10/ata-3@20000/disk@0:,\\:tbxi'

rm -f "$out/monitor.sock"
exec qemu-system-ppc -M g3beige -m 256 \
	-display "$display" \
	-serial "file:$out/serial.log" \
	-monitor "unix:$out/monitor.sock,server,nowait" \
	-drive "file=$disk,format=raw,media=disk" \
	-prom-env "boot-command=$patch boot $boot" \
	"$@"
