# CPU16 Monitor: Disk Boot, Revision 4

This document describes booting the Monitor from disk 0: the ROM startup code, FS16 volume boot code, and the kernel file format (`A:/sys/monitor.sys`). Booting ends when control reaches the kernel's `boot_entry` entry point, whose required initial state is specified in *CPU16 Monitor: Kernel Internals*, Section 8.3.

Sections 1–6 are normative. Section 7 describes tools and tests.

## 1. Related documents

| Document | Relevant material |
| --- | --- |
| *CPU16 Machine: Memory and Devices*, revision 9 | Post-reset state, ROM (sections 7.6 and 7.7), disk controller (section 6), MMU (section 7) |
| *FS16 File System*, revision 3 | Cluster 0: superblock and boot-code sectors 1–3 (section 2); directory entries and cluster addresses (sections 5 and 6) |
| *CPU16 Monitor: Kernel Internals*, revision 6 | Kernel frames (section 2.1), `boot_entry` contract (section 8.3) |
| *CPU16 Monitor: System Calls*, revision 6 | `DISK_IO` (section 9.8), used to write sectors 1–3 |
| *CPU16 Monitor: Shell and Utilities*, revision 6 | `cp`, `attr`, and `sysboot` utilities |

The documents are referred to below as the *Machine*, *FS16*, and *Kernel Internals* specifications.

## 2. Boot sequence

| Stage | Code | Execution location | Action |
| --- | --- | --- | --- |
| 1 | ROM | `$0000…`, frames 0–3, MMU disabled | Reads sectors 1–3 of disk 0 into `$C000`, checks the signature, jumps to `$C008` |
| 2 | Volume boot code | `$C000–$C5FF`, frame 6 | Locates `/sys/monitor.sys`, loads it into the kernel frames, configures the kernel mapping, and jumps to the entry point from the file header |
| 3 | `boot_entry` | Page 7, frame 7 | Initializes vectors and kernel data, then enters `kmain` (*Kernel Internals*, Section 8.3) |

**Memory during boot:**

| Frames | Use |
| --- | --- |
| 0–3 | ROM (in the emulator, the ROM image occupies RAM) |
| 6 | Volume boot code at `$C000–$C5FF`; buffers at `$D000–$D4FF`; ROM and bootloader stack growing downward from `$E000` |
| 7 | Resident kernel code and interrupt vector table |
| 8–11 | Kernel code |
| 12 | Kernel data, initialized by `boot_entry` |

The boot code and its stack do not overlap any frames being filled. The kernel excludes frames 0–6 from its allocation pool (*Kernel Internals*, Section 2.1).

## 3. ROM

After reset, IP = 0, the MMU is disabled, IF = 0, and the timer is disabled (*Machine*, sections 3 and 7). The ROM code:

1. Sets SP to `$E000` (stack on page 6).
2. Initializes all 16 interrupt vectors to point to its fault handler.
3. Sets the display to teletype mode with cursor enabled and ATTR = `$07`.
4. Checks that disk 0 is attached (bit 0 of DRIVES).
5. Reads three sectors starting at LBA 1 of disk 0 into `$C000`, polling STATUS until BUSY = 0 (IEN = 0), then acknowledges the command (ACK).
6. Validates the boot-code header (section 4.1).
7. Jumps to `$C008` with the MMU still disabled.

**Failures:** The ROM prints the applicable English-language diagnostic, disables interrupts with `CLI`, and halts in a `WFI` loop:

| Condition | Message |
| --- | --- |
| Disk 0 not attached | `ROM: no disk 0` |
| Read failure | `ROM: disk 0 read error` |
| Invalid boot-code header | `ROM: disk 0 is not bootable` |
| Invalid instruction or interrupt | `ROM: fault` |

The ROM is independent of FS16: it reads the three fixed boot sectors. The ROM image may occupy at most 32 KB (frames 0–3); the current code is 251 bytes long.

## 4. Volume boot code

### 4.1. Header

Sectors 1–3 of an FS16 volume (FS16, section 2) contain 1,536 bytes loaded by the ROM into `$C000`:

| Offset | Size | Contents |
| --- | --- | --- |
| 0 | 4 | Magic `"CXBT"` |
| 4 | 2 | Boot-code version: 1 |
| 6 | 2 | 0 |
| 8 | … | Code; ROM jumps to `$C008` |

An FS16 volume whose sectors 1–3 are zero-filled is not bootable (FS16, section 2).

### 4.2. Operation

Like the ROM, the volume boot code runs with interrupts disabled and waits for disk operations by polling STATUS.

1. **Superblock.** Read disk sector 0 and check for the `"FS16"` signature.
2. **Locate the kernel file.** Starting from the ROOT directory entry in the superblock, find directory `sys` in the root, then file `monitor.sys` in that directory. Names are compared case-insensitively (FS16, section 7.1), across all 32 bytes of the NAME field: `SYS` and `Monitor.sys` are also accepted.
3. **MMU.** Enable this mapping: PAGE0–PAGE3 = frames 0–3; PAGE4 = window; PAGE5 = 5; PAGE6 = 6; PAGE7 = 7.
4. **File header.** Read the file's first sector and validate the header (section 5).
5. **Code segment.** Read sector j (j = 0, 1, …) of the segment using PAGE4 as a window onto frame 8 + j / 16, at address `$8000` + (j mod 16) · 512.
6. **Resident segment.** Read sector j directly into address `segment address + 512 · j` on page 7.
7. **Kernel mapping.** Set PAGE0–PAGE4 to frames 8–12, PAGE5 to scratch frame 13, and leave PAGE6 mapped to frame 6, where the boot code is still executing.
8. **Transfer control** to the entry point specified in the file header. SP remains on page 6 and IF = 0, as required by the `boot_entry` contract.

**Limitations.** The boot code supports direct cluster pointers and the single-indirect index IND, but not the double-indirect index DIND (FS16, Section 6). The `/` and `/sys` directories and the `monitor.sys` file must therefore each occupy no more than 1,034 data clusters (2,068 KB). The boot code scans each directory in full, regardless of where its entries appear.

### 4.3. Errors

| Condition | Message |
| --- | --- |
| No valid FS16 superblock on disk 0 | `boot: no FS16 volume on disk 0` |
| `/sys` or `/sys/monitor.sys` missing; `sys` is not a directory or `monitor.sys` is a directory | `boot: no file A:/sys/monitor.sys` |
| Invalid kernel file header | `boot: bad file A:/sys/monitor.sys` |
| A zero cluster pointer occurs within the recorded file size, or locating the data requires DIND | `boot: damaged volume or file too large` |
| Disk read failure | `boot: disk 0 read error` |

On any of these failures, the volume boot code prints the corresponding English-language diagnostic and halts with interrupts disabled (`CLI`, then `WFI`).

## 5. Kernel file `monitor.sys`

The first sector contains this header, padded with zeros to 512 bytes:

| Offset | Size | Field | Contents |
| --- | --- | --- | --- |
| 0 | 4 | `magic` | `"CXMS"` |
| 4 | 2 | `version` | 1 |
| 6 | 2 | `entry` | `boot_entry` address within the resident segment |
| 8 | 2 | `code_len` | Code-segment length in bytes, at most `$8000` |
| 10 | 2 | `res_addr` | Resident-segment address: `$E000` |
| 12 | 2 | `res_len` | Resident-segment length in bytes |

Two segments follow, each padded with zeros to a sector boundary:

1. **Code segment:** Contents of kernel-map addresses `$0000` through `code_len − 1` (kernel code and initialized data templates, ending at `__image_end`).
2. **Resident segment:** Contents of addresses `res_addr` through `res_addr + res_len − 1`.

The code segment is placed in frames 8, 9, … (page k maps to frame 8 + k). The resident segment goes into frame 7. Unused bytes in the last sectors are loaded as well: on page 7 they fall in a region of the resident code that the kernel never uses; in code frames they occupy addresses beyond `__image_end`.

Setting the SYSTEM and RDONLY flags on the kernel file is recommended to prevent accidental deletion. The bootloader does not inspect those flags.

## 6. Updating and installing on a running system

`monitor.sys` is an ordinary volume file: replacing it installs a new kernel that takes effect at the next boot. If the file is marked RDONLY, temporarily clear that flag while replacing it:

```sh
attr -r /sys/monitor.sys
cp new.sys /sys/monitor.sys
attr +r /sys/monitor.sys
```

The boot code in sectors 1–3 does not change: it finds the kernel by filename.

**Installing boot code.** Run `sysboot X [file]`, where X is a volume letter `A`–`H` or a disk number 0–7 (*Shell and Utilities*, section 6). The utility writes the specified boot-code file (default `A:/sys/fsboot.bin`, a copy of `out/fsboot.bin` on the system volume) into sectors 1–3 of disk X through `DISK_IO`. The Monitor does not otherwise use those sectors, so they may be written even when the volume is mounted read-write, including volume `A:` (*System Calls*, section 9.8). `sysboot` checks the boot-code header (section 4.1) and verifies that the disk contains an FS16 volume.

**Creating a bootable disk from the running system:**

```sh
mkdir B:/sys
mkdir B:/bin
cp /sys/monitor.sys B:/sys
cp /sys/shell.cx B:/sys
cp /bin/ls.cx B:/bin            # And any other required utilities
sysboot B
```

If `/sys/monitor.sys` is missing, `sysboot` warns that the disk will not boot. To boot from the newly prepared disk, attach it as disk 0 (volume `A:`).

## 7. Tools and verification

| File | Purpose |
| --- | --- |
| `boot/rom.asm` | ROM; assembled as `out/rom.bin` |
| `boot/fsboot.asm` | Volume boot code; base address `--base $C000`; assembled as `out/fsboot.bin`, at most 1,536 bytes |
| `tools/mksys.py` | Builds `monitor.sys` from `monitor.bin` and memory-map symbols (`__image_end`, `resident_end`, `boot_entry`) |
| `tools/fs16tool.py boot IMG CODE` | Writes boot code to sectors 1–3 of an FS16 volume; a volume-build specification supplies the boot-code file through its `"boot"` field |
| `make system` | Creates `out/system.img`: boot code, `/sys/monitor.sys`, `/sys/fsboot.bin`, shell, and utilities |

To boot from the disk image, start the emulator with the ROM image rather than the standalone Monitor image:

```sh
../cpu16/build/cpu16 --disk 0=out/system.img out/rom.bin
```

Harness scenarios (`make test`):

- **boot, bootsh:** Run the `tsys` test and a set of utility commands under a kernel booted from disk, with all timer-tick schedules.
- **nodisk, noboot, nomon, badmon:** Check the ROM and bootloader diagnostics for a missing disk, missing boot code, missing `monitor.sys`, or an invalid `monitor.sys`.
- **sysboot:** Prepare a bootable disk on a running system (`mkdir`, `cp`, `sysboot`); test `sysboot` errors and installing boot code on mounted volume 0, then boot from the copied disk.

## 8. Revision history

### Revision 4

Paths now include volume letters (*System Calls*, revision 6): bootloader diagnostics are `boot: no file A:/sys/monitor.sys` and `boot: bad file A:/sys/monitor.sys`. `sysboot` accepts a volume letter (`sysboot B`) as well as the previously supported disk number. Boot code and on-disk formats are unchanged.

### Revision 3

The `/sys/monitor.sys` lookup now compares filenames case-insensitively, like the Monitor (section 4; FS16 revision 3). Update boot code on previously prepared disks with `sysboot`: earlier boot code found the file only if the names were lowercase.

### Revision 2

Added boot-code installation from the running CPU16 machine using `sysboot`, and a boot-code copy at `/sys/fsboot.bin` on the system volume (sections 6 and 7).

### Revision 1

Initial specification. At the same time, the `boot_entry` entry point in *Kernel Architecture* (section 8.3) moved into the resident segment and became responsible for initializing the interrupt vectors.
