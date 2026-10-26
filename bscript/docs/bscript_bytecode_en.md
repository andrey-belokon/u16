# BScript Bytecode Format, Version 1

This document supplements *BScript — Language Specification (Phase 1)*, version 1.0, section 12.5 (implementation plan, section 13.1). It defines the `.bsc` files produced by the `bsc` compiler and executed by the `bsvm` virtual machine, along with the bytecode instruction set. Shared implementation definitions reside in `common/bsdefs.b16`.

All multibyte fields use little-endian byte order.

## 1. The `.bsc` file

| Section | Size |
| --- | --- |
| Header | 32 bytes |
| Source-file table | `srcsz` (even) |
| Constants | `constsz` (even) |
| Functions | `funcsz` (even) |
| Code | `codesz` (even) |
| Source-line table | `linesz` |

Sections are contiguous, with no gaps. `bsvm` loads the source-file table and constants into near memory, and the function table and code into the module frame. It does not load the source-line table; instead, it reads it from the file when it needs to report a source location (section 5).

### 1.1. Header

| Offset | Field | Value |
| --- | --- | --- |
| 0 | Magic | `"BSC1"` |
| 4 | `version` | 1 |
| 6 | `flags` | 0 |
| 8 | `entry` | Entry function index; always 0 in version 1 |
| 10 | `nglobals` | Number of globals; global 0 is `args` |
| 12 | `nfiles` | Number of source files |
| 14 | `srcsz` | Source-file table size |
| 16 | `nconst` | Number of constants |
| 18 | `constsz` | Constants section size |
| 20 | `nfunc` | Number of functions |
| 22 | `funcsz` | Function table size |
| 24 | `codesz` | Code size |
| 26 | `linesz` | Source-line table size |
| 28 | Reserved | 0 |

The VM rejects files with an incorrect magic or version, exiting with status 126.

### 1.2. Source-file table

For each program source file, with the main file first: MTIME (4 bytes, FS16 timestamp), path length (1 byte), and an absolute normalized path. An extra zero byte is appended if the total section size is odd. The table is used for cache validation by `bs` (specification, section 11.4) and for error reporting.

### 1.3. Constants

Each constant is a string encoded as a VM heap object so the VM can use it directly in module memory, without copying:

| Word | Meaning |
| --- | --- |
| 0 | Type 3 (string); the VM adds the “static object” flag |
| 1 | String length in bytes |
| 2 | 0; the VM writes the handle number here |

The string bytes follow, plus one padding byte for odd lengths. Constants are numbered from zero in file order. Constant 0 is the empty string (the name of the top-level function).

### 1.4. Functions

There are `nfunc` entries of 10 bytes each, followed by an annotation block.

| Offset | Field |
| --- | --- |
| 0 | Constant index containing the function name |
| 2 | Function code offset relative to the start of the code section |
| 4 | Number of parameters (byte) |
| 5 | Number of local slots, including parameters (byte) |
| 6 | Number of results (byte) |
| 7 | 1 if any parameter or result annotations require checking |
| 8 | Offset into annotation block: parameters first, then results, one byte each |

Function 0 is the implicit top-level function. For a script it contains top-level statements and global initializers; for a program defining `main`, it contains initializers followed by a call to `main`. User-defined functions receive indices starting at 1, in declaration order.

Annotations: 0 = none; 1 = `bool`; 2 = `int`; 3 = `string`; 4 = `array`; 5 = `map`; 6 = `file`; 7 = `error`; 15 = `value`. The value `null` satisfies any annotation.

### 1.5. Code

All function code occupies one instruction sequence. The top-level code starts at offset 0. Each function body is placed at its declaration position within that sequence, and a `jmp` causes top-level execution to skip the body.

**Code frames.** Code is divided into 8 KB frames (offsets `$0000–$1FFF`, `$2000–$3FFF`, and so on). If the module exceeds one frame, the VM keeps its code in multiple extended-memory frames and maps the required one into a window (specification, section 12.1). No instruction may cross a frame boundary. If fewer than seven bytes remain before a boundary, the compiler inserts a `jmp` to the beginning of the next frame and fills the remaining bytes with zeros. Jump and call operands refer to offsets in the entire code section and may target another frame.

## 2. Values

A value occupies four bytes (specification, section 12.2): word 0 contains the payload or handle; word 1 contains the tag.

| Tag | Type |
| --- | --- |
| `$8000` | null |
| `$8001` | bool (0/1) |
| `$8002` | int |
| `$8010` | string |
| `$8011` | array |
| `$8012` | map |
| `$8013` | file |
| `$8014` | error |

## 3. Instructions

The VM is stack-based. A function's local slots reside on the value stack, relative to its frame base. `s8`, `u8`, `u16`, and `s16` denote instruction operands. An `s16` branch displacement is relative to the end of its instruction.

### 3.1. Loading and storing

| Opcode | Instruction | Operand | Action |
| --- | --- | --- | --- |
| `$01` | `null` | | Push null |
| `$02` | `true` | | Push true |
| `$03` | `false` | | Push false |
| `$04` | `int8` | s8 | Push integer −128…127 |
| `$05` | `int16` | u16 | Push integer |
| `$06` | `str` | u16 | Push string constant |
| `$07` | `ldl` | u8 | Load local slot |
| `$08` | `stl` | u8 | Pop into local slot |
| `$09` | `ldg` | u16 | Load global variable |
| `$0A` | `stg` | u16 | Pop into global variable |
| `$0B` | `pop` | | Pop top value |
| `$0C` | `dup` | | Duplicate top value |
| `$0D` | `dup2` | | Duplicate top two values |
| `$0E` | `popn` | u8 | Pop n values |
| `$0F` | `chkt` | u8 | Check the top value against a type annotation (`type mismatch`) |

### 3.2. Operations

`$10`–`$1A`: `add sub mul div mod and or xor shl shr ushr` — binary operators. `add` also concatenates two strings. `$1B neg`, `$1C not` (boolean), `$1D bnot`, and `$1E pos` (integer type check) are unary operators.

`$20`–`$26`: `eq ne lt le gt ge in` — comparisons and membership tests yielding bool.

### 3.3. Branches

| Opcode | Instruction | Action |
| --- | --- | --- |
| `$28` | `jmp s16` | Unconditional branch |
| `$29` | `jf s16` | Pop bool; branch if false |
| `$2A` | `jt s16` | Pop bool; branch if true |
| `$2B` | `jfk s16` | If top bool is false, branch and retain it; otherwise pop it (`&&`) |
| `$2C` | `jtk s16` | Equivalent for true (`||`) |
| `$2D` | `jnn s16` | If top is non-null, branch and retain it; otherwise pop it (`??`) |
| `$2E` | `chkb` | Require top value to be bool |

Using a non-boolean value where a bool is required causes a runtime error.

### 3.4. Calls

| Opcode | Instruction | Operands |
| --- | --- | --- |
| `$30` | `call` | u16 function index, u8 mode |
| `$31` | `callb` | u8 built-in function, u8 argument count, u8 mode |
| `$32` | `callm` | u8 method, u8 argument count, u8 mode |
| `$33` | `ret` | u8 result count |
| `$34` | `fail` | u8 operand count, 1…3 |
| `$35` | `errc` | u8 code: the value of an `errors.*` constant |

Arguments appear on the stack in their original order. For `callm`, the receiver is placed below the arguments. Built-in function IDs (`B_*`) and method IDs (`M_*`) are defined in `bsdefs.b16`.

**Call mode** is a byte:

| Mode | Meaning |
| --- | --- |
| `$80` | Call as a statement: discard results and propagate errors |
| `$40 + n` | Require exactly n values, with no error capture: for an expression call (n = 1) or `return f()` |
| `n` (1…9) | n assignment targets: if n equals the result count, do not capture errors; if n equals the result count plus one, the final target receives an error or null |

For user-defined and built-in functions, the compiler checks that the call mode matches the number of results. A method receiver's type is not known at compile time, so the VM performs that check, raising `wrong number of results` on mismatch.

**`ret n`** replaces the call arguments with n result values and applies the call mode. Within function 0, `ret` terminates the program: no results or null yield exit code 0; otherwise the returned value must be a nonnegative int.

**Errors** (specification, section 12.7). A failing built-in function or `fail` sets the current-error register. If the current function's call site captures errors, its results are replaced by null values and the error object. Otherwise, the VM unwinds call frames until it reaches a call site that captures errors. An error escaping function 0 is printed as `name: object: message`, and the program exits with code 1.

**`fail n`:** Operands specify a message or error, an object (string or null), and a code. New errors record a `file:line` source location. The single-operand form `fail e` propagates an existing error unchanged.

### 3.5. Indexing and literals

| Opcode | Instruction | Action |
| --- | --- | --- |
| `$38` | `getidx` | `c i → c[i]` |
| `$39` | `setidx` | `c i v →`: assign `c[i] = v` |
| `$3A` | `setidxd u8` | Pop `v`; `c` and `i` are located d values below `v` (for multiple assignment) |
| `$3B` | `arr u16` | Convert n values into a new array |
| `$3C` | `map u16` | Convert n key/value pairs into a new map |
| `$3D` | `concat u8` | Convert n values with `str()` and concatenate them into one new string (interpolation) |

For `a[i], m[k] = f()`, the compiler pushes the container/index pairs for the destinations, makes the call, and stores the results in reverse order. A variable destination uses `stl` or `stg`. An indexed destination uses `setidxd`, where depth d counts the values between its container/index pair and the result being stored. Finally `popn` removes the saved pairs.

### 3.6. `for` loops

A `for` loop uses three hidden slots, s, s+1, and s+2, holding the collection, current index, and length at loop entry.

| Opcode | Instruction | Action |
| --- | --- | --- |
| `$3E` | `forinit u8` | Pop an array or string into slot s; initialize index to 0 and store the length |
| `$3F` | `fornext1 u8 s16` | Branch if index ≥ length; otherwise push element and increment index |
| `$40` | `fornext2 u8 s16` | Same, but push the index before the element |

If the array's length changes during iteration, `fornext` raises `array changed during iteration`.

### 3.7. `switch`

There is no dedicated opcode. The selector is stored in a hidden slot; each case is tested using `ldl`, the case value, `eq`, and `jt`/`jf`. Control reaches `default` after all previous checks have failed.

## 4. VM frames and memory

A call frame stores the return address, base of local slots, function index, and call mode. If the function table, code, and 800-byte call-frame table fit within 8 KB, they are stored together in the module frame, an extended-memory frame mapped through the page-6 window (specification, section 12.9). Otherwise, the function and call-frame tables are stored in near memory, and code spans several frames (section 1.5). The source-file table resides in near memory. Constants are also stored there unless doing so would leave less than 6 KB of heap space, in which case they become static objects in extended-memory frames (specification, section 12.10). Parameters occupy the first local slots; other slots are initialized to null on entry. Call depth is limited to 100 frames and the value stack to 384 entries; exceeding either limit raises `stack overflow`.

## 5. Source-line table

A stream of entries sorted by ascending code offset:

| Entry | Size | Meaning |
| --- | --- | --- |
| `0, file u8, offset u16, line u16` | 6 bytes | Absolute source location |
| `dcode (1…255), dline s8` | 2 bytes | Code-offset and line deltas relative to the previous entry, in the same file |

The source location of an instruction is the last entry whose offset does not exceed the instruction's offset. The VM searches this table only when needed: for runtime diagnostics, `e.where()`, or printing an unhandled error.

## 6. Listing

`bsc -l` prints the function table and instructions with offsets, for example:

```text
fn sq params 1 locals 1 results 1 at 000A
000A  ldl 0
000C  ldl 0
000E  mul
000F  ret 1
```
