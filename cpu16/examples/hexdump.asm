; hexdump.asm - a memory dump: the address, 16 bytes in hexadecimal, a text column.
;
;   0000  F3 06 00 FF 73 00 36 00  77 D0 14 00 01 01 F3 01  ....s.6.w.......
;
; The program prints two dumps:
;   - its own code and data in ROM (shows how the program and its strings lie in memory);
;   - the interrupt vector table filled in by the prologue.
; What to dump is set by the constants DUMP_START and DUMP_LEN below. No input is needed.
; It is better not to dump at address $FF20: reading the console port takes an input byte.
;
; Run:  cpu16asm hexdump.asm && cpu16 hexdump.bin
;
; Used from stdlib.asm: vectors_init, puts, putc, newline, print_hex8, print_hex16.

        .equ    VECTORS,    $FF00
        .equ    STACK_TOP,  $FF00

        .equ    DUMP_START, $0000               ; what to dump
        .equ    DUMP_LEN,   rom_end - DUMP_START ; by default - the whole program ROM

; ============================================================================
; Prologue
; ============================================================================
        .org    $0000
reset:  LD      SP, #STACK_TOP
        LD      R0, #on_fault           ; ERRCMD goes to on_fault (first of all),
        CALL    vectors_init            ;   the other vectors - to the empty irq_ignore
        CALL    main                    ; interrupts are not needed: IF stays 0
halt:   CLI
@stop:  WFI
        JMP     @stop

on_fault:                               ; [SP+2] is the address where it happened
        LD      R0, #msg_fault
        CALL    puts
        LD      R0, [SP+2]
        CALL    print_hex16
        CALL    newline
        JMP     halt

; ============================================================================
; Main program
; ============================================================================
main:   PUSH    LR
        LD      R0, #msg_rom
        CALL    puts
        LD      R0, #DUMP_START
        LD      R1, #DUMP_LEN
        CALL    dump
        LD      R0, #msg_vectors
        CALL    puts
        LD      R0, #VECTORS
        LD      R1, #32                 ; 16 vectors of 2 bytes
        CALL    dump
        POP     IP

; dump - print R1 bytes of memory starting at address R0.
; R4 is the address of the current line, R5 how many bytes are left,
; R6 the number of bytes in this line (up to 16), R7 the byte number within the line.
dump:   PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        LD      R4, R0
        LD      R5, R1
@line:  TEST    R5, R5
        JZ      @done
        LD      R6, #16                 ; R6 = min(16, left)
        CMP     R5, R6
        JAE     @addr
        LD      R6, R5
@addr:  LD      R0, R4                  ; the line address
        CALL    print_hex16
        CALL    space
        CALL    space
        LD      R7, #0
@hex:   CMP     R7, #8                  ; an extra space in the middle of the line
        JNZ     @hex1
        CALL    space
@hex1:  CMP     R7, R6
        JAE     @pad                    ; the bytes ran out: spaces so that the text lands in its place
        LD      R1, R4
        ADD     R1, R7
        LDZX    R0, [R1]                ; the byte at address R4 + R7
        CALL    print_hex8
        JMP     @sep
@pad:   CALL    space
        CALL    space
@sep:   CALL    space
        INC     R7
        CMP     R7, #16
        JB      @hex
        CALL    space
        LD      R7, #0                  ; the text column
@text:  LD      R1, R4
        ADD     R1, R7
        LDZX    R0, [R1]
        CMP     R0, #' '
        JB      @dot                    ; control characters
        CMP     R0, #'~'
        JBE     @show                   ; printable ASCII
@dot:   LD      R0, #'.'
@show:  CALL    putc
        INC     R7
        CMP     R7, R6
        JB      @text
        CALL    newline
        ADD     R4, R6                  ; the next line
        SUB     R5, R6
        JMP     @line
@done:  POP     R7
        POP     R6
        POP     R5
        POP     R4
        POP     IP

; space - print a space (a leaf routine).
space:  LD      R0, #' '
        JMP     putc                    ; a tail call

; ============================================================================
; Constants
; ============================================================================
msg_rom:        .asciz  "Program ROM:\n"
msg_vectors:    .asciz  "\nInterrupt vectors (filled in by the prologue):\n"
msg_fault:      .asciz  "\nfault at $"

        .include "../lib/stdlib.asm"
rom_end:                                ; the end of the program in ROM
