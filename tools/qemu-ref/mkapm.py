#!/usr/bin/env python3
"""Wrap a raw HFS volume in an Apple Partition Map so Open Firmware can boot it."""
import struct
import sys

PART_START = 64


def entry(map_count, start, count, name, ptype, status):
    e = struct.pack('>2sHIII32s32sIII', b'PM', 0, map_count, start, count,
                    name.encode(), ptype.encode(), 0, count, status)
    return e.ljust(512, b'\0')


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: mkapm.py <volume.hfs> <out.img>')
    hfs = open(sys.argv[1], 'rb').read()
    if len(hfs) % 512:
        sys.exit('volume size must be a multiple of 512')
    blocks = len(hfs) // 512
    ddm = struct.pack('>2sHI', b'ER', 512, PART_START + blocks).ljust(512, b'\0')
    img = (ddm
           + entry(2, 1, PART_START - 1, 'Apple', 'Apple_partition_map', 0x3)
           + entry(2, PART_START, blocks, 'MacOS', 'Apple_HFS', 0x7F))
    with open(sys.argv[2], 'wb') as f:
        f.write(img.ljust(PART_START * 512, b'\0') + hfs)


if __name__ == '__main__':
    main()
