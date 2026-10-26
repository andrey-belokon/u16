; boot.asm - prologue for the kernel module tests/kernel/kmod.b16 (b16c kmod.b16 -o kmod.s; target
; kernel, code at $1000 and data at $8000 are set in kmod.b16 by option declarations).
        .org    $0000
        LD      SP, #$FF00
        CALL    __init_data             ; data image -> $8000, bss - zeros
        LD      R0, #2
        CALL    kentry                  ; R0 = result
        CALL    putu
        CALL    putnl
        CLI
@h:     WFI
        JMP     @h

        .include "kmod.s"
