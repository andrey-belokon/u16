# CPU16 Monitor, Revision 2, Step 4: Command History, Boot-Code Installation, and Final Document Revisions

## 1. Command history

When the shell's standard input is a console, the shell puts the console into raw mode with `NOBREAK` and assembles the input line itself from keyboard events: printable characters, Backspace, Ctrl+U, Enter, Ctrl+C (discard the current line), Ctrl+Z on an empty line (end of input), and the **Up/Down arrow keys (↑ ↓)**. History holds the last 16 nonempty lines, without repeating the immediately preceding entry. Pressing Down after the newest history entry restores the line that was being typed before history navigation began.

When its input is redirected, the shell reads one line at a time, as it does for scripts. Programs launched by the shell receive the console in the normal line-input mode.

## 2. Installing boot code on the target machine

- **Kernel:** `DISK_IO` permits writing sectors 1–3 of any volume, including a volume mounted for writing. The kernel neither uses nor caches these sectors. A request extending beyond sector 3 follows the normal rule (`BUSY` for a write-mounted volume).
- **`sysboot N [file]` utility:** Writes boot code (by default, `0:/sys/fsboot.bin`) to sectors 1–3 of disk N. It verifies the version 1 `CXBT` header and the maximum size of 1536 bytes, checks that the disk contains an FS16 volume, and warns if `/sys/monitor.sys` is missing from that volume.
- **System volume:** Contains `/sys/fsboot.bin`, a copy of the boot code.

## 3. Verification

The entire `make test` suite passes (18 scenarios), including the `-fcopy-params` build. New cases include:

- **shell:** History navigation: Up followed by typing more text, Up twice to retrieve an older line, and Up then Down to restore the line being entered.
- **fs:** Writes to sectors 1–3 of a mounted volume are allowed; a write spanning the boundary after sector 3 returns `BUSY`.
- **sysboot:** On a running system, copies system directories and files onto disk 1 and runs `sysboot 1`; exercises errors (invalid boot code, no volume, missing `monitor.sys`) and installation onto mounted volume 0. The test harness then **boots from disk 1** through the ROM, and the copied shell executes its own `/sys/startup`.

The `dist/` distribution was rebuilt and checked in the emulator.

## 4. Documentation

All Monitor documents have had their “draft” designation removed:

| Document | Revision |
| --- | --- |
| System Calls | 2 |
| Kernel Architecture | 2 |
| Shell and Utilities | 2 |
| Disk Boot | 2 |
| Executable File Format | 3 |
| FS16 | 2 (unchanged) |

Cross-references have been updated to point to these revisions.

## 5. Revision 2 is complete

Deferred features: the debugger (`TRAP #10`) and asynchronous disk I/O. Accepted limitations are listed in *Shell and Utilities*, Section 7.
