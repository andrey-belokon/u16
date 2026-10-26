#!/usr/bin/env python3
# intro_test.py - a check of the examples from cpu16_intro_en.md.
#
#   - every program marked <!-- run [stdin="..."] --> is assembled by cpu16asm, executed
#     in cpu16, and its output is compared with the following ```text block;
#   - the other ```asm blocks are assembled to check the syntax (fragments: only
#     "undefined symbol" errors are allowed, lines "..." are skipped);
#   - the source text in appendix A must match stdlib.asm.
#
# Run: python3 intro_test.py path/to/cpu16asm path/to/cpu16

import ast
import os
import re
import shutil
import subprocess
import sys
import tempfile

ASM, EMU = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LIB = os.path.join(ROOT, "lib", "stdlib.asm")
SKELETON = os.path.join(ROOT, "docs", "skeleton.asm")
doc = open(os.path.join(ROOT, "docs", "cpu16_intro_en.md"), encoding="utf-8").read()
lib = open(LIB, encoding="utf-8").read()
passed = failed = 0


def check(cond, what, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL:", what, detail)


def assemble(src, tmp):
    with open(os.path.join(tmp, "p.asm"), "w", encoding="utf-8") as f:
        f.write(src)
    return subprocess.run([ASM, "p.asm", "-o", "p.bin"], cwd=tmp, capture_output=True, text=True)


blocks = list(re.finditer(r"(<!-- run(?: stdin=(\"(?:[^\"\\\\]|\\\\.)*\"))? -->\n)?```(asm|text)\n(.*?)```", doc, re.S))
programs = fragments = 0
with tempfile.TemporaryDirectory() as tmp:
    shutil.copy(LIB, tmp)
    for i, m in enumerate(blocks):
        kind, body = m.group(3), m.group(4)
        if kind != "asm":
            continue
        if body.rstrip("\n") == lib.rstrip("\n"):
            continue   # appendix A: checked separately
        if m.group(1):   # a complete program with the expected output
            programs += 1
            stdin = ast.literal_eval(m.group(2)).encode() if m.group(2) else b""
            nxt = blocks[i + 1] if i + 1 < len(blocks) else None
            expected = nxt.group(4) if nxt and nxt.group(3) == "text" else None
            r = assemble(body, tmp)
            if r.returncode:
                check(False, f"program #{programs} assembles", r.stderr[:400])
                continue
            out = subprocess.run([EMU, "-n", "20000000", "p.bin"], cwd=tmp, input=stdin, capture_output=True).stdout
            got = out.decode("utf-8", "replace").rstrip("\n")
            check(expected is not None and got == expected.rstrip("\n"), f"program #{programs} output",
                  f"\n  expected: {expected!r}\n  got:      {got!r}\n  source starts: {body[:80]!r}")
        else:            # a fragment: syntax only
            fragments += 1
            src = "\n".join(l for l in body.split("\n") if l.strip() not in ("...",) and not re.match(r"^\s+\.\.\.\s*(;.*)?$", l))
            r = assemble(src, tmp)
            bad = [l for l in r.stderr.splitlines() if ": error: " in l and "undefined symbol" not in l]
            check(not bad, f"fragment #{fragments} syntax", "\n  " + "\n  ".join(bad[:5]) + f"\n  in: {body[:100]!r}")

check(("```asm\n" + lib.rstrip("\n") + "\n```") in doc, "appendix A equals stdlib.asm")
skel = open(SKELETON, encoding="utf-8").read()
check(("```asm\n" + skel + "```") in doc, "section 6.4 equals skeleton.asm")
print(f"{programs} programs run, {fragments} fragments checked")
print(f"{'FAILED' if failed else 'OK'}: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
