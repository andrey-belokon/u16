# CPU16 Monitor: System Calls, Revision 6

Revision 6 introduces letter-based volume prefixes `A:`–`H:` in paths (Section 4.1). Revision 5 introduced sessions: multiple process chains, each with its own screen, and switching between them (Sections 2.4 and 9.9). Revision 4 added keyboard event modifiers (Shift, Ctrl, Alt) to the `CON_GETEV` result (Sections 6.1 and 9.5). Revision 3 made file names case-insensitive (Sections 4.1, 9.3 and 9.4) and defined the Ctrl+Enter event (Section 6.1). Revision 2 introduced in-place program replacement (`EXEC`), Ctrl+C and illegal-instruction event handlers (`ONEVENT`), resizing a process's memory (`SETTOP`), extended memory (`XALLOC`, `XFREE`, `XMAP`), and modification-time updates (`SETMTIME`). See Section 11 for the revision history.

This document specifies the programming interface of the CPU16 Monitor: the system model, system-call convention, file and device names, file/console/process operations, error codes, and individual calls. It does not describe the internal implementation of the Monitor kernel.

Sections 1–9 are normative. Section 10 specifies the B16 interface and provides sample wrappers.

## 1. Overview

### 1.1. Related documents

| Document | Relevant material |
| --- | --- |
| “16-bit Processor Architecture,” Revision 6 | registers, `TRAP`, `IRET`, interrupts |
| “CPU16 Machine: Memory and Devices,” Revision 9 | console and keyboard (MODS modifiers), screen readback (SCRD), timer, clock, disk controller |
| “FS16 File System,” Revision 3 | volume format, directory entries, file names and timestamps |
| “CPU16 Monitor: Executable File Format and Program Startup,” Revision 4 | program format, PIB, argument block, command-line parsing, initial machine state and interrupt rules |
| “B16 Language Specification,” Revision 3 | calling convention, `long`/`ulong` types and `include asm` (Section 10) |

The shorter names *processor*, *machine*, *FS16*, *CX16 format* and *B16* are used throughout this document.

### 1.2. Terminology

| Term | Meaning |
| --- | --- |
| process | a loaded program together with its memory, descriptors and current directory |
| process chain | a series of parent/child processes; only its last process executes (Section 2.1) |
| descriptor | a number from 0 through 15 used by a process to access an open file or device |
| open-file record | a Monitor record containing the file/device object, access mode and position; multiple descriptors can refer to one record |
| volume | a disk numbered 0–7 containing FS16 and mounted by the Monitor |
| tick | a timer event occurring 50 times per second (machine, Section 3) |
| keyboard event | a word containing the character in its low byte and the scan code in its high byte (machine, Section 2.6) |
| `top` | the upper boundary of a process's memory (CX16 format, Section 6) |

## 2. System Model

### 2.1. Process chain

The Monitor does not time-slice processes. Exactly one process executes at a time: the last process in the chain.

- A process starts a child using `SPAWN` and waits inside that call until the child terminates.
- The child terminates using `EXIT` or by returning through LR (CX16 format, Section 8.1).
- The parent receives the child's exit status as the `SPAWN` result and resumes execution.

The chain's first process is the shell (Section 2.3). There may be several chains, one per session (Section 2.4), but no more than 16 processes in all chains combined, including shells.

Each process has its own memory (`$0000 … top − 1`; CX16 format, Section 2), descriptor table (Section 5.1), and current directory (Section 4.3). While a child runs, its parent's memory, descriptors and registers remain unchanged.

### 2.2. Machine resources

There is no hardware protection for memory or devices, so resource isolation is defined by rules. Violating them constitutes a programming error with undefined results.

| Resource | Access by a program |
| --- | --- |
| memory `$0000 … top − 1` | owned by the program; unrestricted access |
| memory `top … $DFFF`, page 7 | prohibited |
| vector table `$FF00–$FF1F` | prohibited; vectors belong to the Monitor; use `TICKHOOK` for timer events (Section 7.4) |
| display output: write `$FF20`, registers `$FF22–$FF26`; screen readback via SCRD `$FF50` | **permitted** (Section 6.4) |
| keyboard: read `$FF20`, `$FF21` | prohibited; the Monitor consumes keyboard events; use system calls for input (Section 6) |
| timer `$FF29` | prohibited; the tick rate is fixed at 50 Hz |
| real-time clock | prohibited; use `TIME_GET` and `TIME_SET` |
| disk controller | prohibited; use `DISK_IO` for raw sector access |
| memory manager | prohibited |
| interrupts (`CLI`, `STI`) | permitted briefly (CX16 format, Section 8.3) |
| `TRAP #9` | system-call entry (Section 3) |
| `TRAP #10` | reserved for Monitor debugger breakpoints; forbidden to programs |
| `TRAP #11` … `TRAP #15` | reserved by the Monitor; forbidden to programs |

### 2.3. System startup and shutdown

At startup, the Monitor attempts to mount each attached disk containing a valid FS16 volume (Section 5.3). It then starts `A:/sys/shell.cx` with the command line `A:/sys/shell.cx`. The shell begins in `A:/`, with descriptors 0, 1 and 2 connected to the console.

If disk 0 could not be mounted or the initial shell could not be started, the Monitor reports the failure on the console and halts before accepting commands.

When the root process of the last remaining session terminates (normally the shell; Section 2.4):

- With exit code 0, the Monitor shuts down (`SHUTDOWN`, Section 9.8): it flushes pending writes, unmounts volumes, and halts.
- With any other exit code, the Monitor reports the code and starts the standard shell `A:/sys/shell.cx` again in that session.

### 2.4. Sessions

A **session** consists of a process chain as defined in Section 2.1 and its own screen. At most four sessions may exist, numbered 0–3. Session 0, containing the initial shell, is created first; additional sessions are created by `SESS_NEW` (Section 9.9).

Sessions do **not** provide multitasking. Only the **current session** executes, and only the last process in its chain runs (Section 2.1). The last process of each other session is **suspended**: its instructions do not execute; it receives neither `TICKHOOK` callbacks nor Ctrl+C; and any pending blocking call (Section 3.4) makes no progress. Time itself continues: after resumption, `TICKS` and the clock report elapsed real time.

**Switching sessions:**
- **Keyboard:** Ctrl+] selects the next session cyclically; Ctrl+[ selects the previous one. The Monitor recognizes these keys regardless of console mode or `NOBREAK` and never passes them to programs. Recognition uses the scan code for `[` or `]` with Ctrl in the modifier bits (machine, Section 2.6), or character `$1D` as emitted by a terminal for Ctrl+]. A terminal emits Esc for Ctrl+[, so the command-line emulator only supports Ctrl+].
- **System call:** `SESS_SWITCH`, or `SESS_NEW` without `SN_BACK`, causes the Monitor to switch sessions as the system call finishes, before the caller executes another instruction. The caller can observe the returned result only after its session becomes current again.

The transition is transparent to programs:
- **Processor:** a suspended process resumes at precisely the same instruction, with the same registers and flags.
- **Blocking calls:** when the session becomes current again, the Monitor restarts any pending wait (Section 3.4) transparently, without returning `INTR` merely because of the switch. Each session retains its unfinished line-input buffer; `SLEEP` waits only for the ticks that remained at the time of the switch.
- **Display:** each session has its own screen. On switching, the Monitor saves the outgoing screen contents, CTRL, ATTR and cursor position, then restores the incoming session's state. A newly created session starts with an empty screen in teletype mode, an active cursor, and ATTR = `$07`.
- **Keyboard:** events queued before a switch are discarded because they belonged to the previous session.

**Processes in a session.** The root process created by `SESS_NEW` is the program named in the supplied command line, usually a shell. Its descriptors 0, 1 and 2 refer to the console, and it inherits the calling process's current directory. Child processes behave as in Section 2.1. Memory, files and volumes are shared across sessions: a file opened in one session is visible to another, and deleting a file opened by a suspended process returns `BUSY` (Section 5.2).

**Ending a session:**
- If its root process terminates while other sessions exist, the session closes and the next session becomes current. No process receives the root's exit status.
- If the **last** session's root terminates, Section 2.3 applies: code 0 shuts down the machine; any other code causes the standard shell to be started again in that session, not necessarily the program that just exited.
- `SESS_KILL` terminates all processes in a session without notification, as during power-off; it closes descriptors and frees memory.
- `SHUTDOWN` halts the system and all sessions.

## 3. Calling Convention

### 3.1. Invocation

A system call consists of a `TRAP #9` instruction immediately followed by a word containing the **call number**:

```asm
        TRAP    #9
        .word   $12             ; READ
```

The Monitor returns to the address immediately following the call-number word.

**Arguments** are passed in R0–R3 according to the B16 argument-slot convention (B16, Section 9.3):
- A word-sized argument occupies one register.
- A `long`/`ulong` argument occupies two consecutive registers, with the low word first.

Each call defines its own arguments. Values in R0–R3 that are not used as arguments do not affect the result.

### 3.2. Return values

| Resource | State after the call |
| --- | --- |
| R0 | **error code** (Section 8); 0 means success |
| ZF | 1 if R0 = 0; otherwise 0 |
| R1–R3 | return values, using the argument-slot convention; registers not used for results are undefined |
| R4–R12 (including LR), SP | preserved |
| IF | preserved |
| CF, SF, OF | undefined |

Results in R1–R3 are defined only when R0 = 0, unless the individual call specifies otherwise. For example, `READ` and `WRITE` report the number of bytes transferred even on failure.

An unknown call number returns `NOSYS`.

### 3.3. Pointers and strings

- **Process memory** consists of pages mapped to frames owned by the process: its private pages below `top` and pages containing extended-memory frames (Section 7.6), including pages above `top`.
- A **buffer** `[p, p + n)` must fit entirely within process memory. Otherwise, the call returns `FAULT` without performing any operation. A zero-length buffer is not checked.
- A **string** (path or command line) is a sequence of bytes in the machine encoding, terminated by a zero byte. The entire string, including the terminator, must reside in process memory and meet the length limit specified by the call. Otherwise the error is `FAULT` (memory boundary) or `BADPATH`/`ARGS` (length).
- A **code address** (for `TICKHOOK` or `ONEVENT` handlers) must be even and reside in process memory; otherwise the call returns `FAULT`.

Buffers supplied to the same call must not overlap. The behavior of a call with overlapping buffers is undefined.

### 3.4. Blocking calls and interruption

Most calls complete immediately. Only the following can block:
- Console reads (`READ` on the console; `CON_GETEV` without `NOWAIT`).
- `SLEEP`.
- `SPAWN`, while the child is running.

The Monitor continues handling ticks and keyboard input while a call is blocked.

If Ctrl+C occurs during a blocking call and is configured to act as an interrupt, the wait ends and the call returns `INTR`. Section 6.3 specifies whether the process is then terminated or its `ONEVENT` handler is invoked. Ctrl+C does not interrupt a nonblocking call midway: that call finishes before the interrupt takes effect.

Switching sessions (Section 2.4) does not complete a pending wait or return `INTR`. When the process becomes current again, the Monitor retries the call transparently. For `SLEEP`, only the ticks remaining at the switch must still elapse; time spent in another session is not counted toward that remainder. A nonblocking call completes before the switch takes effect.

### 3.5. Numbering

Call numbers are organized into groups of 16:

| Numbers | Group |
| --- | --- |
| `$00–$0F` | processes |
| `$10–$1F` | descriptors and I/O |
| `$20–$2F` | file system |
| `$30–$3F` | console |
| `$40–$4F` | memory |
| `$50–$5F` | time |
| `$60–$6F` | system |
| `$70–$7F` | sessions; `$7F` is reserved by the Monitor |

Numbers not listed in Section 9.1 are available for future revisions and return `NOSYS`.
## 4. Names

### 4.1. Paths

A path is a string of no more than 255 bytes, excluding the terminating zero. It identifies a file or directory on a volume, or a device.

```text
path       = [ prefix ] [ "/" ] components
prefix     = volume ":"        volume: a letter A–H or digit 0–7
           | "dev:"            device (Section 4.2)
components = component { "/" component } [ "/" ]
```

The **volume** in a prefix is a case-insensitive letter `A`–`H`: `A:` denotes volume 0, `B:` volume 1, through `H:` for volume 7. For compatibility with earlier revisions, a numeric volume prefix is also accepted: `0:` is equivalent to `A:`, and `7:` to `H:`. Paths returned by the Monitor (`GETCWD`, messages) always use an uppercase letter. In calls such as `MOUNT`, `VOLINFO` and `DISK_INFO`, volume numbers remain integers in the range 0–7. Any other character before `:` (such as `I:` or `8:`) is not a valid volume prefix and produces `BADNAME`.

**Starting point for path resolution:**

| Form | Resolution starts at |
| --- | --- |
| `X:/a/b`, `X:a/b` | volume X root; `X:x` is equivalent to `X:/x` |
| `X:` | volume X root |
| `/a/b` | root of the volume containing the current directory |
| `a/b` | current directory (Section 4.3) |

**Components** are resolved according to FS16, Section 7:
- `.` represents an empty step.
- `..` moves one step back along the path.
- Multiple consecutive `/` characters are equivalent to one.
- A trailing `/` is accepted and has no effect.

A `..` component that attempts to move above the volume root returns `BADPATH`.

An FS16 **name** used as a path component must be 1–32 bytes long, with every byte at least `$20` and neither `/` nor `:` permitted (FS16, Section 7). The special components `.` and `..` are path-navigation steps, not file names. A name longer than 32 bytes or containing a prohibited byte returns `BADNAME`. A colon is allowed only in the path prefix; any other colon returns `BADNAME`.

**Limits:**
- A path longer than 255 bytes returns `BADPATH`.
- After resolving `.` and `..`, a path from the volume root may contain at most 16 names; exceeding this depth returns `BADPATH`.
- An empty path (`""`) returns `BADPATH`.

**Lookup failures:**

| Condition | Error |
| --- | --- |
| volume is not mounted | `NODEV` |
| intermediate component does not exist | `NOENT` |
| intermediate component is a file rather than a directory | `NOTDIR` |
| final component does not exist | `NOENT` |

The last error applies when a call is not creating an object; creation calls specify their own behavior.

Names are compared without regard to letter case (FS16, Section 7.1): `A:/BIN/LS.CX` and `A:/bin/ls.cx` denote the same file. **Original spelling is preserved:** when creating or renaming an object, the final component is stored as provided; `READDIR` and `GETCWD` return the names stored on disk. For example, if a directory is named `bin`, `CHDIR /BIN` followed by `GETCWD` returns `A:/bin`.

Volume prefixes `X:`, the prefix `dev:`, and device names (Section 4.2) are not FS16 file names and are exempt from the preceding comparison rule.

### 4.2. Devices

Device paths begin with `dev:`:

| Path | Device |
| --- | --- |
| `dev:con` | console: reads receive keyboard input (Section 6); writes produce display output |
| `dev:nul` | null device: reads immediately report end-of-file; writes accept and discard data |

Other names following `dev:` return `NOENT`. Devices can be opened with `OPEN` and queried using `STAT`; creating, deleting or renaming them, or making a device the current directory, returns `INVAL`.

### 4.3. Current directory

Each process has one current directory, represented by a volume and a path relative to its root.

- **Inheritance:** a child receives a copy of its parent's current directory. A child's `CHDIR` does not affect the parent.
- **Representation:** `GETCWD` returns `X:/a/b`, with an uppercase volume letter and no trailing slash; the root is `X:/`.
- **Unmounted volume:** if the volume holding the current directory is unmounted or its disk is disconnected, paths without a volume prefix return `NODEV` until the process changes directory.
- **Deletion and renaming:** a directory cannot be deleted or renamed if it is the current directory of any process in a process chain or an ancestor of such a directory; the error is `BUSY`.

## 5. Files and Descriptors

### 5.1. Descriptors

Process descriptors are integers in the range 0–15:

| Descriptor | Purpose |
| --- | --- |
| 0 | standard input |
| 1 | standard output |
| 2 | standard error |

There is no other special distinction between descriptors. `OPEN` allocates the lowest free descriptor, as does `DUP` when its target is `$FFFF` (Section 9.3). An out-of-range or unopened descriptor produces `BADF`.

**Inheritance.** A child receives only descriptors 0, 1 and 2, as specified by its parent in `SPAWN` (Section 7.1). No other parent descriptors are inherited.

**Closing on termination.** All descriptors are closed when the process terminates (Section 7.2).

### 5.2. Open-file records and sharing

Each `OPEN` creates a new open-file record with its own file position. `DUP` and descriptor inheritance through `SPAWN` create another descriptor referring to the **same** record; those descriptors share a file position. A record closes when its last referencing descriptor is closed. The entire system may have at most 32 open-file records; exhausting them returns `MFILE`.

**Sharing rules:**
- A file may be opened multiple times, including simultaneous read and write access.
- Changes through one record are immediately visible through other records; data and file size are shared.
- Each record has its own position. If a file is shortened with `SETSIZE`, another record's position beyond the new end remains unchanged: reads from that position report EOF and writes return `INVAL`.

**Open objects:** an open file or directory cannot be deleted or renamed; doing so returns `BUSY`.

### 5.3. Volumes

**Mounting.** At startup, the Monitor attempts to mount each connected disk with a valid FS16 superblock (FS16, Section 3). Subsequent mounts use `MOUNT`.

| Disk state | Mount behavior |
| --- | --- |
| disk is write-protected (RO in the INFO block) | read-only |
| DIRTY flag is set in the superblock | read-only; the Monitor reports this at startup and `MOUNT` returns the condition in a result flag. A program such as `fsck` checks and repairs the volume using `DISK_IO`; it then remounts the volume read-write using `MOUNT` with `MOUNT_REMOUNT` |
| otherwise | read-write; the Monitor sets DIRTY (FS16, Section 10) |

Filesystem writes to a volume mounted read-only return `RDONLY`; raw sector access through `DISK_IO` follows separate rules (Section 9.8).

**Checking a DIRTY volume.** Raw `DISK_IO` writes are allowed even when the Monitor has mounted a volume read-only, provided the physical disk permits writing (Section 9.8); the filesystem layer itself does not write to that volume. After a sector write, the Monitor discards the disk-sector cache for that disk. Once checking has cleared DIRTY, `MOUNT` with `MOUNT_REMOUNT` can mount the volume read-write again. This also permits repairing volume 0, from which programs were loaded: no files on that volume remain open during the repair, and a process using it as its current directory does not prevent remounting.

**Unmounting** writes deferred changes, clears DIRTY and issues a disk FLUSH. An ordinary `UNMOUNT` returns `BUSY` while the volume contains an open file or is referenced by any process's current directory. `SHUTDOWN` first terminates the processes in all sessions and then unmounts the volumes.

**Disk removal.** Before accessing a mounted volume, the Monitor checks whether its disk is still connected. If the disk has been removed (possible with REMOVABLE disks; machine, Section 6):
- the volume is marked lost;
- filesystem operations on that volume, including operations through already-open descriptors, return `NODEV`; those descriptors can still be closed with `CLOSE`;
- the lost volume remains in the mount table until `UNMOUNT` removes it without writing to the disk. After `UNMOUNT` and reconnection, the disk can be mounted again with `MOUNT`.

### 5.4. Disk write ordering

The Monitor performs FS16 operations in the order specified by Sections 9–10 of the FS16 specification. The following guarantees apply to programs:

- Data passed to `WRITE` has been written to disk before the call returns.
- File size and address fields in the directory entry are written before a modifying call returns (`WRITE`, `SETSIZE`, `OPEN` with `TRUNC`).
- A file's modification time (MTIME) is updated whenever its data or size changes (`WRITE`, `SETSIZE`, `OPEN` with `O_TRUNC`), and is written to disk with its directory entry before the call returns. A directory's MTIME changes when entries within it are created, deleted or renamed.
- Updates that mark freed clusters as available in the allocation bitmap may be deferred until `SYNC`, `UNMOUNT` or `SHUTDOWN` (FS16, Section 10). If power is lost before those updates reach disk, the clusters can remain marked as allocated even though nothing references them. This leaks space without compromising the volume's structural integrity.

`SYNC` writes deferred changes and issues a disk FLUSH for each volume mounted read-write. It does not clear DIRTY; that flag is cleared by a successful clean unmount (FS16, Section 10).
## 6. Console

### 6.1. Input modes

The Monitor owns the keyboard. At every tick, it transfers events from the hardware keyboard queue into its own buffer, which holds up to 256 events. Excess events are discarded.

`CON_MODE` controls input behavior using the following bit flags:

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `RAW` | 0 = line mode (Section 6.2); 1 = raw mode |
| 1 | `NOECHO` | suppress echo of typed characters in line mode |
| 2 | `NOBREAK` | treat Ctrl+C as a normal character, not an interrupt (Section 6.3) |

The mode is process-specific. A new process starts in mode 0 (line editing with echo; Ctrl+C interrupts). When a child terminates, the Monitor restores its parent's mode.

**Line mode.** A console `READ` receives a line entered and edited by the user (Section 6.2).

**Raw mode.** A console `READ`:
- waits until at least one event with a nonzero character is available;
- returns characters from all such available events, up to the requested count;
- performs no echo or editing; events with no character (arrow keys, F1–F12) are discarded.

**Ctrl+Enter** produces an event with character 0 and scan code `$1C` (machine, Section 2.6). Line mode ignores it and raw `READ` discards it; only `CON_GETEV` can distinguish Ctrl+Enter from Enter.

`CON_GETEV` provides complete keyboard events, including scan codes and modifier bits. It works in either mode and removes the next event from the Monitor buffer. It does not retrieve characters already consumed into a partially entered line in line mode.

### 6.2. Line input

The Monitor collects a line of up to 255 characters and makes it available to `READ` after Enter.

| Key | Character | Action |
| --- | --- | --- |
| printable characters `$20` and above; Tab | | append to the line; ignored when the buffer is full |
| Backspace | `$08` | remove the last character and erase it from the screen (`\b`, space, `\b`) |
| Ctrl+U | `$15` | clear the line and erase it from the screen |
| Enter | `$0A` | complete the line: append `\n` and output a newline |
| Ctrl+Z at the start of a line | `$1A` | end-of-file; `READ` returns zero bytes |
| Ctrl+C | `$03` | see Section 6.3 |
| other control characters and characterless events | | ignored |

Ctrl+Z is ignored when the line is not empty.

`READ` returns no more bytes than requested. Any remaining bytes from the same line are returned by subsequent `READ` calls; a new line is not started until the preceding line has been fully consumed.

### 6.3. Ctrl+C

Unless `NOBREAK` is enabled for the process, Ctrl+C is an **interrupt**. The Monitor detects it on a timer tick, regardless of what the process is doing:

1. The keyboard event buffer and unfinished line are discarded; the Monitor outputs `^C` followed by a newline.
2. If the process is executing outside the Monitor, it is terminated with exit code `$8001` (Section 7.2).
3. If the process is blocked in a system call (Section 3.4), the call returns `INTR` and the process then terminates with `$8001`.
4. If the process is executing a nonblocking system call, that call completes before the process terminates with `$8001`.

If a **Ctrl+C event handler** has been installed using `ONEVENT` (Section 7.5), it runs instead of process termination in cases 2–4. The handler runs immediately in case 2 and after the system call returns in cases 3 and 4; a blocked call in case 3 returns `INTR` before the handler is entered.

The interrupt targets the currently running process, which is the last process in its chain. If Ctrl+C terminates that process rather than being handled, its parent (if it has one) resumes and receives `$8001` as the child's exit status.

With `NOBREAK` enabled, Ctrl+C does not terminate the process. Instead, the input subsystem receives it as character `$03`, with behavior determined by the input mode:
- In raw mode it is delivered like any other input event.
- In line mode it cancels the current line: the line is discarded, the Monitor prints `^C` and a newline, and a pending `READ` returns `INTR`.

This allows a shell with `NOBREAK` enabled to cancel a partially entered command without terminating itself.

### 6.4. Output and display

A console `WRITE` sends bytes consecutively to the DATA register without transformation (machine, Section 2.3). In teletype mode, `\n` advances to the next line.

Programs may control the display directly through DATA, CTRL, ATTR, COL, ROW and CMD. This is faster than system calls and supports full-screen applications. Direct writes bypass standard-output redirection and always appear on the display.

Line-input echo and Monitor messages are written to DATA using the current CTRL and ATTR settings. If a program has enabled coordinate display mode, its echo follows that mode's rules.

When a child terminates, the Monitor restores the values of CTRL and ATTR saved when it started. Screen contents and cursor position are not restored.

The SCRD register provides screen readback (machine, Section 2.2): a word read at `$FF50` yields the character and attribute at the current readback position and advances that position. The Monitor saves and restores the screen, CTRL, ATTR and position on session switches (Section 2.4); programs need take no action.

## 7. Processes

### 7.1. Starting a child

`SPAWN` takes a command line of at most 255 bytes and three parent descriptors that supply the child's standard input, output, and error streams.

1. **Parsing.** The command line is parsed as specified by the CX16 format, Section 5.2. Its first word is a path to the program file (Section 4.1). The Monitor does not search directories or append a file extension; the shell is responsible for that. `argv[0]` is the first command-line word, unchanged.
2. **Loading.** The file is loaded according to the CX16 format, Section 9.1; failures use the error codes in Section 8. If loading fails, no child runs, the parent remains current, and the call returns an error rather than a child exit status.
3. **Standard streams.** Child descriptors 0, 1 and 2 refer to the same open-file records as the selected parent descriptors (Section 5.2). For each stream, `$FFFF` selects the parent descriptor with the same number (0, 1 or 2); otherwise the supplied number identifies a parent descriptor explicitly. Every selected parent descriptor must be open, or `SPAWN` returns `BADF`.
4. **PIB.** The Monitor sets redirection flags in the PIB (CX16 format, Section 4):
   - bit 0 is set if child descriptor 0 is not the console;
   - bit 1 is set if child descriptor 1 is not the console.
5. **Child environment:** a copy of the parent's current directory, console mode 0, and no tick or event handlers.
6. **Return.** Once the child has started, the parent waits in `SPAWN` until that child terminates. The completed call then returns R0 = `OK` and R1 = the child's exit code, including Monitor-generated codes (`$8000` and above).

`SPAWN` returns `NOPROC` if the process chain already contains 16 processes.

### 7.2. Termination and exit codes

A process can terminate by:
- calling `EXIT` with its exit code in R0;
- returning to the initial LR (the Monitor exit stub at `$E000`, CX16 format, Section 8.1), also with the exit code in R0;
- being terminated by the Monitor (for example, on Ctrl+C or an illegal instruction).

When a child process terminates, the Monitor performs the following cleanup and restores the waiting parent (the distinct rules for a session root are in Sections 2.3–2.4):
1. closes all process descriptors;
2. removes tick and event handlers;
3. restores the parent's console mode, CTRL and ATTR (Sections 6.1 and 6.4);
4. releases the process's memory;
5. resumes the parent from its `SPAWN` call.

**Exit codes:**

| Code | Meaning |
| --- | --- |
| `$0000` | success |
| `$0001–$7FFF` | application-defined exit codes |
| `$8000–$FFFF` | reserved by the Monitor |
| `$8001` | interrupted by Ctrl+C |
| `$8008` | illegal instruction |
| `$8009` | other causes of Monitor-initiated process termination |

Applications must avoid using the Monitor-reserved range `$8000–$FFFF` for their own exit codes. The Monitor does not enforce this convention: it passes the supplied code to the parent unchanged.

### 7.3. Illegal instruction

When a process triggers the processor's `ERRCMD` exception by executing an invalid instruction, the Monitor:
- prints `monitor: illegal instruction at $XXXX` directly to the console, bypassing redirection;
- terminates the process with code `$8008`.

If the process has installed an illegal-instruction handler (`ONEVENT`, Section 7.5), the handler runs instead, and no message is printed.

### 7.4. Tick handler

`TICKHOOK` installs a process-specific timer-tick handler.

The Monitor invokes the handler **after processing the timer tick**, but only when that tick interrupted process code rather than Monitor code. Ticks that occur while a process is executing or blocked inside a system call are not delivered to its handler, either immediately or later.

The handler is entered with the standard interrupt stack frame:
- `[SP]` contains FLAGS; `[SP+2]` contains the address at which the interrupted program would have continued.
- IF is 0.
- Registers contain the interrupted program's values.

The handler must preserve every register it changes and return using `IRET`. In B16, declare it with `interrupt fn` (B16, Section 9.5).

Handler rules:
- Keep execution shorter than one tick; ticks occurring during the handler are coalesced (machine, Section 3).
- **Do not make system calls** from a tick handler; the result is undefined.
- On entry, the CPU has already used 4 bytes of the process stack for the interrupt frame. Any stack space used by the handler is additional; the program must allow for both at every possible interruption point.

A handler belongs to its process: children do not inherit it; the parent's handler is not invoked while its child runs; termination removes the handler.

### 7.5. Event handlers

`ONEVENT` registers a handler for one of the following process events:

| Event | Constant | When invoked | Saved return address |
| --- | --- | --- | --- |
| Ctrl+C | `EVT_BREAK` | instead of termination with `$8001` (Section 6.3) | interruption point: instruction executing at the tick, or address following the system call |
| illegal instruction | `EVT_BADINSN` | instead of termination with `$8008` (Section 7.3) | first word of the illegal instruction (processor, Section 8.4) |

An event handler is entered with an interrupt-style stack frame:
- `[SP]` contains FLAGS, and `[SP+2]` contains the return address from the table.
- IF is 0.
- Registers contain the interrupted program's values. If Ctrl+C occurred during a system call, R0–R3 hold that call's results (`INTR` in case 3 of Section 6.3), and the saved FLAGS reflect the call's final flags, with ZF derived from R0.

The handler must preserve the registers it modifies and return with `IRET` (in B16, `interrupt fn`). Execution resumes at the IP saved in the frame. An illegal-instruction handler must adjust that saved IP by the instruction length if it intends to skip or emulate the instruction; otherwise `IRET` retries the same invalid instruction. A handler may instead call `EXIT` without returning.

Unlike a tick handler, an **event handler is allowed to make system calls** because it runs as normal program code.

Stack requirements: when Ctrl+C is delivered after a system call, the Monitor places a 4-byte system-call return frame on the process stack; entry into the handler requires another 4-byte interrupt frame. Thus, 8 bytes below the original SP must be available. Otherwise (for example, if SP is below 8 or above `top`), the process terminates with `$8001` as if no handler were registered.

Because a handler starts with IF = 0, another Ctrl+C is normally deferred until the handler returns. If the handler enables interrupts or waits inside a system call, however, Ctrl+C may invoke it again before the first invocation finishes.

Event handlers are process-specific: children do not inherit them, and a parent's handlers do not run while its child is active. `EXEC` and process termination clear the registrations. With `NOBREAK` enabled, Ctrl+C is treated as ordinary input, so the Ctrl+C handler is not called.

### 7.6. Process memory and extended memory

**Private memory** consists of `top / $2000` frames mapped into pages 0 through `top / $2000 − 1` (CX16 format, Section 6). `SETTOP` grows or shrinks that allocation in whole-page increments.

**Extended memory** consists of additional frames beyond the private allocation. `XALLOC` provides a zero-filled frame, `XMAP` maps it into any page from 0 through 6, and `XFREE` releases it. This allows programs to work with more than 56 KiB by switching frames through one or two window pages.

- **Frame number:** a physical frame number from 14 through 255. The application receives it from `XALLOC` and supplies it to `XMAP` and `XFREE`. Frames owned by other processes, and its own private frames, are rejected by these calls.
- **Window:** a page mapped to an extended-memory frame counts as process memory (Section 3.3), even above `top`; it may contain call buffers, stacks or code. Applications must take care when mapping over a page containing executing code or a stack: the new mapping takes effect immediately after `XMAP` returns.
- **No protection:** writing directly to memory-manager registers remains forbidden (Section 2.2). The Monitor maintains each process's page map and restores it on every return from a system call.
- **Ownership:** extended-memory frames belong to their process; children do not inherit them, and both `EXIT` and `EXEC` release them along with private memory.
- **No per-process limit:** if no free frames remain, `XALLOC`, `SETTOP` and program launches through `SPAWN`/`EXEC` return `NOMEM`.

## 8. Error Codes

| Code | Name | Meaning |
| --- | --- | --- |
| 0 | `OK` | success |
| 1 | `NOSYS` | unknown or reserved call number |
| 2 | `INVAL` | invalid argument or operation unsupported for this object type |
| 3 | `FAULT` | buffer, string or address outside process memory |
| 4 | `NOENT` | file, directory or device not found |
| 5 | `EXIST` | an object with that name already exists |
| 6 | `NOTDIR` | an intermediate path component is not a directory, or the operation requires a directory |
| 7 | `ISDIR` | operation requires a file but was applied to a directory |
| 8 | `NOTEMPTY` | directory is not empty |
| 9 | `RDONLY` | write to a file/directory marked RDONLY, a read-only volume, or a write-protected disk |
| 10 | `NOSPC` | volume is full or the file reached the FS16 size limit |
| 11 | `BADNAME` | invalid file name (Section 4.1) |
| 12 | `BADPATH` | invalid path: length, depth, `..` at root, or empty path |
| 13 | `BADF` | invalid descriptor or descriptor not opened with the required access mode |
| 14 | `MFILE` | no free descriptor or open-file record |
| 15 | `NODEV` | disk not present, volume not mounted, or disk disconnected |
| 16 | `IO` | disk I/O error |
| 17 | `NOEXEC` | file is not executable (CX16 format, Section 3.3) |
| 18 | `BADVER` | unsupported CX16 format version |
| 19 | `ARGS` | command-line error (CX16 format, Section 5.2) |
| 20 | `NOMEM` | insufficient memory |
| 21 | `NOPROC` | process chain is full |
| 22 | `AGAIN` | no data currently available (nonblocking call) |
| 23 | `BUSY` | object is in use: open, used as current directory, or volume mounted |
| 24 | `INTR` | blocking call interrupted by Ctrl+C |
| 25 | `XDEV` | attempt to rename across volumes |
| 26 | `NOTFS` | disk does not contain a valid FS16 volume |

Program loading failures (CX16 format, Section 9.1):

| Load failure | Error |
| --- | --- |
| path not found | `NOENT` (or other path errors from Section 4.1; `ISDIR` if the target is a directory) |
| file not executable | `NOEXEC` |
| unsupported version | `BADVER` |
| invalid command-line arguments | `ARGS` |
| insufficient memory | `NOMEM` |
| I/O error | `IO` |
## 9. System Calls

### 9.1. Summary

| No. | Name | Purpose | No. | Name | Purpose |
| --- | --- | --- | --- | --- | --- |
| `$00` | `EXIT` | terminate process | `$29` | `MOUNT` | mount volume |
| `$01` | `SPAWN` | start child and wait | `$2A` | `UNMOUNT` | unmount volume |
| `$02` | `EXEC` | replace process program | `$2B` | `SYNC` | flush deferred changes |
| `$03` | `ONEVENT` | install event handler | `$2C` | `SETMTIME` | set modification time |
| `$10` | `OPEN` | open object | `$30` | `CON_MODE` | console mode |
| `$11` | `CLOSE` | close descriptor | `$31` | `CON_GETEV` | keyboard event |
| `$12` | `READ` | read | `$32` | `CON_FLUSH` | discard input |
| `$13` | `WRITE` | write | `$40` | `MEMINFO` | memory information |
| `$14` | `SEEK` | set/get position | `$41` | `SETTOP` | resize process memory |
| `$15` | `DUP` | duplicate descriptor | `$42` | `XALLOC` | allocate extended-memory frame |
| `$16` | `HINFO` | descriptor type and mode | `$43` | `XFREE` | free frame |
| `$17` | `SETSIZE` | change file size | `$44` | `XMAP` | map frame to page |
| `$18` | `FSTAT` | opened-object information | `$50` | `TIME_GET` | current time |
| `$20` | `STAT` | file information | `$51` | `TIME_SET` | set time |
| `$21` | `DELETE` | delete object | `$52` | `TICKS` | tick count |
| `$22` | `RENAME` | rename/move object | `$53` | `SLEEP` | wait for ticks |
| `$23` | `MKDIR` | create directory | `$54` | `TICKHOOK` | tick handler |
| `$24` | `CHDIR` | change current directory | `$60` | `DISK_INFO` | disk information |
| `$25` | `GETCWD` | get current directory | `$61` | `DISK_IO` | sector I/O |
| `$26` | `READDIR` | next directory entry | `$62` | `SYSINFO` | system information |
| `$27` | `SETATTR` | change attributes | `$63` | `SHUTDOWN` | shut down system |
| `$28` | `VOLINFO` | volume information | `$70` | `SESS_INFO` | session/process information |
|  |  |  | `$71` | `SESS_NEW` | create session |
|  |  |  | `$72` | `SESS_SWITCH` | switch session |
|  |  |  | `$73` | `SESS_KILL` | terminate session |

**Notation.** Each call below is given as a B16-style prototype. Arguments and return values occupy R0–R3 according to the slot convention (Section 3.1). The first return value is always the error code in R0. Structures are defined in Section 10.2. `FAULT` for invalid buffers/pointers (Section 3.3) and path-resolution errors (Section 4.1) apply to all calls accepting the corresponding arguments and are not repeated in every error list.

### 9.2. Processes

#### `$00` EXIT — terminate a process

```text
fn sys_exit(code: uint)
```

R0 = exit code. Terminates the process (Section 7.2); does not return.

#### `$01` SPAWN — launch a child process

```text
fn sys_spawn(cmd: ptr byte, fin: uint, fout: uint, ferr: uint): (uint, uint)
```

R0 = command line; R1–R3 = open parent descriptors to use for the child's streams 0, 1 and 2, respectively (`$FFFF` selects the corresponding parent descriptor number). After the child exits, R1 contains its exit code.

If the child is launched successfully, the caller waits until it exits (Section 7.1). If launch fails, `SPAWN` returns the error directly without entering a child.

Errors: `BADF` (a selected parent descriptor is invalid or closed), `ARGS` (command line exceeds 255 bytes, too many arguments, or unmatched quote), load errors (Section 8), `NOPROC`.

#### `$02` EXEC — replace the current program

```text
fn sys_exec(cmd: ptr byte): uint
```

R0 = command line (at most 255 bytes). Loads a program in the same way as `SPAWN` (Section 7.1, steps 1–2), but replaces the calling program **within the same process**. On success, the call does not return: the new program starts in the CX16-defined initial state (Section 8.1).

**Preserved:** the process's place in its chain (the parent eventually receives the replacement program's exit code), its open descriptors, and its current directory.

**Reset:** the program image and its memory allocation, console mode (set to 0), tick handler, and event handlers. PIB redirection flags are determined from descriptors 0 and 1 of the existing process.

**Memory requirement:** the replacement is loaded into newly allocated frames while the current program remains intact. Only after loading succeeds does the Monitor free the old image and transfer ownership of the new frames. A load failure returns an error to the original program without resetting its handlers or replacing its memory. Both images must fit simultaneously during loading; `NOMEM` may therefore occur even if the replacement would fit after the old image was freed.

Errors: `ARGS`, `FAULT`, load errors (Section 8).

#### `$03` ONEVENT — register an event handler

```text
fn sys_onevent(ev: uint, h: ptr interrupt fn()): (uint, ptr interrupt fn())
```

R0 = event (`EVT_BREAK` or `EVT_BADINSN`); R1 = handler address (Section 7.5), or 0 to remove the handler. Returns the previous handler address for this event in R1, or 0.

Errors: `INVAL` (unknown event), `FAULT` (handler address at or above `top`, or odd address).

### 9.3. Descriptors and I/O

**Open mode flags** (`OPEN` argument `mode`):

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `O_READ` | read access |
| 1 | `O_WRITE` | write access |
| 2 | `O_CREATE` | create the file if it does not exist |
| 3 | `O_TRUNC` | truncate to zero length; requires `O_WRITE` |
| 4 | `O_EXCL` | together with `O_CREATE`, fail if the file already exists |
| 5 | `O_APPEND` | position at EOF before each write |

#### `$10` OPEN — open an object

```text
fn sys_open(path: ptr byte, mode: uint, attr: uint): (uint, uint)
```

R0 = path; R1 = mode; R2 = flags for a newly created file (`A_RDONLY`, `A_HIDDEN`, `A_SYSTEM`, used only for creation). Returns the lowest available descriptor number in R1.

**Object types:**
- A **file** is opened with `O_READ`, `O_WRITE`, or both. Its initial position is zero.
- A **directory** can only be opened with `O_READ` and no other mode bits. Use `READDIR` to read it.
- A **device** (`dev:con` or `dev:nul`) is opened with `O_READ`, `O_WRITE`, or both; all other bits are ignored.

**Creation.** With `O_CREATE`, if the final path component does not exist, an empty file is created with the supplied `attr` flags and MTIME set to the current time (FS16, Section 9). Name lookup is case-insensitive (Section 4.1): if a file already exists with a name differing only in case, it is opened without changing its spelling. A newly created file with `A_RDONLY` is still writable through this particular descriptor if `O_WRITE` was specified.

**`O_APPEND`.** Before each write, the file position moves to EOF. Reads use the current position.

Errors:
- `INVAL`: neither `O_READ` nor `O_WRITE` is set, or `O_TRUNC` is used without `O_WRITE`;
- `NOENT`: no matching file and `O_CREATE` is absent;
- `EXIST`: `O_CREATE | O_EXCL` was specified and the file already exists;
- `ISDIR`: directory opened with `O_WRITE`, `O_CREATE` or `O_TRUNC`;
- `RDONLY`: write access to an RDONLY file, creation in an RDONLY directory, or a write on a read-only volume;
- `NOSPC`: insufficient space for a new directory entry;
- `MFILE` (no free descriptor or open-file record), `NODEV`, `IO`.

#### `$11` CLOSE — close a descriptor

```text
fn sys_close(h: uint): uint
```

Closes a descriptor. Any disk writes required by prior file-modifying calls have already been performed (Section 5.4); `CLOSE` performs no additional disk write. Errors: `BADF`, `IO` (the descriptor is closed even when this error occurs).

#### `$12` READ — read bytes

```text
fn sys_read(h: uint, buf: ptr, n: uint): (uint, uint)
```

R0 = descriptor, R1 = buffer, R2 = byte count. R1 returns the number of bytes read, **even on error**.

| Object | Behavior |
| --- | --- |
| file | reads up to n bytes from the current position and advances it; returns 0 at EOF |
| console | follows the input mode (Sections 6.1–6.2); waits for input |
| `dev:nul` | returns 0 |

Errors: `BADF` (not opened for reading), `ISDIR` (directory), `INTR`, `NODEV`, `IO`.

#### `$13` WRITE — write bytes

```text
fn sys_write(h: uint, buf: ptr, n: uint): (uint, uint)
```

R0 = descriptor, R1 = buffer, R2 = byte count. R1 returns the number of bytes written, **even on error**.

| Object | Behavior |
| --- | --- |
| file | writes at the current position, extending the file if needed, and advances the position; data and size are on disk before return (Section 5.4) |
| console | outputs bytes (Section 6.4) |
| `dev:nul` | R1 = n |

If space runs out, `WRITE` writes as many bytes as will fit and returns `NOSPC`; R1 still reports the number of bytes actually written. A position beyond EOF (after another descriptor called `SETSIZE`; Section 5.2) yields `INVAL`.

Errors: `BADF` (not opened for writing), `ISDIR`, `NOSPC`, `RDONLY`, `INVAL`, `NODEV`, `IO`.

#### `$14` SEEK — get/set file position

```text
fn sys_seek(h: uint, off: long, whence: uint): (uint, ulong)
```

R0 = descriptor, R1:R2 = offset, R3 = origin: 0 for beginning, 1 for current position, 2 for EOF. Returns the new position in R1:R2. `SEEK(h, 0, 1)` queries the current position.

- The new position must be between zero and file size, inclusive. Otherwise the call returns `INVAL` and leaves the position unchanged. FS16 does not support sparse files.
- **Directory:** only `SEEK(h, 0, 0)` is permitted; the next `READDIR` starts at the first entry.
- **Console and `dev:nul`:** these have no file position; return `INVAL`.

Errors: `BADF`, `INVAL`.

#### `$15` DUP — duplicate a descriptor

```text
fn sys_dup(h: uint, target: uint): (uint, uint)
```

R0 = source descriptor; R1 = desired target number or `$FFFF`. Returns the new descriptor in R1; it references the same open-file record (Section 5.2).

- With `target = $FFFF`, the lowest free descriptor is allocated.
- Otherwise, an already-open `target` descriptor is closed first.
- `target = h` is a no-op.

Errors: `BADF` (invalid or unopened source, or explicit target outside 0–15); `MFILE` (no free descriptor when `target = $FFFF`).

#### `$16` HINFO — descriptor information

```text
fn sys_hinfo(h: uint): (uint, uint, uint)
```

Returns R1 = object type (0 = file, 1 = directory, 2 = console, 3 = `dev:nul`), R2 = opening mode (`O_…` bits). Error: `BADF`.

#### `$17` SETSIZE — resize a file

```text
fn sys_setsize(h: uint, size: ulong, flags: uint): uint
```

R0 = descriptor; R1:R2 = new size; R3 = flags, where bit 0 `SZ_ZERO` requests that newly exposed bytes be zero-filled. The descriptor's position does not change unless the file is truncated below that position, in which case it moves to the new EOF.

**Shrinking.** Surplus clusters are freed as specified by FS16, Section 9 (“Truncation”).

**Growing.** Clusters are added to reach the new size as specified by FS16, Section 9 (“Adding a Cluster”). The directory entry is written once, at the end:
- Without `SZ_ZERO`, data in new clusters is not written to disk. All bytes between the old and new EOF are **undefined**; they may contain anything, including data from deleted files. This is fast because it only updates the allocation bitmap, index clusters and directory entry.
- With `SZ_ZERO`, the Monitor writes zeros to every byte between the old and new EOF, including any unused tail of the old final cluster. This requires writing every additional sector.

If space is insufficient, the file size remains unchanged: any newly allocated clusters are released and the call returns `NOSPC`. A requested size greater than 2^27 − 1 also returns `NOSPC`.

If the size is unchanged, the call does nothing.

Errors: `BADF` (descriptor not writable), `INVAL` (not a file, or unsupported flag bits), `NOSPC`, `RDONLY`, `NODEV`, `IO`.

#### `$18` FSTAT — information about an open object

```text
fn sys_fstat(h: uint, st: ptr Stat): uint
```

Fills `Stat` (Section 10.2) from the current state of the open object. Errors: `BADF`, `NODEV`.
### 9.4. File system

#### `$20` STAT — file information

```text
fn sys_stat(path: ptr byte, st: ptr Stat): uint
```

Fills `Stat` (Section 10.2) with the first 40 bytes of an FS16 directory entry: name, size and attribute flags, and MTIME.
- For a volume root, the name is empty and the DIR flag is set.
- For a device, the name is the device name (`con`, `nul`), size is 0, attributes are clear, and MTIME is 0.

Errors: path-related errors, `NODEV`, `IO` only.

#### `$21` DELETE — delete an object

```text
fn sys_delete(path: ptr byte): uint
```

Deletes a file or an empty directory (FS16, Section 9).

Errors:
- `RDONLY`: the object or its containing directory has the RDONLY attribute, or the volume is read-only;
- `NOTEMPTY`;
- `BUSY`: object is open or used as a current directory (Sections 4.3 and 5.2);
- `INVAL`: volume root or device;
- `NODEV`, `IO`.

#### `$22` RENAME — rename or move an object

```text
fn sys_rename(old: ptr byte, new: ptr byte): uint
```

Renames an object or moves it to another directory on the same volume (FS16, Section 9). The object's own MTIME does not change. The object's RDONLY flag does not prohibit renaming.

**Case-only rename.** If `new` identifies the same object as `old` (for example, `readme` → `README`), the call changes only the spelling of its name: the NAME field is replaced with the last component of `new`. This does not produce `EXIST`. If the spellings are byte-for-byte identical, the call succeeds without changing anything.

Errors:
- `EXIST`: `new` already identifies a different object (case-insensitive comparison);
- `XDEV`: source and destination are on different volumes;
- `INVAL`: attempt to move a directory into one of its own subdirectories, or to rename a volume root or device;
- `RDONLY`: the directory containing the source or the destination has the RDONLY attribute, or the volume is mounted read-only;
- `BUSY`: object is open or used as a current directory;
- `NOSPC`, `NODEV`, `IO`.

#### `$23` MKDIR — create a directory

```text
fn sys_mkdir(path: ptr byte): uint
```

Creates an empty directory. Errors: `EXIST` (an object already has that name, ignoring case), `RDONLY`, `NOSPC`, `INVAL` (device), `NODEV`, `IO`.

#### `$24` CHDIR — change the current directory

```text
fn sys_chdir(path: ptr byte): uint
```

Errors: `NOTDIR` (path identifies a file), `INVAL` (device), `NODEV`, `IO`.

#### `$25` GETCWD — get the current directory

```text
fn sys_getcwd(buf: ptr byte, n: uint): (uint, uint)
```

Writes the current-directory path in the form specified by Section 4.3, including its terminating zero. Returns the path length, excluding the terminator, in R1. Error: `INVAL` if n bytes are insufficient (the buffer is left unchanged).

#### `$26` READDIR — next directory entry

```text
fn sys_readdir(h: uint, st: ptr Stat): (uint, uint)
```

R0 = a directory descriptor opened with `OPEN`. Copies the next occupied directory entry into `Stat`. Returns R1 = 1 if an entry was found, or 0 at the end of the directory (in which case `Stat` is unchanged).

Entries are returned in their on-disk order. Free slots are skipped; entries marked HIDDEN or SYSTEM are included. If the directory changes during enumeration, each entry that remains unchanged throughout the enumeration is still returned exactly once.

Errors: `BADF`, `NOTDIR` (descriptor is not a directory), `NODEV`, `IO`.

#### `$27` SETATTR — change object attributes

```text
fn sys_setattr(path: ptr byte, attr: uint): uint
```

Sets the object's RDONLY, HIDDEN and SYSTEM flags using `A_RDONLY`, `A_HIDDEN`, `A_SYSTEM` (Section 10.2). The DIR flag is not changed; any unsupported bits return `INVAL`. This call may clear RDONLY. MTIME does not change.

Errors: `INVAL` (root, device, unsupported bits), `RDONLY` (read-only volume), `NODEV`, `IO`.

#### `$28` VOLINFO — volume information

```text
fn sys_volinfo(drv: uint, vi: ptr VolInfo): uint
```

Fills `VolInfo` (Section 10.2) with volume label, serial number, total and free cluster counts, and mount flags. Errors: `NODEV`, `IO`.

#### `$29` MOUNT — mount a volume

```text
fn sys_mount(drv: uint, flags: uint): (uint, uint)
```

R0 = disk number 0–7; R1 = flags: bit 0 `MOUNT_RDONLY` forces read-only access; bit 1 `MOUNT_REMOUNT` requests remounting. Mounts the volume (Section 5.3). Returns `VolInfo.flags` in R1, allowing callers to determine whether the volume is read-only and why.

**`MOUNT_REMOUNT`** applies only to an already mounted volume. If it is mounted read-write, the Monitor first writes deferred changes. It then discards the disk-sector cache, rereads the superblock, and mounts the volume again under the normal rules. Processes keep their current directories on that volume. If the new mount fails, the volume is left unmounted. Use this flag after checking or repairing a volume (Section 5.3).

Errors: `NODEV` (disk absent; for `MOUNT_REMOUNT`, volume not mounted or lost), `BUSY` (already mounted without `MOUNT_REMOUNT`; with it, files or directories still open on the volume), `INVAL` (unsupported flag bits), `NOTFS`, `IO`.

#### `$2A` UNMOUNT — unmount a volume

```text
fn sys_unmount(drv: uint): uint
```

Errors: `NODEV` (volume not mounted), `BUSY` (volume still in use; Section 5.3), `IO`.

#### `$2B` SYNC — flush deferred changes

```text
fn sys_sync(): uint
```

Writes deferred changes on volumes mounted read-write and issues a disk FLUSH for each (Section 5.4). Error: `IO`.

#### `$2C` SETMTIME — set modification time

```text
fn sys_setmtime(path: ptr byte, t: ulong): uint
```

R0 = path, R1:R2 = timestamp in FS16 format (FS16, Section 5.1), or 0 to indicate no timestamp. Updates the MTIME of a file, directory or volume root. Neither the object's flags nor RDONLY prevent this change. Open descriptors immediately reflect the updated value; the next change to file contents or size updates MTIME again (Section 5.4).

Errors: `INVAL` (invalid timestamp field, or device), `RDONLY` (read-only volume), `NODEV`, `IO`.

### 9.5. Console

#### `$30` CON_MODE — console mode

```text
fn sys_con_mode(mode: uint): (uint, uint)
```

R0 = new mode (Section 6.1), or `$FFFF` to query without changing it. Returns the previous mode in R1. A mode change discards an unfinished line being edited in line-input mode. Error: `INVAL` (unsupported bits).

#### `$31` CON_GETEV — read a keyboard event

```text
fn sys_con_getev(flags: uint): (uint, uint, uint)
```

R0 = flags; bit 0 `NOWAIT` means do not block. Returns R1 = event (character in low byte, scan code in high byte) and R2 = modifier bits for that event: bit 0 Shift, bit 1 Ctrl, bit 2 Alt (machine MODS register, Section 2.6). If the event buffer is empty and `NOWAIT` is clear, the call waits.

The Monitor records modifier bits when each event enters its buffer, so they describe that specific event even if the application consumes it later. The emulator determines which combinations are available at all (machine, Sections 9.2 and 9.3): for example, a Unix terminal does not transmit modifier state for ordinary characters. Applications supporting combinations such as Shift+Arrow should also offer alternative keys. Calling with only two results (`e, ev = sys_con_getev(0)`) remains valid; the unused return value is discarded.

Ctrl+C can be returned as a keyboard event only when `NOBREAK` is enabled. In line-input mode, a pending console `READ` may consume it instead to cancel the unfinished line (Section 6.3).

Errors: `AGAIN` (`NOWAIT` with no pending events), `INTR`, `INVAL`.

#### `$32` CON_FLUSH — discard pending input

```text
fn sys_con_flush(): uint
```

Discards buffered keyboard events and the partially entered line in line mode.

### 9.6. Memory

#### `$40` MEMINFO — memory information

```text
fn sys_meminfo(): (uint, uint, uint, uint)
```

Returns:
- R1 = process `top`;
- R2 = number of free physical memory frames (8 KiB each);
- R3 = number of extended-memory frames owned by the process.

#### `$41` SETTOP — resize process memory

```text
fn sys_settop(top: uint): uint
```

R0 = new `top`, a multiple of `$2000` in the range `$2000` through `$E000`. Newly allocated private pages receive zero-filled frames, and surplus frames are freed. If a page currently maps an extended-memory frame, that mapping remains in place; its underlying private frame is allocated or freed normally. The PIB `top` field is updated. When shrinking memory, data and stack contents above the new `top` are lost; the program must relocate them before calling.

Errors: `INVAL` (invalid value), `NOMEM` (insufficient free frames; the process memory layout remains unchanged).

#### `$42` XALLOC — allocate extended-memory frame

```text
fn sys_xalloc(): (uint, uint)
```

Allocates a zero-filled frame to the process (Section 7.6). Returns its frame number in R1. Error: `NOMEM`.

#### `$43` XFREE — release extended-memory frame

```text
fn sys_xfree(f: uint): uint
```

Returns extended-memory frame f to the system. Any pages that mapped it revert to their mapping without `XMAP`: their private frame below `top`, or no frame above `top` (accessing such pages from a system call produces `FAULT`). Error: `INVAL` (frame is not owned by the process as extended memory).

#### `$44` XMAP — map a frame to a page

```text
fn sys_xmap(page: uint, f: uint): (uint, uint)
```

R0 = page 0–6; R1 = extended-memory frame number, or `XM_OWN` (`$FFFF`) to restore the page's private frame (above `top`, to unmap the page). Returns in R1 the previous mapping: extended-memory frame number, or `XM_OWN` if the page previously mapped its private frame or had no mapping. The same frame may be mapped into several pages.

Error: `INVAL` (page number greater than 6; frame not owned by the process as extended memory).
### 9.7. Time

#### `$50` TIME_GET — get current time

```text
fn sys_time_get(): (uint, ulong)
```

Returns the time as an FS16 timestamp (FS16, Section 5.1) in R1:R2: R1 contains the time of day and R2 the date. Values can be compared as `ulong`. If the clock has not been set, the returned value is whatever the machine's clock reports (machine, Section 8).

#### `$51` TIME_SET — set current time

```text
fn sys_time_set(t: ulong): uint
```

R0:R1 = FS16-format timestamp. Error: `INVAL` (invalid timestamp field, including zero).

#### `$52` TICKS — elapsed tick count

```text
fn sys_ticks(): (uint, ulong)
```

Returns in R1:R2 the number of ticks since Monitor startup, modulo 2^32. The counter runs continuously, including during system calls, although ticks received while a program has IF = 0 may be coalesced (CX16 format, Section 8.3).

#### `$53` SLEEP — wait for ticks

```text
fn sys_sleep(n: uint): uint
```

Waits for n ticks (at least n − 1 complete 20 ms intervals). For n = 0, returns immediately. Error: `INTR`.

#### `$54` TICKHOOK — set tick handler

```text
fn sys_tickhook(h: ptr interrupt fn()): (uint, ptr interrupt fn())
```

R0 = handler address (Section 7.4), or 0 to disable it. Returns the previous handler in R1, or 0. Error: `FAULT` (address at or above `top`).

### 9.8. System

#### `$60` DISK_INFO — disk information

```text
fn sys_disk_info(drv: uint, info: ptr): (uint, uint)
```

R0 = disk number 0–7; R1 = a 32-byte buffer for the disk INFO block (machine, Section 6.3). Returns state bits in R1: bit 0 = mounted, bit 1 = mounted read-only. Errors: `NODEV` (disk absent or number greater than 7), `IO`.

#### `$61` DISK_IO — raw sector I/O

```text
fn sys_disk_io(req: ptr DiskReq): uint
```

R0 points to a `DiskReq` structure (Section 10.2). The request reads or writes `count` sectors (1–255) of disk `drive`, starting at `lba`. Its buffer must span `512·count` bytes of the calling process’s memory.

Volume-checking and formatting utilities use this call under the following mount-state restrictions:
- On an unmounted disk, both reads and writes are permitted.
- On a volume mounted read-only, raw reads and writes are also permitted; after a raw write, the Monitor discards cached sectors belonging to that disk (Section 5.3).
- On a volume mounted read-write, only reads are allowed; writing returns `BUSY`.
- **Boot sectors 1–3** (FS16, Section 2) are exempt from the `BUSY` restriction on writes to a read-write mounted volume, because the Monitor does not use them. The exception applies only when the entire request lies within sectors 1–3; requests also covering sector 0 or any sector after 3 follow the general rule. A physically write-protected disk still returns `RDONLY`.

Errors:
- `INVAL`: invalid operation or sector count, or a request extending beyond the end of the disk;
- `NODEV`;
- `RDONLY`: disk is write-protected;
- `BUSY`;
- `IO`.

#### `$62` SYSINFO — system information

```text
fn sys_sysinfo(si: ptr SysInfo): uint
```

Fills a `SysInfo` structure (Section 10.2).

#### `$63` SHUTDOWN — shut down the system

```text
fn sys_shutdown()
```

Terminates the processes in all sessions without notification (Section 2.4), performs `SYNC`, attempts to unmount every volume, displays the halt message, and stops the machine. The call never returns. Write errors are reported on the console but do not prevent the shutdown.

### 9.9. Sessions

Sessions are identified by a number from 0 through 3, or by `SAME` to indicate the caller's session. A session number greater than 3 returns `INVAL`; a valid number for a nonexistent session returns `NOENT`.

#### `$70` SESS_INFO — query session information

```text
fn sys_sess_info(s: uint, k: uint, pi: ptr ProcInfo): (uint, uint, uint, uint)
```

R0 = session; R1 = index within the process chain (0 means root); R2 = pointer to `ProcInfo` (Section 10.2), or 0. Returns R1 = session flags (`SF_CUR` means current), R2 = process count in the chain, R3 = resolved session number (so an application using `SAME` can learn its own session number). When the R2 argument is nonzero, the Monitor fills `ProcInfo` for process k: descriptor number, state (`PS_RUN` = executing, `PS_WAIT` = waiting for a child, `PS_SUSP` = suspended), `top`, private and extended memory frame counts, and the beginning of the command line that launched the process (`SPAWN`, `EXEC`, `SESS_NEW`).

To enumerate sessions, call with s = 0, 1, 2, 3 and `pi` = 0, skipping `NOENT` results.

Errors: `INVAL` (invalid session number or k at or above the process count), `NOENT`.

#### `$71` SESS_NEW — create a session

```text
fn sys_sess_new(cmd: ptr byte, flags: uint): (uint, uint)
```

R0 = command line for the new session's root process (parsed like `SPAWN`; Section 7.1), or 0 for the default shell `A:/sys/shell.cx`. R1 = flags; `SN_BACK` requests that the caller remain in its current session. The Monitor allocates the lowest available session number, loads the program (load errors are as in `SPAWN`), opens descriptors 0–2 on the console, and copies the caller's current directory. Without `SN_BACK`, the new session becomes current immediately after the call returns (Section 2.4). Returns the new session number in R1.

Errors: `INVAL` (flags), `ARGS`, program-loading errors, `NOMEM` (including memory needed for the session screen), `NOPROC` (all four sessions occupied or the system already has 16 processes).

#### `$72` SESS_SWITCH — switch sessions

```text
fn sys_sess_switch(s: uint): uint
```

Session s becomes current immediately after the call returns. The calling process observes its return value only after its own session becomes current again. Switching to the current session is a no-op. Errors: `INVAL`, `NOENT`.

#### `$73` SESS_KILL — terminate a session

```text
fn sys_sess_kill(s: uint, flags: uint): uint
```

Terminates every process in session s and closes it (Section 2.4). Without `SK_FORCE`, a session containing more than one process is not terminated (`BUSY`); this prevents accidentally discarding a shell's running editor. If s is the caller's own session, the call does not return and the next session cyclically becomes current.

Errors: `INVAL` (session, flags), `NOENT`, `BUSY` (multiple processes without `SK_FORCE`; the system's last session must be terminated by allowing its root to exit).
## 10. B16 Interface

### 10.1. Integration

`sysdefs.b16` contains the constants, structures (via `abi.b16`), and call prototypes. The `sys.asm` module provides the wrappers, each only three words long. The `sys.b16` module includes both: `sysdefs.b16` through `include` and `sys.asm` through `include asm` (B16, Sections 3.2 and 10.4). This places the wrappers in the image before `__image_end`. Applications therefore need only one line:

```text
include "sys.b16"
```

```text
b16c prog.b16 -o prog.s
```

Including `sysdefs.b16` without `sys.b16` is useful for code that calls wrappers already present in another image. For example, BScript overlays use the wrappers in the resident portion of `bsvm`.

A wrapper is a B16 function using the same register assignment (B16, Section 9.3), so no argument rearrangement is needed:

```asm
sys_read:
        TRAP    #9
        .word   $12
        RET
sys_seek:
        TRAP    #9
        .word   $14
        RET
```

The call number is the word immediately following `TRAP #9` (Section 3.1). The Monitor preserves LR (Section 3.2), so the subsequent `RET` returns to the application. The error code is both the first result and the basis for ZF, allowing assembly code to test the result with `JNZ` immediately after a call.

### 10.2. Constants and structures
```text
// error codes (Section 8)
const OK = 0
const E_NOSYS = 1
const E_INVAL = 2
const E_FAULT = 3
const E_NOENT = 4
const E_EXIST = 5
const E_NOTDIR = 6
const E_ISDIR = 7
const E_NOTEMPTY = 8
const E_RDONLY = 9
const E_NOSPC = 10
const E_BADNAME = 11
const E_BADPATH = 12
const E_BADF = 13
const E_MFILE = 14
const E_NODEV = 15
const E_IO = 16
const E_NOEXEC = 17
const E_BADVER = 18
const E_ARGS = 19
const E_NOMEM = 20
const E_NOPROC = 21
const E_AGAIN = 22
const E_BUSY = 23
const E_INTR = 24
const E_XDEV = 25
const E_NOTFS = 26

// standard descriptors
const STDIN = 0
const STDOUT = 1
const STDERR = 2
const SAME = $FFFF                  // SPAWN: corresponding parent descriptor; DUP: any free descriptor

// OPEN mode flags
const O_READ = $01
const O_WRITE = $02
const O_CREATE = $04
const O_TRUNC = $08
const O_EXCL = $10
const O_APPEND = $20

// SETSIZE
const SZ_ZERO = $01

// SEEK
const SEEK_SET = 0
const SEEK_CUR = 1
const SEEK_END = 2

// descriptor types (HINFO)
const H_FILE = 0
const H_DIR = 1
const H_CON = 2
const H_NUL = 3

// object attributes: bits in the high word of the size field (FS16, Section 5)
const A_DIR = $8000
const A_RDONLY = $4000
const A_HIDDEN = $2000
const A_SYSTEM = $1000
const SIZE_MASK = $07FFFFFF

// console mode (CON_MODE)
const CM_RAW = $01
const CM_NOECHO = $02
const CM_NOBREAK = $04

// CON_GETEV: input flags and event modifiers (R2)
const EV_NOWAIT = $01
const EVM_SHIFT = $01
const EVM_CTRL = $02
const EVM_ALT = $04

// XMAP: restore the page's own frame
const XM_OWN = $FFFF

// ONEVENT event types
const EVT_BREAK = 1                 // Ctrl+C
const EVT_BADINSN = 2               // illegal instruction

// MOUNT call flags
const MOUNT_RDONLY = $01            // read-only
const MOUNT_REMOUNT = $02           // remount an already mounted volume

// MOUNT and VolInfo.flags
const MNT_RDONLY = $01              // mounted read-only
const MNT_DIRTY = $02               // read-only because DIRTY is set
const MNT_PROTECT = $04             // read-only because the disk is write-protected

// DISK_INFO
const DI_MOUNTED = $01
const DI_RDONLY = $02

// DiskReq.op
const DIO_READ = 1
const DIO_WRITE = 2

// sessions (Sections 2.4, 9.9)
const MAXSESS = 4
const SF_CUR = $01                  // SESS_INFO: currently active session
const SN_BACK = $01                 // SESS_NEW: do not switch to the new session
const SK_FORCE = $01                // SESS_KILL: also kill sessions with multiple processes
const PS_RUN = 1                    // ProcInfo.state: currently executing
const PS_WAIT = 2                   // waiting for a child
const PS_SUSP = 3                   // suspended: last process of another session
const PI_CMDLEN = 56

// Monitor-defined exit codes
const EXIT_BREAK = $8001
const EXIT_BADINSN = $8008
const EXIT_KILLED = $8009

// First 40 bytes of an FS16 directory entry.
struct Stat {
    name: [32]byte                  // zero-padded
    size: ulong                     // bits 0-26: size; 27-31: attributes
    mtime: ulong                    // FS16 timestamp (Section 5.1)
}
assert sizeof(Stat) == 40, "Stat"

struct VolInfo {
    label: [16]byte                 // FS16 volume label
    serial: ulong
    clusters: uint                  // total cluster count N
    free: uint                      // free cluster count
    flags: uint                     // MNT_... flags
}
assert sizeof(VolInfo) == 26, "VolInfo"

struct DiskReq {
    op: uint                        // DIO_READ or DIO_WRITE
    drive: uint                     // disk number 0-7
    count: uint                     // sector count, 1-255
    buf: ptr                        // 512 * count bytes
    lba: ulong
}
assert sizeof(DiskReq) == 12, "DiskReq"

struct SysInfo {
    version: uint                   // Monitor version: high byte is this document's revision
    frames: uint                    // total physical memory frames
    free_frames: uint               // free frames
    depth: uint                     // process count in caller's session chain, including caller
    max_depth: uint                 // 16 processes across all sessions combined
    mounted: uint                   // bit N: volume N is mounted
    session: uint                   // caller's session number
    max_sess: uint                  // 4
}
assert sizeof(SysInfo) == 16, "SysInfo"

struct ProcInfo {
    pid: uint                       // process descriptor number
    state: uint                     // PS_... state
    top: uint                       // private memory upper boundary
    frames: uint                    // private and extended memory frame count
    cmd: [PI_CMDLEN]byte            // initial command-line prefix, null-terminated
}
assert sizeof(ProcInfo) == 64, "ProcInfo"
```

Extract the file size using `st.size & SIZE_MASK` and the attribute flags using `uint(st.size >> 16) & $F800`.

### 10.3. Prototypes

```text
extern fn sys_exit(code: uint)
extern fn sys_spawn(cmd: ptr byte, fin: uint, fout: uint, ferr: uint): (uint, uint)
extern fn sys_exec(cmd: ptr byte): uint
extern fn sys_onevent(ev: uint, h: ptr interrupt fn()): (uint, ptr interrupt fn())

extern fn sys_open(path: ptr byte, mode: uint, attr: uint): (uint, uint)
extern fn sys_close(h: uint): uint
extern fn sys_read(h: uint, buf: ptr, n: uint): (uint, uint)
extern fn sys_write(h: uint, buf: ptr, n: uint): (uint, uint)
extern fn sys_seek(h: uint, off: long, whence: uint): (uint, ulong)
extern fn sys_dup(h: uint, target: uint): (uint, uint)
extern fn sys_hinfo(h: uint): (uint, uint, uint)
extern fn sys_setsize(h: uint, size: ulong, flags: uint): uint
extern fn sys_fstat(h: uint, st: ptr Stat): uint

extern fn sys_stat(path: ptr byte, st: ptr Stat): uint
extern fn sys_delete(path: ptr byte): uint
extern fn sys_rename(old: ptr byte, new: ptr byte): uint
extern fn sys_mkdir(path: ptr byte): uint
extern fn sys_chdir(path: ptr byte): uint
extern fn sys_getcwd(buf: ptr byte, n: uint): (uint, uint)
extern fn sys_readdir(h: uint, st: ptr Stat): (uint, uint)
extern fn sys_setattr(path: ptr byte, attr: uint): uint
extern fn sys_volinfo(drv: uint, vi: ptr VolInfo): uint
extern fn sys_mount(drv: uint, flags: uint): (uint, uint)
extern fn sys_unmount(drv: uint): uint
extern fn sys_sync(): uint
extern fn sys_setmtime(path: ptr byte, t: ulong): uint

extern fn sys_con_mode(mode: uint): (uint, uint)
extern fn sys_con_getev(flags: uint): (uint, uint, uint)
extern fn sys_con_flush(): uint

extern fn sys_meminfo(): (uint, uint, uint, uint)
extern fn sys_settop(top: uint): uint
extern fn sys_xalloc(): (uint, uint)
extern fn sys_xfree(f: uint): uint
extern fn sys_xmap(page: uint, f: uint): (uint, uint)

extern fn sys_time_get(): (uint, ulong)
extern fn sys_time_set(t: ulong): uint
extern fn sys_ticks(): (uint, ulong)
extern fn sys_sleep(n: uint): uint
extern fn sys_tickhook(h: ptr interrupt fn()): (uint, ptr interrupt fn())

extern fn sys_disk_info(drv: uint, info: ptr): (uint, uint)
extern fn sys_disk_io(req: ptr DiskReq): uint
extern fn sys_sysinfo(si: ptr SysInfo): uint
extern fn sys_shutdown()

extern fn sys_sess_info(s: uint, k: uint, pi: ptr ProcInfo): (uint, uint, uint, uint)
extern fn sys_sess_new(cmd: ptr byte, flags: uint): (uint, uint)
extern fn sys_sess_switch(s: uint): uint
extern fn sys_sess_kill(s: uint, flags: uint): uint
```

### 10.4. Example

```text
include "sys.b16"

var buf: [512]byte

// Copy the file named by argv[1] to standard output.
fn main(argc: int, argv: ptr ptr byte): int {
    if argc < 2 { return 1 }
    var e: uint
    var h: uint
    e, h = sys_open(argv[1], O_READ, 0)
    if e != OK { return 2 }
    loop {
        var n: uint
        e, n = sys_read(h, &buf[0], 512)
        if e != OK || n == 0 { break }
        e, n = sys_write(STDOUT, &buf[0], n)
        if e != OK { break }
    }
    e = sys_close(h)
    return 0
}
```
## 11. Revision History

### Revision 6

- **B16 interface** (Section 10.1): declarations moved into `sysdefs.b16`. `sys.b16` includes those declarations and the `sys.asm` wrappers using `include asm` (B16 Revision 3); `--asm sys.asm` is no longer needed.
- **Volume letters** (Section 4.1): volume path prefixes now use `A:`–`H:` in either case. Numeric prefixes `0:`–`7:` remain accepted. `GETCWD` and Monitor messages output an uppercase letter; older programs that expect a digit at the start of a `GETCWD` path need to be updated. Numeric volume arguments to system calls have not changed. The shell is launched as `A:/sys/shell.cx`. `SYSINFO.version` = `$0600`.

### Revision 5

- **Sessions** (Sections 2.4 and 9.9): up to four process chains, each with its own screen; only the current session executes while the others are suspended. Switching uses Ctrl+], Ctrl+[, or a system call. Added `SESS_INFO`, `SESS_NEW`, `SESS_SWITCH`, `SESS_KILL`, and `ProcInfo`; call number `$7F` is reserved by the Monitor.
- The limit of 16 processes now applies across all sessions (Section 2.1). Exiting the root process shuts down the machine only for the final remaining session (Section 2.3).
- Pending calls are restarted after a session switch; `SLEEP` waits for the remaining interval (Section 3.4).
- Programs are permitted to read display contents through SCRD (Sections 2.2 and 6.4; machine Revision 9).
- `SysInfo`: former `reserved` fields became `session` and `max_sess`; `depth` now counts the caller's own session chain. `SYSINFO.version` = `$0500`.

### Revision 4

- **Event modifiers** (Sections 6.1 and 9.5; machine Revision 8, Section 2.6): `CON_GETEV` now returns the event's Shift, Ctrl and Alt flags in R2. Existing applications are unaffected because they did not read R2.
- `SYSINFO.version` = `$0400`.

### Revision 3

- **Case-insensitive names** (Section 4.1; FS16 Revision 3, Section 7.1): lookup, creation (`OPEN` with `O_CREATE`, `MKDIR`) and `EXIST` checks compare names ignoring case. Original spelling is retained on disk, and `READDIR` and `GETCWD` return stored spelling. `RENAME` can change capitalization without changing which file is referenced (Section 9.4).
- **Ctrl+Enter** (Section 6.1; machine Revision 7, Section 2.6): event with character 0 and scan code `$1C`, available through `CON_GETEV`.
- `SYSINFO.version` = `$0300`. Applications depending on case-insensitive lookup can check the high byte: values of 3 or greater indicate case-insensitive naming.
- Volume/device prefixes and device names are unchanged.

### Revision 2

- **`EXEC`** (`$02`, Section 9.2): replace the program in the current process while preserving its position in the chain, descriptors and current directory.
- **`ONEVENT`** (`$03`, Sections 7.5 and 9.2): Ctrl+C and illegal-instruction handlers can run instead of terminating the process; they are allowed to make system calls.
- Sections 6.3, 7.1, 7.2 and 7.3 gained references to event handlers; Sections 10.2 and 10.3 gained the `EVT_BREAK` and `EVT_BADINSN` constants and prototypes.
- **`SETMTIME`** (`$2C`, Section 9.4): set a file's or directory's modification timestamp.
- **`DISK_IO`** (Section 9.8): writing sectors 1–3 is now permitted on any volume, including read-write mounted volumes, so boot code can be installed on the machine itself. Writes on read-only mounted volumes support filesystem checking (Section 5.3).
- **`SETTOP`, `XALLOC`, `XFREE`, `XMAP`** (`$41–$44`, Sections 7.6 and 9.6): private-memory resizing and extended memory. `MEMINFO` reports extended-memory frame count in R3.
- Section 3.3 clarifies that process memory includes pages mapped to its frames, including windows above `top`; handler addresses are checked using the same rules.

### Revision 1

- Clarifications following implementation: for a DIRTY volume, `DISK_IO` may write a read-only mounted volume, and `MOUNT_REMOUNT` supports subsequent remounting (Sections 5.3, 9.4, 9.8 and 10.2). Monitor messages are in English (Section 7.3). File MTIME changes on each data or size modification, not on `CLOSE` (Sections 5.4 and 9.3).

Initial edition. A process-chain model without preemption; calls for processes, descriptors, filesystem, console, memory information, time, tick hooks, and raw disk sectors.

The following were reserved for a later revision:
- `$02`: replace a process image;
- `$03`: event handlers (Ctrl+C, illegal instruction);
- `$41–$44`: `top` resizing and extended memory;
- `TRAP #10`: debugger.
