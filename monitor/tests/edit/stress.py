#!/usr/bin/env python3
"""stress.py [FIRST_SEED [SEED_COUNT [ACTION_COUNT]]] - a random check of the edit editor against a model.

Builds a variant of the editor with small blocks (BLK = 512, FILL = 448: block splitting and merging
at every step) as out/edits.cx, puts random text on the volume, runs random keys under kmon
(characters, Backspace, Del, Enter with indentation, Ctrl+Y, arrows, Home, End,
Ctrl+B, Ctrl+E, selection Shift+Right, Ctrl+C, Ctrl+X, Ctrl+V, Del and typing over a selection),
writes the file and compares it with a Python model; then undoes all steps (Ctrl+Z) and checks
that the text returned to the original (written through F12 as u.txt).
It also checks the return of all frames and the volume's integrity. The built out/monitor.bin,
out/kmon, out/shell.bin (make all; tools/b16build.sh user/shell/shell.b16).
"""
import random, subprocess, json, sys, os
M = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
O = M + '/out'

def build_variant():
    os.makedirs(O + '/edits', exist_ok=True)
    src = open(M + '/user/edit/edit.b16').read()
    src = src.replace('const BLK = 8192 ', 'const BLK = 512 //').replace('const FILL = 7168 ', 'const FILL = 448 //')
    src = src.replace('include "../fm/scr.b16"', 'include "%s/user/fm/scr.b16"' % M)
    open(O + '/edits/edits.b16', 'w').write(src)
    r = subprocess.run('tools/b16build.sh out/edits/edits.b16', shell=True, cwd=M, capture_output=True)
    if r.returncode: print(r.stdout.decode(), r.stderr.decode()); sys.exit(1)

def run(seed, nops):
    R=random.Random(seed)
    clip=[]
    words=['abc','x','hello','  ','\t','zz9','Q']
    init=''.join(R.choice(['', ' ','\t'])+''.join(R.choice('abcdefgh') for _ in range(R.randint(0,40)))+'\n' for _ in range(R.randint(300,900)))
    open(O + '/edits/init.txt','w').write(init)
    t=list(init); cur=0; want=0
    def ls(p):
        while p>0 and t[p-1]!='\n': p-=1
        return p
    def le(p):
        while p<len(t) and t[p]!='\n': p+=1
        return p
    def col(l,p):
        c=0
        for i in range(l,p): c+= (8-(c&7)) if t[i]=='\t' else 1
        return c
    def atcol(l,w):
        c=0
        while l<len(t) and t[l]!='\n':
            cw=(8-(c&7)) if t[l]=='\t' else 1
            if c+cw>w: break
            c+=cw; l+=1
        return l
    scn=['after "A:/> " steps 2000 type "cd /data\\n"','after "A:/data> " steps 2000 type "edits t.txt\\n"','screen "Ln 1/" steps 2000 dump']
    W=' steps 150000 '
    ops=[]
    for _ in range(nops):
        r=R.random(); vert=False
        if r<0.30:
            w=R.choice(words)
            for ch in w: t.insert(cur,ch); cur+=1
            ops.append('after ""'+W+'type "%s"'%w.replace('\t','\\t'))
        elif r<0.40:
            if cur>0: del t[cur-1]; cur-=1
            ops.append('after ""'+W+'key 08 0E')
        elif r<0.48:
            if cur<len(t): del t[cur]
            ops.append('after ""'+W+'key 00 53')
        elif r<0.55:
            l=ls(cur); ind=[]
            i=l
            while len(ind)<120 and i<cur and t[i] in ' \t': ind.append(t[i]); i+=1
            t.insert(cur,'\n'); cur+=1
            for k,c in enumerate(ind): t.insert(cur+k,c)
            cur+=len(ind)
            ops.append('after ""'+W+'type "\\n"')
        elif r<0.60:
            l=ls(cur); e=le(cur)
            if e<len(t): e+=1
            del t[l:e]; cur=l
            ops.append('after ""'+W+'key 19 15')
        elif r<0.68:
            k=R.randint(1,30)
            for _ in range(k):
                if cur>0: cur-=1
            ops.append(('after ""'+W+'key 00 4B\n')*k)
        elif r<0.74:
            if cur<len(t): cur+=1
            ops.append('after ""'+W+'key 00 4D')
        elif r<0.84:
            vert=True
            k=R.randint(1,40); d=R.choice([-1,1])
            for _ in range(k):
                p=cur
                if d<0:
                    l=ls(p)
                    if l>0: p=ls(l-1)
                else:
                    e=le(p)
                    if e<len(t): p=e+1
                cur=atcol(ls(p),want)
            ops.append(('after ""'+W+'key 00 %s\n'%('48' if d<0 else '50'))*k)
        elif r<0.86:
            # selection Shift+Right*k, then: copy or cut with a jump and paste,
            # deletion (Del) or typing over the selection
            k=R.randint(1,700); s0=cur; e0=min(len(t),cur+k)
            seq=['after ""'+W+'key 00 4D 1']*(e0-s0)
            cur=e0
            op=R.choice(['copy','cut','del','type'])
            if e0>s0:
                if op=='copy': clip=t[s0:e0]; seq.append('after ""'+W+'key 03 2E 2')
                elif op=='cut': clip=t[s0:e0]; del t[s0:e0]; cur=s0; seq.append('after ""'+W+'key 18 2D 2')
                elif op=='del': del t[s0:e0]; cur=s0; seq.append('after ""'+W+'key 00 53')
                else: del t[s0:e0]; cur=s0; t.insert(cur,'Z'); cur+=1; seq.append('after ""'+W+'type "Z"')
            if op in ('copy','cut'):
                d=R.choice(['b','e','l'])
                if d=='b': cur=0; seq.append('after ""'+W+'key 00 47 2')
                elif d=='e': cur=len(t); seq.append('after ""'+W+'key 00 4F 2')
                else:
                    j=R.randint(0,300); j=min(j,cur)
                    if e0>s0 and op=='copy': seq.append('after ""'+W+'key 00 4B'); cur-=1 if cur>0 else 0; j=min(j,cur)
                    cur-=j; seq+=['after ""'+W+'key 00 4B']*j
                if clip:
                    t[cur:cur]=clip; cur+=len(clip)
                    seq.append('after ""'+W+'key 16 2F 2')
            elif e0==s0 and op=='type':
                t.insert(cur,'Z'); cur+=1; seq.append('after ""'+W+'type "Z"')
            ops.append('\n'.join(seq))
        elif r<0.88: cur=ls(cur); ops.append('after ""'+W+'key 00 47')
        elif r<0.92: cur=le(cur); ops.append('after ""'+W+'key 00 4F')
        elif r<0.95: cur=0; ops.append('after ""'+W+'key 02 30')
        else: cur=len(t); ops.append('after ""'+W+'key 05 12')
        if not vert: want=col(ls(cur),cur)
    scn += [o.rstrip('\n') for o in ops]
    scn += ['after ""'+W+'key 13 1F','screen "saved" steps 2000 dump']
    # undo of all steps: the text is the original again; write it as u.txt
    scn += ['after ""'+W+'key 1A 2C 2'] * (nops + 20)
    scn += ['after ""'+W+'key 00 58','screen "Save as" steps 2000 type "\\x15u.txt\\n"','screen "saved" steps 2000 key 1B 01',
            'after "A:/data> " steps 2000 type "exit\\n"']
    open(O + '/edits/s.scn','w').write('\n'.join(scn)+'\n')
    e=[{"path":"/sys","dir":True},{"path":"/bin","dir":True},{"path":"/sys/shell.cx","src":"out/shell.bin"},
       {"path":"/bin/edits.cx","src":"out/edits.bin"},{"path":"/data","dir":True},{"path":"/data/t.txt","src":O + "/edits/init.txt"}]
    json.dump({"sectors":2048,"label":"SYSTEM","entries":e},open(O + '/st.json','w'))
    subprocess.run('python3 tools/fs16tool.py build out/st.img out/st.json',shell=True,cwd=M,capture_output=True)
    r=subprocess.run('out/kmon out/monitor.bin --map out/monitor.map --disk out/st.img --tick-every 5000 --script out/edits/s.scn --max-steps 2000000000',shell=True,cwd=M,capture_output=True)
    info=json.loads([l for l in r.stderr.decode().splitlines() if l.startswith('KMON ')][0][5:])
    got=subprocess.run('python3 tools/fs16tool.py get out/st.img /data/t.txt',shell=True,cwd=M,capture_output=True).stdout.decode('latin1')
    exp=''.join(t)
    gotu=subprocess.run('python3 tools/fs16tool.py get out/st.img /data/u.txt',shell=True,cwd=M,capture_output=True).stdout.decode('latin1')
    chk=subprocess.run('python3 tools/fs16tool.py check out/st.img',shell=True,cwd=M,capture_output=True).stdout.decode().strip().splitlines()[-1]
    ok = got==exp and gotu==init and not info['timeout'] and info['nfree']==242
    print('seed %d ops %d len %d->%d: %s, undo all: %s, timeout=%s nfree=%d %s'%(seed,nops,len(init),len(exp),'OK' if got==exp else 'MISMATCH','OK' if gotu==init else 'MISMATCH',info['timeout'],info['nfree'],chk.split(':')[-1]))
    if got!=exp:
        i=next((k for k in range(min(len(got),len(exp))) if got[k]!=exp[k]),min(len(got),len(exp)))
        print(' first diff at',i,repr(got[i-20:i+20]),repr(exp[i-20:i+20]))
    return got == exp and gotu == init and not info['timeout'] and info['nfree'] == 242 and chk.endswith('consistent')

if __name__ == '__main__':
    first = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    nops = int(sys.argv[3]) if len(sys.argv) > 3 else 300
    build_variant()
    bad = sum(0 if run(s, nops) else 1 for s in range(first, first + count))
    print('FAILED: %d' % bad if bad else 'ALL OK')
    sys.exit(1 if bad else 0)
