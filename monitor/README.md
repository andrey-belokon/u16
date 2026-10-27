# CPU16 Operating System, Revision 4

A single-tasking operating system for the CPU16 machine. It includes a kernel with a process chain and the FS16 file system, a command shell, utilities, and disk boot support. See `docs/` for the specifications.

This directory is part of a four-component project:

| Project directory | Contents |
| --- | --- |
| `../b16c/` | B16 compiler |
| `../cpu16/` | Emulator, assembler, and machine documentation |
| `../webdbg/` | Browser debugger (can boot a generated Monitor disk image and access FS16 volumes) |
| `monitor/` | This directory |

## Building and Testing

    make            Build tools (../cpu16, ../b16c), out/monitor.bin, and the out/kmon test harness
    make test       Run all harness scenarios, then repeat with a -fcopy-params build
    make system     Build out/system.img, a bootable volume containing the bootloader, kernel, shell, and utilities
    make dist       Produce the prebuilt distribution in dist/: system.img and rom.bin
    make clean      Remove out/

## Running

Start the prebuilt distribution (see `dist/README_en.md`; `make dist` regenerates it):

    ../cpu16/build/cpu16 --disk 0=dist/system.img dist/rom.bin

Run a custom program instead of the shell, using the standalone monitor image:

    tools/b16build.sh tests/progs/thello.b16
    echo '{"sectors":512,"entries":[{"path":"/sys","dir":true},
           {"path":"/sys/shell.cx","src":"out/thello.bin"}]}' > h.json
    python3 tools/fs16tool.py build out/h.img h.json
    ../cpu16/build/cpu16 --disk 0=out/h.img out/monitor.bin

## Directory Layout

| Directory | Contents |
| --- | --- |
| `docs/` | Monitor specifications (listed below) and milestone reports in `docs/reports/` |
| `kernel/` | Kernel: B16 sources (`kernel.b16` and modules) and assembly sources (`resident.asm`, `boot.asm`, `klib.asm`, `monitor.asm`); `abi.b16` defines the binary interface shared with applications |
| `boot/` | ROM source `rom.asm` and volume bootloader `fsboot.asm` |
| `user/lib/` | Application interface: `sys.b16` (includes declarations from `sysdefs.b16` and wrappers from `sys.asm`), plus `ulib.b16` (functions shared by the shell and utilities) |
| `user/shell/` | Command shell `shell.b16`, which also locates BScript files named `name.bs` |
| `user/bin/` | Utilities: ls, cat, cp, mv, rm, mkdir, rmdir, attr, touch, df, mount, umount, sync, date, mem, fsck, sysboot |
| `user/fm/` | Dual-pane file manager `fm`: copy, move, delete, view, launch; F4 opens the editor |
| `user/edit/` | Text editor `edit`: text storage, undo history, and clipboard use extended-memory frames; supports files up to 1.5 MB, Shift+arrow selection, and Save As |
| `tests/progs/` | Kernel test programs, assembly helpers, and shared `tlib.b16` |
| `tests/fm/` | Manual file-manager tests: `run.sh`, volume generator `mkvol.py`, screenshot utility `show.py` |
| `tests/edit/` | Randomized editor testing against a reference model: `stress.py` |
| `tests/scripts/` | Keyboard scenarios for the harness (`*.scn`) and shell startup scenarios (`*.txt`) |
| `tools/` | Test harness `kmon.cpp` (`--profile` counts instructions by address), `run_tests.py`, FS16 volume tool `fs16tool.py`, kernel-image tool `mksys.py`, volume tool `mksysvol.py`, damaged-volume generator `mkdamaged.py` (for fsck testing), and `b16build.sh` |
| `dist/` | Prebuilt distribution (`system.img`, `rom.bin`) and its README; `make dist` regenerates the binaries |

## Documentation (`docs/`)

| File | Document |
| --- | --- |
| `cpu16_monitor_syscalls_en.md` | CPU16 Monitor: System Calls, Revision 6 |
| `cpu16_monitor_kernel_en.md` | CPU16 Monitor: Kernel Internals, Revision 6 |
| `cpu16_shell_en.md` | CPU16 Monitor: Shell and Utilities, Revision 6 (BScript lookup) |
| `cpu16_boot_en.md` | CPU16 Monitor: Disk Boot, Revision 4 |
| `cpu16_exec_format_en.md` | CPU16 Monitor: Executable File Format and Program Startup, Revision 4 |
| `filesystem_spec_en.md` | FS16 File System, Revision 3 |
| `cpu16_fm_en.md` | CPU16 Monitor: fm File Manager, Revision 6 (launching BScript with Enter) |
| `cpu16_edit_en.md` | CPU16 Monitor: edit Text Editor, Revision 2 |
