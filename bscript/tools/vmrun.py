#!/usr/bin/env python3
"""vmrun.py - running monitor shell commands in the kmon test bench.

    vmrun.py [-f host:/path ...] [--steps N] [--keep img] command ...

Volume: the shell, the utilities, the BScript out/*.bin in /bin, the directory /tmp, the -f files. The commands are run
by the startup script /sys/startup, ending with exit. The console output goes to stdout (UTF-8)."""
import json, os, subprocess, sys, tempfile

B = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
R = os.path.dirname(B)
M = os.path.join(R, 'monitor')
UTILS = ['ls', 'cat', 'cp', 'mv', 'rm', 'mkdir', 'rmdir', 'attr', 'df', 'mount', 'umount', 'sync', 'date', 'mem', 'touch']

def to_machine(s):
    out = bytearray()
    for ch in s:
        o = ord(ch)
        if o < 128: out.append(o)
        elif 0x410 <= o <= 0x44F: out.append(0xC0 + o - 0x410)
        elif ch == 'ё': out.append(0xA3)
        elif ch == 'Ё': out.append(0xB3)
        else: raise ValueError('char %r' % ch)
    return bytes(out)

def from_machine(b):
    s = []
    for c in b:
        if c >= 0xC0: s.append(chr(0x410 + c - 0xC0))
        elif c == 0xA3: s.append('ё')
        elif c == 0xB3: s.append('Ё')
        elif c < 128: s.append(chr(c))
        else: s.append('\\x%02X' % c)
    return ''.join(s)

def run(cmds, files=(), steps=400000000, extra_entries=(), shell=None, keep=None, tick=50, script=None, profile=None):
    tmp = tempfile.mkdtemp(prefix='bsrun')
    e = [{"path": "/sys", "dir": True}, {"path": "/bin", "dir": True}, {"path": "/tmp", "dir": True},
         {"path": "/sys/shell.cx", "src": shell or os.path.join(M, 'out/shell.bin')}]
    e += [{"path": "/bin/%s.cx" % u, "src": os.path.join(M, 'out/%s.bin' % u)} for u in UTILS]
    od = os.environ.get('BSOUT', os.path.join(B, 'out'))       # BScript program directory
    for p in ('bsc', 'bsvm', 'bs'):
        f = os.path.join(od, p + '.bin')
        if not os.path.exists(f): f = os.path.join(B, 'out', p + '.bin')
        if os.path.exists(f): e.append({"path": "/bin/%s.cx" % p, "src": f})
    f = os.path.join(od, 'bsvm.ovl')
    if os.path.exists(f): e.append({"path": "/bin/bsvm.ovl", "src": f})
    dirs = set()
    n = 0
    for host, path in files:
        parts = path.strip('/').split('/')[:-1]
        for i in range(len(parts)):
            d = '/' + '/'.join(parts[:i + 1])
            if d not in dirs and d not in ('/bin', '/sys', '/tmp'):
                dirs.add(d); e.append({"path": d, "dir": True})
        if host.endswith('.bs') or host.endswith('.txt'):
            n += 1
            conv = os.path.join(tmp, 'f%d' % n)
            open(conv, 'wb').write(to_machine(open(host, encoding='utf-8', newline='').read()))
            host = conv
        e.append({"path": path, "src": host})
    e += list(extra_entries)
    st = os.path.join(tmp, 'startup')
    open(st, 'wb').write(to_machine('\n'.join(list(cmds) + ['exit']) + '\n'))
    e.append({"path": "/sys/startup", "src": st})
    img = keep or os.path.join(tmp, 'd.img')
    json.dump({"sectors": 4096, "label": "SYSTEM", "entries": e}, open(img + '.json', 'w'))
    r = subprocess.run(['python3', os.path.join(M, 'tools/fs16tool.py'), 'build', img, img + '.json'], capture_output=True)
    if r.returncode: sys.stderr.write(r.stdout.decode() + r.stderr.decode()); sys.exit(1)
    r = subprocess.run([os.path.join(M, 'out/kmon'), os.path.join(M, 'out/monitor.bin'), '--map', os.path.join(M, 'out/monitor.map'),
                        '--disk', img, '--tick-every', str(tick), '--max-steps', str(steps)] + (['--script', script] if script else []) + (['--profile', profile] if profile else []), capture_output=True)
    info = {}
    for l in r.stderr.decode().splitlines():
        if l.startswith('KMON '): info = json.loads(l[5:])
    return from_machine(r.stdout), info

if __name__ == '__main__':
    a = sys.argv[1:]
    files = []; steps = 400000000; keep = None; script = None
    while a and a[0].startswith('-'):
        if a[0] == '-f': h, p = a[1].split(':', 1); files.append((h, p)); a = a[2:]
        elif a[0] == '--steps': steps = int(a[1]); a = a[2:]
        elif a[0] == '--keep': keep = a[1]; a = a[2:]
        elif a[0] == '--script': script = a[1]; a = a[2:]
        else: break
    out, info = run(a, files, steps, keep=keep, script=script)
    sys.stdout.write(out)
    sys.stderr.write('\n[exit %s steps %s timeout %s]\n' % (info.get('exit'), info.get('steps'), info.get('timeout')))
