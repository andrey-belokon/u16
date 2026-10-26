; resident.asm - the resident part of the monitor: page 7, frame 7 in all maps
; ("CPU16 Monitor: Kernel Internals", sect. 2.4, 3).
;
; Constants, addresses of `at` variables (RES, KF, PAGE, CON_DATA, ...) and field offsets
; (ResData.ticks, ...) are emitted by the B16 compiler in kernel.s as .equ directives.
;
; The kernel map: pages 0-4 = frames 8-12, pages 5-6 = windows (on entry - the junk frame),
; page 7 = frame 7. The process map is RES.cmap.

        .equ    K_MAP01, 9 << 8 | 8
        .equ    K_MAP23, 11 << 8 | 10
        .equ    K_MAP45, GARBAGE << 8 | 12
        .equ    K_MAP67, FRAME_C << 8 | GARBAGE
        .equ    KF_REGS, KF + 2 * F_LR + 2      ; the top of the R0..R12 area of the frame

; ---- the exit routine: $E000, the address in the process's LR (CX16 format, sect. 8.1) ----
        .org    $E000
exit_proc:
        TRAP    #9
        .word   SYS_EXIT

; ---- the session switch stub: $E004 = PREEMPT_PROC (sect. 3.8) ----------------------------
; irq0 puts its address in place of the IP of a process interrupted by a tick while a switch to another session is awaited;
; the former IP is in RES.sw_ip. IRET restored the process flags, TRAP saves them in the frame as they are.
preempt_proc:
        TRAP    #9
        .word   SYS_PREEMPT

; ---- entry into the kernel at startup (sect. 8.3 of the kernel design) ------------------------------
; State: the MMU is on, PAGE0-PAGE4 = frames 8-12 (the kernel code is copied), PAGE7 = frame 7,
; IF = 0, SP is a working stack outside pages 0-4 and 7. Both the initial code of the bare
; machine (boot.asm) and the disk loader (boot/fsboot.asm) come here.
boot_entry:
        LD      R0, #irq_ignore                     ; IRQ0..7
        LD      R1, #$FF00
        LD      R2, #8
@v1:    ST      [R1++], R0
        DEC     R2
        JNZ     @v1
        LD      R0, #trap_bad                       ; ERRCMD, TRAP #9..#15 are refined below
        LD      R2, #8
@v2:    ST      [R1++], R0
        DEC     R2
        JNZ     @v2
        LD      R0, #irq0
        ST      [$FF00], R0
        LD      R0, #irq1
        ST      [$FF02], R0
        LD      R0, #err8
        ST      [$FF10], R0
        LD      R0, #trap9
        ST      [$FF12], R0
        CALL    __init_data                         ; the kernel data in page 4, bss - zeros
        LD      SP, #KF
        CALL    kmain
@dead:  JMP     @dead

; ---- the XMAP fast path (sect. 3.2a) ---------------------------------------------------------
; From trap9: R0 = page, R1 = frame, the process's R2 is in RES.sv0, IF = 0; SP is the process stack,
; [SP] = FLAGS, [SP + 2] = the IP after TRAP. A valid call is executed here, without entering the kernel:
; RES.cmap (the main copy of the map, sect. 2.3) and the page's PAGE register are changed. The result is the same
; as sys_xmap's; erroneous arguments go by the full path, sys_xmap returns INVAL. R2, R3 after
; the call are undefined ("System Calls", sect. 10.3), as after the full path.
xmap_fast:
        CMP     R0, #6
        JA      trap9_full
        CMP     R1, #XM_OWN
        JZ      @own
        CMP     R1, #256
        JAE     trap9_full
        LD      R2, R1                          ; an extended-memory frame of the process?
        SHR     R2, #3
        LDZX    R2, [R2 + RES + ResData.xbits]
        LD      R3, R1
        AND     R3, #7
        SHR     R2, R3
        TEST    R2, #1
        JZ      trap9_full
        LD      R2, R1                          ; R2 is the new frame
        JMP     @set
@own:   LDZX    R2, [R0 + RES + ResData.omap]
@set:   LDZX    R1, [R0 + RES + ResData.cmap]   ; the former frame
        STB     [R0 + RES + ResData.cmap], R2
        LDZX    R3, [R0 + RES + ResData.omap]
        CMP     R1, R3
        JNZ     @res
        LD      R1, #XM_OWN                     ; the former one is own or junk
@res:   ST      [RES + ResData.sv0], R1
        LD      R1, [SP]                        ; FLAGS: ZF = 1, R0 = OK (as in resume)
        OR      R1, #1
        LD      R3, [SP + 2]                    ; the IP after the number word
        ADD     R3, #2
        STB     [R0 + PAGE], R2                 ; the new map; IRET is the synchronization point
        ST      [SP], R1                        ; the return frame is by the new map, as in resume
        ST      [SP + 2], R3
        LD      R1, [RES + ResData.sv0]
        LD      R0, #OK
        IRET

; ---- TRAP #9 (sect. 3.2) ------------------------------------------------------------
trap9:
        ST      [RES + ResData.sv0], R2         ; XMAP by the fast path
        LD      R2, [SP + 2]
        LD      R2, [R2]
        CMP     R2, #SYS_XMAP
        JZ      xmap_fast
trap9_full:
        LD      R2, [RES + ResData.sv0]
        ST      [RES + ResData.psp], SP
        LD      SP, #KF_REGS
        PUSH    LR
        PUSH    R11
        PUSH    R10
        PUSH    R9
        PUSH    R8
        PUSH    R7
        PUSH    R6
        PUSH    R5
        PUSH    R4
        PUSH    R3
        PUSH    R2
        PUSH    R1
        PUSH    R0
        LD      R0, [RES + ResData.psp]
        LD      R1, [R0++]                      ; the process FLAGS
        LD      R2, [R0++]                      ; IP -> the word with the call number
        ST      [KF + 2 * F_SP], R0             ; the process SP before TRAP
        ST      [KF + 2 * F_FL], R1
        LD      R3, [R2++]                      ; the call number
        ST      [KF + 2 * F_IP], R2
        LD      R0, #K_MAP01                    ; the kernel map
        ST      [PAGE + 0], R0
        LD      R0, #K_MAP23
        ST      [PAGE + 2], R0
        LD      R0, #K_MAP45
        ST      [PAGE + 4], R0
        LD      R0, #K_MAP67
        ST      [PAGE + 6], R0
; ---- the dispatcher (sect. 3.3) --------------------------------------------------------------
trap9_disp:
        LD      R4, #sys_nosys
        CMP     R3, #NSYS
        JAE     @call
        ADD     R3, R3
        LD      R4, [R3 + sys_table]
@call:  LD      SP, #KF                         ; the kernel stack under the frame
        LD      R0, [KF + 0]
        LD      R1, [KF + 2]
        LD      R2, [KF + 4]
        LD      R3, [KF + 6]
trap9_call:
        CALL    R4
        LD      R4, [RES + ResData.kf_set]
        CMP     R4, #0
        JNZ     @kfs
        ST      [KF + 0], R0                    ; the results go into the frame
        ST      [KF + 2], R1
        ST      [KF + 4], R2
        ST      [KF + 6], R3
        JMP     @pb
@kfs:   LD      R4, #0                          ; the frame was filled by the handler (SPAWN, EXIT)
        ST      [RES + ResData.kf_set], R4
@pb:    CALL    kexit                           ; Ctrl+C and the switch to a session on exit (sect. 3.4, 3.8)

; ---- exit into the process (sect. 3.4) ---------------------------------------------------------
resume:
        LD      R1, [RES + ResData.cmap]
        ST      [PAGE + 0], R1
        LD      R1, [RES + ResData.cmap + 2]
        ST      [PAGE + 2], R1
        LD      R1, [RES + ResData.cmap + 4]
        ST      [PAGE + 4], R1
        LD      R1, [RES + ResData.cmap + 6]
        ST      [PAGE + 6], R1
        LD      R1, [KF + 2 * F_FL]
        LD      R0, [RES + ResData.kf_raw]
        CMP     R0, #0
        JZ      @zf
        LD      R0, #0                          ; the exact frame (sect. 3.8): flags as they are
        ST      [RES + ResData.kf_raw], R0
        JMP     @nz
@zf:    AND     R1, #$FFFE                      ; ZF = 0
        LD      R0, [KF + 0]
        CMP     R0, #0
        JNZ     @nz
        OR      R1, #1                          ; R0 = 0: ZF = 1
@nz:    LD      R2, [KF + 2 * F_SP]
        ST      [R2 - 4], R1                    ; FLAGS onto the process stack
        LD      R1, [KF + 2 * F_IP]
        ST      [R2 - 2], R1                    ; IP onto the process stack
        LD      SP, #KF + 2
        POP     R1
        POP     R2
        POP     R3
        POP     R4
        POP     R5
        POP     R6
        POP     R7
        POP     R8
        POP     R9
        POP     R10
        POP     R11
        POP     LR
        LD      R0, [KF + 2 * F_SP]
        SUB     R0, #4
        LD      SP, R0
        LD      R0, [KF + 0]
resume_iret:
        IRET

; ---- IRQ0: the timer (sect. 3.5.1) ------------------------------------------------------------
irq0:
        ST      [RES + ResData.sv0], R0
        ST      [RES + ResData.sv1], R1
        LD      R0, [RES + ResData.ticks]
        ADD     R0, #1
        ST      [RES + ResData.ticks], R0
        JNC     @kbd
        LD      R0, [RES + ResData.ticks + 2]
        ADD     R0, #1
        ST      [RES + ResData.ticks + 2], R0
@kbd:   LD      R0, [CON_DATA]                  ; an event: character | scancode << 8
        CMP     R0, #0
        JZ      @done
        ; switching sessions (sect. 3.8): Ctrl+[ / Ctrl+] - the scancode [ / ] with Ctrl in MODS;
        ; instead of Ctrl+] a terminal sends the character $1D. Such events do not enter the ring.
        LDZX    R1, R0
        CMP     R1, #$1D
        JZ      @snext
        LD      R1, R0
        SHR     R1, #8
        CMP     R1, #$1A
        JB      @brk
        CMP     R1, #$1B
        JA      @brk
        LDZX    R1, [CON_MODS]
        TEST    R1, #EVM_CTRL
        JZ      @brk
        LD      R1, R0
        SHR     R1, #8
        CMP     R1, #$1A
        JNZ     @snext
        LD      R1, #$FFFF                      ; Ctrl+[: the previous session
        JMP     @sw
@snext: LD      R1, #1                          ; Ctrl+]: the next one
@sw:    ADD     R1, [RES + ResData.sw_req]
        ST      [RES + ResData.sw_req], R1
        JMP     @kbd
@brk:   LDZX    R1, R0
        CMP     R1, #3                          ; Ctrl+C
        JNE     @enq
        LD      R1, [RES + ResData.nobreak]
        CMP     R1, #0
        JNZ     @enq                            ; NOBREAK: an ordinary event
        LD      R1, #1
        ST      [RES + ResData.abort], R1
        JMP     @kbd
@enq:   LD      R1, [RES + ResData.kb_head]
        ST      [R1 + RES + ResData.kb], R0
        LDZX    R0, [CON_MODS]                  ; modifiers of the same event
        SHR     R1, #1
        STB     [R1 + RES + ResData.kbm], R0
        SHL     R1, #1
        ADD     R1, #2
        AND     R1, #KB_MASK
        CMP     R1, [RES + ResData.kb_tail]
        JZ      @kbd                            ; the ring is full: the event is lost
        ST      [RES + ResData.kb_head], R1
        JMP     @kbd
@done:  CMP     SP, #$E000
        JAE     @ret                            ; a tick in the kernel: the short path
        LD      R0, [RES + ResData.abort]
        CMP     R0, #0
        JNZ     tk_kill
        LD      R0, [RES + ResData.sw_req]      ; waiting for a session switch: the process goes to the stub
        CMP     R0, #0
        JZ      @hk
        LD      R0, [SP + 2]
        CMP     R0, #preempt_proc               ; already redirected (a tick between IRET and TRAP)
        JZ      @hk
        ST      [RES + ResData.sw_ip], R0
        LD      R0, #preempt_proc
        ST      [SP + 2], R0
        JMP     @ret
@hk:    LD      R0, [RES + ResData.hook]
        CMP     R0, #0
        JNZ     tk_hook
@ret:   LD      R0, [RES + ResData.sv0]
        LD      R1, [RES + ResData.sv1]
irq0_iret:
        IRET
tk_hook:
        LD      R0, [RES + ResData.sv0]
        LD      R1, [RES + ResData.sv1]
irq0_hook:
        JMP     [RES + ResData.hook]            ; the process handler does IRET itself
tk_kill:
        LD      R0, #0
        ST      [RES + ResData.abort], R0
        LD      R0, [RES + ResData.brk_hook]
        CMP     R0, #0
        JZ      @kill
        ; the Ctrl+C handler (ONEVENT): ^C, the ring is discarded, the console line will be reset by the kernel;
        ; the processor frame on the process stack is the interrupted place, the handler does IRET
        LD      R0, #'^'
        STB     [CON_DATA], R0
        LD      R0, #'C'
        STB     [CON_DATA], R0
        LD      R0, #$0A
        STB     [CON_DATA], R0
        LD      R0, [RES + ResData.kb_head]
        ST      [RES + ResData.kb_tail], R0
        LD      R0, #1
        ST      [RES + ResData.ln_drop], R0
        LD      R0, [RES + ResData.sv0]
        LD      R1, [RES + ResData.sv1]
        JMP     [RES + ResData.brk_hook]
@kill:  ; Ctrl+C: the process enters EXIT with $8001
        LD      R0, #1
        ST      [RES + ResData.brk_echo], R0
        LD      R0, #exit_proc
        ST      [SP + 2], R0
        LD      R1, [RES + ResData.sv1]
        LD      R0, #EXIT_BREAK
        IRET

; ---- IRQ1: the disk controller --------------------------------------------------------------------
irq1:
        ST      [RES + ResData.sv0], R0
        LD      R0, #0
        STB     [DK_CMD], R0                    ; ACK
        LD      R0, #1
        ST      [RES + ResData.dk_done], R0
        LD      R0, [RES + ResData.sv0]
        IRET

; ---- ERRCMD (vector 8, sect. 3.5.2) -----------------------------------------------------------------
err8:
        ST      [RES + ResData.sv0], R0
        LD      R0, [SP + 2]                    ; the address of the illegal instruction
        CMP     SP, #$E000
        JAE     @kfault
        ST      [RES + ResData.fault_ip], R0
        LD      R0, [RES + ResData.bad_hook]
        CMP     R0, #0
        JZ      @kill
        LD      R0, [RES + ResData.sv0]         ; the process handler (ONEVENT): the frame is the instruction address
        JMP     [RES + ResData.bad_hook]
@kill:  LD      R0, #exit_proc
        ST      [SP + 2], R0
        LD      R0, #EXIT_BADINSN
        IRET
@kfault:
        CALL    kernel_fault

; ---- TRAP #10..#15: reserved by the monitor --------------------------------------------------------
trap_bad:
        ST      [RES + ResData.sv0], R0
        LD      R0, [SP + 2]
        SUB     R0, #2                          ; the address of the TRAP instruction itself
        CMP     SP, #$E000
        JAE     @kfault
        ST      [RES + ResData.fault_ip], R0
        LD      R0, #exit_proc
        ST      [SP + 2], R0
        LD      R0, #EXIT_KILLED
        IRET
@kfault:
        CALL    kernel_fault

irq_ignore:
        IRET

resident_end:
