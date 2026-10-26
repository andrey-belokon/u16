; tsys.asm - assembly helpers of the tsys test: the register state around a call,
; ZF, a computational loop under ticks, working with the stack at the lower memory boundary.

; (R0 = the result, R1 = FLAGS after return) of an empty call
t_null_flags:
        TRAP    #9
        .word   SYS_TICKS
        LD      R1, FLAGS
        RET

; the same for an unknown call number
t_bad_flags:
        TRAP    #9
        .word   $7F
        LD      R1, FLAGS
        RET

; uint t_sp(): the current SP
t_sp:
        LD      R0, SP
        RET

; uint t_regs(uint mode): an empty call with IF = 1 (mode = 0) or IF = 0 (mode = 1).
; The result is a mask of mismatches: bits 0-7 - R4..R11, bit 8 - LR, bit 9 - SP, bit 10 - IF.
t_regs:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        PUSH    R8
        PUSH    R9
        PUSH    R10
        PUSH    R11
        PUSH    FLAGS
        LD      R1, #8                  ; the expected IF after the call
        CMP     R0, #0
        JZ      @if1
        LD      R1, #0
        CLI
        JMP     @go
@if1:   STI
@go:    ST      [t_exp_if], R1
        LD      R4, #$0401
        LD      R5, #$0502
        LD      R6, #$0603
        LD      R7, #$0704
        LD      R8, #$0805
        LD      R9, #$0906
        LD      R10, #$0A07
        LD      R11, #$0B08
        LD      LR, #$C0DE
        ST      [t_sp_save], SP
        TRAP    #9
        .word   SYS_TICKS
        LD      R3, #0
        CMP     R4, #$0401
        JZ      @c5
        OR      R3, #$01
@c5:    CMP     R5, #$0502
        JZ      @c6
        OR      R3, #$02
@c6:    CMP     R6, #$0603
        JZ      @c7
        OR      R3, #$04
@c7:    CMP     R7, #$0704
        JZ      @c8
        OR      R3, #$08
@c8:    CMP     R8, #$0805
        JZ      @c9
        OR      R3, #$10
@c9:    CMP     R9, #$0906
        JZ      @c10
        OR      R3, #$20
@c10:   CMP     R10, #$0A07
        JZ      @c11
        OR      R3, #$40
@c11:   CMP     R11, #$0B08
        JZ      @clr
        OR      R3, #$80
@clr:   CMP     LR, #$C0DE
        JZ      @csp
        OR      R3, #$100
@csp:   CMP     SP, [t_sp_save]
        JZ      @cif
        OR      R3, #$200
@cif:   LD      R2, FLAGS
        AND     R2, #8
        CMP     R2, [t_exp_if]
        JZ      @out
        OR      R3, #$400
@out:   POP     FLAGS                   ; IF as before the call
        POP     R11
        POP     R10
        POP     R9
        POP     R8
        POP     R7
        POP     R6
        POP     R5
        POP     R4
        POP     LR
        LD      R0, R3
        RET

; uint t_spin(uint n): n turns of a loop that involves R0..R3, R4..R11 and the flags.
; The result is a checksum; it must not depend on ticks interrupting the loop at any
; place. The values of R4..R11 at the end are compared with the originals: a mismatch is added to the sum.
t_spin:
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        PUSH    R8
        PUSH    R9
        PUSH    R10
        PUSH    R11
        LD      R4, #$0401
        LD      R5, #$0502
        LD      R6, #$0603
        LD      R7, #$0704
        LD      R8, #$0805
        LD      R9, #$0906
        LD      R10, #$0A07
        LD      R11, #$0B08
        LD      R1, #1
        LD      R2, #3
        LD      R3, #5
@lp:    ADD     R1, R4
        XOR     R1, R5
        ADD     R2, R1
        SUB     R2, R6
        ADC     R3, R2
        ROR     R3, #1
        ADD     R1, R7
        XOR     R2, R8
        ADD     R3, R9
        XOR     R1, R10
        ADD     R2, R11
        SUB     R0, #1
        JNZ     @lp
        LD      R0, R1
        ADD     R0, R2
        ADD     R0, R3
        CMP     R4, #$0401
        JZ      @k5
        ADD     R0, #1
@k5:    CMP     R5, #$0502
        JZ      @k6
        ADD     R0, #2
@k6:    CMP     R6, #$0603
        JZ      @k7
        ADD     R0, #4
@k7:    CMP     R7, #$0704
        JZ      @k8
        ADD     R0, #8
@k8:    CMP     R8, #$0805
        JZ      @k9
        ADD     R0, #16
@k9:    CMP     R9, #$0906
        JZ      @k10
        ADD     R0, #32
@k10:   CMP     R10, #$0A07
        JZ      @k11
        ADD     R0, #64
@k11:   CMP     R11, #$0B08
        JZ      @kend
        ADD     R0, #128
@kend:  POP     R11
        POP     R10
        POP     R9
        POP     R8
        POP     R7
        POP     R6
        POP     R5
        POP     R4
        RET

; uint t_lowsp(uint n): moves SP 4 bytes above the lower memory boundary (SP = 4, an interrupt
; writes IP and FLAGS to addresses 2 and 0) and spins a loop n turns with IF = 1. R4..R11 are set to
; a pattern. Result: a mask of mismatches (R4..R11 - bits 0-7, SP - bit 8).
t_lowsp:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        PUSH    R8
        PUSH    R9
        PUSH    R10
        PUSH    R11
        LD      R1, R0
        LD      R4, #$1111
        LD      R5, #$2222
        LD      R6, #$3333
        LD      R7, #$4444
        LD      R8, #$5555
        LD      R9, #$6666
        LD      R10, #$7777
        LD      R11, #$8888
        CLI
        ST      [t_sp_save], SP
        LD      SP, #4
        STI                             ; from this moment a tick writes to 0..3
@lp:    SUB     R1, #1
        JNZ     @lp
        CLI
        LD      SP, [t_sp_save]
        STI
        LD      R3, #0
        CMP     R4, #$1111
        JZ      @d5
        OR      R3, #$01
@d5:    CMP     R5, #$2222
        JZ      @d6
        OR      R3, #$02
@d6:    CMP     R6, #$3333
        JZ      @d7
        OR      R3, #$04
@d7:    CMP     R7, #$4444
        JZ      @d8
        OR      R3, #$08
@d8:    CMP     R8, #$5555
        JZ      @d9
        OR      R3, #$10
@d9:    CMP     R9, #$6666
        JZ      @d10
        OR      R3, #$20
@d10:   CMP     R10, #$7777
        JZ      @d11
        OR      R3, #$40
@d11:   CMP     R11, #$8888
        JZ      @dsp
        OR      R3, #$80
@dsp:   LD      R0, SP
        CMP     R0, [t_sp_save]
        JZ      @dend
        OR      R3, #$100
@dend:  POP     R11
        POP     R10
        POP     R9
        POP     R8
        POP     R7
        POP     R6
        POP     R5
        POP     R4
        POP     LR
        LD      R0, R3
        RET

t_exp_if:   .word   0
t_sp_save:  .word   0

; uint t_hook_bad(): TICKHOOK with an address outside process memory
t_hook_bad:
        LD      R0, #$FFFE
        TRAP    #9
        .word   SYS_TICKHOOK
        RET
