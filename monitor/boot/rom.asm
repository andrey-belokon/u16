; rom.asm - the startup code in ROM ("CPU16 Monitor: Disk Boot", sect. 3).
; After reset: IP = 0, the MMU is off, IF = 0. It reads sectors 1-3 of disk 0 (the FS16 volume
; boot code) to address $C000, checks the signature and passes control to $C008.
; In hardware - frames 0-3 ($0000-$7FFF); in the emulator - an image loaded at address 0.

        .equ    CON_DATA,  $FF20
        .equ    CON_CTRL,  $FF22
        .equ    CON_ATTR,  $FF23
        .equ    DK_STATUS, $FF30
        .equ    DK_CMD,    $FF30
        .equ    DK_DC,     $FF32                ; DISK in the low byte, COUNT in the high byte
        .equ    DK_ADDR,   $FF34
        .equ    DK_LBA_LO, $FF36
        .equ    DK_LBA_HI, $FF38
        .equ    DK_DRIVES, $FF3B
        .equ    BOOT_ADDR, $C000
        .equ    BOOT_SP,   $E000                ; stack - the top of page 6

        .org    $0000
rom:    LD      SP, #BOOT_SP
        LD      R0, #rom_fault                  ; all vectors go to the failure handler
        LD      R1, #$FF00
        LD      R2, #16
@v:     ST      [R1++], R0
        DEC     R2
        JNZ     @v
        LD      R0, #3                          ; teletype, the cursor is visible
        STB     [CON_CTRL], R0
        LD      R0, #$07
        STB     [CON_ATTR], R0
        LDZX    R0, [DK_DRIVES]
        TEST    R0, #1
        JZ      @nodisk
        LD      R0, #3 << 8 | 0                 ; disk 0, 3 sectors
        ST      [DK_DC], R0
        LD      R0, #BOOT_ADDR
        ST      [DK_ADDR], R0
        LD      R0, #1
        ST      [DK_LBA_LO], R0
        LD      R0, #0
        ST      [DK_LBA_HI], R0
        LD      R0, #1                          ; READ
        STB     [DK_CMD], R0
@wait:  LDZX    R0, [DK_STATUS]
        TEST    R0, #1                          ; BUSY
        JNZ     @wait
        TEST    R0, #2                          ; ERR
        JNZ     @ioerr
        LD      R0, #0                          ; ACK
        STB     [DK_CMD], R0
        LD      R0, [BOOT_ADDR]                 ; signature "CXBT", version 1
        CMP     R0, #$5843
        JNE     @noboot
        LD      R0, [BOOT_ADDR + 2]
        CMP     R0, #$5442
        JNE     @noboot
        LD      R0, [BOOT_ADDR + 4]
        CMP     R0, #1
        JNE     @noboot
        JMP     #BOOT_ADDR + 8
@nodisk:
        LD      R1, #m_nodisk
        JMP     rom_halt
@ioerr: LD      R1, #m_ioerr
        JMP     rom_halt
@noboot:
        LD      R1, #m_noboot
        JMP     rom_halt
rom_fault:
        LD      R1, #m_fault
; R1 is a message; print it and halt the machine (the timer is off after reset).
rom_halt:
@p:     LDZX    R0, [R1++]
        CMP     R0, #0
        JZ      @h
        STB     [CON_DATA], R0
        JMP     @p
@h:     CLI
@l:     WFI
        JMP     @l

m_nodisk:   .asciz  "ROM: no disk 0\n"
m_ioerr:    .asciz  "ROM: disk 0 read error\n"
m_noboot:   .asciz  "ROM: disk 0 is not bootable\n"
m_fault:    .asciz  "ROM: fault\n"
