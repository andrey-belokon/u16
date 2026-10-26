; monitor.asm - the monitor image ("CPU16 Monitor: Kernel Internals", sect. 8.2).
;   $0000        JMP #boot (start from a bare machine)
;   $0004..      kernel code (pages 0-3), data from $8000 (page 4)
;   $C000        the initial code (page 6)
;   $E000-$E3FF  resident code; $E400 - RES; $E800-$EDDF kernel stack; $EDE0 - KF
;   $EE00-$FDFF  sector buffers; $FE00-$FEFF reserve; $FF00 - vectors and devices
        .org    $0000
        JMP     #boot
        .include "kernel.s"
        .include "boot.asm"
        .include "resident.asm"

; limits: an overlap is a build error
        .org    $A000
        .reserve $2000                              ; window W0: neither kernel code nor data
        .org    RES
        .reserve $E800 - RES                        ; RES
        .org    $E800
        .reserve $EE00 - $E800                      ; the kernel stack and KF
        .org    $EE00
        .reserve $FF00 - $EE00                      ; sector buffers and the reserve
