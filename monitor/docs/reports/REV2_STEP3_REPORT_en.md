# CPU16 Monitor, Revision 2, Step 3: Project Layout and SETMTIME

## 1. Project layout

The project has three top-level directories:

| Directory | Contents |
| --- | --- |
| `b16c/` | B16 compiler |
| `cpu16/` | Emulator, assembler, web debugger (including the check that `--exec` output does not extend beyond `image_end`), and the machine and assembler documentation |
| `monitor/` | All Monitor components: kernel, boot code, user programs, tests, tools, the ready-to-use `dist/` distribution, and `docs/` documentation |

`monitor/docs/` contains all Monitor specifications: system calls, kernel architecture, shell and utilities, disk boot process, executable format, and FS16. Reports reside in `monitor/docs/reports/`. Build scripts (`Makefile`, `tools/b16build.sh`, and `tools/run_tests.py`) use paths relative to `../b16c` and `../cpu16`. The `.git` directory is not included in the archive.

## 2. SETMTIME

The new `$2C SETMTIME(path, timestamp)` call (see *System Calls*, Revision 2, Section 9.4) sets the modification time of a file, directory, or volume root; a value of 0 means no timestamp. Timestamp fields are validated according to FS16 (`INVAL` for invalid values). Device paths return `INVAL`, while read-only volumes return `RDONLY`. Open file descriptors immediately reflect the new value.

The new `touch [-t YYYY-MM-DD HH:MM:SS] file ...` utility creates missing files and sets their modification times to the current or specified time. Date parsing was moved to `ulib.b16` so that `touch` and `date` share the same implementation.

## 3. Verification

The entire `make test` suite passes, including the `-fcopy-params` build. New checks cover `SETMTIME` on a file, observation through an already-open descriptor, the volume root, a zero timestamp, an invalid timestamp, a device, a read-only volume, and a missing file (the `fs` scenario). The `utils` scenario checks `touch -t` on an existing file and on a new file, as well as an invalid date. The web debugger also passes its comparison with `cpu16asm`.
