#!/usr/bin/env python3
"""Walk Mac OS 8.5-9.2.2's heap zones in a RAM dump and report broken blocks.

  heap.py RAM [ZONE...]          the system heap (SysZone, 0x2A6), the current
                                 application's (ApplZone, 0x2AA) and these zones
  heap.py RAM --find ADDR...     the system heap's block holding each address

RAM is a dump from `syshm boot ... --dump-ram FILE` (at address 0); it is read
a block header at a time, never loaded whole. ZONE and ADDR are hex.

The Modern Memory Manager's zones, as found in Mac OS 9.2.2's: blocks run from
the zone + 0x30 to the last block, whose address is at the zone + 0x40 (the
rover the allocator starts from is at + 0x2C). Each block starts with a 16-byte
header: the previous block's address, a tag word (its top byte 0x6B for a
pointer's block, 0xAB for a handle's, 0x00 or 0x10 free), the block's size
with the header, and the zone or the handle. A block whose back link isn't the
block before it, or whose size runs off, is where a zone is broken.
"""
import argparse
import struct
import sys


class Ram:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(0, 2)
        self.size = self.f.tell()

    def read(self, addr, n):
        if addr + n > self.size:
            return None
        self.f.seek(addr)
        return self.f.read(n)

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('>I', b)[0] if b else None


def walk(ram, zone, name, find=()):
    """Checks a zone's blocks; returns the number of broken ones."""
    last = ram.u32(zone + 0x40)
    if last is None:
        print('%s zone %08X: not in the dump' % (name, zone))
        return 1
    block, prev, count, bad, kinds = zone + 0x30, None, 0, 0, {}
    while True:
        header = ram.read(block, 16)
        if header is None:
            print('  %s: block %08X is past the dump' % (name, block))
            bad += 1
            break
        back, tag, size, owner = struct.unpack('>IIII', header)
        kinds[tag >> 24] = kinds.get(tag >> 24, 0) + 1
        for addr in find:
            if block <= addr < block + size:
                print('%08X: in block %08X, size %08X, tag %08X, +12 %08X' % (addr, block, size, tag, owner))
        if prev is not None and back != prev:
            print('  %s: block %08X links back to %08X, not %08X (tag %08X, size %08X)' %
                  (name, block, back, prev, tag, size))
            bad += 1
            if bad > 10:
                break
        count += 1
        if block == last:
            break
        if size < 16 or size % 16 or block + size > ram.size:
            print('  %s: block %08X has size %08X (tag %08X, back %08X)' % (name, block, size, tag, back))
            bad += 1
            break
        prev, block = block, block + size
    if not find:
        print('%s zone %08X: %d blocks, %d broken, last %08X; tags %s' %
              (name, zone, count, bad, last, ' '.join('%02X:%d' % kv for kv in sorted(kinds.items()))))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('ram')
    ap.add_argument('zones', nargs='*', help='more zones (hex)')
    ap.add_argument('--find', nargs='+', metavar='ADDR', help='the system heap block holding each address (hex)')
    args = ap.parse_args()
    ram = Ram(args.ram)
    sys_zone, appl_zone = ram.u32(0x2A6), ram.u32(0x2AA)
    if args.find:
        walk(ram, sys_zone, 'system', [int(a, 16) for a in args.find])
        return 0
    bad = walk(ram, sys_zone, 'system') + walk(ram, appl_zone, 'application')
    for z in args.zones:
        bad += walk(ram, int(z, 16), 'zone')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
