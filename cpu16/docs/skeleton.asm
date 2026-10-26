        .equ    RAM_START,  $8000
        .equ    VECTORS,    $FF00       ; 16 words: IRQ0..7, ERRCMD (8), TRAP 9..15
        .equ    STACK_TOP,  $FF00

; ---------------- ROM: program code ----------------
        .org    $0000
reset:  LD      SP, #STACK_TOP          ; 1. set up the stack
        LD      R0, #on_errcmd          ; 2. install ERRCMD first
        CALL    vectors_init            ; 3. initialize all other vectors to irq_ignore
        LD      R0, #on_tick            ;    then install the active handlers:
        ST      [VECTORS + 2*0], R0     ;    IRQ0: timer
        LD      R0, #bss_start          ; 4. zero-initialize variables
        LD      R1, #0
        LD      R2, #bss_end - bss_start
        CALL    memset
        STI                             ; 5. enable interrupts
        CALL    main                    ; 6. run the main program
halt:   CLI                             ;    then stop
@stop:  WFI
        JMP     @stop

main:   PUSH    LR
        LD      R0, #msg_hello
        CALL    puts
        LD      R0, #msg_ask
        CALL    puts
        LD      R0, #1                  ; echo enabled
        CALL    read_dec
        TEST    R1, R1
        JNZ     @bad
        LD      R4, R0
        LD      R0, #msg_hex
        CALL    puts
        LD      R0, R4
        CALL    print_hex16
        LD      R0, #msg_square
        CALL    puts
        LD      R0, R4
        LD      R1, R4
        CALL    mulu16                  ; R1:R0 = R4 * R4
        CALL    print_u32
        CALL    newline
        JMP     @wait
@bad:   LD      R0, #msg_bad
        CALL    puts
@wait:  LD      R0, #msg_wait
        CALL    puts
@tick:  WFI                             ; wait for interrupts
        LD      R0, [ticks]
        CMP     R0, #3
        JB      @tick
        LD      R0, #msg_done
        CALL    puts
        POP     IP

on_tick:                                ; IRQ0 handler: save everything it modifies
        PUSH    LR
        PUSH    R0
        PUSH    R1
        PUSH    R2
        PUSH    R3
        LD      R0, [ticks]
        INC     R0
        ST      [ticks], R0
        LD      R0, #'.'
        CALL    putc
        POP     R3
        POP     R2
        POP     R1
        POP     R0
        POP     LR
        IRET

on_errcmd:                              ; stack: [SP] = FLAGS, [SP+2] = instruction address
        LD      R0, #msg_errcmd
        CALL    puts
        LD      R0, [SP+2]
        CALL    print_hex16
        CALL    newline
        JMP     halt

msg_hello:  .asciz  "CPU16 skeleton\n"
msg_ask:    .asciz  "Enter a number: "
msg_hex:    .asciz  "hex: $"
msg_square: .asciz  ", square: "
msg_bad:    .asciz  "\nnot a number\n"
msg_wait:   .asciz  "Waiting for 3 timer interrupts (IRQ 0)"
msg_done:   .asciz  " done\n"
msg_errcmd: .asciz  "\nERRCMD at $"

        .include "stdlib.asm"

; ---------------- RAM: variables ----------------
        .org    RAM_START
bss_start:
ticks:  .space  2
bss_end:
