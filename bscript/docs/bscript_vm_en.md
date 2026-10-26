# BScript — Architecture of the `bsvm` Virtual Machine

This document explains how the Phase 1 BScript virtual machine (`bscript/vm/`) works and why it was designed this way. The language is specified in *BScript — Language Specification* (`bscript_spec_en.md`); the `.bsc` file layout and instructions are specified in *BScript Bytecode Format, Version 1* (`bscript_bytecode_en.md`). This document covers the implementation between those two interfaces: process memory, values and objects, the interpreter, the heap and garbage collector, extended memory, overlays, I/O, build procedures, and performance measurements.

`bsvm` is written in B16 and runs under CPU16 Monitor. Its resident executable occupies 27.6 KB (32.3 KB including data), with another 21.1 KB of overlays stored in `bsvm.ovl`.

## 1. Source files

| File | Purpose |
| --- | --- |
| `types.b16` | `Val`, `Obj`, `Slot`, and `Frame` structures; object data layouts; overlay numbers |
| `bsvm.b16` | Entry point: stack relocation, loading, near-memory layout, and invocation of function 0 |
| `interp.b16` | Interpreter registers, calls and returns, call modes, and error unwinding |
| `ops.b16` | Bytecode instruction handlers, the `ops` dispatch table, and the `run` loop |
| `value.b16` | Tags and types, equality, `str(x)`, error objects, and error messages |
| `heap.b16` | Handles, allocation, access functions, write barrier, near-heap collection, arrays, and chunks |
| `map.b16` | Dictionaries: insertion-ordered hash table |
| `io.b16` | Stream/file slot table, buffers, string builder, and Ctrl+C handler |
| `lib.b16`, `libstr.b16` | Resident built-in functions and methods; `bfuncs`, `mt_str`, `mt_arr`, `mt_map`, `mt_file`, and `mt_err` tables |
| `ovl.b16` | Window manager: module frame, code frames, overlay loading and calls |
| `ov_init.b16` | `init` overlay: module loader |
| `ov_fs.b16` | `fs` overlay: `fs` and `path` namespaces |
| `ov_sys.b16` | `sys` overlay: `proc`, `time`, infrequently used methods, sorting, runtime error reporting, and `e.where()` |
| `ov_gc.b16` | `gc` overlay: transition to extended mode, promotion, full collection, far-memory allocation, `fail`, and `error()` |
| `ov_common.b16` | Shared by `fs` and `sys`: path arguments, open-with-GC-retry, and reading an entire file |

Definitions shared with the compiler live in `common/bsdefs.b16` (the `.bsc` format, opcodes, and built-in function and method IDs) and `common/util.b16`.

## 2. Process memory

`bsvm` requests `max_top = $C000`. CPU16 has eight 8 KB address-space pages; the VM uses them as follows:

| Pages | Addresses | Purpose |
| --- | --- | --- |
| 0–4 | `$0000–$9FFF` | Resident image, arguments, B16 stack, module tables, value stack, handles, and near heap |
| 5 | `$A000–$BFFF` | Near-heap continuation; the far-data window in extended mode; the `init` loader window during startup |
| 6 | `$C000–$DFFF` | Code and overlay window: normally the module frame or current code frame; temporarily an overlay frame during a call |

Within the VM's own memory, areas are laid out from low to high addresses:

| Area | Size | Defined by |
| --- | --- | --- |
| Resident code, data, BSS, stream and file buffers | 32.3 KB (23.9 KB code, 8.4 KB data, including 2.3 KB of buffers) | — |
| Command-line argument block | Up to 300 bytes | Monitor |
| B16 stack | 1.5 KB (`CSTACK`); measured peak around 1 KB | `bsvm.b16` |
| Module source-file table and constants | Depends on `.bsc` size | `ov_init.b16` |
| Function table and frame table (only for modules larger than 8 KB) | Size-dependent, plus 800 bytes | `ov_init.b16` |
| Global variables | 4 bytes per global | `bsvm.b16` |
| Constant-number-to-handle table | 2 bytes per constant | `bsvm.b16` |
| Value stack | 384 values, 1.5 KB (`VSMAX`) | `bsvm.b16` |
| Handle table | 3 bytes (address and segment) per 22 bytes of heap | `heap_init` |
| Near heap | Remaining space up to `$C000` (up to `$A000` in extended mode) | `heap.b16` |

**The B16 stack is relocated.** `main` in `bsvm.b16` immediately switches `SP` to the area above the argument block (`onstack`) before calling `vm_main`. The stack must not remain below `max_top`: page 5 must be available as a data window.

## 3. Startup

1. `io_init` initializes stream slots 0–2 and uses `HINFO` to identify console streams.
2. `ovl_init` constructs the path to `bsvm.ovl` by replacing the `.cx` extension of `argv[0]` with `.ovl`.
3. `init_run` allocates a frame (`XALLOC`), maps it to page 5, loads the `init` image there, and calls `vm_load`. When it returns, the VM's own frame is restored to page 5 and the loader frame is freed. The loader runs only once and never occupies window 6.
4. `vm_load` reads the `.bsc` header, validates the `BSC1` signature and version, and lays out the module (Section 3.1). On failure, it prints `bsvm: path: message` and exits with status 126.
5. `vm_main` lays out near memory, initializes the handle table and heap, zeroes the globals, registers constants as static objects (`hstatic`), and builds the `args` array in global slot 0.
6. The VM installs an `EVT_BREAK` handler through `ONEVENT`, invokes function 0 in `MODE_DISCARD`, and enters the `run` loop. Control never returns from this loop: program termination goes through `do_exit`.

Starting an empty script takes approximately 186,000 CPU16 instructions; launching it through `bs` with a valid cache takes approximately 250,000.

### 3.1. Module placement

| Condition | Function table and code | Frame table | Constants |
| --- | --- | --- | --- |
| Functions + code + 800 bytes fit within 8 KB | Module frame in window 6 | In the same frame | Near memory |
| More than 8 KB (`cf_multi`) | Function table in near memory; code in up to 4 frames (`CF_MAX`), or 32 KB | Near memory | Near memory |
| Fewer than 6 KB of heap would remain after loading constants, and constants occupy at least 2 KB | As above | As above | Static far-memory segments; the program starts directly in extended mode |

The line-number table is not loaded into memory. Instead, the VM consults the `.bsc` file when an error location is needed (`ov_sys.b16`).

String constants in the file already use the heap-object representation (a `type, length, 0` header followed by bytes). The VM therefore uses them in place in module memory: `hstatic` allocates a handle, sets `F_STATIC`, and writes the handle number into the header. The collector neither moves nor frees static objects; they reside below `heap_lo`.

## 4. Values and objects

### 4.1. Values

```text
struct Val { v: uint; t: uint }      // word 0: value bits or reference; word 1: tag
```

| Tag | Type |
| --- | --- |
| `$8000` | `null` |
| `$8001` | `bool` (0/1) |
| `$8002` | `int` (16-bit representation) |
| `$8010` … `$8014` | String, array, dictionary, file, error; `v` holds an object reference |
| `$0000–$7FFF` | Unused; reserved for Phase 2 `ulong` |

A value occupies 4 bytes and is copied in two word operations. Determining whether a value refers to an object takes just one comparison: `t >= $8010`.

### 4.2. Heap objects

```text
struct Obj { fl: uint; size: uint; h: uint }   // 6-byte header, followed by data
```

| `fl` bits | Meaning |
| --- | --- |
| `$00FF` | Type: 3 = string, 4 = array, 5 = dictionary, 6 = file, 7 = error; 8–10 = chunks (Section 7.5) |
| `$8000` `F_MARK` | Marked by the collector |
| `$4000` `F_SCAN` | Marked but not scanned because the mark stack filled up |
| `$2000` `F_STATIC` | Module constant |
| `$1000` `F_REM` | Included in the write-barrier remembered set |
| `$0800` `F_DEAD` | Data moved to far memory; original storage is garbage |
| `$0400` `F_CHUNK` | Array or dictionary data is stored in chunks |

`size` is the data length in bytes; objects are aligned to an even address. `h` is the handle number, which allows compaction to update the handle table.

**Per-type data layout** (offsets are relative to the start of the data area):

| Type | Layout |
| --- | --- |
| String | Bytes `0…size−1` |
| Array | `0`: length; `2 + 4i`: element `i` (`Val`) |
| Dictionary | `0`: count; `2`: used; `4`: cap; `6`: nb; from `8`: 8-byte entries, followed by buckets |
| Error | `0`: code; `2`: message (reference); `4`: object (reference or 0); `6`: location code offset (`$FFFF` if unavailable) |
| File | `0`: file-table slot number (0 means closed); `2`: path (reference) |

## 5. Interpreter

### 5.1. Registers

| Variable | Meaning |
| --- | --- |
| `ip` | Address of the next opcode byte (in window 6 or the module frame) |
| `vs`, `vsp`, `vlim` | Value-stack base, top, and limit |
| `fbase` | First local slot of the current function |
| `glob`, `nglob` | Global variables and their count |
| `frames`, `nfr` | Call-frame table and frame count |
| `konst` | Constant-number-to-handle map |
| `cur_err` | Current-error register (reference or 0) |
| `bbase`, `bnargs`, `bn` | Built-in argument base, argument count, and result count |

### 5.2. Instruction dispatch loop

```text
fn run() {
    loop {
        var p = ip
        var o = uint(*p)
        ip = p + 1
        if o > OP_LAST { op_bad() }
        ops[o]()
    }
}
```

`ops` is a table of 65 function pointers, one per opcode. Unassigned opcodes dispatch to `op_bad` (`bad instruction`). Each handler reads its own operands through `rd8` or `rd16`. Frequently executed handlers (`ldl`, `stl`, `ldg`, `int8`) avoid calling `push` and perform a single stack-limit check. The average cost is roughly 59 CPU16 instructions per bytecode instruction.

**Safe points.** After a `jmp` (backward loop branch), a call, a return, or a built-in call, the interpreter checks `promote_pending`. If set, it promotes objects to far memory (Section 7.3). Between instructions, all references reside on the value stack, among globals, or in objects, so the VM can update them safely.

### 5.3. Calls and frames

```text
struct Frame { ret: uint; base: ptr Val; fnum: uint; mode: uint }   // 8 bytes; at most 100 frames
```

`call_fn(f, mode)` expects arguments to be on the value stack. The new frame base is `vsp − parameter count`. It checks the frame limit and available value-stack space (`stack overflow`), then verifies parameter annotations if the function enables checking. Remaining local slots are initialized to `null`, and `ip` is set to the function's code.

`op_ret(n)` checks result annotations, copies the `n` results over the argument slots, restores `ip` and `fbase`, and applies the call mode through `finish_call`. Returning from function 0 terminates the program; its result determines the exit status.

**Call mode** is the byte following a call instruction (bytecode format, Section 3.4): `$80` discards results; `$40 + n` demands exactly `n` values; `n` specifies the number of destinations, with a trailing `null` supplied if there is exactly one more destination than actual results (this is not an error). A method's result count is known only at runtime, so `finish_call` checks it (`wrong number of results`).

**Built-ins and methods.** `callb` selects `bfuncs[id]`; `callm` selects an entry from the receiver type's method table (`mt_str`, `mt_arr`, etc.). A missing entry produces `no method m for type`. The function reads arguments with helpers such as `arg_int` and `arg_str` (a type mismatch raises a runtime error), stores results through `res(k, t, v)`, and sets their count in `bn`. A return value of 0 indicates success; 1 indicates that `cur_err` holds an error. The temporary-root stack is cleared after the call.

### 5.4. Error handling

- **Expected error.** A built-in calls `fail_code(code, object)`, which constructs an error object containing a message from `err_texts` and the current instruction location, then stores it in `cur_err`. Monitor errors `FAULT`, `BADF`, `NOSYS`, and `AGAIN` become the runtime error `internal error`; `INTR` terminates the program with `$8001`.
- **Error capture** (`call_failed`). If the call mode requests *results + 1*, the VM pushes `null` for each expected result, followed by the error, and clears `cur_err`.
- **Unwinding** (`fail_current`). Otherwise, frames are popped until a call configured to capture errors is found; that frame receives the error as above. If unwinding reaches function 0, `top_error` prints `name: object: message` and exits with status 1.
- **`fail`.** `op_fail` reads its operands and instruction location while the module frame is still mapped, then creates the error in the `gc` overlay (`g_fail`) and unwinds the stack.
- **Runtime error** (`rt_err2`). The message is copied to a near-memory buffer (it may originally reside in an overlay image). The location and function name are saved, the module frame is restored to the window, and the `sys` overlay prints `bsvm: file:line: in function name: message` after reading line information from `.bsc`. Finally, `do_exit(125)` flushes buffers.

### 5.5. Code spanning multiple frames

For modules larger than 8 KB, code occupies multiple 8 KB frames. Window 6 maps whichever frame is executing (`cf_cur`). `code_at` converts a code offset `off` into an address: frame `off >> 13`, address `$C000 + (off & $1FFF)`; it maps the required frame through `XMAP` if necessary. Branches crossing a frame boundary also go through `code_at`. No instruction crosses such a boundary: the compiler inserts a `jmp` to the start of the next frame. Call frames retain return *offsets*, rather than addresses, so a return into another frame also uses `code_at`.

## 6. Heap

### 6.1. Handles

Neither bytecode nor values contain object addresses: a near-object reference is a 15-bit handle number. `htab[h]` holds the object header address, while `hseg[h]` indicates its location (`HS_NEAR` = near heap or static object; `HS_OWN5` = VM-owned page-5 frame; `HS_FWD` = promoted, with the far reference stored in `htab[h]`). Free entries form a linked list encoded as `(next << 1) | 1`. Handle 0 is invalid and denotes “no reference.”

The handle count is established at startup, with one entry per 22 bytes of heap. Running out of handles is treated as running out of memory.

### 6.2. Allocation

`alloc(type, size)` reserves space at the heap top (`heap_top`) and obtains a free handle. If either space or handles are exhausted, the VM tries the following, in order:

1. A normal garbage collection (`gc`).
2. If that is insufficient and extended mode is not yet active, transition to extended mode through the `gc` overlay (Section 7.2).
3. In extended mode, allocate directly in far memory (`far_alloc`) and set `promote_pending` so near objects will be promoted at the next safe point.
4. If no space is available anywhere, raise `out of memory`.

`new_str` rejects strings longer than 32,767 bytes (`too large`).

### 6.3. Object access rule

Any allocation may trigger garbage collection and move objects; in extended mode, it can also remap the data window. Therefore:

- Outside `heap.b16` and `map.b16`, object data is accessed only through heap functions: `hgw`/`hsw` (word), `hgb`/`hsb` (byte), `hget`/`hput` (transfers to/from near memory), `hmove`, `hcmp`, `hfind`, `hfill` (operations between objects), and `aget`/`aput` (array elements).
- A data address returned by `hp()` remains valid only until the next call to *any* heap function. At most one object's data address is used at a time.
- Across an allocation, a built-in retains handles, never raw addresses. A newly created object that is not yet reachable from normal roots is registered on the temporary-root stack (`tpush`, 16 entries); `op_callb` and `op_callm` clear this stack after the call.

For example, without this rule, `split` could create an array, then trigger a collection while constructing the next substring; the array, not yet reachable from the root set, could be reclaimed.

Near-memory areas such as the value stack, globals, file buffers, and local arrays in B16 functions are *not* heap objects and are never relocated.

When two far objects occupy different frames, `hmove` and `hcmp` use the 128-byte near-memory `bounce` buffer to copy or compare their contents. `hpair` indicates whether both objects are accessible simultaneously.

### 6.4. Near-heap collection

The collector uses a non-generational mark-compact algorithm in normal mode.

- **Roots:** values from `vs` to `vsp`, globals, temporary roots, `cur_err`, and the string builder's reference (`bld_h`). Extended mode also includes references held by remembered far objects. Static module constants are not marked.
- **Marking** is iterative, using a 48-reference stack (`mstack`). If the stack fills, the object receives `F_SCAN`; after the main pass, `rescan_all` repeatedly traverses the heap until no such objects remain.
- **Object scanning** (`scan`) visits array elements; dictionary keys and entry values; chunk references; error messages and objects; and file paths. Strings require no scanning.
- **Compaction** (`compact_near`) slides live objects toward lower addresses and updates `htab` using each object's header field `h`. Dead handles are released. A dead file is first finalized by `file_finalize`, which flushes its buffer and calls `CLOSE`; any error during finalization is discarded. The resulting heap has no fragmentation.

The access rules in Section 6.3 are tested with the `GC_STRESS` build, which collects before *every* allocation (`make stress`). Tests and examples must produce the same output as the normal build.

## 7. Far memory

If collection cannot free enough near-heap space, the VM enters **extended mode** and stores objects in frames of extended memory. Programs that fit in the near heap never pay this cost: in normal mode, access functions need only check the `fastpath` condition.

### 7.1. References

| Kind | Bits | Interpretation |
| --- | --- | --- |
| Near | Bit 15 = 0 | Handle number (Section 6.1) |
| Far | Bit 15 = 1 | Bits 14–8: segment (1…127); bits 7–0: index in that segment's table |

A **segment** is an 8 KB frame. Its first 512 bytes contain a 256-entry table; each entry stores either an object offset within the frame or a forwarding record (bit 0 = 1). Objects follow the table, giving far-memory objects a maximum data size of 7,674 bytes. The limits are 127 segments (approximately 1 MB) and 32,512 objects. The VM caps its own extended-memory use at these values to leave memory available for child processes.

A near reference remains unchanged while its object stays in near memory; a far reference never changes. The Section 6.3 access rule therefore still applies: a raw address is valid only until the next heap-function call, whereas an object reference remains valid. `oaddr(r)` maps the required frame into the page-5 data window and returns the object header address.

### 7.2. Entering extended mode

`far_activate` (in the `gc` overlay) retains objects above `$A000` in place as the “VM-owned page-5 frame” segment (`HS_OWN5`, visible with `XM_OWN`). An object straddling the boundary is copied into a far segment through `bounce`. The near heap is then capped at `$A000`, and page 5 becomes the far-data window. References do not change during this transition. Extended mode leaves roughly 3.6 KB of near heap.

### 7.3. Generations and promotion

- New objects normally go into the near heap. If insufficient space is available while executing a built-in, an object is allocated directly in far memory.
- **Promotion** (`promote`) copies all live near objects into segments and replaces references to them with far references—in roots, remembered far objects, and the promoted objects themselves. Promoted objects are scanned in allocation order, as in Cheney's algorithm. Objects too large for a segment stay near, and their references are updated separately. Handles belonging to promoted objects are then freed, followed by an ordinary collection of the remaining near heap.
- Promotion changes references, so it runs only at the safe points described in Section 5.2.
- **Minor collection** examines only the near heap. The **write barrier** (`wbar`, `hset_ref`) tracks far objects that receive near-object references (array elements, dictionary entries, error fields, and file fields) in a 128-object remembered list. Those references are treated as roots. If the list overflows (`rem_all`), all far objects are scanned.
- **Major collection** (`major_gc`) marks all objects, releases dead segment-table entries, closes unreachable files, and compacts frames. It runs before promotion if far memory has insufficient free space or table entries.

### 7.4. Cost

Remapping the window takes approximately 50 CPU16 instructions with the B16 wrapper (about 38 in Monitor, thanks to the fast `XMAP` path). A loop that alternately accesses an array chunk and a string in another frame runs about 1.6 times slower than normal mode: traversing an array of strings takes 1,960 instructions per element instead of 1,210.

### 7.5. Arrays and dictionaries

An **array** stores its length and 4-byte elements. Data up to 4 KB resides in the object itself; larger arrays use 4 KB chunks (`F_CHUNK`, with `TY_VCH` chunks containing 1,024 values). The fixed header is followed by the chunk count and chunk references. Each chunk is a separate object and may reside in a different frame. Growing the array adds a chunk without copying its existing data. `delem` locates an element by index; `dobj` identifies the containing object for the write barrier. The array limit is 32,767 elements.

A **dictionary** is an insertion-ordered hash table (`map.b16`):

- Fixed fields: `count` (live entries), `used` (occupied entries, including deleted ones), `cap` (entry capacity), and `nb` (number of buckets, a power of two and at least `2 · cap`).
- Each 8-byte entry stores a key reference, hash, and value (`Val`). Entries retain insertion order, so `keys()` and `values()` return that order without sorting.
- Buckets contain entry numbers plus one (zero denotes an empty bucket); collision resolution uses open addressing with linear probing.
- String hashes use djb2: `x = x · 33 + byte`, starting from 5381.
- Deletion sets the entry's key to zero. Its bucket remains in the table and is skipped during lookup. When the table fills, `map_rebuild` reconstructs it from live entries in their original order.
- In normal mode, `map_find` accesses a non-chunked dictionary directly without heap access functions.
- Above 4 KB, entries and buckets are stored in chunks (`TY_ECH` for 512 entries, and `TY_BCH` for buckets). The dictionary limit is 16,384 keys.

## 8. Input and output

The VM keeps an 11-entry `slots` table in fixed, non-heap memory: entry 0 represents stream 0; entries 1 and 2 represent streams 1 and 2; entries 3–10 are script-owned files. This limits a program to eight open files. Each entry stores a descriptor, mode, 192-byte buffer, read position, end-of-file indicator, console indicator, and deferred write error.

- **Writes** are buffered. A buffer is flushed when full, on `\n` to a console stream, on `flush`, before reading stream 0, before launching a program, and at exit. A `WRITE` error is saved and reported by the next `write`, `flush`, or `close`.
- A **`file` object** contains a slot number and path. Buffers live outside the heap, so the collector never moves them and the Ctrl+C handler can flush them.
- **Descriptor exhaustion.** If Monitor returns `MFILE`, `open_gc` performs a collection (closing unreachable files) and retries `OPEN`.
- The **string builder** (`bld_start`, `bld_put`, `bld_done`) assembles results of unknown length, such as file contents. Its string buffer grows by doubling up to 32,767 bytes and is registered as a GC root (`bld_h`).
- **Ctrl+C.** The `on_break` handler calls `flush_all` and exits with `$8001`. It neither allocates memory nor executes bytecode. `slot_flush` clears the buffer length *before* calling `WRITE`, preventing an interrupt handler from writing the same contents twice.

## 9. Overlays

An entirely resident VM left too little memory for the heap. The first version occupied 38 KB including BSS and left approximately 12.5 KB for the heap, against a target of 16–24 KB. Infrequently used and long-running library code was therefore moved into overlay images, mapped at the window-6 address and stored in `bsvm.ovl`.

| Overlay | Contents | Image size |
| --- | --- | --- |
| `fs` | `fs` and `path` namespaces | 5.5 KB |
| `sys` | `proc`, `time`, `str.hex`; `replace`, `equal_fold`, `repeat`, `reverse`, `sort`, `keys`, `values`, and dictionary `clone` methods; runtime error reporting; `e.where()` | 6.6 KB |
| `gc` | Entry into extended mode, promotion, major GC, far-memory allocation, chunks, `fail`, `error()` | 5.6 KB |
| `init` | Module loader; linked for page 5 and executed once | 3.3 KB |

**Residency boundary.** Frequently called, short operations remain resident: common string, array, dictionary, file, and error methods, I/O, and global built-ins. Two window switches would otherwise cost about as much as the operation itself. Overlays contain operations that invoke Monitor (thousands of instructions per call) or are rarely used.

**Calling overlays.** The `bfuncs` and `mt_*` tables point to stubs generated by `mkovl.py`, of the form `fn name(): uint { return ov_call(OV_x, i) }`. `ov_call` saves the current window mapping, maps the target overlay frame (allocating it and loading the image from `bsvm.ovl` on first use), invokes entry `i` through the image's `ov_table`, and restores the previous mapping: a module frame, current code frame, or another overlay. The overhead is roughly 100 instructions beyond the function's own work (two `XMAP` operations); before Monitor's fast `XMAP` path it was around 700. In the examples, switching accounts for less than 0.5% of execution time.

**Nested calls.** An overlay may invoke another overlay—for example, a garbage collection during allocation in `fs.read_text`. On return, the caller's overlay is mapped again. Accordingly, functions used this way cannot accept pointers into the caller overlay's data or read module tables directly from the window. The resident code reads operands and error locations *before* making the overlay call. Overlays access objects only through heap functions.

**Failures.** Missing `bsvm.ovl`: `bsvm: path: cannot open`; malformed file: `bad overlay file`; no available frame: `not enough memory`. All these cases exit with status 126.

**`bsvm.ovl` format.** Header: `"BS"`, image count, and one `(offset, length)` pair for each image. Each image contains the word `"OV"`, the address of the `ov_table` entry table, and the code and data linked for the window address.

**Build process** (`tools/mkovl.py`):

1. `stubs` generates the overlay entry stubs for the resident image.
2. `keep` lists resident functions called exclusively from overlays, ensuring they are retained in the resident image.
3. The resident image is linked and produces `bsvm.map`.
4. `env` generates resident declarations for the overlays: functions become `extern fn`, and globals become `var NAME at ADDRESS`.
5. `overlay` builds each overlay for the window address (`init` uses `$A000`), resolving resident symbols from the map.
6. `pack` writes `bsvm.ovl` in `OV_*` number order.

## 10. Building, testing, and profiling

| Command | Action |
| --- | --- |
| `make` | Builds `out/bsc.bin`, `out/bsvm.bin`, `out/bsvm.ovl`, and `out/bs.bin` |
| `make test` | Runs `tests/*.bs` under `kmon`; compares output and exit status with `tests/*.out` |
| `make examples` | Runs `examples/*.bs` against `examples/expected/*.out` |
| `make stress` | `tools/stress.sh gc`: collect before every allocation (`GC_STRESS`); `tools/stress.sh far`: immediately enter extended mode with a 2 KB near heap (`FAR_TEST`) |
| `tools/profile.py MAP command` | Counts CPU16 instructions by function using `kmon --profile` |
| `tools/vmrun.py` | Runs shell commands on a temporary volume containing the complete toolchain |

Far-memory stress cases (`farbig`, `farchurn`, `bigmod`, `bigconst`) each run in a separate test-harness invocation.

## 11. Limits and where they are defined

| Limit | Value | Constant / source |
| --- | --- | --- |
| Value stack | 384 values | `VSMAX`, `bsvm.b16` |
| Call depth | 100 frames | `FRMAX`, `types.b16` |
| Temporary roots | 16 | `TRMAX`, `types.b16` |
| Mark stack | 48 | `MSMAX`, `heap.b16` |
| Write-barrier remembered set | 128 objects | `REMMAX`, `heap.b16` |
| Script-owned open files; buffer | 8; 192 bytes | `NSLOT`, `SBUF`, `types.b16` |
| Module code frames | 4 (32 KB) | `CF_MAX`, `ovl.b16` |
| Far-memory segments | 127 (1 MB); 256 objects each | `SEG_MAX`, `heap.b16` |
| Far-memory object | 7,674 data bytes | `FAR_MAXOBJ`, `heap.b16` |
| Array or dictionary chunk | 4 KB | `CHUNK`, `heap.b16` |
| String; array; dictionary | 32,767 bytes; 32,767 elements; 16,384 keys | `new_str`, `MAP_MAXCAP` |
| B16 stack | 1.5 KB | `CSTACK`, `bsvm.b16` |
| Minimum near heap at load time | 6 KB (otherwise constants are placed in frames) | `NEAR_MIN`, `ov_init.b16` |

## 12. Measurements

CPU16 instruction counts measured using the `kmon` test harness.

| Measurement | Result |
| --- | --- |
| `bsvm` resident image | 27.6 KB; 32.3 KB including BSS; 21.1 KB of overlays |
| Near heap for a small script | Approximately 11.8 KB; approximately 3.6 KB in extended mode |
| Starting an empty script in `bsvm` | 186,000 instructions |
| Launching through `bs` with a valid cache | Approximately 250,000 instructions |
| Arithmetic loop containing 17 bytecode instructions | Approximately 1,010 instructions per iteration; 59 per bytecode instruction |
| Script-function call with two arguments | Approximately 1,530 instructions per loop iteration including the call |
| Overlay-function call | Approximately 100 instructions beyond the function's own work |
| Splitting a string into 8 parts | Approximately 9,900 instructions |
| 400 strings of the form `$"item {|i|}"`, followed by `str.join` | 1.6 million instructions |
| 3,000 iterations using an array, string, and dictionary (including collections) | 12.3 million instructions; working set retained |
| Filling an array with strings and traversing it, per element | 4,300 instructions at 500 strings; around 16,000 at 8,000 strings (extended mode) |
| Traversing an array of strings with `a[j].len()`, per element | 1,210 at 500 strings; 1,960 at 8,000 |
| `XMAP` window switch | Approximately 50 instructions via B16 wrapper; about 38 in Monitor (previously about 336) |
| Large module `tests/bigmod.bs` (1,100 lines, 20 KB `.bsc`) | Executes from multiple code frames |
| Module with 7 KB of constants, `tests/bigconst.bs` | Constants in frames; extended mode from startup |
| Array containing 30,000 strings | Fits in far memory |

## 13. Design decisions

| Decision | Rationale | Rejected alternative |
| --- | --- | --- |
| 4-byte values with the tag in the high word | Copying needs two word operations; Phase 2 `ulong` fits without changing the representation | Variable-length values |
| Handles instead of raw addresses | Objects can move: mark-compact avoids fragmentation, and far objects are transparent to bytecode | Raw addresses with a nonmoving heap |
| Mark-compact with an explicit marking stack | Recursive marking in B16 would consume the 1.5 KB call stack; rescanning after rare mark-stack overflow is cheaper | Recursive marking |
| File buffers outside the heap | The Ctrl+C handler can flush them without allocating memory or accessing the heap | Heap-allocated buffer objects |
| Rarely used code in overlays | Increases the available near heap beyond roughly 12.5 KB and preserves space for far-memory support | Entirely resident VM; a “sticky” module-code overlay in near memory would consume the module's size to save about 3% |
| Generational far memory with a write barrier | Raises the data limit to 1 MB rather than a 48 KB process; normal mode has little overhead | Keeping the entire heap in frames from startup |
| Fast `XMAP` path in Monitor | Extended mode is 1.6× rather than 3–5× slower; overlay calls cost 100 instead of 700 instructions | A second data window at the expense of a near-memory page |
| Loading line-number information on demand | Requires no persistent memory; needed only for diagnostic messages | Loading the entire line-number table |
| Unstable quicksort | Avoids a second array copy, because heap memory is the primary constraint | Merge sort |
