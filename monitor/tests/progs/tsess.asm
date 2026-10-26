; tsess.asm - helpers of the tsess test.
;
; uint t_spin(uint n): n turns of a computational loop in which between setting the flags (CMP)
; and checking them (JNZ) there are 8 instructions that do not change flags, and R4-R12 hold known values.
; A tick that interrupted the loop for a switch to another session must return the process with the same flags and
; registers (kernel design, sect. 3.8). Result: 0 - everything was preserved, otherwise the number of errors.
t_spin:
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        PUSH    R8
        PUSH    R9
        PUSH    R10
        PUSH    R11
        PUSH    LR
        LD      R4, #$1111
        LD      R5, #$2222
        LD      R6, #$3333
        LD      R7, #$4444
        LD      R8, #$5555
        LD      R9, #$6666
        LD      R10, #$7777
        LD      R11, #$8888
        LD      LR, #$9999
        LD      R1, R0                  ; counter
        LD      R2, #0                  ; errors
@l:     CMP     R1, R1                  ; ZF = 1, CF = 0
        LD      R3, R3
        LD      R3, R3
        LD      R3, R3
        LD      R3, R3
        LD      R3, R3
        LD      R3, R3
        LD      R3, R3
        LD      R3, R3
        JNZ     @bad                    ; ZF must stay 1
        JC      @bad
        CMP     R4, #$1111
        JNZ     @bad
        CMP     R11, #$8888
        JNZ     @bad
        CMP     LR, #$9999
        JNZ     @bad
        JMP     @next
@bad:   ADD     R2, #1
@next:  SUB     R1, #1
        JNZ     @l
        CMP     R5, #$2222
        JNZ     @bad2
        CMP     R6, #$3333
        JNZ     @bad2
        CMP     R7, #$4444
        JNZ     @bad2
        CMP     R8, #$5555
        JNZ     @bad2
        CMP     R9, #$6666
        JNZ     @bad2
        CMP     R10, #$7777
        JZ      @out
@bad2:  ADD     R2, #1
@out:   LD      R0, R2
        POP     LR
        POP     R11
        POP     R10
        POP     R9
        POP     R8
        POP     R7
        POP     R6
        POP     R5
        POP     R4
        RET
