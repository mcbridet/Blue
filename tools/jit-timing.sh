#!/bin/bash
# Times Blue's JIT on this machine (Linux or macOS, x86_64 or AArch64) and
# writes what it found to a log to send back (as an issue at
# https://github.com/mcbridet/Blue/issues, say):
#
#   tools/jit-timing.sh [runs]
#
# It builds Blue, runs the JIT's synthetic benchmark (guest instructions a
# second for a few kernels, interpreted and translated), then the CD boots
# whose files are there, each `runs` times (3 by default) in instruction
# time (the same guest work on every host) with the JIT and once
# interpreted: the 8.5 CD (MOS.rom and 85.iso), 9.1's (91.iso) and
# 9.2.2's (922.iso), from the top of the tree or where BLUE_ROM,
# BLUE_85_ISO, BLUE_91_ISO and BLUE_922_ISO say. It also reads the
# processor's counters, host instructions and cycles per guest instruction:
# on Linux for the emulation thread, where the kernel allows
# (perf_event_paranoid at most 2); on macOS for the whole process (under
# Rosetta, the ARM instructions it runs). Nothing is written outside the
# tree but the log.
set -u
cd "$(dirname "$0")/.." || exit 1
runs=${1:-3}
log="jit-timing-$(uname -n | cut -d. -f1)-$(date +%Y%m%d-%H%M).txt"
exec > >(tee "$log") 2>&1

echo "== Blue JIT timing, $(date)"
echo "host: $(uname -srm)"
if [ "$(uname -s)" = Darwin ]; then
	echo "cpu: $(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.physicalcpu) cores"
	[ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ] && echo "(under Rosetta)"
else
	echo "cpu: $(grep -m1 '^model name' /proc/cpuinfo | cut -d: -f2-), $(nproc) threads"
fi
echo "commit: $(git describe --always --dirty 2>/dev/null)"

echo "== build"
make -j"$(getconf _NPROCESSORS_ONLN)" all build/blue-tests > /dev/null || exit 1

echo "== the synthetic benchmark (BLUE_JIT_BENCH=1)"
BLUE_JIT_BENCH=1 build/blue-tests jit | grep -v '^jit  *ok'

TIMEFORMAT='%R %U'
rom=${BLUE_ROM:-MOS.rom}
boot() {        # name, then boot's arguments
	local name=$1 times=() t max=0 prev= a
	shift
	for a in "$@"; do
		[ "$prev" = --max ] && max=$a
		prev=$a
	done
	if ! build/blue boot "$@" --max 1 > /dev/null 2>&1; then
		echo "$name: not here (or doesn't start)"
		return
	fi
	for k in $(seq "$runs"); do
		t=$({ time build/blue boot "$@" --fixed-time 1000000000 > build/jit-timing.out 2>&1; } 2>&1)
		times+=("${t%% *}")
	done
	local jit=$(grep -E '^(jit|no JIT)' build/jit-timing.out | head -2)
	local sorted=$(printf '%s\n' "${times[@]}" | sort -n | tr '\n' ' ')
	t=$({ time build/blue boot "$@" --interp --fixed-time 1000000000 > /dev/null 2>&1; } 2>&1)
	echo "$name: JIT ${sorted}s (sorted wall times), interpreted ${t%% *}s"
	echo "$jit" | sed 's/^/  /'
	if [ "$(uname -s)" = Linux ]; then
		BLUE_PERF=1 build/blue boot "$@" --fixed-time 1000000000 2>&1 | grep -E '^(perf total|BLUE_PERF)' | sed 's/^/  /'
	else
		/usr/bin/time -l build/blue boot "$@" --fixed-time 1000000000 2>&1 > /dev/null |
			awk -v n="$max" '/instructions retired/ { i = $1 } /cycles elapsed/ { c = $1 }
				END { if (i && n) printf "  host (whole process): %.2f instructions, %.2f cycles per guest instruction\n", i / n, c / n }'
	fi
	rm -f build/jit-timing.out
}

echo "== CD boots in instruction time"
boot "8.5 CD, 4 billion instructions" --rom "$rom" --cdrom "${BLUE_85_ISO:-85.iso}" --max 4000000000
boot "9.1 CD, 4 billion instructions" --cdrom "${BLUE_91_ISO:-91.iso}" --max 4000000000
boot "9.2.2 CD, 3 billion instructions" --cdrom "${BLUE_922_ISO:-922.iso}" --max 3000000000
echo "== written to $log"
