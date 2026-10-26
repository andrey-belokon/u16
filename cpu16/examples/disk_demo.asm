; disk_demo.asm - the disk controller: disk information and a launch counter on a disk.
;
; The program:
;   1. finds the connected disks by the DRIVES register and prints information for each
;      from the INFO block: the number of sectors, the flags and the label;
;   2. on the first disk without write protection reads sector 0, prints its first 16 bytes,
;      increments the launch counter in the first word of the sector, writes the sector back
;      and saves it on the medium with the FLUSH command.
; At every launch with the same image the counter grows: the data really is on the disk.
;
; Run (from the project root):
;   python3 -c "open('demo.img','wb').write(bytes(64*512))"     # an empty image of 64 sectors
;   build/cpu16asm examples/disk_demo.asm -o disk_demo.bin
;   build/cpu16 --disk 0=demo.img disk_demo.bin                 # the counter grows with each launch
;   build/cpu16 --disk 0=demo.img:ro disk_demo.bin              # write protection: information only
; The image size must be a multiple of 512 bytes. In the web debugger disks are attached on the
; "Disks" tab (the "New…" and "Attach file…" buttons); there the commands take 1 ms per sector.
;
; The driver is the disk_read/disk_write routines with polling waits, as in "CPU16 Machine",
; sect. 6.6. Used from stdlib.asm: vectors_init, puts, putc, newline, print_u16, print_u32, print_hex8.

        .equ    DK_CMD,     $FF30
        .equ    DK_STATUS,  $FF30
        .equ    DK_ERROR,   $FF31
        .equ    DK_DISK,    $FF32
        .equ    DK_COUNT,   $FF33
        .equ    DK_ADDR,    $FF34
        .equ    DK_LBA_LO,  $FF36
        .equ    DK_LBA_HI,  $FF38
        .equ    DK_DRIVES,  $FF3B
        .equ    DK_READ,    1
        .equ    DK_WRITE,   2
        .equ    DK_INFO,    3
        .equ    DK_FLUSH,   4
        .equ    DK_BUSY,    $01
        .equ    INFO_RO,    $01         ; flags of the INFO block (offset 4)
        .equ    INFO_REMOVABLE, $02
        .equ    INFO_SMALL, $04

        .equ    VECTORS,    $FF00
        .equ    STACK_TOP,  $FF00
        .equ    RAM_START,  $8000

; ============================================================================
; Prologue
; ============================================================================
        .org    $0000
reset:  LD      SP, #STACK_TOP
        LD      R0, #on_fault           ; ERRCMD goes to on_fault (first of all),
        CALL    vectors_init            ;   the other vectors - to the empty irq_ignore
        CALL    main                    ; interrupts are not needed: IF stays 0
halt:   CLI
@stop:  WFI
        JMP     @stop

on_fault:                               ; [SP+2] is the address where it happened
        LD      R0, #msg_fault
        CALL    puts
        LD      R0, [SP+2]
        CALL    print_hex16
        CALL    newline
        JMP     halt

; ============================================================================
; Main program
; ============================================================================
; R4 is the disk number, R5 the DRIVES mask, R6 the disk for the counter (-1 - none yet).
main:   PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        LDZX    R5, [DK_DRIVES]
        CMP     R5, #0
        JNZ     @have
        LD      R0, #msg_nodisk
        CALL    puts
        JMP     @ret
@have:  LD      R4, #0
        LD      R6, #-1
@next:  LD      R0, R5
        SHR     R0, R4                  ; the bit of disk R4 into bit 0
        TEST    R0, #1
        JZ      @skip
        LD      R0, R4
        CALL    show_disk               ; R0 = the INFO flags or -1 on error
        CMP     R0, #-1
        JZ      @skip
        TEST    R0, #INFO_RO
        JNZ     @skip
        CMP     R6, #-1                 ; the first writable disk - for the counter
        JNZ     @skip
        LD      R6, R4
@skip:  INC     R4
        CMP     R4, #8
        JB      @next

        CALL    newline
        CMP     R6, #-1
        JNZ     @count
        LD      R0, #msg_allro
        CALL    puts
        JMP     @ret
@count: LD      R0, R6
        CALL    bump_counter
@ret:   POP     R6
        POP     R5
        POP     R4
        POP     IP

; show_disk - print information about a disk. R0 is the disk number.
; Result: R0 is the flags byte from the INFO block or -1 on error. R4 is preserved.
show_disk:
        PUSH    LR
        PUSH    R4
        LD      R4, R0
        LD      R0, #msg_disk
        CALL    puts
        LD      R0, R4
        CALL    print_u16
        LD      R0, #msg_colon
        CALL    puts
        LD      R0, R4
        LD      R1, #info
        LD      R2, #DK_INFO
        CALL    disk_cmd
        JZ      @ok
        CALL    print_error
        LD      R0, #-1
        JMP     @ret
@ok:    LD      R0, [info]              ; the number of sectors: 32 bits, the low word first
        LD      R1, [info+2]
        CALL    print_u32
        LD      R0, #msg_sectors
        CALL    puts
        LDZX    R4, [info+4]            ; flags
        TEST    R4, #INFO_RO
        JZ      @rw
        LD      R0, #msg_ro
        CALL    puts
@rw:    TEST    R4, #INFO_SMALL
        JZ      @big
        LD      R0, #msg_small
        CALL    puts
@big:   TEST    R4, #INFO_REMOVABLE
        JZ      @lbl
        LD      R0, #msg_removable
        CALL    puts
@lbl:   LD      R0, #msg_label
        CALL    puts
        LD      R0, #info+16            ; label: a zero-terminated string, up to 15 characters
        CALL    puts
        LD      R0, #'"'
        CALL    putc
        CALL    newline
        LD      R0, R4
@ret:   POP     R4
        POP     IP

; bump_counter - the launch counter in sector 0 of disk R0.
bump_counter:
        PUSH    LR
        PUSH    R4
        LD      R4, R0
        LD      R0, #msg_sector0
        CALL    puts
        LD      R0, R4
        CALL    print_u16
        LD      R0, #msg_colon
        CALL    puts
        LD      R0, R4                  ; read sector 0
        LD      R1, #0
        LD      R2, #sector
        LD      R3, #1
        CALL    disk_read
        JNZ     @err
        LD      R1, #sector             ; the first 16 bytes
@dump:  LDZX    R0, [R1++]
        PUSH    R1
        CALL    print_hex8
        LD      R0, #' '
        CALL    putc
        POP     R1
        CMP     R1, #sector+16
        JB      @dump
        CALL    newline
        LD      R0, [sector]            ; the launch counter
        INC     R0
        ST      [sector], R0
        LD      R0, R4                  ; write the sector back
        LD      R1, #0
        LD      R2, #sector
        LD      R3, #1
        CALL    disk_write
        JNZ     @err
        LD      R0, R4                  ; and save it on the medium
        LD      R1, #0
        LD      R2, #DK_FLUSH
        CALL    disk_cmd
        JNZ     @err
        LD      R0, #msg_runs
        CALL    puts
        LD      R0, [sector]
        CALL    print_u16
        CALL    newline
        JMP     @ret
@err:   CALL    print_error
@ret:   POP     R4
        POP     IP

; print_error - print a disk error. R0 is the error code (1..6).
print_error:
        PUSH    LR
        PUSH    R0
        LD      R0, #msg_error
        CALL    puts
        POP     R0
        PUSH    R0
        CALL    print_u16
        LD      R0, #' '
        CALL    putc
        POP     R0
        CMP     R0, #7
        JB      @known
        LD      R0, #0
@known: SHL     R0, #1
        LD      R0, [R0+err_names]
        CALL    puts
        CALL    newline
        POP     IP

; ============================================================================
; The small-disk driver: waiting by polling ("CPU16 Machine", sect. 6.6)
; ============================================================================
; disk_read, disk_write - R0 = disk, R1 = LBA, R2 = buffer address, R3 = number of sectors.
; disk_cmd - a command without sector transfer (INFO, FLUSH): R0 = disk, R1 = address (for INFO),
;            R2 = command code.
; Output: R0 = error code, ZF = 1 on success. R4 is used and saved on the stack.
disk_read:
        PUSH    R4
        LD      R4, #DK_READ
        JMP     disk_io
disk_write:
        PUSH    R4
        LD      R4, #DK_WRITE
disk_io:                                ; R4 = the command, the saved R4 is on the stack
        STB     [DK_DISK], R0
        STB     [DK_COUNT], R3
        ST      [DK_ADDR], R2
        ST      [DK_LBA_LO], R1
        LD      R0, #0
        ST      [DK_LBA_HI], R0         ; a small disk: the high word is 0
        STB     [DK_CMD], R4
@wait:  LDZX    R0, [DK_STATUS]
        TEST    R0, #DK_BUSY
        JNZ     @wait
        LDZX    R0, [DK_ERROR]
        POP     R4
        CMP     R0, #0
        RET
disk_cmd:
        STB     [DK_DISK], R0
        ST      [DK_ADDR], R1
        STB     [DK_CMD], R2
@wait:  LDZX    R0, [DK_STATUS]
        TEST    R0, #DK_BUSY
        JNZ     @wait
        LDZX    R0, [DK_ERROR]
        CMP     R0, #0
        RET

; ============================================================================
; Constants
; ============================================================================
err_names:      .word   e_unknown, e_nodisk, e_badcmd, e_range, e_badaddr, e_readonly, e_io
e_unknown:      .asciz  "?"
e_nodisk:       .asciz  "NODISK: no disk"
e_badcmd:       .asciz  "BADCMD: unknown command"
e_range:        .asciz  "RANGE: past the end of the disk"
e_badaddr:      .asciz  "BADADDR: buffer past $FF1F"
e_readonly:     .asciz  "READONLY: disk is write-protected"
e_io:           .asciz  "IO: media error"
msg_nodisk:     .asciz  "No disks. Run: cpu16 --disk 0=demo.img disk_demo.bin\n"
msg_disk:       .asciz  "disk "
msg_colon:      .asciz  ": "
msg_sectors:    .asciz  " sectors"
msg_ro:         .asciz  ", read-only"
msg_small:      .asciz  ", small"
msg_removable:  .asciz  ", removable"
msg_label:      .asciz  ", label \""
msg_allro:      .asciz  "All disks are read-only: nothing to write.\n"
msg_sector0:    .asciz  "sector 0 of disk "
msg_runs:       .asciz  "runs counted on this disk: "
msg_error:      .asciz  "error "
msg_fault:      .asciz  "\nfault at $"

        .include "../lib/stdlib.asm"

; ============================================================================
; RAM
; ============================================================================
        .org    RAM_START
info:   .space  32                      ; the INFO block
sector: .space  512                     ; the sector buffer
