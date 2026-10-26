#!/usr/bin/env python3
"""profile.py - program profile in the kmon test bench: the number of CPU16 instructions per function.

    profile.py MAP [-f host:/path ...] command ...

MAP is the program's memory map (out/bsc.map, out/bsvm.map). The kmon --profile counters are taken by
logical addresses, so the profile also shows the shell and other programs if their addresses
fall into functions of the map; run only the one program you are interested in."""
import os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmrun

def objects(mp):
    out = []
    f = False
    for l in open(mp):
        if '== Objects by address' in l: f = True; continue
        if f and l.startswith('$'):
            p = l.split()
            if len(p) >= 5 and p[1].startswith('$') and p[2].isdigit() and 'code' in p[3] and not p[4].startswith('@'):
                out.append((int(p[0][1:], 16), int(p[1][1:], 16), p[4]))
    return sorted(out)

def main():
    a = sys.argv[1:]
    mp = a.pop(0)
    files = []
    while a and a[0] == '-f':
        h, p = a[1].split(':', 1); files.append((h, p)); a = a[2:]
    prof = tempfile.mktemp(suffix='.prof')
    out, info = vmrun.run(a, files, steps=4000000000, profile=prof)
    cnt = {}
    for l in open(prof):
        x, n = l.split(); cnt[int(x, 16)] = int(n)
    objs = objects(mp)
    per = {}
    for lo, hi, name in objs:
        per[name] = sum(cnt.get(x, 0) for x in range(lo, hi + 1))
    total = sum(per.values())
    print('total in map functions: %d (all steps %d)' % (total, info.get('steps')))
    for name, n in sorted(per.items(), key=lambda t: -t[1])[:30]:
        if n: print('%9d %5.1f%%  %s' % (n, 100.0 * n / total, name))

main()
