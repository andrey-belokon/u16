# CPU16 Machine: Memory and Devices. Revision 9

Revision 9 introduces the SCRD screen-read register (Section 2.2). Revision 8 adds the MODS keyboard modifier register (Section 2.6). Revision 7 distinguishes Ctrl+Enter from Enter (Section 2.6). Following prototype testing, Revision 6 corrects the procedure for stopping a program (Section 3) and adds an example of waiting for IRQ1 from code running with IF = 0 (Section 6.6). Revision 5 added the real-time clock (Section 8), moving the Emulators chapter to Section 9. Revision 4 reserves the entire `$FF20–$FFFF` device window for peripherals and specifies disk-command validation order and common emulator behavior. Revision 3 added the paged memory manager (Section 7). The complete revision history is in Section 10.

This document specifies the machine built around the CPU16 processor: its memory map and I/O devices. The processor itself is defined in the *16-bit Processor Architecture* specification (Revision 6), and its assembly language in the *Assembler Reference*. This document covers the components outside the processor: the console with its display and keyboard, the timer, real-time clock, disk controller, memory manager, and character encoding. The clock value format comes from the *FS16 File System* specification.

Both the `cpu16` command-line emulator and the web debugger implement the machine described here. The terms **must** and **must not** express normative requirements.

## 1. Memory Map

The processor has a 64 KiB address space. Addresses in this section are **logical addresses**, as seen by the processor. The machine supports up to 2 MiB of physical memory, to which logical addresses are mapped by the memory manager (Section 7). With the memory manager disabled, physical and logical addresses are identical.

| Address range | Purpose |
|---|---|
| `$0000–$FEFF` | memory (entirely RAM in the emulators) |
| `$FF00–$FF1F` | interrupt vector table (ordinary memory; processor specification, Section 8) |
| `$FF20–$FFFF` | **device window**, subdivided as follows: |
| `$FF20–$FF29` | — console, display, keyboard, and timer (Sections 2–4) |
| `$FF2A–$FF2E` | — real-time clock (Section 8) |
| `$FF2F` | — unused (reserved for future devices) |
| `$FF30–$FF3B` | — disk controller (Section 6) |
| `$FF3C` | — MODS: keyboard modifiers (Section 2.6) |
| `$FF3D–$FF3F` | — unused (reserved for the disk controller) |
| `$FF40–$FF48` | — memory manager (Section 7) |
| `$FF49–$FF4F` | — unused (reserved for the memory manager) |
| `$FF50–$FF51` | — SCRD: screen readback (Section 2.2) |
| `$FF52–$FFFF` | — unused (reserved for future devices) |

After reset, execution starts at `$0000`, all processor registers are zero, and the memory manager is disabled. The recommended program memory layout—ROM at `$0000–$7FFF`, RAM at `$8000–$FEFF`, and a stack growing downward from `$FF00`—is described in Section 6 of the *Introduction*.

**The device window `$FF20–$FFFF` is not memory.** The whole range belongs to the device bus, whether or not a device occupies a particular address:

- **Logical-address decoding.** Accesses to the window are recognized by their logical address before memory-manager translation. They never reach physical memory, regardless of EN or PAGE7 (Section 7.5).
- **Unused addresses.** Reading an address with no device returns 0; writes are ignored. No memory byte is stored at that address.
- **Forward compatibility.** Programs must not use unused addresses: future revisions may assign devices to them. A zero read guarantees that such an access cannot damage memory; it does not provide data storage.
- **Vectors remain memory.** The interrupt vector table at `$FF00–$FF1F` lies below the device window. It is ordinary memory, translated through PAGE7. A stack growing downward from `$FF00` does not enter the device window.

**Device ports are byte-addressed.** Each register occupies one byte and is accessed with `LDZX`, `LDSX`, or `STB`. A word access at an even address A constitutes a single access to a register pair: A supplies the low byte and A + 1 the high byte. If either address is unused, the corresponding half of a read word is zero, and writes to it are discarded. The following word accesses are supported:

- Console: only a word read at `$FF20` (Section 2.6), which dequeues one keyboard event; a word read at `$FF50` (SCRD, Section 2.2) is also a single operation, reading one screen cell and advancing the position once.
- Clock: word reads and writes at `$FF2A` and `$FF2C` (Section 8.2).
- Disk controller: word reads and writes at any even register address (Section 6.1).
- Memory manager: word reads and writes at any even register address (Section 7.2).

Programs must not make any other word accesses to the console or clock. Emulators implement such accesses as two byte accesses, first at A and then at A + 1 (Section 9).

## 2. Console and Display

The console, display, and keyboard form a single device. Output goes to an 80-column by 25-row text screen; input arrives from the keyboard.

### 2.1. Screen

- The display has 80 columns and 25 rows, numbered from zero. Position (0, 0) is the top-left corner.
- Each screen cell holds a **character** (one byte in the machine encoding, Section 5) and an **attribute** (color and decoration, Section 2.4).
- The **position** identifies the current output cell; it is also the location of the visible cursor.
- Screen memory is internal to the device and is not part of the processor address space. It can be read through SCRD (Section 2.2).

### 2.2. Registers

| Address | Name | Read | Write |
|---|---|---|---|
| `$FF20` | DATA | character from the next keyboard event, or 0 if none; word: character and scan code (Section 2.6) | output a character (Section 2.3) |
| `$FF21` | SCAN | scan code of the next keyboard event (dequeues the entire event) | — |
| `$FF22` | CTRL | mode | mode |
| `$FF23` | ATTR | current attribute | current attribute |
| `$FF24` | COL | current column | current column (values above 79 are clamped to 79) |
| `$FF25` | ROW | current row | current row (values above 24 are clamped to 24) |
| `$FF26` | CMD | 0 | command (Section 2.5) |
| `$FF27` | COLS | screen width: 80 | — |
| `$FF28` | ROWS | screen height: 25 | — |
| `$FF3C` | MODS | modifiers of the last dequeued keyboard event (Section 2.6) | — |
| `$FF50` | SCRD | character at the current position; a word reads the entire cell (see below) | — |
| `$FF51` | — | attribute of the cell at the current position | — |

Writes to read-only registers have no effect.

**CTRL:**

| Bit | Name | Meaning |
|---|---|---|
| 0 | TTY | 1: teletype mode; 0: positioned mode |
| 1 | CURSOR | 1: cursor visible |
| 2 | ATTRMODE | 1: attribute mode |
| 3–7 | — | 0 |

**Reset state:**

- CTRL = `%011`: teletype mode, cursor visible;
- ATTR = `$07`: white on black;
- position (0, 0);
- screen filled with spaces carrying attribute `$07`;
- keyboard queue empty, MODS = 0.

**SCRD: reading screen contents.** A word read from `$FF50` (`LD R0, [$FF50]`) returns the cell at the current position: the character in the low byte and the attribute in the high byte. It then advances the position exactly as a write in positioned mode does (Section 2.3): after column 79, it moves to the start of the next row; after the last cell, it wraps to (0, 0). This behavior applies regardless of CTRL. Thus, 2,000 consecutive word reads beginning at (0, 0) capture the whole screen. A byte read from `$FF50` returns the character and a byte read from `$FF51` returns the attribute of the same cell; neither advances the position. Writes to SCRD are ignored.

```asm
        LD      R1, #0
        STB     [$FF24], R1             ; COL = 0
        STB     [$FF25], R1             ; ROW = 0
        LD      R1, #2000
@cell:  LD      R2, [$FF50]             ; character | attribute << 8; advance position
        ST      [R0++], R2
        SUB     R1, #1
        JNZ     @cell                   ; position returns to (0, 0)
```

To restore a screen from such a snapshot, select positioned mode and, for each cell, write its attribute to ATTR and its character to DATA. After the final cell, the position wraps to (0, 0) without scrolling.

MODS is outside `$FF20–$FF29` because subsequent addresses were already assigned to the clock and disk controller; it is nevertheless a console register, like the others in the table. It occupies the first address in the disk-controller reserve: using `$FF2F` would have caused an even-addressed word read at `$FF2E` to include a clock register.

### 2.3. Output: Writing DATA

The effect of a byte write to DATA depends on CTRL. Attribute mode takes precedence over the other modes.

**Teletype mode (TTY = 1, ATTRMODE = 0)** behaves like an ordinary console:

| Byte | Action |
|---|---|
| `$0A` (`\n`) | move to the start of the next row |
| `$0D` (`\r`) | move to the start of the current row |
| `$08` (`\b`) | move back one column (no effect at column 0); do not erase the character |
| `$09` (`\t`) | move to the next column divisible by 8; if past the end, move to the start of the next row |
| `$00` | no effect |
| all others | write the character at the current position with attribute ATTR and advance one column; after column 79, move to the start of the next row |

If advancing to the next row would pass row 24, the screen scrolls up by one row; the bottom row is filled with spaces carrying attribute ATTR, and the position remains on row 24.

**Positioned mode (TTY = 0, ATTRMODE = 0):** any byte, including `$00` and control codes, is stored as a character at the current position with attribute ATTR. The position advances one column, moves to the beginning of the next row after column 79, and wraps to (0, 0) after the last screen cell. No scrolling occurs.

**Attribute mode (ATTRMODE = 1; TTY is irrelevant):** the written byte becomes the **attribute** of the cell at the current position; the character is unchanged. The position advances as in positioned mode. This allows a portion of the screen to be highlighted by color or inverse video without redrawing its text.

### 2.4. Attributes and Palette

```text
   7      6      5    4    3     2    1    0
| INV  | UNDL | background (0..7) | foreground (0..7) |
```

- **Foreground** and **background** select palette color numbers.
- **INV** (inverse video) swaps foreground and background colors.
- **UNDL** (underline) draws a line beneath the character in the foreground color.

| Code | Color | RGB | Code | Color | RGB |
|---|---|---|---|---|---|
| 0 | black | `#000000` | 4 | red | `#C50F1F` |
| 1 | blue | `#0037DA` | 5 | yellow | `#C19C00` |
| 2 | green | `#13A10E` | 6 | gray | `#767676` |
| 3 | cyan | `#3A96DD` | 7 | white | `#CCCCCC` |

An attribute can be constructed as `BACKGROUND << 3 | FOREGROUND`. Examples:

| Attribute | Expression | Appearance |
|---|---|---|
| `$07` | `0 << 3 \| 7` | white on black |
| `$0F` | `1 << 3 \| 7` | white on blue |
| `$0D` | `1 << 3 \| 5` | yellow on blue |
| `$87` | `$80 \| $07` | inverse: black on white |
| `$47` | `$40 \| $07` | underlined white on black |

### 2.5. Commands: Writing CMD

| Code | Action |
|---|---|
| 1 | clear the screen; set position to (0, 0) |
| 2 | clear from the current position through the end of the row |
| 3 | clear from the current position through the end of the screen |
| 4 | scroll up one row, leaving a blank bottom row |
| 5 | scroll down one row, leaving a blank top row |

Cleared and newly exposed cells are filled with spaces carrying the current ATTR value. Commands 2–5 leave the position unchanged. Unknown command codes have no effect.

### 2.6. Input: Keyboard

The keyboard places **events** in a queue. Each event is a pair comprising a **character** (one byte in the machine encoding) and a **scan code** (the key number, Section 2.7). Reading the registers removes an event from the queue:

| Access | Result | Event |
|---|---|---|
| byte `$FF20` (`LDZX R0, [$FF20]`) | character | dequeued |
| word `$FF20` (`LD R0, [$FF20]`) | character in low byte, scan code in high byte | dequeued |
| byte `$FF21` (`LDZX R0, [$FF21]`) | scan code | dequeued; character discarded |

An empty queue produces 0. There is no ready-status register; programs wait by polling or using timer interrupts.

- The **character** depends on keyboard layout and Shift: pressing the S key may produce `s`, `S`, `ы`, or `Ы`. The scan code is independent of the layout.
- Special characters:
  - Enter: `$0A` (`\n`, as in the console stream).
  - Ctrl+Enter: character 0 with Enter scan code `$1C`, treated like a non-character key such as an arrow key. A program reading a word can therefore distinguish Ctrl+Enter from Enter. Ctrl+Enter neither terminates a line nor appears in the character stream.
  - Backspace: `$08`; Tab: `$09`; Esc: `$1B`.
  - Ctrl plus a letter: `$01..$1A`.
- Keys without a character (arrows, F1–F12, Home, etc.) have character 0.
- If the key's character is not representable in the machine encoding, the character is 0 but the scan code is retained.
- Modifier keys (Shift, Ctrl, Alt) do not generate separate events. Holding a key repeats its event; key releases are not reported.
- **Event modifiers** are stored with each event. The MODS register (`$FF3C`) reports the modifiers of the event most recently dequeued by reading DATA or SCAN; reading MODS does not dequeue an event. MODS remains unchanged until another event is dequeued. Reading from an empty queue resets MODS to 0.

  | Bit | Modifier |
  |---|---|
  | 0 | Shift |
  | 1 | Ctrl |
  | 2 | Alt |
  | 3–7 | 0 |

  This lets programs distinguish Shift+Arrow from Arrow and Shift+F4 from F4: for keys without a character, the modifiers are available only through MODS. Byte `$FF3D` reads as 0; a word read at `$FF3C` returns MODS in the low byte. Writes to MODS have no effect.

  ```asm
          LD      R0, [$FF20]             ; event: character | scan code << 8
          LDZX    R1, [$FF3C]             ; modifiers for that event
  ```

  Key combinations intercepted by the host (browser or terminal) never reach the machine. See Sections 9.2 and 9.3 for the modifiers delivered by each emulator.
- The queue holds up to 256 events; excess events are discarded.
- A zero character byte cannot be distinguished from "no event" by reading the character alone. Programs that need special keys, including Ctrl+Enter, must read a word.

The console remains fully backward compatible: writing a byte outputs a character in teletype mode, and reading a byte returns a character or zero.

### 2.7. Scan Codes

Scan codes follow the standard keyboard make-code set 1.

| Key | Scan code | Key | Scan code |
|---|---|---|---|
| Esc | `$01` | F1..F10 | `$3B..$44` |
| 1..9, 0 | `$02..$0B` | F11, F12 | `$57`, `$58` |
| `-` `=` | `$0C`, `$0D` | Home | `$47` |
| Backspace | `$0E` | ↑ | `$48` |
| Tab | `$0F` | PgUp | `$49` |
| Q W E R T Y U I O P | `$10..$19` | ← | `$4B` |
| `[` `]` | `$1A`, `$1B` | → | `$4D` |
| Enter | `$1C` | End | `$4F` |
| A S D F G H J K L | `$1E..$26` | ↓ | `$50` |
| `;` `'` `` ` `` | `$27..$29` | PgDn | `$51` |
| `\` | `$2B` | Ins | `$52` |
| Z X C V B N M | `$2C..$32` | Del | `$53` |
| `,` `.` `/` | `$33..$35` | Space | `$39` |

Keys on the numeric keypad generate the same scan codes as the corresponding main-keyboard keys with the same action.

## 3. Timer

| Address | Name | Read | Write |
|---|---|---|---|
| `$FF29` | TIMER | bit 0: timer enabled | bit 0: 1 = enable, 0 = disable |

The timer is disabled after reset.

While enabled, the timer raises an **IRQ0** request every **20 milliseconds of real time** (50 Hz). The general processor interrupt rules then apply (processor specification, Section 8): a request is accepted when IF = 1 and wakes `WFI`.

- **Real-time interval.** The number of instructions executed in 20 ms depends on emulator and host speed. Programs that depend on the rate of events (such as animations or games) must count timer ticks rather than instructions.
- **Ticks do not accumulate.** IRQ0 has a single pending-request flag. If an earlier request remains unhandled (IF = 0 or the handler takes too long), a subsequent tick coalesces with it.
- **Enable sequence:** initialize the IRQ0 vector (and the rest of the vector table), enable the timer, then execute `STI`.
- **`WFI` with an enabled timer waits; it does not halt permanently.** The next tick wakes the processor.
- **Pending IRQ0.** Disabling the timer does not clear an IRQ0 request that has already been raised but not yet accepted. With IF = 0, such a request wakes every `WFI` without entering the handler. Therefore, "disable timer, `CLI`, `WFI`" does not stop the machine: `WFI` immediately wakes, and the program continues looping through `WFI` / `JMP`.
- **Stopping a program:** disable the timer, then execute `STI` followed by `CLI` so that a pending IRQ0 can be accepted and cleared by its handler. Only then execute `WFI`. With no outstanding requests and the timer disabled, `WFI` halts the processor. The IRQ0 vector must point to a handler that returns via `IRET` (`on_tick` in the example; an empty handler containing only `IRET` also works). The emulator considers a program finished when the processor is in `WFI`, no requests are pending, the timer is disabled, and no disk command can subsequently generate IRQ1 (BUSY = 1 with IEN = 1; Section 6.5).

```asm
        .equ    TIMER, $FF29
        LD      R0, #on_tick
        ST      [$FF00 + 2*0], R0       ; IRQ0 vector
        LD      R0, #1
        STB     [TIMER], R0             ; enable timer
        STI
        ...
halt:   LD      R0, #0
        STB     [TIMER], R0             ; disable timer
        STI                             ; accept any pending IRQ0:
        CLI                             ;   handler runs and returns
@stop:  WFI                             ; no requests: processor halts
        JMP     @stop
```

If no IRQ0 was pending, `STI` and `CLI` do not affect this procedure; it is safe in either case.

The IRQ 0 button in the web debugger raises the same request as the timer.

## 4. Register Summary

| Address | Register | Description |
|---|---|---|
| `$FF20` | DATA | write: output character; read: character (byte) or character + scan code (word) |
| `$FF21` | SCAN | read: scan code |
| `$FF22` | CTRL | bit 0: teletype; bit 1: cursor; bit 2: attribute mode |
| `$FF23` | ATTR | current attribute |
| `$FF24` | COL | current column |
| `$FF25` | ROW | current row |
| `$FF26` | CMD | 1: clear screen; 2: to end of row; 3: to end of screen; 4: scroll up; 5: scroll down |
| `$FF27` | COLS | 80 |
| `$FF28` | ROWS | 25 |
| `$FF29` | TIMER | bit 0: 50 Hz IRQ0 timer |
| `$FF2A` | TIME | time (word); clock data register |
| `$FF2C` | DATE | date and PM flag (word); clock data register |
| `$FF2E` | RTC_STATUS / RTC_CMD | read: bit 0 ERR; write: 1 READ, 2 WRITE, 3 WRITE_TIME, 4 WRITE_DATE |
| `$FF30` | STATUS / CMD | read: bit 0 BUSY, bit 1 ERR, bit 2 DONE, bit 3 PRESENT, bit 4 RO; write: 0 ACK, 1 READ, 2 WRITE, 3 INFO, 4 FLUSH |
| `$FF31` | ERROR | disk error code |
| `$FF32` | DISK | disk number 0..7 |
| `$FF33` | COUNT | sector count |
| `$FF34` | ADDR | buffer address (word) |
| `$FF36` | LBA_LO | sector number, low word |
| `$FF38` | LBA_HI | sector number, high word |
| `$FF3A` | CTRL | bit 0: generate IRQ1 on disk command completion |
| `$FF3B` | DRIVES | connected-disk bitmask |
| `$FF3C` | MODS | modifiers of the last dequeued keyboard event: bit 0 Shift, 1 Ctrl, 2 Alt |
| `$FF40–$FF47` | PAGE0..PAGE7 | frame number for pages 0..7 |
| `$FF48` | MMU_CTRL | bit 0: memory manager enabled |
| `$FF50` | SCRD | screen readback: word = current cell character + attribute, then advance position; bytes at `$FF50`, `$FF51` = character and attribute without advancing |

All remaining addresses in `$FF20–$FFFF` are unmapped device locations: reads return 0 and writes are ignored (Section 1).

## 5. Character Encoding

The machine uses a single-byte character encoding:

| Range | Contents |
|---|---|
| `$00` | blank cell (not displayed in teletype mode) |
| `$01–$1F`, `$7F` | symbols such as `☺ ♥ ♦ ♣ ♠`, arrows, triangles, and musical notes |
| `$20–$7E` | ASCII |
| `$80–$BF` | KOI8-R-derived box-drawing characters (single and double lines, blocks, shading), `ё` (`$A3`), `Ё` (`$B3`), `°`, `²`, `·`, `÷`, `©`, and non-breaking space |
| `$C0–$FF` | CP1251-derived Cyrillic letters `А–Я`, `а–я` in alphabetical order |

The Cyrillic letters `А–я` occur in alphabetical order, with uppercase and lowercase forms differing in bit 5 (`$20`), as with Latin letters.

In positioned mode, symbols `$01–$1F` always appear as glyphs. In teletype mode, `$08`, `$09`, `$0A`, and `$0D` control the position, `$00` is ignored, and the remaining bytes display glyphs.

| | x0 | x1 | x2 | x3 | x4 | x5 | x6 | x7 | x8 | x9 | xA | xB | xC | xD | xE | xF |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **0x** | — | &#9786; | &#9787; | &#9829; | &#9830; | &#9827; | &#9824; | &#8226; | &#9688; | &#9675; | &#9689; | &#9794; | &#9792; | &#9834; | &#9835; | &#9788; |
| **1x** | &#9658; | &#9668; | &#8597; | &#8252; | &#182; | &#167; | &#9644; | &#8616; | &#8593; | &#8595; | &#8594; | &#8592; | &#8735; | &#8596; | &#9650; | &#9660; |
| **2x** | SP | &#33; | &#34; | &#35; | &#36; | &#37; | &#38; | &#39; | &#40; | &#41; | &#42; | &#43; | &#44; | &#45; | &#46; | &#47; |
| **3x** | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | &#58; | &#59; | &#60; | &#61; | &#62; | &#63; |
| **4x** | &#64; | A | B | C | D | E | F | G | H | I | J | K | L | M | N | O |
| **5x** | P | Q | R | S | T | U | V | W | X | Y | Z | &#91; | &#92; | &#93; | &#94; | &#95; |
| **6x** | &#96; | a | b | c | d | e | f | g | h | i | j | k | l | m | n | o |
| **7x** | p | q | r | s | t | u | v | w | x | y | z | &#123; | &#124; | &#125; | &#126; | &#8962; |
| **8x** | &#9472; | &#9474; | &#9484; | &#9488; | &#9492; | &#9496; | &#9500; | &#9508; | &#9516; | &#9524; | &#9532; | &#9600; | &#9604; | &#9608; | &#9612; | &#9616; |
| **9x** | &#9617; | &#9618; | &#9619; | &#8992; | &#9632; | &#8729; | &#8730; | &#8776; | &#8804; | &#8805; | NBSP | &#8993; | &#176; | ² | &#183; | &#247; |
| **Ax** | &#9552; | &#9553; | &#9554; | ё | &#9555; | &#9556; | &#9557; | &#9558; | &#9559; | &#9560; | &#9561; | &#9562; | &#9563; | &#9564; | &#9565; | &#9566; |
| **Bx** | &#9567; | &#9568; | &#9569; | Ё | &#9570; | &#9571; | &#9572; | &#9573; | &#9574; | &#9575; | &#9576; | &#9577; | &#9578; | &#9579; | &#9580; | &#169; |
| **Cx** | А | Б | В | Г | Д | Е | Ж | З | И | Й | К | Л | М | Н | О | П |
| **Dx** | Р | С | Т | У | Ф | Х | Ц | Ч | Ш | Щ | Ъ | Ы | Ь | Э | Ю | Я |
| **Ex** | а | б | в | г | д | е | ж | з | и | й | к | л | м | н | о | п |
| **Fx** | р | с | т | у | ф | х | ц | ч | ш | щ | ъ | ы | ь | э | ю | я |

Characters **not** available in this encoding (unlike CP1251) include quotation marks `« »`, `„ “ ”`, dashes `– —`, `№`, `€`, `™`, and letters specific to the Ukrainian, Belarusian, and Serbian alphabets.

### 5.1. Strings in Source Files

Source files use UTF-8. The assembler converts string literals (`.ascii`, `.asciz`) and character literals (`'Ё'`) to the machine encoding. A character that cannot be represented in this encoding causes an assembly error. Escape sequences (`\n`, `\x41`) specify byte values directly without character conversion.

### 5.2. Encoding in the Command-Line Emulator

- The emulator converts bytes output in teletype mode to UTF-8 for the terminal, including box-drawing characters and symbols.
- Input read from UTF-8 stdin is converted to the machine encoding. Characters missing from the encoding table are discarded.
- The emulator recognizes terminal key sequences (arrows, Home, End, Del, F1–F12) and converts them to events with a scan code and character 0.

## 6. Disk Controller

The controller supports up to eight disks, numbered 0–7. Each disk is a sequence of **512-byte sectors**. Sector numbers (LBAs) are 32 bits wide, so a disk can contain up to 2^32 sectors.

Disks containing no more than 2^16 sectors are called **small disks**: all their sector numbers fit in 16 bits. The on-disk data structures are defined by software (the file system). The controller does not distinguish small from large disks; to the controller, the difference is only the value in the high LBA word.

Transfers use **direct memory access (DMA)**: the controller copies sectors directly between the disk and memory. The processor supplies command parameters, starts an operation, and waits for it to finish.

### 6.1. Registers

| Address | Name | Read | Write |
|---|---|---|---|
| `$FF30` | STATUS / CMD | status | command (Section 6.3) |
| `$FF31` | ERROR | error code from the last command (Section 6.4) | — |
| `$FF32` | DISK | selected disk number | disk number; only bits 0–2 are used |
| `$FF33` | COUNT | number of sectors remaining | sector count 0..255 |
| `$FF34–$FF35` | ADDR | current memory address | buffer address |
| `$FF36–$FF37` | LBA_LO | current sector, low word | sector number, low word |
| `$FF38–$FF39` | LBA_HI | current sector, high word | sector number, high word |
| `$FF3A` | CTRL | mode | mode |
| `$FF3B` | DRIVES | connected-disk bitmask: bit n represents disk n | — |
| `$FF3D–$FF3F` | — | 0 (reserved) | ignored |

**Word access.** All controller registers can be read or written as words at even addresses: the low byte corresponds to the register at that address and the high byte to the next register. Thus, ADDR, LBA_LO, and LBA_HI can each be set by one `ST` instruction, while DISK and COUNT can be set together:

```asm
        LD      R0, #4 << 8 | 1          ; COUNT = 4, DISK = 1
        ST      [$FF32], R0
```

Reading controller registers has no side effects. Reading a word at `$FF30` returns STATUS in its low byte and ERROR in its high byte. On a word write to `$FF30`, the low byte executes a command; the high byte, corresponding to read-only ERROR, is ignored.

**CTRL:**

| Bit | Name | Meaning |
|---|---|---|
| 0 | IEN | 1: request IRQ1 when a command completes (Section 6.5) |
| 1–7 | — | 0 |

CTRL may be written at any time, including while a command is executing (BUSY = 1). Bits 1–7 are discarded on writes and always read as zero.

**STATUS:**

| Bit | Name | Meaning |
|---|---|---|
| 0 | BUSY | command in progress |
| 1 | ERR | last command ended with an error (ERROR ≠ 0) |
| 2 | DONE | command completed but not acknowledged (Section 6.5) |
| 3 | PRESENT | disk selected by DISK is connected |
| 4 | RO | disk selected by DISK is write-protected |
| 5–7 | — | 0 |

BUSY and ERR occupy the lowest bits so that tests fit in a short immediate constant, for example `TEST R0, #1`.

**Reset state:**

- All registers except DRIVES are zero.
- No command is running; IEN = 0.
- DRIVES reflects the currently connected disks.

Resetting the machine does not disconnect disks.

### 6.2. Command Execution

1. Software fills the parameter registers and writes the command code to CMD.
2. The controller sets BUSY = 1, clears DONE and ERROR, and validates the parameters. If validation fails, the command finishes immediately with an error and transfers no data (see the validation order below).
3. Sectors are transferred one at a time. After each sector, the 32-bit LBA increments by 1, ADDR increments by 512, and COUNT decrements by 1.
4. On completion, BUSY = 0 and DONE = 1. If the command failed, ERR = 1 and ERROR holds the error code.

**Validation order.** Checks are performed in the listed order. The first failed check determines ERROR:

| Step | Check | Error | Commands |
|---|---|---|---|
| 1 | command code is 1..4 | BADCMD | all except ACK |
| 2 | disk selected by DISK is connected | NODISK | READ, WRITE, INFO, FLUSH |
| 3 | COUNT = 0: complete immediately and successfully | — | READ, WRITE |
| 4 | disk is not write-protected | READONLY | WRITE |
| 5 | LBA + COUNT does not exceed disk sector count | RANGE | READ, WRITE |
| 6 | ADDR + 512·COUNT − 1 ≤ `$FF1F` (for INFO, ADDR + 31 ≤ `$FF1F`) | BADADDR | READ, WRITE, INFO |

- Sums in Steps 5 and 6 are calculated without overflow, not modulo 2^32 or 2^16. LBA + COUNT may require 33 bits; ADDR + 512·COUNT − 1 may require 18 bits.
- Step 3 means that READ and WRITE with COUNT = 0 always succeed on a connected disk, even if it is write-protected and regardless of LBA or ADDR.

**Registers advance during transfer.** To read subsequent sectors, software only needs to set COUNT again and repeat the command: LBA and ADDR already point to the next positions. If an I/O error interrupts a transfer, COUNT indicates how many sectors remain. To reread the same sector after a successful command, software must write LBA again.

**Execution time** is implementation-dependent. An emulator may execute commands instantly; a debugger may introduce delays. Programs must wait for BUSY = 0, either by polling STATUS or using IRQ1. DMA transfers occur between processor instructions.

**While BUSY = 1:**

- Writes to CMD (including ACK) and to parameter registers (DISK, COUNT, ADDR, LBA) are ignored; writes to CTRL still take effect.
- Reads of registers report transfer progress.
- Programs must not access the buffer: its contents are undefined during the transfer.

**The DMA buffer** occupies logical addresses ADDR through ADDR + 512·COUNT − 1 and must lie entirely within `$0000–$FF1F`. The disk controller may read and write the interrupt vector table, but not the device window: DMA never accesses `$FF20–$FFFF`.

**DMA and the memory manager.** The buffer address is logical. When the memory manager is enabled (Section 7), every DMA access is translated through the PAGE registers in the same way as a processor access. To transfer data into any physical frame, map that frame into one of the pages. While BUSY = 1, programs must not change MMU_CTRL or any PAGE register covering the buffer: otherwise the physical destination of the data is undefined.

**Loading executable code.** A DMA write is an ordinary data write to memory. If the buffer receives executable code, a taken branch after the transfer completes serves as the synchronization point (processor specification, Section 11), and execution then uses the new code.

### 6.3. Commands

| Code | Command | Action |
|---|---|---|
| 0 | ACK | clear DONE and deassert IRQ1; do not change ERR or ERROR |
| 1 | READ | read COUNT sectors starting at LBA into memory at ADDR |
| 2 | WRITE | write COUNT sectors from memory at ADDR to disk starting at LBA |
| 3 | INFO | write a 32-byte disk-information block into memory at ADDR |
| 4 | FLUSH | commit data from earlier WRITE commands that have not yet reached the underlying medium |
| 5–255 | — | BADCMD error |

- ACK executes immediately without setting BUSY.
- READ and WRITE with COUNT = 0 finish successfully without transferring data.
- INFO and FLUSH use only DISK (INFO also uses ADDR) and do not modify parameter registers.
- ACK is ignored while BUSY = 1, just like any other write to CMD.
- After a successful WRITE, the data is visible to subsequent READ commands. FLUSH is needed only to guarantee persistence on the underlying medium (the disk-image file in an emulator).

**INFO block (32 bytes):**

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | sector count (32-bit little-endian) |
| 4 | 1 | flags: bit 0 RO (write-protected), bit 1 REMOVABLE (disk can be connected or disconnected while running), bit 2 SMALL (small disk: at most 2^16 sectors) |
| 5 | 11 | 0 (reserved) |
| 16 | 16 | disk label: null-terminated string in the machine encoding, up to 15 characters; bytes after the null are `0` |

The SMALL flag eliminates a boundary-case test: on a disk containing exactly 2^16 sectors, the high word of its size is 1, but the disk is still classified as small.

### 6.4. Errors

| Code | Name | Cause |
|---|---|---|
| 0 | OK | no error |
| 1 | NODISK | selected disk is not connected |
| 2 | BADCMD | unknown command |
| 3 | RANGE | LBA + COUNT exceeds disk sector count |
| 4 | BADADDR | buffer extends beyond `$FF1F` (Section 6.2, Step 6) |
| 5 | READONLY | WRITE attempted on a write-protected disk |
| 6 | IO | medium error (in the emulator: disk-image file error or disk disconnected during command) |

Errors 1–5 are detected before transfer begins and leave the parameter registers unchanged. Error 6 may occur during transfer. In that case, registers indicate where the failure occurred: LBA and ADDR point to the sector that could not be transferred, and COUNT includes that sector.

ERR and ERROR remain set until the start of the next command, except ACK, which does not clear them.

### 6.5. Interrupt

The **IRQ1** request remains asserted as long as DONE = 1 and IEN = 1 (a level-triggered request; processor specification, Section 8.2). The interrupt handler clears the request with ACK. Starting a new command also clears DONE. If completion is not acknowledged, the handler will be invoked again after `IRET`.

Timer IRQ0 has higher priority.

A disk command can finish immediately after CMD is written, before the next processor instruction. This makes `STI` followed by `WFI` unsafe: the interrupt might be serviced between those instructions, leaving `WFI` waiting indefinitely. Instead, wait with IF = 0. A pending request then wakes `WFI` without entering the interrupt handler; the subsequent `STI` allows the handler to run immediately (Section 6.6, second example).

### 6.6. Examples

A small-disk driver using polling:

```asm
        .equ    DK_CMD,    $FF30
        .equ    DK_STATUS, $FF30
        .equ    DK_ERROR,  $FF31
        .equ    DK_DISK,   $FF32
        .equ    DK_COUNT,  $FF33
        .equ    DK_ADDR,   $FF34
        .equ    DK_LBA_LO, $FF36
        .equ    DK_LBA_HI, $FF38
        .equ    DK_CTRL,   $FF3A
        .equ    DK_DRIVES, $FF3B
        .equ    DK_ACK,    0
        .equ    DK_READ,   1
        .equ    DK_WRITE,  2
        .equ    DK_INFO,   3
        .equ    DK_FLUSH,  4
        .equ    DK_BUSY,   $01

; Input: R0 = disk, R1 = LBA, R2 = buffer address, R3 = sector count
; Output: R0 = error code, ZF = 1 on success.
; R4 is used and preserved on the stack.
disk_read:
        PUSH    R4
        LD      R4, #DK_READ
        JMP     disk_io
disk_write:
        PUSH    R4
        LD      R4, #DK_WRITE
disk_io:                                ; R4 = command; saved R4 on the stack
        STB     [DK_DISK], R0
        STB     [DK_COUNT], R3
        ST      [DK_ADDR], R2
        ST      [DK_LBA_LO], R1
        LD      R0, #0
        ST      [DK_LBA_HI], R0          ; small disk: high word is zero
        STB     [DK_CMD], R4
@wait:  LDZX    R0, [DK_STATUS]
        TEST    R0, #DK_BUSY
        JNZ     @wait
        LDZX    R0, [DK_ERROR]
        POP     R4
        CMP     R0, #0
        RET
```

The examples follow the calling convention: arguments and results use R0–R3, which are not callee-saved; a subroutine must preserve the other registers.

The driver always sets LBA_HI. The registers advance during transfers, and after the final sector of a 2^16-sector disk is read, LBA_HI becomes 1. Its previous value therefore cannot be assumed to be zero.

**Race-free IRQ1 wait.** The IRQ1 vector is initialized, IEN = 1, and parameters have been set:

```asm
; Input: R1 = command code. Clobbers R0.
disk_start_wait:
        CLI
        LD      R0, #0
        STB     [dk_done], R0
        STB     [DK_CMD], R1            ; start command
@wait:  LDZX    R0, [dk_done]
        CMP     R0, #0
        JNZ     @done
        WFI                             ; IF = 0: wake on request without entering handler
        STI                             ; request accepted immediately after STI
        CLI
        JMP     @wait
@done:  STI
        RET

disk_irq:                               ; IRQ1 vector: ST [$FF00 + 2*1], ...
        PUSH    R0
        LD      R0, #DK_ACK
        STB     [DK_CMD], R0            ; deassert request
        LD      R0, #1
        STB     [dk_done], R0
        POP     R0
        IRET

dk_done: .byte  0
```

Timer ticks may also wake `WFI` while the program waits. The loop simply checks the flag again.

The preceding version is for callers that normally run with IF = 1; it returns with interrupts enabled by `STI`. Code such as the Monitor kernel, which enters the wait with IF = 0, must also return with IF = 0. The following version therefore has no final `STI`. It assumes IRQ1 is installed, IEN = 1, the command parameters are set, and IF = 0:

```asm
; Input: R1 = command code. Clobbers R0. IF = 0 on entry and return.
disk_start_wait0:
        LD      R0, #0
        STB     [dk_done], R0
        STB     [DK_CMD], R1            ; start command
@wait:  LDZX    R0, [dk_done]
        CMP     R0, #0
        JNZ     @done
        WFI                             ; IF = 0: wake on request without entering handler
        STI                             ; request accepted immediately after STI
        CLI                             ; restore IF = 0
        JMP     @wait
@done:  RET                             ; IF = 0, as on entry
```

The `disk_irq` handler is unchanged from the previous example. Checking the completion flag before `WFI` ensures that a command completed immediately after CMD was written is not missed: the loop simply exits. A request already pending when `WFI` is reached also wakes it immediately, without invoking the handler while IF = 0. No initial `CLI` is needed because IF is already 0. A caller that might enter with IF = 1 must save FLAGS (`PUSH FLAGS`), clear IF with `CLI`, and restore FLAGS (`POP FLAGS`) afterward.

## 7. Memory Manager

The memory management unit (MMU) expands physical memory to **2 MiB**. It sits between the processor and memory. The processor itself is unchanged: it continues to use 16-bit **logical** addresses, which the MMU translates into 21-bit **physical** addresses.

### 7.1. Pages and Frames

The logical address space is divided into eight **pages** of 8 KiB each; physical memory comprises 256 **frames** of 8 KiB each. Each logical page is associated with a PAGEp register containing its physical frame number.

| Logical addresses | Page | Register |
|---|---|---|
| `$0000–$1FFF` | 0 | PAGE0 |
| `$2000–$3FFF` | 1 | PAGE1 |
| `$4000–$5FFF` | 2 | PAGE2 |
| `$6000–$7FFF` | 3 | PAGE3 |
| `$8000–$9FFF` | 4 | PAGE4 |
| `$A000–$BFFF` | 5 | PAGE5 |
| `$C000–$DFFF` | 6 | PAGE6 |
| `$E000–$FFFF` | 7 | PAGE7 |

```text
logical address:  | p:3     | offset:13 |
physical address: | PAGEp:8 | offset:13 |     21 bits
```

For example, with PAGE6 = 20, logical address `$C010` maps to physical address 20 · `$2000` + `$0010` = `$28010`.

- The same physical frame may be mapped into several logical pages simultaneously.
- A two-word instruction may straddle a page boundary: each instruction word is translated according to its own page.
- Words are aligned, so a single word cannot cross a page boundary.

### 7.2. Registers

| Address | Name | Read | Write |
|---|---|---|---|
| `$FF40` + p | PAGEp | frame number of page p | frame number 0..255 |
| `$FF48` | MMU_CTRL | mode | mode |
| `$FF49–$FF4F` | — | 0 (reserved for memory-manager extensions) | ignored |

**MMU_CTRL:**

| Bit | Name | Meaning |
|---|---|---|
| 0 | EN | 1: enabled; 0: disabled |
| 1–7 | — | 0; discarded on writes |

- Registers support byte access and word access at even addresses (Section 1): `ST [$FF40], R0` sets PAGE0 and PAGE1 together.
- Reads have no side effects.
- PAGE registers can be read and written regardless of EN.

**Reset state:**

- EN = 0: the memory manager is disabled.
- PAGE0..PAGE7 values are **undefined** (the emulators initialize them to zero, Section 9, but programs must not rely on this).

### 7.3. Operating Modes

**Disabled (EN = 0).** Physical addresses equal logical addresses: the processor sees the first 64 KiB of physical memory, frames 0–7. PAGE registers have no effect on memory accesses and may be configured in advance. This mode is identical to the Revision 2 machine. Programs that do not need the MMU need not be aware of it.

**Enabled (EN = 1).** An access to logical page p goes to frame PAGEp (Section 7.1).

**Programs must initialize all eight PAGE registers before enabling the MMU.** Their post-reset values are undefined, and enabling the MMU does not initialize them. The program enabling address translation is responsible for establishing a valid map.

Disabling the MMU (writing EN = 0) is permitted and restores identity mapping.

Disk-controller DMA follows the same translation rules as processor accesses (Section 6.2).

### 7.4. When Changes Take Effect

- **Data accesses.** Writes to PAGEp or MMU_CTRL affect accesses beginning with the next instruction. The instruction performing the write, including any second instruction word, executes using the previous mapping.
- **Instruction fetches.** Changing a mapping is equivalent to changing code (processor specification, Section 11). The new mapping is guaranteed to take effect after a synchronization point: a taken branch, `CALL`, `RET`, `TRAP`, `IRET`, or interrupt entry. Before such a point, each instruction word fetched from an affected page may come from either the old or the new mapping.
- **Safe map changes.** The page containing code that performs a mapping change must refer to the same physical frame both before and after the change.
  - When enabling the MMU, this means PAGEp = p for the page p containing the enable code.
  - To replace the physical frame underlying the currently executing page, first branch to trampoline code in another page.

### 7.5. Addresses Not Translated

**The device window `$FF20–$FFFF`** is decoded using the logical address before translation and belongs entirely to devices (Section 1):

- Devices, including MMU registers, remain visible for all values of EN and PAGE7.
- Unused device addresses return zero for any mapping; they do not access the frame assigned to page 7.
- The bytes at offsets `$1F20–$1FFF` in the frame assigned to page 7 cannot be accessed through page 7 by either the processor or DMA. They remain accessible as ordinary memory when that frame is mapped into another page.
- With EN = 0, frame 7 occupies page 7, so physical addresses `$0FF20–$0FFFF` cannot be accessed at all. Accessing them requires enabling the MMU and mapping frame 7 into another logical page.

**The interrupt vector table `$FF00–$FF1F`** is ordinary memory translated through PAGE7. The vectors, handler code, and handler stack must remain mapped whenever an interrupt may be accepted. ERRCMD cannot be masked, so vector 8 must always be valid.

A common page 7, containing vectors, the stack, and kernel data and mapped to the same frame in all address maps, is recommended.

**An interrupt does not change the address map.** A handler that changes the map must restore it before returning. The whole map can be read as four words:

```asm
        .equ    MMU_PAGE, $FF40

        LD      R0, [MMU_PAGE+0]         ; PAGE0, PAGE1
        PUSH    R0
        LD      R0, [MMU_PAGE+2]         ; PAGE2, PAGE3
        PUSH    R0
        LD      R0, [MMU_PAGE+4]         ; PAGE4, PAGE5
        PUSH    R0
        LD      R0, [MMU_PAGE+6]         ; PAGE6, PAGE7
        PUSH    R0
```

### 7.6. Physical Memory

- Physical memory spans 2 MiB, frames 0–255.
- With EN = 0, physical addresses `$00000–$0FFFF` form the 64 KiB address space of Revision 2. The processor can access `$00000–$0FF1F`; the remainder is hidden by the device window (Section 7.5).
  - In a hardware implementation, following the *Introduction* recommendations, frames 0–3 (`$0000–$7FFF`) may be ROM containing bootstrap code, with frames 4–7 implemented as RAM.
  - The composition of the remaining frames is implementation-defined.
- All 2 MiB are RAM in the emulators.
- There is no memory protection: the MMU only remaps addresses.

### 7.7. Boot and Enable Sequence

A proposed boot sequence for hardware implementations:

1. **Reset.** IP = 0, MMU disabled. The processor starts executing bootstrap code from ROM.
2. **ROM code:**
   - initializes SP;
   - installs interrupt vectors, starting with ERRCMD (processor specification, Section 9);
   - loads the bootloader from disk into RAM;
   - transfers control to the bootloader.
3. **Bootloader:**
   - initializes all PAGE registers;
   - enables the MMU;
   - loads the operating system into the required physical frames and jumps to it.

A ROM program that does not require the MMU does not enable it.

**Enable example.** The bootloader executes in page 4; the stack and vectors are in page 7. Pages 0–3 are mapped to RAM instead of ROM, while pages 4–7 retain identity mappings:

```asm
        .equ    MMU_PAGE, $FF40
        .equ    MMU_CTRL, $FF48
        .equ    MMU_EN,   $01

; Clobbers R0.
mmu_init:
        LD      R0, #9 << 8 | 8          ; PAGE0 = 8, PAGE1 = 9
        ST      [MMU_PAGE+0], R0
        LD      R0, #11 << 8 | 10        ; PAGE2 = 10, PAGE3 = 11
        ST      [MMU_PAGE+2], R0
        LD      R0, #5 << 8 | 4          ; PAGE4 = 4 (bootloader code), PAGE5 = 5
        ST      [MMU_PAGE+4], R0
        LD      R0, #7 << 8 | 6          ; PAGE6 = 6, PAGE7 = 7 (vectors, stack)
        ST      [MMU_PAGE+6], R0
        LD      R0, #MMU_EN
        STB     [MMU_CTRL], R0           ; MMU enabled
        RET                              ; taken branch: synchronization point
```

ROM remains accessible after enabling the MMU: simply map one of its frames into a logical page.

## 8. Real-Time Clock

The real-time clock (RTC) maintains local date and time to the nearest second and runs independently of the processor. Its value uses the FS16 file-system timestamp format (*FS16 File System*, Section 5.1), so it can be stored directly in a directory entry's MTIME field.

Programs do not access the clock state directly. TIME and DATE serve as transfer registers: READ copies the current clock state into them, while write commands transfer their values into the clock.

### 8.1. Value Format

The value occupies 32 bits in little-endian order: the low word TIME contains time of day; the high word DATE contains the date and PM flag.

```text
TIME:   15      12  11            6  5             0
      | hour 0..11 | minute 0..59  | second 0..59     |

DATE:   15          10  9       6  5        1    0
      | year − 2025   | month    | day        | PM |
```

- **Time uses a 12-hour clock.** Hour 0 means 12 o'clock: midnight is AM, hour 0; noon is PM, hour 0; 23:59:59 is PM, 11:59:59. The hour in 24-hour notation is `hour + 12 · PM`.
- **The PM flag** refers to the time of day but resides in the least significant bit of DATE.
- **Range:** January 1, 2025, AM 0:00:00 through December 31, 2088, PM 11:59:59.
- **Comparison:** fields are ordered from most significant to least significant, so timestamps can be compared as unsigned 32-bit integers.
- **Local time:** neither time-zone offset nor daylight-saving status is stored.

**Valid time:** hour 0..11, minute and second 0..59, either PM value. **Valid date:** any year value (0..63), month 1..12, day 1 through the number of days in that month. Leap-year February has 29 days; within 2025–2088, years divisible by four are leap years. A timestamp is valid only if both its time and date are valid.

The clock state is always valid (Sections 8.3 and 8.4). In particular, it never reports zero: FS16 reserves that timestamp value for "time unknown", and month 0 is invalid.

### 8.2. Registers

| Address | Name | Read | Write |
|---|---|---|---|
| `$FF2A–$FF2B` | TIME | time | time |
| `$FF2C–$FF2D` | DATE | date and PM flag | date and PM flag |
| `$FF2E` | STATUS / CMD | status | command (Section 8.3) |

- **TIME and DATE are data registers.** Reads and writes have no side effects and do not alter the clock itself; only commands transfer values between registers and clock state. Values are stored verbatim, without validation.
- **Word access.** TIME and DATE support byte access and word access at `$FF2A` and `$FF2C` (Section 1). CMD is byte-access only.

**STATUS:**

| Bit | Name | Meaning |
|---|---|---|
| 0 | ERR | last write command rejected: register values were invalid |
| 1–7 | — | 0 |

**Reset state:**

- TIME = 0, DATE = 0, ERR = 0.
- Reset does not affect the clock's value or progression (Section 8.4).

### 8.3. Commands

| Code | Command | Action |
|---|---|---|
| 1 | READ | copy the current clock state into TIME and DATE |
| 2 | WRITE | set date and time: time of day from TIME and DATE.PM, date from DATE |
| 3 | WRITE_TIME | set time of day only, from TIME and DATE.PM; preserve date |
| 4 | WRITE_DATE | set date only, from DATE's year, month, and day (ignore PM); preserve time of day |
| 0, 5–255 | — | no effect |

- **Immediate execution.** The command runs when CMD is written, and its result is visible to the very next processor instruction. No completion wait is required.
- **READ** copies all four bytes atomically. Thus TIME and DATE correspond to the same instant, even if midnight passes between the program's reads of the two words. READ does not change ERR.
- **Validation on writes.** A write command validates exactly the fields being set: WRITE validates the entire value, WRITE_TIME only the time of day, and WRITE_DATE only the date (Section 8.1).
  - Valid value: update the clock and clear ERR.
  - Invalid value: **leave the clock unchanged** and running at its previous setting; set ERR = 1.
- **Fractional seconds.** WRITE and WRITE_TIME reset the fractional second: the next second begins exactly one second after the command. WRITE_DATE leaves the fractional second intact and does not disturb second transitions.
- Write commands and unknown command codes do not change TIME or DATE; unknown codes also leave ERR unchanged.

**Usage sequence:**

- To read: issue READ, then read TIME and DATE.
- To set: write TIME and DATE, issue WRITE, then inspect ERR.
- To set only time or date: write the required register and issue WRITE_TIME or WRITE_DATE. For WRITE_TIME, DATE must contain the desired PM bit; the simplest approach is READ followed by changes to only the required fields.

### 8.4. Clock Progression

- Once per second of real time, the value advances by one second with carry propagation: AM 11:59:59 → PM 0:00:00 (noon); PM 11:59:59 → AM 0:00:00 of the following day; last day of a month → first day of the next. Month lengths and leap years are accounted for.
- **The clock wraps around:** December 31, 2088, PM 11:59:59 is followed by January 1, 2025, AM 0:00:00. Comparing timestamps recorded on opposite sides of that wrap gives an incorrect chronological order.
- **Initial value is unspecified.** On power-up, the clock may contain any valid date and time and immediately begins advancing. There is no "clock not set" flag. Programs needing accurate real-world time must obtain it from a user or another source and issue WRITE.
- **Machine reset** does not alter the clock value or its progression.
- Timekeeping accuracy is implementation-dependent.
- The RTC does not raise interrupts. Programs can detect a new second by polling, for example in a timer handler.

**Interrupt safety.** TIME, DATE, and RTC ERR are shared registers. If an interrupt handler uses the RTC, the main program must interact with it with IF = 0: from READ until both registers have been read, and from writing the registers until ERR has been checked. Otherwise a handler running in between can overwrite those registers.

### 8.5. Examples

Reading and setting the clock. R0 (time) and R1 (date) use the MTIME format: R0 is stored at offset +36 of a directory entry and R1 at offset +38.

```asm
        .equ    RTC_TIME,       $FF2A
        .equ    RTC_DATE,       $FF2C
        .equ    RTC_CMD,        $FF2E
        .equ    RTC_STATUS,     $FF2E
        .equ    RTC_READ,       1
        .equ    RTC_WRITE,      2
        .equ    RTC_WRITE_TIME, 3
        .equ    RTC_WRITE_DATE, 4
        .equ    RTC_ERR,        $01

; Output: R0 = time, R1 = date.
rtc_get:
        PUSH    FLAGS                   ; critical section: READ and register reads
        CLI                             ;   without a handler running between them
        LD      R0, #RTC_READ
        STB     [RTC_CMD], R0           ; clock state -> TIME, DATE
        LD      R0, [RTC_TIME]
        LD      R1, [RTC_DATE]
        POP     FLAGS
        RET

; Input: R0 = time, R1 = date.
; Output: R0 = 0 and ZF = 1: clock updated;
;         R0 = 1: invalid value; clock left unchanged.
rtc_set:
        PUSH    FLAGS
        CLI
        ST      [RTC_TIME], R0
        ST      [RTC_DATE], R1
        LD      R0, #RTC_WRITE
        STB     [RTC_CMD], R0
        LDZX    R0, [RTC_STATUS]        ; bit 0 = ERR; all other bits 0
        POP     FLAGS
        CMP     R0, #0
        RET
```

If interrupt handlers do not access the RTC, `PUSH FLAGS`, `CLI`, and `POP FLAGS` are unnecessary.

Converting an hour to 24-hour notation:

```asm
; Input: R0 = time, R1 = date. Output: R0 = hour 0..23.
rtc_hour24:
        SHR     R0, #12                 ; hour 0..11
        TEST    R1, #1                  ; PM?
        JZ      @am
        ADD     R0, #12
@am:    RET
```

## 9. Emulators

### 9.1. Behavior Shared by Both Emulators

The command-line emulator and web debugger implement the same machine. Their agreement is tested by comparing execution of randomized programs. The table below specifies implementation choices left open by the machine specification and adopted by both emulators.

| Item | Implementation |
|---|---|
| Memory | 2 MiB of RAM, initialized to zero on startup |
| Memory manager after reset | EN = 0, PAGE0..PAGE7 = 0 |
| Unused device-window addresses | reads return 0; writes ignored (Section 1) |
| Word accesses to the device window other than those allowed in Section 1 | implemented as two byte accesses, first at A, then at A + 1. For example, a word write to `$FF20` outputs the low byte; the high byte is written to SCAN and discarded |
| Modified executable code and changes to address translation | effective immediately, beginning with the next instruction-word fetch (also permitted by the processor specification, Section 11) |
| Constrained-unpredictable instruction encodings | ERRCMD (processor specification, Section 6.3) |
| Device polling | Timer ticks and progress of delayed disk commands are checked between processor instructions once every 256 instructions and whenever execution waits in `WFI`; IRQ0 and delayed disk completion may therefore become visible up to 256 instructions late |
| Disk commands without a delay | execute completely when CMD is written; DONE and IRQ1 are visible to the next processor instruction |
| Disk label | filename without directory, first 15 characters; characters not representable in the machine encoding and control characters are replaced with `?` |
| RTC initial value | host local time at startup, or January 1, 2025, AM 0:00:00 if the host time falls outside 2025–2088; not persisted across runs |
| RTC progression | advances uniformly using a monotonic host clock from startup or the most recent setting; changes to the host clock and daylight-saving transitions do not affect machine time |
| RTC READ | value is calculated when CMD is written; the RTC needs no polling between instructions |
| RTC on machine reset | continues advancing; TIME, DATE, and ERR are reset to 0 |

### 9.2. `cpu16` Command-Line Emulator

| Item | Implementation |
|---|---|
| Teletype output | written to stdout in UTF-8 |
| Positioned output | not written to stdout; `--screen` prints the screen contents when execution finishes |
| Input | Non-blocking stdin: reads return 0 when no input is available. On Unix terminals, a line normally arrives only after Enter. A terminal usually transmits Ctrl+Enter as Enter, which the emulator cannot distinguish. When a terminal sends `ESC [13;5u` (CSI u) or `ESC [27;5;13~` (xterm modifyOtherKeys) for Ctrl+Enter, the emulator produces an event with scan code `$1C`, character 0, and MODS = Ctrl. Other modifier states are extracted from xterm sequences: `ESC [1;mX` instead of `ESC [X` and `ESC O X` (arrows, Home, End, F1–F4), or `ESC [n;m~` instead of `ESC [n~` (PgUp, PgDn, Ins, Del, F5–F12), where m − 1 is the sum of Shift = 1, Alt = 2, and Ctrl = 4. Terminals send these sequences for modified keys without characters. Character input does not carry modifiers: MODS = 0, even for Ctrl+letter (which remains distinguishable by its character `$01..$1A`). On Windows, console keystrokes are read immediately, one at a time: Enter produces `$0A`, Ctrl+Enter produces scan code `$1C` with character 0 (Section 2.6), arrows, Home, End, PgUp, PgDn, Ins, Del, and F1–F12 produce events with scan codes and character 0, and letters arrive as Unicode, independently of the console code page. Windows modifiers are derived from console key codes for Ctrl-, Shift-, and Alt-modified F1–F12, and Ctrl-modified arrows, Home, End, PgUp, PgDn, Ins, and Del. Shift with other non-character keys is determined from the Shift key state (`GetKeyState`) |
| Timer | real 20 ms intervals; during `WFI`, the emulator sleeps until the next tick |
| Disks | `--disk N=file[:ro]`, N = 0..7; may be specified repeatedly (details below) |
| Disk commands | execute immediately when CMD is written; software never observes BUSY asserted |
| Termination | processor in `WFI`, no pending requests, timer disabled, and no disk command capable of generating IRQ1 (Section 3) |
| Program image | flat binary file of at most 64 KiB, loaded at the address specified by `-l` (default 0). Since the MMU is disabled after reset, this is both the logical and physical address. Image bytes loaded into `$FF20–$FFFF` are not visible to the processor |
| RTC | `--rtc DD.MM.YYYYTHH:MM:SS` sets the initial time instead of using host time (24-hour notation, converted to the format in Section 8.1). An invalid date or year outside 2025–2088 is a startup error. Useful for reproducible runs and emulator comparisons |
| Other options | `-t`: trace execution; `-n N`: execute at most N instructions; `--strict-align`: warn about odd-address word accesses and branches; `--selftest`: run built-in tests |

**Command-line disk images:**

```text
./cpu16 --disk 0=sys.img --disk 1=data.img:ro prog.bin
```

- **Image:** a flat file of sectors. Sector n begins at offset 512·n. The file size must be divisible by 512; otherwise startup fails. The number of sectors equals file size divided by 512.
- **`:ro` suffix:** write-protects the disk. A file that cannot be opened for writing is also attached read-only.
- **Label:** follows the rule in Section 9.1.
- **Writes:** modify the image file. FLUSH and emulator shutdown flush data to the host storage medium.
- The REMOVABLE flag is 0 for command-line disks.
- **Startup errors:** disk number outside 0..7, duplicate disk number, unreadable image, or file size not divisible by 512. The emulator reports the error without starting the program.

### 9.3. Web Debugger

| Item | Implementation |
|---|---|
| Console (bottom panel) | scrollback log of teletype output; accepts keyboard input when focused |
| Display | The **Display** button next to the console opens a separate browser window, or a full-height panel on the right if popup windows are blocked (for example in an embedded chat viewer). Both support fullscreen mode |
| Program draws while the display is closed | the Display button becomes highlighted |
| Keys when display is focused | all passed to the program except Pause/Break, which pauses the debugger |
| Ctrl+Enter | in either console or display focus, produces scan code `$1C` with character 0 (Section 2.6); default browser action is suppressed |
| Ctrl+[ and Ctrl+] | in either console or display focus, produce scan codes `$1A` and `$1B`, character 0, and MODS = Ctrl (the Monitor uses them to switch sessions); other Ctrl combinations in console focus remain available to the browser and debugger |
| Modifiers | MODS reflects Shift, Ctrl, and Alt at keydown (`KeyboardEvent`). Combinations not delivered to the page by the browser (such as Ctrl+T, Ctrl+W, Ctrl+N) do not reach the program |
| Timer | real 20 ms intervals while the program runs; stops when paused or single-stepping, and missed ticks do not accumulate |
| Disks | **Disks** tab next to the console: attach an image from the computer (or drop an `.img` or `.dsk` file onto the page), create an empty image with a specified sector count, download an image, enable or disable write protection, and disconnect disks; the current controller registers are also displayed |
| Hot-plugging | permitted; DRIVES updates immediately. All debugger disks have REMOVABLE = 1. Disconnecting a disk during a command ends it with IO error |
| Disk command latency | 1 ms of real time per sector (INFO and FLUSH take 1 ms total); configurable via **Time per sector** in the Disks tab. A value of 0 makes commands execute on CMD writes, as in the command-line emulator. Validation errors (Section 6.2) finish immediately regardless of latency |
| DMA while paused | transfers stop during pauses and single-stepping; when execution resumes, the elapsed time for the current sector starts anew |
| `WFI` waiting for disk | with a command in progress and IEN = 1, `WFI` is a wait, not a halt; the status line reads "Running (WFI, disk)" |
| Image persistence | images are held in page memory; changes are saved only by downloading the image. Modified images are labeled "modified"; FLUSH always succeeds |
| Memory manager | **Memory manager** section in the register panel: EN, PAGE0..PAGE7 (double-click to edit), and physical page addresses |
| RTC | **Clock** section in the register panel: clock date and time (24-hour format), TIME, DATE, and ERR. Double-click to set the clock; invalid values are rejected. **Host time** sets the clock to host local time. The clock keeps running while paused and single-stepping: unlike the timer, it represents elapsed time rather than counted ticks |
| Memory viewer | views memory using either logical or 21-bit physical addresses via the **Logical / Physical** button. In logical mode, the device window displays current register values without side effects (keyboard events are not dequeued) and cannot be edited. The disassembler and breakpoints operate on logical addresses |
| Reset and rebuild | reset processor, devices, disk-controller registers, MMU, and RTC registers; attached disks and the running clock state remain |

In fullscreen mode, the browser intercepts Esc to leave fullscreen. Programs should not make Esc their only way to exit.

## 10. Revision History

### Revision 9

- **SCRD register** (`$FF50–$FF51`, Section 2.2): reads the character and attribute of the screen cell at the current position; a word read advances the position just like a write in positioned mode. This allows Monitor to save the screen of a session. Existing programs are unaffected: the addresses were unused and previously returned zero.
- **Web debugger** (Section 9.3): with console focus, Ctrl+[ and Ctrl+] are now also delivered to programs (previously Ctrl+Enter was the only delivered Ctrl combination).

Section references in the description of each revision use that revision's section numbering.

### Revision 8

- **MODS register** (`$FF3C`, Sections 1, 2.2, 2.6, and 4; the disk-controller reserve is now `$FF3D–$FF3F`): reports Shift, Ctrl, and Alt modifiers of the event most recently dequeued by the program. Previously no modifiers were reported, so Shift+Arrow was indistinguishable from Arrow.
- **Emulators** (Sections 9.2 and 9.3): the web debugger provides modifiers for every key; the command-line emulator obtains them from modified xterm sequences and Windows console codes. Ctrl+Enter sets MODS = Ctrl.
- **Full backward compatibility:** event format and all other registers remain unchanged; `$FF3C` previously returned zero.

### Revision 7

- **Ctrl+Enter** (Sections 2.6, 9.2, and 9.3): previously produced the same event as Enter (`$0A`, `$1C`) and was indistinguishable to programs. It now produces a non-character event with character 0 and scan code `$1C`. The command-line emulator distinguishes Ctrl+Enter on the Windows console and on terminals that transmit `ESC [13;5u` or `ESC [27;5;13~`; other Unix terminals pass Ctrl+Enter as Enter.
- **Compatibility:** programs reading byte `$FF20` no longer see Ctrl+Enter; previously it terminated a line like Enter. Other keys are unaffected.

### Revision 6

These changes were prompted by the Prototype B report (Section 6, items 1 and 4). Section references use this revision's numbering; section numbering itself did not change.

- **Program halt procedure** (Section 3): the sequence "disable timer, `CLI`, `WFI`" was not guaranteed to stop the machine. An IRQ0 raised before disabling the timer remains pending and, with IF = 0, repeatedly wakes `WFI` without entering the handler; the emulator therefore does not terminate. The revised procedure is to disable the timer, execute `STI` / `CLI` (allowing the handler to accept the pending request), and then execute `WFI`. The rules and example were updated.
- **Waiting for IRQ1 with IF = 0** (Section 6.6): a second example was added for code running with IF = 0, alongside the earlier wait routine that ends in `STI` and assumes IF = 1. The new loop checks the flag, then executes `WFI` / `STI` / `CLI`, without a final `STI`; IF remains zero after the wait.
- **References to the *Introduction*:** the halt procedure occurs in this document only in Section 3. The Revision 1 note about the "standard halt procedure in the Introduction" has been retained as history and cross-referenced to this correction. The text of the *Introduction* (B16 Reference, Section 9.2, and the executable format document) is outside this file. In the B16 Reference, `__halt` already follows the updated procedure (`STI`, then `CLI`, then `WFI`); the executable format document (Section 8.3) still describes the old version and needs a separate correction.
- **Compatibility:** neither machine nor emulator behavior changed; only recommendations were corrected. Programs using the old halt sequence still stop if they have no pending IRQ0; if one is pending, they did not stop under the previous revision either.

### Revision 5

- **Real-time clock** (Section 8): a new device at `$FF2A–$FF2E`.
  - The value is an FS16 timestamp: TIME word (12-hour time with one-second resolution) and DATE word (year 2025–2088, month, day, PM).
  - TIME and DATE registers and a command register provide access: READ captures the clock state; WRITE, WRITE_TIME, and WRITE_DATE set the entire value, time of day only, or date only.
  - Invalid values are rejected: the clock retains its previous value and keeps running, and STATUS.ERR is set.
  - Initial value is unspecified but always valid. The clock wraps from 2088 to 2025. Machine reset does not change the clock.
- **Memory map** (Section 1): `$FF2A–$FF2E` assigned to the RTC; `$FF2F` and `$FF50–$FFFF` remain unused; word accesses to the RTC added.
- **Register summary** (Section 4) extended with clock registers.
- **Emulators** (Section 9): RTC starts at host local time and advances uniformly; `--rtc` option in the command-line emulator; Clock panel in the web debugger.
- **Section numbering:** Emulators moved to Section 9 (Sections 9.1–9.3); Revision History moved to Section 10. Sections 1–7 retained their numbers.
- **Compatibility:** Revision 4 programs run unchanged. Addresses `$FF2A–$FF2E` were unused under Revision 4 and programs were not permitted to use them.

### Revision 4

- **Device window** (Sections 1 and 7.5): the entire `$FF20–$FFFF` range belongs to devices, whether populated or not. Reads of unused addresses return zero and writes are ignored; there is no accessible memory at those addresses under any mapping. Revision 3's Section 1 said reserved addresses acted like ordinary memory in emulators, while Section 7.5 excluded the device window from memory. Revision 4 resolves this contradiction.
- **Memory map** (Section 1): actual register boundaries clarified—disk controller `$FF30–$FF3B`, memory manager `$FF40–$FF48`; remaining addresses within their ranges reserved. Word accesses spanning unused addresses are specified.
- **Disk controller** (Sections 6.1–6.4): command validation order established as BADCMD, NODISK, (COUNT = 0 means success), READONLY, RANGE, BADADDR; sums in validation checks do not overflow. CTRL writes take effect with BUSY = 1; ACK is ignored while BUSY = 1. The location reported after an IO failure, persistence of ERR and ERROR, and the INFO label contents were clarified.
- **Memory manager** (Sections 7.2, 7.5, and 7.6): unused addresses `$FF49–$FF4F` read as zero; physical addresses `$0FF20–$0FFFF` are inaccessible with EN = 0.
- **Emulators** (then Section 8): new Section 8.1 collected behavior shared by both emulators—zero-filled memory at startup, PAGE = 0 after reset, device-window word access, device polling, and disk labels. Command-line emulator changes: complete termination condition, program image size and load address, options, `--disk` errors, and Windows console input (Enter = `$0A`). Web-debugger changes: 1 ms per sector by default instead of zero, behavior while paused, "Running (WFI, disk)", Disks and Memory manager panels, device-window inspection, and reset.
- **Section numbering:** in Section 8, the Command-Line Emulator and Web Debugger became Sections 8.2 and 8.3. All other sections retained their numbers.
- **Compatibility:** Revision 3 programs run unchanged provided they do not access reserved addresses, which was prohibited already.

### Revision 3

- **Memory manager** (Section 7): a new device in `$FF40–$FF4F`.
  - Logical memory is divided into eight 8 KiB pages, each mapped to one of 256 physical frames (2 MiB total).
  - PAGE0..PAGE7 and MMU_CTRL registers were introduced. The MMU is disabled on reset and PAGE values are undefined; any program enabling it must initialize all of them.
  - Device window `$FF20–$FFFF` is not translated. The vector table is translated through PAGE7.
  - Instruction-fetch mapping changes follow synchronization-point rules (processor specification, Section 11).
- **Memory map** (Section 1): addresses defined as logical; physical address space specified; `$FF40–$FF4F` assigned to the memory manager, while `$FF2A–$FF2F` and `$FF50–$FFFF` remained reserved.
- **Disk controller** (Section 6.2): buffer address is logical and translated through the MMU when enabled; page mappings covering an active transfer must not change.
- **Register summary** (Section 4) extended with MMU registers.
- **Emulators** (then Section 8): 2 MiB of RAM and MMU panel in web debugger.
- **Section numbering:** Emulators moved to Section 8 and Revision History to Section 9; Sections 1–6 retained their numbers.
- **Compatibility:** Revision 2 programs run unchanged. The MMU is disabled after reset and the machine behaves as in Revision 2; `$FF40–$FF4F` had been reserved under that revision.

### Revision 2

- **Disk controller** (Section 6): new device in `$FF30–$FF3F`.
  - Up to eight disks, 512-byte sectors, 32-bit sector numbers (up to 2^32 sectors). Disks containing no more than 2^16 sectors are called small.
  - Transfers use DMA. Commands: ACK, READ, WRITE, INFO, FLUSH. Error codes appear in ERROR.
  - LBA, ADDR, and COUNT advance during transfers.
  - Command completion sets DONE and may raise a level-triggered IRQ1 request, cleared by ACK.
- **Memory map** (Section 1): `$FF30–$FF3F` assigned to the disk controller; `$FF2A–$FF2F` and `$FF40–$FFFF` remained reserved.
- **Device word access** (Section 1) defined by the general rule: a word at even address A is a single operation on registers A and A + 1. Console behavior remained unchanged: only word reads at `$FF20` were permitted. Disk-controller registers supported word reads and writes.
- **Program termination** (Section 3): an emulator does not terminate at `WFI` while a disk command capable of raising IRQ1 is executing (BUSY = 1 with IEN = 1).
- **Register summary** (Section 4) extended with disk-controller registers.
- **Emulators:**
  - Command-line emulator gained `--disk N=file[:ro]`.
  - Web debugger gained a disk panel.
- **Section numbering:** Emulators moved to Section 7 and Revision History to Section 8. Sections 1–5 kept their numbers, so external references to them (for example, encoding in Section 5) remained valid.
- **Compatibility:** Revision 1 programs run unchanged. `$FF30–$FF3F` were reserved in Revision 1, so programs were not permitted to use them.

### Revision 1

Changes compared with the earlier machine, which exposed only a console at port `$FF20`:

- **Port `$FF20`:** byte reads and writes retain their previous behavior. Word reads (character and scan code) and registers `$FF21–$FF29` were added.
- **Timer:** a new device, disabled by default, so existing programs are unaffected.
- **Assembler:** string literals are converted to the machine encoding. Previously the source bytes (UTF-8) were copied verbatim. Programs using only ASCII strings are unchanged.
- **Emulators:** with the timer enabled, `WFI` no longer terminates execution. The standard halt sequence in the *Introduction* was extended to disable the timer. (The halt procedure was corrected in Revision 6.)
