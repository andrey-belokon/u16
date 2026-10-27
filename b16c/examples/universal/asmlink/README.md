# Linking B16 and Assembly Modules

`main.b16` calls functions defined in `util.asm`; in turn, `util.asm` calls B16 functions through pointers.

```sh
b16c -I ../../lib main.b16 -o main.s
cpu16asm --exec -o main.bin main.s
cpu16 main.bin
```

- The `include asm "util.asm"` statement in `main.b16` inserts the module with an `.include` directive **after the program code and runtime but before the data**. Its code and initialized data (`.word`, `.byte`) therefore reside in the image before `__image_end`. The Monitor loads only those bytes from the executable file. If you instead include the assembly module after the compiler's output in a separate assembly source (`.include "main.s"` followed by `.include "util.asm"`), the module's code ends up after `__bss_end`. The program may still work on a bare machine, but under the Monitor calls will jump into zero-filled memory. `cpu16asm --exec` rejects this layout.
- The assembly module may contain code and initialized data. It does not need `.reserve` sections; declare uninitialized storage in B16 instead.
- Assembly-defined functions are declared in B16 as `extern fn`: a prototype without a body, implemented outside B16. Such declarations do not generate missing-definition warnings, so this example builds with `-Werror`.
- `util.asm` follows the B16 calling convention (programmer's guide, section 8).
- A B16 function that is called **by name** from assembly but is not referenced anywhere in B16 source will be discarded as unused. Mark it `export` to retain it.
