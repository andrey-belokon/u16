; calc.asm - a calculator: reads lines like "a op b" and prints the result.
;
;   > 1234 * 567
;   = 699678 ($000AAD1E)
;
; The operands are decimal numbers 0..65535, the operations are + - * / %. The result is 32-bit,
; so a product is not lost; a difference can be negative.
; An empty line or q exits.
;
; Run:  cpu16asm calc.asm && cpu16 calc.bin            (keyboard input)
;          cpu16 calc.bin < input.txt                       (input from a file)
; The line is printed by the program (echo), so in a terminal the typed line
; appears twice: once from the terminal, once from the program.
;
; Used from stdlib.asm: vectors_init, gets, puts, putc, newline, add32, sub32, mulu16,
; divu32, print_u32, print_s32, print_hex32.

        .equ    VECTORS,    $FF00
        .equ    STACK_TOP,  $FF00
        .equ    RAM_START,  $8000
        .equ    LINE_SIZE,  40

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
; The main loop
; ============================================================================
; R5 is the first operand, R6 the operation, R7 the second operand,
; R9:R8 the result (high:low word).
main:   PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        PUSH    R8
        PUSH    R9
        LD      R0, #msg_banner
        CALL    puts

@loop:  LD      R0, #msg_prompt
        CALL    puts
        LD      R0, #line
        LD      R1, #LINE_SIZE
        LD      R2, #1                  ; with echo
        CALL    gets
        LD      R0, #line
        CALL    skip_spaces             ; R0 is a pointer, R1 the first character
        CMP     R1, #0
        JZ      @quit                   ; an empty line
        CMP     R1, #'q'
        JZ      @quit

        CALL    parse_u16               ; the first operand
        TEST    R2, R2
        JNZ     @bad_num
        LD      R5, R0
        LD      R0, R1
        CALL    skip_spaces             ; R1 is the operation sign
        LD      R6, R1
        LD      R2, #ops                ; is it in the list of operations?
@find:  LDZX    R3, [R2++]
        CMP     R3, #0
        JZ      @bad_op
        CMP     R3, R6
        JNZ     @find
        INC     R0                      ; past the operation sign
        CALL    skip_spaces
        CALL    parse_u16               ; the second operand
        TEST    R2, R2
        JNZ     @bad_num
        LD      R7, R0
        LD      R0, R1
        CALL    skip_spaces
        CMP     R1, #0
        JNZ     @bad_end                ; something follows after the second number

        ; ---- computation: the result in R1:R0
        LD      R0, R5
        CMP     R6, #'*'
        JZ      @mul
        LD      R1, #0                  ; for + - / %: a = 0:R5, b = 0:R7
        LD      R2, R7
        LD      R3, #0
        CMP     R6, #'+'
        JZ      @add
        CMP     R6, #'-'
        JZ      @sub
        CALL    divu32                  ; / and %: R1:R0 is the quotient, R2 the remainder
        JO      @div0
        CMP     R6, #'/'
        JZ      @show
        LD      R0, R2                  ; %: the remainder
        LD      R1, #0
        JMP     @show
@add:   CALL    add32
        JMP     @show
@sub:   CALL    sub32                   ; on a borrow the high word is $FFFF: a ready sign extension
        JMP     @show
@mul:   LD      R1, R7
        CALL    mulu16

        ; ---- printing: "= <decimal> ($<hexadecimal>)"
@show:  LD      R8, R0
        LD      R9, R1
        LD      R0, #msg_eq
        CALL    puts
        LD      R0, R8
        LD      R1, R9
        CMP     R6, #'-'
        JNZ     @unsigned
        CALL    print_s32
        JMP     @hex
@unsigned:
        CALL    print_u32
@hex:   LD      R0, #msg_hex
        CALL    puts
        LD      R0, R8
        LD      R1, R9
        CALL    print_hex32
        LD      R0, #')'
        CALL    putc
        CALL    newline
        JMP     @loop

@bad_num:
        LD      R0, #msg_bad_num
        JMP     @error
@bad_op:
        LD      R0, #msg_bad_op
        JMP     @error
@bad_end:
        LD      R0, #msg_bad_end
        JMP     @error
@div0:  LD      R0, #msg_div0
@error: CALL    puts
        JMP     @loop

@quit:  LD      R0, #msg_bye
        CALL    puts
        POP     R9
        POP     R8
        POP     R7
        POP     R6
        POP     R5
        POP     R4
        POP     IP

; ============================================================================
; Parsing a line
; ============================================================================

; skip_spaces - skip blanks. R0 is a pointer to the line.
; Result: R0 is a pointer to the first non-blank, R1 is that character.
skip_spaces:
        LDZX    R1, [R0]
        CMP     R1, #' '
        JNZ     @done
        INC     R0
        JMP     skip_spaces
@done:  RET

; parse_u16 - read a decimal number from a line. R0 is a pointer to the first digit.
; Result: R0 is the number, R1 is a pointer past the last digit,
;            R2 - 0 if read; 1 if there are no digits or the number is above 65535.
parse_u16:
        LD      R1, R0
        LD      R0, #0
        LD      R2, #1                  ; until a digit has been seen - an error
@digit: LDZX    R3, [R1]
        SUB     R3, #'0'
        CMP     R3, #9
        JA      @end                    ; not a digit (unsigned subtraction: characters below '0' come here too)
        MUL     R0, #10
        JC      @ovf
        ADD     R0, R3
        JC      @ovf
        CMP     R2, #2
        JZ      @next
        LD      R2, #0                  ; there is a digit
@next:  INC     R1
        JMP     @digit
@ovf:   LD      R2, #2                  ; overflow: read the remaining digits
        JMP     @next
@end:   CMP     R2, #2
        JNZ     @ret
        LD      R2, #1
@ret:   RET

; ============================================================================
; Constants
; ============================================================================
ops:            .asciz  "+-*/%"
msg_banner:     .asciz  "CPU16 calculator. Enter: a op b (op is + - * / %), empty line or q to quit.\n"
msg_prompt:     .asciz  "> "
msg_eq:         .asciz  "= "
msg_hex:        .asciz  " ($"
msg_bad_num:    .asciz  "error: expected a number 0..65535\n"
msg_bad_op:     .asciz  "error: unknown operation\n"
msg_bad_end:    .asciz  "error: unexpected text after the second number\n"
msg_div0:       .asciz  "error: division by zero\n"
msg_bye:        .asciz  "bye\n"
msg_fault:      .asciz  "\nfault at $"

        .include "../lib/stdlib.asm"

; ============================================================================
; RAM
; ============================================================================
        .org    RAM_START
line:   .space  LINE_SIZE
