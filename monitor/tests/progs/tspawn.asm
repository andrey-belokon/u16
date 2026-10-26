; tspawn.asm - SPAWN with a check of the parent's registers and IF.

; (uint err, uint code, uint mask) t_spawn_regs(ptr cmd): R4..R11 and LR are set to a pattern;
; mask - the mismatches after return (bits 0-7 R4..R11, bit 8 LR, bit 9 SP).
t_spawn_regs:
        PUSH    LR
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
        LD      LR, #$C0DE
        ST      [t_sp_save], SP
        LD      R1, #$FFFF
        LD      R2, #$FFFF
        LD      R3, #$FFFF
        TRAP    #9
        .word   SYS_SPAWN
        LD      R2, #0
        CMP     R4, #$0401
        JZ      @c5
        OR      R2, #$01
@c5:    CMP     R5, #$0502
        JZ      @c6
        OR      R2, #$02
@c6:    CMP     R6, #$0603
        JZ      @c7
        OR      R2, #$04
@c7:    CMP     R7, #$0704
        JZ      @c8
        OR      R2, #$08
@c8:    CMP     R8, #$0805
        JZ      @c9
        OR      R2, #$10
@c9:    CMP     R9, #$0906
        JZ      @c10
        OR      R2, #$20
@c10:   CMP     R10, #$0A07
        JZ      @c11
        OR      R2, #$40
@c11:   CMP     R11, #$0B08
        JZ      @clr
        OR      R2, #$80
@clr:   CMP     LR, #$C0DE
        JZ      @csp
        OR      R2, #$100
@csp:   CMP     SP, [t_sp_save]
        JZ      @out
        OR      R2, #$200
@out:   POP     R11
        POP     R10
        POP     R9
        POP     R8
        POP     R7
        POP     R6
        POP     R5
        POP     R4
        POP     LR
        RET

; (uint err, uint code, uint ifbits) t_spawn_if0(ptr cmd): SPAWN with IF = 0; ifbits - IF after
; return (must be 0). IF is restored.
t_spawn_if0:
        PUSH    FLAGS
        CLI
        LD      R1, #$FFFF
        LD      R2, #$FFFF
        LD      R3, #$FFFF
        TRAP    #9
        .word   SYS_SPAWN
        LD      R2, FLAGS
        AND     R2, #8
        POP     FLAGS
        RET

; uint t_hookaddr(): the address of the exit routine (for checking LR at launch)
t_lr:
        LD      R0, LR
        RET

t_sp_save:  .word   0
