# CPU16

CPU16 is a minimal 16-bit processor designed with a microcoded implementation in mind. The project includes:

- Revision 6 of the processor architecture specification, plus the machine and assembly-language documentation;
- a C++ emulator and assembler;
- a browser-based debugger;
- a standard library and example programs.

## Project layout

```text
cpu16/
├── README_en.md               This file
├── Makefile                Build and test targets
├── docs/
│   ├── cpu16_spec_r6_en.md        Processor architecture, revision 6 (normative)
│   ├── cpu16_machine_en.md        Machine memory map and devices, revision 9 (normative)
│   ├── cpu16_asm_reference_en.md  Assembly-language reference (normative); section 17 covers cpu16asm options and file formats
│   ├── cpu16_intro_en.md          Introduction to the architecture and assembly language (start here)
│   ├── skeleton.asm            Program skeleton from section 6.4 of the introduction
│   └── skeleton_exec.asm       Program skeleton in CX16 executable format
├── tools/
│   ├── cpu16_isa.hpp           Instruction set shared by the emulator, assembler, and JS table generator
│   ├── cpu16_machine.hpp       Machine definitions: ports, character encoding, scan codes, devices, and disk controller
│   ├── cpu16.cpp               Emulator
│   └── cpu16asm.cpp            Assembler
├── lib/
│   └── stdlib.asm              Standard library: I/O, strings, 32-bit arithmetic, display
├── examples/                   Example programs (see examples/README_en.md)
└── tests/
    ├── cpu16asm_tests.cpp      Assembler tests, including execution in the emulator
    ├── stdlib_test.py          Standard-library tests
    ├── intro_test.py           Tests of the introduction's examples
    └── machine_test.py         Tests based on the CPU16 machine specification (disks, MMU) and disk_demo.asm
```

## Getting started

1. Read `docs/cpu16_intro_en.md`. Consult the processor specification and assembler reference for details.
2. Open `../webdbg/cpu16_debugger.html` in your browser. The debugger is located at the top level of the project, in `webdbg/`. A demo program is preassembled: press F9 to run or F7 to step. You can paste an example from `examples/` into the **Source** tab and assemble it with Ctrl+B.

## Machine model

The machine specification is included in this project as [`docs/cpu16_machine_en.md`](docs/cpu16_machine_en.md) (Revision 9). It is the reference for the emulator and browser debugger. The main machine features are:

- 2 MB of physical memory, with an MMU comprising eight 8 KB pages and registers `$FF40–$FF48`. After reset, the MMU is disabled. Software must initialize all eight PAGE registers before enabling it; their reset values are not specified by the machine interface.
- Console, display, and keyboard ports at `$FF20–$FF28`; timer at `$FF29` (IRQ0).
- Real-time clock at `$FF2A–$FF2E`. Its value uses the FS16 timestamp format; software accesses it through the TIME and DATE registers and the READ, WRITE, WRITE_TIME, and WRITE_DATE commands.
- Disk controller at `$FF30–$FF3B`, supporting up to eight disks with 512-byte sectors, DMA transfers, and level-triggered IRQ1.
- Machine character encoding.

The device window `$FF20–$FFFF` is selected using the logical address *before* MMU translation and is reserved entirely for devices. Reads from unmapped device addresses return zero; writes are ignored. The vector table at `$FF00–$FF1F` is ordinary memory.

Emulator-specific choices are documented in Section 9 of the Revision 9 machine specification:

- **Disk-command validation order:** BADCMD → NODISK → (COUNT = 0: success) → READONLY → RANGE → BADADDR.
- **Command timing:** In the command-line emulator, disk commands complete when CMD is written. In the debugger, transfers take 1 ms per sector (or 1 ms total for INFO and FLUSH); the delay is configurable on the **Disks** tab. Transfers do not progress while execution is paused or single-stepped.
- **Clock:** Initially set to the host's local time (or 2025-01-01 00:00:00 if the host date is outside 2025–2088). Machine seconds advance along with host seconds, using a monotonic host clock so later changes to the host's wall clock have no effect. Machine reset clears TIME, DATE, and ERR but does not stop or reset the running clock.
- **Program image:** At most 64 KB, loaded at the address specified by `-l` (default 0). Immediately after reset, that address is both a logical and a physical address.

## Building

Requires a C++17-capable `g++` and Python 3. Tests also require Node.js; UI tests require Playwright with Chromium.

```sh
make                    # Build build/cpu16, build/cpu16asm, and ../webdbg/cpu16_debugger.html
make examples           # Build all examples into build/examples/*.bin
```

Without `make`:

```sh
g++ -std=c++17 -O2 -o cpu16 tools/cpu16.cpp
g++ -std=c++17 -O2 -o cpu16asm tools/cpu16asm.cpp
python3 ../webdbg/build.py
```

## Running

```sh
build/cpu16asm examples/snake.asm -o snake.bin -l snake.lst -m snake.map
build/cpu16 snake.bin
```

- **Memory map:** `-m file.map` writes a map of every object in the image: address, size, kind (code, data, BSS, alignment), source location, memory usage totals by kind and file, objects sorted by size, and constants. See section 17.4 of the assembler reference for the format. Use the map to track memory consumption or locate an object from a traced address.
- **Original source locations:** The `.line "file", N` directive (or `.line N` for the same file) associates subsequent assembly lines, up to the next `.line`, with a location in the original source program (for example, B16). Diagnostics, the memory map, and the listing's source-location column refer to that source. Error messages also identify the corresponding assembly line: `kernel.b16:120 (kernel.asm:345): error: ...`. Pass `--no-line` to disable `.line` handling entirely. All `cpu16asm` options and the listing, map, and diagnostic formats are documented in section 17 of the assembler reference.
- **Console I/O:** The emulator writes console text to stdout and reads keyboard input from stdin. Terminal arrow and function keys are converted to scan codes; xterm sequences that include Shift, Ctrl, or Alt also set the corresponding modifiers in the MODS register (machine specification, sections 2.6 and 9.2).
- **Display:** The 80×25 screen is visible only in the browser debugger. `--screen` prints the final screen contents after the emulator exits.
- **Disks:** `--disk N=file[:ro]` attaches a disk image to slot N (0–7); the option may be repeated. Image size must be a multiple of 512 bytes. `:ro` (or a file that cannot be opened for writing) makes the disk read-only. The disk label is the image filename. See `examples/disk_demo.asm`.
- **Real-time clock:** `--rtc DD.MM.YYYYTHH:MM:SS` sets the initial clock instead of using the host's local time. The exact argument format is defined in section 9.2 of the machine specification: 24-hour time, years 2025–2088. This is useful for reproducible runs. See `examples/clock.asm`.
- **Termination:** Execution stops when the processor is in WFI, no interrupts are pending, the timer is disabled, and no disk command is in progress that could raise IRQ1.
- **Other options:** `-t` enables tracing; `-n N` limits the number of execution steps; `--strict-align` reports unaligned accesses.

```sh
python3 -c "open('demo.img','wb').write(bytes(64*512))"
build/cpu16asm examples/disk_demo.asm -o disk_demo.bin
build/cpu16 --disk 0=demo.img disk_demo.bin
```

### Executables for the CPU16 Monitor

The executable format is described in *CPU16 Monitor: Executable File Format and Program Startup* ([`../monitor/docs/cpu16_exec_format_en.md`](../monitor/docs/cpu16_exec_format_en.md), Revision 4). Monitor-launched programs begin with interrupts enabled; on a bare machine, as after reset, IF = 0. The executable header is written explicitly in assembly. Uninitialized data is allocated with `.reserve` at the end of the program and does not occupy space in the file. The assembler's `--exec` option validates the completed image's header:

```sh
build/cpu16asm --exec examples/calc_exec.asm -o calc_exec.bin
build/cpu16 calc_exec.bin              # A CX16 executable also runs on a bare machine
```

See `examples/calc_exec.asm` for a working example and `docs/skeleton_exec.asm` for a starting template. Both run under the Monitor and on a bare machine.

## Tests

```sh
make test               # Emulator, assembler, library, introduction, debugger/C++ comparison
make test-ui            # Browser debugger UI scenarios in headless Chromium
```

Test coverage includes:

- **Emulator:** Built-in self-test (`build/cpu16 --selftest`).
- **Assembler:** Tests that run programs in the emulator; `.reserve`; `--exec` header validation; execution of `calc_exec.asm` and `skeleton_exec.asm` both on a bare machine and with a simulated revision-2 Monitor launch (IF = 1, Monitor interrupt vectors, timer ticks between program instructions starting after the first instruction).
- **Standard library:** Boundary values and hundreds of randomized 32-bit arithmetic cases.
- **Introduction:** Every example in the introduction is assembled and run, with output compared against the documented output.
- **CPU16 machine:** Disk-driver examples, IRQ1 waits, MMU activation, and RTC access are executed in the emulator using disk images and `--rtc`. Tests also cover invalid `--disk`/`--rtc` options, `disk_demo.asm`, and `clock.asm`. The relevant code fragments are embedded in the tests. `make test MACHINE_DOC=path/to/cpu16_machine_en.md` additionally compares them with the specification.
- **Browser debugger:**
  - The JavaScript engine is compared with the C++ emulator using randomized programs, including all devices, timer, RTC (with initial values near rollover boundaries), disks (instant and delayed transfers), and the MMU.
  - The disassembler is checked by round-tripping its output through the assembler.
  - The JavaScript assembler is compared byte-for-byte with the C++ assembler on a shared suite of source programs, including executable-header diagnostics.
