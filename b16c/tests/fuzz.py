"""A differential test: random B16 expressions against a Python reference.
   python3 tests/fuzz.py SEED [--nodiv]
   --nodiv: without / and %, the function stays a leaf (variables in R0-R3).
   --deep: expressions of depth 6 (checks the rollback when registers run out).
   Paths: the environment variables B16C, CPU16ASM, CPU16."""
import os, random, subprocess, sys
R = random.Random(int(sys.argv[1]))
NODIV = '--nodiv' in sys.argv
DEPTH = 6 if '--deep' in sys.argv else 4
B16C = os.environ.get('B16C', './b16c')
ASM = os.environ.get('CPU16ASM', 'cpu16asm')
EMU = os.environ.get('CPU16', 'cpu16')
def s16(v): v &= 0xFFFF; return v - 0x10000 if v & 0x8000 else v
CONSTS = [1, -1, 2, -2, 3, -3, 4, 5, 6, 7, -7, 8, 10, 12, 16, -16, 100, 128, 1000, 641, 4096, 10000, 16384, 32767, -32767, -1000, 255, 256]
VARS = ['a','b','c','g','h','*p','q[1]','m[b&3][c&3]','pm[0][a&3][b&3]']   # a,b,c locals(int), g,h globals, *p, q[1] memory
def expr(d):
    if d == 0 or R.random() < 0.25:
        if R.random() < 0.3: return str(R.randint(-40, 300))
        return R.choice(VARS)
    ops = ['+','-','*','&','|','^','<<','>>','/','%','==','<','!=','>=','&&','||']
    if NODIV: ops = [o for o in ops if o not in '/%']
    op = R.choice(ops)
    l, r = expr(d-1), expr(d-1)
    if op in ('<<','>>'): r = str(R.randint(0, 15))
    # constant divisors and multipliers: shifts, masks, multiplication by the reciprocal (MULHU)
    if op in ('/','%') and R.random() < 0.5: r = str(R.choice(CONSTS))
    elif op in ('/','%'): r = '(' + r + ' | 1)'
    if op == '*' and R.random() < 0.3: r = str(R.choice(CONSTS + [0]))
    isnum = lambda x: x.lstrip('-').isdigit()
    if isnum(l) and isnum(r):                          # a constant with a constant is computed exactly, without wrap-around
        if op in ('<<', '>>'): l = R.choice(VARS)
        elif op in ('/', '%'): r = '(' + R.choice(VARS) + ' | 1)'
        else: r = R.choice(VARS)
    res = '(' + l + ' ' + op + ' ' + r + ')'
    if not any(ch.isalpha() for ch in res): return R.choice(VARS)   # without constant subexpressions
    return res
lines = []; checks = []
for i in range(40):
    e = expr(DEPTH)
    lines.append(e)
src = ['var OUT at $FF20: byte','var g: int = 7','var h: int = -300','var cell: int = 1234','var q: [3]int = {5, -6, 77}', 'var m: [4][4]int', 'var pm: ptr [4][4]int = &m',
 'fn putc(c: byte) { OUT = c }',
 'fn puth(v: int) { var i = 0; while i < 4 { var d = (v >> 12) & 15; if d < 10 { putc(byte(d + 48)) } else { putc(byte(d + 55)) }; v <<= 4; i++ } ; putc(10) }',
 'fn f(a, b, c: int) {', '    var p: ptr int = &cell']
src[-2] = 'fn f(a, b, c: int) {'
src.insert(-2, 'var res: [62]int')
src.insert(-2, 'fn init_m() { var i = 0; while i < 16 { m[i >> 2][i & 3] = i * 37 - 200; i++ } }')
k = 0
for e in lines:
    src.append('    res[%d] = %s' % (k, e)); k += 1
src.append('    a = a * 3 + b; b -= c; c ^= a; g += h; *p += a; q[1] -= b')
for e in lines[:10]:
    src.append('    res[%d] = %s' % (k, e)); k += 1
# assignments to register variables: checks computation straight into the variable's register
assigns = []
for i in range(12):
    tgt = R.choice(['a', 'b', 'c'])
    assigns.append((tgt, expr(DEPTH - 1)))
for tgt, e in assigns:
    src.append('    %s = %s' % (tgt, e))
    src.append('    res[%d] = %s' % (k, tgt)); k += 1
src.append('}')
src.append('fn main() { init_m(); f(11, -3, 400); var i = 0; while i < 62 { puth(res[i]); i++ } }')
open('/tmp/b16fuzz.b16','w').write('\n'.join(src)+'\n')
# reference evaluation in python
def cdiv(x,y):
    q = abs(x)//abs(y); return q if (x<0)==(y<0) else -q
env = {'a':11,'b':-3,'c':400,'g':7,'h':-300,'cell':1234,'q':[5,-6,77]}
def evaluate(e, env):
    # tiny recursive evaluator over our own grammar
    toks = e.replace('(',' ( ').replace(')',' ) ').split()
    pos = [0]
    def get():
        t = toks[pos[0]]; pos[0]+=1; return t
    def prim():
        t = get()
        if t == '(':
            l = prim(); op = get(); r = prim(); get()
            return binop(op, l, r)
        if t == '*p': return env['cell']
        if t == 'q[1]': return env['q'][1]
        if t == 'm[b&3][c&3]': return s16((env['b'] & 3) * 4 * 37 + (env['c'] & 3) * 37 - 200)
        if t == 'pm[0][a&3][b&3]': return s16((env['a'] & 3) * 4 * 37 + (env['b'] & 3) * 37 - 200)
        if t.lstrip('-').isdigit(): return s16(int(t))
        return env[t]
    return prim()
def binop(op, l, r):
    if op=='+': return s16(l+r)
    if op=='-': return s16(l-r)
    if op=='*': return s16(l*r)
    if op=='&': return s16(l&r)
    if op=='|': return s16(l|r)
    if op=='^': return s16(l^r)
    if op=='<<': return s16(l<<r)
    if op=='>>': return s16(l>>r)
    if op=='/': return s16(cdiv(l,r))
    if op=='%': return s16(l - cdiv(l,r)*r)
    if op=='==': return int(l==r)
    if op=='!=': return int(l!=r)
    if op=='<': return int(l<r)
    if op=='>=': return int(l>=r)
    if op=='&&': return int(bool(l) and bool(r))
    if op=='||': return int(bool(l) or bool(r))
exp = [evaluate(e, env) for e in lines]
a,b,c = env['a'],env['b'],env['c']
a = s16(a*3+b); b = s16(b-c); c = s16(c^a); env['g']=s16(env['g']+env['h']); env['cell']=s16(env['cell']+a); env['q'][1]=s16(env['q'][1]-b)
env.update(a=a,b=b,c=c)
exp += [evaluate(e, env) for e in lines[:10]]
for tgt, e in assigns:
    env[tgt] = evaluate(e, env)
    exp.append(env[tgt])
want = ''.join('%04X\n' % (v & 0xFFFF) for v in exp)
r = subprocess.run(f'{B16C} /tmp/b16fuzz.b16 -o /tmp/b16fuzz.s && {ASM} -o /tmp/b16fuzz.bin /tmp/b16fuzz.s >/dev/null && {EMU} /tmp/b16fuzz.bin', shell=True, capture_output=True, text=True)
got = r.stdout
if got != want:
    print('MISMATCH seed', sys.argv[1], r.stderr[-500:])
    for i,(x,y) in enumerate(zip(got.split(), want.split())):
        if x!=y: print(i, x, y, (lines+lines[:10]+[t+' = '+e for t,e in assigns])[i]); break
    sys.exit(1)
