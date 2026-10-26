# CPU16 Assembly Language Reference

This document defines the assembly language for the processor described in `cpu16_spec_r6` (Revision 6). It is the normative reference for assembler implementations. The terms **must**, **prohibited**, and **error** state requirements; other text provides explanations and examples.

Sections 1–16 describe the language and assembly output independently of any implementation. Section 17 documents the current implementations—the `cpu16asm` command-line assembler and the web debugger's assembler—including command-line options, listing and memory-map formats, messages, and limitations.

References of the form “Architecture Specification, Section N” refer to the processor architecture document.

## 1. Source File Structure

A source file consists of lines. Each line has the form:

```
[label:] [instruction or directive] [; comment]
```

- Any part may be omitted. Empty lines and comment-only lines are allowed.
- A comment begins with `;` and continues to the end of the line, except when the semicolon appears inside a string or character literal.
- The starting column is irrelevant. Spaces and tabs may be used freely between tokens.
- Each line may contain at most one instruction or directive. A label may appear on its own line or before an instruction or directive.
- Line continuation is not supported.
- Source files must use UTF-8. Comments may contain arbitrary characters. String and character literals are converted to the CPU16 character encoding (see “Strings and Characters”).

```
        .org    0          ; directive
start:  LD      SP, #$FF00 ; label, instruction, comment
loop:                      ; standalone label
        JMP     loop
```

### Case Sensitivity

| Element | Case-sensitive? |
|---|---|
| Mnemonics, register names, directives, `lo`, `hi` | No (`ld`, `LD`, and `Ld` are equivalent) |
| Labels and `.equ` symbols | Yes (`Loop` and `loop` are distinct symbols) |

## 2. Lexical Conventions

### Identifiers

- A global identifier matches `[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z0-9_]+)*`; examples include `con.data`, `list.next`, `main.loop`, and `pt.x.2`. A period is allowed only within a name: it cannot be the first character (a leading period begins a directive name; see Section 9), the last character, or immediately adjacent to another period. `end.` and `a..b` cause an `invalid name` error.
- A local label consists of `@` followed by an identifier, such as `@loop` or `@retry.2` (see Section 4).
- Register names (Section 5), all instruction mnemonics (including pseudo-instructions, Section 7), and the functions `lo` and `hi` are reserved; they cannot be used as label or symbol names. Dotted names are not identical to any reserved word: `R1.x`, `ld.b`, and `lo.y` are ordinary names.
- A period is part of the entire identifier: `a.b` is one name, not `a` followed by another construct. A colon still separates a label from a directive: `buf:.word 1` is valid.
- A dotted name is an ordinary global label; the period does not create a namespace. Like any global label, `main.loop:` begins a new local-label scope.
- Directive names do not contain periods. `.word.x` is parsed as directive `.word` followed by an invalid operand.
- A number cannot continue with a period. `1.5` and `$FF.x` cause a `malformed number` error.

### Numbers

| Notation | Base or meaning | Examples |
|---|---|---|
| `123` | Decimal | `0`, `65535` |
| `$FF`, `0xFF` | Hexadecimal | `$FF00`, `0x1234` |
| `%1010`, `0b1010` | Binary | `%1111_0000` is invalid; underscores are not supported |
| `'A'` | Character code | `'a'`, `'\n'`, `'\''` |

A `$` immediately followed by a hexadecimal digit starts a number. Otherwise, `$` denotes the location counter (Section 3).

A `%` at the beginning of an operand or after an operator introduces a binary number. There is no remainder operator.

### Strings and Characters

- A string is written as `"..."`. A character is written as `'x'`; its value is the character's CPU16 encoding.
- **Character encoding.** String and character literals are converted from UTF-8 to the CPU16 character set (“CPU16 Machine: Memory and Devices,” Section 5). The supported repertoire includes ASCII, Cyrillic letters (`А–я` and `ёЁ`), box-drawing and block characters, and symbols such as `☺ ♥ → ▲`. A character outside that repertoire (for example `€`, `«`, or `№`) causes `character '€' is not in the CPU16 character set`. Invalid UTF-8 causes `invalid UTF-8 in string`.
- Escape sequences supported in strings and characters are `\n` (10), `\r` (13), `\t` (9), `\0` (0), `\\`, `\"`, `\'`, and `\xHH` (exactly two hexadecimal digits). Escapes specify bytes directly; they are not converted through the character encoding.
- String literals are permitted only in `.ascii`, `.asciz`, and `.include` directives. The filename in `.include` is not converted to the CPU16 character encoding.

```
msg:    .asciz  "Привет, ёлка! ╔═╗"     ; Cyrillic and box drawing in the CPU16 character set
        .byte   'Ж', '\xD0'             ; $C6, followed by the literal byte $D0
```

## 3. Expressions

Expressions are evaluated using signed integer arithmetic with a width of at least 32 bits. Operands may be numbers, symbols, labels, the location counter `$`, or function calls.

| Precedence (highest first) | Operators |
|---|---|
| 1 | Unary `+`, `-`, `~` |
| 2 | `*`, `/` (integer division truncated toward zero) |
| 3 | `+`, `-` |
| 4 | `<<`, `>>` (arithmetic right shift) |
| 5 | `&` |
| 6 | `^` |
| 7 | `\|` |

- Parentheses `( )` override the normal precedence.
- `lo(x)` evaluates to `x & $FF`; `hi(x)` evaluates to `(x >> 8) & $FF`.
- `$` is the address of the first byte of the current instruction or directive.
- Registers cannot appear in expressions.
- Division by zero, shift counts outside 0..31, and unclosed parentheses are errors.
- A label evaluates to its address (0..65535).

### Range Checking at the Destination

The final value must fit the destination or assembly fails. Negative values are encoded in two's complement.

| Destination | Permitted range |
|---|---|
| Word (`.word`, `disp`, long immediate) | −32768..65535 |
| Byte (`.byte`) | −128..255 |
| Short immediate (4 bits) | 0..15 |
| Short conditional branch (JR) | −64..+63 words |
| Short CALL | −8..+7 words |

## 4. Symbols and Labels

- A **label** (`name:`) takes the value of the location counter after any implicit alignment (Section 9). A label immediately before an instruction or `.word` is therefore always even-aligned. Duplicate definitions are errors. Forward references are permitted.
- A **local label** (`@name:`) is visible within the region beginning at the preceding global label and ending just before the next global label. The same local name in different regions denotes different labels. Local labels preceding the first global label belong to an anonymous scope.
- A **constant** is defined by `.equ name, expression`. Its expression may reference labels or constants defined later in the file. Cyclic definitions are errors.
- Symbol names must be unique across labels and constants.

```
        .equ    CONSOLE, $FF20
        .equ    IF_BIT, $08

start:  LD      R0, #1
@wait:  SUB     R0, #1
        JNZ     @wait        ; local label within start
other:  JMP     @wait        ; error: @wait is not defined in this scope
```

## 5. Registers

| Name | Number | Description |
|---|---|---|
| `R0`..`R11` | 0..11 | General-purpose registers |
| `R12`, `LR` | 12 | Link register |
| `R13`, `SP` | 13 | Stack pointer |
| `R14`, `IP` | 14 | Instruction pointer |
| `R15`, `FLAGS` | 15 | Flags register |

Register aliases are fully interchangeable: `SP` and `R13` produce the same encoding.

## 6. Operands and Addressing Modes

An instruction word has the layout `OPCODE:5 R1:4 R2:4 R2MOD:3` (most significant bit on the left). The source operand (the “second operand”) determines R2 and R2MOD and, when required, an additional `disp` word.

| Syntax | MOD | R2 | Second word | Meaning |
|---|---|---|---|---|
| `Rn` | 0 | n | None | Register value |
| `#expr` (0..15) | 1 | Value | None | Short immediate |
| `#expr` (otherwise) | 3 | 14 (IP) | Value | Long immediate |
| `[Rn]` | 2 | n | None | Word/byte at address Rn |
| `[Rn++]` | 3 | n | None | Memory access, then Rn += stride |
| `[--Rn]` | 4 | n | None | Rn −= stride, then memory access |
| `[expr]` | 5 | 0 | Address | Absolute memory address |
| `[Rn+expr]`, `[Rn-expr]` | 6 | n | Displacement | Register plus displacement, dereferenced |
| `Rn+expr`, `Rn-expr` | 7 | n | Displacement | Address calculation without dereferencing |

Rules:

- Whitespace is allowed inside brackets and around `+`, `-`, `++`, and `--`.
- If the first token inside brackets or in the operand is a register name, the operand uses a register-based form. Otherwise, it is an expression.
- For `Rn-expr`, the encoded displacement is `−expr` modulo 2^16.
- The `++`/`--` stride is 2 for word instructions and 1 for byte instructions (LDZX, LDSX, STB, XCHB).
- `[IP++]` is prohibited in source code: an immediate value must use `#expr` instead.
- Additional target forms for branches are specified in Section 7.5.
- R2 is unused and must be zero when MOD=5 (Architecture Specification, Section 3). The assembler always emits zero in this field.

### Choosing the `#expr` Encoding

For LD, MOV, ADD, ADC, SUB, SBC, AND, OR, XOR, CMP, and TEST:

- If the expression is resolved and its value is in 0..15, use the **short form** (MOD=1, one word).
- Otherwise, use the **long form** (MOD=3, R2=IP, two words). The value must fit a word (−32768..65535).

The assembler makes this choice according to Section 10. Other instructions use the fixed forms described below.

## 7. Instructions

As a general rule, `OP Ra, B` places the first operand in R1 and the second operand in R2/MOD (plus an optional second word). The exceptions are the memory-store and exchange instructions `ST`, `STB`, and `XCHB`: their **first** source-level operand specifies the memory address and their second specifies the register. That register is encoded in R1, as with the `PUSH` pseudo-instruction.

### 7.1. Loads, Stores, and Exchange

| Syntax | Effect |
|---|---|
| `LD Rd, src` | Rd = operand word |
| `LDZX Rd, src` | Rd = operand byte, zero-extended |
| `LDSX Rd, src` | Rd = operand byte, sign-extended |
| `ST [mem], Rs` | Store Rs as a word |
| `STB [mem], Rs` | Store the low byte of Rs |
| `XCHB [mem], Rs` | Atomic exchange: Rs = old memory byte (zero-extended); memory = previous low byte of Rs |

- `src` may use any addressing form from Section 6, subject to Section 8.
- `mem` may be `[Rn]`, `[Rn++]`, `[--Rn]`, `[expr]`, or `[Rn+expr]`.
- For `LDZX`/`LDSX`, `#expr` is allowed only as a short immediate (0..15, MOD=1), and the `Rn+expr` form is prohibited. `LDZX Rd, Rs` uses the low byte of Rs.

```
        LD      R1, R2           ; copy register
        LD      R3, #1000        ; long immediate
        LDZX    R0, [R4++]       ; byte, post-increment by 1
        LDSX    R0, [R4+2]
        ST      [$0200], R1      ; word at absolute address
        STB     [R5], R0         ; low byte of R0
        XCHB    [lock], R1       ; atomic byte exchange
```

### 7.2. Arithmetic and Logic

| Syntax | Effect | Flags |
|---|---|---|
| `ADD Rd, src` | Rd = Rd + src | ZF CF SF OF |
| `ADC Rd, src` | Rd = Rd + src + CF | ZF CF SF OF |
| `SUB Rd, src` | Rd = Rd − src | ZF CF (borrow) SF OF |
| `SBC Rd, src` | Rd = Rd − src − CF | ZF CF (borrow) SF OF |
| `AND Rd, src`, `OR`, `XOR` | Bitwise operations | ZF SF; CF=0, OF=0 |
| `CMP Ra, src` | Like SUB, but discard the result | ZF CF SF OF |
| `TEST Ra, src` | Like AND, but discard the result | ZF SF; CF=0, OF=0 |
| `NEG Rd` | Rd = 0 − Rd | ZF CF SF OF |
| `NOT Rd` | Rd = ~Rd | Flags unchanged |
| `BSX Rd` | Sign-extend the low byte of Rd | Flags unchanged |
| `MUL Rd, src` | Rd = low word of Rd × src | ZF SF; CF=OF=1 if high word ≠ 0 |
| `MULHU Rd, src` | Rd = high word of unsigned Rd × src | ZF SF; CF=0, OF=0 |
| `DIVU Rd, src` | Rd = Rd / src (unsigned) | ZF SF; CF=0; OF=1 on division by zero |
| `REMU Rd, src` | Rd = Rd mod src (unsigned) | ZF SF; CF=0; OF=1 on division by zero |

`NEG`, `NOT`, and `BSX` are single-operand instructions encoded using the combined MISC opcode: R2 selects the operation, R2MOD is zero, and the instruction occupies one word.

`MUL`, `MULHU`, `DIVU`, and `REMU` are ordinary ALU-group instructions. All operand forms are supported, and `#expr` selects short or long encoding as for ADD. Division by zero does not raise an exception: `DIVU` returns $FFFF, `REMU` leaves the dividend unchanged, and OF=1 (test with `JO`). Signed multiply and division variants do not exist; signed operations must be implemented in software.

```
        ADD     R0, R1           ; register
        ADD     R0, #5           ; short immediate
        SUB     R2, [R3]         ; from memory
        AND     R0, #$00FF       ; long immediate
        CMP     R0, #0
        TEST    R1, R1
        NEG     R2
        MUL     R0, #10          ; multiply by an immediate
        DIVU    R1, [R3]         ; divisor from memory
        JO      div_by_zero
```

### 7.3. Shifts and Rotates

| Syntax | Effect |
|---|---|
| `SHL Rd, cnt` | Logical left shift |
| `SHR Rd, cnt` | Logical right shift |
| `SAR Rd, cnt` | Arithmetic right shift |
| `ROR Rd, cnt` | Rotate right |

`cnt` may be `#expr` (0..15 only; always MOD=1) or register `Rn` (only the low four bits are used). Other operand forms are also valid under the general rules (memory and `Rn+expr`), although they are rarely useful in practice.

```
        SHL     R0, #4
        SHR     R0, R1           ; shift count from R1
        ROR     R2, #8           ; swap the two bytes of a word
```

### 7.4. Pseudo-Instructions

Pseudo-instructions expand into actual machine instructions; they have no dedicated opcodes.

| Syntax | Expansion | Words |
|---|---|---|
| `MOV Rd, src` | `LD Rd, src` | Same as LD |
| `LEA Rd, Rs+expr` | `LD Rd, Rs+expr` (MOD=7) | 2 |
| `LEA Rd, [Rs+expr]` | Same | 2 |
| `NOP` | `LD R0, R0` (word `$0000`) | 1 |
| `RET` | `LD IP, LR` | 1 |
| `PUSH Rs` | `ST [--SP], Rs` | 1 |
| `POP Rd` | `LD Rd, [SP++]` | 1 |
| `INC Rd` | `ADD Rd, #1` | 1 |
| `DEC Rd` | `SUB Rd, #1` | 1 |
| `STI` | `OR FLAGS, #$08` (enable interrupts) | 1 |
| `CLI` | `AND FLAGS, #$07` (disable interrupts) | 1 |
| `STC` | `OR FLAGS, #$02` (set CF) | 1 |
| `CLC` | `AND FLAGS, #$0D` (clear CF) | 1 |
| `CMC` | `XOR FLAGS, #$02` (toggle CF) | 1 |

- Unlike on many other processors, `INC` and `DEC` also modify CF.
- Flag-manipulation pseudo-instructions are atomic and occupy one word. Their effects on flags (Architecture Specification, Section 2.2) are:

  | Pseudo-instruction | ZF | CF | SF | IF | OF |
  |---|---|---|---|---|---|
  | `STI` | — | — | — | 1 | — |
  | `CLI` | — | — | — | 0 | **0** |
  | `STC` | — | 1 | — | — | — |
  | `CLC` | — | 0 | — | — | **0** |
  | `CMC` | — | inverted | — | — | — |

  “—” indicates that a flag remains unchanged.
- **Side effect:** `CLI` and `CLC` clear OF. Any `AND FLAGS` with a short immediate clears OF because bit 4 (OF) of that immediate is zero. To preserve OF, explicitly use a long immediate: `AND FLAGS, #$FFF7` instead of CLI or `AND FLAGS, #$FFFD` instead of CLC (two words, without this side effect).
- `TEST FLAGS, #$08` (test IF) overwrites ZF, CF, SF, and OF: ZF=1 when IF=0, while the other three flags are cleared.
- `PUSH SP` pushes the value of SP before decrementing it. `POP SP` loads SP from the stack.

### 7.5. Control Transfer

The instruction's target is the operand value (an address). R1 encodes the condition for Jxx; it is ignored for CALL.

| Syntax | Condition | R1 code |
|---|---|---|
| `JMP` | Always | 0 |
| `JZ`, `JE` | ZF=1 | 1 |
| `JNZ`, `JNE` | ZF=0 | 2 |
| `JC`, `JB` | CF=1 (unsigned below) | 3 |
| `JNC`, `JAE` | CF=0 | 4 |
| `JA` | CF=0 and ZF=0 | 5 |
| `JBE` | CF=1 or ZF=1 | 6 |
| `JS` | SF=1 | 7 |
| `JNS` | SF=0 | 8 |
| `JO` | OF=1 | 9 |
| `JNO` | OF=0 | 10 |
| `JL` | SF≠OF (signed less) | 11 |
| `JGE` | SF=OF | 12 |
| `JG` | ZF=0 and SF=OF | 13 |
| `JLE` | ZF=1 or SF≠OF | 14 |

`CALL target` saves the address of the following instruction in LR, then transfers control to the target. It does not use the stack.

#### Target Forms

| Syntax | MOD | Words | Meaning |
|---|---|---|---|
| `expr` (label, number, expression) | Automatic | 1 or 2 | Branch to address `expr`, using short or long PC-relative encoding (see below) |
| `Rn` | 0 | 1 | Address held in register |
| `[Rn]` | 2 | 1 | Address read from the word at address Rn |
| `[expr]` | 5 | 2 | Address read from the word at address `expr` (jump table) |
| `[Rn+expr]` | 6 | 2 | Address read from the word at address Rn+expr |
| `Rn+expr` | 7 | 2 | Address Rn+expr (`IP+expr` uses a raw displacement from the next instruction) |
| `#expr` | 3 (R2=IP) | 2 | Absolute address; force long form |

`[Rn++]` and `[--Rn]` are not valid branch targets.

Automatic selection for `expr` (target address `T`, with `$` denoting the instruction address):

- `T` must be even-aligned; odd targets are errors.
- **Short Jxx form (JR instruction, opcode 6):** the word is `6<<11 | cond<<7 | (k & $7F)`, where `k = (T − ($+2)) / 2`; permitted range: −64 ≤ k ≤ 63. One word.
- **Short CALL form:** MOD=1, R2=`k & 15`, with −8 ≤ k ≤ 7. One word.
- **Long relative form** (for both Jxx and CALL): MOD=7, R2=14 (IP), `disp = (T − ($+4)) & $FFFF`. Two words.
- The assembler never emits Jxx with MOD=1 (±8 words), since JR covers a larger range including that interval. There is no separate `JR` mnemonic in the language; the assembler chooses the short conditional-branch encoding automatically.
- The assembler does not choose absolute addressing automatically. Explicit `#expr` syntax requests it.

```
        JMP     start            ; automatic selection
        JNZ     loop
        CALL    func
        JMP     R3               ; indirect through a register
        JMP     [vector]         ; indirect through a memory word
        CALL    #$8000           ; absolute address
        JZ      IP+6             ; raw displacement: six bytes past the next instruction
```

### 7.6. Interrupts and Wait

| Syntax | Encoding | Words |
|---|---|---|
| `TRAP #n` | MOD=1, R2=n; n in 0..15 | 1 |
| `TRAP Rn` | MOD=0; vector comes from the low four bits of Rn | 1 |
| `IRET` | Combined MISC opcode, subcode 3; R1 and MOD are zero | 1 |
| `WFI` | Combined MISC opcode, subcode 4; R1 and MOD are zero | 1 |

For `TRAP #expr`, any value outside 0..15 is an error.

```
        TRAP    #9               ; software trap, vector 9
        WFI                      ; wait for an interrupt
        IRET
```

## 8. Operand Validity

The assembler must reject all prohibited encodings of both categories—**reserved** and **constrained-unpredictable** (Architecture Specification, Sections 6.2 and 6.3)—with a clear diagnostic. Programs must not use these encodings, even if a particular processor implementation executes them without trapping. Validation should preferably use the same legality-checking function as the emulator.

### 8.1. Permitted Forms by Instruction Group

| Group | `Rn` | `#expr` | `[Rn]` | `[Rn++]` | `[--Rn]` | `[expr]` | `[Rn+expr]` | `Rn+expr` |
|---|---|---|---|---|---|---|---|---|
| LD, MOV, ALU, CMP, TEST, MUL, MULHU, DIVU, REMU, shifts | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| LDZX, LDSX | Yes | 0..15 | Yes | Yes* | Yes* | Yes | Yes | No |
| ST, STB, XCHB | No | No | Yes** | Yes* | Yes* | Yes | Yes | No |
| Jxx, CALL (target) | Yes | Yes (absolute) | Yes | No | No | Yes | Yes | Yes |
| TRAP | Yes | 0..15 | No | No | No | No | No | No |

\* For byte operations (LDZX, LDSX, STB, XCHB), prohibited when Rn is SP or IP; for ST, prohibited when Rn is IP.

\*\* `[IP]` is prohibited for ST, STB, and XCHB.

For shifts, `#expr` must be in 0..15 (MOD=1).

The assembly language supports only `TRAP #n` and `TRAP Rn`. Other TRAP operand forms supported by the processor (Architecture Specification, Section 6.1) are intentionally omitted because they offer little practical value.

### 8.2. Additional Prohibitions

Each violation below is an assembly error; at machine-code level, it represents either a reserved or a constrained-unpredictable encoding:

1. `Rn = FLAGS` is permitted only in the register-direct form `Rn` (MOD=0). It is prohibited in `[Rn]`, `[Rn++]`, `[--Rn]`, `[Rn+expr]`, and `Rn+expr` (Architecture Specification, Section 6.3, item 1).
2. `[--IP]` is prohibited (Section 6.3, item 2).
3. A long `#expr` (immediate through IP) is permitted only for LD, MOV, ALU, CMP, TEST, Jxx, and CALL. Shifts, LDZX, LDSX, and TRAP permit only short immediates in 0..15; ST, STB, and XCHB do not accept immediate operands (Sections 6.2 item 3 and 6.3 item 3).
4. `[IP]` is prohibited for ST, STB, and XCHB (Section 6.3, item 4).
5. Byte instructions using `[Rn++]` or `[--Rn]` are prohibited when Rn is SP or IP (Section 6.3, item 5).
6. Jxx and CALL cannot use `[Rn++]` or `[--Rn]` as targets (Sections 6.2 item 3 and 6.3 item 6).
7. Jxx and JR condition code 15 is not expressible in the language: no such mnemonic exists (Section 6.2, item 2).
8. Every instruction must have exactly the required number of operands; both missing and extra operands are errors.
9. The assembler always sets unused fields to zero (Architecture Specification, Sections 3 and 6.2 item 4): MOD for NEG, NOT, BSX, IRET, and WFI (whose R2 field is the MISC subcode); R1 for IRET, WFI, CALL, and TRAP; and R2 when MOD=5. The language has no `MISC` mnemonic and provides no syntax for assigning nonzero values to these unused fields.

## 9. Directives

Directive names begin with `.` and are case-insensitive.

| Directive | Effect |
|---|---|
| `.org expr` | Set the location counter. The expression must be in 0..65535 and **must not depend on labels**. |
| `.word e1, e2, ...` (`.dw`) | Emit little-endian words. The address is implicitly word-aligned before the first word. Each value must be in −32768..65535. |
| `.byte e1, e2, ...` (`.db`) | Emit bytes, each in −128..255. |
| `.ascii "str", ...` | Emit bytes of each string in the CPU16 encoding, without a trailing zero. |
| `.asciz "str", ...` | As above, but append a zero byte after each string. |
| `.space n [, fill]` | Emit `n` bytes with value `fill` (default 0); `n` must not depend on labels. |
| `.reserve n` | Reserve `n` bytes: advance the location counter without emitting data. `n` must be in 0..65536 and must not depend on labels. |
| `.align n [, fill]` | Explicit alignment: emit bytes with value `fill` (default 0) until the address is a multiple of `n`. `n` must be at least 1 and must not depend on labels. |
| `.equ name, expr` | Define a constant. |
| `.include "file"` | Include another source file. The path is relative to the including file's directory. Maximum nesting depth is implementation-defined (Section 17.7). |
| `.line "file", N` | Assign the source location `file:N` to all following lines until the next `.line`. Source locations are used in diagnostics, memory maps, and listings. |
| `.line N` | Use the same file as the preceding `.line` directive, with line number `N`. |
| `.line` | Resume using the actual assembly source filename and line numbers. |

- The initial location counter value is zero.
- **Source locations (`.line`).** This directive is intended for assembly generated by a compiler. Diagnostics, memory maps, and listings can then point to lines of the original source (for example, B16) rather than to lines of generated assembly. The compiler emits a `.line` before the code generated for each source statement:
  - **Fixed location:** all lines after `.line`, up to the next `.line`, retain the identical `file:N` location; the line number does not increment. One source statement may expand to several instructions, all associated with the same source line. Blank lines, comments, and `.include` lines inside the region do not change the location (this resembles GNU as `.loc`, not C `#line`).
  - `file` must be a nonempty quoted string and is not transcoded (as with `.include`). `N` must be a literal number in 1..1000000. Expressions and symbols are not accepted by `.line`, because source locations are needed at parse time.
  - Without a filename, `.line N` changes only the line number. The filename remains the one from the most recent `.line "file", N` in the current file. It is an error if no such directive has occurred or a bare `.line` has reset the location since then.
  - The line containing `.line`, and any lines preceding the first `.line`, keep their own actual source locations.
  - `.line` remains effective until the next `.line` or the end of the **current** file. Included files start with their own source locations; upon returning, the including file resumes the location established by its `.line` directive.
  - `.include` paths remain relative to the directory of the actual source file, not the filename supplied to `.line`.
  - `.line` does not alter emitted code or data. A label before `.line` binds to the next item as usual.

```
        .line   "kernel.b16", 120
kmain:  LD      R0, #1                  ; kernel.b16:120
        LD      R1, #2                  ; kernel.b16:120
        .line   121
        RET                             ; kernel.b16:121
        .line   "kernel.b16", 135
buf:    .reserve 64                     ; kernel.b16:135
        .line                           ; subsequent lines use actual assembly source locations
```

## 10. Instruction Size and Encoding Selection (Normative)

An instruction occupies:

> **Two words if MOD ∈ {5, 6, 7}, or if MOD=3 and R2=IP; otherwise one word.**

Only two groups have sizes that depend on the values of expressions:

1. `#expr` in LD, MOV, ALU, CMP, and TEST: short (one word) or long (two words).
2. Expression targets for Jxx (JR or long relative form) and CALL (MOD=1 or long relative form).

All other forms (shifts with `#`, LDZX, LDSX, TRAP, and every register-based or bracketed form) have fixed sizes determined solely by their syntax.

### Algorithm

1. Parse the file into a list of items. Determine the sizes of all fixed-size items.
2. Initialize every variable-size item to the SHORT state.
3. Repeat:
   1. Compute the addresses of all items and the values of all labels using the current states.
   2. For each SHORT item, determine whether the current values fit (immediate in 0..15; Jxx displacement in −64..+63 words; CALL displacement in −8..+7 words; even-aligned target). If not, change the item's state to LONG.
   3. If any item changed state, repeat the iteration.
4. Make a final pass to encode every item and check ranges (Section 3), operand legality (Section 8), and overlapping output regions.

The LONG state is irreversible: an item must never revert to SHORT. Consequently, the loop terminates within at most N+1 iterations, where N is the number of variable-size items. The result is deterministic. Undefined symbols are errors detected in the final pass.

The arguments of `.org`, `.space`, `.reserve`, and `.align` are evaluated once before iteration because they cannot depend on labels. `.equ` values that depend on labels are recalculated on every iteration.

Implicit alignment bytes and `.align` padding depend on the current location counter. They are therefore recalculated on each address-calculation iteration (step 3.1). This does not affect termination, which is guaranteed by irreversible SHORT → LONG transitions.

## 11. Assembly Output

The result is a **memory image**: a set of bytes addressed from 0..65535. Each byte is emitted (by an instruction or data directive), reserved (`.reserve`), or free.

- Words are little-endian: the least-significant byte is stored at the lower address.
- There is no entry-point declaration. After reset, the processor starts at address zero, so startup code must reside there.
- The **binary image** contains bytes from the starting address `base` (default 0) through the highest **emitted** address, inclusive. Free addresses within that interval are filled with the configured fill byte (default 0), while reserved addresses are always filled with zero. `.reserve` bytes after the last emitted byte are omitted and do not extend the image. Emitting data below `base` is an error.
- In addition to the image, the assembler may produce a **listing** (address, emitted words and bytes, source line) and a **memory map** (image objects with types and sizes, summary, constants). Locations reported in maps and diagnostics honor `.line` (Section 9).
- On request, the assembler validates the header of a completed CX16 executable image (“CPU16 Monitor: Executable File Format and Program Startup,” Section 3).

Section 17 describes the current implementations' command-line options, listing and memory-map formats, executable header validation, and related features.

## 12. Diagnostics

The assembler must report **every** error it finds, not just the first, identifying its file and line. If any error occurs, it must not produce an output image. When `.line` is in effect, the reported location must follow it (Section 9), but diagnostics must also identify the generated assembly line: an error in generated assembly often indicates a compiler defect and needs to be traced there. The current diagnostic format is specified in Section 17.6.

| Category | Diagnostics emitted by `cpu16asm` |
|---|---|
| Syntax | `unknown mnemonic 'LDD'`, `expected ',' or end of line`, `invalid operand syntax inside brackets`, `invalid name 'end.': '.' must be followed by a letter, digit or '_'` |
| Operands | `wrong number of operands: LD expects 2, got 1`, `invalid form [Rn++] (SP) for STB: byte instruction with SP or IP auto-increment/decrement (spec 6.3.5)` |
| Symbols | `undefined symbol 'foo'`, `duplicate definition of 'foo' (first defined on line 1)`, `.equ A: circular definition of 'A'` |
| Ranges | `value 300 does not fit in a byte`, `shift count must be 0..15 (got 16)` |
| Alignment | `branch target is odd ($0065)` (e.g. `JMP 101`, or branching to a label before `.byte`) |
| Memory image | `overlapping output at address $0200 (already used by line 5)`, `reserved area overlaps output at address $0200`, `output exceeds 64 KB address space` |
| Executable header (`--exec`) | `executable header: version is 2, expected 1`, `executable header: start $001D is odd`, `executable header: image_end $0200 exceeds the file length (300 bytes)`, `executable header: output above image_end $0200: 4 byte(s) up to the end of the file` |
| Directives | `.org cannot depend on labels or '$': value depends on label 'LBL'`, `.line: line number out of range 1..1000000` |
| Evaluation | `division by zero` |

## 13. Short Examples

### 13.1. Printing a String (Console Port $FF20; See Section 13.12)

```
        .equ    CONSOLE, $FF20
        .org    0
start:  LD      SP, #$FF00
        LD      R0, #msg
@next:  LDZX    R1, [R0++]
        CMP     R1, #0
        JZ      @done
        STB     [CONSOLE], R1
        JMP     @next
@done:  WFI
msg:    .asciz  "Hello!\n"
```

### 13.2. Summing an Array

```
        LD      R0, #table       ; pointer
        LD      R1, #5           ; element count
        LD      R2, #0           ; sum
loop:   ADD     R2, [R0++]       ; sum += *p++ (step: 2 bytes)
        DEC     R1
        JNZ     loop
        WFI
table:  .word   10, 20, 30, 40, 50
```

### 13.3. Subroutine Calls and Preserving LR

```
        LD      SP, #$FF00
        LD      R0, #7
        CALL    twice_plus_one
        WFI

twice_plus_one:
        PUSH    LR               ; nested call: save LR
        ADD     R0, R0
        CALL    inc_r0
        POP     LR
        RET

inc_r0: INC     R0
        RET                      ; leaf subroutine: no need to save LR
```

### 13.4. 32-bit Addition

```
        ; R1:R0 += R3:R2
        ADD     R0, R2
        ADC     R1, R3
```

### 13.5. Unsigned and Signed Comparisons

```
        CMP     R0, R1
        JB      below            ; R0 < R1, unsigned
        JL      less             ; R0 < R1, signed
        JAE     above_or_equal   ; unsigned >=
        JGE     greater_or_equal ; signed >=
```

### 13.6. Copying a String Byte by Byte

```
; R0 = source, R1 = destination
copy:   LDZX    R2, [R0++]
        STB     [R1++], R2
        CMP     R2, #0
        JNZ     copy
        RET
```

### 13.7. Jump Table

```
        ; R0 = index 0..2
        ADD     R0, R0           ; index * 2
        JMP     [R0+table]       ; load the target address from word table[R0]
table:  .word   case0, case1, case2
```

### 13.8. Spinlock Using XCHB

```
acquire:
        LD      R1, #1
@try:   XCHB    [lock], R1       ; R1 = previous value; lock = 1
        CMP     R1, #0
        JNZ     @try
        RET

release:
        LD      R1, #0
        STB     [lock], R1
        RET

lock:   .byte   0
```

### 13.9. Shifts and Flag Constants

```
        .equ    IF, $08
        SHL     R0, #4
        ROR     R2, #8           ; swap the two bytes
        OR      FLAGS, #IF       ; equivalent to STI (one word)
        AND     FLAGS, #$0F & ~IF ; equivalent to CLI: one word, also clears OF
        AND     FLAGS, #~IF      ; CLI without side effects: ~$08 = $FFF7, two words

        PUSH    FLAGS            ; critical section: preserve IF and OF
        CLI
        ; ...
        POP     FLAGS            ; restore all flags, including OF
```

### 13.10. Interrupts: Vectors and Handler

```
        .org    0
        LD      SP, #$FF00
        STI
idle:   WFI
        JMP     idle

irq0:   PUSH    R0               ; the handler must save registers itself
        LD      R0, [ticks]
        INC     R0
        ST      [ticks], R0
        POP     R0
        IRET                     ; restores FLAGS and IP
ticks:  .word   0
svc:    IRET                     ; software-call handler for TRAP #9

        .org    $FF00            ; vector table: 16 words
        .word   irq0, 0, 0, 0, 0, 0, 0, 0     ; vectors 0..7: external IRQs
        .word   0, svc, 0, 0, 0, 0, 0, 0      ; vector 8: ERRCMD; 9..15: software traps
```

### 13.11. Relative Addressing (Independent of Load Address)

```
        LEA     R0, IP+(msg-$-4) ; R0 = address of msg; $ is this instruction's address, +4 its size
        LD      R1, [IP+(val-$-4)]
        WFI
msg:    .asciz  "xy"             ; 3 bytes: location counter becomes odd
val:    .word   $BEEF            ; automatically aligned; val has an even address
```

### 13.12. Console Input

The console port at `$FF20` is a convention implemented by the emulator and debugger, **not part of the processor architecture**:

- **Writing a byte** outputs a character.
- **Reading a byte** consumes the next input byte, or returns 0 if no input is available. There is no blocking wait and no ready/status register.
- **Use byte-load instructions** (`LDZX`, `LDSX`) to read the port. A word read at `$FF20` also consumes one input byte (the low half of the word is read from the port).
- **An input byte with value 0 cannot be distinguished** from the absence of input.

The `cpu16` emulator reads input from standard input (`./cpu16 prog.bin < input.txt`); the browser debugger reads it from the console pane.

```
; echo: copy input to output until a period is received
loop:   LDZX    R1, [$FF20]      ; input byte, or 0
        CMP     R1, #0
        JZ      loop             ; no input available yet
        STB     [$FF20], R1
        CMP     R1, #'.'
        JNZ     loop
        WFI
```

### 13.13. Printing an Unsigned Integer in Decimal

```
        .equ    CONSOLE, $FF20
        .org    0
        LD      SP, #$FF00
        LD      R0, #40000
        CALL    print_dec        ; prints 40000
        LD      R1, #10
        STB     [CONSOLE], R1
        WFI

; R0 = unsigned value; R0, R1, and R2 are clobbered
print_dec:
        LD      R2, #0           ; number of digits pushed onto the stack
@div:   LD      R1, R0
        REMU    R1, #10          ; next digit, least significant first
        DIVU    R0, #10
        ADD     R1, #'0'
        PUSH    R1
        INC     R2
        CMP     R0, #0
        JNZ     @div
@out:   POP     R1               ; pop digits most significant first
        STB     [CONSOLE], R1
        DEC     R2
        JNZ     @out
        RET
```

## 14. Encoding Examples

The following instruction words may be used as reference values in tests. `d` denotes the instruction's second word.

| Assembly form | Word(s) |
|---|---|
| `NOP` (`LD R0, R0`) | `0000` |
| `LD R1, R2` | `0090` |
| `LD R3, #5` | `01A9` |
| `LD R3, #$1234` | `01F3`, `1234` |
| `ADD R0, [R1++]` | `400B` |
| `ST [R2+4], R0` | `2016`, `0004` |
| `STB [$FF20], R1` | `2885`, `FF20` |
| `XCHB [lock], R1` | `1885`, `lock` |
| `PUSH R0` | `206C` |
| `POP R0` | `006B` |
| `RET` | `0760` |
| `SHL R0, #4` | `9021` |
| `ROR R2, #8` | `A941` |
| `NEG R2` | `B100` |
| `JMP` forward by 3 words (JR) | `3003` |
| `JNZ` backward by 2 words (JR) | `317E` |
| `CALL` forward by 3 words (short) | `D019` |
| `JZ` long relative | `C8F7`, `disp` |
| `JMP #addr` (absolute) | `C873`, `addr` |
| `JMP [R0+tbl]` | `C806`, `tbl` |
| `CALL` long relative | `D077`, `disp` |
| `LEA R0, IP+d` | `0077`, `d` |
| `STI` (`OR FLAGS, #$08`) | `6FC1` |
| `CLI` (`AND FLAGS, #$07`) | `67B9` |
| `AND FLAGS, #$FFF7` (CLI without side effects) | `67F3`, `FFF7` |
| `STC` (`OR FLAGS, #$02`) | `6F91` |
| `CLC` (`AND FLAGS, #$0D`) | `67E9` |
| `CMC` (`XOR FLAGS, #$02`) | `7791` |
| `TEST FLAGS, #$08` | `8FC1` |
| `TRAP #9` | `D849` |
| `IRET` | `B018` |
| `WFI` | `B020` |
| `NOT R1` | `B088` |
| `BSX R3` | `B190` |
| `MUL R0, R1` | `E008` |
| `MULHU R2, #5` | `E929` |
| `DIVU R3, #1000` | `F1F3`, `03E8` |
| `REMU R4, [R5+2]` | `FA2E`, `0002` |

Opcodes (the five most significant bits of the instruction word):

| Code | Mnemonic | Code | Mnemonic |
|---|---|---|---|
| 0 | LD | 16 | CMP |
| 1 | LDZX | 17 | TEST |
| 2 | LDSX | 18 | SHL |
| 3 | XCHB | 19 | SHR |
| 4 | ST | 20 | SAR |
| 5 | STB | 21 | ROR |
| 6 | JR | | |
| 8 | ADD | 22 | MISC: NEG, NOT, BSX, IRET, WFI (subopcode in R2) |
| 9 | ADC | 23 | — |
| 10 | SUB | 24 | — |
| 11 | SBC | 25 | Jxx |
| 12 | AND | 26 | CALL |
| 13 | OR | 27 | TRAP |
| 14 | XOR | 28 | MUL |
| | | 29 | MULHU |
| | | 30 | DIVU |
| | | 31 | REMU |

## 15. Formal Grammar (EBNF)

```
file        = { line } ;
line        = [ label ] [ statement ] [ comment ] newline ;
label       = ( ident | local ) ":" ;
local       = "@" ident ;
comment     = ";" { any char } ;
statement   = instruction | directive ;

instruction = mnemonic [ operand { "," operand } ] ;
operand     = reg
            | "#" expr
            | "[" memref "]"
            | reg ( "+" | "-" ) expr
            | expr ;                      (* bare expression for Jxx and CALL only *)
memref      = reg [ "++" ]
            | "--" reg
            | reg ( "+" | "-" ) expr
            | expr ;

directive   = "." dname [ dargs ] ;
dargs       = expr { "," expr }                        (* .word, .byte, .org, .space, .reserve, .align *)
            | string { "," string }                    (* .ascii, .asciz *)
            | ident "," expr                           (* .equ *)
            | string                                   (* .include *)
            | [ [ string "," ] number ] ;              (* .line *)

expr        = term { ( "|" | "^" | "&" | "<<" | ">>" | "+" | "-" ) term } ;   (* precedence rules: Section 3 *)
term        = unary { ( "*" | "/" ) unary } ;
unary       = [ "+" | "-" | "~" ] primary ;
primary     = number | char | ident | local | "$" | "(" expr ")" | ( "lo" | "hi" ) "(" expr ")" ;

reg         = "R0".."R15" | "LR" | "SP" | "IP" | "FLAGS" ;
ident       = ( letter | "_" ) { idchar } { "." idchar { idchar } } ;   (* period only within a name *)
idchar      = letter | digit | "_" ;
dname       = ( letter | "_" ) { idchar } ;                         (* periods prohibited in directive names *)
```

## 16. Features Excluded from Version 1

Macros; conditional assembly; sections and object-file linking; output formats other than a flat binary image (Intel HEX may be added later); warnings (all violations are errors); and register-to-register symbolic expressions of the `R+R` form.

## 17. Implementations: cpu16asm and the Web Debugger Assembler

This section documents the current implementations of the language: the `cpu16asm` command-line assembler (`tools/cpu16asm.cpp`) and the web debugger assembler (`debugger/src/assembler.js`). Both accept the same language and produce byte-for-byte identical binary images, listings, memory maps, and diagnostics. This equivalence is checked against a corpus of assembly source files.

### 17.1. Running cpu16asm

```
cpu16asm [-o image.bin] [-l listing.lst] [-m map.map] [--base ADDRESS] [--fill BYTE] [--exec] source.asm
```

| Option | Meaning |
|---|---|
| `-o file` | Binary image (Section 17.2). By default, the source filename with its extension replaced by `.bin` (`prog.asm` → `prog.bin`); if the source name has no extension, `.bin` is appended. |
| `-l file` | Listing (Section 17.3); no listing is written unless requested. |
| `-m file` | Memory map (Section 17.4); no map is written unless requested. |
| `--base ADDRESS` | Starting address of the binary image, 0..65535; default: 0. |
| `--fill BYTE` | Fill byte for unused addresses within the image, 0..255; default: 0. |
| `--exec` | Validate the CX16 executable-file header (Section 17.5); requires `--base 0`. |
| `--no-line` | Ignore the source-location effects of `.line`. The directives are still validated, but diagnostics, memory maps, and listings refer to the assembly source itself. Useful when debugging a compiler. |
| `-h`, `--help` | Print brief help to stdout and exit with status 0. |

- Numeric option values may be written as `123`, `$FF`, or `0xFF`.
- Exactly one source file is accepted. Options and the source filename may appear in any order.
- Output files are written only after successful assembly and validation. On success, stderr receives a line such as `cpu16asm: prog.bin: 1096 bytes, addresses $0000..$0447` (or `program is empty`).

**Exit status:**

| Code | Meaning |
|---|---|
| 0 | Success, including `--help`. |
| 1 | Errors in the source, output below `--base`, invalid executable header with `--exec`, or failure to write an output file. |
| 2 | Invalid invocation: unknown option, missing option argument, invalid `--base` or `--fill` value, `--exec` with `--base` other than 0, no source file, or multiple source files. |

### 17.2. Binary Image

The output is a flat file: the byte at memory address `base + i` appears at file offset `i`. The file spans from `base` through the highest emitted address, inclusive; an empty program produces an empty file. Unused addresses within the image contain the `--fill` byte; `.reserve` regions contain zero bytes (Sections 9 and 11).

### 17.3. Listing

The listing is a text file with one entry for every line of assembly source, including lines from `.include` files at the point where the inclusion occurs, in parse order:

```
ADDRESS  DATA                   SOURCE LINE
```

- `ADDRESS` consists of four hexadecimal digits followed by two spaces. For lines without an address (comments, standalone labels, `.equ`, `.line`), six spaces are used instead.
- The `DATA` column is padded to a width of 22 characters:
  - Instructions and `.word` values are displayed as four-digit hexadecimal words, up to four words per line. Any additional words appear on continuation lines with addresses but no source text.
  - `.byte`, `.ascii`, `.asciz`, `.space`, and `.align` values are displayed as two-digit hexadecimal bytes, up to eight bytes per line and no more than four lines. Remaining bytes are represented by `... (N bytes)`.
  - `.equ` is shown as `= $XXXX` when resolved, or `= ?` if its value is not yet known.
  - `.reserve` is shown as `(N bytes reserved)`.
  - An implicit alignment byte is shown on its own line as `00`, with the text `; alignment`, immediately before the instruction or `.word` directive that requires it.
- **Source-location column.** When the source contains `.line` and `--no-line` is not set, an additional location column appears between the data and the source line. Its width is determined by the longest displayed location, followed by two spaces. The column is empty for lines that use their own actual locations, for `.line` directives themselves, and for data continuation lines. `.line` directive lines are printed verbatim.

```
                                                   ; generated by the compiler
                                                     .line "kernel.b16", 120
0000  0009                   kernel.b16:120  kmain:  LD R0, #1
0002  0091                   kernel.b16:120          LD R1, #2
                                                     .line 121
0004  0760                   kernel.b16:121          RET
```

```
0000  06F3 FF00              start:  LD      SP, #$FF00
      = $FF20                        .equ    CON, $FF20
0004  48 69 00               msg:    .asciz  "Hi"
0007  00                     ; alignment
0008  0001 0002              tbl:    .word   1, 2
```

### 17.4. Memory Map

The memory map is a text file used to account for memory consumption and to associate addresses (for example, addresses from an emulator trace) with program objects. It is written only after successful assembly. Addresses are formatted as `$XXXX`; sizes are decimal byte counts. Its sections are:

- **Summary:** Address range containing allocated bytes (`occupied`), the output-file range and size (`file`, starting at `--base`), and byte counts by category: `code`, `data`, `bss`, and `padding`. `gaps` counts unused addresses within the file, filled with `--fill`. `total` is the sum of the allocated categories.
- **By source file:** The same categories, grouped by source file. An `.include` file is counted separately; repeated inclusions of the same file share one entry.
- **Objects by address:** Objects in ascending address order, showing the first address, last address, size, type, name, and definition location (`file:line`).
- **Objects by size:** Nonempty top-level objects sorted by decreasing size, with each object's percentage of `total` (excluding alignment).
- **Constants:** `.equ` constants, showing value, name, and definition location. This section is omitted when no constants are defined.

Rules for the object sections:

- **Object:** A global label defines an object spanning from the label's address to the next global label, but no farther than the end of the contiguous allocated region. A gap introduced by `.org` terminates the object. Allocated bytes without a preceding global label (for example, code at the start of a program or immediately after `.org`) belong to an object named `(no label)`.
- **Multiple labels at the same address:** Every label except the last defines a zero-size object; the last label owns the allocated region. A label at an address where no bytes are emitted (for example, `image_end:` at the end of the source) also defines a zero-size object. Such objects have no printed end address.
- **Alignment at an object's end:** The implicit alignment byte (Section 9) and `.align` padding bytes are listed separately as a `(padding)` entry of type `pad`. The object's size includes only its own bytes.
- **Local labels:** These appear as indented entries within their containing object, up to the next label or the end of the object. A local label is displayed as `@name` if it belongs to the object; otherwise its full `scope@name` form is shown.
- **Type:** Indicates which categories of bytes occur in an object: `code` (instructions), `data` (`.word`, `.byte`, `.ascii`, `.asciz`, `.space`), and `bss` (`.reserve`). Mixed objects use `+` between type names, for example `code+data`. `pad` identifies padding only; `-` identifies a zero-size object.

Example (excerpt):

```
== Summary ==
occupied  $0000..$0047
file      $0000..$0042   67 bytes
code          12 bytes
data           8 bytes
bss            5 bytes   .reserve
padding        7 bytes   alignment
gaps          40 bytes   unused addresses inside the file
total         32 bytes   code + data + bss + padding

== Objects by address ==
start  end      size  type       name        source
$0000  $0001       2  code       (no label)  prog.asm:3
$0002  $0009       8  code       main        prog.asm:4
$0004  $0007       4  code         @lp       prog.asm:5
$000A  $000C       3  data       msg         prog.asm:8
$000D  $000D       1  pad        (padding)   prog.asm:9
$000E  $0011       4  data       tbl         prog.asm:9
$0043              0  -          end         prog.asm:15
$0043  $0047       5  bss        buf         prog.asm:16
```

Object source locations and the **By source file** section honor `.line`. For compiler-generated assembly, they identify locations in the original source language instead of the generated assembly file.

### 17.5. Executable-File Header Validation

The `--exec` option validates the completed image against the CX16 executable-file format (“CPU16 Monitor: Executable File Format and Program Startup,” Revision 4, Sections 3 and 10). The header must be written explicitly in the assembly source; the assembler only validates the resulting image. The conditions from Section 3.3 of that specification are checked:

- The word at `$0000` equals `$C873` (`JMP #start`), and bytes `$0004–$0007` equal `"CX16"`.
- `version` equals 1.
- `$001C` ≤ `image_end` ≤ `bss_end` ≤ `$E000`.
- `start` is even and `$001C` ≤ `start` < `image_end`.
- `image_end` does not exceed the emitted file length.
- **No emitted bytes occur after `image_end`**—no instructions, data (`.byte`, `.word`, `.ascii`, `.space`), or alignment bytes. `.reserve` regions beyond `image_end` are allowed: these are BSS and are not included in the output file.

The last rule catches a common build error: code inadvertently placed after `image_end`. A typical example is an assembly module included with `.include` after the B16 compiler's generated output. The module is assembled after `__bss_end`. On a bare machine, the emulator loads the entire file, so the program appears to work; Monitor, however, loads only bytes up to `image_end`, so a call into the module reaches zero-filled memory. The diagnostic states how many extra output bytes were found and the address of the first emitted byte beyond `image_end`. Reserved bytes in the middle of the file do not count:

```text
prog.asm: error: executable header: output above image_end $05B2: 52 byte(s) up to the end of the file, first output byte at $05C8
```

The CX16 format itself permits **additional data** after `image_end` in the file (Section 3.1 of the format specification), such as debugging information or other metadata not required by the running program. The loader ignores these bytes. The assembler cannot emit this appended data directly from its source file: it must be appended to the completed image in a separate step, using another tool or a future assembler option, **after** `--exec` validation. The assembled image ends exactly at `image_end`, so appended data begins at that file offset.

The `flags`, reserved words, and `min_free` fields are not validated. Violations are reported as `file: error: executable header: message`, and no output files are written. This option requires `--base 0`.

### 17.6. Diagnostics

Errors are written to stderr, one per line, in discovery order:

```
file:line: error: message
file:line (asm-file:asm-line): error: message
```

- The second form applies to lines controlled by `.line`: it reports the mapped `.line` location followed by the actual assembly-source location in parentheses, for example `kernel.b16:120 (kernel.asm:345): error: undefined symbol 'x'`. With `--no-line`, the first form is always used with actual assembly-source locations.
- References to another location use `(first defined on line N)` if it is in the same file, or `(first defined at file:line)` otherwise. The same rule applies to overlap diagnostics (`already used by`, `reserved by`).
- If no line is available (for example, when a file cannot be opened), the format is `file: error: message`; errors from `--exec` use `file: error: executable header: …`.
- A final line reads `cpu16asm: N error(s)`; the exit status is 1.
- Diagnostic message text is in English; Section 12 explains the error categories.

### 17.7. Implementation Limits

| Item | Limit |
|---|---|
| `.include` nesting depth | 16 |
| Line number in `.line` | 1..1000000 |
| Numeric values in expressions | Up to 32 bits unsigned in source notation; evaluation uses at least 32-bit signed arithmetic (Section 3). |

### 17.8. Web Debugger Assembler

- The language, binary image, listing, memory map, and diagnostic text match those of `cpu16asm`.
- There is no filesystem: `.include` can access only the built-in `stdlib.asm` library.
- The image base address is 0, and the fill byte is 0.
- Diagnostics appear in the Build tab using the same format as `cpu16asm`. Clicking a diagnostic moves the cursor to the corresponding assembly-source line and highlights that line in the editor. The `--no-line` option is not exposed (the engine provides the `ignoreLine` property instead).
- The engine implements `--exec` header validation, but the option is not yet exposed in the user interface.
- The **Download .zip** button exports `.asm`, `.bin`, `.lst`, and `.map` in the same formats. **Open .map** loads either the current memory-map format (labels from the objects section, local labels as `object@name`, constants from the constants section) or the earlier map format.
