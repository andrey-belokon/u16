# CPU16 Monitor: Shell and Utilities — Revision 6

This document specifies the shell (`A:/sys/shell.cx`, the first program started by Monitor) and the initial set of utilities in `A:/bin`. The shell and utilities are ordinary B16 programs in CX16 format: they use system calls exclusively and have no special privileges.

Sections 1–7 are normative. Section 8 provides implementation recommendations.

## 1. Related Documents

| Document | Relevant material |
| --- | --- |
| “CPU16 Monitor: System Calls”, revision 6 | All calls; paths (Section 4.1); console and Ctrl+C (Section 6); starting child processes and exit codes (Section 7); sessions (Sections 2.4 and 9.9) |
| “CPU16 Monitor: Executable File Format and Program Startup”, revision 4 | Command-line parsing (Section 5.2) and PIB redirection flags |
| “FS16 File System”, revision 3 | Names, flags, and timestamps |

These documents are referred to below as “System Calls”, “CX16 Format”, and “FS16”.

## 2. Startup and Exit

**Startup by Monitor.** Monitor starts the shell with the command line `A:/sys/shell.cx` (“System Calls”, Section 2.3). The shell then:

1. **Checks volumes.** For each volume mounted read-only by Monitor because of the DIRTY flag (`VOLINFO`, `MNT_DIRTY`), the shell prints `shell: volume X: was not unmounted cleanly, running fsck` (X is the volume letter) and runs `A:/bin/fsck.cx X`. Only if `fsck` exits with code 0 does the shell attempt to remount that volume using `MOUNT_REMOUNT` (`MOUNT`; “System Calls”, Section 5.3), reporting `shell: volume X: mounted read-write` on a successful read-write remount. The procedure also applies to `A:`, the volume containing the running shell.
2. Creates `A:/tmp` if necessary (Section 4.4).
3. Executes `A:/sys/startup`, if present, as a command script (Section 3.6).
4. Displays a prompt and reads commands from the console.

Steps 1–3 run only during system startup. When `SESS_INFO` reveals another session, the shell is starting as the root of an additional session (`session new`, Section 5.1); it skips the volume checks, temporary-directory setup, and startup script and goes directly to interactive input (step 4).

**Script mode.** Invoking `shell.cx FILE` reads and executes commands from FILE instead of entering the interactive command loop. The shell then terminates with the status of the last command executed, including a command that ended the script early. Startup steps 1–3 are not performed.

**Exit.** The `exit` built-in (Section 5) terminates the shell. If Monitor started the shell, an exit code of 0 halts the system; any other code causes Monitor to restart the shell (“System Calls”, Section 2.3).

## 3. Command Line

### 3.1. Input

If standard input is a console (`HINFO`), the shell selects raw mode with `NOBREAK` (“System Calls”, Sections 6.1–6.3). Thus Ctrl+C reaches the shell as a keyboard event rather than terminating it; the shell builds and echoes the input line itself using `CON_GETEV`:

| Key | Action |
| --- | --- |
| Characters `$20` and above; Tab | Append to the line, up to a maximum of 255 characters |
| Backspace | Delete the last character |
| Ctrl+U | Clear the entire line |
| Enter | Execute the line |
| ↑ (scan code `$48`) | Recall the previous history entry |
| ↓ (scan code `$50`) | Recall the next history entry; beyond the newest entry, restore the line being typed before history navigation |
| Ctrl+C | Discard the input line, print `^C`, and display a new prompt |
| Ctrl+Z at the beginning of an empty line | End input, as though `exit 0` had been entered |

Other keys are ignored. Editing within a line using ← and → is not supported.

**History** holds the last 16 executed nonempty command lines, omitting a line if it is identical to the immediately preceding entry. It exists only for the lifetime of the shell and is not saved to disk.

When a child program uses the console, it starts in the normal line-input mode rather than inheriting the shell’s raw `NOBREAK` mode. Console mode is a per-process setting (“System Calls”, Section 6.1).

When standard input is redirected, the shell reads it line by line, like a script (Section 3.6), without the interactive line editor or prompt.

**Prompt.** The prompt is the current working directory (`GETCWD`) followed by `> `, for example, `A:/bin> `. If the system has more than one session (Section 5.1), the shell prefixes its own session number in brackets: `[1] A:/bin> `. The prompt is redrawn before each command. Thus, a shell waiting for input in another session will not add or remove its session-number prefix until its next command.

**Volume names.** Paths use letters `A:`–`H:` in either case; the older numeric forms `0:`–`7:` are also recognized (“System Calls”, Section 4.1). Utilities accept volume arguments as `B`, `b`, `B:`, or `1`, and display the corresponding letter with a colon (`B:`).

Input lines are limited to 255 bytes by Monitor. An empty or all-space line has no effect.

### 3.2. Tokens

A line is split into words according to “CX16 Format”, Section 5.2. Space and Tab separate words. Double quotes `"` group text containing spaces into a single word and are not part of the resulting word. `""` produces an empty word; an unmatched quote is an error.

Outside quotes, these characters have special meaning:

| Character | Meaning |
| --- | --- |
| `;` | Command separator; execute commands in sequence |
| `\|` | Pipeline (Section 3.5) |
| `<`, `>`, `>>`, `2>` | Redirections (Section 3.4) |
| `#` at the start of a word | Comment extending to the end of the line |

Command separators and redirection operators delimit words even without spaces: `ls>out` becomes `ls`, `>`, `out`. Inside double quotes these symbols are literal text; `#` starts a comment only at the beginning of an unquoted word.

### 3.3. Commands

The first word of a command is its **name**. If it names a built-in (Section 5), the shell executes that built-in. Otherwise the shell locates the program file (Section 4.1) and starts it with `SPAWN`.

The shell builds a new command-line string for `SPAWN` rather than passing along the text entered by the user unchanged:

- The first word is the pathname selected by the search rules in Section 4.1. It may be relative to the current directory; Monitor does not perform another search.
- The remaining words are the program arguments. The shell quotes empty arguments and arguments containing spaces, tabs, or shell-special characters when rebuilding the launch string, so the Monitor parser receives those words intact.

A literal `"` cannot occur within an argument because CX16 defines no way to escape quotation marks. Subject to the command-line length limit, all arguments representable under these rules retain their values.

Redirection operators are not included in the program's command line.

### 3.4. Redirections

| Syntax | Effect |
| --- | --- |
| `< file` | Standard input from a file (`OPEN` with `O_READ`) |
| `> file` | Standard output to a file, creating or truncating it (`O_WRITE \| O_CREATE \| O_TRUNC`) |
| `>> file` | Standard output appended to a file (`O_WRITE \| O_CREATE \| O_APPEND`) |
| `2> file` | Standard error to a file (same creation/truncation behavior as `>`) |

The target filename is the word following the operator. Redirections may occur anywhere in the command; if the same stream is redirected more than once, the last redirection determines the handle used. A device path is also valid, for example `> dev:nul`.

Before launching a program, the shell opens redirection targets and supplies the selected handles to `SPAWN`. It closes the handles it opened once the command finishes. If an `OPEN` fails, it reports the error without starting the command. Standard-output redirection also applies to built-in commands.

### 3.5. Pipelines

A pipeline such as `a | b | c` is sequential, not concurrent: the shell runs each stage in order, passing its output to the next through temporary files (Section 4.4):

1. Run `a`, sending its output to temporary file T1.
2. Run `b`, reading from T1 and sending output to T2.
3. Run `c`, reading from T2.

Temporary files are removed after the pipeline completes. Each stage may have its own redirections; explicit redirections take precedence over the implicit pipeline connection. If a stage exits with code `$8001` (Ctrl+C), subsequent stages are not executed.

The pipeline's exit code is the exit code of its last executed command.

### 3.6. Scripts

A command script consists of command lines separated by newline (`\n`) characters, processed with the same command rules as console input. An ordinary command failure does not itself abort the script. Script execution stops:

- after `exit`;
- when a program exits with code `$8001` (Ctrl+C);
- at end of file.

Commands launched from a script still receive the console as standard input unless explicitly redirected; reading commands from a script file does not by itself redirect the standard input of the programs it starts.

## 4. Programs and Files

### 4.1. Program Lookup

| Name | Search location |
| --- | --- |
| Contains `/` or `:` | Only the specified path |
| Otherwise | Current directory, then `A:/bin` |

At each search location, the shell tries `name`, `name.cx`, then `name.bs`, in that order. A name containing `/` or `:` is checked only at the specified location, but these suffix checks still apply. Filenames are compared case-insensitively: `LS` can find `ls.cx`. If the name already ends in `.cx`, regardless of case, neither suffix is added.

**BScript scripts.** A file whose extension is `.bs` (case-insensitive) is launched as `A:/bin/bs.cx path arguments...` (“BScript — Language Specification (Phase 1)”, Section 11.4). Shell redirections apply to that interpreter process. Thus, `A:/bin/backup.bs` can be run simply as `backup`. An explicitly named compiled script `.bsc` (such as `prog.bsc` or `/t/prog.bsc`) is launched as `A:/bin/bsvm.cx path -- arguments...`; `--` tells the VM that the program to load is the specified `.bsc` file. The `.bsc` extension is **not** appended during name lookup: an extensionless command finds the `.bs` source, whose cache is checked by `bs`.

Built-in names (Section 5) are also case-insensitive: `CD` and `cd` denote the same command. Only regular files, not directories, qualify as programs. Once a candidate file is found, a loading failure is reported immediately; the shell does not continue searching for another extension or directory.

### 4.2. Exit Status

The shell records a status for every command, including built-ins (“System Calls”, Section 7.2). If `SPAWN` fails before starting a child, the shell reports the system-call error (Section 4.3) rather than interpreting it as the child's exit code.

| Code | Shell output |
| --- | --- |
| 0 | Nothing |
| `$0001–$7FFF` | `exit code N` (decimal) on standard error |
| `$8001` | Nothing; Monitor has already printed `^C` |
| `$8008`, `$8009` | Nothing; Monitor has already reported the error |
| Other `$8000–$FFFF` codes | `exit code $XXXX` |

### 4.3. Error Messages

The shell and utilities send errors to standard error (handle 2) in the form `name: object: message`; for example, `cat: A:/a: no such file or directory`. Error text is the English message for the corresponding error code from “System Calls”, Section 8 (table in Section 8.2). All other shell messages (parse errors, `command not found`, exit statuses, `help`) and utility messages (Section 6) are likewise in English.

### 4.4. Temporary Files

A pipeline uses temporary files named `A:/tmp/.pipeN`, with N identifying the pipeline command. The shell creates `A:/tmp` on startup. If it cannot create this directory (for example because the volume is read-only), pipeline execution fails.

## 5. Built-in Commands

| Command | Action |
| --- | --- |
| `cd [path]` | Change the current directory; without an argument, use `A:/` |
| `pwd` | Print the current directory |
| `echo [word ...]` | Print the words separated by spaces, followed by a newline |
| `exit [code]` | Exit the shell with the specified code (default 0) |
| `cls` | Clear the screen |
| `help` | List built-ins and utilities in `A:/bin` |
| `session [N \| new [-b] [command ...] \| kill [-f] [N]]` | Manage sessions (Section 5.1) |

Built-in exit codes: 0 on success, 1 on failure, 2 for invalid arguments.

### 5.1. Sessions

The `session` built-in manages Monitor sessions (“System Calls”, Section 2.4). Keyboard shortcuts are the preferred way to switch sessions: Ctrl+] selects the next session, while Ctrl+[ selects the previous session (only Ctrl+] is supported by the command-line emulator).

| Form | Action |
| --- | --- |
| `session` | List sessions |
| `session N` | Switch to session N |
| `session new` | Start a new session containing a shell and switch to it |
| `session new command ...` | Start a new session with the specified program as its root process. Program lookup and script execution work as for ordinary commands (Section 4.1). The session closes when that program exits |
| `session new -b ...` | Same, but remain in the current session; print `session N started` |
| `session kill [N]` | Terminate session N (default: current session). Refuse if it contains more than one active process: `session: the session runs several programs; use kill -f` |
| `session kill -f [N]` | Terminate the session and all its processes |

A newly created session inherits the creating shell's working directory and starts with a cleared screen. Only the current session executes; for example, `session new -b` creates a session that remains suspended until selected (“System Calls”, Section 2.4). When a session's root process exits, that session closes and the next session becomes active. Termination of the last remaining session follows Section 2.

The listing contains one line per session: `*` marks the current session, followed by its number, total process memory in KB, and the process chain from the root, separated by ` > `:

```text
[1] A:/data> session
  0    80K  A:/sys/shell.cx > A:/bin/edit.cx s.bs
* 1    56K  A:/sys/shell.cx
```

Errors:

- `session: N: no such session` — the session does not exist, or N exceeds 3;
- `session: the only session; use exit` — the last remaining session cannot be terminated with `session kill`;
- `SESS_NEW` failures use the format in Section 4.3 (`session: not enough memory`; `session: too many nested programs` when all four sessions or all 16 processes are occupied).

## 6. Utilities

Utilities reside in `A:/bin` and are invoked as ordinary programs. Common conventions:

- **Errors:** written to handle 2 in the format described in Section 4.3.
- **Exit codes:** 0 for success; 1 if an operation fails for at least one object; 2 for invalid arguments.
- **Options:** words beginning with `-`, preceding the first non-option argument.
- **Volume arguments** (X in the table): a letter `A`–`H` in either case, with or without a colon, or a number 0–7. Messages display the letter with a colon (for example, `fsck: volume B: consistent`).
- A utility accepting **multiple files** processes all of them, even if some operations fail.

| Utility | Arguments | Action |
| --- | --- | --- |
| `ls` | `[-l] [-a] [path ...]` | List the contents of a directory (the current directory by default), or show information about a file. With `-l`, show flags (`d`, `r`, `h`, `s`), size, and modification date/time; use dashes if the timestamp is absent. Show HIDDEN files only with `-a` |
| `cat` | `[file ...]` | Concatenate file contents to standard output; with no arguments, copy standard input |
| `cp` | `source destination` | Copy a file. If the destination is a directory, use the source filename within it. Reject a destination path that differs from the source only in letter case (`cp /a /A`) with `same file` |
| `mv` | `source destination` | Rename or move with `RENAME`; across volumes, copy the file and then delete the source. If the destination names the source object (`mv readme README`), treat it as a case-only rename, not a move into a directory, and pass the destination unchanged to `RENAME` |
| `rm` | `file ...` | Delete files; directories are an error |
| `mkdir` | `directory ...` | Create directories |
| `rmdir` | `directory ...` | Remove empty directories |
| `attr` | `[+r\|-r\|+h\|-h\|+s\|-s ...] path ...` | Change RDONLY, HIDDEN, and SYSTEM flags; without options, display the flags |
| `df` | `[X ...]` | For mounted volumes, display letter (`A:`), label, total and free KB, and mount mode (`rw`, `ro`, `ro dirty`, `ro protected`) |
| `mount` | `X [-r]` | Mount volume X (`-r` requests read-only mode) |
| `umount` | `X` | Unmount volume X |
| `sync` | — | Invoke `SYNC` |
| `date` | `[YYYY-MM-DD HH:MM:SS]` | Display the date and time, or set them when arguments are supplied |
| `touch` | `[-t YYYY-MM-DD HH:MM:SS] file ...` | Create a missing file and set its modification time to the current or specified time (`SETMTIME`). For a directory or an RDONLY file, update only the timestamp |
| `mem` | — | Display the current process's `top`, its extended memory, free memory, and process-chain depth (`MEMINFO`, `SYSINFO`) |
| `sysboot` | `X [file]` | Write volume boot code (default `A:/sys/fsboot.bin`) to sectors 1–3 of disk X (“Disk Boot”, Section 6). Verify that the file is valid boot code (`CXBT`, version 1, at most 1536 bytes) and the disk contains an FS16 volume. Warn if `/sys/monitor.sys` is missing from that volume |
| `fsck` | `X [-n]` | Check volume X against the FS16 integrity rules (Section 8) and repair it according to Section 10. Starting at ROOT, build a new allocation bitmap. Truncate files before invalid or shared clusters; remove duplicate entries left by interrupted moves; clear pointers beyond SIZE and clear free directory entries that still contain nonzero bytes. Replace the bitmap, recalculate FREE, and clear DIRTY. Report invalid names without changing them. When two names differ only in letter case, rename the later entry by appending 2, 3, and so on (FS16, Section 10, item 6), and report `fsck: /path: name differs only in letter case from another entry, renamed to NEWNAME` (`would be renamed to` with `-n`). Uses raw `DISK_IO`: repairs require an unmounted or read-only mounted volume. `-n` checks without writing and is also allowed on a read-write mounted volume. Exit status: 0 if consistent or repaired; 1 if problems remain; 2 for invalid arguments |

## 7. Limitations

There is no in-line cursor editing, environment variables, wildcard expansion (`*`), background jobs, `&&`, or `||`. The `ls` utility lists entries in directory order.

## 8. Implementation Recommendations

### 8.1. Organization

- The shell is a single B16 module with system-call wrappers (`sys.b16`, which includes declarations from `sysdefs.b16` and wrappers from `sys.asm`). Utilities are separate programs. Common functions (output, error messages, option parsing) live in `ulib.b16`.
- A `max_top` of 16–24 KB is enough for each utility, except `fsck`, which needs 40 KB for the reconstructed bitmap (up to 8 KB) and cluster buffers. Set the value in the utility source, for example `option max_top = $6000` (`$A000` for `fsck`). This conserves memory when utilities are invoked through nested shells.
- Use a 4 KB copy buffer in `cat` and `cp`; full-sector reads and writes go directly to process memory.

### 8.2. Error Strings

| Code | Message |
| --- | --- |
| `NOENT` | no such file or directory |
| `EXIST` | already exists |
| `NOTDIR` | not a directory |
| `ISDIR` | is a directory |
| `NOTEMPTY` | directory not empty |
| `RDONLY` | read-only |
| `NOSPC` | no space left |
| `BADNAME` | invalid name |
| `BADPATH` | invalid path |
| `NODEV` | volume not mounted |
| `IO` | I/O error |
| `NOEXEC` | not an executable |
| `BADVER` | unsupported program version |
| `ARGS` | bad arguments |
| `NOMEM` | not enough memory |
| `NOPROC` | too many nested programs |
| `BUSY` | busy |
| `XDEV` | different volumes |
| `NOTFS` | no file system |
| `INVAL` | invalid argument |
| `MFILE` | too many open files |
| Other | error N |

### 8.3. Testing

The shell is tested with emulator scenarios, just like the kernel: use `type` to simulate keyboard input and compare actual output with expected output. Test utilities through a shell script (Section 3.6) that invokes each utility and verifies its results, for example by displaying file contents with `cat` and comparing the output against a reference.

## 9. Revision History

### Revision 6

- **Program builds (B16 revision 3).** Each utility sets `max_top` in its source using `option` declarations. `sys.b16` includes the system-call wrappers through `include asm`; build commands require no extra options.
- **Volume letters** (“System Calls”, revision 6): shell and utility paths use `A:/…`. Utilities accept volume letters as well as the older numeric identifiers and display letters (`A:  SYSTEM` in `df`, `fsck: volume A: consistent`, `sysboot: boot code installed on drive B:`). Usage hints take forms such as `usage: mount A-H [-r]` (Sections 2, 3, and 6).
- **Session number in the prompt:** prefix the working directory with `[N] ` when more than one session exists (Section 3).

### Revision 5

- New `session` built-in (Section 5.1): list and switch sessions, create a session with a shell or program, and terminate sessions normally or forcibly.
- Volume checks, creation of `A:/tmp`, and execution of `A:/sys/startup` now run only at system startup, not when each new session is created (Section 2).

### Revision 4

- Program lookup (Section 4.1): after `name.cx`, also try `name.bs`; run BScript source files via `A:/bin/bs.cx` (required by BScript Section 11.4) and compiled `.bsc` programs via `A:/bin/bsvm.cx`.
- `MFILE` now maps to `too many open files` (Section 8.2), matching BScript.

### Revision 3

- Case-insensitive filenames (“System Calls”, revision 3): program lookup (Section 4.1), `cp`'s `same file` check, and case-only renames through `mv` (Section 6).
- Built-in command names are case-insensitive (Section 4.1); `help` strips the `.cx` suffix in any case.
- `fsck` renames entries whose names collide under case-insensitive comparison (Section 6).

### Revision 2

- **Command history** (Section 3.1): the shell implements its own line editor using raw console mode and ↑/↓ keys.
- **Utilities** `fsck`, `touch`, and `sysboot` (Section 6); checks for DIRTY volumes during startup (Section 2).
- All shell and utility messages are in English (Sections 4.2, 4.3, 6, 8.2).

### Revision 1

Clarifications from the initial implementation: `cp` must reject a copy onto the same file; `ls -l` must handle entries with no timestamp.

Initial specification.
