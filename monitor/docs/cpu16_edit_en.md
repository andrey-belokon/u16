# CPU16 Monitor: The `edit` Text Editor, Revision 2

`edit` is a full-screen text editor. It is a regular B16 program in CX16 format (`A:/bin/edit.cx`) and uses only system calls. From the `fm` file manager, F4 opens the selected file in the editor and Shift+F4 opens an untitled document (see *The fm File Manager*, Section 3). From the shell, run `edit`.

Revision 2 introduces Windows-style Shift+arrow selection and a clipboard, undo (Ctrl+Z), untitled documents, and a Save As dialog. Changes are summarized in Section 10.

Sections 1–6 form the user guide; Section 7 describes the implementation, and Section 8 explains testing.

## 1. Starting the editor

    edit [file]

- **With a file.** A relative path is immediately converted to an absolute path, which appears in the status line. If the file does not exist, the editor opens an empty buffer (status: `new file`) and creates the file when saving.
- **Without a file.** Opens an empty, `untitled` buffer. The first save (F2) opens Save As (Section 6.2).
- More than one argument prints `usage: edit [file]` and returns exit status 2.

Like `fm`, the editor draws directly to the display in coordinate mode (see *System Calls*, Section 6.4). The Display window of the web debugger is a convenient way to use it. On exit, the display returns to ordinary teletype mode and is cleared.

Exit status 0 indicates success; 1 indicates that the file could not be opened (an error dialog was displayed) or startup ran out of memory.

## 2. Screen layout

```text
 A:/data/hello.txt                                * Ln 13/67  Col 34  Ins  CRLF  3937    status line
Line number 10 of the text file, padded to be somewhat long.                           lines 1–23: text
...
1Help   2Save   3Next   4Replac 5       6       7Search 8       9GoLine 10Quit        function keys
```

- On the **left**, the file path is shown (its end is retained if it is too long), or `untitled` for a nameless buffer. The most recent operation may temporarily replace it with a message such as `saved`, `not found`, `5 replaced`, `nothing to undo`, or `not enough memory`, until the next keypress.
- On the **right**, `*` means the text has changed since being loaded or saved; the display also shows the cursor line and total lines, column, `Ins` or `Ovr` (insert/overwrite), `CRLF` for files with `\r\n` line endings (Section 6.1), `RO` for RDONLY files, and text length in bytes.
- Long lines do not wrap. The view scrolls horizontally when the cursor moves beyond the screen edge.
- Tabs expand to the next multiple-of-eight column. Control characters are rendered with display glyphs (see *Machine*, Section 5).
- Selected text is shown in black on cyan; a selected newline highlights the rest of its screen line.

## 3. Keyboard shortcuts

| Key | Action |
| --- | --- |
| ← → ↑ ↓ | Move cursor; vertical movement retains the column at which the movement started |
| Ctrl+← Ctrl+→ | Move one word left or right |
| Home, End | Beginning or end of line |
| PgUp, PgDn | Move one page up or down; scrolling stops at the final text line |
| Ctrl+Home, Ctrl+End | Beginning or end of document; also Ctrl+B and Ctrl+E for terminals that do not send modifiers |
| Shift + any navigation key above | Select text (Section 4) |
| Ctrl+A | Select all |
| Ctrl+C, Ctrl+Ins | Copy selection to clipboard |
| Ctrl+X, Shift+Del | Cut selection |
| Ctrl+V, Shift+Ins | Paste, replacing the selection if present |
| Ctrl+Z | Undo (Section 5) |
| Enter | Insert a newline with the current line's indentation (spaces and tabs before the cursor) |
| Backspace, Del | Delete character before/at cursor; delete selection if one exists |
| Ctrl+Y | Delete line |
| Ins | Toggle insert/overwrite mode |
| Tab | Insert tab character |
| F1 | Help |
| F2, Ctrl+S | Save; Save As for an untitled document |
| Shift+F2, F12 | Save As (Section 6.2) |
| F7 | Find; F3 or Ctrl+L finds next (Section 5.2) |
| F4 | Replace |
| F9, Ctrl+G | Go to a line number |
| F10, Esc | Exit; if modified, prompt `[Y]es [N]o [Esc] Cancel` (Y and Enter save and exit) |

Typing a character, Enter, or Tab replaces the current selection.

The editor obtains modifier state from keyboard events (`CON_GETEV`, R2; see *System Calls*, Section 9.5). The web debugger supplies it for all keys. The command-line emulator can supply it only when the terminal sends modifier-aware xterm sequences (see *Machine*, Section 9.2), so primary commands also have Ctrl+letter alternatives. Ctrl+T, Ctrl+W, and Ctrl+N are unused because browsers intercept them in the web debugger.

The editor enables raw input with `NOBREAK`, so Ctrl+C copies text rather than terminating the program and losing unsaved changes.

## 4. Selection and clipboard

A selection is the range between an **anchor** and the cursor. Shift+movement starts the selection at the original cursor position (the anchor), or extends an existing selection. Movement without Shift clears it. Ctrl+A places the anchor at the start of the document and the cursor at its end.

- **Copy** (Ctrl+C) copies selected text to the clipboard and leaves the selection intact.
- **Cut** (Ctrl+X) copies and deletes the selection.
- **Paste** (Ctrl+V) deletes an existing selection, inserts clipboard contents, and leaves the cursor after the inserted text.
- **Del, Backspace, and typing** delete the selection, if present. Typing also inserts the new character.

Clipboard contents persist until the editor exits; their size is limited only by memory availability (Section 7.6). If copying or pasting runs out of memory, neither the text nor the clipboard changes, and the status line reports `not enough memory`.

## 5. Undo and search

### 5.1. Undo

Ctrl+Z undoes the most recent **editing step** and restores the cursor position from before that step. The step boundaries are:

- A run of consecutively typed characters without intervening cursor movement counts as one step; the same applies to consecutive Backspace or Del presses.
- Enter, Ctrl+Y, paste, cut, and deletion of a selection each create a step. Typing over a selection counts as a single step together with subsequent continuous typing.
- A complete Replace operation (F4) is one step, regardless of how many occurrences it changes.

At most **1,000** previous steps can be undone. The undo log uses at most **256 KB** (32 frames), and it never allocates a frame if that would leave fewer than 16 free frames in the machine. When space runs short, the oldest steps are evicted. If the text itself needs more frames, the log gives up its oldest entries as well: preserving the text takes priority over undo history.

Deleting more than 253,952 bytes (the log capacity minus one frame) cannot be undone. Before such an operation, the editor asks `Deleting N bytes cannot be undone. Continue?` (`[Y]es [N]o`; Enter is not accepted). If an editing step still cannot fit in the log, the log is cleared and the status becomes `undo history lost`. If there is no step to undo, the status is `nothing to undo`. Redo is not supported.

Undo sets the modified marker (`*`) even if it restores exactly the last saved contents.

### 5.2. Find and replace

**Find (F7)** searches forward from the cursor, case-insensitively, including Latin and Cyrillic letters and the Cyrillic Yo variants, using the same comparison rules as filenames. A match becomes the selection, with the cursor at its end. F3 and Ctrl+L find the next occurrence using the same search string; if no string has been entered, the editor prompts for one. A failed search reports `not found`.

**Replace (F4)** prompts for the search and replacement strings and proceeds from the cursor (or the start of the selection, if present). Each match is selected and followed by the prompt `[Y]es [N]o [A]ll [Esc] Stop`. A replaces the current and all later matches without asking again. The final status is `N replaced`. To avoid obscuring the current line with the prompt, the editor scrolls it to the third screen row. Replacement inserts the new text *before* deleting the match, so a memory failure leaves the original match unchanged.

## 6. Reading and writing files

### 6.1. File I/O

**Line endings.** On input, `\r\n` pairs are converted to `\n`, and the status line shows `CRLF`. On output, each `\n` is converted back to `\r\n`. An isolated `\r` is preserved as-is and written back unchanged. A file containing a mixture of `\r\n` and `\n` line endings is saved entirely with `\r\n` endings.

**Saving (F2)** opens the file with `O_WRITE | O_CREATE`. It first expands the file to the required length through `SETSIZE`, writes from the beginning, then truncates it at the end if the new text is shorter. If the volume lacks space, `SETSIZE` fails *before any data is overwritten*, preserving the previous file contents. Monitor updates the modification time; file flags are not changed.

**Read/write errors** are shown in a red dialog containing the path and error message (see *Shell and Utilities*, Section 8.2):

- Opening a directory as a file: `is a directory`.
- File exceeds the editor limit (Section 7.6): `too large: N bytes, the editor holds M`.
- Frame allocation fails while reading: `not enough memory`.
- Writing an RDONLY file or a read-only volume: `read-only`.

A file-open failure terminates the editor with status 1. A save failure leaves the editor running with its current text intact.

### 6.2. Save As

Shift+F2 or F12—and F2 for an untitled document—opens the file browser:

```text
┌────────────────────────────────── Save as ───────────────────────────────────┐
│ A:/data                                                                      │
│  ..                                                                  <DIR>   │
│  sub                                                                 <DIR>   │
│  hello.txt                                                            3895   │
│ ...                                                                          │
│ Name: hello2.txt                                                             │
│ Enter - save / open   Tab - list / name   Esc - cancel                       │
└──────────────────────────────────────────────────────────────────────────────┘
```

- **Initial directory:** the current file's directory, or the editor's current directory for an untitled document (the active panel's directory when launched from `fm`). The list shows `..`, then directories, then files, sorted case-insensitively by name. It holds at most 200 entries; excess entries are indicated by `(the list is cut)`.
- **Name field:** initially contains the current filename. Typing and Backspace edit it; Ctrl+U clears it.
- **Arrows, PgUp, PgDn, Home, End:** navigate the list. Highlighting a file copies its name to the name field. Tab switches focus between list and field.
- **Enter on a directory in the list** (including `..`) opens that directory.
- **Enter in the name field** (or on a listed file): if the name denotes a directory or volume (`sub`, `..`, `1:`, `A:/bin`), navigate there. Otherwise treat it as a filename relative to the dialog's current directory. If the file exists, prompt `The file exists. Overwrite it?` (`[Y]es [N]o`; Enter does not confirm). Then save the text (Section 6.1) and continue editing under the new filename.
- **Esc** closes the dialog without saving. If Save As was opened during an exit-with-save operation, cancelling it also cancels the exit.

## 7. Implementation

| File | Contents |
| --- | --- |
| `user/edit/edit.b16` | Complete text editor |
| `user/fm/scr.b16` | Shared with `fm`: display registers, frames and borders, keyboard (`getkey`, with modifier state `kmods`), and dialogs |

Build with `tools/b16build.sh user/edit/edit.b16` (also invoked by `make system`). The header fields are set in `edit.b16`: `option max_top = PTOP` (`$A000`) and `option min_free = 4096`. `PTOP` is also the window-B address and the upper limit checked by `edit` at startup.

### 7.1. Memory

The editor's own process memory ends at `$A000` (`max_top` = 40 KB; code and data currently end at `$6A7A`) and holds its code, data, tables, and stack. Text, undo log, clipboard, and the Save As directory list are stored in extended-memory frames allocated through `XALLOC` (see *System Calls*, Section 7.6). Two windows access and modify those frames:

| Window | Page | Addresses | Purpose |
| --- | --- | --- | --- |
| A | 6 | `$C000–$DFFF` | Text block being edited; source for insertion (text, clipboard, log) |
| B | 5 | `$A000–$BFFF` | Second block for inter-block transfers; newly allocated insertion frames; undo-log and clipboard writes; Save As directory list |

The windows are declared as `at` variables. A frame is mapped with `XMAP` only when a different frame currently occupies the window. The editor tracks both window mappings, so any function can map its frame into the appropriate window. If the process's own memory extends beyond `$A000`, the editor refuses to start (`edit: not enough memory`). Monitor frees the frames when the process terminates.

While the editor runs, `fm` remains resident with its own frames: the editor is a separate child process (see *System Calls*, Section 2.1), and its text does not share memory with the `fm` panels.

### 7.2. Text blocks

The block table records each frame number and block length in document order. Text positions are 32-bit quantities. Block start offsets are not stored: the editor caches the last located block and its starting offset, then searches forward or backward from there. Sequential text traversal therefore costs only one block-table step per block.

| Parameter | Value |
| --- | --- |
| `FRAME` | 8192-byte frame; log and clipboard are streams of whole frames |
| `BLK` | 8192-byte maximum text-block length |
| `FILL` | 7168-byte target fill on file read and insertion; reserves 1 KB for edits without splitting |
| `MAXBLK` | At most 224 blocks |

An empty block is allowed only as the sole block of an empty document.

- **Inserting** a few bytes modifies one block by shifting its tail inside window A. Inserting at the start of a block uses spare space at the end of the preceding block, if available, without shifting. If the block lacks space, it splits in half; the latter half is transferred into a new frame through window B.
- **Deleting** a range frees blocks wholly contained in it and shifts the tails of boundary blocks. After deletion, the block at the deletion point merges with a neighbor when either is shorter than `BLK / 4` and their combined length is no more than `FILL`.
- **Streaming insertion of arbitrary length** (`ins_stream`, used for clipboard paste and undoing deletions) splits the cursor block at the cursor, builds inserted content in new frames through window B, and only then splices the new blocks into the table. `src_at` reads the source through window A, mapping frames from the text, clipboard, or log as required. Text positions remain unchanged while the insertion is being assembled. If frame allocation fails, the new frames are freed and the document is unchanged.
- **Lines:** the cursor line number and total line count are cached and updated after edits and movement by counting `\n` characters in the traversed range.
- **Redrawing:** after a keypress, the entire display is redrawn only if the text scrolls, the line count changes, or a selection is active. Otherwise, only the cursor line and status line are redrawn.
- **File reading:** data is read directly into window A in `FILL`-byte chunks. `\r\n` pairs are collapsed in place, and a trailing `\r` at a chunk boundary is resolved when the next chunk arrives. **Writing** streams blocks directly from window A, using a 512-byte conversion buffer when `\r\n` endings are required.

### 7.3. Editing and the undo log

Edits have two layers:

- `t_insb`, `t_delete`, and `ins_stream` update text, cursor, line number/count, viewport start, and selection anchor without touching the log. Undo calls these functions too.
- `ed_insb`, `ed_delete`, and `ed_stream` do the same but also record the change. A deletion is logged *before* changing the text (because its record contains the removed bytes); an insertion is logged afterward.

Each command begins an edit with `ed_begin(kind)`. Kind 0 always begins a new step. Typing (1), Backspace (2), and Del (3) extend the previous step only when the preceding command had the same kind and the cursor has not moved in between.

The **undo log** is a stream of records in `[jbeg, jend)`, backed by up to 32 frames in `jfr`; `jf0` is the offset of the first frame's beginning within the stream. Each record has this layout:

| Offset | Length | Contents |
| --- | --- | --- |
| 0 | 1 | Kind: 1 = insertion, 2 = deletion |
| 1 | 1 | Flags: bit 0 indicates the start of an editing step |
| 2 | 4 | Position |
| 6 | 4 | Length |
| 10 | 4 | Cursor position before the step |
| 14 | length | Deleted bytes, for deletion records |
| end − 4 | 4 | Total record length |

Undo walks records backward using their trailers; eviction of old steps walks forward using their headers. Consecutively typed characters do not create separate records: a new insertion that continues the previous insertion in the same step extends that record's length. Undo processes a step's records in reverse order, deleting insertions and reinserting deletions from the log (`ins_stream` with the log as source), then restores the cursor. Frames beyond the new log end are freed.

The limits (`JSTEPS` = 1,000 steps, `JMAXF` = 32 frames, `JRES` = 16 free machine frames) are enforced by evicting entire oldest steps and freeing any frames completely before the new log start. The current step is never evicted. If even that one step cannot fit, the log is cleared (`undo history lost`) and further records from the current step are omitted.

### 7.4. Clipboard

The clipboard holds a sequence of fully used frames (except possibly the last) and a length. Copying a selection frees the old clipboard, allocates frames, and copies text from window A into window B. Pasting calls `ins_stream` with the clipboard as its source.

### 7.5. Save As

The dialog allocates a frame and maps it into window B. It holds directory entries from `READDIR` (up to 200 entries at 40 bytes each). Display order is held as a byte array in process memory and produced by insertion sort: directories first, then case-insensitive name order. Navigating directories uses the editor process's `CHDIR`; after leaving the dialog, the editor restores its startup directory. `canon` builds the file path: the parent directory must already exist, and `GETCWD` returns its absolute path.

### 7.6. Limits

- Maximum file size on open: `(MAXBLK − 8) · FILL` = 1,548,288 bytes; eight blocks are reserved for editing and splitting.
- Text size during editing is limited by `MAXBLK` and the number of free machine frames (see *Machine*, Section 7.6); the undo log yields its frames as needed. If no frames remain, insertion, copy, and paste fail with `not enough memory` and leave the text unchanged.
- The undo log retains up to 1,000 steps and 256 KB (Section 5.1).
- Lines longer than 65,535 columns are displayed incorrectly (the column value is 16 bits).

## 8. Testing

The following scenarios run through `tools/run_tests.py` under every timer-tick schedule; volume integrity and the return of allocated frames are checked:

- `edit` (`tests/scripts/edit.scn`): edit and save a CRLF file while preserving its endings; Shift+↓ selection, cut, paste at end of document, selection deletion, overwrite mode, Del, undo typing, and Ctrl+S; create a new file; a 100-character line (horizontal scrolling); find using F7, F3, and Ctrl+L; go to a line; exit with save; F4 from `fm` and return to panels; Shift+F4 to edit an untitled buffer and save via Save As in the panel directory; Shift+F2 to save in a subdirectory; F12 to overwrite with confirmation.
- `editbig` (`tests/scripts/edit_big.scn`): edit a 280,000-byte file (40 blocks) at both ends; search; select 28 KB through end-of-file, copy and paste at the end, undo the paste, paste again, and save. Also load and save a 59,500-byte CRLF file. `run_tests.py` creates the inputs under `out/` and checks the resulting volume files byte-for-byte using `fs16tool.py get`.
- `editundo` (`tests/scripts/edit_undo.scn`): undo-log limits. From 1,200 distinct Enter steps, only the last 1,000 can be undone. Deleting a whole 280,000-byte file prompts for confirmation, and declining leaves the text intact. After three deletions of about 100 KB each, the oldest one is evicted from the log; the last two can be undone, followed by `nothing to undo`.

Randomized testing uses `tests/edit/stress.py [first_seed [seed_count [action_count]]]`. Random key sequences—typing, Backspace, Del, autoindenting Enter, Ctrl+Y, arrows, Home, End, Ctrl+B, Ctrl+E, Shift+→ selection, Ctrl+C, Ctrl+X, Ctrl+V, Del, and typing over a selection—are compared against a Python reference model. The test build sets `BLK` = 512 and `FILL` = 448 so splitting and merging occur frequently. The saved file is checked, then every step is undone; the text saved through F12 must match the original. All frames must be returned and the volume must remain valid.

## 9. Limitations and planned work

- No redo support.
- No soft wrapping of long lines.
- No rectangular selection.
- The clipboard belongs to one editor invocation and is not shared across processes.
- In the command-line emulator, Shift+arrow selection works only when the terminal transmits modifier-aware sequences (see *Machine*, Section 9.2).

## 10. Revision history

### Revision 2

- **Program build (B16 revision 3).** Image parameters (`max_top`, `min_free`) are declared through `option` in the source. System-call wrappers come from `sys.b16`, included via `include asm`. `max_top` equals the window-B address through `PTOP`; the build command no longer needs options.
- **Selection and clipboard** (Sections 3–4): Shift+movement, Ctrl+A, Ctrl+C / Ctrl+X / Ctrl+V, and Ctrl+Ins / Shift+Del / Shift+Ins; typing replaces a selection. The old F3 mark, F5 copy, F6 move, and F8 delete-block operations have been removed in favor of selection, clipboard, and Del.
- **Undo** (Sections 5.1 and 7.3): Ctrl+Z, up to 1,000 steps and a 256 KB undo log.
- **Find:** a match becomes the selection with the cursor at its end; F3 finds next (Section 5.2).
- **Untitled documents and Save As** (Sections 1, 6.2, 7.5): `edit` without arguments; Shift+F2 and F12 open the file browser.
- **Word navigation:** Ctrl+← and Ctrl+→; beginning and end of document: Ctrl+Home and Ctrl+End.
- **Memory** (Section 7.1): the process is limited to `$A000` (40 KB). The undo log, clipboard, and dialog listing use frames, still accessed through two windows.

### Revision 1

Initial version: text blocks stored in extended-memory frames; F3, F5, F6, and F8 for marking and block operations; search, replace, go-to-line, and `\r\n` handling.
