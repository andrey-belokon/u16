"""A differential test of B16 32-bit arithmetic: random programs with long/ulong against
   a Python reference written from the specification (sect. 4.1, 4.6, 4.7, 6.2, 6.4, 6.5, 9.3).

   python3 tests/fuzz32.py SEED [--level N] [--deep] [--nodiv] [--emit-only]
   python3 tests/fuzz32.py --selftest        check the reference against an independent C implementation

   --level N  which features are enabled (for stage-by-stage readiness of the compiler):
       1  types, casts, assignments, arrays, structs, pointers, global initializers
       2  + calls: direct, through a pointer, several results, stack arguments
       3  + - ~ ! & | ^, comparisons, && ||, shifts by a constant, +=, -=, &=, |=, ^=, ++, --, if
       4  the same as 3 (use with --deep: expressions of depth 6, register shortage)
       5  + * / % and shifts by a variable count (the default)
   --nodiv    without / and % (at level 5)
   --regs     narrow variables are global, the registers go to long/ulong
   --emit-only  only write the program and the expected output to /tmp/b32fuzz.*
   Paths: the environment variables B16C, CPU16ASM, CPU16."""
import copy, os, random, subprocess, sys

# ------------------------------------------------------------------ types and values
W = {'int': 16, 'uint': 16, 'long': 32, 'ulong': 32}
SIGNED = {'int': True, 'uint': False, 'long': True, 'ulong': False}
WIDE = lambda t: W[t] == 32


class Undefined(Exception):
    """Undefined behavior or an invalid combination: the expression is discarded."""


def norm(v, t):
    w = W[t]
    v &= (1 << w) - 1
    if SIGNED[t] and v >> (w - 1):
        v -= 1 << w
    return v


def sext8(v):
    v &= 255
    return v - 256 if v & 128 else v


def to_decl(v, decl):
    """Storing a value into an object of the declared type (6.2): the low bits, interpreted by the type."""
    if decl == 'byte':
        return v & 255
    if decl == 'char':
        return sext8(v)
    return norm(v, decl)


def const_range(t):
    """The range of an untyped constant that takes type t (4.7): -2^(n-1) ... 2^n - 1."""
    w = W[t]
    return -(1 << (w - 1)), (1 << w) - 1


def trunc_div(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def result_type(tl, tr):
    """The result type of + - * & | ^ / % (6.4); an untyped constant has already taken the type of the second operand."""
    if WIDE(tl) and WIDE(tr):
        return tl
    if WIDE(tl):
        return tl
    if WIDE(tr):
        return tr
    return tl


def sign_ok(tl, tr):
    """The signedness rule for / % < > <= >= (6.4)."""
    if SIGNED[tl] == SIGNED[tr]:
        return True
    return (tl == 'uint' and tr == 'long') or (tl == 'long' and tr == 'uint')


# ------------------------------------------------------------------ reference computation
def binval(op, lv, tl, rv, tr):
    """The value of a binary operation on values lv (type tl) and rv (type tr) -> (value, type)."""
    if op in ('<<', '>>'):
        w = W[tl]
        c = rv & (w - 1)
        if op == '<<':
            return norm(lv << c, tl), tl
        return norm(lv >> c, tl), tl          # Python >> on negatives is arithmetic
    R = result_type(tl, tr)
    w = W[R]
    mask = (1 << w) - 1
    if op in ('+', '-', '*', '&', '|', '^'):
        a, b = lv & mask, rv & mask
        r = {'+': a + b, '-': a - b, '*': a * b, '&': a & b, '|': a | b, '^': a ^ b}[op]
        return norm(r, R), R
    if op in ('/', '%'):
        if not sign_ok(tl, tr):
            raise Undefined('sign')
        if rv == 0:
            if not SIGNED[tl] and not SIGNED[tr] and R == 'ulong':
                return (norm(0xFFFFFFFF, R) if op == '/' else norm(lv, R)), R
            raise Undefined('div0')
        if SIGNED[tl] or SIGNED[tr]:
            if R == 'int' and lv == -32768 and rv == -1:
                raise Undefined('int MIN/-1')
        q = trunc_div(lv, rv)
        r = q if op == '/' else lv - q * rv
        return norm(r, R), R
    if op in ('==', '!='):
        w = 32 if (WIDE(tl) or WIDE(tr)) else 16
        mask = (1 << w) - 1
        e = (lv & mask) == (rv & mask)
        return int(e if op == '==' else not e), 'int'
    if op in ('<', '>', '<=', '>='):
        if not sign_ok(tl, tr):
            raise Undefined('sign')
        r = {'<': lv < rv, '>': lv > rv, '<=': lv <= rv, '>=': lv >= rv}[op]
        return int(r), 'int'
    raise ValueError(op)


# Nodes: ('v', LV) ('k', value, type) ('cast', C, e) ('un', op, e) ('not', e)
#       ('bin', op, l, r) ('shift', op, l, r) ('cmp', op, l, r) ('logic', op, l, r)
#       ('call', fname, args, SIG, i, ptrname)
def promote(c):
    return {'byte': 'uint', 'char': 'int'}.get(c, c)


def ty(n):
    k = n[0]
    if k == 'v': return n[1].ptype
    if k == 'k': return n[2]
    if k == 'cast': return promote(n[1])
    if k == 'un': return ty(n[2])
    if k in ('not', 'cmp', 'logic'): return 'int'
    if k == 'bin': return result_type(ty(n[2]), ty(n[3]))
    if k == 'shift': return ty(n[2])
    if k == 'call': return n[3][2][0]
    raise ValueError(k)


def ev(n, env):
    k = n[0]
    if k == 'v':
        return n[1].get(env)
    if k == 'k':
        return norm(n[1], n[2])
    if k == 'cast':
        v = ev(n[2], env)
        c = n[1]
        return to_decl(v, c)
    if k == 'un':
        v = ev(n[2], env)
        t = ty(n[2])
        return norm(-v if n[1] == '-' else ~v, t)
    if k == 'not':
        return int(ev(n[1], env) == 0)
    if k == 'shift' and n[3][0] == 'k':
        # a constant count: n >= the width gives 0 (for signed >> the sign), otherwise an ordinary shift (6.5)
        l, tl = ev(n[2], env), ty(n[2])
        c = n[3][1]
        if c >= W[tl]:
            return -1 if (n[1] == '>>' and SIGNED[tl] and l < 0) else 0
        return binval(n[1], l, tl, c, 'uint')[0]
    if k in ('bin', 'cmp', 'shift'):
        l, r = ev(n[2], env), ev(n[3], env)
        return binval(n[1], l, ty(n[2]), r, ty(n[3]))[0]
    if k == 'logic':
        l = ev(n[2], env) != 0
        if n[1] == '&&':
            return int(l and ev(n[3], env) != 0)
        return int(l or ev(n[3], env) != 0)
    if k == 'call':
        name, args, sig, i, _ = n[1], n[2], n[3], n[4], n[5]
        params, results = sig[1], sig[2]
        vals = [norm(ev(a, env), t) for a, t in zip(args, params)]
        return norm(vals[i], results[0])
    raise ValueError(k)


# ------------------------------------------------------------------ output to B16 source text
def lit(v, rnd):
    s = ('$%X' % abs(v)) if rnd.random() < 0.3 else str(abs(v))
    return '-' + s if v < 0 else s


def render(n, rnd):
    k = n[0]
    if k == 'v': return n[1].src
    if k == 'k': return lit(n[1], rnd)
    if k == 'cast': return '%s(%s)' % (n[1], render(n[2], rnd))
    if k == 'un': return '(%s%s)' % (n[1], render(n[2], rnd))
    if k == 'not': return '(!%s)' % render(n[1], rnd)
    if k in ('bin', 'cmp', 'shift', 'logic'):
        return '(%s %s %s)' % (render(n[2], rnd), n[1], render(n[3], rnd))
    if k == 'call':
        fn = n[5] or ('%s_%d' % (n[3][0], n[4]))
        return '%s(%s)' % (fn, ', '.join(render(a, rnd) for a in n[2]))
    raise ValueError(k)


# ------------------------------------------------------------------ lvalues
class LV:
    def __init__(self, src, decl, get, put):
        self.src, self.decl, self.get_, self.put_ = src, decl, get, put
        self.ptype = promote(decl)

    def get(self, env): return self.get_(env)
    def put(self, env, v): self.put_(env, to_decl(v, self.decl))


def simple(name, decl):
    return LV(name, decl, lambda e: e[name], lambda e, v: e.__setitem__(name, v))


def arr_fixed(arr, i, decl, src=None):
    return LV(src or '%s[%d]' % (arr, i), decl, lambda e: e[arr][i], lambda e, v: e[arr].__setitem__(i, v))


def arr_dyn(arr, idx, decl):
    return LV('%s[%s & 3]' % (arr, idx), decl,
              lambda e: e[arr][e[idx] & 3], lambda e, v: e[arr].__setitem__(e[idx] & 3, v))


def field(f, decl):
    return LV('st.' + f, decl, lambda e: e['st'][f], lambda e, v: e['st'].__setitem__(f, v))


LOCALS = {'a': 'int', 'b': 'int', 'u': 'uint', 'v': 'uint', 'bb': 'byte', 'cc': 'char',
          'x': 'long', 'y': 'long', 'z': 'long', 'p': 'ulong', 'q': 'ulong', 'r': 'ulong'}
LVALS = [simple(n, d) for n, d in LOCALS.items()] + [
    simple('gl', 'long'), simple('gu', 'ulong'), simple('gi', 'int'),
    arr_fixed('arr', 2, 'long'), arr_fixed('arr', 1, 'long', '*pl'), arr_dyn('arr', 'u', 'long'),
    arr_fixed('parr', 3, 'ulong'), arr_dyn('parr', 'v', 'ulong'),
    field('w', 'long'), field('d', 'uint'), field('c', 'byte')]
LV_BY_SRC = {l.src: l for l in LVALS}
WIDE_TARGETS = [LV_BY_SRC[n] for n in ('x', 'y', 'z', 'p', 'q', 'r')]
NARROW_TARGETS = [LV_BY_SRC[n] for n in ('a', 'b', 'u', 'v')]

# signatures of the called functions: (name, parameters, results); f_i returns R(parameter i)
SIGS = [('sa', ['long', 'long'], ['long']),
        ('sb', ['uint', 'long', 'uint'], ['ulong']),
        ('sc', ['uint', 'uint', 'uint', 'long', 'uint'], ['long']),     # d, e - on the stack
        ('sd', ['long', 'uint', 'uint', 'uint', 'int'], ['ulong']),     # the 5th and 6th slots - the stack
        ('se', ['ulong', 'long', 'long'], ['ulong']),                   # the third long - on the stack
        ('sf', ['int', 'long', 'ulong'], ['long']),                     # ulong does not fit in R3 - the stack
        ('sg', ['int', 'uint', 'long'], ['ulong'])]                     # long in R2:R3
PTR_SIGS = {'sb': ('fpb', 1), 'se': ('fpe', 2), 'sf': ('fpf', 1)}      # pointer variables
# functions with several results: (name, parameters, results, the rule for result i)
MULTI = [('mr2', ['long', 'long'], ['long', 'long'], lambda a: (a[1], a[0])),
         ('mr3', ['uint', 'long', 'ulong'], ['long', 'uint', 'uint'], lambda a: (a[2], a[1], a[0])),
         ('mr4', ['ulong', 'ulong'], ['ulong', 'ulong'], lambda a: (a[1], a[0]))]


def pool(t, rnd):
    w = W[t]
    lo, hi = const_range(t)
    edges = [0, 1, -1, 2, 3, 255, 256, 1 << (w - 1), (1 << (w - 1)) - 1, -(1 << (w - 1)),
             (1 << w) - 1, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 12345, 100000, 0x12345678 if w == 32 else 4660]
    if rnd.random() < 0.6:
        v = rnd.choice(edges)
    else:
        v = rnd.randint(lo, hi)
    return max(lo, min(hi, v))


# ------------------------------------------------------------------ generator
class Gen:
    def __init__(self, seed, level, deep, nodiv, regs=False):
        self.regs = regs
        self.rnd = random.Random(seed)
        self.level, self.depth0, self.nodiv = level, 6 if deep else 4, nodiv
        self.env = {'a': 0, 'b': 0, 'u': 0, 'v': 0, 'bb': 0, 'cc': 0, 'x': 0, 'y': 0, 'z': 0,
                    'p': 0, 'q': 0, 'r': 0, 'gl': -100000, 'gu': 0xDEADBEEF, 'gi': -3,
                    'arr': [1, -2, 100000, 0x7FFFFFFF], 'parr': [7, 0xFFFFFFFF, 0x10000, 0],
                    'st': {'c': 0, 'w': 0, 'd': 0}}
        self.lines, self.res = [], []
        self.trace = []              # (the number of lines, the state) after each statement - for debugging

    # ----- constants adapted to a type
    def const(self, t, allow_neg_unsigned=False):
        v = pool(t, self.rnd)
        if not SIGNED[t] and v < 0 and not allow_neg_unsigned:
            v = norm(v, t)
        return ('k', v, t)

    def leaf(self):
        return ('v', self.rnd.choice(LVALS))

    # ----- building nodes with the type rules taken into account
    def fix_sign(self, l, r):
        """If the combination of signedness is forbidden, the right operand is cast to the type of the left."""
        if not sign_ok(ty(l), ty(r)):
            r = ('cast', ty(l), r)
        return l, r

    def make_bin(self, op, l, r):
        if op in ('/', '%'):
            l, r = self.fix_sign(l, r)
            tl, tr = ty(l), ty(r)
            R = result_type(tl, tr)
            unsigned_wide = not SIGNED[tl] and not SIGNED[tr] and R == 'ulong'
            if r[0] == 'k':
                if norm(r[1], r[2]) == 0:
                    r = ('k', 1, r[2])
            elif not unsigned_wide:
                r = ('bin', '|', r, ('k', 1, tr))
        return ('bin', op, l, r)

    def expr(self, d):
        rnd, lv = self.rnd, self.level
        if d <= 0 or rnd.random() < 0.2:
            return self.leaf()
        forms = ['cast', 'cast']
        if lv >= 2:
            forms += ['call', 'call']
        if lv >= 3:
            forms += ['un', 'not', 'bin', 'bin', 'bin', 'cmp', 'cmp', 'cmp', 'logic', 'shiftc', 'compose', 'half', 'half']
        if lv >= 5:
            forms += ['bin', 'bin', 'shiftv', 'shiftv', 'divmod', 'divmod', 'mulw', 'mulp2', 'divpow2']
        f = rnd.choice(forms)
        if f == 'cast':
            return ('cast', rnd.choice(['int', 'uint', 'long', 'ulong', 'byte', 'char']), self.expr(d - 1))
        if f == 'call':
            sig = rnd.choice(SIGS)
            i = rnd.randrange(len(sig[1]))
            ptr = None
            if sig[0] in PTR_SIGS and rnd.random() < 0.5:
                pn, pi = PTR_SIGS[sig[0]]
                ptr, i = pn, pi
            args = []
            for t in sig[1]:
                a = self.expr(d - 1)
                if WIDE(ty(a)) and not WIDE(t):
                    a = ('cast', t, a)
                args.append(a)
            return ('call', None, args, sig, i, ptr)
        if f == 'un':
            return ('un', rnd.choice(['-', '~']), self.expr(d - 1))
        if f == 'not':
            return ('not', self.expr(d - 1))
        if f == 'logic':
            return ('logic', rnd.choice(['&&', '||']), self.expr(d - 1), self.expr(d - 1))
        if f in ('bin', 'divmod', 'cmp'):
            if f == 'cmp':
                op = rnd.choice(['==', '!=', '<', '>', '<=', '>='])
            elif f == 'divmod':
                if self.nodiv:
                    return self.expr(d - 1)
                op = rnd.choice(['/', '%'])
            else:
                op = rnd.choice(['+', '-', '&', '|', '^'] + (['*'] if lv >= 5 else []))
            return self.operands(op, d, f == 'cmp')
        if f == 'mulw':
            # ulong(x) * ulong(y), x and y unsigned and no wider than a word, or a constant up to 65535: MUL and MULHU without a call
            xa = ('cast', 'ulong', ('cast', rnd.choice(['uint', 'byte']), self.expr(d - 1)))
            if rnd.random() < 0.5:
                yb = ('cast', 'ulong', ('cast', rnd.choice(['uint', 'byte']), self.expr(d - 1)))
            else:
                yb = ('k', rnd.choice([0, 1, 2, 1000, 65535]), 'ulong')
            return ('bin', '*', xa, yb) if rnd.random() < 0.5 else ('bin', '*', yb, xa)
        if f == 'mulp2':
            k = ('k', 1 << rnd.choice([0, 1, 4, 15, 16, 17, 31]), rnd.choice(['ulong', 'long']))
            x = ('cast', ty(k), self.expr(d - 1))
            return ('bin', '*', x, k) if rnd.random() < 0.5 else ('bin', '*', k, x)
        if f == 'divpow2':
            if self.nodiv:
                return self.expr(d - 1)
            x = ('cast', 'ulong', self.expr(d - 1))
            c = ('k', rnd.choice([3, 5, 7, 10, 100, 200, 255]) if rnd.random() < 0.5 else 1 << rnd.choice([0, 1, 3, 8, 15, 16, 17, 31]), 'ulong')
            return self.make_bin(rnd.choice(['/', '%']), x, c)
        if f == 'compose':
            # (ulong(h) << 16) | l: the words are assembled without computation
            hx = ('cast', rnd.choice(['uint', 'int', 'byte', 'char']), self.expr(d - 1))
            ly = ('cast', rnd.choice(['uint', 'uint', 'byte', 'int', 'char']), self.expr(d - 1))
            sh = ('shift', '<<', ('cast', 'ulong', hx), ('k', 16, 'uint'))
            lw = ('cast', rnd.choice(['ulong', 'long']), ly)
            a, b = (sh, lw) if rnd.random() < 0.6 else (lw, sh)
            return ('bin', rnd.choice(['|', '|', '+', '^']), a, b)
        if f == 'half':
            # uint(x >> k) for k = 16..31 and uint(x << k) for k = 1..15: half without computing the pair
            wx = ('cast', rnd.choice(['long', 'ulong']), self.expr(d - 1))
            if rnd.random() < 0.7:
                sh = ('shift', '>>', wx, ('k', rnd.choice([16, 17, 20, 24, 31]), 'uint'))
            else:
                sh = ('shift', '<<', wx, ('k', rnd.choice([1, 4, 8, 15]), 'uint'))
            return ('cast', rnd.choice(['uint', 'int', 'byte', 'char']), sh)
        if f == 'shiftc':
            l = self.expr(d - 1)
            n = rnd.choice(list(range(0, 5)) + [8, 15, 16, 17, 24, 31, 32, 33, 40])
            return ('shift', rnd.choice(['<<', '>>']), l, ('k', n, 'uint'))
        if f == 'shiftv':
            return ('shift', rnd.choice(['<<', '>>']), self.expr(d - 1), self.expr(d - 1))
        raise ValueError(f)

    def operands(self, op, d, is_cmp):
        rnd = self.rnd
        mode = rnd.random()
        if mode < 0.2:
            r = self.expr(d - 1)
            l = self.const(ty(r), allow_neg_unsigned=not is_cmp and op not in ('/', '%'))
            if is_cmp and rnd.random() < 0.55: l = ('k', 0, ty(r))
        elif mode < 0.45:
            l = self.expr(d - 1)
            r = self.const(ty(l), allow_neg_unsigned=not is_cmp and op not in ('/', '%'))
            if is_cmp and rnd.random() < 0.55: r = ('k', 0, ty(l))
        else:
            l, r = self.expr(d - 1), self.expr(d - 1)
        if op in ('<', '>', '<=', '>='):
            l, r = self.fix_sign(l, r)
            return ('cmp', op, l, r)
        if op in ('==', '!='):
            return ('cmp', op, l, r)
        return self.make_bin(op, l, r)

    # ----- safe computation with a retry on undefined behavior
    def good_expr(self, d, extra=None):
        for _ in range(200):
            e = self.expr(d) if extra is None else extra()
            try:
                ev(e, self.env)
                return e
            except Undefined:
                continue
        return self.leaf()

    def safe(self, e):
        try:
            ev(e, self.env)
            return True
        except Undefined:
            return False

    # ----- statements
    def conv_rhs(self, lv, e):
        """Implicit assignment: narrowing long/ulong to a narrow type is possible only with an explicit cast."""
        if WIDE(ty(e)) and lv.decl in ('int', 'uint', 'byte', 'char'):
            return ('cast', lv.decl, e)
        return e

    def st_assign(self, ind='    '):
        rnd = self.rnd
        lv = rnd.choice(LVALS)
        if rnd.random() < 0.25:
            t = promote(lv.decl)
            lo, hi = (0, 255) if lv.decl == 'byte' else (-128, 127) if lv.decl == 'char' else const_range(t)
            v = pool(t, rnd) if lv.decl not in ('byte', 'char') else rnd.randint(lo, hi)
            v = max(lo, min(hi, v))
            lv.put(self.env, v)
            return ['%s%s = %s' % (ind, lv.src, lit(v, rnd))]
        if self.level >= 3 and rnd.random() < 0.3:
            # the right-hand side reads the destination: x = y - x, x = x ^ f(x)
            op = rnd.choice(['+', '-', '&', '|', '^'])
            other = self.expr(self.depth0 - 1)
            leaf = ('v', lv)
            e = self.make_bin(op, leaf, other) if rnd.random() < 0.5 else self.make_bin(op, other, leaf)
            if not self.safe(e):
                e = self.good_expr(self.depth0)
        else:
            e = self.good_expr(self.depth0)
        e = self.conv_rhs(lv, e)
        lv.put(self.env, ev(e, self.env))
        return ['%s%s = %s' % (ind, lv.src, render(e, rnd))]

    def st_opassign(self, ind='    '):
        rnd = self.rnd
        lv = rnd.choice(LVALS)
        ops = ['+', '-', '&', '|', '^', '<<', '>>']
        if self.level >= 5:
            ops += ['*'] + ([] if self.nodiv else ['/', '%'])
        op = rnd.choice(ops)
        for _ in range(100):
            r = self.expr(self.depth0 - 1)
            if op in ('<<', '>>'):
                if self.level < 5:
                    r = ('k', rnd.choice(list(range(0, 5)) + [8, 15, 16, 17, 24, 31, 32, 33, 40]), 'uint')
                n = ('shift', op, ('v', lv), r)
            else:
                if not WIDE(lv.ptype) and WIDE(ty(r)):
                    r = ('cast', lv.ptype, r)
                n = self.make_bin(op, ('v', lv), r)
                if n[0] == 'bin' and n[1] in ('/', '%') and ty(n) != lv.ptype and not WIDE(lv.ptype):
                    continue
            if self.safe(n):
                lv.put(self.env, ev(n, self.env))
                return ['%s%s %s= %s' % (ind, lv.src, op, render(n[3], rnd))]
        return []

    def st_incdec(self, ind='    '):
        lv = self.rnd.choice(LVALS) if self.rnd.random() < 0.5 else self.rnd.choice(WIDE_TARGETS)
        op = self.rnd.choice(['++', '--'])
        pre = []
        if self.rnd.random() < 0.6:
            # a value at the border of carry or borrow between words
            t = promote(lv.decl)
            w = W[t]
            edge = self.rnd.choice([0, 1, (1 << (w // 2)) - 1, 1 << (w // 2), (1 << w) - 1, (1 << (w - 1)) - 1, 1 << (w - 1)])
            if lv.decl in ('byte', 'char'): edge = self.rnd.choice([0, 1, 127, 128, 255])
            lv.put(self.env, edge)
            pre = ['%s%s = %s' % (ind, lv.src, lit(to_decl(edge, lv.decl) if lv.decl in ('byte','char') else norm(edge, t), self.rnd))]
        lv.put(self.env, lv.get(self.env) + (1 if op == '++' else -1))
        src = '(%s)' % lv.src if lv.src.startswith('*') else lv.src      # *p-- is *(p--)
        return pre + ['%s%s%s' % (ind, src, op)]

    def st_if(self):
        rnd = self.rnd
        cond = self.good_expr(self.depth0 - 1)
        taken = ev(cond, self.env) != 0
        out = ['    if %s {' % render(cond, rnd)]
        saved = {k: (dict(v) if isinstance(v, dict) else list(v) if isinstance(v, list) else v)
                 for k, v in self.env.items()}
        branches = []
        for _ in range(2):
            self.env = {k: (dict(v) if isinstance(v, dict) else list(v) if isinstance(v, list) else v)
                        for k, v in saved.items()}
            body = []
            for _ in range(rnd.randint(1, 2)):
                body += self.st_assign('        ')
            branches.append((body, self.env))
        out += branches[0][0] + ['    } else {'] + branches[1][0] + ['    }']
        self.env = branches[0][1] if taken else branches[1][1]
        return out

    def st_multi(self):
        rnd = self.rnd
        m = rnd.choice(MULTI)
        name, params, results, rule = m
        args = []
        for t in params:
            a = self.good_expr(self.depth0 - 1)
            if WIDE(ty(a)) and not WIDE(t):
                a = ('cast', t, a)
            args.append(a)
        targets, used = [], set()
        for t in results:
            cand = [l for l in (WIDE_TARGETS if WIDE(t) else WIDE_TARGETS + NARROW_TARGETS) if l.src not in used]
            l = rnd.choice(cand)
            used.add(l.src)
            targets.append(l)
        vals = [norm(ev(a, self.env), t) for a, t in zip(args, params)]
        outv = rule(vals)
        for l, v, t in zip(targets, outv, results):
            l.put(self.env, norm(v, t))
        return ['    %s = %s(%s)' % (', '.join(l.src for l in targets), name,
                                      ', '.join(render(a, rnd) for a in args))]

    def st_record(self):
        e = self.good_expr(self.depth0)
        k = len(self.res)
        self.res.append(to_decl(ev(e, self.env), 'ulong'))
        return ['    res[%d] = ulong(%s)' % (k, render(e, self.rnd))]

    def build(self, nrec=40):
        rnd, lv = self.rnd, self.level
        # initial values of local variables
        for name, decl in LOCALS.items():
            t = promote(decl)
            if decl == 'byte':
                v = rnd.choice([0, 1, 127, 128, 255, rnd.randint(0, 255)])
            elif decl == 'char':
                v = rnd.choice([0, 1, -1, 127, -128, rnd.randint(-128, 127)])
            else:
                v = pool(t, rnd)
                v = norm(v, t)
            self.env[name] = v
            self.lines.append('    %s = %s' % (name, lit(v, rnd)))
        total = nrec * 3
        for i in range(total):
            r = rnd.random()
            if i % 3 == 2 or r < 0.34:
                self.lines += self.st_record()
                continue
            kinds = ['assign', 'assign']
            if lv >= 2: kinds += ['multi']
            if lv >= 3: kinds += ['opassign', 'opassign', 'incdec', 'if']
            k = rnd.choice(kinds)
            self.lines += {'assign': self.st_assign, 'multi': self.st_multi, 'opassign': self.st_opassign,
                           'incdec': self.st_incdec, 'if': self.st_if}[k]()
            self.trace.append((len(self.lines), copy.deepcopy(self.env)))
        return self


# ------------------------------------------------------------------ program
def program(g):
    nres = len(g.res)
    s = ['var OUT at $FF20: byte',
         'struct S { c: byte; w: long; d: uint }',
         'var gl: long = -100000',
         'var gu: ulong = $DEADBEEF',
         'var gi: int = -3',
         'var arr: [4]long = {1, -2, 100000, $7FFFFFFF}',
         'var parr: [4]ulong = {7, $FFFFFFFF, $10000, 0}',
         'var st: S',
         'var pl: ptr long = &arr[1]',
         'var res: [%d]ulong' % nres,
         'fn putc(c: byte) { OUT = c }',
         'fn puth(v: uint) { var i = 0; while i < 4 { var d = (v >> 12) & 15; if d < 10 { putc(byte(d + 48)) } else { putc(byte(d + 55)) }; v <<= 4; i++ } }']
    if g.level >= 2:
        for name, params, results in SIGS:
            plist = ', '.join('_p%d: %s' % (i, t) for i, t in enumerate(params))
            for i in range(len(params)):
                s.append('fn %s_%d(%s): %s { return %s(_p%d) }' % (name, i, plist, results[0], results[0], i))
        s.append('var fpb: ptr fn(uint, long, uint): ulong = sb_1')
        s.append('var fpe: ptr fn(ulong, long, long): ulong = se_2')
        s.append('var fpf: ptr fn(int, long, ulong): long = sf_1')
        s.append('fn mr2(a, b: long): long, long { return b, a }')
        s.append('fn mr3(a: uint, b: long, c: ulong): long, uint, uint { return long(c), uint(b), a }')
        s.append('fn mr4(a, b: ulong): ulong, ulong { return b, a }')
    narrow = ['var a, b: int', 'var u, v: uint', 'var bb: byte', 'var cc: char']
    if g.regs:
        s += narrow          # narrow variables are global: all registers go to the pairs
    s.append('fn f() {')
    if not g.regs:
        s += ['    ' + d for d in narrow]
    s.append('    var x, y, z: long')
    s.append('    var p, q, r: ulong')
    s += g.lines
    s.append('}')
    s.append('fn main() {')
    s.append('    f()')
    s.append('    var pw: ptr uint = ptr uint(&res[0])')
    s.append('    var i: uint = 0')
    s.append('    while i < %d { puth(pw[i * 2 + 1]); puth(pw[i * 2]); putc(10); i++ }' % nres)
    s.append('}')
    return '\n'.join(s) + '\n'


# ------------------------------------------------------------------ self-check of the reference in C
def selftest():
    """The same operations computed in C on exact 128-bit integers with explicit truncation to the width."""
    rnd = random.Random(1)
    types = ['int', 'uint', 'long', 'ulong']
    ops = ['+', '-', '*', '&', '|', '^', '/', '%', '<<', '>>', '<', '>', '<=', '>=', '==', '!=']
    cases = []
    for _ in range(4000):
        tl, tr = rnd.choice(types), rnd.choice(types)
        op = rnd.choice(ops)
        lv, rv = norm(pool(tl, rnd), tl), norm(pool(tr, rnd), tr)
        try:
            val, rt = binval(op, lv, tl, rv, tr)
        except Undefined:
            continue
        cases.append((op, tl, lv, tr, rv, val))
    wd = {'int': 16, 'uint': 16, 'long': 32, 'ulong': 32}
    sg = {'int': 1, 'uint': 0, 'long': 1, 'ulong': 0}
    c = ['#include <stdio.h>', 'typedef __int128 I;',
         'static I wrap(I v, int w, int s) { I m = ((I)1 << w) - 1; v &= m; if (s && ((v >> (w - 1)) & 1)) v -= (I)1 << w; return v; }',
         'int main(void) { int bad = 0; I l, r, res;']
    for i, (op, tl, lv, tr, rv, val) in enumerate(cases):
        wl, wr = wd[tl], wd[tr]
        wcommon = 32 if 32 in (wl, wr) else 16
        m = (1 << wcommon) - 1
        if op in ('<<', '>>'):
            wres, sres = wl, sg[tl]
            e = 'l %s (r & %d)' % (op, wl - 1)
        elif op in ('<', '>', '<=', '>='):
            wres, sres, e = 32, 1, '(l %s r)' % op
        elif op in ('==', '!='):
            wres, sres, e = 32, 1, '((l & %d) %s (r & %d))' % (m, op, m)
        else:
            if wl == 32 and wr == 32: rtype = tl
            elif wl == 32: rtype = tl
            elif wr == 32: rtype = tr
            else: rtype = tl
            wres, sres = wd[rtype], sg[rtype]
            if op in ('+', '-', '*', '&', '|', '^'):
                e = '(l & %d) %s (r & %d)' % (m, op, m)
            elif op == '/':
                e = '(r == 0 ? (I)0xFFFFFFFF : l / r)'
            else:
                e = '(r == 0 ? l : l % r)'
        c.append('  l = %d; r = %d; res = wrap(%s, %d, %d);' % (lv, rv, e, wres, sres))
        c.append('  if ((long long)res != %dLL) { printf("BAD case %d: %s %s %s\\n"); bad++; }' % (val, i, tl, op.replace('%', '%%'), tr))
    c.append('  printf("selftest: %d cases, %%d mismatches\\n", bad); return bad != 0; }' % len(cases))
    open('/tmp/b32self.c', 'w').write('\n'.join(c) + '\n')
    r = subprocess.run('gcc -O1 -o /tmp/b32self /tmp/b32self.c && /tmp/b32self', shell=True, capture_output=True, text=True)
    print(r.stdout + r.stderr, end='')
    return r.returncode


# ------------------------------------------------------------------ run
def main():
    args = sys.argv[1:]
    if '--selftest' in args:
        sys.exit(selftest())
    seed = int(args[0])
    level = 5
    if '--level' in args:
        level = int(args[args.index('--level') + 1])
    g = Gen(seed, level, '--deep' in args, '--nodiv' in args, '--regs' in args).build()
    src = program(g)
    want = ''.join('%08X\n' % (v & 0xFFFFFFFF) for v in g.res)
    open('/tmp/b32fuzz.b16', 'w').write(src)
    open('/tmp/b32fuzz.expected', 'w').write(want)
    if '--emit-only' in args:
        print('/tmp/b32fuzz.b16  /tmp/b32fuzz.expected')
        return
    B16C = os.environ.get('B16C', './b16c')
    ASM = os.environ.get('CPU16ASM', 'cpu16asm')
    EMU = os.environ.get('CPU16', 'cpu16')
    r = subprocess.run('%s /tmp/b32fuzz.b16 -o /tmp/b32fuzz.s && %s -o /tmp/b32fuzz.bin /tmp/b32fuzz.s >/dev/null && %s /tmp/b32fuzz.bin'
                       % (B16C, ASM, EMU), shell=True, capture_output=True, text=True)
    if r.stdout != want:
        print('MISMATCH seed', seed, 'level', level, r.stderr[-800:])
        for i, (x, y) in enumerate(zip(r.stdout.split(), want.split())):
            if x != y:
                print('result', i, 'got', x, 'want', y)
                break
        else:
            print('output length', len(r.stdout.split()), 'expected', len(want.split()))
        sys.exit(1)


if __name__ == '__main__':
    main()
