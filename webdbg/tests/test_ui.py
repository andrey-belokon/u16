#!/usr/bin/env python3
# test_ui.py - debugger interface scenarios in headless Chromium (Playwright).
#
# Run: python3 test_ui.py path/to/cpu16asm [cpu16_debugger.html]
# Needs the playwright package with Chromium installed.

import io
import os
import subprocess
import zipfile
import sys
import tempfile

from playwright.sync_api import sync_playwright

ASM = sys.argv[1] if len(sys.argv) > 1 else "cpu16asm"
HTML = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else
                       os.path.join(os.path.dirname(__file__), "..", "cpu16_debugger.html"))
passed = failed = 0


def check(cond, what, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL:", what, detail)


def assemble(tmp, name, source):
    src = os.path.join(tmp, name + ".asm")
    with open(src, "w") as f:
        f.write(source)
    subprocess.run([ASM, src, "-o", os.path.join(tmp, name + ".bin"), "-m", os.path.join(tmp, name + ".map")],
                   check=True, capture_output=True)
    return os.path.join(tmp, name + ".bin"), os.path.join(tmp, name + ".map")


with tempfile.TemporaryDirectory() as tmp, sync_playwright() as p:
    loop_bin, loop_map = assemble(tmp, "loop", "start:  INC R0\n        JMP start\n")
    b = p.chromium.launch()
    pg = b.new_page(viewport={"width": 1400, "height": 860})
    errors = []
    pg.on("pageerror", lambda e: errors.append(str(e)))
    pg.goto("file://" + HTML)
    pg.wait_for_timeout(200)

    ip = lambda: pg.inner_text("#st-ip")
    msg = lambda: pg.inner_text("#msg")
    con = lambda: pg.inner_text("#conout")
    reg = lambda n: pg.inner_text(f"[data-reg='{n}']")

    check(pg.inner_text("#engine-badge") == "JS engine" and ip() == "$0000", "starts with the JS engine at $0000")
    check(pg.get_attribute("#tab-src", "aria-selected") == "true" and pg.inner_text("#src-state") == "demo.asm",
          "opens on the Source tab with the demo built")

    # the demo: run to WFI
    pg.keyboard.press("F9"); pg.wait_for_timeout(300)
    check(pg.inner_text("#st-state") == "Waiting (WFI)" and con() == "Hello 16bit world!\n", "F9 runs the demo to WFI")
    # an interrupt
    pg.click("#irqs button >> nth=0")
    check("pending" in pg.get_attribute("#irqs button >> nth=0", "class") and "IRQ0 requested" in msg(), "IRQ0 becomes pending")
    pg.keyboard.press("F7")
    check(ip() == "$003A" and "IRQ0 taken" in msg(), "F7 enters the IRQ0 handler")
    check("pending" not in pg.get_attribute("#irqs button >> nth=0", "class"), "IRQ0 no longer pending")
    pg.keyboard.press("F9"); pg.wait_for_timeout(300)
    check(con() == "Hello 16bit world!\nIRQ0\n", "handler prints IRQ0")

    # reset; then the disassembler tab
    pg.click("#tab-dis")
    pg.keyboard.press("Control+F2"); pg.wait_for_timeout(100)
    check(ip() == "$0000" and con() == "" and pg.inner_text("#st-steps") == "0", "Ctrl+F2 resets and reloads")
    # a step over CALL
    pg.keyboard.press("F7"); pg.keyboard.press("F7")
    check(ip() == "$0008", "two steps reach CALL")
    pg.keyboard.press("F8"); pg.wait_for_timeout(200)
    check(ip() == "$000C" and con() == "Hello 16bit world!\n", "F8 steps over CALL print")
    # to the cursor and a breakpoint in a loop
    pg.click(".drow[data-addr='20'] .addr")
    pg.keyboard.press("F4"); pg.wait_for_timeout(200)
    check(ip() == "$0014", "F4 runs to cursor (sum)")
    pg.keyboard.press("F5")
    pg.keyboard.press("F9"); pg.wait_for_timeout(200)
    check(ip() == "$0014" and "Breakpoint at $0014" in msg() and reg(4) == "0004", "breakpoint stops the next loop iteration")
    pg.keyboard.press("F5")
    # editing a register and continuing
    pg.dblclick("[data-reg='4']"); pg.keyboard.press("Control+a"); pg.keyboard.type("1"); pg.keyboard.press("Enter")
    pg.keyboard.press("F9"); pg.wait_for_timeout(300)
    check(pg.inner_text("#st-state") == "Waiting (WFI)" and reg(2) == "001E", "execution continues after editing R4 (sum 10+20)")

    # ERRCMD: the word $3800 (a reserved opcode) at address 0
    pg.keyboard.press("Control+F2"); pg.wait_for_timeout(100)
    pg.dblclick(".b[data-addr='0']"); pg.keyboard.type("00"); pg.keyboard.press("Enter")
    pg.dblclick(".b[data-addr='1']"); pg.keyboard.type("38"); pg.keyboard.press("Enter")
    row0 = pg.inner_text(".drow[data-addr='0']")
    check(".word" in row0 and "reserved opcode" in row0, "invalid word shown as .word with the reason")
    pg.keyboard.press("F7")
    check("ERRCMD at $0000" in msg() and ip() == "$0000", "step reports ERRCMD and jumps through vector 8")

    # its own program: an infinite loop, pause
    pg.set_input_files("#f-bin", loop_bin); pg.wait_for_timeout(200)
    check("Loaded loop.bin: 4 bytes at $0000" in msg(), "loads a .bin")
    pg.set_input_files("#f-map", loop_map); pg.wait_for_timeout(200)
    check("Loaded 1 label from" in msg() and pg.locator(".lrow").first.inner_text() == "start:", "loads a .map and shows labels")
    # a memory map with local labels, alignment and a constant: the labels "object@name", the constants
    _, rich_map = assemble(tmp, "rich", ".equ PORT, $FF20\nmain: NOP\n@lp: JMP @lp\nmsg: .byte 1\ntbl: .word 2\nmain.end:\n")
    pg.set_input_files("#f-map", rich_map); pg.wait_for_timeout(200)
    syms = pg.evaluate("[...ui.symbols].map(([a, n]) => a + ':' + n)")
    consts = pg.evaluate("[...ui.constants].map(([a, n]) => a + ':' + n)")
    check("Loaded 5 labels from" in msg() and syms == ["0:main", "2:main@lp", "4:msg", "6:tbl", "8:main.end"] and consts == ["65312:PORT"],
          f"loads labels and constants from a memory map: {syms} {consts}")
    pg.set_input_files("#f-map", loop_map); pg.wait_for_timeout(200)
    pg.keyboard.press("F9"); pg.wait_for_timeout(400)
    check(pg.inner_text("#st-state") == "Running", "infinite loop keeps running")
    pg.keyboard.press("Escape"); pg.wait_for_timeout(200)
    steps = int(pg.inner_text("#st-steps"))
    check(pg.inner_text("#st-state") == "Stopped" and "Paused" in msg() and steps > 10000,
          f"Esc pauses ({steps} steps)")
    # IRQ during execution: IF=0, the request stays pending
    pg.keyboard.press("F9"); pg.wait_for_timeout(100)
    pg.click("#irqs button >> nth=2"); pg.wait_for_timeout(100)
    pg.keyboard.press("Escape"); pg.wait_for_timeout(200)
    check("pending" in pg.get_attribute("#irqs button >> nth=2", "class"), "IRQ raised while running stays pending with IF=0")

    # loading at another address
    pg.fill("#loadat", "$0100")
    pg.set_input_files("#f-bin", loop_bin); pg.wait_for_timeout(200)
    check("at $0100" in msg() and pg.get_attribute(".drow >> nth=0", "data-addr") == "256", "loads at $0100")
    # scrolling up through zero
    pg.click("#b-reset"); pg.wait_for_timeout(100)
    pg.fill("#goto-dis", "$0000"); pg.press("#goto-dis", "Enter")
    pg.mouse.move(400, 300); pg.mouse.wheel(0, -40); pg.wait_for_timeout(100)
    check(pg.get_attribute(".drow >> nth=0", "data-addr") == str(0xFFFE), "wheel scrolls above $0000 to $FFFE")

    # ---------------- the source ----------------
    demo_src = open(os.path.join(os.path.dirname(__file__), "..", "src", "demo.asm"), encoding="utf-8").read()
    pg.click("#tab-src")
    check("running loop.bin" in pg.inner_text("#src-state"), "source tab says a .bin is running instead of the source")
    pg.click("#b-demo"); pg.wait_for_timeout(150)
    check(pg.get_attribute("#tab-src", "aria-selected") == "true" and pg.inner_text("#src-state") == "demo.asm" and ip() == "$0000",
          "Demo rebuilds the demo program and shows the source")
    cur_line = lambda: pg.evaluate("(document.querySelector('.gl.cur') || {}).textContent || ''")
    caret_line = lambda: pg.evaluate("document.getElementById('code').value.slice(0, document.getElementById('code').selectionStart).split('\\n').length")
    # a breakpoint in the editor field (line 29: JMP @next)
    pg.click(".gl[data-line='29']")
    check("Breakpoint set at line 29" in msg(), "gutter click sets a source breakpoint")
    pg.keyboard.press("F9"); pg.wait_for_timeout(200)
    check(ip() == "$0032" and cur_line() == "29" and pg.locator(".ln.cur").count() == 1, "runs to the source breakpoint and marks the line")
    pg.click(".gl[data-line='29']")
    # to the line under the cursor (line 17: ST [result], R2)
    pg.evaluate("""() => { const t = document.getElementById('code'); const l = t.value.split(String.fromCharCode(10));
                   let p = 0; for (let i = 0; i < 16; i++) p += l[i].length + 1; t.focus(); t.setSelectionRange(p, p); }""")
    pg.keyboard.press("F4"); pg.wait_for_timeout(200)
    check(cur_line() == "17" and con().startswith("Hello 16bit world!"), "F4 runs to the caret line")
    # editing and reassembling with the breakpoint carried over by line number
    pg.click(".gl[data-line='26']")
    pg.fill("#code", demo_src + "        NOP\n")
    check("(modified" in pg.inner_text("#src-state"), "editing marks the source modified")
    pg.keyboard.press("Control+b"); pg.wait_for_timeout(150)
    check("Build succeeded" in msg() and pg.inner_text("#src-state") == "demo.asm", "Ctrl+B rebuilds")
    check(pg.locator(".gl.bp").count() == 1 and pg.get_attribute(".gl.bp", "data-line") == "26", "breakpoint stays on its line after rebuild")
    pg.click(".gl[data-line='26']")
    # an assembly error
    pg.fill("#code", demo_src + "        BAD R0\n")
    pg.keyboard.press("Control+b"); pg.wait_for_timeout(150)
    bad_line = len(demo_src.split("\n"))
    check(pg.get_attribute("#tab-build", "aria-selected") == "true" and f"demo.asm:{bad_line}" in pg.inner_text("#buildout")
          and "unknown mnemonic 'BAD'" in pg.inner_text("#buildout"), "build error is listed with file and line")
    check(pg.locator(f".gl.err[data-line='{bad_line}']").count() == 1 and "has errors" in pg.inner_text("#src-state"), "error line is marked")
    pg.click("#buildout .diag")
    check(caret_line() == bad_line, "clicking an error moves the caret to its line")
    # an error in a line under .line: the .line location and the text line are shown; the text line is marked
    pg.fill("#code", "        NOP\n        .line \"kernel.b16\", 40\n        NOP\n        BAD R0\n")
    pg.keyboard.press("Control+b"); pg.wait_for_timeout(150)
    check("kernel.b16:40 (demo.asm:4): unknown mnemonic" in pg.inner_text("#buildout") and pg.locator(".gl.err[data-line='4']").count() == 1,
          ".line: error shows the B16 place and marks the assembler line")
    pg.click("#buildout .diag")
    check(caret_line() == 4, ".line: clicking the error moves the caret to the assembler line")
    # restoring after a reload
    pg.fill("#code", demo_src + "; kept between sessions\n")
    pg.wait_for_timeout(600)
    pg.reload(); pg.wait_for_timeout(300)
    check("kept between sessions" in pg.input_value("#code") and "Restored and built" in msg(), "source is restored after reload")
    # downloading a .zip (outside claude.ai - an ordinary browser download) and comparing with cpu16asm
    with pg.expect_download() as dl:
        pg.click("#b-zip")
    data = open(dl.value.path(), "rb").read()
    z = zipfile.ZipFile(io.BytesIO(data))
    names = sorted(z.namelist())
    check(names == ["demo.asm", "demo.bin", "demo.lst", "demo.map"], f"zip contains asm, bin, lst, map: {names}")
    src_path = os.path.join(tmp, "demo.asm")
    with open(src_path, "wb") as f:
        f.write(z.read("demo.asm"))
    # from the tmp directory: the source name in the memory map is "demo.asm", as in the debugger
    subprocess.run([os.path.abspath(ASM), "demo.asm", "-o", "ref.bin", "-l", "ref.lst", "-m", "ref.map"],
                   check=True, capture_output=True, cwd=tmp)
    check(z.read("demo.bin") == open(os.path.join(tmp, "ref.bin"), "rb").read(), "zipped .bin equals cpu16asm output")
    check(z.read("demo.lst") == open(os.path.join(tmp, "ref.lst"), "rb").read() and
          z.read("demo.map") == open(os.path.join(tmp, "ref.map"), "rb").read(), "zipped .lst and .map equal cpu16asm output")
    check(z.testzip() is None, "zip archive is valid")
    # opening an .asm
    loop_src = os.path.join(tmp, "loop.asm")
    pg.set_input_files("#f-asm", loop_src); pg.wait_for_timeout(200)
    check(pg.inner_text("#src-state") == "loop.asm" and "Opened and built loop.asm" in msg(), "Open .asm loads and builds the file")
    pg.click("#b-demo"); pg.wait_for_timeout(100)
    check("Click Demo again" in msg(), "Demo asks for a second click before replacing other source")
    pg.click("#b-demo"); pg.wait_for_timeout(150)
    check(pg.inner_text("#src-state") == "demo.asm", "second click restores the demo")

    # ---------------- console input (port $FF20) ----------------
    echo_src = ("loop:   LDZX    R1, [$FF20]\n        CMP     R1, #0\n        JZ      loop\n"
                "        STB     [$FF20], R1\n        CMP     R1, #'.'\n        JNZ     loop\n        WFI\n")
    pg.fill("#code", echo_src); pg.keyboard.press("Control+b"); pg.wait_for_timeout(150)
    pending = lambda: pg.evaluate("engine.pendingInput")
    pg.click("#conout"); pg.keyboard.type("Hi"); pg.wait_for_timeout(50)
    check(pending() == 2 and "2 keys waiting" in pg.inner_text("#con-hint"), "typing in the console queues input while stopped")
    pg.fill("#goto-mem", "$FF20")
    pg.press("#goto-mem", "Enter"); pg.wait_for_timeout(50)
    check(pending() == 2, "memory view at $FF20 does not consume input")
    pg.click("#conout")
    pg.keyboard.press("F9"); pg.wait_for_timeout(300)
    check(con() == "Hi" and pg.inner_text("#st-state") == "Running" and pending() == 0, "program reads the queued input")
    pg.evaluate("""() => { const c = document.getElementById('conout');
        for (const k of ['П','р','и','в','е','т',' ']) c.dispatchEvent(new KeyboardEvent('keydown', {key: k, bubbles: true, cancelable: true})); }""")
    pg.evaluate("""() => { const dt = new DataTransfer(); dt.setData('text', 'мир.');
        document.getElementById('conout').dispatchEvent(new ClipboardEvent('paste', {clipboardData: dt, bubbles: true, cancelable: true})); }""")
    pg.wait_for_timeout(300)
    check(con() == "HiПривет мир." and pg.inner_text("#st-state") == "Waiting (WFI)", "input typed while running, including UTF-8 and paste")
    pg.click("#conout"); pg.keyboard.type("zz")
    pg.keyboard.press("Control+F2"); pg.wait_for_timeout(100)
    check(pending() == 0 and con() == "", "Reset clears the input queue")
    pg.click("#conout"); pg.keyboard.press("Enter")
    check(pending() == 1, "Enter sends a newline byte")
    pg.keyboard.press("Control+Enter")
    check(pg.evaluate("engine.dev.keys.map(k => [k.ch, k.scan])") == [[10, 0x1C], [0, 0x1C]],
          "Ctrl+Enter in the console: symbol 0, scancode $1C")
    pg.keyboard.press("Control+BracketLeft"); pg.keyboard.press("Control+BracketRight")
    check(pg.evaluate("engine.dev.keys.slice(2).map(k => [k.ch, k.scan, k.mods])") == [[0, 0x1A, 2], [0, 0x1B, 2]],
          "Ctrl+[ and Ctrl+] in the console: scancodes $1A, $1B with Ctrl (session switching)")
    pg.keyboard.press("Control+KeyK")
    check(len(pg.evaluate("engine.dev.keys")) == 4, "other Ctrl combinations in the console stay with the browser")

    # ---------------- the built-in library through .include ----------------
    pg.fill("#code", "        LD      SP, #$FF00\n        LD      R0, #1\n        LD      R1, #0\n"
                     "        LD      R2, #$5678\n        LD      R3, #1\n        CALL    add32\n        CALL    print_u32\n"
                     "        WFI\n        .include \"stdlib.asm\"\n")
    pg.keyboard.press("Control+b"); pg.wait_for_timeout(200)
    pg.keyboard.press("F9"); pg.wait_for_timeout(300)
    check(con() == str(0x0001 + 0x15678), ".include \"stdlib.asm\" works in the browser")
    # the program skeleton from the "Introduction": a prologue, number input, a timer on IRQ0
    skel = open(os.path.join(os.path.dirname(__file__), "..", "..", "cpu16", "docs", "skeleton.asm"), encoding="utf-8").read()
    pg.fill("#code", skel); pg.keyboard.press("Control+b"); pg.wait_for_timeout(200)
    pg.keyboard.press("F9"); pg.wait_for_timeout(200)
    pg.click("#conout"); pg.keyboard.type("250"); pg.keyboard.press("Enter"); pg.wait_for_timeout(300)
    check(con() == "CPU16 skeleton\nEnter a number: 250\nhex: $00FA, square: 62500\nWaiting for 3 timer interrupts (IRQ 0)",
          "skeleton: prologue, read_dec, mulu16", repr(con()))
    for _ in range(3):
        pg.click("#irqs button >> nth=0"); pg.keyboard.press("F9"); pg.wait_for_timeout(200)
    check(con().endswith("(IRQ 0)... done\n") and pg.inner_text("#st-state") == "Waiting (WFI)", "skeleton: three IRQ0 ticks, then halt")
    pg.fill("#code", '        .include "other.asm"\n'); pg.keyboard.press("Control+b"); pg.wait_for_timeout(150)
    check("cannot open file 'other.asm'" in pg.inner_text("#buildout"), "other includes report a missing file")

    # ---------------- panel splitters ----------------
    box = lambda sel: pg.locator(sel).bounding_box()

    def drag(sel, dx, dy):
        bb = box(sel)
        x, y = bb["x"] + bb["width"] / 2, bb["y"] + bb["height"] / 2
        pg.mouse.move(x, y); pg.mouse.down(); pg.mouse.move(x + dx, y + dy); pg.mouse.up(); pg.wait_for_timeout(150)
    for sel in ("#sp-v", "#sp-h1", "#sp-h2"):
        pg.locator(sel).dblclick()
    w0, c0, rows0 = box("#regs")["width"], box("#con")["height"], pg.locator(".mrow").count()
    drag("#sp-v", -150, 0)
    drag("#sp-h1", 0, -100)
    drag("#sp-h2", 0, -80)
    check(abs(box("#regs")["width"] - (w0 + 150)) < 3 and abs(box("#con")["height"] - (c0 + 80)) < 3, "splitters resize the panels")
    check(pg.locator(".mrow").count() > rows0, "panels re-render to the new size")
    sizes = (box("#regs")["width"], box("#con")["height"])
    pg.reload(); pg.wait_for_timeout(300)
    check(all(abs(a - b) < 2 for a, b in zip(sizes, (box("#regs")["width"], box("#con")["height"]))), "panel sizes persist after reload")
    pg.locator("#sp-v").dblclick()
    check(abs(box("#regs")["width"] - 340) < 2, "double-click resets a splitter")

    # ---------------- display, keyboard, timer ----------------
    for sel in ("#sp-v", "#sp-h1", "#sp-h2"):
        pg.locator(sel).dblclick()
    keys_src = """        LD      SP, #$FF00
@key:   LD      R4, [$FF20]             ; word: character and scancode
        TEST    R4, R4
        JZ      @key
        LD      R0, R4
        CALL    print_hex16
        CALL    newline
        CMP     R4, #$011B              ; Esc
        JNZ     @key
        WFI
        .include "stdlib.asm"
"""
    pg.fill("#code", keys_src); pg.keyboard.press("Control+b"); pg.wait_for_timeout(150)
    # a separate window
    with pg.expect_popup() as pop:
        pg.click("#b-display")
    win = pop.value
    win.wait_for_load_state(); win.wait_for_timeout(300)
    check(pg.get_attribute("#b-display", "aria-pressed") == "true" and int(win.evaluate("document.body.dataset.frames || 0")) >= 1,
          "Display opens a separate window that receives frames")
    pg.keyboard.press("F9"); pg.wait_for_timeout(200)
    win.click("canvas")
    for k in ("ArrowUp", "a", "F5", "Escape"):
        win.keyboard.press(k); win.wait_for_timeout(120)
    pg.wait_for_timeout(300)
    check(con().split() == ["4800", "1E61", "3F00", "011B"], f"keys from the window arrive with scancodes: {con().split()}")
    check(pg.inner_text("#st-state") == "Waiting (WFI)", "program ends after Esc")
    win.close(); pg.wait_for_timeout(700)
    check(pg.get_attribute("#b-display", "aria-pressed") == "false", "closing the window turns the display off")
    # a spare panel when windows are forbidden
    pg.evaluate("window.__open = window.open; window.open = () => null")
    pg.keyboard.press("Control+F2"); pg.wait_for_timeout(100)
    pg.click("#b-display"); pg.wait_for_timeout(300)
    check(pg.is_visible("#disp") and "shown as a panel" in msg(), "falls back to the display panel when windows are blocked")
    pg.keyboard.press("F9"); pg.wait_for_timeout(100)    # focus in the display panel: F9 goes to the program, the debugger does not run
    check(pg.inner_text("#st-state") == "Stopped" and pending() == 1, "with the display focused, F9 goes to the program")
    engine_keys = pg.evaluate("engine.dev.keys.map(k => k.scan)")
    pg.evaluate("engine.clearInput()")
    pg.click("#b-run"); pg.wait_for_timeout(150)
    pg.click("#disp-body")
    pg.keyboard.press("F7")                       # with the display focused F7 goes to the program
    pg.evaluate("""() => document.getElementById('disp-body').dispatchEvent(
        new KeyboardEvent('keydown', { key: 'ы', code: 'KeyS', bubbles: true, cancelable: true }))""")
    pg.wait_for_timeout(250)                      # the program handles the key presses in the next frame
    pg.keyboard.press("Pause"); pg.wait_for_timeout(300)
    check(con().split() == ["4100", "1FFB"], f"panel keys: F7 and Cyrillic go to the program: {con().split()}")
    check(pg.inner_text("#st-state") == "Stopped" and "Paused" in msg(), "Pause/Break pauses from the display")
    pg.click("#b-disp-close"); pg.wait_for_timeout(100)
    check(not pg.is_visible("#disp"), "the panel closes")
    pg.evaluate("window.open = window.__open")
    # button highlight when the program draws on a closed display
    pg.fill("#code", "        LD R0, #0\n        STB [$FF22], R0\n        LD R0, #'X'\n        STB [$FF20], R0\n        WFI\n")
    pg.keyboard.press("Control+b"); pg.wait_for_timeout(150)
    pg.keyboard.press("F9"); pg.wait_for_timeout(300)
    check("attention" in pg.get_attribute("#b-display", "class") and con() == "", "drawing on a closed display highlights the button")
    # the real-time timer: 25 ticks of 20 ms
    timer_src = """        LD      SP, #$FF00
        LD      R0, #tick
        ST      [$FF00], R0
        LD      R0, #1
        STB     [$FF29], R0             ; timer
        STI
@wait:  WFI
        LD      R0, [ticks]
        CMP     R0, #25
        JB      @wait
        LD      R0, #0
        STB     [$FF29], R0             ; stop: timer off, CLI, WFI
        CLI
        LD      R0, [ticks]
        CALL    print_u16
        WFI
tick:   PUSH    R0
        LD      R0, [ticks]
        INC     R0
        ST      [ticks], R0
        POP     R0
        IRET
ticks:  .word   0
        .include "stdlib.asm"
"""
    pg.fill("#code", timer_src); pg.keyboard.press("Control+b"); pg.wait_for_timeout(150)
    t0 = pg.evaluate("performance.now()")
    pg.keyboard.press("F9")
    pg.wait_for_function("document.getElementById('st-state').textContent === 'Waiting (WFI)'", timeout=5000)
    dt = pg.evaluate("performance.now()") - t0
    check(con() == "25" and 450 <= dt <= 2500, f"timer: 25 ticks take about half a second of real time ({dt:.0f} ms)")

    # ---------------- disks: creating, attaching a file, exchange from a program, RO, delay, downloading
    pg.click("#tab-disks")
    check(pg.is_visible("#disks-body") and pg.input_value("#dk-delay") == "1" and pg.evaluate("engine.diskDelayMs") == 1,
          "Disks tab: 1 ms per sector by default")
    pg.click("[data-act='new'][data-n='0']")
    pg.fill("#dk-sectors", "300"); pg.keyboard.press("Enter"); pg.wait_for_timeout(50)
    disks = pg.evaluate("engine.getDisks()")
    check(disks[0] and disks[0]["sectors"] == 300 and disks[0]["label"] == "disk0.img" and "disk0.img" in pg.inner_text("#dk-rows"),
          "New… creates an empty image of the given size")
    img_path = os.path.join(tmp, "data.img")
    with open(img_path, "wb") as f:
        f.write(bytes([0x5A] * 512 + [0xA5] * 512))
    with pg.expect_file_chooser() as fc:
        pg.click("[data-act='attach'][data-n='1']")
    fc.value.set_files(img_path); pg.wait_for_timeout(150)
    disks = pg.evaluate("engine.getDisks()")
    check(disks[1] and disks[1]["sectors"] == 2 and disks[1]["label"] == "data.img" and pg.evaluate("engine.rd8(0xFF3B)") == 3,
          "Attach file… connects an image; DRIVES changes at once")
    bad_path = os.path.join(tmp, "bad.img")
    with open(bad_path, "wb") as f:
        f.write(b"x" * 100)
    with pg.expect_file_chooser() as fc:
        pg.click("[data-act='attach'][data-n='2']")
    fc.value.set_files(bad_path); pg.wait_for_timeout(150)
    check("not a multiple of 512" in msg() and not pg.evaluate("engine.getDisks()[2]"), "an image whose size is not a multiple of 512 is refused")
    disk_src = """        .equ    DK_CMD, $FF30
        .equ    DK_STATUS, $FF30
        .equ    DK_ERROR, $FF31
        .equ    DK_DISK, $FF32
        .equ    DK_ADDR, $FF34
        .equ    DK_LBA_LO, $FF36
        .equ    DK_LBA_HI, $FF38
        .equ    DK_CTRL, $FF3A
        LD      SP, #$FF00
        LD      R0, #disk_irq
        ST      [$FF00 + 2*1], R0       ; IRQ1 vector
        LD      R0, #1
        STB     [DK_CTRL], R0           ; IEN
        ; disk 1, sector 1 -> buf (polling)
        LD      R0, #1 << 8 | 1
        ST      [DK_DISK], R0
        LD      R0, #buf
        ST      [DK_ADDR], R0
        LD      R0, #1
        ST      [DK_LBA_LO], R0
        LD      R0, #0
        ST      [DK_LBA_HI], R0
        LD      R1, #1
        CALL    run_wait
        LDZX    R0, [buf]
        CALL    print_hex8
        CALL    newline
        ; $1000.. -> disk 0, sectors 0..99 (wait on IRQ1, 100 ms at 1 ms per sector)
        LD      R0, #100 << 8 | 0
        ST      [DK_DISK], R0
        LD      R0, #$1000
        ST      [DK_ADDR], R0
        LD      R0, #0
        ST      [DK_LBA_LO], R0
        LD      R1, #2
        CALL    run_wait
        LDZX    R0, [DK_ERROR]
        CALL    print_u16
        CALL    newline
        CLI
        WFI
; R1 = command; wait on IRQ1 without a race (sect. 6.5)
run_wait:
        CLI
        LD      R0, #0
        STB     [dk_done], R0
        STB     [DK_CMD], R1
@wait:  LDZX    R0, [dk_done]
        CMP     R0, #0
        JNZ     @done
        WFI
        STI
        CLI
        JMP     @wait
@done:  STI
        RET
disk_irq:
        PUSH    R0
        LD      R0, #0
        STB     [DK_CMD], R0            ; ACK
        LD      R0, #1
        STB     [dk_done], R0
        POP     R0
        IRET
dk_done: .byte  0
        .include "stdlib.asm"
        .org    $0F00
buf:    .space  512
"""
    pg.fill("#code", disk_src); pg.keyboard.press("Control+b"); pg.wait_for_timeout(150)
    pg.click("#tab-disks")
    t0 = pg.evaluate("performance.now()")
    pg.keyboard.press("F9")
    try:
        pg.wait_for_function("document.getElementById('st-state').textContent === 'Running (WFI, disk)'", timeout=3000)
        saw_disk = True
    except Exception:
        saw_disk = False
    pg.wait_for_function("document.getElementById('st-state').textContent === 'Waiting (WFI)'", timeout=8000)
    dt = pg.evaluate("performance.now()") - t0
    img0 = pg.evaluate("Array.from(engine.getDiskImage(0).subarray(0, 1024))")
    check(con() == "A5\n0\n" and img0[0] == 0xA5 and img0[600] == 0, f"program reads disk 1 and writes disk 0 through DMA ({con()!r})")
    check(saw_disk and dt >= 95, f"status shows Running (WFI, disk); 100 sectors take about 100 ms ({dt:.0f} ms)")
    check("modified" in pg.inner_text("#dk-rows"), "a written image is marked as modified")
    with pg.expect_download() as dl:
        pg.click("[data-act='download'][data-n='0']")
    data = open(dl.value.path(), "rb").read()
    check(dl.value.suggested_filename == "disk0.img" and len(data) == 300 * 512 and data[0] == 0xA5 and "modified" not in pg.inner_text("#dk-rows"),
          "Download saves the image and clears the mark")
    # write protection: the command ends with the READONLY error (5)
    pg.click("[data-act='ro'][data-n='0']")
    pg.click("#b-reset"); pg.keyboard.press("F9")
    pg.wait_for_function("document.getElementById('st-state').textContent === 'Waiting (WFI)'", timeout=8000)
    check(con() == "A5\n5\n" and pg.evaluate("engine.getDisks()[0].readOnly"), f"read-only checkbox: WRITE fails with READONLY ({con()!r})")
    # instant exchange and disconnecting a disk during a command
    pg.fill("#dk-delay", "0"); pg.dispatch_event("#dk-delay", "change")
    check(pg.evaluate("engine.diskDelayMs") == 0, "time per sector can be set to 0")
    pg.fill("#dk-delay", "1000"); pg.dispatch_event("#dk-delay", "change")
    pg.evaluate("engine.wr16(0xFF32, 2 << 8 | 1); engine.wr16(0xFF34, 0x1000); engine.wr8(0xFF30, 1)")
    pg.click("[data-act='detach'][data-n='1']")
    check("I/O error" in msg() and pg.evaluate("engine.getDiskRegs().error") == 6 and not pg.evaluate("engine.getDisks()[1]"),
          "detaching a disk during a command ends it with an I/O error")
    pg.fill("#dk-delay", "1"); pg.dispatch_event("#dk-delay", "change")
    pg.click("[data-act='detach'][data-n='0']")

    # ---------------- the memory manager: the panel and the physical dump
    mmu_src = """        LD      R0, #5 << 8 | 40        ; PAGE4 = 40, PAGE5 = 5
        ST      [$FF44], R0
        LD      R0, #1 << 8 | 0
        ST      [$FF40], R0
        LD      R0, #3 << 8 | 2
        ST      [$FF42], R0
        LD      R0, #7 << 8 | 6
        ST      [$FF46], R0
        LD      R0, #1
        STB     [$FF48], R0
        LD      R0, #$4321
        ST      [$8000], R0
        WFI
"""
    pg.fill("#code", mmu_src); pg.keyboard.press("Control+b"); pg.wait_for_timeout(150)
    check("off" in pg.inner_text("#mmu-body"), "MMU panel: off after reset")
    pg.keyboard.press("F9"); pg.wait_for_timeout(300)
    t = pg.inner_text("#mmu-body")
    check("PAGE4 8000" in t and "28" in t and "050000" in t and "pages map" in t, "MMU panel shows EN, PAGE4 = $28 and its physical address")
    mem_first = lambda: pg.inner_text("#mem-body .mrow .b")
    pg.fill("#goto-mem", "8000"); pg.keyboard.press("Enter"); pg.wait_for_timeout(50)
    check(mem_first() == "21", "logical dump goes through the mapping")
    pg.click("#b-memmode")
    pg.fill("#goto-mem", "050000"); pg.keyboard.press("Enter"); pg.wait_for_timeout(50)
    check(pg.inner_text("#b-memmode") == "Physical" and pg.inner_text("#mem-body .mrow .addr") == "050000" and mem_first() == "21",
          "physical dump with 21-bit addresses")
    pg.fill("#goto-mem", "008000"); pg.keyboard.press("Enter"); pg.wait_for_timeout(50)
    check(mem_first() == "00", "physical $008000 is not written while page 4 maps elsewhere")
    pg.dblclick("#mmu-body .frame[data-page='4']"); pg.keyboard.type("4"); pg.keyboard.press("Enter")
    pg.click("#b-memmode")
    pg.fill("#goto-mem", "8000"); pg.keyboard.press("Enter"); pg.wait_for_timeout(50)
    check(pg.evaluate("engine.getMmu().pages[4]") == 4 and mem_first() == "00", "PAGE edited in the panel remaps the logical dump")
    pg.fill("#goto-mem", "FF20"); pg.keyboard.press("Enter"); pg.wait_for_timeout(50)
    check(pg.inner_text("#mem-body .mrow .b:nth-child(8)") == "50", "device window in the dump shows register values (COLS = 80)")
    pg.click("#mmu-en")
    check(not pg.evaluate("engine.getMmu().enabled") and "off" in pg.inner_text("#mmu-body"), "EN toggles from the panel")

    # ---------------- the clock: the panel, editing, host time, reset, the program sees the set value
    clock_src = """        LD      R0, #1
        STB     [$FF2E], R0             ; READ
        LD      R1, [$FF2A]
        LD      R2, [$FF2C]
        WFI
"""
    pg.fill("#code", clock_src); pg.keyboard.press("Control+b"); pg.wait_for_timeout(150)
    now = lambda: pg.inner_text("#rtc-now")
    t0 = now()
    check(len(t0) == 19 and t0[2] == "." and t0[5] == "." and t0[10] == " ", f"Clock panel shows the value: {t0!r}")
    pg.dblclick("#rtc-now"); pg.keyboard.press("Control+a"); pg.keyboard.type("29.02.2027 10:00:00"); pg.keyboard.press("Enter")
    check("not a valid date and time" in msg() and now() != "29.02.2027 10:00:00", "an invalid value is not accepted")
    pg.dblclick("#rtc-now"); pg.keyboard.press("Control+a"); pg.keyboard.type("28.02.2028 23:59:58"); pg.keyboard.press("Enter")
    check(now() == "28.02.2028 23:59:58" and "Clock set" in msg(), f"double-click sets the clock ({now()!r})")
    pg.wait_for_timeout(2300)
    check(now().startswith("29.02.2028 00:00:0"), f"the clock runs while paused ({now()!r})")
    pg.keyboard.press("F9"); pg.wait_for_timeout(300)
    t = pg.inner_text("#rtc-body")
    check("29.02.2028" in t and "AM" in t and pg.evaluate("engine.getRegs()[2]") == pg.evaluate("engine.getRtc().date"),
          f"READ in the program fills TIME and DATE, the panel decodes them: {t!r}")
    pg.click("#b-reset")
    pg.wait_for_timeout(100)
    r = pg.evaluate("engine.getRtc()")
    check(r["time"] == 0 and r["date"] == 0 and not r["err"] and r["text"].startswith("29.02.2028"),
          "reset clears TIME, DATE, ERR and keeps the clock")
    pg.click("#rtc-host")
    hy = pg.evaluate("new Date().getFullYear()")
    check(now()[6:10] == str(hy) or not (2025 <= hy <= 2088), f"Host time sets the host's local time ({now()!r})")

    check(not errors, f"no JavaScript errors: {errors}")
    b.close()

print(f"{'FAILED' if failed else 'OK'}: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
