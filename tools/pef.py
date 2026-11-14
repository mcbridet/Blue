#!/usr/bin/env python3
"""Inspect PEF containers (Code Fragment Manager format), e.g. NDRVs.

  pef.py <file> [offset]      dump sections, imports, exports and, for
                              drivers, TheDriverDescription
"""
import struct
import sys

KINDS = {0: 'code', 1: 'data', 2: 'pidata', 3: 'const', 4: 'loader', 5: 'debug',
         6: 'execdata', 7: 'exception', 8: 'traceback'}


def pidata(packed, size):
    """Expands a pattern-initialized data section."""
    out = bytearray()
    p = 0

    def count():
        nonlocal p
        v = 0
        while True:
            b = packed[p]
            p += 1
            v = (v << 7) | (b & 0x7F)
            if not b & 0x80:
                return v

    while p < len(packed) and len(out) < size:
        op, n = packed[p] >> 5, packed[p] & 0x1F
        p += 1
        if n == 0:
            n = count()
        if op == 0:
            out += bytes(n)
        elif op == 1:
            out += packed[p:p + n]
            p += n
        elif op == 2:
            reps = count() + 1
            out += packed[p:p + n] * reps
            p += n
        elif op in (3, 4):
            common = n
            custom = count()
            reps = count()
            block = packed[p:p + common] if op == 3 else bytes(common)
            if op == 3:
                p += common
            for _ in range(reps):
                out += block
                out += packed[p:p + custom]
                p += custom
            out += block
        else:
            raise ValueError('bad pidata opcode %d' % op)
    return bytes(out)


class PEF:
    def __init__(self, blob):
        if blob[:12] != b'Joy!peffpwpc':
            raise ValueError('not a PowerPC PEF container')
        self.blob = blob
        (nsec,) = struct.unpack('>H', blob[32:34])
        self.sections = []
        for k in range(nsec):
            h = blob[40 + 28 * k:68 + 28 * k]
            name, addr, total, unpacked, packed, off, kind, share, align, _ = struct.unpack('>iIIIIIBBBB', h)
            raw = blob[off:off + packed]
            data = pidata(raw, unpacked) + bytes(total - unpacked) if kind == 2 else raw + bytes(max(0, total - len(raw)))
            self.sections.append(dict(kind=kind, addr=addr, total=total, unpacked=unpacked,
                                      packed=packed, offset=off, data=data))
        loader = next(s for s in self.sections if s['kind'] == 4)['data']
        f = struct.unpack('>iIiIiIIIIIIIII', loader[:56])
        (self.main_sec, self.main_off, self.init_sec, self.init_off, self.term_sec, self.term_off,
         nlibs, nimports, nrelocsec, reloc_off, strings_off, hash_off, hash_power, nexports) = f
        strings = loader[strings_off:]

        def cstr(o):
            return strings[o:strings.index(b'\0', o)].decode('latin1')

        self.libs = []
        for k in range(nlibs):
            name_off, old, cur, nsym, first, opts, _, _ = struct.unpack('>IIIIIBBH', loader[56 + 24 * k:80 + 24 * k])
            self.libs.append((cstr(name_off), first, nsym))
        imp = 56 + 24 * nlibs
        self.imports = []
        for k in range(nimports):
            (w,) = struct.unpack('>I', loader[imp + 4 * k:imp + 4 * k + 4])
            self.imports.append((w >> 24 & 0xF, cstr(w & 0xFFFFFF)))
        keys = hash_off + 4 * (1 << hash_power)
        syms = keys + 4 * nexports
        self.exports = []
        for k in range(nexports):
            w, value, sec = struct.unpack('>IIh', loader[syms + 10 * k:syms + 10 * k + 10])
            (key,) = struct.unpack('>I', loader[keys + 4 * k:keys + 4 * k + 4])
            name_off, name_len = w & 0xFFFFFF, key >> 16
            name = strings[name_off:name_off + name_len].decode('latin1')
            self.exports.append((name, w >> 24, value, sec))

    def symbol(self, name):
        for n, cls, value, sec in self.exports:
            if n == name:
                return sec, value
        return None


def pstr(b, o, maxlen=31):
    n = b[o]
    return b[o + 1:o + 1 + min(n, maxlen)].decode('latin1')


def describe_driver(pef):
    s = pef.symbol('TheDriverDescription')
    if not s:
        return
    d = pef.sections[s[0]]['data'][s[1]:]
    sig, ver = d[0:4], struct.unpack('>I', d[4:8])[0]
    print('TheDriverDescription: signature %s version %d' % (sig, ver))
    print('  device name match: "%s"  version %08x' % (pstr(d, 8), struct.unpack('>I', d[40:44])[0]))
    runtime = struct.unpack('>I', d[44:48])[0]
    print('  runtime flags %08x  driver name "%s"' % (runtime, pstr(d, 48)))
    o = 48 + 32 + 32                    # driverName[32], reserved[8 longs]
    (nservices,) = struct.unpack('>I', d[o:o + 4])
    for k in range(nservices):
        cat, typ, vers = struct.unpack('>4s4sI', d[o + 4 + 12 * k:o + 16 + 12 * k])
        print('  service %s/%s version %08x' % (cat.decode('latin1'), typ.decode('latin1'), vers))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    blob = open(sys.argv[1], 'rb').read()
    off = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    pef = PEF(blob[off:])
    for k, s in enumerate(pef.sections):
        print('section %d: %-8s size %6d (unpacked %d, packed %d)' % (
            k, KINDS.get(s['kind'], s['kind']), s['total'], s['unpacked'], s['packed']))
    for name, first, n in pef.libs:
        print('imports from %s:' % name, ', '.join(pef.imports[i][1] for i in range(first, first + n)))
    print('exports:', ', '.join('%s (sec %d +0x%x)' % (n, sec, v) for n, cls, v, sec in pef.exports))
    describe_driver(pef)


if __name__ == '__main__':
    main()
