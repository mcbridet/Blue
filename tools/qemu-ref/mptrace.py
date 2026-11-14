#!/usr/bin/env python3
"""Trace NanoKernel 2's Multiprocessing Services calls in a QEMU reference run.

  mptrace.py OUTDIR [--calls N] [--seconds S] [--snap-at CALLNO[,CALLNO...]]
             [--no-ram] [--kcalls N[,N...]] [--port P] [qemu args...]

Boots the 9.1 CD with run91.sh (QEMU_MACHINE etc. apply) stopped under
QEMU's gdbstub, and drives it with rsp.py (no gdb needed). Breakpoints:

  - IntSyscall's main path (positive `sc` numbers, past its checks for the
    special sc -1/-2: "is this task blue" and the like), found through the
    system-call vector at 0xC00 (SPRG3 + 0x30);
  - the 68k emulator's kernel-call trap for MPDispatch (`twi 31,r31,8` at
    0x6806E8E0: the FE1F trap's path for MPCreateEvent, MPSetSWIEvent, ...);
  - SetEvent's software-interrupt case in the kernel (found by its code);
  - each call's return address (srr0 for sc, LR for the trap), matched by
    the caller's r1, to log the results.

Each line of OUTDIR/mptrace.txt: a call (sequence number, the running task
ID from EWA.PA_CurTask, how it came, call number and name, r3-r10, return
address and LR), a return (r3-r10), or an event-group software interrupt.
After N calls (default 400) the breakpoints go and the guest runs free for
S more seconds (default 0); then OUTDIR/end.png and RAM dumps.

--snap-at 7 saves all of RAM (OUTDIR/ram-at-<seq>-call<n>.bin) when call 7
is made the first time. At the first call (MPYieldWithHint from the
emulator, before any other) it always saves OUTDIR/ram-first.bin: KDP, PSA,
pool objects, page map and the device-tree copy are as the kernel and
Trampoline left them. RAM dumps are QEMU_RAM MB each (256 by default; read
them with nk2dump.py); each comes with a .txt of the registers (QEMU's
gdbstub reads BATs as 0: nk2dump.py shows the kernel's BAT tables instead).
--no-ram keeps only the .txt files. --kcalls 4,5 also logs those kernel
calls (the emulator's `twi 31,r31,n` at 0x6806E8C0 + 4n; 0 and 1 are too
frequent) as one line each, without results. Times (t=) are guest
timebase seconds at mac99's 25 MHz.
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from rsp import Target  # noqa: E402

NAMES = {
    0: 'MPCall_0', 1: 'MPCall_1', 2: 'MPRegisterCpuPlugin', 3: 'MPProcessors', 4: 'MPCreateProcess',
    5: 'MPTerminateProcess', 6: 'MPDeleteProcess', 7: 'MPCreateTask', 8: 'MPStartTask',
    9: 'MPTerminateTask', 10: 'MPCall_10', 11: 'MPIsTaskBlue', 12: 'MPCurrentTaskID',
    13: 'MPYieldWithHint', 14: 'MPSetTaskWeight', 15: 'MPCreateQueue', 16: 'MPDeleteQueue',
    17: 'MPNotifyQueue', 18: 'MPWaitOnQueue', 19: 'MPQueryQueue', 20: 'MPCreateSemaphore',
    21: 'MPDeleteSemaphore', 22: 'MPSignalSemaphore', 23: 'MPWaitOnSemaphore',
    24: 'MPQuerySemaphore', 25: 'MPCreateCriticalRegion', 26: 'MPDeleteCriticalRegion',
    27: 'MPEnterCriticalRegion', 28: 'MPExitCriticalRegion', 29: 'MPQueryCriticalRegion',
    30: 'NKSetTimerNotify', 31: 'MPArmTimer', 32: 'MPCancelTimer', 33: 'MPDelayUntil',
    34: 'MPCall_34', 35: 'MPCall_35', 36: 'MPCall_36', 37: 'MPGetNextID', 38: 'MPCall_38',
    39: 'MPSetQueueReserve', 40: 'NKCreateTimer', 41: 'NKDeleteTimer', 42: 'MPCreateProcessor',
    43: 'MPDeleteProcessor', 44: 'MPStartScheduling', 45: 'MPStopScheduling', 46: 'MPCpuPlugin',
    47: 'MPCall_47', 48: 'MPCall_48', 49: 'MPCreateEvent', 50: 'MPDeleteEvent', 51: 'MPSetEvent',
    52: 'MPWaitForEvent', 53: 'MPQueryEvent', 54: 'MPSetSWIEvent', 55: 'MPDelayUntilSys',
    56: 'MPSetExceptionHandler', 57: 'MPThrowException', 58: 'MPCall_58', 59: 'MPCall_59',
    60: 'MPCall_60', 61: 'MPCall_61', 62: 'MPGetNextCpuID', 63: 'MPCall_63',
    64: 'MPCreateNotification', 65: 'MPDeleteNotification', 66: 'MPModifyNotification',
    67: 'MPCauseNotification', 68: 'MPGetPageSizeClasses', 69: 'MPGetPageSize',
    70: 'MPCreateAddressSpace', 71: 'MPDeleteAddressSpace', 72: 'MPCreateArea',
    73: 'MPCreateAliasArea', 74: 'MPDeleteArea', 75: 'MPSetAreaSize', 76: 'MPSetAreaAccess',
    77: 'MPSetAreaBackingProvider', 78: 'MPCall_78', 79: 'MPGetNextAreaID',
    80: 'MPGetAreaFromAddress', 81: 'MPGetPhysicalAddress', 82: 'MPRegisterAger',
    83: 'MPGetFreePage', 84: 'MPPutFreePage', 85: 'MPMapPage', 86: 'MPUnmapPages',
    87: 'MPLockPages', 88: 'MPUnlockPages', 89: 'MPHoldPages', 90: 'MPUnholdPages',
    91: 'MPGetPageAttributes', 92: 'MPSetPageAttributes', 93: 'MPGetPageAge', 94: 'MPSetPageAge',
    95: 'MPFindVictimPage', 96: 'MPDebugPutString', 97: 'MPDebugPutHex',
    98: 'MPGetPhysicalExtent', 99: 'MPSetBlueProcessID', 100: 'MPGetFreePageCount',
    101: 'MPGetUnheldPageCount', 102: 'MPGetKernelStateSize', 103: 'MPGetKernelState',
    104: 'MPRegisterThermalHandler', 105: 'MPRegisterPMFHandler', 106: 'MPMarkPMFTask',
    107: 'NKLocateInfoRecord', 108: 'NKSetPrInfoClockRates', 114: 'MPSetTaskCPU',
    115: 'MPCall_115', 116: 'MPGetNextIDOwnedByProcess', 117: 'MPCurrentAddressSpace',
    118: 'MPHomeAddressSpace', 119: 'MPSetTaskAddressSpace', 120: 'MPCall_120',
    121: 'MPRegisterExternalHandler', 122: 'KCPropogateExternalInterrupt',
    123: 'MPGetAreaAccess', 124: 'MPDebugPutDec', 125: 'MPGetNextAreaFromAddress',
    126: 'KCSetTaskType', 127: 'MPMakePhysicallyContiguous', 128: 'MPCall_128',
    129: 'MPGetPageHoldLockCounts', 130: 'MPSetSubPageAccess', 131: 'NKSetClockStep',
    132: 'NKSetClockDriftCorrection', 133: 'MPCall_133', 200: 'NKDebug',
}

EMU_KCALL8 = 0x6806E8E0     # emulator code + kernel trap table (+0xE8C0) + 8*4
KCALLS = {0: 'ReturnFromException', 1: 'RunAlternateContext', 2: 'ResetSystem', 3: 'VMDispatch',
          4: 'PrioritizeInterrupts', 5: 'PowerDispatch', 6: 'RTASDispatch', 7: 'CacheDispatch'}
SWI_CODE = bytes.fromhex('821F0018561106F7')    # lwz r16,EventGroup.SWI(r31); rlwinm. r17,r16,0,27,27


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def ostype(v):
    b = struct.pack('>I', v)
    return "'%s'" % b.decode('latin-1') if all(32 <= c < 127 for c in b) else ''


class Tracer:
    def __init__(self, t, out, snap_at, ram_mb, save_ram=True, kcalls=()):
        self.kcall_bps = {EMU_KCALL8 + 4 * (n - 8) for n in kcalls}
        self.t = t
        self.out = out
        self.save_ram = save_ram
        self.log = open(os.path.join(out, 'mptrace.txt'), 'w')
        self.snap_at = set(snap_at)
        self.ram = ram_mb << 20
        self.pending = {}       # (return pc, r1) -> seq
        self.seq = 0
        self.calls = 0
        self.kdp = None

    def emit(self, line):
        print(line)
        self.log.write(line + '\n')
        self.log.flush()

    def snap(self, name):
        path = os.path.join(self.out, name)
        if self.save_ram:
            self.t.monitor('pmemsave 0 0x%x "%s"' % (self.ram, path))
        regs = self.t.monitor('info registers')
        with open(path.replace('.bin', '.txt'), 'w') as f:
            f.write(regs)
        self.emit('# saved RAM to %s, registers to %s' % (name, name.replace('.bin', '.txt')))

    def cur_task(self):
        task = self.t.w32(self.kdp - 8, phys=True)
        if not task:
            return 0, 0
        return self.t.w32(task, phys=True), task

    def seconds(self):
        """Guest time: the timebase, at mac99's 25 MHz (OpenBIOS's timebase-frequency)."""
        return (self.t.reg('tbu') << 32 | self.t.reg('tbl')) / 25e6

    def setup(self):
        t = self.t
        t.set_break(0xC00)
        while True:
            t.cont()
            if t.reg('r0') < 0x80000000:
                break
        t.clear_break(0xC00)
        self.kdp = t.reg('sprg0')
        vec = t.reg('sprg3')
        self.intsyscall = t.w32(vec + 0x30, phys=True)
        code = t.mem(self.intsyscall, 0x80, phys=True)
        self.sc_bp = None
        for i in range(0, len(code), 4):
            w = struct.unpack('>I', code[i:i + 4])[0]
            if w & 0xFC000003 == 0x48000001:
                self.sc_bp = self.intsyscall + i
                break
        kern = self.intsyscall & ~0xFFFF
        blob = t.mem(kern - 0x20000, 0x60000, phys=True)
        i = blob.find(SWI_CODE)
        self.swi_bp = kern - 0x20000 + i + 12 if i >= 0 else None
        self.emit('# KDP (SPRG0) 0x%08x, vector table (SPRG3) 0x%08x, IntSyscall 0x%08x, '
                  'sc breakpoint 0x%08x, SWI breakpoint %s' % (
                      self.kdp, vec, self.intsyscall, self.sc_bp,
                      '0x%08x' % self.swi_bp if self.swi_bp else 'not found'))
        self.snap('ram-first.bin')
        t.set_break(self.sc_bp)
        t.set_break(EMU_KCALL8)
        for b in self.kcall_bps:
            t.set_break(b)
        if self.swi_bp:
            t.set_break(self.swi_bp)
        # we are at 0xC00 with the first sc: run to the sc breakpoint
        assert t.cont() == self.sc_bp

    def on_call(self, via):
        t = self.t
        g = t.gprs()
        if via == 'sc':
            ret, r1, lr = t.reg('srr0'), t.reg('sprg1'), t.reg('sprg2')
        else:
            ret, r1, lr = t.reg('lr'), g[1], t.reg('lr')
        tid, _ = self.cur_task()
        n = g[0]
        self.seq += 1
        self.calls += 1
        key = (ret, r1)
        again = ' (again)' if key in self.pending else ''
        self.pending[key] = (self.seq, n)
        t.set_break(ret)
        extra = ''
        if n in (8, 126):
            extra = ' ' + ostype(g[4])
        if via == 'kc8':
            extra += ' 68k pc %08x' % g[24]
        extra += ' t=%.3fs' % self.seconds()
        self.emit('%5d task %08x %-3s call %3d %-24s r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x '
                  'r8=%08x r9=%08x r10=%08x  ret %08x lr %08x%s%s' % (
                      self.seq, tid, via, n, NAMES.get(n, '?'), *g[3:11], ret, lr, extra, again))
        if n in self.snap_at:
            self.snap_at.discard(n)
            self.snap('ram-at-%d-call%d.bin' % (self.seq, n))

    def on_return(self, pc):
        t = self.t
        g = t.gprs()
        key = (pc, g[1])
        if key not in self.pending:
            return
        seq, n = self.pending.pop(key)
        if not any(k[0] == pc for k in self.pending):
            t.clear_break(pc)
        tid, _ = self.cur_task()
        st = g[3] - (1 << 32) if g[3] & 0x80000000 else g[3]
        self.emit('%5d task %08x  -> call %3d returns r3=%08x (%d) r4=%08x r5=%08x r6=%08x '
                  'r7=%08x r8=%08x r9=%08x r10=%08x' % (seq, tid, n, g[3], st, *g[4:11]))

    def on_swi(self):
        t = self.t
        r31 = t.reg('r31')
        gid = t.w32(r31, phys=True)
        swi = t.w32(r31 + 0x18, phys=True)
        tid, _ = self.cur_task()
        for _ in range(4):
            t.step()
        ecb, offs, cur = t.reg('r17'), t.reg('r18'), t.reg('r19')
        self.emit('      task %08x  SWI: event group %08x set, SWI word %08x (level %d); ECB 0x%08x '
                  'EDPOffsetSWIRelated %08x SWIEventGroupID %08x t=%.3fs' % (
                      tid, gid, swi, swi & 15, ecb, offs, cur, self.seconds()))

    def run(self, limit):
        t = self.t
        last = 0
        self.on_call('sc')
        while self.calls < limit:
            pc = t.cont()
            if pc == self.sc_bp:
                self.on_call('sc')
            elif pc == EMU_KCALL8:
                self.on_call('kc8')
            elif pc == self.swi_bp:
                self.on_swi()
            elif pc in self.kcall_bps:
                g = t.gprs()
                tid, _ = self.cur_task()
                self.emit('      task %08x  kernel call %d (%s) r3=%08x r4=%08x r5=%08x lr %08x 68k pc %08x '
                          't=%.3fs' % (tid, (pc - EMU_KCALL8) // 4 + 8, KCALLS.get((pc - EMU_KCALL8) // 4 + 8, '?'),
                                       g[3], g[4], g[5], t.reg('lr'), g[24], self.seconds()))
            elif any(k[0] == pc for k in self.pending):
                self.on_return(pc)
            else:
                self.emit('# stopped at unexpected pc %08x' % pc)
                t.clear_break(pc)
            if self.seq % 500 == 0 and self.seq != last:
                last = self.seq
                print('# %d calls' % self.seq, file=sys.stderr)

    def release(self):
        for b in list(self.t.breaks):
            self.t.clear_break(b)


def main():
    ap = argparse.ArgumentParser(usage=__doc__)
    ap.add_argument('out')
    ap.add_argument('--calls', type=int, default=400)
    ap.add_argument('--seconds', type=float, default=0)
    ap.add_argument('--snap-at', default='')
    ap.add_argument('--port', type=int, default=0)
    ap.add_argument('--no-ram', action='store_true')
    ap.add_argument('--kcalls', default='')
    args, qemu_args = ap.parse_known_args()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    port = args.port or free_port()
    env = dict(os.environ, QEMU_REF_OUT=out)
    q = subprocess.Popen([os.path.join(here, 'run91.sh')] + qemu_args +
                         ['-gdb', 'tcp:127.0.0.1:%d' % port, '-S'], env=env,
                         stdout=open(os.path.join(out, 'qemu.out'), 'w'), stderr=subprocess.STDOUT)
    try:
        t = None
        for _ in range(100):
            try:
                t = Target(port=port)
                break
            except OSError:
                time.sleep(0.1)
        if t is None:
            sys.exit('no gdbstub on port %d (see %s/qemu.out)' % (port, out))
        tr = Tracer(t, out, [int(x) for x in args.snap_at.split(',') if x],
                    int(os.environ.get('QEMU_RAM', '256')), not args.no_ram,
                    [int(x) for x in args.kcalls.split(',') if x])
        tr.setup()
        t0 = time.time()
        tr.run(args.calls)
        tr.emit('# %d calls traced in %.0f s' % (tr.calls, time.time() - t0))
        tr.release()
        if args.seconds:
            t._send('c')
            time.sleep(args.seconds)
            t.interrupt()
        t.monitor('screendump "%s"' % os.path.join(out, 'end.ppm'))
        import qmon
        qmon.ppm_to_png(os.path.join(out, 'end.ppm'), os.path.join(out, 'end.png'))
        os.unlink(os.path.join(out, 'end.ppm'))
        tr.snap('ram-end.bin')
        t.kill()
    finally:
        q.kill()
        q.wait()


if __name__ == '__main__':
    main()
