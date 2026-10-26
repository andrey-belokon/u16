; boot.asm - the initial code for starting the monitor image on a bare machine (sect. 8.3
; of the kernel design): the emulator loaded monitor.bin at address 0, the MMU is off, IF = 0.
; The image lies in frames 0-7: the kernel code is pages 0-3, this code is page 6, the resident
; part is page 7 (straight in frame 7). The kernel code is copied to frames 8.., the kernel
; map is set (PAGE6 stays in place), then boot_entry in the resident part.

        .equ    NKF, (__image_end + $1FFF) >> 13    ; pages of kernel code
        .equ    BOOT_SP, $E000                      ; the stack of the initial code: the top of page 6

        .org    $C000
boot:   LD      SP, #BOOT_SP
        LD      R0, #1 << 8 | 0                     ; a temporary map
        ST      [PAGE + 0], R0
        LD      R0, #3 << 8 | 2
        ST      [PAGE + 2], R0
        LD      R0, #8 << 8 | 4                     ; PAGE5 is a window onto the destination frame
        ST      [PAGE + 4], R0
        LD      R0, #7 << 8 | 6
        ST      [PAGE + 6], R0
        LD      R0, #1
        STB     [MMU_CTRL], R0

        LD      R4, #0                              ; copying pages 0..NKF-1
@copy:  LD      R0, R4
        ADD     R0, #FRAME_K0
        STB     [PAGE + 5], R0
        LD      R1, R4
        SHL     R1, #13
        LD      R2, #$A000
        LD      R3, #4096
@w:     LD      R0, [R1++]
        ST      [R2++], R0
        DEC     R3
        JNZ     @w
        INC     R4
        CMP     R4, #NKF
        JB      @copy

        LD      R0, #K_MAP01                        ; the kernel map, except PAGE6
        ST      [PAGE + 0], R0
        LD      R0, #K_MAP23
        ST      [PAGE + 2], R0
        LD      R0, #K_MAP45
        ST      [PAGE + 4], R0
        JMP     #boot_entry
