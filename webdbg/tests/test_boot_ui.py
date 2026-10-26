#!/usr/bin/env python3
"""End-to-end Boot button regression, no C++ assembler required.

Run: python tests/test_boot_ui.py [path/to/system.img]
The bundled HTML is loaded without external Google Fonts (for offline CI).
"""
from pathlib import Path
import re
import sys
from playwright.sync_api import sync_playwright

BASE = Path(__file__).resolve().parent.parent
SYSTEM = Path(sys.argv[1]) if len(sys.argv) > 1 else BASE.parent / "disk.img" / "system.img"
html = (BASE / "cpu16_debugger.html").read_text(encoding="utf-8")
html = re.sub(r'<link[^>]*(?:fonts\.googleapis|fonts\.gstatic)[^>]*>', "", html)

with sync_playwright() as p:
    try:
        browser = p.chromium.launch()
    except Exception:
        browser = p.chromium.launch(executable_path="/usr/bin/chromium", args=["--no-sandbox"])
    page = browser.new_page(viewport={"width": 1400, "height": 860})
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.set_content(html, wait_until="domcontentloaded", timeout=15000)
    assert page.locator("#b-boot").count() == 1
    assert page.inner_text("#st-ip") == "$0000"
    page.click("#b-boot")
    page.wait_for_function('document.querySelector("#conout").textContent.includes("ROM: no disk 0")')
    page.wait_for_function('!ui.running')
    assert page.inner_text("#msg") == "ROM: no disk 0"
    assert page.evaluate("engine.steps > 0"), "Boot must execute ROM even if disk 0 is absent"
    assert page.evaluate("ui.binName") == "rom.bin"
    assert page.get_attribute("#tab-con", "aria-selected") == "true"

    def attach(data, name):
        page.click("#tab-disks")
        if page.locator('[data-act="detach"][data-n="0"]').count():
            page.locator('[data-act="detach"][data-n="0"]').click()
        page.locator('[data-act="attach"][data-n="0"]').click()
        page.set_input_files("#f-disk", {"name": name, "mimeType": "application/octet-stream", "buffer": data})
        page.wait_for_function("engine.getDiskImage(0) !== null")

    attach(bytes(1024), "short.img")
    page.click("#b-boot")
    page.wait_for_function('document.querySelector("#conout").textContent.includes("ROM: disk 0 read error")')
    page.wait_for_function('!ui.running')
    assert page.inner_text("#msg") == "ROM: disk 0 read error"
    attach(bytes(2048), "blank.img")
    page.click("#b-boot")
    page.wait_for_function('document.querySelector("#conout").textContent.includes("ROM: disk 0 is not bootable")')
    page.wait_for_function('!ui.running')
    assert page.inner_text("#msg") == "ROM: disk 0 is not bootable"

    attach(SYSTEM.read_bytes(), "system.img")
    page.click("#b-boot")
    page.wait_for_function('document.querySelector("#conout").textContent.includes("A:/> ")', timeout=15000)
    assert "CPU16 monitor" in page.inner_text("#conout")
    assert page.evaluate("engine.getMmu().enabled")
    assert page.evaluate("ui.binName") == "rom.bin"
    assert page.evaluate("engine.getDisks()[0] !== null")
    page.click("#b-pause")
    page.wait_for_function("!ui.running")
    page.click("#b-reset")
    assert page.evaluate("engine.getRegs()[14]") == 0
    assert page.evaluate("engine.mem[0] === BOOT_ROM[0]")
    assert page.evaluate("engine.getDiskImage(0) !== null"), "Reset must preserve attached disk"
    page.click("#b-step")
    assert page.evaluate("engine.getRegs()[13]") == 0xE000, "First ROM instruction sets SP"
    page.click("#tab-disks")
    page.locator('[data-act="detach"][data-n="0"]').click()
    page.click("#b-boot")
    page.wait_for_function('document.querySelector("#conout").textContent.includes("ROM: no disk 0")')
    page.wait_for_function('!ui.running')
    assert page.inner_text("#msg") == "ROM: no disk 0"
    assert not page.evaluate("engine.getMmu().enabled"), "Boot must reset MMU even after monitor ran"
    assert not errors, "Page JS errors: " + "; ".join(errors)
    print("OK: Boot executes ROM on missing/short/blank disks, shell boot, Pause/Reset/Step")
    browser.close()
