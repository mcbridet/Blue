#!/usr/bin/env python3
"""Decode NanoKernel 2's state from a RAM dump of a QEMU reference run.

  nk2dump.py RAMDUMP [--kdp ADDR] [--tree]

RAMDUMP is physical RAM from address 0 (mptrace.py's ram-*.bin, or the
monitor's `pmemsave 0 0x10000000 "file"`). KDP is the kernel's global page
(SPRG0; 0x0FBFE000 with 256 MB under QEMU's mac99). Prints:

  - the InfoRecord pointer table (KDP+0xFC0) and NanoKernelInfo's v2 fields,
  - kernel globals (ConfigInfo, ECB, page map, HTAB, ...) and BAT ranges,
  - the page map the Trampoline handed the kernel (KDP.SegMaps: 16 segment
    entries pointing at 8-byte PMDTs: logical page offset, page count - 1,
    physical page | attributes) and SystemInfo's RAM banks,
  - every object in the kernel's ID index, decoded for processes, tasks,
    queues, semaphores, event groups (with SWI levels), address spaces and
    areas (logical range, physical base for contiguous ones, PTE settings),
  - with --tree, the device-tree copy (HWInfo.DeviceTreeBase) via
    tools/devtree.py.

Field offsets follow github.com/elliotnunn/powermac-rom (Internal/NKOpaque.a,
NKPublic.a, InfoRecords.a); the 9.1 kernel (NanoKernelInfo version 2.21)
agrees with them where checked (object signatures, KDP page map and BATs).
"""
import argparse
import os
import struct
import subprocess
import sys

CLASSES = {1: 'process', 2: 'task', 3: 'timer', 4: 'queue', 5: 'semaphore', 6: 'critical region',
           7: 'cpu', 8: 'address space', 9: 'event group', 10: 'coherence group', 11: 'area',
           12: 'notification', 13: 'console log'}


class Dump:
    def __init__(self, path, kdp):
        self.d = open(path, 'rb').read()
        self.kdp = kdp

    def u32(self, a):
        return struct.unpack('>I', self.d[a:a + 4])[0]

    def u16(self, a):
        return struct.unpack('>H', self.d[a:a + 2])[0]

    def u8(self, a):
        return self.d[a]

    def ostype(self, a):
        b = self.d[a:a + 4]
        return b.decode('latin-1') if all(32 <= c < 127 for c in b) else b.hex()

    # logical -> physical through the Trampoline's page map and the kernel's BAT ranges
    def bats(self):
        out = []
        for i in range(16):
            up, lo = self.u32(self.kdp + 0x280 + 8 * i), self.u32(self.kdp + 0x284 + 8 * i)
            if up & 3:
                size = ((up >> 2 & 0x7FF) + 1) << 17
                out.append((up & 0xFFFE0000, size, lo & 0xFFFE0000, lo & 0x7F))
        return out

    def pmdts(self, seg):
        p = self.u32(self.kdp + 0x80 + 8 * seg)
        out = []
        while True:
            lbase, count, pflags = self.u16(p), self.u16(p + 2), self.u32(p + 4)
            out.append((p, (seg << 28) + (lbase << 12), (count + 1) << 12, pflags))
            if lbase == 0 and count == 0xFFFF or len(out) > 64:
                return out
            p += 8

    def phys(self, ea):
        for la, size, pa, _ in self.bats():
            if la <= ea < la + size:
                return pa + ea - la
        for _, la, size, pf in self.pmdts(ea >> 28):
            if la <= ea < la + size and pf & 0xE00 == 0:
                return (pf & 0xFFFFF000) + ea - la
        return None


def info_records(m):
    k = m.kdp
    print('InfoRecord (KDP+0xFC0, logical 0x%08x):' % m.u32(k + 0xFC0))
    names = ['ProcessorState', 'HWInfo', 'ProcessorInfo', 'NanoKernelInfo', 'DiagInfo',
             'SystemInfo', 'ProcessorInfo (2)']
    for i, n in enumerate(names):
        a = k + 0xFC8 + 8 * i
        la = m.u32(a)
        pa = m.phys(la)
        print('  %-18s logical %08x  phys %s  version %04x  length 0x%x' % (
            n, la, '%08x' % pa if pa is not None else '?', m.u16(a + 4), m.u16(a + 6)))
    nki = m.phys(m.u32(k + 0xFE0))
    print('NanoKernelInfo v2 fields:')
    for off, n in ((0x100, 'blueProcessID'), (0x104, 'blueTaskID'), (0x108, 'pageQueueID'),
                   (0x10C, 'TaskCount'), (0x110, 'FreePoolExtendCount'), (0x11C, 'ConfigFlags'),
                   (0x13C, 'IDCtr')):
        print('  +0x%03x %-20s %08x' % (off, n, m.u32(nki + off)))
    calls = [m.u32(nki + 0x80 + 4 * i) for i in range(16)]
    print('  kernel call counts   %s' % ' '.join('%d' % c for c in calls))
    hw = m.phys(m.u32(k + 0xFD0))
    print('HWInfo: MacROM_Base %08x DeviceTreeBase %08x UniversalInfoTableBase %08x '
          'ConfigInfoTableBase %08x' % tuple(m.u32(hw + o) for o in (0, 4, 8, 12)))


def globals_(m):
    k = m.kdp
    print('Kernel globals (KDP 0x%08x):' % k)
    for off, n in ((0x630, 'PA_ConfigInfo'), (0x634, 'PA_EmulatorData'), (0x638, 'KernelMemoryBase'),
                   (0x63C, 'KernelMemoryEnd'), (0x640, 'PA_RelocatedLowMemInit'),
                   (0x648, 'LA_EmulatorKernelTrapTable'), (0x64C, 'PA_NanoKernelCode'),
                   (0x650, 'PA_FDP'), (0x654, 'LA_ECB'), (0x658, 'PA_ECB'), (0x670, 'TestIntMaskInit'),
                   (0x674, 'PostIntMaskInit'), (0x678, 'ClearIntMaskInit'), (0x67C, 'PA_EmulatorIplValue'),
                   (0x684, 'PA_PageMapStart'), (0x66C, 'PA_PageMapEnd'), (0x688, 'PageAttributeInit'),
                   (0x6A0, 'PTEGMask'), (0x6A4, 'HTABORG'), (0x6A8, 'VMLogicalPages'),
                   (0x6AC, 'TotalPhysicalPages'), (0x6B0, 'FlatPageListPtr'), (0x6B4, 'VMMaxVirtualPages')):
        print('  +0x%03x %-27s %08x' % (off, n, m.u32(k + off)))
    print('BAT ranges (KDP+0x280):')
    for la, size, pa, attr in m.bats():
        print('  %08x-%08x -> %08x  WIMG %x PP %d' % (la, la + size - 1, pa, attr >> 3 & 15, attr & 3))


def page_map(m):
    print('Page map (KDP.SegMaps -> PMDTs; attr: 0xC00 paged RAM, 0xA00/0xA01 none, '
          'else contiguous, low bits WIMG<<3|PP):')
    seen = {}
    for seg in range(16):
        sr = m.u32(m.kdp + 0x84 + 8 * seg)
        for p, la, size, pf in m.pmdts(seg):
            if pf & 0xE00 and la & 0x0FFFFFFF == 0 and size == 0x10000000:
                if not seen.get(seg):
                    print('  seg %X (SR %08x): nothing mapped (pmdt %08x %08x)' % (seg, sr, size, pf))
                continue
            seen[seg] = True
            kind = 'paged RAM' if pf & 0xC00 == 0xC00 else 'none' if pf & 0xE00 else 'contiguous'
            print('  %08x-%08x  pmdt@%08x  attr %03x  %-10s %s' % (
                la, la + size - 1, p, pf & 0xFFF, kind,
                '-> %08x  WIMG %x PP %d' % (pf & 0xFFFFF000, pf >> 3 & 15, pf & 3) if kind == 'contiguous' else
                'word %08x' % pf))
    si = m.phys(m.u32(m.kdp + 0xFF0))
    print('SystemInfo: physical %08x usable %08x logical %08x hash table %08x' % tuple(
        m.u32(si + o) for o in (0, 4, 8, 12)))
    for b in range(26):
        st, sz = m.u32(si + 0x30 + 8 * b), m.u32(si + 0x34 + 8 * b)
        if sz:
            print('  RAM bank %d: %08x-%08x' % (b, st, st + sz - 1))


def objects(m, verbose=True):
    """Walk the ID index: Index ('INDX') -> pages of 0x1FA 8-byte entries ('IDs ')."""
    i = m.d.find(b'INDX', m.kdp - 0x100000, m.kdp)
    if i < 0:
        print('no ID index found')
        return []
    index = i - 4
    out = []
    for pg in range(0, 0x80):
        page = m.u32(index + 8 + 4 * pg)
        if not page:
            continue
        base = m.u16(page)
        for e in range(0x1FA):
            a = page + 8 + 8 * e
            cls = m.u8(a)
            if not cls:
                continue
            uses, ptr = m.u16(a + 2), m.u32(a + 4)
            out.append(((base + e) << 16 | uses, cls, ptr))
    print('ID index at %08x: %d objects' % (index, len(out)))
    for oid, cls, p in out:
        line = '  %08x %-16s @%08x' % (oid, CLASSES.get(cls, str(cls)), p)
        if cls == 1:
            line += '  flags %08x address space %08x' % (m.u32(p + 8), m.u32(p + 0xC))
        elif cls == 2:
            line += "  '%s' state %d priority %d weight %d flags %08x process %08x" % (
                m.ostype(p + 0x74), m.u8(p + 0x18), m.u8(p + 0x19), m.u32(p + 0x1C),
                m.u32(p + 0x64), m.u32(p + 0x60))
            cb = m.u32(p + 0x88)
            if cb and cb < len(m.d):
                line += ' context %08x (pc %08x)' % (cb, m.u32(cb + 0xFC) if cb + 0x100 < len(m.d) else 0)
        elif cls == 4:
            line += '  reserve %d messages %d waiting %d' % (m.u32(p + 0x24), m.u32(p + 0x30), m.u32(p + 0x2C))
        elif cls == 5:
            line += '  value %d max %d' % (m.u32(p + 0x10), m.u32(p + 0x14))
        elif cls == 9:
            swi = m.u32(p + 0x18)
            line += '  flags %08x swi %08x%s' % (m.u32(p + 0x10), swi,
                                                  ' (68k level %d)' % (swi & 15) if swi & 0x10 else '')
        elif cls == 8:
            srs = [m.u32(p + 0x30 + 4 * i) for i in range(16)]
            line += '  SR0 %08x SR15 %08x' % (srs[0], srs[15])
        elif cls == 11:
            line += area(m, p)
        elif cls == 12:
            line += '  queue %08x event group %08x flags %08x semaphore %08x' % (
                m.u32(p + 0xC), m.u32(p + 0x1C), m.u32(p + 0x20), m.u32(p + 0x24))
        if verbose or cls in (1, 2, 8, 9, 11):
            print(line)
    return out


def area(m, p):
    base, end, length = m.u32(p + 0x24), m.u32(p + 0x28), m.u32(p + 0x2C)
    flags, pte, contig, paged = m.u32(p + 8), m.u32(p + 0x1C), m.u32(p + 0x70), m.u32(p + 0x74)
    s = '  %08x-%08x len %08x flags %08x pteconfig %08x' % (base, end, length, flags, pte)
    if contig:
        s += ' contig %08x' % contig
    if paged:
        s += ' paged-template %08x' % paged
    bp = m.u32(p + 0x18)
    if bp:
        s += ' backing %08x' % bp
    return s


def tree(m):
    hw = m.phys(m.u32(m.kdp + 0xFD0))
    dt = m.u32(hw + 4)
    pa = m.phys(dt)
    print('Device tree copy: logical %08x physical %08x' % (dt, pa))
    sys.stdout.flush()
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'devtree.py')
    subprocess.call([sys.executable, tool, m.path, hex(pa)])


def main():
    ap = argparse.ArgumentParser(usage=__doc__)
    ap.add_argument('dump')
    ap.add_argument('--kdp', type=lambda s: int(s, 0), default=0x0FBFE000)
    ap.add_argument('--tree', action='store_true')
    a = ap.parse_args()
    m = Dump(a.dump, a.kdp)
    m.path = a.dump
    info_records(m)
    globals_(m)
    page_map(m)
    objects(m)
    if a.tree:
        tree(m)


if __name__ == '__main__':
    main()
