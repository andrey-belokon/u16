# B16: Programmer's Guide

This guide is intended for programmers who are comfortable with C and are not afraid of assembly language. Rather than restating the language specification, it explains *how to think in B16*: what each construct compiles into, and how to write compact, fast code for CPU16. For exact language rules and compiler options, see [`B16_reference_en.md`](B16_reference_en.md); for a syntax overview, see the project [`README_en.md`](../README_en.md). The processor architecture is documented in [`cpu16_spec_r6_en.md`](../../cpu16/docs/cpu16_spec_r6_en.md), and its devices in [`cpu16_machine_en.md`](../../cpu16/docs/cpu16_machine_en.md).

This guide covers the language features described in the current Revision 3 specification, including the `long` and `ulong` types. Section 14 of the language reference records changes across revisions.

All assembly listings below are actual `b16c` output, with `.line` directives omitted for clarity (see Section 12).

## 1. The Design of B16

B16 was designed to keep the underlying machine visible. Each statement should compile into a short, predictable sequence of instructions, and expensive operations should look expensive. It draws primarily on two languages: B, in which nearly everything is a word and types rarely get in the way, and C, with its pointers, structures, and expression syntax.

Several principles follow from this design and are worth keeping in mind from the outset.

**The machine word is the unit of computation.** Ordinary expressions operate on 16-bit words. `byte` and `char` describe how values are stored in memory: a byte is extended to a word on load (with zeros or the sign bit) and truncated on store. There are no C-style integer promotions at the word level, because there is no wider ordinary word type to promote to.

**Signedness matters only where the hardware distinguishes it.** Addition, subtraction, the low word of multiplication, bitwise operations, and `<<` have the same behavior for signed and unsigned operands. Relational comparisons (`< > <= >=`), division, remainder, and `>>` do not. B16 requires matching signedness precisely for those operations. That one check replaces C's complex *usual arithmetic conversions* rules and catches meaningful mistakes.

**A variable stays in a register unless something prevents it.** Local scalar variables are allocated to registers by default. Taking their address, declaring an array or a structure, or running out of registers forces them into memory.

**The compiler does not hide function calls.** The only implicit calls are for signed division and remainder by a variable (`__divs` and `__mods`), as explicitly documented. Division by a constant is implemented inline. There is no implicit structure copying, no constructors, no implicit conversions, and no heap management.

## 2. Quick Start

```
make                                      # build b16c
./run.sh examples/universal/hello.b16     # compile, assemble, and run in the emulator
./run.sh examples/bare/screen.b16 --screen  # options after the source file go to cpu16
./run.sh examples/universal/echo.b16 < examples/universal/echo.txt
```

`run.sh` runs `b16c -I lib`, then `cpu16asm`, then `cpu16`. Pass additional compiler options through `B16FLAGS`, for example `B16FLAGS=-s ./run.sh prog.b16`. The assembler and emulator are located using the `CPU16ASM` and `CPU16` environment variables, or through `PATH`. The source directory receives a `.s` file (compiler output), a `.bin` file, and a `.lst` assembler listing. Keep the listing open while learning the code generator: it shows both addresses and encoded instruction words.

Here is a minimal program. The `io.b16` library provides output that works both on bare hardware and under Monitor:

```
include "io.b16"

fn main() {
    puts("Hello, CPU16!\n")
}
```

The options used most often are `-o file`, `-I directory`, and `-s` (include source text in the generated output; see Section 12). Configuration that belongs to the program itself—target, code and data addresses, CX16 header fields, and assembly modules—is expressed in the source with `option` declarations and `include asm` lines (see *Image Options* below). This means every program can be built with the same command. The complete options, defaults, and target-specific behavior are documented in Section 13 of `B16_reference_en.md`.

Diagnostics use the forms `file:line: error: message` and `file:line: warning: message`. After an error, the compiler resumes at the next statement or declaration and reports up to 20 errors per invocation.

### Executable Image Layout

A program is packaged as a CX16 executable (see *CPU16 Monitor: Executable File Format and Program Startup*). The same image can run under Monitor or in the bare-machine emulator. Its layout, in address order, is:

1. The 28-byte CX16 header (`__header`): `JMP #__start`, the `"CX16"` signature, version, `image_end`, `bss_end`, `min_free`, and `max_top`.
2. Startup code at `__start`, the halt routine `__halt`, and the empty interrupt handler `__irq_ignore`.
3. Functions in source order (included files are inserted at the point of `include`).
4. The division runtime, if required.
5. Initialized global variables and string literals.
6. `.align 2`, the `__image_end` label, the uninitialized globals in a `.reserve` region, and the `__bss_end` label. Reserved space is not stored in the executable file.

Startup code selects its execution environment by inspecting the signature at `$0004`. Under Monitor, that address contains the PIB (`"CXPB"`). Monitor owns the stack, interrupt vectors, and timer; it has already zeroed BSS and passed `argc` and `argv` in R0 and R1. Startup saves LR, calls `main`, and returns through LR with the exit status in R0. On a bare machine, `$0004` contains the program's own `"CX16"` header, and startup initializes the environment itself:

- Sets `SP = $FF00`.
- Installs `__fault` as the ERRCMD vector and `__irq_ignore` (`IRET`) as the other 15 vectors. The program can replace these placeholders with working handlers.
- Clears BSS, because memory contents after reset are undefined.
- Calls `main` with `argc = 0`, `argv = 0`. When it returns, `__halt` disables the timer, acknowledges any pending interrupt request using `STI`, `CLI`, and stops the processor with `WFI`.

`main` may accept command-line arguments and return an exit status:

```
fn main(argc: uint, argv: ptr ptr byte): int {
    var i: uint = 1
    while i < argc { puts(argv[i]); putnl(); i++ }
    return 0
}
```

Supported forms include `fn main()`, `fn main(): int`, `fn main(argc: uint, argv: ptr ptr byte)`, and the argument-taking form with an `int` return type. If `main` returns no value, Monitor receives exit status zero.

The stack grows down from its top address (`top` under Monitor, `$FF00` on a bare machine) toward the end of BSS. A program can locate the image boundaries through `__image_end` and `__bss_end`, both of type `ptr`; `__bss_end` is a convenient starting point for a heap.

**Separate code and data.** With `option data = $8000`, data begins at `$8000`. Initialized globals are also stored next to the code in a template named `__data_init`; startup copies that template into RAM. This allows the program to run from ROM. String literals remain alongside the code and are not copied, so they must not be modified.

Only reachable functions are emitted: those reachable from `main`, functions marked `export`, interrupt handlers, functions referred to by global initializers, and functions referenced from `asm` blocks. You can therefore include an entire library without pulling unused functions into the executable.

## 3. Syntax for C Programmers

The syntax deliberately resembles C. The table below lists the main differences.

| Construct | C | B16 |
|---|---|---|
| Declaration | `int x = 5;` | `var x: int = 5` or `var x = 5` |
| Function | `int f(int a, int b)` | `fn f(a, b: int): int` |
| Multiple results | — | `fn divmod(a, b: uint): uint, uint` |
| Pointer | `char *p` | `p: ptr byte` |
| Array | `int a[10]` | `a: [10]int` |
| Function pointer | `int (*f)(int)` | `f: ptr fn(int): int` |
| Cast | `(unsigned)x` | `uint(x)` |
| Field through a pointer | `p->x` | `p.x` (also works for `(*p).x`) |
| Infinite loop | `for (;;)` | `loop { }` |
| Braces around `if`/`while` bodies | Optional | Required |
| Parentheses around a condition | Required | Not needed: `if x < 5 { }` |
| `;` | Required | Optional |

Because control-flow bodies always use braces, there is no dangling-`else` ambiguity. Parentheses around conditions are optional, so constructs such as `if (a) (b)` cannot introduce the same ambiguities as in C.

### Semicolons and Line Breaks

As in Go, a line break terminates a statement if the line ends with something that could complete an expression:

- An identifier, number, or string.
- `)`, `]`, or `}`.
- `++` or `--`.
- `break`, `continue`, or `return`.
- A type name.

This has two practical consequences:

```
x = a +         // valid: the line ends with an operator
    b
x = a           // not what you want: "- b" becomes a separate statement;
    - b         // its result is evaluated and discarded
```

An `else` may appear on the line after `}`; this is the sole exception to the rule. You need a semicolon only to put multiple statements on one line, as in `i++; j--`.

### Literals

- Numbers: `255`, `$FF`, `0xFF`, `0b1111`.
- Characters: `'A'`, `'\n'`, `'Ж'`.
- Strings: `"текст"`.

Source files use UTF-8, but literals are converted into the machine's character encoding. That encoding covers ASCII, Cyrillic `А–я` and `ёЁ`, box-drawing characters (`╔═╗░▒▓█`), and symbols (`☺ ♥ → ▲`). Thus `'Ж'` has the value `$C6`, while `"╔═╗"` occupies three bytes. A character unavailable in the machine encoding—such as `€`, `«`, or `№`—causes a compilation error. The escape sequences `\n \r \t \0 \\ \' \" \xHH` specify bytes directly.

A string literal is a zero-terminated byte array in the data area. In an expression it evaluates to the address of that array.

### Constants

```
const N = 64
const MASK = N - 1
const IRQ_TIMER = 0
```

`const` exists only at compile time. Its value is substituted and folded with other constants, so `MASK` costs exactly as much as writing the literal `63`. Constants from 0 to 15 are particularly cheap: they fit directly into an instruction (Section 5).

## 4. Types and Their Costs

| Type | Size (bytes) | Value in a register | Memory load | Store |
|---|---|---|---|---|
| `byte` | 1 | 0..255 | `LDZX` | `STB` |
| `char` | 1 | −128..127 | `LDSX` | `STB` |
| `uint` | 2 | 0..65535 | `LD` | `ST` |
| `int` | 2 | −32768..32767 | `LD` | `ST` |
| `ulong` | 4 | 0..4294967295 | `LD` ×2 | `ST` ×2 |
| `long` | 4 | −2147483648..2147483647 | `LD` ×2 | `ST` ×2 |
| `ptr T` | 2 | address | `LD` | `ST` |
| `[N]T` | N·size(T) | — | — | — |
| `struct` | sum of fields, including padding | — | — | — |

Here `char` means a *signed byte*, not necessarily a textual character. For text, `byte` is often more convenient: Cyrillic letters occupy `$C0–$FF` in the machine encoding, so using signed `char` would make their numeric values negative. `ptr byte` serves as a general-purpose typed pointer, much like `char *` in C.

### Byte Variables in Registers

A register-resident `byte` always contains a zero-extended value in the range 0..255. When an assignment writes to it, one single-word `LDZX Rd, Rs` instruction truncates the value (or `LDSX` for `char`):

```
var b: byte
b = x + y        // ADD ...; then LDZX Rb, Rb
```

If the right-hand side already has type `byte`—because it was loaded from a byte array, produced by `byte(...)`, or is a constant in 0..255—no additional truncation is needed.

### Signedness in Mixed Expressions

```
var i: int
var u: uint
i + u            // allowed: addition does not depend on signedness
i == u           // allowed
i < u            // error: signed and unsigned operands mixed
i < int(u)       // correct: explicit cast
i / 3            // allowed: the literal takes the other operand's signedness
```

Untyped constants adopt the type of the other operand, so comparisons against literals normally need no casts. In mixed operations, the result takes the left operand's type (after promoting `byte` to `uint` or `char` to `int`); if either operand is `long` or `ulong`, the result uses that wider type. For `>>`, the left operand alone determines signedness: `x >> n` emits `SAR` for `int` and `SHR` for `uint`.

### Casts

| Expression | Generated code |
|---|---|
| `int(x)`, `uint(x)`, `ptr T(x)` | None: only the type changes |
| `byte(x)` | `LDZX Rd, Rs` (one word) |
| `char(x)` | `LDSX Rd, Rs` (one word) |
| `long(x)`, `ulong(x)` | `LD Rhi, #0` from `uint`/`byte`; `LD` + `SAR Rhi, #15` from `int`/`char`; no code when casting between `ulong` and `long` |
| `uint(x)`, `int(x)` from `long`/`ulong` | Keep the low word: `LD` or no code |

Constant casts are evaluated at compile time. A constant that does not fit the destination type is an error.

### Double-Word Values: `long` and `ulong`

`long` and `ulong` are signed and unsigned 32-bit types. In memory, the low word is stored at the lower address, as in the 32-bit fields of FS16. In registers, a value occupies a **register pair**; the registers need not be adjacent, and the compiler chooses the pair. Arithmetic is implemented with instruction sequences that propagate carries:

```
var a, b: long
a = a + b           // ADD Ra, Rb ; ADC Ra', Rb'
if a < b { ... }    // CMP Ra, Rb ; SBC Ra', Rb' ; JGE (flags represent the entire value)
```

**Constants** are evaluated exactly and take their type from the context. A value that cannot fit is an error, not a silent truncation:

```
var n = 100000       // long: too large for int or uint
var u: uint = 70000  // error: constant 70000 does not fit in uint
var m: ulong = $FFFFFFFF
var k: long = $FFFFFFFF   // -1: the bit pattern is preserved
```

**Widening is implicit; narrowing is explicit.** A narrower operand is extended according to its own signedness, and the result has the wider type. Assigning a `long` to `uint` or `int` requires a cast:

```
var l: long
var u: uint
l = u + l             // u is zero-extended; result is long
u = uint(l)           // low word only; without the cast this is an error
l < u                 // allowed: uint fits in long without loss
ul < i                // error: ulong and int mix signedness
```

The size and signedness of `*`, `/`, `%`, and comparisons follow the same rules as word-sized values. `+ - * & | ^` do not depend on signedness, so `ulong + int` is allowed.

**Access either half at no arithmetic cost.** `uint(x)` obtains the low word, `uint(x >> 16)` the high word, and `(ulong(h) << 16) | l` combines two words. These operations only read or write registers and memory words; no arithmetic is required. Multiplying two word values as `ulong(a) * ulong(b)` uses `MUL` and `MULHU`, requiring six instructions and no function call.

**Arguments and results.** A `long` or `ulong` occupies two argument slots. For `fn f(a: long, b: uint): long`, `a` arrives in R0:R1, `b` in R2, and the result is returned in R0:R1. If an argument cannot fit entirely into the R0–R3 argument slots, it and all subsequent arguments go on the stack. A function may return no more than four words in total.

**Expensive operations.** Addition, subtraction, bitwise operations, comparisons, shifts by constants, and `++` take two to four instructions on 32-bit values. Multiplication and division cost more:

| Operation | Implementation | Instructions |
|---|---|---|
| `a * b` | Call `__mul32` | 16 |
| `ulong(x) * ulong(y)`, `x * 2^k` | `MUL` + `MULHU`, or a shift | 6 or fewer |
| `a / b`, `a % b` | Call `__divu32`, `__divs32`, `__modu32`, or `__mods32` | ≈390 (20 if both halves are below 65536) |
| `a / k`, `a % k` for `ulong`, k = 3…255 | Call `__udivc32` or `__umodc32` | ≈45 |
| `a / 2^k`, `a % 2^k` for `ulong` | Shift and mask | 1–5 |
| `a << n`, `a >> n` with a variable `n` | Call `__shl32`, `__shr32`, or `__sar32` | ≈17 |

Unsigned 32-bit division by zero returns `$FFFFFFFF` and leaves the dividend as the remainder. For signed `long`, `-2147483648 / -1` returns `-2147483648`, with remainder zero. A variable shift count is taken modulo 32. Shifting by a constant of 32 or more produces zero (or the sign bits for `long >>`) and emits a warning.

**Complex `long` expressions are register-limited.** In a function that performs 32-bit arithmetic, no more than six registers are allocated to register-resident variables; the remaining registers are reserved for temporaries. If an expression is too deeply nested, the compiler reports `expression too complex`. Split it into separate statements.

**Access to `long` is not atomic.** Reading or writing a 32-bit value requires two instructions. If an interrupt handler also accesses the value, protect both reads and writes with `atomic`.

### Strict Pointer Type Checking

Getting a pointer type wrong changes the increment performed by `++` and the load instruction selected by the compiler. B16 therefore rejects pointer type mismatches rather than silently accepting them:

- A `ptr T` may be assigned only to a `ptr T` with the same `T`, whether in assignment, initialization, argument passing, or return. `ptr byte` and `ptr char` are different types, as are `ptr [4]int` and `ptr int`.
- An array of type `[N]T` decays to `ptr T`; a string literal has type `ptr byte`.
- An integer cannot be assigned to a pointer, nor a pointer to an integer. The exception is the integer constant zero, which can initialize or be assigned to any pointer.
- Pointers may be compared only when their types match (or one is untyped); comparison with zero is always allowed.
- Other conversions require explicit casts, which generate no code: `ptr T(x)` from any pointer or integer, and `uint(p)` or `int(p)` from a pointer.

```
var CONSOLE at $FF20: byte
var p: ptr byte = &CONSOLE        // the address of an at-variable is already ptr byte
var q: ptr uint = ptr uint($8000) // converting an integer to a pointer requires a cast
var a: uint = uint(q) & 1         // alignment check
```

**The untyped pointer `ptr`** holds the address of data with an unknown element type. It is intended for functions that manipulate memory generically. Any `ptr T` other than a function pointer can be assigned to `ptr` and back without a cast. But operations that require an element size—`*p`, `p[i]`, `p.f`, `++`, or `+ i`—are not allowed on an untyped pointer. Cast it to `ptr byte`, or assign it to a typed pointer, before performing address arithmetic:

```
fn checksum(buf: ptr, n: uint): uint {
    var p: ptr uint = buf          // implicit conversion: ptr -> ptr uint
    var s: uint = 0
    while n > 0 { s += *p; p++; n -= 2 }
    return s
}
```

A line break after `ptr` ends a statement, just as it does after `int`. The pointee type therefore cannot be moved onto the next line.

Pointer arithmetic is scaled by element size, as in C: `p + 1` advances a `ptr int` by two bytes. The difference between two pointers is measured in elements.

### Arrays

`[N]T` is a contiguous block of elements. In an expression, an array name denotes the address of its first element. Multidimensional arrays are arrays of arrays: `[3][5]int` is accessed as `a[i][j]`.

Arrays cannot be copied or passed by value. Functions receive a pointer instead:

```
fn sum(p: ptr int, n: uint): int
...
sum(&data[0], 16)        // or simply sum(data, 16)
```

### Structures

```
struct Node {
    next: ptr Node       // self-references are allowed
    key: int
    tag: byte
}
```

`uint`, `int`, and `ptr` fields are word-aligned; byte fields are not. The total size is rounded up to an even number of bytes. Here `sizeof(Node) = 6`: `next` is at offset 0, `key` at 2, `tag` at 4, followed by one padding byte. Field order affects size, so group byte fields together when practical.

A structure defines a memory layout, not a value that can be copied wholesale. Assigning, comparing, or passing entire structures is unsupported; copying must be explicit (for example, with `memcpy`). In return, field access is straightforward: offset zero uses `[Rp]` (one instruction word), while a nonzero offset uses `[Rp+offset]` (two words).

```
fn f1(p: ptr P, d: int) { p.x += d; p.y = g }
```
```
f1:
	LD    R2, [R0]
	ADD   R2, R1
	ST    [R0], R2
	LD    R2, [g]
	ST    [R0+2], R2
	RET
```

Structures may contain nested structures, arrays, and function pointers, supporting expressions such as `r.b.y`, `pr.name[2]`, and `pr.f(x)`.

## 5. The Cost Model

CPU16 has a two-operand instruction set with a versatile second operand. Almost every ALU instruction accepts any of eight addressing modes for that operand. The B16 code generator is built around this feature: if the right-hand side of a binary operation fits an addressing mode, it is encoded directly in the instruction, without an intermediate load.

| Right-hand side | Addressing mode | Instruction words |
|---|---|---|
| Register-allocated variable | `Rn` | 1 |
| Constant 0–15 | `#k` | 1 |
| Other constant or address of a global | `#imm` | 2 |
| Global word variable | `[g]` | 2 |
| Stack-resident local | `[SP+d]` | 2 (1 when d = 0) |
| `*p` | `[Rp]` | 1 |
| `*p++`, `*--p` | `[Rp++]`, `[--Rp]` | 1 |
| `p.f` | `[Rp+off]` | 2 |
| `buf[i]` (global byte array) | `[Ri+buf]` | 2 |
| `&x` (address on the stack) | `SP+d` | 2 |

This leads to some practical conclusions:

- Accessing a register-allocated variable incurs no extra instruction words; accessing a global costs two words per operation. Copy a frequently used global into a local before a hot loop.
- Constants from 0 through 15 take half as much instruction space as other constants. `x - 3` and `x + (-3)` generate the same `SUB Rx, #3` instruction.
- A byte in memory cannot be used directly as an ALU operand: it must first be loaded with `LDZX`, adding an instruction.
- Comparing against a variable in memory takes only one instruction: `CMP R4, [ticks]`.

### Flags and Conditions

A condition compiles directly to `CMP` or `TEST` followed by a branch; the compiler does not first compute a Boolean 0 or 1. It remembers which register the flags describe after the previous ALU instruction and avoids repeating the test:

```
fn f2(a: ptr int, n: uint): int { var s = 0; while n != 0 { s += *a++; n-- }; return s }
```
```
f2:
	LD    R2, #0
	TEST  R1, R1
	JZ    @L4
@L2:
	ADD   R2, [R0++]
	SUB   R1, #1
	JNZ   @L2           ; flags already set by SUB; no TEST needed
@L4:
	LD    R0, R2
	RET
```

The loop body consists of just three instructions. In a condition, `x & M` becomes a single `TEST`; `if x & 4 { }` becomes `TEST R0, #4` followed by `JZ`.

A `while` condition is checked before entering the loop and then at the bottom (a *rotated loop*). Each iteration executes only one conditional branch.

A comparison **used as a value** (`x = a < b`) costs more: load 0, perform the comparison, then conditionally load 1. If the result is needed only for branching, write the comparison directly in an `if`.

### Multiplication, Division, and Shifts

| Operation | Generated code |
|---|---|
| `a * b` | `MUL` (low word, identical for `int` and `uint`) |
| `mulhu(a, b)` | `MULHU` (high word of the unsigned product) |
| `a / b`, `a % b` with `uint` | `DIVU`, `REMU` |
| `a / b`, `a % b` with `int` | Calls to `__divs`, `__mods` (approximately 15 instructions; makes the function non-leaf) |
| `x * 2^k` | `SHL` (with `NEG` for a negative exponent) |
| `x / 2^k`, `x % 2^k` with `uint` | `SHR`, `AND` |
| `x / 2^k`, `x % 2^k` with `int` | `SAR` with a correction for a negative dividend (5 instructions) |
| `x / k`, `x % k` with `int`, other constants | Reciprocal multiplication via `MULHU` (approximately 11 instructions, no call) |
| `x << k`, `x >> k` | `SHL`, `SHR`/`SAR` |

Signed division by a variable is the only operation that implicitly calls a function. Division by a constant needs no call and does not turn a leaf function into a non-leaf function. If the dividend is known to be nonnegative, cast the operands to `uint`: `uint(a) / 10` compiles to a single `DIVU` instruction. For a negative `int`, right-shifting rounds toward negative infinity, while `/` rounds toward zero.

A variable shift count uses only its low four bits, as dictated by the hardware; a shift by 16 cannot be expressed as a single instruction. The compiler constant-folds a shift by a constant of 16 or more to zero (or to the sign for `int >>`).

Division by zero does not raise an exception. `DIVU` yields `$FFFF`, while `REMU` returns the dividend. The language does not define the result of signed division by zero (in practice, 1 or −1). Signed `int` overflow silently wraps modulo 2^16.

### Evaluation Order

As in C, the order of operand evaluation is unspecified. The compiler usually evaluates function calls and the more expensive expression subtree first to minimize register pressure. Do not write `a[i] = i++` or depend on the order of side effects within one expression. `&&` and `||` evaluate left to right with short-circuiting, as in C.

## 6. Variables and Registers

### Where a Local Variable Lives

Registers are assigned to local scalars (and parameters) according to a weighted count of accesses: an access in a loop counts four times as much, and one in a nested loop counts sixteen times as much. Other rules depend on the function.

**Leaf function** (makes no calls). Parameters remain in their incoming registers (R0–R3). Other variables use free registers starting with R0; up to nine variables may reside in registers. At least three registers are reserved for temporary values. If R0–R3 suffice, the function needs neither a prologue nor an epilogue beyond `RET`.

**Non-leaf function.** Register-resident locals use R4–R11, which are preserved across calls. Parameters that must survive a call are copied into these registers in the prologue. A parameter used only before the first statement capable of clobbering R0–R3 may stay in its incoming register; `-fcopy-params` disables this optimization (see `B16_reference_en.md`, Section 13.6). The prologue saves LR and any callee-saved registers it uses; the epilogue restores them and returns with `POP IP`.

The following variables go **on the stack** (`[SP+d]`):

- Variables whose address is taken (`&x`).
- Local arrays and structures.
- Variables for which no register is available.

The stack frame is allocated with a single `SUB SP, #size`. There is no frame pointer: the compiler knows the SP offset statically at every point in the function.

### When Registers Run Out

Temporary values use registers not occupied by variables. In a non-leaf function with eight register variables, all of R4–R11 are occupied, leaving only R0–R3 for intermediate results. An expression such as `(*cur)[y][x]` needs two registers (base and index); a complex formula with nested comparisons and subscripts can need more.

The compiler first tries to generate each binary operation, comparison, subscript, and argument list in the ordinary way. If it runs out of registers, it rolls back that part of code generation and tries again: it evaluates the right operand first, parks it on the stack with `PUSH` while evaluating the left operand, and retrieves it with `POP`. The resulting code is longer and slower, but correct. The diagnostic `expression too complex: out of registers` remains only for pathological cases.

If extra `PUSH`/`POP` instructions show up in a hot loop (inspect the output of `b16c -s`), try the following:

- **Use fewer variables per function.** A temporary used to swap values (`var t = cur; cur = nxt; nxt = t`) occupies a register for the entire function. Consider moving the inner loop into a separate function.
- **Introduce an intermediate pointer.** Compute `var p: ptr byte = &(*cur)[y][0]` before the inner loop, then use `*p++` in its body. Instead of recomputing the row address on every iteration, the compiler can use one auto-increment instruction.
- **Split a formula into statements** with an intermediate variable. Temporary expression values do not persist between statements.

Two further rules of thumb:

- Do not take the address of a frequently accessed variable. A single `&x` anywhere in the function forces `x` into memory **throughout** that function.
- Do not declare spare variables unnecessarily. There is no live-range analysis: every variable occupies its own register for the entire function, even if it is used on only two lines. Reuse temporaries manually or split the function.

### Global Variables

A global variable always lives in memory, and each access generates an instruction referring to `[g]`. The compiler never keeps a copy of a global in a register across statements. This has a useful consequence: globals effectively behave like `volatile` variables in C. A variable modified by an interrupt handler can be polled without additional annotations:

```
while ticks < 60 { wfi() }      // each access reads memory again
```

Global initializers may contain constants, strings, addresses of globals, and function names. Arrays and structures additionally accept positional and designated initializer lists:

```
var count: uint = 10
var msg = "текст"                       // [6]byte: the bytes live directly in the variable
var p: ptr byte = "текст"               // pointer to a separate string
var tbl: [4]int = {1, -2, 3}            // remaining elements are zero-filled
var first: ptr int = &tbl[1]
var ops: [2]ptr fn(int): int = {sq, inc}
var keys: [2]Key = { { $48, "^" }, { .scan = $50, .name = "v" } }
```

Such tables can replace parallel arrays. A field without an initializer receives zero. After `.field = ...`, positional initializers continue from the next field.

Note the difference between `var msg = "..."` (an array) and `var p: ptr byte = "..."` (a pointer plus a separate string in the data area). For a local variable, `var s = "..."` has type `ptr byte`; the string itself is stored in the data area.

Uninitialized local variables contain indeterminate values, and the compiler warns if a variable might be read before assignment. A local array or structure can be initialized with a list; in that case, the object is initialized every time execution reaches its declaration:

```
var a: [4]int = { n, n * 2 }     // assignments; remaining elements are zero
var big: [32]int = { 1, 2, n }   // copy from main.i0 template, then big[2] = n
```

Objects of up to 16 bytes are initialized by individual assignments. Larger objects are copied word by word from a template in the data area (label `function.i<n>`), then nonconstant elements are assigned.

A **static local variable**, `static var n: uint = 10`, resides in the data area like a global but is visible only in its declaring block. Its value persists across calls, and its initializer must be constant. In assembly it is named `function.name`.

**File scope.** Placing `static` before a top-level `fn` or `var` makes that name visible only within its source file. Two libraries may then define functions with the same name; within a file, its `static` name hides an identically named global. The assembly symbol has the form `file.name` (`string.helper`).

### Memory-Mapped Locations

```
var CONSOLE at $FF20: byte
var VEC     at $FF00: [16]ptr interrupt fn()
```

`var ... at address` declares a variable at a fixed address. It consumes no space in the generated image: each access compiles to an instruction with an absolute address, such as `STB [$FF20], R0`. The type determines the instruction: `byte` uses `LDZX`/`STB`, while a word uses `LD`/`ST`. This mechanism describes device ports, the interrupt-vector table, screen memory, and anything else stored at a known address.

## 7. Control Flow

### Loops

```
while cond { ... }
loop { ... }                 // infinite loop; exit with break, return, or goto
break
continue
```

There is no separate `for` statement. An explicit loop variable requires less code than it might seem, and the compiler generates a rotated loop with one conditional branch per iteration:

```
var i: uint = 0
while i < N { ...; i++ }
```

Counting down to zero is cheaper still because `SUB` or `--` sets the required flags directly:

```
while n != 0 { ...; n-- }     // SUB Rn, #1 / JNZ
```

### goto

```
fn find(v: int): int, int {
    var i, j: uint
    i = 0
    while i < 3 {
        j = 0
        while j < 5 {
            if grid[i][j] == v { goto found }
            j++
        }
        i++
    }
    return -1, -1
found:
    return int(i), int(j)
}
```

Rules:

- A label (`name:`) appears at the beginning of a statement. It is visible throughout the function, including before its definition.
- Label names occupy a separate namespace from variable names.
- A label need not be followed by another statement; it may appear immediately before `}`.
- Jumps into loops and blocks are permitted. Skipping a `var x = ...` initializer leaves `x` uninitialized, as in C; storage for the variable was allocated for the whole function.
- **A jump into an `atomic` block is forbidden** (compile-time error): interrupts would not have been disabled, and leaving the block would pop a value that had never been pushed.
- A jump **out of** an `atomic` block is allowed. The compiler restores FLAGS once for each exited nesting level, as it does for `break` and `return`.

At the machine level, `goto` is just one `JMP` with no additional overhead. A state machine written with `goto` can be shorter and faster than a `switch` in a loop (see `examples/universal/fsm.b16`):

```
space:
    c = *s++
    if c == 0 { goto done }
    if classify(c) == 2 { words++; goto word }
    goto space
word:
    c = *s++
    if classify(c) != 0 { goto word }
    s--
    goto space
```

There is no `switch` statement. For a short decision chain, use `if ... else if`; for a longer or dense dispatch, use a table of function pointers (Section 8) or a data table.

## 8. Functions

### Declaration and Call

```
fn sum3(a, b, c: int): int { return a + b + c }
```
```
sum3:
	ADD   R0, R1
	ADD   R0, R2
	RET
```

Parameters of the same type can be grouped as `a, b, c: int`. Parameters and return values must be scalars. Top-level declaration order does not matter: the compiler collects all declarations in a file before compiling functions, so a function can be called before its definition. A prototype without a body (`fn f(x: int): int`) is useful in two cases:

- To make mutually recursive functions explicit.
- To declare functions implemented in assembly (Section 12).

### Calling Convention

This convention is the interface to assembly code, so it is worth knowing precisely.

- **Arguments.** R0–R3 provide four word slots in argument order: a word uses one slot, and a `long`/`ulong` uses two (low word in the first slot). An argument that does not fit, and all subsequent arguments, are pushed onto the stack from right to left. On entry, the first stack argument is at `[SP]`, the next one at a higher address; for a two-word argument, the low word is at the lower address. The caller removes stack arguments with a single `ADD SP, #2·k`, where `k` is the number of stack argument words (not the number of arguments).
- **Return values.** Results occupy R0–R3 under the same rules, up to four words in total.
- **Registers.** The callee may clobber R0–R3 and LR, but must preserve R4–R11.
- **Byte arguments and return values** are passed already extended to a full word: the caller extends arguments, and the callee extends results.
- **Return sequence.** A leaf function returns with `RET` (`LD IP, LR`); a non-leaf function uses `POP IP`. LR is saved on the stack only when the function makes calls.

The cost of a call to a non-leaf function is:

1. The caller places arguments in R0–R3 and, if the argument slots do not all fit, pushes the remaining arguments on the stack as described above.
2. `CALL` executes (one or two instruction words; the assembler selects the short encoding when possible).
3. The callee prologue uses `PUSH LR`, plus one `PUSH` for each R4–R11 register it uses.
4. The epilogue uses the corresponding `POP` instructions.

A leaf function using only R0–R3 needs nothing beyond `CALL` and `RET`.

If a call occurs in an expression that already has intermediate values in R0–R3, those values are preserved around the call using `PUSH`/`POP`:

```
fn caller(x: int): int { return sum3(x, 2, 3) + sum3(x, x, x) }
```
```
	CALL  sum3
	PUSH  R0            ; preserve the first result across the second call
	LD    R0, R4
	...
	CALL  sum3
	LD    R1, R0
	POP   R0
	ADD   R0, R1
```

If this occurs in a hot path, assign the result to a local variable: it will reside in R4 or above, eliminating the extra `PUSH`/`POP` pair.

### Multiple Return Values

A function can return up to four values, all in registers. This avoids pointer-based output parameters, which in C would force the target variable into memory:

```
fn divmod(a, b: uint): uint, uint { return a / b, a % b }
...
var q, r: uint
q, r = divmod(v, 10)
_, r = divmod(v, 10)          // discard the unused result with _
```

The right side of a multiple assignment must be a function call. If there are more results than variables on the left, the extras are discarded. In a leaf function, `return a, b` arranges the values in the result registers automatically, even when this requires a cyclic permutation (`return b, a`).

A useful parsing pattern is to return a value together with the next input position. The library's `atou` and the calculator in `examples/universal/calc.b16` use this approach:

```
fn primary(s: ptr byte): int, ptr byte {
    ...
    v, s = expr(s + 1)
    ...
}
```

### Function Pointers

```
fn apply(f: ptr fn(int): int, x: int): int { return f(x) }
var ops: [2]ptr fn(int): int = {sq, inc}
...
apply(sq, 5)
ops[1](5)
c.run(arg)                    // structure field
```

Function signatures are checked exactly: a function can be assigned to a function pointer only when the numbers and types of its parameters and results match. A function-pointer type may specify multiple return values, delivered in R0–R3 just like a direct call:

```
var sys: [8]ptr fn(int, ptr byte, uint): int, uint    // error code and data
var err: int
var n: uint
err, n = sys[k](fd, buf, len)
```

A comma after a return type extends the result list of the nearest `fn`. To terminate the list earlier, put it in parentheses: `ptr fn(ptr fn(): (int), uint)` describes a function taking two arguments. Parentheses are optional in a function declaration: `fn divmod(a, b: uint): (uint, uint)` and `fn divmod(a, b: uint): uint, uint` mean the same thing. An interrupt handler has the distinct type `ptr interrupt fn()`: it returns with `IRET`, so it is incompatible with `ptr fn()` and cannot be called through a function pointer.

Using a function name in an expression yields its address; `&f` is optional. Function pointers can be compared with zero or with one another, stored in arrays or structure fields, and returned from functions. Examples of callbacks, dispatch tables, method-table-based "objects", and state machines that store their state as a function pointer appear in `examples/universal/funcptr.b16`. An indirect call compiles to `CALL Rn`, `CALL [g]`, or `CALL [Rp+off]`; no extra register load is needed when the pointer already occupies one of those locations.

### Recursion

Recursion works in the usual way. Its depth is limited only by the stack (growing downward from `$FF00` toward the end of the data area). There is no stack overflow check. Tail calls are not optimized, so use a loop instead of deep tail recursion.

The `qsort` routine in `examples/universal/sort.b16` shows how to bound recursion depth: recurse into the smaller partition and process the larger one iteratively, keeping depth at O(log n).

## 9. Pointers and Memory

### Idioms That Map Directly to Addressing Modes

| B16 | Instruction |
|---|---|
| `x = *p` | `LD Rx, [Rp]` |
| `x = *p++` | `LD Rx, [Rp++]` (step of 2 for `ptr int`, 1 for `ptr byte`) |
| `*--p = x` | `ST [--Rp], Rx` |
| `x = p.f` | `LD Rx, [Rp+off]` |
| `x = g[3]` | `LD Rx, [g+6]` |
| `b = buf[i]` (global `[N]byte`) | `LDZX Rb, [Ri+buf]` |
| `x = tbl[i]` (global `[N]int`) | `LD t, Ri` / `SHL t, #1` / `LD Rx, [t+tbl]` |
| `p = &a[k]` | `LD Rp, #a+2k` |

Auto-increment is available only for a pointer held in a register and only if its increment matches the access size. Thus `*p++` is a single instruction for `ptr byte` and `ptr int`, but not for `ptr Node`: the latter needs an addition.

String copying is the classic use case for these addressing modes:

```
fn copy(d, s: ptr byte) { while (*d++ = *s++) != 0 {} }
```
```
@L7:
	LDZX  R2, [R1++]
	STB   [R0++], R2
	TEST  R2, R2
	JNZ   @L7
```

### Indexing Versus Pointers

Indexing a global byte array using a register variable takes one instruction. Indexing an array of words also requires multiplying the index by two, adding two instructions. When traversing an array sequentially, a pointer is almost always preferable: `ADD Rs, [Rp++]` versus three address-calculation instructions on every iteration.

### Pointers to Pointers

A useful linked-list technique is deletion without a special case for the head node:

```
fn remove_all(head: ptr Node, key: int): ptr Node {
    var link: ptr ptr Node = &head
    while *link != 0 {
        var n = *link
        if n.key == key { *link = n.next; release(n) } else { link = &n.next }
    }
    return head
}
```

Here, taking `&head` places the `head` parameter in the stack frame. That is expected: the parameter is accessed through its address.

### Strings

A string is a `ptr byte` pointing to a NUL-terminated sequence. Its length is not stored; string-handling routines are in `lib/string.b16`. String literals are *read-only* objects: writing to one has undefined behavior. With `option data`, literals remain alongside the code in ROM rather than being copied into RAM. Declare writable text as a global array (`var s = "..."`) or a local array (`var s: [16]byte = "..."`). Identical string literals are not merged.

## 10. Interrupts and Atomicity

### Interrupt Handlers

```
var VEC   at $FF00: [16]ptr interrupt fn()
var TIMER at $FF29: byte
var ticks: uint

interrupt fn on_tick() { ticks++ }

fn main() {
    VEC[0] = on_tick          // install the vector first,
    TIMER = 1                 // then enable the source,
    ei()                      // and finally execute STI
    while ticks < 50 { wfi() }
    TIMER = 0
    di()
}
```

Properties of `interrupt fn`:

- **Registers.** A handler saves every register it uses. If it calls functions, it must also preserve R0–R3 and LR because called functions may clobber them.
- **Flags and return address** are saved by the processor on interrupt entry and restored by `IRET`.
- **Restrictions.** A handler has no parameters or return values. It cannot be called like an ordinary function: it ends with `IRET`, not `RET`. For the same reason, the vector table has type `[16]ptr interrupt fn()`, not `[16]ptr fn()`.

A handler that touches only a global variable compiles into a short sequence:

```
on_tick:
	PUSH  R0
	LD    R0, [ticks]
	ADD   R0, #1
	ST    [ticks], R0
	POP   R0
	IRET
```

Under Monitor, interrupt vectors belong to Monitor and an application must not modify them. On bare hardware, startup code installs a stop handler for ERRCMD and a no-op handler for the other vectors, so an unexpected interrupt will not break the program. Install vectors by writing the table at `$FF00`: IRQ0–IRQ7 use vectors 0–7, ERRCMD uses vector 8, and 9–15 are available for `trap(n)`. A software `trap(9)` invokes vector 9 with interrupts disabled, making it a convenient system-call mechanism.

### atomic

```
atomic { s = sec; c = cs }
```

An `atomic` block compiles into `PUSH FLAGS`, `CLI`, the block body, and `POP FLAGS`. Interrupts are disabled during the body, and the *previous* IF state is restored on exit. This also makes nested blocks and blocks entered with interrupts already disabled work correctly. Exits via `return`, `break`, `continue`, or `goto` restore FLAGS as well.

The overhead is three single-word instructions. Use a block when several accesses to data shared with an interrupt handler must appear indivisible. Reading or writing a single word is already atomic because an instruction cannot be interrupted halfway through. In contrast, `ticks++` on a global variable consists of three instructions (`LD`, `ADD`, `ST`): if a handler also modifies `ticks`, the main program must wrap its update in `atomic`.

### xchb and Spinlocks

`xchb(p, v)` atomically exchanges a byte using `XCHB`, holding the bus throughout the exchange. It returns the old value:

```
var lock: byte
fn acquire() { while xchb(&lock, 1) != 0 {} }
fn release() { lock = 0 }
```

On a uniprocessor, a spinlock is mainly useful for coordinating with interrupt handlers that must not enter a critical section. An `atomic` block is usually sufficient.

### Waiting

`wfi()` stops the processor until an interrupt arrives. The emulator considers a program finished when the CPU is waiting in `WFI`, there are no pending requests, and the timer is off. This is how the startup code stops after `main` returns. Consequently, waiting with `wfi()` makes sense only if an interrupt source is enabled. A typical event-wait loop is:

```
while !done { wfi() }
```
## 11. Machine Devices

The `lib/console.b16` library declares the console ports and provides functions built on them. Everything in this section applies to bare hardware. Under Monitor, the timer, interrupt vectors, and keyboard are owned by Monitor, and applications access them through system calls (see `examples/monitor/keys.b16` and `ticks.b16`). The essential device behavior is:

- **Output.** Writing a byte to `$FF20` immediately outputs a character; there is no ready-status register.
- **Input.** Reading `$FF20` returns the character from the next keyboard event, or zero if none is available. The read never waits.
- **Word read from `$FF20`.** The low byte contains the character, and the high byte contains its scan code. This distinguishes arrow and function keys, which have a zero character code. The library function `getkey()` returns both values.
- **Emulator input.** `cpu16` reads stdin. After end-of-file, reads always return zero; therefore `waitc()` would wait forever. Programs that read input must detect an end condition in the input data itself (as in `examples/universal/echo.b16`).

The 80×25 display has three modes, selected by writing to `CON_CTRL`:

- **Teletype** (default): an ordinary scrolling console.
- **Positioned output:** a character is written at a specified position (`gotoxy`), without scrolling.
- **Attribute mode:** writing to DATA changes the color of an existing cell without changing its character.

Construct an attribute with `attr(foreground, background)`; `ATTR_UNDERLINE` and `ATTR_INVERSE` may be added. `examples/bare/screen.b16` demonstrates all of these features; view its result with `cpu16 --screen`.

The timer (`$FF29`, bit 0) raises IRQ0 every 20 ms of wall-clock time. The number of instructions executed between ticks depends on the host, so elapsed time is measured in ticks (`examples/bare/clock.b16`). After `main` returns, startup code turns off the timer and acknowledges an already-pending request. Otherwise a request left pending while interrupts were disabled could wake `WFI` repeatedly. If the program ends in another way, such as an endless `wfi()` loop, turn the timer off explicitly.

## 12. Using Assembly Alongside B16

### Reading the Generated Output

The output of `b16c` is ordinary `cpu16asm` source: you can inspect and edit it by hand. Its layout is designed so that the memory map (`cpu16asm -m`), listing, and assembler diagnostics describe the B16 program rather than merely the generated text.

- **Objects are global labels.** Each function, global variable, string literal, runtime function, and startup routine receives its own label. Labels within a function are local (`@L<n>`), so a function appears as one object in the map.
- **Literal labels** are named after their owners: `putu.s0`, `main.s2`, or, in a global initializer, `name.s0`. Division runtime routines are `__divs` and `__mods`; startup labels are `__header`, `__start`, `__halt`, `__fault`, and `__irq_ignore`.
- **Source locations.** A `.line "file.b16", N` or `.line N` directive precedes the code for each source line. A bare `.line` precedes startup and runtime code. The map's “By source file” section attributes bytes to B16 files, and assembler errors point to the B16 line, with the `.s` line in parentheses. The assembler option `--no-line` suppresses this remapping when debugging the compiler itself.
- **Memory categories.** Initialized data uses `.word`/`.byte` (reported as `data` in the map); uninitialized data uses `.reserve` (`bss`).
- **Constants visible to assembly.** Every `const` is emitted as `.equ`, as is the address of an `at` variable. For every structure, the compiler emits `.equ S.field` (field offset) and `.equ S.sizeof` (size). Assembly accessing a B16 structure can use these definitions rather than silently breaking when its layout changes. In B16 itself, use `offsetof(S, field)`.
- **Branches** use mnemonics such as `JNZ @L5`. The assembler selects the short form (JR, ±64 words) or long form. It also reduces a `CALL` to a single-word encoding when its target is within 8 words.
- **A comment** before each function identifies leaf functions, for example `; fn strlen (leaf)`.

The assembler disallows labels that match mnemonics or registers (`inc`, `neg`, `push`, `ret`, `lo`, `hi`, `r1`, etc., case-insensitively). B16 allows these identifiers but prefixes them in the generated assembly: a function called `neg` becomes the label `__neg`. Names beginning with `__` are reserved in B16, so no collision is possible. `static` symbols are emitted as `file.name`; a name containing a period matches neither a mnemonic nor a register.

**Translation-time validation.** Use `assert` declarations to verify layouts imposed by external formats:

```
assert sizeof(DirEntry) == 64
assert offsetof(Header, image_end) == 12, "CX16 header"
```

### Including Source Text in the Output: -s

With `-s`, the compiler prints the corresponding B16 source line as a comment before the instructions for each statement. The function heading and the comments immediately preceding it are shown before each function. When no more than eight lines of the same file separate two displayed source lines, the intervening lines (declarations and comments) are printed as well. A previously displayed line is repeated only when its generated code reappears later, as with a `while` condition tested at the bottom of a loop. Line numbers refer to the original file. A file change after `include` is marked by `; ---- file`.

```
remove_all:
	;   51      var link: ptr ptr Node = &head
	LD    R4, SP
	;   52      while *link != 0 {
	LD    R0, [R4]
	TEST  R0, R0
	JZ    @L32
@L30:
	;   53          var n = *link
	LD    R5, [R4]
	;   54          if n.key == key { *link = n.next; release(n) } else { link = &n.next }
	CMP   R6, [R5+2]
	JNZ   @L33
	...
@L34:
	;   52      while *link != 0 {
	LD    R0, [R4]
	TEST  R0, R0
	JNZ   @L30
```

This makes the inverted loop layout easy to see: the condition from line 52 is checked both on entry and at the bottom. Compiling with or without `-s` produces identical code; only the comments differ.

### asm Blocks

```
fn addpair(alo, ahi, blo, bhi: uint): uint, uint {
    asm {
        ADD   R0, R2
        ADC   R1, R3
    }
    return alo, ahi
}
```

The block's text is copied to assembly output verbatim. The compiler neither parses its instructions nor knows which registers they modify. Consequently, the following rules apply:

- **Compiler state after a block.** The compiler assumes that the flags are unknown but that every variable is still in its assigned register with the value the compiler put there. If the block modifies a variable's register, the compiler cannot detect it. The example deliberately relies on this: in a leaf function, `alo` is in R0 and `ahi` in R1, and `return alo, ahi` returns the modified registers.
- **Determining variable registers.** Only leaf-function parameters have a reliably predictable location: parameter i resides in Ri. Other variable placements depend on access counts and may change when adjacent code is edited. Inspect the generated output.
- **Free registers** are those not assigned to variables; no temporary expression values remain between statements. In a leaf function, unused registers from R0–R3 are available beyond the parameters and variables. In a non-leaf function, variables occupy R4–R11, so R0–R3 are always available inside an `asm` block. The block must preserve any R4–R11 registers that the compiler itself did not use.
- **A `CALL` inside the block makes the function non-leaf.** The compiler detects `CALL` in block text outside comments and treats it like a B16 call: it preserves LR and places variables in R4–R11. This makes it possible to call B16 functions or separate assembly routines from `asm` using the standard calling convention, without managing LR manually. If a non-leaf function has only one variable used by B16 code, that variable is guaranteed to be in R4.
- **Arguments on entry to the first `asm` block.** If the function body starts with `asm`, R0–R3 contain the arguments according to the calling convention (Section 8), and the prologue does not change them. You can therefore write a function entirely in assembly even if its parameters are never referenced by B16 statements (`fn call_on_stack(f: ptr fn(), top: ptr) { asm { ... } }`).
- **Global variables and functions** are available by name: `LD R0, [count]`, `ST [flags_copy], R0`, `CALL print_pair`. This is the most reliable way to pass values between B16 and `asm` without depending on register allocation (allowing for the `__` prefix on mnemonic-like names).
- **Labels** in blocks must be local `@name` labels; `@L<number>` is reserved for the compiler. A global label would disrupt the function's local-label scope, so the compiler rejects it.
- **Function names** occurring in block text count as references; a function called only from `asm` will therefore not be dropped from the image.
- **Assembly-based interrupt handlers.** If an `interrupt fn` contains no B16 statements, the compiler saves no registers and emits only `IRET`. The block must save whatever it clobbers. It sees the incoming stack unchanged: `[SP]` contains FLAGS and `[SP+2]` the return address. The ERRCMD handler in `examples/bare/inline_asm.b16` uses this technique to skip an invalid instruction.

The most predictable arrangement is a function with an all-assembly body, with B16 providing only its signature:

```
fn ror8(x: uint): uint { asm { ROR R0, #8 } return x }
```
```
ror8:
	ROR R0, #8
	RET
```

### Functions Implemented in Separate Assembly Files

Declare the function in B16 as `extern fn` (a prototype without a body, defined outside B16), then define it in a `.asm` file following the calling convention:

```
// prog.b16
extern fn bswap(x: uint): uint    // defined in bswap.asm
```
```
; bswap.asm
bswap:  ROR     R0, #8
        RET
```
```
; top.asm - assemble the files together
        .include "prog.s"
        .include "bswap.asm"
```

Compile `prog.b16` to `prog.s`, then assemble `top.asm`. **In this arrangement the module ends up beyond `__bss_end`, so the program does not work under Monitor**: Monitor loads only the bytes up to `__image_end` from the file, whereas on bare hardware the emulator loads the entire file, hiding the problem. `cpu16asm --exec` rejects this layout. The supported solution is `include asm`: the compiler emits the module's `.include` after functions and runtime code but before data. Both its code and initialized data (`.word`, `.byte`) then reside in the image below `__image_end`:

```
// prog.b16
include asm "bswap.asm"           // search path: relative to this file, then -I
extern fn bswap(x: uint): uint
```
```
b16c prog.b16 -o prog.s
cpu16asm --exec -o prog.bin prog.s
```

Keep `include asm` next to the module's prototypes. A library needing an assembly module can include it itself, leaving users to include only the B16 library; Monitor's `sys.b16` is organized this way. Repeated `include asm` directives for the same file are ignored. The `--asm file` option does the same thing from the command line.

Modules may contain only code and initialized data; declare uninitialized data in B16. An ordinary prototype without `extern` also compiles, but lacking a B16 definition it causes a warning: `function bswap is declared but not defined`. That warning is meant to catch missing function bodies, so always mark assembly-implemented functions `extern`: this keeps `-Werror` builds working and leaves real warnings visible. Defining an `extern` function in B16 as well is an error. Calls in the opposite direction, from assembly to B16, use an ordinary `CALL name` or `CALL Rn` through a function pointer, with arguments in R0–R3.

A B16 function called by name from an external assembly module, but never referenced by B16 code, would be removed as unreachable: the compiler has no knowledge of the external `.asm`. Mark it `export`. Exported functions are always emitted, retain their assembly names, and act as roots for reachability analysis of other functions.

```
export fn sys_read(fd: int, buf: ptr byte, n: uint): int, uint { ... }
```

For a complete example of calls in both directions, two results returned by an assembly function, and a callback through a function pointer, see `examples/universal/asmlink`.

### Kernels and Modules: option target = kernel

Code that is not a standalone application, such as the Monitor kernel or a module invoked from assembly, declares `option target = kernel`. No executable header or startup code is emitted. Unreachable functions are still removed; the entry points are `export` functions and interrupt handlers. `option code` specifies where code starts, and `option data` specifies where data starts. With `option data`, the compiler emits `__init_data`, a routine that copies initialized data from its template and zeros BSS; the kernel prologue must call it. Without `option data`, data immediately follows code and whoever loads the kernel must zero BSS (`__image_end` … `__bss_end`). See `tests/kernel`. The image layout for each target and the function reachability rules are documented in Sections 13.3 and 13.4 of `B16_reference_en.md`.

```
// kernel.b16
option target = kernel
option code = $E000
option data = $F000
```
```
b16c kernel.b16 -o kernel.s
```

```
; boot.asm
        LD      SP, #$FF00
        CALL    __init_data
        CALL    kmain
        .include "kernel.s"
```

In an `asm` block, names of `static` functions and variables from the current file, including static local variables, are written as they appear in B16. The compiler substitutes their `file.name` assembly names automatically.

### Image Settings: option

Put image settings required for correct execution in the program's main source file:

```
const PTOP = $A000              // process memory ends at PTOP; frame windows are above it
option max_top = PTOP           // max_top field in the CX16 header
option min_free = 4096          // space reserved for stack and heap
var WB at PTOP: [8192]byte      // the same value is used for the window address
```

| Option | Meaning | Default | Target |
| --- | --- | --- | --- |
| `target` | `std` or `kernel` | `std` | — |
| `code` | starting code address | 0 | `kernel` |
| `data` | starting data address | immediately after code | both |
| `min_free` | memory reserved for stack and heap | 2048 | `std` |
| `max_top` | process-memory upper bound, 0 means the entire range | 0 | `std` |

Each value is a constant expression; constants it uses must be declared earlier. Options are allowed only in the main source file: a library must not change another program's image layout. Each option may appear only once. Using a target-specific option with the wrong target (`code` in a `std` program or `max_top` in a kernel) is an error. `option` is not a reserved keyword: a variable or function may still use that name.

The command-line options `--target`, `--code`, `--data`, `--min-free`, and `--max-top` override source-level options, for example when building a test image at another address. If a command-line value differs from the corresponding `option`, the compiler warns; with `-Werror`, it stops. This exposes a forgotten build-script override immediately. The beginning of the `.s` output shows the effective values and their origins.
## 13. include and the Standard Library

```
include "console.b16"
include "../lib/string.b16"
```

Rules:

- `include "file"` appears at the start of a line and replaces that line with the file's contents. `include asm "file"` includes an assembly module (Section 12).
- Paths are searched first relative to the including file, then in directories specified by `-I`. `run.sh` adds `-I lib`.
- Each file is included **only once**. Repeated `include` directives for the same file, including ones reached through other files, are ignored. Separate include guards are unnecessary.
- The maximum nesting depth is 16.
- Errors identify the included file and its line number.

Declaration order does not matter, and unreachable functions are omitted from the image. You can therefore include an entire library anywhere in a file. You pay only for the library's global variables: they remain in the image even when unused.

### Standard Library

**`io.b16`** — text I/O for programs that run both on bare hardware and under Monitor. The launch environment is determined on every access by checking the signature at address `$0004` (`on_monitor()`). On bare hardware, output goes to the console port and input is obtained by polling the keyboard. Under Monitor, `WRITE` and `READ` system calls operate on standard streams, so `prog > file` and `prog < file` work as expected. Monitor calls are implemented with `asm` blocks; `sys.asm` is not required. Output functions have the same names as those in `console.b16`, so do not include both libraries. Available only for the `std` target.

| Function | Action |
|---|---|
| `putc`, `puts`, `putnl`, `putu`, `puti`, `puthex`, `putul`, `putl`, `puthexl` | same as in `console.b16` |
| `eputs(s)` | error message: to stream 2 under Monitor, or to the console on bare hardware |
| `readc(): uint` | waits for the next character; `IO_EOF` indicates end-of-input (only under Monitor) |
| `readline(buf, max): uint, uint` | reads up to `\n` and returns the length and 1, or `0, 0` at end-of-input |
| `on_monitor(): uint` | returns 1 when the application was launched by Monitor |

**`console.b16`** — the bare-machine console: display and keyboard ports. Under Monitor, only direct display output is safe to use (it cannot be redirected); Monitor owns keyboard input (see “System Calls”, Section 2.2).

| Function | Action |
|---|---|
| `putc(c)`, `puts(s)`, `putnl()` | output a character, a string, or a newline |
| `putu(v)`, `puti(v)`, `puthex(v, digits)` | decimal and hexadecimal formatting |
| `putul(v)`, `putl(v)`, `puthexl(v, digits)` | the same for `ulong` and `long` (up to 8 hexadecimal digits) |
| `getc(): byte` | key character or 0, without waiting |
| `getkey(): byte, byte` | character and scan code |
| `waitc(): byte` | wait for a character by polling |
| `readline(buf, max): uint` | read through `\n`; returns the length |
| `cls()`, `gotoxy(col, row)`, `setattr(a)`, `setmode(ctrl)` | screen operations |
| `attr(fg, bg)` | construct an attribute |

The file also declares color constants, screen modes and commands, and the `CON_*` ports.

**`string.b16`** — string operations.

| Function | Action |
|---|---|
| `strlen`, `strcpy`, `strcmp` | as in C |
| `memcpy(d, s, n)`, `memset(d, v, n)` | memory block operations |
| `toupper(c)` | uppercasing Latin and Cyrillic letters, including `ё` |
| `isdigit`, `isspace` | character classification |
| `atou(s): uint, ptr byte` | parsed number and pointer to the remainder of the string |
| `utoa(v, buf): ptr byte` | convert number to string (buffer of at least 6 bytes) |
| `atoul(s): ulong, ptr byte` | same for `ulong` |
| `ultoa(v, buf): ptr byte` | same for `ulong` (buffer of at least 11 bytes) |

**`math.b16`** — arithmetic on `int`, `uint`, `long`, and `ulong`.

| Function | Action |
|---|---|
| `abs`, `min`, `max`, `umin`, `umax` | standard operations |
| `gcd`, `isqrt` | greatest common divisor and integer square root |
| `mul16x16(a, b): ulong` | full 32-bit product of two words (`MUL` + `MULHU`) |
| `labs`, `lmin`, `lmax`, `ulmin`, `ulmax` | absolute value, minimum, and maximum for `long` and `ulong` |
| `lgcd`, `ulsqrt` | greatest common divisor and integer square root for `ulong` |
| `srand(seed)`, `rand()` | xorshift16 pseudorandom-number generator |

## 14. Writing Fast, Compact Code

The following techniques make a noticeable difference to compiler output. Check the generated `.s`: the cost is directly visible there.

1. **Keep hot data in local variables.** A global accessed in a loop costs two words per reference and extra load instructions; a local can stay in a register.
2. **Prefer `uint` when signed values are unnecessary.** Unsigned division and remainder take one instruction, whereas signed operations with a variable divisor call a helper function. Comparisons cost the same.
3. **Count down to zero.** `while n != 0 { ...; n-- }` does not need a separate comparison: `SUB` produces the flags.
4. **Traverse arrays with pointers.** `*p++` uses an addressing mode; `a[i]` for word arrays needs three instructions.
5. **Avoid taking the address of a local without a reason.** `&x` places `x` in memory throughout the function. If you need an output parameter, consider returning a second value.
6. **Return multiple values** rather than using a structure or output pointers for a pair of results.
7. **Store a call's result in a variable** if the same expression will make another call afterward. Otherwise an intermediate value must be saved with `PUSH`/`POP`.
8. **Keep small functions leaf functions.** A leaf function using R0–R3 for variables needs no prologue. A single call inside it (including signed division by a variable) adds `PUSH LR`, `POP IP`, and preservation of R4 and above.
9. **Constants from 0 to 15 are cheaper.** Small masks, increments, and offsets fit inside an instruction. Structure fields at offset zero can be accessed with a single-word instruction.
10. **Group byte-sized structure fields** to avoid paying for alignment padding.
11. **Avoid too many variables in one large function.** There is no lifetime analysis: a ninth register variable spills to the stack. A `long` occupies two registers.
12. **For `ulong`, divide by a constant in 3…255 instead of a variable.** The resulting helper is nine times shorter than the general 32-bit division routine; powers of two use shifts or masks.
13. **Cast words to `ulong` before multiplying:** `ulong(a) * ulong(b)` uses `MUL` and `MULHU` without a function call.

## 15. Pitfalls

- **A newline ends a statement.** Writing `x = a`, then a newline, then `- b` produces two statements: the second evaluates a value and discards it. The compiler warns that the expression's value is unused. Using `+` instead causes an error because unary plus does not exist. For a continued expression, leave the operator at the end of the preceding line.
- **`char` is signed.** Use `byte` for text, particularly Cyrillic text.
- **Local and global `var s = "..."` have different types.** A global declaration creates an array; a local declaration creates a pointer to a string in the data area, which must not be written to. For writable text inside a function, use a typed array: `var s: [8]byte = "tmp0000"`.
- **Signed `/` with a variable divisor implicitly calls a function.** The containing function is no longer a leaf function.
- **Variable shift counts are taken modulo 16 (modulo 32 for `long` and `ulong`).** `x << n` with `n = 16` returns `x`, not zero.
- **The order of side effects within one expression is unspecified.**
- **An `asm` block communicates nothing to the compiler.** If it clobbers a variable's register or R4–R11, you must handle the consequences.
- **Input after stdin reaches EOF consists of zeros.** `waitc()` and `readline()` may wait forever.
- **The stack has no bounds checking.** Deep recursion or large local arrays can silently overwrite data as the stack grows downward toward `__bss_end`. Under Monitor, reserve space using `option min_free`.
- **String literals with `option data` are not copied to RAM.** Do not write to a literal; use a writable array, either global (`var s = "..."`) or local (`var s: [N]byte = "..."`).

## 16. What B16 Omits, and Why

- **`switch`, `for`, and `do-while`.** The combinations of `if`/`else if`, `while`, `loop`, and `goto` cover these cases. Explicit tables of function pointers are often better for dispatch.
- **By-value structures, structure assignment, and structure comparison.** These would introduce hidden copies, contrary to the language's design.
- **Floating point and 64-bit types.** On CPU16 they require library routines, and their cost should remain visible. Built-in 32-bit integer types (`long`, `ulong`) are available; 64-bit and floating-point support can be built in libraries on top of them.
- **A preprocessor.** Use `const` for constants and `include` for source inclusion. There are no macros.
- **Lifetime analysis and global register allocation.** This is an intentional implementation limit, keeping the code generator simple and predictable and its output readable without a debugger. The costs include additional register moves in non-leaf functions (when parameters need to survive calls) and a limit on register-allocated variables. See Section 6 for the parameter-register optimization.
- **Tail-call optimization** and merging identical string literals.

The working programs under `examples/` demonstrate every language feature. They are organized by execution environment (see [`examples/README_en.md`](../examples/README_en.md) for details):

- **`universal/`** — one image runs both on bare hardware and under Monitor. Text output uses `io.b16` (Section 13): the bare machine writes to the console port, while Monitor uses a standard stream that can be redirected to a file.
- **`bare/`** — bare hardware only: own vector table, timer, keyboard-port input, and screen.
- **`monitor/`** — Monitor only: arguments, files, processes, keyboard events, and time through system calls. Built by the Monitor script `tools/b16build.sh`.

The `universal/` and `bare/` examples are tested with `make test` against reference output; `universal/` is also tested under simulated Monitor execution. The test suite only builds the `monitor/` examples.

| Example | Environment | Demonstrates |
|---|---|---|
| `hello` | universal | minimal program and `include` |
| `sieve` | universal | arrays, nested loops, constants |
| `sort` | universal | recursion, function pointers, pointer arithmetic, strings |
| `list` | universal | structures, memory pool, pointers to pointers, multiple return values |
| `calc` | universal | recursive parsing, value/position pairs, signed division |
| `fsm` | universal | a `goto`-based finite-state machine |
| `dispatch` | universal | command table using structures with function pointers |
| `funcptr` | universal | callbacks (map/filter/reduce), bytecode dispatch table, method-table "objects", pointer-based state machine, function returned as a result |
| `bignum` | universal | `ulong`: factorial and Fibonacci values up to 2³², `putul` |
| `echo` | universal | line input (keyboard or stdin), Cyrillic uppercasing with `toupper` |
| `asmlink/` | universal | separate `.asm` module with `include asm`, calls in both directions |
| `tetris` | universal | one game for both launch environments: vectors and timer on bare hardware, `TICKS`, `SLEEP`, `CON_GETEV` under Monitor |
| `inline_asm` | bare | assembly functions, flags and carry, local labels, named globals, calling B16 from `asm`, ERRCMD and TRAP handlers |
| `devices` | bare | console registers as a structure at a fixed address, keyboard scan codes |
| `life` | bare | Conway's Game of Life: two-dimensional arrays, pointers to arrays, `char`, display |
| `clock` | bare | timer, interrupt handler, `atomic` |
| `timer` | bare | minimal IRQ0 handler and `wfi` |
| `screen` | bare | positioned mode, box-drawing graphics, colors, attribute mode |
| `demo` | bare | survey of the language in one file |
| `tetris_bare` | bare | Tetris for bare hardware only |
| `args` | monitor | `argv`, PIB redirection flags, `MEMINFO`, error stream, exit code, `option max_top` |
| `wc` | monitor | file operations: `OPEN`, `READ`, `CLOSE`, utility-style errors, `ulong` counters |
| `keys` | monitor | raw console mode, `CON_GETEV` events with modifier flags |
| `ticks` | monitor | `TICKS`, `SLEEP`, `TICKHOOK` tick callback, `ONEVENT` Ctrl+C handler |
| `run` | monitor | `SPAWN` with output redirected to a file, child exit status |
