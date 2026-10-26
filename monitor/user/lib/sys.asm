; sys.asm - system call wrappers ("System Calls", sect. 10.1): TRAP #9, the number word,
; RET. The register layout matches the B16 convention, so a wrapper rearranges nothing.
; It is included by the line include asm "sys.asm" in sys.b16.

sys_exit:
        TRAP    #9
        .word   SYS_EXIT
        RET
sys_spawn:
        TRAP    #9
        .word   SYS_SPAWN
        RET
sys_exec:
        TRAP    #9
        .word   SYS_EXEC
        RET
sys_onevent:
        TRAP    #9
        .word   SYS_ONEVENT
        RET
sys_open:
        TRAP    #9
        .word   SYS_OPEN
        RET
sys_close:
        TRAP    #9
        .word   SYS_CLOSE
        RET
sys_read:
        TRAP    #9
        .word   SYS_READ
        RET
sys_write:
        TRAP    #9
        .word   SYS_WRITE
        RET
sys_seek:
        TRAP    #9
        .word   SYS_SEEK
        RET
sys_dup:
        TRAP    #9
        .word   SYS_DUP
        RET
sys_hinfo:
        TRAP    #9
        .word   SYS_HINFO
        RET
sys_setsize:
        TRAP    #9
        .word   SYS_SETSIZE
        RET
sys_fstat:
        TRAP    #9
        .word   SYS_FSTAT
        RET
sys_stat:
        TRAP    #9
        .word   SYS_STAT
        RET
sys_delete:
        TRAP    #9
        .word   SYS_DELETE
        RET
sys_rename:
        TRAP    #9
        .word   SYS_RENAME
        RET
sys_mkdir:
        TRAP    #9
        .word   SYS_MKDIR
        RET
sys_chdir:
        TRAP    #9
        .word   SYS_CHDIR
        RET
sys_getcwd:
        TRAP    #9
        .word   SYS_GETCWD
        RET
sys_readdir:
        TRAP    #9
        .word   SYS_READDIR
        RET
sys_setattr:
        TRAP    #9
        .word   SYS_SETATTR
        RET
sys_volinfo:
        TRAP    #9
        .word   SYS_VOLINFO
        RET
sys_mount:
        TRAP    #9
        .word   SYS_MOUNT
        RET
sys_unmount:
        TRAP    #9
        .word   SYS_UNMOUNT
        RET
sys_sync:
        TRAP    #9
        .word   SYS_SYNC
        RET
sys_setmtime:
        TRAP    #9
        .word   SYS_SETMTIME
        RET
sys_con_mode:
        TRAP    #9
        .word   SYS_CON_MODE
        RET
sys_con_getev:
        TRAP    #9
        .word   SYS_CON_GETEV
        RET
sys_con_flush:
        TRAP    #9
        .word   SYS_CON_FLUSH
        RET
sys_meminfo:
        TRAP    #9
        .word   SYS_MEMINFO
        RET
sys_settop:
        TRAP    #9
        .word   SYS_SETTOP
        RET
sys_xalloc:
        TRAP    #9
        .word   SYS_XALLOC
        RET
sys_xfree:
        TRAP    #9
        .word   SYS_XFREE
        RET
sys_xmap:
        TRAP    #9
        .word   SYS_XMAP
        RET
sys_time_get:
        TRAP    #9
        .word   SYS_TIME_GET
        RET
sys_time_set:
        TRAP    #9
        .word   SYS_TIME_SET
        RET
sys_ticks:
        TRAP    #9
        .word   SYS_TICKS
        RET
sys_sleep:
        TRAP    #9
        .word   SYS_SLEEP
        RET
sys_tickhook:
        TRAP    #9
        .word   SYS_TICKHOOK
        RET
sys_disk_info:
        TRAP    #9
        .word   SYS_DISK_INFO
        RET
sys_disk_io:
        TRAP    #9
        .word   SYS_DISK_IO
        RET
sys_sysinfo:
        TRAP    #9
        .word   SYS_SYSINFO
        RET
sys_shutdown:
        TRAP    #9
        .word   SYS_SHUTDOWN
        RET
sys_sess_info:
        TRAP    #9
        .word   SYS_SESS_INFO
        RET
sys_sess_new:
        TRAP    #9
        .word   SYS_SESS_NEW
        RET
sys_sess_switch:
        TRAP    #9
        .word   SYS_SESS_SWITCH
        RET
sys_sess_kill:
        TRAP    #9
        .word   SYS_SESS_KILL
        RET
