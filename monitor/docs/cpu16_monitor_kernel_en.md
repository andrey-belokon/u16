# CPU16 Monitor: Kernel Internals. Revision 6

This document specifies the internal design of the CPU16 Monitor kernel: memory allocation, kernel entry and return, interrupts, processes, the console, disks and file systems, module organization, and building the system. It is an implementation specification. The external interface visible to applications is defined in *System Calls*; this document describes its implementation rather than restating it.

Sections 1–8 are normative for kernel implementations: **must** and **must not** express requirements. Sections 9–11 cover validation, resource budgets, and deferred decisions.

The design builds on two prototypes: A (the FS16 module written in B16) and B (the resident component, memory-map switching, and the process chain). Their code serves as the implementation baseline. Any required departures from the prototypes are stated explicitly.

## 1. Overview

### 1.1. Related documents

| Document | Relevant material |
| --- | --- |
| *16-bit Processor Architecture*, revision 6 | registers, `TRAP`, `IRET`, interrupts, ERRCMD |
| *CPU16 Machine: Memory and Devices*, revision 9 | memory manager, timer, keyboard (including MODS), display (including SCRD screen reads), clock, disk controller, `WFI` wait instruction |
| *Assembler Reference* | `.org`, `.reserve`, `.include`, memory map |
| *B16 Language Specification*, revision 3 | `option target = kernel`, `option code`, `option data`, `include asm`, `export`, `extern`, `at`, `.equ` definitions for structures, calling convention |
| *FS16 File System*, revision 3 | volume layout, operations, write ordering, name comparison |
| *CPU16 Monitor: Executable File Format and Program Startup*, revision 4 | program loading, PIB, startup state |
| *CPU16 Monitor: System Calls*, revision 6 | the interface the kernel must implement |

The last three documents are referred to below as *FS16*, *CX16 Format*, and *System Calls*.

### 1.2. Design principles

1. **Non-preemptive process chain** (*System Calls*, §2.1). Only the last process in the chain executes. The kernel never switches processes on its own: the current process changes only during `SPAWN` and process termination.
2. **The kernel is non-preemptible and runs with IF = 0.** Kernel entry clears IF; interrupts are enabled only at designated wait points (§3.6). System calls are therefore atomic, and no locks are needed within the kernel.
3. **One kernel memory map and one current-process memory map** (§2). The kernel accesses process memory through mapping windows.
4. **One entry frame and one kernel stack.** The kernel is not reentrant: its stack is empty between calls, and the saved context of a parent waiting for a child resides in that parent's process descriptor (§4.3).
5. **Synchronous disk operations.** The kernel starts a controller command and waits for IRQ1 without changing the memory map (§6.1).
6. **Interrupt handlers do not change the memory map.** Their code and data reside in page 7 and therefore operate identically under either map.
7. **Resident code is written in assembly; the rest of the kernel is written in B16.** The image contains a single B16 module (B16, §10.4).

## 2. Memory

### 2.1. Physical frames

| Frames | Purpose |
| --- | --- |
| 0–6 | visible as pages 0–6 with the memory manager disabled; used only during startup (§8.3), then left unused |
| 7 | **shared frame C**: page 7 in all memory maps. With the memory manager disabled it already occupies page 7, so enabling the manager does not relocate the resident component |
| 8–12 | kernel frames K0–K4: pages 0–4 in the kernel map |
| 13 | sink frame: process pages above `top` and idle kernel windows |
| 14–255 | frame pool: process memory (242 frames, 1,936 KiB) |

Frames 0–6 are not returned to the pool: on physical machines frames 0–3 may be ROM (*Machine*, §7.6), and reclaiming the other frames is not worth the added complexity.

Frames are tracked by a one-byte-per-frame ownership table `owner[256]` and the free-frame counter `nfree`:

| `owner[f]` | Meaning |
| --- | --- |
| 0 | free |
| `$FF` | kernel and frames 0–13 |
| `$FE` | a new program during `EXEC` (§4.5) |
| 1–16 | process: descriptor number + 1; covers both its private frames and extended-memory frames |

Allocation selects the first free frame. On process termination, all frames owned by that process are released, so a separate per-process list of extended-memory frames is unnecessary.

### 2.2. Kernel memory map

| Page | Addresses | Frame | Contents |
| --- | --- | --- | --- |
| 0–3 | `$0000–$7FFF` | 8–11 | kernel code and initial-data templates |
| 4 | `$8000–$9FFF` | 12 | kernel data and BSS: tables, buffers (§2.5) |
| 5 | `$A000–$BFFF` | window W0 | process memory; sink frame while idle |
| 6 | `$C000–$DFFF` | window W1 | same |
| 7 | `$E000–$FFFF` | 7 | shared page (§2.4) |

The kernel maps process frames into windows W0 and W1 only for the duration of a process-memory access (§2.6). Every access explicitly configures the windows; their contents between accesses are unspecified.

### 2.3. Process memory map

Each process has a **page map** identifying the physical frame assigned to each logical page, 0–6. Without extended memory, the mapping is:

| Page | Frame |
| --- | --- |
| 0 … top/$2000 − 1 | the process's private frames (`Proc.fr`) |
| top/$2000 … 6 | sink frame 13 |
| 7 | 7 |

`XMAP` maps an extended-memory frame into a page; `SETTOP` and `XFREE` restore private frames or the sink frame.

**Map storage.** The currently running process's map resides in the eight-byte `RES.cmap` array on page 7 (§2.4); the eighth entry always names frame 7. This is the **authoritative map** used by `XMAP`, `XFREE`, `SETTOP`, memory validation, and the process-memory windows (§2.6). The kernel does not maintain a separate `Proc.map` copy for that process. Instead, `Proc.map` stores the map of a process that is not running, such as a parent waiting for its child or a program being loaded. The map is copied between these locations when the current process changes:
- On entering a process (`proc_enter`: child at `SPAWN`, parent at `EXIT`, `EXEC`, or shell startup), `cmap_set` copies `Proc.map` into `RES.cmap`.
- At `SPAWN`, before entering the child, `cmap_save` copies `RES.cmap` into the parent's `Proc.map`.

Thus the `XMAP` fast path (§3.2a) changes only `RES.cmap` and the PAGE register, while full kernel entry copies no memory map. The function `pmap(p)` returns the map for process `p`: `RES.cmap` if it is current, otherwise `Proc.map`.

The following current-process data is stored alongside the map in `RES` so the fast path can validate requests without accessing kernel data (page 4 is not visible under the process map):
- `RES.omap[8]` — the process's own-frame map: `Proc.fr[i]` for pages below `top`, and the sink frame for pages above it. `XMAP` with `XM_OWN` restores the requested page from this array; it does not replace the entire page map. If `RES.cmap[i]` equals `RES.omap[i]`, that page is not mapped to an extended-memory frame. Updated on process entry and by `SETTOP`.
- `RES.xbits[32]` — bitmap of extended-memory frames belonging to the current process (bit `f & 7` of byte `f >> 3`): `owner[f]` equals the process number, and the frame is not private. `XALLOC` sets a bit; `XFREE` clears it. On process entry, the bitmap is rebuilt by scanning `owner[]` only if the process owns extended-memory frames (`Proc.nx` ≠ 0); otherwise it is zeroed.

The resident component installs the process map from `RES.cmap` on return from the kernel.

### 2.4. Shared page 7

| Addresses | Size | Contents |
| --- | --- | --- |
| `$E000–$E003` | 4 | exit stub: `TRAP #9` / `.word SYS_EXIT` (*CX16 Format*, §8.1) |
| `$E004–$E007` | 4 | `PREEMPT_PROC` session-switch stub: `TRAP #9` / `.word SYS_PREEMPT` (§3.8) |
| `$E008–$E3FF` | up to 1016 | resident code: vector stubs, entry and return, dispatcher, interrupt handlers |
| `$E400–$E7FF` | 1024 | `RES` — data shared across both maps (§3.5): handler register saves, tick counter, flags (`abort`, `nobreak`, `brk_echo`, `dk_done`, `kf_set`, `kf_raw`), session-switch request `sw_req` and `sw_ip` (§3.8), tick handler `hook`, `fault_ip`, keyboard event ring buffer with modifiers, current-process map `RES.cmap`, private map `RES.omap`, and extended-memory frame bitmap `RES.xbits` (§2.3) |
| `$E800–$EDDF` | 1504 | kernel stack, growing downward from `$EDE0` |
| `$EDE0–$EDFF` | 32 | `KF` — kernel entry frame (§3.1) |
| `$EE00–$FDFF` | 4096 | eight 512-byte sector buffers (§6.2) |
| `$FE00–$FEFF` | 256 | reserved; unused |
| `$FF00–$FF1F` | 32 | vectors |
| `$FF20–$FFFF` | | devices |

Page-7 data is declared in B16 using `at` variables (`RES`, `KF`, cache buffers). Assembly refers to it through compiler-generated `.equ` definitions (B16, §10.3). The master build file reserves these addresses with `.reserve` (§8.2), so resident code that grows into the data area causes an assembly error.

### 2.5. Kernel data

Kernel data and BSS reside on page 4: `kernel.b16` declares `option data = $8000` (§8.2). Approximate usage:

| Table | Size |
| --- | --- |
| process descriptors, 16 × approximately 160 bytes (§4.1) | ≈ 2.6 KiB |
| open nodes, 32 (§6.5) | ≈ 1.2 KiB |
| open files, 32 (§6.5) | ≈ 0.3 KiB |
| volumes, 8 (§6.3) | ≈ 0.3 KiB |
| cache buffer descriptors, 8 | ≈ 0.1 KiB |
| command-line and path buffers, name buffer | ≈ 0.6 KiB |
| console line, 256 bytes | 0.25 KiB |
| `owner` frame ownership table, miscellaneous | ≈ 0.3 KiB |
| sessions, 4 × 4 bytes (§4.7) | 16 bytes |
| **total** | **≈ 5.5 KiB of 8 KiB** (revision-5 implementation: 7.7 KiB, because process descriptors exceed the estimate) |

Screens, console lines, and command lines of processes belonging to inactive sessions do not fit into kernel data: they reside in the session frame (§4.7).

Exceeding 8 KiB is a build error (§8.2).

### 2.6. Accessing process memory

The kernel accesses the current process's memory exclusively through mapping windows. The fundamental operation is **mapping a span**:

```text
fn uwin(ua: uint, n: uint): (uint, ptr, uint)    // error code, address in window, span length
```

Process memory consists of pages whose entries in the process map (`pmap`, §2.3) do not refer to the sink frame. `uchk` verifies that every page within `[ua, ua + n)` meets this condition (otherwise `FAULT`). `pwin` maps the frame for page `ua >> 13` into W0 and the frame for the next page into W1, then returns address `$A000 + (ua & $1FFF)` and the length of the span visible in its entirety. That length is the smaller of `n` and `$4000 − (ua & $1FFF)`; it is therefore at least 8 KiB + 1 byte when `n` is larger than that minimum.

Rules:
- **Call buffers** (`READ`, `WRITE`, `FSTAT`, etc.) are processed in spans: `uwin` → operate on the span → proceed to the next span. A span does not exceed 8 KiB, so any sector or cluster contained within it is contiguous in the window.
- **Direct DMA into process memory** is permitted: FS16 reads and writes entire sectors directly at the address in the window (§6.4). This is safe because the kernel does not change the windows while a command is active (§6.1).
- **Strings** (paths and command lines) are copied into a kernel buffer using `ustr(ua, max)`, one byte at a time through a window until a NUL byte is found. No NUL within `max` bytes yields `BADPATH` or `ARGS` (depending on the call); an access above `top` yields `FAULT`.
- **Result structures** (`Stat`, `VolInfo`, `SysInfo`) are populated inside the kernel and copied into the process in one `ucopyout` operation.
- Large-block copying and zeroing (frames during loading, buffer spans) are performed by assembly functions `kcopy` and `kzero` from `klib.asm` (`extern fn`). A B16 loop is noticeably slower at these locations (§10).

## 3. Kernel entry and return

### 3.1. Entry frame

`KF` consists of 16 words at address `$EDE0`, at the top of the kernel stack:

| Word | Contents |
| --- | --- |
| 0–11 | process registers R0–R11 |
| 12 | LR (R12) |
| 13 | process SP before entry (excluding the four bytes pushed by the processor) |
| 14 | return IP: for a system call, the address after the call-number word |
| 15 | process FLAGS |

There is only one entry frame: the kernel cannot enter itself recursively. Call handlers read arguments from registers (§3.3), but may also inspect and modify `KF`; for example, `SPAWN` and `EXIT` replace it entirely (§§4.3–4.4).

### 3.2. TRAP #9

The `trap9` stub on page 7 performs these steps in order:

0. Save R2 in `RES.sv0` and fetch the call number using the return IP saved on the process stack. For `XMAP`, branch to the fast path (§3.2a); for other calls, restore R2 and proceed to `trap9_full`. The dispatch check adds six instructions to each call.
1. Save the process SP in `RES.psp`, then move SP to the top of the register area in `KF` and save R0–R12 there. No additional space is taken from the process stack: only four bytes below its SP are guaranteed to be available (*CX16 Format*, §8.3).
2. Through `RES.psp`, pop FLAGS and IP from the process stack, read the call number at IP, and record the pre-entry SP, FLAGS, and IP after the call-number word in the frame.
3. Install the kernel map (PAGE0–PAGE7: frames 8–12, sink, sink, 7).
4. Transfer control to the dispatcher (§3.3).

### 3.2a. XMAP fast path

`xmap_fast` performs a valid `XMAP` call entirely on page 7, under the process map and with IF = 0, without saving registers in `KF` or installing the kernel map:

1. Check that page ≤ 6 and that the requested frame is either `XM_OWN` or a process-owned extended-memory frame (`f` < 256, with its bit set in `RES.xbits`). A failed check branches to `trap9_full`, where `sys_xmap` returns `INVAL`.
2. Select `f` as the new frame, or `RES.omap[page]` when `f` is `XM_OWN`. Report the previous mapping from `RES.cmap[page]` in R1; report `XM_OWN` instead if that mapping matches `RES.omap[page]`.
3. Update `RES.cmap[page]` and that page's PAGE register.
4. Read the saved FLAGS and IP from the process stack **before** switching maps. **After** the page-register update, write the return frame through the *new mapping*, as `resume` does (§3.4): set ZF = 1 because R0 will be `OK`, preserve the other FLAGS bits, and advance IP past the call-number word.
5. Return R0 = `OK` and R1 = the previous-frame result, then execute `IRET`. That instruction synchronizes instruction fetching after the mapping change.

The results and resumed process state are the same as on the full path. As with other system calls, R2 and R3 have unspecified values on return. The fast path bypasses `pending_break` (§3.4), performs no wait, and runs with interrupts disabled. Ctrl+C pressed during this interval is handled at the first timer tick after `IRET` (§3.5.1, item 4), either by the process handler or by process termination, as on normal kernel exit. The fast path takes approximately 38 instructions instead of approximately 330.

### 3.3. Dispatcher and handlers

The `sys_table` handler table is an array of code labels in the kernel (`klib.asm`, pages 0–3; the dispatcher accesses it after installing the kernel map). It has 128 entries, with entry *i* pointing to the handler for call number *i*. Unassigned and reserved call numbers point to `sys_nosys`, which returns `NOSYS`. A number beyond the table also produces `NOSYS`.

The dispatcher:
1. Sets SP = `KF` (the kernel stack below the entry frame).
2. Loads R0–R3 from the frame.
3. Invokes the handler with `CALL`.
4. If `RES.kf_set` = 0, writes R0–R3 back into the frame; otherwise only clears `RES.kf_set`.
5. Calls `kexit()` (§3.4, item 1; §3.8) and branches to the return path.

A **handler** is an `export` B16 function. Its prototype matches the call prototype in *System Calls*, §10.3, without the user-wrapper prefix: arguments follow the R0–R3 slot convention, and the first return value is an error code. The dispatcher copies all four registers R0–R3 into the frame regardless of the number of results: *System Calls* does not define the unused values in R1–R3.

**Replacing the frame.** A handler that has filled the entire frame itself (successful `SPAWN`, `EXIT`; §§4.3–4.4), or a call that must be restarted (§3.8), sets `RES.kf_set` = 1. The dispatcher then does not overwrite the frame with the R0–R3 values left by the handler: it already contains the state of the process being resumed. `SHUTDOWN` does not return.

### 3.4. Returning to a process

Returning to a process consists of the kernel-exit check in `kexit()` followed by the page-7 `resume` routine:

1. **Check for interruption before `resume`.** `kexit()` calls `pending_break()`: if `RES.abort` = 1 and the current process does not have `NOBREAK`, clear `RES.abort` (§5.1), then:
   - If the process has a Ctrl+C handler (`Proc.brkh`) and there are eight bytes of its memory below process SP, display `^C`, flush input, push the call-return frame (FLAGS with ZF determined by frame R0, and IP) onto the process stack, decrement the frame SP by four, and set frame IP to the handler and IF = 0. `resume` enters the handler; the handler's `IRET` returns to the instruction after the system call with the original result.
   - Otherwise replace the frame with entry into the exit stub: IP = `$E000`, R0 = `$8001`.

   The B16 kernel function `pending_break()` performs this check. The dispatcher calls `kexit()`, which runs `pending_break()` and then switches sessions if requested (§3.8). For an exact frame (§3.8), the Ctrl+C handler's return frame receives the existing FLAGS unchanged.
2. Install the process map from `RES.cmap`.
3. Set the ZF bit in the saved FLAGS to 1 when frame R0 = 0, or to 0 otherwise. Leave all other flags unchanged, including the process IF. **Exception:** when `RES.kf_raw` = 1, the frame holds the exact state interrupted by a tick for session switching (§3.8). Preserve the entire FLAGS word, including ZF, and clear `RES.kf_raw`.
4. Write FLAGS and IP from the frame to `SP − 4` and `SP − 2` on the process stack, where SP is word 13 of the frame. The kernel may have changed IP, FLAGS, and SP itself (new process, exit stub), so these values must be rewritten rather than reused from the stack.
5. Restore R1–R12 from the frame, set SP = process SP − 4, restore R0, and execute `IRET`.

`IRET` pops IP and the complete FLAGS word, including the process IF. It is also an instruction-fetch synchronization point after a mapping change or after DMA has written executable code (processor specification, §11).

The kernel also starts new processes through `resume`: the loader fills the frame according to *CX16 Format*, §8.1 (IP = `$0000`, FLAGS = `$08`, SP = `top`, R0 = argc, R1 = argv, LR = `$E000`, all other registers zero), then `resume` transfers control as required by that format (§9.1).

### 3.5. Interrupts

**Identifying the interrupted context.** After interrupt entry, the handler distinguishes kernel code from process code using SP: SP ≥ `$E000` identifies the kernel (whose stack resides on page 7); a lower SP identifies a process. A process stack is always below `top` ≤ `$E000`.

Interrupt handlers preserve any registers they modify in `RES` (`RES.sv0`, `RES.sv1`), rather than pushing them onto the process stack: only four bytes of that stack are guaranteed to be available.

| Vector | Handler | Action |
| --- | --- | --- |
| 0, IRQ0 | `irq0` | §3.5.1 |
| 1, IRQ1 | `irq1` | acknowledge the disk controller; `RES.dk_done` = 1 |
| 2–7 | `irq_ignore` | `IRET` |
| 8, ERRCMD | `err8` | §3.5.2 |
| 9, TRAP #9 | `trap9` | system call |
| 10–15 | `trap_bad` | terminate process with status `$8009` and a message; panic if in kernel |

#### 3.5.1. Timer tick

The `irq0` handler:

1. Increments the 32-bit counter `RES.ticks`. Only `irq0` writes it, and the kernel reads it with IF = 0, so no additional atomicity mechanism is needed.
2. **Collects keyboard events:** drains the hardware queue by reading words from `$FF20` until a read returns zero.
   - Ctrl+] (character `$1D`, or scan code `$1B` with Ctrl in MODS) and Ctrl+[ (scan code `$1A` with Ctrl) are reserved for session switching: neither enters the event ring, regardless of console mode. The handler adds +1 or −1, respectively, to `RES.sw_req` (§3.8).
   - An event with character `$03`, when `RES.nobreak` = 0, does not enter the ring; the handler sets `RES.abort` = 1.
   - All other events are placed in `RES.kb`, which holds up to 256 events. Immediately after dequeuing an event, `irq0` reads MODS (*Machine*, §2.6) and stores the modifier byte in the corresponding slot of `RES.kbm`. If the ring is full, the new event is dropped (*System Calls*, §6.1).
3. **If the tick interrupted the kernel:** return with `IRET` without invoking `TICKHOOK` (*System Calls*, §7.4). Any pending Ctrl+C is handled at a kernel wait point (§3.6) or on exit from the kernel.
4. **If the tick interrupted a process:**
   - If `RES.abort` = 1 and `RES.brk_hook` ≠ 0 (the process's Ctrl+C handler), clear `RES.abort`, display `^C`, discard the ring contents, set `RES.ln_drop` = 1 (the kernel discards its console input line on the next read), restore registers, and branch to `RES.brk_hook`. The processor frame on the process stack points to the interrupted instruction.
   - If `RES.abort` = 1 without a handler, terminate the process: clear `RES.abort`, set `RES.brk_echo` = 1, replace the IP in the processor frame on the process stack (`[SP + 2]`) with the exit-stub address `$E000`, set R0 = `$8001`, and execute `IRET`. The process immediately enters `EXIT`.
   - Otherwise, if `RES.sw_req` ≠ 0 and the IP in the processor frame is not `PREEMPT_PROC`, save the IP in `RES.sw_ip`, replace it with `PREEMPT_PROC`, and execute `IRET` (§3.8).
   - Otherwise, if `RES.hook` ≠ 0, restore the saved registers and jump to `RES.hook`. The process tick handler receives the interrupt frame on the process stack and must execute `IRET` itself.
   - Otherwise, execute `IRET`.

#### 3.5.2. Illegal instruction

The `err8` handler handles processor exception `ERRCMD` (vector 8):
- **In a process with a handler** (`RES.bad_hook` ≠ 0): restore R0 and transfer control to `RES.bad_hook`. The saved IP in the processor frame points to the first word of the invalid instruction.
- **In a process without a handler:** save the faulting instruction address in `RES.fault_ip` and, as for Ctrl+C, replace IP with `$E000` and set R0 = `$8008`. `EXIT` displays the message specified in *System Calls*, §7.3, using status `$8008` and `RES.fault_ip`.
- **In the kernel:** call `kernel_fault(ip)`, display `monitor: kernel fault at $XXXX`, and halt (§9.2).

### 3.6. Waiting in the kernel

For disk completion, keyboard input, and timer ticks, the kernel follows the IF = 0 waiting pattern described in *Machine*, §6.5:

```text
while !condition {
    if interruptible && RES.abort != 0 && !nobreak { return E_INTR }
    if interruptible && RES.sw_req != 0 { return wait_restart() }  // §3.8
    wfi()               // with IF = 0, wakes on a request without entering a handler
    ei()                // handler accepts the request
    di()
}
```

The loop leaves IF = 0. When IRQ0 is accepted during `ei()`, its handler takes the kernel-interrupted return path described in §3.5.1, item 3.

A disk wait cannot be interrupted: an issued command must run to completion. Console waits and `SLEEP` can be interrupted (*System Calls*, §3.4). When such a call returns `INTR`, `pending_break()` (§3.4) either terminates the process or invokes its Ctrl+C handler; if the handler returns, the process receives `INTR`.

### 3.7. Rules for kernel code

- The kernel does not execute `TRAP` and does not enable interrupts outside the loops defined in §3.6.
- The kernel does not modify PAGE0–PAGE4 or PAGE7. Only `uwin` and loader functions change PAGE5 and PAGE6 (the windows).
- The kernel does not change windows while a disk command is in progress.
- Kernel stack usage must remain within 1,504 bytes (§10). The kernel uses no recursion.

### 3.8. Switching between sessions

Sessions (*System Calls*, §2.4) are not multitasking: only the top process in the current session runs. Switching sessions changes the current process, as `SPAWN` and `EXIT` do, but switches between process chains. It happens **only with an empty kernel stack**, in `kexit()` during return from the kernel. This preserves the single `KF` frame and single kernel stack (principle 4).

**Switch requests.** `sw_to` (kernel data) holds an explicit destination set by `SESS_SWITCH` or `SESS_NEW`. Keyboard Ctrl+] and Ctrl+[ presses accumulate signed steps (+1 and −1) in `RES.sw_req`. After `pending_break()`, `kexit()` gives priority to `sw_to`; if no explicit destination is set, it advances by `sw_req` steps through the occupied sessions, wrapping around. Both requests are then cleared. If the selected session differs from the current one, the switch proceeds as follows:
1. `sess_leave`: save `KF` to the current process's `ctx` along with its exact-frame indicator (`Proc.raw`); save the memory map in `Proc.map` (`cmap_save`); set state `PS_SUSP`; save the screen, CTRL, ATTR, cursor position, and console line into the session frame (§4.7).
2. `sess_enter`: restore the destination session's screen and saved console input state from its frame; discard pending keyboard events so they cannot leak into the destination session; clear `RES.abort`; and call `proc_enter` for that session's top process.

A request reaches `kexit()` through one of three paths:
- **A nonblocking call:** switch immediately after the call. Its results are already in the frame, and the process receives them when it resumes.
- **Waiting inside a call** (§3.6): the loop observes `RES.sw_req` ≠ 0 and calls `wait_restart()`, setting `RES.kf_set` = 1 and moving the frame IP back four bytes (to `TRAP #9`). Internal status `E_SWITCH` propagates to the dispatcher. Arguments remain in the frame, so the call is simply retried. The line-input buffer survives the restart in the session frame; before restarting, `SLEEP` writes the remaining number of ticks to frame R0.
- **A tick interrupting a process** (§3.5.1): `irq0` redirects the process to the `PREEMPT_PROC` stub at `$E004`, saving the previous IP in `RES.sw_ip`. `IRET` restores the process FLAGS, and the stub performs a full `TRAP #9` entry with call number `SYS_PREEMPT` (`$7F`). The process stack does not grow; the frame represents the exact program state. `sys_preempt` sets frame IP = `RES.sw_ip`, `RES.kf_raw` = 1 (preserve FLAGS), and `RES.kf_set` = 1. Calling `$7F` from anywhere other than the stub (return IP ≠ `$E008`) returns `NOSYS`. A tick occurring between the stub's `IRET` and `TRAP` sees IP = `PREEMPT_PROC` and does not redirect the process a second time.

A process with IF = 0 (inside a tick or Ctrl+C handler) cannot be interrupted by a tick. The switch waits until that process's next system call.

## 4. Processes

### 4.1. Process descriptor

```text
struct Proc {
    state: byte                 // FREE, RUN, WAIT (waiting for child), SUSP (top process in another session)
    sess: byte                  // session
    parent: byte                // parent descriptor index; $FF means session root
    raw: byte                   // ctx contains an exact frame (§3.8)
    top: uint                   // upper memory boundary
    nfr: uint                   // number of private frames: top / $2000
    fr: [7]byte                 // private frames for pages 0 .. nfr - 1
    map: [7]byte                // map of pages 0..6 while process is not current (§2.3)
    nx: byte                    // number of extended-memory frames
    hook: uint                  // TICKHOOK or 0
    brkh, badh: uint            // Ctrl+C and illegal-instruction handlers (ONEVENT), or 0
    conmode: uint               // console mode (System Calls, §6.1)
    disp_ctrl, disp_attr: byte  // display CTRL and ATTR saved on child startup
    fd: [16]byte                // descriptors: open-file index or $FF
    cwd: Chain                  // current directory (§6.6)
    ctx: [16]uint               // KF frame while process is not current
}
```

The process descriptors form a `procs[16]` array shared by all sessions. A new process receives any free descriptor (`proc_alloc`). A session's process chain consists of `parent` links from its top process (`sess[s].top`) back to the root. The index of the current process is `cur`; `cursess` identifies the current session. `proc_enter` sets `cur` and `sess[s].top`. Checks for an object being in use (`BUSY`, §§6.5–6.6) inspect all occupied descriptors in every session.

### 4.2. Process memory

**Extended memory and `SETTOP`** (*System Calls*, §§7.6 and 9.6) are implemented in `sys.b16`:
- `XALLOC`: call `frame_alloc` with the process index, zero the frame through W0, set its bit in `RES.xbits`, increment `Proc.nx`.
- `XFREE`: validate through `RES.xbits`; pages mapped to this frame revert to the private or sink frame from `RES.omap`; clear the bit and decrement `Proc.nx`.
- `XMAP`: valid calls use the resident fast path (§3.2a). Handler `sys_xmap` performs the same operation using `RES.cmap`, `RES.omap`, and `RES.xbits`, but in practice receives only invalid requests.
- `SETTOP`: allocate or release private frames; update `Proc.fr`, `RES.cmap`, `RES.omap`, and `top`; write `top` into the PIB.
- `MEMINFO`: count extended-memory frames as frames owned by the process minus private frames.

The extended-memory frame number exposed to a program is the machine's physical frame number.

### 4.2.1. Private memory at load time

Process frames are allocated from the pool when a program loads and are released when it terminates. The process receives `top / $2000` frames (*CX16 Format*, §6). The amount of *available memory* used to calculate `top` is `nfree × $2000`, capped at `$E000`.

### 4.3. SPAWN

1. **Validation.** A free descriptor must exist; otherwise return `NOPROC` (the limit is 16 processes across all sessions). Parent descriptors `fin`, `fout`, and `ferr` must be open; otherwise return `BADF`.
2. **Command line.** Copy into a kernel buffer using `ustr` (maximum 255 bytes; otherwise `ARGS`), and parse according to *CX16 Format*, §5.2, into a temporary argument block in kernel data.
3. **Loading**, as specified in *CX16 Format*, §9.1:
   - Open the file named by `argv[0]` through the FS16 module.
   - Read the first sector into a cache buffer; copy and validate the header.
   - Compute `need`, `want`, and `top` using `ulong` arithmetic without overflow; allocate frames.
   - Map each frame into W0 and zero it (`kzero`).
   - Read the image: for each page 0 … ⌈`image_end` / $2000⌉ − 1, read whole sectors directly into W0; zero again the tail beyond `image_end` in the last sector.
   - Write the argument block and PIB through the windows; set PIB flags based on descriptor types `fin` and `fout`.

   If loading fails, close the program file if it was opened, release any frames already allocated, and return the error without creating a child or changing the parent's process state.
4. **New descriptor.** Allocate a free descriptor; set `sess` to the parent's session and `parent` to its index; store the command line in the session frame (§4.7):
   - `fd[0..2]` reference the parent's `fin`, `fout`, and `ferr` open files (`$FFFF` selects its descriptors 0, 1, 2); increment those open files' reference counts.
   - Set all other `fd` entries to `$FF`.
   - Copy the parent's `cwd`; set `hook` = 0 and `conmode` = 0.
   - Save the current display CTRL and ATTR in the parent's descriptor (`disp_ctrl`, `disp_attr`).
5. **Context switch.**
   - Copy `KF` into the parent's `ctx` and set its state to WAIT.
   - Move the parent's map from `RES.cmap` into `Proc.map` (`cmap_save`, §2.3); rebuild `RES.cmap`, `RES.omap`, and `RES.xbits` for the child; set `RES.hook` = 0, `RES.nobreak` = 0.
   - Fill the frame with the child's initial state (§3.4). Return from the handler to the dispatcher; `resume` starts the child.

Only after the child has loaded successfully does `SPAWN` save the parent's context, place it in WAIT, and enter the child with a complete initial frame (`RES.kf_set` = 1; §3.3). On failure, it returns an ordinary error without suspending the parent or replacing its frame.

### 4.4. EXIT

`EXIT` is entered either by an explicit process call or through the exit stub (on return to the initial LR, Ctrl+C termination, or an illegal instruction). Its handler:

1. **Messages.**
   - For status `$8001` with `RES.brk_echo` = 1 (or on `INTR`, §3.6), display `^C` and a newline, and clear the keyboard ring and console line.
   - For status `$8008`, display an illegal-instruction message with address `RES.fault_ip`.
2. **Resources.** Close all descriptors owned by the terminating process and release its allocated frames.
3. **Session root** (`parent` = `$FF`). If other sessions exist, close this session (release its session frame) and use `sess_enter` to select the next session cyclically (§3.8). If this was the last session, follow §4.6.
4. **Parent.**
   - Load the waiting parent's saved `ctx` into `KF`; set frame R0 = `OK` and frame R1 = the child's exit status. The parent becomes the top process in its session.
   - Restore the parent's map, tick handler (`RES.cmap`, `RES.hook`), console mode, and `RES.nobreak`.
   - Restore display CTRL and ATTR from the parent's `disp_ctrl`, `disp_attr`.
5. **Return.** Set `RES.kf_set` = 1 so the dispatcher keeps the restored frame; `resume` then returns control to the parent at the end of its `SPAWN` call.

### 4.5. EXEC

1. Copy and parse the command line exactly as for `SPAWN` (§4.3, item 2).
2. Load the replacement into the temporary descriptor `xproc` with the loader from §4.3, item 3. Derive PIB redirection flags from the current process's descriptors 0 and 1. A load failure releases the temporary frames and returns an error to the original program, which remains active with its previous memory and handlers.
3. Only after loading succeeds, release the old program's frames and transfer the new frames, `top`, and initial execution context from `xproc` into the existing process descriptor. Then clear its tick and event handlers and reset console mode; its open descriptors and current directory remain intact.
4. Call `proc_enter` for the same descriptor and set `RES.kf_set` = 1. `resume` starts the replacement program.

While loading, frames belonging to both programs are allocated (*System Calls*, §9.2).

### 4.6. Shell, startup, and shutdown

**Startup.** After initialization (§8.3), `kmain`:
- Attempts to mount the connected FS16 volumes (§6.3).
- Displays the Monitor banner.
- Creates session 0 (§4.7) and starts the shell `A:/sys/shell.cx` in it as the session root (`root_load`). This uses the common loading code from §4.3, items 2–5, but has no parent: descriptors 0–2 are opened on `dev:con`, and the current directory is `A:/`. The screen is not restored from the session frame at this point, so the Monitor banner remains visible.

If the initial shell `A:/sys/shell.cx` cannot be loaded, Monitor displays the error code and halts without entering an interactive session.

**Termination of the last session's root** (normally the shell):
- Status 0: perform `SHUTDOWN`.
- Any other status: report the exit code and try to start the standard shell again from `A:/sys/shell.cx` in the remaining session. If that load fails, report its error and halt.

**Shutdown** (`SHUTDOWN`):
1. Release the processes in every session and the session frames, then synchronize deferred filesystem writes (`SYNC`).
2. Unmount every volume.
3. Display a message.
4. Halt the machine using the procedure from *Machine*, §3: disable the timer, execute `ei()`, then `di()`, then loop on `wfi()`.

### 4.7. Sessions

```text
struct Sess {
    used: byte
    frame: byte                 // session frame containing SessSave
    top: byte                   // top process in the chain
    pad: byte
}
var sess: [4]Sess
var cursess: uint               // current session
var sw_to: uint                 // switch on kernel exit (§3.8), or $FF

struct SessSave {               // in the session frame, via window W0
    scr: [2000]uint             // display: character | attribute << 8
    ctrl, attr, col, row: byte
    ln_len, ln_pos, ln_ready, ln_eof: uint
    ln: [256]byte               // console line (§5.2)
    names: [16][56]byte         // command lines for this session's processes, by descriptor index
}
```

A **session frame** is a pool frame owned by `OWN_SESS` (`$FD`). `sess_create` allocates it with a blank screen (spaces, attribute `$07`), CTRL = 3, cursor at (0, 0), and an empty input line; `sess_free` releases it when the session closes. To save the outgoing screen, `scr_save` in `klib.asm` performs 2,000 consecutive word reads through SCRD (*Machine*, §2.2), starting at (0, 0). `scr_restore` draws the incoming screen in coordinate mode (CTRL = 0), writing ATTR and DATA for each cell. It then returns to (0, 0) without scrolling, and restores COL, ROW, ATTR, and CTRL, in that order. A switch takes approximately 25,000 instructions.

The active **console line** (§5.2) remains in global kernel data. `sess_leave` saves its state to the outgoing session frame, and `sess_enter` restores the incoming session's state; the console routines therefore need no session-specific logic. Separately, `proc_enter` restores the event handlers and console mode belonging to the incoming session's current process.

**System calls** are implemented in `sess.b16`:
- `SESS_INFO`: follow `parent` links from `sess[s].top` and report the *k*th process from the end; read its command line from the session frame.
- `SESS_NEW`: run `sess_create`, then `root_load` (load the requested root program, attach descriptors 0–2 to the console, inherit the caller's `cwd`, and leave the new root in `PS_SUSP`). If root loading fails, return its error and release the new session frame; no active session is added. Unless `SN_BACK` is set, assign the successful new session to `sw_to`.
- `SESS_SWITCH`: set `sw_to` = s.
- `SESS_KILL`: call `proc_release` for every process in the session and then `sess_free`. If terminating the current session, enter the next session with `sess_enter` and set `RES.kf_set` = 1.

## 5. Console

### 5.1. Events and Ctrl+C

The event ring `RES.kb` on page 7 has 256 word-sized slots, each containing a character and scan code. `RES.kb_head` is the producer position, advanced only by `irq0`, and `RES.kb_tail` is the consumer position, advanced by the kernel; both are stored as byte offsets. Equal positions mean that the ring is empty. The parallel array `RES.kbm` holds 256 modifier bytes, one for each event slot. `ev_get` copies the retrieved event's modifiers to `ev_mods`, which `CON_GETEV` returns in R2.

`RES.nobreak` mirrors the `NOBREAK` bit of the current process's console mode and is updated by `CON_MODE`, `SPAWN`, and `EXIT`. When the bit is set, `irq0` queues Ctrl+C as a normal event with character `$03` instead of marking a pending interrupt.

`RES.abort` = 1 means that Ctrl+C has not yet been handled. It is processed by:
- `irq0` if the tick interrupted a process (§3.5.1);
- a wait loop (§3.6);
- `pending_break()` on kernel exit (§3.4).

`CON_FLUSH` discards queued events (`kb_tail` = `kb_head`) and resets the console line buffer. Handling Ctrl+C as an interrupt also discards pending input; if the handler runs directly from `irq0`, line invalidation is deferred via `RES.ln_drop` until the next console operation.

### 5.2. Line input

A line is held in a 256-byte buffer in kernel data, together with its length `ln_len`, a `ln_ready` flag (Enter has completed the line but the application has not yet read all of it), and a read position `ln_pos`.

Console `READ` in line mode:
- If `ln_ready`, returns bytes starting at `ln_pos`, up to the requested count. When all bytes have been delivered, clears `ln_ready`.
- Otherwise waits for events (§3.6), processing them according to the table in *System Calls*, §6.2, echoing through `kputc` until Enter arrives or Ctrl+Z is received at the start of a line.

In line-input mode, even with `NOBREAK` enabled, an event with character `$03` cancels the unfinished line and makes `READ` return `INTR` (*System Calls*, §6.3). Because `NOBREAK` is set, `pending_break()` does not terminate the process.

The input line belongs to the console rather than to an individual process: an unfinished line is discarded when that process terminates. Each session keeps its own line state (§4.7), including the read position of a completed line that has only been partly consumed.

### 5.3. Output

The kernel writes to the display through `kputc`, which writes one byte to DATA. Monitor messages go directly to the display, bypassing descriptors.

Console `WRITE` copies the buffer in spans through the mapping windows and outputs each byte via `kputc`.

**Monitor messages** are in English:

| Situation | Message |
| --- | --- |
| startup | `CPU16 monitor, revision 6` |
| volume has DIRTY at startup | `monitor: volume X: was not unmounted cleanly (DIRTY), mounted read-only` (X is the volume letter) |
| disk 0 not mounted at startup | `monitor: disk 0 is not mounted` |
| shell cannot start | `monitor: cannot start the shell, error $XXXX` |
| shell exits | `monitor: shell exited, code $XXXX` |
| shell restarts | `monitor: restarting the shell` |
| process terminated by Ctrl+C | `^C` |
| illegal instruction | `monitor: illegal instruction at $XXXX` |
| `TRAP #10` … `#15` | `monitor: reserved instruction at $XXXX` |
| error writing a volume during shutdown | `monitor: error writing volume N, error $XXXX` |
| halt | `monitor: halted` |
| kernel fault | `monitor: kernel fault at $XXXX` |

## 6. Disks and file system

### 6.1. Disk driver

```text
fn disk_io(cmd: uint, drv: uint, lba: ulong, buf: ptr, count: uint): uint
```

`buf` is a logical address in the kernel map: a cache buffer on page 7, a span in a mapping window, or kernel data. The buffer must not extend beyond `$FF1F` (*Machine*, §6.2).

`disk_io`:
1. Writes the controller registers and issues a command with IEN = 1.
2. Waits for `RES.dk_done` in a noninterruptible loop (§3.6).
3. Returns the kernel error code corresponding to the controller status:

| Controller | Kernel |
| --- | --- |
| OK | `OK` |
| NODISK | `NODEV` |
| RANGE | `INVAL` |
| BADADDR | `FAULT` (kernel error: the buffer should have been validated in advance) |
| READONLY | `RDONLY` |
| IO | `IO` |

Before each access to a mounted volume, the kernel checks the corresponding bit in DRIVES. If the disk is absent, the volume is marked lost (*System Calls*, §5.3), its cached sectors are discarded, and the operation returns `NODEV`.

### 6.2. Sector cache

Eight 512-byte buffers on page 7 (`$EE00–$FDFF`) have corresponding descriptors in kernel data. Each descriptor holds a key (disk, LBA), VALID, DIRTY, and PIN flags, and a clock-replacement marker. This is the unmodified `cache.b16` module from prototype A.

- A buffer marked PIN cannot be evicted. FS16 pins an index-cluster buffer while traversing a file.
- A modified buffer is written when evicted, during `cache_flush`, and at points required by FS16 write ordering (§6.7).
- `cache_drop(drv)` discards every buffer belonging to the specified disk on unmount or volume loss.

### 6.3. Volumes

The `vols[8]` volume table has one entry per disk:
- State: unmounted, read-write, read-only, or lost.
- Reason for read-only mode: `MNT_DIRTY` or `MNT_PROTECT`.
- Required superblock fields: N, B, FREE, HINT, SERIAL, LABEL, and the ROOT entry.

**Mounting** uses `vol_mount` from prototype A, with three changes:
- A volume marked DIRTY is mounted read-only with `MNT_DIRTY` instead of being rejected.
- A write-protected disk is mounted read-only with `MNT_PROTECT`.
- Error codes follow *System Calls*.

At startup, `kmain` attempts to mount each disk reported by DRIVES. It warns about volumes marked `MNT_DIRTY` but silently skips disks that do not contain a valid FS16 volume.

**Unmounting** uses `vol_umount`. For a read-write volume it flushes deferred writes, clears the on-disk DIRTY flag, and issues disk FLUSH commands before discarding cached sectors with `cache_drop`. A lost volume is removed without attempting disk writes.

**Checking a DIRTY volume** is performed by the `fsck` application using `DISK_IO`. This call is allowed to write to a volume mounted read-only; after such a write, `sys_disk_io` invokes `cache_drop`. `sys_disk_io` may write sectors 1–3 (the boot code) on any volume: the kernel neither reads nor caches those sectors. Thus `sysboot` can install boot code on mounted volume 0. `MOUNT` with `MOUNT_REMOUNT`, when the volume has no open nodes, runs `vol_sync` (if read-write), `cache_drop`, and `vol_mount` again. The current-directory chains of processes are preserved: file-system checking does not relocate entries; it only truncates files and removes duplicates left by an interrupted move. At system startup the shell's current directory is the root, so the issue does not arise for it.

### 6.4. FS16 module

The base is provided by the following prototype-A modules: `disk.b16`, `cache.b16`, `volume.b16`, `path.b16`, `dir.b16`, and `file.b16`. Porting them into the kernel requires these changes:

1. **Error codes:** use the codes in *System Calls*, §8, instead of the prototype's private codes:

   | Prototype A | Kernel |
   | --- | --- |
   | `E_EXISTS` | `EXIST` |
   | `E_FULL`, `E_TOOBIG` | `NOSPC` |
   | `E_ROFS` | `RDONLY` |
   | `E_NOTMNT` | `NODEV` |
   | `E_LOOP` | `INVAL` |
   | `E_DIRTY` | (not needed; §6.3) |
   | `E_NOBUF` | `IO` (kernel error) |

2. **Shared node** across all opens of a given file (§6.5). In the prototype, each `File` contains its own copy of `Node`, so a size change through one open instance is invisible to another. *System Calls*, §5.2, requires shared metadata and size.
3. **Current directory:** use the current process descriptor (`procs[cur].cwd`) instead of the global `cwd`.
4. **Path prefixes:** parse `X:` and `dev:` before invoking `path_walk` (§6.6). Volume `X:` accepts letters `A`–`H`, case-insensitively, or digits `0`–`7` (function `drv_prefix` in `fs_path.b16`; *System Calls*, §4.1).
5. **In-use checks:** deleting or renaming an open object, or a directory on the current-directory path of any process in a chain, returns `BUSY` (*System Calls*, §§4.3 and 5.2).
6. **File growth:** add a size-extension function for `SETSIZE`, both with and without `SZ_ZERO`, based on `node_alloc` / `node_link`.
7. **Call buffers:** the supplied address points into a mapping window (§2.6). The prototype's `fs_read` and `fs_write` already transfer full sectors directly to or from this address and use the cache for partial sectors. Keep this behavior.

### 6.5. Nodes, open files, and descriptors

Use three levels, as in *System Calls*, §5:

| Level | Table | Contents |
| --- | --- | --- |
| node | `nodes[32]` | prototype-A `Node` (disk, entry address, size, flags, MTIME, addresses) plus open-file reference count |
| open file | `ofiles[32]` | kind (file, directory, console, `dev:nul`), `O_…` mode, node index, position, cached last address (prototype `lk`, `lc`), descriptor reference count |
| descriptor | `Proc.fd[16]` | open-file index or `$FF` |

- **Open:** `OPEN` looks up a node by (disk, entry address). If absent, it loads the node from the directory entry; it then creates a new open-file object.
- **Sharing:** `DUP` and `SPAWN` increment the open-file reference count.
- **Last descriptor:** when the last descriptor referring to an open file is closed, decrement the node's reference count; release the node when its count reaches zero. Closing does not write to disk: the call that modified the file has already updated its directory entry with size and MTIME.
- **In-use state** (§6.4, item 5) is determined from the node table: an object is open when a node exists for its entry address.
- **Console and `dev:nul`** do not have nodes.

### 6.6. Paths

`kpath` parses a path previously copied into a kernel buffer:
- Prefix `0:`–`7:` → select volume and start at its root.
- Prefix `dev:` → device (compare name with `con` and `nul`).
- No prefix → use the process's current volume and current-directory chain.

The remainder of the path is passed to prototype-A `path_walk`, which handles `.`, `..`, repeated and trailing `/`, validates names and checks maximum depth (`Chain` with 16 elements).

**Name comparison.** Here the kernel differs from prototype A, which compares NAME byte-for-byte. The kernel must compare names case-insensitively according to FS16, §7.1, using a single `name_eq` function and `fold` conversion. This applies when finding path components in `path_walk` and checking that a name does not exist (`OPEN` with `O_CREATE`, `MKDIR`, `RENAME`). Case folding does not change the name length, so `name_eq` still compares 32-byte fields. NAME is written with the original casing from the path; it is not folded on storage.

**Case-only `RENAME`.** If `new` resolves to the same directory-entry address as `old`, `RENAME` only rewrites that entry's NAME. This case must not fail with `EXIST`.

The current directory is a `Chain` of directory-entry addresses starting from the root. `GETCWD` constructs a path such as `X:/a/b`, using an uppercase volume letter and reading each entry's name via the cache.

### 6.7. Write ordering and SYNC

The FS16 module implements the write-ordering rules from FS16, §§9–10, as in prototype A:
- **Allocation bitmap:** write it immediately when allocating.
- **Bitmap releases:** defer writes (mark the bitmap buffer DIRTY). `SYNC`, unmount, or buffer eviction writes them.
- **Data:** write immediately.
- **Directory entries:** write once at the end of the operation, before returning from the call (*System Calls*, §5.4).

For each read-write mounted volume, `SYNC` flushes dirty cache buffers, updates the superblock FREE field, and issues a disk FLUSH. The on-disk DIRTY flag remains set until a clean unmount (FS16, §10).

## 7. Time

- `TIME_GET` reads the real-time clock and returns time and date as an FS16-format `ulong` (*Machine*, §8; prototype-A function `now()`).
- `TIME_SET` validates the fields and writes the clock.
- MTIME is set using the same `now()` function.
- `TICKS` returns `RES.ticks`.
- `SLEEP(n)` waits until `RES.ticks` has advanced by n, using the interruptible loop in §3.6.
- The timer is enabled in `kmain` and disabled only during shutdown.

## 8. Modules, build, and startup

### 8.1. Modules

| File | Language | Purpose |
| --- | --- | --- |
| `monitor.asm` | assembly | master build file (§8.2) |
| `boot.asm` | assembly | initial image-startup code on a bare machine (§8.3) |
| `resident.asm` | assembly | page 7: exit stub, `PREEMPT_PROC` stub, `boot_entry`, vector stubs, `trap9`, dispatcher, `resume`, `irq0`, `irq1`, `err8` |
| `klib.asm` | assembly | `kcopy`, `kzero`, `scr_save`, `scr_restore`, handler table `sys_table` |
| `kernel.b16` | B16 | `kmain`; `include` of the remaining modules |
| `kdefs.b16` | B16 | error codes, call numbers, constants and structures from *System Calls*, §10.2 |
| `res.b16` | B16 | `RES`, `KF`, device registers (`at`) |
| `mm.b16` | B16 | frames, current-process map (`RES.cmap`, `RES.omap`, `RES.xbits`, `pmap`), `uwin`, `ustr`, `ucopyin`, `ucopyout` |
| `proc.b16` | B16 | descriptors, CX16 loader, `SPAWN`, `EXEC`, `EXIT`, session root, `pending_break` |
| `sess.b16` | B16 | sessions: session frame, `sess_leave`, `sess_enter`, `kexit`, `SESS_…`, `sys_preempt` (§§3.8, 4.7) |
| `console.b16` | B16 | event ring, line input, `kputc`, modes |
| `disk.b16`, `cache.b16`, `volume.b16`, `path.b16`, `dir.b16`, `file.b16` | B16 | FS16 module (§6.4) |
| `fd.b16` | B16 | nodes, open files, descriptors, devices, `kpath` |
| `sys.b16` | B16 | call handlers (`export fn`) |
| `time.b16` | B16 | clock, `SLEEP`, `TICKS` |

The call indices in `sys_table` and the `SYS_…` constants must have the same source of truth. Constants in `kdefs.b16` are emitted by the compiler as `.equ` definitions (B16, §10.3), and `klib.asm` builds the table from them.

### 8.2. Building

```text
b16c -Werror kernel.b16 -o kernel.s
cpu16asm -m monitor.map -l monitor.lst -o monitor.bin monitor.asm
```

`monitor.asm`:

```asm
        .org    $0000
        JMP     #boot                   ; bare-machine startup (§8.3)
        .include "kernel.s"             ; code $0004..$7FFF, data from $8000
        .include "boot.asm"             ; .org $C000
        .include "resident.asm"         ; .org $E000

; limits: any overlap is a build error
        .org    $A000
        .reserve $C000 - $A000          ; window W0: no kernel code or data
        .org    RES
        .reserve $E800 - RES            ; RES data
        .org    $E800
        .reserve $EE00 - $E800          ; kernel stack and KF
        .org    $EE00
        .reserve $FF00 - $EE00          ; sector buffers and reserved space
```

Address-range checks:
- **Kernel code** must not reach `$8000`, where data begin (`option data = $8000`); their `.reserve` directives would overlap the code.
- **Kernel data** must not extend past `$9FFF`, because the `.reserve` for window W0 starts there.
- **Resident code** must not grow into `RES`.

Initial startup code resides in page 6 (W1) and is not needed after startup.

### 8.3. Startup

Monitor can start in two ways. Both converge at the `boot_entry` label in the resident component (page 7):
- **From disk:** ROM, volume boot code, and `A:/sys/monitor.sys` (see *CPU16 Monitor: Disk Boot*).
- **As a bare-machine image:** the emulator loads `monitor.bin` starting at address 0 with the memory manager disabled (used for kernel debugging and tests). The image occupies frames 0–7; the resident component already occupies frame 7. Initial `boot.asm` code runs on page 6 (`$C000`), with a stack growing downward from `$E000` on that same page. It:
  1. Enables the memory manager with a temporary map, then copies kernel code pages (0 … ⌈`__image_end` / $2000⌉ − 1) into frames 8 and up through a window.
  2. Installs the kernel map except for PAGE6, which contains the executing startup code.
  3. Jumps to `boot_entry`.

**The `boot_entry` contract** defines the machine state in which any loader hands off control:
- Frames 8 and above hold the kernel code pages (addresses `$0000` … `__image_end`); frame 7 holds the resident component (`$E000` … `resident_end`).
- The memory manager is enabled; PAGE0–PAGE4 map frames 8–12 and PAGE7 maps frame 7. PAGE5 and PAGE6 may map anything.
- SP points to a working stack outside pages 0–4 and 7; IF = 0.
- The vector table may contain arbitrary values.

**`boot_entry`** (resident assembly):
1. Installs the vector table (§3.5).
2. Calls `__init_data` (copies initial values into page 4 and zeros BSS).
3. Sets SP = `KF` and calls `kmain`, which never returns.

In order, `kmain`:
1. Initializes `owner` (frames 14–255 free) and all tables.
2. Puts the display into teletype mode with cursor enabled and ATTR = `$07`.
3. Enables the timer.
4. Mounts volumes, displays the banner, and launches the shell (§4.6).

## 9. Debugging and validation

### 9.1. In-kernel instrumentation

- **Stack canary.** In a debug build, word `$E800` contains a known sentinel value. `resume` verifies it and calls `kernel_fault` if it has changed.
- **Consistency checks** performed at each `SPAWN` and `EXIT` in debug builds:
  - The chain `depth` and descriptor states agree.
  - The number of free frames in `owner` matches `nfree`.
  - Node and open-file reference counts agree with their respective tables.

### 9.2. Kernel fault

`kernel_fault(ip)` displays the address and halts the machine. It is invoked for ERRCMD in kernel mode, canary corruption, or failed consistency checks. A kernel fault always indicates an implementation defect; execution must not continue afterward.

### 9.3. Tests

The baseline is the prototype-B test harness (emulator with controlled tick schedules) and the prototype-A FS16 tests (disk images, host utility, checksums). Kernel validation includes:
- **Per-call tests:** every call listed in *System Calls*, §9, both with a successful result and with every error described for that call.
- **Scenarios:**
  - Process chain of depth 16 and `NOPROC` refusal.
  - Ctrl+C in a computation loop, console `READ`, `SLEEP`, and during a disk operation.
  - `NOBREAK` in line and raw input modes.
  - `TICKHOOK` and process termination with a handler installed.
  - Redirection of standard streams to a file.
  - Disk removal while files remain open.
  - Mounting a DIRTY volume.
  - Filling a disk until `NOSPC`.
  - Growing and truncating with `SETSIZE`.
  - Sessions (`tsess`, `session`, `sessfm`): every call and its error cases; switching with Ctrl+] during a computation loop (preserving FLAGS and registers, including between `CMP` and a branch); Ctrl+[ during `CON_GETEV` and `SLEEP` (restarting the call, remaining sleep time); screen and display mode after a switch; closing a session by root termination and by `SESS_KILL`; finishing execution in another session; shell `session` command; file manager in one session and shell in another.
- **Every tick schedule** supported by the prototype-B harness, both with and without `-fcopy-params`.
- **Integrity:** use the host utility to check the volume after every series of write operations.
- **Stack:** measure stack depth as in the prototypes under boundary-case scenarios.

## 10. Resource budgets

| Resource | Prototype measurements / estimates | Limit |
| --- | --- | --- |
| kernel code | FS16 9.5 KiB plus estimated 8–12 KiB for the rest ≈ 18–22 KiB | pages 0–3: 32 KiB minus 4 bytes |
| kernel code (revision 5) | 27.0 KiB, occupied through `$6BEE` | |
| kernel data and BSS | 7.7 KiB in the revision-5 implementation, occupied through `$9EA2` (§2.5 estimated 5.5 KiB) | page 4: 8 KiB |
| resident component | 476 bytes of code and data in prototype B; revision-5 implementation extends to `$E394` (call table moved into kernel code) | `$E000–$E3FF`: 1024 bytes |
| `RES` | approximately 860 bytes with 256 keyboard events and their modifier bytes | 1024 bytes |
| session frames | one pool frame per session | 4 |
| kernel stack | FS16 220 bytes + calls 86 bytes + tick during a wait ≈ 350–400 bytes | 1504 bytes |
| sector buffers | 8 | 8 |

Kernel code size is refined from the memory map at every implementation stage. If code reaches 28 KiB, revisit §2.2 and consider moving infrequently used code (loader, mounting) into an overlay temporarily mapped into a window during a call.

## 11. Deferred work

| Feature | How the kernel design accommodates it |
| --- | --- |
| asynchronous disk I/O | restrict DMA to buffers on page 7; switch processes while waiting |
| debugger (`TRAP #10`) | vector 10 is currently assigned to the `trap_bad` stub |

## 12. Revision history

### Revision 6

- **Build** (§8.2): the target, code start address, data start address, and `klib.asm` module are specified in `kernel.b16` (`option target = kernel`, `option code = $0004`, `option data = $8000`, `include asm "klib.asm"`; B16 revision 3). The build command no longer needs parameter flags.
Implementation of *System Calls* revision 6: volume letters are supported. `drv_prefix` (`fs_path.b16`) accepts `A:`–`H:` regardless of case and retains `0:`–`7:` for compatibility; `GETCWD` and kernel messages use uppercase volume letters. The shell path is `A:/sys/shell.cx` (§§4.6, 6.6). Startup message is `CPU16 monitor, revision 6`; `SYSINFO.version` = `$0600`.

### Revision 5

Implementation of *System Calls* revision 5 — sessions:
- Session switches occur only with an empty kernel stack in `kexit()`. The three request paths are: after a call; restarting a blocked call (`wait_restart`); and the `PREEMPT_PROC` stub reached from a tick with an exact frame (`RES.kf_raw`, `Proc.raw`) (§3.8).
- The process chain uses `parent` links rather than contiguous descriptors 0 … `depth − 1`. `depth` was removed; `cur` is now the current descriptor index. The limit of 16 applies across all sessions (§§4.1, 4.3, 4.4).
- Each session frame holds the screen (saved using SCRD reads; *Machine*, revision 9), console line, and process command lines (§4.7).
- `irq0` recognizes Ctrl+] and Ctrl+[ (§3.5.1); `resume` does not recompute ZF in an exact frame (§3.4).
- The 128-entry `sys_table` was moved from the resident component into `klib.asm` because of resident-space pressure (§3.3).
- Startup message: `CPU16 monitor, revision 5`; `SYSINFO.version` = `$0500`.

### Revision 4

Implementation of *System Calls* revision 4:
- Keyboard event modifiers (*Machine*, revision 8, MODS register): `irq0` stores them in `RES.kbm` alongside the event ring, and `CON_GETEV` returns them as a third result (§§3.5.1, 5).
- `RES` moved from `$E500` to `$E400` and grew to 1,024 bytes; resident code is now limited to `$E3FF` (§2.4).
- Startup message: `CPU16 monitor, revision 4`; `SYSINFO.version` = `$0400`.

Fast `XMAP` (no changes to *System Calls*):
- `RES.cmap` is the authoritative current-process map; `Proc.map` is stored only for non-current processes. It is transferred when the current process changes (`cmap_set`, `cmap_save`), not on every kernel entry (§2.3).
- `RES` gained the current process's private map `omap` and extended-frame bitmap `xbits`; `Proc` gained counter `nx` (§§2.3, 4.1).
- The resident `XMAP` fast path takes approximately 38 instructions instead of about 330. Other calls take six extra instructions (§§3.2, 3.2a).
- `XFREE`, `XMAP`, and `SETTOP` use `RES.xbits` and `RES.omap` rather than scanning `Proc.fr` to validate frames (§4.2).

### Revision 3

Implementation of *System Calls* revision 3:
- Case-insensitive name comparison (`name_eq`, `fold`) in `path_walk` and existence checks; case-only `RENAME` (§6.6).
- Ctrl+Enter arrives from the machine as an event with character 0 and scan code `$1C`; line mode ignores it, so no kernel changes are needed.
- Startup message: `CPU16 monitor, revision 3`; `SYSINFO.version` = `$0300`.

### Revision 2

Implementation of *System Calls* revision 2:
- Frame ownership table `owner` replaced `fmap`; current-process map `Proc.map`; `SETTOP` and extended memory (§§2.1, 2.3, 2.6, 4.2).
- `EXEC` (§4.5), event handlers in `pending_break`, `irq0`, and `err8` (§§3.4, 3.5); fields `Proc.brkh`, `Proc.badh`, `RES.brk_hook`, `RES.bad_hook`, `RES.ln_drop`.
- `SETMTIME`; `DISK_IO` may write sectors 1–3 on any volume (§6.3).
- Startup message: `CPU16 monitor, revision 2`; `SYSINFO.version` = `$0200`.

### Revision 1

Implementation-driven refinements: checking a DIRTY volume requires `DISK_IO` writes to a read-only mounted volume and `MOUNT_REMOUNT` (§6.3); all kernel messages are in English. For disk boot, `boot_entry` was moved into the resident component and now installs the vectors itself, simplifying the loader contract (§8.3). The startup-code stack occupies the top of page 6 rather than reserved space on page 7 (§§2.4, 8.3, stage 1). MTIME is updated by the call that modifies a file, not on close (§6.5, stage 2).

First edition. Based on the Monitor design, resolutions of open design questions, *System Calls* revision 1, and prototypes A and B, revision 2.
