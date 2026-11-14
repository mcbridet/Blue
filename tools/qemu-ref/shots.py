#!/usr/bin/env python3
"""Boot the 9.1 CD under QEMU (run91.sh) and take screenshots at given times.

  shots.py OUTDIR SECONDS[,SECONDS...] [extra qemu args...]
    QEMU_MACHINE, QEMU_RAM, ... as for run91.sh; QEMU_REF_OUT is set to OUTDIR.

Writes OUTDIR/shot-<seconds>.png (host seconds after launch) and prints the
PowerPC pc and 68k pc (r24) at each, then kills QEMU.
"""
import os
import re
import subprocess
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import qmon  # noqa: E402


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = os.path.abspath(sys.argv[1])
    times = [float(t) for t in sys.argv[2].split(',')]
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ, QEMU_REF_OUT=out)
    log = open(os.path.join(out, 'qemu.out'), 'w')
    q = subprocess.Popen([os.path.join(here, 'run91.sh')] + sys.argv[3:], env=env,
                         stdout=log, stderr=subprocess.STDOUT)
    t0 = time.time()
    try:
        sock = os.path.join(out, 'monitor.sock')
        while not os.path.exists(sock):
            time.sleep(0.1)
        time.sleep(0.5)
        m = qmon.Monitor(sock)
        for t in times:
            time.sleep(max(0, t0 + t - time.time()))
            if q.poll() is not None:
                print('qemu exited (%d)' % q.returncode)
                break
            png = os.path.join(out, 'shot-%g.png' % t)
            m.cmd('screendump ' + png + '.ppm', wait=1.0)
            qmon.ppm_to_png(png + '.ppm', png)
            os.unlink(png + '.ppm')
            r = m.cmd('info registers')
            nip = re.search(r'NIP (\S+)', r)
            g24 = re.search(r'GPR24 (\S+)', r)
            print('%5gs  %s  pc %s  68k pc %s' % (t, png, nip.group(1) if nip else '?',
                                                g24.group(1)[-8:] if g24 else '?'))
    finally:
        q.kill()
        q.wait()


if __name__ == '__main__':
    main()
