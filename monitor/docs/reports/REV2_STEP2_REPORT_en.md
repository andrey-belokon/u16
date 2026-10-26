# CPU16 Monitor, Revision 2, Step 2: Directory Cleanup, SETTOP, and Extended Memory

## 1. Directory layout

| Directory | Contents |
| --- | --- |
| `user/lib/` | `sys.b16`, `sys.asm`, `ulib.b16` |
| `user/shell/` | `shell.b16` |
| `user/bin/` | Utilities |
| `tests/progs/` | Test programs, their `.asm` files, and `tlib.b16` |
| `tests/scripts/` | Keyboard scripts (`*.scn`) and shell startup scripts (`*.txt`) |

`tools/b16build.sh` looks for include files in the source file's directory, `user/lib`, and `kernel`. It looks for a program's assembly module alongside that program's source file. The obsolete, unused `tests/fs*.json` files have been removed.

## 2. Memory management

As specified in *System Calls*, Revision 2, Sections 7.6 and 9.6:

| Call | Behavior |
| --- | --- |
| `SETTOP` (`$41`) | Resizes the calling process's own memory in whole pages; newly allocated frames are zero-filled and `top` in the PIB is updated. |
| `XALLOC` (`$42`) | Allocates a zero-filled extended-memory frame; its frame number is the physical frame number in the machine. |
| `XFREE` (`$43`) | Releases a frame; pages that mapped it revert to their own frame, if any, or become unmapped. |
| `XMAP` (`$44`) | Maps a frame into page 0–6, including pages above `top`; `XM_OWN` restores the process's own frame. |
| `MEMINFO` | R3 returns the number of extended-memory frames owned by the process. |

**Kernel changes:**

- An owner table, `owner[256]`, replaces the `fmap` bitmap: 0 means free, `$FF` means kernel-owned, 1–16 identifies the owning process, and `$FE` marks a new program while `EXEC` is in progress.
- Each process has a current mapping, `Proc.map[7]`. A process's accessible memory consists of pages mapped to its frames. System-call buffers, handler addresses, and the stack used to deliver Ctrl+C are all validated against this mapping.
- On `EXIT` or `EXEC`, every frame owned by the process is released according to the owner table.

No separate page bitmap was added to the process descriptor, since it would duplicate information in the owner table.

**The Monitor now identifies itself as Revision 2** (`CPU16 monitor, revision 2`, `SYSINFO.version` = `$0200`). The `mem` utility reports the process's extended-memory allocation.

**Size:** kernel code occupies 23.5 KB of 32 KB; kernel data occupies 7.4 KB of 8 KB (570 bytes free); the resident portion occupies 878 bytes.

## 3. Verification

The new **xmem** scenario (the `txmem` shell, `max_top` = `$A000`) covers: `XALLOC`; mapping a frame above `top`; alternately mapping two frames into the same page while preserving their contents; mapping a frame into page 6; passing a system-call buffer spanning two extended-memory pages; `READ` from a file into an extended-memory frame; `XM_OWN`; `FAULT` on an unmapped page; `XFREE` of a mapped frame; invalid `XFREE` and `XMAP` requests; growing and shrinking memory with `SETTOP`, including PIB updates; invalid `SETTOP` requests; exhausting available memory (`NOMEM` from `XALLOC` and `SPAWN`) and recovering it; and releasing a child process's extended-memory frames on `EXIT` and `EXEC`.

The entire `make test` suite passes (17 scenarios), including the build with `-fcopy-params`. The `dist/` distribution has been rebuilt.

## 4. Remaining Revision 2 work

- Shell command history.
- Installing boot code from within the CPU16 machine.
