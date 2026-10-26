; The CPU16 debugger demo program.
; Prints a string, sums an array, then waits for interrupts.
; The IRQ 0 button in the debugger calls the irq0 handler: it counts calls and prints "IRQ0".

        .equ    CONSOLE, $FF20

        .org    0
start:  LD      SP, #$FF00
        LD      R0, #msg
        CALL    print
        LD      R2, #0          ; the sum of the elements of table
        LD      R3, #table
        LD      R4, #5
sum:    ADD     R2, [R3++]
        DEC     R4
        JNZ     sum
        ST      [result], R2
        STI                     ; enable interrupts
idle:   WFI
        JMP     idle

; Printing a zero-terminated string. R0 is the string address (it advances), R1 is preserved.
print:  PUSH    LR
        PUSH    R1
@next:  LDZX    R1, [R0++]
        CMP     R1, #0
        JZ      @done
        STB     [CONSOLE], R1
        JMP     @next
@done:  POP     R1
        POP     LR
        RET

; The IRQ0 handler: increment ticks and print a message.
irq0:   PUSH    R0
        PUSH    LR
        LD      R0, [ticks]
        INC     R0
        ST      [ticks], R0
        LD      R0, #irq_msg
        CALL    print
        POP     LR
        POP     R0
        IRET

msg:     .asciz  "Hello 16bit world!\n"
irq_msg: .asciz  "IRQ0\n"
table:   .word   10, 20, 30, 40, 50
result:  .word   0
ticks:   .word   0

        .org    $FF00           ; the vector table
        .word   irq0            ; vector 0: IRQ0
