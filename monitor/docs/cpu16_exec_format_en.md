# CPU16 Monitor: Executable File Format and Program Startup — Revision 4

Revision 4 documents how to build programs with B16 revision 3: the `min_free` and `max_top` header fields and assembly modules are specified in the source code. Revision 3 brings this document into alignment with the Monitor system-call specification and B16 revision 2. The file format, header, PIB, and argument block remain unchanged. Section 11 contains the revision history.

This document specifies the executable file format used by CPU16 Monitor, the process information block (PIB), the layout of command-line arguments, and the initial state of a program.

**Related documents:**

| Document | Referenced features |
|---|---|
| *16-bit Processor Architecture*, revision 6 | Registers, interrupts, `TRAP`, `IRET` |
| *CPU16 Machine: Memory and Devices*, revision 9 | Memory manager, timer, console, shutdown procedure |
| *Assembler Reference* | `.reserve`, `.include`, and the `--exec` option |
| *B16 Language Specification*, revision 3 | `std` and `kernel` targets, startup code, `option min_free`, `option max_top`, `include asm` |
| *CPU16 Monitor: System Calls*, revision 6 | Process termination, `SPAWN`, error codes, rules for applications |

The last document is referred to below as *System Calls*.

Sections 1–8 are normative: “must” and “must not” express requirements for Monitor and applications. Section 9 provides implementation guidance, and Section 10 describes toolchain changes. Neither is normative.

System calls, error and exit codes, child-process creation (`SPAWN`), and rules for accessing devices are specified in *System Calls*. They are mentioned here only where relevant to program startup.

## 1. Overview

The executable file is a flat memory image starting at address `$0000`, as produced by the assembler. It differs from a raw image in one respect: immediately after the initial jump is a **header** containing 12 words of information for the loader.

The Monitor loader:

1. Validates the header and allocates memory for the program.
2. Loads the image at address `$0000` and zeroes uninitialized data.
3. Places the parsed command line (the **argument block**) immediately after the program's static data.
4. Overwrites the header with the **PIB**; the header is no longer needed after loading.
5. Transfers control to `$0000`.

No code relocation takes place. The memory manager gives each process a separate address space, and every program is loaded at `$0000`.

**Terminology:**

| Term | Meaning |
|---|---|
| image | File contents corresponding to addresses `$0000` through `image_end − 1` |
| bss | Uninitialized static data in the range `image_end … bss_end − 1`; not stored in the file and cleared by the loader |
| `bss_end′` | `bss_end` rounded up to an even address |
| argument block | The `argv` array and argument strings, starting at `bss_end′` |
| `args_size` | Size of the argument block in bytes, always even (Section 6) |
| `top` | Upper boundary of the memory allocated to the process, aligned to `$2000` |
| PIB | Process information block at addresses `$0004–$001B` |

## 2. Process Address Space

Monitor runs with the memory manager enabled. Page 7 (`$E000–$FFFF`) is shared by all processes and contains the resident Monitor kernel, interrupt vector table, and device window. Pages 0–6 are available to the process.

| Address range | Purpose |
|---|---|
| `$0000 … top − 1` | Process memory |
| `top … $DFFF` | Unallocated; the process must not access this range |
| `$E000–$FFFF` | Shared Monitor page (resident kernel code and exit routine), vectors, and devices |

- `top` cannot exceed `$E000`.
- There is no memory protection. Accessing memory beyond `top`, or writing to page 7, is a programming error with undefined results.

## 3. Executable File

### 3.1. Layout

| File offset | Contents |
|---|---|
| `$0000–$0003` | Initial `JMP #start` instruction |
| `$0004–$001B` | Header |
| `$001C … image_end − 1` | Code and initialized data |
| `image_end …` | Optional trailing data, not read by the loader |

The file is a memory image: the byte at file offset A is loaded at address A.

### 3.2. Header

All fields are words. Byte order is the machine's native order: the least significant byte is stored at the lower address.

| Address | Field | Meaning |
|---|---|---|
| `$0000` | jump | `$C873` — instruction word for `JMP #expression` |
| `$0002` | `start` | Entry-point address |
| `$0004` | `magic` | Four bytes, `"CX16"`: `$43 $58 $31 $36` |
| `$0008` | `version` | Format version, 1 |
| `$000A` | `flags` | 0 |
| `$000C` | `image_end` | Image size in bytes |
| `$000E` | `bss_end` | End of static data |
| `$0010` | `min_free` | Minimum free space for the stack and heap, in bytes |
| `$0012` | `max_top` | Requested upper memory boundary; 0 requests the maximum |
| `$0014–$001B` | reserved | Four words; must all be zero |

The initial jump must use the absolute form `JMP #start`. Only that form is guaranteed to occupy two words, ensuring that the header always begins at `$0004`.

### 3.3. Header Validation

The loader rejects a file if any of the following conditions is not met:

1. The word at `$0000` is `$C873`, and bytes `$0004–$0007` contain `"CX16"`.
2. `version` = 1.
3. `flags` = 0. Unknown flag bits cause rejection; they are not silently ignored.
4. All reserved words are zero.
5. `$001C` ≤ `image_end` ≤ `bss_end` ≤ `$E000`.
6. `start` is even and `$001C` ≤ `start` < `image_end`.
7. `min_free` is even.
8. The file is at least `image_end` bytes long.

`max_top` is not validated: values greater than `$E000` are capped at that address (Section 6).

## 4. Process Information Block (PIB)

Once loading is complete, addresses `$0004–$001B` contain the PIB. The `version` and `bss_end` fields occupy the same offsets as in the executable header.

| Address | Field | Meaning |
|---|---|---|
| `$0004` | `magic` | Four bytes, `"CXPB"`: `$43 $58 $50 $42` |
| `$0008` | `version` | PIB version, 1 |
| `$000A` | `flags` | Bit 0: standard input is redirected (descriptor 0 is not the console). Bit 1: standard output is redirected (descriptor 1 is not the console). All other bits are zero. Monitor sets these bits (*System Calls*, Section 7.1) |
| `$000C` | `argc` | Number of arguments; at least 1 |
| `$000E` | `bss_end` | Value copied from the header |
| `$0010` | `argv` | Address of the `argv[0..argc]` array |
| `$0012` | `args_end` | First byte past the argument block (even address) |
| `$0014` | `top` | Upper boundary of allocated memory |
| `$0016–$001B` | reserved | Three words, all zero |

- Monitor neither reads nor modifies the PIB after starting the program. The program may reuse this memory for any purpose.
- Program labels located within `$0004–$001B` refer to PIB fields after loading. A running program must not attempt to read its original executable header.
- The signature identifies the startup mode: `"CXPB"` at `$0004` means the program was started by Monitor; `"CX16"` means the image was loaded directly on a bare machine (Section 8).

## 5. Command-Line Arguments

### 5.1. Argument Block

The argument block occupies addresses `bss_end′ … args_end − 1`. Its contents, in ascending address order, are:

1. The `argv[0..argc]` array, with one word per entry holding a string address and `argv[argc] = 0`.
2. Argument strings in index order, each null-terminated and packed immediately after the preceding string.
3. One zero padding byte if the total length is odd.

The `argv` field in the PIB always equals `bss_end′`. Programs should nevertheless obtain this address from the PIB or register R1.

The argument block is ordinary process memory and is not protected by Monitor. A program that needs its arguments must read or copy them before reusing this region for the heap (Section 7).

### 5.2. Parsing

Monitor parses the command line passed to a program-launch call. The same word-separation and quoting rules govern the arguments reconstructed by the shell and those supplied by a program calling `SPAWN` or `EXEC`.

- **Separators** are spaces (`$20`) and tabs (`$09`). Runs of separators are treated as a single separator; leading and trailing separators are ignored.
- **Quotes.** Text inside double quotes (`"…"`) belongs to one argument, including any separators. The quotes themselves are removed. Quoting may occur within a word: `a"b c"d` yields `ab cd`. An unmatched quote is an error. There is no escape mechanism, so a literal double quote cannot be included in an argument.
- **Empty arguments.** `""` is valid and produces a zero-length string.
- **`argv[0]`** is the first word of the command-line string supplied to `SPAWN` or `EXEC`, passed to the program without alteration by Monitor. That word is also the path used to open the executable. Monitor does not search directories or append an extension; when launching a program, the shell resolves the program name and constructs this first word (*System Calls*, Section 7.1; *CPU16 Monitor: Shell and Utilities*, Section 3.3).
- **Input/output redirection** is handled by the shell before launching the program. Redirection syntax is not included in the argument block.
- **Limits:** a command line may contain at most 255 bytes and at most 32 arguments, including `argv[0]`. Exceeding either limit prevents the program from starting.

The original, unparsed command line is not passed to the program; only the parsed arguments are available.

## 6. Memory Allocation

Let `args_size` be the size of the argument block (Section 5.1), in bytes: 2 × (`argc` + 1), plus the lengths of all argument strings including their null terminators, rounded up to an even number. The block spans `bss_end′ … args_end − 1`, so `args_end` = `bss_end′` + `args_size`. Let `⌈x⌉` denote rounding upward to a multiple of `$2000`.

| Quantity | Formula |
|---|---|
| Required memory, `need` | `⌈bss_end′ + args_size + min_free⌉` |
| Requested memory, `want` | `$E000` if `max_top` = 0 or `max_top` > `$E000`; otherwise `⌈max_top⌉` |
| `top` | The lesser of `want` and the amount of available memory, but never less than `need` |

- If `need` exceeds `$E000` or the available memory, program startup fails.
- If `want` is less than `need`, `need` takes precedence.
- Calculations must not overflow. Any intermediate sum exceeding `$E000` causes startup to fail.

The process receives `top / $2000` frames mapped into pages 0 through `top/$2000 − 1`. All allocated frames are zero-filled except for the data written by the loader. Free process memory spans `args_end … top − 1`. The stack starts at `top` and grows downward.

## 7. Process Memory After Loading

| Address range | Contents |
|---|---|
| `$0000–$0003` | `JMP #start` |
| `$0004–$001B` | PIB |
| `$001C … image_end − 1` | Image loaded from the file |
| `image_end … bss_end − 1` | Zero-filled bss |
| `bss_end′ … args_end − 1` | Argument block |
| `args_end … SP` | Free memory |
| `… top − 1` | Stack |

Once the program has read the arguments, it can treat `bss_end′` as the lower boundary of free memory. If it needs to retain the argument block, the lower boundary is `args_end` instead. The heap grows upward and the stack downward. Collisions between them are not detected.

## 8. Initial Program State

### 8.1. Under Monitor

| Resource | Initial value |
|---|---|
| IP | `$0000` |
| R0 | argc |
| R1 | argv |
| SP | `top` |
| LR | `$E000` — address of the Monitor exit routine |
| FLAGS | `$08`: IF = 1 (interrupts enabled); ZF, CF, SF, and OF are zero |
| Other registers | 0 |

The exit routine takes the program's exit code from R0. A program can therefore finish with `RET` after restoring its original SP and LR, or terminate explicitly with `EXIT`. See *System Calls*, Section 7.2, for exit codes and Monitor-initiated termination.

The exit routine does not depend on IF: a program can terminate from a critical section without first re-enabling interrupts.

### 8.2. On a Bare Machine

An executable in this format can be run directly by the emulator without Monitor: `JMP #start` skips over the header. In this mode, address `$0004` still contains `"CX16"`; there is no PIB or argument block; all registers, including SP, are zero; and the memory manager is disabled.

Interrupts are disabled on a bare machine (IF = 0; processor specification, Section 9): the vector table is uninitialized, and the program must decide whether to use interrupts. The program is responsible for stopping the bare machine, using the procedure in *CPU16 Machine*, Section 3: disable the timer, then execute `STI`, `CLI`, `WFI`.

A program intended for both environments must inspect the signature before its first use of the stack (Section 9.3) and must not rely on a particular initial IF value (Section 8.3). A Monitor-only program may treat a bare-machine launch as user error.

### 8.3. Interrupts Under Monitor

Applications run with interrupts enabled. While a program is executing, Monitor services interrupts from the devices it owns (the timer and disk controller). Monitor's handlers return to the interrupted instruction stream without changing the program's registers or flags, except as described below.

**Stack.** An interrupt may arrive between any two program instructions. On interrupt entry, the processor pushes four bytes onto the program stack: IP and FLAGS (processor specification, Section 8.3). Therefore, while IF = 1:

- The four bytes below SP, at addresses `SP − 4 … SP − 1`, must belong to the process's allocated memory (below `top`).
- Data stored below SP may be overwritten at any time; a program must not keep anything there.
- SP must not be used as a general-purpose register.

A program that temporarily needs to violate these rules (for example, while switching stacks) must do so with IF = 0.

**The vector table and interrupt handlers** belong to Monitor and reside in page 7 (Section 2). Applications must neither modify interrupt vectors nor enable or disable the timer. To receive timer ticks, a program registers a handler with `TICKHOOK` (*System Calls*, Section 7.4). The rules for direct device access versus access through system calls are given in *System Calls*, Section 2.2.

**Disabling interrupts.** A program may execute `CLI`, but only for a short critical section consisting of a few instructions. While IF = 0:

- Monitor cannot service devices: multiple timer ticks coalesce into a single pending request and are lost (*CPU16 Machine*, Section 3), so timekeeping based on ticks falls behind.
- Monitor cannot dequeue keyboard events: the queue holds 256 events, and excess events are discarded (*CPU16 Machine*, Section 2.6).
- Monitor actions performed from interrupt handlers do not occur, including Ctrl+C program interruption (*System Calls*, Section 6.3) and calls to a `TICKHOOK` handler.

A program that leaves IF = 0 for an extended period disrupts Monitor. If it enters an infinite loop with IF = 0, the system becomes unresponsive and can recover only through a machine reset. The program is responsible for keeping critical sections short.

**Critical sections** must preserve the previous IF value rather than unconditionally setting IF = 1:

```asm
        PUSH    FLAGS
        CLI
        ...                             ; critical section
        POP     FLAGS
```

The `CLI` … `STI` sequence is valid only if the program knows IF was 1 before `CLI`. Code that can run both under Monitor and on a bare machine must use `PUSH FLAGS` / `POP FLAGS`: an unconditional `STI` on a bare machine would enable interrupts while the vector table is still uninitialized.

**System calls** enter Monitor through `TRAP`, which clears IF, and return through `IRET`, which restores the program's complete FLAGS register. Unless the system-call specification says otherwise, IF has the same value after a call as before it.

**`WFI`.** When IF = 1, `WFI` waits for the next interrupt (for example, a timer tick) and then returns control to the program. Applications should wait for input, timeouts, and other events using system calls (`READ`, `CON_GETEV`, `SLEEP`), not a loop around `WFI`. The bare-machine shutdown procedure (Section 8.2) does not apply under Monitor: Monitor owns the timer, and `WFI` merely waits while the timer is running. Programs terminate via the exit routine or `EXIT`.

## 9. Implementation Guidance

### 9.1. Loader

**Loading sequence:**

1. Locate the file and read its first sector into a kernel buffer. Copy the header fields into kernel variables, because the image will overwrite the header later.
2. Validate the header (Section 3.3).
3. Parse the command line into a temporary kernel buffer and calculate `args_size` (Section 6).
4. Calculate `need`, `want`, and `top` (Section 6), then allocate the frames. If validation or allocation fails, release any resources obtained so far and report the error without starting the program.
5. Map the process frames into pages 0 through `top/$2000 − 1` and clear them.
6. Read the image directly into process memory using DMA.
7. Write the argument block starting at `bss_end′`, followed by the PIB.
8. Initialize the registers (Section 8.1) and transfer control to `$0000`.

**Transfer of control.** IF must become 1 at the same instant control transfers to `$0000`; otherwise an interrupt could occur before the registers and program mappings are ready. `IRET` provides a convenient way to do this: the loader writes FLAGS = `$08` at `top − 4` and IP = `$0000` at `top − 2`, sets SP = `top − 4`, initializes the remaining registers according to Section 8.1, and executes `IRET`. SP is then `top`. The two words below `top` remain in process memory as ordinary, insignificant stack contents.

**Where the loader runs.** The loader is part of the Monitor kernel and runs in the kernel memory map. It maps the new process's frames into kernel-map windows one at a time, clears them, and reads the image into them using DMA. The process memory map is activated only when control is transferred. This is done by the resident Monitor code in shared page 7: it maps pages 0–6 to the process frames and executes `IRET`. As the address of code in page 7 does not change during remapping, no trampoline is needed. `IRET` also provides the synchronization point for changing mappings and beginning execution of the code loaded by DMA.

**Reading the image.** Read the image in complete sectors: `⌈image_end / 512⌉` sectors, starting at address `$0000`. The final sector may extend up to 511 bytes beyond `image_end`, but cannot extend beyond `top`: `top` is aligned to 8 KiB, which is itself a multiple of 512. After reading, zero the bytes from `image_end` through `⌈image_end/512⌉·512 − 1` again; they may contain extra file data or garbage past the end of the file.

**Clearing memory.** Entire frames must be zeroed, not just bss, or a process could see data belonging to a previous owner. It is convenient to clear frames before reading the image, leaving only the tail of the final sector to be cleared afterward.

**Pages beyond `top`.** These must still be mapped somewhere, because the memory manager has no “page not present” state. A dedicated, shared **discard frame** is recommended: an invalid write beyond `top` then damages only that frame rather than another process's memory.

**Arithmetic.** The sums in Section 6 can overflow 16 bits. Check the carry flag (CF) before each addition, or validate the operands against `$E000` beforehand. The rounding expression `(x + $1FFF) & $E000` overflows if `x > $E000`, so the bound must be checked before rounding. In a B16 kernel it is easier to calculate `need` using `ulong` (B16 revision 2) and compare the result with `$E000`, avoiding overflow.

**Load errors.** Monitor distinguishes the following failures and returns their codes from `SPAWN` (*System Calls*, Section 8):

| Error | Cause | Code |
|---|---|---|
| File not found | Missing file or invalid path | `NOENT` (4), path errors (*System Calls*, Section 4.1); `ISDIR` (7) if the path names a directory |
| Not an executable | Any failure of Section 3.3 except the version check | `NOEXEC` (17) |
| Unsupported version | `version` ≠ 1 | `BADVER` (18) |
| Invalid arguments | Command-line length, argument count, or unmatched quote | `ARGS` (19) |
| Insufficient memory | `need` exceeds `$E000` or the available free frames | `NOMEM` (20) |
| I/O failure | Disk read error | `IO` (16) |

For `SPAWN`, these failures return an error before a child can start; the parent continues running. For `EXEC`, a failed load returns to the original program without replacing it or discarding its execution state (*System Calls*, Section 9.2).

### 9.2. Program Startup Code

- Read the arguments before initializing the heap. The heap starts at `bss_end′` (`bss_end` from the PIB, rounded up to an even address) if the arguments have been copied, or at `args_end` if the program retains pointers into the original `argv` block.
- The heap's upper limit is the current SP minus a safety margin reserved for the stack. Monitor does not know how much stack space the program needs; `min_free` guarantees only that some memory is available.
- Preserve the return address in LR if the program intends to terminate with `RET` after making function calls.
- Interrupts are already enabled on entry (Section 8.1). Startup code for a dual-mode program must not execute `STI` unconditionally: on a bare machine, the bare-machine startup path must decide when to enable interrupts, after setting up the vector table. If the stack must be changed, do so with IF = 0 (Section 8.3).
- Choose `min_free` with sufficient margin for the expected stack depth. For small utilities, 1–4 KiB may be enough; a compiler may need more space for tables stored in the heap.
- Set `max_top` = 0 for a program that needs as much memory as possible. A small utility can set `max_top` to `need` to avoid tying up frames needed by other processes.

### 9.3. Detecting the Startup Mode

```asm
        .equ    PIB_MAGIC, $0004

start:  LD      R4, [PIB_MAGIC+2]
        CMP     R4, #'P' | 'B' << 8     ; "CXPB": second half
        JZ      @under_monitor
        LD      SP, #$FF00              ; bare machine: set up our own stack
        ...
@under_monitor:
        ...
```

Comparing the second word of the signature is sufficient: the first word is the same (`"CX"`) in the executable header and in the PIB.

This test is safe in both environments. Under Monitor, interrupts are enabled, but SP already equals `top`, so the stack is valid from the first instruction. On a bare machine, SP = 0, but interrupts are disabled; therefore nothing is pushed onto the stack before SP is initialized.

### 9.4. Example Program

The following program prints each argument on a separate line directly to the console and terminates with exit code 0 using `RET`. Direct output to the DATA register is permitted under Monitor, but bypasses standard-output redirection (*System Calls*, Section 6.4). Programs whose output needs to be redirected must use `WRITE` instead. The example has no data, so `image_end` and `bss_end` are equal. Labels preceding `.align` receive the current location-counter value (*Assembler Reference*, “Labels and Alignment”); this makes it convenient to mark the end of the image.

```asm
        .equ    CON_DATA, $FF20

        .org    0
        JMP     #start
        .ascii  "CX16"
        .word   1, 0                    ; version, flags
        .word   image_end, bss_end
        .word   512                     ; min_free
        .word   $2000                   ; max_top: one page is sufficient
        .word   0, 0, 0, 0              ; reserved

start:  LD      R2, R1                  ; R2 = argv
@arg:   LD      R3, [R2++]              ; next pointer
        CMP     R3, #0
        JZ      @done
@ch:    LDZX    R4, [R3++]
        CMP     R4, #0
        JZ      @eol
        STB     [CON_DATA], R4
        JMP     @ch
@eol:   LD      R4, #10
        STB     [CON_DATA], R4
        JMP     @arg
@done:  LD      R0, #0                  ; exit code
        RET                             ; LR points to the exit routine

image_end:
bss_end:
        .align  2
```

### 9.5. Loader Tests

At minimum, test the following cases:

- Each condition in Section 3.3 independently: wrong jump opcode, magic, version, flags, nonzero reserved fields, `image_end` < `$001C`, `bss_end` < `image_end`, `bss_end` > `$E000`, an odd `start` or one outside the image, and a file shorter than `image_end`.
- `image_end` both aligned and unaligned to 512 bytes.
- Odd `bss_end`: the argument block must begin at `bss_end + 1`.
- `max_top` = 0, less than `need`, and greater than `$E000`.
- `need` exactly `$E000`, and one byte larger.
- Command lines of 255 and 256 bytes; 32 and 33 arguments; an empty argument `""`; quotes embedded in a word; an unmatched quote.
- File data after `image_end` must not appear in process memory.
- All allocated frames must be zero-filled except for the image and argument block.
- The PIB must be completely initialized, including zeroed reserved fields.
- Initial registers must match Section 8.1, including FLAGS = `$08`.
- A timer tick arriving during the program's very first instruction must not corrupt its registers or memory above `SP − 4`.
- A program that executes `CLI` and terminates through the exit routine must exit correctly, and the next program must start with IF = 1.
- Every load failure must return its own error code from Section 9.1 and leave no frames allocated.

## 10. Toolchain Support

The assembler and B16 compiler support this format. In assembly programs, the executable header is written explicitly in the source. Uninitialized data is placed after the `image_end` label using `.reserve` and is not included in the file. The B16 compiler generates the header, startup code, and data layout automatically.

**Assembler (implemented).** The normative description is in *Assembler Reference*, Section 17.5.

- **The `.reserve n` directive** advances the location counter by `n` bytes without emitting them.
  - `n` must be between 0 and 65536 and cannot depend on labels (the same restriction as `.space`). A label before `.reserve` receives the address of the reserved area.
  - A reserved region at the end of the program does not extend the image: the file ends after the last emitted byte, at `image_end`, and does not contain bss.
  - A reserved region inside the image (followed by emitted content) is written as zero bytes in the file, regardless of `--fill`.
  - Reserved addresses count as occupied. Emitting data into them (for example, by moving backward with `.org`), or reserving space over emitted data, causes an overlap error. A reservation extending beyond 64 KiB is an error.
  - `.reserve` does not perform alignment. If the location counter could be odd and the reserved area contains words, use `.align 2` before `image_end`.
- **A label at the end of the source**, with no further space-consuming elements, receives the current location-counter value. The value must fit within 0..65535.
- **Header validation with `--exec`.** The assembler validates the completed image against Section 3.3:
  - The word at `$0000` is `$C873` (`JMP #start`); bytes `$0004–$0007` contain `"CX16"`; `version` = 1.
  - `$001C` ≤ `image_end` ≤ `bss_end` ≤ `$E000`.
  - `start` is even and `$001C` ≤ `start` < `image_end`.
  - `image_end` does not exceed the length of the emitted file.
  - No bytes are emitted after `image_end` (whether code or initialized data). `.reserve` regions beyond `image_end` are permitted. An error identifies the first emitted byte beyond `image_end`.

  The last check catches programs whose code accidentally extends beyond `image_end` (for example, an assembly module appended after B16 compiler output). Such a program may work on a bare machine, but the extra code will not be loaded under Monitor. Consequently, the assembler cannot emit the optional trailing file data described in Section 3.1; it must be appended to the finished file by some other tool.

  The option does not validate `flags`, reserved fields, or `min_free`. Violations produce `executable header: …` errors, and no output file is written. The option requires `--base 0`. The web debugger's JavaScript assembler implements the same validation, though it is not yet exposed in the UI.

A typical program ending is:

```asm
        .align  2                       ; .reserve does not align
image_end:
buf:    .reserve 512                    ; not stored in file; zeroed by loader
count:  .reserve 2
bss_end:
```

Examples: `examples/calc_exec.asm` (a calculator with a header and a reserved buffer) and `docs/skeleton_exec.asm` (an executable program template). Both detect the startup mode (Section 9.3) and work under Monitor as well as on a bare machine.

**B16 compiler (implemented).** The normative description is in the *B16 Language Specification* (`B16_reference_en.md`), Sections 3.4, 9.1, and 9.2; compiler options and image layout are covered in Section 13.

- **Target.** By default (`option target = std`), a program is built in CX16 format. `option target = kernel` generates code without an executable header or startup code, for the Monitor kernel or modules linked by the assembler. Image options are written in the main program source file (B16, Section 3.6); `--target`, `--code`, `--data`, `--min-free`, and `--max-top` can override them for an individual build.
- **Header.** The image begins with `JMP #__start` and a header where `version` = 1, `flags` = 0, `image_end` = `__image_end`, `bss_end` = `__bss_end`, and `min_free` and `max_top` come from `option min_free` (default 2048) and `option max_top` (default 0). Reserved words are zero. The resulting image passes `cpu16asm --exec` validation.
- **Data layout.** Code, initialized global variables, and string literals are followed by `.align 2`, the `__image_end` label, uninitialized variables in `.reserve` regions, and the `__bss_end` label. The reserved area is not stored in the file. `option data = ADDRESS` relocates all variables to the specified address. Initial values of initialized variables are stored as templates alongside the code and copied by the startup code (for a ROM-based program with data in RAM).
- **Startup code** supports both launch environments (Section 8), using the signature check described in Section 9.3. Under Monitor, it does not set SP, change the timer or vectors, or execute `CLI` or `WFI`: it saves LR, calls `main(argc, argv)`, and terminates by branching to LR with the exit code in R0 (zero if `main` returns no value). On a bare machine, it sets SP to `$FF00`, sets the ERRCMD vector to a shutdown handler and the other vectors to no-op handlers (`IRET`), zeroes bss (memory contents after reset are undefined), calls `main` with argc = argv = 0, and halts the processor after `main` returns.
- **`main` forms:** `fn main()`, `fn main(): int`, `fn main(argc: uint, argv: ptr ptr byte)`, and the argument-taking form returning `int`.
- **Assembly modules**, such as system-call wrappers, are included with `include asm "file"` in the source (B16, Sections 3.2 and 10.4), usually from a library that declares the module's functions. The `--asm file` command-line option does the same thing. The compiler emits `.include` for each module after generated functions and runtime code but before data, so module code remains within the image before `__image_end` and is loaded by Monitor. Modules may contain only code and initialized data, not `.reserve` regions.

  ```text
  // prog.b16
  include "sys.b16"               // call declarations and include asm "sys.asm"
  option max_top = $6000          // 24 KiB is sufficient
  option min_free = 1024
  ```

  ```text
  b16c prog.b16 -o prog.s
  cpu16asm --exec -o prog.cx prog.s
  ```

  An assembly module cannot be appended to the compiler's generated assembly file: its code would lie beyond `__bss_end`, and `cpu16asm --exec` would reject the build.
- **Memory boundaries** are accessible in B16 as the `__image_end` and `__bss_end` addresses (type `ptr`). The heap starts at `__bss_end`. Programs retaining pointers from `argv` must instead start the heap at `args_end` (from the PIB).

## 11. Revision History

### Revision 4

- **Toolchain** (Section 10): B16 revision 3 programs specify `min_free` and `max_top` with `option` declarations, and assembly modules with `include asm`. The build example no longer needs command-line options. Compiler options remain available as overrides.
- The B16 reference now points to revision 3.

### Revision 3

- **Alignment with the system-call specification.** Added the related-documents table and replaced references to features that would be defined later with references to the corresponding *System Calls* sections:
  - Exit routine at fixed address `$E000` (Section 8.1).
  - `EXIT` and exit codes (Section 8.1).
  - PIB redirection flags (Section 4).
  - `TICKHOOK`, Ctrl+C, and device-access rules (Section 8.3).
  - Numeric load-error codes (Section 9.1).
- **`argv[0]`** (Section 5.2) is the original first command-line word: Monitor neither searches for the program nor appends an extension.
- **Shutdown** (Sections 8.2 and 8.3): the bare-machine shutdown procedure follows *CPU16 Machine* revision 6 (disable timer, then `STI`, `CLI`, `WFI`). Under Monitor, programs do not touch the timer and exit through the exit routine or `EXIT`.
- **Loader** (Section 9.1): it uses windows in the kernel memory map rather than running in page 7. The resident Monitor code activates the process map and transfers control. In a B16 kernel, Section 6 calculations can conveniently use `ulong`.
- **Toolchain** (Section 10):
  - `cpu16asm --exec` rejects images containing emitted bytes beyond `image_end`.
  - Assembly modules are linked into B16 programs via `--asm`.
- The Section 9.4 example notes that direct output to DATA bypasses redirection.
- Section 9.5 adds checks for load-error codes.
- References to *CPU16 Machine* and B16 now identify revisions 6 and 2, respectively.

### Revision 2

- **Section 10:** The B16 compiler changes (the `std` and `kernel` targets, executable header, dual-mode startup code, `main` with arguments and return code, bss in `.reserve`, and separation of data from code) are described as implemented rather than planned.
- **Interrupts enabled at startup** (Section 8.1): FLAGS = `$08` (IF = 1), instead of zero. Starting a program with interrupts disabled makes little sense: Monitor would be unable to service devices until the program executed `STI` itself. Disabling interrupts is now an explicit, brief operation used only where necessary.
- **New Section 8.3, “Interrupts Under Monitor”:** stack requirements (four bytes below SP belong to the process; data below SP is not preserved), Monitor ownership of vectors, consequences of disabling interrupts, critical sections using `PUSH FLAGS` / `POP FLAGS`, IF handling across system calls, and `WFI` behavior under Monitor.
- Section 8.1 states that the exit routine works with either IF value. Section 8.2 retains IF = 0 on a bare machine; dual-mode programs must not rely on one initial IF value.
- **Implementation guidance** (Sections 9.1, 9.2, 9.3, and 9.5): the loader uses `IRET` to enable interrupts at the same instant it transfers control; startup code does not execute unnecessary `STI`; the signature check is safe with IF = 1; additional loader test cases are included.
- The argument block size is called `args_size` (Sections 1, 6, and 9.1), rather than `B`; Section 6 explicitly states `args_end` = `bss_end′` + `args_size`.
- The *CPU16 Machine* reference now points to revision 5.
- **Compatibility.** The executable file format, header, PIB, and argument block are unchanged: revision 1 executables load without modification. Programs written for revision 1 that assumed IF = 0 on entry should be reviewed: they must meet the Section 8.3 stack requirement from the first instruction and explicitly protect their own critical sections. Programs that already executed `STI` after initialization continue to work as before.

### Revision 1

Initial edition: file format, PIB, argument block, memory allocation, and program startup state.

Clarifications made during the project:

- Section 10 documents the implemented assembler changes (`.reserve`, a label at the end of the source, and `--exec`) instead of describing them as planned; the B16 compiler changes remained proposals.
- The *CPU16 Machine* reference now points to revision 4.
