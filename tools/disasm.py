#!/usr/bin/env python3
"""Disassemble PowerPC or 68k guest code, naming the code fragment it is in.

  disasm.py [--ppc | --68k] FILE... ADDR [COUNT]    COUNT instructions (24)
  disasm.py [--ppc | --68k] FILE... ADDR:END
  disasm.py --list FILE...                          what the files hold, where

FILE is a "Mac OS ROM" file or a 4 MB Toolbox image (mapped at 0xFFC00000),
and/or a RAM dump from `syshm boot ... --dump-ram FILE` (at address 0).

ADDR is hex, or the name of a code fragment or of a symbol one exports, with
an optional +hex offset: MPCreateTask, StartLib+0x40. The fragments are the
PEF containers in the ROM image (named by the ROM resources they are in), the
libraries and drivers in the ROM file's parcels, and the PEF containers in
RAM (named when they are one of those; code sections copied elsewhere in RAM
are recognised too). A parcel's library that isn't in RAM is shown at its
code section's offsets.

The instruction set follows what ADDR is in (PowerPC in PEF code sections, the
NanoKernel, the 68k emulator and in RAM; 68k in the rest of the 68k ROM);
--ppc and --68k override it. PowerPC listings show the fragment's exports
and entry points as labels, and name imports where the code loads them from
the TOC or calls them through glue.

  tools/disasm.py MOS91.rom 0xFFCE2700
  tools/disasm.py MOS91.rom FFC015E0 40
  tools/disasm.py ram.bin MOS91.rom 0x1B200
  tools/disasm.py MOS91.rom MPCreateTask

Needs capstone (pip install capstone) for disassembling; --list doesn't.
"""
import argparse
import collections
import hashlib
import os
import re
import signal
import struct
import sys

from pef import PEF, pstr

try:
    import capstone
except ImportError:
    capstone = None

ROM_BASE = 0xFFC00000
ROM_SIZE = 0x400000
CONFIGINFO = 0x30D000                   # in the image
CODE_KINDS = (0, 6)                     # code, executable data

# ConfigInfo's PowerPC components: (offset field, size field, name); offsets are from ConfigInfo
ROM_PARTS = ((0x3C, 0x40, 'PowerPC exception table'), (0x44, 0x48, 'hardware init code'),
             (0x4C, 0x50, 'NanoKernel'), (0x54, 0x58, '68k emulator'), (0x5C, 0x60, 'opcode table'),
             (0x364, 0x368, 'Open Firmware bundle'))


def u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def lzss(src, size):
    """Decodes the LZSS of "Mac OS ROM" files (4 KB ring, starting at 0xFEE)."""
    ring = bytearray(4096)
    r = 0xFEE
    out = bytearray()
    i, n = 0, len(src)
    while len(out) < size and i < n:
        flags = src[i] | 0xFF00
        i += 1
        while flags & 0x100 and len(out) < size:
            if flags & 1:
                if i >= n:
                    break
                c = src[i]
                i += 1
                out.append(c)
                ring[r] = c
                r = (r + 1) & 0xFFF
            else:
                if i + 1 >= n:
                    break
                pos = src[i] | (src[i + 1] & 0xF0) << 4
                for k in range((src[i + 1] & 0x0F) + 3):
                    c = ring[(pos + k) & 0xFFF]
                    out.append(c)
                    ring[r] = c
                    r = (r + 1) & 0xFFF
                i += 2
            flags >>= 1
    return bytes(out[:size])


def cstr(b):
    return b.split(b'\0')[0].decode('mac_roman')


def quoted(b, off):
    """The C or Pascal string at off, quoted, if it looks like one."""
    if off is None or not 0 <= off < len(b):
        return None
    text = lambda s: len(s) >= 3 and all(32 <= c < 127 for c in s)
    end = b.find(b'\0', off, off + 100)
    if end > off and text(b[off:end]):
        return '"%s"' % b[off:end].decode('latin1')
    if text(b[off + 1:off + 1 + b[off]]) and off + 1 + b[off] <= len(b):
        return '"%s"' % b[off + 1:off + 1 + b[off]].decode('latin1')
    return None


def parcels(chain):
    """The parcels of a chain ('prcl'): dicts of type, a, b and blobs (type, name, data)."""
    out = []
    at = u32(chain, 12)
    while at:
        link, ptype, _, _, count, size = struct.unpack_from('>I4sIIII', chain, at)
        blobs = []
        for k in range(count):
            b = at + 88 + k * size
            btype, _, compress, unpacked, _, packed, ptr = struct.unpack_from('>4sI4sIIII', chain, b)
            data = chain[ptr:ptr + packed]
            blobs.append(dict(type=btype.decode('mac_roman'), name=cstr(chain[b + 28:b + 60]),
                              data=lzss(data, unpacked) if compress == b'lzss' else data))
        out.append(dict(type=ptype.decode('mac_roman'), a=cstr(chain[at + 24:at + 56]),
                        b=cstr(chain[at + 56:at + 88]), blobs=blobs))
        at = link
    return out


def read_rom_file(blob):
    """The 4 MB Toolbox image of a ROM file or image, and the file's parcels (or [])."""
    if len(blob) == ROM_SIZE:
        return blob, []
    end = blob.find(b'</CHRP-BOOT>', 0, 0x20000)
    script = blob[:end].decode('latin1')
    consts = dict((name, int(value, 16)) for value, name in re.findall(r'([0-9a-fA-F]+) constant ([\w-]+)', script))
    off = consts.get('parcels-offset', consts.get('lzss-offset'))
    size = consts.get('parcels-size', consts.get('lzss-size'))
    if off is None or size is None:
        sys.exit('no parcels-offset or lzss-offset in the boot script')
    packed = blob[off:off + size]
    if not packed.startswith(b'prcl'):
        return lzss(packed, ROM_SIZE), []
    ps = parcels(packed)
    image = next((b['data'] for p in ps if p['type'] == 'rom ' for b in p['blobs'] if b['type'] == 'rom '), None)
    if image is None or len(image) != ROM_SIZE:
        sys.exit('no 4 MB Toolbox image among the parcels')
    return image, ps


def pef_extent(blob, off):
    """The length of the PEF container at off: to the end of its last section."""
    (nsec,) = struct.unpack_from('>H', blob, off + 32)
    end = 40 + 28 * nsec
    for k in range(nsec):
        packed, soff = struct.unpack_from('>II', blob, off + 40 + 28 * k + 16)
        end = max(end, soff + packed)
    return end


def relocations(pef):
    """Where each instantiated section's loader relocations point:
    {section: {offset: ('section', n) or ('import', n)}}."""
    loader = next(s for s in pef.sections if s['kind'] == 4)['data']
    nlibs, nimports, nsecs, instr = struct.unpack_from('>IIII', loader, 24)
    heads = 56 + 24 * nlibs + 4 * nimports
    out = {}
    for k in range(nsecs):
        sec, _, count, first = struct.unpack_from('>HHII', loader, heads + 12 * k)
        code = struct.unpack_from('>%dH' % count, loader, instr + first)
        where = out.setdefault(sec, {})
        st = dict(pos=0, imp=0, c=0, d=1)
        limit = pef.sections[sec]['total'] if sec < len(pef.sections) else 0

        def put(target):
            where[st['pos']] = target
            st['pos'] += 4

        def run(lo, hi):
            i = lo
            while i < hi and st['pos'] <= limit:        # (past the section: not relocations)
                w = code[i]
                i += 1
                if w >> 14 == 0:                        # RelocBySectDWithSkip
                    st['pos'] += 4 * (w >> 6 & 0xFF)
                    for _ in range(w & 0x3F):
                        put(('section', st['d']))
                elif w >> 13 == 2:                      # RelocBySectC/D, TVector12/8, VTable8, ImportRun
                    sub = w >> 9 & 0xF
                    for _ in range((w & 0x1FF) + 1):
                        if sub in (0, 2, 3):
                            put(('section', st['c']))
                        if sub in (1, 2, 3, 4):
                            put(('section', st['d']))
                        if sub in (2, 4):
                            st['pos'] += 4
                        if sub == 5:
                            put(('import', st['imp']))
                            st['imp'] += 1
                elif w >> 13 == 3 or w >> 10 in (0x29, 0x2D):
                    if w >> 13 == 3:                    # RelocSmByImport, SmSetSectC/D, SmBySection
                        sub, n = w >> 9 & 0xF, w & 0x1FF
                    elif w >> 10 == 0x29:               # RelocLgByImport
                        sub, n = 0, (w & 0x3FF) << 16 | code[i]
                        i += 1
                    else:                               # RelocLgBySection, LgSetSectC/D
                        sub, n = (3, 1, 2, None)[min(w >> 6 & 0xF, 3)], (w & 0x3F) << 16 | code[i]
                        i += 1
                    if sub == 0:
                        put(('import', n))
                        st['imp'] = n + 1
                    elif sub == 1:
                        st['c'] = n
                    elif sub == 2:
                        st['d'] = n
                    elif sub == 3:
                        put(('section', n))
                elif w >> 12 == 8:                      # RelocIncrPosition
                    st['pos'] += (w & 0xFFF) + 1
                elif w >> 12 == 9 or w >> 10 == 0x2C:   # RelocSmRepeat, RelocLgRepeat
                    at = i - 1
                    if w >> 12 == 9:
                        blocks, times = (w >> 8 & 0xF) + 1, (w & 0xFF) + 1
                    else:
                        blocks, times = (w >> 6 & 0xF) + 1, ((w & 0x3F) << 16 | code[i]) + 1
                        i += 1
                    # (times more, after the first: so the relocations of Apple's fragments land on pointers)
                    for _ in range(times):
                        if st['pos'] > limit:
                            break
                        run(at - blocks, at)
                elif w >> 10 == 0x28:                   # RelocSetPosition
                    st['pos'] = (w & 0x3FF) << 16 | code[i]
                    i += 1
                else:
                    return

        run(0, len(code))
    return out


class Fragment:
    """A PEF container: its name, where it came from, and where its code is in guest memory."""

    def __init__(self, pef, blob, name, origin, container=None):
        self.pef, self.blob, self.name, self.origin = pef, blob, name, origin
        self.container = container
        self.code = {k: (container + s['offset'] if container is not None else None)
                     for k, s in enumerate(pef.sections) if s['kind'] in CODE_KINDS}
        self._relocs = self._labels = self._toc = None

    def label(self):
        if self.name:
            return self.name
        desc = self.pef.symbol('TheDriverDescription')
        if desc and 0 <= desc[0] < len(self.pef.sections):
            d = self.pef.sections[desc[0]]['data'][desc[1]:]
            if len(d) > 80 and d[:4] == b'mtej':
                return 'driver "%s" for "%s"' % (pstr(d, 48), pstr(d, 8))
        names = [n for n, cls, v, sec in self.pef.exports if sec >= 0][:3]
        return 'PEF' + (' exporting %s%s' % (', '.join(names), '...' if len(self.pef.exports) > 3 else '')
                        if names else '')

    def code_size(self, k):
        return self.pef.sections[k]['total']

    def code_at(self, addr):
        """(section, offset) of a guest address in one of the code sections, or None."""
        for k, base in self.code.items():
            if base is not None and base <= addr < base + self.code_size(k):
                return k, addr - base
        return None

    def relocs(self):
        if self._relocs is None:
            try:
                self._relocs = relocations(self.pef)
            except (struct.error, IndexError, StopIteration, RecursionError):
                self._relocs = {}
        return self._relocs

    def word(self, sec, off):
        """A word of a section's initial contents, and where its relocation points."""
        data = self.pef.sections[sec]['data']
        if off < 0 or off + 4 > len(data):
            return None, None
        return u32(data, off), self.relocs().get(sec, {}).get(off)

    def tvector(self, sec, off):
        """The code (section, offset) and TOC (section, offset) of a transition vector."""
        if sec < 0 or sec >= len(self.pef.sections):
            return None, None
        code, cr = self.word(sec, off)
        toc, tr = self.word(sec, off + 4)
        entry = (cr[1], code) if cr and cr[0] == 'section' and cr[1] in self.code else None
        if entry is None and not self.relocs() and 0 in self.code and code is not None and code < self.code_size(0):
            entry = (0, code)
        return entry, ((tr[1], toc) if tr and tr[0] == 'section' else None)

    def entries(self):
        """The transition vectors' (name, code (section, offset), TOC (section, offset)): exports and
        the main, init and term entry points."""
        out = []
        for name, cls, value, sec in self.pef.exports:
            if cls & 0xF == 2 and 0 <= sec < len(self.pef.sections):
                out.append((name,) + self.tvector(sec, value))
        for what, sec, off in (('main', self.pef.main_sec, self.pef.main_off),
                               ('init', self.pef.init_sec, self.pef.init_off),
                               ('term', self.pef.term_sec, self.pef.term_off)):
            out.append(('(%s)' % what,) + self.tvector(sec, off))
        return out

    def labels(self):
        """{(code section, offset): name} for the exports, entry points and glue."""
        if self._labels is not None:
            return self._labels
        out = {}
        for name, cls, value, sec in self.pef.exports:
            if cls & 0xF == 0 and sec in self.code:
                out[(sec, value)] = name
        for name, entry, toc in self.entries():
            if entry:
                out.setdefault(entry, name)
        # glue: lwz r12,d(r2); stw r2,20(r1); lwz r0,0(r12); lwz r2,4(r12); mtctr r0; bctr
        for k in self.code:
            code = self.pef.sections[k]['data']
            for m in re.finditer(b'\x81\x82..\x90\x41\x00\x14\x80\x0c\x00\x00\x80\x4c\x00\x04'
                                 b'\x7c\x09\x03\xa6\x4e\x80\x04\x20', code, re.S):
                imp = self.toc_entry(struct.unpack_from('>h', code, m.start() + 2)[0])
                if m.start() & 3 == 0 and imp and not imp.startswith('"'):
                    out.setdefault((k, m.start()), imp + ' (glue)')
        self._labels = out
        return out

    def toc(self):
        """The TOC as (section, offset): the one most transition vectors give."""
        if self._toc is None:
            tocs = collections.Counter(toc for _, _, toc in self.entries() if toc)
            self._toc = tocs.most_common(1)[0][0] if tocs else False
        return self._toc or None

    def toc_entry(self, d):
        """What the TOC entry at TOC + d points to: an import's name, or a string in a data section."""
        toc = self.toc()
        if toc is None:
            return None
        value, r = self.word(toc[0], toc[1] + d)
        if r and r[0] == 'import' and r[1] < len(self.pef.imports):
            return self.pef.imports[r[1]][1]
        if r and r[0] == 'section' and r[1] not in self.code and r[1] < len(self.pef.sections):
            return quoted(self.pef.sections[r[1]]['data'], value)
        return None


class Guest:
    """Guest memory (the ROM image and/or RAM) and what is in it."""

    def __init__(self):
        self.image = self.ram = None
        self.frags = []                 # Fragment
        self.regions = []               # (start, end, name), the ROM's parts and resources
        self.parcels = []

    def read(self, addr, n):
        if self.image is not None and ROM_BASE <= addr < ROM_BASE + ROM_SIZE:
            return self.image[addr - ROM_BASE:min(addr - ROM_BASE + n, ROM_SIZE)]
        if self.ram is not None and 0 <= addr < len(self.ram):
            return self.ram[addr:addr + n]
        return b''

    def load_rom(self, blob):
        self.image, self.parcels = read_rom_file(blob)
        img = self.image
        for off, size_field, name in ROM_PARTS:
            start = CONFIGINFO + struct.unpack_from('>i', img, CONFIGINFO + off)[0]
            size = u32(img, CONFIGINFO + size_field)
            if 0 < size and 0 <= start < start + size <= ROM_SIZE:
                self.regions.append((ROM_BASE + start, ROM_BASE + start + size, name))
        self.regions.append((ROM_BASE + CONFIGINFO, ROM_BASE + CONFIGINFO + 0x1000, 'ConfigInfo'))
        m68k = u32(img, CONFIGINFO + 0x38)
        if not 0 < m68k <= ROM_SIZE:
            m68k = 0x300000
        rsrc = u32(img, 26)
        if not 0 < rsrc < m68k:
            rsrc = 0
        self.regions.append((ROM_BASE, ROM_BASE + (rsrc or m68k), '68k ROM code'))
        resources = []
        link = u32(img, rsrc) if rsrc else 0
        while link and link < m68k and len(resources) < 4096:
            _, nxt, data, rtype, rid = struct.unpack_from('>QII4sh', img, link)
            name = img[link + 24:link + 24 + img[link + 23]].decode('mac_roman')
            size = u32(img, data - 8) - 12           # (the 16 bytes before the data: 'Kurt', 0xC0A00000, size + 12, ...)
            what = "'%s' %d" % (rtype.decode('mac_roman'), rid) + (' "%s"' % name if name else '')
            resources.append((ROM_BASE + data, ROM_BASE + data + size, what, name))
            self.regions.append((ROM_BASE + data, ROM_BASE + data + size, 'resource ' + what))
            link = nxt
        if resources:
            self.regions.append((min(r[0] for r in resources), ROM_BASE + m68k, '68k ROM resources'))
        self.regions.append((ROM_BASE, ROM_BASE + m68k, '68k ROM'))
        for off in self.find_pefs(img):
            addr = ROM_BASE + off
            res = next((r for r in resources if r[0] <= addr < r[1]), None)
            self.add_pef(img, off, res[3] if res else None,
                         'resource %s in the ROM image' % res[2] if res else 'in the ROM image', addr)
        for p in self.parcels:
            node = next((cstr(b['data']) for b in p['blobs'] if b['name'] == 'name'), '')
            given = next((cstr(b['data']) for b in p['blobs'] if b['name'] == 'code,AAPL,MacOS,name'), None)
            for b in p['blobs']:
                if b['data'].startswith(b'Joy!peffpwpc'):
                    if ',' not in b['name']:
                        name = b['name']
                    else:
                        name = given or '%s %s' % (p['a'] or p['b'], b['name'])
                    where = node if p['type'] == 'node' else '%s/%s' % (p['a'], p['b'])
                    self.add_pef(b['data'], 0, name, "'%s' in parcel '%s' %s" % (b['type'], p['type'], where))

    def load_ram(self, blob):
        self.ram = blob
        known = {}
        for f in self.frags:
            known.setdefault(hashlib.sha1(f.blob[f.offset:f.offset + f.length]).digest(), f)
        for off in self.find_pefs(blob):
            try:
                length = pef_extent(blob, off)
            except struct.error:
                continue
            if off + length > len(blob) or length > 64 << 20:
                continue                        # (a header at the end of RAM, or not one)
            same = known.get(hashlib.sha1(blob[off:off + length]).digest())
            self.add_pef(blob, off, same.name if same else None,
                         'in RAM' + (' (%s)' % same.origin if same else ''), off)

    @staticmethod
    def find_pefs(blob):
        off = blob.find(b'Joy!peffpwpc')
        while off >= 0:
            yield off
            off = blob.find(b'Joy!peffpwpc', off + 1)

    def add_pef(self, blob, off, name, origin, container=None):
        try:
            length = pef_extent(blob, off)
            pef = PEF(blob[off:off + length])
        except Exception:
            return None
        f = Fragment(pef, blob, name, origin, container)
        f.offset, f.length = off, length
        self.frags.append(f)
        return f

    def fragment_at(self, addr):
        """The fragment whose container or code section holds addr, as (fragment, code section, offset)."""
        for f in self.frags:
            at = f.code_at(addr)
            if at:
                return f, at[0], at[1]
        for f in self.frags:
            if f.container is not None and f.container <= addr < f.container + f.length:
                return f, None, addr - f.container
        if self.ram is not None and addr < len(self.ram):
            return self.find_copy(addr)
        return None

    def find_copy(self, addr):
        """A named fragment's code section copied to RAM around addr (all of it the same)."""
        addr &= ~3
        window = self.read(addr, 32)
        if len(window) < 32 or len(set(window)) < 4:
            return None
        for f in list(self.frags):
            if f.name is None:
                continue
            for k in f.code:
                code = f.pef.sections[k]['data'][:f.pef.sections[k]['packed']]
                at = code.find(window)
                while at >= 0:
                    base = addr - at
                    if at & 3 == 0 and base >= 0 and self.read(base, len(code)) == code:
                        if f.code[k] is not None:
                            f = self.add_pef(f.blob, f.offset, f.name, f.origin)
                        f.code[k] = base
                        f.origin += ', its code copied to RAM'
                        return f, k, at
                    at = code.find(window, at + 1)
        return None

    def region_at(self, addr):
        """The innermost ROM part or resource holding addr."""
        best = None
        for start, end, name in self.regions:
            if start <= addr < end and (best is None or end - start < best[1] - best[0]):
                best = (start, end, name)
        return best

    def symbol(self, text):
        """Resolves FRAGMENT[+off] or SYMBOL[+off]: (fragment, code section, offset), or None."""
        name, _, plus = text.partition('+')
        delta = int(plus, 16) if plus else 0
        frags = sorted(self.frags, key=lambda f: all(b is None for b in f.code.values()))
        for f in frags:
            if f.name == name and f.code:
                return f, min(f.code), delta
        for f in frags:
            for (k, off), label in f.labels().items():
                if label == name:
                    return f, k, off + delta
        return None


def describe(g, addr):
    """Where addr is, in words, and the instruction set to read it with."""
    hit = g.fragment_at(addr)
    if hit:
        f, k, off = hit
        where = '%s: %s' % (f.label(), f.origin)
        if f.container is not None:
            where += ', container at %08X' % f.container
        if k is None:
            kinds = [s for s in f.pef.sections if s['offset'] <= off < s['offset'] + s['packed']]
            what = ('%s section' % {1: 'data', 2: 'pidata', 3: 'const', 4: 'loader'}.get(
                kinds[0]['kind'], 'kind %d' % kinds[0]['kind'])) if kinds else 'header'
            return '%s; in its %s, +0x%X' % (where, what, off), 'ppc'
        before = [(o, name) for (s, o), name in f.labels().items() if s == k and o <= off]
        where += '; code section +0x%X' % off
        if before:
            o, name = max(before)
            where += ' (0x%X after %s)' % (off - o, name) if o < off else ' (%s)' % name
        return where, 'ppc'
    region = g.region_at(addr)
    if region:
        start, end, name = region
        arch = '68k' if name.startswith(('68k ROM', 'resource ')) else 'ppc'
        return '%s (%08X-%08X), +0x%X' % (name, start, end - 1, addr - start), arch
    return ('RAM' if g.ram is not None and addr < len(g.ram) else 'not in the files'), 'ppc'


def labels_by_address(g):
    out = {}
    for f in g.frags:
        if any(base is not None for base in f.code.values()):
            for (k, off), name in f.labels().items():
                if f.code.get(k) is not None:
                    out.setdefault(f.code[k] + off, name)
    return out


def show(line, note, column):
    print(('%-*s; %s' % (column, line, note)) if note else line)


def disassemble_ppc(g, addr, end, labels):
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    prev = g.read((addr & ~3) - 4, 4)
    r0 = u32(prev, 0) & 0xFFFF if len(prev) == 4 and u32(prev, 0) >> 16 == 0x3800 else None
    for a in range(addr & ~3, end, 4):
        b = g.read(a, 4)
        if len(b) < 4:
            print('%08X  (outside the files)' % a)
            return
        if a in labels:
            print('%s:' % labels[a])
        w = u32(b, 0)
        i = next(md.disasm(b, a), None)
        text = '%s %s' % (i.mnemonic, i.op_str) if i else '.long 0x%08x' % w
        note = None
        if w >> 26 == 18:                           # b, bl
            li = w & 0x03FFFFFC
            note = labels.get(((li ^ 0x02000000) - 0x02000000 + (0 if w & 2 else a)) & 0xFFFFFFFF)
        elif w >> 26 == 32 and (w >> 16 & 31) == 2:     # lwz rD,d(r2): a TOC entry
            f = next((f for f in g.frags if f.code_at(a)), None)
            note = f and f.toc_entry(struct.unpack('>h', b[2:])[0])
        elif w == 0x44000002 and r0 is not None:
            note = 'MP call %d' % r0
        r0 = w & 0xFFFF if w >> 16 == 0x3800 else None      # li r0,n
        show('%08X  %08x  %s' % (a, w, text.strip()), note, 56)


def disassemble_68k(g, addr, end, count):
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_040)
    a = addr & ~1
    n = 0
    while a < end and n < count:
        b = g.read(a, 10)
        if len(b) < 2:
            print('%08X  (outside the files)' % a)
            return
        w = struct.unpack('>H', b[:2])[0]
        i = None
        if w >> 12 != 0xF or (w >> 9 & 7) < 4:     # (F-line words for coprocessors 4-7 are emulator traps)
            i = next(md.disasm(b, a, 1), None)
        if i is None or i.size == 0:
            size, text = 2, 'dc.w $%04x' % w
        else:
            size, text = i.size, '%s %s' % (i.mnemonic, i.op_str)
        note = None
        if w >> 12 == 0xA:
            note = 'A-line trap (%s)' % ('Toolbox' if w & 0x800 else 'OS')
        elif w >> 12 == 0xF and size == 2:
            note = 'F-line trap'
        else:
            m = re.search(r'\$([0-9a-f]+)\(pc\)', text)
            if m:
                note = quoted(g.read(int(m.group(1), 16), 100), 0)
        show('%08X  %-20s  %s' % (a, b[:size].hex(), text.strip()), note, 66)
        a += size
        n += 1


def list_contents(g):
    rows = []
    for start, end, name in g.regions:
        if not name.startswith('resource ') or any(f.container == start for f in g.frags):
            continue
        rows.append((start, end, name, ''))
    for start, end, name in g.regions:
        if not name.startswith('resource '):
            rows.append((start, end, name, ''))
    for f in g.frags:
        code = ', '.join('code %08X-%08X' % (b, b + f.code_size(k) - 1) for k, b in f.code.items() if b is not None)
        if f.container is not None:
            rows.append((f.container, f.container + f.length, '%s: %s' % (f.label(), f.origin), code))
        else:
            unmapped = [x for x in g.frags if x.name == f.name and x is not f and x.container is not None]
            if not unmapped:
                rows.append((None, None, '%s: %s' % (f.label(), f.origin), code or 'not in guest memory'))
    rows.sort(key=lambda r: (r[0] is None, r[0] or 0, -(r[1] or 0)))
    for start, end, name, extra in rows:
        span = '%08X-%08X' % (start, end - 1) if start is not None else '-' * 17
        print('%s  %s%s' % (span, name, ('  [%s]' % extra) if extra else ''))


def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument('--ppc', action='store_const', const='ppc', dest='arch')
    ap.add_argument('--68k', action='store_const', const='68k', dest='arch')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('-h', '--help', action='store_true')
    ap.add_argument('args', nargs='*')
    opt = ap.parse_args()
    files = [a for a in opt.args if os.path.isfile(a)]
    rest = [a for a in opt.args if not os.path.isfile(a)]
    if opt.help or not files or (not rest and not opt.list) or len(rest) > 2:
        sys.exit(__doc__.strip())

    g = Guest()
    rams = []
    for path in files:
        blob = open(path, 'rb').read()
        if blob.startswith(b'<CHRP-BOOT>') or len(blob) == ROM_SIZE:
            g.load_rom(blob)
        else:
            rams.append(blob)
    for blob in rams:
        g.load_ram(blob)

    if opt.list:
        list_contents(g)
        return
    if capstone is None:
        sys.exit('disasm.py needs capstone to disassemble: pip install capstone')

    text = rest[0]
    end = None
    if ':' in text:
        text, _, last = text.partition(':')
        end = int(last, 16)
    count = int(rest[1], 0) if len(rest) > 1 else 24
    sym = g.symbol(text)
    if sym is None and not re.fullmatch(r'(0x)?[0-9a-fA-F]+', text):
        sys.exit('%s: no such fragment or exported symbol' % text)
    if sym and sym[0].code[sym[1]] is None:
        f, k, off = sym
        view = Guest()
        view.ram = f.pef.sections[k]['data']
        view.frags = [Fragment(f.pef, f.blob, f.name, f.origin)]
        view.frags[0].code[k] = 0
        print('%s: %s; not in guest memory, so the addresses are offsets in its code section' % (
            f.label(), f.origin))
        g, addr = view, off
    else:
        addr = sym[0].code[sym[1]] + sym[2] if sym else int(text, 16)
        where, arch = describe(g, addr)
        print('%08X: %s' % (addr, where))
    arch = opt.arch or ('ppc' if sym else arch)
    if arch == 'ppc':
        disassemble_ppc(g, addr, end if end is not None else (addr & ~3) + 4 * count, labels_by_address(g))
    else:
        disassemble_68k(g, addr, end if end is not None else 1 << 32, count if end is None else 1 << 32)


if __name__ == '__main__':
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    main()
