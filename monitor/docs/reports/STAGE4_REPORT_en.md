# CPU16 Monitor: Stage 4 Report — Booting from Disk

## 1. Implementation

The boot process follows *CPU16 Monitor: Disk Boot*, Revision 1: ROM → volume boot code → `/sys/monitor.sys` → `boot_entry`.

| Component | File | Size |
| --- | --- | --- |
| ROM: fault vectors, display, reading sectors 1–3 of disk 0, and validating `"CXBT"` | `boot/rom.asm` | 251 bytes |
| Volume boot code: reading the superblock, locating `/sys/monitor.sys` (direct and indirect/IND addresses), loading the code segment through a mapping window into frames 8 onward, loading the resident segment into frame 7, and setting up the kernel mapping | `boot/fsboot.asm` | 955 of 1536 bytes |
| Kernel image `monitor.sys`, built from `monitor.bin` and its memory map | `tools/mksys.py` | 24,064 bytes |
| Writing boot code to sectors 1–3 | `tools/fs16tool.py boot`; the volume specification's `"boot"` field | — |
| Bootable system volume | `make system` → `out/system.img` | — |

**Kernel changes:**

- `boot_entry` was moved out of the initial boot code (`boot.asm`, page 6) into the resident part (page 7), and now installs the vectors itself. Otherwise, the disk bootloader, which also runs in page 6, could not transfer control to it.
- `boot.asm` now only copies kernel code into frames and jumps to `boot_entry`.
- The resident part grew by 74 bytes, to 806 of the available 1276 bytes.

These changes are documented in *Kernel Architecture*, Section 8.3. The new `boot_entry` contract is simpler: the bootloader no longer needs to install the vectors.

## 2. Verification

The entire `make test` suite passes: 15 scenarios, followed by the same tests with `-fcopy-params`.

| Scenario | What it verifies |
| --- | --- |
| **boot** | Runs `tsys` under a kernel booted from disk through the ROM, using all tick schedules. |
| **bootsh** | Runs utility tests (shell, utilities, two volumes) under a kernel booted from disk. |
| **nodisk** | `ROM: no disk 0` |
| **noboot** | An FS16 volume without boot code: `ROM: disk 0 is not bootable` |
| **nomon** | Boot code is present but the kernel file is missing: `boot: no file 0:/sys/monitor.sys` |
| **badmon** | `monitor.sys` is not a valid kernel file: `boot: bad file 0:/sys/monitor.sys` |

`monitor.sys` occupies 12 clusters, so these tests also exercise loading through an indirect-index cluster. Booting was also verified in the standalone emulator:

    cpu16/build/cpu16 --disk 0=out/system.img out/rom.bin

## 3. Bug discovered during implementation

Initially, the bootloader read the `monitor.sys` header into a dedicated buffer that overlapped its own variables, including the current directory-entry addresses. Reading a whole sector overwrote those variables, causing boot to stop with a corrupted-volume error. The header is now read into the shared sector buffer.

## 4. Remaining work

- **Installing boot code from the target machine:** Sectors 1–3 of disk 0 cannot currently be written while the system is running: `DISK_IO` does not write to a mounted volume. For now, boot code is installed by a host-side tool. A target-side installer would require either permission to write sectors 1–3 of a mounted volume through `DISK_IO`, or a separate system call.
- **Monitor Revision 2:** `EXEC`, event handlers, extended memory, and debugger support.
- **Shell command history.**
- **The compiler's deferred-feature list.**
