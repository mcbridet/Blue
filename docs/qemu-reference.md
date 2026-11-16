# The QEMU reference

QEMU's PowerPC machines run Apple's own Trampoline and NanoKernel, which
makes them the reference for what Blue's replacements must do. These
scripts, in `tools/qemu-ref/`, run them and look inside.

Stock QEMU can't boot a 1.x ROM: its machines aren't models the ROM supports.
`tools/qemu-ref/run.sh` puts the ROM on a small bootable disk and patches the
g3beige device tree (`tools/qemu-ref/bw-g3.fs`) so the Trampoline accepts it.
Needs `hfsutils` and `qemu-system-ppc`.

```
tools/qemu-ref/run.sh [MOS.rom]                  # QEMU_DISPLAY=none for headless
tools/qemu-ref/qmon.py sample 10                 # PowerPC pc and 68k pc
tools/qemu-ref/qmon.py screenshot shot.png
tools/qemu-ref/qmon.py cmd 'x /16wx 0x5fffefc0'  # any monitor command
tools/qemu-ref/snapshot.sh                       # needs gdb; state at 68k emulator entry
```

With this ROM the Trampoline and NanoKernel boot and the 68k emulator runs ROM
code, then spins at 68k `0xFFC18316`. The Trampoline didn't resolve any
interrupt sources from the patched tree, so every vector in `NKHWInfo` is
`0x0800`, and the ROM's SCC setup loops forever when SCC B has no vector. It
is still the reference for what the Trampoline and kernel produce: the
snapshot gave Blue its info records, universal tables and low-memory setup.

For Mac OS 9.1, QEMU's `mac99` runs Apple's real Trampoline and NanoKernel 2
unpatched, to the Finder: `tools/qemu-ref/run91.sh [91.iso]` boots the CD,
`mptrace.py` logs every Multiprocessing Services call with its results
through QEMU's gdbstub (`rsp.py`, no gdb needed), `nk2dump.py` decodes the
kernel's state and the device tree from a RAM dump, and `shots.py` takes
screenshots. What they found is in [nk2-reference.md](nk2-reference.md).
