; util.asm - assembly functions for main.b16.
; B16 calling convention: arguments and results in R0..R3; R0..R3 and LR may be
; clobbered, R4..R11 must be preserved; a non-leaf function saves LR.

; uint checksum(ptr byte p, uint n): sum of bytes
checksum:
        LD      R2, #0
        TEST    R1, R1
        JZ      @done
@loop:  LDZX    R3, [R0++]
        ADD     R2, R3
        DEC     R1
        JNZ     @loop
@done:  LD      R0, R2
        RET

; void each(ptr int p, uint n, ptr fn(int) f): call f(x) for each element.
; It calls a function, so it saves LR and keeps p, n, f in R4..R6.
each:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        LD      R4, R0
        LD      R5, R1
        LD      R6, R2
        TEST    R5, R5
        JZ      @end
@next:  LD      R0, [R4++]
        CALL    R6
        DEC     R5
        JNZ     @next
@end:   POP     R6
        POP     R5
        POP     R4
        POP     IP

; int, int minmax(ptr int p, uint n): two results, in R0 and R1 (n > 0).
minmax:
        LD      R2, [R0]        ; min = max = p[0]
        LD      R3, R2
@scan:  DEC     R1
        JZ      @out
        ADD     R0, #2
        CMP     R2, [R0]        ; min > p[i]? (signed comparison)
        JLE     @nomin
        LD      R2, [R0]
@nomin: CMP     R3, [R0]        ; max < p[i]?
        JGE     @scan
        LD      R3, [R0]
        JMP     @scan
@out:   LD      R0, R2
        LD      R1, R3
        RET
