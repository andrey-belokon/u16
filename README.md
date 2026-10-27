[![Stand With Ukraine](https://raw.githubusercontent.com/vshymanskyy/StandWithUkraine/main/banner2-direct.svg)](https://stand-with-ukraine.pp.ua)
# μ16 Project

μ16 is a simple 16-bit computer built from scratch, together with everything needed to use it: a processor and machine specification, two emulators, an assembler, a systems programming language with its compiler, a scripting language, a file system, and an operating system called **Monitor**. All components are documented in detail, and the specifications are normative: the tools are tested against them.

The project is meant to be understood in full. The processor has a small, regular instruction set; the machine has a handful of memory-mapped devices; the operating system is non-preemptive and has no hidden magic. The same system can be examined at every level, from a single instruction in the debugger to a shell script running on the finished OS.

<img src="img\u16_demo_1.jpg"></img>

## How the pieces fit together

```text
 B16 source ──b16c──▶ assembly ──cpu16asm──▶ binary image ──▶ cpu16 / web debugger
 hand-written .asm ───────────────────────────▲                      │
                                                                     ▼
              BScript ──bsc──▶ bytecode ──bsvm──▶  Monitor (kernel, shell, utilities)
              (itself written in B16 and running on the CPU16 machine)
```

| Layer | What it is |
| --- | --- |
| CPU16 processor | 16-bit processor: 16 registers, compact one- and two-word instructions, interrupts |
| CPU16 machine | Memory map, 80×25 color display, keyboard, 50 Hz timer, real-time clock, disk controller, memory manager |
| Monitor | Operating system: boot from disk, FS16 file system, processes, sessions, system calls, shell, utilities |
| Languages | `cpu16asm` assembly, **B16** (compiled systems language), **BScript** (dynamic scripting language) |
| Tools | `cpu16` command-line emulator, browser-based debugger, `b16c` compiler, `cpu16asm` assembler |

## Repository layout

| Directory | Contents |
| --- | --- |
| `b16c/` | B16 language compiler (`b16c`), the B16 library (`io.b16`, etc.), and example programs for bare hardware and for Monitor |
| `cpu16/` | `cpu16` machine emulator, `cpu16asm` assembler, standard library (`stdlib.asm`), and documentation for the CPU, machine, and assembler |
| `webdbg/` | Browser-based debugger with an emulator, assembler, disks, and an FS16 file manager (`webdbg/cpu16_debugger.html`) |
| `monitor/` | CPU16 operating system: kernel, shell, utilities, disk boot support, specifications (`monitor/docs/`), and tooling for generating a bootable distribution (`monitor/dist/` via `make dist`) |
| `bscript/` | BScript: the `bsc` compiler, the `bsvm` virtual machine, the `bs` launcher, tests, and example scripts |

## The CPU16 processor

- **Registers.** Sixteen 16-bit registers: `R0`–`R11` are general-purpose, `R12` is the link register `LR`, `R13` is the stack pointer `SP`, `R14` is the instruction pointer `IP`, and `R15` is `FLAGS`. `IP` and `FLAGS` are ordinary registers: a jump is a write to `IP`, and `LD IP, LR` is a return.
- **Memory.** 64 KiB byte-addressable address space, little-endian words, words and instructions aligned to even addresses.
- **Instructions.** One or two words long, with at most one memory access each. Two-operand format (`ADD R0, [R1++]` means `R0 = R0 + [R1]`, then `R1 += 2`). Addressing modes include immediate, register, indirect, auto-increment and auto-decrement, absolute, and base plus displacement.
- **Arithmetic.** Add and subtract with carry for multiword arithmetic, 16×16 multiply (`MUL`, `MULHU`), unsigned divide and remainder. Division by zero does not trap: it sets a flag. Signed multiply and divide are done in software.
- **Subroutines.** `CALL` stores the return address in `LR` instead of pushing it, so leaf routines need no stack. The standard calling convention passes arguments in `R0`–`R3` and requires `R4`–`R11` to be preserved.
- **Interrupts.** Eight IRQ lines, software interrupts (`TRAP #n`), and the ERRCMD exception for invalid instructions, dispatched through a 16-entry vector table at `$FF00`.
- **Reset state.** All registers are zero, interrupts are off, and execution starts at `$0000`.

The architecture is defined in *Minimalist 16-bit Processor Architecture* (revision 6), and the assembly language in the *Assembly Language Reference*.

## The CPU16 machine

The machine specification (revision 9) defines everything outside the processor:

- **Console and display.** One port serves as a plain teletype and as an 80×25 text display with per-cell color attributes, a cursor, positioned output, and screen read-back. The single-byte character encoding combines ASCII, box-drawing characters, and Cyrillic.
- **Keyboard.** Character codes plus scan codes and Shift/Ctrl/Alt modifiers.
- **Timer and clock.** A 50 Hz timer on IRQ0 and a real-time clock whose value format is shared with the file system's timestamps.
- **Disk controller.** Up to eight disks of 512-byte sectors; commands `READ`, `WRITE`, `INFO`, `FLUSH`; completion signalled on IRQ1.
- **Memory manager.** Eight 8 KiB logical pages mapped onto up to 256 physical frames (2 MiB). Monitor uses it to give every process its own address space.

The device window occupies `$FF20–$FFFF`; ordinary programs may use the full 64 KiB below it.

## Tools

### Emulators

- **`cpu16`** is the command-line emulator. It runs flat binary images, attaches disk images (`--disk N=file[:ro]`), and supports instruction tracing (`-t`), step limits (`-n`), strict alignment checks, a fixed starting time for reproducible runs (`--rtc`), and a screen dump at exit (`--screen`).
- **The web debugger** (`webdbg/cpu16_debugger.html`) is a single HTML file that runs entirely in the browser. It includes an editor, an assembler, source-level stepping and breakpoints, a disassembler, register, memory, and device panels, a separate Display window, a disk manager with an embedded boot ROM, and an FS16 file manager.

Both implement the same machine, and their agreement is verified by comparing the execution of randomized programs.

### Assembler: `cpu16asm`

```text
cpu16asm prog.asm            # produces prog.bin
cpu16asm prog.asm -l prog.lst -m prog.map
```

Labels (global and `@local`), expressions, `.org`, `.word`, `.asciz`, `.include`, and other directives; optional listing and memory-map output. The standard library `stdlib.asm` provides console I/O, decimal and hexadecimal printing, line input, string and memory routines, 32-bit arithmetic, display, keyboard, timer, and clock helpers.

### B16 and `b16c`

B16 is a compiled systems language in the spirit of B and C, designed so that the underlying machine stays visible: every statement compiles to a short, predictable sequence of instructions, and expensive operations look expensive.

- Types: `byte`, `char`, `int`, `uint`, `long`, `ulong`, typed pointers, arrays, structures, and function pointers.
- Multiple return values, `interrupt fn` handlers, `goto`, and inline assembly. Linking to separate assembly modules is done with `include asm`.
- Local variables live in registers unless something forces them into memory.
- No preprocessor, floating point, or hidden copies of structures.

A B16 program is built as a CX16 executable that runs either on bare hardware or under Monitor, using the same image. The compiler emits assembly for `cpu16asm`.

```text
include "io.b16"

fn main() {
    puts("Hello, CPU16!\n")
}
```

### BScript

BScript is a small dynamic language for the jobs that are tedious in B16: walking directories, parsing text, launching utilities, and checking their output. It has `int` (signed 16-bit), strings, arrays, maps, multiple return values, string interpolation, and errors as values that propagate automatically unless a script captures them. Source is compiled to bytecode by `bsc` and executed by `bsvm`; the `bs` launcher caches the compiled `.bsc` file beside the source. All three tools are written in B16 and run on the CPU16 machine itself.

```text
println("hello, world")
```

## The Monitor operating system

Monitor is a compact, readable operating system. The kernel is written mostly in B16, with a small resident part in assembly.

**Boot.** The ROM reads three boot sectors from disk 0, the volume boot code finds `/sys/monitor.sys` on the FS16 volume and loads it, and the kernel starts the shell from `A:/sys/shell.cx`. The browser debugger embeds the boot ROM; for the command-line emulator, `monitor/dist/rom.bin` is the ROM.

**Processes.** A program is a CX16 executable loaded at address `$0000` in its own address space, with no relocation. Processes form a chain: a process starts a child with `SPAWN` and waits for it, and only the last process in the chain runs. The kernel never switches processes on its own, and it runs with interrupts disabled, so system calls are atomic and need no locks. Up to four **sessions** (each with its own process chain and screen) can be kept and switched between, but only one runs at a time.

**System calls.** Programs use `TRAP #9`. The interface covers processes (`SPAWN`, `EXEC`, `EXIT`), file and device descriptors, the file system, console input events and positioned output, memory (including extended memory), time, sessions, and raw disk access.

**File system.** FS16 supports volumes of up to 128 MB: 512-byte sectors, 2 KiB clusters, 16-bit cluster numbers, 64-byte directory entries that carry the file metadata, direct, indirect, and double-indirect block pointers, and a free-space bitmap. Volumes are addressed as `A:`–`H:`, names are case-insensitive, and a DIRTY flag with write ordering rules lets `fsck` repair a volume after an unclean shutdown.

**Shell and utilities.** The shell supports pipelines, redirections (`<`, `>`, `>>`, `2>`), command scripts, a startup script (`A:/sys/startup`), and sessions. Utilities in `A:/bin` include `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `attr`, `df`, `mount`, `umount`, `sync`, `date`, `touch`, `mem`, `sysboot`, and `fsck`. Two full-screen programs are provided: **`fm`**, a two-panel file manager in the Norton Commander style, and **`edit`**, a text editor with selection, clipboard, undo, and search and replace. The BScript tools (`bs`, `bsc`, `bsvm`) are installed alongside them.

Shell, utilities, `fm`, `edit`, and the BScript tools are ordinary programs: they use only system calls and have no special privileges.

## Quick start

**Run an assembly program** (put `stdlib.asm` next to it, or adjust the `.include` path):

```asm
        LD      SP, #$FF00      ; initialize the stack
        LD      R0, #hello
        CALL    puts            ; library routine
        WFI                     ; stop
hello:  .asciz  "Hello, CPU16!\n"
        .include "stdlib.asm"
```

```text
cpu16asm hello.asm
cpu16 hello.bin
```

**Boot Monitor** with the prebuilt distribution (`make dist` rebuilds it from the sources):

```text
cd monitor && make dist
../cpu16/build/cpu16 --disk 0=dist/system.img dist/rom.bin
```

The archive includes the prebuilt `monitor/dist/system.img` and `monitor/dist/rom.bin` files; `make dist` regenerates them. To boot in the browser debugger, attach `monitor/dist/system.img` as disk 0 and click **Boot** (the debugger has its boot ROM embedded). `fm` and `edit` draw directly on the display, so use the debugger's **Display** window for them: the command-line emulator does not forward positioned output to stdout.

**Build and run a B16 program** from `b16c/` (compiles, assembles, and runs in the emulator):

```text
./run.sh examples/universal/hello.b16
```

## Build and test

```text
cd cpu16 && make test           # Emulator, assembler, and library; browser debugger via ../webdbg
cd webdbg && make test          # Browser debugger: comparison against C++ and FS16 tests
cd webdbg && make test-ui       # UI tests (Playwright)
cd monitor && make test         # CPU16 operating system
```

## Documentation

Every document is available in English and Russian (`*_en.md` and `*_ru.md`).

| Topic | Document |
| --- | --- |
| Learning CPU16 assembly | *Introduction to the CPU16 Architecture and Assembly Language* (includes the full standard library) |
| Processor | *Minimalist 16-bit Processor Architecture*, revision 6 |
| Assembler | *CPU16 Assembly Language Reference* |
| Machine and devices | *CPU16 Machine: Memory and Devices*, revision 9 |
| B16 language | *B16: Programmer's Guide* and *B16 Language Specification*, revision 3 |
| BScript | *Getting Started with BScript* and *BScript — Language Specification* |
| File system | *FS16 File System — Specification*, revision 3 |
| Boot | *CPU16 Monitor: Disk Boot*, revision 4 |
| Executables | *CPU16 Monitor: Executable File Format and Program Startup*, revision 4 |
| System calls | *CPU16 Monitor: System Calls*, revision 6 |
| Kernel | *CPU16 Monitor: Kernel Internals*, revision 6 |
| Shell and utilities | *CPU16 Monitor: Shell and Utilities*, revision 6 |
| Applications | *CPU16 Monitor: fm File Manager* and *The `edit` Text Editor* |

A good reading order for newcomers: the assembly introduction, then the machine specification, then the B16 guide, and finally the Monitor system calls.
