#!/usr/bin/env python3
"""run_tests.py - BScript tests in the emulator: tests/*.bs with the references tests/*.out.

    run_tests.py [--update] [--examples] [--skip name,...] [--steps N] [name ...]

--examples: the examples examples/*.bs (on the volume - /ex), references examples/expected/*.out.

Each test: the file tests/NAME.bs, started by the command from the first line of the form
`// run: command` (by default `bs /t/NAME.bs`). The reference is the console output between the markers,
including the shell message "exit code N". The files tests/data/* are put into /t/data."""
import os, sys, glob, difflib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmrun

B = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def main():
    upd = '--update' in sys.argv
    ex = '--examples' in sys.argv
    T = os.path.join(B, 'examples' if ex else 'tests')
    V = '/ex' if ex else '/t'
    E = os.path.join(T, 'expected') if ex else T
    args = sys.argv[1:]
    skip = set(); steps = 4000000000; only = []
    i = 0
    while i < len(args):
        if args[i] == '--skip': skip = set(args[i + 1].split(',')); i += 2; continue
        if args[i] == '--steps': steps = int(args[i + 1]); i += 2; continue
        if not args[i].startswith('--'): only.append(args[i])
        i += 1
    names = sorted(os.path.basename(p)[:-3] for p in glob.glob(T + '/*.bs'))
    if only: names = [n for n in names if n in only]
    names = [n for n in names if n not in skip]
    files = [(p, V + '/' + os.path.basename(p)) for p in glob.glob(T + '/*.bs')]
    for sub in ('inc', 'lib', 'data'):
        for p in glob.glob(T + '/%s/*' % sub):
            files.append((p, '%s/%s/%s' % (V, sub, os.path.basename(p))))
    cmds = []
    for n in names:
        first = open(os.path.join(T, n + '.bs'), encoding='utf-8').readline()
        cmd = 'bs %s/%s.bs' % (V, n)
        if first.startswith('// run:'): cmd = first[7:].strip()
        cmds += ['echo ==== %s' % n, 'cd ' + V, cmd, 'cd /']
    cmds.append('echo ==== end')
    out, info = vmrun.run(cmds, files, steps=steps)
    if info.get('timeout'): print('TIMEOUT'); 
    parts = out.split('==== ')
    got = {}
    for p in parts[1:]:
        nm, _, body = p.partition('\n')
        # there is no need to remove the command echo: the startup script does not print commands
        got[nm.strip()] = body
    fails = 0
    for n in names:
        g = got.get(n, '<no output>\n')
        ef = os.path.join(E, n + '.out')
        os.makedirs(E, exist_ok=True)
        if upd or not os.path.exists(ef):
            open(ef, 'w', encoding='utf-8', newline='').write(g)
            print('%-20s written' % n); continue
        e = open(ef, encoding='utf-8', newline='').read()
        if e == g: print('%-20s ok' % n)
        else:
            fails += 1
            print('%-20s FAIL' % n)
            for l in difflib.unified_diff(e.splitlines(), g.splitlines(), 'expected', 'got', lineterm=''): print('   ' + l)
    print('steps %s' % info.get('steps'))
    print('FAILED %d' % fails if fails else 'ALL OK')
    return 1 if fails else 0

sys.exit(main())
