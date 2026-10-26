; klib.asm - the assembly functions and the kernel call table (included into the kernel by the include asm line in kernel.b16).
; B16 convention: arguments R0..R2, only R0..R3 are clobbered.

; void kcopy(ptr dst, ptr src, uint n): n bytes; by words if dst, src and n are even
kcopy:
        CMP     R2, #0
        JZ      @done
        LD      R3, R0
        OR      R3, R1
        OR      R3, R2
        TEST    R3, #1
        JNZ     @bytes
        SHR     R2, #1
@w:     LD      R3, [R1++]
        ST      [R0++], R3
        SUB     R2, #1
        JNZ     @w
        RET
@bytes: LDZX    R3, [R1++]
        STB     [R0++], R3
        SUB     R2, #1
        JNZ     @bytes
@done:  RET

; void kzero(ptr dst, uint n): dst and n are even
kzero:
        SHR     R1, #1
        JZ      @done
        LD      R2, #0
@w:     ST      [R0++], R2
        SUB     R1, #1
        JNZ     @w
@done:  RET

; ---- the screen (the machine, sect. 2.2: SCRD) - sessions, sess.b16 ----------------------------------------
; void scr_save(ptr dst): 2000 words "character | attribute << 8" from cell (0, 0); the position is clobbered
scr_save:
        LD      R1, #0
        STB     [CON_COL], R1
        STB     [CON_ROW], R1
        LD      R1, #2000
@w:     LD      R2, [CON_SCRD]                  ; cell; the position goes to the next one
        ST      [R0++], R2
        SUB     R1, #1
        JNZ     @w
        RET

; void scr_restore(ptr src): the screen from 2000 words in coordinate mode (CTRL = 0) from (0, 0);
; after the last cell the position is (0, 0) again, there is no scrolling. CTRL, ATTR and the position are set by the caller.
scr_restore:
        LD      R1, #0
        STB     [CON_CTRL], R1
        STB     [CON_COL], R1
        STB     [CON_ROW], R1
        LD      R1, #2000
@w:     LD      R2, [R0++]
        LD      R3, R2
        SHR     R3, #8
        STB     [CON_ATTR], R3
        STB     [CON_DATA], R2
        SUB     R1, #1
        JNZ     @w
        RET

; ---- the call handler table (kernel design, sect. 3.3; "System Calls", sect. 9.1) ----
; It lies in the kernel code (pages 0-3): the dispatcher reads it already in the kernel map.
sys_table:
        .word   sys_exit, sys_spawn, sys_exec, sys_onevent, sys_nosys, sys_nosys, sys_nosys, sys_nosys  ; $00
        .word   sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys
        .word   sys_open, sys_close, sys_read, sys_write, sys_seek, sys_dup, sys_hinfo, sys_setsize     ; $10
        .word   sys_fstat, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys
        .word   sys_stat, sys_delete, sys_rename, sys_mkdir, sys_chdir, sys_getcwd, sys_readdir, sys_setattr ; $20
        .word   sys_volinfo, sys_mount, sys_unmount, sys_sync, sys_setmtime, sys_nosys, sys_nosys, sys_nosys
        .word   sys_con_mode, sys_con_getev, sys_con_flush, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys ; $30
        .word   sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys
        .word   sys_meminfo, sys_settop, sys_xalloc, sys_xfree, sys_xmap, sys_nosys, sys_nosys, sys_nosys ; $40
        .word   sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys
        .word   sys_time_get, sys_time_set, sys_ticks, sys_sleep, sys_tickhook, sys_nosys, sys_nosys, sys_nosys ; $50
        .word   sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys
        .word   sys_disk_info, sys_disk_io, sys_sysinfo, sys_shutdown, sys_nosys, sys_nosys, sys_nosys, sys_nosys ; $60
        .word   sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys
        .word   sys_sess_info, sys_sess_new, sys_sess_switch, sys_sess_kill, sys_nosys, sys_nosys, sys_nosys, sys_nosys ; $70
        .word   sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_nosys, sys_preempt
sys_table_end:
