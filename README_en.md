# μ16 Project

μ16 is a simple 16-bit computer that includes emulator, assembler, compilers, and operating system.

| Directory | Contents |
| --- | --- |
| `b16c/` | B16 language compiler |
| `cpu16/` | Machine emulator, `cpu16asm` assembler, standard library, and documentation for the CPU, machine, and assembler |
| `webdbg/` | Browser-based debugger with an emulator, assembler, disks, and an FS16 file manager (`webdbg/cpu16_debugger.html`) |
| `monitor/` | CPU16 operating system: kernel, shell, utilities, disk boot support, specifications (`monitor/docs/`), and tooling for generating a bootable distribution (`monitor/dist/` via `make dist`) |

Build and test:

    cd cpu16 && make test           # Emulator, assembler, and library; browser debugger via ../webdbg
    cd webdbg && make test          # Browser debugger: comparison against C++ and FS16 tests
    cd webdbg && make test-ui       # UI tests (Playwright)
    cd monitor && make test         # CPU16 operating system

Start the emulator with the prebuilt Monitor distribution (`make dist` rebuilds it from the sources):

    cd monitor && make dist
    ../cpu16/build/cpu16 --disk 0=dist/system.img dist/rom.bin

The archive includes the prebuilt `monitor/dist/system.img` and `monitor/dist/rom.bin` files; `make dist` regenerates them. To boot in the browser debugger, attach `monitor/dist/system.img` as disk 0 and click **Boot** (the debugger has its boot ROM embedded).
