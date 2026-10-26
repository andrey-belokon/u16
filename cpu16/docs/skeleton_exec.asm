; skeleton_exec.asm — skeleton of a CX16 executable.
; (See "CPU16 Monitor: Executable File Format and Program Startup".)
;
; The program supports both startup environments (executable format, Section 8):
;   - Under Monitor, address $0004 holds a PIB ("CXPB"), R0 = argc, R1 = argv, SP = top,
;     LR points to the exit routine, and interrupts are enabled (IF = 1). Monitor owns the stack and vectors.
;   - On bare hardware (the emulator without Monitor), address $0004 holds the header ("CX16"), registers start at zero,
;     and IF = 0. The program sets up its own stack and ERRCMD vector; no arguments are provided (argc = argv = 0).
; Under Monitor, an interrupt may occur between any two instructions (format specification, Section 8.3):
; do not store data below SP or use SP for anything other than the stack. Protect critical sections using
; PUSH FLAGS / CLI ... POP FLAGS. Use STI only on the bare-machine path, after initializing the vectors.
; To exit from anywhere, jump to exit with the exit status in R0. Under Monitor, this restores SP and LR
; and returns via RET; on bare hardware, it halts the processor.
;
; Build: cpu16asm --exec skeleton_exec.asm           (--exec validates the executable header)
; Run on bare hardware: cpu16 skeleton_exec.bin
;
; Adapt MIN_FREE, MAX_TOP, main, and the data section for your program. Place uninitialized data
; after image_end with .reserve: it is omitted from the executable and zeroed by Monitor.
; On bare hardware, the program must initialize it explicitly if zero initialization is required.

        .equ    PIB_MAGIC,  $0004       ; PIB under Monitor, executable header on bare hardware
        .equ    VECTORS,    $FF00
        .equ    STACK_TOP,  $FF00       ; Stack on bare hardware
        .equ    MIN_FREE,   1024        ; Minimum free memory for stack and heap, in bytes (must be even)
        .equ    MAX_TOP,    $2000       ; Upper memory limit; 0 means use all memory granted by Monitor

; ============================================================================
; Header (12 words immediately after the initial jump)
; ============================================================================
        .org    $0000
        JMP     #start                  ; Absolute form: exactly two words
        .ascii  "CX16"                  ; magic
        .word   1, 0                    ; version, flags
        .word   image_end, bss_end
        .word   MIN_FREE, MAX_TOP
        .word   0, 0, 0, 0              ; Reserved

; ============================================================================
; Startup
; ============================================================================
start:  LD      R4, [PIB_MAGIC+2]
        CMP     R4, #'P' | 'B' << 8     ; "CXPB": second half of the signature
        JZ      @monitor
        LD      SP, #STACK_TOP          ; Bare hardware: initialize SP before using the stack
        LD      R0, #on_errcmd          ; Install on_errcmd as the ERRCMD handler first;
        CALL    vectors_init            ; install irq_ignore for all other vectors
        LD      R2, #0
        ST      [exit_lr], R2           ; Zero means there is no Monitor exit routine
        LD      R0, #0                  ; argc = 0, argv = 0
        LD      R1, #0
        JMP     @run
@monitor:
        ST      [exit_lr], LR           ; Monitor exit routine
        ST      [exit_sp], SP           ; Stack pointer to restore on exit, regardless of call depth
@run:   CALL    main                    ; R0 = argc, R1 = argv
        ; main returns its exit status in R0

; exit: terminate the program with the status in R0.
exit:   LD      R4, [exit_lr]
        TEST    R4, R4
        JZ      @bare
        LD      SP, [exit_sp]           ; A single instruction restores SP; the stack is valid before and after it (IF = 1)
        LD      LR, R4
        RET                             ; Monitor exit routine; status is in R0
@bare:  CLI                             ; Bare hardware: halt
@stop:  WFI
        JMP     @stop

on_errcmd:                              ; Bare hardware only: [SP+2] holds the faulting instruction address
        LD      R0, #msg_errcmd
        CALL    puts
        LD      R0, [SP+2]
        CALL    print_hex16
        CALL    newline
        LD      R0, #1
        JMP     exit

; ============================================================================
; Program body: print each command-line argument on a separate line
; ============================================================================
; main: R0 = argc, R1 = argv; returns an exit status in R0.
main:   PUSH    LR
        PUSH    R4
        PUSH    R5
        LD      R4, R0
        LD      R5, R1
        LD      R0, #msg_hello
        CALL    puts
        TEST    R4, R4
        JNZ     @args
        LD      R0, #msg_bare
        CALL    puts
        JMP     @done
@args:  LD      R0, #msg_argc
        CALL    puts
        LD      R0, R4
        CALL    print_u16
        CALL    newline
@arg:   LD      R0, [R5++]              ; argv[i]; argv[argc] = 0
        TEST    R0, R0
        JZ      @done
        CALL    puts
        CALL    newline
        JMP     @arg
@done:  LD      R0, #0                  ; Exit status
        POP     R5
        POP     R4
        POP     IP

msg_hello:  .asciz  "CX16 skeleton\n"
msg_bare:   .asciz  "started without the monitor: no arguments\n"
msg_argc:   .asciz  "argc = "
msg_errcmd: .asciz  "\nERRCMD at $"

        .include "../lib/stdlib.asm"

; ============================================================================
; Uninitialized data; not stored in the executable
; ============================================================================
        .align  2                       ; .reserve does not align addresses, but the following items are words
image_end:
exit_lr:    .reserve 2                  ; Monitor exit routine (0 on bare hardware)
exit_sp:    .reserve 2                  ; Initial SP under Monitor
bss_end:
