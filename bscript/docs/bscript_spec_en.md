# BScript — Language Specification (Phase 1)

This document specifies BScript as implemented in `bscript/`: the `bsc` compiler, the `bsvm` virtual machine, and the `bs` launcher. It supersedes Part I of specification version 1.1. The companion documents are:

| Document | File |
| --- | --- |
| Getting Started with BScript — an informal introduction | `bscript_intro_en.md` |
| BScript — Virtual Machine Internals | `bscript_vm_en.md` |
| BScript — Language Roadmap | `bscript_roadmap_en.md` |
| BScript Bytecode Format, Version 1 | `bscript_bytecode_en.md` |

## 1. Purpose and Status

BScript is a small dynamic language for scripts launched from the CPU16 Monitor shell. Typical uses include traversing files, processing text, launching programs, and interpreting their output. Programs are compiled to bytecode and executed by a virtual machine. All three tools are written in B16 and run on the CPU16 machine itself.

**Status.** This is the normative Phase 1 language specification, language version 1.1. Its provisions are covered by the `tests/*.bs` test suite and the programs in `examples/*.bs`. A discrepancy between the specification and an implementation is considered a defect and must be corrected in both.

**Compatibility.** Future language versions must compile and execute programs written for this version without modification. Their virtual machines must also execute bytecode format version 1.

**Normative terminology.** A *compile-time error* means that `bsc` rejects the program (Section 11.2). A *runtime error* means that `bsvm` terminates the program with exit status 125 (Section 6.5). An operation that *fails* returns an `error` value that can be caught by the program (Section 6.4).

### 1.1. Related System Specifications

| Document | Revision |
| --- | --- |
| B16 Language Specification (`B16_reference_en.md`) | 3 |
| CPU16 Monitor: System Calls | 6 |
| CPU16 Monitor: Kernel Internals (the `XMAP` fast path, Section 3.2a) | 6 |
| CPU16 Monitor: Executable File Format and Program Startup | 4 |
| FS16 File System | 3 |
| Shell and Utilities | 6 (script lookup as `name.bs`, `.bsc` execution) |
| `fm` File Manager | 6 (Enter on `.bs` or `.bsc` launches the script) |
| BScript Bytecode Format (`bscript_bytecode_en.md`) | 1 |

### 1.2. Language Overview

- Values: `null`, `bool`, signed 16-bit `int`, strings, arrays, maps, files, and errors.
- Functions can return multiple values; optional type annotations are checked at runtime.
- Errors are `error` values. They propagate automatically through calls and can be caught by supplying an additional target variable on the left-hand side.
- Two program forms: scripts with top-level statements and programs with a `main` function.
- String interpolation and `include` directives.
- A library for strings, arrays, maps, text files, paths, and launching programs with redirection and captured output.

## 2. Lexical Structure

### 2.1. Source Text

Source text is a sequence of bytes in the CPU16 machine character encoding. Whitespace consists of spaces, tabs, and carriage returns (CR). A line feed (LF) has special significance (Section 2.3). CR LF pairs are permitted.

Comments have the same syntax as in B16:

- `// ...` runs to the end of the line.
- `/* ... */` is a non-nesting block comment. Line feeds within a block comment do not terminate statements.

The source is tokenized from left to right, selecting the longest possible token at each position. The one exception is the interpolation terminator `|}` (Section 2.8).

### 2.2. Identifiers and Reserved Names

```
IDENT  = letter { letter | digit } .
letter = "A"…"Z" | "a"…"z" | "_" .
digit  = "0"…"9" .
```

Identifiers are case-sensitive. An identifier may contain at most 62 characters; a longer name causes the compile-time error `name too long`. Names beginning with `__` are reserved for the implementation. The identifier `_` means “discard this value” and is allowed only as a target of multiple assignment (Section 5.3).

**Keywords:**

```
break    case     const    continue default  else     fail     false
fn       for      if       in       include  loop     null     return
switch   true     var      while
```

**Reserved names** may not be declared as variables, constants, or functions (error `reserved name: <name>`):

- `enum`, `record`, `static`, and `defer` are reserved for future versions.
- Library namespace names: `io`, `fs`, `path`, `proc`, `str`, `time`, and `errors`. The name `str` also denotes the global function `str(x)`: `str.` begins a namespace member access, whereas `str(` calls the function.
- `args` is a predefined global variable (Section 3.2).
- Names of global built-in functions (Section 9.1).

**Type names** (`int`, `bool`, `string`, `array`, `map`, `file`, `error`, and `value`) are ordinary identifiers that have special meaning only in type annotations (Section 4.3). Variables with these names are allowed but discouraged; `error` remains a built-in function and cannot be redeclared.

### 2.3. Statement Termination

The rule is the same as in B16 (Section 2.3), adjusted for BScript's token set. A line feed becomes an `EOS` (end-of-statement) token when the last token on the line is one of the following:

- An identifier, numeric literal, character literal, string literal, or interpolated string.
- `true`, `false`, or `null`.
- `)`, `]`, or `}`.
- `++` or `--`.
- `break`, `continue`, or `return`.

Exceptions:

- If the first nonempty token following the line feed is `else`, ignoring blank lines and `//` comments, no `EOS` is inserted.
- No `EOS` is inserted within parentheses `( )` or square brackets `[ ]`.
- Within a map literal `{ ... }`, the parser ignores `EOS` tokens between entries.

End of file after one of these tokens also produces `EOS`. A semicolon `;` always denotes `EOS`; consecutive `EOS` tokens are permitted. An `EOS` before a block-closing `}` is optional: `if bad { return 1 }` is valid syntax.

Consequently, an expression may span lines only after an operator, a comma, or an opening bracket.

### 2.4. Numbers

```
NUMBER = decimal
       | "$" hexDigit { hexDigit }
       | ("0x" | "0X") hexDigit { hexDigit }
       | ("0b" | "0B") binaryDigit { binaryDigit } .
```

- **Decimal literals** range from 0 to 32767. The number 32768 is allowed only as the operand of unary minus: `-32768`. Otherwise `bsc` reports `number too large`.
- **Hexadecimal and binary literals** range from 0 to `$FFFF`. They specify the 16-bit representation of an `int`: `$FFFF` is −1, and `$8000` is −32768.
- A number cannot be immediately followed by a letter, a digit invalid in its base, or `_`.

There are no negative literals: `-1` is unary minus applied to a literal (a constant expression).

### 2.5. Character Literals

A character literal consists of one character or escape sequence enclosed in single quotes: `'A'`, `'\n'`, `'Ж'`. Its value is the character code in the machine encoding, and its type is `int` (0–255). An empty literal `''` or a literal containing multiple characters is a compile-time error.

Character literals are useful for byte comparisons: `s[i] == ','` compares two `int` values. In contrast, `s[i] == ","` compares an integer with a string and always evaluates to `false` (Section 4.4).

### 2.6. Escape Sequences

The same escapes apply to character literals, ordinary strings, and interpolated strings:

| Sequence | Value |
| --- | --- |
| `\n` | 10 |
| `\r` | 13 |
| `\t` | 9 |
| `\e` | 27 |
| `\0` | 0 |
| `\\`, `\'`, `\"`, `` \` ``, `\{` | The character following `\` |
| `\xH`, `\xHH` | A byte with the specified hexadecimal value |

Any other sequence beginning with `\` is a compile-time error.

### 2.7. Strings

A string consists of characters and escape sequences enclosed in double quotes `"..."` or backticks `` `...` ``. Both notations produce the same kind of string. Within backticks, double and single quotes do not require escaping; within double quotes, single quotes and backticks do not require escaping:

```text
var a = `строка "без" экранирования`
var b = "it's ok"
```

A line feed inside a string literal is a compile-time error. A string literal can contain at most 512 bytes (`string too long`). Strings can contain any byte, including zero, because their lengths are stored explicitly. Strings are immutable.

Longer text can be constructed through concatenation by placing a line break after `+`. Concatenation of literals is evaluated at compile time, up to 1024 bytes:

```text
var help = "usage: tool [-v] file\n" +
           "  -v  verbose\n"
```

### 2.8. Interpolated Strings

An interpolated string begins with `$"` or `` $` `` and contains substitutions of the form `{|expression|}`. The quoting rules are the same as for ordinary strings:

```text
println($"{|name|}: {|count|} lines")
```

- Each substitution value is converted to text as by `str(x)` (Section 9.1).
- A substitution may contain any expression except another interpolated string. Ordinary strings are allowed within a substitution: `{|m["key"]|}`.
- A substitution cannot span lines. An empty substitution `{||}` causes `empty substitution`; a single interpolated string may contain at most 255 substitutions.
- **End of a substitution.** At the beginning of each token inside a substitution, outside brackets and strings, the lexer first checks for the two-character sequence `|}`. If present, it ends the substitution. Thus `{|a || b|}` represents `a || b`, while `{|flags | 1|}` represents `flags | 1`.
- To include `{|` literally in the resulting text, write `\{|`. The pair `|}` outside a substitution is ordinary text.

An interpolated string is compiled as a single concatenation and creates one new string. The `{|...|}` delimiter was chosen so that braces in ordinary text, such as JSON, need not be escaped: `` $`{"name": "{|name|}"}` `` is valid.

### 2.9. Operators and Delimiters

```
+   -   *   /   %   &   |   ^   ~   !   <<  >>  >>>
==  !=  <   >   <=  >=  &&  ||  ??  ++  --
=   +=  -=  *=  /=  %=  &=  |=  ^=  <<= >>= >>>=
(   )   [   ]   {   }   ,   ;   :   .
```

Any other character outside comments, strings, or character literals causes a compile-time error.

## 3. Program Structure

### 3.1. Two Forms of Program

A program consists of a main source file and files included through `include`. A file's top level contains `include` directives, declarations (`const`, `var`, `fn`), and, in scripts, statements.

**Program with `main`.** If a function named `main` is declared, only declarations and `include` directives are allowed at the top level. A top-level statement causes the compile-time error `statements are not allowed at top level of a program with main`. Execution proceeds as follows:

1. Global variable initializers run in source order, accounting for the order of included files.
2. `main` is called.

`main` must have one of the following signatures; any other signature causes `invalid signature of main`:

```text
fn main() { ... }
fn main(): int { ... }
fn main(args: array) { ... }
fn main(args: array): int { ... }
```

The result of `main` is the process exit code (0–32767). If it returns no value or returns `null`, the exit code is 0.

**Script.** If no `main` function is declared, top-level statements from all files run in order, interleaved with global variable initializers. A top-level `return` terminates the script; its single return value is the exit code. A bare `return` or reaching the end of the source text yields exit code 0.

### 3.2. Arguments

Both program forms have a predefined global variable `args`, an array of command-line argument strings excluding the program name and the script path. The `args` parameter of `main` refers to the same array. The script path, as supplied to the launcher, is returned by `proc.script()` (Section 10.7).

### 3.3. Global and Local Names

- Global variables, constants, and functions share a single namespace. Duplicate declarations are compile-time errors.
- **Functions** can access any global name in the program regardless of its position in the source. The compiler collects declarations in a preliminary pass, so functions can be called before their definitions.
- **Top-level code** in a script may access a global variable or constant only after its declaration in source order. Otherwise `bsc` reports `used before declaration: <name>`.
- A global variable accessed by a function before its initializer has run has the value `null`.
- A `var` declaration inside a block at a script's top level (for example, inside an `if` body) declares a local variable of that block, not a global variable.
- Functions may be declared only at the top level (`functions can be declared only at top level`).

### 3.4. `include`

```text
include "lib/strings.bs"
```

- An `include` directive must appear at the beginning of a file, before any declaration or statement (`include must come before declarations and statements`).
- Unless absolute, the path is resolved relative to the directory containing the file with the directive. Absolute paths have a volume prefix `X:` (letter `A`–`H` or digit `0`–`7`) or begin with `/`.
- **Duplicate includes** of the same file are skipped. Files are compared by normalized absolute paths, case-insensitively (as with `path.equal`, Section 10.6), so circular includes are harmless.
- Inclusion is textual: names declared by included files belong to the program's shared namespace.
- The maximum `include` nesting depth is 8, and a program may contain at most 32 source files in total.

## 4. Values and Types

### 4.1. Value Types

| Type | `type(x)` | Description |
| --- | --- | --- |
| null | `"null"` | Absence of a value |
| bool | `"bool"` | `true`, `false` |
| int | `"int"` | Signed 16-bit integer: −32768…32767 |
| string | `"string"` | Immutable byte sequence |
| array | `"array"` | Mutable array of values |
| map | `"map"` | Mutable map with string keys |
| file | `"file"` | Open text file (Section 10.5) |
| error | `"error"` | Error value: code, message, and associated object (Section 6.2) |

Strings, arrays, maps, files, and errors are heap objects. Variables hold references to objects: assignment and argument passing do not copy an object. `clone()` creates a copy. The garbage collector reclaims memory; programs do not free it explicitly.

A string may contain at most 32767 bytes, an array at most 32767 elements, and a map at most 16384 keys. Exceeding these limits is a runtime error `too large` (functions reading external data instead return `errors.TOOBIG`; Section 6.6).

### 4.2. Integers

- Addition, subtraction, and multiplication are performed modulo 2^16; overflow is not detected.
- `/` truncates toward zero; `%` has the sign of the dividend: `a == (a / b) * b + a % b`.
- Division or remainder by zero is a runtime error. `-32768 / -1` is −32768, and `-32768 % -1` is 0.
- `~`, `&`, `|`, and `^` operate on the 16-bit representation.
- `a << n` shifts left, `a >> n` shifts right arithmetically, and `a >>> n` shifts right logically. For n ≥ 16, `<<` and `>>>` produce 0; `>>` produces 0 or −1 according to the sign of a. A negative n causes a runtime error.
- The comparisons `<`, `<=`, `>`, and `>=` are signed. `str.hex(n, w)` displays the unsigned representation (Section 10.2).

### 4.3. Type Annotations

Type annotations are optional for variables and parameters, but required for function results (Section 5.1).

```text
var name: string = "x"
fn count(lines: array, mark: string): int { ... }
```

Valid names are `int`, `bool`, `string`, `array`, `map`, `file`, `error`, and `value` (any type). Other names cause `type name expected`.

Annotations are checked at runtime: when assigning to a variable, on function entry for parameters, and on return for results. A mismatch causes the runtime error `type mismatch: T expected, got U`. A value of `null` is valid for any annotation.

### 4.4. Equality and Ordering

- `a == b` is true when the values have the same type and:
    - For `int` and `bool`, they have the same value.
    - For strings, their bytes are identical.
    - For arrays, maps, and files, they reference the same object.
    - For errors, their codes are equal (Section 6.2).
    - `null == null` is true.
- Values of different types are **not equal**; this is not an error. Thus `x == null` and `cfg["port"] == 80` are safe regardless of the type of `x`.
- `<`, `<=`, `>`, and `>=` are valid for pairs of `int` values (signed comparison) and pairs of strings (bytewise order, treating bytes as 0–255; a shorter prefix precedes a longer string). Other operand types cause a runtime error.

### 4.5. Boolean Values

Conditions in `if` and `while`, and operands of `&&`, `||`, and `!`, must be `bool`. There is no implicit conversion: `if count { }` causes `bool expected, got int`; write `if count != 0 { }` instead.

## 5. Declarations and Functions

### 5.1. Functions

```text
fn name(p1: T1, p2, ...): R1, R2 {
    ...
}
```

- A function may have at most 16 parameters and 8 return values. An error does not count as a return value (Section 6.4).
- Return types are mandatory: they tell the compiler how many values the function returns. The type `value` accepts any value.
- The argument count is checked at compile time (`wrong number of arguments for <name>`). User-defined functions have neither optional parameters nor variadic arguments; built-in functions may have them (Sections 9–10).
- Nested functions and first-class function values are not supported. Referring to a function by name without calling it causes `functions are not values: <name>`.
- Recursion is supported, up to a depth of 100 calls. Beyond that limit, execution fails with `stack overflow` (Section 12).

**Function termination.** A function declaring return values must end with `return` or `fail`. The compiler requires the end of its body to be unreachable. A statement is considered to terminate control flow if it is:

- `return` or `fail`;
- An `if` with an `else` branch, where both branches terminate;
- A `loop` with no `break` targeting that loop;
- A `switch` with a `default` branch, where every branch terminates.

Otherwise the compiler reports `missing return in function <name>`. This check applies only to declared functions: a script's top-level code may end without a `return`.

### 5.2. Variables and Constants

```text
var count = 0
var name: string
var a, b = split_pair(s)
const LIMIT = 100
const GREETING = "hello"
```

- A `var` declaration without an initializer produces `null`.
- A local variable is in scope from its declaration to the end of the enclosing block. Redeclaration in the same block is an error; redeclaration in a nested block is allowed and shadows the outer variable.
- `const` assigns a name to a constant expression: literals, other constants, arithmetic and bitwise operations on `int`, or string concatenation using `+`. Anything else causes `constant expression expected`. Constants can be declared both at the top level and inside blocks.

### 5.3. Multiple Return Values

```text
fn divmod(a: int, b: int): int, int {
    return a / b, a % b
}

var q, r = divmod(17, 5)
q, r = divmod(20, 3)
var _, rest = divmod(9, 4)
```

- A multiple declaration or assignment must have exactly one function call on its right-hand side. The number of targets must equal the number of results, or exceed it by one: the extra final target catches a call error (Section 6.4). Swapping variables with `a, b = b, a` is not supported.
- A target can be a variable, an indexed element `a[i]` or `m[k]`, or `_`. There can be at most nine targets.
- `return f()` is allowed if `f` returns exactly as many values as the current function. `return x, f()` is invalid if `f` returns multiple values.
- Using a multi-result function call where a single value is required causes `function returns several results`; using a function with no result causes `function has no result`.
- A call used as a statement discards all results, but any error from the call still propagates (Section 6.4).

## 6. Errors

### 6.1. Two Kinds of Errors

**Expected failures** include a missing file, an out-of-space volume, an invalid number in input text, or a failure reported deliberately by a program. These are values of type `error`. They originate in library functions or the `fail` statement, propagate through callers, and can be caught (Section 6.4).

**Runtime errors** indicate programming errors such as an operand of the wrong type, an out-of-range index, or division by zero. They cannot be caught and terminate the program (Section 6.5).

### 6.2. The `error` Value

An `error` value is immutable:

| Method | Description |
| --- | --- |
| `e.code() → int` | Error code: a Monitor code, a BScript code, or an application-defined code (Section 6.6) |
| `e.text() → string` | Error message, e.g. `"no such file or directory"` |
| `e.object() → string` | Error object, usually a path, or `null` |
| `e.where() → string` | Origin as `file:line`, or `null` for `errors.*` constants |

- `str(e)` produces `object: message`, or just `message` if there is no object.
- **Error equality** compares codes: `err == errors.NOENT` is true for any “file not found” error, regardless of its associated object.
- `error(text[, object[, code]]) → error` constructs an error value without raising it. The default code is 100 (`errors.FAIL`).

### 6.3. The `fail` Statement

`fail` terminates the current function with an error. For the control-flow check in Section 5.1, it is a terminating statement just like `return`.

| Syntax | Raised error |
| --- | --- |
| `fail text` | `error(text)`: code `errors.FAIL`, no object |
| `fail text, object` | `error(text, object)` |
| `fail text, object, code` | `error(text, object, code)` |
| `fail e` | The existing value `e` of type `error` |
| `fail e, object` | A copy of `e` with a different object, e.g. `fail errors.NOENT, name` |

- The first operand must be a string or an `error`, otherwise a runtime error occurs. `object` must be a string or `null`; `code` must be an `int`.
- `fail` records the origin as `file:line` if the error has no origin yet. `fail e` leaves the origin unchanged.
- Codes 1–999 are reserved for Monitor and BScript. Application-defined error codes must be 1000 or greater.

### 6.4. Propagation and Catching

Any call—to a user function, built-in function, method, or namespace function—can fail with an error.

**Catching an error.** If a call is the right-hand side of a declaration or assignment and there is one more target than return values, the final target receives the error, or `null` on success. If the call fails, all other targets receive `null`.

```text
var text, err = fs.read_text(name)     // one result + error
var err2 = fs.remove(tmp)              // zero results + error
var code, out, perr = proc.capture("ls", [])
var t, _ = fs.read_text(name)          // explicitly discard the error
```

An error can be caught only when the call constitutes the entire right-hand side. A call nested in an expression, such as `fs.read_text(p).lines()`, cannot be caught there.

**Propagation.** An uncaught error immediately terminates the current function with the same error and no return values. It propagates up the call chain until a caller catches it or it reaches the top level.

**Top level.** An error that reaches a script's top level or escapes from `main` is written to stream 2 as `name: object: message` (`name` is `proc.name()`). Output buffers are flushed, and the program exits with status 1. This follows the shell utility error-message convention: even a script with no explicit error checks behaves like a well-formed utility.

**Resources.** Files opened by functions through which an error propagates remain open until garbage collection or program termination (Section 10.5).

```text
fn load_config(name: string): map {
    var cfg = {}
    for line in fs.read_text(name).lines() {   // reading errors propagate to the caller
        var i = line.find("=")
        if i < 0 {
            fail $`bad line "{|line|}"`, name
        }
        cfg[line.slice(0, i).trim()] = line.slice(i + 1).trim()
    }
    return cfg
}

var cfg, err = load_config("app.cfg")
if err == errors.NOENT {
    cfg = {}                                  // missing file: use default settings
} else if err != null {
    fail err
}
```

### 6.5. Runtime Errors

A runtime error writes the following diagnostic to stream 2:

```text
bsvm: file:line: in function name: message
```

For top-level code, the `in function name: ` portion is omitted. `file` is the absolute path of the source file. The virtual machine then flushes the buffers of open files and terminates with exit status 125. Runtime errors also include `assert` failures (Section 9.1), `out of memory`, and `stack overflow`.

### 6.6. Error Codes and Messages

Monitor error codes match their system call error numbers (System Calls, Section 8); messages match the shell error-message table (Section 8.2).

| Constant | Code | Message | Source |
| --- | --- | --- | --- |
| `errors.INVAL` | 2 | invalid argument | Monitor |
| `errors.NOENT` | 4 | no such file or directory | Monitor |
| `errors.EXIST` | 5 | already exists | Monitor |
| `errors.NOTDIR` | 6 | not a directory | Monitor |
| `errors.ISDIR` | 7 | is a directory | Monitor |
| `errors.NOTEMPTY` | 8 | directory not empty | Monitor |
| `errors.RDONLY` | 9 | read-only | Monitor |
| `errors.NOSPC` | 10 | no space left | Monitor |
| `errors.BADNAME` | 11 | invalid name | Monitor |
| `errors.BADPATH` | 12 | invalid path | Monitor |
| `errors.MFILE` | 14 | too many open files | Monitor |
| `errors.NODEV` | 15 | volume not mounted | Monitor |
| `errors.IO` | 16 | I/O error | Monitor |
| `errors.NOEXEC` | 17 | not an executable | Monitor |
| `errors.BADVER` | 18 | unsupported program version | Monitor |
| `errors.ARGS` | 19 | bad arguments | Monitor |
| `errors.NOMEM` | 20 | not enough memory | Monitor |
| `errors.NOPROC` | 21 | too many nested programs | Monitor |
| `errors.BUSY` | 23 | busy | Monitor |
| `errors.XDEV` | 25 | different volumes | Monitor |
| `errors.NOTFS` | 26 | no file system | Monitor |
| `errors.FAIL` | 100 | failed | BScript: `fail` and `error()` with no explicit code |
| `errors.TOOBIG` | 101 | too large | BScript: result longer than 32767 |
| `errors.NUMBER` | 102 | invalid number | BScript: `to_int` |
| `errors.SAMEFILE` | 103 | same file | BScript: `fs.copy` |
| `errors.CLOSED` | 104 | file closed | BScript: operation on a closed file |
| — | N | error N | Other Monitor codes |

The message `failed` belongs only to the `errors.FAIL` constant; `fail text` supplies its own message. Under correct operation, `FAULT`, `BADF`, `NOSYS`, and `AGAIN` should not occur; if they do, they indicate an internal VM defect (a runtime error). `INTR` is not returned: Ctrl+C terminates the script (Section 11.5).
## 7. Expressions

### 7.1. Precedence

Operator precedence follows B16 (Section 6.1), with the addition of `in`, `??`, and `>>>`.

| Level | Operators | Associativity |
| --- | --- | --- |
| 1 (highest) | `f(...)` `a[i]` `x.m(...)` `ns.f(...)` | left to right |
| 2 | unary `-` `+` `~` `!` | right to left |
| 3 | `*` `/` `%` | left to right |
| 4 | `+` `-` | left to right |
| 5 | `<<` `>>` `>>>` | left to right |
| 6 | `&` | left to right |
| 7 | `^` | left to right |
| 8 | `\|` | left to right |
| 9 | `==` `!=` `<` `>` `<=` `>=` `in` | non-associative |
| 10 | `&&` | left to right |
| 11 | `\|\|` | left to right |
| 12 (lowest) | `??` | right to left |

Bitwise operators bind more tightly than comparisons: `x & M == 0` means `(x & M) == 0`. Chained comparisons such as `a < b < c` produce the error `comparisons cannot be chained`. Assignment is a statement, not an expression. Expression nesting (parentheses, argument lists, and literals) is limited to 24 levels.

### 7.2. Operators

- **Arithmetic and bitwise operators** require two `int` operands (Section 4.2). Other operand types cause a runtime error, except for `+` applied to two strings.
- **String `+`** creates a new string by concatenating the bytes of both operands. `"a" + 1` is a runtime error; convert the number explicitly with `str(n)` or use interpolation.
- **`&&` and `||`** use short-circuit evaluation; their operands must be `bool` values.
- **`a in b`** checks membership according to the type of `b`:
    - array: whether an element compares equal to `a` (Section 4.4);
    - map: whether key `a` exists (`a` must be a string);
    - string: whether string `a` occurs as a substring of `b`;
    - any other type: runtime error.
- **`a ?? b`** evaluates to `a` unless it is `null`, in which case it evaluates to `b`. The expression `b` is evaluated only when `a` is `null`.
- Expressions consisting entirely of literals and constants may be evaluated at compile time; this does not change program semantics.

### 7.3. Indexing

- For an array, `a[i]` requires an `int` index with `0 <= i < length`; otherwise the runtime error is `index out of range`. Negative indexing relative to the end is not supported.
- For a string, `s[i]` returns a byte as an `int` in the range 0–255, subject to the same index bounds. Assignment to `s[i]` is an error: strings are immutable.
- For a map, `m[k]` requires a string key and returns the associated value, or `null` if the key does not exist. Assignment `m[k] = v` inserts or replaces an entry.

### 7.4. Array and Map Literals

```text
var a = [1, 2, 3]
var empty = []
var m = {"name": "x", "size": 10}
var nested = {
    "list": [1, 2],
    "flags": {"v": true},
}
```

Each literal creates a new object. Map literal keys must be string-valued expressions; a duplicate key replaces its previous value. A trailing comma is permitted.

### 7.5. Methods and Namespaces

`x.m(args)` invokes a built-in method of `x` (Section 9). Available methods depend on the value's runtime type. Calling a method unavailable for that type raises `no method m for type` at runtime.

A method with a given name has the same arity for every type that supports it. The compiler therefore checks method argument counts. A method name not recognized for any type is a compile-time error, `unknown method`.

`ns.f(args)` invokes a namespace function (Section 10); `ns` must be one of the reserved namespace names (Section 2.2). Namespace constants use the same qualification syntax: `errors.NOENT`, `proc.BREAK`.

Values do not have fields: `x.name` without parentheses is a compile-time error, `values have no fields`, except when referring to a namespace constant.

## 8. Statements

### 8.1. Blocks and Simple Statements

A block is a sequence of statements enclosed in braces. Braces are mandatory for `if`, `else`, loops, and `switch`. Blocks and loops may be nested to a depth of 32 levels each.

Simple statements include:

- `var` or `const` declarations (Section 5.2);
- assignments `target = expression`, compound assignments `target op= expression` (Section 8.2), and multiple assignments (Section 5.3);
- `target++` and `target--`, each as a separate statement, with an `int`-valued target;
- function or method calls;
- `return`, `fail` (Section 6.3), `break`, and `continue`.

Other expressions cannot be used as statements: a standalone `x + 1` raises the compile-time error `expression is not a statement`.

### 8.2. Assignment

An assignment target must be a variable, array element `a[i]`, or map element `m[k]`; any other expression produces `cannot assign to this expression`.

Compound assignment operators are `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, and `>>>=`. In `a[i] += 1`, `a` and `i` are each evaluated exactly once. For strings, `s += t` concatenates the operands.

### 8.3. `if`

```text
if count == 0 {
    println("empty")
} else if count == 1 {
    println("one")
} else {
    println("many")
}
```

The condition must evaluate to `bool`. In the conditions of `if`, `while`, `for`, and `switch`, an opening brace begins the body. Therefore a map literal in a condition must be parenthesized: `if m == ({}) { }`. Otherwise the compiler reports `map literal in a condition must be in parentheses`.

### 8.4. Loops

```text
while i < n { ... }

loop {
    ...
    if done { break }
}

for line in lines { ... }
for i, line in lines { ... }
```

- `while` evaluates its condition before every iteration. `loop` repeats indefinitely until terminated by `break`, `return`, or `fail`.
- `for v in e` iterates over the elements of an array or the bytes of a string (as `int`). Expression `e` is evaluated once. The two-name form provides the zero-based index (`int`) and the value. Loop variables are new locals scoped to the loop body.
- If the array's length changes during iteration, the next iteration raises `array changed during iteration`. Modifying array elements without changing its length is allowed.
- To iterate through maps, use `m.keys()` or `m.values()`, which each return a new array.
- `break` and `continue` target the nearest enclosing loop, including when they appear inside a `switch`. Outside a loop, they raise `break or continue outside a loop`.

### 8.5. `switch`

```text
switch cmd {
case "add", "plus":
    total += n
case "clear":
    total = 0
case limit_name():
    println("limit")
default:
    eprintln($"unknown command: {|cmd|}")
}
```

- The selector is evaluated once.
- **Case labels** may be any expressions. Cases are tried from top to bottom and labels within each case from left to right, using `==` (Section 4.4). Evaluation stops at the first match, so the order of side effects is defined. Labels of different types are allowed.
- Repeated constant labels (literals or constants) cause `duplicate case label`. The first 64 constant labels of a `switch` are checked; duplicates among computed labels are not checked.
- **A case body** starts after `:` and runs to the next `case`, `default`, or closing `}`. Each case has its own scope. There is no fallthrough; an empty case has no effect.
- There may be at most one `default` (`several default clauses`). It may appear anywhere and executes when no case label matches.
- **Within `switch`, `break` and `continue` apply to the enclosing loop, not to `switch`**, unlike C:

```text
for line in lines {
    switch line {
    case "":
        continue          // skip to the next line
    case "end":
        break             // exit the for loop
    }
    handle(line)
}
```

### 8.6. `return`

A `return` must supply exactly as many values as the function declares as results (`wrong number of return values`). At script top level, use `return` or `return code` (Section 3.1).

## 9. Built-in Functions and Methods

Square brackets in a signature denote an optional argument, and `→` introduces results. An argument of the wrong type causes a runtime error.

### 9.1. Global Functions

| Function | Description |
| --- | --- |
| `type(x) → string` | Type name (Section 4.1) |
| `str(x) → string` | Text representation: `int` as decimal; `bool` as `true`/`false`; `null` as `null`; a string as itself; an array as `<array N>`; a map as `<map N>`; a file as `<file>`; an error as `object: message` |
| `error(text[, object[, code]]) → error` | Creates an error without raising it (Section 6.2) |
| `assert(cond[, msg])` | If `cond == false`, raises the runtime error `assertion failed: msg` |
| `min(a, b) → int`, `max(a, b) → int` | Operate on two `int` values |
| `abs(x) → int` | Absolute value; `abs(-32768)` is a runtime error |
| `print(x)`, `eprint(x)` | Identical to `io.print`, `io.eprint` (Section 10.3) |
| `println([x])`, `eprintln([x])` | Identical to `io.println`, `io.eprintln`; with no argument, output only `\n` |

### 9.2. String Methods

Arguments `start` and `end` specify a half-open byte interval `[start, end)`; values outside `0…len` cause a runtime error.

| Method | Description |
| --- | --- |
| `s.len() → int` | Length in bytes |
| `s.slice(start[, end]) → string` | Substring; `end` defaults to the length |
| `s.find(sub[, from]) → int` | Index of the first occurrence at or after `from`; −1 if absent |
| `s.contains(sub) → bool` | Equivalent to `sub in s` |
| `s.starts_with(p) → bool`, `s.ends_with(p) → bool` | Tests a prefix or suffix |
| `s.split(sep) → array` | Splits on nonempty `sep`: `"a,,b".split(",")` → `["a", "", "b"]`; `"".split(",")` → `[""]` |
| `s.lines() → array` | Splits at `\n`, removes a trailing `\r` from each line, and discards the final empty part; `"".lines()` → `[]` |
| `s.replace(old, new) → string` | Replaces every occurrence of nonempty `old` with `new` |
| `s.trim() → string` | Removes leading and trailing spaces, tabs, `\r`, and `\n` |
| `s.upper() → string`, `s.lower() → string` | Case conversion for Latin and Cyrillic letters using the FS16 fold table (Section 7.1): `A–Z`, `А–Я`, `Ё` ↔ `a–z`, `а–я`, `ё` |
| `s.equal_fold(t) → bool` | Compares both strings after `lower()` |
| `s.repeat(n) → string` | Produces `n` copies, with `n >= 0` |
| `s.pad_left(w[, ch]) → string`, `s.pad_right(w[, ch]) → string` | Pads to width `w` using byte `ch` (`int`, space by default); does not truncate longer strings |
| `s.to_int() → int` | Parses a number as described below |

**`to_int`** accepts optional surrounding whitespace and either a decimal number with optional `-` or `+` (−32768…32767), a hexadecimal number prefixed by `$`, `0x`, or `0X`, or a binary number prefixed by `0b` or `0B`. Nondecimal forms are parsed as 16-bit bit patterns (0…`$FFFF`). Other input raises `errors.NUMBER`, with the input string as the error object.

### 9.3. Array Methods

| Method | Description |
| --- | --- |
| `a.len() → int` | Number of elements |
| `a.push(v)` | Appends a value |
| `a.pop() → value` | Removes and returns the last value; a runtime error if empty |
| `a.insert(i, v)` | Inserts before position `i`, with `0 <= i <= len` |
| `a.remove(i) → value` | Removes and returns element `i` |
| `a.slice(start[, end]) → array` | New array containing `[start, end)` |
| `a.clear()` | Removes all elements |
| `a.clone() → array` | Shallow copy |
| `a.contains(v) → bool` | Equivalent to `v in a` |
| `a.index_of(v) → int` | Index of the first equal element, or −1 |
| `a.reverse()` | Reverses in place |
| `a.sort()` | Sorts in ascending order in place (Section 4.4). All elements must be `int`, or all must be strings; otherwise a runtime error. Sorting is unstable (median-of-three quicksort) and requires no additional heap memory |

### 9.4. Map Methods

Keys are strings. `keys()` and `values()` enumerate keys in their original insertion order; removing and reinserting a key places it at the end.

| Method | Description |
| --- | --- |
| `m.len() → int` | Number of entries |
| `m.has(k) → bool` | Equivalent to `k in m` |
| `m.get(k[, default]) → value` | Value or `default` (which defaults to `null`) |
| `m.set(k, v)` | Equivalent to `m[k] = v` |
| `m.delete(k) → bool` | Removes the key; `false` if it was absent |
| `m.keys() → array`, `m.values() → array` | New arrays |
| `m.clone() → map` | Shallow copy |

The value `null` is stored like any other value: after `m.set("a", null)`, `m.has("a")` is true and `m["a"]` equals `null`.

### 9.5. File and Error Methods

File methods are specified in Section 10.5; error methods in Section 6.2.
## 10. Standard Library

### 10.1. General Rules

- Paths use Monitor syntax: an optional volume `X:` (a letter `A`–`H`, case-insensitive, or digit `0`–`7`) or `dev:`, with `/` separators and `.` or `..` components. Paths longer than 255 bytes or deeper than 16 names fail with `errors.BADPATH`.
- File names are compared case-insensitively according to FS16 (Section 7.1). The same rules apply to `path.equal`, `fs.copy`, and `include` protection.
- Functions that invoke Monitor operations raise an error on failure (Section 6.4). The error object is the path of the failing operation. For `rename` and `copy`, it is whichever of the two paths caused the failure; for `proc` functions, it is the program name or redirection path.
- Only successful return values are shown below; an error can be caught from any call that can return one.
- Monitor limits: 16 file descriptors per process (including three standard streams), and 32 open files system-wide. The VM temporarily uses extra descriptors beyond those the script requires only within a single function call (`fs.list`, `fs.copy`, or `proc.capture`).

### 10.2. `str`

| Function | Description |
| --- | --- |
| `str.join(a, sep) → string` | Joins an array of strings with `sep`; a non-string array element is a runtime error |
| `str.from_byte(b) → string` | One-byte string; `0 <= b <= 255` |
| `str.hex(n[, width]) → string` | Uppercase hexadecimal representation of the 16-bit bit pattern, without a prefix, zero-padded to `width`: `str.hex(-1)` → `"FFFF"`, `str.hex(10, 4)` → `"000A"` |

### 10.3. `io` — Standard Streams

| Function | Description |
| --- | --- |
| `io.print(x)`, `io.println([x])` | Writes `str(x)` to stream 1; `println` appends `\n` |
| `io.eprint(x)`, `io.eprintln([x])` | Equivalent operations on stream 2 |
| `io.read_line() → string` | Reads the next line from stream 0, without `\n` or a trailing `\r`; returns `null` at end of input |
| `io.read_all() → string` | Reads the remainder of stream 0; more than 32767 bytes raises `errors.TOOBIG` |
| `io.flush()` | Writes buffered output |

**Buffering.** Output to streams 1 and 2 is buffered. The buffer is flushed on `\n` when the destination is the console; on `io.flush()`; before `io.read_line`, `io.read_all`, `proc.run`, or `proc.capture`; and on program termination, including runtime errors and Ctrl+C. Write failures on streams 1 and 2 are not reported. Programs that need to detect output errors should write through a file object instead.

### 10.4. `fs` — Files and Directories

| Function | Description |
| --- | --- |
| `fs.read_text(path) → string` | Reads the entire file; more than 32767 bytes raises `errors.TOOBIG` |
| `fs.write_text(path, text)` | Creates or replaces a file |
| `fs.append_text(path, text)` | Appends, creating the file if necessary |
| `fs.open(path, mode) → file` | Opens a text file (Section 10.5) |
| `fs.exists(path) → bool` | Whether a file, directory, or device exists; returns `false` on any error |
| `fs.is_dir(path) → bool`, `fs.is_file(path) → bool` | Existence tests restricted to the indicated entry type |
| `fs.list(dir) → array` | Lists directory entries as described below |
| `fs.mkdir(path)` | Creates a directory |
| `fs.remove(path)` | Removes a file or empty directory |
| `fs.rename(old, new)` | Renames or moves within a volume |
| `fs.copy(src, dst)` | Copies a file as described below |
| `fs.cwd() → string` | Current directory of the process |
| `fs.chdir(path)` | Changes the current directory, including for subsequently launched programs |

**`fs.list`** returns an array of maps in on-disk directory-entry order, including hidden and system entries. Each map contains `"name"` (entry name), `"dir"` (true for a directory), and flags `"hidden"`, `"system"`, and `"rdonly"`. File size and modification time are not returned. The entire directory is read and closed before the function returns.

**`fs.copy`** copies the bytes of `src` to `dst`, creating or replacing `dst`. If `dst` is an existing directory, the copy uses the name of `src` inside that directory. If `src` and `dst` refer to the same file (their normalized absolute paths compare equal ignoring case), it raises `errors.SAMEFILE` without modifying the file. A directory as `src` raises `errors.ISDIR`. The copy's modification timestamp is the current time.

### 10.5. Text Files

| `fs.open` mode | Meaning |
| --- | --- |
| `"r"` | Read |
| `"w"` | Write from the beginning; create or truncate the file |
| `"a"` | Append to the end; create the file if missing |

Any other mode string causes a runtime error. Opening a directory raises `errors.ISDIR`. A script may have no more than eight open files (`errors.MFILE`). Before reporting that error, the VM runs garbage collection, which closes unreachable files, and retries the operation.

| Method | Description |
| --- | --- |
| `f.read_line() → string` | Reads the next line without `\n` or trailing `\r`; `null` at EOF; more than 32767 bytes raises `errors.TOOBIG` |
| `f.read_all() → string` | Reads the remainder of the file; more than 32767 bytes raises `errors.TOOBIG` |
| `f.write(s)` | Writes a string (buffered) |
| `f.flush()` | Flushes the buffer to disk |
| `f.close()` | Flushes and closes; a second close does nothing |
| `f.path() → string` | Path used to open the file |

- Reading a write-only file or writing a read-only file causes a runtime error. Any operation other than `close` and `path` on a closed file raises `errors.CLOSED`.
- A write failure, such as `errors.NOSPC`, may not surface until a subsequent `write`, `flush`, or `close`. By default, errors from `close` propagate so they are not silently lost.
- An unreachable file is flushed and closed during garbage collection; a flush error at that time is lost. All open files are flushed and closed when the program terminates.
- Seeking, file-size queries, and binary reads are not supported.

### 10.6. `path`

These functions operate only on path strings; they do not access the disk.

| Function | Description |
| --- | --- |
| `path.join(a, b) → string` | Returns `b` if it is absolute; otherwise joins `a + "/" + b` without a duplicate `/` |
| `path.basename(p) → string` | Last path component: `"A:/a/b.txt"` → `"b.txt"` |
| `path.dirname(p) → string` | Removes the last component: `"A:/a/b.txt"` → `"A:/a"`; `"b.txt"` → `"."`; `"A:/x"` → `"A:/"` |
| `path.extension(p) → string` | Extension of the final name, including the dot (`".txt"`), or `""`; a leading dot does not count as an extension |
| `path.normalize(p) → string` | Removes `.` components, repeated and trailing `/`; collapses `name/..`; preserves leading `..` in a relative path and preserves case |
| `path.is_absolute(p) → bool` | Whether the path has a volume or begins with `/` |
| `path.equal(a, b) → bool` | Normalizes both and compares case-insensitively; relative paths are compared as written, without the current directory |
| `path.match(pattern, name) → bool` | `*` matches any sequence of bytes except `/`, `?` matches one byte, and other characters match literally ignoring case |

### 10.7. `proc` — Processes

| Function | Description |
| --- | --- |
| `proc.run(name, args[, redirect]) → int` | Starts a program and waits for it; returns the exit status |
| `proc.capture(name, args[, redirect]) → int, string` | Same, but returns the content of stream 1 as a string as well |
| `proc.which(name) → string` | Returns the program path found by the search rules, or `null` |
| `proc.exit(code)` | Terminates the program with status 0…32767 after flushing buffers |
| `proc.script() → string` | Script path as passed to `bsvm` |
| `proc.name() → string` | Name used in diagnostics: final component of the script path without its extension |

**Program lookup** by `run`, `capture`, and `which` follows the shell rules:

- If `name` contains `/` or `:`, try the path as given, then with `.cx`, then with `.bs`.
- Otherwise search the current directory, then `A:/bin`, trying `name`, `name.cx`, and `name.bs` in each location. Monitor compares names case-insensitively.

A found `.bs` file is launched as `A:/bin/bs.cx path args...`. If no program is found, the error is `errors.NOENT`.

**Command line construction.** The command line consists of the resolved path followed by the elements of `args` (an array of strings). An empty argument or an argument containing a space or tab is enclosed in quotation marks. An argument containing a double quote, a command line longer than 255 bytes, or more than 32 words raises `errors.ARGS`.

**Redirection** is specified by an optional `redirect` map:

| Key | Value |
| --- | --- |
| `"stdin"` | File path for the child's stream 0 |
| `"stdout"` | File path for stream 1 (not permitted with `capture`) |
| `"stderr"` | File path for stream 2 |
| `"append"` | If `true`, append to `stdout` and `stderr` instead of replacing them |

An unknown key or a value of the wrong type causes a runtime error. If opening a redirection file fails, the call raises an error and the child is not launched. If `stdout` and `stderr` specify the same path, both streams write to one file.

**`capture`** redirects the child's stream 1 to a temporary file `A:/tmp/bs-NNNN.out`, creating the directory if needed. After the child exits, the file is read and deleted. Output exceeding 32767 bytes raises `errors.TOOBIG`.

**Exit codes.** The child's exit code is returned as `int`. Monitor codes `$8000` and above appear negative:

| Constant | Code | Meaning |
| --- | --- | --- |
| `proc.BREAK` | `$8001` (−32767) | Child interrupted by Ctrl+C |
| `proc.BADINSN` | `$8008` (−32760) | Child terminated for an invalid instruction |

If the child exits with `proc.BREAK`, the script also terminates with status `$8001`; `run` does not return in that case. Launch failures (`NOENT`, `NOEXEC`, `NOMEM`, `NOPROC`, `ARGS`, etc.) raise errors whose object is the program name. A nonzero child exit status is not itself an error.

### 10.8. `time`

| Function | Description |
| --- | --- |
| `time.sleep(ticks)` | Sleeps for a number of Monitor timer ticks (50 Hz, 20 ms each); `0 <= ticks <= 32767` |

### 10.9. `errors`

The constants are the `error` values in the table in Section 6.6, with no object. This namespace has no functions.

## 11. Tools and Execution

### 11.1. Components

| Program | Purpose |
| --- | --- |
| `A:/bin/bsc.cx` | Compiler: source code → `.bsc` bytecode file |
| `A:/bin/bsvm.cx` | Virtual machine: executes `.bsc`; overlays reside alongside it in `A:/bin/bsvm.ovl` |
| `A:/bin/bs.cx` | Script launcher: compiles when needed and then launches |

### 11.2. `bsc`

```text
bsc [-l] source.bs [-o out.bsc]
bsc --run cache.bsc source.bs [args...]
```

- Without `-o`, the result is `source.bsc` next to the source. `-l` writes a bytecode listing to stream 1.
- `--run` (used by `bs`) compiles into `cache.bsc` and, on success, performs `EXEC` into `A:/bin/bsvm.cx cache.bsc source.bs args...`.
- **Diagnostics:** `file:line:column: message` on stream 2. The compiler stops at the first error.
- **Exit status:** 0 for success, 1 for a source-program error, 2 for command-line or I/O failure.
- The `.bsc` output is first written to a temporary file in the same directory and renamed on completion. An interrupted compilation therefore does not leave a corrupted cache file.

### 11.3. `bsvm`

```text
bsvm program.bsc [script-path] [args...]
```

- `script-path` is the source path used by `proc.script()` and diagnostics. If omitted, the path from the `.bsc` header is used. When invoking the VM directly and omitting the script path, supply `--` before any arguments: `bsvm prog.bsc -- a b`.
- **Exit status:** program's exit code (Section 3.1); 1 for an uncaught error (Section 6.4); 125 for a runtime error (Section 6.5); 126 when the `.bsc` file is missing, corrupt, or from an unsupported version, or `bsvm.ovl` or sufficient memory is unavailable; `$8001` for Ctrl+C.

### 11.4. `bs` and the Shell

```text
bs script.bs [args...]
```

1. Resolves an absolute, normalized script path.
2. Selects the cache: `script.bsc` alongside the source, or `A:/tmp/name.bsc` if the source directory is read-only.
3. A cache is valid only if it has the correct version, the main source path recorded in the cache equals the script path ignoring case, and the modification time (MTIME) of every source (main and included) matches.
4. For a valid cache, performs `EXEC` into `bsvm`; otherwise performs `EXEC` into `bsc --run`. Only one program in the execution chain is resident at a time, so the running program gets all available memory.

The auxiliary words (`bs` or `bsc --run` and paths) count toward the 255-byte and 32-word command-line limits, leaving fewer arguments available to the script than to an ordinary program.

**The shell** (revision 6) tries `name.bs` after `name.cx` during program lookup and launches a matching file through `bs`. Thus `A:/bin/backup.bs` can be invoked simply as `backup`, and shell redirection applies. An explicitly specified `prog.bsc` is launched by the shell as `bsvm path -- args...` without checking its cache. The `fm` file manager (revision 6) runs `.cx`, `.bs`, and `.bsc` files when Enter is pressed.

### 11.5. Ctrl+C

- While a child runs, the child receives Ctrl+C; the script follows the rule in Section 10.7.
- While the VM itself runs, its handler flushes standard-stream and open-file buffers, then terminates the process with status `$8001`.
- The compiler and `bs` do not install a handler: interrupting either cannot leave a corrupted output file.

### 11.6. `.bsc` File Format

A `.bsc` file (signature `"BSC1"`, format version 1) contains a header, a source-file table (absolute paths and MTIME, main file first), constants, a function table, stack-machine bytecode, and a source-line table for error reporting. The encoding is defined in *BScript Bytecode Format*, version 1.

## 12. Implementation Limits

A program exceeding a compiler limit is rejected with the diagnostic shown in the table. Exceeding a VM limit at runtime is a runtime error. Limits were measured for the Phase 1 implementation. Heap, overlay, and far-memory organization is documented in *BScript — Virtual Machine Internals* (`bscript_vm_en.md`).

| Quantity | Limit | Diagnostic |
| --- | --- | --- |
| Module code | 32 KB | `program too large` |
| Constants | 16 KB; 512 strings | `constants too large`, `too many constants` |
| Functions; global names | 160; 160 | `too many functions`, `too many global names` |
| Function locals | 250 slots; 96 names simultaneously | `too many local variables`, `too many local names` |
| Parameters; results; assignment targets | 16; 8; 9 | `too many parameters`, `too many results`, `too many targets` |
| Identifier | 62 characters | `name too long` |
| String literal; constant string expression | 512; 1024 bytes | `string too long`, `constant string too long` |
| Expression nesting | 24 levels | `expression nested too deep` |
| Block nesting; loop nesting | 32; 32 | `blocks nested too deep`, `loops nested too deep` |
| `include`: depth; number of files | 8; 32 | — |
| String; array | 32,767 bytes; 32,767 elements | `too large` |
| Map | 16,384 keys | `too large` |
| Call depth; value stack | 100 frames; 384 values | `stack overflow` |
| Open files per script | 8, each with a 192-byte buffer | `errors.MFILE` |
| Data memory | Near heap approximately 11.8 KB, then far memory up to 1 MB and 32,512 objects; far-memory string up to 7,674 bytes | `out of memory` |
## Appendix A. Grammar

Notation: `{ x }` means zero or more repetitions, `[ x ]` is optional, and `|` denotes an alternative. `EOS` is the end of a statement (Section 2.3). Tokens are written in quotation marks or uppercase letters.

```
program     = { include } { toplevel } .
include     = "include" STRING EOS .
toplevel    = constdecl | vardecl | fndecl | statement .

fndecl      = "fn" IDENT "(" [ params ] ")" [ ":" types ] block .
params      = param { "," param } .
param       = IDENT [ ":" TYPE ] .
types       = TYPE { "," TYPE } .
TYPE        = IDENT .              (* int bool string array map file error value *)

constdecl   = "const" IDENT "=" expr EOS .
vardecl     = "var" names [ ":" TYPE ] [ "=" expr ] EOS .
names       = IDENT { "," IDENT } .

block       = "{" { statement } "}" .
statement   = constdecl | vardecl | simple EOS | ifstmt | whilestmt
            | loopstmt | forstmt | switchstmt | returnstmt | failstmt
            | "break" EOS | "continue" EOS | EOS .
simple      = target { "," target } "=" expr
            | target assignop expr
            | target ( "++" | "--" )
            | call .
target      = IDENT | "_" | postfix index .
assignop    = "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^="
            | "<<=" | ">>=" | ">>>=" .

ifstmt      = "if" expr block [ "else" ( ifstmt | block ) ] .
whilestmt   = "while" expr block .
loopstmt    = "loop" block .
forstmt     = "for" IDENT [ "," IDENT ] "in" expr block .
switchstmt  = "switch" expr "{" { EOS } { caseclause } "}" .
caseclause  = ( "case" expr { "," expr } | "default" ) ":" { statement } .
failstmt    = "fail" expr [ "," expr [ "," expr ] ] EOS .
returnstmt  = "return" [ expr { "," expr } ] EOS .

expr        = orexpr [ "??" expr ] .
orexpr      = andexpr { "||" andexpr } .
andexpr     = cmpexpr { "&&" cmpexpr } .
cmpexpr     = bitor [ cmpop bitor ] .
cmpop       = "==" | "!=" | "<" | "<=" | ">" | ">=" | "in" .
bitor       = bitxor { "|" bitxor } .
bitxor      = bitand { "^" bitand } .
bitand      = shift { "&" shift } .
shift       = additive { ( "<<" | ">>" | ">>>" ) additive } .
additive    = term { ( "+" | "-" ) term } .
term        = unary { ( "*" | "/" | "%" ) unary } .
unary       = ( "-" | "+" | "~" | "!" ) unary | postfix .
postfix     = primary { index | "." IDENT [ args ] | args } .
index       = "[" expr "]" .
args        = "(" [ expr { "," expr } ] ")" .
call        = postfix .            (* final element is args *)
primary     = NUMBER | CHAR | STRING | ISTRING | "true" | "false" | "null"
            | IDENT | "(" expr ")" | arraylit | maplit .
arraylit    = "[" [ expr { "," expr } [ "," ] ] "]" .
maplit      = "{" [ expr ":" expr { "," expr ":" expr } [ "," ] ] "}" .
```

Restrictions not expressed by the grammar:

- `EOS` may be omitted before `}` (Section 2.3). An `EOS` after the `}` of a compound statement is parsed as an empty statement.
- `"." IDENT` without `args` is allowed only after a namespace name (for example, `errors.NOENT`).
- `args` following `primary` are allowed only for a function name.
- Multiple names in `vardecl`, and multiple targets in `simple`, are allowed only with a single call on the right-hand side. The number of targets must match the number of return values, or exceed it by one (Sections 5.3 and 6.4).
- `ISTRING` is tokenized together with its substitutions (Section 2.8).
- In an `if`, `while`, `for`, or `switch` condition, an unparenthesized map literal is not allowed (Section 8.3).

## Appendix B. Examples

Runnable examples with reference output are provided under the implementation's `examples/` directory: `logscan.bs` (directory traversal, error catching), `wc.bs` (`main`, `include`, multiple return values), `grep.bs` (options and `switch`), `tree.bs` (recursion), `backup.bs` (`fail` and application-defined error codes), `wordfreq.bs`, `calc.bs`, `todo.bs`, and `checkutils.bs` (`proc.run`, `proc.capture`).
