# CPU16 Monitor: fm File Manager — Revision 6

`fm` is a full-screen, Norton Commander-style file manager with two directory panels, a command line, and F1–F10 shortcuts. It is an ordinary B16 program in CX16 executable format (`A:/bin/fm.cx`) and uses system calls exclusively.

Sections 1–5 are the user guide; section 6 describes the implementation; section 7 describes validation; section 9 is the change history.

## 1. Starting the file manager

    fm [path]

The left panel displays the current directory; the right panel displays `path` (also the current directory by default). F10 terminates `fm` with exit code 0. The shell's working directory remains unchanged, because `fm` runs in its own process.

The screen is drawn directly to the display using coordinate-based output (see *System Calls*, section 6.4). The command-line `cpu16` emulator does not forward coordinate-based output to stdout. The web debugger's **Display** window is therefore the most convenient way to use `fm`.

## 2. Screen layout

```text
╔══════════ A:/data ═════╤════════╗╔═══ B:/ ═══ ... ╗   panel directory; the active panel is highlighted
║        Name        │  Size  │  Date  ║               column headers
║..                  │      Up│        ║               ".." goes to the parent (except at volume root)
║sub                 │  Folder│18.10.26║               directories appear first, in white
║hello.txt           │    3895│--.--.--║               files are ordered by case-insensitive name
╟────────────────────┴────────┴────────╢
║hello.txt                        23:38║               full name and modification time of selected entry
╚═══ 74028 in 4 files, 828K free ═════╝               directory totals and free space, or marked-entry totals
A:/data> _                                             command line
1Help   2Drive  3View   4Edit   5Copy   6RenMov 7Mkdir  8Delete 9Attr   10Quit
```

- Names longer than 20 characters are truncated with `}`; the full name appears in the details line.
- In the Size column, `..` is shown as `Up` and directories as `Folder`. The Date field for `..` is blank.
- When nothing is marked, the lower border shows the total size and number of files in the directory (excluding subdirectories), plus free space on the volume. Starting at 10,000,000 bytes, the total is displayed in kilobytes with a `K` suffix. `(ro)` indicates a read-only volume. When entries are marked, the border shows their count and total size instead.
- Sizes above 99,999,999 bytes are displayed in kilobytes with a `K` suffix.
- Colors: directories are white, `.cx` programs green, HIDDEN or SYSTEM files gray, and marked entries yellow.
- Entries without timestamps are displayed with dashes.
- A panel holds at most 204 entries. For larger directories the lower border displays `list truncated`.

## 3. Keyboard commands

| Key | Action |
| --- | --- |
| ↑ ↓ PgUp PgDn Home End | Move the cursor |
| Tab | Switch to the other panel |
| Enter | Open a directory; execute a `.cx` program; run a `.bs` BScript program through `A:/bin/bs.cx`, or a compiled `.bsc` program through `A:/bin/bsvm.cx path --`. Other files are not opened (use F3 to view them). If the command line is nonempty, execute the entered command instead. |
| Ins | Mark the entry and advance to the next one |
| `+` `-` `*` | With an empty command line: mark all files, clear all marks, or invert marks |
| F1 | Help |
| F2 | Select a volume for the active panel (arrow keys and Enter, a volume letter, or digit 0–7) |
| F3 | View a file (section 4), or calculate a directory's size (section 5) |
| F4 | Edit a file with `A:/bin/edit.cx` (see *edit Text Editor*, section 3) |
| Shift+F4 | Start the editor without a file; the name is requested when saving |
| F5 | Copy |
| F6 | Rename or move |
| F7 | Create a directory |
| F8 | Delete |
| F9 | Edit RDONLY, HIDDEN, and SYSTEM attributes |
| F10 | Quit |
| Ctrl+R | Refresh both panels |
| Ctrl+Enter | Insert the selected entry's name into the command line (see below) |
| Ctrl+E, Ctrl+X | Previous/next command in the eight-entry history |
| Esc, Ctrl+U | Clear the command line |
| Backspace | Delete one command-line character |

**Command line.** `fm` handles `cd path`, `X:` (switch volumes, using a letter `A`–`H` or a digit), and `exit` internally. Any other command is written to `A:/tmp/.fmcmd` and executed as a shell script with `A:/sys/shell.cx A:/tmp/.fmcmd`, using the active panel's directory. Consequently, executable lookup, redirection, pipelines, and shell built-ins all work. If the temporary file cannot be written, the original command is passed directly to `SPAWN`.

**Ctrl+Enter** produces an event with character code 0 and Enter scan code `$1C` (see *Machine*, revision 9, Section 2.6); ordinary Enter produces character `$0A`. `fm` inserts the selected name, quoting it if it contains spaces or shell metacharacters, followed by a space. It does nothing for `..`. Ctrl+Enter works in the web debugger (both its main window and display panel) and in the Windows console version of the command-line emulator. Unix terminals generally send it as Enter (*Machine*, section 9.2).

**Launching programs.** Before launching a program, `fm` switches the display to ordinary teletype mode. When the child exits, `fm` displays `--- fm: press any key ---` and waits. Screen memory is not readable; once the panels are redrawn, there would be no way to recover the child's output. Both panels are refreshed after the keypress.

**Editor (F4).** The editor is a separate program, `A:/bin/edit.cx`. `fm` passes the complete path of the selected file, quoting it when necessary. F4 does nothing on directories or `..`. Because the editor is full-screen, `fm` neither prints a command line nor waits for a key after it returns; it refreshes the panels immediately. If the editor cannot be started (for example, if `A:/bin/edit.cx` is missing), `fm` displays `fm: A:/bin/edit.cx: error text` and waits for a key. Running the editor in a separate process gives it its own memory instead of sharing `fm`'s panel buffers (see *edit Text Editor*, section 7.1).

**Shift+F4** starts the editor without opening a file, in the active panel's directory. The user names the file through the editor's **Save As** dialog; that dialog initially points to the panel directory. Alternatively, `edit name` on the `fm` command line creates a new file with the specified name. `fm` detects Shift through the event modifiers (`CON_GETEV`, R2; see *System Calls*, section 9.5). In the command-line emulator, Shift+F4 is distinguishable only when the terminal sends an xterm sequence for it (*Machine*, section 9.2).

## 4. File viewer (F3)

Text mode wraps long lines, expands tabs to the next multiple-of-eight column, and skips `\r` characters. The viewer does not load an entire file into memory: it reads only the window starting at the top visible line. File size is therefore not limited by available memory. A file containing many control characters in its first 256 bytes opens in hexadecimal mode automatically.

| Key | Action |
| --- | --- |
| ↑ ↓ PgUp PgDn Home End | Scroll |
| F4 | Switch between text and hexadecimal views |
| Esc, F3, F10, `q` | Exit the viewer |

The top line displays the path, starting offset of the top line, file size, and percentage viewed.

## 5. File operations

F5, F6, F8, and F9 operate on marked entries, or on the selected entry if none are marked. `..` can never be selected as an operation target.

**Copy (F5).** The destination defaults to the other panel's directory but can be edited. Relative paths are resolved against the active panel's directory.

- If the destination is an existing directory, entries are copied into it under their original names.
- Otherwise, the destination is treated as a new name; only one entry can be copied in this case.
- Directories are copied recursively. Copies retain their source modification times and attributes.
- If the destination file exists, prompt `[Y]es [N]o [A]ll [Esc] Cancel`.
- Copying a file onto itself produces `same file`; copying a directory into its own subtree produces `destination is inside the directory`. Paths are compared case-insensitively, just like file names (see *System Calls*, section 4.1); copying `a` over `A` in the same directory also produces `same file`.
- An incomplete destination copy is removed after an error or interruption.

**Move (F6).** On the same volume, use `RENAME`; between volumes, copy and then delete the source. If anything was skipped during copying, the source is retained.

- **Case-only rename.** A destination naming the same entry with different letter case (`readme` → `README`) requests a rename rather than a move, and the name adopts the supplied capitalization. The same applies to directories: destination `DIR` for directory `dir` is not interpreted as a target directory into which to move it.
- **Existing destination.** If the target file exists, prompt `[Y]es [N]o [A]ll [Esc] Cancel`; if confirmed, remove it before moving the source. The source itself, even when named with different capitalization, is never treated as an existing destination to be removed.

**Delete (F8).** There is one confirmation for the entire operation. Directories are deleted recursively. Only `Y`/`y` confirms deletion; `N`/`n` and Esc cancel. Enter does not count as an answer. RDONLY entries require an additional `[Y]es [N]o [A]ll [Esc] Cancel` prompt, again without Enter as a valid response; confirming clears the attribute.

**Directory size (F3 on a directory).** `fm` traverses the directory and all its subdirectories (Esc cancels) and shows a dialog containing the number of files and subdirectories, their total file size in bytes, and volume space used. Space usage includes data clusters and IND, DIND, and second-level index clusters (see *FS16*, section 6) belonging to files and directories, including the selected directory itself.

**Create directory (F7).** A name that differs from an existing one only in letter case (`NEWDIR` when `newdir` exists) fails with `already exists`. A directory cannot contain two such names (see *FS16*, section 7.1).

**Attributes (F9).** R, H, and S toggle the corresponding attributes in the dialog; Enter applies the selected flags to all selected entries.

**Errors.** An error affecting an entry appears in a dialog with its path and the error message (see *Shell and Utilities*, section 8.2). Enter or S skips the entry; Esc aborts the operation. Pressing Esc during a copy, move, or delete operation requests confirmation before aborting. Keys pressed during an operation are not lost: `fm` queues them for later processing.

After each operation, both panels are refreshed. The cursor remains on the same entry if it still exists; its name is matched case-insensitively, so changing capitalization does not reset the cursor. A panel whose directory was deleted moves up to the nearest existing ancestor.

## 6. Implementation

| File | Purpose |
| --- | --- |
| `user/fm/fm.b16` | Panels, command line, keyboard handling, volume selection, help, `main` |
| `user/fm/scr.b16` | Display registers, borders, keyboard and deferred-event queue, dialogs |
| `user/fm/view.b16` | File viewer |
| `user/fm/ops.b16` | Copy, move, delete, directory operations, attributes |

Build with `tools/b16build.sh user/fm/fm.b16` (also performed by `make system`). Executable header fields are declared in `fm.b16`: `option max_top = EWIN` (`$A000`) and `option min_free = 4096`. The same `EWIN` constant specifies the panel-entry array address.

**Memory.** The process's own address space ends at `$A000` (`max_top` = 40 KB). Panel entries (`[2][204]Stat`, 16,320 bytes) occupy two extended-memory frames. At startup `fm` allocates these frames with `XALLOC`, maps them to pages 5 and 6 with `XMAP`, and accesses the array declared `at $A000`. If two free frames are not available, `fm` exits with `fm: not enough memory`, code 1. Monitor frees the frames when the process terminates.

Static data plus `min_free` (4 KB reserved for the stack) must end at or below `$A000`; otherwise Monitor would allocate process memory up to `$C000` and page 5 would already be occupied. For this reason the copy buffer is 3.5 KB (seven sectors per read), not 4 KB. The viewer uses the first 2 KB of the same buffer. Path buffers (`spath`, `dpath`, `cdst`, `inbuf`, `cwdbuf`) are each 256 bytes: paths are at most 255 bytes (see *System Calls*, section 4.1), and `GETCWD` is called with a size of 256. The `clbuf` child command-line buffer is 276 bytes (editor path, space, quoted path of up to 255 bytes, terminator). Entry ordering in `ord` uses bytes because each panel has at most 204 entries. Following revision 4, only 442 bytes remain below `$A000`; further features in `fm` must be small, with substantial additions implemented as separate programs, as the editor is.

**Directories.** A panel stores its path in `GETCWD` format (`X:/a/b`, uppercase volume letter). To enter a directory, `fm` uses `CHDIR` for the panel directory, followed by `CHDIR` for the entry name and `GETCWD`. Monitor therefore handles `..` resolution and path validation. The process's working directory matches the active panel's directory.

**Tree traversal.** During copy and delete operations, directories are not kept open while their children are processed. Names are read into a 4 KB pool, the directory handle is closed, and entries are processed one by one. Recursion depth is constrained by the path length (*System Calls*, section 4.1), not by the number of open handles. Paths are built in two buffers by appending a name at each level and removing it when returning.

**Keyboard.** Input uses raw mode with `NOBREAK` and `CON_GETEV` events. Ctrl+Enter differs from Enter only by its scan code: character 0 and scan code `$1C` (`K_ENTER` in `scr.b16`). During operations, `fm` polls for Esc using `CON_GETEV` with `NOWAIT`. Other events are deferred in a 16-entry queue and delivered first afterward.

## 7. Testing

F4 and Shift+F4 editor invocation is covered by the `edit` scenario (see *edit Text Editor*, section 8). The `fm` scenario in `tools/run_tests.py` (`tests/scripts/fm.scn`) checks: Ctrl+Enter inserting a name on the command line; copying a file to volume `B:`; creating a directory and rejecting a case-only duplicate with `already exists`; deleting a directory; renaming and changing name capitalization; rejecting a case-only self-copy with `same file`; viewing a file; running `ls` from the command line; and quitting. The scenario waits for screen text and compares screen captures. The `kmon` test harness understands these commands:

    screen "text" steps N ...          wait for text on the screen (including coordinate-based output)
    after "text" steps N dump          print screen contents (dumpa also includes attributes)

Screen captures are written to stdout as UTF-8, after console output. For interactive troubleshooting, use `tests/fm/run.sh SCENARIO` (volumes `out/fmt0.img` and `out/fmt1.img`, configured by `out/fmt*.json`) and `tests/fm/show.py`.

## 8. Limitations and planned improvements

- Program output cannot be restored to the display after returning (Ctrl+O), because screen memory cannot be read.
- No sorting by size or modification time, compact panel view, or file search.
- At most 204 entries per panel.

## 9. Change history

### Revision 6

- **Building programs (B16 revision 3).** Executable parameters (`max_top`, `min_free`) are specified using `option` declarations in the source. System-call wrappers come from `sys.b16` via `include asm`. Both `max_top` and the panel-entry address use the `EWIN` constant. No command-line options are required for the build.
- Volumes use letters (*System Calls*, revision 6): panel headings and the F2 volume list show `A:`, `B:`, and so on. Volumes can be selected by a case-insensitive letter, or by a digit as before. The command line accepts `B:` or `1:`.

### Revision 5

- Enter on a `.bs` file runs it as a BScript program through `A:/bin/bs.cx`; on a `.bsc` file it runs the compiled program through `A:/bin/bsvm.cx path --` (see *BScript*, section 11.4).
- Enter on a file that is neither a program nor a script does nothing; use F3 to view it.

### Revision 4

- **Shift+F4 starts the editor without a file** (section 3). `getkey` in `scr.b16` obtains event modifiers from `CON_GETEV` and stores them in `kmods`; events deferred by `poll_esc` retain their modifiers.
- **Memory** (section 6): path buffers reduced from 260 to 256 bytes; `clbuf` from 300 to 276 bytes; `ord` switched from words to bytes (saving 408 bytes). Remaining space below `$A000`: 442 bytes.

### Revision 3

- **F4 opens the editor** (section 3): the selected file is opened by the separate `A:/bin/edit.cx` program. `fm` does not wait for a key afterward. `exec_child` gained a full-screen-program flag; launching a `.cx` file with Enter and opening the editor now share `run_entry`.
- **Operation path buffers** reduced from 300 to 260 bytes (section 6): otherwise the image with F4 support would not fit below `$A000`.

### Revision 2

- **Ctrl+Enter** (sections 3, 6): event with character 0 and scan code `$1C` (*Machine*, revision 7). The old test for character `$0D` was removed: Ctrl+Enter does not produce that event, while `$0D` with scan code `$32` is Ctrl+M.
- **Case-insensitive names** (section 5; *System Calls*, revision 3): `same file` and “destination inside source directory” checks now compare paths case-insensitively; F6 can change the capitalization of file and directory names; moving an entry does not delete its own source if destination and source differ only in letter case; F7 reports `already exists` for a case-only duplicate; cursor restoration is case-insensitive; sorting accounts for Cyrillic capital Yo (U+0401).
- **Copy buffer** reduced to 3.5 KB (section 6).

### Revision 1

Initial release.
