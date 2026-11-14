#!/usr/bin/env python3
"""Dump the Mac OS device-tree copy (NKHWInfo.DeviceTreeBase format).

  devtree.py <memory dump> <offset of the tree in the dump>

Layout: 1000 node entries of {peer, child, props} offsets (each relative to
the entry), then property records {next (relative, 0 = last), name[32],
length, value padded to 4 bytes}.
"""
import struct
import sys


def show(value):
    if value and all(32 <= c < 127 for c in value.rstrip(b'\0')) and value.endswith(b'\0'):
        return '"%s"' % value.rstrip(b'\0').decode('latin1').replace('\0', '", "')
    if len(value) <= 32:
        return value.hex(' ', 4)
    return '<%d bytes: %s...>' % (len(value), value[:16].hex(' ', 4))


def main():
    blob = open(sys.argv[1], 'rb').read()
    base = int(sys.argv[2], 0)
    u32 = lambda o: struct.unpack('>I', blob[base + o:base + o + 4])[0]
    s32 = lambda o: struct.unpack('>i', blob[base + o:base + o + 4])[0]

    def props(entry):
        rel = s32(entry + 8)
        out = []
        if rel == 0:
            return out
        p = entry + rel
        while True:
            nxt = s32(p)
            name = blob[base + p + 4:base + p + 36].split(b'\0')[0].decode('latin1')
            n = u32(p + 36)
            out.append((name, blob[base + p + 40:base + p + 40 + n]))
            if nxt == 0:
                return out
            p += nxt

    def walk(entry, path, depth):
        ps = props(entry)
        name = next((v.rstrip(b'\0').decode('latin1') for k, v in ps if k == 'name'), '?')
        print('%s%s' % ('  ' * depth, name if depth else '/'))
        for k, v in ps:
            if k != 'name':
                print('%s    %-28s %s' % ('  ' * depth, k, show(v)))
        child = s32(entry + 4)
        if child:
            walk(entry + child, path + '/' + name, depth + 1)
        peer = s32(entry)
        if peer and depth:
            walk(entry + peer, path, depth)

    walk(0, '', 0)


if __name__ == '__main__':
    main()
