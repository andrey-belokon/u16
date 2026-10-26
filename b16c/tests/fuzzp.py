"""A differential test of parameter placement: a parameter that is read only before the first call
   stays in the argument register (R0-R3), otherwise it is copied to R4+. The same random program
   is built with -fcopy-params (parameters are always copied) and without it; the output must match.

   python3 tests/fuzzp.py SEED
   Paths: the environment variables B16C, CPU16ASM, CPU16."""
import os, random, subprocess, sys

TYPES = ['uint', 'int', 'ulong', 'long', 'byte']


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)

    # ---- expressions: a uint expression and a ulong expression over the parameters through casts
    def uleaf(self, ps):
        r = self.r
        if ps and r.random() < 0.75:
            return 'uint(%s)' % r.choice(ps)[0]
        return str(r.choice([0, 1, 2, 7, 100, 255, 1000, 40000, 65535]))

    def uexpr(self, ps, d=2):
        r = self.r
        if d == 0 or r.random() < 0.3:
            return self.uleaf(ps)
        op = r.choice(['+', '-', '^', '&', '|'])
        a, b = self.uexpr(ps, d - 1), self.uexpr(ps, d - 1)
        if a.isdigit() and b.isdigit():
            a = 'uint(%s)' % a      # a constant with a constant is computed exactly, without wrap-around
        return '(%s %s %s)' % (a, op, b)

    def wleaf(self, ps):
        r = self.r
        if ps and r.random() < 0.7:
            return 'ulong(%s)' % r.choice(ps)[0]
        return r.choice(['ulong(0)', 'ulong(1)', 'ulong(100000)', 'ulong($DEADBEEF)'])

    def wexpr(self, ps, d=2):
        r = self.r
        if d == 0 or r.random() < 0.3:
            return self.wleaf(ps)
        k = r.random()
        if k < 0.6:
            return '(%s %s %s)' % (self.wexpr(ps, d - 1), r.choice(['+', '-', '^', '&', '|']), self.wexpr(ps, d - 1))
        if k < 0.8:
            return '(%s %s %d)' % (self.wexpr(ps, d - 1), r.choice(['<<', '>>']), r.choice([1, 3, 8, 16, 20]))
        return 'ulong(%s)' % self.uexpr(ps, d - 1)

    def cond(self, ps):
        return '%s %s %s' % (self.uexpr(ps, 1), self.r.choice(['<', '>', '==', '!=']), self.uexpr(ps, 1))

    # ---- statements
    def stmt(self, ps, locs, calls=True):
        r = self.r
        k = r.random()
        if not calls and k >= 0.82:       # without calls: re-pick from the simple forms
            k = r.random() * 0.82
        if k < 0.18:
            return ['gu = %s' % self.uexpr(ps + locs)]
        if k < 0.30:
            return ['gw = %s' % self.wexpr(ps)]
        if k < 0.38:
            return ['gu += %s' % self.uexpr(ps + locs)]
        if k < 0.46:
            return ['gw ^= %s' % self.wexpr(ps)]
        if k < 0.56:
            return ['if %s { gu = %s } else { gw = %s }' % (self.cond(ps + locs), self.uexpr(ps), self.wexpr(ps))]
        if k < 0.64:
            return ['var i%d: uint = 0' % len(locs), 'while i%d < %d { gu += %s; i%d++ }' % (len(locs), r.randint(1, 4), self.uexpr(ps), len(locs))]
        if k < 0.70 and ps:
            p = r.choice(ps)
            return ['%s = %s(uint(%s))' % (p[0], p[1], self.uexpr(ps))]
        if k < 0.76:
            n = len(locs)
            locs.append(('t%d' % n, 'uint'))
            return ['var t%d: uint = %s' % (n, self.uexpr(ps))]
        if k < 0.82:
            return ['if %s { return %s }' % (self.cond(ps), self.uexpr(ps))]
        if k < 0.90:
            return ['gu += h1(%s)' % self.uexpr(ps)]
        if k < 0.96:
            return ['gw += h2(%s, %s)' % (self.wexpr(ps), self.uexpr(ps))]
        return ['gu = h1(%s) ^ %s' % (self.uexpr(ps), self.uexpr(ps))]

    def func(self, idx):
        r = self.r
        n = r.randint(1, 4)
        ps = [('p%d' % i, r.choice(TYPES)) for i in range(n)]
        sig = ', '.join('%s: %s' % p for p in ps)
        locs = []
        body = []
        # a prefix without calls reads the parameters; the part with calls reads them only sometimes
        for _ in range(r.randint(1, 5)):
            body += self.stmt(ps, locs, calls=False)
        late = ps if r.random() < 0.35 else []
        if r.random() < 0.85:           # a call right after the prefix: the function is non-leaf
            if r.random() < 0.5:        # a call whose arguments read the parameters: R0-R3 are overwritten along the way
                body.append('gw += h2(%s, %s)' % (self.wexpr(ps, 1), self.uexpr(ps, 1)))
            else:
                body.append(r.choice(['gu += h1(%s)', 'gu = h1(%s) ^ 3']) % self.uexpr(late + locs, 1))
        for _ in range(r.randint(1, 4)):
            body += self.stmt(late, locs, calls=r.random() < 0.8)
        # after the calls a parameter is read again only in some of the functions
        body.append('return %s' % self.uexpr(late + locs))
        return ps, 'fn f%d(%s): uint {\n    %s\n}' % (idx, sig, '\n    '.join(body))

    def program(self):
        r = self.r
        nf = 6
        funcs = [self.func(i) for i in range(nf)]
        out = ['include "console.b16"', 'var gu: uint', 'var gw: ulong',
               'fn h1(a: uint): uint { return a ^ (a >> 3) }',
               'fn h2(a: ulong, b: uint): ulong { return a + ulong(b) }']
        out += [f[1] for f in funcs]
        out.append('fn main() {\n    var acc: ulong = 1')
        for i, (ps, _) in enumerate(funcs):
            for _ in range(3):
                args = []
                for _, t in ps:
                    v = r.choice([0, 1, 5, 200, 40000, 65535, 123456, 0xDEADBEEF, -1, -300])
                    if t in ('ulong', 'long'): w = v
                    elif t == 'byte': w = v & 255
                    elif t == 'uint': w = v & 0xFFFF
                    else: w = ((v + 32768) & 0xFFFF) - 32768       # int
                    args.append('%s(%d)' % (t, w))
                out.append('    acc = acc * 31 + ulong(f%d(%s))' % (i, ', '.join(args)))
        out.append('    putul(acc); putc(\' \'); putu(gu); putc(\' \'); putul(gw); putnl()\n}')
        return '\n'.join(out) + '\n'


def build_run(src, flag, tag):
    B16C = os.environ.get('B16C', './b16c')
    ASM = os.environ.get('CPU16ASM', 'cpu16asm')
    EMU = os.environ.get('CPU16', 'cpu16')
    base = '/tmp/b16fuzzp_%s' % tag
    open(base + '.b16', 'w').write(src)
    cmd = '%s -w -I lib %s %s.b16 -o %s.s && %s -o %s.bin %s.s >/dev/null 2>&1 && %s %s.bin' % (B16C, flag, base, base, ASM, base, base, EMU, base)
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    return r


def main():
    seed = int(sys.argv[1])
    src = Gen(seed).program()
    a = build_run(src, '', 'new')
    b = build_run(src, '-fcopy-params', 'ref')
    if a.stdout != b.stdout or not a.stdout.strip():
        print('MISMATCH seed', seed)
        print('  without the key :', a.stdout.strip()[:120] or a.stderr[-300:])
        print('  -fcopy-params:', b.stdout.strip()[:120] or b.stderr[-300:])
        sys.exit(1)


if __name__ == '__main__':
    main()
