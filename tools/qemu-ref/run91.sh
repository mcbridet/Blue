#!/bin/bash
# Reference run for Mac OS 9.1: boot the 9.1 CD on stock qemu-system-ppc. On
# mac99 with a PMU (the default here) the CD's own "Mac OS ROM" (6.1f9, the
# same as MOS91.rom) passes the Trampoline's model check unpatched and Mac OS
# reaches the Finder in under a minute, so this runs Apple's real Trampoline
# and NanoKernel 2.
#
# usage: tools/qemu-ref/run91.sh [cd-image (default 91.iso)] [extra qemu args...]
#   QEMU_MACHINE  machine and options (default mac99,via=pmu)
#   QEMU_RAM      MB of RAM (default 256; the addresses in docs/nk2-reference.md are for 256)
#   QEMU_DISPLAY  gtk|sdl|cocoa|none (default none)
#   QEMU_REF_OUT  directory for the monitor socket and serial log (default build/qemu-ref91)
# Extra args go to QEMU, e.g. "-gdb tcp::1234 -S" for mptrace.py, or
# "-d int -D int.log" to log every exception (sc lines carry r0 and r3-r8).
# Monitor: tools/qemu-ref/rsp.py mon CMD (with -gdb), or the unix socket
# $QEMU_REF_OUT/monitor.sock. QEMU runs until killed: bound it with timeout(1).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cd_image=$root/91.iso
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
	cd_image=$1
	shift
fi

out=${QEMU_REF_OUT:-$root/build/qemu-ref91}
mkdir -p "$out"
rm -f "$out/monitor.sock"

exec qemu-system-ppc -M "${QEMU_MACHINE:-mac99,via=pmu}" -m "${QEMU_RAM:-256}" \
	-display "${QEMU_DISPLAY:-none}" \
	-serial "file:$out/serial.log" \
	-monitor "unix:$out/monitor.sock,server,nowait" \
	-drive "file=$cd_image,format=raw,index=2,media=cdrom,readonly=on" \
	-boot d \
	"$@"
