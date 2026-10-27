# B16 Examples

Examples are grouped by the environment in which they run. The CX16 executable image is identical for both execution modes (B16 reference, section 9.2), but the facilities used by a program can tie it to a particular environment. On a bare machine, the program owns the interrupt vectors, timer, and keyboard. Under the Monitor, those resources belong to the operating system and are accessible only through system calls (*CPU16 Monitor: System Calls*, section 2.2).

| Directory | Runs on | Uses |
| --- | --- | --- |
| `universal/` | Bare machine and Monitor | `lib/io.b16`: console port or standard streams, selected by the signature at `$0004` |
| `bare/` | Bare machine only | Own interrupt vector table, timer `$FF29`, keyboard and display ports (`lib/console.b16`) |
| `monitor/` | Monitor only | Monitor system calls (`sys.b16`) for arguments, files, processes, keyboard events, and ticks |

## universal — bare machine and Monitor

| Example | Demonstrates |
| --- | --- |
| `hello` | Minimal program |
| `sieve` | Arrays, nested loops, constants |
| `sort` | Recursion, function pointers, pointer arithmetic, strings |
| `list` | Structures, memory pool, pointer-to-pointer, multiple results |
| `calc` | Recursive parsing, value-and-position pairs, signed division |
| `fsm` | Finite-state machine using `goto` |
| `dispatch` | Command dispatch table: structures containing function pointers |
| `funcptr` | Callbacks, jump tables, “objects” with method tables, returning a function |
| `bignum` | `ulong`: factorials and Fibonacci numbers up to 2³² |
| `echo` | Line input: keyboard on bare hardware, stdin under the Monitor (`echo < echo.txt`) |
| `asmlink/` | Linking with an assembly module (`include asm`), calling in both directions |
| `tetris` | One game supporting both environments: its own vectors and timer on bare hardware; `TICKS`, `SLEEP`, and `CON_GETEV` under the Monitor. The game controls the display directly. |

Except for `tetris`, these programs print text through `lib/io.b16`. Under the Monitor, output goes to the standard output stream, so redirection such as `hello > A:/tmp/h.txt` works. For `echo < file`, input comes from the file, whose text uses the machine character encoding.

On a bare machine:

```sh
./run.sh examples/universal/hello.b16
./run.sh examples/universal/echo.b16 < examples/universal/echo.txt
```

Under the Monitor (from `monitor/`):

```sh
tools/b16build.sh ../b16c/examples/universal/hello.b16      # -> out/hello.bin
```

Copy `out/hello.bin` to the volume as `/bin/hello.cx` (using `tools/fs16tool.py` or the shell's `cp` command), then run it with `hello`.

## bare — bare machine only

| Example | Demonstrates |
| --- | --- |
| `inline_asm` | Assembly functions, flags, local labels, calling B16 from `asm`, ERRCMD and TRAP handlers |
| `devices` | Console-register structure at a fixed address; keyboard scan codes (`cpu16 devices.bin < devices.txt`) |
| `life` | Conway's Game of Life: 2D arrays, pointers to arrays, `char`, screen, timer |
| `clock` | Timer, interrupt handler, `atomic` |
| `timer` | Minimal IRQ0 handler and `wfi` |
| `screen` | Positioned display mode, box-drawing characters, colors, and attribute mode |
| `demo` | One-file overview of the language |
| `tetris_bare` | Bare-machine-only Tetris; compare with `universal/tetris.b16` |

```sh
./run.sh examples/bare/screen.b16 --screen
```

Do not run these programs under the Monitor: writing to the interrupt vector table or timer and reading keyboard ports directly would interfere with the operating system.

## monitor — Monitor only

| Example | Demonstrates |
| --- | --- |
| `args` | `argv`, redirection flags in PIB, `MEMINFO`, error stream, exit status, `option max_top` |
| `wc` | `OPEN`, `READ`, `CLOSE`; utility-style errors; `ulong` counters; stdin |
| `keys` | Raw console mode (`CON_MODE`); `CON_GETEV` events with scan codes and modifiers |
| `ticks` | `TICKS`, `SLEEP`, the `TICKHOOK` handler, and `ONEVENT` for Ctrl+C |
| `run` | `SPAWN` with child output redirected to a file, exit status, elapsed time |

`mon.b16` provides shared helpers for these examples (detecting the execution environment, writing characters, numbers, and errors). String output and `puthex` are provided by `sys.b16`.

Build from `monitor/`:

```sh
tools/b16build.sh ../b16c/examples/monitor/wc.b16          # -> out/wc.bin
```

Once copied onto the volume as `/bin/wc.cx`, run it from the shell:

```sh
wc /data/a.txt /data/b.txt
wc < /data/a.txt
run -o A:/tmp/ls.txt A:/bin/ls.cx -l /bin
ticks
```

The compiler's `make test` only assembles these examples: `tests/monrun` emulates just the `READ`, `WRITE`, and `EXIT` system calls.

## Windows

The examples are distributed alongside `b16c.exe`, `cpu16asm.exe`, and two batch scripts:

- **`c.bat example.b16`** builds a single example. The compiler's `-s` option includes source text as assembly comments and writes `example.s`; the assembler's `--exec` mode produces an image, `.lst` listing, and `.map` file beside the source. The output image type depends on the directory: `bare\` produces `example.bin` for bare hardware (`cpu16 example.bin`); `monitor\` produces `example.cx` for the Monitor; `universal\` and `universal\asmlink\` produce both filenames for the same image. When compiling from `monitor\`, the compiler also receives `..\..\monitor\user\lib` and `..\..\monitor\kernel` include paths. Pass additional compiler options through `B16FLAGS`.
- **`call.bat`** builds all examples in the subdirectories (excluding the `monitor\mon.b16` library) and reports how many succeeded or failed.

Place the `.cx` file on a Monitor volume as `/bin/example.cx` (with `monitor\tools\fs16tool.py` or the shell's `cp` command), then start it by entering `example`. Build artifacts are listed in `.gitignore`.
