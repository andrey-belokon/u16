# BScript for CPU16: Phase 1

BScript is a scripting language for CPU16, as defined in *BScript — Language Specification (Phase 1)*, version 1.1 (`docs/bscript_spec_en.md`). The implementation comprises a compiler, a virtual machine, and a script launcher. All three are written in B16 and run under the CPU16 Monitor.

| Program | Purpose |
| --- | --- |
| `A:/bin/bsc.cx` | Compiler: `bsc [-l] source.bs [-o out.bsc]`, `bsc --run cache.bsc source.bs [args...]` |
| `A:/bin/bsvm.cx` | Virtual machine: `bsvm program.bsc [script-path] [args...]`; its overlays are stored alongside it in `A:/bin/bsvm.ovl` |
| `A:/bin/bs.cx` | Launcher: `bs script.bs [args...]`; compiles when necessary (using a `.bsc` cache) and then executes the script |

When searching for a command, the revision-4 Monitor shell also looks for scripts named `name.bs`, so `A:/bin/backup.bs` can be launched simply by entering `backup`.

## Building and testing

    make            Build out/bsc.bin, out/bsvm.bin, out/bsvm.ovl, out/bs.bin (requires ../b16c/b16c and ../cpu16/build/cpu16asm)
    make test       Run tests/*.bs in the ../monitor/out/kmon harness, comparing output with tests/*.out
    make examples   Run examples/*.bs and compare output with examples/expected/*.out
    make stress     Build test variants of bsvm: GC before every allocation (rule 12.3),
                    and immediate far-memory mode with a 2 KB near heap (section 12.10)
    python3 tools/run_tests.py --update [name ...]     Regenerate expected outputs

`make system` and `make dist` in `../monitor` build BScript and place `bsc.cx`, `bsvm.cx`, `bsvm.ovl`, and `bs.cx` in the system volume's `/bin` directory.

`tools/vmrun.py` runs shell commands on a temporary volume containing all of the tools:

    python3 tools/vmrun.py -f prog.bs:/t/prog.bs "bs /t/prog.bs" "bsc -l /t/prog.bs"

## Directory layout

| Directory | Contents |
| --- | --- |
| `common/` | `bsdefs.b16`: `.bsc` format, opcodes, built-in function and method IDs; `util.b16`: path and string helpers |
| `bsc/` | Compiler: lexer `lex.b16`, symbol tables `sym.b16`, expressions `expr.b16`, statements `stmt.b16`, built-ins `builtins.b16`, extended-memory buffers `xbuf.b16`, listing generator `list.b16`, and entry point `bsc.b16` |
| `vm/` | VM resident code: types `types.b16`; heap, garbage collector, and accessors `heap.b16`; maps `map.b16`; values `value.b16`; streams and files `io.b16`; interpreter `interp.b16`; operations `ops.b16`; frequently used built-in functions and methods `lib.b16` and `libstr.b16`; overlay manager `ovl.b16`; startup `bsvm.b16`. Overlays: `ov_fs.b16` (fs, path), `ov_sys.b16` (proc, time, infrequent methods, error messages), `ov_gc.b16` (infrequent far-memory routines, `fail`, `error()`), `ov_init.b16` (loader), and shared routines `ov_common.b16` |
| `bs/` | Script launcher |
| `examples/` | Sample scripts and programs (`examples/README_en.md`) with expected outputs |
| `tests/` | Test cases: a `.bs` program and a corresponding `.out` reference file. The first-line `// run: command` specifies how to launch it. Includes `tests/inc/` and `tests/data/` for supporting files and data. |
| `bench/` | Benchmark programs (specification, section 13.2) |
| `docs/` | Language specification version 1.1 and *BScript Bytecode Format* |
| `tools/` | `build.sh`, `mkovl.py` (overlay builder), `vmrun.py`, `run_tests.py`, `stress.sh` (`gc`, `far`, `both`), and `profile.py` (function-level profiling with `kmon --profile`) |

## Implementation

**Compiler.** Code, constants, and the string table reside in extended-memory frames accessed through a window on page 6 (`bsc/xbuf.b16`). A preliminary pass gathers global names, function signatures, and constants. The second pass performs recursive-descent parsing with code generation in a single pass. An expression descriptor defers the emission of constants and lvalues, allowing constant folding and assignment targets to be handled without constructing a syntax tree. Function bodies appear at their declaration positions within the top-level code; jumps allow top-level execution to skip over them. The compiler holds the complete module in memory and writes the `.bsc` output via a temporary file followed by a rename.

**Virtual machine.** Values occupy four bytes (section 12.2). Heap objects are referenced through a handle table. The mark-compact collector uses an explicit marking stack and rescans the heap if that stack overflows. Built-in functions follow the temporary-root rule. All library code accesses objects exclusively through heap-access functions (rule 12.3; `vm/heap.b16`); `make stress` tests compliance by collecting before every allocation. Module constants are static objects stored directly in the module image. The source-line table is not loaded into memory: when an error occurs, the relevant location is looked up in the `.bsc` file. Stream and file buffers are pinned; the Ctrl+C handler flushes them before terminating with status `$8001`.

**Overlays** (section 12.9). Page 6 serves as a mapping window. Normally it contains the module frame: function table, bytecode, and call frames. Less frequently used, larger library routines are assembled for the window address and stored in `bsvm.ovl`: `fs` (fs, path), `sys` (proc, time, less-used methods, error messages), and `gc` (less-used far-memory support). Common string, array, map, and file functions remain resident. Calling an overlay function maps the corresponding frame into the window (loading it on first use); returning restores the previous frame. Nested overlay calls are supported. The loader is an `init` overlay mapped on page 5 and executed once. `tools/mkovl.py` identifies resident functions that invoke overlays (only these need to be included in the image), creates entry stubs, generates resident declarations for overlays, and builds the overlay file.

**Far memory** (section 12.10). When the near heap cannot accommodate more objects, the VM switches to extended-memory mode. Page 5 becomes a data window. New objects are allocated in the near heap, while survivors are moved into extended-memory frames at safe points between bytecode instructions. A write barrier records far-memory containers that reference near-heap objects. Full garbage collection compacts the frames. Arrays and maps exceeding 4 KB are stored in chunks. Far memory is limited to 1 MB and 32,512 objects. The B16 stack is moved beyond the argument block to free page 5.

**Launcher.** Cached bytecode is valid only if the compiler version, main source path, and MTIME of every source file match. For a valid cache, the launcher executes `bsvm` via `EXEC`; otherwise, it invokes `bsc --run`. When the source directory is read-only, the cache is placed in `A:/tmp`.

## Measurements and limits

See specification sections 12.1, 12.9, 12.10, and 13.2 for details.

| Item | Value |
| --- | --- |
| `bsvm` | Resident image: 27.6 KB; 32.3 KB including data. Overlays: 21.1 KB total (`fs` 5.5, `sys` 6.6, `gc` 5.6, `init` 3.3 KB). |
| `bsc` | 31.4 KB image; 44 KB near memory. Code, constants, and the string table reside in extended memory. Compiling takes approximately 9,400 instructions per source line. |
| VM near heap | Approximately 11.8 KB for a small script; approximately 3.6 KB in extended-memory mode. |
| Far memory | Up to 1 MB (127 frames), at most 32,512 objects; strings stored there can be up to 7,674 bytes long. |
| Module | Up to 32 KB of code and 16 KB of constants (`bsc` limits, section 12.6). Modules up to 8 KB fit in one frame; larger ones use multiple code frames. Large constants are stored in frames. |
| Collections | Up to 32,767 array elements or 16,384 map keys. |
| `bsc` nesting | Up to 24 expression levels and 32 nested blocks. |
| Calls | Up to 100 call frames and a 384-entry value stack. |
| Files | Up to 8 open files per program, each with a 192-byte buffer. |
| Performance | About 59 CPU16 instructions per bytecode instruction. An overlay call adds roughly 100 instructions (using the Monitor's fast `XMAP` path), accounting for less than 0.5% of example runtimes. Extended-memory mode is approximately 1.6× slower than normal mode when accessing objects across frames. |
