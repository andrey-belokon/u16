# B16 Compiler for CPU16 (Revision 6)

B16 is a language at roughly the B/C level for the 16-bit CPU16 processor. Its constructs translate into short, predictable instruction sequences. `b16c` is a dependency-free, single-file C99 compiler. It emits source for the `cpu16asm` assembler; the resulting program can be run in the `cpu16` emulator.

- **`docs/B16_guide_en.md`** — the programmer's guide: how to think in B16, how constructs are compiled, calling conventions, interrupts, assembly, common techniques, and pitfalls. Start here.
- **`docs/B16_reference_en.md`** — language reference (revision 3; section 14 lists changes by revision): syntax, semantics, binary interface, and, in section 13, `b16c` implementation details: command-line options, image layout, function reachability, diagnostics, and limitations.
- **This file** — a quick overview.

## Building and running

```sh
make                                       # Or: cc -O2 -o b16c b16c.c (Windows: g++ -x c++ -O2 -o b16c b16c.c)
./run.sh examples/universal/hello.b16      # b16c -I lib -> cpu16asm -> cpu16
./run.sh examples/bare/screen.b16 --screen # Arguments after the source filename are passed to cpu16
B16FLAGS=-s ./run.sh examples/universal/sort.b16  # Pass compiler options via B16FLAGS
make test CPU16ASM=../cpu16/build/cpu16asm CPU16=../cpu16/build/cpu16 CPU16SRC=../cpu16/tools
make fuzz CPU16ASM=../cpu16/build/cpu16asm CPU16=../cpu16/build/cpu16
```

```text
b16c [options] prog.b16
  -o file  -I directory  -a  -s  -Werror  -w  -Wmaybe-uninit  -fcopy-params  -n (deprecated)
  Image options that override option and include asm directives in the source:
  --target std|kernel  --code ADDRESS  --data ADDRESS  --min-free N  --max-top N  --asm file
```

The main source file sets image options (target, code and data addresses, and CX16 header fields) using `option` declarations. Assembly modules are included with `include asm` statements (reference sections 3.2 and 3.6). Section 13 of `docs/B16_reference_en.md` is the authoritative source for all switches, defaults, exit statuses, image layout for each target, and function selection rules.

A program defining `main` is packaged as a CX16 executable: header, startup code, functions, runtime, initialized data, the `__image_end` marker, and uninitialized variables allocated with `.reserve` up to `__bss_end`. The image passes `cpu16asm --exec` validation and can run under the Monitor or in the bare-machine emulator. Under the Monitor, startup calls `main(argc, argv)` and returns its exit status in LR. On a bare machine, it sets `SP = $FF00`, points the ERRCMD vector at a halt handler and the other vectors at empty handlers, clears BSS, and halts after `main` returns.

Only functions reachable from `main`, `export` functions, interrupt handlers, global initializers, and `asm` blocks are included in the image.

## Contents

| Path | Description |
| --- | --- |
| `b16c.c` | Compiler |
| `lib/io.b16` | Text I/O for bare-machine and Monitor programs: console port or standard streams (`WRITE`, `READ`) |
| `lib/console.b16` | Bare-machine console: numeric and string output, keyboard, display, and colors |
| `lib/string.b16` | `strlen`, `strcmp`, `memcpy`, `toupper`, `atou`, `utoa`, `atoul`, `ultoa`, and more |
| `lib/math.b16` | `gcd`, `isqrt`, `labs`, `lmin`, `lgcd`, `ulsqrt` for `long`/`ulong`, plus `rand` |
| `examples/` | Examples organized by execution environment (`examples/README_en.md`): `universal/` has 11 programs for both environments plus `asmlink/`; `bare/` has 8 bare-machine programs (vectors, timer, keyboard, screen); `monitor/` has 5 syscall-based programs (arguments, files, keyboard, time, processes) |
| `tests/t1..t16.b16` | Regression tests: t4 covers new language features and `main` arguments; t5 tests `option data`; t6–t11 cover `long`/`ulong` data, calls, arithmetic, register pairs, and runtime; t12 library; t13 arguments passed to `asm`; t14 mutable strings; t15 parameters in argument registers; t16 parenthesized results |
| `tests/fuzz.py`, `tests/fuzz32.py`, `tests/fuzzp.py` | Random-program tests: compare with Python reference implementations (`make fuzz`, `make fuzz32`, `make fuzz32-deep`) and compare parameter placement with `-fcopy-params` (`make fuzzp`) |
| `tests/kernel/` | Module using `option target = kernel` and an assembly prologue |
| `tests/diag/` | Compiler diagnostic tests (reference output in `tests/expected/diag`) |
| `tests/monrun.cpp` | Runs images with simulated Monitor services (`READ`, `WRITE`, and `EXIT` for `io.b16`), and on a bare machine with deliberately initialized garbage memory |
| `tests/expected/` | Reference output for tests and examples |
| `tests/run_tests.sh` | Build (with `--exec` and memory map), run on bare machine and under simulated Monitor, and compare output (`--update` refreshes references) |
| `tests/fuzz.py` | Random expressions compared with a reference evaluator in Python |
| `run.sh` | Compile, assemble, and execute in one command |

## B16 at a glance

```b16
include "console.b16"                 // Must start at the beginning of a line; each file is included once

const N = 10                          // Compile-time constant
var count: uint                       // Global variable: always stored in memory
var tbl: [4]int = {1, 2, 3}           // Remaining elements initialized to zero
var msg = "hello"                     // [6]byte, in the machine encoding
var CONSOLE at $FF20: byte            // Port at a fixed address
struct Node { next: ptr Node; key: int; tag: byte }
struct Key { scan: byte; name: ptr byte }
var keys: [2]Key = { { $48, "^" }, { .scan = $50, .name = "v" } }
static fn helper() { }                // Visible only within this file
export fn entry(): int { return 1 }   // Entry point callable from assembly
extern fn memfill(p: ptr byte, n: uint, v: byte)  // Implemented in assembly
assert sizeof(Node) == 6

fn divmod(a, b: uint): uint, uint { return a / b, a % b }
interrupt fn on_tick() { count++ }    // Saves registers and returns with IRET

fn main(): int {
    var q, r: uint
    q, r = divmod(100, 7)             // Multiple return values
    _, r = divmod(5, 2)               // Discard an unwanted result
    var p: ptr Node = 0
    if p != 0 && p.key > 0 { p = p.next } else if q == 0 { r++ }
    while r != 0 { r-- }
    loop { if q > 3 { break }; q++ }
    atomic { count = 0 }              // PUSH FLAGS / CLI ... POP FLAGS
    goto done
done:
    puts("ok\n")
    return 0
}
```

**Types**

| Type | Size (bytes) | Signedness |
| --- | --- | --- |
| `byte` | 1 | Unsigned |
| `char` | 1 | Signed |
| `uint` | 2 | Unsigned |
| `int` | 2 | Signed |
| `ulong` | 4 | Unsigned |
| `long` | 4 | Signed |
| `ptr T`, `ptr` | 2 | Unsigned |

In addition to scalars, B16 supports arrays `[N]T`, `struct` types, function pointers `ptr fn(T…): R…` (up to four return values), and interrupt-handler pointers `ptr interrupt fn()`. Pointer types are checked strictly: `ptr T` can be assigned only a matching `ptr T`, and the only integer value assignable to a pointer is the constant zero. The untyped data pointer `ptr` is compatible with any data-pointer type.

Operations on `byte`, `char`, `int`, `uint`, and pointers are 16-bit. Operations on `long` and `ulong` are 32-bit, using register pairs and `ADD`/`ADC` or `SUB`/`SBC` chains; multiplication, division, and variable shifts use runtime helpers. Narrow integers are implicitly extended to `long`/`ulong` according to their original signedness; narrowing requires an explicit cast. Signed and unsigned operands cannot be mixed in `<`, `>`, `<=`, `>=`, `/`, or `%`. Untyped constants are evaluated exactly and adapted to the other operand; a value that does not fit the destination type is an error.

Casts: `int(x)`, `uint(x)`, `long(x)`, `ulong(x)`, `byte(x)`, `char(x)`, `ptr T(x)`, `ptr(x)`. Also available: `sizeof(T)` and `offsetof(S, field)`.

**Statements.** `var`, `if`/`else if`/`else`, `while`, `loop`, `break`, `continue`, `return` (up to four values), multiple assignment, `goto` with `name:` labels, `atomic { }`, and `asm { }`. Statement bodies always use braces; conditions do not require parentheses. Statements end at a newline or semicolon, as in Go.

**Operator precedence** (highest to lowest):

1. `() [] . x++ x--`;
2. unary `- ~ ! * & ++ --`;
3. `* / %`;
4. `+ -`;
5. `<< >>`;
6. `&`;
7. `^`;
8. `|`;
9. comparisons (not chainable);
10. `&&`;
11. `||`;
12. assignments.

Bitwise operators bind more tightly than comparisons: `x & M == 0` means `(x & M) == 0`.

**`goto`.** Labels have function-wide scope. Jumps into an `atomic` block are forbidden. Jumps or other exits from an `atomic` block (`goto`, `break`, `continue`, `return`) restore FLAGS.

**Built-ins.** `mulhu(a, b)`, `ror(x, n)`, `xchb(p, v)`, `di()`, `ei()`, `wfi()`, `trap(n)`. Ordinary `long`/`ulong` operations handle 32-bit values without special built-ins.

**Calling convention**

- Arguments 1–4 and results 1–4 use R0–R3. Additional arguments are pushed right-to-left; the caller removes them.
- R0–R3 and LR are caller-clobbered; R4–R11 are callee-saved.
- Leaf functions return with `RET`; non-leaf functions return with `POP IP`.

## Generated assembly

The output follows the assembler reference and is designed to work with `cpu16asm -m` memory maps:

- Each function, global variable, string literal (`function.s<n>`), runtime function, and startup-code component is an object with a global label. Labels inside functions are local (`@L<n>`).
- Each B16 source line is preceded by `.line "file.b16", N`. Startup and runtime code use `.line` without arguments. Memory maps, listings, and assembler diagnostics therefore point back to the original B16 source.
- Initialized data uses `.word`, `.byte`, `.space`; uninitialized data uses `.reserve`.
- Constants, addresses of `at` variables, field offsets (`S.field`), and structure sizes (`S.sizeof`) use `.equ`.
- `static` names become `file.name`.
- Branches use `Jxx` mnemonics; the assembler selects short or long encoding. Pseudoinstructions include `PUSH`, `POP`, `RET`, `CLI`, and `STI`.

Names that collide with instruction mnemonics or register names are prefixed with `__` (for example, a function named `neg` produces the `__neg` label). Strings and character literals are transcoded from UTF-8 into the machine character set. Directive and mnemonic names are listed near the beginning of `b16c.c`.

## Verification

- **`make test`** compiles all tests and examples, assembles them with `--exec` header validation and memory maps, runs them with `cpu16 --strict-align`, and checks the output against reference files. For `screen`, it compares a screen snapshot; for `echo`, it reads input from `echo.txt`. If CPU16 tool sources are available (`CPU16SRC`, default `../cpu16/tools`), it also builds `tests/monrun` and runs each program twice more: on a bare machine with garbage-filled memory and under a simulated Monitor (IF = 1, IRQ0 every seven instructions, exit through LR with an exit-code check). The kernel module and compiler diagnostics are tested separately.
- **`make fuzz`** generates 140 programs, each with 62 random expressions and assignments involving register, global, and indirect operands, including two-dimensional arrays, array pointers, and multiplication or division by constants. Forty programs contain depth-six expressions that exceed the available registers. Results are checked against a reference evaluator.

## Compiler diagnostics

Diagnostics are in English, using the assembler-style format `file:line: error: message` or `file:line: warning: message`. This makes them readable in any console, including Windows environments without UTF-8. Parsing continues at the next statement or declaration after an error, reporting up to 20 errors per invocation.

Warnings cover unused variables, parameters, and labels (except names beginning with `_`); use before assignment; comparisons between unsigned values and negative constants; discarded expression values; and prototypes without a definition (except `extern fn`). A function declared with return values that may reach its end without a `return` is an error. In the original implementation, comments emitted into generated `.s` files are in Russian.

## Limitations

- No live-range analysis: a variable keeps its register for the entire function. A leaf function can have at most nine register variables; excess variables spill to the stack. Non-leaf functions copy parameters to R4 and higher.
- Temporaries that must survive a call within an expression are saved with `PUSH`/`POP`. If an expression needs more registers than are available, the compiler regenerates that portion using stack temporaries. This increases code size but avoids a compilation error.
- No `switch`, `for`, by-value structures, floating point, macros, or tail-call optimization.
- Library globals remain in the image even when unused.
