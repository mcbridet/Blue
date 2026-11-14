#!/usr/bin/env python3
"""A small GDB remote-protocol client for QEMU's gdbstub (no gdb needed).

Used as a library by the other tools/qemu-ref scripts (mptrace.py, ...):

    from rsp import Target
    t = Target(port=1234)               # qemu-system-ppc ... -gdb tcp::1234 -S
    t.set_break(0xC00)
    t.cont()                            # until a breakpoint (returns the pc)
    t.reg('r3'), t.reg('srr0')          # registers by name (from QEMU's target.xml)
    t.mem(addr, n)                      # virtual memory, as the CPU sees it now
    t.mem(addr, n, phys=True)           # physical memory
    t.monitor('info tlb')               # any QEMU monitor command, its output

Run as a script for a quick look at a stopped or running guest:

    rsp.py [port] regs | mem ADDR LEN [phys] | mon CMD...
"""
import re
import socket
import struct
import sys


class Target:
    def __init__(self, host='127.0.0.1', port=1234):
        self.s = socket.create_connection((host, port))
        self.buf = b''
        self.ack = True
        self.cmd('QStartNoAckMode')
        self.ack = False
        self.phys = None
        self.breaks = set()
        self.regnum = {}
        self.regsize = {}
        self._load_regs()

    # --- packet layer ---
    def _send(self, data):
        if isinstance(data, str):
            data = data.encode()
        pkt = b'$' + data + b'#%02x' % (sum(data) & 0xFF)
        self.s.sendall(pkt)
        if self.ack:
            while True:
                c = self._getc()
                if c == b'+':
                    break

    def _getc(self):
        while not self.buf:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError('gdbstub closed')
            self.buf += chunk
        c, self.buf = self.buf[:1], self.buf[1:]
        return c

    def _recv(self):
        while True:
            c = self._getc()
            if c == b'$':
                break
        data = b''
        while True:
            c = self._getc()
            if c == b'#':
                break
            data += c
        self._getc()
        self._getc()
        if self.ack:
            self.s.sendall(b'+')
        # run-length decoding
        out = b''
        i = 0
        while i < len(data):
            if data[i:i + 1] == b'*' and out:
                n = data[i + 1] - 29
                out += out[-1:] * n
                i += 2
            elif data[i:i + 1] == b'}':
                out += bytes([data[i + 1] ^ 0x20])
                i += 2
            else:
                out += data[i:i + 1]
                i += 1
        return out.decode('latin-1')

    def cmd(self, data):
        self._send(data)
        return self._recv()

    # --- registers ---
    def _load_regs(self):
        xml = self._xfer('target.xml')
        files = [xml] + [self._xfer(f) for f in re.findall(r'<xi:include href="([^"]+)"', xml)]
        n = 0
        for f in files:
            for m in re.finditer(r'<reg ([^>]*)/?>', f):
                attrs = dict(re.findall(r'(\w+)="([^"]*)"', m.group(1)))
                if 'regnum' in attrs:
                    n = int(attrs['regnum'])
                name = attrs['name'].lower()
                if name not in self.regnum:     # (the SPR list repeats lr, ctr, xer)
                    self.regnum[name] = n
                    self.regsize[name] = int(attrs.get('bitsize', '32'))
                n += 1

    def _xfer(self, annex):
        out = ''
        off = 0
        while True:
            r = self.cmd('qXfer:features:read:%s:%x,%x' % (annex, off, 0x3000))
            out += r[1:]
            off += len(r) - 1
            if r[:1] == 'l':
                return out
            if r[:1] != 'm':
                raise IOError('qXfer %s: %r' % (annex, r))

    def reg(self, name):
        r = self.cmd('p%x' % self.regnum[name.lower()])
        return int(r, 16) & ((1 << self.regsize[name.lower()]) - 1)

    def regs(self, names):
        return {n: self.reg(n) for n in names}

    def gprs(self):
        r = self.cmd('g')
        return list(struct.unpack('>32I', bytes.fromhex(r[:256])))

    def set_reg(self, name, value):
        size = self.regsize[name.lower()] // 8
        self.cmd('P%x=%s' % (self.regnum[name.lower()], value.to_bytes(size, 'big').hex()))

    # --- memory ---
    def _mode(self, phys):
        if self.phys != phys:
            self.cmd('Qqemu.PhyMemMode:%d' % int(phys))
            self.phys = phys

    def mem(self, addr, n, phys=False):
        self._mode(phys)
        out = b''
        while n:
            k = min(n, 0x800)
            r = self.cmd('m%x,%x' % (addr, k))
            if r.startswith('E'):
                raise IOError('read %08x: %s' % (addr, r))
            out += bytes.fromhex(r)
            addr += k
            n -= k
        return out

    def w32(self, addr, phys=False):
        return struct.unpack('>I', self.mem(addr, 4, phys))[0]

    # --- execution ---
    def set_break(self, addr):
        if addr in self.breaks:         # (QEMU would keep a second copy)
            return True
        self.breaks.add(addr)
        return self.cmd('Z0,%x,4' % addr) == 'OK'

    def clear_break(self, addr):
        self.breaks.discard(addr)
        return self.cmd('z0,%x,4' % addr) == 'OK'

    def cont(self):
        pc = self.reg('pc')
        if pc in self.breaks:           # step off it first, as gdb does
            self.cmd('z0,%x,4' % pc)
            self.step()
            self.cmd('Z0,%x,4' % pc)
            if self.reg('pc') in self.breaks:
                return self.reg('pc')
        r = self.cmd('c')
        return self._stopped(r)

    def step(self):
        r = self.cmd('s')
        return self._stopped(r)

    def _stopped(self, r):
        if r.startswith('W') or r.startswith('X'):
            raise EOFError('target exited: ' + r)
        return self.reg('pc')

    def interrupt(self):
        self.s.sendall(b'\x03')
        return self._stopped(self._recv())

    def monitor(self, line):
        self._send('qRcmd,' + line.encode().hex())
        out = ''
        while True:
            r = self._recv()
            if r.startswith('O') and r != 'OK':
                out += bytes.fromhex(r[1:]).decode(errors='replace')
            else:
                return out

    def kill(self):
        try:
            self._send('k')
        except OSError:
            pass


def main():
    args = sys.argv[1:]
    port = 1234
    if args and args[0].isdigit():
        port = int(args.pop(0))
    if not args:
        sys.exit(__doc__)
    t = Target(port=port)
    what = args[0]
    if what == 'regs':
        print(t.monitor('info registers'))
    elif what == 'mem':
        addr, n = int(args[1], 0), int(args[2], 0)
        data = t.mem(addr, n, phys=len(args) > 3)
        for i in range(0, len(data), 16):
            print('%08x: %s' % (addr + i, ' '.join(data[i + j:i + j + 4].hex()
                                                   for j in range(0, 16, 4))))
    elif what == 'mon':
        print(t.monitor(' '.join(args[1:])))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
