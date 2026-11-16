#!/usr/bin/env python3
"""
Makes Blue's icons from art/, with Python's standard library and headless
Chrome (for the SVG master). The outputs are committed, so building Blue
needs neither.

  tools/icons.py [app] [finder] [site]      (all three without arguments)

app     art/blue.svg (the master) rendered to art/png/blue-N.png, N from 16
        to 1024, and src/icon_data.inc: the window's icon (SDL sets the
        Dock's from it on macOS), 256 x 256: the 512 one halved, in 256
        colours, its pixels LZSS-packed as lzss_decode() reads them.
finder  The Finder icons of the guest tools and their volume in Mac OS,
        pixel art in art/finder-icons.txt, to src/finder_icons.inc (each
        icon's ICN#, icl8, icl4, ics#, ics8 and ics4 resources) and a
        preview, art/finder-icons.png.
site    The master's stack as site/favicon.svg and site/index.html's splash
        logo (inline, a group for each slab), and site/og-image.png (1200 x 630).

CHROME names the browser (else Chrome, Chrome Canary or Chromium where they
usually are, or on the PATH).
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, 'art')
APP_SIZES = (16, 24, 32, 48, 64, 128, 256, 512, 1024)
WINDOW_ICON = 256


# ---- PNG ------------------------------------------------------------------

def png_read(path):
    """(width, height, RGBA bytes) of an 8-bit RGB or RGBA PNG, not interlaced"""
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat = 8, b''
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and ctype in (2, 6) and not interlace, path
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray()
    prev = bytearray(stride)
    for y in range(h):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if bpp == 4:
            out += line
        else:
            for x in range(w):
                out += line[3 * x:3 * x + 3] + b'\xff'
    return w, h, bytes(out)


def png_write(path, w, h, rgba):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


# ---- LZSS, as lzss_decode() reads it (src/lzss.h) -----------------------------

def lzss_encode(data):
    ring, start, min_match, max_match = 4096, 0xFEE, 3, 18
    out = bytearray()
    heads = {}
    i, n = 0, len(data)
    while i < n:
        flags_at = len(out)
        out.append(0)
        for bit in range(8):
            if i >= n:
                break
            best, best_at = 0, 0
            for j in reversed(heads.get(data[i:i + 3], [])[-64:]):
                if i - j >= ring - max_match:
                    break
                k = 0
                while k < max_match and i + k < n and data[j + k] == data[i + k]:
                    k += 1
                if k > best:
                    best, best_at = k, j
                    if k == max_match:
                        break
            if best >= min_match:
                pos = (start + best_at) % ring
                out += bytes((pos & 0xFF, (pos >> 4) & 0xF0 | (best - min_match)))
                step = best
            else:
                out[flags_at] |= 1 << bit
                out.append(data[i])
                step = 1
            for k in range(i, i + step):
                heads.setdefault(data[k:k + 3], []).append(k)
            i += step
    return bytes(out)


def lzss_decode(src, size):
    ring = bytearray(4096)
    r, out, i, flags = 0xFEE, bytearray(), 0, 0
    while len(out) < size:
        flags >>= 1
        if not flags & 0x100:
            flags = src[i] | 0xFF00
            i += 1
        if flags & 1:
            c = src[i]
            i += 1
            out.append(c)
            ring[r] = c
            r = (r + 1) & 4095
        else:
            pos = src[i] | (src[i + 1] & 0xF0) << 4
            length = (src[i + 1] & 15) + 3
            i += 2
            for k in range(length):
                c = ring[(pos + k) & 4095]
                out.append(c)
                ring[r] = c
                r = (r + 1) & 4095
    return bytes(out[:size])


def c_bytes(name, data, comment=''):
    lines = [f'static const uint8_t {name}[{len(data)}] = {{{comment}']
    for k in range(0, len(data), 16):
        lines.append('\t' + ', '.join(f'0x{b:02X}' for b in data[k:k + 16]) + ',')
    lines.append('};')
    return '\n'.join(lines) + '\n'


GENERATED = '/* Generated by tools/icons.py from {}: not to be edited. */\n'


# ---- rendering with Chrome --------------------------------------------------

def chrome():
    candidates = [os.environ.get('CHROME'),
                  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
                  '/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary',
                  '/Applications/Chromium.app/Contents/MacOS/Chromium',
                  shutil.which('google-chrome'), shutil.which('chromium'), shutil.which('chromium-browser')]
    for c in candidates:
        if c and os.path.exists(c):
            return c
    sys.exit('icons: no Chrome or Chromium (set CHROME)')


def render(html, width, height, out, tmp):
    """A page's screenshot, transparent where the page draws nothing"""
    page = os.path.join(tmp, 'page.html')
    with open(page, 'w') as f:
        f.write(html)
    subprocess.run([chrome(), '--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
                    '--default-background-color=00000000', f'--window-size={width},{height}',
                    f'--user-data-dir={os.path.join(tmp, "profile")}', f'--screenshot={out}', 'file://' + page],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def master():
    with open(os.path.join(ART, 'blue.svg')) as f:
        return f.read()


def page(body, css=''):
    return ('<!doctype html><html><head><meta charset="utf-8"><style>html,body{margin:0;background:transparent}'
            f'svg{{display:block}}{css}</style></head><body>{body}</body></html>')


def app_icon_css(size):
    """
    At small sizes the tile fills the square (no room for its shadow) and
    the stack is bigger, with outlines at least about a pixel wide.
    """
    if size >= 128:
        return f'svg{{width:{size}px;height:{size}px}}'
    return (f'svg{{width:{size}px;height:{size}px}}'
            '#tile rect{x:24px;y:24px;width:976px;height:976px;rx:200px;filter:none}'
            '#tile rect+rect{x:32px;y:32px;width:960px;height:960px;rx:192px}'
            '#stack{transform:translate(512px,524px) scale(29) translate(-16px,-17px);'
            f'--stroke:{max(0.4, 0.8 * 1024 / (29 * size)):.3f}px}}')


def make_app(tmp):
    svg = master()
    os.makedirs(os.path.join(ART, 'png'), exist_ok=True)
    for size in APP_SIZES:
        out = os.path.join(ART, 'png', f'blue-{size}.png')
        render(page(svg, app_icon_css(size)), size, size, out, tmp)
        print('icons:', os.path.relpath(out, ROOT))
    w, h, rgba = png_read(os.path.join(ART, 'png', f'blue-{2 * WINDOW_ICON}.png'))
    rgba = halve(w, h, rgba)
    palette, pixels = median_cut(rgba, 256)
    packed = lzss_encode(pixels)
    assert lzss_decode(packed, len(pixels)) == pixels
    out = os.path.join(ROOT, 'src', 'icon_data.inc')
    with open(out, 'w') as f:
        f.write(GENERATED.format('art/blue.svg'))
        f.write(f'/* art/png/blue-{2 * WINDOW_ICON}.png at half its size: its colours (RGBA), and its pixels, LZSS-packed */\n')
        f.write(c_bytes('icon_palette', b''.join(bytes(c) for c in palette) + bytes(4 * (256 - len(palette)))))
        f.write(c_bytes('icon_lzss', packed))
    print(f'icons: {os.path.relpath(out, ROOT)} ({len(packed)} bytes for {WINDOW_ICON} x {WINDOW_ICON} pixels)')


def halve(w, h, rgba):
    """Half the size, each pixel the average of four (weighted by their alpha)"""
    out = bytearray()
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            px = [rgba[((y + dy) * w + x + dx) * 4:((y + dy) * w + x + dx) * 4 + 4] for dy in (0, 1) for dx in (0, 1)]
            a = sum(p[3] for p in px)
            if not a:
                out += bytes(4)
                continue
            out += bytes(round(sum(p[c] * p[3] for p in px) / a) for c in range(3)) + bytes((round(a / 4),))
    return bytes(out)


def median_cut(rgba, n):
    """At most n colours for an RGBA image (median cut), and its pixels as their indices"""
    count = {}
    for k in range(0, len(rgba), 4):
        count[rgba[k:k + 4]] = count.get(rgba[k:k + 4], 0) + 1
    boxes = [list(count.items())]
    while len(boxes) < n:
        best = None
        for i, box in enumerate(boxes):
            if len(box) < 2:
                continue
            ranges = [max(c[0][ch] for c in box) - min(c[0][ch] for c in box) for ch in range(4)]
            ch = max(range(4), key=lambda c: ranges[c])
            score = ranges[ch] * sum(c[1] for c in box) ** 0.5
            if not best or score > best[0]:
                best = (score, i, ch)
        if not best or not best[0]:
            break
        _, i, ch = best
        box = sorted(boxes.pop(i), key=lambda c: c[0][ch])
        total, acc, k = sum(c[1] for c in box), 0, 0
        while k < len(box) - 1 and acc + box[k][1] < total / 2:
            acc += box[k][1]
            k += 1
        k = max(1, k)
        boxes += [box[:k], box[k:]]
    palette = []
    for box in boxes:
        t = sum(c[1] for c in box)
        palette.append(tuple(round(sum(c[0][ch] * c[1] for c in box) / t) for ch in range(4)))
    index = {col: min(range(len(palette)), key=lambda i: sum((palette[i][ch] - col[ch]) ** 2 for ch in range(4)))
             for col in count}
    return palette, bytes(index[rgba[k:k + 4]] for k in range(0, len(rgba), 4))


# ---- the Finder icons -----------------------------------------------------------

def mac_clut8():
    """Mac OS's 8-bit system palette ('clut' 8)"""
    steps = [0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00]
    c = [(r, g, b) for r in steps for g in steps for b in steps][:215]
    ramp = [0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11]
    c += [(v, 0, 0) for v in ramp] + [(0, v, 0) for v in ramp] + [(0, 0, v) for v in ramp] + [(v, v, v) for v in ramp]
    return c + [(0, 0, 0)]


# Mac OS's 4-bit system palette ('clut' 4)
CLUT4 = [(0xFF, 0xFF, 0xFF), (0xFC, 0xF3, 0x05), (0xFF, 0x64, 0x02), (0xDD, 0x08, 0x06), (0xF2, 0x08, 0x84),
         (0x46, 0x00, 0xA5), (0x00, 0x00, 0xD4), (0x02, 0xAB, 0xEA), (0x1F, 0xB7, 0x14), (0x00, 0x64, 0x11),
         (0x56, 0x2C, 0x05), (0x90, 0x71, 0x3A), (0xC0, 0xC0, 0xC0), (0x80, 0x80, 0x80), (0x40, 0x40, 0x40),
         (0x00, 0x00, 0x00)]


def read_finder_art():
    """The palette (key: 8-bit colour, 4-bit index, 1-bit pattern) and the icons, in the file's order"""
    keys, icons, current = {}, [], None
    with open(os.path.join(ART, 'finder-icons.txt')) as f:
        lines = f.read().split('\n')
    clut8 = mac_clut8()
    for line in lines:
        if line.startswith('#') or not line.strip():
            continue
        m = re.match(r'key (\S) +#([0-9A-Fa-f]{6}) +(\d+) +([wbhq])\b', line)
        if m:
            rgb = tuple(int(m.group(2)[k:k + 2], 16) for k in (0, 2, 4))
            assert rgb in clut8, f'{line}: not in the 8-bit palette'
            keys[m.group(1)] = (clut8.index(rgb), int(m.group(3)), m.group(4))
            continue
        m = re.match(r'icon (\w+) (32|16)$', line)
        if m:
            current = (m.group(1), int(m.group(2)), [])
            icons.append(current)
            continue
        assert current and len(current[2]) < current[1], line
        assert len(line) == current[1] and all(c == '.' or c in keys for c in line), line
        current[2].append(line)
    return keys, icons


def bw_bit(pattern, x, y):
    return {'w': 0, 'b': 1, 'h': (x + y) & 1, 'q': (x & 1) == 0 and (y & 1) == 0}[pattern]


def icon_resources(keys, grid):
    """(1-bit icon and mask, 8-bit, 4-bit) of a 32 x 32 or 16 x 16 grid"""
    n = len(grid)
    bits, mask, i8, i4 = bytearray(n * n // 8), bytearray(n * n // 8), bytearray(), []
    for y, row in enumerate(grid):
        for x, c in enumerate(row):
            if c == '.':
                i8.append(0)
                i4.append(0)
                continue
            k8, k4, bw = keys[c]
            i8.append(k8)
            i4.append(k4)
            mask[(y * n + x) // 8] |= 0x80 >> (x & 7)
            if bw_bit(bw, x, y):
                bits[(y * n + x) // 8] |= 0x80 >> (x & 7)
    nib = bytes(i4[k] << 4 | i4[k + 1] for k in range(0, len(i4), 2))
    return bytes(bits + mask), bytes(i8), nib


def make_finder(tmp):
    keys, icons = read_finder_art()
    names = []
    for name, _, _ in icons:
        if name not in names:
            names.append(name)
    out = [GENERATED.format('art/finder-icons.txt'),
           '/* Each icon\'s resources, as the Finder takes them (a custom icon: ID -16455) */\n',
           'enum { ' + ', '.join(f'FINDER_ICON_{n.upper()}' for n in names) + ', FINDER_ICONS };\n\n',
           'static const struct finder_icon {\n'
           '\tuint8_t icn[256], icl8[1024], icl4[512];\t/* ICN# (icon and mask), icl8, icl4: 32 x 32 */\n'
           '\tuint8_t ics[64], ics8[256], ics4[128];\t\t/* ics#, ics8, ics4: 16 x 16 */\n'
           '} finder_icons[FINDER_ICONS] = {\n']
    clut8 = mac_clut8()
    scale, pad = 4, 8
    sheet_w = pad + len(names) * (32 * scale + pad)
    sheet_h = pad + 3 * (32 * scale + pad) + 3 * (16 * scale + pad)
    sheet = bytearray(b'\xdd\xdd\xdd\xff' * sheet_w * sheet_h)
    for col, name in enumerate(names):
        res = {}
        for size in (32, 16):
            grid = [g for nm, sz, g in icons if nm == name and sz == size]
            assert len(grid) == 1 and len(grid[0]) == size, f'{name} {size}'
            res[size] = icon_resources(keys, grid[0])
            # the preview: 8-bit, 4-bit and 1-bit, at 32 and 16
            bits, i8, i4 = res[size]
            for depth in range(3):
                oy = pad + (depth * (32 * scale + pad) if size == 32 else 3 * (32 * scale + pad) +
                            depth * (16 * scale + pad))
                ox = pad + col * (32 * scale + pad)
                for y in range(size):
                    for x in range(size):
                        k = y * size + x
                        if not bits[size * size // 8 + k // 8] & (0x80 >> (x & 7)):
                            continue
                        if depth == 0:
                            rgb = clut8[i8[k]]
                        elif depth == 1:
                            rgb = CLUT4[i4[k // 2] >> (0 if k & 1 else 4) & 15]
                        else:
                            rgb = (0, 0, 0) if bits[k // 8] & (0x80 >> (x & 7)) else (255, 255, 255)
                        for dy in range(scale):
                            at = ((oy + y * scale + dy) * sheet_w + ox + x * scale) * 4
                            sheet[at:at + scale * 4] = bytes(rgb + (255,)) * scale
        out.append(f'\t[FINDER_ICON_{name.upper()}] = {{\n')
        for label, data in (('icn', res[32][0]), ('icl8', res[32][1]), ('icl4', res[32][2]),
                            ('ics', res[16][0]), ('ics8', res[16][1]), ('ics4', res[16][2])):
            out.append(f'\t\t.{label} = {{\n')
            for k in range(0, len(data), 16):
                out.append('\t\t\t' + ', '.join(f'0x{b:02X}' for b in data[k:k + 16]) + ',\n')
            out.append('\t\t},\n')
        out.append('\t},\n')
    out.append('};\n')
    path = os.path.join(ROOT, 'src', 'finder_icons.inc')
    with open(path, 'w') as f:
        f.write(''.join(out))
    print('icons:', os.path.relpath(path, ROOT))
    preview = os.path.join(ART, 'finder-icons.png')
    png_write(preview, sheet_w, sheet_h, bytes(sheet))
    print('icons:', os.path.relpath(preview, ROOT), '(8-, 4- and 1-bit, at 32 and 16 pixels, magnified)')


# ---- the site ------------------------------------------------------------------

def stack_parts():
    """The master's stack alone, in its own 32-unit space: its gradients, and its paths"""
    svg = master()
    defs = re.search(r'<defs>(.*?)</defs>', svg, re.S).group(1)
    defs = re.sub(r'\s*<filter.*?</filter>', '', defs, flags=re.S)
    defs = re.sub(r'\s*<linearGradient id="tile-.*?</linearGradient>', '', defs, flags=re.S)
    paths = re.search(r'<g id="stack"[^>]*>(.*?)</g>', svg, re.S).group(1)
    return defs.strip('\n'), paths.strip('\n')


def indent(text, by):
    lines = [line.rstrip() for line in text.split('\n') if line.strip()]
    least = min(len(line) - len(line.lstrip()) for line in lines)
    return '\n'.join(by + line[least:] for line in lines)


def stack_svg():
    defs, paths = stack_parts()
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32">\n  <defs>\n' + indent(defs, '    ') +
            '\n  </defs>\n  <g stroke="#0b2e38" stroke-linejoin="round">\n' + indent(paths, '    ') + '\n  </g>\n</svg>\n')


def stack_slabs(paths):
    """The stack's paths in a list for each slab, bottom first: each slab's starts with its top face"""
    slabs = []
    for line in (line.strip() for line in paths.split('\n') if line.strip()):
        if '-top)' in line or not slabs:
            slabs.append([])
        slabs[-1].append(line)
    return slabs


def splash_logo():
    """... as index.html's splash logo, inline, each slab in a group of its own so they can drop into place"""
    defs, paths = stack_parts()
    slabs = ''.join('\n            <g class="slab">\n' + '\n'.join('              ' + p for p in slab) + '\n            </g>'
                    for slab in stack_slabs(paths))
    return ('<svg class="splash-logo" viewBox="0 0 32 32" aria-hidden="true">\n          <defs>\n' + indent(defs, '            ') +
            '\n          </defs>\n          <g class="stack" stroke="#0b2e38" stroke-linejoin="round">' + slabs +
            '\n          </g>\n        </svg>')


def make_site(tmp):
    favicon = os.path.join(ROOT, 'site', 'favicon.svg')
    with open(favicon, 'w') as f:
        f.write(stack_svg())
    print('icons:', os.path.relpath(favicon, ROOT))
    index = os.path.join(ROOT, 'site', 'index.html')
    with open(index) as f:
        html = f.read()
    html = re.sub(r'\n  <symbol id="i-logo".*?</symbol>', '', html, count=1, flags=re.S)
    html, n = re.subn(r'<svg class="splash-logo".*?</svg>', lambda m: splash_logo(), html, count=1, flags=re.S)
    assert n == 1, 'index.html: no splash logo'
    with open(index, 'w') as f:
        f.write(html)
    print('icons:', os.path.relpath(index, ROOT), '(the splash logo)')
    svg = master()
    card = ('<div style="width:1200px;height:630px;position:relative;overflow:hidden;'
            'background:#0095b6 linear-gradient(160deg,#36b4cf 0%,#0095b6 55%,#007894 100%);'
            'font-family:Charcoal,Chicago,Geneva,\'Lucida Grande\',\'Helvetica Neue\',Arial,sans-serif">'
            '<div style="position:absolute;left:70px;top:95px;width:440px;height:440px">' + svg.replace(
                '<svg ', '<svg style="width:440px;height:440px" ', 1) + '</div>'
            '<div style="position:absolute;left:560px;top:190px;color:#fff">'
            '<div style="font-size:150px;font-weight:700;line-height:1;letter-spacing:-2px;'
            'text-shadow:0 4px 0 rgba(11,46,56,.35)">Blue</div>'
            '<div style="margin-top:28px;font-size:40px;line-height:1.25;color:#eafaff">'
            'A blazing-fast, feature-rich<br>Mac OS 8, 9 &amp; Pippin runtime</div></div></div>')
    out = os.path.join(ROOT, 'site', 'og-image.png')
    render(page(card), 1200, 630, out, tmp)
    print('icons:', os.path.relpath(out, ROOT))


def main():
    what = sys.argv[1:] or ['app', 'finder', 'site']
    with tempfile.TemporaryDirectory() as tmp:
        for w in what:
            {'app': make_app, 'finder': make_finder, 'site': make_site}[w](tmp)


if __name__ == '__main__':
    main()
