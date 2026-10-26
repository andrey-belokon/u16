; primes.asm - primes up to N by the Sieve of Eratosthenes.
;
; Prints a table of primes, their count, their sum (32-bit) and the largest
; gap between neighboring primes. The bound N is set by the constant below (2..30000).
;
; The sieve array is N+1 bytes in RAM from address $8000: byte k is 1 while k is considered prime.
; It is convenient to look at in the web debugger's memory dump by setting a breakpoint in the inner
; loop (@inner).
;
; Run:  cpu16asm primes.asm && cpu16 primes.bin
;
; Used from stdlib.asm: vectors_init, memset, add32, puts, newline, print_u16, print_u32,
; and also the CONSOLE constant.

        .equ    VECTORS,    $FF00
        .equ    STACK_TOP,  $FF00
        .equ    RAM_START,  $8000

        .equ    N,          2000        ; the upper search bound
        .equ    PER_LINE,   10          ; numbers per table row

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
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        PUSH    R8
        PUSH    R9
        PUSH    R10
        PUSH    R11

        ; ---- 1. consider all numbers 0..N prime, except 0 and 1
        LD      R0, #sieve
        LD      R1, #1
        LD      R2, #N+1
        CALL    memset
        LD      R0, #0
        STB     [sieve], R0
        STB     [sieve+1], R0

        ; ---- 2. the sieve: for each prime i cross out i*i, i*i+i, ...
        LD      R4, #2                  ; i
@outer: LD      R0, R4
        MUL     R0, R4                  ; i*i
        CMP     R0, #N
        JA      @list                   ; i*i > N: everything is crossed out
        LDZX    R1, [R4+sieve]          ; sieve[i]: R4 is the index, sieve is the array address
        TEST    R1, R1
        JZ      @next_i                 ; i is composite: its multiples are already crossed out
        LD      R5, R0                  ; j = i*i
        LD      R1, #0
@inner: STB     [R5+sieve], R1          ; sieve[j] = 0
        ADD     R5, R4
        CMP     R5, #N
        JBE     @inner
@next_i:
        INC     R4
        JMP     @outer

        ; ---- 3. the table and statistics
        ; R4 is the number k, R5 the count of primes, R6 the previous prime,
        ; R7 the largest gap, R8 its start, R10:R9 the sum, R11 the position in the row
@list:  LD      R0, #msg_list
        CALL    puts
        LD      R4, #2
        LD      R5, #0
        LD      R6, #0
        LD      R7, #0
        LD      R8, #0
        LD      R9, #0
        LD      R10, #0
        LD      R11, #0
@scan:  LDZX    R0, [R4+sieve]
        TEST    R0, R0
        JZ      @skip
        INC     R5
        LD      R0, R9                  ; sum += k (32 bits)
        LD      R1, R10
        LD      R2, R4
        LD      R3, #0
        CALL    add32
        LD      R9, R0
        LD      R10, R1
        TEST    R6, R6                  ; the gap to the previous prime
        JZ      @prev
        LD      R0, R4
        SUB     R0, R6
        CMP     R0, R7
        JBE     @prev
        LD      R7, R0
        LD      R8, R6
@prev:  LD      R6, R4
        LD      R0, R4                  ; into the table
        CALL    print_w6
        INC     R11
        CMP     R11, #PER_LINE
        JB      @skip
        CALL    newline
        LD      R11, #0
@skip:  INC     R4
        CMP     R4, #N
        JBE     @scan
        TEST    R11, R11                ; an unfinished table row
        JZ      @stats
        CALL    newline

@stats: LD      R0, #msg_n
        CALL    puts
        LD      R0, #N
        CALL    print_u16
        LD      R0, #msg_count
        CALL    puts
        LD      R0, R5
        CALL    print_u16
        LD      R0, #msg_sum
        CALL    puts
        LD      R0, R9
        LD      R1, R10
        CALL    print_u32
        TEST    R7, R7                  ; one prime: no gaps
        JZ      @end
        LD      R0, #msg_gap
        CALL    puts
        LD      R0, R7
        CALL    print_u16
        LD      R0, #msg_open
        CALL    puts
        LD      R0, R8
        CALL    print_u16
        LD      R0, #msg_dots
        CALL    puts
        LD      R0, R8
        ADD     R0, R7
        CALL    print_u16
        LD      R0, #msg_close
        JMP     @out
@end:   LD      R0, #msg_end            ; without a gap - only a newline
@out:   CALL    puts

        POP     R11
        POP     R10
        POP     R9
        POP     R8
        POP     R7
        POP     R6
        POP     R5
        POP     R4
        POP     IP

; print_w6 - print R0 unsigned, right-aligned in a field of width 6.
print_w6:
        LD      R1, #5                  ; spaces before a one-digit number
        CMP     R0, #10
        JB      @pad
        DEC     R1
        CMP     R0, #100
        JB      @pad
        DEC     R1
        CMP     R0, #1000
        JB      @pad
        DEC     R1
        CMP     R0, #10000
        JB      @pad
        DEC     R1
@pad:   LD      R2, #' '
@space: STB     [CONSOLE], R2           ; spaces straight to the port, R0 is not touched
        DEC     R1
        JNZ     @space
        JMP     print_u16               ; a tail call

; ============================================================================
; Constants
; ============================================================================
msg_list:   .asciz  "Primes:\n"
msg_n:      .asciz  "\nN = "
msg_count:  .asciz  "\nprimes:      "
msg_sum:    .asciz  "\nsum:         "
msg_gap:    .asciz  "\nlargest gap: "
msg_open:   .asciz  " ("
msg_dots:   .asciz  ".."
msg_close:  .asciz  ")\n"
msg_end:    .asciz  "\n"
msg_fault:  .asciz  "\nfault at $"

        .include "../lib/stdlib.asm"

; ============================================================================
; RAM
; ============================================================================
        .org    RAM_START
sieve:  .space  N+1                     ; the sieve: sieve[k] = 1 if k is prime
