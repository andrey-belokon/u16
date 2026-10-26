; tev.asm - helpers of the tev test: a computational pause.
; void t_pause(uint n): n turns of an empty loop (IF as in the caller)
t_pause:
        SUB     R0, #1
        JNZ     t_pause
        RET
