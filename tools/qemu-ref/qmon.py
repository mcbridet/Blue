#!/usr/bin/env python3
"""Talk to the QEMU monitor of a running tools/qemu-ref/run.sh session.

  qmon.py screenshot <out.png>   save the guest display as PNG
  qmon.py regs                   print CPU registers
  qmon.py sample [n]             sample PowerPC pc and 68k pc (r24) n times
  qmon.py cmd <monitor command>  run any monitor command
"""
import os
import re
import socket
import struct
import sys
import time
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SOCK = os.path.join(ROOT, 'build', 'qemu-ref', 'monitor.sock')


class Monitor:
    def __init__(self, path=SOCK):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.read()

    def read(self, wait=0.3):
        time.sleep(wait)
        self.s.setblocking(False)
        data = b''
        try:
            while True:
                chunk = self.s.recv(65536)
                if not chunk:
                    break
                data += chunk
        except BlockingIOError:
            pass
        self.s.setblocking(True)
        return data.decode(errors='replace')

    def cmd(self, line, wait=0.5):
        self.s.sendall(line.encode() + b'\n')
        out = self.read(wait)
        # drop the echoed command line and prompt noise
        out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', out)
        return '\n'.join(l for l in out.splitlines() if not l.startswith('(qemu)'))


def ppm_to_png(ppm_path, png_path):
    data = open(ppm_path, 'rb').read()
    magic, dims, _maxval, pixels = data.split(b'\n', 3)
    w, h = map(int, dims.split())
    raw = b''.join(b'\0' + pixels[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        return (struct.pack('>I', len(body)) + tag + body
                + struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF))

    with open(png_path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n'
                + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw))
                + chunk(b'IEND', b''))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    m = Monitor()
    what = sys.argv[1]
    if what == 'screenshot':
        out = os.path.abspath(sys.argv[2])
        ppm = out + '.ppm'
        m.cmd('screendump ' + ppm, wait=1.0)
        ppm_to_png(ppm, out)
        os.unlink(ppm)
        print(out)
    elif what == 'regs':
        print(m.cmd('info registers'))
    elif what == 'sample':
        for _ in range(int(sys.argv[2]) if len(sys.argv) > 2 else 10):
            r = m.cmd('info registers')
            nip = re.search(r'NIP (\S+)', r)
            g24 = re.search(r'GPR24 (\S+)', r)
            print('pc %s  68k pc %s' % (nip.group(1) if nip else '?',
                                         g24.group(1)[-8:] if g24 else '?'))
    elif what == 'cmd':
        print(m.cmd(' '.join(sys.argv[2:]), wait=1.0))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
