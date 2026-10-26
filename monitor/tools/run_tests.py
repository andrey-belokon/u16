#!/usr/bin/env python3
"""run_tests.py - kernel scenarios: building programs, FS16 volumes (tools/fs16tool.py), running under the kmon test bench
with several tick schedules. A scenario passes if the output contains "ALL OK" (or the
expected lines), contains no "FAIL", the shell ended with code 0, page 7 and the vectors
are intact, all frames are free, and the volumes are consistent after the stop (fs16tool.py check).

    run_tests.py [--copy-params] [scenario ...]     --copy-params: build the kernel and programs with -fcopy-params;
                                                    names - run only these scenarios
"""
import json, os, subprocess, sys

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
O = os.path.join(D, 'out')

PROGS = ['tsys', 'tspawn', 'tchild', 'tev', 'tcon', 'trestart', 'thello', 'tfs', 'tlost', 'shell', 'txmem', 'tsess']
UTILS = ['ls', 'cat', 'cp', 'mv', 'rm', 'mkdir', 'rmdir', 'attr', 'df', 'mount', 'umount', 'sync', 'date', 'mem', 'fsck', 'touch', 'sysboot']

def vol(shell, extra=(), sectors=2048, label='SYSTEM'):
    """System volume: /sys/shell.cx, /bin/tchild.cx and additional entries."""
    e = [{"path": "/sys", "dir": True}, {"path": "/bin", "dir": True},
         {"path": "/sys/shell.cx", "src": "out/%s.bin" % shell},
         {"path": "/bin/tchild.cx", "src": "out/tchild.bin"}] + list(extra)
    return {"sectors": sectors, "label": label, "entries": e}

def shvol(extra=()):
    """A volume with the shell and utilities."""
    e = [{"path": "/sys", "dir": True}, {"path": "/bin", "dir": True},
         {"path": "/sys/shell.cx", "src": "out/shell.bin"}]
    e += [{"path": "/bin/%s.cx" % u, "src": "out/%s.bin" % u} for u in UTILS]
    e += [{"path": "/data", "dir": True}, {"path": "/data/hello.txt", "size": 30, "seed": 1},
          {"path": "/data/s2", "src": "tests/scripts/s2.txt"}] + list(extra)
    return {"sectors": 2048, "label": "SYSTEM", "entries": e}

def bootvol(spec):
    """Boot volume: boot code and /sys/monitor.sys."""
    spec = dict(spec)
    spec["entries"] = spec["entries"] + [{"path": "/sys/monitor.sys", "src": "out/monitor.sys"}]
    spec["boot"] = "out/fsboot.bin"
    return spec

BADS = [{"path": "/bin/bad.cx", "src": "out/bad.bin"}, {"path": "/bin/badver.cx", "src": "out/badver.bin"},
        {"path": "/bin/huge.cx", "src": "out/huge.bin"}]
FSDATA = [{"path": "/data", "dir": True},
          {"path": "/data/small", "size": 100, "seed": 1},
          {"path": "/data/clu", "size": 2048, "seed": 2},
          {"path": "/data/ind", "size": 25000, "seed": 3},
          {"path": "/data/big", "size": 70000, "seed": 4},
          {"path": "/data/ro", "size": 10, "seed": 6, "flags": ["RDONLY"]},
          {"path": "/data/sub", "dir": True}]
# scenario disks: (a volume specification or None - a clean disk without FS16, protected, check after the run)
FSDISKS = [(vol('tfs', FSDATA), False, True),
           ({"sectors": 200, "label": "SMALL", "entries": []}, False, True),
           ({"sectors": 256, "label": "DIRTY", "dirty": True, "entries": [{"path": "/x", "size": 10, "seed": 1}]}, False, False),
           ({"sectors": 256, "label": "PROTECTED", "entries": [{"path": "/x", "size": 10, "seed": 1}]}, True, False),
           (None, False, False)]
SCEN = [
    ('sys',     [(vol('tsys'), False, True)], None, ['ALL OK']),
    ('spawn',   [(vol('tspawn', BADS), False, True)], None, ['ALL OK']),
    ('ev',      [(vol('tev'), False, True)], 'tests/scripts/ev.scn', ['ALL OK']),
    ('console', [(vol('tcon'), False, True)], 'tests/scripts/console.scn', ['ALL OK']),
    ('restart', [(vol('trestart'), False, True)], None,
                ['first run', 'shell exited, code $0005', 'restarting the shell', 'second run', 'shell exited, code $0000']),
    ('fs',      FSDISKS, None, ['ALL OK']),
    ('shell',   [(shvol(), False, True)], 'tests/scripts/shell.scn',
                ['built-in commands: cd pwd echo exit cls help', 'utilities in A:/bin: ls cat cp mv rm mkdir rmdir attr df mount umount sync date mem',
                 'one\ntwo\n', 'bbbX\n', 'bbbY\n', 'zzq\n', 'A:/> abc^C\nA:/> ', 'cat\ntyped line\ntyped line\n^C\nA:/> ', 'A:/data\n',
                 'hello.txt\ns2\n', 'shell: unclosed quote', 'shell: file name expected after redirection',
                 'shell: empty command in pipeline', 'nothing: command not found', 'shell exited, code $0007', 'restarting the shell'],
                ),
    ('utils',   [(shvol([{"path": "/sys/startup", "src": "tests/scripts/utils_startup.txt"}]), False, True),
                 ({"sectors": 200, "label": "SECOND", "entries": []}, False, True)], None,
                ['start\nabc\n', 'f1\nf3\n', 'f3\nabc\n', 'rm: /w: is a directory', 'rmdir: /w: directory not empty',
                 'rm: /w/none: no such file or directory', 'r--  /w/f1', 'rm: /w/f1: read-only',
                 'cp: /w/f1: same file', 'A:  SYSTEM', 'B:  SECOND', 'usage: df [A-H ...]', 'df: 1: volume not mounted',
                 '2030-05-06 13:14', 'a b c  d\na b c  d\nhello', 'hello.txt\ns2\nf1\n', 'f1\nf3\n', 
                 'nested\nexit code 5\nafter nested\n', 'cat: /none: no such file or directory\n',
                 '----        30  ----------------  /data/hello.txt',
                 '----        30  2031-01-02 03:04  /data/hello.txt\n', '----         0  2031-01-02 03:04  /data/new\n',
                 'usage: touch [-t YYYY-MM-DD HH:MM:SS] file ...', 'cp: /C/A: same file\nexit code 1\nA\nD/\nA:/c\nA\nD/\n',
                 'shell exited, code $0000']),
    # boot from disk: ROM out/rom.bin, the volume boot code, /sys/monitor.sys
    ('boot',    [(bootvol(vol('tsys')), False, True)], None, ['ALL OK'], {'image': 'rom'}),
    ('bootsh',  [(bootvol(shvol([{"path": "/sys/startup", "src": "tests/scripts/utils_startup.txt"}])), False, True),
                 ({"sectors": 200, "label": "SECOND", "entries": []}, False, True)], None,
                ['nested\nexit code 5\nafter nested\n', 'shell exited, code $0000'], {'image': 'rom'}),
    # the boot code and the monitor find /SYS/Monitor.Sys and /SYS/shell.cx ignoring case
    ('bootuc',  [({"sectors": 2048, "label": "UPPER", "boot": "out/fsboot.bin", "entries": [
                    {"path": "/SYS", "dir": True}, {"path": "/BIN", "dir": True},
                    {"path": "/SYS/Shell.CX", "src": "out/tsys.bin"}, {"path": "/BIN/TChild.cx", "src": "out/tchild.bin"},
                    {"path": "/SYS/Monitor.Sys", "src": "out/monitor.sys"}]}, False, True)], None, ['ALL OK'], {'image': 'rom'}),
    ('nodisk',  [], None, ['ROM: no disk 0'], {'image': 'rom', 'halt': True}),
    ('noboot',  [(vol('tsys'), False, False)], None, ['ROM: disk 0 is not bootable'], {'image': 'rom', 'halt': True}),
    ('nomon',   [(dict(vol('tsys'), boot='out/fsboot.bin'), False, False)], None,
                ['boot: no file A:/sys/monitor.sys'], {'image': 'rom', 'halt': True}),
    ('badmon',  [(dict(vol('tsys', [{"path": "/sys/monitor.sys", "src": "out/bad.bin"}]), boot='out/fsboot.bin'), False, False)], None,
                ['boot: bad file A:/sys/monitor.sys'], {'image': 'rom', 'halt': True}),
    # volume check and repair at startup: volume 0 is corrupted (tools/mkdamaged.py), volume 1 has DIRTY
    ('fsck',    [(shvol([{"path": "/data/big", "size": 10000, "seed": 2}, {"path": "/data/ind", "size": 25000, "seed": 3},
                         {"path": "/tmp", "dir": True}, {"path": "/sys/startup", "src": "tests/scripts/fsck_startup.txt"}]), False, True),
                 ({"sectors": 200, "label": "SECOND", "dirty": True, "entries": [{"path": "/x", "size": 10, "seed": 1}]}, False, True)],
                None,
                ['monitor: volume A: was not unmounted cleanly (DIRTY), mounted read-only',
                 'shell: volume A: was not unmounted cleanly, running fsck',
                 'fsck: /data/big: truncated: bad or shared cluster address', 'fsck: /data/ind: addresses beyond the size cleared',
                 'fsck: /tmp/dup: duplicate entry removed', 'fsck: non-zero free directory entry cleared',
                 'fsck: /tmp/ReadMe: name differs only in letter case from another entry, renamed to ReadMe3',
                 'fsck: /tmp/README: name differs only in letter case from another entry, renamed to README4',
                 'readme\nReadMe3\nREADME4\nreadme2\n',
                 'fsck: 3 leaked cluster(s) freed', 'fsck: volume A: repaired, DIRTY cleared', 'shell: volume A: mounted read-write',
                 'fsck: volume B: consistent, DIRTY cleared', 'shell: volume B: mounted read-write',
                 '6144  ----------------  big', 'rw\nB:  SECOND', 'rw\nfsck: volume A: consistent\n',
                 'fsck: volume A: is mounted read-write; unmount it or use -n', 'fsck: volume B: consistent\n',
                 'shell exited, code $0000'], {'damage': 0}),
    ('xmem',    [(vol('txmem'), False, True)], None, ['ALL OK']),
    # installing the boot code from the machine itself and booting from the copied system
    ('sysboot', [(bootvol(shvol([{"path": "/sys/fsboot.bin", "src": "out/fsboot.bin"},
                                 {"path": "/data/s3", "src": "tests/scripts/s3.txt"},
                                 {"path": "/sys/startup", "src": "tests/scripts/sysboot_startup.txt"}])), False, True),
                 ({"sectors": 1024, "label": "COPY", "entries": []}, False, True),
                 ({"sectors": 256, "label": "NOMON", "entries": []}, False, True)], None,
                ['sysboot: boot code installed on drive B:', 'sysboot: /data/hello.txt: not FS16 boot code',
                 'sysboot: boot code installed on drive C:\nsysboot: warning: C:/sys/monitor.sys not found',
                 'sysboot: 7: volume not mounted', 'sysboot: boot code installed on drive A:\n',
                 'shell exited, code $0000'],
                {'image': 'rom', 'reboot': (1, ['booted from the copy', 'monitor.sys\nshell.cx\nstartup\n', 'shell exited, code $0000'])}),
    # the fm file manager: copying to volume 1, directory, deleting, renaming, viewing, command
    ('fm',      [(shvol([{"path": "/bin/fm.cx", "src": "out/fm.bin"}, {"path": "/tmp", "dir": True}]), False, True),
                 ({"sectors": 200, "label": "SECOND", "entries": []}, False, True)], 'tests/scripts/fm.scn',
                ['╔════════════════ B:/ ════════╤════════╗', '║║hello.txt           │      30│',
                 '║newdir              │  Folder│', '║..                  │      Up│        ║',
                 '══════ 66 in 2 files, 914K free ══════', '═══════ 30 in 1 file, 92K free ═══════', '║moved.txt           │      30│',
                 '║Moved.txt           │      30│', 'cannot copy: same file', 'already exists',
                 ' A:/data/Moved.txt ', '00000000: ', 'A:/data> ls\nMoved.txt\ns2\n', '--- fm: press any key ---',
                 '║ A:  SYSTEM       912K    ║', '║ B:  SECOND       92K     ║',
                 '╔════════════════ A:/ ════════╤════════╗╔════════════════ B:/ ════════╤════════╗',
                 'shell exited, code $0000']),
    # the edit editor: \r\n, selection, cut and paste, replace, Del, undo, new file, horizontal
    # scrolling, search, line, F4 and Shift+F4 in fm, "Save as"
    ('edit',    [(shvol([{"path": "/bin/fm.cx", "src": "out/fm.bin"}, {"path": "/bin/edit.cx", "src": "out/edit.bin"},
                         {"path": "/tmp", "dir": True}, {"path": "/data/crlf.txt", "src": "tests/scripts/crlf.txt"},
                         {"path": "/data/t.txt", "src": "tests/scripts/edit_t.txt"}, {"path": "/data/sub", "dir": True}]), False, True)], 'tests/scripts/edit.scn',
                ['Ln 4/4  Col 5  Ins  CRLF  20', 'cat crlf.txt\none1\r\nXtwo\r\nthree\r\nfourA:/data> ',
                 '* Ln 5/5  Col 1  Ins  23', 'cat t.txt\nbeta\nDlta\nalpha\nA:/data> ',
                 'Ln 2/4  Col 2  Ins  16',
                 '* Ln 1/3  Col 101  Ins  113', '║                           Save it?                           ║',
                 'cat new.txt\n' + '0123456789' * 10 + '\nsecond\nthirdA:/data> ',
                 ' A:/data/t.txt                                           Ln 1/4  Col 1  Ins  16',
                 '║                     A:/data/sub/made.txt                     ║',
                 'cat made.txt sub/made.txt\nmade in fmmade in fmA:/data> ',
                 'shell exited, code $0000']),
    # editor: files larger than the process memory (out/edit_big.txt, out/edit_crlf.txt - make_edit_files)
    ('editbig', [({"sectors": 4096, "label": "SYSTEM", "entries": [
                    {"path": "/sys", "dir": True}, {"path": "/bin", "dir": True}, {"path": "/sys/shell.cx", "src": "out/shell.bin"},
                    {"path": "/bin/ls.cx", "src": "out/ls.bin"}, {"path": "/bin/edit.cx", "src": "out/edit.bin"}, {"path": "/data", "dir": True},
                    {"path": "/data/big.txt", "src": "out/edit_big.txt"}, {"path": "/data/crlf.txt", "src": "out/edit_crlf.txt"}]}, False, True)],
                'tests/scripts/edit_big.scn',
                ['* Ln 11001/11001  Col 4  Ins  308009', ' saved                                       Ln 11001/11001  Col 4  Ins  308009',
                 'Ln 3501/3501  Col 1  Ins  CRLF  56000', '    308009  ', '     59500  ', 'shell exited, code $0000'],
                {'steps': 400000000, 'files': [(0, '/data/big.txt', lambda: edit_big_expected()),
                                               (0, '/data/crlf.txt', lambda: open(O + '/edit_crlf.txt', 'rb').read())]}),
    # editor: limits of the edit log - 1000 steps, 256 KB, a deletion that cannot be undone
    ('editundo', [({"sectors": 4096, "label": "SYSTEM", "entries": [
                    {"path": "/sys", "dir": True}, {"path": "/bin", "dir": True}, {"path": "/sys/shell.cx", "src": "out/shell.bin"},
                    {"path": "/bin/ls.cx", "src": "out/ls.bin"}, {"path": "/bin/edit.cx", "src": "out/edit.bin"}, {"path": "/data", "dir": True},
                    {"path": "/data/t.txt", "src": "tests/scripts/edit_t.txt"}, {"path": "/data/big.txt", "src": "out/edit_big.txt"}]}, False, True)],
                'tests/scripts/edit_undo.scn',
                [' nothing to undo                                  * Ln 201/205  Col 1  Ins  223', '       223  ',
                 'Deleting 280000 bytes cannot be undone.', '* Ln 1/1  Col 1  Ins  0',
                 ' nothing to undo                             * Ln 6431/6431  Col 1  Ins  180040', 'shell exited, code $0000'],
                {'steps': 2000000000}),
    # sessions: calls, switching with Ctrl+] and Ctrl+[, the session screen, restarting the wait
    ('sess',    [(vol('tsess', [{"path": "/tmp", "dir": True}]), False, True)], 'tests/scripts/sess.scn', ['ALL OK', 'LAST OK']),
    # shell command session: list, new session, switch, terminate
    ('session', [(shvol([{"path": "/sys/startup", "src": "tests/scripts/session_startup.txt"}]), False, True)], 'tests/scripts/session.scn',
                ['A:/> session\n* 0    56K  A:/sys/shell.cx\n', 'session 1 started\n',
                 '* 0    56K  A:/sys/shell.cx\n  1    56K  A:/sys/shell.cx\n', 'session: 7: no such session',
                 'usage: session [N | new [-b] [command ...] | kill [-f] [N]]',
                 '  0    56K  A:/sys/shell.cx\n  1    56K  A:/sys/shell.cx\n* 2    56K  A:/sys/shell.cx\n',
                 'line for cat\nline for cat\n', '  1    24K  A:/bin/cat.cx\n* 2',
                 '  0    80K  A:/sys/shell.cx > A:/bin/cat.cx\n* 2    56K  A:/sys/shell.cx\n',
                 'session: the session runs several programs; use kill -f', 'A:/data> session\n* 2    56K  A:/sys/shell.cx\n',
                 'session: the only session; use exit', 'shell exited, code $0000',
                 # the session number in the prompt - only when there are several sessions
                 'startup ran\nA:/> session\n* 0', 'session 1 started\n[0] A:/> session\n',
                 '[0] A:/data> session new\n[2] A:/data> session\n',
                 '[2] A:/data> session kill -f 0\nA:/data> session\n* 2'],
                {'once': 'startup ran'}),
    # fm in one session, the shell in another: the fm screen survives the switch there and back
    ('sessfm',  [(shvol([{"path": "/bin/fm.cx", "src": "out/fm.bin"}, {"path": "/tmp", "dir": True}]), False, True)],
                'tests/scripts/sessfm.scn',
                ['=== SCREEN @', '===\n[1] A:/> echo in session 1\n\n\n', '║bin                 │  Folder│--.--.--║║bin  ',
                 '0 in 0 files, 914K free', '9Attr   10Quit\n=== END', 'shell exited, code $0000']),
    ('lost',    [(vol('tlost'), False, True), ({"sectors": 200, "label": "REMOVABLE", "entries": []}, False, False)],
                'tests/scripts/lost.scn', ['ALL OK']),
]
TICKS = [[50], [7, 'f'], [13, 'f'], [101, 'f'], [500, 'fw'], [0, 'w']]
NOTICK_OK = {'sys', 'spawn', 'restart', 'fs'}     # without ticks the SLEEP and Ctrl+C tests will not finish
# the TICKHOOK handler is longer than 7 instructions: with a tick every 7 instructions the program makes no progress
MIN_TICK = {'ev': 13}

def make_edit_files():
    """Files of the editbig scenario: 10000 lines of 28 bytes and 3500 lines with \\r\\n."""
    open(O + '/edit_big.txt', 'wb').write(b''.join(b'line %06d of the big file\n' % i for i in range(10000)))
    open(O + '/edit_crlf.txt', 'wb').write(b''.join(b'crlf line %05d\r\n' % i for i in range(3500)))

def edit_big_expected():
    """big.txt after edit_big.scn: BEG at the start, END at the end, the tail from line 9001 moved to the end."""
    orig = open(O + '/edit_big.txt', 'rb').read()
    return b'BEG' + orig + b'END' + orig[9000 * 28:] + b'END'

def machine_to_utf8(b):
    s = []
    for c in b:
        if c >= 0xC0: s.append(chr(0x410 + c - 0xC0))
        elif c == 0xA3: s.append('ё')
        elif c == 0xB3: s.append('Ё')
        else: s.append(chr(c))
    return ''.join(s)

def decode_out(b):
    """kmon output: the console is in the machine's encoding, the screen snapshots (dump) are already UTF-8."""
    k = b.find(b'=== SCREEN')
    if k < 0: return machine_to_utf8(b)
    return machine_to_utf8(b[:k]) + b[k:].decode('utf-8', 'replace')

def sh(cmd, env):
    return subprocess.run(cmd, shell=True, cwd=D, env=env, capture_output=True)

def main():
    flags = '-fcopy-params' if '--copy-params' in sys.argv else ''
    env = dict(os.environ, B16C=D + '/../b16c/b16c', CPU16ASM=D + '/../cpu16/build/cpu16asm', B16FLAGS=flags)
    r = sh('make -s all boot B16FLAGS=%s' % flags, env)
    if r.returncode: print(r.stdout.decode(), r.stderr.decode()); return 1
    for p in PROGS:
        src = 'user/shell/shell.b16' if p == 'shell' else 'tests/progs/%s.b16' % p
        r = sh('tools/b16build.sh %s' % src, env)
        if r.returncode: print(r.stdout.decode(), r.stderr.decode()); return 1
    r = sh('tools/b16build.sh user/fm/fm.b16', env)
    if r.returncode: print(r.stdout.decode(), r.stderr.decode()); return 1
    r = sh('tools/b16build.sh user/edit/edit.b16', env)
    if r.returncode: print(r.stdout.decode(), r.stderr.decode()); return 1
    for p in UTILS:
        r = sh('tools/b16build.sh user/bin/%s.b16' % p, env)
        if r.returncode: print(r.stdout.decode(), r.stderr.decode()); return 1
    make_edit_files()
    import struct
    b = bytearray(open(O + '/thello.bin', 'rb').read())
    v = bytearray(b); v[8:10] = struct.pack('<H', 2); open(O + '/badver.bin', 'wb').write(v)
    h = bytearray(b); h[16:18] = struct.pack('<H', 0xDFF0); open(O + '/huge.bin', 'wb').write(h)
    open(O + '/bad.bin', 'wb').write(b"this is not a program\n" * 10)
    fails = 0
    maxstack = 0
    only = [a for a in sys.argv[1:] if not a.startswith('--')]
    for sc in SCEN:
        name, disks, script, want = sc[:4]
        if only and name not in only: continue
        opts = sc[4] if len(sc) > 4 else {}
        image = 'out/rom.bin' if opts.get('image') == 'rom' else 'out/monitor.bin'
        for t in TICKS:
            if opts.get('halt') and t != TICKS[0]: continue
            fl = t[1] if len(t) > 1 else ''
            if t[0] == 0 and name not in NOTICK_OK: continue
            if 0 < t[0] < MIN_TICK.get(name, 0): continue
            opt = '--tick-every %d' % t[0] + (' --tick-first' if 'f' in fl else '') + (' --tick-in-wait' if 'w' in fl else '')
            if script: opt += ' --script ' + script
            dopt = ''
            for k, (spec, ro, chk) in enumerate(disks):
                img = '%s/d_%s_%d.img' % (O, name, k)
                if spec is None:
                    open(img, 'wb').write(bytes(256 * 512))
                else:
                    json.dump(spec, open(img + '.json', 'w'))
                    r = sh('python3 tools/fs16tool.py build %s %s.json' % (img, img), env)
                    if r.returncode: print(r.stdout.decode(), r.stderr.decode()); return 1
                    if opts.get('damage') == k:
                        r = sh('python3 tools/mkdamaged.py %s' % img, env)
                        if r.returncode: print(r.stdout.decode(), r.stderr.decode()); return 1
                dopt += (' --disk-ro ' if ro else ' --disk ') + img
            if opts.get('halt'): opt += ' --max-steps 20000000'
            if opts.get('steps'): opt += ' --max-steps %d' % opts['steps']
            r = sh('out/kmon %s --map out/monitor.map %s %s --strict-align' % (image, dopt, opt), env)
            out = decode_out(r.stdout)
            err = r.stderr.decode()
            info = json.loads([l for l in err.splitlines() if l.startswith('KMON ')][0][5:])
            viol = [l for l in err.splitlines() if 'violations: ' in l and not l.endswith(': 0')]
            problems = []
            for w in want:
                if w not in out: problems.append('no "%s"' % w)
            if 'FAIL' in out: problems.append('FAIL in output')
            if 'once' in opts and out.count(opts['once']) != 1: problems.append('"%s" %d times, not once' % (opts['once'], out.count(opts['once'])))
            if opts.get('halt'):
                if info['timeout']: problems.append('timeout')
                print('%-8s %-36s %s' % (name, '(halt)', 'ok' if not problems else 'FAIL: ' + '; '.join(problems)))
                if problems:
                    fails += 1
                    open('%s/fail_%s.out' % (O, name), 'w').write(out)
                continue
            if info['exit'] != 0: problems.append('shell exit code %s' % info['exit'])
            if info['timeout']: problems.append('timeout')
            if not info['canary_ok']: problems.append('page 7 reserve corrupted')
            if not info['vectors_ok']: problems.append('vector table changed')
            if info['nfree'] != 242: problems.append('frames leaked: nfree = %d' % info['nfree'])
            if viol: problems.append('alignment violations')
            for k, (spec, ro, chk) in enumerate(disks):
                if chk:
                    c = sh('python3 tools/fs16tool.py check %s/d_%s_%d.img' % (O, name, k), env)
                    if c.returncode: problems.append('volume %d: %s' % (k, c.stdout.decode().strip().splitlines()[-1]))
            for k, fpath, want_fn in opts.get('files', []):
                g = sh('python3 tools/fs16tool.py get %s/d_%s_%d.img %s' % (O, name, k, fpath), env)
                if g.returncode or g.stdout != want_fn(): problems.append('%s: contents differ' % fpath)
            if 'reboot' in opts and not problems:
                k, want2 = opts['reboot']
                img = '%s/d_%s_%d.img' % (O, name, k)
                r2 = sh('out/kmon out/rom.bin --map out/monitor.map --disk %s %s --strict-align' % (img, opt), env)
                out2 = machine_to_utf8(r2.stdout)
                info2 = json.loads([l for l in r2.stderr.decode().splitlines() if l.startswith('KMON ')][0][5:])
                for w in want2:
                    if w not in out2: problems.append('reboot: no "%s"' % w)
                if info2['exit'] != 0: problems.append('reboot: shell exit code %s' % info2['exit'])
                c = sh('python3 tools/fs16tool.py check %s' % img, env)
                if c.returncode: problems.append('reboot: volume not consistent')
                out += '\n--- reboot from disk %d ---\n' % k + out2
            maxstack = max(maxstack, info['stack_depth'])
            print('%-8s %-36s %s' % (name, opt.replace(' --script ' + (script or ''), ''), 'ok' if not problems else 'FAIL: ' + '; '.join(problems)))
            if problems:
                fails += 1
                open('%s/fail_%s.out' % (O, name), 'w').write(out)
    print('kernel stack depth (max): %d bytes' % maxstack)
    print('FAILED: %d' % fails if fails else 'ALL SCENARIOS OK')
    return 1 if fails else 0

sys.exit(main())
