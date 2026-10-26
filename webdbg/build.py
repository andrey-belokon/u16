#!/usr/bin/env python3
# build.py - builds webdbg/cpu16_debugger.html from the sources.
#
# Run:  python3 webdbg/build.py [output.html]
# Needs g++ (C++17) and Python 3. Sources: webdbg/src (template, engine, assembler, demo),
# tools (cpu16_isa.hpp, cpu16_machine.hpp, cpu16asm.cpp), lib/stdlib.asm.
#
# Steps:
#   1. gen_isa_js.cpp -> ISA tables for JavaScript (source of truth: cpu16_isa.hpp);
#   2. cpu16asm checks that demo.asm assembles (the demo goes into the page as source text);
#   3. template + ISA tables + engine.js + assembler.js + demo -> one HTML file.

import json
import re
import unicodedata
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))     # webdbg/
SRC = os.path.join(HERE, "src")
ROOT = os.path.dirname(HERE)                          # project root: b16c/, cpu16/, monitor/, webdbg/
CPU16 = os.path.join(ROOT, "cpu16")
TOOLS = os.path.join(CPU16, "tools")
LIB = os.path.join(CPU16, "lib")


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit(f"build.py: command failed: {' '.join(cmd)}")
    return r.stdout


def box_glyphs(isa_js):
    """Box-drawing geometry for the display, from Unicode character names:
    lines {code: [up, down, left, right]} (1 - single, 2 - double) and blocks {code: shape}."""
    m = re.search(r"charset: \[([0-9,]+)\]", isa_js)
    charset = [int(x) for x in m.group(1).split(",")]
    weight = {"LIGHT": 1, "SINGLE": 1, "HEAVY": 1, "DOUBLE": 2}
    kinds = {"UPPER HALF BLOCK": "upper", "LOWER HALF BLOCK": "lower", "FULL BLOCK": "full", "LEFT HALF BLOCK": "left",
             "RIGHT HALF BLOCK": "right", "LIGHT SHADE": "shade1", "MEDIUM SHADE": "shade2", "DARK SHADE": "shade3"}
    boxes, blocks = {}, {}
    for code, cp in enumerate(charset):
        if not cp:
            continue
        name = unicodedata.name(chr(cp), "")
        if name in kinds:
            blocks[code] = kinds[name]
        if not name.startswith("BOX DRAWINGS "):
            continue
        words = name[len("BOX DRAWINGS "):].split()
        glob = weight.get(words[0], 1)
        arms = {"UP": 0, "DOWN": 0, "LEFT": 0, "RIGHT": 0}
        for part in " ".join(words).split(" AND "):
            ws = [weight[w] for w in part.split() if w in weight]
            w8 = ws[-1] if ws else glob
            for w in part.split():
                if w in arms:
                    arms[w] = w8
                elif w == "HORIZONTAL":
                    arms["LEFT"] = arms["RIGHT"] = w8
                elif w == "VERTICAL":
                    arms["UP"] = arms["DOWN"] = w8
        boxes[code] = [arms["UP"], arms["DOWN"], arms["LEFT"], arms["RIGHT"]]
    return {"boxes": boxes, "blocks": blocks}


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "cpu16_debugger.html")
    with tempfile.TemporaryDirectory() as tmp:
        gen = os.path.join(tmp, "gen_isa_js")
        asm = os.path.join(tmp, "cpu16asm")
        run(["g++", "-std=c++17", "-O2", "-I" + TOOLS, "-o", gen, os.path.join(SRC, "gen_isa_js.cpp")])
        run(["g++", "-std=c++17", "-O2", "-I" + TOOLS, "-o", asm, os.path.join(TOOLS, "cpu16asm.cpp")])
        isa_js = run([gen])

        # the demo is assembled by the real cpu16asm: an error in demo.asm will stop the page build
        run([asm, os.path.join(SRC, "demo.asm"), "-o", os.path.join(tmp, "demo.bin")])

    # The demo is embedded as source text: in the browser it is assembled by the JS assembler.
    demo_src = open(os.path.join(SRC, "demo.asm"), encoding="utf-8").read()
    # stdlib.asm is embedded as a virtual file for .include in the browser
    stdlib_src = open(os.path.join(LIB, "stdlib.asm"), encoding="utf-8").read()
    demo = {"source": demo_src, "stdlib": stdlib_src}
    rom = list(open(os.path.join(SRC, "rom.bin"), "rb").read())
    glyphs = box_glyphs(isa_js)
    tpl = open(os.path.join(SRC, "debugger.template.html"), encoding="utf-8").read()
    engine = open(os.path.join(SRC, "engine.js"), encoding="utf-8").read()
    assembler = open(os.path.join(SRC, "assembler.js"), encoding="utf-8").read()
    filesystem = open(os.path.join(SRC, "fs16.js"), encoding="utf-8").read()
    filesystem_ui = open(os.path.join(SRC, "fs16_ui.js"), encoding="utf-8").read()
    for marker in ("/*__ISA__*/", "/*__ENGINE__*/", "/*__ASSEMBLER__*/", "/*__FS16__*/", "__DEMO__", "__GLYPHS__", "__ROM_BYTES__"):
        if tpl.count(marker) != 1:
            sys.exit(f"build.py: marker {marker} must occur exactly once in the template")
    html = (tpl.replace("/*__ISA__*/", isa_js.strip())
               .replace("/*__ENGINE__*/", engine.strip())
               .replace("/*__ASSEMBLER__*/", assembler.strip())
               .replace("/*__FS16__*/", filesystem.strip() + "\n" + filesystem_ui.strip())
               .replace("__DEMO__", json.dumps(demo, separators=(",", ":")))
               .replace("__ROM_BYTES__", json.dumps(rom, separators=(",", ":")))
               .replace("__GLYPHS__", json.dumps(glyphs, separators=(",", ":"))))
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(html)
    print(f"build.py: wrote {out_path} ({len(html)} bytes)")


if __name__ == "__main__":
    main()
