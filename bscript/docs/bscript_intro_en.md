# Getting Started with BScript

BScript is a language for small programs that would take too much effort to write in B16: traversing directories, parsing text, launching utilities, and checking their output. This introduction teaches the language through examples. For precise rules, see the *BScript Language Specification* (`bscript_spec_en.md`).

## Why BScript? Your first program

If you have used Go, Python, or JavaScript, much of BScript will feel familiar. It has dynamic types, garbage collection, and built-in strings, arrays, and maps. Its lexical conventions come from B16: the same comment syntax and line-ending rules, and character literals such as `'A'` represented as numbers.

The shortest program is a single line. Save this in `hello.bs`:

```text
println("hello, world")
```

Run it from the shell:

```text
bs hello.bs
```

The `bs` launcher compiles the file into `hello.bsc` beside the source and executes it. On subsequent runs, compilation is skipped if the source has not changed. If the script is in the current directory or `A:/bin`, the shell can find it by name like an ordinary utility: simply enter `hello`.

Command-line arguments are available in the `args` array:

```text
if args.len() == 0 {
    println("hello, stranger")
} else {
    println($"hello, {|args[0]|}")
}
```

The `$"...{|expression|}..."` syntax is string interpolation: the expression's value is inserted as text.

**Scripts versus programs with `main`.** The preceding examples are scripts: statements appear at top level and run in source order. Larger programs may use a `main` function whose return value becomes the exit status:

```text
fn main(args: array): int {
    if args.len() == 0 {
        eprintln("usage: hello name")
        return 2
    }
    println($"hello, {|args[0]|}")
    return 0
}
```

In a program defining `main`, only declarations may appear at top level. The two styles cannot be mixed.

**Newlines and semicolons.** A newline terminates a statement; no semicolon is required. To continue a long expression, break the line immediately after an operator, comma, or opening bracket:

```text
var total = first +
            second          // Valid continuation
var wrong = first
            + second        // Error: the preceding line already ended the statement
```

## Variables, numbers, and strings

Declare variables with `var` and constants with `const`. Types can be inferred, or specified explicitly; explicit types are checked at every assignment:

```text
var count = 0
var name: string = "report.txt"
var nothing                 // null
const LIMIT = 100
```

BScript has eight value kinds: `null`, `bool`, `int`, string, array, map, open file, and error. Use `type(x)` to inspect the kind of a value.

**Numbers.** There is one integer type, `int`: signed, 16-bit, ranging from −32,768 to 32,767. Overflow wraps silently modulo 2^16, as in B16: `32767 + 1` yields −32768. Division `/` truncates toward zero. Hexadecimal (`$FF`, `0xFF`) and binary (`0b1010`) literals, bitwise operators, and shifts (including unsigned `>>>`) are supported. There is no floating-point type.

**Strings.** Strings are immutable and may contain arbitrary bytes. They can be enclosed in double quotes or backticks; backticks are convenient for text containing quotation marks:

```text
var a = "line\n"
var b = `he said "yes"`
var c = a + b               // Concatenation
var n = c.len()             // Length in bytes
```

Numbers do not convert to strings automatically: `"n = " + 5` is an error. Write `"n = " + str(5)` or, preferably, `$"n = {|5|}"`. Use `s.to_int()` for the reverse conversion.

Frequently used string methods include `slice`, `find`, `split`, `lines`, `trim`, `replace`, `starts_with`, `upper`, `lower`, and `pad_left`. Use `in` to test for a substring: `"err" in line`.

**Characters are numbers.** `s[i]` returns a byte as `int`, while `'A'` denotes the character code. Compare against character literals, not strings:

```text
if s[0] == '#' { continue }     // Correct
if s[0] == "#" { continue }     // Always false: an integer is not a string
```

**Equality.** `==` between values of different kinds returns `false` rather than failing. Checks such as `x == null` or `cfg["port"] == 80` are therefore safe. Arrays and maps compare equal only when they refer to the same object.

**`null` and `??`.** A missing map key, an uninitialized `var`, and end-of-input are all represented by `null`. The `a ?? b` operator evaluates to `b` if `a` is null:

```text
var port = cfg["port"] ?? "8080"
```

## Conditions and loops

Conditions do not require parentheses, but statement blocks must be enclosed in braces:

```text
if n == 0 {
    println("none")
} else if n < 10 {
    println("few")
} else {
    println("many")
}
```

A condition must actually be `bool`. Writing `if n { }` when `n` is an integer raises a runtime error; use `if n != 0 { }`. Logical operators `&&`, `||`, and `!` also require boolean operands.

**Three kinds of loops.** `while` is conditional, `loop` is unconditional, and `for ... in` iterates over a collection:

```text
var i = 0
while i < 10 { i++ }

loop {
    var line = io.read_line()
    if line == null { break }   // End of input
    println(line.upper())
}

for name in ["a", "b", "c"] { println(name) }
for i, ch in "abc" { println($"{|i|}: {|ch|}") }   // String bytes as integers
```

Do not append to or remove from an array while iterating over it with `for`; changing existing elements is allowed. To iterate over a map, use `m.keys()` or `m.values()`.

**`switch`** compares the selector with case expressions in order, without falling through. Case labels can be arbitrary expressions, and several may appear in one case separated by commas:

```text
switch cmd {
case "add", "+":
    total += n
case "clear":
    total = 0
default:
    eprintln($"unknown command {|cmd|}")
}
```

Unlike C, `break` and `continue` inside a `switch` apply to an enclosing loop. A `break` in a case exits the loop, not the `switch`.

`++` and `--` are statements rather than expressions: `i++` is valid, but `a[i++]` is not. Assignment is also a statement, so constructs like `if (x = f()) ...` are not supported.

## Functions and errors

Functions are declared with `fn`. Parameter type annotations are optional, but result types are required: the compiler uses them to determine how many values a function returns.

```text
fn greet(name: string) {
    println($"hello, {|name|}")
}

fn divmod(a: int, b: int): int, int {
    return a / b, a % b
}

var q, r = divmod(17, 5)        // 3, 2
var _, rest = divmod(9, 4)      // _ discards a result
```

A function may be called before its definition. Functions are not first-class values: you cannot store them in variables or pass them as arguments. Nested functions are not supported either. For functions with results, the compiler checks that all execution paths end in `return`.

**Errors are values.** Library functions that may fail (missing file, insufficient space, invalid integer) return values of type `error`. The key rule is that **an error you do not explicitly capture propagates automatically through the calling function**. A minimal script need not check errors at all to behave correctly:

```text
var text = fs.read_text(args[0])
println(text.lines().len())
```

If the script is named `count.bs` and the file is absent, it prints `count: notes.txt: no such file or directory` (like a shell utility) and exits with status 1.

**Capture an error with one extra variable.** Add an assignment target after the normal results of a call. That target receives the error, or `null` if the call succeeded:

```text
var text, err = fs.read_text(name)
if err == errors.NOENT {
    text = ""                       // Missing file is acceptable
} else if err != null {
    eprintln($"{|proc.name()|}: {|err|}")
    return 1
}
```

Errors compare by code: `err == errors.NOENT` matches any “file not found” error. Methods on an error include `code()`, `text()`, `object()` (usually the path), and `where()` (its source location).

**Raise your own errors with `fail`.** `fail` returns an error from a function, much as `return` returns a normal value:

```text
fn parse_port(s: string): int {
    var n, err = s.to_int()
    if err != null || n <= 0 {
        fail "bad port", s           // Error message and associated object
    }
    return n
}
```

Use `fail err` to propagate a captured error unchanged. Start custom error codes at 1000: `fail "locked", name, 1001`.

**Programming mistakes** are different. An out-of-bounds index, division by zero, or addition of a number and string cannot be caught: the program stops with `bsvm: file:line: message` and exit code 125. These errors are meant to be fixed in the program rather than handled at runtime.

## Arrays and maps

An **array** is a mutable sequence of values of any type, indexed from zero:

```text
var a = [3, 1, 2]
a.push(5)
a[0] = 4
a.sort()                         // [1, 2, 4, 5]
println(a.len())                 // 4
if 2 in a { println("found") }
var tail = a.slice(1)            // New array [2, 4, 5]
```

Other array methods include `pop`, `insert`, `remove`, `index_of`, `reverse`, `clear`, and `clone`. There are no negative indices counting from the end: access the last element with `a[a.len() - 1]`. `sort` accepts only an array containing all integers or all strings.

A **map** associates string keys with values of any type. Keys are iterated in insertion order:

```text
var m = {"name": "x", "size": 10}
m["owner"] = "root"
println(m["none"])               // null: a missing key is not an error
if "size" in m { m.delete("size") }
for k in m.keys() { println($"{|k|} = {|m[k]|}") }
```

A common pattern is counting occurrences, using `get` with a default value:

```text
var freq = {}
for w in words {
    freq[w] = freq.get(w, 0) + 1
}
```

**References, not copies.** Arrays and maps are passed by reference: a function receiving an array can modify the original. Use `clone()` for a shallow copy; nested arrays remain shared.

**Map literals in conditions.** After `if`, `while`, `for`, or `switch`, an opening `{` begins a block. Enclose map literals in parentheses in those positions: `if key in ({"a": 1, "b": 2}) { }`. Parentheses are unnecessary in ordinary expressions.

Data size is limited: strings and arrays can contain at most 32,767 bytes or elements, and maps at most 16,384 keys. If near memory is insufficient, the VM automatically switches to extended memory (up to 1 MB). No source changes are required, although execution becomes slower.

## I/O, files, and programs

The library is organized into namespaces: `io`, `fs`, `path`, `proc`, `str`, `time`, and `errors`. Access members with a dot, as in `fs.read_text(p)`. The most common functions are available without a prefix: `print`, `println`, `eprint`, `eprintln`, `str`, `type`, `min`, `max`, `abs`, `assert`, and `error`.

**Standard streams.** `println` writes to stream 1, and `eprintln` writes diagnostics to stream 2. Output is buffered and flushed automatically on a console newline, before reading input, before launching another program, and at exit. `io.read_line()` reads a line from stream 0 and returns `null` at EOF; `io.read_all()` reads everything remaining.

**Whole-file I/O** is simplest for small files:

```text
var text = fs.read_text("notes.txt")
fs.write_text("upper.txt", text.upper())
fs.append_text("log.txt", "done\n")
```

**Line-by-line I/O** uses an open file:

```text
var f = fs.open("big.log", "r")          // "r", "w", or "a"
var n = 0
loop {
    var line = f.read_line()
    if line == null { break }
    if "ERROR" in line { n++ }
}
f.close()
```

Close files explicitly: a program may have only eight open files at once. The garbage collector eventually closes forgotten files, but any error while flushing them will be lost. Only text files are supported so far; seeking, file-size queries, and binary reads are not available.

**Directories and paths.** `fs.list(dir)` returns an array of maps with keys `"name"`, `"dir"`, `"hidden"`, `"system"`, and `"rdonly"`. The `path` namespace manipulates path strings without accessing the disk:

```text
for e in fs.list(".") {
    if !e["dir"] && path.match("*.txt", e["name"]) {
        println(path.join(fs.cwd(), e["name"]))
    }
}
```

FS16 filenames are case-insensitive. Compare them with `path.equal(a, b)` or `a.equal_fold(b)`, not `==`.

**Launching programs.** `proc.run` launches a program as the shell would, waits for it, and returns its exit status. `proc.capture` also returns the program's output as a string. Redirections are passed as a map:

```text
var code = proc.run("b16c", ["prog.b16", "-o", "prog.s"])
if code != 0 { fail $"b16c failed: {|code|}" }

var rc, out = proc.capture("ls", ["/bin"])
proc.run("sort", [], {"stdin": "in.txt", "stdout": "out.txt"})
```

A nonzero child exit status is not an error object; your code must inspect the value. An error means that the program could not be started (`errors.NOENT`, `errors.NOEXEC`). Pressing Ctrl+C in a child also terminates the script.

**Including source files.** Put shared functions in a separate file and include it at the beginning of the program:

```text
include "lib/text.bs"
```

The include path is resolved relative to the including file's directory. Repeated includes are skipped. All declarations share one program namespace, so choose names that do not collide.

## Complete example and common pitfalls

The following script counts lines containing a given marker across all `*.log` files in a directory. It combines most topics from this introduction: arguments, `fs.list`, continuing after a captured error, line-by-line input, and interpolation.

```text
// logscan.bs — count lines containing a marker in the directory's *.log files
//   logscan mark [dir]
if args.len() < 1 {
    eprintln("usage: logscan mark [dir]")
    return 2
}
var mark = args[0]
var dir = "."
if args.len() > 1 { dir = args[1] }

var total = 0
// A directory-read error is not captured, so the script exits
// with "logscan: directory: message" and status 1.
for e in fs.list(dir) {
    if e["dir"] || !path.match("*.log", e["name"]) { continue }
    var name = path.join(dir, e["name"])
    var f, err = fs.open(name, "r")
    if err != null {
        eprintln($"{|proc.name()|}: {|err|}")   // Report the error and continue
        continue
    }
    var count = 0
    loop {
        var line = f.read_line()
        if line == null { break }
        if mark in line { count++ }
    }
    f.close()
    println($"{|e["name"].pad_right(20)|} {|count|}")
    total += count
}
println($"total {|total|}")
```

More examples with reference output are included in the implementation's `examples/` directory: `wc.bs`, `grep.bs`, `tree.bs`, `backup.bs`, `wordfreq.bs`, `calc.bs`, `todo.bs`, and `checkutils.bs`.

### Pitfalls

| Code | What happens | Correct approach |
| --- | --- | --- |
| `if count { }` | Runtime error: condition is not bool | `if count != 0 { }` |
| `s[i] == "a"` | Always false: a byte is an integer | `s[i] == 'a'` |
| `"n=" + n` | Runtime error: string plus integer | `$"n={|n|}"` or `"n=" + str(n)` |
| Breaking an expression before `+` | The preceding line already ended the statement | Break after the operator |
| `break` in a `switch` inside a loop | Exits the loop rather than the `switch` | Cases do not fall through; no `break` is needed |
| `a == b` for arrays | Compares references, not contents | Compare elements explicitly |
| `fs.read_text(p).lines()` with an error check | Errors inside an expression are not captured | Use `var t, err = fs.read_text(p)` and then `t.lines()` |
| `name == "README.TXT"` for filenames | Case-sensitive comparison | Use `path.equal(name, "README.TXT")` |
| `a.push(x)` during `for x in a` | Raises `array changed during iteration` | Collect modifications in a separate array |
| `30000 + 30000` | Silently wraps to −5536 | Keep values within the `int` range, −32768…32767 |
| Reading a large file with `fs.read_text` | `errors.TOOBIG` beyond 32,767 bytes | Read line by line using `fs.open` |

If something behaves unexpectedly, consult the specification: it defines the rules precisely and they are covered by implementation tests.
