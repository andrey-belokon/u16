; fsboot.asm - the FS16 volume boot code: sectors 1-3 of disk 0, 1536 bytes
; ("CPU16 Monitor: Disk Boot", sect. 4). The ROM reads it at address $C000 (page 6,
; frame 6) and passes control to $C008 with the MMU off.
;
; The loader finds /sys/monitor.sys (root -> sys -> monitor.sys; direct addresses and IND),
; reads the code segment into frames 8.. through the window (page 4), the resident segment straight into
; frame 7 (page 7), sets the kernel map (except PAGE6, where it runs itself) and jumps to
; the entry point from the file header. Errors - a message and a halt.

        .equ    CON_DATA,  $FF20
        .equ    DK_STATUS, $FF30
        .equ    DK_CMD,    $FF30
        .equ    DK_DC,     $FF32
        .equ    DK_ADDR,   $FF34
        .equ    DK_LBA_LO, $FF36
        .equ    DK_LBA_HI, $FF38
        .equ    PAGE,      $FF40
        .equ    MMU_CTRL,  $FF48

        .equ    BUF,   $D000                    ; data sector
        .equ    IBUF,  $D200                    ; index cluster sector
        .equ    HDR,   BUF                      ; the monitor.sys header: sector 0 of the file in BUF
        .equ    N_SZL, $D420                    ; the current entry: the size (low word)
        .equ    N_SZH, $D422                    ;   the high word with flags
        .equ    N_ADR, $D424                    ;   12 addresses
        .equ    IB_LO, $D43C                    ; LBA of the sector in IBUF ($FFFF - none)
        .equ    IB_HI, $D43E
        .equ    FSEC,  $D440                    ; file sector number
        .equ    BOOT_SP, $E000

        .org    $C000
        .ascii  "CXBT"
        .word   1                               ; boot code version
        .word   0
start:  LD      SP, #BOOT_SP
        LD      R0, #$FFFF
        ST      [IB_LO], R0
        ST      [IB_HI], R0
        ; superblock
        LD      R0, #0
        LD      R1, #0
        LD      R2, #BUF
        CALL    dk_read
        LD      R0, [BUF]
        CMP     R0, #$5346                      ; "FS16"
        JNE     e_notfs
        LD      R0, [BUF + 2]
        CMP     R0, #$3631
        JNE     e_notfs
        LD      R1, #BUF + 36                   ; the ROOT entry
        CALL    take
        LD      R5, #n_sys
        CALL    lookup
        LD      R0, [N_SZH]
        TEST    R0, #$8000                      ; sys - a directory
        JZ      e_nomon
        LD      R5, #n_mon
        CALL    lookup
        LD      R0, [N_SZH]
        TEST    R0, #$8000
        JNZ     e_nomon
        ; MMU: pages 0-3, 5-7 - their own frames, page 4 - the window
        LD      R0, #1 << 8 | 0
        ST      [PAGE + 0], R0
        LD      R0, #3 << 8 | 2
        ST      [PAGE + 2], R0
        LD      R0, #5 << 8 | 8
        ST      [PAGE + 4], R0
        LD      R0, #7 << 8 | 6
        ST      [PAGE + 6], R0
        LD      R0, #1
        STB     [MMU_CTRL], R0
        ; header: sector 0 of the file
        LD      R0, #0
        ST      [FSEC], R0
        LD      R2, #HDR
        CALL    fread
        LD      R0, [HDR]
        CMP     R0, #$5843                      ; "CXMS"
        JNE     e_badmon
        LD      R0, [HDR + 2]
        CMP     R0, #$534D
        JNE     e_badmon
        LD      R0, [HDR + 4]
        CMP     R0, #1
        JNE     e_badmon
        LD      R0, [HDR + 8]                   ; code segment length: at most $8000
        CMP     R0, #$8001
        JAE     e_badmon
        ; code segment: sectors j = 0.. into frame 8 + j / 16, offset (j % 16) * 512
        LD      R4, #0
@code:  LD      R0, R4
        SHL     R0, #9
        CMP     R0, [HDR + 8]
        JAE     @res
        LD      R0, R4
        SHR     R0, #4
        ADD     R0, #8
        STB     [PAGE + 4], R0
        LD      R2, R4
        AND     R2, #15
        SHL     R2, #9
        ADD     R2, #$8000
        CALL    fread
        INC     R4
        JMP     @code
        ; the resident segment: straight into page 7
@res:   LD      R4, #0
@r:     LD      R0, R4
        SHL     R0, #9
        CMP     R0, [HDR + 12]
        JAE     @go
        LD      R2, R0
        ADD     R2, [HDR + 10]
        CALL    fread
        INC     R4
        JMP     @r
        ; the kernel map: PAGE0-4 = 8-12, PAGE5 = the junk frame 13, PAGE6 stays, PAGE7 = 7
@go:    LD      R0, #9 << 8 | 8
        ST      [PAGE + 0], R0
        LD      R0, #11 << 8 | 10
        ST      [PAGE + 2], R0
        LD      R0, #13 << 8 | 12
        ST      [PAGE + 4], R0
        LD      R0, [HDR + 6]                   ; the entry point (boot_entry)
        JMP     R0

; CALL does not use the stack (the return address is in LR), so functions that call
; others save LR on the stack.

; The next sector of the file (FSEC) at address R2. Clobbers R0-R3.
fread:  PUSH    LR
        LD      R0, [FSEC]
        SHR     R0, #2                          ; file cluster
        CALL    bmap
        LD      R1, R0                          ; LBA = c * 4 + FSEC % 4
        SHR     R1, #14
        SHL     R0, #2
        LD      R3, [FSEC]
        AND     R3, #3
        OR      R0, R3
        LD      R3, [FSEC]
        INC     R3
        ST      [FSEC], R3
        POP     LR
        JMP     dk_read

; The disk cluster for cluster R0 of the current entry (direct addresses and IND). Clobbers R0-R3.
bmap:   CMP     R0, #10
        JAE     @ind
        ADD     R0, R0
        LD      R0, [R0 + N_ADR]
        CMP     R0, #0
        JZ      e_badfs
        RET
@ind:   SUB     R0, #10
        CMP     R0, #1024
        JAE     e_badfs                         ; the loader does not support DIND
        PUSH    LR
        PUSH    R0
        LD      R3, [N_ADR + 20]                ; IND
        LD      R1, R3
        SHR     R1, #14
        SHL     R3, #2
        SHR     R0, #8                          ; index cluster sector
        OR      R0, R3
        CMP     R0, [IB_LO]
        JNE     @rd
        CMP     R1, [IB_HI]
        JE      @have
@rd:    ST      [IB_LO], R0
        ST      [IB_HI], R1
        PUSH    R2
        LD      R2, #IBUF
        CALL    dk_read
        POP     R2
@have:  POP     R0
        POP     LR
        AND     R0, #255
        ADD     R0, R0
        LD      R0, [R0 + IBUF]
        CMP     R0, #0
        JZ      e_badfs
        RET

; The directory entry at address R1 becomes current (size and addresses). Clobbers R0-R2.
take:   LD      R0, [R1 + 32]
        ST      [N_SZL], R0
        LD      R0, [R1 + 34]
        ST      [N_SZH], R0
        ADD     R1, #40
        LD      R2, #N_ADR
        LD      R0, #12
@t:     PUSH    R0
        LD      R0, [R1++]
        ST      [R2++], R0
        POP     R0
        DEC     R0
        JNZ     @t
        RET

; Find the name R5 in the current directory (32 bytes, zero-padded) and make the entry current.
; Names are compared case-insensitively (FS16, sect. 7.1). The names sought are written in lower-case
; Latin letters, digits and a dot, so it is enough to lower-case A-Z of the entry
; byte: the lowered Cyrillic letters will not match them anyway.
lookup: PUSH    LR
        LD      R0, [N_SZL]                     ; the number of directory clusters: size / 2048
        SHR     R0, #11
        LD      R1, [N_SZH]
        AND     R1, #$07FF
        SHL     R1, #5
        OR      R0, R1
        ST      [FSEC], R0                      ; FSEC here is the number of clusters
        LD      R4, #0                          ; directory sector number
@sec:   LD      R0, R4
        SHR     R0, #2
        CMP     R0, [FSEC]
        JAE     e_nomon
        CALL    bmap
        LD      R1, R0
        SHR     R1, #14
        SHL     R0, #2
        LD      R3, R4
        AND     R3, #3
        OR      R0, R3
        LD      R2, #BUF
        CALL    dk_read
        LD      R1, #BUF                        ; 8 entries of a sector
@ent:   LD      R2, R1
        LD      R3, R5
        LD      R0, #32
@cmp:   PUSH    R0
        LDZX    R0, [R2++]
        CMP     R0, #'A'
        JB      @lc
        CMP     R0, #'Z'
        JA      @lc
        ADD     R0, #$20
@lc:    LDZX    R6, [R3++]
        CMP     R0, R6
        POP     R0
        JNE     @next
        DEC     R0
        JNZ     @cmp
        POP     LR
        JMP     take                            ; found: entry R1
@next:  ADD     R1, #64
        CMP     R1, #BUF + 512
        JB      @ent
        INC     R4
        JMP     @sec

; Read sector LBA = R1:R0 of disk 0 to address R2 (poll BUSY). Clobbers R0.
dk_read:
        PUSH    R0
        LD      R0, #1 << 8 | 0
        ST      [DK_DC], R0
        ST      [DK_ADDR], R2
        POP     R0
        ST      [DK_LBA_LO], R0
        ST      [DK_LBA_HI], R1
        LD      R0, #1
        STB     [DK_CMD], R0
@w:     LDZX    R0, [DK_STATUS]
        TEST    R0, #1
        JNZ     @w
        TEST    R0, #2
        JNZ     e_io
        LD      R0, #0
        STB     [DK_CMD], R0                    ; ACK
        RET

e_notfs:
        LD      R1, #m_notfs
        JMP     halt
e_nomon:
        LD      R1, #m_nomon
        JMP     halt
e_badmon:
        LD      R1, #m_badmon
        JMP     halt
e_badfs:
        LD      R1, #m_badfs
        JMP     halt
e_io:   LD      R1, #m_io
halt:
@p:     LDZX    R0, [R1++]
        CMP     R0, #0
        JZ      @h
        STB     [CON_DATA], R0
        JMP     @p
@h:     CLI
@l:     WFI
        JMP     @l

n_sys:  .ascii  "sys"
        .space  29
n_mon:  .ascii  "monitor.sys"
        .space  21
m_notfs:  .asciz "boot: no FS16 volume on disk 0\n"
m_nomon:  .asciz "boot: no file A:/sys/monitor.sys\n"
m_badmon: .asciz "boot: bad file A:/sys/monitor.sys\n"
m_badfs:  .asciz "boot: damaged volume or file too large\n"
m_io:     .asciz "boot: disk 0 read error\n"
fsboot_end:
