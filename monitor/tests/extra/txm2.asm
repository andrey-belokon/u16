; txm2.asm - XMAP with samples in registers: what the call preserves
; uint t_xregs(uint page, uint f): a mask of mismatches after the call: bits 0-7 - R4..R11,
; bit 8 - LR, bit 9 - SP, bit 10 - ZF does not correspond to R0, bit 11 - IF changed
t_xregs:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        PUSH    R8
        PUSH    R9
        PUSH    R10
        PUSH    R11
        LD      R4, #$4444
        LD      R5, #$5555
        LD      R6, #$6666
        LD      R7, #$7777
        LD      R8, #$8888
        LD      R9, #$9999
        LD      R10, #$AAAA
        LD      R11, #$BBBB
        LD      LR, #$CCCC
        LD      R3, FLAGS
        PUSH    R3
        ST      [t_spv], SP
        TRAP    #9
        .word   SYS_XMAP
        LD      R2, FLAGS
        POP     R3                      ; FLAGS before the call
        XOR     R3, R2
        AND     R3, #8                  ; IF
        SHL     R3, #8                  ; bit 11
        AND     R2, #1                  ; ZF
        CMP     R0, #0
        JNZ     @nz
        XOR     R2, #1                  ; R0 = 0: ZF must be 1
@nz:    SHL     R2, #10
        OR      R3, R2
        LD      R2, [t_spv]
        ADD     R2, #2                  ; FLAGS are already popped
        CMP     R2, SP
        JZ      @sp
        OR      R3, #$200
@sp:
        CMP     R4, #$4444
        JZ      @r4
        OR      R3, #1
@r4:     CMP     R5, #$5555
        JZ      @r5
        OR      R3, #2
@r5:     CMP     R6, #$6666
        JZ      @r6
        OR      R3, #4
@r6:     CMP     R7, #$7777
        JZ      @r7
        OR      R3, #8
@r7:     CMP     R8, #$8888
        JZ      @r8
        OR      R3, #$10
@r8:     CMP     R9, #$9999
        JZ      @r9
        OR      R3, #$20
@r9:     CMP     R10, #$AAAA
        JZ      @r10
        OR      R3, #$40
@r10:    CMP     R11, #$BBBB
        JZ      @r11
        OR      R3, #$80
@r11:    CMP     LR, #$CCCC
        JZ      @r12
        OR      R3, #$100
@r12:    LD      R0, R3
        POP     R11
        POP     R10
        POP     R9
        POP     R8
        POP     R7
        POP     R6
        POP     R5
        POP     R4
        POP     LR
        RET

; uint t_page(uint p): the frame in register PAGEp
t_page:
        LDZX    R0, [R0 + $FF40]
        RET

t_spv:  .word   0
