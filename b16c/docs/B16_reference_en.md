# B16 Language Specification

Revision 3 · October 23, 2026 · Andrey (first revision: October 8, 2026; second revision: October 16, 2026)

## 1. Introduction

B16 is a compiled systems programming language for the 16-bit CPU16 processor (revision 6). This document defines the language syntax and semantics, as well as the binary interface followed by compiled programs. This is **revision 3** of the language specification, distinct from CPU16 processor revision 6. In this revision, image options (target, code and data addresses, and CX16 header fields) and assembly modules are specified in the source file; see Section 14 for the revision history. The reference implementation is the `b16c` compiler. Its implementation-defined behavior, command-line options, and limitations are documented in Section 13.

This specification does not define the processor architecture, assembler, machine devices, or Monitor. Those are covered by separate documents:

| Document | Relevant definitions |
| --- | --- |
| *16-bit Processor Architecture*, revision 6 | Instruction set, registers, and interrupts |
| *Assembler Reference* | The `cpu16asm` assembler used to process compiler output |
| *CPU16 Machine: Memory and Devices*, revision 9 | Device addresses, timer, and the shutdown procedure (9.2) |
| *CPU16 Monitor: Executable File Format and Program Startup*, revision 4 | CX16 format, PIB, and initial machine state (9.1, 9.2, 13.3) |
| *CPU16 Monitor: System Calls*, revision 6 | Monitor interface for programs built for the `std` target |

For a less formal introduction with examples, see *B16: Programmer's Guide*.

### Notation

Grammar productions are written in Extended Backus–Naur Form (EBNF):

| Notation | Meaning |
| --- | --- |
| `Name = ... .` | A grammar production |
| `"text"` | A literal token |
| `A \| B` | Either alternative |
| `[ A ]` | Optional element |
| `{ A }` | Zero or more repetitions |
| `( A )` | Grouping |

Token names are uppercase: `IDENT`, `NUMBER`, `STRING`, and `EOS` (end of statement; Section 2.3).

### Terminology

- **Error:** A violation that the implementation is required to diagnose; translation must fail.
- **Undefined behavior:** A construct whose result is not specified by the language; the implementation is not required to diagnose it.
- **Implementation-defined:** Behavior selected and documented by the implementation.
- **Scalar:** A value of type `byte`, `char`, `uint`, `int`, `long`, `ulong`, or a pointer type (Section 4).
- **Word:** 16 bits. **Byte:** 8 bits. **Double word:** 32 bits, stored as two words with the low word first. Operations on `byte`, `char`, `uint`, `int`, and pointer values use words; operations on `long` and `ulong` values use double words.

## 2. Lexical Structure

### 2.1. Source text

A source file is a sequence of UTF-8 characters. Spaces, tabs, and carriage returns (CR) are whitespace. Line feeds (LF) have special significance (2.3). CR LF line endings are accepted.

There are two forms of comments:

- `// ...` extends to the end of the line.
- `/* ... */` is a block comment; block comments cannot be nested. Line feeds inside a block comment do not terminate a statement.

### 2.2. Tokens

The source is scanned from left to right, always selecting the longest valid token. Token categories are identifiers, keywords, numbers, character literals, strings, operators, and delimiters.

### 2.3. End of statement

A line feed becomes an `EOS` (end-of-statement) token if the last token on that line is one of the following:

- An identifier, number, character literal, or string;
- `)`, `]`, or `}`;
- `++` or `--`;
- `break`, `continue`, or `return`;
- `byte`, `char`, `int`, `uint`, `long`, `ulong`, or `ptr`.

Consequently, the pointee type cannot be placed on the next line: a line ending in `var p: ptr` declares an untyped pointer (4.2).

There is one exception: if the first nonempty token after the line feed is `else` (ignoring blank lines and `//` comments), no `EOS` token is inserted. End of file following a token from the list above also produces `EOS`. An explicit semicolon `;` always denotes `EOS`; consecutive `EOS` tokens are allowed.

Therefore, an expression may span multiple lines only after an operator, comma, or opening bracket or parenthesis.

### 2.4. Identifiers and keywords

```ebnf
IDENT = letter { letter | digit } .
letter = "A"…"Z" | "a"…"z" | "_" .
digit = "0"…"9" .
```

Identifiers are case-sensitive. Names beginning with `__` are reserved for the implementation and must not be declared by a program. In expressions, `_` means “discard this value” and is permitted only on the left-hand side of a multiple assignment (7.8).

The following keywords are reserved:

```
asm      assert   atomic   at       break    byte     char     const
continue else     export   extern   fn       goto     if       include
int      interrupt long    loop     offsetof ptr      return   sizeof
static   struct   uint     ulong    var      while
```

`option` is not a reserved word. It has special meaning only at the start of a top-level declaration (3.6); elsewhere it is an ordinary identifier. The names of image options (`target`, `code`, `data`, `min_free`, `max_top`) and the values `std` and `kernel` are not reserved either.

Built-in function names (`mulhu`, `ror`, `xchb`, `di`, `ei`, `wfi`, `trap`) are not reserved. However, when an identifier from this list is followed by `(`, it always denotes a call to the corresponding built-in function (Section 8).

### 2.5. Numbers

```ebnf
NUMBER = decimal | "$" hexDigit { hexDigit }
       | ("0x" | "0X") hexDigit { hexDigit }
       | ("0b" | "0B") binaryDigit { binaryDigit } .
```

Numbers have values in the range 0 through 4294967295 (`$FFFFFFFF`); a value outside this range is an error. A number is an untyped constant (4.7). Its type is determined by context or, if there is no contextual type, by its value (5.3). A numeric literal must not be immediately followed by a letter, a digit invalid in its base, or `_`. There are no negative literals: `-1` is the unary minus operator applied to a literal (constant expressions, 6.10).

### 2.6. Character literals

A character literal is a single character or escape sequence enclosed in single quotes, such as `'A'`, `'\n'`, or `'Ж'`. Its value is the character code in the CPU16 machine character set. Using a source character that cannot be represented in that character set is an error. Like a number, a character literal is an untyped integer constant (4.7).

| Sequence | Value |
| --- | --- |
| `\n` | 10 |
| `\r` | 13 |
| `\t` | 9 |
| `\e` | 27 |
| `\0` | 0 |
| `\\`, `\'`, `\"` | The character following `\` |
| `\xH`, `\xHH` | A byte with the specified hexadecimal value, without character-set conversion |

All other sequences beginning with `\` are errors.

### 2.7. Strings

A string is a sequence of characters and escape sequences enclosed in double quotes. A line feed within a string literal is an error. Characters are converted to the machine character set using the same rules as character literals. A string containing *n* characters defines an array of type `[n+1]byte` with a terminating zero byte (6.3).

In an expression, a string literal is read-only; writing to it is undefined behavior (12.1). To obtain writable text, declare an array initialized from a string: either a global `var s = "..."` (with type `[n+1]byte`) or a local `var s: [N]byte = "..."` (where N is at least n+1, creating a separate copy; 5.3). A local declaration `var s = "..."` without an explicit type does **not** create an array: it creates a `ptr byte` pointing to the literal, which must not be modified.

### 2.8. Operators and delimiters

```
+   -   *   /   %   &   |   ^   ~   !   <<  >>
==  !=  <   >   <=  >=  &&  ||  ++  --
=   +=  -=  *=  /=  %=  &=  |=  ^=  <<= >>=
(   )   [   ]   {   }   ,   ;   :   .
```

Any other character outside a comment, string, or character literal is an error.

## 3. Program Structure

### 3.1. Translation unit

A program consists of a primary source file and the files it includes through `include`. After inclusion, these form a translation unit: a sequence of top-level declarations separated by `EOS` tokens.

```ebnf
Program = { Declaration | EOS } .
Declaration = ConstantDeclaration | [ "static" ] VariableDeclaration | StructureDeclaration
            | FunctionDeclaration | Assertion | ImageOption .
```

Statements are not allowed outside functions. `ImageOption` configures the output image and is permitted only in the primary source file (3.6).

### 3.2. `include`

A line of the form

```b16
include "path"
```

is replaced by the contents of the named file. An `include` directive is recognized only when it is the first construct on a line (preceding spaces and tabs are allowed). After the closing quote, only a `//` comment is allowed. `include` has no effect inside a block comment. Using `include` anywhere else is an error.

Files are located and included according to these rules:

1. An absolute path is used directly.
2. A relative path is searched first relative to the directory containing the including file, then in the implementation's search directories (`b16c -I`), in order.
3. Each file is included at most once. A subsequent `include` naming the same canonical path is ignored.
4. Inclusion may be nested up to 16 levels.
5. A missing file is an error.

Diagnostics refer to the file and line containing the offending text.

**Assembly modules.** A line of the form

```b16
include asm "path"
```

adds an assembly module to the output image (10.4). The module's text is **not** substituted into the B16 source; instead, the implementation emits the module into the image before `__image_end`. This directive follows the same recognition rules as `include`: it must appear first on the line, outside a block comment, with only an optional `//` comment after the closing quote. The file is located using rules 1 and 2 above; a missing file is an error.

The directive is permitted in any file in the translation unit. Thus, a library requiring an assembly module can include it itself (Monitor's `sys.b16` includes `sys.asm`). Repeated inclusion of the same canonical path is ignored, even if the module was also supplied externally to the implementation (`b16c --asm`). Modules are emitted in the order of their first appearance in the translation unit. The implementation limits the number of modules (13.8).

### 3.3. Declaration order

The scope of a top-level declaration covers the entire translation unit, including source text preceding that declaration. A function may call another function, or refer to a global variable, declared later in the file. No prototype (5.5) is needed for this purpose.

The exceptions are contexts evaluated while declarations are being parsed:

- A constant referenced in a constant expression (array bound, `at` address, or another constant's value) must be declared earlier.
- A structure used by value, for example as a field type or global variable type, must already have been defined. A pointer `ptr S` may refer to a structure defined later.
- The initializer of a global variable with an inferred type (`var x = ...`) may refer only to earlier declarations. The initializer of a variable with an explicit type may refer to any declaration.
- An `option` value (3.6) is a constant expression and follows the first rule.

### 3.4. Entry point

The program entry point is the `main` function. The following signatures are valid:

```b16
fn main()
fn main(): int
fn main(argc: uint, argv: ptr ptr byte)
fn main(argc: uint, argv: ptr ptr byte): int
```

Parameter names may vary, but all other combinations of parameters and return values are errors. `argc` is the number of command-line arguments. `argv` points to an array of `argc` pointers to strings, followed by a terminating null pointer; `argv[0]` is the program name. A returned value is the process exit status. If `main` has no return value, the process exits with status 0. `main` cannot be `static`.

The implementation supplies startup code (9.2) that calls `main`. A program without `main`, or code that does not need startup code (a kernel or assembler-linked modules), is built for the `kernel` target by writing `option target = kernel` (3.6).

### 3.5. Function reachability

Only reachable functions are included in the output image. The reachability roots are:

- `main`;
- Functions marked `export` (5.5);
- All interrupt handlers (`interrupt fn`);
- Functions whose names appear in global variable initializers.

A function is reachable if another reachable function refers to it through a call, an address-taking expression, or its name in an `asm` block. All global variables are included in the image, whether or not they are referenced.

### 3.6. Image options

```ebnf
ImageOption = "option" IDENT "=" ( ConstantExpression | IDENT ) EOS .
```

Image options control how a program is laid out in memory and started. `option` is a contextual word (2.4): a top-level declaration beginning with the identifier `option` is interpreted as an image option.

| Name | Value | Default | Target | Effect |
| --- | --- | --- | --- | --- |
| `target` | `std` or `kernel` | `std` | — | `std`: a CX16 executable with a header and startup code (9.2), requiring `main` (3.4). `kernel`: code without a header or startup code (for the kernel or assembler-linked modules); `main` is an ordinary function. |
| `code` | Even address, `$0000`…`$FFFE` | 0 | `kernel` | Starting address of the code. |
| `data` | Even address, `$0000`…`$FFFE` | Immediately after code | Both | Start of the global-variable region; initial values of initialized variables are stored in a template next to the code (9.1). |
| `min_free` | Even value, 0…`$E000` | 2048 | `std` | CX16 header field `min_free`: memory for stack and heap in addition to BSS. |
| `max_top` | 0…`$FFFF` | 0 | `std` | CX16 header field `max_top`: preferred upper bound of process memory; zero means all available memory. |

The value of `target` is an identifier. Other option values are constant expressions (6.10), and any constants used in those expressions must have been declared earlier (3.3). Thus, the same constant can serve as both an image option and an address used by the program:

```b16
const PTOP = $A000              // Process memory ends at PTOP; frame windows are above it.
option max_top = PTOP
option min_free = 4096
var WB at PTOP: [8192]byte
```

Rules:

1. Image options are permitted only in the **primary source file**, the file passed to the implementation. An option in a file included through `include` is an error: a library must not change its caller's memory layout. A library can still include its own assembly dependencies through `include asm` (3.2).
2. Each option may be specified at most once.
3. An option's position within the primary source file does not matter: all options apply to the entire image. Conventionally, they appear near the beginning of the file.
4. An unknown option name, out-of-range value, or odd address is an error.
5. An option inapplicable to the selected target is an error: `code` is invalid for `std` (whose code always starts at zero with the header), while `min_free` and `max_top` are invalid for `kernel` (which has no header).

The implementation may also accept these options externally (the `b16c` command-line flags; 13.2). An externally specified value takes precedence over an option in the source file.

Example: Monitor kernel configuration:

```b16
option target = kernel
option code = $0004             // At $0000, monitor.asm contains a jump.
option data = $8000             // Data and BSS start on page 4.
include asm "klib.asm"
```

## 4. Types

```ebnf
Type = "byte" | "char" | "uint" | "int" | "long" | "ulong"
     | "ptr" [ Type ]                        (* Without a type: untyped pointer *)
     | "[" ConstantExpression "]" Type
     | IDENT                                 (* Structure name *)
     | "fn" "(" [ Type { "," Type } ] ")" [ ":" Results ]
     | "interrupt" "fn" "(" ")" .
Results = Type { "," Type } | "(" Type { "," Type } ")" .
```

### 4.1. Scalar types

| Type | Size (bytes) | Range | Signedness |
| --- | --- | --- | --- |
| `byte` | 1 | 0…255 | Unsigned |
| `char` | 1 | −128…127 | Signed |
| `uint` | 2 | 0…65535 | Unsigned |
| `int` | 2 | −32768…32767 | Signed |
| `ulong` | 4 | 0…4294967295 | Unsigned |
| `long` | 4 | −2147483648…2147483647 | Signed |
| `ptr T`, `ptr` | 2 | Address 0…`$FFFF` | Unsigned |

`byte` and `char` are **byte types**; `uint`, `int`, and pointers are **word types**; `long` and `ulong` are **double-word**, or **wide**, types. When a byte value is loaded, it is extended to a word: zero extension for `byte`, sign extension for `char`. When storing to a byte object, only the low byte is written. In an expression, a byte value has its **promoted** type: `byte` becomes `uint`, and `char` becomes `int`.

A double-word object occupies four bytes and is word-aligned. Its low word is stored at the lower address (little-endian). It is read and written using two instructions, low word first (9.7). This layout matches 32-bit FS16 filesystem fields: a directory-entry or file-descriptor field can be declared as `ulong` without an auxiliary structure.

There is no Boolean type. A condition is true if its value is nonzero. Comparison and logical operators produce an `int` whose value is either 0 or 1.

### 4.2. Pointers

`ptr T` is the address of an object of type T. The pointer's element type is T, and its element size is `sizeof(T)`; element size is undefined for `ptr fn(...)`. Zero is the null pointer. Dereferencing a null pointer is not prohibited by the language: it accesses address zero.

**Compatibility.** Two pointer types are identical if their element types are identical: scalar types match by kind (`ptr byte` and `ptr char` differ); array types match by length and element type (`ptr [4]int` differs from `ptr int`); structure types match by name; function types match by parameter and result counts and types, and by the presence of `interrupt`. A value of type `ptr T` can be assigned implicitly (6.2) only to another `ptr T` with the same T, or to an untyped pointer. Integers cannot be assigned to pointers, nor pointers to integers, except that the constant zero may be assigned to any pointer. Other conversions require explicit casts (4.6).

An **untyped pointer**, written `ptr` without an element type, refers to data of unspecified type. It is intended for generic memory operations (copying, filling, allocation) and for data interpreted differently according to its contents. It is an unsigned two-byte scalar, permitted wherever scalar types are permitted: variables, parameters, return values, fields, array elements, and `at` variables.

- Any `ptr T` other than a function pointer converts implicitly to `ptr`, and `ptr` converts implicitly to any `ptr T` other than a function pointer. Conversion between function pointers and `ptr` must be explicit.
- Operations requiring an element size are forbidden on `ptr`: dereference (`*p`), indexing (`p[i]`), field access (`p.f`), `++` and `--`, addition or subtraction with an integer, `+=` and `-=`, and subtraction of two pointers. To perform address arithmetic, cast the pointer explicitly to `ptr byte`.
- Assignment, comparison, conditional tests (`if p`, `!p`), and operations on its 16 address bits producing a `uint` (6.6) are allowed; for example, `p & 1` tests alignment.
- Built-ins that require a specifically typed pointer (such as `xchb`) do not accept an untyped pointer.

### 4.3. Arrays

`[N]T` denotes N consecutive objects of type T, where N is a constant expression greater than zero. Its size is N × `sizeof(T)`. A multidimensional array is an array of arrays, for example `[3][5]int`. Arrays cannot be copied or compared as a whole. In an expression, an array **decays** to a pointer to its first element (`[N]T` becomes `ptr T`). There are no exceptions: the operand of `sizeof` is a type, not an expression (4.8).

### 4.4. Structures

A structure is a named layout of fields in memory (5.4). Fields are laid out as follows:

- Fields appear in declaration order.
- **Alignment** is 1 for byte types, the element alignment for an array, the maximum field alignment for a structure, and 2 for all other types.
- A field requiring alignment 2 is placed at an even offset, inserting one padding byte if necessary.
- The total structure size is rounded up to an even number of bytes.

Operations on an entire structure are limited to field access (`.`), taking its address (`&`), and applying `sizeof` to its type. Assigning, comparing, passing, or returning a structure by value is an error.

### 4.5. Function types

`fn(T1, ..., Tn): R1, ..., Rk` is a function type with scalar parameters T1…Tn and scalar results R1…Rk; omitting `:` means no results. Results occupy no more than four words. Each `byte`, `char`, `uint`, `int`, or pointer result occupies one word; each `long` or `ulong` occupies two (9.3). A function type is permitted only as a pointer element type, for example `ptr fn(int): int` or `ptr fn(int, ptr byte, uint): int, uint`. A function name in an expression decays to a pointer to its function type. Calling through such a pointer returns all results in R0–R3, just like a direct call (9.3).

A comma after a result type continues the result list of the nearest function type if it is followed by a scalar type. In `ptr fn(ptr fn(): int, uint)`, the inner function type has two results and the outer function has one parameter. To end the inner result list earlier, use parentheses: `ptr fn(ptr fn(): (int), uint)` denotes a function with two parameters. The same `Results` grammar production is used in function declarations (5.5), where there is no ambiguity and the parentheses are optional: `fn f(): (int, uint)` and `fn f(): int, uint` mean the same thing.

`interrupt fn()` is the type of an interrupt handler (5.6). An interrupt handler returns from an interrupt differently from an ordinary function returning from a call. Therefore, `ptr interrupt fn()` and `ptr fn()` are incompatible: a handler can only be assigned to a handler pointer, and calling through that pointer is an error. An interrupt vector table can be declared as `[16]ptr interrupt fn()`.

### 4.6. Type casts

```ebnf
Cast = ScalarType "(" Expression ")" .
ScalarType = "byte" | "char" | "uint" | "int" | "long" | "ulong" | "ptr" [ Type ] .
```

| Cast | Result |
| --- | --- |
| `uint(x)`, `int(x)` | The same 16 bits, with a different type; x must be an integer or pointer. For `long` or `ulong`, the low 16 bits are taken. |
| `long(x)`, `ulong(x)` | `byte` and `uint` are zero-extended; `char` and `int` are sign-extended; `long` and `ulong` retain the same 32 bits but change type. Casting a pointer is an error: use `uint(p)` first. |
| `ptr T(x)`, `ptr(x)` | The same 16 bits, interpreted as a pointer; x must be a word-sized integer or any pointer. Casting from `long` or `ulong` is an error: use `uint(x)` first. |
| `byte(x)` | The low byte, zero-extended; for `long` or `ulong`, this is the low byte of the low word. |
| `char(x)` | The low byte, sign-extended; for `long` or `ulong`, this is the low byte of the low word. |

The cast operand must be a scalar (an array decays to a pointer). Casting a pointer directly with `byte(p)` or `char(p)` is an error: use `uint(p)` first. Casting a constant produces a constant, and its value must fit the target type according to the constant rules (4.7).

### 4.7. Untyped constants

Numeric and character literals, named constants (`const`), and `sizeof(T)` are **untyped constants**. In an operation with a typed operand, an untyped constant adopts that operand's type (6.4). A constant expression consisting entirely of untyped constants is evaluated **exactly**, using signed integers without type-width wraparound, and remains untyped. A result or intermediate value outside −2^62 through 2^62−1 is an error. Integer division truncates toward zero, the remainder has the sign of the dividend, right shift (`>>`) is arithmetic, and a constant shift by 63 bits or more is an error.

**Casting an untyped constant to T** accepts values from −2^(n−1) through 2^n−1, where n is the width of T in bits (8, 16, or 32). Values outside that range are errors. A value that does not fit the signed range but fits the unsigned range, or vice versa, retains its bit representation: `int($FFFF)` is −1, `uint(-1)` is 65535, and `ulong(-1)` is `$FFFFFFFF`. The same range check applies when an untyped constant adopts a type in an operation (6.4), assignment, argument passing, or initialization (6.2).

If an expression contains a cast or a typed operand, its result is typed and evaluated according to Section 6.5 at the width of that type, wrapping modulo 2^n.

**Complementing constants.** Unary `~` and `-` on untyped constants are also evaluated exactly, so their results may fall outside the range accepted for the other operand's type. A common example is clearing a flag with a mask:

```b16
const F_DIR = $8000
var flags: uint
...
flags = flags & ~F_DIR          // Error: ~$8000 = -32769, outside -32768..65535.
flags = flags & ~uint(F_DIR)    // Correct: ~ is evaluated as uint, yielding $7FFF.
flags = flags & $7FFF           // Correct: the mask is written explicitly.
```

If the constant's highest bit is clear, the error does not occur: `~$4000` is −16385, represented as `$BFFF` in `uint`. The same principle applies to constants starting at `$80000000` for 32-bit operands: `size & ~$80000000` is an error, while `size & ~ulong($80000000)` is correct.

### 4.8. `sizeof` and `offsetof`

`sizeof(T)` is the size of type T in bytes and is an untyped constant. Its operand must be a type, not an expression. The size of a function type is undefined and using it is an error.

`offsetof(S, f)` is the byte offset of field f from the beginning of structure S and is an untyped constant. The field may be specified through nested structures, for example `offsetof(Rec, pos.hi)`. The structure must have been defined earlier in the source.

## 5. Declarations

### 5.1. Namespaces and scope

Names belong to four namespaces:

| Namespace | Members | Scope |
| --- | --- | --- |
| Global names | Constants, global variables, `at` variables, functions | Entire translation unit (3.3); `static` names are file-scoped (5.8) |
| Local names | Parameters and local variables | From their declaration to the end of the enclosing block |
| Structures | Structure names | Entire translation unit |
| Labels | `goto` labels | Entire function (7.6) |

The fields of each structure form a separate namespace. Redeclaring a top-level name in the same namespace is an error. A local name may shadow a global name or a local name from an enclosing block. Function parameters belong to the outermost block of the function body.

### 5.2. Constants

```ebnf
ConstantDeclaration = "const" IDENT "=" ConstantExpression EOS .
```

A constant is a name for the value of a constant expression (6.10). It occupies no storage and is an untyped constant (4.7).

### 5.3. Variables

```ebnf
VariableDeclaration = "var" NameList ":" Type EOS
                    | "var" IDENT [ ":" Type ] "=" Initializer EOS
                    | "var" IDENT "at" ConstantExpression ":" Type EOS .
NameList = IDENT { "," IDENT } .
Initializer = Expression | "{" [ Element { "," Element } [ "," ] ] "}" .
Element = [ "." IDENT "=" ] Initializer .
```

An initializer is permitted only when one variable name is declared. A variable cannot have a function type.

**Type inference from an initializer.** If no type is given, it is inferred as follows:

| Initializer | Global variable | Local variable |
| --- | --- | --- |
| String of length n | `[n+1]byte`; the string bytes are stored in the variable itself | `ptr byte`; the string resides in the data region |
| Untyped constant | Determined by its value (below) | Determined by its value (below) |
| Expression of type T | T after array decay | T after array decay |

**String-initialized local variables.** An untyped local declaration such as `var s = "tmp0000"` creates a pointer to a string in the data region, **not** a private copy. Writing `s[3] = '1'` modifies the literal (12.1); if `option data` places it in ROM, the literal resides in read-only memory. Declare an array to obtain a mutable template inside a function: `var s: [8]byte = "tmp0000"`. A fresh copy is initialized each time the declaration executes (13.6), and that copy can safely be modified. A pointer to such a local array must not be returned from the function because the array resides in the stack frame (9.4); a pointer to a string literal may be returned.

**Type inferred from a constant value.** Choose the first of `int`, `uint`, `long`, and `ulong` whose range contains the value: −32768…32767 selects `int`; 32768…65535 selects `uint`; −2147483648…−32769 or 65536…2147483647 selects `long`; 2147483648…4294967295 selects `ulong`. Values outside −2^31…2^32−1 are errors. An explicit type may be supplied by annotation or cast: `var x: long = 5`, `var y = ulong(5)`.

**Global variables** exist throughout program execution. Without an initializer they contain zeros. A global initializer is evaluated at translation time and may be:

- A constant expression;
- A string, for a byte array (the complete zero-terminated string must fit; remaining elements are zero-filled) or for a pointer (the variable then points to the string in the data region);
- The address of a global object: `&g`, `&g[k]`, or `&g.f`, with constant indices, or an array name;
- A function name (a function address);
- The address of the `__image_end` or `__bss_end` label (9.1);
- A cast of any of the above;
- A possibly nested `{ ... }` initializer list for an array or structure.

Values are checked according to the assignment rules (6.2). A byte-type scalar can be initialized only with a numeric value; a byte array can also be initialized with a string.

**Initializer lists.** Array list elements initialize successive array elements; there must be no more elements than the array length. A structure list initializes fields: a positional element initializes the next field, while `.f = value` initializes field `f`; subsequent positional elements resume at the field after `f`. Initializing a field twice or naming a nonexistent field is an error. Unspecified elements and fields, as well as alignment padding bytes, are set to zero. The `.f =` form is allowed only in structure initializer lists.

```b16
struct Key { scan: byte; name: ptr byte }
var keys: [2]Key = { { $48, "^" }, { .scan = $50, .name = "v" } }
```

**Local variables** are declared by `var` statements inside a function body (7.2). They exist for the duration of the function call, regardless of the block in which they are declared. A local scalar initializer may be any expression. A local array or structure may be initialized with an initializer list containing arbitrary expressions (and a byte array may be initialized from a string). The object is initialized each time execution reaches the declaration; omitted parts are zero-filled. Without an initializer, its value is unspecified.

**Static local variables** use `static var` in a function body, with the same syntax but exactly one variable name. Like globals, they reside in the data region, exist for the entire lifetime of the program, and retain their values between calls. Their names are visible only within their declaring blocks. Their initializers follow the rules for global variables.

**Address-mapped variables** (`at`) provide a global name for an object of type T at a specified address. Such a declaration allocates no storage and has no initializer. T may be a scalar, array, or structure. Every access through the variable becomes a memory access at that address (9.7). This makes `at` suitable for device registers and the interrupt vector table.

### 5.4. Structures

```ebnf
StructureDeclaration = "struct" IDENT "{" { FieldList EOS | EOS } "}" .
FieldList = IDENT { "," IDENT } ":" Type .
```

Fields are separated by `EOS` tokens (newlines or semicolons). A field may have any type except a function type, including `ptr S` where S is the structure being declared. Field layout is defined in 4.4. Defining a structure more than once or repeating a field name is an error.

### 5.5. Functions

```ebnf
FunctionDeclaration = [ "static" | "export" | "extern" ] [ "interrupt" ] "fn" IDENT "(" [ Parameters ] ")" [ ":" Results ] ( Block | EOS ) .
Parameters = ParameterGroup { "," ParameterGroup } .
ParameterGroup = IDENT { "," IDENT } ":" Type .
```

The `Results` nonterminal is defined in 4.5 (a list of types, with or without parentheses). The `static`, `export`, `extern`, and `interrupt` modifiers are part of the function declaration; permitted combinations are specified below.

- Parameter and result types must be scalar. `a, b: int` declares two `int` parameters.
- A function may return at most four words of results (`long` and `ulong` occupy two words each; all other scalar results occupy one). Thus there may be 0–4 results, with no more than two double-word results.
- A declaration without a body is a **prototype**. A function may have at most one definition with a body and any number of prototypes. Prototypes and the definition must agree on parameter and result types and on the `interrupt` and `static` modifiers.
- The `export` modifier makes a function an entry point. An exported function must be defined within the program: an `export` prototype without a definition is an error. It is always included in the image, serves as a root for reachability analysis (3.5), and retains its unmodified assembly name (10.3), so assembly modules can call it. This modifier applies to all target modes, including programs that define `main`, and need only appear on one of the function's declarations. `export` may be combined with `interrupt`, but not with `static`.
- A function declared only through prototypes must be defined outside the B16 program, for example in an assembly module (10.4).
- The `extern` modifier declares a function defined outside B16. An `extern fn` declaration is always a prototype: providing it with a body is an error, as is defining the same function with a body anywhere in the B16 program, before or after its `extern` declaration. It is enough to mark any one declaration. A prototype without `extern` and without a definition in the program is permitted, but the implementation warns about it (12.2), helping detect a missing function body. `extern` applies only to functions, including `interrupt fn` (for an assembly-language handler whose address the program stores in the vector table). Combining `extern` with `static` or `export` is an error. Assembly names for external functions follow 10.3.

### 5.6. Interrupt handlers

`interrupt fn` declares an interrupt handler, which has no parameters or results. Calling a handler as an ordinary function is an error. Its address has type `ptr interrupt fn()` (4.5); this address can be obtained from its name and stored in the interrupt vector table. Section 9.5 describes how handler code differs from ordinary function code.

### 5.7. Translation-time assertions

```ebnf
Assertion = "assert" ConstantExpression [ "," STRING ] EOS .
```

A top-level `assert` evaluates its constant expression during translation. If the value is zero, translation fails with an error, using the supplied string as the message if present. Assertions are useful for layouts defined by external formats, such as FS16 directory entries, CX16 headers, and the PIB:

```b16
assert sizeof(DirEntry) == 64
assert offsetof(Header, bss_end) == 14, "CX16 header"
```

Names in the assertion expression must have been declared earlier in the source (3.3).

### 5.8. File-local visibility

The `static` modifier on a function or global variable (including an `at` variable) restricts its name to the source file containing its declaration, **not** the entire translation unit. Within that file, the `static` name shadows a global name from another file. Two `static` names in different files may be identical; a file-local `static` name may also match a global name from another file. Two declarations with the same name in one file are not permitted. `static` applies only to `fn` and `var`. In assembly, these names take the form `file.name` (10.3).

## 6. Expressions

### 6.1. Precedence and associativity

| Level | Operators | Associativity |
| --- | --- | --- |
| 1 (highest) | `f(...)` `a[i]` `s.f` `x++` `x--` | Left to right |
| 2 | Unary `-` `~` `!` `*` `&` `++x` `--x` | Right to left |
| 3 | `*` `/` `%` | Left to right |
| 4 | `+` `-` | Left to right |
| 5 | `<<` `>>` | Left to right |
| 6 | `&` | Left to right |
| 7 | `^` | Left to right |
| 8 | `\|` | Left to right |
| 9 | `==` `!=` `<` `>` `<=` `>=` | Nonassociative |
| 10 | `&&` | Left to right |
| 11 | `\|\|` | Left to right |
| 12 (lowest) | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | Right to left |

Bitwise operators bind more tightly than comparisons: `x & M == 0` means `(x & M) == 0`. Chained comparisons such as `a < b < c` are errors.

### 6.2. Values, lvalues, and decay

An **lvalue** designates an object in memory or a register: a variable name (local, global, or `at`), `*p`, `a[i]`, or `s.f`. All other expressions are rvalues. Assignment, `++`, and `--` require a scalar lvalue; `&` applies to any lvalue.

In an expression, an array value decays to a pointer to its first element; a function name decays to a function pointer. Byte-typed values are promoted to word types (4.1). For every operator except `&`, `.`, and `sizeof`, operands must be scalar after decay; otherwise translation fails.

**Implicit assignment** of a value to an object of type T occurs in assignment expressions, initialization, argument passing, result return, and multiple assignment. Integer types are mutually assignable except when narrowing: a `long` or `ulong` cannot be assigned to a `byte`, `char`, `uint`, or `int` without an explicit cast (4.6). Assignment from a narrower integer type to `long` or `ulong` extends according to the source's signedness: `byte` and `uint` are zero-extended; `char` and `int` are sign-extended. `long` and `ulong` can be assigned to each other without changing their bit representations. Storing to a byte object writes only the low byte. Pointers cannot be assigned to `long` or `ulong`, or vice versa. Pointer assignments obey 4.2: a pointer may be assigned only to a compatible pointer type; an integer may be assigned to a pointer only when it is the constant zero; a pointer may never be assigned to an integer. Violations are errors.

### 6.3. Primary expressions

```ebnf
Primary = NUMBER | CHAR | STRING | IDENT | "(" Expression ")"
        | "sizeof" "(" Type ")" | "offsetof" "(" IDENT "," IDENT { "." IDENT } ")"
        | Cast | BuiltinCall .
```

- A **name** refers to a local variable, global variable, constant, or function. An unknown name is an error.
- A **string** denotes an unnamed `[n+1]byte` array in the data region that exists throughout program execution. Its contents can be modified. Identical string literals are distinct objects.

### 6.4. Signedness and result types

Each scalar type is signed (`int`, `char`) or unsigned (`uint`, `byte`, pointers).

**Signedness rule.** The operands of `/`, `%`, `<`, `>`, `<=`, and `>=` must have matching signedness. An untyped constant takes the signedness of the other operand. One exception is allowed: an unsigned operand no wider than a word (`byte`, `uint`, or pointer) may be combined with a signed double-word operand (`long`); the unsigned value can be extended without loss, and the operation is signed. Other combinations are errors, including `int` with `ulong` and `long` with `ulong`. The operation uses the common signedness. For `>>`, only the left operand determines signedness. Other operations do not depend on signedness, and mixed-signedness operands are permitted.

**Result type** of a binary `+ - * / % & | ^` operation:

1. If either operand is double-word, the result is double-word: it takes the left operand's type if both are double-word, or the double-word operand's type otherwise. The other operand is extended according to its own signedness (4.1, 6.2); an untyped constant adopts the double-word type.
2. Otherwise, if the left operand is an untyped constant, the result has the right operand's promoted type.
3. Otherwise, the result has the left operand's promoted type.
4. If this yields a pointer type and the operation is not pointer arithmetic (6.6), the result type is `uint`.

An operation with one double-word operand and one narrower operand is performed at double-word width. Thus `uint(a) * uint(b)` yields a 16-bit product, whereas `ulong(a) * ulong(b)` yields the full 32-bit product.

For `<<` and `>>`, the result has the left operand's promoted type; the right operand may be any integer type, and if it is double-word only its low word is used. An untyped constant to the left of a shift with a nonconstant count has type `int` (use a cast such as `ulong(1) << n` for a 32-bit shift). Comparisons, `!`, `&&`, and `||` yield `int`.

### 6.5. Arithmetic and bitwise operators

Operations on values no wider than a word use 16-bit words and produce results modulo 2^16. Operations on double-word values use 32 bits and produce results modulo 2^32. Overflow is not detected.

| Operation | Meaning |
| --- | --- |
| `-x` | Two's complement: 0 − x |
| `~x` | Bitwise complement |
| `!x` | 1 if x = 0, otherwise 0 |
| `a * b` | Low 16 (or, for double-word operands, 32) product bits; identical for signed and unsigned operands |
| `a / b` | Quotient; signed division truncates toward zero |
| `a % b` | Remainder; for signed division, the remainder has the dividend's sign and a = (a / b) · b + a % b |
| `a + b`, `a - b` | Sum, difference |
| `a & b`, `a \| b`, `a ^ b` | Bitwise AND, OR, XOR |
| `a << n` | Left shift |
| `a >> n` | Right shift: logical for unsigned a, arithmetic for signed a |

**Division by zero.** For unsigned word division, `a / 0` yields $FFFF and `a % 0` yields a (processor-dependent behavior). For unsigned double-word (`ulong`) division, `a / 0` yields $FFFFFFFF and `a % 0` yields a on every processor. Signed division or remainder by zero is undefined behavior.

**Minimum value divided by −1.** For `long`, `-2147483648 / -1` yields −2147483648 and `-2147483648 % -1` yields 0 (wraparound, not undefined behavior).

**Shifts.** If n is constant and n ≥ 16, `<<` and unsigned `>>` yield zero; signed `>>` yields 0 or −1 according to the sign of a. When n is evaluated at runtime, its low four bits determine the count (shift by n mod 16); for a double-word left operand, its low five bits are used (shift by n mod 32). If the left operand is double-word and n is a constant ≥ 32, `<<` and unsigned `>>` yield zero; signed `>>` yields 0 or −1 according to the sign of a. The implementation warns when a constant shift count is at least the operand width (12.2).

### 6.6. Pointer arithmetic

Let p and q have type `ptr T`, i be an integer, and s = sizeof(T):

| Expression | Value | Type |
| --- | --- | --- |
| `p + i`, `i + p` | Address p + i · s | `ptr T` |
| `p - i` | Address p − i · s | `ptr T` |
| `p - q` | (p − q) / s, signed | `int` |
| `p + q` | Error | — |

For `p - q`, the pointers must have the same type and s must be a power of two; otherwise translation fails. These operations are invalid for untyped pointers or function pointers (4.2). Pointers are compared as unsigned integers. Other operations on pointers act on their 16 address bits and yield `uint`.

### 6.7. Comparisons and logical operators

A comparison yields 1 if the relation holds, otherwise 0. The operands are converted to a common width: if either operand is double-word, the other is extended to double-word width according to its own signedness (6.4). For `==` and `!=`, the resulting bit patterns are compared. Double-word values are compared as complete values. A condition involving a double-word value is true if the two words together are nonzero, i.e., if the value is not zero. Two pointers may be compared only if their types match or either is an untyped `ptr` (except for comparisons with function pointers). A pointer may always be compared to the constant zero but not to another integer. In `a && b`, b is evaluated only when a is nonzero; in `a || b`, b is evaluated only when a is zero. Both operators yield 0 or 1.

### 6.8. Pointers, subscripts, and fields

- `*p` designates the object of type T referenced by `p` of type `ptr T`. Other operand types, or a function type T, are errors.
- `&x` yields the address of lvalue x, of type `ptr T`. For a function f, `&f` equals f; `&*p` equals p. For an `at` variable v, `&v` yields its address as a `ptr T` constant. Taking the address of a local variable is allowed: the variable resides in memory (9.4).
- `a[i]` means `*(a + i)`. Here a is an array or pointer, and i is an integer no wider than a word (not a pointer); `long` and `ulong` indices must be explicitly cast, as in `a[uint(i)]`. The same requirement applies to `p + i`, `p - i`, `p += i`, and `p -= i`. An untyped constant used for indexing or pointer arithmetic takes type `int`. Function types cannot be element types. Array bounds are not checked.
- `s.f` accesses field f of structure s. When s is a pointer to a structure, dereferencing is automatic: `p.f` is equivalent to `(*p).f`.

### 6.9. Assignment, increment, and calls

**Assignment** `l = e` stores the value of e into l and itself evaluates to the stored value (truncated for a byte-typed lvalue), with the type of l. A compound assignment `l op= e` is equivalent to `l = l op e`, except that l is evaluated only once. Pointer `+=` and `-=` apply the scaling defined in 6.6; `i += p`, where i is an integer and p a pointer, is an error.

**Increment and decrement.** `++x` and `--x` change x by one (or by the element size for a pointer) and yield the new value. `x++` and `x--` yield the old value.

**Call** `f(e1, ..., en)`: f must be a function name or an expression of type `ptr fn(...)`. The number of arguments must equal the number of parameters, and every argument must be scalar and assignable to its parameter type (6.2). A byte parameter truncates its argument to a byte. The value of a call is its first result; using a call to a function with no results as a value is an error. Calling an interrupt handler is an error.

**Evaluation order** of operands and arguments is unspecified, except for `&&` and `||`. Reading and modifying the same object within one expression without an intervening `&&` or `||` (for example, `a[i] = i++`) is undefined behavior.

### 6.10. Constant expressions

A constant expression consists of untyped constants, casts of constants, and operations on them, excluding assignments, increments, and calls. Untyped constants are evaluated exactly (4.7); typed constants follow the rules of 6.5. Division by zero in a constant expression is an error.

A constant expression is required in a `const` declaration, an `assert`, an array bound, an `at` address, and the argument to `trap`. In other contexts, constant subexpressions are also evaluated at translation time.

## 7. Statements

```ebnf
Statement = Block | [ "static" ] VariableDeclaration | ExpressionStatement | If | While | Loop
          | Break | Continue | Return | MultipleAssignment
          | Label | Goto | Atomic | Asm .
Block = "{" { Statement | EOS } "}" .
```

Statements execute in sequence. A simple statement ends with `EOS` or appears immediately before `}`. The body of an `if`, `else`, `while`, `loop`, or `atomic` must always be a brace-enclosed block. Conditions are written without parentheses.

### 7.1. Blocks

A block introduces a scope for local names (5.1). An empty block `{}` is permitted.

### 7.2. Variable declarations

Inside a function, `var` declares local variables (5.3), while `static var` declares a static local variable. A `var` declaration with an initializer performs the assignment (or fills the object, for an array or structure) when execution reaches that declaration. Jumping over a declaration with `goto` is permitted; in that case the variable remains uninitialized. A local variable inferred from a string initializer has pointer type and points to the literal (5.3); mutable text requires an explicitly typed array.

### 7.3. Expression statements

```ebnf
ExpressionStatement = Expression EOS .
```

The expression is evaluated and its result discarded. Typically it is an assignment, an increment, or a call. Calling a function with no results is permitted as a statement. If the statement is not an assignment, increment, decrement, or call, the implementation warns that its value is unused (12.2). This helps detect a line break such as `x = a` / `- b`, where the second line has inadvertently become a separate statement.

### 7.4. `if`

```ebnf
If = "if" Expression Block [ "else" ( If | Block ) ] .
```

The condition must be scalar. If it is nonzero, the first block executes. Otherwise the `else` branch executes, if present.

### 7.5. Loops

```ebnf
While = "while" Expression Block .
Loop = "loop" Block .
Break = "break" EOS .
Continue = "continue" EOS .
```

`while` tests the condition before each iteration and executes its block while the condition is nonzero. `loop` executes its block indefinitely. `break` exits the innermost enclosing loop. `continue` transfers control to the condition test of the innermost enclosing `while`, or to the start of the innermost enclosing `loop`. Using `break` or `continue` outside a loop is an error.

### 7.6. Labels and `goto`

```ebnf
Label = IDENT ":" .
Goto = "goto" IDENT EOS .
```

A label is an identifier followed by `:` on the same line, in statement position. A statement may follow the label on the same line or the next line. A label may also be the last item in a block.

- Labels are visible throughout their containing function, including before their definitions. Labels in different functions are independent.
- Defining a label twice in one function, or jumping to an undefined label, is an error.
- `goto L` transfers control to the statement following label L. Jumps into or out of ordinary blocks and loops are permitted.
- Jumping into an `atomic` block from a `goto` outside that block is an error. Jumps out of `atomic` blocks are permitted: the interrupt state is restored for each block exited (7.9).

### 7.7. `return`

```ebnf
Return = "return" [ Expression { "," Expression } ] EOS .
```

The number of expressions must exactly match the number of function results. `return` terminates the function and returns those values; byte-type results are truncated. A function with no results may fall through the end of its body. It is an error for a function with results to have a control-flow path that reaches the end of its body. End-of-function reachability is determined from control flow without evaluating nonconstant conditions: a `loop` with no `break`, or `while 1`, cannot reach the end, while an `if` without an `else` can.

### 7.8. Multiple assignment

```ebnf
MultipleAssignment = Target "," Target { "," Target } "=" Call EOS .
Target = ExpressionWithoutAssignment | "_" .
```

`ExpressionWithoutAssignment` is an expression with no top-level assignment (defined in Section 11); otherwise, in `a, b = f()`, the target `b = f()` would be parsed as an assignment.

The right-hand side must be a function call. The left-hand side contains 2–4 scalar lvalues or `_` placeholders. Function results are assigned to targets in order; `_` discards the corresponding result. There cannot be more targets than results; additional results are discarded. A call through a function pointer returns the number of results specified by the function type (4.5). Each result is assigned to its target under the rules of 6.2.

### 7.9. `atomic`

```ebnf
Atomic = "atomic" Block .
```

Maskable interrupts are disabled for the duration of the block. Upon every exit—falling through the end, `break`, `continue`, `return`, or `goto`—the IF flag is restored to its value before entry. `atomic` blocks may be nested; leaving an inner block restores the outer block's state, in which interrupts remain disabled.

### 7.10. `asm`

```ebnf
Asm = "asm" "{" AssemblyText "}" .
```

Text enclosed in braces is passed to the assembler; nested pairs of `{}` are permitted. The opening `{` must be on the same line as `asm`. Section 10 defines the text format and how assembly interacts with compiler-generated code.

## 8. Built-in Functions

```ebnf
BuiltinCall = BuiltinName "(" [ Expression { "," Expression } ] ")" .
BuiltinName = "mulhu" | "ror" | "xchb" | "di" | "ei" | "wfi" | "trap" .
```

A built-in compiles to a single processor instruction without a function call. A built-in name without parentheses does not refer to a function (2.4), so its address cannot be taken. An incorrect argument count is an error.

| Built-in | Result | Effect | Instruction |
| --- | --- | --- | --- |
| `mulhu(a, b)` | `uint` | High 16 bits of the unsigned product a · b | `MULHU` |
| `ror(x, n)` | `uint` | Rotate x right by n mod 16 bits | `ROR` |
| `xchb(p, v)` | `uint` | Atomically writes the low byte of v to address p and returns the previous byte, zero-extended; p must point to a byte type | `XCHB` |
| `di()` | None | Disable maskable interrupts (IF = 0) | `CLI` |
| `ei()` | None | Enable maskable interrupts (IF = 1) | `STI` |
| `wfi()` | None | Wait for an interrupt | `WFI` |
| `trap(n)` | None | Software interrupt with vector n; n must be a constant expression in 0…15 | `TRAP` |

Using a call to `di`, `ei`, `wfi`, or `trap` as a value is an error. The low 16 bits of a product are available through `*` (6.5). Both arguments to `mulhu` and `ror`, and the second argument to `xchb`, must be integers; pointers are not accepted in those positions.

## 9. Execution Model and Application Binary Interface

This section is normative for code that interacts with a B16 program at the machine level, including assembly modules, interrupt handlers, and debuggers.

### 9.1. Memory Layout

Programs are built in the CX16 executable-file format (*CPU16 Monitor: Executable File Format and Program Startup*, Section 3). The image is laid out in ascending address order:

| Address range | Contents |
| --- | --- |
| `$0000`–`$001B` | CX16 header (9.2) |
| Starting at `$001C` | Startup code, followed by function code in source order, then runtime routines (9.6) |
| Immediately after the code | Initialized global variables in declaration order, followed by string literals |
| Thereafter | `.align 2`, the `__image_end` label, uninitialized global variables, and the `__bss_end` label |
| Below the top of the stack | Downward-growing stack |

Uninitialized variables (BSS) are not stored in the file. They are zeroed by the Monitor loader or, on a bare machine, by the startup code. The stack top is `top` under Monitor and `$FF00` on a bare machine. On a bare machine, `$FF00`–`$FF1F` holds the interrupt vector table, while `$FF20`–`$FFFF` contains device ports.

If a separate data start address is specified (`option data`, 3.6), **all** global variables, initialized and uninitialized, reside at that address and above. Initial values for initialized variables are kept in a **data initialization template** alongside the code; the startup code copies that template to the data area. String literals remain alongside the code. This makes it possible to execute a program from ROM.

`__image_end` and `__bss_end` are predefined names of type `ptr`: they denote the end of the image and the end of static data, respectively. The heap starts at `__bss_end`; on a bare machine, this address also indicates the boundary toward which the stack grows.

Every object in the data area occupies an even number of bytes and starts at an even address. Word-sized objects, word-sized fields, and the stack are always word-aligned. Stack overflow is not detected.

### 9.2. Startup Code

When a program defines `main`, the image begins with a CX16 header and startup code:

```asm
__header:
        JMP   #__start
        .ascii "CX16"
        .word 1, 0                    ; version, flags
        .word __image_end, __bss_end
        .word min_free, max_top        ; option min_free, max_top; defaults: 2048 and 0 (3.6)
        .word 0, 0, 0, 0
__start:
        LD    R4, [$0006]             ; "CXPB": Monitor; "CX16": bare machine
        CMP   R4, #$4250
        JZ    @monitor
        ...                           ; bare machine: SP, vectors, BSS, argc = argv = 0
        LD    LR, #__halt
@monitor:
        PUSH  LR
        ...                           ; copy the data initialization template, if present
        CALL  main
        LD    R0, #0                  ; if main has no return value
        POP   IP
```

The same image supports two startup modes (*CPU16 Monitor*, Section 8).

- **Under Monitor**, address `$0004` contains a PIB with the `"CXPB"` signature. The stack, timer, and interrupt vectors are owned by Monitor; BSS has already been cleared; R0 = argc, R1 = argv, and LR is the exit routine. Startup code does not set SP, touch the timer or interrupt vectors, or execute `CLI` or `WFI`. It saves LR, calls `main` with argc and argv, and terminates by branching to LR with the exit status in R0.
- **On a bare machine**, address `$0004` contains the image's own header (`"CX16"`). Startup code sets SP to `$FF00`, installs `__fault` (which halts the machine) as the ERRCMD handler, points all other vectors at the no-op `__irq_ignore` handler (`IRET`), and clears BSS because memory contents after reset are unspecified. It calls `main` with argc = 0 and argv = 0. When `main` returns, `__halt` disables the timer, acknowledges any pending interrupt request (`STI`, followed by `CLI`), and halts the processor with `WFI`. Acknowledging that request is essential: disabling the timer does not clear IRQ0 if it was asserted while IF = 0. In that case, `WFI` with IF = 0 would wake repeatedly without entering a handler, preventing the machine from remaining halted. Because unused vectors point to `__irq_ignore`, the pending request can be acknowledged by that handler. `__fault` uses the same halt sequence.

On entry to `main`, all registers except R0, R1, and SP have unspecified values. The entry interrupt state depends on the startup mode: IF = 1 under Monitor and IF = 0 on a bare machine.

### 9.3. Calling Convention

| Resource | Purpose | Preservation |
| --- | --- | --- |
| R0–R3 | First four argument and result slots | Caller-saved |
| R4–R11 | Variables | Callee-saved |
| LR | Return address, set by `CALL` | Not preserved |
| SP | Stack pointer | Must be restored |
| FLAGS | Condition flags | Not preserved (the function must not change IF) |

1. **Slots.** An argument of type `byte`, `char`, `uint`, `int`, or pointer occupies one slot. `long` and `ulong` occupy two consecutive slots, with the low word first and the high word second. Slots are numbered from zero in left-to-right argument order. Slots 0–3 map to R0–R3. An argument is passed in registers only if **all** its slots fall within 0–3. Otherwise, that argument and all subsequent arguments are passed on the stack, even if some argument registers remain unused. For example, in `fn(a, b, c: uint, d: long, e: uint)`, both d and e are passed on the stack.
2. **Stack.** Stack arguments are pushed right to left; each occupies one or two words. At function entry, the first stack argument is at SP, with subsequent arguments at higher addresses. In a two-word argument, the low word occupies the lower address. The caller removes stack arguments after the return.
3. **Results.** Return values use slots 0–3 under the same rules, with a combined maximum of four words. Result *i* is returned in the registers corresponding to its slots (for `long`, the low word goes in Rk and the high word in Rk+1).
4. The caller extends a byte-sized argument; the callee extends a byte-sized result. A byte-sized value held in a register is always extended: zero extension for `byte`, sign extension for `char`.
5. A function call uses `CALL`; returning means branching to the address in LR. A function that calls other functions saves LR on the stack.

For example, `fn f(a: uint, b: long, c: uint): ulong` receives a in R0, b in R1 (low word) and R2 (high word), and c in R3; it returns its result in R0 (low word) and R1 (high word).

### 9.4. Function Stack Frame

A function that must preserve registers or allocate stack storage constructs a frame:

```asm
        PUSH  LR              ; if the function calls other functions
        PUSH  Rk ...          ; used R4..R11, in ascending register order
        SUB   SP, #size       ; local-variable storage, if required
```

It dismantles the frame in reverse order. The final instruction of a non-leaf function restores the saved LR directly into IP (`POP IP`); a leaf function executes `RET` (`LD IP, LR`).

In ascending address order from SP, the local-variable area holds variables that could not be assigned registers: local arrays and structures, address-taken variables, and variables exceeding the available registers. A `long` or `ulong` variable occupies two registers (not necessarily adjacent), or four bytes within the frame. Each variable occupies an even number of bytes. Saved registers follow above the variable area, then LR, then stack arguments. There is no frame pointer.

### 9.5. Interrupts

On entering an interrupt handler, the processor pushes IP and FLAGS onto the stack and disables interrupts. A handler declared as `interrupt fn`:

- Saves every R0–R11 register it uses;
- Also saves R0–R3 and LR if it calls functions;
- Returns with `IRET`, restoring FLAGS (including IF) and IP.

A handler is installed by writing its address into the interrupt vector table, for example through an `at` variable:

```b16
var VEC at $FF00: [16]ptr interrupt fn()
interrupt fn on_tick() { ticks++ }
...
VEC[0] = on_tick
```

Vectors 0–7 are IRQ0–IRQ7; vector 8 denotes an invalid instruction (ERRCMD); vectors 9–15 are available for `trap(n)`. Under Monitor, interrupt vectors belong to Monitor and must not be written by the program.

### 9.6. Runtime Routines

Signed word division `/` and remainder `%` with a divisor that is not a constant (or with the constant divisor −32768), as well as double-word operations that cannot be generated inline, are implemented through runtime routine calls. The reference implementation adds these routines to the image as needed:

| Routine | Operation |
| --- | --- |
| `__divs` | R0 = R0 / R1 (signed, rounded toward zero) |
| `__mods` | R0 = R0 % R1 (sign follows the dividend) |
| `__mul32` | R0:R1 = R0:R1 · R2:R3 (low 32 bits, identical for signed and unsigned operands) |
| `__divu32`, `__modu32` | R0:R1 = R0:R1 / R2:R3 and R0:R1 % R2:R3, unsigned; division by zero: 6.5 |
| `__divs32`, `__mods32` | The corresponding signed operations; quotient rounded toward zero, remainder sign follows dividend |
| `__shl32`, `__shr32`, `__sar32` | Shift R0:R1 by R2 mod 32 bits: left, logical right, or arithmetic right |
| `__udivc32`, `__umodc32` | Unsigned R0:R1 / R2 and R0:R1 % R2 for a constant divisor 3…255: four `DIVU`/`REMU` steps on the dividend bytes; the quotient is in R0:R1 and the remainder in R2 (`__udivc32`) or in R0:R1 (`__umodc32`) |

For a double-word value in R0:R1, R0 holds the low word and R1 the high word. Every routine follows the calling convention (9.3) and has the same signature as `fn(a, b: long): long` (for shifts: `fn(a: long, n: uint): long`). A function using such an operation is treated as a function that calls other functions (9.3, item 5). Division and remainder by other constants are generated inline: powers of two use shifts and masks; signed word division by other constants uses reciprocal multiplication via `MULHU`. Multiplication by a power of two is implemented as a shift.

The following double-word operations are generated inline: `+`, `-`, unary negation, `~`, `&`, `|`, `^`, comparisons, and shifts by constants. The product of two `byte` or `uint` values converted to `ulong` (`ulong(a) * ulong(b)`) is calculated with `MUL` and `MULHU`, without a runtime call.

### 9.7. Memory Accesses

- Each occurrence of a global variable or an `at` variable within an expression causes a separate memory access at the point when that occurrence is evaluated. Global values are not retained in registers across accesses. Compound assignment and increment read and write the object once each.
- A word-sized object at an aligned address is read or written with a single instruction, so the access cannot be interrupted. Reading or writing a `long` or `ulong` object takes two instructions, accessing the low word first. An interrupt can occur between them. Therefore, when an interrupt handler also accesses or modifies such an object, read and write it within an `atomic` block. This also applies to `at` objects: the low word is accessed before the high word. Compound assignment and increment of a global variable (`g++`) comprise multiple instructions; if an interrupt handler modifies the same variable, the operation must be performed in an `atomic` block.
- The number and ordering of accesses to local variables are unspecified.

## 10. Inline Assembly

### 10.1. Block Contents

Each nonempty line in an `asm` block is passed to the assembler as a program line, after stripping any `;` comment through the end of that line. A line may begin with a label, subject to these rules:

- Only local labels of the form `@name` are permitted; they are scoped to the enclosing function.
- Labels starting with `@L` followed by digits are reserved for the implementation.
- A global label (without `@`) is an error.

The compiler does not validate the instructions in an `asm` block; the assembler reports errors in them.

### 10.2. Interaction with Generated Function Code

An `asm` block is a statement. The following rules apply on entry and exit:

1. Condition flags are treated as unknown after the block.
2. The compiler assumes that register-allocated variables retain their values across the block. If the block modifies a register allocated to a variable, that variable takes on the new value.
3. There are no live intermediate expression values between statements, so registers not allocated to variables are available inside the block. The block itself must preserve any R4–R11 registers it uses that are not allocated to variables by the compiler.
4. If the block text contains the `CALL` mnemonic outside comments, the containing function is treated as calling other functions: the compiler saves LR and allocates variables in R4–R11. Consequently, R0–R3 are free inside an `asm` block in such a function, and functions called from the block may clobber them.
5. Names of global variables and functions referenced in the block are available as assembler symbols (10.3), and references to functions make them reachable (3.5). Names of `static` objects from the current source file, or visible static local variables, are replaced in the block text by their assembler names (`helper` → `util.helper`).
6. If an `interrupt fn` contains no statements other than `asm` blocks without `CALL`, the compiler does not save any registers. On entry to the block, `[SP]` contains saved FLAGS and `[SP+2]` contains the return address. The block must save and restore any registers it modifies.
7. If a function body starts with an `asm` block, R0–R3 contain its arguments as specified in 9.3 when that block is entered; the function prologue does not modify R0–R3. This also holds for parameters not used by B16 code: no variable register is allocated for them, and their original R0–R3 contents remain intact.

The language does not specify register allocation for variables. The reference implementation guarantees only two cases (13.6): in a function that makes no calls, a parameter resides in the registers corresponding to its argument slots (9.3; a `long` parameter occupies Rk and Rk+1); in a function that makes calls, the sole variable **used by B16 code** resides in R4, or, for a sole `long` or `ulong` variable used by B16 code, in R4 (low word) and R5 (high word). A variable referenced only from `asm` does not receive a variable register.

### 10.3. Assembler Symbol Names

Each function, global variable, string literal, runtime routine, and startup-code component is a separate object with a global label. Labels within functions are local, so a function appears as one object in the assembler memory map.

| B16 object | Assembler symbol |
| --- | --- |
| Function or global variable | Its name |
| Name identical to a register or assembler mnemonic | `__` + name |
| `static` function or variable | `file.name` |
| Static local variable | `function.name` (`function.name.2` … for duplicates) |
| String literal | `owner.s<n>` (the function or variable in which it appears) |
| Local-object initialization template | `function.i<n>` |
| Labels within functions | `@L<n>` |
| Runtime | `__divs`, `__mods`, `__mul32`, `__divu32`, `__modu32`, `__divs32`, `__mods32`, `__shl32`, `__shr32`, `__sar32`, `__udivc32`, `__umodc32` |
| Header and startup code | `__header`, `__start`, `__halt`, `__fault`, `__irq_ignore` |
| Data boundaries | `__data_start`, `__image_end`, `__bss_start`, `__bss_end`; template: `__data_init` |

`file` is the source file's basename without its directory or extension. Characters outside `[A-Za-z0-9_]` are replaced by `_`, and `_` is prepended if the basename begins with a digit. Duplicate file prefixes are an error. An identifier containing a dot cannot be mistaken for a mnemonic or register.

**Constants.** The compiler emits `.equ` definitions for use by assembly code: every `const` with its value, every `at` variable with its address, and, for every structure S, `S.field` for each field offset and `S.sizeof` for the structure size. Assembly that uses these names needs no manual changes when the structure definition changes in B16; it is rebuilt with updated constants.

Mnemonic matching is case-insensitive. Reserved assembler names include registers `R0`–`R15`, `LR`, `SP`, `IP`, `FLAGS`; instruction, conditional branch, and pseudo-instruction mnemonics; and `lo` and `hi`. For example, a B16 function named `neg` is named `__neg` in assembly.

### 10.4. External Modules

A function defined in an assembly module is declared in B16 with `extern fn` (5.5) and must obey the calling convention (9.3). Assembly code may call B16 functions by name (subject to 10.3) or through pointers. A B16 function referenced only by an external assembly module must be marked `export` (5.5); otherwise it is not reachable (3.5) and will not be included in the image.

**Including a module in an image.** Use `include asm "file"` (3.2) in any source file, usually next to the corresponding `extern fn` declarations. The implementation also accepts `--asm file` (13.2). The compiler emits `.include "file"` after functions and runtime routines but before data (13.3); consequently the module's code and initialized data (`.word`, `.byte`) are within the image before `__image_end` and are loaded by Monitor. Modules may contain only code and initialized data. `.reserve` is not allowed: before `__image_end`, a reserved region would be emitted as zero bytes in the file rather than as BSS. Declare uninitialized data in B16 instead. Including the assembly module *after* the compiler's output (`.include "prog.s"`, followed by `.include "mod.asm"`) puts its code beyond `__bss_end`: it works on a bare machine, but a call under Monitor branches into zero-filled memory, because Monitor only loads file bytes preceding `__image_end`. `cpu16asm --exec` rejects this arrangement and reports the address of the first byte beyond `image_end`.

## 11. Consolidated Grammar

The grammar describes the token stream after `include` expansion and `EOS` insertion (2.3); `include asm` directives (3.2) do not appear in that stream. Constraints that the grammar cannot express (types, lvalues, constant-expression requirements) are specified in Sections 4–8. Nonterminal definitions are the same as in the main text, except that `Primary` and `Statement` are expanded here: `Cast`, `BuiltinCall`, and statements such as `If` and `While` are incorporated directly, and a built-in function call has the same grammar as an ordinary call.

```ebnf
Program       = { Declaration | EOS } .
Declaration      = ConstantDeclaration | [ "static" ] VariableDeclaration | StructureDeclaration
                | FunctionDeclaration | Assertion | ImageOption .

ConstantDeclaration  = "const" IDENT "=" ConstantExpression EOS .
Assertion        = "assert" ConstantExpression [ "," STRING ] EOS .
ImageOption        = "option" IDENT "=" ( ConstantExpression | IDENT ) EOS .   (* "option" - IDENT, 2.4 *)
VariableDeclaration = "var" NameList ":" Type EOS
                     | "var" IDENT [ ":" Type ] "=" Initializer EOS
                     | "var" IDENT "at" ConstantExpression ":" Type EOS .
NameList      = IDENT { "," IDENT } .
Initializer   = Expression | "{" [ Element { "," Element } [ "," ] ] "}" .
Element         = [ "." IDENT "=" ] Initializer .

StructureDeclaration = "struct" IDENT "{" { FieldList EOS | EOS } "}" .
FieldList     = IDENT { "," IDENT } ":" Type .

FunctionDeclaration = [ "static" | "export" | "extern" ] [ "interrupt" ] "fn" IDENT "(" [ Parameters ] ")"
                    [ ":" Results ] ( Block | EOS ) .
Parameters       = ParameterGroup { "," ParameterGroup } .
ParameterGroup = IDENT { "," IDENT } ":" Type .

Type             = "byte" | "char" | "uint" | "int" | "long" | "ulong" | "ptr" [ Type ]
                | "[" ConstantExpression "]" Type | IDENT
                | "fn" "(" [ Type { "," Type } ] ")" [ ":" Results ]
                | "interrupt" "fn" "(" ")" .
Results      = Type { "," Type } | "(" Type { "," Type } ")" .

Block            = "{" { Statement | EOS } "}" .
Statement        = Block | [ "static" ] VariableDeclaration | Expression EOS
                | "if" Expression Block [ "else" ( If | Block ) ]
                | "while" Expression Block | "loop" Block
                | "break" EOS | "continue" EOS
                | "return" [ Expression { "," Expression } ] EOS
                | Target "," Target { "," Target } "=" Expression EOS
                | IDENT ":" | "goto" IDENT EOS
                | "atomic" Block | "asm" "{" AssemblyText "}" .
If              = "if" Expression Block [ "else" ( If | Block ) ] .
Target            = ExpressionWithoutAssignment | "_" .

Expression       = ExpressionWithoutAssignment [ AssignmentOp Expression ] .
AssignmentOp   = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=" .
ExpressionWithoutAssignment = LogicalOr .
LogicalOr             = LogicalAnd { "||" LogicalAnd } .
LogicalAnd               = Comparison { "&&" Comparison } .
Comparison       = BitwiseOr [ ( "==" | "!=" | "<" | ">" | "<=" | ">=" ) BitwiseOr ] .
BitwiseOr          = BitwiseXor { "|" BitwiseXor } .
BitwiseXor         = BitwiseAnd { "^" BitwiseAnd } .
BitwiseAnd            = Shift { "&" Shift } .
Shift           = Sum { ( "<<" | ">>" ) Sum } .
Sum           = Product { ( "+" | "-" ) Product } .
Product    = Unary { ( "*" | "/" | "%" ) Unary } .
Unary         = ( "-" | "~" | "!" | "*" | "&" | "++" | "--" ) Unary | Postfix .
Postfix     = Primary { "(" [ Expression { "," Expression } ] ")"
                | "[" Expression "]" | "." IDENT | "++" | "--" } .
Primary       = NUMBER | CHAR | STRING | IDENT | "(" Expression ")"
                | "sizeof" "(" Type ")" | "offsetof" "(" IDENT "," IDENT { "." IDENT } ")"
                | ScalarType "(" Expression ")" .
ScalarType    = "byte" | "char" | "uint" | "int" | "long" | "ulong" | "ptr" [ Type ] .
ConstantExpression = ExpressionWithoutAssignment .
```

A built-in function call (Section 8) is a `Primary` consisting of an `IDENT` followed by the `(...)` postfix, where `IDENT` names a built-in function. A label is recognized only when `:` appears on the same line as its `IDENT`.

## 12. Undefined Behavior and Diagnostics

### 12.1. Undefined Behavior

The following have undefined behavior:

- Reading and modifying the same object within one expression without an intervening `&&` or `||` (6.9);
- Signed division or remainder with a zero divisor (6.5);
- Reading a local variable before assigning it, including when a `goto` skips its declaration;
- Access beyond array bounds or through a pointer that does not point to an object of a suitable type;
- Stack overflow;
- Writing to a string literal;
- Modifying an R4–R11 register not allocated to a variable from an `asm` block without restoring it, or modifying SP;
- Calling, through an explicitly cast function pointer, a function whose parameter types or result types differ from those of the pointer type.

### 12.2. Diagnostics

The implementation reports messages in the following format:

```text
file:line: error: message
file:line: warning: message
```

The file and line identify the location in the original source, before `include` expansion. After an error, translation resumes at the next statement or declaration so that several errors can be reported in one run. No output is produced when there are errors. A warning does not stop translation. An implementation may, at the user's request, treat warnings as errors. For `b16c`-specific behavior, see 13.5.

The implementation issues warnings for:

- An unused local variable, parameter, or label. Names beginning with `_` are exempt. Variables and parameters in a function containing `asm` blocks are not checked, because a block may refer to them through registers. Parameters of `main` under the `std` target are also exempt: their signature is fixed by the startup convention (3.4), and a program cannot declare only `argv` while omitting `argc`.
- Reading a local variable that has not been assigned on **any** path (`variable x is used before it is assigned`).
- Reading a local variable that may be unassigned on **some** path (`variable x may be used before it is assigned`) — **only when `-Wmaybe-uninit` is enabled** (13.2). The analysis does not correlate conditions. If a variable is assigned and read under the same condition, the access is safe but may be flagged. An assignment in a loop body is treated as potentially reaching the beginning of the next iteration.
- Comparing an unsigned value with a negative constant.
- A constant shift count greater than or equal to the width of the left operand (16 bits for a word, 32 bits for a double word).
- A discarded expression value (7.3).
- A function prototype lacking a definition, except for functions declared with `extern fn` (5.5).
- An image option overridden by a different value supplied externally through a `b16c` command-line option (3.6, 13.2).
- A `max_top` value exceeding `$E000`: Monitor limits the memory range to that address regardless.

## 13. The `b16c` Reference Implementation

This section documents the reference implementation: compiler invocation; every command-line option and its relationship to image options (3.6); image layout for each target; function selection; diagnostics; code-generation characteristics; and limits. These properties are not part of the language definition, but build scripts and assembly code may rely on them when targeting `b16c`.

### 13.1. Invocation

```sh
b16c [options] prog.b16
```

The compiler accepts one UTF-8 source file, expanding its `include` directives (3.2). It produces assembly source for `cpu16asm` (*Assembler Reference*), writing to stdout by default. Options and the input filename may appear in any order. Diagnostics are written to stderr.

| Exit status | Meaning |
| --- | --- |
| 0 | Translation succeeded; warnings may have been issued |
| 1 | Source errors; if `-o` was specified, the output file is deleted |
| 2 | Invalid command-line options or option values; brief usage information is displayed |

Numeric option arguments (`ADDRESS`, `N`) may be decimal or hexadecimal with a `$` or `0x` prefix. A `$` must be protected with single quotes when passed through a shell, for example `--data '$8000'`.

With no arguments, `b16c` displays a brief option summary.

### 13.2. Command-Line Options

Options fall into two categories. **Image parameters** — target, addresses, header fields, and assembly modules — are essential to running the program. They belong in the source (`option`, 3.6; `include asm`, 3.2). Command-line overrides are intended for one-off builds, such as a test configuration at a different address or an address computed by a build script. **Other options** describe the build environment (`-o`, `-I`), diagnostic policy, or debugging and do not have source-level counterparts.

| Option | Default | Source equivalent | Effect |
| --- | --- | --- | --- |
| `-o file` | stdout | — | Output file |
| `-I directory` | — | — | Include search directory; may be repeated (up to 32) |
| `--target std` | Yes | `option target = std` | CX16 executable (13.3) |
| `--target kernel` | — | `option target = kernel` | Code without a header or startup code (13.3) |
| `--code ADDRESS` | 0 | `option code` | Code start address; `kernel` only |
| `--data ADDRESS` | Immediately after code | `option data` | Start of all global variables; both targets |
| `--min-free N` | 2048 | `option min_free` | CX16 header `min_free` field; `std` only |
| `--max-top N` | 0 | `option max_top` | CX16 header `max_top` field; `std` only |
| `--asm file` | — | `include asm "file"` | Assembly module included in the image before `__image_end` (13.3); repeatable |
| `-n` | — | — | Treat every non-`static` function as an entry point (13.4); deprecated in favor of `export` |
| `-a` | — | — | Emit all functions (13.4) |
| `-s` | — | — | Include source text as comments in the generated output |
| `-Werror` | — | — | Treat warnings as errors |
| `-w` | — | — | Suppress warnings |
| `-Wmaybe-uninit` | — | — | Also warn when a variable may be read before assignment (12.2) |
| `-fcopy-params` | — | — | Always copy parameters into R4–R11 (13.6), for comparison and debugging |

**Command-line options versus `option`.** The effective value is, in order of precedence: the command-line argument, an `option` in the primary source file, or the default. If an argument overrides an `option` with a different value, the compiler warns at the `option` line:

```text
kernel.b16:9: warning: --data $9000 overrides option data = $8000
```

With `-Werror`, this becomes an error, making it easy to detect obsolete command-line overrides after moving image parameters into source files. Identical values do not trigger a warning. Command-line values are validated according to the same rules as `option` values (3.6, rules 4 and 5). An invalid numeric range or alignment in a command-line value causes exit status 2; supplying a parameter not applicable to the selected target is a normal translation error (status 1).

**Assembly modules** supplied with `--asm` are emitted after the modules specified by `include asm`. A module already included from source is not emitted again. The total limit is 16 modules.

**Comments in generated assembly.** The output begins with the effective image parameters and the origin of each: an `option` line, a command-line argument, or the default. Every emitted module `.include` line identifies the originating `include asm` directive or `--asm` option:

```asm
; image parameters:
;   target    kernel           kernel/kernel.b16:7
;   code      $0004            kernel/kernel.b16:8
;   data      $8000            kernel/kernel.b16:9
```

**`-o file`.** Without it, output is written to stdout. On errors, the output file is not created (or an already-created file is deleted). When writing to stdout, some output may already have been emitted when an error is detected.

**`-I directory`.** For `include "file"`, the compiler first searches relative to the including file's directory, then the `-I` directories in the order supplied. Absolute paths are used directly. A resolved path is normalized to remove `./` and `directory/../` segments; this canonical form appears in diagnostics and `.line` directives. Re-including the same canonical file is ignored, as required by 3.2.

**`--target std`** (default; `option target = std`). The program must define `main` (3.4). Otherwise, compilation fails with a suggestion to use `option target = kernel`. The image begins with a CX16 header and startup code (9.2), runs under Monitor or on a bare machine, and passes `cpu16asm --exec` validation.

**`--target kernel`** (`option target = kernel`). The image has neither a header nor startup code. `main`, if present, is an ordinary function whose signature is not checked. This target is intended for the Monitor kernel, modules called from assembly, and ROM code with its own startup prologue.

**`--code ADDRESS`** (`option code`). Sets the starting address of code for the `kernel` target by emitting `.org ADDRESS` before the first function. The default address is zero. `std` code always begins at zero, where the header must reside; using the parameter is an error (`option code applies only to target kernel`, or `--code applies only to target kernel` for the command-line form). The address must be even.

**`--data ADDRESS`** (`option data`). Sets the starting address of *all* global variables, initialized and uninitialized, on either target. Initial values for initialized variables are stored in the `__data_init` template immediately after the code; storage for those variables is reserved at `--data` with `.reserve`. String literals stay alongside the code and are not copied (2.7). On `std`, startup code copies the template to the data area; on `kernel`, the `__init_data` routine does so (13.3). This option supports ROM-resident programs with RAM data and kernel images. The address must be even and no lower than the end of the initialization template (`__image_end`); otherwise the assembler reports an overlap. Under Monitor, the data area must be below `$E000`. Without this option, data immediately follows code and neither a template nor a copy operation is needed.

**`--min-free N`, `--max-top N`** (`option min_free`, `option max_top`). These specify CX16 header fields (*CPU16 Monitor: Executable File Format and Program Startup*, Section 3). `min_free` is the required memory for stack and heap beyond BSS (even, at most `$E000`). `max_top` is the requested upper process-memory boundary, where zero means all available memory. Monitor refuses to start a program if `min_free` cannot be satisfied. Values of `max_top` above `$E000` are capped at that address by Monitor, and the compiler warns. The `kernel` target has no header, so either parameter is an error (`option min_free applies only to target std`).

**`--asm file`** (`include asm "file"`). Includes an assembly module within the image. The compiler emits `.include "file"` after all functions and runtime routines (and, for `kernel`, after `__init_data`) but before data. The file must exist. A path passed through `--asm` is relative to the current working directory; an `include asm` path is resolved according to 3.2. The compiler rebases the path in the emitted `.include` relative to the output file's directory, matching the assembler's lookup rules, so output may be placed in another directory. The option is repeatable; command-line modules follow source-level modules in their specified order. Module code and initialized data lie before `__image_end`; restrictions are specified in 10.4.

**`-n`** (deprecated). Treats all non-`static` functions as reachability roots, as though marked `export` (5.5). On `std`, the header and startup code remain in place. This is only useful while module entry points have not yet been annotated with `export`; the source-level replacement is `export`.

**`-a`.** Emits every function, including unreachable `static` functions, without changing the reachability roots. This is useful for inspecting generated code for all functions, such as while debugging the compiler. The difference from `-n` is explained in 13.4.

**`-s`.** Emits the original source line as a comment before the instructions generated for each statement, and emits each function's signature and preceding comments before the function. If two displayed lines from the same file are separated by no more than eight lines, the intervening lines are included as well. A line that has already been shown is repeated only if its code appears again later (for example, the `while` condition at the bottom of a loop). A source-file change is marked `; ---- file`. Generated instructions are unchanged.

**`-Werror`.** Reports every warning as an `error`; compilation returns 1 and no output file is produced.

**`-w`.** Suppresses warnings. If combined with `-Werror`, warnings are neither displayed nor treated as errors.

**`-Wmaybe-uninit`.** Enables warnings for variables that may be read on a path lacking an assignment (12.2). It is disabled by default because correlated conditions can cause false positives. Warnings for variables unassigned on *every* path are always enabled.

**`-fcopy-params`.** Disables keeping parameters in argument registers (13.6): in functions that make calls, parameters are always copied into R4–R11. Programs must behave identically with or without this option; tests use it for comparison.

Option behavior by target:

| Option | `std` | `kernel` |
| --- | --- | --- |
| `--code` | Error | Start of code |
| `--data` | Data at supplied address, copied by startup code | Data at supplied address, copied by `__init_data` |
| `--min-free`, `--max-top` | Header fields | Error |
| `-n` | All functions become roots; `main` and startup code remain | All functions become roots |
| `--asm` | After functions and runtime | After runtime and `__init_data` |
| `-a`, `-s`, `-I`, `-o`, `-Werror`, `-w`, `-Wmaybe-uninit`, `-fcopy-params` | Same | Same |

### 13.3. Targets and Image Layout

**`std` target.** The memory layout is defined in 9.1, and the header and startup code in 9.2. Objects are emitted in this order:

```text
__header        CX16 header (28 bytes)
__start         startup code
__fault, __halt bare-machine halt paths (ERRCMD and return from main)
__irq_ignore    no-op handler for unused interrupt vectors
functions       in source order
runtime         __divs, __mods, __mul32, and other routines from 9.6, as needed
modules         .include for each module: include asm first, then --asm (10.4)
data            see below
```

**`kernel` target.** Identical, but without the first four objects; `option code` adds a preceding `.org`. Modules are emitted after `__init_data` and before data. If `option data` is specified, the compiler emits the `__init_data` routine after the runtime. This routine copies the `__data_init` template into `__data_start` … `__bss_start`, zeroes `__bss_start` … `__bss_end`, and returns with `RET`, clobbering R0–R2. The kernel's startup prologue must call it before any access to global variables. Without `--data`, BSS (`__image_end` … `__bss_end`) must be cleared by whichever component loads the kernel.

**Data without `option data`:**

```asm
__data_start:            initialized variables (.word, .byte, .space)
                         string literals
        .align 2
__image_end:
__bss_start:             uninitialized variables (.reserve)
__bss_end:
```

**Data with `option data = ADDRESS`:**

```asm
                         string literals (alongside the code)
__data_init:             initialization template for global variables
        .align 2
__image_end:
        .org  ADDRESS
__data_start:            initialized variables (.reserve; populated by copying)
__bss_start:             uninitialized variables (.reserve)
__bss_end:
```

Each variable occupies an even number of bytes. Local-object initialization templates (`function.i<n>`) and static local variables are treated as initialized data. A local-object template is emitted only if its containing function is emitted. Global variables are always emitted, even when unused.

Assembler constants (`.equ`, 10.3) are emitted before the data: first `const` declarations and `at` variable addresses in declaration order, followed by structure field offsets and structure sizes.

### 13.4. Function Selection

The image includes functions reachable from the roots (3.5). The roots depend on the target and options:

| Reachability root | `std` | `kernel` | With `-n` |
| --- | --- | --- | --- |
| `main` | Yes | No | Yes |
| `export` functions | Yes | Yes | Yes |
| `interrupt fn` handlers | Yes | Yes | Yes |
| Functions referenced from global initializers | Yes | Yes | Yes |
| Other non-`static` functions | No | No | Yes |
| `static` functions | No | No | No |

`-a` emits all functions regardless of reachability. Thus, `-n` and `-a` differ only in how they handle unreferenced `static` functions: `-n` discards them, while `-a` emits them. They also have different purposes: `-n` declares module entry points, while `-a` is a debugging option.

Unreachable functions are still parsed and checked: errors and warnings in them are reported, but their code is not emitted.

### 13.5. Diagnostics

See 12.2 for the diagnostic format and warning categories. Diagnostic messages are written in English so they can be displayed in any console.

- **Error recovery.** An error within a statement skips the remainder of the statement up to the end of the line or `}`; an error in a top-level declaration skips the remainder of that declaration. Variables introduced by an erroneous `var` remain declared to avoid cascades of `unknown name` messages. Translation stops after 20 errors with `too many errors`.
- **Check order.** Compilation proceeds in three passes: top-level declarations, function bodies, and code generation. Errors from the first pass (function signatures, `const`, `struct`, `assert`, `option`, array bounds) stop translation after that pass; function bodies are not checked. After pass one, the effective image options are resolved (13.2). Errors for parameters invalid for the target also stop translation. Then `main` is checked for the `std` target. Code-generation errors (for example, `function f can reach the end without return`) are reported only if the first two passes had no errors.
- **Fatal errors** stop translation immediately: end of file within a block, unterminated `asm` block, and errors processing `include` or `include asm` (file not found, nesting beyond 16, more than 16 modules).
- **Variable warnings** are produced after parsing the function, so they may not appear in source-line order.
- **Assembler diagnostics.** `.line` directives (13.6) allow `cpu16asm` to report the corresponding B16 source line for generated assembly, with the `.s` line in parentheses.

### 13.6. Code Generation

- **Register allocation.** Local scalars whose addresses are not taken receive registers according to their weighted access frequency (accesses inside loops have greater weight). A `long` or `ulong` variable occupies two registers, which need not be adjacent. In a function that makes calls, the sole such variable used by B16 code receives R4 and R5. For a function that makes no calls, parameters remain in their argument-slot registers (9.3); at most nine register words are allocated to variables. If the function performs arithmetic on `long` or `ulong` rather than merely copying them, no more than six register words are allocated, leaving other registers for temporaries. In calling functions, variables use R4–R11 (at most eight). Other variables reside in the frame (9.4). There is no live-range analysis.
- **Parameters before the first call.** A parameter in a function that makes calls may remain in its argument register (9.3) if it is used only before the first top-level statement that could clobber R0–R3: a function or runtime call, `asm` block, multiple assignment, or label. The parameter is not read after that statement, so it need not be copied into R4–R11 or saved in the prologue. The statement in which the parameter is read cannot itself be such a clobbering statement: function-call arguments are evaluated sequentially in R0–R3 and could overwrite the parameter. This optimization is disabled for functions containing labels (`goto`), with `-fcopy-params`, or when fewer than two temporary R0–R3 registers remain after allocation (fewer than three for arithmetic on `long` or `ulong`); in those cases the parameter is copied as usual.
- **Register pressure.** If an expression has too few registers for its intermediate values, the affected portion is regenerated with intermediate values stored on the stack. `expression too complex` is possible only when these spill regions are nested more than 256 levels deep.
- **Evaluation order.** Function calls and more complex subexpressions are generally evaluated before other operands.
- **Multiplication and division by constants** use shifts, masks, and reciprocal multiplication with `MULHU` (9.6), without runtime calls. For double-word values, multiplication by a power of two uses a shift; division and remainder by a power of two use shifts and masks; division and remainder by a constant from 3 to 255 use four `DIVU`/`REMU` steps on the dividend's bytes.
- **Double-word halves.** `uint(x)`, `uint(x >> 16)`, `int(x >> 16)`, and `ulong(h) << 16 | l` (where h and l are unsigned words) require no arithmetic: they select or combine registers or memory words. Testing a 32-bit value against zero takes one `OR` of its two words; sign tests `x < 0` and `x >= 0` use `TEST` on the high word.
- **Local initializers.** Objects of at most 16 bytes are initialized by assignments. Larger objects are copied word by word from a `function.i<n>` template and then have their nonconstant parts assigned.
- **Source locations.** The compiler emits `.line "file", N` (on a file change) or `.line N` before the code for each B16 source line. Before the header, startup code, and runtime it emits `.line` without arguments. A global variable or constant is associated with its declaration line; a literal with the line on which it appears. Consequently, the assembler memory map (`cpu16asm -m`) associates objects with B16 files and line numbers.
- **Names** of assembler objects are defined in 10.3.

### 13.7. Building and Running

The compiler generates assembly only. Since the source itself specifies image parameters and modules, the build sequence is independent of the particular program:

```sh
b16c -I lib prog.b16 -o prog.s
cpu16asm --exec -m prog.map -l prog.lst -o prog.bin prog.s
cpu16 prog.bin
```

`--exec` validates the CX16 header (for `std`); `-m` emits a memory map and `-l` an assembly listing. The `run.sh prog.b16 [cpu16 options]` script runs the same three steps with `-I lib`. Additional compiler options (usually diagnostic options or `-s`) are passed through `B16FLAGS`; tool paths are supplied in `CPU16ASM` and `CPU16`.

A `kernel` target module (`option target = kernel`) can be incorporated into an assembly program with `.include "kmod.s"`; see the compiler's `tests/kernel` directory.

### 13.8. Limits

| Quantity | Limit |
| --- | --- |
| Function parameters | 32 |
| Function results | 4 words (language limit) |
| Names in one `var` declaration | 32 |
| Names in one structure field group | 16 |
| `include` nesting | 16 |
| Assembly modules (`include asm` and `--asm` combined) | 16 |
| `-I` search directories | 32 |
| Loop nesting | 64 |
| Local variables in a function's scope | 1024 |
| Variable register words in a function | 9 (non-calling), 8 (calling); 6 and 6 when doing arithmetic on `long` and `ulong` |
| Numeric literal value | 4294967295 |
| Errors reported per run | 20 |

## 14. Revision History

Revision numbers in this section refer to the B16 language specification and its `b16c` reference compiler, not to the CPU16 processor. CPU16 processor revision 6 is unrelated to this numbering. The history describes changes between language revisions. A **breaking change** means a revision 1 program might no longer compile or might behave differently.

| Revision | Date | Summary |
| --- | --- | --- |
| 1 | Oct 8, 2026 | Initial specification: 16-bit types, structures, arrays, pointers, multiple return values, interrupts, inline `asm`, and `add32`, `sub32`, `cmp32`, `shr32` built-ins operating on 32-bit objects in memory |
| 2 | Oct 16, 2026 | Full arithmetic for `long` and `ulong`; exact constants with range checks; calling-convention slots; grammar and diagnostic fixes; `--asm`, `-Wmaybe-uninit`, and `-fcopy-params` options |
| 3 | Oct 23, 2026 | Source-level image parameters (`option`); assembly modules through `include asm`; command-line parameters become overrides |

### 14.0. Revision 3

**Language changes**

| Change | Sections |
| --- | --- |
| New `option NAME = value` declaration: `target`, `code`, `data`, `min_free`, `max_top`; allowed only in the main file, once per parameter; value must be a constant expression (or `std`/`kernel` for `target`) | 3.1, 3.6, 11 |
| `option` and parameter names are not reserved: `option` is recognized only at the beginning of a top-level declaration | 2.4 |
| `include asm "path"` includes an assembly module in the image; allowed in any file, with duplicate inclusions ignored | 3.2, 10.4, 11 |
| A program without `main` (a kernel or assembly-linked module) declares `option target = kernel` | 3.4 |
| A parameter used with the wrong target is an error (`code` on `std`; `min_free` and `max_top` on `kernel`); command-line parameters were previously ignored in these cases | 3.6, 13.2 |

**Reference implementation changes**

| Change | Sections |
| --- | --- |
| `--target`, `--code`, `--data`, `--min-free`, `--max-top`, and `--asm` now override source-defined parameters; mismatches with `option` produce warnings | 13.2 |
| `--code` for `std`, and `--min-free` or `--max-top` for `kernel`, produce errors rather than warnings or silent no-ops | 13.2 |
| Warning for `max_top` exceeding `$E000` | 12.2, 13.2 |
| `include asm` modules are emitted before `--asm` modules; the combined limit is 16 | 13.2, 13.3, 13.8 |
| Generated assembly begins with comments identifying effective image parameters and their sources, plus the origin of each module `.include` | 13.2 |
| `-n` is deprecated | 13.2 |

**Documentation correction.** The `-I` description (13.2) previously claimed that repeating an `include` for the same file expands it again. That contradicted both 3.2 and compiler behavior: repeated inclusion is ignored.

**Related documents and sources.** The Monitor `sys.b16` module was split: declarations are now in `sysdefs.b16`, and `sys.b16` includes both those declarations and `include asm "sys.asm"`. Thus applications need only `include "sys.b16"`. The kernel, `fm`, `edit`, utilities, tests, and BScript now specify image parameters in source; their build scripts no longer pass image-parameter options. Affected documentation includes *CPU16 Monitor: Executable File Format and Program Startup* (Section 10), *System Calls* (10.1), *Kernel Internals* (8.2), `cpu16_edit_en.md`, `cpu16_fm_en.md`, and *Shell* (8.1).

### 14.1. Revision 2: Language Changes

**New features**

| Change | Sections |
| --- | --- |
| Signed `long` and unsigned `ulong` types, both 32-bit; `long`/`ulong` keywords and `long(x)`/`ulong(x)` conversions | 1, 2.4, 4.1, 4.6 |
| Numeric literals up to `$FFFFFFFF` (previously `$FFFF`) | 2.5, 13.8 |
| Full `long`/`ulong` operators: `+ - * / %`, `& \| ^ ~`, unary minus, comparisons, shifts, `++ --`, compound assignment, conditions; sign extension of narrower integers according to their source type | 6.2, 6.4, 6.5, 6.7 |
| `long` and `ulong` usable in parameters, results, structure fields, array elements, pointers, globals, locals, and initializers | 4, 5, 9.3 |
| Parenthesized result lists allowed in declarations, for example `fn f(): (int, uint)` (previously only in function types) | 4.5, 5.5 |
| If a function body begins with an `asm` block, R0–R3 are guaranteed to contain its arguments upon entering the block; the prologue does not change them | 10.2 |
| Assembly modules placed before `__image_end` (option `--asm`) | 10.4, 13.2, 13.3 |

**Breaking changes**

| Previous behavior | New behavior | Sections |
| --- | --- | --- |
| Untyped constants were evaluated as signed 16-bit values: `$FFFF + 1` and `1 << 16` both yielded 0 | Constants are evaluated exactly (within ±2^62): `$FFFF + 1` and `1 << 16` both yield 65536 | 4.7, 6.10 |
| A constant outside its destination type was silently truncated (`var b: byte = 300`) | Error. For a type of width n, the accepted value range is −2^(n−1)…2^n−1; `int($FFFF)` yields −1, `ulong(-1)` yields `$FFFFFFFF` | 4.6, 4.7 |
| `var x = 40000` had type `int` (40000 wrapped to −25536) | Type is inferred from value: `int`, then `uint`, `long`, `ulong`; 40000 has type `uint` | 5.3 |
| Built-ins `add32`, `sub32`, `cmp32`, `shr32` worked on 32-bit values stored as `{ lo, hi: uint }` or `[2]uint` (former Section 8.1) | Removed with no transitional replacement. Use `long`/`ulong` and ordinary operators; these names are no longer reserved | 2.4, 8 |
| `long` and `ulong` were available as identifiers | They are now reserved keywords | 2.4 |
| Complementing a constant with its high bit set produced a mask: `flags & ~F_DIR`, with `F_DIR = $8000`, was equivalent to `flags & $7FFF` | Error: `~$8000` evaluates exactly to −32769 and does not fit `uint`. Write `~uint(F_DIR)` or an explicit mask (4.7) | 4.7 |

Other changes are safe for revision 1 programs: they affect only `long`/`ulong` or explicitly document rules that already applied.

**Semantic refinements without breaking 16-bit programs**

- Function results are counted in words: at most four words, with `long` and `ulong` occupying two each. The previous maximum of four results for other types is unchanged (4.5, 5.5).
- The index in `a[i]` and the offset in `p + i` must be an integer no wider than one word. Explicitly convert a `long`/`ulong` value, for example `a[uint(i)]` (6.8).
- Assigning `long` or `ulong` to `uint`, `int`, `byte`, or `char` without an explicit conversion is an error; widening in the opposite direction is implicit (6.2).
- An untyped constant on the left of a shift with a nonconstant shift count has type `int` (as it did previously). This rule is now explicit. Use `ulong(1) << n` for a 32-bit shift (6.4).
- When an operand is `long` or `ulong`, the binary result has the wider operand's type; comparisons and `/` or `%` may combine a signed `long` with an unsigned operand no wider than one word (6.4). Word-sized operand rules are unchanged.
- A variable shift count is reduced modulo 32 for `long` and `ulong`, but modulo 16 for other integer types (6.5). A constant shift by the type width or more yields zero (or the sign value for signed `>>`) and triggers a warning.
- Dividing `ulong` by zero produces `$FFFFFFFF`, with the remainder equal to the dividend, on every processor. For `long`, `-2147483648 / -1` produces `-2147483648` with remainder zero (6.5).
- The calling convention counts **slots**: word-sized values take one, `long` and `ulong` take two consecutive slots. An argument that cannot fit wholly in R0–R3, and all later arguments, go on the stack (9.3). Without `long` or `ulong`, the ABI is unchanged: arguments 1–4 in R0–R3 and subsequent arguments on the stack.
- Reading or writing `long`/`ulong` requires two instructions and is not atomic; values shared with an interrupt handler must be read and written inside an `atomic` block (9.7).
- Runtime routines were added for 32-bit multiplication, division, and shifts (9.6, 10.3).
- An untyped constant is range-checked whenever it takes on a type: assignment, argument passing, return, initializer, cast, and operand evaluation (4.7).

### 14.2. Revision 2: Diagnostics

| Change | Sections |
| --- | --- |
| The warning for reading a variable before assignment is now split: a variable unassigned on every path always triggers a warning, whereas one unassigned on only some paths is warned about only with `-Wmaybe-uninit`. Correlated conditions can make the latter warning a false positive. Assignments within a loop body are considered able to reach the next iteration; assignments in the right-hand side of `&&`/`\|\|` are treated as possible | 12.2, 13.2 |
| No unused-parameter warning for `main` under `std`, since its signature is determined by the startup convention | 12.2 |
| Warning for a constant shift count at least as large as the operand width | 12.2 |
| New errors: constant outside the destination type's range, implicit narrowing from `long`/`ulong`, using a pointer with `long`/`ulong` in the same operation, an index of type `long`/`ulong`, or function results exceeding four words | 4.6, 4.7, 5.5, 6.2, 6.4, 6.8 |

### 14.3. Revision 2: Clarifications and Corrections

- **Grammar.** The `Results` nonterminal is defined once (4.5) and used both in function types and function declarations. The `static`, `export`, and `extern` modifiers are part of `FunctionDeclaration` throughout (3.1, 5.5, 11). `Declaration` in the consolidated grammar agrees with 3.1, and `Assertion` was added.
- Other grammar definitions were made consistent: `Statement` (7.1) has `[ "static" ]` before `VariableDeclaration`, as in 7.2; `ScalarType` (4.6) includes `"ptr" [ Type ]`; `StructureDeclaration` (5.4) allows blank lines between fields; `Primary` (6.3) includes `offsetof`; `Target` (7.8) is `ExpressionWithoutAssignment`. Section 11 explicitly notes that `Primary` and `Statement` are expanded there.
- **Strings.** A local declaration `var s = "..."` without a type is a `ptr byte` pointing to a string literal and must not be modified. Mutable text inside a function requires an array such as `var s: [N]byte = "..."` (2.7, 5.3, 7.2).
- **Registers in `asm`.** The phrase “the sole variable in R4” was clarified to mean the sole variable **used by B16 code** (10.2, 13.6).
- **Startup.** `__halt` and `__fault` must acknowledge a pending interrupt request (`STI`, then `CLI`) before `WFI`. Otherwise a timer interrupt asserted while IF = 0 would repeatedly wake the CPU without allowing it to remain halted (9.2).
- **Image layout.** Including an assembly module *after* the compiler's output places it beyond `__bss_end`. That can run on a bare machine but fails under Monitor, and is rejected by `cpu16asm --exec` (10.4).
- **Constant complement.** The `flags & ~F_DIR` example with `F_DIR = $8000` was added: exact constant evaluation makes this an error; `~uint(F_DIR)` is correct (4.7, 14.1).
- **Related documents.** The introduction was updated to use the actual document names and revisions, including *CPU16 Monitor: Executable File Format and Program Startup*, revision 3, and *CPU16 Monitor: System Calls* (Sections 1, 13).

### 14.4. Revision 2: Reference Implementation

| Change | Sections |
| --- | --- |
| `--asm file` includes a module via `.include` after functions and runtime, but before data; repeatable | 13.2, 13.3 |
| Added `-Wmaybe-uninit` | 13.2 |
| Added `-fcopy-params`, forcing parameters to be copied into R4–R11 for testing and debugging | 13.2, 13.6 |
| A parameter in a calling function can stay in its argument register if used only before the first statement that might clobber R0–R3 | 13.6 |
| `long`/`ulong` variables use register pairs, not necessarily adjacent; arithmetic on them limits variable allocation to six registers | 13.6, 13.8 |
| `uint(x)`, `uint(x >> 16)`, and `ulong(h) << 16 \| l` avoid full double-word calculations. Word multiplication `ulong(a) * ulong(b)` uses `MUL`/`MULHU`; powers-of-two multiplication and division use shifts/masks; `ulong` division by 3…255 uses four `DIVU`/`REMU` steps | 9.6, 13.6 |
| Limits include 4294967295 for literals, four words of function results, and variable register words | 13.8 |
| The compiler does not depend on the host's `long` width (constants and offsets use 64-bit types) and builds with `cc` and `g++` | — |

### 14.5. Related Documentation

The programmer's guide ([`B16_guide_en.md`](B16_guide_en.md)) and the project [`README_en.md`](../README_en.md) were updated for revision 2 with a “Double words: `long` and `ulong`” section, an operation-cost table, and library routines (`putul`, `putl`, `puthexl`, `atoul`, `ultoa`, `labs`, `lmin`, `lmax`, `ulmin`, `ulmax`, `lgcd`, `ulsqrt`; the `add32v`, `sub32v`, `mul32x16`, `divmod32`, and `putu32` functions were removed). *CPU16 Monitor: Executable File Format and Program Startup* (revision 3) was clarified: `cpu16asm --exec` rejects bytes emitted beyond `image_end`, and assembly modules can be linked into B16 programs with `--asm`. The Monitor application interface is documented in *CPU16 Monitor: System Calls* (revision 1).
