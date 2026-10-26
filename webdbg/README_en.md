# CPU16 Browser Debugger

The `webdbg/` directory is at the top level of the project, alongside `b16c/`, `cpu16/`, and `monitor/`. The build produces a single file, `webdbg/cpu16_debugger.html`, which can be opened in any browser without installation.

## Files

| File | Purpose |
| --- | --- |
| `cpu16_debugger.html` | Built debugger page |
| `build.py` | Page builder |
| `src/debugger.template.html` | UI and engine interface contract |
| `src/engine.js` | JavaScript engine (port of `tools/cpu16.cpp`), MMU, devices, disk controller, and disassembler |
| `src/fs16.js` | Revision-3 FS16 implementation: reading, writing, cluster allocation, directories, and indexes; case-insensitive filename comparisons |
| `src/fs16_ui.js` | Separate file-manager window and drag-and-drop file transfers |
| `build_local.py` | Rebuilds a standalone debugger distribution without the C++ dependencies omitted from that distribution |
| `tests/test_fs16.js`, `tests/test_fs16_ui.py` | FS16 format and Chromium UI tests |
| `src/assembler.js` | JavaScript assembler (port of `tools/cpu16asm.cpp`) for the integrated editor |
| `src/gen_isa_js.cpp` | Generates JS tables: ISA from `tools/cpu16_isa.hpp`, encoding, palette, ports, disk and MMU constants, and scan codes from `tools/cpu16_machine.hpp` |
| `src/demo.asm` | Built-in demonstration program, embedded as source and assembled in the browser |
| `tests/gen_cases.cpp` | C++ reference cases for comparing the emulator engines |
| `tests/test_engine.js` | Compares the JS engine with C++ and tests the demo |
| `tests/test_disasm.js` | Disassembler round-trip tests using `cpu16asm` |
| `tests/asm_oracle.cpp` | C++ reference for the JS assembler |
| `tests/test_assembler.js` | Compares the JS assembler with `cpu16asm` on a shared source corpus |
| `tests/test_ui.py` | Browser UI scenarios in headless Chromium |

The built page also embeds `lib/stdlib.asm` (available inside the debugger as `.include "stdlib.asm"`) and generated tables from `tools/`.

## Building

A sibling `../cpu16` directory is required for the ISA tables, assembler, and standard library:

```sh
make                          # From webdbg/; or: python3 build.py; from cpu16/: make debugger
```

If you have only `webdbg/` without `../cpu16`, run `python3 build_local.py`. This helper reuses the ISA tables and standard library embedded in the existing HTML file, while taking updated JavaScript and HTML from `src/`. Changes to the ISA tables or standard library require a full `build.py` run in the complete source tree.

Rebuild the page after changing `../cpu16/tools/cpu16_isa.hpp`, `../cpu16/tools/cpu16_machine.hpp`, `../cpu16/lib/stdlib.asm`, or any file under `src/`. The `make` target handles this automatically.

## Tests

Run from `webdbg/` (or invoke `make test-debugger` and `make test-ui` from `cpu16/`). The `make test` target includes session tests that require a generated `../monitor/dist/system.img`; first run `make dist` from `monitor/`:

```sh
make test                     # Compare engine, disassembler, and assembler with C++; test FS16
make test-ui                  # UI and file-manager scenarios (requires Playwright with Chromium)
```

The tests extract the engine from the built page, so they exercise exactly the code shipped to the browser. The comparison with C++ covers every device: all screen modes, keyboard, timer and RTC with simulated clock jumps, disk controller (immediate commands and transfers taking 1 ms per sector, level-triggered IRQ1, matching test images), MMU, and unmapped device addresses. All physical memory and disk contents are compared. Some assembler test cases are taken from `../cpu16/tests/cpu16asm_tests.cpp` and `../cpu16/docs/cpu16_asm_reference_en.md`.

## Devices and memory

Devices are specified in [*CPU16 Machine: Memory and Devices*, Revision 9](../cpu16/docs/cpu16_machine_en.md): console and timer (`$FF20–$FF29`), keyboard modifier register MODS (`$FF3C`), real-time clock (`$FF2A–$FF2E`), disk controller (`$FF30–$FF3B`), and MMU (`$FF40–$FF48`). The `$FF20–$FFFF` device window bypasses MMU translation; reads from unimplemented addresses return zero and writes are ignored.

- **Console** (bottom panel): A teletype-style output log. Click it and type to send keystrokes to the guest (character, scan code, and Shift/Ctrl/Alt state via MODS). Pasted text is sent as characters.
- **80×25 display:** Open with **Display** beside the console. The debugger first tries to open a separate window; if popups are blocked (for example, when embedded in a chat viewer), it shows a resizable panel on the right. Both offer full-screen mode via a button or double-click. While the display has focus, all keystrokes go to the guest, except Pause/Break, which pauses execution. If a program draws to a closed display, the **Display** button is highlighted.
- **Timer:** Advances in real 20 ms intervals while the program runs; it stops during pauses and single-stepping. If the guest executes `WFI` with the timer enabled, execution continues and the status bar reads `Running (WFI, timer)`. The execution speed is shown in the status bar.
- **Boot:** The **Boot** button always performs a hardware reset, loads the embedded `src/rom.bin` at `$0000`, and immediately runs the ROM, even with no disk 0 attached. The ROM checks for a disk, reads sectors 1–3, and validates the FS16 bootloader signature (`CXBT`); the loader then loads the Monitor from `0:/sys/monitor.sys`. If no disk is present, the ROM prints `ROM: no disk 0`; read failures produce `ROM: disk 0 read error`, and a missing bootloader produces `ROM: disk 0 is not bootable`. These messages are emitted **by the ROM itself** through the emulated `$FF20` port to the **Console** tab, after which it halts. The debugger also mirrors them in its status bar. Errors from later boot stages (`boot: …`) appear similarly. Boot discards the previous program because it reboots the machine, but attached disk images remain. `rom.bin` is embedded in the standalone HTML by `build.py` or `build_local.py`.
- **Disks:** Open the **Disks** tab beside the console. Each of eight slots has **Attach file…** to select a local image (size must be a multiple of 512 bytes; you can also drop a `.img` or `.dsk` file onto the page), and **New…** to create a blank disk of a specified sector count. Attached disks provide a write-protection checkbox, **Download**, and **Detach**. Disk images live in page memory; modifications persist only if you download the image, and changed images are marked `• modified`. Drives may be attached or detached while the guest runs (REMOVABLE = 1); detaching during a command terminates it with an IO error. Controls above the list show per-sector timing (default 1 ms; zero means instantaneous completion) and live disk-controller registers. Transfers progress in real time while execution runs, but pause during single-stepping or when execution is paused. If a program waits in `WFI` for a command with IEN = 1, execution continues (`Running (WFI, disk)`).
- **FS16 file manager:** Click **Files…** on an attached disk's row in **Disks**. A separate two-pane window opens: local host files selected with **Add files…**, **Add folder…**, or drag-and-drop appear on the left; the FS16 volume appears on the right. Double-click folders to enter them; use **↑ Up** to go back. Select multiple items with Ctrl/Shift. Drag a selection from right to left to download it (as ZIP), or from left to right to copy it onto the disk. Dropping onto a folder icon copies into that folder. **Copy →** and **← Download** perform the same actions without dragging. A single-file download returns the original file; multiple files or any directories are packaged as uncompressed ZIP. Files copied from a host folder retain their subdirectory structure. **New folder** creates a directory; **Delete** removes files or empty directories. On-disk names are case-insensitive (FS16, section 7.1): a file or directory differing only by letter case is rejected with `Already exists`.
  - For a blank, unformatted image, the window offers **Format FS16…** with an explicit confirmation that the entire disk contents will be erased. The disk must have at least 12 sectors, each 512 bytes; the maximum FS16 volume contains 65,535 clusters.
  - FS16 validates the signature, version, geometry, and superblock checksum. It supports direct, indirect, and double-indirect cluster references; updates the bitmap, size, free-cluster count, and CHECKSUM; sets DIRTY while writing; and encodes names using the CPU16 character set. Name collisions do not overwrite existing files: conflicting files are skipped with a message. Invalid names and unsupported characters are not silently transliterated.
  - **Safety restrictions:** Changes are blocked while the CPU is executing, the disk controller is busy, or the image has the `readOnly` or `DIRTY` flag. If a guest OS has mounted a volume read-write, unmount it cleanly first: the guest may have cached metadata. The CPU16 Monitor sets DIRTY = 1 on read-write volumes, so the file manager refuses to modify those volumes. A volume mounted read-only by the Monitor *can* be modified, but the Monitor sees the changes only after `umount` and `mount` (or a reboot). If an FS16 update fails partway through, the volume retains `DIRTY=1` to require a filesystem check. After copying, **download the complete disk image using Download on the Disks tab**, or all changes will be lost when the page closes. Browsers cannot write directly into arbitrary local directories, so files are exported through downloads.
- **Memory manager:** The **Memory manager** section in the register panel shows the EN bit (click to toggle), PAGE0..PAGE7 registers (double-click to edit), and the physical address mapped by each page.
- **Clock:** The **Clock** section in the register panel shows RTC time as `DD.MM.YYYY HH:MM:SS` (24-hour format), followed by the TIME, DATE, and ERR exchange registers and their decoded values. Double-click the displayed time to edit it in the same format. Invalid dates (for example, 29.02.2027 or a year outside 2025–2088) are rejected. **Host time** sets the computer's local time. Editing the time through the debugger changes the clock itself, not TIME/DATE/ERR, which belong to the guest program. The clock runs even while the guest is paused or single-stepped: it measures real time, not timer ticks. When the page opens, the clock initially shows the computer's local time.
- **Memory:** Switch between **Logical** and **Physical** in the memory pane. In logical mode, memory is shown as the CPU sees it through the MMU; device addresses show register values without side effects (for example, reading a keyboard register does not consume an event) and cannot be edited. Physical mode addresses all 2 MB using 21-bit addresses. **Go to** accepts an address such as `$XXXXXX` or a label (resolved to a physical address using the current mapping).

Resetting or reassembling clears the screen, keyboard queue, timer, RTC exchange registers (TIME, DATE, ERR), disk-controller registers, and MMU state (disabled, PAGE = 0). Attached disks and the running RTC itself are preserved.

## Downloading program files

**Download .zip** creates an archive containing `.asm`, `.bin`, `.lst`, and `.map` (in the `cpu16asm -m` memory-map format). **Open .map** loads a map associated with a `.bin` image, making labels and constants visible in the disassembler. In the debugger page published on claude.ai, downloads use the platform's `downloads` facility (the viewer confirms saving the file; the platform does not allow separate `.asm` and `.bin` files, hence the archive). When opened as a local file, the debugger uses the browser's normal download mechanism.

## Moving to WebAssembly

The UI communicates with the engine exclusively through the interface documented in a comment in `debugger.template.html`. A WebAssembly build of `cpu16.cpp` would need to implement the same object. It could then replace `Cpu16Engine`, and `test_engine.js`/`test_ui.py` would exercise it against the existing test scenarios. Likewise, a WebAssembly build of `cpu16asm.cpp` could replace `Cpu16Assembler`: the UI accesses only `assembleText`, `diags`, `image`/`owner`, `listing`, `memoryMap`, `symbols`, and `lineMap`.

### Disk-boot testing (standalone distribution)

The boot ROM is included as `src/rom.bin`, but this source archive does **not** contain a `disk.img/` directory or a prebuilt `system.img`. From `monitor/`, generate a bootable FS16 image using `make dist`. Then run the following from `webdbg/`, passing the image path explicitly:

```sh
node tests/test_boot_engine.js ../monitor/dist/system.img
python3 tests/test_boot_ui.py ../monitor/dist/system.img
```

The second test requires Playwright with Chromium. Rebuild the standalone `cpu16_debugger.html` with `python3 build_local.py`; in the full CPU16 source tree, `python3 build.py` is also available.
