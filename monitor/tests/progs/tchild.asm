; tchild.asm - child helpers: an illegal instruction, reading IF, littering memory.

; an illegal instruction: opcode 00111 is reserved (sect. 6.2 of the processor specification)
t_bad_insn:
t_bad_addr:
        .word   $3800
        RET

; uint t_if(): the value of IF (0 or 8)
t_if:
        LD      R0, FLAGS
        AND     R0, #8
        RET

; void t_dirty(ptr from, uint words): fills memory with the pattern $DEAD
t_dirty:
        LD      R2, #$DEAD
@lp:    ST      [R0++], R2
        SUB     R1, #1
        JNZ     @lp
        RET

; uint t_nonzero(ptr from, uint words): the number of nonzero words
t_nonzero:
        LD      R2, #0
@lp:    LD      R3, [R0++]
        CMP     R3, #0
        JZ      @z
        ADD     R2, #1
@z:     SUB     R1, #1
        JNZ     @lp
        LD      R0, R2
        RET

; uint t_sp(): the current SP
t_sp:
        LD      R0, SP
        RET

; an illegal-instruction handler: skip it (the instruction $3800 is one word) and return
t_skip_insn:
        PUSH    R0
        LD      R0, [SP + 4]                    ; the instruction address in the frame (above the saved R0)
        ADD     R0, #2
        ST      [SP + 4], R0
        POP     R0
        IRET

; uint t_onevent_bad(uint h): ONEVENT(EVT_BADINSN, h) with an arbitrary address
t_onevent_raw:
        LD      R1, R0
        LD      R0, #EVT_BADINSN
        TRAP    #9
        .word   SYS_ONEVENT
        RET

; the reserved instruction TRAP #10
t_trap10:
        TRAP    #10
        RET
