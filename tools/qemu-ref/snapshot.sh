#!/bin/bash
# Snapshot the QEMU reference at the moment Apple's NanoKernel first enters the
# 68k emulator: registers plus physical memory dumps, for comparing with syshm.
#
# usage: tools/qemu-ref/snapshot.sh [rom-file] [break-address]
# Needs gdb with PowerPC support. Output goes to build/qemu-ref/snap/.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
rom=${1:-$root/MOS.rom}
bp=${2:-0x6806E708}
out=$root/build/qemu-ref/snap
mkdir -p "$out"

cat > "$out/snap.gdb" <<EOF
set pagination off
set confirm off
set architecture powerpc:common
set endian big
target remote :1234
break *$bp
continue
info registers
maint packet Qqemu.PhyMemMode:1
dump binary memory $out/phys-00000000.bin 0x00000000 0x01000000
dump binary memory $out/phys-0fe00000.bin 0x0FE00000 0x10000000
kill
EOF

QEMU_DISPLAY=none "$here/run.sh" "$rom" -gdb tcp::1234 -S > "$out/qemu.log" 2>&1 &
qemu=$!
trap 'kill $qemu 2>/dev/null || true' EXIT
sleep 1
timeout 150 gdb --batch -x "$out/snap.gdb" > "$out/regs.txt" 2>&1
echo "snapshot in $out (registers in regs.txt)"
