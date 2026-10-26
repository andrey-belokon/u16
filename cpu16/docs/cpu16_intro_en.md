# Introduction to the CPU16 Architecture and Assembly Language

This introduction is intended for readers who have worked with an assembly language before. It explains the CPU16 processor, its addressing modes and instructions, shows how a typical program and its memory layout are organized, and includes a ready-to-use standard library.

Some details are intentionally omitted here, including the exact instruction-encoding validity rules, every flag-related corner case, and the complete language grammar. For authoritative definitions, refer to:

- **CPU16 16-bit Processor Architecture** (Revision 6) — the normative processor specification;
- **CPU16 Assembly Language Reference** — the normative language specification.

Tools:

| Tool | Purpose |
|---|---|
| `cpu16asm` | Assembler: `cpu16asm prog.asm` produces `prog.bin`; `-l` generates a listing, and `-m` a symbol map |
| `cpu16` | Emulator: `cpu16 prog.bin`; program input comes from stdin; `-t` enables an instruction trace |
| Web debugger | Browser-based editor, assembler, and step-by-step debugger |
| `stdlib.asm` | Standard library (Appendix A), imported with `.include` |

## 1. Your First Program

<!-- run -->
```asm
        LD      SP, #$FF00      ; initialize the stack
        LD      R0, #hello      ; address of the string
        CALL    puts            ; print the string (library routine)
        WFI                     ; stop execution
hello:  .asciz  "Hello, CPU16!\n"
        .include "stdlib.asm"
```

```text
Hello, CPU16!
```

To assemble and run this program from the command line, place `stdlib.asm` beside it. In the project archive, the library is under `lib/stdlib.asm`; the programs under `examples/` include it as `../lib/stdlib.asm`:

```text
cpu16asm hello.asm
cpu16 hello.bin
```

In the web debugger, paste the program into the **Source** tab, press Ctrl+B, then F9. The debugger includes the library, so `.include "stdlib.asm"` works there too.

`WFI` means *wait for interrupt*. When interrupts are disabled and none are pending, the processor remains idle. The command-line emulator terminates in this state, making `WFI` a convenient way to stop a simple program.

## 2. Processor Overview

### 2.1. Memory and Data

- A machine word is 16 bits wide. The byte-addressable address space is 64 KiB.
- Words use little-endian byte order: the least significant byte is stored at the lower address.
- Words and instructions occupy even addresses. The assembler aligns them automatically. For word accesses, address bit 0 is simply ignored.
- I/O devices are memory-mapped; accessing them uses ordinary reads and writes to their assigned addresses.

### 2.2. Registers

The processor has sixteen 16-bit registers:

| Register | Name | Purpose |
|---|---|---|
| R0..R11 | — | General-purpose registers |
| R12 | LR | Link register: `CALL` writes the return address here rather than pushing it onto the stack |
| R13 | SP | Stack pointer; the stack grows downward |
| R14 | IP | Instruction pointer |
| R15 | FLAGS | Status flags |

One notable feature is that IP and FLAGS are **ordinary registers**. Writing to IP performs a jump (`LD IP, LR` returns from a subroutine), while FLAGS can be read, saved on the stack, and modified using `AND`, `OR`, and `XOR`.

Under the standard library calling convention, R0..R3 carry arguments and return values and may be overwritten by a callee. A subroutine must preserve R4..R11 (Section 5.6).

### 2.3. Flags

| Flag | Bit | Mask | Meaning |
|---|---|---|---|
| ZF | 0 | `$01` | The result is zero |
| CF | 1 | `$02` | Carry on addition; **borrow** on subtraction: after `SUB` or `CMP`, CF = 1 if the minuend is less than the subtrahend in unsigned arithmetic |
| SF | 2 | `$04` | Sign of the result (bit 15) |
| IF | 3 | `$08` | Interrupts are enabled |
| OF | 4 | `$10` | Signed overflow; multiplication and division assign it specific meanings (Section 5.2) |

Only arithmetic, logical, and shift instructions modify the condition flags. Data transfers (`LD`, `ST`, `PUSH`, and `POP`) **do not modify them**. To test a value after loading it, use `TEST R0, R0` or `CMP R0, #0`.

### 2.4. General Instruction-Set Principles

- **Two-operand format:** `OP Rd, src` means `Rd = Rd OP src`. The first operand is a register and is also the destination. Stores are an exception: their first operand is a memory address (`ST [address], R1`).
- **At most one memory access per instruction.** The source operand can refer to memory (`ADD R0, [R1]`), but there are no memory-to-memory instructions.
- **Instructions are one or two words long.** A second word carries a full 16-bit constant, address, or displacement. Immediate values 0..15 fit into the instruction word, so `ADD R0, #1` is one word long.
- **Invalid instructions are not executed.** The processor raises the ERRCMD exception (interrupt vector 8) and records the offending instruction's address. This also catches jumps into data.

## 3. Assembly Language at a Glance

A source line consists of an optional label, an instruction or directive, and optionally a comment introduced by `;`:

```asm
; comment extending to the end of the line
        .equ    COUNT, 10       ; define constant COUNT = 10
start:  LD      R0, #COUNT      ; label start, LD instruction, operands R0 and #COUNT
table:  .word   1, 2, $FFFF     ; label table and three words of data
text:   .asciz  "abc"           ; label text and a null-terminated string
```

### 3.1. Labels

A label is a name followed by a colon at the beginning of a line. Its value is the address of the instruction or data that follows. The name may subsequently be used wherever that address is required: as a branch target (`JMP start`), a memory address (`LD R0, [table]`), or a pointer constant (`LD R0, #text`).

There are two kinds of labels:

- **Global labels** (`start:`, `table:`) are visible throughout the program and must have unique names.
- **Local labels** begin with `@` (`@loop:`, `@done:`). They are visible only within the scope extending from the preceding global label to the next one. Each subroutine can therefore use the same `@loop` and `@done` names without having to invent unique labels.

```asm
clear:  LD      R2, #0          ; global label clear begins a scope
@loop:  STB     [R0++], R2      ; @loop belongs to clear's scope
        DEC     R1
        JNZ     @loop           ; branches to @loop within the same scope
        RET
count:  LD      R2, #0          ; global label count begins a new scope
@loop:  INC     R2              ; a different @loop, local to count
        DEC     R1
        JNZ     @loop           ; uses count's @loop, not clear's
        RET
```

A local label cannot be accessed from another scope. For example, `JMP @loop` in a third subroutine will produce an `undefined symbol` error unless that subroutine defines its own `@loop`. Use a global label for a destination shared by several subroutines.

### 3.2. Numbers, Expressions, and Directives

- **Numbers:** decimal `123`, hexadecimal `$7F` or `0x7F`, binary `%1010` or `0b1010`, or a character such as `'A'`.
- **Expressions:** `+ - * / & | ^ ~ << >>`, parentheses, `lo()`, `hi()`; `$` denotes the address of the current instruction.
- **Case sensitivity:** mnemonics and register names are case-insensitive; labels are case-sensitive.

Common directives:

| Directive | Effect |
|---|---|
| `.org address` | Place subsequent code and data starting at this address |
| `.word`, `.byte` | Emit words or bytes |
| `.ascii`, `.asciz` | Emit a string (`.asciz` adds a terminating zero byte) |
| `.space n` | Reserve `n` bytes initialized to zero |
| `.align n` | Align the current address to `n` |
| `.equ name, value` | Define a constant |
| `.include "file"` | Insert the contents of another source file |

## 4. Addressing Modes

### 4.1. Labels, Addresses, and Pointers

A label is simply a numeric address. How an operand is written determines what is obtained from that address:

| Syntax | Value obtained | Example: `numbers` is at $0100 and contains the word 100 |
|---|---|---|
| `#numbers` | The address itself (a pointer) | `LD R1, #numbers` → R1 = $0100 |
| `[numbers]` | The value stored at that address | `LD R0, [numbers]` → R0 = 100 |
| `[R1]` | The value stored at the address held in the register | If R1 = $0100, `LD R0, [R1]` → R0 = 100 |

A pointer held in a register can advance to the next word by 2 (`ADD R1, #2` or auto-increment `[R1++]`), or to the next byte by 1. `[R1+4]` reads the word four bytes beyond the pointer, i.e., the third element of a word array. `[R2+numbers]` accesses an array element at the byte offset held in R2; for word arrays, multiply the index by 2.

<!-- run -->
```asm
        LD      SP, #$FF00
        LD      R1, #numbers    ; R1 points to the array
        LD      R0, R1
        CALL    print_hex16     ; $0100: the pointer itself
        CALL    newline
        LD      R0, [numbers]   ; contents at the label's address
        CALL    print_u16       ; 100
        CALL    newline
        LD      R1, #numbers
        LD      R0, [R1+4]      ; third element: two words = four bytes
        CALL    print_u16       ; 300
        CALL    newline
        LD      R2, #3          ; element index 3
        ADD     R2, R2          ; multiply by 2 to obtain a byte offset
        LD      R0, [R2+numbers]
        CALL    print_u16       ; 400
        WFI
        .org    $0100
numbers: .word  100, 200, 300, 400, 500
        .include "stdlib.asm"
```

```text
0100
100
300
400
```

Notice that `LD R1, #numbers` does not access memory: it only loads the address into the register. Actual accesses to the data use operands enclosed in square brackets.

### 4.2. Addressing-Mode Reference

Only the source operand has an addressing mode. Assume R1 = $2000:

| Syntax | Operand value | Example |
|---|---|---|
| `R1` | Register value | `ADD R0, R1` |
| `#expr` | Immediate constant | `ADD R0, #100` |
| `[R1]` | Word at address R1 | `LD R0, [R1]` |
| `[R1++]` | Word at R1, then R1 += 2 | `ADD R0, [R1++]` |
| `[--R1]` | Decrement R1 by 2, then read the word at R1 | `LD R0, [--R1]` |
| `[address]` | Word at an absolute address | `LD R0, [counter]` |
| `[R1+expr]` | Word at R1 plus displacement | `LD R0, [R1+4]` |
| `R1+expr` | Sum without a memory access | `LEA R0, R1+4` |

- For byte instructions (`LDZX`, `LDSX`, `STB`, `XCHB`), `++` and `--` adjust the address by 1; other instructions use a step of 2.
- Stack operations use the same addressing modes: `PUSH R0` is equivalent to `ST [--SP], R0`, while `POP R0` is equivalent to `LD R0, [SP++]`.
- The assembler selects the immediate encoding automatically: `#5` needs one instruction word, whereas `#500` needs two.

Example: walking through an array. R1 points to the current element. `[R1++]` reads the pointed-to word and then advances R1 to the next element.

<!-- run -->
```asm
        LD      SP, #$FF00
        LD      R1, #numbers    ; address of the first element
        LD      R2, #5          ; number of elements
        LD      R0, #0          ; running sum
@sum:   ADD     R0, [R1++]      ; add the current word, then R1 += 2
        DEC     R2
        JNZ     @sum
        CALL    print_u16
        WFI
numbers: .word  100, 200, 300, 400, 500
        .include "stdlib.asm"
```

```text
1500
```

## 5. Instructions

### 5.1. Data Transfer

| Instruction | Effect |
|---|---|
| `LD Rd, src` (or `MOV`) | Rd = src |
| `ST [address], Rs` | Store the word in Rs to memory |
| `LDZX Rd, src` / `LDSX Rd, src` | Load a byte, zero-extend / sign-extend it |
| `STB [address], Rs` | Store the low byte of Rs to memory |
| `XCHB [address], Rs` | Atomic byte exchange (useful for semaphores) |
| `LEA Rd, Rs+expr` | Rd = Rs + expr (calculate an address without accessing memory) |
| `PUSH Rs`, `POP Rd` | Stack operations |

```asm
        LD      R0, R1          ; copy a register
        LD      R0, #msg        ; load a label's address
        LD      R0, [msg]       ; load the word stored at that address
        ST      [result], R0    ; write to memory
        LDZX    R2, [R3++]      ; fetch the next byte of a string
        PUSH    R4              ; save a register
        POP     R4
        LD      IP, R5          ; jump to the address in R5
```

### 5.2. Arithmetic

| Instruction | Effect | Flags |
|---|---|---|
| `ADD`, `ADC` | Rd = Rd + src (+ CF) | All |
| `SUB`, `SBC` | Rd = Rd − src (− CF); CF indicates borrow | All |
| `CMP` | Like SUB, without changing Rd | All |
| `NEG Rd` | Rd = −Rd | All |
| `INC Rd`, `DEC Rd` | ±1 (equivalent to `ADD`/`SUB` with `#1`; **CF also changes**) | All |
| `MUL Rd, src` | Low word of the product | CF = OF = 1 if the high word is nonzero |
| `MULHU Rd, src` | High word of an unsigned product | CF = OF = 0 |
| `DIVU Rd, src` | Unsigned quotient | OF = 1 on division by zero |
| `REMU Rd, src` | Unsigned remainder | OF = 1 on division by zero |

- **There are no signed multiply or divide instructions.** Signed operations are implemented in software using their unsigned counterparts.
- **Division by zero does not raise an exception.** `DIVU` produces $FFFF, `REMU` returns the dividend, and OF is set; test it with `JO`.
- **Multiword arithmetic** uses `ADC` and `SBC`. For example, `ADD R0, R2` followed by `ADC R1, R3` adds two 32-bit register pairs. The library supplies `add32`, `sub32`, `mul32`, and `divu32`.

Example: computing 10! as a 32-bit value with the library's `mul32` routine.

<!-- run -->
```asm
        LD      SP, #$FF00
        LD      R0, #1          ; R1:R0 = 1
        LD      R1, #0
        LD      R4, #2          ; multiplier (R4 is preserved by the library)
@next:  LD      R2, R4          ; R3:R2 = R4
        LD      R3, #0
        CALL    mul32           ; R1:R0 *= R3:R2
        INC     R4
        CMP     R4, #10
        JBE     @next
        CALL    print_u32
        WFI
        .include "stdlib.asm"
```

```text
3628800
```

### 5.3. Logic and Bit Operations

| Instruction | Effect | Flags |
|---|---|---|
| `AND`, `OR`, `XOR` | Bitwise operations | ZF, SF; CF = OF = 0 |
| `TEST Ra, src` | Like AND, without modifying Ra | ZF, SF; CF = OF = 0 |
| `NOT Rd` | Bitwise complement | Unchanged |
| `BSX Rd` | Sign-extend the low byte | Unchanged |

Common bit operations on bit 5 (mask `$20`):

```asm
        OR      R0, #$20        ; set the bit
        AND     R0, #~$20       ; clear the bit
        XOR     R0, #$20        ; toggle the bit
        TEST    R0, #$20        ; test the bit: JNZ means it is set
        JNZ     bit_set
```

### 5.4. Shifts and Rotates

| Instruction | Effect |
|---|---|
| `SHL Rd, n` | Logical shift left |
| `SHR Rd, n` | Logical shift right |
| `SAR Rd, n` | Arithmetic shift right (propagates the sign bit) |
| `ROR Rd, n` | Rotate right |

- The shift count comes from the operand's low four bits (0..15) and can be supplied in a register.
- CF receives the last bit shifted out. A count of zero leaves both the value and all flags unchanged.
- A left rotation by k can be expressed as `ROR Rd, #16-k`; `ROR Rd, #8` swaps the two bytes of a word.

Example: count the set bits in a word. Each bit shifted out enters CF and is accumulated using `ADC`.

<!-- run -->
```asm
        LD      SP, #$FF00
        LD      R1, #$BEEF      ; value to examine
        LD      R0, #0          ; bit count
@bit:   SHR     R1, #1          ; least significant bit enters CF
        ADC     R0, #0          ; R0 += CF
        TEST    R1, R1
        JNZ     @bit
        CALL    print_u16
        WFI
        .include "stdlib.asm"
```

```text
13
```

### 5.5. Branches

`JMP label` is an unconditional branch. Conditional branches inspect flags set by `CMP`, `SUB`, `TEST`, and similar instructions:

| Condition | Unsigned | Signed |
|---|---|---|
| Equal / not equal | `JZ` (`JE`) / `JNZ` (`JNE`) | Same |
| Less than | `JB` (`JC`) | `JL` |
| Less than or equal | `JBE` | `JLE` |
| Greater than | `JA` | `JG` |
| Greater than or equal | `JAE` (`JNC`) | `JGE` |
| Sign and overflow | `JS`, `JNS` | `JO`, `JNO` |

- The assembler chooses the branch encoding: one word for targets within ±64 words, and two words for more distant targets.
- A branch target can come from a register (`JMP R1`) or memory (`JMP [R1+table]`), enabling jump-table dispatch similar to `switch`.

Example: compare the same pair of numbers as unsigned and signed values.

<!-- run -->
```asm
        LD      SP, #$FF00
        LD      R4, #$FFFF      ; 65535 unsigned, −1 signed
        CMP     R4, #1
        JB      @u_less
        LD      R0, #msg_u_ge
        JMP     @u_done
@u_less: LD     R0, #msg_u_lt
@u_done: CALL   puts
        CMP     R4, #1
        JL      @s_less
        LD      R0, #msg_s_ge
        JMP     @s_done
@s_less: LD     R0, #msg_s_lt
@s_done: CALL   puts
        WFI
msg_u_lt: .asciz "unsigned: less\n"
msg_u_ge: .asciz "unsigned: greater or equal\n"
msg_s_lt: .asciz "signed: less\n"
msg_s_ge: .asciz "signed: greater or equal\n"
        .include "stdlib.asm"
```

```text
unsigned: greater or equal
signed: less
```

Example: a jump table.

<!-- run -->
```asm
        LD      SP, #$FF00
        LD      R0, #2          ; case number (0..2)
        ADD     R0, R0          ; multiply by 2 (word size)
        JMP     [R0+cases]      ; read the target address from the table
cases:  .word   case0, case1, case2
case0:  LD      R0, #'A'
        JMP     show            ; global target: case0's local @show would not be visible here
case1:  LD      R0, #'B'
        JMP     show
case2:  LD      R0, #'C'
show:   CALL    putc
        WFI
        .include "stdlib.asm"
```

```text
C
```

### 5.6. Subroutines

- `CALL label` writes the return address to **LR** and transfers control; it does not touch the stack.
- `RET` is equivalent to `LD IP, LR`.
- A leaf subroutine (one that makes no calls) need not use the stack at all.
- A subroutine that calls others must preserve its own LR. It can return with `POP IP`, which restores the saved return address and branches to it in one instruction.

```asm
leaf:   ADD     R0, R1          ; leaf routine: no stack needed
        RET

outer:  PUSH    LR              ; save LR before making further calls
        PUSH    R4              ; preserve any R4..R11 registers this routine modifies
        ...
        CALL    leaf
        ...
        POP     R4
        POP     IP              ; return through the saved LR
```

**Calling convention** (followed by the standard library):

- Arguments are passed in R0..R3; results are returned in R0..R3.
- The callee may overwrite R0..R3, LR, and FLAGS.
- The callee must restore R4..R11 and SP to their incoming values.
- If there are more than four arguments, the caller pushes the rest before the call and removes them afterward.

Example: passing a fifth argument on the stack.

<!-- run -->
```asm
        LD      SP, #$FF00
        LD      R0, #1
        LD      R1, #2
        LD      R2, #3
        LD      R3, #4
        LD      R4, #5
        PUSH    R4              ; fifth argument
        CALL    sum5
        ADD     SP, #2          ; remove the stack argument
        CALL    print_u16
        WFI

; sum5: R0 = R0 + R1 + R2 + R3 + [SP] (fifth argument)
sum5:   ADD     R0, R1
        ADD     R0, R2
        ADD     R0, R3
        ADD     R0, [SP]        ; CALL does not push anything: argument is at top of stack
        RET
        .include "stdlib.asm"
```

```text
15
```

If the subroutine first executes `PUSH LR`, the same argument is available at `[SP+2]`.

**Tail calls.** If the final action of a subroutine is calling another subroutine, `JMP` can replace `CALL` followed by `RET`: LR still points to the original caller. The library uses this technique in `print_s16` and `print_hex32`.

### 5.7. Interrupts

- **Sources:** eight external IRQ lines (IRQ0..IRQ7), software interrupts through `TRAP #n`, and the ERRCMD exception.
- **Vector table:** sixteen words beginning at `$FF00`; vector n is stored at `$FF00 + 2·n`.

| Vector | Purpose |
|---|---|
| 0..7 | IRQ0..IRQ7 |
| 8 | ERRCMD |
| 9..15 | Software interrupts `TRAP #9` through `TRAP #15` |

On interrupt entry, the processor pushes IP, then FLAGS, clears IF, and branches through the appropriate vector. `IRET` restores FLAGS and IP.

| Instruction | Effect |
|---|---|
| `STI`, `CLI` | Enable / disable interrupts (`CLI` also clears OF) |
| `TRAP #n` | Software interrupt through vector n |
| `IRET` | Return from an interrupt handler |
| `WFI` | Wait for an interrupt |

An interrupt handler must preserve **every register it changes**, including R0..R3 and LR. The interrupted code did not call the handler and cannot expect those registers to be clobbered. `IRET` restores FLAGS.

```asm
on_irq: PUSH    LR
        PUSH    R0
        PUSH    R1
        PUSH    R2
        PUSH    R3
        ...                     ; handler work, including library calls
        POP     R3
        POP     R2
        POP     R1
        POP     R0
        POP     LR
        IRET
```

A critical section that restores the previous IF state:

```asm
        PUSH    FLAGS
        CLI
        ...                     ; interrupts disabled here
        POP     FLAGS
```

### 5.8. Input and Output

The console is mapped at port `$FF20`, exposed by the library as the constant `CONSOLE`:

- Writing a byte (`STB [CONSOLE], R0`) outputs a character.
- Reading a byte (`LDZX R0, [CONSOLE]`) returns the next input byte, or zero if no input is available.

The port must be accessed with byte instructions. An actual input byte of zero cannot be distinguished from “no input.” The command-line emulator reads from stdin; the web debugger receives input from its console pane.

This is only the simplest console interface. The same device can act as an 80×25 color display with cursor-positioned output; the keyboard also reports scan codes. A 50 Hz timer and a real-time clock are available as well. They are documented in *CPU16 Machine: Memory and Devices*. Appendix A describes the library routines, while `display_demo.asm`, `keys.asm`, `stopwatch.asm`, `clock.asm`, and `snake.asm` demonstrate these facilities.

Example: read a line and print it in reverse order.

<!-- run stdin="stressed\n" -->
```asm
        LD      SP, #$FF00
        LD      R0, #buf
        LD      R1, #32         ; buffer capacity
        LD      R2, #0          ; echo disabled
        CALL    gets            ; R0 = length
        LD      R4, R0
        ADD     R4, #buf        ; R4 points just past the final character
@back:  CMP     R4, #buf
        JZ      @done
        DEC     R4
        LDZX    R0, [R4]
        CALL    putc
        JMP     @back
@done:  CALL    newline
        WFI
buf:    .space  32
        .include "stdlib.asm"
```

```text
desserts
```

## 6. System Memory and Program Startup

### 6.1. Typical Memory Layout

| Address range | Contents |
|---|---|
| `$0000..$7FFF` | **ROM:** executable code, constants (strings and tables), and library routines |
| `$8000..` | **RAM:** variables, growing toward higher addresses |
| `..$FEFF` | **RAM:** stack, growing downward from `$FF00` |
| `$FF00..$FF1F` | **RAM:** interrupt vector table (16 words) |
| `$FF20..$FFFF` | Memory-mapped I/O devices (`$FF20` is the console port) |

```text
$0000 +----------------------+
      | ROM code, constants  |
$8000 +----------------------+
      | variables      |    |
      |                v     |
      |                      |
      |                ^     |
      | stack          |    |
$FF00 +----------------------+
      | interrupt vectors    |
$FF20 +----------------------+
      | I/O devices          |
$FFFF +----------------------+
```

There are two important consequences:

- **RAM contents are undefined at power-on.** Declare storage with `.space` and initialize variables in the startup code. Read-only strings and tables belong in ROM.
- **The vector table is also in RAM**, so the program must initialize it. Until then, any interrupt, including ERRCMD, may branch to an unpredictable address.

The emulator treats the entire address space as RAM and loads the complete image. A program that follows these rules will also run on actual hardware.

### 6.2. Reset State

After a reset:

- All registers, including SP, are zero.
- Interrupts are disabled (IF = 0).
- Execution starts at address `$0000`.

The first instruction must therefore be at address 0, and setting up the stack should be the program's first action.

### 6.3. Standard Startup Sequence

1. **Set up the stack:** `LD SP, #$FF00`. The first pushed word is stored at `$FEFE`, just below the vector table.
2. **Install the ERRCMD handler first**, so errors in the remaining startup code can be caught.
3. **Initialize the remaining vectors** to a no-op handler that immediately returns (`irq_ignore` in the library). This allows the program to survive an unexpected interrupt, for example from a device added to the machine later or a TRAP through an unused vector. Then install the handlers the program actually needs. Library routine `vectors_init` performs steps 2 and 3 (R0 holds the ERRCMD handler address).
4. **Zero-initialize variables** with `memset` over the `.space` region.
5. **Enable interrupts** with `STI`.
6. **Call `main`**. On return, stop by executing `CLI` followed by `WFI` in a loop.

### 6.4. Complete Program Skeleton

The following is the full example from `skeleton.asm`:
- Startup follows Section 6.3.
- The program requests a number, then prints its hexadecimal representation and square.
- Next, it waits for three IRQ0 timer interrupts, printing a dot for each tick. In the web debugger, use the **IRQ 0** button to generate them. The command-line emulator has no interrupt source for this example, so execution stops while waiting.

<!-- run stdin="250\n" -->
```asm
        .equ    RAM_START,  $8000
        .equ    VECTORS,    $FF00       ; 16 words: IRQ0..7, ERRCMD (8), TRAP 9..15
        .equ    STACK_TOP,  $FF00

; ---------------- ROM: program code ----------------
        .org    $0000
reset:  LD      SP, #STACK_TOP          ; 1. set up the stack
        LD      R0, #on_errcmd          ; 2. install ERRCMD first
        CALL    vectors_init            ; 3. initialize all other vectors to irq_ignore
        LD      R0, #on_tick            ;    then install the active handlers:
        ST      [VECTORS + 2*0], R0     ;    IRQ0: timer
        LD      R0, #bss_start          ; 4. zero-initialize variables
        LD      R1, #0
        LD      R2, #bss_end - bss_start
        CALL    memset
        STI                             ; 5. enable interrupts
        CALL    main                    ; 6. run the main program
halt:   CLI                             ;    then stop
@stop:  WFI
        JMP     @stop

main:   PUSH    LR
        LD      R0, #msg_hello
        CALL    puts
        LD      R0, #msg_ask
        CALL    puts
        LD      R0, #1                  ; echo enabled
        CALL    read_dec
        TEST    R1, R1
        JNZ     @bad
        LD      R4, R0
        LD      R0, #msg_hex
        CALL    puts
        LD      R0, R4
        CALL    print_hex16
        LD      R0, #msg_square
        CALL    puts
        LD      R0, R4
        LD      R1, R4
        CALL    mulu16                  ; R1:R0 = R4 * R4
        CALL    print_u32
        CALL    newline
        JMP     @wait
@bad:   LD      R0, #msg_bad
        CALL    puts
@wait:  LD      R0, #msg_wait
        CALL    puts
@tick:  WFI                             ; wait for interrupts
        LD      R0, [ticks]
        CMP     R0, #3
        JB      @tick
        LD      R0, #msg_done
        CALL    puts
        POP     IP

on_tick:                                ; IRQ0 handler: save everything it modifies
        PUSH    LR
        PUSH    R0
        PUSH    R1
        PUSH    R2
        PUSH    R3
        LD      R0, [ticks]
        INC     R0
        ST      [ticks], R0
        LD      R0, #'.'
        CALL    putc
        POP     R3
        POP     R2
        POP     R1
        POP     R0
        POP     LR
        IRET

on_errcmd:                              ; stack: [SP] = FLAGS, [SP+2] = instruction address
        LD      R0, #msg_errcmd
        CALL    puts
        LD      R0, [SP+2]
        CALL    print_hex16
        CALL    newline
        JMP     halt

msg_hello:  .asciz  "CPU16 skeleton\n"
msg_ask:    .asciz  "Enter a number: "
msg_hex:    .asciz  "hex: $"
msg_square: .asciz  ", square: "
msg_bad:    .asciz  "\nnot a number\n"
msg_wait:   .asciz  "Waiting for 3 timer interrupts (IRQ 0)"
msg_done:   .asciz  " done\n"
msg_errcmd: .asciz  "\nERRCMD at $"

        .include "stdlib.asm"

; ---------------- RAM: variables ----------------
        .org    RAM_START
bss_start:
ticks:  .space  2
bss_end:
```

Output in the command-line emulator (input `250`; the program remains waiting for interrupts):

```text
CPU16 skeleton
Enter a number: 250
hex: $00FA, square: 62500
Waiting for 3 timer interrupts (IRQ 0)
```

After three presses of **IRQ 0** in the web debugger, `... done` is appended to the final line.

If the program encounters an invalid instruction, the ERRCMD handler prints its address, for example `ERRCMD at $0044`, and stops the processor.

## 7. Tools

- **`cpu16asm prog.asm -l prog.lst -m prog.map`** assembles the program and produces a listing and memory map, showing the location, type, and size of each program object. The `prog.bin` image starts at address 0. If the program includes a RAM region (`.org $8000`), the image extends to that region.
- **`cpu16 prog.bin < input.txt`** runs a program using a file as stdin. Other options include:
  - `-t` — trace each instruction;
  - `-n N` — limit execution to N steps;
  - `--strict-align` — report word accesses at odd addresses.
- **Web debugger:**
  - The **Source** tab provides an editor, assembly with Ctrl+B, and source-level debugging: F7 steps into, F8 steps over a CALL, F4 runs to the cursor, F5 toggles a breakpoint, and F9 runs.
  - The **Disassembly** tab shows the machine code.
  - IRQ buttons generate interrupts, and the console panel accepts input.
  - **Download .zip** saves the source, image, listing, and map.

## Appendix A. Standard Library

The `stdlib.asm` file is included using `.include "stdlib.asm"`, normally after the main program. The library requires no RAM variables and can reside in ROM. It also defines the `CONSOLE` constant as `$FF20`.

A 32-bit value is passed in a register pair, with the high word in R1 (or R3) and the low word in R0 (or R2). The notation R1:R0 means high word followed by low word.

### Interrupt Vectors

| Routine | Input | Result |
|---|---|---|
| `vectors_init` | R0: ERRCMD handler address | Vector 8 is set to R0 first; the other 15 vectors are set to `irq_ignore`; clobbers R1 and R2 |
| `irq_ignore` | — | Empty interrupt handler; immediately executes `IRET` |

These routines are intended for bare-metal startup (Section 6.3). Under Monitor, the vectors belong to Monitor and must not be modified by an application. `irq_ignore` does not acknowledge level-sensitive device interrupts. Devices do not assert those requests after reset until the program enables them; if your program enables a device's interrupts, it must install a working handler.

### Output

| Routine | Input | Result |
|---|---|---|
| `putc` | R0: character | — |
| `newline` | — | Output a newline |
| `puts` | R0: address of a null-terminated string | — |
| `print_u16` / `print_s16` | R0 | Unsigned / signed decimal integer |
| `print_hex8` / `print_hex16` | R0 | Two / four hexadecimal digits |
| `print_u32` / `print_s32` | R1:R0 | Unsigned / signed 32-bit decimal integer |
| `print_hex32` | R1:R0 | Eight hexadecimal digits |

### Input

| Routine | Input | Result |
|---|---|---|
| `getc` | — | R0: next input byte; waits until one is available |
| `getc_nb` | — | R0: input byte, or zero without waiting; ZF = 1 if zero |
| `gets` | R0: buffer address, R1: buffer capacity (≥ 1), R2: echo (0/1) | Null-terminated line without `\n`; R0: length; supports Backspace |
| `read_dec` | R0: echo (0/1) | R0: number (may have a leading `-`); R1 = 0 on success, 1 if no digits or overflow |

`read_dec` skips leading spaces and consumes the input character that terminates the number, such as a newline.

### Memory and Strings

| Routine | Input | Result |
|---|---|---|
| `memcpy` | R0: destination, R1: source, R2: byte count | — |
| `memset` | R0: destination, R1: byte value, R2: byte count | — |
| `strlen` | R0: string address | R0: length |

### 32-bit Arithmetic (a = R1:R0, b = R3:R2)

| Routine | Result |
|---|---|
| `add32` / `sub32` | R1:R0 = a ± b; CF indicates carry / borrow |
| `neg32` | R1:R0 = −a |
| `cmp32u` | Sets flags for `JZ`, `JNZ`, `JB`, `JAE`, `JA`, `JBE` (unsigned comparison) |
| `mulu16` | R1:R0 = R0 × R1 (16 × 16 = 32 bits) |
| `mul32` | R1:R0 = low 32 bits of a × b |
| `divu32` | a / R2: R1:R0 is the quotient, R2 the remainder; on division by zero, OF = 1 and quotient = $FFFFFFFF |

### Display, Keyboard, and Timer

These devices are described in *CPU16 Machine: Memory and Devices*. The library defines port constants (`CON_CTRL`, `CON_ATTR`, `CON_COL`, `CON_ROW`, `CON_CMD`, `TIMER`), display modes (`MODE_TTY`, `MODE_CURSOR`, `MODE_ATTR`), commands (`CMD_CLEAR`, `CMD_CLEOL`, etc.), colors (`BLACK` through `WHITE`, `UNDERLINE`, `INVERSE`; the attribute is `BACKGROUND << 3 | FOREGROUND`), and scan codes (`SC_ESC`, `SC_ENTER`, `SC_UP`, `SC_F1`, etc.).

| Routine | Input | Result |
|---|---|---|
| `scr_mode` | R0: mode (`MODE_TTY \| MODE_CURSOR \| MODE_ATTR`; zero selects cursorless coordinate mode) | — |
| `scr_attr` | R0: attribute | — |
| `scr_goto` | R0: column, R1: row | — |
| `scr_clear` | — | Clear the screen with the current attribute and move to (0, 0) |
| `scr_print_at` | R0: column, R1: row, R2: string address | — |
| `scr_paint` | R0: attribute, R1: cell count | Change attributes of cells starting at the current position, without altering text |
| `scr_box` | R0, R1: top-left corner; R2: width, R3: height | Draw a double-line box in coordinate mode |
| `getkey` | — | R0: character (low byte) and scan code (high byte); waits for a key |
| `getkey_nb` | — | R0: character and scan code, or zero without waiting; ZF = 1 if zero |
| `timer_on`, `timer_off` | — | Enable / disable the 50 Hz timer on IRQ0 |

### Real-Time Clock

The RTC is described in Section 8 of *CPU16 Machine: Memory and Devices*. The value occupies two registers: R0 contains the time (hour 0..11, minute, second), while R1 contains the date (year minus 2025, month, day) and the PM indicator in bit 0. This is the FS16 filesystem timestamp format; the pair can be written directly to MTIME without conversion. The library defines the ports `RTC_TIME`, `RTC_DATE`, `RTC_CMD`, `RTC_STATUS`, commands `RTC_READ`, `RTC_WRITE`, `RTC_WRITE_TIME`, `RTC_WRITE_DATE`, and status bit `RTC_ERR`.

| Routine | Input | Result |
|---|---|---|
| `rtc_get` | — | R0: time, R1: date |
| `rtc_set` | R0: time, R1: date | R0 = 0 and ZF = 1 on success; R0 = 1 for invalid input, with the clock unchanged |
| `rtc_hour24` | R0: time, R1: date | R0: hour in the range 0..23 |
| `print_time` | R0: time, R1: date | Output `HH:MM:SS` in 24-hour notation |
| `print_date` | R1: date | Output `DD.MM.YYYY` |

`rtc_get` and `rtc_set` access the RTC with IF = 0, so they can also be called from interrupt handlers. The command-line emulator initializes the RTC to the host's local time. Use `--rtc DD.MM.YYYYTHH:MM:SS` to supply a fixed starting time for reproducible runs.

For special keys (arrows, Esc, F1–F12), check the scan code; for printable keys (space and letters), check the character code. When using stdin with the command-line emulator, ordinary characters have no scan code.

All library routines preserve R4..R11.

### Source Code

```asm
; stdlib.asm - a small CPU16 standard library (the specification, revision 6).
;
; Inclusion: .include "stdlib.asm" at the place in the program where the library
; code should lie (usually after the main code, in ROM). The library uses no
; variables in memory, so it can be placed in ROM.
;
; Calling convention
;   - arguments in R0..R3 (if there are more - through the stack, the caller pops them);
;   - the result in R0..R3;
;   - a routine may clobber R0..R3, LR and FLAGS (scratch registers);
;   - a routine must preserve R4..R11 and SP;
;   - 32-bit numbers are in a register pair: the high word in R1 (R3), the low in R0 (R2),
;     written R1:R0.
;
; The console is port $FF20 ("CPU16 Machine: Memory and Devices"): writing a byte prints a character,
; reading a byte gives the character of the next key press or 0 if there are none; reading a word gives the character
; and the scancode. A zero byte in the input is indistinguishable from "no input", so getc waits for a nonzero byte.
; The section "Display, keyboard, timer" at the end of the file - coordinate output, color, keys
; with scancodes and the 50 Hz timer; the section "Real-time clock" - reading, setting and printing
; the date and time.

        .equ    CONSOLE, $FF20

; ============================================================================
; Interrupt vectors (bare machine)
; ============================================================================

; vectors_init - fill the vector table $FF00..$FF1F. R0 is the ERRCMD handler (vector 8).
; Vector 8 is set first, all the others (IRQ0..IRQ7, TRAP 9..15) go to the empty handler
; irq_ignore. This way the program survives an interrupt it knows nothing about (for example, from
; a device that appeared later) and a TRAP on a free vector. Working handlers are set
; after the call. irq_ignore does not clear a level request, but after reset the device
; raises no requests until the program enables them. Under the monitor the vectors belong to the monitor:
; vectors_init is not called there. Clobbers R1, R2.
vectors_init:
        ST      [$FF00 + 2*8], R0       ; ERRCMD first of all
        LD      R1, #$FF00
        LD      R2, #irq_ignore
@next:  CMP     R1, #$FF00 + 2*8
        JZ      @skip
        ST      [R1], R2
@skip:  ADD     R1, #2
        CMP     R1, #$FF20
        JB      @next
        RET

; irq_ignore - an empty interrupt handler: changes nothing and returns at once.
irq_ignore:
        IRET

; ============================================================================
; Output
; ============================================================================

; putc - print a character. R0 is the character.
putc:   STB     [CONSOLE], R0
        RET

; newline - a newline.
newline:
        LD      R0, #10
        STB     [CONSOLE], R0
        RET

; puts - print a zero-terminated string. R0 is the string address.
puts:   LDZX    R1, [R0++]
        CMP     R1, #0
        JZ      @done
        STB     [CONSOLE], R1
        JMP     puts
@done:  RET

; print_u16 - print an unsigned number in decimal. R0 is the number.
print_u16:
        LD      R2, #0          ; the number of digits on the stack
@div:   LD      R1, R0
        REMU    R1, #10         ; the lowest digit
        DIVU    R0, #10
        ADD     R1, #'0'
        PUSH    R1
        INC     R2
        CMP     R0, #0
        JNZ     @div
@out:   POP     R1              ; the digits from the stack go from the highest to the lowest
        STB     [CONSOLE], R1
        DEC     R2
        JNZ     @out
        RET

; print_s16 - print a signed number in decimal. R0 is the number.
print_s16:
        TEST    R0, R0
        JNS     print_u16       ; non-negative: print as is
        LD      R1, #'-'
        STB     [CONSOLE], R1
        NEG     R0              ; -32768 gives $8000 = 32768, which is what is needed
        JMP     print_u16       ; a tail call: LR points to the caller

; print_hex8 - print the low byte of R0 as two hexadecimal digits.
print_hex8:
        SHL     R0, #8          ; the byte into the high half
        LD      R2, #2
        JMP     _hex_digits

; print_hex16 - print R0 as four hexadecimal digits.
print_hex16:
        LD      R2, #4
_hex_digits:                    ; R0 is the value (the high digits on top), R2 the number of digits
        ROR     R0, #12         ; a rotate left by 4: the high nibble goes down
        LD      R1, R0
        AND     R1, #15
        CMP     R1, #10
        JB      @digit
        ADD     R1, #'A'-'0'-10
@digit: ADD     R1, #'0'
        STB     [CONSOLE], R1
        DEC     R2
        JNZ     _hex_digits
        RET

; print_hex32 - print R1:R0 as eight hexadecimal digits.
print_hex32:
        PUSH    LR
        PUSH    R0
        LD      R0, R1
        CALL    print_hex16     ; the high word
        POP     R0
        POP     LR
        JMP     print_hex16     ; the low word, a tail call

; print_u32 - print R1:R0 unsigned in decimal.
print_u32:
        PUSH    LR
        PUSH    R4
        LD      R4, #0          ; the number of digits on the stack
@div:   LD      R2, #10
        CALL    divu32          ; R1:R0 = R1:R0 / 10, R2 = the remainder
        ADD     R2, #'0'
        PUSH    R2
        INC     R4
        LD      R3, R0
        OR      R3, R1          ; is the quotient still nonzero?
        JNZ     @div
@out:   POP     R0
        STB     [CONSOLE], R0
        DEC     R4
        JNZ     @out
        POP     R4
        POP     IP              ; return: the saved LR is on top

; print_s32 - print R1:R0 signed in decimal.
print_s32:
        TEST    R1, R1
        JNS     print_u32
        LD      R2, #'-'
        STB     [CONSOLE], R2
        NOT     R0              ; R1:R0 = -R1:R0
        NOT     R1
        ADD     R0, #1
        ADC     R1, #0
        JMP     print_u32

; ============================================================================
; Input
; ============================================================================

; getc - wait for an input byte. Result: R0 is the byte (not 0).
; If the input has ended, it waits forever (0 in the input is indistinguishable from "no input").
getc:   LDZX    R0, [CONSOLE]
        TEST    R0, R0
        JZ      getc
        RET

; getc_nb - take an input byte without waiting. Result: R0 is the byte or 0; ZF = 1 if 0.
getc_nb:
        LDZX    R0, [CONSOLE]
        TEST    R0, R0
        RET

; gets - read a line up to a newline.
;   R0 is the buffer, R1 the buffer size in bytes (at least 1), R2 the echo (0 - no, otherwise yes).
; The buffer gets the line without '\n' with a terminating zero; extra characters are discarded.
; Backspace (8 or 127) deletes the last character, CR (13) is ignored.
; Result: R0 is the length of the line.
gets:   PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        LD      R4, R0          ; the start of the buffer
        LD      R5, R0          ; the current position
        LD      R6, R1
        DEC     R6              ; how many characters still fit
        LD      R7, R2          ; echo
@next:  CALL    getc
        CMP     R0, #10
        JZ      @end
        CMP     R0, #13
        JZ      @next
        CMP     R0, #8
        JZ      @back
        CMP     R0, #127
        JZ      @back
        TEST    R6, R6
        JZ      @next           ; the buffer is full
        STB     [R5++], R0
        DEC     R6
        TEST    R7, R7
        JZ      @next
        STB     [CONSOLE], R0
        JMP     @next
@back:  CMP     R5, R4
        JZ      @next           ; nothing to delete
        DEC     R5
        INC     R6
        TEST    R7, R7
        JZ      @next
        LD      R0, #8          ; echo: step back, a space, step back
        STB     [CONSOLE], R0
        LD      R0, #' '
        STB     [CONSOLE], R0
        LD      R0, #8
        STB     [CONSOLE], R0
        JMP     @next
@end:   LD      R0, #0
        STB     [R5], R0
        TEST    R7, R7
        JZ      @len
        LD      R0, #10
        STB     [CONSOLE], R0
@len:   LD      R0, R5
        SUB     R0, R4
        POP     R7
        POP     R6
        POP     R5
        POP     R4
        POP     IP

; read_dec - read a decimal number: blanks, an optional '-', digits.
; Reading ends at the first character that is not a digit (it is taken from the input).
;   R0 is the echo (0 - no, otherwise yes).
; Result: R0 is the number (a negative one in two's complement),
;            R1 - 0 if a number was read; 1 - no digits or overflow
;                 (above 65535, and for negative ones below -32768).
read_dec:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        LD      R6, R0          ; echo
        LD      R4, #0          ; value
        LD      R5, #0          ; 1 - there was a minus
        LD      R7, #0          ; 0 - no digits, 1 - there are, 2 - overflow
@next:  CALL    getc
        TEST    R6, R6
        JZ      @check
        STB     [CONSOLE], R0
@check: CMP     R0, #'0'
        JB      @other
        CMP     R0, #'9'
        JA      @other
        SUB     R0, #'0'
        MUL     R4, #10
        JC      @ovf            ; the product did not fit in 16 bits
        ADD     R4, R0
        JC      @ovf
        CMP     R7, #2
        JZ      @next           ; overflow has already happened
        LD      R7, #1
        JMP     @next
@ovf:   LD      R7, #2
        JMP     @next
@other: TEST    R7, R7
        JNZ     @done           ; the end of the number
        CMP     R0, #' '
        JZ      @next           ; blanks before the number
        CMP     R0, #'-'
        JNZ     @done           ; neither a digit nor a sign: an error
        TEST    R5, R5
        JNZ     @done           ; a second minus: an error
        LD      R5, #1
        JMP     @next
@done:  LD      R1, #1          ; an error by default
        CMP     R7, #1
        JNZ     @ret
        TEST    R5, R5
        JZ      @ok
        CMP     R4, #$8000
        JA      @ret            ; below -32768
        NEG     R4
@ok:    LD      R1, #0
@ret:   LD      R0, R4
        POP     R7
        POP     R6
        POP     R5
        POP     R4
        POP     IP

; ============================================================================
; Memory and strings
; ============================================================================

; memcpy - copy R2 bytes from R1 to R0.
memcpy: TEST    R2, R2
        JZ      @done
@loop:  LDZX    R3, [R1++]
        STB     [R0++], R3
        DEC     R2
        JNZ     @loop
@done:  RET

; memset - fill R2 bytes at address R0 with the value R1.
memset: TEST    R2, R2
        JZ      @done
@loop:  STB     [R0++], R1
        DEC     R2
        JNZ     @loop
@done:  RET

; strlen - the length of a zero-terminated string. R0 is the address. Result: R0 is the length.
strlen: LD      R1, R0
@loop:  LDZX    R2, [R1++]
        TEST    R2, R2
        JNZ     @loop
        SUB     R1, R0
        DEC     R1
        LD      R0, R1
        RET

; ============================================================================
; 32-bit arithmetic. Operands: a = R1:R0, b = R3:R2.
; ============================================================================

; add32 - R1:R0 = a + b. CF is the carry out of the high word.
add32:  ADD     R0, R2
        ADC     R1, R3
        RET

; sub32 - R1:R0 = a - b. CF is the borrow (a < b unsigned).
sub32:  SUB     R0, R2
        SBC     R1, R3
        RET

; neg32 - R1:R0 = -a.
neg32:  NOT     R0
        NOT     R1
        ADD     R0, #1
        ADC     R1, #0
        RET

; cmp32u - compare a and b unsigned; the result is in the flags
; for JZ, JNZ, JB, JAE, JA, JBE (signed conditions do not work after it).
cmp32u: CMP     R1, R3
        JNZ     @done
        CMP     R0, R2
@done:  RET

; mulu16 - R1:R0 = R0 * R1 (16 x 16 = 32 bits, unsigned).
mulu16: LD      R2, R0
        MUL     R0, R1          ; the low word
        MULHU   R2, R1          ; the high word
        LD      R1, R2
        RET

; mul32 - R1:R0 = the low 32 bits of a * b (the same for signed and unsigned numbers).
mul32:  PUSH    R4
        LD      R4, R0
        MULHU   R4, R2          ; the high word of a0*b0
        MUL     R1, R2          ; a1*b0, the low word
        ADD     R4, R1
        LD      R1, R0
        MUL     R1, R3          ; a0*b1, the low word
        ADD     R1, R4          ; the high word of the result
        MUL     R0, R2          ; the low word of a0*b0
        POP     R4
        RET

; divu32 - unsigned division of R1:R0 by the 16-bit R2.
; Result: R1:R0 is the quotient, R2 the remainder, OF = 0.
; Division by 0: R1:R0 = $FFFFFFFF, R2 = the low word of the dividend, OF = 1 (as with DIVU).
divu32: TEST    R2, R2
        JNZ     @go
        LD      R2, R0
        LD      R0, #$FFFF
        LD      R1, R0
        OR      FLAGS, #$10     ; OF = 1
        RET
@go:    PUSH    R4
        LD      R3, #0          ; remainder
        LD      R4, #32         ; a quotient bit
@loop:  ADD     R0, R0          ; shift the dividend left, the high bit goes into the remainder
        ADC     R1, R1
        ADC     R3, R3
        JC      @sub            ; the remainder became 17-bit: certainly not less than the divisor
        CMP     R3, R2
        JB      @next
@sub:   SUB     R3, R2
        INC     R0              ; the next quotient bit = 1
@next:  DEC     R4
        JNZ     @loop
        LD      R2, R3
        POP     R4
        AND     FLAGS, #$FFEF   ; OF = 0, the other flags are not touched
        RET

; ============================================================================
; Display, keyboard, timer ("CPU16 Machine: Memory and Devices")
; ============================================================================

        .equ    CON_CTRL,   $FF22       ; mode: MODE_TTY | MODE_CURSOR | MODE_ATTR
        .equ    CON_ATTR,   $FF23       ; current attribute
        .equ    CON_COL,    $FF24       ; position column 0..79
        .equ    CON_ROW,    $FF25       ; position row 0..24
        .equ    CON_CMD,    $FF26       ; command
        .equ    CON_COLS,   $FF27       ; screen width (80)
        .equ    CON_ROWS,   $FF28       ; screen height (25)
        .equ    TIMER,      $FF29       ; bit 0 - the 50 Hz timer on IRQ0

        .equ    MODE_TTY,    1          ; teletype: \n \r \b \t, scrolling
        .equ    MODE_CURSOR, 2          ; cursor visible
        .equ    MODE_ATTR,   4          ; a write changes only attributes

        .equ    CMD_CLEAR,  1           ; clear the screen, position (0, 0)
        .equ    CMD_CLEOL,  2           ; clear to the end of the line
        .equ    CMD_CLEOS,  3           ; clear to the end of the screen
        .equ    CMD_SCROLLUP,   4       ; scroll up
        .equ    CMD_SCROLLDOWN, 5       ; scroll down

        ; colors; attribute = BACKGROUND << 3 | TEXT, plus UNDERLINE and INVERSE
        .equ    BLACK, 0
        .equ    BLUE, 1
        .equ    GREEN, 2
        .equ    CYAN, 3
        .equ    RED, 4
        .equ    YELLOW, 5
        .equ    GRAY, 6
        .equ    WHITE, 7
        .equ    UNDERLINE, $40
        .equ    INVERSE, $80

        ; scancodes (the high byte of a key code)
        .equ    SC_ESC,   $01
        .equ    SC_BACKSPACE, $0E
        .equ    SC_TAB,   $0F
        .equ    SC_ENTER, $1C
        .equ    SC_SPACE, $39
        .equ    SC_F1,    $3B           ; F1..F10 - $3B..$44 consecutively
        .equ    SC_F11,   $57
        .equ    SC_F12,   $58
        .equ    SC_HOME,  $47
        .equ    SC_UP,    $48
        .equ    SC_PGUP,  $49
        .equ    SC_LEFT,  $4B
        .equ    SC_RIGHT, $4D
        .equ    SC_END,   $4F
        .equ    SC_DOWN,  $50
        .equ    SC_PGDN,  $51
        .equ    SC_INS,   $52
        .equ    SC_DEL,   $53

; scr_mode - the display mode. R0 is MODE_TTY | MODE_CURSOR | MODE_ATTR.
scr_mode:
        STB     [CON_CTRL], R0
        RET

; scr_attr - the current attribute. R0 is the attribute.
scr_attr:
        STB     [CON_ATTR], R0
        RET

; scr_goto - the position. R0 is the column, R1 the row.
scr_goto:
        STB     [CON_COL], R0
        STB     [CON_ROW], R1
        RET

; scr_clear - clear the screen with the current attribute, position (0, 0).
scr_clear:
        LD      R0, #CMD_CLEAR
        STB     [CON_CMD], R0
        RET

; scr_print_at - print a string at a position. R0 is the column, R1 the row, R2 the string address.
scr_print_at:
        STB     [CON_COL], R0
        STB     [CON_ROW], R1
        LD      R0, R2
        JMP     puts                    ; a tail call

; scr_paint - change the attribute of R1 cells from the position, without changing the text. R0 is the attribute, R1 the number of cells.
; The position moves by R1 cells.
scr_paint:
        LDZX    R2, [CON_CTRL]          ; remember the mode
        LD      R3, R2
        OR      R3, #MODE_ATTR
        STB     [CON_CTRL], R3
        TEST    R1, R1
        JZ      @done
@loop:  STB     [CONSOLE], R0           ; in attribute mode a write changes only the attribute
        DEC     R1
        JNZ     @loop
@done:  STB     [CON_CTRL], R2
        RET

; scr_box - a double frame in coordinate mode. R0 is the column, R1 the row of the top left corner,
; R2 the width, R3 the height (both at least 2). Drawn with the current attribute.
scr_box:
        PUSH    LR                      ; calls _box_run: LR must be saved
        PUSH    R4
        PUSH    R5
        STB     [CON_COL], R0
        STB     [CON_ROW], R1
        LD      R4, R2
        SUB     R4, #2                  ; width without the corners
        LD      R5, #'╔'
        STB     [CONSOLE], R5
        LD      R5, #'═'
        CALL    _box_run
        LD      R5, #'╗'
        STB     [CONSOLE], R5
        LD      R5, R3                  ; the sides: R5 is how many rows are left
        SUB     R5, #2
@side:  TEST    R5, R5
        JZ      @bottom
        INC     R1
        STB     [CON_ROW], R1
        STB     [CON_COL], R0
        LD      R4, #'║'
        STB     [CONSOLE], R4
        LD      R4, R0
        ADD     R4, R2
        DEC     R4
        STB     [CON_COL], R4
        LD      R4, #'║'
        STB     [CONSOLE], R4
        DEC     R5
        JMP     @side
@bottom:
        INC     R1
        STB     [CON_ROW], R1
        STB     [CON_COL], R0
        LD      R4, R2
        SUB     R4, #2
        LD      R5, #'╚'
        STB     [CONSOLE], R5
        LD      R5, #'═'
        CALL    _box_run
        LD      R5, #'╝'
        STB     [CONSOLE], R5
        POP     R5
        POP     R4
        POP     IP
_box_run:                               ; print character R5 R4 times (a leaf: it does not touch LR)
        TEST    R4, R4
        JZ      @done
@loop:  STB     [CONSOLE], R5
        DEC     R4
        JNZ     @loop
@done:  RET

; getkey - wait for a key press. Result: R0 is the character (the low byte) and the scancode (the high byte).
getkey: LD      R0, [CONSOLE]           ; a word read: the character and scancode of one key press
        TEST    R0, R0
        JZ      getkey
        RET

; getkey_nb - a key press without waiting. Result: R0 is the character and scancode or 0; ZF = 1 if 0.
getkey_nb:
        LD      R0, [CONSOLE]
        TEST    R0, R0
        RET

; timer_on, timer_off - the 50 Hz timer on IRQ0. Fill in the IRQ0 vector before enabling it.
timer_on:
        LD      R0, #1
        STB     [TIMER], R0
        RET
timer_off:
        LD      R0, #0
        STB     [TIMER], R0
        RET

; ============================================================================
; Real-time clock ("CPU16 Machine: Memory and Devices", sect. 8)
; ============================================================================
; The clock value is an FS16 timestamp in a register pair: R0 is the time (hour 0..11, minute,
; second), R1 the date (year - 2025, month, day), and bit 0 holds the PM flag. The pair can be written
; without conversion into the MTIME field of a directory entry: R0 into the word +36, R1 into the word +38.
; rtc_get and rtc_set work with the clock at IF = 0, so they can also be called from interrupt
; handlers.

        .equ    RTC_TIME,       $FF2A
        .equ    RTC_DATE,       $FF2C
        .equ    RTC_CMD,        $FF2E
        .equ    RTC_STATUS,     $FF2E
        .equ    RTC_READ,       1
        .equ    RTC_WRITE,      2
        .equ    RTC_WRITE_TIME, 3
        .equ    RTC_WRITE_DATE, 4
        .equ    RTC_ERR,        $01

; rtc_get - read the clock. Result: R0 is the time, R1 the date.
rtc_get:
        PUSH    FLAGS                   ; a critical section: READ and reading
        CLI                             ;   the registers without a handler in between
        LD      R0, #RTC_READ
        STB     [RTC_CMD], R0           ; the clock value -> TIME, DATE
        LD      R0, [RTC_TIME]
        LD      R1, [RTC_DATE]
        POP     FLAGS
        RET

; rtc_set - set the clock. R0 is the time, R1 the date.
; Result: R0 = 0 and ZF = 1 - the clock is set;
;            R0 = 1 - the value is invalid, the clock did not change.
rtc_set:
        PUSH    FLAGS
        CLI
        ST      [RTC_TIME], R0
        ST      [RTC_DATE], R1
        LD      R0, #RTC_WRITE
        STB     [RTC_CMD], R0
        LDZX    R0, [RTC_STATUS]        ; bit 0 - ERR, the others 0
        POP     FLAGS
        CMP     R0, #0
        RET

; rtc_hour24 - the hour in 24-hour notation. R0 is the time, R1 the date. Result: R0 is the hour 0..23.
rtc_hour24:
        SHR     R0, #12                 ; hour 0..11
        TEST    R1, #1                  ; PM?
        JZ      @am
        ADD     R0, #12
@am:    RET

; print_time - print the time HH:MM:SS in 24-hour notation. R0 is the time, R1 the date (the PM flag).
print_time:
        PUSH    LR
        PUSH    R4
        LD      R4, R0                  ; time
        CALL    rtc_hour24
        CALL    _print_2d
        LD      R0, #':'
        STB     [CONSOLE], R0
        LD      R0, R4
        SHR     R0, #6
        AND     R0, #63                 ; minute
        CALL    _print_2d
        LD      R0, #':'
        STB     [CONSOLE], R0
        LD      R0, R4
        AND     R0, #63                 ; second
        CALL    _print_2d
        POP     R4
        POP     IP

; print_date - print the date DD.MM.YYYY. R1 is the date (R0 is not used: the input is the same as for
; print_time - the result of rtc_get).
print_date:
        PUSH    LR
        PUSH    R4
        LD      R4, R1                  ; date
        LD      R0, R4
        SHR     R0, #1
        AND     R0, #31                 ; day
        CALL    _print_2d
        LD      R0, #'.'
        STB     [CONSOLE], R0
        LD      R0, R4
        SHR     R0, #6
        AND     R0, #15                 ; month
        CALL    _print_2d
        LD      R0, #'.'
        STB     [CONSOLE], R0
        LD      R0, R4
        SHR     R0, #10
        ADD     R0, #2025               ; year
        CALL    print_u16
        POP     R4
        POP     IP

_print_2d:                              ; R0 = 0..99 as two digits (a leaf: clobbers R0, R1)
        LD      R1, R0
        DIVU    R1, #10
        ADD     R1, #'0'
        STB     [CONSOLE], R1
        REMU    R0, #10
        ADD     R0, #'0'
        STB     [CONSOLE], R0
        RET
```

## Appendix B. Instruction Cheat Sheet

| Group | Instructions |
|---|---|
| Data transfer | `LD`/`MOV` `ST` `LDZX` `LDSX` `STB` `XCHB` `LEA` `PUSH` `POP` |
| Arithmetic | `ADD` `ADC` `SUB` `SBC` `CMP` `NEG` `INC` `DEC` `MUL` `MULHU` `DIVU` `REMU` |
| Logic | `AND` `OR` `XOR` `TEST` `NOT` `BSX` |
| Shifts | `SHL` `SHR` `SAR` `ROR` |
| Branches | `JMP` `JZ` `JNZ` `JC`/`JB` `JNC`/`JAE` `JA` `JBE` `JS` `JNS` `JO` `JNO` `JL` `JGE` `JG` `JLE` |
| Subroutines | `CALL` `RET` |
| Interrupts | `STI` `CLI` `TRAP` `IRET` `WFI` |
| Flags | `STC` `CLC` `CMC` (and `AND`/`OR`/`XOR` on `FLAGS`) |
| Miscellaneous | `NOP` |
