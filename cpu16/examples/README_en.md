# CPU16 Example Programs

All examples include the standard library using `.include "../lib/stdlib.asm"`. The include path is resolved relative to the example's source file.

- **Command line:** From the project root, run `make examples` to assemble all examples into `build/examples/`. To assemble and run a single example, use `build/cpu16asm examples/snake.asm -o snake.bin` followed by `build/cpu16 snake.bin`.
- **Browser debugger:** Paste an example into the **Source** tab and press Ctrl+B to assemble it. The standard library is bundled with the debugger, so the `.include` directive works unchanged. For programs that use the screen, open **Display** next to the console and click inside the display to direct keyboard input to the program.

## Console programs

| File | Behavior | Demonstrates |
| --- | --- | --- |
| `calc.asm` | Calculator accepting `a op b`, showing results in decimal and hexadecimal | Line input, buffer parsing, 32-bit arithmetic, division by zero |
| `calc_exec.asm` | The same calculator, packaged as a CX16 executable. Assemble with `--exec` (`make examples` does this automatically for `*_exec.asm`). | Executable header, a `.reserve` buffer beyond `image_end`, execution under the Monitor and on bare hardware |
| `disk_demo.asm` | Displays disk information from an INFO block and maintains a boot counter in sector 0. Run with `cpu16 --disk 0=demo.img disk_demo.bin`. | Disk controller: DRIVES, INFO, READ, WRITE, FLUSH; polling-based driver and error codes |
| `hexdump.asm` | Dumps its own ROM and the interrupt vector table | Pointers, byte accesses, hexadecimal output |
| `primes.asm` | Finds primes with the Sieve of Eratosthenes, their sum, and the largest gap | RAM arrays, `memset`, 32-bit sums |

## Display, keyboard, timer, and RTC

| File | Behavior | Demonstrates |
| --- | --- | --- |
| `display_demo.asm` | Tour of colors, text attributes, Cyrillic characters, symbols, box-drawing characters, and recoloring | Positioned output, attributes, borders |
| `keys.asm` | Shows the character, character code, scan code, and name of each pressed key; Esc quits | Reading a word from `$FF20`, scan-code lookup table |
| `stopwatch.asm` | Stopwatch with large block-style digits; Space starts/stops, R resets | 50 Hz timer, IRQ0 handler, `WFI`, nonblocking keyboard input |
| `clock.asm` | Clock with date, weekday, and large time digits; H advances the hour, M advances the minute, 0 clears the seconds, D advances the date. Prints the RTC value upon exit. To set the initial time: `cpu16 --rtc 31.12.2026T23:59:50 clock.bin`. | RTC READ, WRITE_TIME, WRITE_DATE, ERR checks, detecting second changes using timer ticks |
| `snake.asm` | Snake game; arrow keys steer, Space pauses, Esc quits | Timer-driven movement, RAM playfield, ring buffer, random number generator |
| `tetris.asm` | Tetris: ←/→ move, ↑ rotate, ↓ accelerate, Space hard-drops, P pauses. Press D on the title screen for a demo (or wait 30 seconds); any key returns to the title screen. | 4×4 bitmask pieces, RAM playfield, line clearing, shuffled piece bag, ghost piece, 32-bit score, computer player using move search and board evaluation |

Display-based examples also run in the command-line emulator: terminal arrow and function keys are translated to scan codes. The screen itself is not displayed there; pass `--screen` to print its final contents when execution ends.
