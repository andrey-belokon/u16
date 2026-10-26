# CPU16 Monitor: Prebuilt Bootable Distribution

| File | Contents |
| --- | --- |
| `system.img` | Bootable 2 MB FS16 volume (4,096 sectors) for disk 0. Sectors 1–3 contain the boot code; `/sys/monitor.sys` contains the kernel; `/sys/shell.cx` contains the shell; `/sys/fsboot.bin` is a copy of the boot code used by `sysboot`. The volume also includes utilities, the `fm` file manager, the `edit` editor, and BScript tools (`bsc`, `bsvm`, `bs`) in `/bin`, plus an empty `/tmp` directory. |
| `rom.bin` | ROM image that loads the boot code from disk 0 and transfers control to it. |

The binary files listed above are included in the archive. They are generated artifacts: to rebuild them from the sources, run `make dist` from the `monitor/` directory. For an explanation of the boot process, see *CPU16 Monitor: Disk Boot*.

## Starting the system

Command-line emulator (from `monitor/dist`):

    ../../cpu16/build/cpu16 --disk 0=system.img rom.bin

Browser debugger: attach `system.img` as disk 0 on the **Disks** tab, then load `rom.bin` as the program.

If the system stops without executing `exit` (for example, following reset or emulator shutdown), the Monitor mounts the volume read-only on the next boot. The shell automatically invokes `fsck` to repair the volume, then remounts it read-write.

Using the system modifies `system.img`: files created in the shell are written back to the image. Keep a copy of the original image if you want to restore a clean system. Always shut down with `exit` when possible; the Monitor unmounts the volume so it remains consistent. To check the image from the host:

    python3 ../tools/fs16tool.py check system.img

## Volume contents

```text
d         2048  bin
          2144  bin/attr.cx
          3266  bin/bs.cx
         31602  bin/bsc.cx
         27688  bin/bsvm.cx
         21410  bin/bsvm.ovl
          1732  bin/cat.cx
          2300  bin/cp.cx
          2524  bin/date.cx
          2224  bin/df.cx
         23434  bin/edit.cx
         24042  bin/fm.cx
          8620  bin/fsck.cx
          2664  bin/ls.cx
          1088  bin/mem.cx
          1636  bin/mkdir.cx
          1998  bin/mount.cx
          2352  bin/mv.cx
          1708  bin/rm.cx
          1674  bin/rmdir.cx
          1506  bin/sync.cx
          2608  bin/sysboot.cx
          2320  bin/touch.cx
          1742  bin/umount.cx
d         2048  sys
s          936  sys/fsboot.bin
rs       29696  sys/monitor.sys
          8810  sys/shell.cx
d            0  tmp
```

Built-in shell commands: `cd`, `pwd`, `echo`, `exit`, `cls`, `help`. Utilities: `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `attr`, `df`, `mount`, `umount`, `sync`, `date`, `mem`, `fsck`, `touch`, `sysboot`. The shell supports command history using ↑ and ↓. Run the file manager with `fm` and the text editor with `edit [file]` (F4 from `fm`; Shift+F4 creates a new document). File and command names are case-insensitive: `LS /BIN` is equivalent to `ls /bin`.

The BScript compiler `bsc`, virtual machine `bsvm` (using the overlay file `bsvm.ovl`), and launcher `bs` are also installed in `/bin` (sources in `../bscript`). Run a script as `bs file.bs` or simply by its name without `.bs` from the shell. A bytecode cache named `file.bsc` is created alongside the source. An explicitly named `.bsc` file is executed directly by `bsvm`. In `fm`, Enter launches `.cx`, `.bs`, and `.bsc` files; F3 views a file.
